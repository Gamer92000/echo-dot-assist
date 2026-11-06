/* The home app's side of the Echo Show's screen (kiosk.h, docs/screen-apps.md).
 *
 * state/kiosk, written by POST /api/kiosk (web.c, signed, the token sealed with the browser's K as a Wi-Fi password
 * is): lines of
 *
 *   ha <url>                 Home Assistant's address as the integration sees it (get_url: the one the LAN reaches)
 *   token <token>            a long-lived access token of the Echo's own user, made by the integration
 *   screen <name>\t<url>     up to KIOSK_SCREENS: the home app's screens (the drawer offers their names)
 *   default <i>              the screen it starts on and goes back to
 *   return <s>               after this long untouched on another screen, back to the default (0: never)
 *   set <key> <value>        a setting of the app's own (its Immich address and key, ...), for it alone: only in the
 *                            app's config on loopback, never in the signed API's answer
 *   app <package>            the home app as Echo Local names it: made the Screen page's choice too (screen.c), so
 *                            that there is one home app whichever side picked it last
 *
 * The app reads it as JSON from 127.0.0.1 (web.c), and only the home app: the connection's owner (its uid in
 * /proc/net/tcp6 or tcp) owns the home app's data folder, as anyone may install apps on the Echo and the token is a
 * Home Assistant login. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kiosk.h"
#include "alarms.h"
#include "screen.h"
#include "statefile.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define KIOSK_SCREENS 8
#define URL_MAX       400
#define NAME_MAX_     40
#define TOKEN_MAX     600
#define APP_MAX       150
#define SETS 16
#define SET_KEY 40
#define SET_VAL 400
#define FRONT_MS      12000             /* the app polls /front every 5 s while resumed: its claims hold this long */
#define WORDS_MS      20000             /* a request's words stay this long after it ended */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
struct screen { char name[NAME_MAX_ + 1], url[URL_MAX + 1]; };
static struct screen screens[KIOSK_SCREENS];
static int nscreens, def, ret_s = 120;
static char ha[URL_MAX + 1], token[TOKEN_MAX + 1];
struct set { char key[SET_KEY + 1], val[SET_VAL + 1]; };
static struct set sets[SETS];
static int nsets;
static long long front_at; static int draws;

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static void path(char *out, size_t cap)
{
    const char *e = getenv("HASSMIC_STATE");
    snprintf(out, cap, "%s/kiosk", e ? e : "/data/local/hassmic/state");
}

/* printable, no quotes or backslashes to escape for anyone: what a URL, a name and a token are made of */
static int clean(const char *s, size_t max, int tabs)
{
    size_t n = strlen(s);
    if (n > max) return 0;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++)
        if (*c < 0x20 || *c == 0x7f || *c == '"' || *c == '\\' || (!tabs && *c == '\t')) return 0;
    return 1;
}
static int is_url(const char *u) { return (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) && !strchr(u, ' ') && clean(u, URL_MAX, 0); }
/* a screen the app draws itself: "hassmic:clock" (docs/screen-apps.md), lower-case letters and dashes after the colon */
static int is_own(const char *u)
{
    if (strncmp(u, "hassmic:", 8) || !u[8] || strlen(u) > 40) return 0;
    for (const char *c = u + 8; *c; c++) if (!((*c >= 'a' && *c <= 'z') || *c == '-')) return 0;
    return 1;
}

/* a package name as Android takes it: letters, digits, _ and dots */
static int is_pkg(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > APP_MAX || s[0] == '.' || strstr(s, "..")) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '.')) return 0;
    return 1;
}

static int is_key(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > SET_KEY) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '.' || *s == '-')) return 0;
    return 1;
}

int kiosk_draws_parse(const char *t)
{
    if (strncmp(t, "/draws", 6)) return 0;
    if (!t[6]) return KIOSK_DRAWS_WORDS | KIOSK_DRAWS_RING;   /* /front/draws: both, the contract of before */
    if (t[6] != '/') return 0;
    int m = 0; const char *w = t + 7;
    while (*w) {
        size_t n = strcspn(w, ",+");
        if (n == 5 && !strncmp(w, "words", 5)) m |= KIOSK_DRAWS_WORDS;
        else if (n == 4 && !strncmp(w, "ring", 4)) m |= KIOSK_DRAWS_RING;
        w += n; if (*w) w++;                                /* a word not known here (a later contract's): left out */
    }
    return m;
}

static int parse(const char *text, struct screen *sc, int *ns, int *d, int *r, char *h, char *t, char *ap, struct set *st, int *nst,
                 char *err, size_t cap)
{
    *ns = 0; *d = 0; *r = 120; h[0] = t[0] = 0; ap[0] = 0; *nst = 0;
    const char *p = text;
    while (p && *p) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[1100];
        if (n >= sizeof line) { snprintf(err, cap, "a line too long"); return -1; }
        memcpy(line, p, n); line[n] = 0; if (n && line[n - 1] == '\r') line[n - 1] = 0;
        p = e ? e + 1 : NULL;
        char *v = strchr(line, ' ');
        if (!line[0]) continue;
        if (!v) { snprintf(err, cap, "not \"key value\": %.40s", line); return -1; }
        *v++ = 0;
        if (!strcmp(line, "ha")) { if (!is_url(v)) { snprintf(err, cap, "ha: not an http(s) address"); return -1; } snprintf(h, URL_MAX + 1, "%s", v); }
        else if (!strcmp(line, "token")) { if (!clean(v, TOKEN_MAX, 0) || strchr(v, ' ')) { snprintf(err, cap, "token: not a token"); return -1; } snprintf(t, TOKEN_MAX + 1, "%s", v); }
        else if (!strcmp(line, "screen")) {
            char *tab = strchr(v, '\t');
            if (!tab) { snprintf(err, cap, "screen: \"name<tab>url\""); return -1; }
            *tab++ = 0;
            if (*ns >= KIOSK_SCREENS) { snprintf(err, cap, "more than %d screens", KIOSK_SCREENS); return -1; }
            if (!v[0] || !clean(v, NAME_MAX_, 0)) { snprintf(err, cap, "screen: a name of 1 to %d characters", NAME_MAX_); return -1; }
            if (!is_url(tab) && !is_own(tab)) { snprintf(err, cap, "screen %s: not an http(s) address", v); return -1; }
            snprintf(sc[*ns].name, sizeof sc[*ns].name, "%s", v); snprintf(sc[*ns].url, sizeof sc[*ns].url, "%s", tab); (*ns)++;
        }
        else if (!strcmp(line, "set")) {
            char *sp = strchr(v, ' ');
            if (!sp) { snprintf(err, cap, "set: \"key value\""); return -1; }
            *sp++ = 0;
            if (!is_key(v)) { snprintf(err, cap, "set: a key of a-z 0-9 . _ -, 40 at most"); return -1; }
            if (!clean(sp, SET_VAL, 0)) { snprintf(err, cap, "set %s: a value of printable characters, %d at most", v, SET_VAL); return -1; }
            if (*nst >= SETS) { snprintf(err, cap, "more than %d settings", SETS); return -1; }
            snprintf(st[*nst].key, sizeof st[*nst].key, "%s", v); snprintf(st[*nst].val, sizeof st[*nst].val, "%s", sp); (*nst)++;
        }
        else if (!strcmp(line, "app")) { if (!is_pkg(v)) { snprintf(err, cap, "app: not a package name"); return -1; } snprintf(ap, APP_MAX + 1, "%s", v); }
        else if (!strcmp(line, "default")) *d = atoi(v);
        else if (!strcmp(line, "return")) { *r = atoi(v); if (*r < 0 || *r > 86400) { snprintf(err, cap, "return: 0 to 86400 s"); return -1; } }
        /* other keys: a newer integration's; left out */
    }
    if (*d < 0 || *d >= (*ns ? *ns : 1)) *d = 0;
    return 0;
}

static void take(struct screen *sc, int ns, int d, int r, const char *h, const char *t, struct set *st, int nst)
{
    pthread_mutex_lock(&lock);
    memcpy(sets, st, sizeof sets); nsets = nst;
    memcpy(screens, sc, sizeof screens); nscreens = ns; def = d; ret_s = r;
    snprintf(ha, sizeof ha, "%s", h); snprintf(token, sizeof token, "%s", t);
    pthread_mutex_unlock(&lock);
    char names[KIOSK_SCREENS * (NAME_MAX_ + 1) + 1]; kiosk_screen_names(names, sizeof names);
    screen_screens(names);                                  /* the drawer's row: the home app's screens */
}

void kiosk_load(void)
{
    char p[300], err[100]; FILE *f; static char text[16384];
    path(p, sizeof p);
    if (!(f = fopen(p, "r"))) return;
    size_t n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
    static struct screen sc[KIOSK_SCREENS]; int ns, d, r, nst; char h[URL_MAX + 1], t[TOKEN_MAX + 1], ap[APP_MAX + 1]; static struct set st[SETS];
    if (parse(text, sc, &ns, &d, &r, h, t, ap, st, &nst, err, sizeof err)) { fprintf(stderr, "kiosk: state/kiosk refused (%s)\n", err); return; }
    take(sc, ns, d, r, h, t, st, nst);
    fprintf(stderr, "kiosk: %d screen%s%s\n", ns, ns == 1 ? "" : "s", t[0] ? ", with a login" : "");
    /* written by a build before the home app was the Screen page's: its "app" line (the screen app then) is the home
     * app where none was chosen since (state/screen_home) */
    char sh[300]; const char *e = getenv("HASSMIC_STATE");
    snprintf(sh, sizeof sh, "%s/screen_home", e ? e : "/data/local/hassmic/state");
    if (ap[0] && access(sh, F_OK)) { fprintf(stderr, "kiosk: %s, the screen app before, is the home app now\n", ap); screen_set_home(ap); }
}

int kiosk_store(const char *text, char *err, size_t cap)
{
    static struct screen sc[KIOSK_SCREENS]; int ns, d, r, nst; char h[URL_MAX + 1], t[TOKEN_MAX + 1], p[300], ap[APP_MAX + 1];
    static struct set st[SETS];
    if (parse(text, sc, &ns, &d, &r, h, t, ap, st, &nst, err, cap)) return -1;
    /* written back as parsed: only what was taken (the app is the Screen page's, kept there) */
    size_t cap_ = 16384, n = 0; char *o = malloc(cap_);
    if (!o) { snprintf(err, cap, "out of memory"); return -1; }
    if (h[0]) n += (size_t)snprintf(o + n, cap_ - n, "ha %s\n", h);
    if (t[0]) n += (size_t)snprintf(o + n, cap_ - n, "token %s\n", t);
    for (int i = 0; i < ns; i++) n += (size_t)snprintf(o + n, cap_ - n, "screen %s\t%s\n", sc[i].name, sc[i].url);
    n += (size_t)snprintf(o + n, cap_ - n, "default %d\nreturn %d\n", d, r);
    for (int i = 0; i < nst; i++) n += (size_t)snprintf(o + n, cap_ - n, "set %s %s\n", st[i].key, st[i].val);
    path(p, sizeof p);
    int bad = statefile_write(p, 0600, o, n);
    free(o);
    if (bad) { snprintf(err, cap, "cannot write state/kiosk"); return -1; }
    take(sc, ns, d, r, h, t, st, nst);
    /* Echo Local names the home app: the Screen page's choice from now on (one home app, whoever picked it) */
    char cur[APP_MAX + 1] = "";
    if (ap[0] && (screen_home(cur, sizeof cur) || strcmp(cur, ap))) screen_set_home(ap);
    fprintf(stderr, "kiosk: %d screen%s from Home Assistant%s%s%s\n", ns, ns == 1 ? "" : "s", t[0] ? ", with a login" : "",
            ap[0] ? ", home app " : "", ap);
    return 0;
}

char *kiosk_json(int with_token, size_t *len)
{
    size_t cap = 8192 + TOKEN_MAX + SETS * (SET_KEY + SET_VAL + 8), n = 0; char *o = malloc(cap);
    if (!o) return NULL;
    char app[APP_MAX + 1] = ""; if (screen_home(app, sizeof app) || !is_pkg(app)) app[0] = 0;
    pthread_mutex_lock(&lock);
    /* every string passed clean() or is_pkg(): nothing in it to escape */
    n += (size_t)snprintf(o + n, cap - n, "{\"enabled\":true,\"ha\":\"%s\",\"has_token\":%s,", ha, token[0] ? "true" : "false");
    if (with_token) {
        n += (size_t)snprintf(o + n, cap - n, "\"token\":\"%s\",\"settings\":{", token);
        for (int i = 0; i < nsets; i++) n += (size_t)snprintf(o + n, cap - n, "%s\"%s\":\"%s\"", i ? "," : "", sets[i].key, sets[i].val);
        n += (size_t)snprintf(o + n, cap - n, "},");
    }
    n += (size_t)snprintf(o + n, cap - n, "\"app\":\"%s\",\"default\":%d,\"return\":%d,\"screens\":[", app, def, ret_s);
    for (int i = 0; i < nscreens; i++) n += (size_t)snprintf(o + n, cap - n, "%s{\"name\":\"%s\",\"url\":\"%s\"}", i ? "," : "", screens[i].name, screens[i].url);
    n += (size_t)snprintf(o + n, cap - n, "]}");
    pthread_mutex_unlock(&lock);
    if (len) *len = n;
    return o;
}

int kiosk_screen_names(char *out, size_t cap)
{
    size_t n = 0; out[0] = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nscreens; i++)
        if (strncmp(screens[i].url, "hassmic:", 8))         /* the app's own Home and clock: the drawer's Home */
            n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%s%s", n ? "\t" : "", screens[i].name);
    pthread_mutex_unlock(&lock);
    return n < cap ? 0 : -1;
}

void kiosk_polled(int front, int draws_)
{
    char app[APP_MAX + 1] = ""; screen_home(app, sizeof app);
    pthread_mutex_lock(&lock);
    int was = front_at && mono_ms() - front_at < FRONT_MS ? draws : 0;
    if (front) { draws = draws_; front_at = mono_ms(); } else { front_at = 0; draws = 0; }
    int now = draws;
    pthread_mutex_unlock(&lock);
    /* what it claims, for the UI: a ring it draws itself is left to it (only while it says it is in front) */
    if (now != was) screen_claim(app, now);
}

/* the uid of the process at the other end of a loopback TCP connection: the kernel's table of sockets has the
 * client's own entry (local = its address and port, remote = ours) with its owner */
static long peer_uid(int fd)
{
    struct sockaddr_storage a, b; socklen_t na = sizeof a, nb = sizeof b;
    if (getpeername(fd, (struct sockaddr *)&a, &na) || getsockname(fd, (struct sockaddr *)&b, &nb)) return -1;
    unsigned pp = ntohs(a.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&a)->sin6_port : ((struct sockaddr_in *)&a)->sin_port);
    unsigned lp = ntohs(b.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&b)->sin6_port : ((struct sockaddr_in *)&b)->sin_port);
    static const char *const tabs[] = { "/proc/net/tcp6", "/proc/net/tcp" };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(tabs[i], "r"); char line[512];
        if (!f) continue;
        if (!fgets(line, sizeof line, f)) { fclose(f); continue; }          /* the header */
        while (fgets(line, sizeof line, f)) {
            char loc[64], rem[64]; unsigned st; long uid;
            /* sl local rem st tx:rx tr:when retrnsmt uid */
            if (sscanf(line, "%*s %63s %63s %x %*s %*s %*s %ld", loc, rem, &st, &uid) != 4) continue;
            char *lc = strrchr(loc, ':'), *rc = strrchr(rem, ':');
            if (!lc || !rc) continue;
            if (strtoul(lc + 1, NULL, 16) == pp && strtoul(rc + 1, NULL, 16) == lp) { fclose(f); return uid; }
        }
        fclose(f);
    }
    return -1;
}

int kiosk_peer_ok(int fd)
{
    char p[300], pkg[APP_MAX + 1] = ""; struct stat st;
    const char *root = getenv("HASSMIC_APP_DATA");          /* tests: a folder standing for /data/data */
    if (screen_home(pkg, sizeof pkg) || !is_pkg(pkg)) return 0;   /* no app of the owner's is the home app */
    snprintf(p, sizeof p, "%s/%s", root ? root : "/data/data", pkg);
    if (stat(p, &st)) return 0;                             /* not installed */
    long uid = peer_uid(fd);
    if (uid < 0 || (long)st.st_uid != uid) {
        static long last = -2;
        if (uid != last) { fprintf(stderr, "kiosk: config refused to uid %ld (the home app %s is %ld)\n", uid, pkg, (long)st.st_uid); last = uid; }
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- what the screen shows */

static unsigned ver = 1; static int running; static long long words_until;
static char heard[1024], reply[2048], ringing[256];
static void poke(void) { ver++; pthread_cond_broadcast(&changed); }
static void set_text(char *dst, size_t cap, const char *text)
{
    pthread_mutex_lock(&lock); snprintf(dst, cap, "%s", text ? text : ""); poke(); pthread_mutex_unlock(&lock);
}
void kiosk_heard(const char *text) { set_text(heard, sizeof heard, text); }
void kiosk_reply(const char *text) { set_text(reply, sizeof reply, text); }
void kiosk_ringing(const char *what)
{
    pthread_mutex_lock(&lock);
    if (strcmp(ringing, what ? what : "")) { snprintf(ringing, sizeof ringing, "%s", what ? what : ""); poke(); }
    pthread_mutex_unlock(&lock);
}
void kiosk_run(int active)
{
    pthread_mutex_lock(&lock);
    if (active != running) {
        if (active) heard[0] = reply[0] = 0;                /* a new request: the last one's words go */
        running = active; words_until = active ? 0 : mono_ms() + WORDS_MS;
        poke();
    }
    pthread_mutex_unlock(&lock);
}
void kiosk_changed(void) { pthread_mutex_lock(&lock); poke(); pthread_mutex_unlock(&lock); }

static void jesc_(char *o, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(o + n, cap - n, "\\u%04x", c);
        else o[n++] = (char)c;
    }
    o[n] = 0;
}

size_t kiosk_state_json(char *out, size_t cap)
{
    static char h[2100], r[4100], g[520];
    pthread_mutex_lock(&lock);
    unsigned v = ver; int run = running;
    int words = run || (words_until && mono_ms() < words_until && (heard[0] || reply[0]));
    jesc_(h, sizeof h, heard); jesc_(r, sizeof r, reply); jesc_(g, sizeof g, ringing);
    int alarm = strstr(ringing, "Alarm") != NULL;
    pthread_mutex_unlock(&lock);
    int h24 = alarms_h24();
    char next[700] = "null";
    struct alarm a; time_t nx = alarms_next(&a);
    if (nx) {
        char f[96], t[200], te[400], le[120];
        if (nx == a.snoozed) { struct tm tm; localtime_r(&nx, &tm); alarms_time_text(tm.tm_hour, tm.tm_min, h24, f, sizeof f); snprintf(t, sizeof t, "Snoozed until %s", f); }
        else { alarms_format(&a, alarms_now(), h24, f, sizeof f); snprintf(t, sizeof t, "Alarm %s", f); }
        jesc_(te, sizeof te, t); jesc_(le, sizeof le, a.label);
        snprintf(next, sizeof next, "{\"at\":%lld,\"text\":\"%s\",\"label\":\"%s\"}", (long long)nx, te, le);
    }
    return (size_t)snprintf(out, cap, "{\"v\":%u,\"running\":%s,\"words\":%s,\"heard\":\"%s\",\"reply\":\"%s\",\"ringing\":\"%s\","
                            "\"ringing_alarm\":%s,\"h24\":%s,\"clock_set\":%s,\"next_alarm\":%s,\"display\":\"always\"}",
                            v, run ? "true" : "false", words ? "true" : "false", h, r, g, alarm ? "true" : "false",
                            h24 ? "true" : "false", alarms_clock_ok() ? "true" : "false", next);
}

void kiosk_state_wait(unsigned v, int ms)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&lock);
    while (ver == v) if (pthread_cond_timedwait(&changed, &lock, &ts)) break;
    pthread_mutex_unlock(&lock);
}
