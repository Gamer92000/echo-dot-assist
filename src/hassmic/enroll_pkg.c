/* The set of a word trained on the Echo: see enroll_pkg.h. */
#include "enroll_pkg.h"
#include "hash.h"
#include "json.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONF_MAX (256 * 1024)           /* the set's JSON files are a few KB; more is not Amazon's set */
#define MAX_FILES 128
#define MAX_RANGES 64

static void say(char *err, size_t errsz, const char *what) { if (err && errsz) snprintf(err, errsz, "%s", what); }

int enroll_name(const char *phrase, const char *lang, char *key, size_t kcap, char *id, size_t icap, char *err, size_t errsz)
{
    char k[64]; size_t n = 0; int word = 0;
    for (const unsigned char *p = (const unsigned char *)phrase; *p; p++) {
        if (*p >= 0x80) { say(err, errsz, "use letters and digits only (no accents)"); return -1; }
        if (isalnum(*p)) {
            if (word && n < sizeof k - 1) k[n++] = '_';                 /* a gap between words, once */
            word = 0;
            if (n < sizeof k - 1) k[n++] = (char)toupper(*p);
            else { say(err, errsz, "that name is too long"); return -1; }
        } else if (n) word = 1;
    }
    k[n] = 0;
    if (n < 3 || n > 32) { say(err, errsz, n < 3 ? "the name needs at least 3 letters" : "the name is too long (32 letters at most)"); return -1; }
    if (!isalpha((unsigned char)k[0])) { say(err, errsz, "the name must start with a letter"); return -1; }
    if (!strcmp(k, "ALEXA") || !strcmp(k, "STOP")) { say(err, errsz, "that name belongs to Amazon's own words"); return -1; }
    for (const char *l = lang; *l; l++) if (!isalnum((unsigned char)*l) && *l != '-') { say(err, errsz, "bad language"); return -1; }
    if (!*lang || strlen(lang) > 12 || strlen(k) + 1 + strlen(lang) >= icap || n >= kcap) { say(err, errsz, "that name is too long"); return -1; }
    snprintf(key, kcap, "%s", k);
    for (size_t i = 0; i < n; i++) id[i] = (char)tolower((unsigned char)k[i]);
    snprintf(id + n, icap - n, "-%s", lang);
    return 0;
}

/* ---------------------------------------------------------------------------------------------- JSON: objects named */

struct range { int from, to; };
static int cmp_range(const void *a, const void *b) { return ((const struct range *)a)->from - ((const struct range *)b)->from; }

static int named(const struct json *j, int e, const char *name)
{
    if (json_type(j, e) != J_OBJ) return 0;
    int v = json_get(j, e, "name");
    return json_type(j, v) == J_STR && json_streq(j, v, name);
}

/* The text to cut for the deleted elements of array T.  A run of deleted ones takes the comma before it with it, or, at
 * the start, the one after (and the space up to the next element), so what is left is valid whatever the layout was. */
static int walk(const struct json *j, int t, const char *name, struct range *r, int *nr, int *removed)
{
    int arr = json_type(j, t) == J_ARR, prev_kept_end = -1, run_from = -1, run_to = -1;
    for (int e = json_first(j, t); e >= 0; e = json_next(j, t, e)) {
        if (arr && named(j, e, name)) {
            if (run_from < 0) run_from = e;
            run_to = e; (*removed)++;
            continue;
        }
        if (arr && run_from >= 0) {                                         /* a run ends at the kept element e */
            if (*nr == MAX_RANGES) return -1;
            if (prev_kept_end < 0) r[(*nr)++] = (struct range){ j->t[run_from].start, j->t[e].start };
            else r[(*nr)++] = (struct range){ prev_kept_end, j->t[run_to].end };
            run_from = -1;
        }
        if (j->t[e].type == J_OBJ || j->t[e].type == J_ARR) { if (walk(j, e, name, r, nr, removed) < 0) return -1; }
        if (arr) prev_kept_end = j->t[e].end;
    }
    if (arr && run_from >= 0) {                                             /* a run at the end */
        if (*nr == MAX_RANGES) return -1;
        r[(*nr)++] = (struct range){ prev_kept_end < 0 ? j->t[run_from].start : prev_kept_end, j->t[run_to].end };
    }
    return 0;
}

int enroll_strip_named(char *text, size_t *len, size_t cap, const char *name)
{
    struct json j = { 0 }; struct range r[MAX_RANGES]; int nr = 0, total = 0;
    (void)cap;
    if (json_parse(&j, text, *len)) { json_free(&j); return -1; }
    int bad = walk(&j, 0, name, r, &nr, &total);
    json_free(&j);
    if (bad < 0) return -1;
    qsort(r, (size_t)nr, sizeof *r, cmp_range);                            /* nested arrays add theirs before the outer's */
    for (int i = nr - 1; i >= 0; i--) {                                    /* from the back: the earlier offsets stay right */
        memmove(text + r[i].from, text + r[i].to, *len - (size_t)r[i].to);
        *len -= (size_t)(r[i].to - r[i].from);
    }
    text[*len] = 0;
    return total;
}

/* every "ALEXA" (a whole JSON string) -> "KEY"; the number replaced */
static int rename_word(char *text, size_t *len, size_t cap, const char *key)
{
    static const char old[] = "\"ALEXA\""; char *p = text; int n = 0; size_t kl = strlen(key) + 2;
    while ((p = strstr(p, old))) {
        if (*len - 7 + kl + 1 > cap) return -1;
        memmove(p + kl, p + 7, *len - (size_t)(p + 7 - text) + 1);
        p[0] = '"'; memcpy(p + 1, key, kl - 2); p[kl - 1] = '"';
        *len = *len - 7 + kl; p += kl; n++;
    }
    return n;
}

/* ------------------------------------------------------------------------------------------------------- files */

static int read_all(const char *path, char **out, size_t *len, size_t max)
{
    FILE *f = fopen(path, "rb"); long n; char *b;
    if (!f) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
    if (n < 0 || (size_t)n > max || !(b = malloc((size_t)n + 4096))) { fclose(f); return -1; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return -1; }
    fclose(f); b[n] = 0; *out = b; *len = (size_t)n;
    return 0;
}

static int write_all(const char *path, const void *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644); ssize_t w;
    if (fd < 0) return -1;
    w = write(fd, data, n);
    if (w != (ssize_t)n || fsync(fd) || close(fd)) { unlink(path); return -1; }
    return 0;
}

static int copy_file(const char *from, const char *to)
{
    static char buf[65536]; int in = open(from, O_RDONLY | O_NOFOLLOW), out; ssize_t r; int bad = 0;
    if (in < 0) return -1;
    if ((out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644)) < 0) { close(in); return -1; }
    while ((r = read(in, buf, sizeof buf)) > 0) {
        for (ssize_t k = 0; k < r; ) { ssize_t w = write(out, buf + k, (size_t)(r - k)); if (w <= 0) { bad = 1; break; } k += w; }
        if (bad) break;
    }
    if (r < 0) bad = 1;
    close(in);
    if (bad || fsync(out) || close(out)) { unlink(to); return -1; }
    return 0;
}

static void remove_tree(const char *dir)
{
    DIR *d = opendir(dir); struct dirent *e; char p[512]; struct stat st;
    if (d) {
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if (!lstat(p, &st) && S_ISDIR(st.st_mode)) remove_tree(p); else unlink(p);
        }
        closedir(d);
    }
    rmdir(dir);
}

static int cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* the regular files of DIR (SUB inside), two levels of folders at most, as "sub/name" paths into names[n..] */
static int list(const char *dir, const char *sub, char names[][128], int n, int depth)
{
    char p[512], name[128]; struct dirent *e; struct stat st; DIR *d;
    snprintf(p, sizeof p, "%s%s%s", dir, *sub ? "/" : "", sub);
    if (!(d = opendir(p))) return -1;
    while ((e = readdir(d)) && n >= 0) {
        if (e->d_name[0] == '.') continue;
        if (snprintf(name, sizeof name, "%s%s%s", sub, *sub ? "/" : "", e->d_name) >= (int)sizeof name) { n = -1; break; }
        snprintf(p, sizeof p, "%s/%s", dir, name);
        if (lstat(p, &st)) { n = -1; break; }
        if (S_ISDIR(st.st_mode) && depth < 2) n = list(dir, name, names, n, depth + 1);
        else if (S_ISREG(st.st_mode) && n < MAX_FILES) snprintf(names[n++], 128, "%s", name);
        else { n = -1; break; }                                              /* a link, a device, too much: not Amazon's set */
    }
    closedir(d);
    return n;
}

static int mkdirs(const char *root, const char *name)           /* the folders of "a/b/file" under ROOT */
{
    char p[512]; const char *s = name;
    for (const char *slash; (slash = strchr(s, '/')); s = slash + 1) {
        snprintf(p, sizeof p, "%s/%.*s", root, (int)(slash - name), name);
        if (mkdir(p, 0755) && errno != EEXIST) return -1;
    }
    return 0;
}

int enroll_make_set(const char *base, const char *candidate, const char *dest, const char *key, const char *note, char *err, size_t errsz)
{
    static char names[MAX_FILES][128]; char from[512], to[512], *text = NULL; size_t len; int n, rc = -1;
    if (mkdir(dest, 0755)) { say(err, errsz, "cannot make the set's folder"); return -1; }
    if ((n = list(base, "", names, 0, 0)) <= 0) { say(err, errsz, "the Echo's own wake word set is not what this expects"); goto out; }
    qsort(names, (size_t)n, 128, cmp);
    int model = 0;
    for (int i = 0; i < n; i++) {
        snprintf(to, sizeof to, "%.255s/%.127s", dest, names[i]);
        if (mkdirs(dest, names[i])) { say(err, errsz, "cannot make a folder of the set"); goto out; }
        if (!strcmp(names[i], ENROLL_MODEL)) { snprintf(from, sizeof from, "%s", candidate); model = 1; }
        else snprintf(from, sizeof from, "%.255s/%.127s", base, names[i]);
        if (copy_file(from, to)) { say(err, errsz, "cannot copy the set (space?)"); goto out; }
    }
    if (!model) { say(err, errsz, "the Echo's own wake word set has no model to replace"); goto out; }

    /* the word in Alexa's place, STOP out */
    static const char *conf[] = { "kw.cfg.json", "op.cfg.json", "watermark.cfg.json" };
    for (int c = 0; c < 3; c++) {
        snprintf(to, sizeof to, "%s/%s", dest, conf[c]);
        if (read_all(to, &text, &len, CONF_MAX)) { say(err, errsz, "the Echo's own wake word set lacks a file this expects"); goto out; }
        int stop = c < 2 ? enroll_strip_named(text, &len, len + 4096, "STOP") : 0, renamed = -1;
        if (stop >= 0) renamed = rename_word(text, &len, len + 4096, key);
        if (stop < 0 || (c < 2 && !stop) || renamed < 1 || strstr(text, "\"STOP\"")) {
            free(text); text = NULL; say(err, errsz, "the Echo's own wake word set is not Amazon's Alexa and Stop set"); goto out;
        }
        unlink(to);
        if (write_all(to, text, len)) { free(text); text = NULL; say(err, errsz, "cannot write the set's configuration"); goto out; }
        free(text); text = NULL;
    }
    snprintf(to, sizeof to, "%s/words.shrunk.txt", dest);
    if (read_all(to, &text, &len, 4096)) { say(err, errsz, "the Echo's own wake word set lacks its word list"); goto out; }
    {   char a[3][32]; int got = sscanf(text, "<eps> 0 %31s 1 %31s 2 %31s", a[0], a[1], a[2]);    /* the symbols, as Amazon's file has them */
        int ok = got == 2 && !strcmp(a[0], "ALEXA") && !strcmp(a[1], "STOP");
        free(text); text = NULL;
        if (!ok) { say(err, errsz, "the Echo's own word list is not Alexa and Stop"); goto out; }
    }
    char words[160]; snprintf(words, sizeof words, "<eps> 0\n%s 1\nSTOP 2\n", key);
    unlink(to);
    if (write_all(to, words, strlen(words))) { say(err, errsz, "cannot write the word list"); goto out; }

    snprintf(to, sizeof to, "%s/%s", dest, ENROLL_MARK);
    if (write_all(to, note, strlen(note))) { say(err, errsz, "cannot write the set's note"); goto out; }

    /* a name for exactly this content: Amazon's is an md5 the engine does not check; ours is 128 bits of SHA-256 over
     * every file, so a set that changes in any way has another one */
    struct sha256 c; uint8_t dg[32]; char hexs[33];
    sha256_init(&c);
    if ((n = list(dest, "", names, 0, 0)) <= 0) { say(err, errsz, "cannot read the new set"); goto out; }
    qsort(names, (size_t)n, 128, cmp);
    for (int i = 0; i < n; i++) {
        if (!strcmp(names[i], "checksum.txt")) continue;
        snprintf(to, sizeof to, "%.255s/%.127s", dest, names[i]);
        if (read_all(to, &text, &len, 64 * 1024 * 1024)) { say(err, errsz, "cannot read the new set"); goto out; }
        sha256_update(&c, names[i], strlen(names[i]) + 1); sha256_update(&c, text, len);
        free(text); text = NULL;
    }
    sha256_final(&c, dg);
    for (int i = 0; i < 16; i++) snprintf(hexs + 2 * i, 3, "%02x", dg[i]);
    snprintf(to, sizeof to, "%s/checksum.txt", dest);
    unlink(to);
    if (write_all(to, hexs, 32)) { say(err, errsz, "cannot write the set's checksum"); goto out; }
    rc = 0;
out:
    free(text);
    if (rc) remove_tree(dest);
    return rc;
}
