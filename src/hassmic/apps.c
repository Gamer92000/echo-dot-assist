/* Apps of the owner's: which may reach the internet (apps.h).
 *
 * The apps come from PackageManager's own record, /data/system/packages.xml (system:system 0660 on Android 7; hassmic
 * runs as system on the Shows, and it is plain XML there: the binary form came with Android 12).  An app of the
 * owner's is a <package> whose codePath is in /data/app, whose publicFlags lack FLAG_SYSTEM (1: an update of a system
 * app lives in /data/app too) and that has a userId of its own from 10000 on (an app sharing a uid has sharedUserId
 * instead, and system's 1000 is hassmic's).  Root's firewall (lockdown.sh app_uids) applies the same rule again: what
 * hassmic writes is only a wish.
 *
 * state/apps: "net <package>" (may reach the internet) and "lan <package>" (may be reached from the home network: root's
 * firewall admits the ports that app listens on, from private addresses; issue #109) lines, at most APPS_MAX of each.
 *
 * Installing and removing are root's (scripts/device/apps.sh, run by main.sh's ota_watch): hassmic stages the APK in
 * state/apps-staged.apk, checks it whole, and writes state/apps-request ("<id> install <size>" or "<id> uninstall
 * <package>"); root answers in /data/local/hassmic/apps/result ("<id> ok <package>" or "<id> failed <why>"). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "apps.h"
#include "statefile.h"
#include "../third_party/monocypher.h"
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APPS_MAX 32
#define PKG_MAX  150
#define XML_MAX  (4 << 20)              /* the Show 5's is 320 kB */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void state_file(char *out, size_t cap, const char *name)
{
    const char *e = getenv("HASSMIC_STATE");
    snprintf(out, cap, "%s/%s", e ? e : "/data/local/hassmic/state", name);
}
static void state_path(char *out, size_t cap) { state_file(out, cap, "apps"); }
static void result_path(char *out, size_t cap)
{
    const char *e = getenv("HASSMIC_APPS_RESULT");
    snprintf(out, cap, "%s", e ? e : "/data/local/hassmic/apps/result");
}

static long staged = -1;                /* bytes in state/apps-staged.apk; -1: not looked yet */
static unsigned long req_id;            /* the last request of this run; 0: none */

/* letters, digits, _ and dots, no empty part: what Android takes, and nothing a shell or JSON would read otherwise */
static int is_pkg(const char *s, size_t n)
{
    if (!n || n > PKG_MAX || s[0] == '.' || s[n - 1] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return 0;
        if (c == '.' && s[i + 1] == '.') return 0;
    }
    return 1;
}

static char *slurp(const char *p, size_t max, size_t *len)
{
    FILE *f = fopen(p, "r"); if (!f) return NULL;
    char *b = malloc(max + 1); size_t n = 0;
    if (b) { n = fread(b, 1, max, f); b[n] = 0; }
    fclose(f);
    if (len) *len = n;
    return b;
}

/* the value of attribute a in the tag [t, end), copied into out; 0 when it has none (or it does not fit) */
static int attr(const char *t, const char *end, const char *a, char *out, size_t cap)
{
    char k[40]; snprintf(k, sizeof k, " %s=\"", a);
    size_t kl = strlen(k);
    for (const char *p = t; p + kl < end; p++) if (!memcmp(p, k, kl)) {
        const char *v = p + kl, *q = memchr(v, '"', (size_t)(end - v));
        if (!q || (size_t)(q - v) >= cap) return 0;
        memcpy(out, v, (size_t)(q - v)); out[q - v] = 0;
        return 1;
    }
    return 0;
}

/* calls fn for every app of the owner's in packages.xml (with its versionCode); -1 without the file */
static int each_app(void (*fn)(const char *pkg, long version, void *ctx), void *ctx)
{
    const char *e = getenv("HASSMIC_PACKAGES_XML");
    size_t n; char *x = slurp(e ? e : "/data/system/packages.xml", XML_MAX, &n);
    if (!x) return -1;
    static const char tag[] = "<package ";
    for (char *p = x; (p = strstr(p, tag)); p += sizeof tag - 1) {
        char *end = strchr(p, '>'); if (!end) break;
        char name[PKG_MAX + 2], code[300], flags[24], uid[16];
        if (!attr(p, end, "name", name, sizeof name) || !is_pkg(name, strlen(name))) continue;
        if (!attr(p, end, "codePath", code, sizeof code) || strncmp(code, "/data/app/", 10)) continue;
        if (!attr(p, end, "publicFlags", flags, sizeof flags) || (atol(flags) & 1)) continue;   /* FLAG_SYSTEM */
        if (!attr(p, end, "userId", uid, sizeof uid) || atol(uid) < 10000 || atol(uid) > 19999) continue;
        char ver[24] = "0"; attr(p, end, "version", ver, sizeof ver);
        fn(name, atol(ver), ctx);
    }
    free(x);
    return 0;
}

/* ---------------------------------------------------------------- installing and removing (root's apps.sh) */

static int request_pending(void)
{
    char p[300]; state_file(p, sizeof p, "apps-request");
    return access(p, F_OK) == 0;
}

static long staged_size(void)
{
    if (staged < 0) {
        char p[300]; struct stat st; state_file(p, sizeof p, "apps-staged.apk");
        staged = stat(p, &st) == 0 ? (long)st.st_size : 0;
    }
    return staged;
}

/* the result of the last request: root's answer, or still running.  Lock held */
static void install_json(char *o, size_t cap)
{
    char p[300], line[300] = ""; result_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    line[strcspn(line, "\n")] = 0;
    unsigned long id = strtoul(line, NULL, 10);
    char *w = strchr(line, ' '), *rest = w ? strchr(w + 1, ' ') : NULL;
    if (w) *w++ = 0;
    if (rest) *rest++ = 0;
    /* what root wrote is printable (apps.sh: tr -cd); quotes and backslashes cannot be in it */
    for (char *c = rest; c && *c; c++) if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) *c = ' ';
    const char *state = request_pending() || (req_id && id != req_id) ? "installing"
        : w && !strcmp(w, "ok") ? "ok" : w && !strcmp(w, "failed") ? "failed" : staged_size() > 0 ? "receiving" : "idle";
    int ok = !strcmp(state, "ok"), failed = !strcmp(state, "failed");
    snprintf(o, cap, "{\"state\":\"%s\",\"package\":\"%s\",\"error\":\"%s\",\"have\":%ld}", state,
             ok && rest && strcmp(rest, "-") ? rest : "", failed && rest ? rest : "", staged_size());
}

/* the two lists of state/apps: [0] the internet ("net"), [1] the home network ("lan") */
static const char *const kinds[2] = { "net", "lan" };
struct allowed { char pkg[2][APPS_MAX][PKG_MAX + 1]; int n[2]; };

static void load(struct allowed *a)
{
    char p[300]; state_path(p, sizeof p);
    a->n[0] = a->n[1] = 0;
    char *t = slurp(p, 16384, NULL); if (!t) return;
    for (char *l = t, *nl; l && *l; l = nl) {
        if ((nl = strchr(l, '\n'))) *nl++ = 0;
        for (int k = 0; k < 2; k++)
            if (!strncmp(l, kinds[k], 3) && l[3] == ' ' && is_pkg(l + 4, strlen(l + 4)) && a->n[k] < APPS_MAX)
                snprintf(a->pkg[k][a->n[k]++], PKG_MAX + 1, "%s", l + 4);
    }
    free(t);
}

static int allowed_has(const struct allowed *a, int k, const char *pkg)
{
    for (int i = 0; i < a->n[k]; i++) if (!strcmp(a->pkg[k][i], pkg)) return 1;
    return 0;
}

struct out { char *o; size_t n, cap; int count; const struct allowed *a; };
static void add_json(const char *pkg, long version, void *ctx)
{
    struct out *o = ctx;
    if (!o->o || o->n + PKG_MAX + 64 > o->cap) {
        char *b = realloc(o->o, o->cap * 2); if (!b) return;
        o->o = b; o->cap *= 2;
    }
    o->n += (size_t)snprintf(o->o + o->n, o->cap - o->n, "%s{\"package\":\"%s\",\"version\":%ld,\"net\":%s,\"lan\":%s}", o->count++ ? "," : "", pkg,
                             version, allowed_has(o->a, 0, pkg) ? "true" : "false", allowed_has(o->a, 1, pkg) ? "true" : "false");
}

char *apps_json(size_t *len)
{
    static struct allowed a;
    pthread_mutex_lock(&lock);
    load(&a);
    struct out o = { malloc(4096), 0, 4096, 0, &a };
    if (o.o) o.n = (size_t)snprintf(o.o, o.cap, "{\"apps\":[");
    int r = o.o ? each_app(add_json, &o) : -1;
    char st[400]; install_json(st, sizeof st);
    pthread_mutex_unlock(&lock);
    if (r < 0 || !o.o) { free(o.o); return NULL; }
    size_t need = o.n + strlen(st) + 16;
    if (need > o.cap) { char *b = realloc(o.o, need); if (!b) { free(o.o); return NULL; } o.o = b; o.cap = need; }
    o.n += (size_t)snprintf(o.o + o.n, o.cap - o.n, "],\"install\":%s}", st);
    if (len) *len = o.n;
    return o.o;
}

struct find { const char *pkg; int found; };
static void find_one(const char *pkg, long version, void *ctx) { (void)version; struct find *f = ctx; if (!strcmp(pkg, f->pkg)) f->found = 1; }

static int set_kind(int k, const char *pkg, int on)
{
    if (!is_pkg(pkg, strlen(pkg))) return -1;
    static struct allowed a;
    pthread_mutex_lock(&lock);
    load(&a);
    struct find f = { pkg, 0 };
    /* only an installed app of the owner's is added; one gone may still be taken off */
    if (on && (each_app(find_one, &f) < 0 || !f.found)) { pthread_mutex_unlock(&lock); return -1; }
    int i = 0;
    for (; i < a.n[k] && strcmp(a.pkg[k][i], pkg); i++) {}
    if (on && i == a.n[k]) { if (a.n[k] >= APPS_MAX) { pthread_mutex_unlock(&lock); return -2; } snprintf(a.pkg[k][a.n[k]++], PKG_MAX + 1, "%s", pkg); }
    if (!on && i < a.n[k]) { memmove(a.pkg[k][i], a.pkg[k][i + 1], (size_t)(a.n[k] - i - 1) * sizeof a.pkg[k][0]); a.n[k]--; }
    char t[2 * APPS_MAX * (PKG_MAX + 6) + 1]; size_t n = 0;
    for (int j = 0; j < 2; j++) for (int m = 0; m < a.n[j]; m++) n += (size_t)snprintf(t + n, sizeof t - n, "%s %s\n", kinds[j], a.pkg[j][m]);
    char p[300]; state_path(p, sizeof p);
    int bad = statefile_write(p, 0644, t, n);           /* root reads it; nothing secret */
    pthread_mutex_unlock(&lock);
    if (!bad) fprintf(stderr, "apps: %s %s\n", pkg, k == 0 ? (on ? "may reach the internet" : "kept off the internet")
                                                       : (on ? "reachable from the home network" : "not reachable from the home network"));
    return bad ? -2 : 0;
}

int apps_set_net(const char *pkg, int on) { return set_kind(0, pkg, on); }
int apps_set_lan(const char *pkg, int on) { return set_kind(1, pkg, on); }

int apps_chunk(long off, const void *data, size_t n)
{
    char p[300]; state_file(p, sizeof p, "apps-staged.apk");
    pthread_mutex_lock(&lock);
    int r = 0;
    if (off < 0 || (off && off != staged_size())) r = -1;
    else if (off + (long)n > APPS_APK_MAX) r = -2;
    else {
        /* O_NOFOLLOW: a link put there would not be written through */
        int fd = open(p, O_WRONLY | O_CREAT | O_NOFOLLOW | (off ? O_APPEND : O_TRUNC), 0600);
        if (fd < 0) r = -3;
        else {
            const char *b = data; size_t k = 0;
            while (k < n) { ssize_t w = write(fd, b + k, n - k); if (w <= 0) break; k += (size_t)w; }
            if (k < n) r = -3;
            close(fd);
            struct stat st; staged = stat(p, &st) == 0 ? (long)st.st_size : 0;
            if (!off && !r) fprintf(stderr, "apps: an APK coming in\n");
        }
    }
    pthread_mutex_unlock(&lock);
    return r;
}

static int request(const char *what)
{
    char p[300], t[220]; state_file(p, sizeof p, "apps-request");
    unsigned long id = (unsigned long)time(NULL);       /* unique enough: one at a time, and never twice a second */
    if (id <= req_id) id = req_id + 1;
    int n = snprintf(t, sizeof t, "%lu %s\n", id, what);
    if (statefile_write(p, 0644, t, (size_t)n)) return -3;
    req_id = id;
    return 0;
}

int apps_install(long size, const char *hex)
{
    uint8_t want[32], got[32];
    if (strlen(hex) != 64) return -1;
    for (int i = 0; i < 32; i++) { unsigned v; if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1; want[i] = (uint8_t)v; }
    pthread_mutex_lock(&lock);
    if (request_pending()) { pthread_mutex_unlock(&lock); return -2; }
    if (size <= 0 || size != staged_size()) { pthread_mutex_unlock(&lock); return -1; }
    char p[300]; state_file(p, sizeof p, "apps-staged.apk");
    int fd = open(p, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { pthread_mutex_unlock(&lock); return -1; }
    crypto_blake2b_ctx ctx; static uint8_t buf[65536]; ssize_t k; long left = size;
    crypto_blake2b_init(&ctx, 32);
    while (left > 0 && (k = read(fd, buf, sizeof buf)) > 0) { crypto_blake2b_update(&ctx, buf, (size_t)k); left -= k; }
    crypto_blake2b_final(&ctx, got);
    fsync(fd); close(fd);
    if (left || crypto_verify32(want, got)) { pthread_mutex_unlock(&lock); return -1; }
    char what[40]; snprintf(what, sizeof what, "install %ld", size);
    int r = request(what);
    pthread_mutex_unlock(&lock);
    if (!r) fprintf(stderr, "apps: an APK of %ld bytes handed to root to install\n", size);
    return r;
}

int apps_uninstall(const char *pkg)
{
    if (!is_pkg(pkg, strlen(pkg))) return -1;
    pthread_mutex_lock(&lock);
    struct find f = { pkg, 0 };
    if (each_app(find_one, &f) < 0 || !f.found) { pthread_mutex_unlock(&lock); return -1; }
    if (request_pending()) { pthread_mutex_unlock(&lock); return -2; }
    char what[PKG_MAX + 20]; snprintf(what, sizeof what, "uninstall %s", pkg);
    int r = request(what);
    pthread_mutex_unlock(&lock);
    if (!r) fprintf(stderr, "apps: %s handed to root to remove\n", pkg);
    return r;
}
