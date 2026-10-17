/*
 * Online updates: the project's builds as published on GitHub by CI (.github/workflows/build.yml), for whoever switches
 * them on in Home Assistant.  The select "Online updates" picks the channel: off (the default: nothing is fetched),
 * beta (every build of main, as a release candidate, and every release) or release (releases only).  The update entity
 * shows the newest build on that channel against what runs; its install button downloads this model's bundle and hands
 * it to root's installer, like a push from the PC (ota_handoff).
 *
 * Trust: every bundle is signed with the release key, whose secret half only CI has.  keys/release.pub comes with every
 * build and install; root checks the signature against it from the copy that runs or the system partition
 * (scripts/system/main.sh) before anything is installed, and falls back to the factory copy if the new one does not
 * stay up, as for a push.  So the download itself needs no trust: GitHub over HTTPS, through the firmware's own libcurl
 * 7.50.1 (dlopen'ed: OpenSSL and the system CA store, /system/etc/security/cacerts, have GitHub's roots).  Its sockets
 * are opened by us, as the group the egress lock lets out (net_socket); libcurl's own would only reach the LAN.
 *
 * Looked at a minute after start, then every 6 hours, on a change of channel and when Home Assistant asks (the
 * entity's "check").  Release: /releases/latest, the newest that is not a prerelease.  Beta: the highest version among
 * /releases?per_page=10, the newest of all: GitHub's list is not newest first (it starts with the release it marks
 * latest; 2026-10-02 an Echo on beta was offered the release before a newer beta that way).  Versions are the commit's time in UTC (2026.10.02.091530), the same for a commit published as beta
 * (tag v<version>-beta) and as release (v<version>): Home Assistant compares them as numbers and offers what is newer
 * than what runs, so a release of the commit a beta user runs already is not offered again.  Install takes whatever is
 * newest on the channel.
 */
#include "update.h"
#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "board.h"
#include "core.h"
#include "net.h"
#include "ota.h"

#ifdef RELEASE
#define CURRENT VERSION                 /* a build CI published (Makefile RELEASE) */
#else
#define CURRENT VERSION "+" BUILD
#endif
#define REPO "Gamer92000/echo-dot-assist"
#define MAX_BUNDLE (16u << 20)          /* as for a push (ota.c) */
#define FIRST_LOOK 60                   /* s after start: the network may not be up before */
#define INTERVAL (6 * 3600)

const char *const update_channels[3] = { "off", "beta", "release" };

/* tests point these at a local server */
static const char *api_base(void) { const char *e = getenv("HASSMIC_UPDATE_API"); return e ? e : "https://api.github.com/repos/" REPO; }
static const char *dl_base(void) { const char *e = getenv("HASSMIC_UPDATE_DOWNLOAD"); return e ? e : "https://github.com/" REPO "/releases/download"; }

static pthread_mutex_t ulock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ucond = PTHREAD_COND_INITIALIZER;
static int channel, want_check, want_install;           /* ulock */
static struct update_state st = { .current = CURRENT, .latest = CURRENT };      /* ulock */
static char tag[64];                                    /* ulock: tag of st.latest; "" until a look found one */
static void (*notify)(void);

/* ---------------------------------------------------------------- libcurl, from the firmware */

enum { OPT_WRITEDATA = 10001, OPT_URL = 10002, OPT_USERAGENT = 10018, OPT_TIMEOUT = 13, OPT_LOW_SPEED_LIMIT = 19,
       OPT_LOW_SPEED_TIME = 20, OPT_NOPROGRESS = 43, OPT_FAILONERROR = 45, OPT_FOLLOWLOCATION = 52, OPT_XFERINFODATA = 10057,
       OPT_MAXREDIRS = 68, OPT_CONNECTTIMEOUT = 78, OPT_NOSIGNAL = 99, OPT_OPENSOCKETFUNCTION = 20163,
       OPT_OPENSOCKETDATA = 10164, OPT_SOCKOPTFUNCTION = 20148, OPT_SOCKOPTDATA = 10149, OPT_PROTOCOLS = 181, OPT_REDIR_PROTOCOLS = 182, OPT_WRITEFUNCTION = 20011,
       OPT_XFERINFOFUNCTION = 20219, INFO_RESPONSE_CODE = 0x200002, PROTO_HTTP_S = 1 | 2, CURLE_HTTP_RETURNED_ERROR = 22 };
struct curl_sockaddr { int family, socktype, protocol; unsigned addrlen; };     /* followed by the address; unused here */

static struct {
    void *(*init)(void);
    int (*setopt)(void *, int, ...);
    int (*perform)(void *);
    void (*cleanup)(void *);
    int (*getinfo)(void *, int, ...);
    const char *(*strerror)(int);
} curl;

static int curl_load(void)
{
    static int done, ok; void *h;
    if (done) return ok;
    done = 1;
    if (!(h = dlopen("libcurl.so", RTLD_NOW)) && !(h = dlopen("libcurl.so.4", RTLD_NOW))) {    /* Echo; PC */
        fprintf(stderr, "update: no libcurl (%s)\n", dlerror()); return 0;
    }
    curl.init = (void *(*)(void))dlsym(h, "curl_easy_init");
    curl.setopt = (int (*)(void *, int, ...))dlsym(h, "curl_easy_setopt");
    curl.perform = (int (*)(void *))dlsym(h, "curl_easy_perform");
    curl.cleanup = (void (*)(void *))dlsym(h, "curl_easy_cleanup");
    curl.getinfo = (int (*)(void *, int, ...))dlsym(h, "curl_easy_getinfo");
    curl.strerror = (const char *(*)(int))dlsym(h, "curl_easy_strerror");
    return ok = curl.init && curl.setopt && curl.perform && curl.cleanup && curl.getinfo && curl.strerror;
}

struct buf { unsigned char *p; size_t n, cap, max; int progress; };

static size_t on_data(char *d, size_t size, size_t n, void *arg)
{
    struct buf *b = arg; size_t len = size * n;
    if (b->n + len > b->max) return 0;                  /* aborts the transfer */
    if (b->n + len + 1 > b->cap) {
        size_t cap = (b->n + len + 1) * 2; unsigned char *p = realloc(b->p, cap);
        if (!p) return 0;
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, d, len); b->n += len; b->p[b->n] = 0;
    return len;
}

static int on_progress(void *arg, int64_t total, int64_t now, int64_t ut, int64_t un)
{
    struct buf *b = arg; (void)ut; (void)un;
    if (!b->progress || total <= 0) return 0;
    float pct = 100.0f * (float)now / (float)total; int tell;
    pthread_mutex_lock(&ulock);
    tell = pct - st.progress >= 5 || (pct >= 100 && st.progress < 100);
    if (tell) st.progress = pct;
    pthread_mutex_unlock(&ulock);
    if (tell && notify) notify();
    return 0;
}

static int on_socket(void *arg, int purpose, struct curl_sockaddr *a)
{
    (void)arg; (void)purpose;
#ifdef __ANDROID__
    /* libc's connect() fails on athena (netio.h): connected here, and on_sockopt tells libcurl so.  The address
     * follows the four ints; the SO_SNDTIMEO is connect's timeout (libcurl's CONNECTTIMEOUT does not reach it). */
    return net_open_connected(a->family, a->socktype, a->protocol, (const struct sockaddr *)(a + 1), a->addrlen, 10);
#else
    return net_socket(a->family, a->socktype, a->protocol);   /* the egress lock lets this group out: an external URL */
#endif
}
#ifdef __ANDROID__
static int on_sockopt(void *arg, int fd, int purpose) { (void)arg; (void)fd; (void)purpose; return 2; }   /* CURL_SOCKOPT_ALREADY_CONNECTED */
#endif

/* GET URL into B (at most MAX bytes); 0, or -1 with the reason in ERR.  QUICK: the page waits for the answer */
static int fetch(const char *url, struct buf *b, size_t max, int progress, int quick, char *err, size_t cap)
{
    void *h; int rc; long code = 0;
    b->n = 0; b->max = max; b->progress = progress;
    if (!curl_load() || !(h = curl.init())) { snprintf(err, cap, "no libcurl on this device"); return -1; }
    curl.setopt(h, OPT_URL, url);
    curl.setopt(h, OPT_USERAGENT, "hassmic/" CURRENT);   /* GitHub's API refuses requests without one */
    curl.setopt(h, OPT_PROTOCOLS, (long)PROTO_HTTP_S); curl.setopt(h, OPT_REDIR_PROTOCOLS, (long)PROTO_HTTP_S);
    curl.setopt(h, OPT_FOLLOWLOCATION, 1L); curl.setopt(h, OPT_MAXREDIRS, 5L);     /* downloads redirect to a CDN */
    curl.setopt(h, OPT_FAILONERROR, 1L); curl.setopt(h, OPT_NOSIGNAL, 1L);
    curl.setopt(h, OPT_CONNECTTIMEOUT, quick ? 10L : 20L); curl.setopt(h, OPT_TIMEOUT, quick ? 15L : 900L);
    curl.setopt(h, OPT_LOW_SPEED_LIMIT, 512L); curl.setopt(h, OPT_LOW_SPEED_TIME, 60L);
    curl.setopt(h, OPT_WRITEFUNCTION, on_data); curl.setopt(h, OPT_WRITEDATA, b);
    curl.setopt(h, OPT_XFERINFOFUNCTION, on_progress); curl.setopt(h, OPT_XFERINFODATA, b); curl.setopt(h, OPT_NOPROGRESS, 0L);
    curl.setopt(h, OPT_OPENSOCKETFUNCTION, on_socket); curl.setopt(h, OPT_OPENSOCKETDATA, NULL);
#ifdef __ANDROID__
    curl.setopt(h, OPT_SOCKOPTFUNCTION, on_sockopt); curl.setopt(h, OPT_SOCKOPTDATA, NULL);
#endif
    rc = curl.perform(h);
    curl.getinfo(h, INFO_RESPONSE_CODE, &code);
    curl.cleanup(h);
    if (rc == CURLE_HTTP_RETURNED_ERROR) snprintf(err, cap, "HTTP %ld from %s", code, url);
    else if (rc) snprintf(err, cap, "%s: %s", url, curl.strerror(rc));
    return rc ? -1 : 0;
}

/* ---------------------------------------------------------------- the few fields of GitHub's answer */

/* JSON string at P (after its opening quote) into OUT, unescaped, cut to fit at a character boundary; NULL if malformed,
 * else what follows the closing quote. */
static const char *json_string(const char *p, const char *end, char *out, size_t cap)
{
    size_t n = 0; int full = 0;
    while (p < end && *p != '"') {
        char c[4]; int cl = 1; c[0] = *p++;
        if (c[0] == '\\') {
            if (p >= end) return NULL;
            switch (*p++) {
            case 'n': c[0] = '\n'; break;
            case 't': c[0] = '\t'; break;
            case 'r': c[0] = '\r'; break;
            case 'b': case 'f': c[0] = ' '; break;
            case 'u': {
                unsigned u = 0;
                if (end - p < 4) return NULL;
                for (int i = 0; i < 4; i++, p++) u = u << 4 | (isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10) & 15);
                if (u >= 0xd800 && u < 0xe000) u = 0xfffd;                  /* surrogates: not worth pairing up here */
                if (u < 0x80) c[0] = u;
                else if (u < 0x800) { c[0] = 0xc0 | u >> 6; c[1] = 0x80 | (u & 63); cl = 2; }
                else { c[0] = 0xe0 | u >> 12; c[1] = 0x80 | (u >> 6 & 63); c[2] = 0x80 | (u & 63); cl = 3; }
            } break;
            default: c[0] = p[-1];                       /* \" \\ \/ */
            }
        } else if ((c[0] & 0xc0) == 0xc0) {             /* a UTF-8 sequence stays whole */
            while (p < end && (*p & 0xc0) == 0x80 && cl < 4) c[cl++] = *p++;
        }
        if (!full && n + cl < cap) { memcpy(out + n, c, cl); n += cl; } else full = 1;
    }
    if (cap) out[n] = 0;
    return p < end ? p + 1 : NULL;
}

/* KEY of the first object in JS (the object itself, or the first element of an array of them): a string unescaped,
 * anything else as it stands.  1 if there. */
static int json_get(const char *p, const char *end, const char *key, char *out, size_t cap)
{
    int depth = 0; size_t kl = strlen(key);
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p < end && *p == '[') p++;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '{') return 0;
    for (; p < end; p++) {
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') { if (--depth == 0) return 0; }
        else if (*p == '"') {
            const char *s = p + 1, *q;
            for (p = s; p < end && *p != '"'; p++) if (*p == '\\') p++;
            if (p >= end) return 0;
            for (q = p + 1; q < end && isspace((unsigned char)*q); q++) ;
            if (depth != 1 || q >= end || *q != ':' || (size_t)(p - s) != kl || memcmp(s, key, kl)) continue;
            for (q++; q < end && isspace((unsigned char)*q); q++) ;
            if (q < end && *q == '"') return json_string(q + 1, end, out, cap) != NULL;
            size_t n = 0;
            while (q < end && !strchr(",}] \t\r\n", *q) && n + 1 < cap) out[n++] = *q++;
            out[n] = 0;
            return n > 0;
        }
    }
    return 0;
}

/* Element I of the JSON array at P, an object, as [*s, *e); 0 if there is none.  Strings are skipped whole. */
static int json_item(const char *p, const char *end, int i, const char **s, const char **e)
{
    int depth = 0, n = -1;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '[') return 0;
    for (; p < end; p++) {
        if (*p == '"') { for (p++; p < end && *p != '"'; p++) if (*p == '\\') p++; continue; }
        if (*p == '{' || *p == '[') { if (++depth == 2 && *p == '{' && ++n == i) *s = p; }
        else if (*p == '}' || *p == ']') {
            if (--depth == 1 && n == i && *p == '}') { *e = p + 1; return 1; }
            if (depth <= 0) return 0;
        }
    }
    return 0;
}

/* v2026.10.02.091530-beta -> 2026.10.02.091530: the tag says the channel, the version is the same on both */
static void tag_version(const char *t, char *v, size_t cap)
{
    snprintf(v, cap, "%s", t + (t[0] == 'v'));
    char *beta = strstr(v, "-beta"); if (beta && !beta[5]) *beta = 0;
}

static int version_ok(const char *v)    /* goes into URLs and, through root, into file names */
{
    if (!*v || strlen(v) > 60) return 0;
    for (; *v; v++) if (!isalnum((unsigned char)*v) && !strchr(".+_-", *v)) return 0;
    return 1;
}

/* ---------------------------------------------------------------- looking and installing */

static void set_failed(const char *what, const char *why)
{
    pthread_mutex_lock(&ulock);
    snprintf(st.summary, sizeof st.summary, "%s: %.200s", what, why);     /* the reason may be cut, the summary not overflow */
    st.in_progress = 0;
    pthread_mutex_unlock(&ulock);
    fprintf(stderr, "update: %s: %s\n", what, why);
}

/* newest on channel CH into st; 0 if found */
static int look(int ch)
{
    char url[400], err[300], t[64], page[192], body[1024], *notes = body; struct buf b = { 0 }; int found;
    snprintf(url, sizeof url, ch == UPDATE_RELEASE ? "%s/releases/latest" : "%s/releases?per_page=10", api_base());
    if (fetch(url, &b, 1u << 20, 0, 0, err, sizeof err)) {
        set_failed("could not look for updates", strstr(err, "HTTP 404") && ch == UPDATE_RELEASE ? "no release published yet" : err);
        free(b.p); return -1;
    }
    const char *rs = (const char *)b.p, *re = rs + b.n;   /* the release: the answer itself, or (beta) one of the list */
    if (ch == UPDATE_BETA) {
        /* the highest version: they are fixed width, so string order is time order */
        const char *s = NULL, *e = NULL; char best[64] = "", v[64];
        rs = NULL;
        for (int i = 0; b.p && json_item((char *)b.p, (char *)b.p + b.n, i, &s, &e); i++) {
            if (!json_get(s, e, "tag_name", t, sizeof t) || !version_ok(t)) continue;
            tag_version(t, v, sizeof v);
            if (strlen(v) == strlen(best) ? strcmp(v, best) > 0 : !best[0]) { snprintf(best, sizeof best, "%s", v); rs = s; re = e; }
        }
    }
    found = rs && json_get(rs, re, "tag_name", t, sizeof t);
    if (found) {
        if (!json_get(rs, re, "html_url", page, sizeof page)) page[0] = 0;
        if (!json_get(rs, re, "body", body, sizeof body)) body[0] = 0;
    }
    free(b.p);
    if (!found) { set_failed("could not look for updates", ch == UPDATE_BETA ? "nothing published yet" : "unexpected answer from GitHub"); return -1; }
    if (!version_ok(t)) { set_failed("could not look for updates", "a release with an unexpected tag"); return -1; }
    while (isspace((unsigned char)*notes)) notes++;
    pthread_mutex_lock(&ulock);
    snprintf(tag, sizeof tag, "%s", t);
    tag_version(t, st.latest, sizeof st.latest);
    snprintf(st.url, sizeof st.url, "%s", page);
    /* Home Assistant takes 255 characters of summary; cut at a character, not inside one */
    size_t n = strlen(notes);
    if (n >= sizeof st.summary) { n = sizeof st.summary - 4; while (n && (notes[n] & 0xc0) == 0x80) n--; }
    memcpy(st.summary, notes, n); st.summary[n] = 0;
    if (n < strlen(notes)) strcat(st.summary, "...");
    pthread_mutex_unlock(&ulock);
    fprintf(stderr, "update: newest on %s is %s, this is %s\n", update_channels[ch], t, CURRENT);
    return 0;
}

static void install(int ch)
{
    char url[400], err[300], t[64], res[256]; struct buf b = { 0 }, s = { 0 };
    if (look(ch)) return;
    pthread_mutex_lock(&ulock);
    snprintf(t, sizeof t, "%s", tag);
    int same = !strcmp(st.latest, st.current);
    if (!same) { st.in_progress = 1; st.progress = 0; }
    pthread_mutex_unlock(&ulock);
    if (same) { fprintf(stderr, "update: %s runs already, nothing to install\n", CURRENT); return; }
    if (notify) notify();
    fprintf(stderr, "update: downloading %s for %s\n", t, board.codename);
    snprintf(url, sizeof url, "%s/%s/hassmic-%s.bundle", dl_base(), t, board.codename);
    if (fetch(url, &b, MAX_BUNDLE, 1, 0, err, sizeof err)) { set_failed("download failed", err); goto out; }
    snprintf(url, sizeof url, "%s/%s/hassmic-%s.bundle.sig", dl_base(), t, board.codename);
    if (fetch(url, &s, 64, 0, 0, err, sizeof err)) { set_failed("download failed", err); goto out; }
    if (s.n != 64) { set_failed("download failed", "the signature is not 64 bytes"); goto out; }
    if (ota_handoff(b.p, b.n, s.p, res, sizeof res)) { set_failed("not installed", res + (strncmp(res, "FAILED ", 7) ? 0 : 7)); goto out; }
    pthread_mutex_lock(&ulock);
    snprintf(st.summary, sizeof st.summary, "%s installed, restarting", st.latest);    /* root restarts us in 2 s */
    st.progress = 100;
    pthread_mutex_unlock(&ulock);
out:
    free(b.p); free(s.p);
}

static void *thread(void *arg)
{
    time_t next = time(NULL) + FIRST_LOOK;
    (void)arg;
    pthread_mutex_lock(&ulock);
    for (;;) {
        while (!want_check && !want_install && time(NULL) < next) {
            struct timespec ts = { next, 0 };
            pthread_cond_timedwait(&ucond, &ulock, &ts);
        }
        int ch = channel, inst = want_install;
        want_check = want_install = 0;
        next = time(NULL) + INTERVAL;
        if (ch == UPDATE_OFF) {                         /* nothing to offer: what runs is the newest there is */
            snprintf(st.latest, sizeof st.latest, "%s", st.current); tag[0] = st.summary[0] = st.url[0] = 0;
        }
        pthread_mutex_unlock(&ulock);
        if (ch != UPDATE_OFF) { if (inst) install(ch); else look(ch); }
        if (notify) notify();
        pthread_mutex_lock(&ulock);
    }
    return NULL;
}

/* ---------------------------------------------------------------- interface */

void update_start(void (*changed)(void))
{
    pthread_t t;
    notify = changed;
    pthread_mutex_lock(&ulock); want_check = 0; pthread_mutex_unlock(&ulock);     /* the saved channel: FIRST_LOOK applies */
    if (!pthread_create(&t, NULL, thread, NULL)) pthread_detach(t);
}

int update_channel(int ch)
{
    pthread_mutex_lock(&ulock);
    if (ch >= UPDATE_OFF && ch <= UPDATE_RELEASE && ch != channel) { channel = ch; want_check = 1; pthread_cond_signal(&ucond); }
    ch = channel;
    pthread_mutex_unlock(&ulock);
    return ch;
}

/* Before the page switches them on: can this Echo reach GitHub at all?  Echos often sit on a network kept off the
 * internet, and a channel that never gets an answer would only say so in the update entity's summary.  Any HTTP answer
 * counts (a 403 of the API's rate limit too: what is reached is GitHub, over TLS checked against the system's CAs);
 * what fails is name lookup, connecting or TLS. */
int update_probe(char *err, size_t cap)
{
    char url[400], why[300]; struct buf b = { 0 }; int rc;
    snprintf(url, sizeof url, "%s/releases?per_page=1", api_base());
    rc = fetch(url, &b, 1u << 20, 0, 1, why, sizeof why);
    free(b.p);
    if (rc && strstr(why, "HTTP ") == why) rc = 0;
    if (rc) { size_t ul = strlen(url); snprintf(err, cap, "%s", strncmp(why, url, ul) ? why : why + ul + 2); fprintf(stderr, "update: GitHub not reachable: %s\n", why); }
    return rc ? -1 : 0;
}

void update_check(void) { pthread_mutex_lock(&ulock); want_check = 1; pthread_cond_signal(&ucond); pthread_mutex_unlock(&ulock); }
void update_install(void) { pthread_mutex_lock(&ulock); want_install = 1; pthread_cond_signal(&ucond); pthread_mutex_unlock(&ulock); }
void update_get(struct update_state *s) { pthread_mutex_lock(&ulock); *s = st; pthread_mutex_unlock(&ulock); }
