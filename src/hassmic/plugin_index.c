/* plugin_index.h: the plugin index's key, its two signed lists read, the revocation list kept.  docs/plugins.md 5.3,
 * 5.5, 5.6.
 *
 * What is trusted here is the key and nothing else: the site is GitHub Pages, reached over HTTPS, but whoever can
 * write the published branch, or sits where TLS ends, could serve any file.  So a list is read only after its
 * signature held (plugin_fetch.c asks pi_signed first), and the reading is still that of an untrusted text: every
 * length bounded, every field of its kind, nothing past what was allocated for it (tests/unit/plugin_index_test.c
 * changes a real list at random under ASan).  A signed list that does not read is refused whole: it is the index
 * tool's fault, and half of a list is a list that says less than its signer meant.  Fields this build does not use
 * (models, needs, commit, package, and where a version came from: workflow, release, release_sha256, attestation,
 * attestation_sha256) are not looked at: in particular the package's path, since the address a package is
 * downloaded from is made of the base and the checked id and version alone (plugin_fetch.c).
 * The serials are times, held to this build's least ones and to the clock (pi_serial_ok, pi_floor, pi_held); the bounds are
 * plugin_index.h's PI_*, the index's tool has the same numbers. */
#define _GNU_SOURCE
#include "plugin_index.h"
#include "json.h"
#include "plugmsg.h"
#include "statefile.h"
#include "../third_party/monocypher.h"
#include "../include/edkey.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The Makefile passes both from keys/plugins.conf (a fork's: secrets/fork.conf); these, as that file has them, only
 * for a build outside it (an editor's, a unit test's) */
#ifndef PLUGIN_INDEX
#define PLUGIN_INDEX "https://gamer92000.github.io/hassmic-plugins/"
#endif
#ifndef PLUGINS_PUB
#define PLUGINS_PUB "c2db8560bc8fbb3cbbbb3a69755207f1abecfc1e8a89182d10c096ca3826c8f5"
#endif
_Static_assert(sizeof PLUGINS_PUB == 65, "PLUGINS_PUB: the index key's 32 bytes as 64 hex digits");
/* the second key (5.2's first step of a new key): none unless keys/plugins.conf names one */
#ifndef PLUGINS_PUB2
#define PLUGINS_PUB2 ""
#endif
_Static_assert(sizeof PLUGINS_PUB2 == 1 || sizeof PLUGINS_PUB2 == 65, "PLUGINS_PUB2: none, or a key's 32 bytes as 64 hex digits");
/* the least serials this build takes (5.5), and the commit's time (a build's own idea of "now" before the clock is
 * set): the Makefile's from keys/plugins.conf and git; these for a build outside it */
#ifndef PLUGINS_LEAST_INDEX
#define PLUGINS_LEAST_INDEX 0
#endif
#ifndef PLUGINS_LEAST_REVOKED
#define PLUGINS_LEAST_REVOKED 0
#endif
#ifndef BUILD_EPOCH
#define BUILD_EPOCH 1795000000
#endif

static void fail(char *err, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void fail(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); if (err && cap) vsnprintf(err, cap, fmt, ap); va_end(ap);
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
/* 2N lower case hex digits and nothing else: 0 */
static int unhex(uint8_t *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int a = hexv(s[2 * i]), b = a < 0 ? -1 : hexv(s[2 * i + 1]);
        if (b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return s[2 * n] ? -1 : 0;
}

const char *pi_base_builtin(void) { const char *e = getenv("HASSMIC_PLUGIN_INDEX"); return e && *e ? e : PLUGIN_INDEX; }

/* The owner's own index (pi_source, from main() before any thread of the plugins runs, and never again in a run: a
 * change of it restarts hassmic, sources.c, so nothing kept in memory here or in plugin_fetch.c was another
 * index's) */
static char own_base[256];
static uint8_t own_key[32];
static int own_has_key, own_unchecked;
void pi_source(const char *base, const uint8_t *key, int unchecked)
{
    snprintf(own_base, sizeof own_base, "%s", base ? base : "");
    own_has_key = key != NULL; if (key) memcpy(own_key, key, 32);
    own_unchecked = unchecked != 0;
}
int pi_source_trusts(void) { return own_has_key || own_unchecked; }
int pi_source_unchecked(void) { return own_unchecked; }
int pi_source_custom(void) { return own_base[0] != 0; }
const char *pi_key_name(int by) { int nb = pi_keys_builtin(); return by < 0 ? "no" : by < nb ? (by ? "second" : "first") : "own (Sources)"; }
const char *pi_base(void) { return own_base[0] ? own_base : pi_base_builtin(); }

static int key_file(const char *path, uint8_t pk[32])
{
    FILE *f = fopen(path, "rb"); int ok = f && fread(pk, 1, 32, f) == 32;
    if (f) fclose(f);
    return ok ? 0 : -1;
}

/* The tests' first key replaces the built-in ones, both: a test with a key of its own never also takes a list signed
 * by the build's second key.  A second key a test names is read beside whichever first key there is */
static int keys_builtin(uint8_t pk[PI_KEYS][32])
{
    const char *e = getenv("HASSMIC_PLUGINS_PUB"), *e2 = getenv("HASSMIC_PLUGINS_PUB2");
    int own = e && *e;
    if (own ? key_file(e, pk[0]) : unhex(pk[0], PLUGINS_PUB, 32)) return -1;
    if (e2 && *e2) return key_file(e2, pk[1]) ? -1 : 2;
    if (own || !PLUGINS_PUB2[0]) return 1;
    return unhex(pk[1], PLUGINS_PUB2, 32) ? -1 : 2;
}

/* ... and the owner's own after them (pi_source): the built-in ones keep their places, so 5.2's rules (the second
 * key is the new one) and what a listing installed says of its key stay what they were */
static int keys_read(uint8_t pk[PI_KEYS][32])
{
    int n = keys_builtin(pk);
    if (n > 0 && own_has_key) memcpy(pk[n++], own_key, 32);
    return n;
}

int pi_keys_builtin(void) { uint8_t pk[PI_KEYS][32]; int n = keys_builtin(pk); return n < 1 ? 1 : n; }

/* ... and none of them a key that vouches for anything (edkey.h: under a key of small order the null signature
 * holds for every text; the Makefile refuses to build one in, this holds for the tests' keys and for a build made
 * around the Makefile).  One such key and there are none: an index that cannot tell its own lists from anyone's
 * takes nothing (a review of a30d6ed, K3) */
int pi_keys(uint8_t pk[PI_KEYS][32])
{
    static int said;
    int n = keys_read(pk);
    for (int i = 0; i < n; i++)
        if (ed_key_weak(pk[i])) {
            if (!said) { said = 1; fprintf(stderr, "plugins: the plugin index's %s key is one under which any signature holds: no list of the index is taken\n", i == 0 ? "first" : i == 1 && pi_keys_builtin() == 2 ? "second" : "own (Sources)"); }
            return -1;
        }
    return n;
}

int pi_signer(const char *purpose, const void *data, size_t len, const uint8_t *sig, size_t siglen)
{
    uint8_t pk[PI_KEYS][32]; size_t pl = strlen(purpose); uint8_t *msg; int by = -1, n;
    if (siglen != 64 || (n = pi_keys(pk)) < 1 || !(msg = malloc(pl + len + 1))) return -1;
    memcpy(msg, purpose, pl); if (len) memcpy(msg + pl, data, len);
    for (int i = 0; i < n && by < 0; i++) if (!crypto_eddsa_check(sig, pk[i], msg, pl + len)) by = i;
    free(msg);
    /* No check (the owner's word on the settings page, with the action button): the index's two lists that no key
     * signed count as the owner's, in the place the owner's key would have (after the built-in ones).  Never a
     * package's listing (purpose "", plugin_store.c listing_by): that is read from every package, a file uploaded or
     * copied from another Echo too, and unchecked any 128 hex digits made a file "official" (past the community
     * switch, log.read.full's and sync's rule for official ones; a review, 2026-11-24).  A package of an index
     * without a key carries no listing that holds: refused from the index (bring_ok), a file without one installs
     * only as a community plugin, by "Install from file" */
    if (by < 0 && own_unchecked && *purpose) by = pi_keys_builtin();
    return by;
}

int pi_signed(const char *purpose, const void *data, size_t len, const uint8_t *sig, size_t siglen)
{
    return pi_signer(purpose, data, len, sig, siglen) >= 0;
}

/* ---------------------------------------------------------------- the serials (5.5) */

static long long least_env(int which)       /* HASSMIC_PLUGINS_LEAST=<index>:<revoked>, the tests' */
{
    const char *e = getenv("HASSMIC_PLUGINS_LEAST"), *c = e ? strchr(e, ':') : NULL;
    if (!e || !*e) return -1;
    return which ? (c ? atoll(c + 1) : -1) : atoll(e);
}
/* (the owner's own index with a key of its own or no check counts its serials as it likes: the build's least are
 * this project's index's, and would refuse an index the owner made before this build was released) */
static long long least_built_index(void) { long long e = least_env(0); return e >= 0 ? e : (long long)PLUGINS_LEAST_INDEX; }
long long pi_least_index(void) { return pi_source_trusts() ? 0 : least_built_index(); }
/* (a listing the project's key signed keeps the build's least under an index of the owner's too: else one under it,
 * which the least is there to stop, installed as official from a file, a copy or a push; a review, 2026-11-24) */
long long pi_least_listing(int by) { return by >= 0 && by < pi_keys_builtin() ? least_built_index() : 0; }
long long pi_least_revoked(void) { long long e = least_env(1); return pi_source_trusts() ? 0 : e >= 0 ? e : (long long)PLUGINS_LEAST_REVOKED; }

static int (*clock_set)(void);
void pi_clock(int (*synced)(void)) { clock_set = synced; }
static int synced(void) { return clock_set ? clock_set() : 1; }
int pi_clock_set(void) { return synced(); }
/* the build's own time, before the clock is set: the commit's (HASSMIC_PLUGINS_BUILT for the tests, whose tree's
 * last commit is of any age) */
static long long built(void) { const char *e = getenv("HASSMIC_PLUGINS_BUILT"); return e && *e ? atoll(e) : (long long)BUILD_EPOCH; }

int pi_serial_ok(long long serial, char *err, size_t cap)
{
    long long now = (long long)time(NULL);
    if (synced() && serial > now + PI_AHEAD_S) {
        fail(err, cap, "its serial (%lld) is a time more than a day after this Echo's clock: no list the index made, refused", serial);
        return -1;
    }
    return 0;
}

/* The floor line a list of SERIAL taken now is kept with.  With the clock set the serial itself (pi_serial_ok took
 * nothing past now + a day).  Without it at most the build's own time + a day: one day, as the index's tool caps its
 * serial (the time of its commit, a day ahead at most for a clock a little fast).  It is kept as it was made, never
 * computed again from the serial with a later clock (a far serial taken before the clock was set would then be held
 * to "now + a day" for ever, a day ahead of every honest list), and it counts only where the kept serial cannot be
 * believed (pi_held) */
long long pi_floor(long long serial, long long least)
{
    long long cap = (synced() ? (long long)time(NULL) : built()) + PI_AHEAD_S;
    if (serial > cap) serial = cap;
    return serial > least ? serial : least;
}

/* The floor a kept list holds the next to (5.5).  Taken while the clock was not set, a list's floor line was capped
 * at the build's time + a day, and it was that line that held later lists: once the clock was set, an older signed
 * list from between that cap and the kept serial was taken over the newer one, and what the newer one withdrew ran
 * again (a review, 2026-11-21).  Now the kept serial itself is the floor wherever it can be believed: with the clock
 * set, unless it is past now + a day (then it was taken before the clock was set, and it is the floor line, at most
 * the build's time + a day, that lets honest lists in again); and the caller raises the line to the serial the first
 * time the clock vouches for it, so that a clock set behind later does not open it again.  Without a clock the
 * revocation list's keeper takes its kept serial (CLOCKLESS_KEPT: never an older list, the review's rule), the index's
 * its floor line (an older index taken there offers nothing the revocation list kept withdraws, which never goes
 * back) */
long long pi_held(long long serial, long long floor, long long least, int clockless_kept, int *raise)
{
    long long h;
    *raise = 0;
    if (serial < 0) return least;
    if (synced()) {
        if (serial <= (long long)time(NULL) + PI_AHEAD_S) { h = serial; *raise = floor < serial; }
        else h = floor;
    } else h = clockless_kept ? serial : floor;
    return h > least ? h : least;
}

/* ---------------------------------------------------------------- reading, field by field */

/* a text of at most MOST bytes, nothing under 0x20 and no 0x7f (json_str turns some escapes into spaces: the
 * written form is held too, as the store's tstr does), into OUT (at least MOST + 1) */
static int text(const struct json *j, int obj, const char *key, char *out, size_t most)
{
    int t = json_get(j, obj, key); char buf[1024]; size_t n;
    out[0] = 0;
    if (json_type(j, t) != J_STR || j->t[t].end - j->t[t].start >= (int)sizeof buf) return -1;
    for (int i = j->t[t].start; i + 1 < j->t[t].end; i++) {
        char c;
        if (j->s[i] != '\\') continue;
        c = j->s[++i];
        if (c && strchr("bfnrt", c)) return -1;
        if (c == 'u' && i + 4 < j->t[t].end) {
            unsigned v = 0;
            for (int k = 1; k <= 4; k++) { int d = hexv((char)(j->s[i + k] | 0x20)); v = v << 4 | (unsigned)(d < 0 ? 0 : d); }
            if (v < 0x20 || v == 0x7f) return -1;
            i += 4;
        }
    }
    if (!json_str(j, t, buf, sizeof buf) || (n = strlen(buf)) > most) return -1;
    for (size_t i = 0; i < n; i++) if ((unsigned char)buf[i] < 0x20 || buf[i] == 0x7f) return -1;
    memcpy(out, buf, n + 1);
    return 0;
}
/* the same into the index's pool: its offset there (the pool can move until the list is read whole), -1 */
static long pooled(const struct json *j, int obj, const char *key, size_t most, char *pool, size_t *used, size_t cap)
{
    char buf[1024]; size_t n; long at = (long)*used;
    if (text(j, obj, key, buf, most) || (n = strlen(buf)) + 1 > cap - *used) return -1;
    memcpy(pool + *used, buf, n + 1); *used += n + 1;
    return at;
}
/* digits only, no sign, no leading zero, at most 12 of them: a serial (a time), a version, a size */
static int whole(const struct json *j, int t, long long *out)
{
    const char *s; int n; long long v = 0;
    if (json_type(j, t) != J_NUM) return -1;
    s = j->s + j->t[t].start; n = j->t[t].end - j->t[t].start;
    if (n < 1 || n > 12 || (n > 1 && s[0] == '0')) return -1;
    for (int i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') return -1; v = v * 10 + (s[i] - '0'); }
    *out = v;
    return 0;
}
static int hex32(const struct json *j, int obj, const char *key, uint8_t out[32])
{
    char b[80];
    return text(j, obj, key, b, 79) || strlen(b) != 64 || unhex(out, b, 32) ? -1 : 0;
}

static int version_parse(const struct json *j, int o, struct pi_version *v, char *err, size_t cap, const char *id)
{
    int t; long long x;
    memset(v, 0, sizeof *v);
    if (json_type(j, o) != J_OBJ || whole(j, json_get(j, o, "version"), &x) || x < 1 || x > 1000000000) { fail(err, cap, "%s: a version that is no whole number from 1", id); return -1; }
    v->version = (long)x;
    if (text(j, o, "version_name", v->version_name, PI_VERSION_NAME_MAX)) { fail(err, cap, "%s version %ld: its version_name", id, v->version); return -1; }
    /* "requires": what its manifest requires, an object of a scope's name (1 to 24 of a-z, 0-9, . and _) and a whole
     * number from 1 each, core among them (the index's manifest_check took the manifest so); a name this build does
     * not know is a later hassmic's scope: no misreading, a version this build does not run */
    if (json_type(j, t = json_get(j, o, "requires")) != J_OBJ) { fail(err, cap, "%s version %ld: what it requires", id, v->version); return -1; }
    for (int k = json_first(j, t); k >= 0; ) {
        int val = j->t[k].next, sc; char name[PS_SCOPE_NAME + 1]; long long x2; int ok = 1;
        if (val >= j->t[t].next) break;
        if (json_type(j, k) != J_STR || j->t[k].end - j->t[k].start > PS_SCOPE_NAME || !json_str(j, k, name, sizeof name) || !name[0]) ok = 0;
        for (const char *c = name; ok && *c; c++) ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_';
        if (!ok || whole(j, val, &x2) || x2 < 1 || x2 > 65535) { fail(err, cap, "%s version %ld: what it requires", id, v->version); return -1; }
        if ((sc = pm_scope_of(name, strlen(name))) >= 0) v->req[sc] = (uint16_t)x2; else v->req_unknown = 1;
        k = json_next(j, t, val);
    }
    if (!v->req[PM_SCOPE_CORE]) { fail(err, cap, "%s version %ld: what it requires (no core)", id, v->version); return -1; }
    if (hex32(j, o, "manifest", v->manifest) || hex32(j, o, "sha256", v->sha256)) { fail(err, cap, "%s version %ld: its manifest's digest or its package's SHA-256", id, v->version); return -1; }
    if (whole(j, json_get(j, o, "size"), &x) || x < 1) { fail(err, cap, "%s version %ld: its package's size", id, v->version); return -1; }
    v->size = x;
    if (json_type(j, t = json_get(j, o, "capabilities")) != J_OBJ) { fail(err, cap, "%s version %ld: its capabilities", id, v->version); return -1; }
    for (int k = json_first(j, t); k >= 0; ) {
        int val = j->t[k].next, bit; char name[PI_CAP_NAME_MAX + 1];
        if (val >= j->t[t].next) break;
        if (json_type(j, k) != J_STR || !json_str(j, k, name, sizeof name)) { fail(err, cap, "%s version %ld: a capability's name", id, v->version); return -1; }
        for (bit = 0; ps_cap_name(bit) && strcmp(ps_cap_name(bit), name); bit++) ;
        if (ps_cap_name(bit)) v->caps |= PM_CAPBIT(bit); else v->caps_unknown = 1;
        k = json_next(j, t, val);
    }
    /* "broad": a community version's flags, the broad capabilities the index found it asks for (docs/plugins.md 5.3:
     * net.lan with any port, net.internet, call, request.audio, request.text, log.read.full), which the browse page
     * shows beside it.  Only community versions have it.  Where it is there, a list of at most PI_BROAD_MAX names,
     * each of a capability's kind; one this build does not know is left out (the version asks for it, and
     * caps_unknown says it does not run here) */
    if ((t = json_get(j, o, "broad")) >= 0) {
        int n = 0;
        if (json_type(j, t) != J_ARR) { fail(err, cap, "%s version %ld: its broad capabilities", id, v->version); return -1; }
        JSON_EACH(j, t, k) {
            /* (the name's length as written, before json_str: that cuts a longer one short without a word, and a
             * name of PI_CAP_NAME_MAX bytes and more would pass as an unknown one; the index's BOUNDS keeps every
             * name under it, and plugin_index_test refuses one of 32 bytes) */
            char name[PI_CAP_NAME_MAX + 1]; int bit;
            if (++n > PI_BROAD_MAX || json_type(j, k) != J_STR || j->t[k].end - j->t[k].start >= (int)sizeof name || !json_str(j, k, name, sizeof name)) {
                fail(err, cap, "%s version %ld: its broad capabilities", id, v->version); return -1;
            }
            for (bit = 0; ps_cap_name(bit) && strcmp(ps_cap_name(bit), name); bit++) ;
            if (ps_cap_name(bit)) v->broad |= PM_CAPBIT(bit);
        }
    }
    return 0;
}

/* offsets into the pool while it is filled: turned into pointers once it is allocated for good */
struct texts { long name, author, description, source; };

static int plugin_parse(const struct json *j, int o, struct pi_plugin *p, struct texts *tx, struct pi_version *vs, char *pool, size_t *used, size_t pcap,
                        int *dropped, char *err, size_t cap)
{
    char tier[16], key[80]; int vl, k = 0, total; long last = 0;
    if (json_type(j, o) != J_OBJ || text(j, o, "id", p->id, PS_ID_MAX - 1) || !ps_id_ok(p->id)) { fail(err, cap, "a plugin without an id this hassmic takes"); return -1; }
    if ((tx->name = pooled(j, o, "name", PI_NAME_MAX, pool, used, pcap)) < 0 || (tx->author = pooled(j, o, "author", PI_AUTHOR_MAX, pool, used, pcap)) < 0
        || (tx->description = pooled(j, o, "description", PI_DESCRIPTION_MAX, pool, used, pcap)) < 0 || (tx->source = pooled(j, o, "source", PI_SOURCE_MAX, pool, used, pcap)) < 0) {
        fail(err, cap, "%s: its name, author, description or source", p->id); return -1;
    }
    if (text(j, o, "author_key", key, 79) || strlen(key) != 64 || unhex(p->author_key, key, 32)) { fail(err, cap, "%s: its author key", p->id); return -1; }
    if (text(j, o, "tier", tier, 15) || (strcmp(tier, "official") && strcmp(tier, "community"))) { fail(err, cap, "%s: a tier that is neither official nor community", p->id); return -1; }
    p->tier = tier[0] == 'o' ? PS_OFFICIAL : PS_COMMUNITY;
    if (json_type(j, vl = json_get(j, o, "versions")) != J_ARR) { fail(err, cap, "%s: no list of versions", p->id); return -1; }
    total = json_len(j, vl);
    p->v = vs; p->n = 0;
    JSON_EACH(j, vl, e) {
        struct pi_version v;
        if (version_parse(j, e, &v, err, cap, p->id)) return -1;
        /* the index has them lowest first, each higher than the one before (its tool's rule), dropped ones too */
        if (k && v.version <= last) { fail(err, cap, "%s: versions not in order", p->id); return -1; }
        last = v.version;
        if (total - k <= PI_VERSIONS) p->v[p->n++] = v; else (*dropped)++;      /* the highest PI_VERSIONS */
        k++;
    }
    return 0;
}

int pi_index_parse(const char *s, size_t len, struct pi_index *ix, char *err, size_t cap)
{
    struct json j = { 0 }; int pl, r = -1, np, nv = 0, k = 0; long long x; size_t used = 0; struct texts *tx = NULL;
    memset(ix, 0, sizeof *ix);
    if (len > PI_INDEX_MAX || !len || s[0] != '{' || memchr(s, 0, len) || json_parse(&j, s, len) || json_type(&j, 0) != J_OBJ) { fail(err, cap, "the plugin index is no JSON object"); goto out; }
    if (whole(&j, json_get(&j, 0, "index"), &x) || x != 1) { fail(err, cap, "the plugin index is of a form this hassmic does not read"); goto out; }
    if (whole(&j, json_get(&j, 0, "serial"), &x)) { fail(err, cap, "the plugin index has no serial"); goto out; }
    ix->serial = x;
    if (whole(&j, json_get(&j, 0, "revoked_serial"), &x)) { fail(err, cap, "the plugin index does not say which revocation list it was made with (revoked_serial)"); goto out; }
    ix->revoked_serial = x;
    if (json_type(&j, pl = json_get(&j, 0, "plugins")) != J_ARR) { fail(err, cap, "the plugin index has no list of plugins"); goto out; }
    /* sized as they are: the plugins kept, the versions kept of each, every text in one pool no longer than the list
     * itself (no text is longer unescaped than written), cut to what it holds at the end */
    np = json_len(&j, pl); if (np > PI_PLUGINS_MAX) { ix->dropped += np - PI_PLUGINS_MAX; np = PI_PLUGINS_MAX; }
    JSON_EACH(&j, pl, e) {
        int vl = json_get(&j, e, "versions"), n = json_type(&j, vl) == J_ARR ? json_len(&j, vl) : 0;
        if (k++ == np) break;
        nv += n < PI_VERSIONS ? n : PI_VERSIONS;
    }
    if ((np && !(ix->p = calloc((size_t)np, sizeof *ix->p))) || (nv && !(ix->vs = malloc(sizeof *ix->vs * (size_t)nv))) || (np && !(tx = calloc((size_t)np, sizeof *tx)))
        || !(ix->pool = malloc(len + 1))) { fail(err, cap, "out of memory"); goto out; }
    k = 0; nv = 0;
    JSON_EACH(&j, pl, e) {
        if (ix->n == np) break;
        if (plugin_parse(&j, e, &ix->p[ix->n], &tx[ix->n], ix->vs + nv, ix->pool, &used, len + 1, &ix->dropped, err, cap)) goto out;
        for (int q = 0; q < ix->n; q++) if (!strcmp(ix->p[q].id, ix->p[ix->n].id)) { fail(err, cap, "the plugin index has %s twice", ix->p[q].id); goto out; }
        nv += ix->p[ix->n].n;
        ix->n++;
    }
    { char *np2 = realloc(ix->pool, used ? used : 1); if (np2) ix->pool = np2; }
    for (int q = 0; q < ix->n; q++) {
        ix->p[q].name = ix->pool + tx[q].name; ix->p[q].author = ix->pool + tx[q].author;
        ix->p[q].description = ix->pool + tx[q].description; ix->p[q].source = ix->pool + tx[q].source;
    }
    r = 0;
out:
    free(tx);
    json_free(&j);
    if (r) pi_index_free(ix);
    return r;
}

void pi_index_free(struct pi_index *ix) { free(ix->p); free(ix->vs); free(ix->pool); memset(ix, 0, sizeof *ix); }

/* an id as a revocation names one: what the store takes, and what builds before its rule on hyphens took (a plugin
 * put on such an Echo from a file can have one, and the list has to be able to name it: hassmic-plugins ID_WAS) */
static int id_was(const char *id)
{
    size_t n = strlen(id);
    if (n < 3 || n > 24 || id[0] == '-') return 0;
    for (const char *c = id; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) return 0;
    return 1;
}

int pi_revoked_parse(const char *s, size_t len, struct pi_revoked *rv, char *err, size_t cap)
{
    struct json j = { 0 }; int es, r = -1, n; long long x;
    memset(rv, 0, sizeof *rv);
    if (len > PI_REVOKED_MAX || !len || s[0] != '{' || memchr(s, 0, len) || json_parse(&j, s, len) || json_type(&j, 0) != J_OBJ) {
        fail(err, cap, len > PI_REVOKED_MAX ? "the revocation list is over %u kB, more than this hassmic reads: refused whole" : "the revocation list is no JSON object", PI_REVOKED_MAX >> 10); goto out;
    }
    if (whole(&j, json_get(&j, 0, "revoked"), &x) || x != 1) { fail(err, cap, "the revocation list is of a form this hassmic does not read"); goto out; }
    if (whole(&j, json_get(&j, 0, "serial"), &x)) { fail(err, cap, "the revocation list has no serial"); goto out; }
    rv->serial = x;
    if (json_type(&j, es = json_get(&j, 0, "entries")) != J_ARR) { fail(err, cap, "the revocation list has no entries"); goto out; }
    /* Past the bound the whole list is refused, and the look with it: entries left out would be plugins that run
     * though the index withdrew them.  It was the first 2048 taken and the rest counted, never shown (a review,
     * 2026-11-21): fail open.  The index's tool publishes no list past it */
    if ((n = json_len(&j, es)) > PI_REVOCATIONS) { fail(err, cap, "the revocation list has %d entries, more than the %d this hassmic reads: refused whole", n, PI_REVOCATIONS); goto out; }
    if (n && !(rv->r = calloc((size_t)n, sizeof *rv->r))) { fail(err, cap, "out of memory"); goto out; }
    JSON_EACH(&j, es, e) {
        struct pi_revocation *q = &rv->r[rv->n]; char key[80];
        if (json_type(&j, e) != J_OBJ || text(&j, e, "reason", q->reason, PI_REASON_MAX)) { fail(err, cap, "a revocation without its reason"); goto out; }
        if (json_get(&j, e, "author_key") >= 0) {
            if (text(&j, e, "author_key", key, 79) || strlen(key) != 64 || unhex(q->key, key, 32) || json_get(&j, e, "id") >= 0) { fail(err, cap, "a revocation's author key"); goto out; }
            q->by_key = 1;
        } else {
            int u = json_get(&j, e, "upto");
            if (text(&j, e, "id", q->id, PS_ID_MAX - 1) || !id_was(q->id)) { fail(err, cap, "a revocation names no plugin and no author"); goto out; }
            if (u >= 0 && (whole(&j, u, &x) || x < 1 || x > 1000000000)) { fail(err, cap, "%s: a revocation's upto", q->id); goto out; }
            q->upto = u >= 0 ? (long)x : 0;
        }
        rv->n++;
    }
    r = 0;
out:
    json_free(&j);
    if (r) pi_revoked_free(rv);
    return r;
}

void pi_revoked_free(struct pi_revoked *rv) { free(rv->r); memset(rv, 0, sizeof *rv); }

/* ---------------------------------------------------------------- the revocation list this Echo keeps */

/* The rule, all in pi_held: a list is taken only with a higher serial than the one kept (PI_SAME: the same), whatever
 * the clock says, with one exception: the clock is set and says the kept serial is more than a day in the future (it
 * was taken before the clock was set); then any list from its floor line on is taken (the line: the build's time + a
 * day at most, pi_floor).  With the clock never set (Wyoming, no Home Assistant time) the kept list is the floor for
 * good: a list with a serial from the far future, which only the index key can sign and its tool refuses to make,
 * would hold every later list out there until the clock is set (it withdraws what it names meanwhile); an older list
 * taken over a newer one is what anyone who can serve a file could do, a far serial only the key's holder.  The least
 * serial refuses takes and says whether the list kept is current; it never stops a kept list from withdrawing */

static pthread_mutex_t rl = PTHREAD_MUTEX_INITIALIZER;
static struct pi_revoked kept;          /* rl */
static long long kept_floor = -1;       /* rl: the file's floor line (pi_floor when it was taken, raised by pi_held) */
static int kept_read, floor_owed;       /* rl: the file was looked at; the line raised in memory, not yet on the flash */
static int kept_bad;                    /* rl: a file was there and its list does not hold (no key of this build signed it) */
static int kept_by = -1;                /* rl: which of this build's keys signed the list kept (pi_signer), -1 none */
/* The word (5.6's warning): when this Echo last had word that its list is the newest.  word_mono on this file's own
 * clock (seconds of CLOCK_BOOTTIME, below 0 where the word is older than the boot: the age kept over a restart),
 * word_wall the wall time where the clock was set then (or since: worked back from word_mono) */
static int word_known;                  /* rl */
static long long word_mono, word_wall;  /* rl */
static long long at_mono = -1, at_age = -1;     /* rl: .revoked-at last written in this run (its time, the age it said) */
static int at_owed;                     /* rl: a word since, not written yet */
static long long skew;                  /* rl: pi_test_skew */

static long long mono_s(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_BOOTTIME, &t)) clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec + skew;
}

static void kept_path(char *p, size_t cap) { snprintf(p, cap, "%s/.revoked", ps_dir()); }
static void at_path(char *p, size_t cap) { snprintf(p, cap, "%s/.revoked-at", ps_dir()); }

static void *slurp(const char *path, size_t max, size_t *len)
{
    FILE *f = ps_fopen(path); char *b; size_t n;
    *len = 0;
    if (!f) return NULL;
    if (!(b = malloc(max + 1))) { fclose(f); return NULL; }
    n = fread(b, 1, max + 1, f); fclose(f);
    if (n > max) { free(b); return NULL; }
    b[n] = 0; *len = n;
    return b;
}

/* The file's form: "floor <n>", a newline, the signature's 128 hex digits, a newline, the list as it came (the last
 * two are listing.sig's own layout, and what another Echo is handed: pi_revoked_file).  One file, written through
 * statefile.c: the old one or the new one after a power cut, never a list beside another list's signature (they were
 * two files, and a cut between their writes forgot the list: a review, 2026-11-21) */
#define SIG_LINE 129

/* the list part of the file B of N bytes: its offset, 0 where it is not of that form */
static size_t file_body(const char *b, size_t n, long long *floor)
{
    const char *nl = n > 6 ? memchr(b, '\n', n < 40 ? n : 40) : NULL; char num[32]; size_t h;
    if (!nl || strncmp(b, "floor ", 6) || (size_t)(nl - b - 6) >= sizeof num || nl == b + 6) return 0;
    memcpy(num, b + 6, (size_t)(nl - b - 6)); num[nl - b - 6] = 0;
    for (const char *c = num; *c; c++) if (*c < '0' || *c > '9') return 0;
    *floor = atoll(num);
    h = (size_t)(nl - b) + 1;
    return n > h + SIG_LINE && b[h + SIG_LINE - 1] == '\n' ? h : 0;
}

/* rl held, a list kept: what the next has to reach (and the floor line raised where the clock vouches for it) */
static long long held(void)
{
    int raise; long long h = pi_held(kept.serial, kept_floor, pi_least_revoked(), 1, &raise);
    if (raise) { kept_floor = kept.serial; floor_owed = 1; }
    return h;
}

/* rl held: the file again with the floor line raised (the list and its signature as they are).  Its line only */
static void floor_flush(void)
{
    char p[400], head[40]; size_t n = 0, h; char *b, *o; long long was; int hl;
    if (!floor_owed || kept.serial < 0) return;
    floor_owed = 0;
    kept_path(p, sizeof p);
    if (!(b = slurp(p, PI_REVOKED_MAX + SIG_LINE + 40, &n)) || !(h = file_body(b, n, &was))) { free(b); return; }
    if (was == kept_floor) { free(b); return; }
    hl = snprintf(head, sizeof head, "floor %lld\n", kept_floor);
    if ((o = malloc((size_t)hl + n - h))) {
        memcpy(o, head, (size_t)hl); memcpy(o + hl, b + h, n - h);
        if (statefile_write(p, 0600, o, (size_t)hl + n - h)) fprintf(stderr, "plugins: cannot write the revocation list's floor (%s): it holds in this run\n", p);
        else fprintf(stderr, "plugins: the revocation list's floor raised to its serial %lld (the clock vouches for it)\n", kept_floor);
    }
    free(o); free(b);
}

/* rl held: what the flash has, checked again as it was when it came: a file changed there since is no list.  One
 * under this build's least serial still withdraws what it names, and is replaced by any list from the least on (it
 * was dropped: an update that raised the least ran what the kept list withdrew, for good where the site is out of
 * reach; a review, 2026-11-21) */
static void kept_load(void)
{
    char p[400], err[200]; size_t n = 0, h; char *b; uint8_t sig[64]; long long floor = -1;
    if (kept_read) return;
    kept_read = 1; kept.serial = -1; kept_floor = -1; floor_owed = 0; kept_bad = 0;
    kept_path(p, sizeof p);
    b = slurp(p, PI_REVOKED_MAX + SIG_LINE + 40, &n);
    kept_by = -1;
    if (b && (h = file_body(b, n, &floor)) && (b[h + SIG_LINE - 1] = 0, !unhex(sig, b + h, 64))
        && (kept_by = pi_signer(PI_SIGN_REVOKED, b + h + SIG_LINE, n - h - SIG_LINE, sig, 64)) >= 0
        && !pi_revoked_parse(b + h + SIG_LINE, n - h - SIG_LINE, &kept, err, sizeof err)) {
        kept_floor = floor > kept.serial ? kept.serial : floor;
        if (kept.serial < pi_least_revoked())
            fprintf(stderr, "plugins: the revocation list kept here (serial %lld) is older than this hassmic wants (%lld): what it withdraws stays withdrawn, and a newer one is asked for\n", kept.serial, pi_least_revoked());
        held();
    }
    else if (b) { fprintf(stderr, "plugins: the revocation list kept here (%s) is not one the index key signed: ignored until the next look\n", p); pi_revoked_free(&kept); kept.serial = -1; kept_bad = 1; kept_by = -1; }
    free(b);
    /* A kept list that no longer holds (a hassmic of other index keys than the one that signed it: 5.2's step back to
     * a build of one key, or the flash changed) is no word either: the word in .revoked-at was on that list, and what
     * that list withdrew runs here again.  So the week's warning comes at once, in words that say why (pf_revoked_stale),
     * not a week after the last word on a list this build cannot read (a review of a30d6ed, K6) */
    if (kept_bad) return;
    /* "word <wall> <age>"; before 2026-11-21's second round "<wall>" alone (written with the clock set only) */
    at_path(p, sizeof p);
    if ((b = slurp(p, 80, &n))) {
        long long w = 0, a = 0;
        /* (the older form is a wall time and nothing else: its age is the clock's, now; with the clock not set
         * there is none, and it counts as no word at all, i.e. old: read as an age of 0 it began the week's count
         * anew at every start before the clock was set.  So does one ahead of the clock: a clock behind the word's
         * time says nothing, and the form has no count of its own to fall back on (it was clipped to an age of 0,
         * "fresh": a review of a6c5a00)) */
        if (sscanf(b, "word %lld %lld", &w, &a) == 2 && w >= 0 && a >= 0) {
            word_known = 1; word_wall = w; word_mono = mono_s() - a; at_age = a;
        } else if (sscanf(b, "%lld", &w) == 1 && w > 0 && synced() && (a = (long long)time(NULL) - w) >= 0) {
            word_known = 1; word_wall = w; word_mono = mono_s() - a; at_age = a;
        }
        free(b);
    }
}

long long pi_revoked_serial(void)
{
    long long s;
    pthread_mutex_lock(&rl); kept_load(); s = kept.serial; pthread_mutex_unlock(&rl);
    return s;
}

int pi_revoked_unread(void)
{
    int r;
    pthread_mutex_lock(&rl); kept_load(); r = kept_bad && kept.serial < 0; pthread_mutex_unlock(&rl);
    return r;
}

int pi_revoked_current(void)
{
    long long s = pi_revoked_serial();
    return s >= 0 && s >= pi_least_revoked();
}

/* rl held: the age now, -1 no word */
static long long age_now(void)
{
    long long a, w;
    if (!word_known) return -1;
    a = mono_s() - word_mono; if (a < 0) a = 0;
    /* the clock set since a word without it: its wall time, worked back (what it counts is the least the age is) */
    if (!word_wall && synced()) { word_wall = (long long)time(NULL) - a; at_owed = 1; }
    /* the clock's word where it says more: this Echo's own count misses the time it was off.  A clock behind the
     * word's time says nothing (it was clipped to 0: "fresh", for as long as Home Assistant's clock was behind) */
    if (word_wall && synced() && (w = (long long)time(NULL) - word_wall) > a) a = w;
    return a;
}

/* rl held: word that the list kept is the newest: now (AS_OF -1, the site's answer), or no later than AS_OF, the
 * serial of a newer list a member handed over.  A member's list says only that it was the newest when it was made
 * (its serial is the time of its last change); dated "now", a member holding an archive of the index's lists, older
 * than the newest but each newer than the last it handed over, kept the week's warning away with one a week (a
 * review, 2026-11-21).  With the clock set the serial itself where it is not ahead of now; without it at least the
 * build's own time after the serial (the build was made after the list: an age it surely has; a serial past the
 * build's time says nothing without a clock) */
static void word(long long as_of)
{
    long long now = (long long)time(NULL), back = 0;
    if (as_of >= 0) {
        if (synced()) back = now - as_of;
        else back = built() - as_of;
        if (back < 0) back = 0;
    }
    word_known = 1; word_mono = mono_s() - back; word_wall = synced() ? now - back : 0; at_owed = 1;
}

/* rl held: .revoked-at's text into T when it is to be written now, its length; 0 not now.  At most once an hour
 * (PI_AT_EVERY_S): a word sooner waits for the hour (lost with a restart, the warning comes that much sooner); and
 * once an hour while the age grows towards the week's warning, so that a restart keeps it (the clock not set, the age
 * kept is all there is) */
static int at_due(char *t, size_t cap)
{
    long long m = mono_s(), a = age_now();
    if (a < 0 || (at_mono >= 0 && m - at_mono < PI_AT_EVERY_S)) return 0;
    if (!at_owed && (a - at_age < PI_AT_EVERY_S || at_age > PI_STALE_S + PI_AT_EVERY_S)) return 0;
    at_owed = 0; at_mono = m; at_age = a;
    return snprintf(t, cap, "word %lld %lld\n", word_wall, a);
}
static void at_write(const char *t, int n)          /* no lock held */
{
    char p[400];
    if (n <= 0) return;
    at_path(p, sizeof p); mkdir(ps_dir(), 0700);
    if (statefile_write(p, 0600, t, (size_t)n)) fprintf(stderr, "plugins: cannot write %s: the time of the last word on the revocation list holds in this run\n", p);
}

/* rl held: .revoked written anew, FLOOR's line, SIG's and the list TEXT of LEN bytes; 0 done */
static int kept_write(long long floor, const uint8_t *sig, const char *text, size_t len)
{
    char p[400], head[40]; int hl = snprintf(head, sizeof head, "floor %lld\n", floor), r = -1;
    char *b = malloc((size_t)hl + SIG_LINE + len);
    kept_path(p, sizeof p);
    mkdir(ps_dir(), 0700);
    if (b) {
        memcpy(b, head, (size_t)hl);
        for (int i = 0; i < 64; i++) snprintf(b + hl + 2 * i, 3, "%02x", sig[i]);
        b[hl + SIG_LINE - 1] = '\n'; memcpy(b + hl + SIG_LINE, text, len);
        r = statefile_write(p, 0600, b, (size_t)hl + SIG_LINE + len) ? -1 : 0;
    }
    free(b);
    return r;
}

int pi_revoked_take(const char *text, size_t len, const uint8_t *sig, size_t siglen, int from, char *err, size_t cap)
{
    struct pi_revoked nr; char p[400], at[80]; int r, atn, by; long long h;
    if ((by = pi_signer(PI_SIGN_REVOKED, text, len, sig, siglen)) < 0) { fail(err, cap, "the revocation list's signature does not hold: it is not one the index key signed"); return PI_REFUSED; }
    if (pi_revoked_parse(text, len, &nr, err, cap)) return PI_REFUSED;
    if (nr.serial < pi_least_revoked()) {
        fail(err, cap, "the revocation list it answered with is older (serial %lld) than this hassmic takes (%lld): not taken", nr.serial, pi_least_revoked());
        pi_revoked_free(&nr); return PI_REFUSED;
    }
    { char e2[200]; if (pi_serial_ok(nr.serial, e2, sizeof e2)) { fail(err, cap, "the revocation list: %s", e2); pi_revoked_free(&nr); return PI_REFUSED; } }
    pthread_mutex_lock(&rl);
    kept_load();
    h = held();
    if (kept.serial >= 0 && nr.serial == kept.serial) {
        /* The same list by another of this build's keys: kept as it came now.  5.2's step (2) signs the same list
         * anew with the new key; kept under the old key's signature, the step-(3) build (the new key alone) read the
         * file as no list, and everything it withdrew ran from that update until the next look reached the site
         * (and its members were handed the old key's list meanwhile).  The floor line stays the one kept (a review
         * of 3c5d7ff, R1; the index's tool also raises the serial at that publish, which makes this a PI_TAKEN).
         * Only towards the second key, which 5.2 makes the new one: rewritten for whichever key came last, a member
         * that handed over the first key's copy of the same list turned the file back to the old key, which the
         * step-(3) build reads as no list, and two sources of one serial had the flash written at every look (a
         * review of 3fcd319, S3) */
        r = PI_SAME;
        if (by > kept_by) {
            kept_path(p, sizeof p);
            if (kept_write(kept_floor, sig, text, len))
                fprintf(stderr, "plugins: cannot keep the revocation list's new signature on the flash (%s): the one kept stays, until the next look\n", p);
            else { fprintf(stderr, "plugins: the revocation list (serial %lld) kept with the signature of the index's %s key now\n", nr.serial, pi_key_name(by)); pi_revoked_free(&kept); kept = nr; kept_by = by; nr.r = NULL; nr.n = 0; }
        }
        pi_revoked_free(&nr); if (from == PI_SITE) word(-1);
    }
    else if (nr.serial < h) {
        fail(err, cap, "the revocation list it answered with is older (serial %lld) than the one this Echo has (%lld): not taken", nr.serial, kept.serial);
        r = PI_OLDER; pi_revoked_free(&nr);
    } else {
        /* (under the one kept and not under what it holds: the one kept is from the future by the clock set now) */
        /* The line never falls: one the clock vouched for stays under every list taken after it.  A list taken
         * while the clock was not set had its line capped at the build's time + a day, under a line raised before
         * to a serial the clock had vouched for; then with Home Assistant's clock more than a day behind the new
         * kept serial, the lowered line held, and a list older than the vouched one was taken (a review,
         * 2026-11-21).  The kept line is at most the kept serial, and that at most this one: never past it */
        long long nf = pi_floor(nr.serial, pi_least_revoked());
        if (kept.serial >= 0 && nf < kept_floor) nf = kept_floor;
        kept_path(p, sizeof p);
        if (kept_write(nf, sig, text, len))
            fprintf(stderr, "plugins: cannot keep the revocation list on the flash (%s): it holds until the next start only\n", p);
        pi_revoked_free(&kept); kept = nr; kept_by = by; kept_floor = nf; floor_owed = 0; kept_read = 1; r = PI_TAKEN; word(from == PI_MEMBER ? kept.serial : -1);
        held();
    }
    floor_flush();
    atn = at_due(at, sizeof at);
    pthread_mutex_unlock(&rl);
    at_write(at, atn);
    return r;
}

void pi_revoked_upkeep(void)
{
    char at[80]; int atn;
    pthread_mutex_lock(&rl);
    kept_load();
    if (kept.serial >= 0) { held(); floor_flush(); }
    atn = kept.serial >= 0 ? at_due(at, sizeof at) : 0;
    pthread_mutex_unlock(&rl);
    at_write(at, atn);
}

size_t pi_revoked_file(char **out)
{
    char p[400]; size_t n = 0, h; char *b; long long s, f;
    *out = NULL;
    pthread_mutex_lock(&rl); kept_load(); s = kept.serial; pthread_mutex_unlock(&rl);
    if (s < 0) return 0;
    kept_path(p, sizeof p);
    if (!(b = slurp(p, PI_REVOKED_MAX + SIG_LINE + 40, &n)) || !(h = file_body(b, n, &f))) { free(b); return 0; }
    memmove(b, b + h, n - h);           /* the signature's line and the list: what the site has, in one */
    *out = b;
    return n - h;
}

long long pi_revoked_age(void)
{
    long long r;
    pthread_mutex_lock(&rl);
    kept_load();
    r = kept.serial >= 0 ? age_now() : -1;
    pthread_mutex_unlock(&rl);
    return r;
}

int pi_withdrawn(const char *id, long version, const uint8_t key[32], char *why, size_t cap)
{
    int r = 0;
    pthread_mutex_lock(&rl);
    kept_load();
    for (int i = 0; !r && i < kept.n; i++) {
        const struct pi_revocation *q = &kept.r[i];
        if (q->by_key ? key && !memcmp(q->key, key, 32) : !strcmp(q->id, id) && (!q->upto || version <= q->upto)) {
            r = 1;
            if (why && cap) snprintf(why, cap, "%s", q->reason);
        }
    }
    pthread_mutex_unlock(&rl);
    return r;
}

void pi_forget(void)
{
    pthread_mutex_lock(&rl);
    pi_revoked_free(&kept); kept_read = 0; kept_floor = -1; floor_owed = 0; kept_by = -1;
    word_known = 0; word_mono = word_wall = 0; at_mono = at_age = -1; at_owed = 0;
    pthread_mutex_unlock(&rl);
}

void pi_test_skew(long long s) { pthread_mutex_lock(&rl); skew += s; pthread_mutex_unlock(&rl); }
