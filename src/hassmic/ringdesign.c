/*
 * The light ring's designs (ringdesign.h, issue #38).  The library and the assignments are the daemon's
 * (state/ring/<id>, state/ring/assign), drafts the settings page edits at will; only "Apply" changes the ring:
 *   - state/ring-applied gets every assigned design in one file, then state/leds-request an id;
 *   - root's main.sh (ota_watch) runs leds(): hassmic -G makes the set from that file (ring.c: listening's design
 *     turned by each LED there, not when drawn), sysinstall.sh writes what changed to the system partition and
 *     ledcontroller starts again (it reads animations only at its start; about a second without the ring);
 *   - root answers "<id> ok|same|failed <why>" in its own leds.result; rd_tick reads it: ok, and ring.c takes the new
 *     set (ring_reload: what showed is set again); failed, and the applied file goes back to what is installed, so a
 *     start of hassmic still finds its own set there.
 * state/ is the daemon's and root parses the applied file: ringdesign.h says how it is read (a regular file, not
 * followed through a link, of bounded size), and rd_parse takes nothing but the format above.  Root writes the set from
 * its own folder, made by its own hassmic; nothing of the daemon's is copied or run.
 */
#include "ringdesign.h"
#include "ring.h"
#include "statefile.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define APPLIED_HEAD "hassmic ring applied 1\n"
#define WAIT_ASK_S   180                    /* root takes a request within 2 s; still there after this: nobody will */
#define WAIT_MAX_S   900                    /* root took it: an install takes seconds, this is root gone for good */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
static void spath(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", state_dir(), rel); }
static const char *result_path(void) { const char *e = getenv("HASSMIC_LEDS_RESULT"); return e ? e : "/data/local/hassmic/leds.result"; }

/* ---------------------------------------------------------------- text */

struct sb { char *p; size_t n, cap; int bad; };
static void sput(struct sb *b, const char *fmt, ...)
{
    va_list ap; int k;
    for (;;) {
        va_start(ap, fmt); k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap); va_end(ap);
        if (k < 0) { b->bad = 1; return; }
        if (b->p && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        size_t cap = (b->cap ? b->cap * 2 : 4096) + (size_t)k;
        char *p = realloc(b->p, cap);
        if (!p) { b->bad = 1; return; }
        b->p = p; b->cap = cap;
    }
}

/* A file read whole: a regular file only, never through a link (state/ is the daemon's; root reads it too), at most
 * max bytes.  malloc'd and NUL-ended, NULL if missing, not regular, too long or unreadable */
static char *read_file(const char *file, size_t max, size_t *len)
{
    int fd = open(file, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC); struct stat st; char *t = NULL;
    if (fd < 0) return NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && (size_t)st.st_size <= max && (t = malloc((size_t)st.st_size + 1))) {
        size_t n = 0; ssize_t k;
        while (n < (size_t)st.st_size && (k = read(fd, t + n, (size_t)st.st_size - n)) > 0) n += (size_t)k;
        t[n] = 0;
        if (len) *len = n;
    }
    close(fd);
    return t;
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* ---------------------------------------------------------------- a design */

void rd_free(struct rd_design *d) { free(d->f); memset(d, 0, sizeof *d); }

static int fail(char *err, size_t cap, int line, const char *why)
{
    if (line) snprintf(err, cap, "line %d: %s", line, why); else snprintf(err, cap, "%s", why);
    return -1;
}

int rd_parse(const char *text, size_t n, struct rd_design *d, char *err, size_t cap)
{
    memset(d, 0, sizeof *d);
    int line = 0, have_loop = 0, capf = 0;
    for (size_t at = 0; at < n; ) {
        const char *l = text + at, *nl = memchr(l, '\n', n - at);
        size_t len = nl ? (size_t)(nl - l) : n - at;
        at += len + (nl != NULL);
        line++;
        if (len && l[len - 1] == '\r') len--;
        if (!len || l[0] == '#') continue;
        if (len > 5 && !memcmp(l, "name ", 5)) {
            if (len - 5 >= RD_NAME) { rd_free(d); return fail(err, cap, line, "name too long"); }
            for (size_t i = 5; i < len; i++) if ((unsigned char)l[i] < 0x20 || l[i] == 0x7f) { rd_free(d); return fail(err, cap, line, "control character in the name"); }
            memcpy(d->name, l + 5, len - 5); d->name[len - 5] = 0;
        }
        else if (len > 5 && !memcmp(l, "leds ", 5)) {
            char v[8] = ""; int k;
            snprintf(v, sizeof v, "%.*s", (int)(len - 5 < 7 ? len - 5 : 7), l + 5);
            if (d->nframes || sscanf(v, "%d", &k) != 1 || k < 1 || k > RD_LEDS_MAX) { rd_free(d); return fail(err, cap, line, "bad leds"); }
            d->leds = k;
        }
        else if (len > 5 && !memcmp(l, "loop ", 5)) {
            char v[12] = ""; int k;
            snprintf(v, sizeof v, "%.*s", (int)(len - 5 < 11 ? len - 5 : 11), l + 5);
            if (!strcmp(v, "once")) d->loop = -1;
            else if (sscanf(v, "%d", &k) == 1 && k >= 0 && k < RD_FRAMES) d->loop = k;
            else { rd_free(d); return fail(err, cap, line, "bad loop"); }
            have_loop = 1;
        }
        else if (isdigit((unsigned char)l[0])) {
            size_t i = 0; long ms = 0;
            while (i < len && isdigit((unsigned char)l[i]) && ms <= 60000) ms = ms * 10 + (l[i++] - '0');
            if (i >= len || l[i] != ':' || ms < 1 || ms > 60000) { rd_free(d); return fail(err, cap, line, "frame: <1..60000 ms>:<colours>"); }
            if (!d->leds) { rd_free(d); return fail(err, cap, line, "leds before the frames"); }
            if (d->nframes >= RD_FRAMES) { rd_free(d); return fail(err, cap, line, "too many frames"); }
            if (len - i - 1 != (size_t)d->leds * 7 - 1) { rd_free(d); return fail(err, cap, line, "frame: one RRGGBB per LED"); }
            if (d->nframes == capf) {
                capf = capf ? capf * 2 : 16;
                struct rd_frame *f = realloc(d->f, (size_t)capf * sizeof *f);
                if (!f) { rd_free(d); return fail(err, cap, line, "no memory"); }
                d->f = f;
            }
            struct rd_frame *f = &d->f[d->nframes];
            f->ms = (int)ms;
            const char *c = l + i + 1;
            for (int k = 0; k < d->leds; k++, c += 7) {
                if (k && c[-1] != ',') { rd_free(d); return fail(err, cap, line, "frame: colours separated by commas"); }
                for (int ch = 0; ch < 3; ch++) {
                    int hi = hexv(c[2 * ch]), lo = hexv(c[2 * ch + 1]);
                    if (hi < 0 || lo < 0) { rd_free(d); return fail(err, cap, line, "frame: colours are RRGGBB"); }
                    f->px[k][ch] = (unsigned char)(hi << 4 | lo);
                }
            }
            for (int k = d->leds; k < RD_LEDS_MAX; k++) memset(f->px[k], 0, 3);
            d->nframes++;
        }
        else { rd_free(d); return fail(err, cap, line, "not a design's line"); }
    }
    if (!d->nframes) { rd_free(d); return fail(err, cap, 0, "a design needs a frame"); }
    if (!have_loop) d->loop = 0;
    if (d->loop >= d->nframes) { rd_free(d); return fail(err, cap, 0, "the loop starts after the last frame"); }
    if (!d->name[0]) snprintf(d->name, sizeof d->name, "Design");
    return 0;
}

char *rd_format(const struct rd_design *d)
{
    struct sb b = { 0 };
    sput(&b, "name %s\nleds %d\n", d->name, d->leds);
    if (d->loop < 0) sput(&b, "loop once\n"); else sput(&b, "loop %d\n", d->loop);
    for (int k = 0; k < d->nframes; k++) {
        sput(&b, "%d:", d->f[k].ms);
        for (int i = 0; i < d->leds; i++) sput(&b, "%s%02X%02X%02X", i ? "," : "", d->f[k].px[i][0], d->f[k].px[i][1], d->f[k].px[i][2]);
        sput(&b, "\n");
    }
    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}

/* ---------------------------------------------------------------- what is applied */

static int slot_index(const char *id)
{
    for (int i = 0; i < ring_nslots; i++) if (!strcmp(ring_slots[i].id, id)) return i;
    return -1;
}

int rd_applied_parse(const char *text, size_t n, struct rd_design *out, int *ids)
{
    memset(out, 0, sizeof *out * (size_t)ring_nslots);
    for (int i = 0; i < ring_nslots; i++) ids[i] = 0;
    if (n < sizeof APPLIED_HEAD - 1 || memcmp(text, APPLIED_HEAD, sizeof APPLIED_HEAD - 1)) return -1;
    size_t at = sizeof APPLIED_HEAD - 1;
    while (at < n) {
        const char *l = text + at, *nl = memchr(l, '\n', n - at);
        size_t len = nl ? (size_t)(nl - l) : n - at;
        at += len + (nl != NULL);
        char id[32]; int did;
        if (!len) continue;
        if (len >= 40 || len < 6 || memcmp(l, "slot ", 5)) return -1;
        char hd[48]; memcpy(hd, l, len); hd[len] = 0;
        if (sscanf(hd, "slot %31s %d", id, &did) != 2) return -1;
        /* its design: up to the line "end" */
        size_t from = at, to = at;
        for (;;) {
            if (to >= n) return -1;
            const char *e = memchr(text + to, '\n', n - to);
            size_t ll = e ? (size_t)(e - text - to) : n - to;
            if (ll == 3 && !memcmp(text + to, "end", 3)) { at = to + ll + (e != NULL); break; }
            to += ll + (e != NULL);
        }
        int i = slot_index(id); char err[80];
        if (i < 0 || out[i].nframes) continue;          /* a slot of a newer build: left alone */
        if (rd_parse(text + from, to - from, &out[i], err, sizeof err)) { fprintf(stderr, "ring: applied design for %s: %s\n", id, err); continue; }
        ids[i] = did;
    }
    return 0;
}

char *rd_applied_read(size_t *n)
{
    char p[320]; spath(p, sizeof p, "ring-applied");
    return read_file(p, RD_APPLIED_MAX, n);
}

/* state/ring-installed: the applied designs root is known to have installed (ring.c installed() confirms them by the
 * installed set's digest).  Apart from state/ring-applied, which is what root is asked to install and may not be:
 * a failed or cut install leaves ring-applied a draft, and this tells the set that is really there */
char *rd_installed_read(size_t *n)
{
    char p[320]; spath(p, sizeof p, "ring-installed");
    return read_file(p, RD_APPLIED_MAX, n);
}

void rd_installed_set(const char *text, size_t n)
{
    char p[320]; spath(p, sizeof p, "ring-installed");
    if (statefile_write(p, 0600, text, n)) fprintf(stderr, "ring: cannot write %s: %s\n", p, strerror(errno));
}

static long long mono_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec; }

/* ---------------------------------------------------------------- the library (lock held) */

static int assign[RING_SLOTS_MAX];          /* design id per slot, 0: stock's */
static int loaded;
static int waiting; static unsigned wait_id; static long long wait_since;     /* CLOCK_MONOTONIC s: clock.c sets the wall clock */
static char last_error[200];

/* lock held: the slot lines' design ids of an applied file into ids[] (0: none) */
static void slot_ids(const char *t, int *ids)
{
    for (int i = 0; i < RING_SLOTS_MAX; i++) ids[i] = 0;
    for (const char *l = t; l && *l; ) {
        const char *e = strchr(l, '\n'); char id[32]; int did;
        if (!strncmp(l, "slot ", 5) && sscanf(l, "slot %31s %d", id, &did) == 2) { int i = slot_index(id); if (i >= 0) ids[i] = did; }
        l = e ? e + 1 : NULL;
    }
}

static void design_path(char *out, size_t n, int id) { char rel[32]; snprintf(rel, sizeof rel, "ring/%d", id); spath(out, n, rel); }

static int design_exists(int id);

/* The assignments at a start.  A design gone meanwhile (deleted while state/ring/assign could not be written) is
 * stock's again here: else every Apply would refuse that slot, and the page would show a design that is not there */
static void load_assign(void)
{
    char p[320]; spath(p, sizeof p, "ring/assign");
    for (int i = 0; i < RING_SLOTS_MAX; i++) assign[i] = 0;
    char *t = read_file(p, 1 << 16, NULL);
    for (char *l = t; l && *l; ) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        char id[32]; int did;
        if (sscanf(l, "%31s %d", id, &did) == 2) { int i = slot_index(id); if (i >= 0 && did > 0 && did <= RD_MAX && design_exists(did)) assign[i] = did; }
        l = e ? e + 1 : NULL;
    }
    free(t);
    loaded = 1;
}

/* a: what to write (assign, or what it is to become once the file has it) */
static int save_assign(const int *a)
{
    char p[320], d[320]; struct sb b = { 0 };
    for (int i = 0; i < ring_nslots; i++) if (a[i]) sput(&b, "%s %d\n", ring_slots[i].id, a[i]);
    spath(d, sizeof d, "ring"); mkdir(d, 0700);
    spath(p, sizeof p, "ring/assign");
    int rc = b.bad ? -1 : statefile_write(p, 0600, b.p ? b.p : "", b.n);
    free(b.p);
    return rc;
}

static char *design_text(int id, size_t *n)
{
    char p[320]; design_path(p, sizeof p, id);
    return read_file(p, RD_TEXT_MAX, n);
}

static void pack_path(char *out, size_t n, int id);
static void packs_forget(int id);
static int design_exists(int id) { char p[320]; struct stat st; design_path(p, sizeof p, id); return !lstat(p, &st) && S_ISREG(st.st_mode); }

/* The applied file the assignments make now, or NULL and why in err.  An assigned design that cannot be read or
 * parsed fails it whole: left out, root would install stock's animation there and Apply would say it was done */
static char *draft_applied(size_t *n, char *err, size_t cap)
{
    struct sb b = { 0 };
    sput(&b, "%s", APPLIED_HEAD);
    for (int i = 0; i < ring_nslots && !b.bad; i++) {
        if (!assign[i]) continue;
        size_t tn; char *t = design_text(assign[i], &tn), why[80];
        struct rd_design d;
        if (!t) { snprintf(err, cap, "%s: design %d cannot be read (%s)", ring_slots[i].id, assign[i], design_exists(assign[i]) ? "too long or unreadable" : "it is gone"); free(b.p); return NULL; }
        if (rd_parse(t, tn, &d, why, sizeof why)) { snprintf(err, cap, "%s: design %d is broken: %s", ring_slots[i].id, assign[i], why); free(t); free(b.p); return NULL; }
        char *f = rd_format(&d);
        if (f) sput(&b, "slot %s %d\n%send\n", ring_slots[i].id, assign[i], f); else b.bad = 1;
        free(f); free(t); rd_free(&d);
    }
    if (b.bad) { snprintf(err, cap, "no memory"); free(b.p); return NULL; }
    *n = b.n;
    return b.p;
}

static void jstr(struct sb *b, const char *s)
{
    sput(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') sput(b, "\\%c", c);
        else if (c < 0x20) sput(b, "\\u%04x", c);
        else sput(b, "%c", c);
    }
    sput(b, "\"");
}

static const char *const kind_names[] = { "plain", "listen", "end", "volume", "assist" };

size_t rd_status_json(char *out, size_t cap)
{
    struct sb b = { 0 }; size_t an = 0, dn = 0, total = 0; int applied_ids[RING_SLOTS_MAX];
    pthread_mutex_lock(&lock);
    if (!loaded) load_assign();
    char why[200], *applied = rd_installed_read(&an), *draft = draft_applied(&dn, why, sizeof why);    /* NULL: pending, Apply says why */
    slot_ids(applied, applied_ids);         /* which design each slot has on the ring: the installed designs */
    int same = applied ? draft && an == dn && !memcmp(applied, draft, an) : draft && dn == sizeof APPLIED_HEAD - 1;
    const char *st = waiting ? "installing" : last_error[0] ? "failed" : !same ? "pending" : "applied";
    sput(&b, "{\"installed\":%s,\"leds\":%d,\"max\":%d,\"frames_max\":%d,\"state\":\"%s\",\"error\":",
         ring_custom() ? "true" : "false", ring_leds(), RD_MAX, RD_FRAMES, st);
    jstr(&b, last_error);
    sput(&b, ",\"slots\":[");
    for (int i = 0; i < ring_nslots; i++)
        sput(&b, "%s{\"id\":\"%s\",\"kind\":\"%s\",\"available\":%s,\"design\":%d,\"applied\":%d,\"live\":%s}", i ? "," : "",
             ring_slots[i].id, kind_names[ring_slots[i].kind], ring_slot_available(i) ? "true" : "false", assign[i],
             applied_ids[i], ring_slot_live(i) ? "true" : "false");
    sput(&b, "],\"designs\":[");
    int first = 1;
    for (int id = 1; id <= RD_MAX; id++) {
        size_t tn; char *t = design_text(id, &tn);
        if (!t) continue;
        total += tn;
        char name[RD_NAME] = "Design"; int frames = 0;
        for (char *l = t; *l; ) {
            char *e = strchr(l, '\n'); size_t len = e ? (size_t)(e - l) : strlen(l);
            if (len > 5 && !strncmp(l, "name ", 5)) snprintf(name, sizeof name, "%.*s", (int)(len - 5), l + 5);
            else if (isdigit((unsigned char)*l)) frames++;
            l = e ? e + 1 : l + len;
        }
        sput(&b, "%s{\"id\":%d,\"name\":", first ? "" : ",", id); jstr(&b, name); sput(&b, ",\"frames\":%d}", frames);
        first = 0; free(t);
    }
    sput(&b, "],\"library_bytes\":%zu,\"library_max\":%d,\"packs_max\":%d}", total, RD_LIBRARY_MAX, RD_PACKS_MAX);
    pthread_mutex_unlock(&lock);
    free(applied); free(draft);
    size_t n = b.bad || !b.p ? 0 : b.n < cap ? b.n : 0;
    if (n) memcpy(out, b.p, n);
    if (cap) out[n < cap ? n : cap - 1] = 0;
    free(b.p);
    return n;
}

char *rd_library_text(size_t *n)
{
    struct sb b = { 0 };
    pthread_mutex_lock(&lock);
    for (int id = 1; id <= RD_MAX; id++) {
        char *t = design_text(id, NULL);
        if (t) sput(&b, "design %d\n%s\n", id, t);
        free(t);
    }
    pthread_mutex_unlock(&lock);
    if (b.bad) { free(b.p); return NULL; }
    if (!b.p) sput(&b, "%s", "");
    *n = b.n;
    return b.p;
}

static size_t library_bytes(int except)
{
    size_t total = 0; char p[320]; struct stat st;
    for (int id = 1; id <= RD_MAX; id++) {
        if (id == except) continue;
        design_path(p, sizeof p, id);
        if (!lstat(p, &st) && S_ISREG(st.st_mode)) total += (size_t)st.st_size;
    }
    return total;
}

int rd_save(int id, const char *text, size_t n, int *saved, char *err, size_t cap)
{
    struct rd_design d; char p[320];
    if (id < 0 || id > RD_MAX) return fail(err, cap, 0, "no such design");
    if (rd_parse(text, n, &d, err, cap)) return -1;
    char *f = rd_format(&d);
    rd_free(&d);
    if (!f) return fail(err, cap, 0, "no memory");
    pthread_mutex_lock(&lock);
    int rc = -1;
    /* a new one: the lowest id free, and not one the designs on the ring or asked for still name (a deleted design's
     * id taken again would look like what is installed, and the change would not show as one to apply) */
    int used[RD_MAX + 1] = { 0 }, ids[RING_SLOTS_MAX];
    for (int f2 = 0; f2 < 2; f2++) {
        char *t = f2 ? rd_applied_read(NULL) : rd_installed_read(NULL);
        slot_ids(t, ids); free(t);
        for (int i = 0; i < RING_SLOTS_MAX; i++) if (ids[i] > 0 && ids[i] <= RD_MAX) used[ids[i]] = 1;
    }
    for (int k = 1; k <= RD_PACKS_MAX; k++) {           /* and none a pack names (one a deletion missed) */
        char pp[320]; pack_path(pp, sizeof pp, k);
        char *t = read_file(pp, RD_PACK_MAX, NULL);
        for (char *l = t; l && *l; ) {
            char *e = strchr(l, '\n'), sl[32]; int did;
            if ((!strncmp(l, "use ", 4) && sscanf(l, "use %31s %d", sl, &did) == 2) || (!strncmp(l, "has ", 4) && sscanf(l, "has %d", &did) == 1))
                if (did > 0 && did <= RD_MAX) used[did] = 1;
            l = e ? e + 1 : NULL;
        }
        free(t);
    }
    /* saved again under its id only while it is there: one deleted meanwhile (another browser) is not made again
     * under a number a pack or the installed set may still name; the page saves a new one as id 0 */
    int again = id;
    if (!id) for (int k = 1; k <= RD_MAX && !id; k++) if (!design_exists(k) && !used[k]) id = k;
    if (!id) for (int k = 1; k <= RD_MAX && !id; k++) if (!design_exists(k)) id = k;
    if (again && !design_exists(again)) fail(err, cap, 0, "no such design (deleted meanwhile? save it as a new one)");
    else if (!id) fail(err, cap, 0, "the library is full: 100 designs");
    else if (library_bytes(id) + strlen(f) > RD_LIBRARY_MAX) fail(err, cap, 0, "the library is full: 4 MB of designs");
    else {
        spath(p, sizeof p, "ring"); mkdir(p, 0700);
        design_path(p, sizeof p, id);
        if (statefile_write(p, 0600, f, strlen(f))) snprintf(err, cap, "cannot write the design: %s", strerror(errno));
        else { rc = 0; *saved = id; }
    }
    pthread_mutex_unlock(&lock);
    free(f);
    return rc;
}

int rd_delete(int id, char *err, size_t cap)
{
    char p[320];
    if (id < 1 || id > RD_MAX) return fail(err, cap, 0, "no such design");
    pthread_mutex_lock(&lock);
    if (!loaded) load_assign();
    design_path(p, sizeof p, id);
    int rc = unlink(p) && errno != ENOENT ? -1 : 0, changed = 0;
    if (rc) snprintf(err, cap, "cannot delete it: %s", strerror(errno));
    for (int i = 0; i < ring_nslots && !rc; i++) if (assign[i] == id) { assign[i] = 0; changed = 1; }
    if (!rc) packs_forget(id);
    if (changed && save_assign(assign)) { rc = -1; snprintf(err, cap, "cannot write the assignments"); }
    pthread_mutex_unlock(&lock);
    return rc;
}

int rd_assign(const char *body, char *err, size_t cap)
{
    int want[RING_SLOTS_MAX], rc = 0;
    pthread_mutex_lock(&lock);
    if (!loaded) load_assign();
    memcpy(want, assign, sizeof want);
    for (const char *l = body; l && *l && !rc; ) {
        const char *e = strchr(l, '\n'); char line[80], id[32]; int did;
        snprintf(line, sizeof line, "%.*s", (int)(e ? (size_t)(e - l) : strlen(l)), l);
        l = e ? e + 1 : NULL;
        if (!line[0]) continue;
        int i = sscanf(line, "%31s %d", id, &did) == 2 ? slot_index(id) : -1;
        if (i < 0 || did < 0 || did > RD_MAX) rc = fail(err, cap, 0, "bad request");
        else if (did && !design_exists(did)) rc = fail(err, cap, 0, "no such design");
        else if (did && !ring_slot_available(i)) rc = fail(err, cap, 0, "this Echo's ring has no such animation");
        else want[i] = did;
    }
    /* the file first: refused, the page shows what is still drafted (assign), not what was asked for */
    if (!rc && save_assign(want)) rc = fail(err, cap, 0, "cannot write the assignments");
    if (!rc) memcpy(assign, want, sizeof want);
    pthread_mutex_unlock(&lock);
    return rc;
}

/* ---------------------------------------------------------------- theme packs */

#define PACK_HEAD "hassmic ring pack 2\n"

static void pack_path(char *out, size_t n, int id) { char rel[40]; snprintf(rel, sizeof rel, "ring/packs/%d", id); spath(out, n, rel); }

/* text of a field: no control characters, at most max bytes */
static int field_ok(const char *v, size_t len, size_t max)
{
    if (len > max) return 0;
    for (size_t i = 0; i < len; i++) if ((unsigned char)v[i] < 0x20 || v[i] == 0x7f) return 0;
    return 1;
}

/* lock held (its designs must be in the library: rd_delete takes them out under it).  A pack as the Echo keeps it
 * (ringdesign.h, version 2): checked, and written again line by line, has and use lines as "has %d" / "use %s %d":
 * packs_forget and the clash check below find a design's id only spelt so ("has 012" would stay naming a deleted
 * design).  Its animations show only its own designs.  has[]: its designs (RD_MAX + 1, 1 = in it).  malloc'd, or
 * NULL and why in err */
static char *pack_normal(const char *t, size_t n, char *has, char *err, size_t cap)
{
    struct sb b = { 0 }; char name[RD_NAME] = "", line[256], used[RING_SLOTS_MAX * 2][32];
    int nuse = 0, lineno = 1, uses_of[RING_SLOTS_MAX * 2];
    memset(has, 0, RD_MAX + 1);
    if (n < sizeof PACK_HEAD - 1 || memcmp(t, PACK_HEAD, sizeof PACK_HEAD - 1)) { fail(err, cap, 1, "not a theme pack as the Echo keeps it"); return NULL; }
    sput(&b, "%s", PACK_HEAD);
    for (size_t at = sizeof PACK_HEAD - 1; at < n; ) {
        const char *l = t + at, *nl = memchr(l, '\n', n - at);
        size_t len = nl ? (size_t)(nl - l) : n - at;
        at += len + (nl != NULL); lineno++;
        if (len && l[len - 1] == '\r') len--;
        if (!len) continue;
        if (len >= sizeof line) { fail(err, cap, lineno, "line too long"); goto bad; }
        memcpy(line, l, len); line[len] = 0;
        if (!strncmp(line, "name ", 5) || !strncmp(line, "author ", 7)) {
            int isname = line[0] == 'n'; const char *v = line + (isname ? 5 : 7);
            if (!field_ok(v, strlen(v), RD_NAME - 1)) { fail(err, cap, lineno, isname ? "bad name" : "bad author"); goto bad; }
            if (isname) snprintf(name, sizeof name, "%s", v);
        }
        else if (!strncmp(line, "about ", 6)) {
            if (!field_ok(line + 6, strlen(line + 6), 199)) { fail(err, cap, lineno, "bad about"); goto bad; }
        }
        else if (!strncmp(line, "shot ", 5)) {
            size_t k = strlen(line + 5);
            if (!k || k > 15 || strspn(line + 5, "abcdefghijklmnopqrstuvwxyz") != k) { fail(err, cap, lineno, "bad shot"); goto bad; }
        }
        else if (!strncmp(line, "has ", 4)) {
            char *e; long v = strtol(line + 4, &e, 10);
            if (*e || e == line + 4 || v < 1 || v > RD_MAX) { fail(err, cap, lineno, "has <design id>"); goto bad; }
            if (has[v]) { fail(err, cap, lineno, "a design in it twice"); goto bad; }
            if (!design_exists((int)v)) { snprintf(err, cap, "no design %ld in the library", v); goto bad; }
            has[v] = 1;
            sput(&b, "has %ld\n", v);
            continue;
        }
        else if (!strncmp(line, "use ", 4)) {
            char slot[32], what[16]; int k = 0;
            if (sscanf(line, "use %31s %15s %n", slot, what, &k) != 2 || line[k] || strspn(slot, "abcdefghijklmnopqrstuvwxyz_") != strlen(slot)) {
                fail(err, cap, lineno, "use <animation> <design id> | stock"); goto bad;
            }
            for (int i = 0; i < nuse; i++) if (!strcmp(used[i], slot)) { fail(err, cap, lineno, "an animation used twice"); goto bad; }
            if (nuse >= (int)(sizeof used / sizeof used[0])) { fail(err, cap, lineno, "too many animations"); goto bad; }
            uses_of[nuse] = 0;
            snprintf(used[nuse++], sizeof used[0], "%s", slot);
            if (strcmp(what, "stock")) {
                char *e; long v = strtol(what, &e, 10);
                if (*e || v < 1 || v > RD_MAX) { fail(err, cap, lineno, "bad design id"); goto bad; }
                uses_of[nuse - 1] = (int)v;
            }
            if (uses_of[nuse - 1]) sput(&b, "use %s %d\n", slot, uses_of[nuse - 1]); else sput(&b, "use %s stock\n", slot);
            continue;
        }
        else { fail(err, cap, lineno, "not a pack's line"); goto bad; }
        sput(&b, "%s\n", line);
    }
    if (!name[0]) { fail(err, cap, 0, "a pack needs a name"); goto bad; }
    for (int i = 0; i < nuse; i++)              /* a pack's animations show its own designs: a folder of its own */
        if (uses_of[i] && !has[uses_of[i]]) { snprintf(err, cap, "%s: design %d is not in the pack", used[i], uses_of[i]); goto bad; }
    if (b.bad) { free(b.p); fail(err, cap, 0, "no memory"); return NULL; }
    if (b.n > RD_PACK_MAX) { free(b.p); fail(err, cap, 0, "the pack is too large"); return NULL; }
    return b.p;
bad:
    free(b.p);
    return NULL;
}

/* lock held: a design deleted: no pack names it any more (its animations are no longer in those packs) */
static void packs_forget(int id)
{
    char p[320], want[16];
    snprintf(want, sizeof want, " %d", id);
    for (int k = 1; k <= RD_PACKS_MAX; k++) {
        size_t n; pack_path(p, sizeof p, k);
        char *t = read_file(p, RD_PACK_MAX, &n);
        if (!t || strncmp(t, PACK_HEAD, sizeof PACK_HEAD - 1)) { free(t); continue; }
        struct sb b = { 0 }; int changed = 0;
        for (char *l = t; *l; ) {
            char *e = strchr(l, '\n'); size_t len = e ? (size_t)(e - l) : strlen(l), wl = strlen(want);
            if ((!strncmp(l, "use ", 4) && len > wl && !memcmp(l + len - wl, want, wl) && l[len - wl - 1] != ' ' && memchr(l + 4, ' ', len - 4) == l + len - wl)
                || (!strncmp(l, "has", 3) && len == 3 + wl && !memcmp(l + 3, want, wl)))
                changed = 1;
            else sput(&b, "%.*s\n", (int)len, l);
            if (!e) break;
            l = e + 1;
        }
        if (changed && !b.bad) statefile_write(p, 0600, b.p, b.n);
        free(b.p); free(t);
    }
}


char *rd_packs_text(size_t *n)
{
    struct sb b = { 0 }; char p[320];
    pthread_mutex_lock(&lock);
    for (int id = 1; id <= RD_PACKS_MAX; id++) {
        pack_path(p, sizeof p, id);
        char *t = read_file(p, 1 << 20, NULL);      /* a version 1 pack (designs inside) is larger: the page converts it */
        if (t) sput(&b, "pack %d\n%s\n", id, t);
        free(t);
    }
    pthread_mutex_unlock(&lock);
    if (b.bad) { free(b.p); return NULL; }
    if (!b.p) sput(&b, "%s", "");
    *n = b.n;
    return b.p;
}

int rd_pack_save(int id, const char *text, size_t n, int *saved, char *err, size_t cap)
{
    char p[320], has[RD_MAX + 1]; struct stat st;
    if (id < 0 || id > RD_PACKS_MAX) return fail(err, cap, 0, "no such pack");
    pthread_mutex_lock(&lock);
    char *f = pack_normal(text, n, has, err, cap);
    if (!f) { pthread_mutex_unlock(&lock); return -1; }
    int rc = -1, clash = 0; char clash_name[RD_NAME] = "";
    if (!id) for (int k = 1; k <= RD_PACKS_MAX && !id; k++) { pack_path(p, sizeof p, k); if (lstat(p, &st)) id = k; }
    /* a design is in one pack at most: a folder */
    for (int k = 1; k <= RD_PACKS_MAX && id && !clash; k++) {
        if (k == id) continue;
        pack_path(p, sizeof p, k);
        char *t = read_file(p, RD_PACK_MAX, NULL);
        for (char *l = t; l && *l && !clash; ) {
            char *e = strchr(l, '\n'); int did;
            if (!strncmp(l, "name ", 5)) snprintf(clash_name, sizeof clash_name, "%.*s", (int)(e ? e - l - 5 : (long)strlen(l + 5)), l + 5);
            else if (!strncmp(l, "has ", 4) && sscanf(l, "has %d", &did) == 1 && did > 0 && did <= RD_MAX && has[did]) clash = did;
            l = e ? e + 1 : NULL;
        }
        free(t);
    }
    if (!id) fail(err, cap, 0, "50 packs at most");
    else if (clash) snprintf(err, cap, "design %d is in the pack \"%s\" already", clash, clash_name);
    else {
        spath(p, sizeof p, "ring"); mkdir(p, 0700);
        spath(p, sizeof p, "ring/packs"); mkdir(p, 0700);
        pack_path(p, sizeof p, id);
        if (statefile_write(p, 0600, f, strlen(f))) snprintf(err, cap, "cannot write the pack: %s", strerror(errno));
        else { rc = 0; *saved = id; }
    }
    pthread_mutex_unlock(&lock);
    free(f);
    return rc;
}

int rd_pack_delete(int id, char *err, size_t cap)
{
    char p[320];
    if (id < 1 || id > RD_PACKS_MAX) return fail(err, cap, 0, "no such pack");
    pthread_mutex_lock(&lock);
    pack_path(p, sizeof p, id);
    int rc = unlink(p) && errno != ENOENT ? -1 : 0;
    if (rc) snprintf(err, cap, "cannot delete it: %s", strerror(errno));
    pthread_mutex_unlock(&lock);
    return rc;
}

/* lock held: what root is asked to install goes back to what is installed (a draft never stays there: root would try it
 * again at every start, and status and start compare against it) */
static void put_back(const char *why)
{
    char p[320]; size_t n = 0; char *t = rd_installed_read(&n);
    spath(p, sizeof p, "ring-applied");
    if (t) statefile_write(p, 0600, t, n); else unlink(p);
    free(t);
    if (why) { snprintf(last_error, sizeof last_error, "%s", why); fprintf(stderr, "ring: designs not installed: %s\n", why); }
    waiting = 0;
}

int rd_apply(char *err, size_t cap)
{
    char p[320], req[32]; size_t dn = 0; int rc = -1;
    if (!ring_custom()) return fail(err, cap, 0, "the ring's own animations are not installed (see the log)");
    pthread_mutex_lock(&lock);
    if (!loaded) load_assign();
    char *draft = waiting ? NULL : draft_applied(&dn, err, cap);
    if (waiting) fail(err, cap, 0, "the ring is being installed already");
    else if (!draft) rc = -1;           /* err: which slot's design, and why */
    else if (!ring_check(draft, dn, err, cap)) {
        spath(p, sizeof p, "ring-applied");
        wait_id = (unsigned)time(NULL) ^ (unsigned)getpid() << 16;
        snprintf(req, sizeof req, "%u\n", wait_id);
        if (statefile_write(p, 0600, draft, dn)) { snprintf(err, cap, "cannot write the designs: %s", strerror(errno)); put_back(NULL); }
        else {
            spath(p, sizeof p, "leds-request");
            if (statefile_write(p, 0600, req, strlen(req))) { snprintf(err, cap, "cannot ask root: %s", strerror(errno)); put_back(NULL); }
            else { waiting = 1; wait_since = mono_s(); last_error[0] = 0; rc = 0; fprintf(stderr, "ring: designs applied, root installs them (%u)\n", wait_id); }
        }
    }
    pthread_mutex_unlock(&lock);
    free(draft);
    return rc;
}

/* At a start, after ring_start (which confirmed what is installed): a request from before it is still root's to answer;
 * else designs asked for and not installed (a boot's install failed, or hassmic stopped in between) go back */
void rd_start(void)
{
    char p[320]; size_t an = 0, in = 0;
    pthread_mutex_lock(&lock);
    spath(p, sizeof p, "leds-request");
    char *req = read_file(p, 64, NULL), *applied = rd_applied_read(&an), *inst = rd_installed_read(&in);
    if (req && sscanf(req, "%u", &wait_id) == 1) { waiting = 1; wait_since = mono_s(); fprintf(stderr, "ring: designs asked for before this start, root installs them\n"); }
    else if (applied && !(inst && an == in && !memcmp(applied, inst, an)) && !(!inst && an == sizeof APPLIED_HEAD - 1))
        put_back("the designs applied before were not installed (the log says why: \"light ring\")");
    pthread_mutex_unlock(&lock);
    free(req); free(applied); free(inst);
}

void rd_tick(void)
{
    int reload = 0;
    pthread_mutex_lock(&lock);
    if (waiting) {
        char *t = read_file(result_path(), 4096, NULL), word[16] = "", p[320]; unsigned id = 0; int k = 0;
        if (t && sscanf(t, "%u %15s %n", &id, word, &k) >= 2 && id == wait_id) {
            if (!strcmp(word, "ok")) { waiting = 0; reload = 1; }
            else if (!strcmp(word, "same")) {       /* nothing to install: what was asked for is what is there */
                size_t an = 0; char *a = rd_applied_read(&an);
                if (a) rd_installed_set(a, an);
                free(a); waiting = 0;
            }
            else {
                char why[160]; snprintf(why, sizeof why, "%s", k ? t + k : "root could not install them");
                for (char *c = why; *c; c++) if ((unsigned char)*c < 0x20) *c = *c == '\n' && !c[1] ? 0 : ' ';
                put_back(why[0] ? why : "root could not install them");
            }
        }
        else {
            /* No answer.  The request still there: root never took it (older scripts, or none running), and taking it
             * back is safe.  Taken: root is at it (an install takes seconds); only much later is it gone for good */
            struct stat st; spath(p, sizeof p, "leds-request");
            long long waited = mono_s() - wait_since;
            int asked = !lstat(p, &st);
            if (asked && waited > WAIT_ASK_S) { unlink(p); put_back("root did not take the request (its scripts are older than this hassmic: restart the Echo)"); }
            else if (!asked && waited > WAIT_MAX_S) put_back("root did not answer");
        }
        free(t);
    }
    pthread_mutex_unlock(&lock);
    if (reload) ring_reload();
}

char *rd_stock(const char *slot, size_t *n)
{
    int i = slot_index(slot); char file[512];
    const char *r = getenv("HASSMIC_LED_ROOT");
    if (i < 0) return NULL;
    if (ring_slots[i].kind == RING_VOLUME || ring_slots[i].kind == RING_ASSIST) {   /* every step, "step NN" before each */
        struct sb b = { 0 };
        for (int st = 1; st <= 99; st++) {
            snprintf(file, sizeof file, "%s/led-resources/volume_step-%02d.animation", r ? r : "/system/etc", st);
            char *t = read_file(file, 1 << 20, NULL);
            if (t) sput(&b, "step %02d\n%s%s", st, t, t[0] && t[strlen(t) - 1] != '\n' ? "\n" : "");
            free(t);
        }
        if (b.bad || !b.p) { free(b.p); return NULL; }
        *n = b.n;
        return b.p;
    }
    snprintf(file, sizeof file, "%s/led-resources/%s.animation", r ? r : "/system/etc", ring_slots[i].stock);
    return read_file(file, 1 << 20, n);
}
