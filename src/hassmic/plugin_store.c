/* plugin_store.h: packages, what was accepted, settings, uploaded files.  docs/plugins.md 4.1 to 4.4, 5.1, 5.4.
 *
 * A package (<id>-<version>.hmp) is a plain tar of manifest.json, manifest.sig, plugin, LICENSE and, when the plugin
 * index lists it, listing.sig.  Signatures are otatool's: Monocypher's EdDSA (edwards25519 with BLAKE2b, not RFC
 * 8032's Ed25519), each over a text that starts with its own purpose, so that none ever passes for another:
 *   manifest.sig   64 bytes, the author's, over "hassmic plugin manifest 1\n" + manifest.json's bytes
 *   listing.sig    text: the signature as hex on the first line, then what it signs, by the index key
 *                  (plugin_index.c; its build adds it, hassmic-plugins' tools/index.py):
 *                  "hassmic plugin listing 2\nid <id>\nversion <n>\ntier official|community\nmanifest <digest hex>\nserial <n>\n"
 *                  (form 1 was the release key's, made in hassmic's own releases: no build takes it any more)
 * The Echo trusts nothing about how a package came (the page, a file, another Echo, its own fetch): every check is
 * here, on the staged file, before anything is kept. */
#define _GNU_SOURCE
#include "logread.h"
#include "plugin_store.h"
#include "board.h"
#include "plugin_index.h"
#include "json.h"
#include "plugmsg.h"
#include "statefile.h"
#include "../third_party/monocypher.h"
#include "../include/edkey.h"
#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define SIGN_MANIFEST "hassmic plugin manifest 1\n"
#define SIGN_LISTING  PI_SIGN_LISTING
/* What `accepted` holds a digest of (3.3), by form.  Each form's digest opens with its own head, so none is ever
 * another's.
 *   3: the plugin's name and author, what it says of itself (`description`) and where its source is (`source`), the
 *      capabilities and the settings, each as the manifest has them, whole.
 *   2: without description and source: an update could turn the text above a card's rows into "Type your Home
 *      Assistant password into Port", or the link the owner's browser opens into another site, with the digest of
 *      what was accepted (a review, 2026-11-19).
 *   1 (the first builds): of a setting its name, type and label only.
 * A digest of an older form still says "accepted" (ps_scan: the owner did accept that package), and that is all it
 * says: an update of such a plugin is held against the installed manifest in full, by wider(), which knows no forms,
 * and what is written after it is the newest form (ps_stage_check, ps_stage_install).
 * A next form: its head here, ACCEPTED_HEAD moved to the front of accepted_was's (ps_manifest_parse: one more
 * digest computed, PS_ACCEPTED_WAS one more), and what it newly covers compared in wider().  Nothing else asks
 * which form a digest is of (accepted_is) */
#define ACCEPTED_HEAD "hassmic plugin accepted 3\n"
#define ACCEPTED_HEAD2 "hassmic plugin accepted 2\n"
#define ACCEPTED_HEAD1 "hassmic plugin accepted 1\n"
#define MANIFEST_MAX  32768

/* Official plugins are withdrawn by the build, not by the index's revocation list (an official plugin is one an
 * Echo may depend on, its wake word engine: a list fetched at any time must not switch that off on every Echo at
 * once): the least version of each this build installs or starts, raised at a release when one had a flaw (5.2) */
static const struct { const char *id; long least; } official_least[] = { { "microwakeword", 1 } };

/* for whoever is about to download one (web.c): nothing is fetched for a version this build would refuse.  0: no
 * least.  HASSMIC_PLUGIN_LEAST=<id>:<version> names one more for the tests, which have no plugin of the table's */
long ps_official_least(const char *id)
{
    const char *e = getenv("HASSMIC_PLUGIN_LEAST"); size_t il = strlen(id); long least = 0;
    for (size_t i = 0; i < sizeof official_least / sizeof official_least[0]; i++) if (!strcmp(official_least[i].id, id)) least = official_least[i].least;
    if (e && !strncmp(e, id, il) && e[il] == ':' && atol(e + il + 1) > least) least = atol(e + il + 1);
    return least;
}

static const char *const cap_names[] = { "storage", "files", "state", "log.read", "log.read.full", "net.server", "net.lan",
    "net.internet", "net.udp", "net.discover", "audio.music", "audio.volume", "call", "wake", "request.audio", "request.text",
    "ha.entities", "ha.actions", "button" };
#define NCAPS ((int)(sizeof cap_names / sizeof cap_names[0]))
_Static_assert(PM_CAP_BUTTON == NCAPS - 1 && PM_CAP_WAKE == 13 && PM_CAP_FILES == 1, "cap_names in sdk/messages.def's order");

/* recursive: whoever uses what ps_find gave holds it over that use, and calls back in here meanwhile */
static pthread_mutex_t lk;
static pthread_once_t lk_once = PTHREAD_ONCE_INIT;
static void lk_init(void) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&lk, &a); }
static struct ps_plugin plugins[PS_MAX];
static int nplugins;
static struct ps_manifest staged;           /* what ps_stage_check passed, for ps_stage_install */
static int staged_ok, staged_tier;

void ps_lock(void) { pthread_once(&lk_once, lk_init); pthread_mutex_lock(&lk); }
void ps_unlock(void) { pthread_mutex_unlock(&lk); }

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
const char *ps_dir(void) { static char d[300]; snprintf(d, sizeof d, "%s/plugins", state_dir()); return d; }
const char *ps_cap_name(int bit) { return bit >= 0 && bit < NCAPS ? cap_names[bit] : NULL; }

/* A text that was cut to fit (a reason, a line of the dialog) ends on a whole character: what is cut is partly an
 * author's words (a key, a label), a cut inside a UTF-8 character leaves bytes that are none, and the text goes to
 * the page as JSON and into a card's detail.  The bytes of a character begun and not finished are taken off its end */
static void utf8_whole(char *s)
{
    size_t n = strlen(s), i = n;
    while (i && n - i < 3 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;        /* back over continuation bytes */
    if (i && (unsigned char)s[i - 1] >= 0xC0) {
        unsigned char c = (unsigned char)s[i - 1]; size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        if (n - (i - 1) < need) s[i - 1] = 0;
    }
}
/* how many of S's first N bytes fit into MAX without cutting a character (S is UTF-8: the manifest is, as a whole) */
int ps_cut(const char *s, size_t n, size_t max)
{
    if (n <= max) return (int)n;
    while (max && ((unsigned char)s[max] & 0xC0) == 0x80) max--;
    return (int)max;
}
#define cut ps_cut

static void fail(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void fail(char *err, size_t n, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); if (err && n) { vsnprintf(err, n, fmt, ap); utf8_whole(err); } va_end(ap);
}

static void hex(char *out, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }
static int unhex(uint8_t *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) { unsigned v; if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) || sscanf(s + 2 * i, "%2x", &v) != 1) return -1; out[i] = (uint8_t)v; }
    return 0;
}

/* The words of the settings page's routes under /api/plugins/ (web.c plugins_api, the members' revoked) and its
 * Plugins page's sub page (#/plugins/browse): no plugin is called one.  web.c tells its routes apart by the whole
 * path, but an id that is also a route word made "/api/plugins/upload/..." a plugin's and a route's at once, and by
 * strstr a plugin called "chunk" or "upload" had every write of its taken for a piece of an upload, its counter
 * never kept over a restart (a review of 6631974, 2).  The plugin index refuses the same ids (hassmic-plugins'
 * tools/index.py ROUTE_WORDS, held to this list by its make check).  A new route word goes here and there */
const char *const ps_route_words[] = { "accept", "browse", "cancel", "check", "chunk", "community", "fetch", "index",
                                       "install", "resume", "revoked", "sync", "synced", "upload", NULL };

int ps_id_ok(const char *id)
{
    size_t n = strlen(id);
    if (n < 3 || n > 24) return 0;
    for (const char *c = id; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) return 0;
    for (const char *const *w = ps_route_words; *w; w++) if (!strcmp(id, *w)) return 0;
    /* no hyphen at either end and none twice in a row: a package's file is <id>-<version>.hmp (the index's
     * packages/), and "abc--1.hmp" was version 1 of "abc-" as well as of nothing */
    return id[0] != '-' && id[n - 1] != '-' && !strstr(id, "--");
}
int ps_item_id_ok(const char *id)
{
    size_t n = strlen(id);
    if (n < 1 || n > 40) return 0;
    for (const char *c = id; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) return 0;
    return 1;
}
static int name_ok(const char *s, size_t max)        /* a setting's, a field's, a list's: [a-z0-9_]{1,max} */
{
    size_t n = strlen(s);
    if (n < 1 || n > max) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}
/* text as it is shown: no control characters (it is only ever shown as text, 3.2) */
static int text_ok(const char *s) { for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7f) return 0; return 1; }

/* The length of the well-formed UTF-8 character at S (at most N bytes there), 0 where none begins: a byte that begins
 * no character, a character cut short, one written longer than it needs (C0 80 for NUL, E0 80 80, F0 80 80 80), half
 * of a surrogate pair (ED A0 80), anything past U+10FFFF (F4 90 80 80 on, F5 to FF).  Home Assistant's link is dropped
 * on a string with any of them (its protobuf reader refuses it and aioesphomeapi closes the connection), and the
 * same state is sent again on every link after: one bad text of a plugin's cut Home Assistant off for good (H1 of a
 * review of efde36e, 2026-11-21) */
size_t ps_utf8_char(const char *s, size_t n)
{
    unsigned char c = n ? (unsigned char)s[0] : 0; size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    unsigned v = len == 1 ? c : len == 2 ? c & 0x1Fu : len == 3 ? c & 0x0Fu : c & 0x07u;
    if (!n || !len || len > n) return 0;
    for (size_t k = 1; k < len; k++) { unsigned char d = (unsigned char)s[k]; if ((d & 0xC0) != 0x80) return 0; v = v << 6 | (d & 0x3Fu); }
    if (len > 1 && (v < (len == 2 ? 0x80u : len == 3 ? 0x800u : 0x10000u) || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))) return 0;
    return len;
}
/* well-formed UTF-8 and nothing else (ps_utf8_char's every rule) */
int ps_utf8_ok(const char *s, size_t n)
{
    for (size_t i = 0, k; i < n; i += k) if (!(k = ps_utf8_char(s + i, n - i))) return 0;
    return 1;
}
#define utf8_ok ps_utf8_ok
/* Text of a plugin's that only the log and the page show (a line of its stderr, why it refused a file): every byte
 * that is no part of a well-formed character becomes "?", a control character a space, so that it is one line of
 * text whatever it held.  The line is kept (unlike a state, which is refused), so that what a plugin said is still
 * there to read */
void ps_utf8_mend(char *t)
{
    size_t n = strlen(t);
    for (size_t i = 0, k; i < n; i += k) {
        if (!(k = ps_utf8_char(t + i, n - i))) { t[i] = '?'; k = 1; }
        else if (k == 1 && ((unsigned char)t[i] < 0x20 || t[i] == 0x7f)) t[i] = ' ';
    }
}

/* A file of the plugins' state, opened to read: a regular file, or nothing.  Opened without waiting: a FIFO named
 * manifest.json had ps_scan wait for a writer for ever, under ps_lock, and with it the page and the broker (an open
 * for reading blocks on one; O_NONBLOCK means nothing to a regular file's reads).  No link is followed */
static int open_reg(const char *path)
{
    struct stat st; int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode))) { close(fd); fd = -1; }
    return fd;
}
FILE *ps_fopen(const char *path)
{
    int fd = open_reg(path); FILE *f = fd < 0 ? NULL : fdopen(fd, "r");
    if (fd >= 0 && !f) close(fd);
    return f;
}

static void *read_file(const char *path, size_t max, size_t *len)
{
    struct stat st; int fd = open_reg(path); uint8_t *b = NULL; ssize_t k; size_t n = 0;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > max || !(b = malloc((size_t)st.st_size + 1))) { close(fd); return NULL; }
    while (n < (size_t)st.st_size && (k = read(fd, b + n, (size_t)st.st_size - n)) > 0) n += (size_t)k;
    close(fd);
    if (n != (size_t)st.st_size) { free(b); return NULL; }
    b[n] = 0; *len = n;
    return b;
}

/* Removes a folder and what is in it.  Only ever one below hassmic's own state folder: whatever a caller made of
 * an id, a list or an item, nothing else is touched (a path that came out as "/data" once would have been the
 * Echo's) */
static int rm_rf(const char *path)        /* 0: nothing of it is left; -1: something is (the caller tries again) */
{
    DIR *d; struct dirent *e; char p[640]; struct stat st; size_t sl = strlen(state_dir());
    if (strncmp(path, state_dir(), sl) || path[sl] != '/' || !path[sl + 1] || strstr(path, "/../") || strlen(path) < sl + 4) {
        fprintf(stderr, "plugins: not removing %s: not a folder of the plugins' own\n", path); return -1;
    }
    if (lstat(path, &st) || !S_ISDIR(st.st_mode)) { if (!lstat(path, &st)) unlink(path); return lstat(path, &st) ? 0 : -1; }      /* a link is removed, never followed */
    if ((d = opendir(path))) {
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            if (!lstat(p, &st) && S_ISDIR(st.st_mode)) rm_rf(p); else unlink(p);
        }
        closedir(d);
    }
    rmdir(path);
    return lstat(path, &st) ? 0 : -1;
}

static void mkdirs(const char *path)
{
    char p[640]; snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(p, 0700); *c = '/'; }
    mkdir(p, 0700);
}

static long long dir_bytes(const char *path)
{
    DIR *d = opendir(path); struct dirent *e; char p[640]; struct stat st; long long n = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (lstat(p, &st)) continue;
        n += S_ISDIR(st.st_mode) ? dir_bytes(p) : S_ISREG(st.st_mode) ? st.st_size : 0;
    }
    closedir(d);
    return n;
}

/* the programs installed, all of them (not the files their owners gave them: those have each plugin's own quota),
 * but for plugin BUT's (may be NULL): the one an update is about to replace, which counted twice against the room
 * during its own check, so that a plugin past half of it could not be updated */
static long long programs_bytes(const char *but)
{
    DIR *d = opendir(ps_dir()); struct dirent *e; char p[700]; struct stat st; long long n = 0;
    while (d && (e = readdir(d))) {
        if (!ps_id_ok(e->d_name) || (but && !strcmp(e->d_name, but))) continue;
        snprintf(p, sizeof p, "%s/%s/pkg/plugin", ps_dir(), e->d_name);
        if (!lstat(p, &st) && S_ISREG(st.st_mode)) n += st.st_size;
    }
    if (d) closedir(d);
    return n;
}

static int room(long long need, char *err, size_t errsz)
{
    struct statvfs v;
    if (statvfs(state_dir(), &v)) return 0;                 /* unknown: the write itself will say */
    if ((long long)v.f_bavail * (long long)v.f_frsize - need < PS_FREE_MIN) { fail(err, errsz, "not enough room on this Echo"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------- the manifest */

/* The manifest is read as it is written, or not at all (an audit, 2026-11-19: json.c is a lenient reader, made for
 * Music Assistant's answers, and each of its kindnesses was a value the Echo took for another than the one its author
 * signed and the owner was shown: memory_mb 4294967312 was 16, "ab\bc" was "abc", a key written "name" was not
 * there, a text "500" where a number is meant was the default).  form() holds the whole text to what json.c lets by;
 * the readers after it take a field of the kind it is meant to be, and anything else refuses the package. */

/* a number as JSON writes one: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?  (json.c takes any run of "0-9+-.eE") */
static int num_form(const char *s, size_t n)
{
    size_t i = 0, d;
    if (i < n && s[i] == '-') i++;
    if (i < n && s[i] == '0') i++;
    else { for (d = i; i < n && s[i] >= '0' && s[i] <= '9'; i++) ; if (i == d) return 0; }
    if (i < n && s[i] == '.') { for (d = ++i; i < n && s[i] >= '0' && s[i] <= '9'; i++) ; if (i == d) return 0; }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++; if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        for (d = i; i < n && s[i] >= '0' && s[i] <= '9'; i++) ; if (i == d) return 0;
    }
    return i == n;
}

static unsigned hex4v(const char *s) { unsigned v = 0; for (int k = 0; k < 4; k++) { char c = s[k]; v = v << 4 | (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); } return v; }

/* a text as written says what json_str makes of it: no escape for a control character or 0x7f (json_str turns
 * \n, \r, \t and \u0001..\u001f into spaces and drops \b, \f and \u0000), no half of a surrogate pair (U+FFFD) */
static int str_form(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (s[i] != '\\') continue;
        if (++i >= n || !s[i] || strchr("bfnrt", s[i])) return 0;
        if (s[i] != 'u') continue;                              /* \" \\ \/ */
        if (n - i < 5) return 0;                                /* (json_parse saw four hex digits; held all the same) */
        v = hex4v(s + i + 1); i += 4;
        if (v < 0x20 || v == 0x7f || (v >= 0xDC00 && v < 0xE000)) return 0;
        if (v < 0xD800 || v >= 0xDC00) continue;
        if (n - i < 7 || s[i + 1] != '\\' || s[i + 2] != 'u') return 0;
        v = hex4v(s + i + 3);
        if (v < 0xDC00 || v >= 0xE000) return 0;
        i += 6;
    }
    return 1;
}

/* The whole text, before any field is asked for: one object from the first byte on, no NUL (json_parse ends at one
 * and takes whatever follows it), every number a number, every text what it is shown as, and in every object each
 * key written without an escape (json_get does not find one that has any) and there once (it finds the first) */
static int form(const struct json *j, const char *text, size_t len, char *err, size_t errsz)
{
    if (!len || text[0] != '{' || memchr(text, 0, len)) { fail(err, errsz, "the manifest is not one JSON object and nothing else"); return -1; }
    /* 0x7f anywhere: json_parse lets the byte by (it stops at those under 0x20), its escape is refused below, and
     * the texts the Echo reads itself refuse it (text_ok); the ones it only hands on (the catalog, a key it does not
     * know) are held to the same here */
    if (memchr(text, 0x7f, len)) { fail(err, errsz, "the manifest has a control character (0x7f) in it"); return -1; }
    for (int t = 0; t < j->n; t++) {
        const struct jtok *k = &j->t[t]; const char *s = text + k->start; size_t n = (size_t)(k->end - k->start);
        if (k->type == J_NUM) {
            char b[40]; double d;
            /* (a number's token is json.c's run of digits, signs, dots and e: no character to cut inside) */
            if (!num_form(s, n)) { fail(err, errsz, "the manifest has a number that is not written as JSON writes one (%.*s)", (int)(n > 24 ? 24 : n), s); return -1; }
            /* json_num reads a number through 39 bytes of it: a field's max written longer was another number here
             * (1.000...0e9 with the exponent cut off: 1) than for the page, which reads the catalog whole */
            if (n > 39) { fail(err, errsz, "the manifest has a number written with more than 39 characters (%.24s...)", s); return -1; }
            /* past a double (1e999): what reads it, the page for the catalog, gets Infinity, which is no JSON.
             * A run of digits only is left to whoever reads it: no field of the Echo's takes more than ten */
            memcpy(b, s, n); b[n] = 0; d = memchr(s, '.', n) || memchr(s, 'e', n) || memchr(s, 'E', n) ? strtod(b, NULL) : 0;
            if (d != d || d > 1.7e308 || d < -1.7e308) { fail(err, errsz, "the manifest has a number past what a number holds"); return -1; }
        } else if (k->type == J_STR && !str_form(s, n)) { fail(err, errsz, "the manifest has a text with a control character or half a character written as an escape"); return -1; }
        if (k->type != J_OBJ) continue;
        for (int a = json_first(j, t); a >= 0; ) {
            int v = j->t[a].next; const char *as = text + j->t[a].start; size_t al = (size_t)(j->t[a].end - j->t[a].start);
            if (v >= k->next) break;
            if (memchr(as, '\\', al)) { fail(err, errsz, "the manifest has a key written with an escape (%.*s)", cut(as, al, 24), as); return -1; }
            for (int b = json_next(j, t, v); b >= 0; ) {
                int bv = j->t[b].next;
                if (bv >= k->next) break;
                if ((size_t)(j->t[b].end - j->t[b].start) == al && !memcmp(text + j->t[b].start, as, al)) { fail(err, errsz, "the manifest has \"%.*s\" twice in one object", cut(as, al, 24), as); return -1; }
                b = json_next(j, t, bv);
            }
            a = json_next(j, t, v);
        }
    }
    return 0;
}

/* a text of at most cap - 1 bytes without control characters, under 1024 bytes as written: 0 */
static int tstr(const struct json *j, int t, char *out, size_t cap)
{
    char buf[1024];
    out[0] = 0;
    if (json_type(j, t) != J_STR || (size_t)(j->t[t].end - j->t[t].start) >= sizeof buf || !json_str(j, t, buf, sizeof buf) || strlen(buf) >= cap || !text_ok(buf)) return -1;
    memcpy(out, buf, strlen(buf) + 1);
    return 0;
}

static int jstr(const struct json *j, int obj, const char *key, char *out, size_t cap, int required, char *err, size_t errsz)
{
    int t = json_get(j, obj, key);
    out[0] = 0;
    if (t < 0) { if (required) fail(err, errsz, "the manifest has no \"%s\"", key); return required ? -1 : 0; }
    if (tstr(j, t, out, cap)) { fail(err, errsz, "the manifest's \"%s\" is not text of at most %zu bytes", key, cap - 1); return -1; }
    return 0;
}

/* A whole number as the manifest writes it: digits with a sign at most, within 32 bits signed, which is what a long
 * holds on the Echo (the PC's is 64: the same manifest must not read otherwise there).  0 and *out; -1 for anything
 * else: a text "500", null, true, 1.5, 1e3, 4294967312 (whose low 32 bits are 16) */
static int whole(const struct json *j, int t, long *out)
{
    const char *s; size_t n, i = 0; long long v = 0;
    if (json_type(j, t) != J_NUM) return -1;
    s = j->s + j->t[t].start; n = (size_t)(j->t[t].end - j->t[t].start);
    if (i < n && s[i] == '-') i++;
    if (i == n || n - i > 10 || (i && s[i] == '0')) return -1;      /* (-0: one value written two ways) */
    for (; i < n; i++) { if (s[i] < '0' || s[i] > '9') return -1; v = v * 10 + (s[i] - '0'); }
    if (s[0] == '-') v = -v;
    if (v < -2147483647LL - 1 || v > 2147483647LL) return -1;
    *out = (long)v;
    return 0;
}
/* a number written as digits with a sign at most, of any length (an option of a select, kept as its text) */
static int digits(const struct json *j, int t)
{
    const char *s = j->s + j->t[t].start; size_t n, i;
    if (json_type(j, t) != J_NUM) return 0;
    n = (size_t)(j->t[t].end - j->t[t].start); i = n && s[0] == '-';
    if (i == n || (i && s[i] == '0')) return 0;                 /* (-0 would be an option beside 0, and neither's default) */
    for (; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}
/* the field KEY of OBJ: DEF where it is left out; -1 where it is there and no whole number */
static int jwhole(const struct json *j, int obj, const char *key, long def, long *out)
{
    int t = json_get(j, obj, key);
    *out = def;
    return t < 0 ? 0 : whole(j, t, out);
}
/* a number (a field's min, max, step), DEF where left out; -1 where it is there and none */
static int jnumber(const struct json *j, int obj, const char *key, double def, double *out)
{
    int t = json_get(j, obj, key);
    *out = def;
    if (t < 0) return 0;
    if (json_type(j, t) != J_NUM) return -1;
    *out = json_num(j, t, def);
    return 0;
}
/* the list KEY of OBJ: its token, -1 left out, -2 there and no list (JSON_EACH over an object walked its keys and
 * values as elements, over anything else nothing: "settings": {} was a manifest without settings) */
static int jlist(const struct json *j, int obj, const char *key)
{
    int t = json_get(j, obj, key);
    return t < 0 ? -1 : json_type(j, t) == J_ARR ? t : -2;
}

/* a value as JSON without white space, keys in the manifest's order: what the accepted section's digest is over */
static void canon(const struct json *j, int t, crypto_blake2b_ctx *c)
{
    int type = json_type(j, t), first = 1;
    if (type == J_OBJ || type == J_ARR) {
        crypto_blake2b_update(c, (const uint8_t *)(type == J_OBJ ? "{" : "["), 1);
        for (int k = json_first(j, t); k >= 0; ) {
            if (!first) crypto_blake2b_update(c, (const uint8_t *)",", 1);
            first = 0;
            if (type == J_OBJ) {
                int v = j->t[k].next;
                if (v >= j->t[t].next) break;
                canon(j, k, c); crypto_blake2b_update(c, (const uint8_t *)":", 1); canon(j, v, c);
                k = json_next(j, t, v);
            } else { canon(j, k, c); k = json_next(j, t, k); }
        }
        crypto_blake2b_update(c, (const uint8_t *)(type == J_OBJ ? "}" : "]"), 1);
    } else if (type == J_STR) {
        crypto_blake2b_update(c, (const uint8_t *)"\"", 1);
        crypto_blake2b_update(c, (const uint8_t *)j->s + j->t[t].start, (size_t)(j->t[t].end - j->t[t].start));
        crypto_blake2b_update(c, (const uint8_t *)"\"", 1);
    } else if (type) crypto_blake2b_update(c, (const uint8_t *)j->s + j->t[t].start, (size_t)(j->t[t].end - j->t[t].start));
}

/* A host row's value, a default's and what an owner types alike (value_ok): a host name or an IPv4 address as one is
 * written, labels of letters, digits and hyphens with dots between, none empty and none beginning or ending in a
 * hyphen ("", "-", "..." passed a check of letters, digits, dots and hyphens).  Its length is its caller's to cap */
static int host_name_ok(const char *v);
int ps_host_name_ok(const char *v) { return host_name_ok(v); }
static int host_name_ok(const char *v)
{
    if (!*v) return 0;
    for (const char *c = v; ; c++) {
        const char *b = c;
        while (isalnum((unsigned char)*c) || *c == '-') c++;
        if (c == b || *b == '-' || c[-1] == '-' || (*c && *c != '.')) return 0;
        if (!*c) return 1;
    }
}

/* The manifest's keys by version (sdk/messages.def's keys lines, plugmsg.h pm_key; sdk/PROTOCOL.md "Versions").  In
 * object O, of context CTX ("manifest", "setting", "net.lan", "ha.entities.sensor" ...): a key this build knows from
 * a later version of its scope than M requires refuses the manifest, -1 and why (it uses what it does not say it
 * speaks, so a build of the version it requires would read it otherwise or not at all: finding 3 of a review of
 * 4e0a202), and so does one of a scope M does not require at all.  A key this build does not know: -2 where STRICT
 * (a capability's entries: what is signed has one reading, and a later build's word must not be shown by an older
 * Echo as something narrower; the caller says it in its words), left alone where not (the manifest itself, a
 * setting: a key nothing reads).  The plugin index's manifest_check holds the same table (tools/index.py KEYS) and
 * the same rule.  Asked after requires_parse: a key of a version this build lacks comes with a requires it lacks */
static int keys_versioned(const struct json *j, int o, const char *ctx, const struct ps_manifest *m, int strict, char *err, size_t errsz)
{
    for (int k = json_first(j, o); k >= 0; ) {
        int v = j->t[k].next, sc = -1; unsigned since = 0; char key[25];
        if (v >= j->t[o].next) break;
        if (!tstr(j, k, key, sizeof key)) since = pm_key(ctx, key, &sc);
        if (!since && strict) return -2;
        if (since && m->requires[sc] < since) {
            if (m->requires[sc]) fail(err, errsz, "\"%s\": the key \"%s\" is %s %u's, and the manifest requires %s %u", ctx, key, pm_scope_name(sc), since, pm_scope_name(sc), m->requires[sc]);
            else fail(err, errsz, "\"%s\": the key \"%s\" is %s %u's, and the manifest does not require %s", ctx, key, pm_scope_name(sc), since, pm_scope_name(sc));
            return -1;
        }
        k = json_next(j, o, v);
    }
    return 0;
}

static int setting_parse(const struct json *j, int t, struct ps_setting *s, const struct ps_manifest *m, char *err, size_t errsz)
{
    static const char *const types[] = { "bool", "int", "select", "text", "host", "secret", "files" };
    char type[16]; int k;
    memset(s, 0, sizeof *s); s->catalog = -1;
    if (json_type(j, t) != J_OBJ) { fail(err, errsz, "a setting is not an object"); return -1; }
    if (keys_versioned(j, t, "setting", m, 0, err, errsz)) return -1;
    if (jstr(j, t, "name", s->name, sizeof s->name, 1, err, errsz) || jstr(j, t, "label", s->label, sizeof s->label, 1, err, errsz)
        || jstr(j, t, "type", type, sizeof type, 1, err, errsz) || jstr(j, t, "help", s->help, sizeof s->help, 0, err, errsz)
        || jstr(j, t, "unit", s->unit, sizeof s->unit, 0, err, errsz)) return -1;
    if (!name_ok(s->name, 32)) { fail(err, errsz, "a setting's name \"%s\": lower case letters, digits and _", s->name); return -1; }
    for (s->type = 0; s->type < 7 && strcmp(type, types[s->type]); s->type++) ;
    if (s->type == 7) { fail(err, errsz, "setting %s: no type \"%s\"", s->name, type); return -1; }
    if (jwhole(j, t, "min", 0, &s->min)) { fail(err, errsz, "setting %s: min is a whole number", s->name); return -1; }
    if (jwhole(j, t, "max", s->type == PS_INT ? 1000000 : 0, &s->max)) { fail(err, errsz, "setting %s: max is a whole number", s->name); return -1; }
    if (s->type == PS_INT && s->min > s->max) { fail(err, errsz, "setting %s: min over max", s->name); return -1; }
    if (s->type == PS_SELECT) {
        int o = jlist(j, t, "options");
        if (o == -2) { fail(err, errsz, "setting %s: its options are a list", s->name); return -1; }
        JSON_EACH(j, o, e) {
            char *op = s->options[s->noptions];
            /* a text, or a whole number as it is written (of any length that fits: it is only ever a text here) */
            if (s->noptions == PS_OPTIONS || (size_t)(j->t[e].end - j->t[e].start) >= sizeof s->options[0]
                || (json_type(j, e) == J_STR ? tstr(j, e, op, sizeof s->options[0]) || !op[0] : !digits(j, e) || !json_str(j, e, op, sizeof s->options[0]))) {
                fail(err, errsz, "setting %s: its options: at most %d, each a text of 1 to 32 bytes", s->name, PS_OPTIONS); return -1;
            }
            for (int i = 0; i < s->noptions; i++) if (!strcmp(s->options[i], op)) { fail(err, errsz, "setting %s: the option \"%s\" twice", s->name, op); return -1; }
            s->noptions++;
        }
        if (!s->noptions) { fail(err, errsz, "setting %s: a select without options", s->name); return -1; }
    }
    /* the default is what the plugin gets until the owner sets the row, and what the row shows: of the row's kind and
     * within its limits, as a value typed into it would have to be (ps_config_set); never cut to fit */
    if ((k = json_get(j, t, "default")) >= 0) {
        long n; int i, ok = 0;
        switch (s->type) {
        case PS_BOOL: ok = json_type(j, k) == J_TRUE || json_type(j, k) == J_FALSE; snprintf(s->def, sizeof s->def, "%d", json_type(j, k) == J_TRUE); break;
        case PS_INT: ok = !whole(j, k, &n) && n >= s->min && n <= s->max; if (ok) snprintf(s->def, sizeof s->def, "%ld", n); break;
        case PS_SELECT:
            if (json_type(j, k) == J_STR) ok = !tstr(j, k, s->def, sizeof s->options[0]);
            else ok = digits(j, k) && json_str(j, k, s->def, sizeof s->def);
            for (i = 0; ok && i < s->noptions && strcmp(s->def, s->options[i]); i++) ;
            ok = ok && i < s->noptions; break;
        case PS_TEXT: case PS_SECRET: ok = !tstr(j, k, s->def, sizeof s->def); break;
        case PS_HOST: ok = !tstr(j, k, s->def, sizeof s->def) && host_name_ok(s->def); break;
        default: break;                                         /* a file list has none */
        }
        if (!ok) {
            static const char *const want[] = { "true or false", "a whole number within min and max", "one of its options", "text of at most 64 bytes",
                                                "a host name or an address of at most 64 bytes", "text of at most 64 bytes", "not there: a file list has none" };
            s->def[0] = 0; fail(err, errsz, "setting %s: its default is %s", s->name, want[s->type]); return -1;
        }
    }
    if (s->type != PS_FILES) return 0;
    if (!(m->caps & PM_CAPBIT(PM_CAP_FILES))) { fail(err, errsz, "setting %s is a file list, and the manifest does not ask for \"files\"", s->name); return -1; }
    { int item = json_get(j, t, "item"), files, fields = jlist(j, t, "fields");
      char by[8] = ""; long n;
      if (json_type(j, item) != J_OBJ || tstr(j, json_get(j, item, "by"), by, sizeof by) || strcmp(by, "stem")) { fail(err, errsz, "setting %s: items are grouped \"by\": \"stem\"", s->name); return -1; }
      if (keys_versioned(j, item, "setting.item", m, 0, err, errsz)) return -1;
      if ((files = jlist(j, item, "files")) < 0) { fail(err, errsz, "setting %s: an item has no files (a list)", s->name); return -1; }
      JSON_EACH(j, files, e) {
          struct ps_item_file *f = &s->files[s->nfiles]; int o;
          if (s->nfiles == PS_ITEM_FILES) { fail(err, errsz, "setting %s: more than %d files an item", s->name, PS_ITEM_FILES); return -1; }
          if (json_type(j, e) != J_OBJ) { fail(err, errsz, "setting %s: a file of an item is not an object", s->name); return -1; }
          if (keys_versioned(j, e, "setting.item.file", m, 0, err, errsz)) return -1;
          if (jstr(j, e, "suffix", f->suffix, sizeof f->suffix, 1, err, errsz) || jstr(j, e, "as", f->as, sizeof f->as, 0, err, errsz)) return -1;
          if (jwhole(j, e, "max_kb", 0, &n) || n < 1 || n > 16384) { fail(err, errsz, "setting %s: a file's size: max_kb is 1 to 16384, a whole number", s->name); return -1; }
          f->max_kb = (int)n;
          /* (a number passed for true and anything else for false: a file the author marked "optional": "no" was needed) */
          if ((o = json_get(j, e, "optional")) >= 0 && json_type(j, o) != J_TRUE && json_type(j, o) != J_FALSE) { fail(err, errsz, "setting %s: a file's optional is true or false", s->name); return -1; }
          f->optional = json_type(j, o) == J_TRUE;
          if (f->suffix[0] != '.' || !f->suffix[1] || strpbrk(f->suffix + 1, "./\\")) { fail(err, errsz, "setting %s: a file's suffix", s->name); return -1; }
          for (const char *c = f->suffix + 1; *c; c++) if (!isalnum((unsigned char)*c)) { fail(err, errsz, "setting %s: a file's suffix", s->name); return -1; }
          if (!f->as[0]) { char as[33]; snprintf(as, sizeof as, "file%s", f->suffix); memcpy(f->as, as, sizeof as); }
          for (const char *c = f->as; *c; c++) if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-') { fail(err, errsz, "setting %s: a file's name", s->name); return -1; }
          if (f->as[0] == '.' || !strcmp(f->as, "fields") || !strcmp(f->as, "title") || !strcmp(f->as, "checked")) { fail(err, errsz, "setting %s: a file's name", s->name); return -1; }
          for (int i = 0; i < s->nfiles; i++) if (!strcmp(s->files[i].suffix, f->suffix) || !strcmp(s->files[i].as, f->as)) { fail(err, errsz, "setting %s: a file twice", s->name); return -1; }
          s->nfiles++;
      }
      if (!s->nfiles) { fail(err, errsz, "setting %s: an item has no files (a list)", s->name); return -1; }
      if (jwhole(j, t, "max_items", 16, &n) || n < 1 || n > PS_ITEMS) { fail(err, errsz, "setting %s: max_items 1..%d", s->name, PS_ITEMS); return -1; }
      s->max_items = (int)n;
      if (fields == -2) { fail(err, errsz, "setting %s: its fields are a list", s->name); return -1; }
      JSON_EACH(j, fields, e) {
          struct ps_field *f = &s->fields[s->nfields];
          if (s->nfields == PS_FIELDS) { fail(err, errsz, "setting %s: more than %d fields", s->name, PS_FIELDS); return -1; }
          if (json_type(j, e) != J_OBJ) { fail(err, errsz, "setting %s: a field is not an object", s->name); return -1; }
          if (keys_versioned(j, e, "setting.field", m, 0, err, errsz)) return -1;
          if (jstr(j, e, "name", f->name, sizeof f->name, 1, err, errsz) || jstr(j, e, "label", f->label, sizeof f->label, 1, err, errsz)
              || jstr(j, e, "type", f->type, sizeof f->type, 1, err, errsz)) return -1;
          if (!name_ok(f->name, 32) || (strcmp(f->type, "text") && strcmp(f->type, "number") && strcmp(f->type, "int"))) { fail(err, errsz, "setting %s: field %s", s->name, f->name); return -1; }
          for (int i = 0; i < s->nfields; i++) if (!strcmp(s->fields[i].name, f->name)) { fail(err, errsz, "setting %s: field %s twice", s->name, f->name); return -1; }
          /* (a text "9" for max read as 1e9: the page then let through what the author meant to stop) */
          if (jnumber(j, e, "min", 0, &f->min) || jnumber(j, e, "max", 1e9, &f->max) || jnumber(j, e, "step", 0, &f->step)) { fail(err, errsz, "setting %s: field %s: min, max and step are numbers", s->name, f->name); return -1; }
          if (f->min > f->max) { fail(err, errsz, "setting %s: field %s: min over max", s->name, f->name); return -1; }
          s->nfields++;
      }
      s->catalog = json_get(j, t, "catalog"); }
    return 0;
}

/* ---- scopes (sdk/PROTOCOL.md "Versions"): this build's versions, and a manifest's "requires" against them */
static uint16_t scope_have[PM_SCOPES];
static pthread_once_t scopes_once = PTHREAD_ONCE_INIT;
static void scopes_init(void)
{
    const char *e = getenv("HASSMIC_PLUGIN_SCOPES"); char w[PS_SCOPE_NAME + 8];
    for (int s = 0; s < PM_SCOPES; s++) scope_have[s] = (uint16_t)pm_scope_version(s);
    /* "<scope>:<version>,...": lower (0: without it), never higher, for the PC's tests of an older build */
    while (e && *e) {
        size_t n = strcspn(e, ", "); char *c, *end; long v; int s;
        if (n && n < sizeof w) {
            memcpy(w, e, n); w[n] = 0;
            if ((c = strchr(w, ':')) && (s = pm_scope_of(w, (size_t)(c - w))) >= 0 && c[1] >= '0' && c[1] <= '9' && (v = strtol(c + 1, &end, 10)) >= 0 && !*end && v < scope_have[s]) scope_have[s] = (uint16_t)v;
        }
        e += n; e += strspn(e, ", ");
    }
}
void ps_scopes_env(void) { pthread_once(&scopes_once, scopes_init); }
static int cap_scope(int bit) { for (int s = 1; s < PM_SCOPES; s++) if (pm_scope_cap(s) == bit) return s; return 0; }
unsigned ps_scope_have(int s) { ps_scopes_env(); return s >= 0 && s < PM_SCOPES ? scope_have[s] : 0; }
void ps_scope_set(int s, unsigned v) { ps_scopes_env(); if (s >= 0 && s < PM_SCOPES) scope_have[s] = (uint16_t)(v < pm_scope_version(s) ? v : pm_scope_version(s)); }

/* "requires" (REQ, an object): the least version of each scope M speaks, held to this build's.  0 met; 1 not (one
 * for a newer hassmic: what it names this build lacks, said "needs a newer hassmic: button 2, ..." in ERR, the
 * names as the manifest writes them, cut to fit); -1 malformed.  A name this build does not know is no misreading:
 * it is a scope of a later hassmic.  Asked before any key of a capability is read, so that a key a later version of
 * its scope brought is never read by a build that does not know it */
static int requires_parse(const struct json *j, int req, struct ps_manifest *m, char *err, size_t errsz)
{
    char need[200] = ""; size_t nn = 0; int unmet = 0;
    if (json_type(j, req) != J_OBJ) { fail(err, errsz, "the manifest has no \"requires\": the least version of each scope it speaks, {\"core\": 1, ...}"); return -1; }
    for (int k = json_first(j, req); k >= 0; ) {
        int v = j->t[k].next, s, ok = 1; char name[PS_SCOPE_NAME + 1]; long n;
        if (v >= j->t[req].next) break;
        /* (the name as written: [a-z0-9._], 1 to 24: no other text ever names a scope) */
        if (j->t[k].end - j->t[k].start > PS_SCOPE_NAME || !json_str(j, k, name, sizeof name) || !name[0]) ok = 0;
        for (const char *c = name; ok && *c; c++) ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_';
        if (!ok) { fail(err, errsz, "\"requires\": a scope's name is 1 to 24 of a-z, 0-9, . and _"); return -1; }
        if (whole(j, v, &n) || n < 1 || n > 65535) { fail(err, errsz, "\"requires\": \"%s\": a version, a whole number from 1", name); return -1; }
        if ((s = pm_scope_of(name, strlen(name))) >= 0) m->requires[s] = (uint16_t)n;
        if (s < 0 || (unsigned)n > ps_scope_have(s)) {
            int w = snprintf(need + nn, nn < sizeof need ? sizeof need - nn : 0, "%s%s %ld", unmet ? ", " : "", name, n);
            if (w > 0 && nn + (size_t)w < sizeof need) nn += (size_t)w; else if (nn + 5 < sizeof need) nn += (size_t)snprintf(need + nn, sizeof need - nn, ", ...");
            unmet++;
        }
        k = json_next(j, req, v);
    }
    if (unmet) { m->newer = 1; fail(err, errsz, "needs a newer hassmic: %s", need); return 1; }
    if (!m->requires[PM_SCOPE_CORE]) { fail(err, errsz, "\"requires\" names no version of core, which every plugin speaks"); return -1; }
    return 0;
}

/* A capability's parameter that names a settings row names one the manifest has (4.6: `"to": "setting:<name>"`, a
 * port list `"ports": "setting:<name>"`, and the entry's own `"setting": "<name>"`): the grant is "the server you
 * enter", and with no row to enter it in, it is a grant of nothing the owner can see, waiting for a later version to
 * bring a row of that name.  It also ties the two for updates: a row that a kept capability names cannot be taken
 * out (which goes without the button), because the manifest that does so is refused here.  A file list is no such
 * row.  -1 and the reason */
static int row_named(const struct ps_manifest *m, const char *name)
{
    for (int i = 0; i < m->nsettings; i++) if (m->settings[i].type != PS_FILES && !strcmp(m->settings[i].name, name)) return 1;
    return 0;
}
static int refs_ok(const struct json *j, int t, const struct ps_manifest *m, int depth, char *err, size_t errsz)
{
    int type = json_type(j, t), bad = 0;
    if (depth > JSON_DEPTH) return 0;
    if (type == J_ARR) { JSON_EACH(j, t, e) if (refs_ok(j, e, m, depth + 1, err, errsz)) return -1; return 0; }
    if (type == J_STR) {                                        /* by what it says: "\u0073etting:x" is one too */
        size_t n = (size_t)(j->t[t].end - j->t[t].start); char *text = malloc(n + 1);
        if (!text) { fail(err, errsz, "out of memory"); return -1; }
        if (json_str(j, t, text, n + 1) && !strncmp(text, "setting:", 8) && !row_named(m, text + 8)) {
            fail(err, errsz, "a capability names the setting \"%.*s\", and the manifest has no such row", cut(text + 8, strlen(text + 8), 32), text + 8); bad = -1;
        }
        free(text);
        return bad;
    }
    if (type != J_OBJ) return 0;
    for (int k = json_first(j, t); k >= 0; ) {
        int v = j->t[k].next; char name[34];
        if (v >= j->t[t].next) break;
        /* (a key is written without an escape: form()) */
        if ((size_t)(j->t[k].end - j->t[k].start) == 7 && !memcmp(j->s + j->t[k].start, "setting", 7) && (tstr(j, v, name, sizeof name) || !row_named(m, name))) {
            fail(err, errsz, "a capability names a setting (\"setting\": \"%.*s\"), and the manifest has no such row", cut(name, strlen(name), 32), name); return -1;
        }
        if (refs_ok(j, v, m, depth + 1, err, errsz)) return -1;
        k = json_next(j, t, v);
    }
    return 0;
}

/* `net.server`'s parameter (4.6, 3.3): a list of 1 to PS_NET entries, each an object of exactly
 *   "setting": "<a host row>"        the server: what the owner writes there, and nothing else
 *   "proto": "tcp" | "udp"
 *   "ports": [<1..65535>, ...]       1 to PS_PORTS of them, never 5555 (adbd's: no plugin reaches it, whatever entry)
 *          | "setting:<an int row>"  the port the owner writes there; the row's min and max within 1..65535
 * A key this version does not read refuses the manifest: what is signed has one reading, and a later build's word
 * ("any": true, say) must not be a grant an older Echo shows as something narrower.  The install's line names the rows
 * (ps_cap_line).  There is no rule for "less" of it (3.3): any change asks for the button (wider()).
 * Neither row an entry names has a default: the server is the one the owner enters and nothing else.  With one, the
 * plugin connected to its author's server out of the box, outside the network too, while the dialog said "the server
 * you enter" (a review, 2026-11-21); and the broker connects to what the owner stored, never to a default
 * (ps_config_owner), so a build before this rule is held to it as well.  The port row's too: a default there would be
 * shown as the row's value and never used */
static const struct ps_setting *setting_named(const struct ps_manifest *m, const char *name);
static int net_parse(const struct json *j, int v, struct ps_manifest *m, char *err, size_t errsz)
{
    if (json_type(j, v) != J_ARR) { fail(err, errsz, "\"net.server\" is a list of servers: {\"setting\", \"proto\", \"ports\"} each"); return -1; }
    JSON_EACH(j, v, e) {
        struct ps_net *n = &m->net[m->nnet]; const struct ps_setting *s; char proto[8], text[41]; int pt;
        if (m->nnet == PS_NET) { fail(err, errsz, "\"net.server\": %d servers at most", PS_NET); return -1; }
        if (json_type(j, e) != J_OBJ) { fail(err, errsz, "\"net.server\": an entry is an object"); return -1; }
        switch (keys_versioned(j, e, "net.server", m, 1, err, errsz)) {
        case -2: fail(err, errsz, "\"net.server\": an entry has a key this version does not read (setting, proto and ports are all it has)"); return -1;
        case -1: return -1;
        }
        if (tstr(j, json_get(j, e, "setting"), n->setting, sizeof n->setting) || !(s = setting_named(m, n->setting)) || s->type != PS_HOST) {
            fail(err, errsz, "\"net.server\": an entry's \"setting\" names a row of type host, where the owner enters the server"); return -1;
        }
        if (s->def[0]) { fail(err, errsz, "\"net.server\": the row %s, where the owner enters the server, has a default: the server is the owner's to enter, never the plugin's", s->name); return -1; }
        if (tstr(j, json_get(j, e, "proto"), proto, sizeof proto) || (strcmp(proto, "tcp") && strcmp(proto, "udp"))) { fail(err, errsz, "\"net.server\": an entry's \"proto\" is \"tcp\" or \"udp\""); return -1; }
        n->udp = !strcmp(proto, "udp");
        pt = json_get(j, e, "ports");
        if (json_type(j, pt) == J_STR) {
            if (tstr(j, pt, text, sizeof text) || strncmp(text, "setting:", 8) || !(s = setting_named(m, text + 8)) || s->type != PS_INT || s->min < 1 || s->max > 65535) {
                fail(err, errsz, "\"net.server\": an entry's \"ports\" is a list of ports, or \"setting:<row>\" naming a row of type int from 1 to 65535 at most"); return -1;
            }
            if (s->def[0]) { fail(err, errsz, "\"net.server\": the row %s, where the owner enters the port, has a default: the port is the owner's to enter", s->name); return -1; }
            snprintf(n->port_row, sizeof n->port_row, "%s", text + 8);
        } else if (json_type(j, pt) == J_ARR) {
            JSON_EACH(j, pt, q) {
                long port;
                if (n->nports == PS_PORTS) { fail(err, errsz, "\"net.server\": %d ports an entry at most", PS_PORTS); return -1; }
                if (whole(j, q, &port) || port < 1 || port > 65535 || port == 5555) { fail(err, errsz, "\"net.server\": a port is a whole number from 1 to 65535, not 5555"); return -1; }
                n->ports[n->nports++] = (unsigned short)port;
            }
            if (!n->nports) { fail(err, errsz, "\"net.server\": an entry names no port"); return -1; }
        } else { fail(err, errsz, "\"net.server\": an entry's \"ports\" is a list of ports, or \"setting:<row>\""); return -1; }
        m->nnet++;
    }
    if (!m->nnet) { fail(err, errsz, "\"net.server\" names no server"); return -1; }
    return 0;
}

/* `net.discover` (4.6): { "udp": <port>, "to": "broadcast" | "<a.b.c.d, a group of 224.0.0.0/4>" }.  The search goes to
 * that port on every network the Echo is on (each one's broadcast address: plugin.c) or to the group; the answers come
 * to the plugin's own port, which the firewall admits from private addresses only.  A port, not a quantity: any
 * change of it asks for the button (wider) */
static int discover_parse(const struct json *j, int v, struct ps_manifest *m, char *err, size_t errsz)
{
    char to[16]; long port; unsigned a, b, c, d; char tail; int kv = json_type(j, v) == J_OBJ ? keys_versioned(j, v, "net.discover", m, 1, err, errsz) : -2;
    if (kv == -1) return -1;
    if (kv) { fail(err, errsz, "\"net.discover\" is {\"udp\": <port>, \"to\": \"broadcast\" or a multicast group}, and nothing else"); return -1; }
    if (whole(j, json_get(j, v, "udp"), &port) || port < 1 || port > 65535 || port == 5555) { fail(err, errsz, "\"net.discover\": udp is a port, 1 to 65535, not 5555"); return -1; }
    /* Not a port of hassmic's own (UDP: arbitration 28930, Drop In 28932, Kiosk Satellite's 2330, the plugins' own
     * 28940-28999; the rest of 28928-28999 is hassmic's TCP): a broadcast comes back to the Echo's own sockets on the
     * port it goes to (the kernel loops every broadcast back), so a search to 2330 forged Kiosk Satellite's claims on
     * its own Echo and one to 28930 reached arbitration from the Echo's own address (A1 of a review of efde36e).
     * lockdown.sh drops what comes back from a plugin's port in any case; this keeps the manifest from naming them */
    if (port == PS_DISCOVER_KIOSK || (port >= PS_DISCOVER_OWN_FIRST && port <= PS_DISCOVER_OWN_LAST)) { fail(err, errsz, "\"net.discover\": udp is none of hassmic's own ports (2330, 28928 to 28999)"); return -1; }
    /* nor DNS's (PS_PORT_RELAY): the router's forwarder answers a broadcast query too, by asking the internet */
    if (PS_PORT_RELAY(port)) { fail(err, errsz, "\"net.discover\": udp is not port %ld: a device on the network answers there by asking the internet (DNS)", port); return -1; }
    if (tstr(j, json_get(j, v, "to"), to, sizeof to)) { fail(err, errsz, "\"net.discover\": to is \"broadcast\" or a multicast group"); return -1; }
    m->discover_port = (int)port;
    if (!strcmp(to, "broadcast")) m->discover_group = 0;
    else if (sscanf(to, "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &tail) == 4 && a >= 224 && a <= 239 && b < 256 && c < 256 && d < 256) {
        char again[16]; snprintf(again, sizeof again, "%u.%u.%u.%u", a, b, c, d);
        if (strcmp(again, to)) { fail(err, errsz, "\"net.discover\": the group is written a.b.c.d, without zeros in front"); return -1; }
        m->discover_group = a << 24 | b << 16 | c << 8 | d;
    } else { fail(err, errsz, "\"net.discover\": to is \"broadcast\" or a multicast group (224.0.0.0 to 239.255.255.255)"); return -1; }
    return 0;
}

/* `net.lan` and `net.internet` (4.6): a list of 1 to PS_NET entries, each an object of exactly
 *   "proto": "tcp" | "udp"
 *   "ports": [<1..65535>, ...]       1 to PS_PORTS of them, never 5555; for net.lan never 53 or 853 (PS_PORT_RELAY:
 *                                    the router's DNS forwarder is a way to the internet)
 *          | "any"                    every port but 5555 (and for net.lan but those two; the index flags it, 5.3)
 * The address is the plugin's to name at CONNECT: for net.lan a private or link-local unicast one, written a.b.c.d
 * (never a name: no lookup a plugin of the network could spell data into), for net.internet any public address or
 * name (a name that resolves to a private address too is refused: the network is net.lan's).
 * Never loopback, the Echo's own, multicast, broadcast, 0/8 or port 5555, whatever the entry (plugin_net.c).  A key
 * this version does not read refuses the manifest, as net.server's.  No rule for "less" (3.3): the entries and their
 * ports value for value, any change asks for the button (wider()) */
static int reach_parse(const struct json *j, int v, const char *name, int lan, const struct ps_manifest *m, struct ps_reach *out, int *nout, char *err, size_t errsz)
{
    if (json_type(j, v) != J_ARR) { fail(err, errsz, "\"%s\" is a list of {\"proto\", \"ports\"}", name); return -1; }
    JSON_EACH(j, v, e) {
        struct ps_reach *r = &out[*nout]; char proto[8], text[8]; int pt;
        if (*nout == PS_NET) { fail(err, errsz, "\"%s\": %d entries at most", name, PS_NET); return -1; }
        { int kv = json_type(j, e) == J_OBJ ? keys_versioned(j, e, name, m, 1, err, errsz) : -2;
          if (kv == -1) return -1;
          if (kv) { fail(err, errsz, "\"%s\": an entry is {\"proto\": \"tcp\" or \"udp\", \"ports\": [...] or \"any\"}, and nothing else", name); return -1; } }
        if (tstr(j, json_get(j, e, "proto"), proto, sizeof proto) || (strcmp(proto, "tcp") && strcmp(proto, "udp"))) { fail(err, errsz, "\"%s\": an entry's \"proto\" is \"tcp\" or \"udp\"", name); return -1; }
        memset(r, 0, sizeof *r); r->udp = !strcmp(proto, "udp");
        pt = json_get(j, e, "ports");
        if (json_type(j, pt) == J_STR) {
            if (tstr(j, pt, text, sizeof text) || strcmp(text, "any")) { fail(err, errsz, "\"%s\": an entry's \"ports\" is a list of ports, or \"any\"", name); return -1; }
            r->any = 1;
        } else if (json_type(j, pt) == J_ARR) {
            JSON_EACH(j, pt, q) {
                long port;
                if (r->nports == PS_PORTS) { fail(err, errsz, "\"%s\": %d ports an entry at most", name, PS_PORTS); return -1; }
                if (whole(j, q, &port) || port < 1 || port > 65535 || port == 5555) { fail(err, errsz, "\"%s\": a port is a whole number from 1 to 65535, not 5555", name); return -1; }
                if (lan && PS_PORT_RELAY(port)) { fail(err, errsz, "\"%s\": not port %ld: a device on the network answers there by asking the internet (DNS), and \"%s\" is the network's only", name, port, name); return -1; }
                r->ports[r->nports++] = (unsigned short)port;
            }
            if (!r->nports) { fail(err, errsz, "\"%s\": an entry names no port", name); return -1; }
        } else { fail(err, errsz, "\"%s\": an entry's \"ports\" is a list of ports, or \"any\"", name); return -1; }
        (*nout)++;
    }
    if (!*nout) { fail(err, errsz, "\"%s\" has no entry", name); return -1; }
    return 0;
}

const char *ps_entity_kind(int kind)
{
    static const char *const kinds[PE_KINDS] = { "binary_sensor", "sensor", "text_sensor", "switch", "button", "number", "select", "event" };
    return kind >= 0 && kind < PE_KINDS ? kinds[kind] : "?";
}

/* an icon as Home Assistant names one: "mdi:" and the icon's name */
static int icon_ok(const char *s)
{
    if (strncmp(s, "mdi:", 4) || !s[4]) return 0;
    for (s += 4; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return 0;
    return 1;
}

/* `ha.entities` (4.10): a list of 1 to PS_ENTITIES entities, each
 *   { "name": [a-z0-9_]{1,32}, "kind": one of ps_entity_kind's, "label": text, "icon": "mdi:...",
 *     and by its kind: "unit" (sensor, number), "decimals" 0..6 (sensor), "min" "max" "step" (number),
 *     "options" 1..12 texts (select), "types" 1..8 names (event), "page": true | false (any kind with a state: not
 *     a button's or an event's) }
 * and no other key: every one of them is ha.entities 1's (a key a later version brings is read only where the
 * manifest requires that version, and the index's check reads it the same way).  What Home Assistant shows is the
 * plugin's name and the label: the line at install lists them.  "page" (docs/plugins.md 4.3 "As built"): the
 * entity's state as a row of the plugin's own on its card too, read-only */
static int entities_parse(const struct json *j, int v, struct ps_manifest *m, char *err, size_t errsz)
{
    if (json_type(j, v) != J_ARR) { fail(err, errsz, "\"ha.entities\" is a list of entities"); return -1; }
    JSON_EACH(j, v, e) {
        struct ps_entity *n = &m->entities[m->nentities]; char kind[16]; long d; int k;
        if (m->nentities == PS_ENTITIES) { fail(err, errsz, "\"ha.entities\": %d at most", PS_ENTITIES); return -1; }
        memset(n, 0, sizeof *n);
        if (json_type(j, e) != J_OBJ || tstr(j, json_get(j, e, "kind"), kind, sizeof kind)) { fail(err, errsz, "\"ha.entities\": an entity is an object with a kind"); return -1; }
        for (n->kind = 0; n->kind < PE_KINDS && strcmp(kind, ps_entity_kind(n->kind)); n->kind++) ;
        if (n->kind == PE_KINDS) { fail(err, errsz, "\"ha.entities\": no kind \"%s\" (binary_sensor, sensor, text_sensor, switch, button, number, select, event)", kind); return -1; }
        /* (each kind's keys: sdk/messages.def's "ha.entities.<kind>") */
        { char ctx[40]; int kv; snprintf(ctx, sizeof ctx, "ha.entities.%s", kind);
          if ((kv = keys_versioned(j, e, ctx, m, 1, err, errsz)) == -1) return -1;
          if (kv) { fail(err, errsz, "\"ha.entities\": a %s has a key this version does not read", kind); return -1; } }
        if (tstr(j, json_get(j, e, "name"), n->name, sizeof n->name) || !name_ok(n->name, 32)) { fail(err, errsz, "\"ha.entities\": a name is 1 to 32 of a-z, 0-9 and _"); return -1; }
        if (tstr(j, json_get(j, e, "label"), n->label, sizeof n->label) || !n->label[0]) { fail(err, errsz, "\"ha.entities\": %s: its label is text of 1 to 64 bytes", n->name); return -1; }
        if ((k = json_get(j, e, "icon")) >= 0 && (tstr(j, k, n->icon, sizeof n->icon) || !icon_ok(n->icon))) { fail(err, errsz, "\"ha.entities\": %s: an icon is \"mdi:<name>\"", n->name); return -1; }
        if ((k = json_get(j, e, "unit")) >= 0 && tstr(j, k, n->unit, sizeof n->unit)) { fail(err, errsz, "\"ha.entities\": %s: its unit is text of at most 12 bytes", n->name); return -1; }
        if (jwhole(j, e, "decimals", 0, &d) || d < 0 || d > 6) { fail(err, errsz, "\"ha.entities\": %s: decimals 0 to 6", n->name); return -1; }
        n->decimals = (int)d;
        /* "page": true or false and nothing else (as an action's "page" and "calls"), and only where there is a state to
         * show: a button has none, an event fires and is gone */
        if ((k = json_get(j, e, "page")) >= 0) {
            if (json_type(j, k) != J_TRUE && json_type(j, k) != J_FALSE) { fail(err, errsz, "\"ha.entities\": %s: page is true or false", n->name); return -1; }
            n->page = json_type(j, k) == J_TRUE;
            if (n->page && (n->kind == PE_BUTTON || n->kind == PE_EVENT)) { fail(err, errsz, "\"ha.entities\": %s: a %s has no state to show on the card (\"page\")", n->name, kind); return -1; }
        }
        if (n->kind == PE_NUMBER) {
            if (jnumber(j, e, "min", 0, &n->min) || jnumber(j, e, "max", 100, &n->max) || jnumber(j, e, "step", 1, &n->step)
                || !(n->min < n->max) || !(n->step > 0) || n->min < -1e9 || n->max > 1e9) { fail(err, errsz, "\"ha.entities\": %s: min below max, a step above 0, within a billion", n->name); return -1; }
        }
        if (n->kind == PE_SELECT || n->kind == PE_EVENT) {
            int o = jlist(j, e, n->kind == PE_SELECT ? "options" : "types");
            if (o < 0) { fail(err, errsz, "\"ha.entities\": %s: its %s are a list", n->name, n->kind == PE_SELECT ? "options" : "types"); return -1; }
            JSON_EACH(j, o, x) {
                char *t = n->kind == PE_SELECT ? n->options[n->noptions] : n->types[n->ntypes]; int *cnt = n->kind == PE_SELECT ? &n->noptions : &n->ntypes;
                if (*cnt == (n->kind == PE_SELECT ? PS_OPTIONS : PS_TYPES) || tstr(j, x, t, 33) || !t[0] || (n->kind == PE_EVENT && !name_ok(t, 32))) {
                    fail(err, errsz, "\"ha.entities\": %s: %s", n->name, n->kind == PE_SELECT ? "options: 1 to 12 texts of 1 to 32 bytes" : "types: 1 to 8 names of a-z, 0-9 and _"); return -1;
                }
                for (int i = 0; i < *cnt; i++) if (!strcmp(n->kind == PE_SELECT ? n->options[i] : n->types[i], t)) { fail(err, errsz, "\"ha.entities\": %s: \"%s\" twice", n->name, t); return -1; }
                (*cnt)++;
            }
            if (!(n->kind == PE_SELECT ? n->noptions : n->ntypes)) { fail(err, errsz, "\"ha.entities\": %s: an empty list", n->name); return -1; }
        }
        for (int i = 0; i < m->nentities; i++) if (!strcmp(m->entities[i].name, n->name)) { fail(err, errsz, "\"ha.entities\": %s twice", n->name); return -1; }
        /* Home Assistant keeps an entity by its type and its name (the plugin's name and the label: aioesphomeapi
         * build_unique_id, version 3), so two of the same label were one entry in its registry, and which of the
         * two a history or an automation was about changed with the order they came in (H3 of a review of efde36e).
         * Any two, whatever their kinds: the install's line and the card name them by the label alone */
        for (int i = 0; i < m->nentities; i++) if (!strcmp(m->entities[i].label, n->label)) { fail(err, errsz, "\"ha.entities\": two are labelled \"%s\"", n->label); return -1; }
        m->nentities++;
    }
    if (!m->nentities) { fail(err, errsz, "\"ha.entities\" names no entity"); return -1; }
    return 0;
}

/* `ha.actions` (4.10): a list of 1 to PS_ACTIONS actions, each { "name", "label", "args": [ { "name", "type": "string"
 * | "int" } ... up to 4 ], "page": true | false } and nothing else.  Home Assistant calls them as
 * esphome.<node>_p_<id>_<name>; "page": a button on the plugin's card as well, for an action without arguments (the
 * card asks for nothing: a value typed there would be one more way in to the plugin).  "calls": true or false (3.6,
 * call), only with the capability `call`: the action brings the plugin a call token, with which it may place a call
 * or take one that rings (plugin.c token_new) */
static int actions_parse(const struct json *j, int v, struct ps_manifest *m, char *err, size_t errsz)
{
    if (json_type(j, v) != J_ARR) { fail(err, errsz, "\"ha.actions\" is a list of actions"); return -1; }
    JSON_EACH(j, v, e) {
        struct ps_action *a = &m->actions[m->nactions]; int args, pg;
        if (m->nactions == PS_ACTIONS) { fail(err, errsz, "\"ha.actions\": %d at most", PS_ACTIONS); return -1; }
        memset(a, 0, sizeof *a);
        { int kv = json_type(j, e) == J_OBJ ? keys_versioned(j, e, "ha.actions", m, 1, err, errsz) : -2;
          if (kv == -1) return -1;
          if (kv) { fail(err, errsz, "\"ha.actions\": an action is {\"name\", \"label\", \"args\", \"page\", \"calls\"} and nothing else"); return -1; } }
        if (tstr(j, json_get(j, e, "name"), a->name, sizeof a->name) || !name_ok(a->name, 32)) { fail(err, errsz, "\"ha.actions\": a name is 1 to 32 of a-z, 0-9 and _"); return -1; }
        if (tstr(j, json_get(j, e, "label"), a->label, sizeof a->label) || !a->label[0]) { fail(err, errsz, "\"ha.actions\": %s: its label is text of 1 to 64 bytes", a->name); return -1; }
        if ((pg = json_get(j, e, "page")) >= 0 && json_type(j, pg) != J_TRUE && json_type(j, pg) != J_FALSE) { fail(err, errsz, "\"ha.actions\": %s: page is true or false", a->name); return -1; }
        a->page = json_type(j, pg) == J_TRUE;
        { int cl = json_get(j, e, "calls");
          if (cl >= 0 && json_type(j, cl) != J_TRUE && json_type(j, cl) != J_FALSE) { fail(err, errsz, "\"ha.actions\": %s: calls is true or false", a->name); return -1; }
          a->calls = json_type(j, cl) == J_TRUE;
          if (a->calls && !(m->caps & PM_CAPBIT(PM_CAP_CALL))) { fail(err, errsz, "\"ha.actions\": %s: an action that places calls (\"calls\") needs the capability \"call\"", a->name); return -1; } }
        if ((args = jlist(j, e, "args")) == -2) { fail(err, errsz, "\"ha.actions\": %s: its args are a list", a->name); return -1; }
        JSON_EACH(j, args, x) {
            char type[8];
            if (a->nargs == PS_ARGS) { fail(err, errsz, "\"ha.actions\": %s: %d arguments at most", a->name, PS_ARGS); return -1; }
            int kv = json_type(j, x) == J_OBJ ? keys_versioned(j, x, "ha.actions.arg", m, 1, err, errsz) : -2;
            if (kv == -1) return -1;
            if (kv || tstr(j, json_get(j, x, "name"), a->args[a->nargs].name, sizeof a->args[0].name) || !name_ok(a->args[a->nargs].name, 32)
                || tstr(j, json_get(j, x, "type"), type, sizeof type) || (strcmp(type, "string") && strcmp(type, "int"))) {
                fail(err, errsz, "\"ha.actions\": %s: an argument is {\"name\", \"type\": \"string\" or \"int\"}", a->name); return -1;
            }
            for (int i = 0; i < a->nargs; i++) if (!strcmp(a->args[i].name, a->args[a->nargs].name)) { fail(err, errsz, "\"ha.actions\": %s: an argument twice", a->name); return -1; }
            a->args[a->nargs++].is_int = !strcmp(type, "int");
        }
        if (a->page && a->nargs) { fail(err, errsz, "\"ha.actions\": %s: a button on the card is for an action without arguments", a->name); return -1; }
        for (int i = 0; i < m->nactions; i++) if (!strcmp(m->actions[i].name, a->name)) { fail(err, errsz, "\"ha.actions\": %s twice", a->name); return -1; }
        /* (the install's line and the card's buttons name an action by its label: two alike are none the owner can
         * tell apart) */
        for (int i = 0; i < m->nactions; i++) if (!strcmp(m->actions[i].label, a->label)) { fail(err, errsz, "\"ha.actions\": two are labelled \"%s\"", a->label); return -1; }
        m->nactions++;
    }
    if (!m->nactions) { fail(err, errsz, "\"ha.actions\" names no action"); return -1; }
    return 0;
}

/* capabilities that are a yes and nothing more (3.3's table: no parameters).  Their value is `true`:
 * "wake": 0 or "state": "no" read as asked for, by an author who may have meant the opposite */
#define CAPS_PLAIN (PM_CAPBIT(PM_CAP_STATE) | PM_CAPBIT(PM_CAP_LOG_READ) | PM_CAPBIT(PM_CAP_LOG_READ_FULL) | PM_CAPBIT(PM_CAP_AUDIO_MUSIC) | PM_CAPBIT(PM_CAP_AUDIO_VOLUME) \
                    | PM_CAPBIT(PM_CAP_CALL) | PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_REQUEST_AUDIO) | PM_CAPBIT(PM_CAP_REQUEST_TEXT) | PM_CAPBIT(PM_CAP_BUTTON))

int ps_manifest_parse(const char *text, size_t len, struct ps_manifest *m, char *err, size_t errsz)
{
    struct json j = { 0 }; char hx[80]; int root = 0, caps, r = -1; long n; crypto_blake2b_ctx acc;
    memset(m, 0, sizeof *m);
    /* its texts go to the page, to Home Assistant (a name, a label, an option) and into the log as they are: Home
     * Assistant drops the link on a string that is not UTF-8, over and over (as ps_item_checked says of a title) */
    if (len <= MANIFEST_MAX && !utf8_ok(text, len)) { fail(err, errsz, "the manifest is not UTF-8: it has bytes that are no character"); goto out; }
    if (len > MANIFEST_MAX || json_parse(&j, text, len)) { fail(err, errsz, "the manifest is not JSON (or over %d bytes)", MANIFEST_MAX); goto out; }
    if (json_type(&j, root) != J_OBJ) { fail(err, errsz, "the manifest is not an object"); goto out; }
    if (form(&j, text, len, err, errsz)) goto out;
    if (jstr(&j, root, "id", m->id, sizeof m->id, 1, err, errsz) || jstr(&j, root, "name", m->name, sizeof m->name, 1, err, errsz)
        || jstr(&j, root, "author", m->author, sizeof m->author, 1, err, errsz)
        || jstr(&j, root, "version_name", m->version_name, sizeof m->version_name, 0, err, errsz) || jstr(&j, root, "source", m->source, sizeof m->source, 0, err, errsz)
        || jstr(&j, root, "license", m->license, sizeof m->license, 0, err, errsz) || jstr(&j, root, "description", m->description, sizeof m->description, 0, err, errsz)) goto out;
    if (!ps_id_ok(m->id)) { fail(err, errsz, "the plugin's id: 3 to 24 lower case letters, digits and - (none first, last or twice in a row), and none of the settings page's words (upload, install, ...)"); goto out; }
    if (!m->name[0] || !m->author[0]) { fail(err, errsz, "the manifest names no plugin or no author"); goto out; }
    if (jwhole(&j, root, "version", 0, &m->version) || m->version < 1 || m->version > 1000000000) { fail(err, errsz, "the manifest's version: a whole number from 1"); goto out; }
    /* what it speaks, before anything that a scope's version may bring (core 1's texts above are read by every build).
     * One for a newer hassmic stops here, its id, name, author and version kept for the card (ps_scan) */
    if (requires_parse(&j, json_get(&j, root, "requires"), m, err, errsz)) goto out;
    if (keys_versioned(&j, root, "manifest", m, 0, err, errsz)) goto out;
    if (jstr(&j, root, "author_key", hx, sizeof hx, 1, err, errsz)) goto out;
    if (strlen(hx) != 64 || unhex(m->author_key, hx, 32)) { fail(err, errsz, "the manifest's author_key: 64 hex digits"); goto out; }
    if (jstr(&j, root, "digest", hx, sizeof hx, 1, err, errsz)) goto out;
    if (strlen(hx) != 64 || unhex(m->digest, hx, 32)) { fail(err, errsz, "the manifest's digest: 64 hex digits"); goto out; }
    if (jwhole(&j, root, "memory_mb", 32, &n) || n < 8 || n > 96) { fail(err, errsz, "the manifest's limits: memory_mb is 8 to 96 (MB), a whole number"); goto out; }
    m->memory_mb = (int)n;
    if (jwhole(&j, root, "threads", 4, &n) || n < 1 || n > 16) { fail(err, errsz, "the manifest's limits: threads is 1 to 16, a whole number"); goto out; }
    m->threads = (int)n;
    { int models = jlist(&j, root, "models"), any = models == -1;            /* none named: every model */
      char name[33];
      if (models == -2) { fail(err, errsz, "the manifest's models are a list of names"); goto out; }
      JSON_EACH(&j, models, e) {
          if (tstr(&j, e, name, sizeof name) || !name[0]) { fail(err, errsz, "the manifest's models are a list of names"); goto out; }
          if (!strcmp(name, "*") || !strcmp(name, board.codename)) any = 1;
      }
      if (!any) { fail(err, errsz, "this plugin is not for this model (%s)", board.codename); goto out; } }

    caps = json_get(&j, root, "capabilities");
    if (json_type(&j, caps) != J_OBJ) { fail(err, errsz, "the manifest has no \"capabilities\""); goto out; }
    for (int k = json_first(&j, caps); k >= 0; ) {
        int v = j.t[k].next, bit; char name[24];
        if (v >= j.t[caps].next) break;
        if (!json_str(&j, k, name, sizeof name) || (size_t)(j.t[k].end - j.t[k].start) >= sizeof name) name[0] = 0;
        for (bit = 0; bit < NCAPS && strcmp(name, cap_names[bit]); bit++) ;
        if (bit == NCAPS) { fail(err, errsz, "the manifest asks for \"%s\", which this version does not know", name); goto out; }
        if (m->caps & PM_CAPBIT(bit)) { fail(err, errsz, "the manifest asks for \"%s\" twice", name); goto out; }
        if (json_type(&j, v) == J_FALSE || json_type(&j, v) == J_NULL) { fail(err, errsz, "capability \"%s\": leave it out instead of false", name); goto out; }
        if ((CAPS_PLAIN & PM_CAPBIT(bit)) && json_type(&j, v) != J_TRUE) { fail(err, errsz, "capability \"%s\": true, or left out (it has no parameters)", name); goto out; }
        /* (its scope required: what of the protocol it speaks says the manifest, never this build's guess) */
        if (!m->requires[cap_scope(bit)]) { fail(err, errsz, "capability \"%s\": its scope is not in \"requires\" (\"%s\": the least version it speaks)", name, name); goto out; }
        m->caps |= PM_CAPBIT(bit);
        if (bit == PM_CAP_STORAGE) { if (whole(&j, v, &n) || n < 1 || n > 1024) { fail(err, errsz, "\"storage\": 1 to 1024 kB, a whole number"); goto out; } m->storage_kb = (int)n; }
        if (bit == PM_CAP_FILES) { if (whole(&j, v, &n) || n < 1 || n > 64) { fail(err, errsz, "\"files\": 1 to 64 MB, a whole number"); goto out; } m->files_mb = (int)n; }
        if (bit == PM_CAP_NET_UDP) { if (whole(&j, v, &n) || n < 1 || n > PS_UDP_MAX) { fail(err, errsz, "\"net.udp\": 1 to %d ports, a whole number", PS_UDP_MAX); goto out; } m->udp_ports = (int)n; }
        k = json_next(&j, caps, v);
    }
    /* requires is core and the capabilities' scopes, nothing else: a scope it names and does not ask for is a version
     * of nothing it may use */
    for (int s = 1; s < PM_SCOPES; s++)
        if (m->requires[s] && !(m->caps & PM_CAPBIT(pm_scope_cap(s)))) { fail(err, errsz, "\"requires\" names %s, which the manifest does not ask for", pm_scope_name(s)); goto out; }
    { int st = jlist(&j, root, "settings");
      if (st == -2) { fail(err, errsz, "the manifest's settings are a list"); goto out; }
      JSON_EACH(&j, st, e) {
          if (m->nsettings == PS_SETTINGS) { fail(err, errsz, "more than %d settings", PS_SETTINGS); goto out; }
          if (setting_parse(&j, e, &m->settings[m->nsettings], m, err, errsz)) goto out;
          for (int i = 0; i < m->nsettings; i++) if (!strcmp(m->settings[i].name, m->settings[m->nsettings].name)) { fail(err, errsz, "setting %s twice", m->settings[i].name); goto out; }
          m->nsettings++;
      } }

    if (refs_ok(&j, caps, m, 0, err, errsz)) goto out;
    if ((m->caps & PM_CAPBIT(PM_CAP_NET_SERVER)) && net_parse(&j, json_get(&j, caps, "net.server"), m, err, errsz)) goto out;
    if ((m->caps & PM_CAPBIT(PM_CAP_NET_DISCOVER)) && discover_parse(&j, json_get(&j, caps, "net.discover"), m, err, errsz)) goto out;
    if ((m->caps & PM_CAPBIT(PM_CAP_NET_LAN)) && reach_parse(&j, json_get(&j, caps, "net.lan"), "net.lan", 1, m, m->lan, &m->nlan, err, errsz)) goto out;
    if ((m->caps & PM_CAPBIT(PM_CAP_NET_INTERNET)) && reach_parse(&j, json_get(&j, caps, "net.internet"), "net.internet", 0, m, m->inet, &m->ninet, err, errsz)) goto out;
    if ((m->caps & PM_CAPBIT(PM_CAP_HA_ENTITIES)) && entities_parse(&j, json_get(&j, caps, "ha.entities"), m, err, errsz)) goto out;
    if ((m->caps & PM_CAPBIT(PM_CAP_HA_ACTIONS)) && actions_parse(&j, json_get(&j, caps, "ha.actions"), m, err, errsz)) goto out;

    /* what the owner accepts (3.3): the plugin's name and author, what it says of itself and where its source is
     * (the card's text above its rows, the link the owner's browser opens), the capabilities with every parameter,
     * and the settings whole: what a row is called, what it says of itself, what it starts at and takes, the files
     * of a list, its fields and its catalog are all read by the owner, typed into, or handed to the plugin or the
     * browser.  Its digest is what `accepted` holds.  Not in it, and free to change: version_name and license, which
     * the page shows only as what they are, a label beside the version's number and a licence's name, each a few
     * bytes of text without control characters */
    crypto_blake2b_init(&acc, 32);
    crypto_blake2b_update(&acc, (const uint8_t *)ACCEPTED_HEAD, strlen(ACCEPTED_HEAD));
    crypto_blake2b_update(&acc, (const uint8_t *)m->name, strlen(m->name) + 1);
    crypto_blake2b_update(&acc, (const uint8_t *)m->author, strlen(m->author) + 1);
    crypto_blake2b_update(&acc, (const uint8_t *)m->description, strlen(m->description) + 1);
    crypto_blake2b_update(&acc, (const uint8_t *)m->source, strlen(m->source) + 1);
    canon(&j, caps, &acc);
    { int st = json_get(&j, root, "settings"); if (st >= 0) canon(&j, st, &acc); }
    crypto_blake2b_final(&acc, m->accepted);
    /* and as builds before this one made it, for what they wrote into `accepted`: form 2, then form 1 */
    crypto_blake2b_init(&acc, 32);
    crypto_blake2b_update(&acc, (const uint8_t *)ACCEPTED_HEAD2, strlen(ACCEPTED_HEAD2));
    crypto_blake2b_update(&acc, (const uint8_t *)m->name, strlen(m->name) + 1);
    crypto_blake2b_update(&acc, (const uint8_t *)m->author, strlen(m->author) + 1);
    canon(&j, caps, &acc);
    { int st = json_get(&j, root, "settings"); if (st >= 0) canon(&j, st, &acc); }
    crypto_blake2b_final(&acc, m->accepted_was[0]);
    crypto_blake2b_init(&acc, 32);
    crypto_blake2b_update(&acc, (const uint8_t *)ACCEPTED_HEAD1, strlen(ACCEPTED_HEAD1));
    crypto_blake2b_update(&acc, (const uint8_t *)m->name, strlen(m->name) + 1);
    crypto_blake2b_update(&acc, (const uint8_t *)m->author, strlen(m->author) + 1);
    canon(&j, caps, &acc);
    for (int i = 0; i < m->nsettings; i++) {
        const struct ps_setting *s = &m->settings[i]; char t[4]; snprintf(t, sizeof t, "%d", s->type);
        crypto_blake2b_update(&acc, (const uint8_t *)s->name, strlen(s->name) + 1);
        crypto_blake2b_update(&acc, (const uint8_t *)t, strlen(t) + 1);
        crypto_blake2b_update(&acc, (const uint8_t *)s->label, strlen(s->label) + 1);
    }
    crypto_blake2b_final(&acc, m->accepted_was[1]);
    crypto_blake2b(m->manifest_digest, 32, (const uint8_t *)text, len);
    r = 0;
out:
    json_free(&j);
    return r;
}

/* The combination rules (3.3), which hold whoever signed.  `wake` is sealed: besides it only files and storage,
 * nothing that carries data out of the plugin */
static int combination(const struct ps_manifest *m, int tier, char *err, size_t errsz)
{
    if ((m->caps & PM_CAPBIT(PM_CAP_WAKE)) && (m->caps & ~(PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES) | PM_CAPBIT(PM_CAP_STORAGE)))) {
        fail(err, errsz, "a plugin that hears the microphone all the time (\"wake\") may have only files and storage besides: this one asks for a way out"); return -1;
    }
    if ((m->caps & PM_CAPBIT(PM_CAP_LOG_READ_FULL)) && tier != PS_OFFICIAL) { fail(err, errsz, "the whole log (\"log.read.full\") is for official plugins only"); return -1; }
    return 0;
}

/* ---- request.audio's two rules beside the combinations (3.3).  Neither is a combination of one manifest: both are
 * about this Echo, so they are asked here and at the broker's start, not of the index's package alone.
 *
 * The second press.  A plugin that hears every request and has any way out (any net.*) can carry the household's
 * requests off the Echo: the Echo wants a second press of the action button for it, asked for on a screen of its own
 * that says where it can send what it hears.  The Echo's rule, not the page's: ps_stage_install takes such a package
 * only with both presses (PS_PRESSED2), or a voucher of another Echo for the first and this Echo's press for the
 * second (PS_VOUCHED_PRESSED; a voucher never stands in for the second, 5.4).  Wherever the button is asked for at
 * all: a first install, an update wider than what was accepted (an update that is what was accepted or less of it
 * asks for neither press: the pair was accepted with both).  So adding a net.* to a request.audio plugin, or
 * request.audio to one with a net.*, takes both presses (a capability not accepted is wider, wider()).
 *
 * The partner (3.3 "A partner"): a wake word engine that is a plugin nobody vouches for (community, or from a file)
 * opens the microphone when it likes, 20 runs in 10 minutes before it is taken as broken (4.13); beside a plugin
 * with request.audio those runs are recordings for it.  So no request.audio plugin runs on an Echo whose wake word
 * engine is such a plugin, and the Echo refuses the second of the two, whichever comes second: a request.audio
 * plugin is not installed while the engine is one (ps_partner), the engine's plugin is not installed as one beside a
 * request.audio plugin that is switched on (an update from a lower tier: ps_partner), that plugin is not chosen as the
 * engine then (ps_engine_refused, main.c core_wake_engine), and where both came to be all the same (a state copied
 * from elsewhere, a build before this one) the broker does not start the request.audio plugin (plugin.c start: the
 * wake word engine is what keeps the Echo working).  The engine is the core's word (plugin.c plugin_wake_engine ->
 * ps_engine_set), kept here under a lock of its own: the core says it with its own lock held */
static pthread_mutex_t eng_mu = PTHREAD_MUTEX_INITIALIZER;
static char eng_id[PS_ID_MAX];
void ps_engine_set(const char *id) { pthread_mutex_lock(&eng_mu); snprintf(eng_id, sizeof eng_id, "%s", id ? id : ""); pthread_mutex_unlock(&eng_mu); }
static void engine_get(char out[PS_ID_MAX]) { pthread_mutex_lock(&eng_mu); memcpy(out, eng_id, PS_ID_MAX); pthread_mutex_unlock(&eng_mu); }

#define NET_ANY (PM_CAPBIT(PM_CAP_NET_SERVER) | PM_CAPBIT(PM_CAP_NET_LAN) | PM_CAPBIT(PM_CAP_NET_INTERNET) | PM_CAPBIT(PM_CAP_NET_UDP) | PM_CAPBIT(PM_CAP_NET_DISCOVER))
int ps_second(const struct ps_manifest *m, char *line, size_t cap)
{
    /* where it can send what it hears, in the owner's words (3.3): the internet (net.internet), your network (net.lan,
     * net.udp: it sends to devices on it, net.discover: its search goes to every device), the server you enter
     * (net.server) */
    const char *to[3]; int n = 0;
    if (!(m->caps & PM_CAPBIT(PM_CAP_REQUEST_AUDIO)) || !(m->caps & NET_ANY)) { if (cap) line[0] = 0; return 0; }
    if (m->caps & PM_CAPBIT(PM_CAP_NET_INTERNET)) to[n++] = "the internet";
    if (m->caps & (PM_CAPBIT(PM_CAP_NET_LAN) | PM_CAPBIT(PM_CAP_NET_UDP) | PM_CAPBIT(PM_CAP_NET_DISCOVER))) to[n++] = "your network";
    if (m->caps & PM_CAPBIT(PM_CAP_NET_SERVER)) to[n++] = "the server you enter";
    if (cap) snprintf(line, cap, "This plugin hears every request and can send what it hears to %s%s%s%s%s.", to[0], n > 2 ? ", " : n > 1 ? " and " : "",
                      n > 1 ? to[1] : "", n > 2 ? " and " : "", n > 2 ? to[2] : "");
    return 1;
}

/* Whether the index vouches for an installed plugin: official, and not withdrawn.  A withdrawn one the owner runs
 * "anyway" (ps_anyway) is the index taking its word back: its false detections are nobody's promise any more, and it
 * counts as unvouched for the partner rule as a community one does (a review of b294fa0, C4) */
static int vouched(const struct ps_plugin *e) { return e->tier == PS_OFFICIAL && !e->withdrawn[0]; }

/* the engine's plugin, if the engine is one, installed and nobody vouches for it (ps_lock held): its entry */
static const struct ps_plugin *engine_unvouched(void)
{
    char eng[PS_ID_MAX]; const struct ps_plugin *e;
    engine_get(eng);
    return eng[0] && (e = ps_find(eng)) && (e->m.caps & PM_CAPBIT(PM_CAP_WAKE)) && !vouched(e) ? e : NULL;
}
int ps_partner(const struct ps_manifest *m, int tier, int install, char *err, size_t errsz)
{
    const struct ps_plugin *e; char eng[PS_ID_MAX];
    if ((m->caps & PM_CAPBIT(PM_CAP_REQUEST_AUDIO)) && (e = engine_unvouched()) && strcmp(e->m.id, m->id)) {
        fail(err, errsz, "this Echo's wake word engine is the plugin %s, which the plugin index %s: no plugin that hears every request runs beside it (a wake word plugin's false detections would open the microphone to it). Choose another wake word engine first", e->m.id,
             e->withdrawn[0] ? "withdrew" : "does not list as official");
        return -1;
    }
    engine_get(eng);
    if (install && eng[0] && !strcmp(eng, m->id) && (m->caps & PM_CAPBIT(PM_CAP_WAKE)) && tier != PS_OFFICIAL)
        for (int i = 0; i < nplugins; i++)
            if (strcmp(plugins[i].m.id, m->id) && plugins[i].enabled && (plugins[i].m.caps & PM_CAPBIT(PM_CAP_REQUEST_AUDIO))) {
                fail(err, errsz, "%s is this Echo's wake word engine, and this version is not listed as official, while %s hears every request: a wake word plugin nobody vouches for does not run as the engine beside it (its false detections would open the microphone to it). Switch %s off first", m->id, plugins[i].m.id, plugins[i].m.id);
                return -1;
            }
    return 0;
}

int ps_stage_second(char *line, size_t cap)
{
    int r;
    ps_lock(); r = staged_ok && ps_second(&staged, line, cap); ps_unlock();
    return r;
}

int ps_engine_refused(const char *id, char *why, size_t cap)
{
    const struct ps_plugin *e; int r = 0;
    ps_lock();
    /* (a community plugin while those are switched off: it does not run, so an engine that would hear nothing.  The
     * switch as it is now, never the scan's mark: an import, "make like this Echo" or a preset that switches
     * community plugins on and chooses such a plugin as the engine in one request had the engine refused, the mark
     * being the scan's from before the switch (a review of 00d492a, 1).  settings.c applies the switch first) */
    if ((e = ps_find(id)) && ps_community_keeps_off(e)) { snprintf(why, cap, "%s is %s, and community plugins are switched off on this Echo: switch them on on the Plugins page first", id, e->tier == PS_COMMUNITY ? "a community plugin" : "from a file, not listed by the plugin index"); r = 1; }
    if (!r && e && !vouched(e))
        for (int i = 0; i < nplugins && !r; i++)
            if (strcmp(plugins[i].m.id, id) && plugins[i].enabled && (plugins[i].m.caps & PM_CAPBIT(PM_CAP_REQUEST_AUDIO))) {
                snprintf(why, cap, "%s hears every request, and the plugin index %s the plugin %s: a wake word engine nobody vouches for does not run beside a plugin that hears every request (its false detections would open the microphone to it). Switch %s off first", plugins[i].m.id,
                         e->withdrawn[0] ? "withdrew" : "does not list as official", id, plugins[i].m.id);
                r = 1;
            }
    ps_unlock();
    return r;
}

/* What holds for a plugin to be installed and, again, to be started (3.3: "the broker refuses to start a plugin
 * whose manifest says otherwise, whoever signed it"): a package put into state/ by other means than the page passed
 * no ps_stage_check */
/* What may run here whoever signed (3.3's combinations).  What this build has behind each capability is its scope's
 * version, which ps_manifest_parse held "requires" to before it read anything a scope brings: a manifest for a newer
 * hassmic never gets this far */
static int runnable(const struct ps_manifest *m, int tier, char *err, size_t errsz)
{
    return combination(m, tier, err, errsz);
}

/* net.server's line at its longest: "Connects to the server you enter" and per entry "; and the one in \"<label>\"
 * (UDP, " and the port row's "the port you enter in \"<label>\")" or "ports " and eight "65535, " */
_Static_assert(40 + PS_NET * (30 + 64 + 30 + (64 > PS_PORTS * 7 ? 64 : PS_PORTS * 7)) < PS_CAP_LINE, "net.server's line fits PS_CAP_LINE");
_Static_assert(sizeof ((struct ps_setting *)0)->label == 65, "PS_CAP_LINE counts labels of 64 bytes");
/* ha.entities' at its longest: "Shows in Home Assistant: " and per entity ", \"<label>\" (<kind>)", the kind's words
 * 15 bytes at most ("a number to set"): 22 around a label; ha.actions' the same with " (also a button on its card)" */
/* " (also a button on its card; and have it place a call)": 56 around a label */
_Static_assert(40 + PS_ENTITIES * (64 + 22) < PS_CAP_LINE && 80 + PS_ACTIONS * (64 + 60) < PS_CAP_LINE, "ha.entities' and ha.actions' lines fit PS_CAP_LINE");
/* net.lan's and net.internet's line: WORDS, then each entry's protocol and ports ("(TCP, ports 80, 443; UDP, any
 * port)"); at its longest PS_NET entries of "; UDP, ports " and eight "65535, ", far within PS_CAP_LINE */
_Static_assert(40 + PS_NET * (16 + PS_PORTS * 7) < PS_CAP_LINE, "net.lan's line fits PS_CAP_LINE");
static void reach_line(char *out, size_t cap, const char *words, const struct ps_reach *r, int n)
{
    size_t k = (size_t)snprintf(out, cap, "%s (", words);
    for (int i = 0; i < n && k < cap; i++) {
        k += (size_t)snprintf(out + k, cap - k, "%s%s, ", i ? "; " : "", r[i].udp ? "UDP" : "TCP");
        if (k < cap && r[i].any) k += (size_t)snprintf(out + k, cap - k, "any port");
        else for (int p = 0; p < r[i].nports && k < cap; p++) k += (size_t)snprintf(out + k, cap - k, "%s%u", p ? ", " : r[i].nports > 1 ? "ports " : "port ", r[i].ports[p]);
    }
    if (k < cap) snprintf(out + k, cap - k, ")");
}
void ps_cap_line(const struct ps_manifest *m, int bit, char *out, size_t cap)
{
    switch (bit) {
    case PM_CAP_STORAGE: snprintf(out, cap, "Keeps up to %d kB of its own data on this Echo", m->storage_kb); break;
    case PM_CAP_FILES: {
        char what[PS_CAP_LINE] = ""; size_t n = 0;
        for (int i = 0; i < m->nsettings; i++) if (m->settings[i].type == PS_FILES) n += (size_t)snprintf(what + n, n < sizeof what ? sizeof what - n : 0, "%s%s", n ? ", " : "", m->settings[i].label);
        snprintf(out, cap, "Reads the files you give it on its page (%s, up to %d MB)", what[0] ? what : "none", m->files_mb); break; }
    case PM_CAP_STATE: snprintf(out, cap, "Sees when this Echo listens and speaks"); break;
    case PM_CAP_LOG_READ: snprintf(out, cap, "Reads this Echo's technical log"); break;
    case PM_CAP_LOG_READ_FULL: snprintf(out, cap, "Reads this Echo's whole log, including what was said"); break;
    case PM_CAP_NET_SERVER: {
        /* each server by the row the owner enters it in, its protocol and its ports: what the owner accepts is
         * where it may connect, so the line says it whole */
        size_t n = (size_t)snprintf(out, cap, "Connects to the server you enter");
        for (int i = 0; i < m->nnet && n < cap; i++) {
            const struct ps_net *e = &m->net[i]; const struct ps_setting *s = setting_named(m, e->setting), *ps = e->port_row[0] ? setting_named(m, e->port_row) : NULL;
            n += (size_t)snprintf(out + n, cap - n, "%s in \"%s\" (%s, ", i ? "; and the one" : "", s ? s->label : e->setting, e->udp ? "UDP" : "TCP");
            if (ps) n += n < cap ? (size_t)snprintf(out + n, cap - n, "the port you enter in \"%s\")", ps->label) : 0;
            else for (int k = 0; k < e->nports && n < cap; k++) n += (size_t)snprintf(out + n, cap - n, "%s%u%s", k ? ", " : e->nports > 1 ? "ports " : "port ", e->ports[k], k == e->nports - 1 ? ")" : "");
        }
        utf8_whole(out);
        break; }
    /* (3.3's words, the entries' protocols and ports filled in: where it may connect is what the owner accepts) */
    case PM_CAP_NET_LAN: reach_line(out, cap, "Connects to any device on your network", m->lan, m->nlan); break;
    case PM_CAP_NET_INTERNET: reach_line(out, cap, "Connects to the internet", m->inet, m->ninet); break;
    /* a port of its own that only devices on the network reach (the firewall and the socket both hold it to private
     * senders), and what it may send from there: to such devices, as its answers go */
    case PM_CAP_NET_UDP: snprintf(out, cap, "Can be reached from your network, and sends to devices on it (%d UDP port%s of its own)", m->udp_ports, m->udp_ports == 1 ? "" : "s"); break;
    /* The answers come to a port of its own, and so may anything any device on the network sends there, as long as
     * the plugin runs: an open port, which the owner accepts as such (A4 of a review of efde36e) */
    case PM_CAP_NET_DISCOVER:
        if (m->discover_group) snprintf(out, cap, "Searches your network for its server (UDP port %d, to the group %u.%u.%u.%u), and can be reached from your network on a UDP port of its own, where the answers come", m->discover_port,
                                         m->discover_group >> 24, m->discover_group >> 16 & 255, m->discover_group >> 8 & 255, m->discover_group & 255);
        else snprintf(out, cap, "Searches your network for its server (UDP port %d, to every device on it), and can be reached from your network on a UDP port of its own, where the answers come", m->discover_port);
        break;
    case PM_CAP_AUDIO_MUSIC: snprintf(out, cap, "Plays sound on this Echo's speaker, whenever it likes"); break;
    case PM_CAP_AUDIO_VOLUME: snprintf(out, cap, "Changes this Echo's volume"); break;
    case PM_CAP_CALL: snprintf(out, cap, "Makes and takes calls: a call coming in is answered only at the Echo, one goes out only when Home Assistant or the settings page asks. Hears the room during a call"); break;
    case PM_CAP_WAKE: snprintf(out, cap, "Hears this Echo's microphone all the time, to listen for its wake words"); break;
    case PM_CAP_REQUEST_AUDIO: snprintf(out, cap, "Hears everything said to this Echo after the wake word or the button"); break;
    case PM_CAP_REQUEST_TEXT: snprintf(out, cap, "Reads what was said to this Echo and what it answered"); break;
    /* (what Home Assistant's "Button" event entity hears, word for word the same presses: main.c button_event) */
    case PM_CAP_BUTTON: snprintf(out, cap, "Hears presses of this Echo's buttons, as Home Assistant's \"Button\" does"); break;
    /* each by the label the owner will see there, and its kind: a switch is something Home Assistant sets */
    case PM_CAP_HA_ENTITIES: {
        size_t n = (size_t)snprintf(out, cap, "Shows in Home Assistant: ");
        for (int i = 0; i < m->nentities && n < cap; i++) {
            static const char *const kw[PE_KINDS] = { "on/off", "a number", "a text", "a switch", "a button", "a number to set", "a choice", "an event" };
            n += (size_t)snprintf(out + n, cap - n, "%s\"%s\" (%s)", i ? ", " : "", m->entities[i].label, kw[m->entities[i].kind]);
        }
        utf8_whole(out);
        break; }
    case PM_CAP_HA_ACTIONS: {
        size_t n = (size_t)snprintf(out, cap, "Home Assistant can tell it to: ");
        /* (3.3: an action marked "calls": "...and have it place a call": what the owner accepts is that a press of it,
         * there or in Home Assistant, opens the room's microphone to a call) */
        for (int i = 0; i < m->nactions && n < cap; i++)
            n += (size_t)snprintf(out + n, cap - n, "%s\"%s\"%s", i ? ", " : "", m->actions[i].label,
                                  m->actions[i].page && m->actions[i].calls ? " (also a button on its card; and have it place a call)"
                                  : m->actions[i].page ? " (also a button on its card)" : m->actions[i].calls ? " (and have it place a call)" : "");
        utf8_whole(out);
        break; }
    default: snprintf(out, cap, "?");
    }
}

/* Every version above 1 that widens what its capability lets a plugin do, up to the one M requires of each scope (and
 * of what this build has), as JSON: [{"scope":"net.lan","version":2,"words":"..."}], in the order of the scopes and
 * their versions.  What an install's dialog and the card show beside the capabilities' lines, which are version 1's
 * words (finding 5 of a review of 60a8589: a plugin that requires net.lan 3 was shown net.lan 1's line alone).  A
 * version within only says more of the same: not listed.  Never cut: "[]" where it does not fit CAP (PS_WIDENS_JSON
 * holds every one this build has) */
size_t ps_widens_json(const struct ps_manifest *m, char *out, size_t cap)
{
    size_t n = 0; int k, first = 1; char w[2 * PM_VERSION_DOC_MAX + 8];
#define WJ(...) do { k = snprintf(out + n, n < cap ? cap - n : 0, __VA_ARGS__); n += k > 0 ? (size_t)k : 0; } while (0)
    WJ("[");
    for (int s = 0; s < PM_SCOPES; s++)
        for (unsigned v = 2; v <= m->requires[s] && v <= pm_scope_version(s); v++)
            if (pm_widens(s, v - 1, v)) {
                if (json_esc(w, sizeof w, pm_version_doc(s, v)) < 0) json_esc(w, sizeof w, "");
                WJ("%s{\"scope\":\"%s\",\"version\":%u,\"words\":%s}", first ? "" : ",", pm_scope_name(s), v, w); first = 0;
            }
    WJ("]");
#undef WJ
    if (n >= cap) n = cap > 2 ? (size_t)snprintf(out, cap, "[]") : 0;
    return n;
}

/* A key as an owner can compare it by eye with what its author publishes: the first 16 hex digits of its BLAKE2b-256,
 * in fours.  64 bits: enough against a key made to look alike by someone who must also get the owner to install it,
 * and short enough to be read at all (sdk/plugcheck.py prints the same) */
void ps_fingerprint(const uint8_t key[32], char out[20])
{
    uint8_t d[32]; char hx[17];
    crypto_blake2b(d, 32, key, 32); hex(hx, d, 8);
    snprintf(out, 20, "%.4s %.4s %.4s %.4s", hx, hx + 4, hx + 8, hx + 12);
}

/* ---------------------------------------------------------------- what an update asks for beyond what was accepted (3.3) */

const char *ps_change_kind(int kind) { return kind == PS_NEW ? "new" : kind == PS_MORE ? "more" : kind == PS_LIMIT ? "limit" : kind == PS_SPEAKS ? "speaks" : "changed"; }

static void change(struct ps_changes *ch, int kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void change(struct ps_changes *ch, int kind, const char *fmt, ...)
{
    va_list ap;
    int shown = kind == PS_LIMIT || kind == PS_SPEAKS;
    if (!shown) ch->ask = 1;                                    /* whether or not the line still fits */
    if (ch->n >= PS_CHANGES - (shown ? PS_CHANGES_ASK : 0)) { if (shown) ch->more_shown++; else ch->more++; return; }
    ch->c[ch->n].kind = kind;
    va_start(ap, fmt); vsnprintf(ch->c[ch->n].line, sizeof ch->c[0].line, fmt, ap); va_end(ap);
    utf8_whole(ch->c[ch->n].line);
    ch->n++;
}

/* Where a value stands, for the two quantities the comparison leaves to its caller: a setting's row (max_items), the
 * item of a file list, that item's files, one of them (max_kb) */
enum { X_PLAIN, X_ROW, X_ITEM, X_FILES, X_FILE, X_ENTS, X_ENT };
/* X_ROW is a file list's row and nothing else's: `max_items` in a number's or a text's row is a key this version
 * reads nothing of, and was left out of the comparison all the same (any value of it, in any row, passed).
 * X_ENT is an entity of ha.entities (X_ENTS the list): its "page" is no power and is left out whatever its
 * value.  The row shows on the owner's own card what Home Assistant is told anyway, under the label the owner
 * accepted; it reaches nobody else and gives the plugin nothing (docs/plugins.md 3.3 "As built", button and rows) */
static int x_skip(int ctx, const char *key) { return (ctx == X_ROW && !strcmp(key, "max_items")) || (ctx == X_FILE && !strcmp(key, "max_kb")) || (ctx == X_ENT && !strcmp(key, "page")); }
static int x_in(int ctx, const char *key) { return ctx == X_ROW && !strcmp(key, "item") ? X_ITEM : ctx == X_ITEM && !strcmp(key, "files") ? X_FILES : X_PLAIN; }

/* Two values that say the same: objects whatever the order of their keys (an author who sorts his manifest asks for
 * nothing new), lists entry by entry in their order, texts by what they say (an escape for a letter is that letter),
 * everything else as it is written: 443 and 443.0 count as two, asked once too often, never once too seldom.  Both
 * are of manifests that passed form(): no key twice, none with an escape.  In a row (X_ROW) and in a file of an item
 * (X_FILE) the quantity that may get smaller is left out; who calls holds it to "not larger" */
static int jsame(const struct json *a, int ta, const struct json *b, int tb, int ctx, int depth)
{
    int type = json_type(a, ta); size_t la, lb;
    if (ta < 0 || tb < 0 || type != json_type(b, tb) || depth > JSON_DEPTH) return 0;
    if (type == J_ARR) {
        int x = json_first(a, ta), y = json_first(b, tb);
        for (; x >= 0 && y >= 0; x = json_next(a, ta, x), y = json_next(b, tb, y)) if (!jsame(a, x, b, y, ctx == X_FILES ? X_FILE : ctx == X_ENTS ? X_ENT : X_PLAIN, depth + 1)) return 0;
        return x < 0 && y < 0;
    }
    if (type == J_OBJ) {
        /* every key of the one in the other, both ways; the values held against each other once */
        for (int side = 0; side < 2; side++) {
            const struct json *p = side ? b : a, *q = side ? a : b; int tp = side ? tb : ta, tq = side ? ta : tb;
            for (int k = json_first(p, tp); k >= 0; ) {
                int v = p->t[k].next, o; char key[200];
                if (v >= p->t[tp].next) return 0;
                if ((size_t)(p->t[k].end - p->t[k].start) >= sizeof key || !json_str(p, k, key, sizeof key)) return 0;
                if (!x_skip(ctx, key) && ((o = json_get(q, tq, key)) < 0 || (!side && !jsame(p, v, q, o, x_in(ctx, key), depth + 1)))) return 0;
                k = json_next(p, tp, v);
            }
        }
        return 1;
    }
    la = (size_t)(a->t[ta].end - a->t[ta].start); lb = (size_t)(b->t[tb].end - b->t[tb].start);
    if (la == lb && !memcmp(a->s + a->t[ta].start, b->s + b->t[tb].start, la)) return 1;
    if (type == J_STR) {                                        /* written otherwise: what they say (never longer than written) */
        char *x = malloc(la + 1), *y = malloc(lb + 1); int same = x && y && json_str(a, ta, x, la + 1) && json_str(b, tb, y, lb + 1) && !strcmp(x, y);
        free(x); free(y);
        return same;
    }
    return 0;
}

static const struct ps_setting *setting_named(const struct ps_manifest *m, const char *name)
{
    for (int i = 0; i < m->nsettings; i++) if (!strcmp(m->settings[i].name, name)) return &m->settings[i];
    return NULL;
}

/* What of a row is not as it was, in the page's words: each key of the two rows that one of them lacks or has
 * otherwise (the label and the quantities apart: those have lines of their own).  "" when nothing is */
static void row_diff(const struct json *o, int to, const struct json *n, int tn, int ctx, char *out, size_t cap)
{
    static const struct { const char *key, *what; } words[] = { { "help", "what it says about itself" }, { "unit", "its unit" }, { "default", "what it is set to at first" },
        { "min", "its lowest value" }, { "max", "its highest value" }, { "options", "its choices" }, { "item", "the files an item is made of" },
        { "fields", "the fields of an item" }, { "catalog", "what it offers to download, and from where" }, { "type", "its kind" } };
    size_t len = 0; int other = 0;
    out[0] = 0;
    for (int side = 0; side < 2; side++) {
        const struct json *p = side ? o : n, *q = side ? n : o; int tp = side ? to : tn, tq = side ? tn : to;
        for (int k = json_first(p, tp); k >= 0; ) {
            int v = p->t[k].next, w; char key[40]; const char *what = NULL;
            if (v >= p->t[tp].next) break;
            if ((size_t)(p->t[k].end - p->t[k].start) >= sizeof key || !json_str(p, k, key, sizeof key)) key[0] = 0;
            w = key[0] ? json_get(q, tq, key) : -1;
            k = json_next(p, tp, v);
            if (x_skip(ctx, key) || !strcmp(key, "label") || !strcmp(key, "name")) continue;
            if (side ? w >= 0 : w >= 0 && jsame(p, v, q, w, x_in(ctx, key), 1)) continue;       /* (the other way: only what the new row lacks) */
            for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) if (!strcmp(key, words[i].key)) what = words[i].what;
            /* a key the Echo reads nothing of is not repeated (an author's text, of any length): counted */
            if (!what) { other = 1; continue; }
            if (!strstr(out, what)) len += (size_t)snprintf(out + len, len < cap ? cap - len : 0, "%s%s", len ? ", " : "", what);
        }
    }
    if (other && len < cap) snprintf(out + len, cap - len, "%s%s", len ? ", " : "", "something this version does not read");
}

/* A raised "requires" (finding 4 of a review of 4e0a202).  A scope's later version only adds to the protocol, and
 * the version spoken with a plugin is what its manifest requires: so an update that requires a later version speaks
 * what that version brought.  Most versions say more of what the capability's line at install says (another field,
 * a value), and some let a plugin do more than that line said: sdk/messages.def marks each version above 1 "widens"
 * or "within", the lock records the word, and here a raise into a widening version (any between the two) is wider
 * than what was accepted, the button, with the version's own words; a raise within is shown and asks for nothing.
 * Every raised scope is listed either way, a line per version between the two (finding 5 of a review of 60a8589: a
 * raise over several versions said only the first widening one's words, a raise within only the last's): the owner
 * sees what each version the update comes to speak brought.  A scope the old manifest did not require is a capability
 * it did not ask for: said as one, with its widening versions' words (wider() below, widening_lines), not here.  OLD
 * NULL: the version installed cannot be read, so whatever this one requires above version 1 is held against
 * version 1 */
static void requires_wider(const struct ps_manifest *om, const struct ps_manifest *nm, struct ps_changes *ch)
{
    for (int s = 0; s < PM_SCOPES; s++) {
        unsigned was = om ? om->requires[s] : 1, now = nm->requires[s]; char of[64];
        if (!now || (om && !was) || now <= was) continue;
        if (om) snprintf(of, sizeof of, ", was %u", was); else snprintf(of, sizeof of, " (what the version installed speaks cannot be read)");
        for (unsigned v = was + 1, to; v <= now; v = to + 1) {
            /* versions within one after another: one line, "Speaks state 2 to 4, was 1: <2's words>; <3's>; <4's>",
             * as many versions' words as fit it whole, the rest on the next line (finding 4 of a review of 4c55b54:
             * a raise over many versions took a line each, and the lines that ask could be left out) */
            char words[sizeof ch->c[0].line - 160]; size_t wn;
            to = v;
            if (pm_widens(s, v - 1, v)) { change(ch, PS_MORE, "Speaks %s %u%s, which lets a plugin do more: %s", pm_scope_name(s), v, of, pm_version_doc(s, v)); continue; }
            wn = (size_t)snprintf(words, sizeof words, "%s", pm_version_doc(s, v)[0] ? pm_version_doc(s, v) : "more of the same");
            while (to < now && !pm_widens(s, to, to + 1)) {
                const char *d = pm_version_doc(s, to + 1)[0] ? pm_version_doc(s, to + 1) : "more of the same";
                if (wn + 2 + strlen(d) >= sizeof words) break;
                wn += (size_t)snprintf(words + wn, sizeof words - wn, "; %s", d); to++;
            }
            if (to == v) change(ch, PS_SPEAKS, "Speaks %s %u%s: %s", pm_scope_name(s), v, of, words);
            else change(ch, PS_SPEAKS, "Speaks %s %u to %u%s: %s", pm_scope_name(s), v, to, of, words);
        }
    }
}
/* A capability asked for anew (an update that adds it): its line says what version 1 of its scope lets a plugin do,
 * and every version up to the one required that widens that is more, said with its words, as part of what is new
 * (finding 5 of a review of 60a8589: only the version-1 line was shown).  A version within says more of the same */
static void widening_lines(const struct ps_manifest *nm, int bit, struct ps_changes *ch)
{
    for (int s = 0; s < PM_SCOPES; s++) {
        if (pm_scope_cap(s) != bit) continue;
        for (unsigned v = 2; v <= nm->requires[s]; v++)
            if (pm_widens(s, v - 1, v)) change(ch, PS_NEW, "Speaks %s %u, which lets a plugin do more: %s", pm_scope_name(s), v, pm_version_doc(s, v));
    }
}

/* What NEW has that is not what the owner accepted with OLD, one line each as the page shows them ("New: ...", "More:
 * ...", "Changed: ...").  A line of those kinds: the button.
 *
 * The rule is the strict one (a review, 2026-11-19: the first one named what counts as wider and let everything else
 * by, and what it let by was a file list renamed "Home Assistant backups and SSH keys" with other suffixes and 256
 * times the size, a number row renamed "Your bank card's PIN", a host row with "evil.example" as its default, a port
 * taken for a count).  Without the button only:
 *   - the capabilities as accepted, value for value, or with less: a capability gone, a smaller `storage` or
 *     `files` number.  Nothing else of a parameter is "less": see below, where a capability's own rule goes;
 *   - the settings as accepted, row for row in their order and key for key, or with less: a row gone, a smaller
 *     max_items, a smaller max_kb of a file.  Whole numbers, as the parser read them.
 * Everything else asks: what is more, what is merely other, what this version does not understand.  No parameter is
 * taken for a quantity because it is a number (net.discover's is a port), and no list for a list of grants whose
 * entries may go: the first rule had "an entry less is less" for the network's and Home Assistant's lists, and that
 * let `"net.lan": [80, 443]` become `[]` (which may come to mean "any"), took out an entry that restricts
 * (`{"not": ...}`) and one whose row was gone (a review, 2026-11-19; none of it reachable then: this build runs none
 * of those capabilities).  The limits (memory_mb, threads) are no powers (5.4): a line of their own kind, shown,
 * never asked for.  What the owner reads of the plugin itself is accepted with it: its name, its author's, what it
 * says of itself and the address its "Source and issues" link opens; any change of those asks, with the new words */
static void wider(const char *ot, size_t ol, const struct ps_manifest *om, const char *nt, size_t nl, const struct ps_manifest *nm, struct ps_changes *ch)
{
    static const char *const kinds[] = { "a switch", "a number", "a choice", "text", "a server's name or address", "a secret", "a list of files" };
    struct json jo = { 0 }, jn = { 0 }; char line[PS_CAP_LINE], diff[300]; int co, cn, so, sn, i = 0, last = -1, said = 0;
    /* the name and the author are what the owner trusted, not powers: an update must not turn into "Home Assistant" */
    if (strcmp(om->name, nm->name)) change(ch, PS_CHANGED, "Its name: \"%s\", was \"%s\"", nm->name, om->name);
    if (strcmp(om->author, nm->author)) change(ch, PS_CHANGED, "Its author's name: \"%s\", was \"%s\"", nm->author, om->author);
    /* its own words above its settings, and the link the owner's browser opens: shown as they will be (the old ones
     * are on the card) */
    if (strcmp(om->description, nm->description)) {
        if (nm->description[0]) change(ch, PS_CHANGED, "What it says about itself: \"%s\"", nm->description);
        else change(ch, PS_CHANGED, "It no longer says anything about itself");
    }
    if (strcmp(om->source, nm->source)) {
        if (nm->source[0]) change(ch, PS_CHANGED, "Its \"Source and issues\" link opens %s%s%s", nm->source, om->source[0] ? ", was " : "", om->source);
        else change(ch, PS_CHANGED, "It no longer has a \"Source and issues\" link");
    }
    if (om->memory_mb != nm->memory_mb) change(ch, PS_LIMIT, "May use up to %d MB of memory, was %d MB", nm->memory_mb, om->memory_mb);
    if (om->threads != nm->threads) change(ch, PS_LIMIT, "May run up to %d threads, was %d", nm->threads, om->threads);
    if (json_parse(&jo, ot, ol) || json_parse(&jn, nt, nl)) { change(ch, PS_CHANGED, "What it may do cannot be compared with what you accepted"); requires_wider(om, nm, ch); goto out; }
    co = json_get(&jo, 0, "capabilities"); cn = json_get(&jn, 0, "capabilities");
    for (int bit = 0; bit < NCAPS; bit++) {
        int to, tn;
        if (!(nm->caps & PM_CAPBIT(bit))) continue;
        ps_cap_line(nm, bit, line, sizeof line);
        if (!(om->caps & PM_CAPBIT(bit))) { change(ch, PS_NEW, "%s", line); widening_lines(nm, bit, ch); continue; }
        if (bit == PM_CAP_STORAGE) { if (nm->storage_kb > om->storage_kb) change(ch, PS_MORE, "%s, was %d kB", line, om->storage_kb); continue; }
        if (bit == PM_CAP_FILES) { if (nm->files_mb > om->files_mb) change(ch, PS_MORE, "%s, was %d MB", line, om->files_mb); continue; }
        /* net.udp's own rule (3.3: a capability built brings its rule for less): its parameter is a count of ports
         * of its own, and fewer is less.  The ports it keeps are the first ones (ps_udp_ports): nothing new opens */
        if (bit == PM_CAP_NET_UDP) { if (nm->udp_ports > om->udp_ports) change(ch, PS_MORE, "%s, was %d", line, om->udp_ports); continue; }
        to = json_get(&jo, co, cap_names[bit]); tn = json_get(&jn, cn, cap_names[bit]);
        if (jsame(&jo, to, &jn, tn, bit == PM_CAP_HA_ENTITIES ? X_ENTS : X_PLAIN, 0)) continue;
        /* Here goes a capability's own rule for what is less of its parameter, when the code that grants it is
         * written and says what each form means (3.3: "a capability that comes to be built brings its own rule"):
         * `if (bit == PM_CAP_... && <nm's is less than om's by that rule>) continue;`.  Until then a parameter is
         * the same, or the capability is gone, or the owner is asked */
        change(ch, PS_CHANGED, "%s: not with the parameters you accepted", line);
    }
    /* An update that drops `wake` is a new install (3.3: what it kept while it heard the microphone must not go out
     * with what it asks for next): asked for as one, and once it is accepted and runs here, what it was given under
     * the seal is wiped (seal_upkeep) */
    if ((om->caps & PM_CAPBIT(PM_CAP_WAKE)) && !(nm->caps & PM_CAPBIT(PM_CAP_WAKE))) change(ch, PS_CHANGED, "No longer a wake word plugin. The files you gave it and the data it kept while it heard the microphone are deleted once this version runs");
    /* The settings, each row against the one of its name.  A row is what the owner reads and types into: an update
     * must be able neither to add "Your alarm system's code" (a number, as a secret is text) nor to rename "Port"
     * into it, nor to put its own server into a host row's default */
    so = json_get(&jo, 0, "settings"); sn = json_get(&jn, 0, "settings");
    JSON_EACH(&jn, sn, e) {
        const struct ps_setting *s, *was; int tw = -1, k = 0;
        if (i >= nm->nsettings) break;                          /* (the parser read as many rows as there are) */
        s = &nm->settings[i++]; was = setting_named(om, s->name);
        /* what is left of the rows stands as it stood: the page shows them in the manifest's order, and a row's help
         * may speak of the one above it */
        if (was && was - om->settings < last && !said) { said = 1; change(ch, PS_CHANGED, "Its settings are in another order"); }
        if (was) last = (int)(was - om->settings);
        if (!was) {
            if (s->type == PS_FILES) change(ch, PS_MORE, "Reads another list of files you give it: \"%s\"", s->label);
            else if (s->type == PS_SECRET || s->type == PS_HOST || s->type == PS_TEXT) change(ch, PS_NEW, "Asks you for \"%s\" (%s)", s->label, kinds[s->type]);
            else change(ch, PS_NEW, "A setting it did not have: \"%s\" (%s)", s->label, kinds[s->type]);
            continue;
        }
        JSON_EACH(&jo, so, x) if (k++ == (int)(was - om->settings)) tw = x;
        if (was->type != s->type) { change(ch, PS_CHANGED, "Its setting \"%s\" was %s and is now %s, called \"%s\"", was->label, kinds[was->type], kinds[s->type], s->label); continue; }
        if (strcmp(was->label, s->label)) change(ch, PS_CHANGED, "Its setting \"%s\" is now called \"%s\"", was->label, s->label);
        if (s->type == PS_FILES) {
            if (s->max_items > was->max_items) change(ch, PS_MORE, "\"%s\" holds up to %d, was %d", s->label, s->max_items, was->max_items);
            /* (a file more, less or of another suffix is the item changed: said below) */
            for (int f = 0; f < s->nfiles && s->nfiles == was->nfiles; f++)
                if (!strcmp(s->files[f].suffix, was->files[f].suffix) && s->files[f].max_kb > was->files[f].max_kb)
                    change(ch, PS_MORE, "\"%s\" takes %s files of up to %d kB, was %d kB", s->label, s->files[f].suffix, s->files[f].max_kb, was->files[f].max_kb);
        }
        if (jsame(&jo, tw, &jn, e, s->type == PS_FILES ? X_ROW : X_PLAIN, 0)) continue;
        row_diff(&jo, tw, &jn, e, s->type == PS_FILES ? X_ROW : X_PLAIN, diff, sizeof diff);
        /* (a row that is not the same and of which nothing can be named: asked all the same) */
        if (diff[0] || !strcmp(was->label, s->label)) change(ch, PS_CHANGED, "Its setting \"%s\" is no longer as you accepted it (%s)", s->label, diff[0] ? diff : "something this version does not read");
    }
    /* then the versions it comes to speak, last: after the capabilities (finding 4 of a review of 4c55b54: before
     * them, a raise over many versions filled the dialog's lines, and a capability asked for anew was left out) and
     * after the settings' rows (finding 3 of a review of 61e3d8e: a new secret row went unshown behind eight lines of
     * versions).  What is left out of many is counted, and the page says how many */
    requires_wider(om, nm, ch);
out:
    json_free(&jo); json_free(&jn);
}

/* ---------------------------------------------------------------- the program (2.3's contract, the part a file shows) */

#if defined(__arm__)
#define ELF_MACHINE EM_ARM
#define ELF_CLASS ELFCLASS32
#define ELF_WHAT "32-bit ARM"
#elif defined(__x86_64__)
#define ELF_MACHINE EM_X86_64           /* the PC build runs test plugins built for the PC (5.8) */
#define ELF_CLASS ELFCLASS64
#define ELF_WHAT "x86_64 (this is the PC build)"
#elif defined(__aarch64__)
#define ELF_MACHINE EM_AARCH64
#define ELF_CLASS ELFCLASS64
#define ELF_WHAT "arm64 (this is the PC build)"
#endif

int ps_elf_check(const uint8_t *b, size_t len, char *err, size_t errsz)
{
    int stack = 0;
    if (len < sizeof(Elf32_Ehdr) || memcmp(b, ELFMAG, SELFMAG) || b[EI_CLASS] != ELF_CLASS || b[EI_DATA] != ELFDATA2LSB) { fail(err, errsz, "the program is not an ELF for %s", ELF_WHAT); return -1; }
#define PHDRS(EH, PH) { \
        EH eh; memcpy(&eh, b, sizeof eh > len ? len : sizeof eh); \
        if (len < sizeof eh || eh.e_machine != ELF_MACHINE || (eh.e_type != ET_EXEC && eh.e_type != ET_DYN)) { fail(err, errsz, "the program is not an executable for %s", ELF_WHAT); return -1; } \
        if (ELF_MACHINE == EM_ARM && (eh.e_flags & 0xff000000) != 0x05000000) { fail(err, errsz, "the program is not EABI5"); return -1; } \
        if (eh.e_phentsize != sizeof(PH) || !eh.e_phnum || eh.e_phoff > len || (size_t)eh.e_phnum * sizeof(PH) > len - eh.e_phoff) { fail(err, errsz, "the program's headers are broken"); return -1; } \
        for (unsigned i = 0; i < eh.e_phnum; i++) { \
            PH ph; memcpy(&ph, b + eh.e_phoff + i * sizeof ph, sizeof ph); \
            if (ph.p_type == PT_INTERP || ph.p_type == PT_DYNAMIC) { \
                /* a static PIE has PT_DYNAMIC and no interpreter: only the interpreter makes it a dynamic program */ \
                if (ph.p_type == PT_INTERP) { fail(err, errsz, "the program is not static (it names an interpreter): a plugin can open no library"); return -1; } } \
            if (ph.p_type == PT_GNU_STACK) { stack = 1; if (ph.p_flags & PF_X) { fail(err, errsz, "the program asks for an executable stack"); return -1; } } \
            if (ph.p_type == PT_LOAD && (ph.p_flags & PF_W) && (ph.p_flags & PF_X)) { fail(err, errsz, "the program has a segment that is writable and executable"); return -1; } \
        } }
    if (ELF_CLASS == ELFCLASS32) PHDRS(Elf32_Ehdr, Elf32_Phdr) else PHDRS(Elf64_Ehdr, Elf64_Phdr)
#undef PHDRS
    /* without the mark the kernel sets READ_IMPLIES_EXEC for the process, and every readable mapping is executable,
     * whatever the filter refused */
    if (!stack) { fail(err, errsz, "the program does not mark its stack as not executable (no PT_GNU_STACK)"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------- packages */

static void pdir(const char *id, char *out, size_t cap) { snprintf(out, cap, "%s/%s", ps_dir(), id); }

/* (a name no plugin can have: an id has no dot) */
static void stage_path(char *out, size_t cap) { snprintf(out, cap, "%s/.stage", ps_dir()); }

/* Whose the staging file is: the claim (web.c's install slot, its generation; never 0) that began it.  The slot
 * decides who may write, under web.c's lock, and lets go of that lock before the write: a piece admitted for one
 * claim could reach the file after that claim was cancelled and the next one's ps_stage_begin had made the file
 * anew, and landed at offset 0 of a stranger's package (a review, 2026-11-20).  So the store holds every write to
 * the claim it is for, under its own lock, where file and owner change together.  (The other way, the slot kept in
 * a state of its own while a piece is written and every claim waiting for it, holds web.c's states to a flash
 * write's time and adds a state to every rule of its table; this is one comparison where the file is opened.) */
static unsigned stage_gen;

int ps_stage_begin(unsigned gen, long size, char *err, size_t errsz)
{
    char p[400]; int fd;
    if (size < 1024 || size > PS_PACKAGE_MAX) { fail(err, errsz, "a package is at most %d MB", PS_PACKAGE_MAX >> 20); return -1; }
    if (room(size * 2, err, errsz)) return -1;
    mkdirs(ps_dir()); stage_path(p, sizeof p);
    ps_lock(); staged_ok = 0; stage_gen = 0;
    unlink(p);
    fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) stage_gen = gen;
    ps_unlock();
    if (fd < 0) { fail(err, errsz, "cannot write on this Echo"); return -1; }
    close(fd);
    return 0;
}

int ps_stage_chunk(unsigned gen, long off, const void *data, size_t n, char *err, size_t errsz)
{
    char p[400]; struct stat st; int fd = -1, r = -1;
    stage_path(p, sizeof p);
    ps_lock();
    /* (another claim's file is not even marked unchecked by a piece that is not for it) */
    if (!gen || gen != stage_gen) { fail(err, errsz, "this upload was ended: another install has taken its place"); ps_unlock(); return -1; }
    staged_ok = 0;
    fd = open(p, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st)) fail(err, errsz, "no upload was started");
    else if (st.st_size != off) fail(err, errsz, "the upload is at %ld, not %ld", (long)st.st_size, off);     /* in order, each piece once */
    else if (off + (long)n > PS_PACKAGE_MAX) fail(err, errsz, "a package is at most %d MB", PS_PACKAGE_MAX >> 20);
    else if (write(fd, data, n) != (ssize_t)n) fail(err, errsz, "cannot write on this Echo");
    else r = 0;
    if (fd >= 0) close(fd);
    ps_unlock();
    return r;
}

void ps_stage_drop(void) { char p[400]; stage_path(p, sizeof p); ps_lock(); staged_ok = 0; stage_gen = 0; unlink(p); ps_unlock(); }

/* a plain tar's members, by name; anything that is not one of the five regular files refuses the package */
struct member { const char *name; const uint8_t *data; size_t len; size_t max; };
static int untar(const uint8_t *b, size_t len, struct member *m, int nm, char *err, size_t errsz)
{
    size_t at = 0;
    while (at + 512 <= len && b[at]) {
        const uint8_t *h = b + at; char name[101], sz[13]; unsigned long size; char *end; int i;
        memcpy(name, h, 100); name[100] = 0; memcpy(sz, h + 124, 12); sz[12] = 0;
        size = strtoul(sz, &end, 8);
        /* a member's name is whatever bytes the package has there, and the reason below goes to the page: none of
         * the five names has anything but these */
        for (i = 0; i < 5 && strcmp(name, (const char *[]){ "manifest.json", "manifest.sig", "plugin", "LICENSE", "listing.sig" }[i]); i++) ;
        if (i == 5) for (char *c = name; *c; c++) if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e || *c == '"' || *c == '\\') *c = '?';
        if ((*end && *end != ' ') || (h[156] != '0' && h[156] != 0) || h[345]) { fail(err, errsz, "the package holds something that is not a plain file (%s)", name); return -1; }
        if (size > len - at - 512) { fail(err, errsz, "the package is cut off"); return -1; }
        for (i = 0; i < nm && strcmp(name, m[i].name); i++) ;
        if (i == nm || m[i].data) { fail(err, errsz, "the package holds a file it should not (%.60s)", name); return -1; }
        if (size > m[i].max) { fail(err, errsz, "the package's %s is too large", name); return -1; }
        m[i].data = h + 512; m[i].len = size;
        at += 512 + (size + 511) / 512 * 512;
    }
    return 0;
}

/* listing.sig against the index key: its tier (PS_OFFICIAL, PS_COMMUNITY), PS_SIDELOAD for no listing, -1 a listing
 * that does not hold (err).  The whole text is what this build would sign for this very manifest, but for the tier
 * and the index's serial, which are read (*SERIAL, -1 for none: ps_stage_check holds it to the build's least) */
static int listing_by(const uint8_t *l, size_t len, const struct ps_manifest *m, long long *serial, int *key, char *err, size_t errsz)
{
    uint8_t sig[64]; char want[400], dg[65]; const char *text, *c; size_t tl, wl; int tier, by;
    *serial = -1; if (key) *key = -1;
    if (!l) return PS_SIDELOAD;
    if (len < 130 || l[128] != '\n' || unhex(sig, (const char *)l, 64)) { fail(err, errsz, "the package's listing is broken"); return -1; }
    text = (const char *)l + 129; tl = len - 129;
    if ((by = pi_signer("", text, tl, sig, 64)) < 0) { fail(err, errsz, "the package's listing is not signed by the plugin index's key"); return -1; }
    if (key) *key = by;
    hex(dg, m->manifest_digest, 32);
    wl = (size_t)snprintf(want, sizeof want, SIGN_LISTING "id %s\nversion %ld\ntier ", m->id, m->version);
    if (tl < wl || memcmp(text, want, wl)) { fail(err, errsz, "the package's listing is for another plugin or version"); return -1; }
    c = text + wl;
    if (tl - wl > 9 && !memcmp(c, "official\n", 9)) { tier = PS_OFFICIAL; c += 9; }
    else if (tl - wl > 10 && !memcmp(c, "community\n", 10)) { tier = PS_COMMUNITY; c += 10; }
    else { fail(err, errsz, "the package's listing is broken"); return -1; }
    wl = (size_t)snprintf(want, sizeof want, "manifest %s\nserial ", dg);
    if ((size_t)(text + tl - c) < wl || memcmp(c, want, wl)) { fail(err, errsz, "the package's listing is for another manifest"); return -1; }
    /* and nothing but the serial after it: the index key signs a text made from a manifest's own fields, and one
     * that carried lines of its own in its version ("7\ntier official\nmanifest <another>\nserial 0") would otherwise
     * have bought a listing that also opens with another manifest's lines (sdk/plugcheck.py refuses to sign it) */
    { const char *d = c + wl, *e = text + tl; size_t k = 0;
      while (d + k < e && d[k] >= '0' && d[k] <= '9') k++;
      if (!k || k > 12 || d + k + 1 != e || d[k] != '\n') { fail(err, errsz, "the package's listing is broken"); return -1; }
      *serial = 0; for (size_t i = 0; i < k; i++) *serial = *serial * 10 + (d[i] - '0'); }
    return tier;
}
static int listing_check(const uint8_t *l, size_t len, const struct ps_manifest *m, long long *serial, char *err, size_t errsz)
{
    return listing_by(l, len, m, serial, NULL, err, errsz);
}
/* The tier a listing that does not hold here says it gives (its own line, for this manifest's id and version; the
 * signature not asked): PS_OFFICIAL, PS_COMMUNITY, -1 where it says neither or is no listing of this package.  Only
 * what plugin.c's self test goes by (ps_plugin listing_said): a word of the index's that this build cannot check,
 * read only to excuse the self test, never to run anything */
static int listing_said(const uint8_t *l, size_t len, const struct ps_manifest *m)
{
    char want[400]; const char *text, *c; size_t tl, wl;
    if (!l || len < 130 || l[128] != '\n') return -1;
    text = (const char *)l + 129; tl = len - 129;
    wl = (size_t)snprintf(want, sizeof want, SIGN_LISTING "id %s\nversion %ld\ntier ", m->id, m->version);
    if (tl < wl || memcmp(text, want, wl)) return -1;
    c = text + wl;
    if (tl - wl >= 9 && !memcmp(c, "official\n", 9)) return PS_OFFICIAL;
    if (tl - wl >= 10 && !memcmp(c, "community\n", 10)) return PS_COMMUNITY;
    return -1;
}

/* ---- what was accepted (state/plugins/<id>/accepted), for the package installed:
 *   id <id>
 *   key <the author key, hex>
 *   digest <hex>           the accepted section's digest (ACCEPTED_HEAD)
 *   how <words>, when <s>
 * What the owner accepted with the button, or as the same or less of what was accepted before.  "requires" is not
 * in the section: a scope's next version stays within its capability's line (sdk/PROTOCOL.md "Versions"), so an
 * update that requires a later one asks nothing for it. */
struct acc { uint8_t key[32], digest[32]; char how[80]; long long when; };

static int acc_read(const char *id, struct acc *a)
{
    char p[400], line[200]; FILE *f; int have = 0;
    memset(a, 0, sizeof *a);
    snprintf(p, sizeof p, "%s/%s/accepted", ps_dir(), id);
    if (!(f = ps_fopen(p))) return -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "key ", 4) && strlen(line + 4) == 64 && !unhex(a->key, line + 4, 32)) have |= 1;
        if (!strncmp(line, "digest ", 7) && strlen(line + 7) == 64 && !unhex(a->digest, line + 7, 32)) have |= 2;
        if (!strncmp(line, "how ", 4)) snprintf(a->how, sizeof a->how, "%s", line + 4);
        if (!strncmp(line, "when ", 5)) a->when = atoll(line + 5);
    }
    fclose(f);
    return have == 3 ? 0 : -1;
}

static int acc_write(const char *id, const struct acc *a)
{
    char p[400], text[400], k[65], d[65]; size_t n;
    hex(k, a->key, 32); hex(d, a->digest, 32);
    n = (size_t)snprintf(text, sizeof text, "id %s\nkey %s\ndigest %s\nhow %.79s\nwhen %lld\n", id, k, d, a->how, a->when);
    snprintf(p, sizeof p, "%s/%s/accepted", ps_dir(), id);
    return statefile_write(p, 0644, text, n);
}

/* D is M's accepted section, in the form this build writes or in one an earlier build wrote (ACCEPTED_HEAD's comment) */
static int accepted_is(const struct ps_manifest *m, const uint8_t *d)
{
    if (!d) return 0;
    if (!memcmp(d, m->accepted, 32)) return 1;
    for (int i = 0; i < PS_ACCEPTED_WAS; i++) if (!memcmp(d, m->accepted_was[i], 32)) return 1;
    return 0;
}

/* The package of ID: its manifest's text (the caller's to free) and what it parses to; NULL where there is none, or
 * none this build reads, or another plugin's */
static char *pkg_manifest(const char *id, struct ps_manifest *om, size_t *len)
{
    char p[700], *text;
    snprintf(p, sizeof p, "%s/%s/pkg/manifest.json", ps_dir(), id);
    if (!(text = read_file(p, MANIFEST_MAX, len))) return NULL;
    if (ps_manifest_parse(text, *len, om, NULL, 0) || strcmp(om->id, id)) { free(text); return NULL; }
    return text;
}

/* The manifest the owner accepted, to hold an update against: the package installed, where `accepted` says it is
 * what was accepted.  Its text (the caller's to free) and what it parses to, or NULL: then there is nothing to hold
 * an update against */
static char *accepted_manifest(const char *id, const struct acc *a, struct ps_manifest *om, size_t *len)
{
    char *text = pkg_manifest(id, om, len);
    if (text && accepted_is(om, a->digest)) return text;
    free(text);
    return NULL;
}

/* The version of the package of ID.  *known 1: by its manifest (one for a newer hassmic says it too: its version is
 * read before what it requires is held to this build's); 2: its manifest is one this build does not read, by its
 * listing (which the index key signed with the version in it); 0: there is a package and nothing says which
 * version; -1: no package there */
static long pkg_version(const char *id, int *known)
{
    char p[700], *text, want[80]; size_t len = 0, ll = 0, wl; uint8_t *l, sig[64]; long v = 0;
    struct ps_manifest *om = malloc(sizeof *om); struct stat st;
    *known = -1;
    snprintf(p, sizeof p, "%s/%s/pkg/manifest.json", ps_dir(), id);
    if (!om || lstat(p, &st)) { free(om); return 0; }
    *known = 0;
    if ((text = pkg_manifest(id, om, &len))) { v = om->version; *known = 1; free(text); free(om); return v; }
    if ((text = read_file(p, MANIFEST_MAX, &len))) {
        if (ps_manifest_parse(text, len, om, NULL, 0) && om->newer && !strcmp(om->id, id)) { v = om->version; *known = 1; }
        free(text);
        if (*known == 1) { free(om); return v; }
    }
    free(om);
    snprintf(p, sizeof p, "%s/%s/pkg/listing.sig", ps_dir(), id);
    if (!(l = read_file(p, 1024, &ll))) return 0;
    wl = (size_t)snprintf(want, sizeof want, SIGN_LISTING "id %s\nversion ", id);
    if (ll > 129 + wl && l[128] == '\n' && !unhex(sig, (const char *)l, 64) && pi_signed("", l + 129, ll - 129, sig, 64) && !memcmp(l + 129, want, wl)) {
        const char *c = (const char *)l + 129 + wl; int n = 0;
        for (; n < 10 && c[n] >= '0' && c[n] <= '9'; n++) v = v * 10 + (c[n] - '0');
        if (n && c[n] == '\n') *known = 2; else v = 0;
    }
    free(l);
    return v;
}

/* the tier of the package of ID installed here, by its listing as it came (no least serial: what is installed keeps
 * the tier it was installed with); PS_SIDELOAD when it has none */
static int installed_tier(const char *id)
{
    char p[700]; int best = PS_SIDELOAD;
    struct ps_manifest *om = malloc(sizeof *om); size_t ml = 0, ll = 0; char *text = om ? pkg_manifest(id, om, &ml) : NULL; uint8_t *l; long long ls; int t;
    if (text) {
        snprintf(p, sizeof p, "%s/%s/pkg/listing.sig", ps_dir(), id);
        l = read_file(p, 1024, &ll);
        t = listing_check(l, ll, om, &ls, NULL, 0); free(l);
        if (t > best) best = t;
    }
    free(text); free(om);
    return best;
}

/* ---- 5.2's move to a new index key: the index's listing for the version installed here (a review of a30d6ed, K2).
 * A key's last step is a hassmic that reads the new key alone; a plugin installed with the old key's listing is a
 * file's under it (its listing no longer holds), and a default Echo (community plugins off) does not start it, the
 * wake word engine's plugin neither.  The same step back to a build of one key (an older online build, ROLLBACK,
 * boot.sh's fall back to the factory copy) does it to a plugin only the other key listed.  And the same version from
 * the index brought nothing: "Browse plugins" said "Installed".  So the index's listing of the very package installed
 * here (the same manifest, whose digest the listing names, and with it the same program: the manifest names its
 * digest) is taken in place of the one it came with: no button, no install; settings, storage, files and the
 * running process stay, the package being the same.  Only where it says at least as much: a tier not lower than the
 * installed listing's under this build's keys (none, or one that no longer holds: a file's); and only where it is new
 * in what counts: a higher tier, another index key (the move itself: before the build of the new key alone, every
 * Echo that looks at the index has its listings by the new key), or the installed one under the build's least index
 * serial (5.5).  A newer serial of the same key and tier is nothing: no write per publish of the index */
struct relisting { int tier, key; long long serial; };
static int relisting_of(const char *id, struct ps_manifest *om, struct relisting *r)     /* ps_lock held */
{
    char p[700]; size_t ml = 0, ll = 0; char *text = pkg_manifest(id, om, &ml); uint8_t *l;
    if (!text) return -1;
    free(text);
    snprintf(p, sizeof p, "%s/%s/pkg/listing.sig", ps_dir(), id);
    l = read_file(p, 1024, &ll);
    r->tier = listing_by(l, ll, om, &r->serial, &r->key, NULL, 0); free(l);
    if (r->tier < 0) { r->tier = PS_SIDELOAD; r->key = -1; r->serial = -1; }
    return 0;
}
static int relist_better(const struct relisting *old, int tier, int key, long long serial)
{
    if (tier < PS_COMMUNITY || serial < pi_least_index() || tier < old->tier) return 0;
    return tier > old->tier || key != old->key || old->serial < pi_least_index();
}

int ps_relist_wanted(const char *id, long version, const uint8_t manifest[32], int tier, int key, long long serial)
{
    struct ps_manifest *om; struct relisting r; int w = 0;
    if (!ps_id_ok(id) || !(om = malloc(sizeof *om))) return 0;
    ps_lock();
    if (!relisting_of(id, om, &r) && om->version == version && !memcmp(om->manifest_digest, manifest, 32)) w = relist_better(&r, tier, key, serial);
    ps_unlock(); free(om);
    return w;
}

/* A listing the index may list anew, as far as the listings installed say (the tier the index lists now is only
 * the index's to say): one that does not hold here (2: its plugin is a file's meanwhile, with community plugins off
 * stopped, the wake word engine's too), one by the first of two keys while the build reads two (5.2's second key is
 * the new one: a step-(1) build reads the index daily until its listings are the new key's, which costs one
 * index.json a day for the time of a key's change), one under the least (1).  A file's (no listing) is none, and so
 * is a version SKIP names: one the last look that had the index found not listed there, which no look can move (a
 * review of 3fcd319, S4: the daily look read index.json for good for a version the index had dropped) */
int ps_relist_reason(const struct ps_idv *skip, int nskip)
{
    const struct ps_plugin *l; uint8_t pk[PI_KEYS][32]; int n, nk = pi_keys(pk), r = 0;
    ps_lock(); n = ps_list(&l);
    for (int i = 0; i < n && r < 2; i++) {
        int s = 0;
        if (!l[i].here || l[i].unread) continue;
        for (int k = 0; k < nskip && !s; k++) s = skip[k].version == l[i].m.version && !strcmp(skip[k].id, l[i].m.id);
        if (s) continue;
        if (l[i].listing_unread) r = 2;
        else if (l[i].tier != PS_SIDELOAD && ((nk == 2 && l[i].listing_key == 0) || l[i].listing_serial < pi_least_index())) r = 1;
    }
    ps_unlock();
    return r;
}

int ps_relist(const uint8_t *pkg, size_t len, char *err, size_t errsz)
{
    struct member mem[] = { { "manifest.json", NULL, 0, MANIFEST_MAX }, { "manifest.sig", NULL, 0, 64 }, { "plugin", NULL, 0, PS_PACKAGE_MAX },
                            { "LICENSE", NULL, 0, 65536 }, { "listing.sig", NULL, 0, 1024 } };
    struct ps_manifest *m = malloc(sizeof *m), *om = malloc(sizeof *om); struct relisting r; char p[700]; long long serial; int t, key, rc = -1;
    /* (what fails here for the moment, memory or the flash, is -2: asked again, and the row says why.  It was -1,
     * which plugin_fetch.c takes for the package's own answer, "nothing to take", and never asked again for that
     * package: a full flash at the one look left the plugin a file's under the new key's build; a review of 3c5d7ff, R6) */
    if (!m || !om) { fail(err, errsz, "out of memory"); rc = -2; goto out; }
    if (untar(pkg, len, mem, 5, err, errsz)) goto out;
    if (!mem[0].data || !mem[4].data) { fail(err, errsz, "the package lacks its manifest or its listing"); goto out; }
    if (ps_manifest_parse((const char *)mem[0].data, mem[0].len, m, err, errsz)) goto out;
    if ((t = listing_by(mem[4].data, mem[4].len, m, &serial, &key, err, errsz)) < PS_COMMUNITY) { if (t >= 0) fail(err, errsz, "the package has no listing"); goto out; }
    ps_lock();
    if (relisting_of(m->id, om, &r) || memcmp(om->manifest_digest, m->manifest_digest, 32)) { fail(err, errsz, "%s is not installed here as this very version", m->id); ps_unlock(); goto out; }
    if (!relist_better(&r, t, key, serial)) { rc = 0; ps_unlock(); goto out; }
    snprintf(p, sizeof p, "%s/%s/pkg/listing.sig", ps_dir(), m->id);
    if (statefile_write(p, 0644, mem[4].data, mem[4].len)) { fail(err, errsz, "cannot write %s", p); rc = -2; ps_unlock(); goto out; }
    fprintf(stderr, "plugins: %s version %ld: the plugin index's listing taken (%s, by its %s key, serial %lld; it was %s)\n", m->id, m->version,
            t == PS_OFFICIAL ? "official" : "community", key ? "second" : "first", serial,
            r.tier == PS_SIDELOAD ? "a file's: no listing that holds" : r.tier == PS_OFFICIAL ? "official" : "community");
    ps_unlock();
    rc = 1;
out:
    free(m); free(om);
    return rc;
}

static void config_tidy(const struct ps_manifest *m);

int ps_stage_check(struct ps_manifest *m, int *tier, int *button, struct ps_changes *ch, char *err, size_t errsz)
{
    char p[400]; size_t len = 0; uint8_t *b, pinned[32], dg[32]; int r = -1, t, unknown = 0, was, had_tier = -1; FILE *f; struct acc a; long long lserial;
    struct ps_changes *c = ch ? ch : calloc(1, sizeof *c);
    struct member mem[] = { { "manifest.json", NULL, 0, MANIFEST_MAX }, { "manifest.sig", NULL, 0, 64 }, { "plugin", NULL, 0, PS_PACKAGE_MAX },
                            { "LICENSE", NULL, 0, 65536 }, { "listing.sig", NULL, 0, 1024 } };
    if (!c) { fail(err, errsz, "out of memory"); return -1; }
    memset(c, 0, sizeof *c);
    stage_path(p, sizeof p);
    ps_lock(); staged_ok = 0;
    if (!(b = read_file(p, PS_PACKAGE_MAX, &len))) { fail(err, errsz, "no package was uploaded"); ps_unlock(); if (c != ch) free(c); return -1; }
    if (untar(b, len, mem, 5, err, errsz)) goto out;
    if (!mem[0].data || !mem[1].data || !mem[2].data || mem[1].len != 64) { fail(err, errsz, "the package lacks its manifest, its signature or its program"); goto out; }
    if (ps_manifest_parse((const char *)mem[0].data, mem[0].len, m, err, errsz)) goto out;
    /* the author's signature; an installed plugin's author is pinned: another key is another plugin.  A key under
     * which any signature holds (edkey.h: small order) is no author's: anyone signs for it, and pinned, anyone's
     * package would be its update (a review of a30d6ed, K3) */
    if (ed_key_weak(m->author_key)) { fail(err, errsz, "the manifest's author key is one under which any signature holds: no author's key, refused"); goto out; }
    { size_t hl = strlen(SIGN_MANIFEST); uint8_t *msg = malloc(hl + mem[0].len); int bad;
      if (!msg) { fail(err, errsz, "out of memory"); goto out; }
      memcpy(msg, SIGN_MANIFEST, hl); memcpy(msg + hl, mem[0].data, mem[0].len);
      bad = crypto_eddsa_check(mem[1].data, m->author_key, msg, hl + mem[0].len); free(msg);
      if (bad) { fail(err, errsz, "the manifest is not signed by the author key it names"); goto out; } }
    snprintf(p, sizeof p, "%s/%s/author.pub", ps_dir(), m->id);
    if ((f = ps_fopen(p))) {
        int ok = fread(pinned, 1, 32, f) == 32; fclose(f);
        if (ok && memcmp(pinned, m->author_key, 32)) { fail(err, errsz, "%s is installed from another author's key: this is not an update of it (remove it first to install this one)", m->id); goto out; }
    }
    crypto_blake2b(dg, 32, mem[2].data, mem[2].len);
    if (memcmp(dg, m->digest, 32)) { fail(err, errsz, "the program is not the one the manifest names"); goto out; }
    if (ps_elf_check(mem[2].data, mem[2].len, err, errsz)) goto out;
    if ((t = listing_check(mem[4].data, mem[4].len, m, &lserial, err, errsz)) < 0) goto out;
    /* A listing made by an index older than this build takes (5.5's least serial, raised at a release) is no word of
     * the index's any more: the version may have been dropped from it since, and a file, a copy or a push would keep
     * it installable as official for good.  It counts as no listing: a sideload, which asks for the button and is
     * neither copied nor pushed.  (The Echo's own fetch brings the site's packages, listed under its index's serial,
     * which is at least the least.)  At install only: what is installed keeps the tier it came with (ps_scan) */
    if (t != PS_SIDELOAD && lserial < pi_least_index()) {
        change(c, PS_CHANGED, "Listed by an index older than this hassmic takes (serial %lld, it takes %lld): treated as a package from a file. Get it from the plugin index again for its listing", lserial, pi_least_index());
        t = PS_SIDELOAD;
    }
    *tier = t;
    /* withdrawn by the index's revocation list (5.6): every tier (the index key that makes a plugin official may
     * withdraw it), a sideload by its id or its author's key too; refused whatever was accepted, and whichever way
     * the package came.  "Run it anyway" is for what is installed, never an install */
    { char why[201];
      if (pi_withdrawn(m->id, m->version, m->author_key, why, sizeof why)) { fail(err, errsz, "the plugin index withdrew this version of %s: %s", m->id, why); goto out; } }
    /* Community plugins are the owner's to let in, by the switch on the Plugins page (2.7, 3.3): while that is off,
     * nothing but an official package is taken, whichever way it came (a file, the index's community listing, a copy
     * from another Echo, a push).  ps_stage_install's look under the lock asks again: switched off between the check
     * and the press, nothing is written */
    if (t != PS_OFFICIAL && !ps_community(-1)) {
        fail(err, errsz, "%s is %s, and community plugins are switched off on this Echo: switch them on on its Plugins page first", m->id,
             t == PS_COMMUNITY ? "a community plugin (the plugin index lists it, not as official)" : "not listed as official by the plugin index (a file, or a listing older than this hassmic takes)");
        goto out;
    }
    /* was: this author's plugin was accepted here before (else `a` says nothing, and all of it is to be accepted) */
    if (!(was = !acc_read(m->id, &a) && !memcmp(a.key, m->author_key, 32))) memset(&a, 0, sizeof a);
    /* (one for a newer hassmic, its requires past this build's, never got past ps_manifest_parse above) */
    if (runnable(m, *tier, err, errsz) || ps_partner(m, *tier, 1, err, errsz)) goto out;
    if (*tier == PS_OFFICIAL && m->version < ps_official_least(m->id)) { fail(err, errsz, "this version of %s was withdrawn: this hassmic runs it from version %ld", m->id, ps_official_least(m->id)); goto out; }
    /* Never back: a version not higher than the one installed is refused, whoever signed it.  A package whose
     * manifest this build does not read says its version by its listing if it has one (signed by the index key), and
     * one for a newer hassmic by its manifest (its version is read before what it requires); one that says it by
     * nothing cannot be held against: then the owner is asked, and told why */
    { int known; long have = pkg_version(m->id, &known);
      if (!known) unknown = 1;
      if (known > 0 && m->version <= have) { fail(err, errsz, "version %ld of %s is not newer than the one installed (%ld)", m->version, m->id, have); goto out; } }
    { int np = 0; DIR *d = opendir(ps_dir()); struct dirent *e; int have = 0;
      if (d) { while ((e = readdir(d))) if (ps_id_ok(e->d_name)) { np++; have |= !strcmp(e->d_name, m->id); } closedir(d); }
      if (!have && np >= PS_MAX) { fail(err, errsz, "%d plugins are installed: remove one first", PS_MAX); goto out; } }
    if (programs_bytes(m->id) + (long long)mem[2].len > PS_PROGRAMS_MAX) { fail(err, errsz, "the plugins on this Echo are %d MB together at most", PS_PROGRAMS_MAX >> 20); goto out; }
    /* The button: for a plugin the owner has not accepted, and for an update that is anything but what was accepted
     * or less of it (3.3, 5.4; wider() says what "less" is, and it is little).  Held against the manifest the owner
     * accepted (accepted_manifest).  The same section to the byte needs no button and no look, but the limits are
     * outside it and are shown when they changed (5.4): so the old manifest is read then too.  A section accepted
     * in an earlier form of the digest is never "the same to the byte": it is compared */
    { if (!was) *button = 1;
      else {
          struct ps_manifest *om = malloc(sizeof *om); char *old = NULL; size_t ol = 0;
          int same = !memcmp(a.digest, m->accepted, 32);
          *button = !same;
          if (om && (old = accepted_manifest(m->id, &a, om, &ol))) {
              wider(old, ol, om, (const char *)mem[0].data, mem[0].len, m, c);
              c->update = 1; *button = c->ask;
          } else if (same) {
              /* (the installed manifest is not one this build reads: what it required is not known, so a raise into
               * a widening version is held against version 1 of each scope, and asks) */
              c->update = 1; requires_wider(NULL, m, c); *button = c->ask;
          }
          free(old); free(om);
      } }
    if (unknown) {
        change(c, PS_CHANGED, "The version installed here has a manifest this hassmic cannot read: whether this one is newer cannot be checked");
        *button = 1; c->unsure = 1;
    }
    /* Where it comes from is part of what was accepted: an update from a lower tier than the version installed
     * (official -> community -> a file) is wider, whoever brings it (the page, a push, the Echo's own fetch, a copy).
     * It was not: whoever had an official plugin's author key (leaked, or a plugin's repository taken over) put an
     * unlisted package of his own in its place without the button from any approved browser, and a push or the
     * wake engine's own fetch would have taken it too (a review, 2026-11-21).  Held against the tier of the package
     * installed */
    if (was && (had_tier = installed_tier(m->id)) > *tier) {
        static const char *const tn[] = { "a file, not listed", "the plugin index, community", "the plugin index, official" };
        change(c, PS_CHANGED, "Comes from %s; the version installed here came from %s", tn[*tier], tn[had_tier]);
        *button = 1;
    }
    staged = *m; staged_tier = *tier; staged_ok = 1;
    r = 0;
out:
    free(b);
    if (c != ch) free(c);
    ps_unlock();
    return r;
}

static int put(const char *dir, const char *name, mode_t mode, const void *data, size_t len)
{
    char p[640]; snprintf(p, sizeof p, "%s/%s", dir, name);
    return statefile_write(p, mode, data, len);
}

static int pkg_there(const char *id)
{
    char p[500]; struct stat st;
    snprintf(p, sizeof p, "%s/%s/pkg", ps_dir(), id);
    return !lstat(p, &st) && S_ISDIR(st.st_mode);
}

/* What an install that did not end left in ID's folder (ps_stage_install writes a package into "pkg.new" and puts
 * it in place with two renames, the one it replaces being "pkg.old" in between): a "pkg.new" is a package nobody
 * finished and goes; a "pkg.old" goes when "pkg" is there (the new one is in place) and comes back as "pkg" when it
 * is not (hassmic ended between the two renames: the package that was installed is the one that stays).  Lock held,
 * or at start; from ps_scan for every plugin and before an install */
static void pkg_settle(const char *id)
{
    char dir[400], q[440], to[440]; struct stat st; int moved = 0;
    pdir(id, dir, sizeof dir);
    snprintf(q, sizeof q, "%s/pkg.old", dir);
    if (!lstat(q, &st)) {
        snprintf(to, sizeof to, "%s/pkg", dir);
        if (!pkg_there(id) && !rename(q, to)) fprintf(stderr, "plugins: %s: an install did not end: the package that was installed before it is back in its place\n", id);
        else rm_rf(q);
        moved = 1;
    }
    snprintf(q, sizeof q, "%s/pkg.new", dir);
    if (!lstat(q, &st)) { rm_rf(q); moved = 1; }
    if (moved) statefile_sync(dir);
}

/* The highest version of each plugin that was ever installed here, by any way: state/
 * plugins/.installed ("<id> <version>" a line; a dot: no plugin's id), only ever rising, gone only with a factory
 * reset (which empties state/).  What it is for: "never back" is held against what is installed, and Remove needs
 * no button, so a browser that may remove (any approved one, a vouched one too) took the plugin off and had an
 * older official version copied in on a voucher: the Echo ran version 1 where its owner had installed version 2
 * with its button (a review, 2026-11-20).  web.c's sync (rule 8) reads this beside the newest version seen listed,
 * and ps_stage_install holds a vouched install to it once more as it writes.  An install with this Echo's own
 * button is not held to it: taking an older version on purpose is the owner's, at the Echo.
 * FLOOR_MAX ids.  The one beyond them has no floor, and from then on none is known for it: ps_installed_max says
 * LONG_MAX for every id (closed: a sync asks for the button), for good, until a factory reset.  That is kept in the
 * file as a line "+" (floors_over; a build before it skips the line): it was in memory only, so after the next
 * start the 65th plugin's floor read as 0, and while it stood no floor was raised at all (a review, 2026-11-20).
 * The ids the file has keep rising all the same, so what it says of them stays true.  A file that is there and
 * cannot be read: closed as well (floors_blind), and nothing is written into it.
 * When it is raised: once the package is in its place (ps_stage_install), so that a version that never arrived is
 * not one that "was installed here".  The window that leaves, hassmic ending between the package and this file, is
 * closed from what is installed: ps_scan raises each plugin's floor to the highest version it finds in its folder
 * (at every scan, the first at start, before anything can remove it), and ps_remove raises it to what it is about
 * to remove.  A raise that could not be written (the package in place, or about to go; the install itself goes on)
 * stays raised in memory and is owed (floors_owed): closed as above until the file is written as memory has it,
 * which every later raise does and every scan tries.  It was one id and version kept aside, and "more than one
 * such" a state that nothing but a restart ended: two plugins installed while the flash refused, and no plugin
 * was taken on a voucher until hassmic was started again (a review, 2026-11-20).  Memory holds any number of them,
 * a removed plugin's too, and one write that works settles all.  What is left: the write failing until a restart
 * on a flash that removed the folder.
 * Lock held by the callers */
#define FLOOR_MAX 64
static struct { char id[PS_ID_MAX]; long version; } floors[FLOOR_MAX];
static int nfloors, floors_read, floors_blind;
static int floors_over, floors_over_saved;     /* more ids than FLOOR_MAX came by; ... and the file says so */
static int floors_owed;                         /* memory is ahead of the file: a raise's write failed */

static void floors_path(char *p, size_t cap) { snprintf(p, cap, "%s/.installed", ps_dir()); }

static void floors_load(void)
{
    char p[400], line[80]; FILE *f; struct stat st;
    if (floors_read) return;
    floors_read = 1; floors_path(p, sizeof p);
    if (!(f = ps_fopen(p))) {
        if (!lstat(p, &st) || errno != ENOENT) { floors_blind = 1; fprintf(stderr, "plugins: %s is there and cannot be read: which versions were installed here before is not known, so no plugin is taken on another Echo's voucher until it can\n", p); }
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char id[PS_ID_MAX]; long v;
        if (line[0] == '+') { floors_over = floors_over_saved = 1; continue; }
        if (sscanf(line, "%24s %ld", id, &v) != 2 || !ps_id_ok(id) || v < 0) continue;
        if (nfloors == FLOOR_MAX) { floors_over = 1; continue; }        /* (no build writes that many: closed, and said in the file at the next write) */
        snprintf(floors[nfloors].id, sizeof floors[0].id, "%s", id); floors[nfloors++].version = v;
    }
    fclose(f);
    if (floors_over) fprintf(stderr, "plugins: more than %d plugins were installed here over time: no plugin is taken on another Echo's voucher\n", FLOOR_MAX);
}

/* the file as memory has it: 0 (nothing is owed any more), or -1
 * (HASSMIC_PLUGIN_FAIL_FLOOR, a file: while it exists this write fails, for the tests) */
static int floors_write(void)
{
    char p[400], *t; size_t cap = FLOOR_MAX * 40 + 4, o = 0; int r; const char *e = getenv("HASSMIC_PLUGIN_FAIL_FLOOR");
    if ((e && !access(e, F_OK)) || !(t = malloc(cap))) return -1;
    for (int i = 0; i < nfloors; i++) o += (size_t)snprintf(t + o, cap - o, "%s %ld\n", floors[i].id, floors[i].version);
    if (floors_over) o += (size_t)snprintf(t + o, cap - o, "+\n");
    floors_path(p, sizeof p); mkdir(ps_dir(), 0700);
    r = statefile_write(p, 0600, t, o);
    free(t);
    if (!r) { floors_over_saved = floors_over; floors_owed = 0; }
    return r;
}

/* ID is installed in VERSION.  Not on the flash: kept in memory all the same and owed (the floor's comment) */
static void floor_raise(const char *id, long version)
{
    int k;
    floors_load();
    if (floors_blind) return;                   /* (nothing to write into: the sync is closed by it, an install with the button is not) */
    for (k = 0; k < nfloors && strcmp(floors[k].id, id); k++) ;
    if (k < nfloors && floors[k].version >= version) return;
    if (k == nfloors && nfloors == FLOOR_MAX) {
        if (!floors_over) fprintf(stderr, "plugins: more than %d plugins were installed here over time: the versions of further ones are not kept, and none is taken on another Echo's voucher from now on\n", FLOOR_MAX);
        floors_over = 1;
        if (!floors_over_saved) floors_write();     /* (closed either way; every scan comes by here again) */
        return;
    }
    if (k == nfloors) snprintf(floors[nfloors++].id, sizeof floors[0].id, "%s", id);
    floors[k].version = version;
    if (floors_write() && !floors_owed) {
        floors_owed = 1;
        fprintf(stderr, "plugins: %s: that version %ld was installed here could not be written: no plugin is taken on another Echo's voucher until it could\n", id, version);
    }
}

static int floors_closed(void) { return floors_blind || floors_over || floors_owed; }

long ps_installed_max(const char *id)
{
    long v = 0;
    ps_lock();
    floors_load();
    if (floors_closed()) v = LONG_MAX;
    else for (int k = 0; k < nfloors; k++) if (!strcmp(floors[k].id, id)) v = floors[k].version;
    ps_unlock();
    return v;
}

/* the tests: as after a restart of hassmic, the file read anew */
void ps_floors_forget(void)
{
    ps_lock(); nfloors = floors_read = floors_blind = floors_over = floors_over_saved = floors_owed = 0; ps_unlock();
}

/* the version of ID's package where it can be told, -1: none */
static long folder_top(const char *id)
{
    int known; long v = pkg_version(id, &known);
    return known > 0 ? v : -1;
}

/* What a plugin's FIRST install left when hassmic ended before its package was in place: the folder with the
 * unfinished "pkg.new" and perhaps the author's key (written after the members, before the package goes in), no
 * "pkg", nothing accepted, nothing stored.  It was listed as a plugin "nothing of it is left but its folder", and the
 * key in it was pinned for an id nobody had installed.  1: it was that, and is gone.  Lock held; before pkg_settle,
 * which would take the "pkg.new" that tells it from any other folder without a package */
static int never_arrived(const char *id)
{
    char dir[400], p[500]; DIR *d; struct dirent *e; int other = 0, neu = 0;
    pdir(id, dir, sizeof dir);
    if (!(d = opendir(dir))) return 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, "pkg.new")) neu = 1;
        else if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcmp(e->d_name, "author.pub")) other = 1;
    }
    closedir(d);
    /* (only that shape, with or without the key: a folder with anything else in it is listed with its reason and
     * Remove, as ever) */
    if (other || !neu) return 0;
    snprintf(p, sizeof p, "%s/pkg.new", dir); rm_rf(p);
    fprintf(stderr, "plugins: %s: its first install did not end and left only its folder: removed\n", id);
    rm_rf(dir);
    return 1;
}

/* The decision is made here, not brought along.  A caller's "the check said no button" is a reading of the store
 * at another moment: `expect` binds the bytes, and nothing bound what the check had found on file.  The plugin's
 * folder gone in between (the owner's Remove), acc_read below failed, the install counted as the plugin's first and
 * a fresh acceptance was written that nobody had pressed a button for (a review, 2026-11-20).  So the whole check
 * runs again here, under the lock that the files are then written under and that ps_remove and every other writer
 * of the store take: what it says now is what PRESS is held against, and a Remove comes wholly before it (the
 * check finds nothing accepted: the button) or wholly after (it removes what was installed). */
int ps_stage_install(const char *how, const uint8_t expect[32], int press, char *err, size_t errsz)
{
    char p[400], dir[400], pkg[440], neu[448], old[448], text[500]; size_t len = 0; uint8_t *b; int r = -1, fresh, tier = 0, button = 1, unsure, had; struct acc a;
    struct member mem[] = { { "manifest.json", NULL, 0, MANIFEST_MAX }, { "manifest.sig", NULL, 0, 64 }, { "plugin", NULL, 0, PS_PACKAGE_MAX },
                            { "LICENSE", NULL, 0, 65536 }, { "listing.sig", NULL, 0, 1024 } };
    struct ps_manifest *cm = malloc(sizeof *cm); struct ps_changes *cc = malloc(sizeof *cc);
    stage_path(p, sizeof p);
    if (!cm || !cc) { free(cm); free(cc); fail(err, errsz, "out of memory"); return -1; }
    ps_lock();
    if (!staged_ok) { fail(err, errsz, "no checked package waits"); free(cm); free(cc); ps_unlock(); return -1; }
    r = ps_stage_check(cm, &tier, &button, cc, err, errsz); unsure = cc->unsure;
    free(cm); free(cc);
    if (r) { ps_unlock(); return -1; }
    r = -1;
    /* the very package the caller saw checked (and the owner saw listed): another one checked since is not it */
    if (expect && memcmp(expect, staged.manifest_digest, 32)) { fail(err, errsz, "another package was uploaded since this one was checked"); ps_unlock(); return -1; }
    if (press == PS_NO_PRESS && button) {
        fail(err, errsz, "%s needs the action button now: what is installed on this Echo changed since this package was checked. Install it again", staged.id);
        ps_unlock(); return PS_NEEDS_BUTTON;
    }
    /* A voucher stands in for the press for what a press may accept of an official package, where "never back"
     * could be held, and for no version older than one that was installed here before.  Of web.c sync_judge's
     * rules these are the ones that are the store's own facts, and they are looked at again here, under the
     * store's lock, as the package is written: 5 (official), 6 (the check itself, above), 7 (`unsure`) and the
     * installed half of 8 (ps_installed_max's floor).  The others are held as sync_judge read them a moment
     * before: 1 to 4 are about the voucher, which does not change; 8's other half (the newest version seen
     * listed, plugin_fetch.c) only rises, by a look at the releases that ends in between, and 9 (`plugins` on) can
     * be switched off in between: either way what is installed is what was right when it was judged, as if the
     * install had ended a moment sooner; 10 is the note, written before this is called */
    floors_load();
    { long least = 0;
      for (int k = 0; k < nfloors; k++) if (!strcmp(floors[k].id, staged.id)) least = floors[k].version;
      if ((press == PS_VOUCHED || press == PS_VOUCHED_PRESSED) && (tier != PS_OFFICIAL || unsure || floors_closed() || staged.version < least)) {
          fail(err, errsz, "%s needs this Echo's own action button now: what is installed here, or was, changed since this package was checked", staged.id);
          ps_unlock(); return PS_NEEDS_BUTTON;
      } }
    /* request.audio with a way out: the second press, here, whatever stood for the first (3.3; "request.audio's two
     * rules" above).  After the voucher's own rules: a voucher that cannot stand for the first press is the plain
     * button's case (both presses here), not a second screen.  Nothing written: the caller asks for it on its own
     * screen and comes again with both */
    if (button && ps_second(&staged, NULL, 0) && press != PS_PRESSED2 && press != PS_VOUCHED_PRESSED) {
        fail(err, errsz, "%s hears every request and can send what it hears off this Echo: it wants a second press of the action button", staged.id);
        ps_unlock(); return PS_NEEDS_SECOND;
    }
    if (!(b = read_file(p, PS_PACKAGE_MAX, &len))) { fail(err, errsz, "no checked package waits"); ps_unlock(); return -1; }
    if (untar(b, len, mem, 5, err, errsz)) goto out;
    crypto_blake2b((uint8_t *)text, 32, mem[0].data, mem[0].len);
    if (memcmp(text, staged.manifest_digest, 32)) { fail(err, errsz, "the package changed after it was checked"); goto out; }
    pdir(staged.id, dir, sizeof dir); snprintf(pkg, sizeof pkg, "%s/pkg", dir);
    snprintf(neu, sizeof neu, "%s.new", pkg); snprintf(old, sizeof old, "%s.old", pkg);
    fresh = access(dir, F_OK) != 0;
    if (!fresh) pkg_settle(staged.id);
    memset(&a, 0, sizeof a); memcpy(a.key, staged.author_key, 32);
    /* The package is in its folder whole, or it is not there.  Written member by member into pkg/ as it was,
     * a write that failed between two of them left the new program under the old manifest: "not installed" as the
     * answer, and after a restart the plugin ran, a program nothing had accepted under a manifest that named
     * another (a review, 2026-11-20: an update on a voucher, its note dropped as the install had "failed").  So
     * every member goes into a folder beside it (each through statefile.c: on the flash before its name), and the
     * folder is put in place by name: the one there aside as "pkg.old", the new one in, the names on the flash,
     * the old one removed.  hassmic ending anywhere in that leaves one whole package, which pkg_settle finds: the
     * old one until the second rename, the new one after it.  A fresh folder each time also means nothing of the
     * package before stays (a listing.sig under an update without one, which is a sideload from then on).
     * `accepted`, the `enabled` mark and the settings' tidying come after and are not part of that: a failure
     * there leaves the new package in place with the acceptance of the old one, which runs it only if it asks
     * for nothing beyond that (ps_scan's `accepted`), and web.c settles a sync's note by the package in the folder.
     * The floor (.installed) comes after too, and its failure fails nothing (below).
     * (HASSMIC_PLUGIN_FAIL_PACKAGE, a file: while it exists the manifest's write fails, for the tests.) */
    rm_rf(neu); mkdirs(neu);
    { const char *e = getenv("HASSMIC_PLUGIN_FAIL_PACKAGE");
      if (put(neu, "plugin", 0755, mem[2].data, mem[2].len) || put(neu, "manifest.sig", 0644, mem[1].data, 64)
          || (mem[4].data && put(neu, "listing.sig", 0644, mem[4].data, mem[4].len))
          || (mem[3].data && put(neu, "LICENSE", 0644, mem[3].data, mem[3].len))
          || (e && !access(e, F_OK)) || put(neu, "manifest.json", 0644, mem[0].data, mem[0].len)
          || statefile_sync(neu)
          || put(dir, "author.pub", 0644, staged.author_key, 32)) { fail(err, errsz, "cannot write on this Echo"); rm_rf(neu); if (fresh) rm_rf(dir); goto out; } }
    rm_rf(old); had = pkg_there(staged.id);
    if ((had && rename(pkg, old)) || rename(neu, pkg)) {
        if (had && !pkg_there(staged.id)) rename(old, pkg);        /* (the one that was there, back) */
        fail(err, errsz, "cannot write on this Echo"); rm_rf(neu); if (fresh) rm_rf(dir); goto out;
    }
    statefile_sync(dir);
    rm_rf(old);
    /* The version is one that was installed here from now, and not before: raised ahead of the renames, an install
     * that failed or ended there left a floor for a version that never was in place (the floor's comment has how
     * the window the other way is closed).  Not written: the install goes on, the package is there, and the sync
     * is closed until the floor is on the flash (floors_owed; the next scan tries again) */
    floor_raise(staged.id, staged.version);
    /* what was accepted, for this package */
    memcpy(a.digest, staged.accepted, 32); snprintf(a.how, sizeof a.how, "%.79s", how); a.when = (long long)time(NULL);
    /* (HASSMIC_PLUGIN_FAIL_ACCEPTED, a file: while it exists this write fails, for the tests: the package is in
     * its folder by now, and web.c has a note to settle against that) */
    { const char *e = getenv("HASSMIC_PLUGIN_FAIL_ACCEPTED");
      if ((e && !access(e, F_OK)) || acc_write(staged.id, &a) || (fresh && put(dir, "enabled", 0644, "1\n", 2))) { fail(err, errsz, "cannot write on this Echo"); goto out; } }
    /* the rows are this version's now: what was stored for a row that is gone, or is of another kind, goes */
    config_tidy(&staged);
    stage_path(p, sizeof p); unlink(p);
    staged_ok = 0; r = 0;
out:
    free(b);
    ps_unlock();
    return r;
}

int ps_remove(const char *id)
{
    char dir[400];
    if (!ps_id_ok(id)) return -1;
    pdir(id, dir, sizeof dir);
    ps_lock();
    /* (what goes was installed here: the floor knows it before the folder is gone, whatever an install's end left
     * unwritten) */
    { long top = folder_top(id); if (top >= 0) floor_raise(id, top); }
    rm_rf(dir); ps_unlock();
    return access(dir, F_OK) ? 0 : -1;
}

int ps_enable(const char *id, int on)
{
    char p[400]; int r;
    if (!ps_id_ok(id)) return -1;
    snprintf(p, sizeof p, "%s/%s/enabled", ps_dir(), id);
    ps_lock(); r = on ? statefile_write(p, 0644, "1\n", 2) : (unlink(p) && errno != ENOENT ? -1 : 0); ps_unlock();
    return r;
}

int ps_develop(const char *id, int on)
{
    char p[400]; int r;
    if (!ps_id_ok(id)) return -1;
    ps_lock();
    snprintf(p, sizeof p, "%s/%s", ps_dir(), id);
    if (access(p, F_OK)) { ps_unlock(); return -1; }
    snprintf(p, sizeof p, "%s/%s/develop", ps_dir(), id);
    r = on ? statefile_write(p, 0644, "1\n", 2) : (unlink(p) && errno != ENOENT ? -1 : 0);
    for (int i = 0; i < nplugins; i++) if (!strcmp(plugins[i].m.id, id)) plugins[i].develop = on && !r;
    ps_unlock();
    return r;
}

int ps_list(const struct ps_plugin **list) { *list = plugins; return nplugins; }

/* ---------------------------------------------------------------- UDP ports of a plugin's own (net.udp, net.discover) */

static int ports_read(const char *id, unsigned short *out, int max)
{
    char path[400], buf[200], *p, *e; int fd, n = 0; ssize_t k;
    snprintf(path, sizeof path, "%s/%s/ports", ps_dir(), id);
    if ((fd = open_reg(path)) < 0) return 0;
    k = read(fd, buf, sizeof buf - 1); close(fd);
    buf[k > 0 ? k : 0] = 0;
    for (p = buf; n < max; p = e) {
        long v = strtol(p, &e, 10);
        if (e == p) break;
        if (v >= PS_PORT_FIRST && v <= PS_PORT_LAST) out[n++] = (unsigned short)v;
    }
    return n;
}

int ps_udp_ports(const char *id, int need, unsigned short *out)
{
    unsigned short have[PS_UDP_MAX + 1]; char taken[PS_PORT_LAST - PS_PORT_FIRST + 1] = { 0 }, path[400], line[64]; size_t len = 0; int n;
    if (need < 1 || need > PS_UDP_MAX + 1) return -1;
    n = ports_read(id, have, need);
    if (n < need) {
        /* every other plugin's, installed now: a removed one's went with its folder */
        for (int i = 0; i < nplugins; i++) {
            unsigned short o[PS_UDP_MAX + 1]; int k;
            if (!strcmp(plugins[i].m.id, id)) continue;
            k = ports_read(plugins[i].m.id, o, PS_UDP_MAX + 1);
            for (int x = 0; x < k; x++) taken[o[x] - PS_PORT_FIRST] = 1;
        }
        for (int x = 0; x < n; x++) taken[have[x] - PS_PORT_FIRST] = 1;
        for (int p = PS_PORT_FIRST; p <= PS_PORT_LAST && n < need; p++) if (!taken[p - PS_PORT_FIRST]) { have[n++] = (unsigned short)p; taken[p - PS_PORT_FIRST] = 1; }
        if (n < need) return -1;
        for (int x = 0; x < n; x++) len += (size_t)snprintf(line + len, sizeof line - len, "%s%u", x ? " " : "", have[x]);
        line[len++] = '\n';
        snprintf(path, sizeof path, "%s/%s/ports", ps_dir(), id);
        if (statefile_write(path, 0600, line, len)) return -1;
    }
    memcpy(out, have, sizeof have[0] * (size_t)need);
    return need;
}

/* ---------------------------------------------------------------- storage (4.4): the plugin's own data
 *
 * state/plugins/<id>/storage/<key>, a file a key, each written through statefile.c (a power cut leaves the old value
 * or the new one, never an empty file).  How often a plugin may write is the broker's to hold (plugin.c: a bucket
 * of ten writes that refills by one a minute: /data is flash); how much is here: the accepted kB, PS_KEYS keys.
 * Remove takes the folder with the plugin; an update keeps it; an update that drops `wake` wipes it (seal_upkeep) */
int ps_store_key_ok(const char *k)
{
    size_t n = strlen(k), i;
    if (!n || n > PS_KEY_MAX || k[0] == '.') return 0;
    for (i = 0; i < n; i++) if (!islower((unsigned char)k[i]) && !isdigit((unsigned char)k[i]) && k[i] != '.' && k[i] != '_' && k[i] != '-') return 0;
    /* (statefile.c writes <name>.tmp<n> beside the file and renames it: a key of that shape would be another's temp) */
    for (i = n; i && isdigit((unsigned char)k[i - 1]); i--) ;
    return !(i >= 4 && !memcmp(k + i - 4, ".tmp", 4));
}

/* the folder of ID's storage, if ID is installed and may keep any (lock held) */
static int store_dir(const char *id, char *out, size_t cap, long *quota)
{
    const struct ps_plugin *pl = ps_find(id);
    if (!pl || !pl->here || pl->unread || !(pl->m.caps & PM_CAPBIT(PM_CAP_STORAGE))) return -1;
    if (quota) *quota = (long)pl->m.storage_kb * 1024;
    return (size_t)snprintf(out, cap, "%s/%s/storage", ps_dir(), id) < cap ? 0 : -1;
}

static long long store_used(const char *dir, int *keys, const char *but)
{
    DIR *d = opendir(dir); struct dirent *e; char p[640]; struct stat st; long long n = 0; int k = 0;
    if (d) {
        while ((e = readdir(d))) {
            if (!ps_store_key_ok(e->d_name) || (but && !strcmp(e->d_name, but))) continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if (lstat(p, &st) || !S_ISREG(st.st_mode)) continue;
            n += st.st_size; k++;
        }
        closedir(d);
    }
    if (keys) *keys = k;
    return n;
}

int ps_store_get(const char *id, const char *key, void *out, size_t cap, size_t *n)
{
    char dir[400], p[460]; int fd, r = -1; struct stat st;
    *n = 0;
    if (!ps_store_key_ok(key)) return PS_STORE_NONE;
    ps_lock();
    if (!store_dir(id, dir, sizeof dir, NULL)) {
        snprintf(p, sizeof p, "%s/%s", dir, key);
        if ((fd = open_reg(p)) < 0) r = PS_STORE_NONE;
        else {
            ssize_t k = 0; size_t got = 0;
            if (!fstat(fd, &st) && st.st_size >= 0) {
                while (got < cap && got < (size_t)st.st_size && (k = read(fd, (char *)out + got, cap - got)) > 0) got += (size_t)k;
                *n = (size_t)st.st_size; r = k < 0 ? -1 : 0;
            }
            close(fd);
        }
    }
    ps_unlock();
    return r;
}

int ps_store_put(const char *id, const char *key, const void *v, size_t n)
{
    char dir[400], p[460]; long quota; int keys, had, r = -1; long long used;
    if (!ps_store_key_ok(key) || n > PS_VALUE_BYTES) return -1;
    ps_lock();
    if (!store_dir(id, dir, sizeof dir, &quota)) {
        snprintf(p, sizeof p, "%s/%s", dir, key);
        had = !access(p, F_OK);
        used = store_used(dir, &keys, key);
        if (used + (long long)n > quota || keys + 1 > PS_KEYS) r = PS_STORE_FULL;
        else { (void)had; mkdir(dir, 0700); r = statefile_write(p, 0600, v, n) ? -1 : 0; }
    }
    ps_unlock();
    return r;
}

int ps_store_del(const char *id, const char *key)
{
    char dir[400], p[460]; int r = -1;
    if (!ps_store_key_ok(key)) return PS_STORE_NONE;
    ps_lock();
    if (!store_dir(id, dir, sizeof dir, NULL)) {
        snprintf(p, sizeof p, "%s/%s", dir, key);
        if (unlink(p)) r = errno == ENOENT ? PS_STORE_NONE : -1;
        else { r = 0; statefile_sync(dir); }
    }
    ps_unlock();
    return r;
}

long long ps_storage_used(const char *id, int *keys)
{
    char dir[400]; long long n = 0;
    if (keys) *keys = 0;
    if (!ps_id_ok(id)) return 0;
    ps_lock();
    snprintf(dir, sizeof dir, "%s/%s/storage", ps_dir(), id);
    n = store_used(dir, keys, NULL);
    ps_unlock();
    return n;
}

/* its keys in name order (at most PS_KEYS), lock held: the count */
static int store_keys(const char *dir, char (*keys)[PS_KEY_MAX + 1])
{
    DIR *d = opendir(dir); struct dirent *e; char p[640]; struct stat st; int n = 0;
    if (!d) return 0;
    while ((e = readdir(d)) && n < PS_KEYS) {
        if (!ps_store_key_ok(e->d_name)) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (lstat(p, &st) || !S_ISREG(st.st_mode)) continue;
        snprintf(keys[n++], PS_KEY_MAX + 1, "%s", e->d_name);
    }
    closedir(d);
    qsort(keys, (size_t)n, PS_KEY_MAX + 1, (int (*)(const void *, const void *))strcmp);
    return n;
}

int ps_storage_digest(const char *id, uint8_t out[32])
{
    char dir[400], p[460], (*keys)[PS_KEY_MAX + 1] = malloc(PS_KEYS * (PS_KEY_MAX + 1)); int n, r = -1; crypto_blake2b_ctx c;
    if (!keys) return -1;
    ps_lock();
    if (!store_dir(id, dir, sizeof dir, NULL)) {
        crypto_blake2b_init(&c, 32);
        n = store_keys(dir, keys);
        r = 0;
        for (int i = 0; i < n; i++) {
            size_t len; uint8_t *v, sz[8];
            snprintf(p, sizeof p, "%s/%s", dir, keys[i]);
            if (!(v = read_file(p, PS_VALUE_BYTES, &len))) { r = -1; break; }
            for (int b = 0; b < 8; b++) sz[b] = (uint8_t)((uint64_t)len >> (8 * b));
            crypto_blake2b_update(&c, (const uint8_t *)keys[i], strlen(keys[i]) + 1); crypto_blake2b_update(&c, sz, 8); crypto_blake2b_update(&c, v, len);
            free(v);
        }
        crypto_blake2b_final(&c, out);
    }
    ps_unlock();
    free(keys);
    return r;
}

/* a plain ustar header for NAME of SIZE bytes */
static void tar_head(uint8_t *h, const char *name, size_t size)
{
    unsigned sum = 0;
    memset(h, 0, 512);
    snprintf((char *)h, 100, "%s", name);
    memcpy(h + 100, "0000600", 8); memcpy(h + 108, "0000000", 8); memcpy(h + 116, "0000000", 8);
    snprintf((char *)h + 124, 12, "%011lo", (unsigned long)size); snprintf((char *)h + 136, 12, "%011lo", 0UL);
    h[156] = '0'; memcpy(h + 257, "ustar", 6); memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum); h[155] = ' ';
}

void *ps_storage_tar(const char *id, size_t *n)
{
    char dir[400], p[460], (*keys)[PS_KEY_MAX + 1] = malloc(PS_KEYS * (PS_KEY_MAX + 1)); uint8_t *o = NULL; size_t on = 0, cap = 0; int k;
    *n = 0;
    if (!keys) return NULL;
    ps_lock();
    if (!store_dir(id, dir, sizeof dir, NULL)) {
        k = store_keys(dir, keys);
        cap = 1024 + (size_t)k * 512 + (size_t)store_used(dir, NULL, NULL) + (size_t)k * 512;
        if ((o = calloc(1, cap))) {
            for (int i = 0; i < k; i++) {
                size_t len; uint8_t *v;
                snprintf(p, sizeof p, "%s/%s", dir, keys[i]);
                if (!(v = read_file(p, PS_VALUE_BYTES, &len))) continue;
                if (on + 512 + (len + 511) / 512 * 512 + 1024 > cap) { free(v); break; }
                tar_head(o + on, keys[i], len); on += 512;
                memcpy(o + on, v, len); on += (len + 511) / 512 * 512;
                free(v);
            }
            on += 1024;                                     /* the two empty blocks of a tar's end */
        }
    }
    ps_unlock();
    free(keys);
    *n = on;
    return o;
}

/* A replacement (ps_storage_replace) that did not end, put right, lock held: without storage/, the new folder if it
 * was written whole (its mark), else the old one back; then what is left of either goes.  ps_scan, and before the
 * next replacement */
#define STORAGE_WHOLE ".whole"
static void storage_settle(const char *id)
{
    char dir[400], neu[420], old[420], p[480];
    if (!ps_id_ok(id) || (size_t)snprintf(dir, sizeof dir, "%s/%s/storage", ps_dir(), id) >= sizeof dir) return;
    snprintf(neu, sizeof neu, "%s.new", dir); snprintf(old, sizeof old, "%s.old", dir); snprintf(p, sizeof p, "%s/%s", neu, STORAGE_WHOLE);
    if (access(neu, F_OK) && access(old, F_OK)) return;
    if (access(dir, F_OK)) {
        if (!access(p, F_OK) && !rename(neu, dir)) fprintf(stderr, "plugins: %s: its data copied from another Echo was put in place after hassmic ended in between\n", id);
        else if (!access(old, F_OK) && !rename(old, dir)) fprintf(stderr, "plugins: %s: its data, being replaced when hassmic ended, is back as it was\n", id);
        snprintf(p, sizeof p, "%s/%s", dir, STORAGE_WHOLE); unlink(p);
        snprintf(p, sizeof p, "%s/%s", ps_dir(), id); statefile_sync(p);
    }
    rm_rf(neu); rm_rf(old);
}

int ps_storage_replace(const char *id, const void *data, size_t len, int check_only, char *err, size_t errsz)
{
    const uint8_t *b = data; char dir[400], neu[420], old[420], p[480]; long quota; size_t at = 0; long long total = 0; int keys = 0, r = -1;
    ps_lock();
    if (store_dir(id, dir, sizeof dir, &quota)) { fail(err, errsz, "it keeps no data on this Echo"); goto out; }
    /* the tar first, whole: a key each, plain files, within the kB this Echo's owner accepted */
    while (at + 512 <= len && b[at]) {
        char name[101], sz[13], *end; unsigned long size;
        memcpy(name, b + at, 100); name[100] = 0; memcpy(sz, b + at + 124, 12); sz[12] = 0;
        size = strtoul(sz, &end, 8);
        if ((*end && *end != ' ') || (b[at + 156] != '0' && b[at + 156] != 0) || !ps_store_key_ok(name) || size > PS_VALUE_BYTES || size > len - at - 512) { fail(err, errsz, "not its data: a key or a value it cannot keep"); goto out; }
        total += (long long)size; keys++;
        at += 512 + (size + 511) / 512 * 512;
    }
    if (total > quota || keys > PS_KEYS) { fail(err, errsz, "more than the %ld kB it may keep here", quota / 1024); goto out; }
    if (check_only) { r = 0; goto out; }
    snprintf(neu, sizeof neu, "%s.new", dir); snprintf(old, sizeof old, "%s.old", dir);
    storage_settle(id);                                     /* (what an earlier one left: put right first) */
    rm_rf(neu); rm_rf(old);
    if (mkdir(neu, 0700)) { fail(err, errsz, "cannot write the plugin's folder"); goto out; }
    for (at = 0; at + 512 <= len && b[at]; ) {
        char name[101], sz[13]; unsigned long size;
        memcpy(name, b + at, 100); name[100] = 0; memcpy(sz, b + at + 124, 12); sz[12] = 0; size = strtoul(sz, NULL, 8);
        snprintf(p, sizeof p, "%s/%s", neu, name);
        if (statefile_write(p, 0600, b + at + 512, size)) { rm_rf(neu); fail(err, errsz, "cannot write the plugin's data"); goto out; }
        at += 512 + (size + 511) / 512 * 512;
    }
    /* whole: marked so (a name no key can have), and synced, before the old folder goes aside */
    snprintf(p, sizeof p, "%s/%s", neu, STORAGE_WHOLE);
    if (statefile_write(p, 0600, "1\n", 2)) { rm_rf(neu); fail(err, errsz, "cannot write the plugin's data"); goto out; }
    /* in place by name: the old folder aside, the new one in, the old one gone.  A power cut between the two renames
     * left no storage/ at all, and the plugin started with nothing (S5 of a review, 2026-11-21): storage_settle puts
     * the whole new one in, or the old one back */
    if (!access(dir, F_OK) && rename(dir, old)) { rm_rf(neu); fail(err, errsz, "cannot replace the plugin's data"); goto out; }
    if (rename(neu, dir)) { rename(old, dir); rm_rf(neu); fail(err, errsz, "cannot replace the plugin's data"); goto out; }
    snprintf(p, sizeof p, "%s/%s", ps_dir(), id); statefile_sync(p);
    snprintf(p, sizeof p, "%s/%s", dir, STORAGE_WHOLE); unlink(p);
    rm_rf(old);
    r = 0;
out:
    ps_unlock();
    return r;
}

/* "Run it anyway" (5.6): state/plugins/<id>/anyway, the serial of the revocation list the owner pressed the button
 * for.  It holds while that is the list this Echo has; a newer list (whatever else it changed) asks him again */
static long long anyway_serial(const char *id)
{
    char p[500]; FILE *f; long long v = -1;
    snprintf(p, sizeof p, "%s/%s/anyway", ps_dir(), id);
    if ((f = ps_fopen(p))) { if (fscanf(f, "%lld", &v) != 1) v = -1; fclose(f); }
    return v;
}

static _Atomic int community;
int ps_community(int on) { if (on >= 0) atomic_store(&community, !!on); return atomic_load(&community); }
/* (a plugin ps_scan refused for another reason first is not the switch's to keep off: its card says that reason; one
 * the scan marked while the switch was off, and the switch still off, is, though its mark's words came first) */
int ps_community_keeps_off(const struct ps_plugin *e)
{
    return e && e->tier != PS_OFFICIAL && !ps_community(-1) && (e->community_off || !e->refused[0]);
}

int ps_anyway(const char *id, long long serial)
{
    char p[500], t[40]; int n, r;
    if (!ps_id_ok(id) || serial < 0) return -1;
    ps_lock();
    snprintf(p, sizeof p, "%s/%s", ps_dir(), id);
    if (access(p, F_OK)) { ps_unlock(); return -1; }
    snprintf(p, sizeof p, "%s/%s/anyway", ps_dir(), id); n = snprintf(t, sizeof t, "%lld\n", serial);
    r = statefile_write(p, 0600, t, (size_t)n);
    ps_unlock();
    return r;
}

/* 3.3, "storage that outlives the seal": an update that drops `wake` is not an update but a new install, so what
 * the plugin was given under the seal, its files and its storage, is wiped, once the package without `wake` is the
 * one that runs here and is accepted (an update that is less needs no button: accepted as it is installed; a package
 * kept for another hassmic's API, when that hassmic starts with it).  Nothing before: the owner who never accepts
 * the update, or falls back, keeps all of it.  state/plugins/<id>/sealed says that the package accepted here had
 * `wake`; written at the scan that first sees it so (also for what a build before this one installed).  Its settings
 * stay: the owner wrote them, the plugin did not.  Every plugin's files are in its own folder (list_dir), so the
 * wipe is of what it was given and of nothing else: microWakeWord's models are its own copies, the built-in engine's
 * folder (state/mww) is not its list.  The mark goes only once both folders are gone: a wipe that left something
 * (a file that would not go) is done again at the next scan, never forgotten.  ps_scan's lock */
int (*ps_busy)(const char *id, const uint8_t manifest[32]);
static void seal_upkeep(const struct ps_plugin *pl)
{
    char mark[400], p[420]; int wake = !!(pl->m.caps & PM_CAPBIT(PM_CAP_WAKE)), left = 0;
    if (!pl->accepted || !pl->here || pl->unread) return;
    snprintf(mark, sizeof mark, "%s/%s/sealed", ps_dir(), pl->m.id);
    if (wake) { if (access(mark, F_OK) && statefile_write(mark, 0644, "wake\n", 5)) fprintf(stderr, "plugins: cannot write %s\n", mark); return; }
    if (access(mark, F_OK)) return;
    /* Not while a process of another package of it runs (the version with `wake`, told to stop, until it is gone):
     * it may write its storage until then, and what it wrote after the wipe stayed, its mark gone (R1 of a review,
     * 2026-11-21).  The mark stays, and the wipe is done at the next look with none running: plugin.c asks for one
     * before the new version starts (ps_seal_settle) */
    if (ps_busy && ps_busy(pl->m.id, pl->m.manifest_digest)) return;
    snprintf(p, sizeof p, "%s/%s/files", ps_dir(), pl->m.id); left |= rm_rf(p);
    snprintf(p, sizeof p, "%s/%s/storage", ps_dir(), pl->m.id); left |= rm_rf(p);
    if (left) fprintf(stderr, "plugins: %s no longer hears the microphone (version %ld has no \"wake\"), and not all of the files and storage it was given while it did could be wiped: tried again at the next look\n", pl->m.id, pl->m.version);
    else if (unlink(mark)) fprintf(stderr, "plugins: cannot remove %s\n", mark);
    else fprintf(stderr, "plugins: %s no longer hears the microphone (version %ld has no \"wake\"): the files and storage it was given while it did are wiped\n", pl->m.id, pl->m.version);
}

static void hakeys_assign(void);
int ps_scan(const struct ps_plugin **list)
{
    DIR *d = opendir(ps_dir()); struct dirent *e; char p[500], err[200]; int n = 0;
    ps_scopes_env();
    while (d && (e = readdir(d)) && n < PS_MAX) {
        struct ps_plugin *pl = &plugins[n]; size_t len = 0, ll = 0; char *text; uint8_t *l; int t; struct acc a; struct stat st;
        if (!ps_id_ok(e->d_name)) continue;
        snprintf(p, sizeof p, "%s/%s", ps_dir(), e->d_name);
        if (lstat(p, &st) || !S_ISDIR(st.st_mode)) continue;
        memset(pl, 0, sizeof *pl);
        storage_settle(e->d_name);      /* (a copy of its data that did not end: the new whole, or the old) */
        if (never_arrived(e->d_name)) continue;
        pkg_settle(e->d_name);          /* (an install that did not end: one whole package, the old or the new) */
        /* (what is installed is a version that was installed here: the floor's comment, the window after an install) */
        { long top = folder_top(e->d_name); if (top >= 0) floor_raise(e->d_name, top); }
        snprintf(p, sizeof p, "%s/%s/enabled", ps_dir(), e->d_name);
        pl->enabled = !access(p, F_OK);
        snprintf(p, sizeof p, "%s/%s/develop", ps_dir(), e->d_name);
        pl->develop = !access(p, F_OK);
        pl->here = pkg_there(e->d_name);
        snprintf(p, sizeof p, "%s/%s/pkg/manifest.json", ps_dir(), e->d_name);
        text = pl->here ? read_file(p, MANIFEST_MAX, &len) : NULL;
        /* What cannot run here is listed all the same, as a plugin that cannot run, under the folder's name (which is
         * an id: ps_id_ok above) and with the reason.  Left out, it had no card and nothing on the page could remove
         * it, while it held one of PS_MAX places and its bytes of the programs' room.  Three kinds: a manifest for a
         * newer hassmic (this one fell back from the build that installed it: it requires a scope this one has not,
         * or a later version of one; its card names it, and it runs again under a hassmic that has it), a manifest
         * this build does not read otherwise (written there), and a folder with no package in it at all */
        err[0] = 0;
        /* (one rule with pkg_manifest: the manifest in pkg/ is this plugin's) */
        if (!text || ps_manifest_parse(text, len, &pl->m, err, sizeof err) || strcmp(pl->m.id, e->d_name)) {
            int newer = text && pl->m.newer && !strcmp(pl->m.id, e->d_name);
            char kname[sizeof pl->m.name], kauthor[sizeof pl->m.author], kvn[sizeof pl->m.version_name]; long kv = pl->m.version; uint16_t kreq[PM_SCOPES];
            memcpy(kname, pl->m.name, sizeof kname); memcpy(kauthor, pl->m.author, sizeof kauthor); memcpy(kvn, pl->m.version_name, sizeof kvn); memcpy(kreq, pl->m.requires, sizeof kreq);
            if (text && !err[0] && strcmp(pl->m.id, e->d_name)) snprintf(err, sizeof err, "its manifest is another plugin's (%.24s)", pl->m.id);
            if (!text && pl->here) snprintf(err, sizeof err, "it is no file that can be read");
            memset(&pl->m, 0, sizeof pl->m); pl->unread = pl->here;
            snprintf(pl->m.id, sizeof pl->m.id, "%s", e->d_name); snprintf(pl->m.name, sizeof pl->m.name, "%s", e->d_name);
            if (newer) {
                /* what it is, for its card: its name, author and version are core 1's, read by every build; and what it
                 * requires (requires_parse read all of it: the card says what this hassmic lacks) */
                memcpy(pl->m.name, kname, sizeof kname); memcpy(pl->m.author, kauthor, sizeof kauthor); memcpy(pl->m.version_name, kvn, sizeof kvn);
                memcpy(pl->m.requires, kreq, sizeof kreq);
                pl->m.version = kv; pl->m.newer = 1;
                snprintf(pl->refused, sizeof pl->refused, "it %.120s (it runs again under a hassmic that has it)", err);
            }
            else if (pl->here) {
                /* its version where its listing names it (the index key signed it in, pkg_version): the card says it,
                 * and the browse page offers an update only of a newer one, as the store takes no other (finding 7 of
                 * a review of 60a8589: the card said 0, and any version was offered, then refused) */
                int known; long v = pkg_version(e->d_name, &known);
                if (known == 2) pl->m.version = v;
                snprintf(pl->refused, sizeof pl->refused, "this hassmic cannot read its manifest (%.110s): remove it, or install a version it reads", err);
            }
            else snprintf(pl->refused, sizeof pl->refused, "nothing of it is left but its folder: remove it");
            utf8_whole(pl->refused);
            fprintf(stderr, "plugins: %s cannot run: %s\n", e->d_name, pl->refused);
            free(text); n++; continue;
        }
        free(text);
        snprintf(p, sizeof p, "%s/%s/pkg/listing.sig", ps_dir(), e->d_name);
        l = read_file(p, 1024, &ll);
        { long long ls = 0; int ct;
          t = listing_by(l, ll, &pl->m, &ls, &pl->listing_key, err, sizeof err);
          pl->tier = t > 0 ? t : PS_SIDELOAD; pl->listing_unread = t < 0; pl->listing_serial = t > 0 ? ls : -1;
          pl->listing_said = t < 0 ? listing_said(l, ll, &pl->m) : -1; free(l);
          /* 3.3's combinations by the tier the listing gives now: one made by an index older than this build takes
           * (5.5's least serial) is no word of the index's for them, as at install (ps_stage_check), so a plugin with
           * log.read.full whose listing is under the least is not started.  The tier shown and held for updates
           * stays the one it came with (5.5: a raised least does not turn what is installed into a file's); the
           * whole log, which only the index's word gives, is held to that word as it stands (N4 of a review of
           * 7be9343: the serial was read and dropped here, and only the install asked) */
          ct = pl->tier != PS_SIDELOAD && ls < pi_least_index() ? PS_SIDELOAD : pl->tier;
          /* listed all the same, so that its card says why it does not run and has Remove */
          pl->refused[0] = 0; pl->withdrawn[0] = 0; pl->anyway = 0;
          if (runnable(&pl->m, ct, pl->refused, sizeof pl->refused)) {
              if (ct != pl->tier && !runnable(&pl->m, pl->tier, err, sizeof err))
                  snprintf(pl->refused, sizeof pl->refused, "its listing is from an index older than this hassmic takes (serial %lld, it takes %lld), and the whole log (\"log.read.full\") is for official plugins only: install it from the plugin index again", ls, pi_least_index());
              fprintf(stderr, "plugins: %s is not started: %s\n", e->d_name, pl->refused);
          }
        }
        /* on the newest revocation list this Echo has (5.6), whatever its tier: stopped and kept, its card says why;
         * removing it is the owner's, and so is running it anyway (with the button, web.c W_ANYWAY), which holds for
         * the list it was pressed for: a newer one that names it again stops it again.  Asked at every scan, so a
         * list that arrives later stops it too, and one that no longer names it lets it run (plugin.c
         * plugin_withdrawn_changed) */
        if (!pl->refused[0]) {
            char why[201]; long long s = pi_revoked_serial();
            if (pi_withdrawn(pl->m.id, pl->m.version, pl->m.author_key, why, sizeof why)) {
                snprintf(pl->withdrawn, sizeof pl->withdrawn, "%s", why); utf8_whole(pl->withdrawn);
                if (s >= 0 && anyway_serial(pl->m.id) == s) pl->anyway = 1;
                else {
                    snprintf(pl->refused, sizeof pl->refused, "the plugin index withdrew it: %.140s", why); utf8_whole(pl->refused);
                    fprintf(stderr, "plugins: %s is not started: %s\n", e->d_name, pl->refused);
                }
            }
        }
        /* not official, with community plugins switched off (2.7): kept, not started, its card says why; switched on,
         * the next scan finds nothing to say (plugin.c plugin_community).  After the rules above: a plugin that cannot
         * run anyway, or the index withdrew, says that first */
        pl->community_off = 0;
        if (!pl->refused[0] && pl->tier != PS_OFFICIAL && !ps_community(-1)) {
            pl->community_off = 1;
            /* (a listing there that does not hold: 5.2's build of the new index key alone, the plugin installed with
             * the old key's listing.  What brings it back is the index's listing of it, which the Echo's own look
             * takes within a minute of the start and hourly after one that failed (plugin_fetch.c relist_tick, by
             * ps_relist_reason 2), or a look from the Plugins page; "its daily one" was the rule before S2 of a
             * review of 3fcd319 (T6 of a review of 9cd7a77).  The text fits `refused` whole: 189 bytes of 200) */
            if (pl->listing_unread)
                snprintf(pl->refused, sizeof pl->refused, "its listing is by a plugin index key this hassmic does not read: as from a file, with community plugins off. The Echo looks at the index by itself within a minute and takes its listing anew");
            else snprintf(pl->refused, sizeof pl->refused, "%s, and " PS_COMMUNITY_OFF, pl->tier == PS_COMMUNITY ? "a community plugin" : "from a file, not listed by the plugin index");
            fprintf(stderr, "plugins: %s is not started: %s\n", e->d_name, pl->refused);
        }
        /* accepted: this package's own line (in whichever form of the digest the build that wrote it had) */
        if (!acc_read(pl->m.id, &a)) { snprintf(pl->how, sizeof pl->how, "%s", a.how); pl->accepted = !memcmp(a.key, pl->m.author_key, 32) && accepted_is(&pl->m, a.digest); }
        seal_upkeep(pl);
        config_tidy(&pl->m);
        n++;
    }
    if (d) closedir(d);
    if (floors_owed) floors_write();    /* (what is owed, however many and where the folder is gone, too: written now if the flash takes it) */
    nplugins = n; *list = plugins;
    hakeys_assign();
    return n;
}

/* ---------------------------------------------------------------- Home Assistant's keys (ha.entities, ha.actions; 4.10)
 *
 * Home Assistant tells a device's entities apart by their key within a listing, and takes an entity that comes back
 * under the same key as the one it had (a rename: the registry entry, its history and the automations on it move).
 * A hash of the plugin's id and the entity's name in 30 bits was a key two plugins could share: a pair was found in
 * three minutes (H2 of a review of efde36e), and then one plugin's state and commands were the other's.  So a key is
 * given, not computed: the next one of a counter that only rises, kept with every key given in state/plugins/.hakeys
 * ("next <hex>", then "<hex> <id> e|a <name>" a line), for as long as the plugin is installed and names that entity
 * or action.  The same at every start and update (Home Assistant keeps the entity by it), never one another has had
 * (the counter, never a key that is free again), never in hassmic's own range (PS_HAKEY_FIRST up).  A key given is
 * only used once the file holds it: one given in memory and lost with a restart could be given again to another */
#define HAKEYS (PS_MAX * (PS_ENTITIES + PS_ACTIONS))
static struct hakey { char id[PS_ID_MAX], name[33], kind; uint32_t key; } hakeys[HAKEYS];
static int nhakeys, hakeys_read, hakeys_blind;
static uint32_t hakey_next = PS_HAKEY_FIRST;
static int hakeys_fail;                 /* (tests: the write fails, HASSMIC_PLUGIN_FAIL_HAKEYS) */

static void hakeys_path(char *out, size_t cap) { snprintf(out, cap, "%s/.hakeys", ps_dir()); }
static int hakey_find(const char *id, char kind, const char *name)
{
    for (int i = 0; i < nhakeys; i++) if (hakeys[i].kind == kind && !strcmp(hakeys[i].id, id) && !strcmp(hakeys[i].name, name)) return i;
    return -1;
}

/* The file read once.  Missing: none given yet here, and the counter starts at a random place within the plugins'
 * first quarter (PS_HAKEY_FIRST + 28 random bits: three quarters stay to count in).  A factory reset or a new
 * install empties state/, while Home Assistant keeps the Echo's device and what it last listed: a counter that
 * started at PS_HAKEY_FIRST again gave the first entity of the next plugin the key of the first entity of the one
 * before, which Home Assistant takes for that entity renamed, its registry entry and history with it (K3 of a review
 * of 92ac2e1).  There and not a file that reads (a FIFO, a folder, an error): no
 * key is given in this run, since what was given before is not known (`blind`; every entity unlisted, said once).
 * A line that does not read is passed over, a key twice or out of range too; the counter goes past every key read, and
 * well past them where its own line is gone (a key on a lost line is then still never given again) */
static void hakeys_load(void)
{
    char path[400], line[160]; FILE *f; uint32_t top = 0; int have_next = 0;
    hakeys_read = 1;
    hakeys_path(path, sizeof path);
    if (getenv("HASSMIC_PLUGIN_FAIL_HAKEYS")) hakeys_fail = 1;
    if (!(f = ps_fopen(path))) {
        struct stat st;
        if (!lstat(path, &st) || errno != ENOENT) { hakeys_blind = 1; fprintf(stderr, "plugins: %s cannot be read: no plugin entity or action goes to Home Assistant in this run\n", path); }
        else {
            uint32_t r = 0; int u = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            if (u < 0 || read(u, &r, sizeof r) != (ssize_t)sizeof r) { r = (uint32_t)time(NULL) ^ (uint32_t)getpid() << 16; }     /* (no urandom: still not PS_HAKEY_FIRST each time) */
            if (u >= 0) close(u);
            hakey_next = PS_HAKEY_FIRST + (r & 0x0fffffffu);
        }
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char id[PS_ID_MAX], name[33], kind[2]; unsigned long k; struct hakey *h;
        if (sscanf(line, "next %lx", &k) == 1) { if (k >= PS_HAKEY_FIRST && k <= PS_HAKEY_LAST + 1ul && (uint32_t)k > hakey_next) hakey_next = (uint32_t)k; have_next = 1; continue; }
        if (sscanf(line, "%lx %24s %1s %32s", &k, id, kind, name) != 4 || k < PS_HAKEY_FIRST || k > PS_HAKEY_LAST || !ps_id_ok(id) || (kind[0] != 'e' && kind[0] != 'a') || !name_ok(name, 32)) continue;
        if ((uint32_t)k > top) top = (uint32_t)k;
        if (hakey_find(id, kind[0], name) >= 0 || nhakeys == HAKEYS) continue;
        { int dup = 0; for (int i = 0; i < nhakeys; i++) dup |= hakeys[i].key == (uint32_t)k; if (dup) continue; }
        h = &hakeys[nhakeys++]; snprintf(h->id, sizeof h->id, "%s", id); snprintf(h->name, sizeof h->name, "%s", name); h->kind = kind[0]; h->key = (uint32_t)k;
    }
    fclose(f);
    if (top >= hakey_next) hakey_next = top + 1;
    if (!have_next && top && hakey_next <= PS_HAKEY_LAST - 0x100000u) hakey_next += 0x100000u;
}

static int hakeys_write(void)
{
    size_t cap = (size_t)HAKEYS * 80 + 32, n = 0; char path[400], *b = malloc(cap); int r;
    if (!b) return -1;
    n += (size_t)snprintf(b + n, cap - n, "next %08x\n", hakey_next);
    for (int i = 0; i < nhakeys && n < cap; i++) n += (size_t)snprintf(b + n, cap - n, "%08x %s %c %s\n", hakeys[i].key, hakeys[i].id, hakeys[i].kind, hakeys[i].name);     /* (69 bytes a line at most) */
    hakeys_path(path, sizeof path);
    r = hakeys_fail ? -1 : statefile_write(path, 0600, b, n);
    free(b);
    return r;
}

/* ps_scan's last step, lock held: every plugin listed gets its keys (the ones it had, or new ones), and the keys of a
 * plugin no longer installed, or of an entity or action its manifest no longer names, are dropped (never given
 * again: the counter).  A plugin whose manifest cannot be read keeps what it had */
static void hakeys_assign(void)
{
    int added[HAKEYS], nadded = 0, dirty = 0;
    if (!hakeys_read) hakeys_load();
    for (int i = 0; i < nhakeys; ) {
        const struct ps_plugin *pl = NULL; int named = 0;
        for (int k = 0; k < nplugins && !pl; k++) if (!strcmp(plugins[k].m.id, hakeys[i].id)) pl = &plugins[k];
        if (pl && pl->unread) named = 1;
        else if (pl && hakeys[i].kind == 'e' && (pl->m.caps & PM_CAPBIT(PM_CAP_HA_ENTITIES))) for (int k = 0; k < pl->m.nentities; k++) named |= !strcmp(pl->m.entities[k].name, hakeys[i].name);
        else if (pl && hakeys[i].kind == 'a' && (pl->m.caps & PM_CAPBIT(PM_CAP_HA_ACTIONS))) for (int k = 0; k < pl->m.nactions; k++) named |= !strcmp(pl->m.actions[k].name, hakeys[i].name);
        if (named) { i++; continue; }
        hakeys[i] = hakeys[--nhakeys]; dirty = 1;
    }
    for (int i = 0; i < nplugins; i++) {
        struct ps_plugin *pl = &plugins[i];
        for (int pass = 0; pass < 2; pass++) {
            int cnt = pass ? pl->m.nactions : pl->m.nentities; char kind = pass ? 'a' : 'e';
            if (pl->unread || !(pl->m.caps & PM_CAPBIT(pass ? PM_CAP_HA_ACTIONS : PM_CAP_HA_ENTITIES))) continue;
            for (int k = 0; k < cnt; k++) {
                const char *name = pass ? pl->m.actions[k].name : pl->m.entities[k].name; int h = hakey_find(pl->m.id, kind, name);
                if (h < 0 && !hakeys_blind && nhakeys < HAKEYS && hakey_next <= PS_HAKEY_LAST) {
                    struct hakey *x = &hakeys[h = nhakeys++];
                    snprintf(x->id, sizeof x->id, "%s", pl->m.id); snprintf(x->name, sizeof x->name, "%s", name); x->kind = kind; x->key = hakey_next++;
                    added[nadded++] = h; dirty = 1;
                }
                *(pass ? &pl->act_key[k] : &pl->ent_key[k]) = h >= 0 ? hakeys[h].key : 0;
            }
        }
    }
    if (dirty && hakeys_write()) {
        /* not on the flash: the new ones are not used (a restart would give them again, to another perhaps), and
         * the counter stays past them.  Asked again at the next scan */
        fprintf(stderr, "plugins: %s/.hakeys cannot be written: %d entit%s or action%s not listed in Home Assistant until it can\n", ps_dir(), nadded, nadded == 1 ? "y" : "ies", nadded == 1 ? "" : "s");
        for (int a = nadded - 1; a >= 0; a--) {
            uint32_t key = hakeys[added[a]].key;
            for (int i = 0; i < nplugins; i++) {
                for (int k = 0; k < PS_ENTITIES; k++) if (plugins[i].ent_key[k] == key) plugins[i].ent_key[k] = 0;
                for (int k = 0; k < PS_ACTIONS; k++) if (plugins[i].act_key[k] == key) plugins[i].act_key[k] = 0;
            }
            hakeys[added[a]] = hakeys[--nhakeys];
        }
    }
}
/* for the tests: the table forgotten, read again at the next scan */
void ps_hakeys_forget(void) { nhakeys = 0; hakeys_read = 0; hakeys_blind = 0; hakeys_fail = 0; hakey_next = PS_HAKEY_FIRST; }

const struct ps_plugin *ps_find(const char *id) { for (int i = 0; i < nplugins; i++) if (!strcmp(plugins[i].m.id, id)) return &plugins[i]; return NULL; }

void ps_seal_settle(const char *id)
{
    const struct ps_plugin *pl;
    ps_lock();
    if ((pl = ps_find(id))) seal_upkeep(pl);
    ps_unlock();
}

int ps_sealed(const char *id)
{
    const struct ps_plugin *pl; char mark[400]; int r;
    ps_lock();
    /* (a plugin whose manifest this build does not read, one for a newer hassmic among them, or a folder with no
     * package: what it asks for is not known, so it counts as a wake plugin's.  It was not: its parsed manifest is
     * zeroed for its card, and a wake plugin there exported its storage as wake=0 to an Echo without wake: finding
     * 7 of a review of 4e0a202) */
    r = (pl = ps_find(id)) && (pl->m.caps & PM_CAPBIT(PM_CAP_WAKE) || pl->unread || pl->m.newer || !pl->here);
    if (!r && ps_id_ok(id)) { snprintf(mark, sizeof mark, "%s/%s/sealed", ps_dir(), id); r = !access(mark, F_OK); }
    ps_unlock();
    return r;
}

int ps_program(const char *id, char *path, size_t cap)
{
    return ps_id_ok(id) && (size_t)snprintf(path, cap, "%s/%s/pkg/plugin", ps_dir(), id) < cap ? 0 : -1;
}

/* ---------------------------------------------------------------- settings */

/* state/plugins/<id>/config: a line a value, `<row>:<type>=<value>`, the type as PS_BOOL..PS_SECRET's number.
 * A value belongs to a row of the type it was stored under, and to no other: the file is read by row name, rows come
 * and go with updates (a row gone needs no button), and a name can come back as another kind of row.  Without the
 * type, version 1's secret `tok` was, after a version without that row and a third with a text row `tok` labelled
 * "Nickname", that row's value: shown on the page, which is plain HTTP; and a number or a choice inherited a text
 * its limits were never held against (a review, 2026-11-19).  So:
 *   - read (ps_config_get): only a line of the row's own type whose value the row as it is now would take;
 *   - an install and every scan (config_tidy): lines whose row is gone, is of another type, or would not take the
 *     value, go (a scan of a manifest this build does not read, one for a newer hassmic, does not tidy: its rows
 *     are not known here, and a fallback must not take the owner's values from the build it falls back from).
 * `<row>=<value>`, without a type, is what builds before this wrote: taken over at the first scan as the type of the
 * row of that name then, if that row takes it, and dropped otherwise.  That is one guess, made once, of installs a
 * day old; after it no value is ever a guess.  Never logged: values are secrets among others */
static const struct ps_setting *setting_of(const struct ps_plugin *p, const char *name)
{
    for (int i = 0; i < p->m.nsettings; i++) if (!strcmp(p->m.settings[i].name, name)) return &p->m.settings[i];
    return NULL;
}

/* A value as row S takes it, made the way it is stored (V, of PS_VALUE_MAX bytes: "on" is 1, "007" is 7): 0, or -1
 * with why.  The one rule for what an owner types, what is found stored, and (host_name_ok) a host row's default */
static int value_ok(const struct ps_setting *s, char *v, char *err, size_t errsz)
{
    if (strlen(v) >= PS_VALUE_MAX || !text_ok(v) || !utf8_ok(v, strlen(v))) { fail(err, errsz, "%s: at most %d bytes of text", s->label, PS_VALUE_MAX - 1); return -1; }
    if (s->type == PS_BOOL) {
        int on = !strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "true"), off = !strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false");
        if (!on && !off) { fail(err, errsz, "%s: on or off", s->label); return -1; }
        snprintf(v, PS_VALUE_MAX, "%d", on);
    } else if (s->type == PS_INT) {
        char *end; long n = strtol(v, &end, 10);
        if (!v[0] || *end || n < s->min || n > s->max) { fail(err, errsz, "%s: %ld to %ld", s->label, s->min, s->max); return -1; }
        snprintf(v, PS_VALUE_MAX, "%ld", n);
    } else if (s->type == PS_SELECT) {
        int i; for (i = 0; i < s->noptions && strcmp(v, s->options[i]); i++) ;
        if (i == s->noptions) { fail(err, errsz, "%s: not one of its choices", s->label); return -1; }
    } else if (s->type == PS_HOST) {
        /* as a default has to be: "", "-" and "..." passed the check of letters, digits, dots and hyphens that stood
         * here.  (No value at all is the row unset: ps_config_set.)  A name is 253 bytes at most */
        if (strlen(v) > 253 || !host_name_ok(v)) { fail(err, errsz, "%s: a host name or an address", s->label); return -1; }
    } else if (s->type == PS_FILES) return -1;
    return 0;
}

/* One line of the file, taken apart in place: its value, the row's name into NAME (33 bytes) and the type it was
 * stored under into *type (-1: a line of a build before the type).  NULL: no such line */
static char *config_line(char *line, char *name, int *type)
{
    char *eq, *colon; size_t nl;
    line[strcspn(line, "\n")] = 0;
    if (!(eq = strchr(line, '='))) return NULL;
    *eq = 0; *type = -1;
    if ((colon = strchr(line, ':'))) {
        if (colon[1] < '0' || colon[1] >= '0' + PS_FILES || colon[2]) return NULL;
        *type = colon[1] - '0'; *colon = 0;
    }
    if ((nl = strlen(line)) < 1 || nl > 32 || !name_ok(line, 32)) return NULL;
    memcpy(name, line, nl + 1);
    return eq + 1;
}

static int config_read(const struct ps_plugin *p, const char *name, char *out, size_t cap, int defaults);
int ps_config_get(const struct ps_plugin *p, const char *name, char *out, size_t cap) { return config_read(p, name, out, cap, 1); }
int ps_config_owner(const struct ps_plugin *p, const char *name, char *out, size_t cap) { return config_read(p, name, out, cap, 0); }
static int config_read(const struct ps_plugin *p, const char *name, char *out, size_t cap, int defaults)
{
    const struct ps_setting *s = setting_of(p, name); char path[400], line[PS_VALUE_MAX + 40], row[33], v[PS_VALUE_MAX], *val; FILE *f; int hit = 0, type;
    out[0] = 0;
    if (!s || s->type == PS_FILES) return 0;
    snprintf(path, sizeof path, "%s/%s/config", ps_dir(), p->m.id);
    if ((f = ps_fopen(path))) {
        while (!hit && fgets(line, sizeof line, f)) {
            if (!(val = config_line(line, row, &type)) || strcmp(row, name)) continue;
            /* another type's, or one this row would not take: not this row's value (config_tidy takes it away) */
            if (type != s->type || strlen(val) >= sizeof v) continue;
            memcpy(v, val, strlen(val) + 1);
            if (!value_ok(s, v, NULL, 0)) { snprintf(out, cap, "%s", v); hit = 1; }
        }
        fclose(f);
    }
    if (!hit && defaults && s->def[0]) { snprintf(out, cap, "%s", s->def); hit = 1; }
    return hit;
}

int ps_net_row(const struct ps_plugin *p, const char *name)
{
    for (int i = 0; i < p->m.nnet; i++) if (!strcmp(p->m.net[i].setting, name) || !strcmp(p->m.net[i].port_row, name)) return 1;
    return 0;
}

int ps_setting_secret(const struct ps_plugin *p, const char *name)
{
    const struct ps_setting *s = setting_of(p, name);
    return s && s->type == PS_SECRET;
}

int ps_config_set(const struct ps_plugin *p, const char *name, const char *value, char *err, size_t errsz)
{
    const struct ps_setting *s = setting_of(p, name); char path[400], v[PS_VALUE_MAX], line[PS_VALUE_MAX + 40], copy[PS_VALUE_MAX + 40], row[33]; struct statefile sf; FILE *f; int r, type, unset;
    if (!s || s->type == PS_FILES) { fail(err, errsz, "%s has no setting %s", p->m.name, name); return -1; }
    if (strlen(value) >= sizeof v) { fail(err, errsz, "%s: at most %zu bytes of text", s->label, sizeof v - 1); return -1; }
    snprintf(v, sizeof v, "%s", value);
    /* nothing typed: a secret and a server's name are unset by it (the row's default again, or none: there is no
     * server called ""); a text row keeps the empty text, which an owner may want in place of a default */
    unset = !v[0] && (s->type == PS_SECRET || s->type == PS_HOST);
    if (!unset && value_ok(s, v, err, errsz)) return -1;
    snprintf(path, sizeof path, "%s/%s/config", ps_dir(), p->m.id);
    ps_lock();
    if (statefile_open(&sf, path, 0600)) { ps_unlock(); fail(err, errsz, "cannot write on this Echo"); return -1; }
    if ((f = ps_fopen(path))) {
        while (fgets(line, sizeof line, f)) {               /* every line of this row goes, of whatever type */
            memcpy(copy, line, sizeof copy);
            if (config_line(copy, row, &type) && !strcmp(row, name)) continue;
            fputs(line, sf.f);
        }
        fclose(f);
    }
    if (!unset) fprintf(sf.f, "%s:%d=%s\n", name, s->type, v);
    r = statefile_commit(&sf);
    ps_unlock();
    if (r) fail(err, errsz, "cannot write on this Echo");
    return r;
}

/* ps_lock held.  The stored values held against the rows of M (the comment above config's format says which go).
 * Written only when something changes */
static void config_tidy(const struct ps_manifest *m)
{
    char path[400], line[PS_VALUE_MAX + 40], row[33], v[PS_VALUE_MAX], seen[PS_SETTINGS][33], *val, *out = NULL; FILE *f; size_t on = 0, cap = 0; int type, dropped = 0, changed = 0, nseen = 0;
    snprintf(path, sizeof path, "%s/%s/config", ps_dir(), m->id);
    if (!(f = ps_fopen(path))) return;
    while (fgets(line, sizeof line, f)) {
        const struct ps_setting *s = NULL; int keep = 0, twice = 0; size_t ll;
        if (!strchr(line, '\n') && !feof(f)) { int c; while ((c = fgetc(f)) != EOF && c != '\n') ; dropped++; continue; }      /* longer than any value: none */
        if ((val = config_line(line, row, &type)) && strlen(val) < sizeof v) {
            memcpy(v, val, strlen(val) + 1);
            for (int i = 0; i < m->nsettings; i++) if (!strcmp(m->settings[i].name, row) && m->settings[i].type != PS_FILES) s = &m->settings[i];
            for (int i = 0; i < nseen; i++) twice |= !strcmp(seen[i], row);
            if (s) keep = !twice && (type < 0 || type == s->type) && !value_ok(s, v, NULL, 0);
            else keep = 0;                                  /* no row of this package's */
            if (keep && s && nseen < PS_SETTINGS) snprintf(seen[nseen++], sizeof seen[0], "%s", row);
        }
        if (!keep) { dropped++; continue; }
        ll = strlen(row) + strlen(v) + 8;
        if (on + ll > cap) { char *t = realloc(out, cap = (on + ll) * 2); if (!t) { free(out); fclose(f); return; } out = t; }
        if (type < 0 || strcmp(v, val)) changed = 1;        /* taken over as its row's type; or written the way it is stored */
        on += (size_t)snprintf(out + on, cap - on, "%s:%d=%s\n", row, s ? s->type : type, v);
    }
    fclose(f);
    if (dropped || changed) {
        if (dropped) fprintf(stderr, "plugins: %s: %d stored setting%s dropped (the row is gone, is of another kind now, or would not take it)\n", m->id, dropped, dropped > 1 ? "s" : "");
        statefile_write(path, 0600, out ? out : "", on);
    }
    free(out);
}

/* ---------------------------------------------------------------- files */

/* Where a list's items are: in the plugin's own folder, every plugin's, by its id alone.  microWakeWord's models
 * were the built-in engine's folder (state/mww) until 2026-11-21: one store for two owners, so a model deleted on
 * the plugin's card was gone for the built-in engine too, a copy or a new version on the built-in engine's page
 * dropped what the plugin kept beside it, both staged into the same .new-<id>, and any plugin that called itself
 * microwakeword got that folder (a review).  The move (web.c mww_migrate) copies the models into the plugin's own
 * list; an Echo whose plugin read state/mww under a build before gets its own copies at the next start
 * (ps_mww_own).  state/mww is the built-in engine's alone */
static int list_dir(const struct ps_plugin *p, const char *list, char *out, size_t cap)
{
    const struct ps_setting *s = setting_of(p, list);
    if (!s || s->type != PS_FILES) return -1;
    snprintf(out, cap, "%s/%s/files/%s", ps_dir(), p->m.id, list);
    return 0;
}

/* An item's folder (staged_: the upload still aside).  -1 when LIST is no file list of the plugin or ITEM no item
 * id: out is "" then, and no caller goes on with it */
static int item_dir(const struct ps_plugin *p, const char *list, const char *item, int staged_, char *out, size_t cap)
{
    char base[400];
    if (cap) out[0] = 0;
    if (!ps_item_id_ok(item) || list_dir(p, list, base, sizeof base)) return -1;
    return (size_t)snprintf(out, cap, "%s/%s%s", base, staged_ ? ".new-" : "", item) < cap ? 0 : -1;
}

static void item_read(const struct ps_setting *s, const char *dir, const char *id, struct ps_item *it)
{
    char p[640], line[320]; FILE *f; struct stat st;
    memset(it, 0, sizeof *it); snprintf(it->id, sizeof it->id, "%.40s", id);
    for (int i = 0; i < s->nfiles; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, s->files[i].as);
        if (!lstat(p, &st) && S_ISREG(st.st_mode)) { snprintf(it->files[it->nfiles].suffix, sizeof it->files[0].suffix, "%s", s->files[i].suffix); it->files[it->nfiles++].size = (long)st.st_size; }
    }
    snprintf(p, sizeof p, "%s/title", dir);
    if ((f = ps_fopen(p))) { if (fgets(it->title, sizeof it->title, f)) it->title[strcspn(it->title, "\n")] = 0; fclose(f); }
    snprintf(p, sizeof p, "%s/checked", dir); it->checked = !access(p, F_OK);
    snprintf(p, sizeof p, "%s/fields", dir);
    if ((f = ps_fopen(p))) {
        while (it->nfields < PS_FIELDS && fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '='); line[strcspn(line, "\n")] = 0;
            if (!eq) continue;
            *eq = 0;
            for (int i = 0; i < s->nfields; i++) if (!strcmp(s->fields[i].name, line)) {
                snprintf(it->fields[it->nfields].name, sizeof it->fields[0].name, "%.32s", line);
                snprintf(it->fields[it->nfields++].value, sizeof it->fields[0].value, "%.256s", eq + 1);
            }
        }
        fclose(f);
    }
}

int ps_items(const struct ps_plugin *p, const char *list, struct ps_item *out, int max)
{
    const struct ps_setting *s = setting_of(p, list); char base[400], dir[700]; DIR *d; struct dirent *e; int n = 0;
    if (list_dir(p, list, base, sizeof base) || !(d = opendir(base))) return 0;
    while ((e = readdir(d)) && n < max) {
        if (!ps_item_id_ok(e->d_name)) continue;                /* .new-<item>: an upload on its way */
        snprintf(dir, sizeof dir, "%s/%s", base, e->d_name);
        item_read(s, dir, e->d_name, &out[n]);
        if (out[n].nfiles) n++;
    }
    closedir(d);
    /* by id: readdir's order is the file system's, and the page's rows and Home Assistant's select must not move */
    for (int i = 1; i < n; i++) for (int k = i; k > 0 && strcmp(out[k - 1].id, out[k].id) > 0; k--) { struct ps_item t = out[k]; out[k] = out[k - 1]; out[k - 1] = t; }
    return n;
}

long long ps_files_used(const struct ps_plugin *p)
{
    char base[400]; long long n = 0;
    for (int i = 0; i < p->m.nsettings; i++) if (p->m.settings[i].type == PS_FILES && !list_dir(p, p->m.settings[i].name, base, sizeof base)) n += dir_bytes(base);
    return n;
}

int ps_item_begin(const struct ps_plugin *p, const char *list, const char *item, char *err, size_t errsz)
{
    const struct ps_setting *s = setting_of(p, list); char dir[500], fin[500]; static struct ps_item have[PS_ITEMS]; int n;
    if (!s || s->type != PS_FILES || !ps_item_id_ok(item)) { fail(err, errsz, "no such list, or not a name for an item (lower case letters, digits, - and _, 40 at most)"); return -1; }
    ps_lock();
    if (item_dir(p, list, item, 0, fin, sizeof fin) || item_dir(p, list, item, 1, dir, sizeof dir)) { ps_unlock(); fail(err, errsz, "no such list"); return -1; }
    n = ps_items(p, list, have, PS_ITEMS);
    if (access(fin, F_OK) && n >= s->max_items) { ps_unlock(); fail(err, errsz, "%s holds %d at most", s->label, s->max_items); return -1; }
    rm_rf(dir); mkdirs(dir);
    ps_unlock();
    if (access(dir, W_OK)) { fail(err, errsz, "cannot write on this Echo"); return -1; }
    return 0;
}

static const struct ps_item_file *file_of(const struct ps_setting *s, const char *suffix)
{
    for (int i = 0; s && i < s->nfiles; i++) if (!strcmp(s->files[i].suffix, suffix)) return &s->files[i];
    return NULL;
}

int ps_item_chunk(const struct ps_plugin *p, const char *list, const char *item, const char *suffix, long off, const void *data, size_t n, char *err, size_t errsz)
{
    const struct ps_setting *s = setting_of(p, list); const struct ps_item_file *f = file_of(s, suffix); char dir[500], path[640]; struct stat st; int fd, r = -1;
    if (!f || !ps_item_id_ok(item)) { fail(err, errsz, "%s takes no %s file", s ? s->label : "this plugin", suffix); return -1; }
    if (off < 0 || off + (long)n > (long)f->max_kb * 1024) { fail(err, errsz, "a %s file is at most %d kB", suffix, f->max_kb); return -1; }
    if (ps_files_used(p) + (long long)n > (long long)p->m.files_mb << 20) { fail(err, errsz, "%s may keep %d MB of files", p->m.name, p->m.files_mb); return -1; }
    if (room((long long)n, err, errsz)) return -1;
    if (item_dir(p, list, item, 1, dir, sizeof dir)) { fail(err, errsz, "no such list"); return -1; }
    snprintf(path, sizeof path, "%s/%s", dir, f->as);
    ps_lock();
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || fstat(fd, &st)) fail(err, errsz, "no upload of %s was started", item);
    else if (st.st_size != off) fail(err, errsz, "the upload is at %ld, not %ld", (long)st.st_size, off);
    else if (write(fd, data, n) != (ssize_t)n) fail(err, errsz, "cannot write on this Echo");
    else r = 0;
    if (fd >= 0) close(fd);
    ps_unlock();
    return r;
}

void ps_item_drop(const struct ps_plugin *p, const char *list, const char *item)
{
    char dir[500];
    if (item_dir(p, list, item, 1, dir, sizeof dir)) return;
    ps_lock(); rm_rf(dir); ps_unlock();
}

int ps_item_keep(const struct ps_plugin *p, const char *list, const char *item, char *err, size_t errsz)
{
    const struct ps_setting *s = setting_of(p, list); char dir[500], fin[500], old[520], path[640]; struct stat st; int r = -1;
    if (!s || s->type != PS_FILES || !ps_item_id_ok(item)) { fail(err, errsz, "no such list or item"); return -1; }
    if (item_dir(p, list, item, 1, dir, sizeof dir) || item_dir(p, list, item, 0, fin, sizeof fin)) { fail(err, errsz, "no such list or item"); return -1; }
    ps_lock();
    for (int i = 0; i < s->nfiles; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, s->files[i].as);
        if ((lstat(path, &st) || !S_ISREG(st.st_mode) || !st.st_size) && !s->files[i].optional) { fail(err, errsz, "%s needs a %s file", item, s->files[i].suffix); goto out; }
        if (!lstat(path, &st)) statefile_sync(path);
    }
    /* the fields the owner set stay with an item that is replaced */
    snprintf(old, sizeof old, "%s/fields", fin); snprintf(path, sizeof path, "%s/fields", dir);
    if (!access(old, F_OK) && access(path, F_OK)) { size_t l = 0; char *t = read_file(old, 8192, &l); if (t) { statefile_write(path, 0600, t, l); free(t); } }
    snprintf(old, sizeof old, "%s.old", dir);
    rm_rf(old);
    if (!access(fin, F_OK) && rename(fin, old)) { fail(err, errsz, "cannot write on this Echo"); goto out; }
    if (rename(dir, fin)) { rename(old, fin); fail(err, errsz, "cannot write on this Echo"); goto out; }
    rm_rf(old);
    { char base[400]; if (!list_dir(p, list, base, sizeof base)) statefile_sync(base); }
    r = 0;
out:
    ps_unlock();
    return r;
}

int ps_item_delete(const struct ps_plugin *p, const char *list, const char *item)
{
    char dir[500];
    if (item_dir(p, list, item, 0, dir, sizeof dir)) return -1;
    ps_lock(); rm_rf(dir); ps_unlock();
    return access(dir, F_OK) ? 0 : -1;
}

/* ps_mww_own (plugin_store.h).  Under the builds before, microWakeWord's list was state/mww itself: the models the
 * plugin checked there carry its `checked` (and its `title`, `fields`), which the built-in engine never writes.  The
 * official plugin, with `wake` (the alias was only ever a wake word plugin's), gets those as its own once: the files
 * (model.tflite, manifest.json) and the threshold and window the owner tuned (`fields`, each through the store's own
 * field check), never `checked` or `title`: those are the plugin's to write, and it writes them when it looks at the
 * model (plugin.c look_unchecked, its FILE_CHECK, as for an item kept while it was down).  A model it has in its own
 * list already is left as it is.  Once: a mark (state/plugins/.mww-own) is written when a pass for that plugin ends
 * with nothing to try again, and from then on nothing is imported, so that a `checked` file that reaches state/mww
 * later (a fallback to a build with the alias, whose plugin checks the models there again) never replaces or adds to
 * what the plugin has; the marks go from state/mww then, at every start, so that the folder is the built-in
 * engine's alone (artifacts.c copies only its two files besides).  Not the official plugin with `wake` there and
 * read (a sideload or another author's under that id: an id is no proof of whose it is; none with `wake`; none
 * installed; its manifest unread at this start): nothing is given, nothing
 * marked, nothing taken out of state/mww, and a later start looks again.  Until a review of 913ec8d the pass was
 * marked done and the marks stripped whatever it found: a start that read the plugin's manifest by chance not at all
 * lost the tuned fields for good, and an Echo without state/plugins logged "cannot write" at every start (a review,
 * 2026-11-21, before that: the pass ran at every start, took any folder with a `checked` file whoever wrote it, an
 * artifact copy from another Echo included, replaced the plugin's own copy, and copied a title nobody had checked;
 * and gave a plugin without `wake` what one with it had checked).  A model whose copy failed keeps its marks and the
 * pass is not marked done: tried again at the next start.  After the first scan (ps_find), ps_lock held inside */
int ps_mww_own(void)
{
    static const char *const marks[] = { "checked", "title", "fields" };
    static const char *const files[][2] = { { "model.tflite", ".tflite" }, { "manifest.json", ".json" } };
    char base[400], own[400], done[400], p[700], err[200], line[320]; DIR *d; struct dirent *e; struct stat st; int n = 0, failed = 0, take, was;
    const struct ps_plugin *sp;
    snprintf(base, sizeof base, "%s/mww", state_dir());
    if (!(d = opendir(base))) return 0;
    snprintf(done, sizeof done, "%s/.mww-own", ps_dir());
    ps_lock();
    was = !access(done, F_OK);
    sp = ps_find("microwakeword");
    take = !was && sp && sp->here && !sp->unread && sp->tier == PS_OFFICIAL && (sp->m.caps & PM_CAPBIT(PM_CAP_WAKE)) && !list_dir(sp, "models", own, sizeof own);
    if (!was && !take) { ps_unlock(); closedir(d); return 0; }          /* (left as it is for a start that can) */
    while ((e = readdir(d))) {
        int bad = 0;
        if (!ps_item_id_ok(e->d_name)) continue;
        snprintf(p, sizeof p, "%s/%s/checked", base, e->d_name);
        if (lstat(p, &st) || !S_ISREG(st.st_mode)) continue;            /* never the plugin's: the built-in engine's alone */
        snprintf(p, sizeof p, "%s/%s", own, e->d_name);
        if (take && lstat(p, &st)) {
            err[0] = 0;
            if (ps_item_begin(sp, "models", e->d_name, err, sizeof err)) bad = -1;    /* (its list full: not tried again) */
            for (size_t i = 0; !bad && i < sizeof files / sizeof files[0]; i++) {
                size_t len = 0; void *b;
                snprintf(p, sizeof p, "%s/%s/%s", base, e->d_name, files[i][0]);
                if (lstat(p, &st) && i) continue;                         /* (no manifest: the plugin's defaults) */
                if (lstat(p, &st)) { snprintf(err, sizeof err, "it has no model.tflite"); bad = -1; break; }
                if (!(b = read_file(p, 2u << 20, &len)) || ps_item_chunk(sp, "models", e->d_name, files[i][1], 0, b, len, err, sizeof err)) bad = 1;
                free(b);
            }
            snprintf(p, sizeof p, "%s/%s/fields", base, e->d_name);
            { FILE *f = bad ? NULL : ps_fopen(p);
              while (f && fgets(line, sizeof line, f)) {
                  char *eq = strchr(line, '='), fe[200];
                  line[strcspn(line, "\r\n")] = 0;
                  if (!eq) continue;
                  *eq = 0;
                  if (ps_item_field_of(sp, "models", line) && ps_item_field_staged(sp, "models", e->d_name, line, eq + 1, fe, sizeof fe))
                      log_private("plugins: microwakeword: model %s: its %s from state/mww left out: %s", e->d_name, line, fe);
              }
              if (f) fclose(f); }
            if (!bad && ps_item_keep(sp, "models", e->d_name, err, sizeof err)) bad = 1;
            if (bad) {
                ps_item_drop(sp, "models", e->d_name);
                log_private("plugins: microwakeword: its model %s could not be copied out of state/mww into its own list (%s)%s", e->d_name, err[0] ? err : "cannot read it",
                        bad > 0 ? ": tried again at the next start" : "");
                if (bad > 0) { failed = 1; continue; }
            } else n++;
        }
        for (size_t i = 0; i < sizeof marks / sizeof marks[0]; i++) { snprintf(p, sizeof p, "%s/%s/%s", base, e->d_name, marks[i]); unlink(p); }
    }
    closedir(d);
    if (n) fprintf(stderr, "plugins: microwakeword: %d model%s it listed from the built-in engine's folder (state/mww, a build before this one) copied into its own list, for it to check; state/mww is the built-in engine's alone\n", n, n == 1 ? "" : "s");
    if (!was && !failed && statefile_write(done, 0644, "1\n", 2)) fprintf(stderr, "plugins: cannot write %s\n", done);
    ps_unlock();
    return n;
}

/* what a plugin names an item goes to Home Assistant (the wake word selects, the run's phrase): letters, digits and
 * spaces, 40 at most, taken once when the owner uploads it and sees it (3.3: no free text at run time) */
int ps_item_checked(const struct ps_plugin *p, const char *list, const char *item, const char *title)
{
    char dir[500], path[640], t[200]; size_t n = 0; int r, chars = 0;
    if (item_dir(p, list, item, 0, dir, sizeof dir)) return -1;
    /* letters, digits and spaces, 40 characters: ASCII by what it is, anything else only as a whole, well-formed
     * UTF-8 character.  Home Assistant's link is dropped on a string that is not UTF-8, over and over, and a title
     * cut inside a character is one */
    for (const unsigned char *c = (const unsigned char *)title; *c && chars < 40; ) {
        int len = (int)ps_utf8_char((const char *)c, strlen((const char *)c));
        if (!len) { c++; continue; }                        /* not a character: left out */
        if (n + (size_t)len > 63) break;                    /* what holds it further on is 64 bytes: never cut inside one */
        if (len > 1 || isalnum(*c) || *c == ' ') { memcpy(t + n, c, (size_t)len); n += (size_t)len; chars++; }
        c += len;
    }
    t[n] = 0;
    if (!t[0]) snprintf(t, sizeof t, "%.40s", item);
    ps_lock();
    snprintf(path, sizeof path, "%s/title", dir);
    r = statefile_write(path, 0644, t, strlen(t));
    snprintf(path, sizeof path, "%s/checked", dir);
    r |= statefile_write(path, 0644, "1\n", 2);
    ps_unlock();
    return r;
}

static int item_field(const struct ps_plugin *p, const char *list, const char *item, const char *name, const char *value, int only_if_unset, int staged_, char *err, size_t errsz)
{
    const struct ps_setting *s = setting_of(p, list); const struct ps_field *fd = NULL; char dir[500], path[640], line[320], v[257]; struct statefile sf; FILE *f; size_t nl = strlen(name); int r, had = 0;
    for (int i = 0; s && i < s->nfields; i++) if (!strcmp(s->fields[i].name, name)) fd = &s->fields[i];
    if (!fd || !ps_item_id_ok(item)) { fail(err, errsz, "no such field"); return -1; }
    /* (a field's value comes from the plugin's FILE_OK as well as from the owner, and the page shows it: strict UTF-8,
     * as every plugin text that is shown) */
    if (strlen(value) >= sizeof v || !text_ok(value) || strchr(value, '\n') || !utf8_ok(value, strlen(value))) { fail(err, errsz, "%s: at most 256 bytes of text", fd->label); return -1; }
    snprintf(v, sizeof v, "%s", value);
    if (strcmp(fd->type, "text")) {
        char *end; double n = strtod(v, &end);
        if (!v[0] || *end || n < fd->min || n > fd->max || (!strcmp(fd->type, "int") && n != (double)(long)n)) { fail(err, errsz, "%s: %g to %g", fd->label, fd->min, fd->max); return -1; }
    }
    if (item_dir(p, list, item, staged_, dir, sizeof dir) || access(dir, F_OK)) { fail(err, errsz, "no such item"); return -1; }
    snprintf(path, sizeof path, "%s/fields", dir);
    ps_lock();
    if ((f = ps_fopen(path))) { while (fgets(line, sizeof line, f)) had |= !strncmp(line, name, nl) && line[nl] == '='; fclose(f); }
    if (had && only_if_unset) { ps_unlock(); return 0; }
    if (statefile_open(&sf, path, 0600)) { ps_unlock(); fail(err, errsz, "cannot write on this Echo"); return -1; }
    if ((f = ps_fopen(path))) { while (fgets(line, sizeof line, f)) if (strncmp(line, name, nl) || line[nl] != '=') fputs(line, sf.f); fclose(f); }
    fprintf(sf.f, "%s=%s\n", name, v);
    r = statefile_commit(&sf);
    ps_unlock();
    if (r) fail(err, errsz, "cannot write on this Echo");
    return r;
}

int ps_item_field(const struct ps_plugin *p, const char *list, const char *item, const char *name, const char *value, int only_if_unset, char *err, size_t errsz)
{
    return item_field(p, list, item, name, value, only_if_unset, 0, err, errsz);
}

int ps_item_field_staged(const struct ps_plugin *p, const char *list, const char *item, const char *name, const char *value, char *err, size_t errsz)
{
    return item_field(p, list, item, name, value, 0, 1, err, errsz);
}

const struct ps_field *ps_item_field_of(const struct ps_plugin *p, const char *list, const char *name)
{
    const struct ps_setting *s = setting_of(p, list);
    for (int i = 0; s && i < s->nfields; i++) if (!strcmp(s->fields[i].name, name)) return &s->fields[i];
    return NULL;
}

int ps_item_open(const struct ps_plugin *p, const char *list, const char *item, const char *suffix, int staged_)
{
    const struct ps_item_file *f = file_of(setting_of(p, list), suffix); char dir[500], path[640]; int fd;
    if (!f || item_dir(p, list, item, staged_, dir, sizeof dir)) return -1;
    snprintf(path, sizeof path, "%s/%s", dir, f->as);
    /* read-only, and a regular file: such a descriptor is safe in a plugin's hands (no open, no mmap of a
     * descriptor, and one opened read-only cannot be made writable).  Opened without waiting (open_reg), and handed
     * on as any file is: the flag means nothing for a regular file, and it is taken off all the same */
    if ((fd = open_reg(path)) < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

int ps_item_digest(const char *id, const char *list, const char *item, uint8_t out[32])
{
    /* The store's lock only while the item's files are opened, never while they are read: an item is megabytes
     * (a wake word model), the page asks for every item of every plugin in one request, and with the lock held over
     * all of that the broker could not start a plugin and no other page could list one for as long as the flash
     * took.  What is read is what was there at the open: ps_item_keep puts a new take of an item into place as a
     * folder renamed, so a descriptor opened here goes on reading the old files whole.  A file that is written in
     * place all the same (nothing of hassmic's does) gives a size that is not what was read: then there is no
     * digest (-1), the page takes the item for one the other Echo has not got, and sends it again */
    char suffix[PS_ITEM_FILES][17]; int fd[PS_ITEM_FILES], n = 0, any = 0, bad = 0; const struct ps_plugin *p; const struct ps_setting *s;
    crypto_blake2b_ctx c; uint8_t *buf;
    ps_lock();
    if ((p = ps_find(id)) && (s = setting_of(p, list)) && s->type == PS_FILES)
        for (int i = 0; i < s->nfiles && n < PS_ITEM_FILES; i++) {
            if ((fd[n] = ps_item_open(p, list, item, s->files[i].suffix, 0)) < 0) continue;     /* an optional file it has not got */
            snprintf(suffix[n++], sizeof suffix[0], "%s", s->files[i].suffix);
        }
    ps_unlock();
    if (!(buf = malloc(16384))) bad = 1;
    crypto_blake2b_init(&c, 32);
    for (int i = 0; i < n; i++) {
        struct stat st; uint8_t sz[8]; ssize_t k; long long got = 0;
        if (!bad && !fstat(fd[i], &st)) {
            for (int b = 0; b < 8; b++) sz[b] = (uint8_t)((unsigned long long)st.st_size >> (8 * b));
            crypto_blake2b_update(&c, (const uint8_t *)suffix[i], strlen(suffix[i]) + 1);
            crypto_blake2b_update(&c, sz, 8);
            while ((k = read(fd[i], buf, 16384)) > 0) { crypto_blake2b_update(&c, buf, (size_t)k); got += k; }
            if (k < 0 || got != (long long)st.st_size) bad = 1; else any = 1;
        }
        close(fd[i]);
    }
    free(buf);
    crypto_blake2b_final(&c, out);
    return any && !bad ? 0 : -1;
}

/* ---------------------------------------------------------------- sync between Echos (5.4) */

int ps_pkg_read(const char *id, const char *member, long off, long len, void **data, size_t *n, long *total)
{
    static const char *const names[] = { "manifest.json", "manifest.sig", "plugin", "LICENSE", "listing.sig" };
    char p[700]; struct stat st; int fd, i, r = -1;
    for (i = 0; i < 5 && strcmp(member, names[i]); i++) ;
    if (i == 5 || !ps_id_ok(id) || off < 0 || len < 0) return -1;
    snprintf(p, sizeof p, "%s/%s/pkg/%s", ps_dir(), id, names[i]);
    ps_lock();
    if ((fd = open_reg(p)) >= 0) {
        if (!fstat(fd, &st) && st.st_size <= PS_PACKAGE_MAX && off <= st.st_size) {
            if (len > st.st_size - off) len = (long)(st.st_size - off);
            if ((*data = malloc((size_t)len + 1)) && (!len || pread(fd, *data, (size_t)len, off) == len)) { *n = (size_t)len; *total = (long)st.st_size; r = 0; }
            else { free(*data); *data = NULL; }
        }
        close(fd);
    }
    ps_unlock();
    return r;
}

void ps_voucher(uint8_t out[16], const uint8_t key[32], const uint8_t target[32], const uint8_t nonce[16], const uint8_t browser[32],
                const char *node, const char *id, const uint8_t manifest[32], const uint8_t author[32], const uint8_t accepted[32])
{
    /* the two texts with their NUL: without it "kitchen" + "-1abc" and "kitchen-1" + "abc" were one voucher */
    crypto_blake2b_ctx c;
    crypto_blake2b_keyed_init(&c, 16, key, 32);
    crypto_blake2b_update(&c, (const uint8_t *)"hassmic plugin acceptance 1", 27);
    crypto_blake2b_update(&c, target, 32); crypto_blake2b_update(&c, nonce, 16); crypto_blake2b_update(&c, browser, 32);
    crypto_blake2b_update(&c, (const uint8_t *)node, strlen(node) + 1); crypto_blake2b_update(&c, (const uint8_t *)id, strlen(id) + 1);
    crypto_blake2b_update(&c, manifest, 32); crypto_blake2b_update(&c, author, 32); crypto_blake2b_update(&c, accepted, 32);
    crypto_blake2b_final(&c, out);
}

size_t ps_catalog_json(const struct ps_plugin *p, const char *list, char *out, size_t cap)
{
    const struct ps_setting *s = setting_of(p, list); char path[500]; size_t len = 0, n = 0; char *text; struct json j = { 0 };
    if (cap) out[0] = 0;
    if (!s || s->catalog < 0) return 0;
    snprintf(path, sizeof path, "%s/%s/pkg/manifest.json", ps_dir(), p->m.id);
    if (!(text = read_file(path, MANIFEST_MAX, &len))) return 0;
    if (!json_parse(&j, text, len) && s->catalog < j.n && json_type(&j, s->catalog) == J_OBJ) {
        n = (size_t)(j.t[s->catalog].end - j.t[s->catalog].start);
        if (n < cap) { memcpy(out, text + j.t[s->catalog].start, n); out[n] = 0; } else n = 0;
    }
    json_free(&j);
    free(text);
    return n;
}
