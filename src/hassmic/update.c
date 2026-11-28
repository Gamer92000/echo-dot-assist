/*
 * Online updates: the project's builds as published on GitHub by CI (.github/workflows/build.yml), for whoever switches
 * them on in Home Assistant.  The select "Online updates" picks the channel: off (the default: nothing is fetched),
 * beta (every build of main, as a release candidate, and every release) or release (releases only).  The update entity
 * shows the newest build on that channel against what runs; its install button downloads this model's bundle and hands
 * it to root's installer, like a push from the PC (ota_handoff).
 *
 * Trust: every bundle is signed with the release key, whose secret half only CI has.  keys/release.pub
 * (keys/release.conf names it; a fork's own: secrets/fork.conf) comes with every build and install; root checks the
 * signature against it from the copy that runs or the system partition (scripts/system/main.sh) before anything is
 * installed, and falls back to the factory copy if the new one does not stay up, as for a push.  So the download itself
 * needs no trust: GitHub over HTTPS, through http.c on hassmic's own TLS (tls.c: the system CA store,
 * /system/etc/security/cacerts, has GitHub's roots).  Its sockets are opened as the group the egress lock lets out
 * (net_socket); a plain one would only reach the LAN.
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
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include "board.h"
#include "core.h"
#include "http.h"
#include "net.h"
#include "ota.h"
#include "plugin.h"
#include "sources.h"

#define CURRENT VERSION_NAME            /* (core.h) */
/* The Makefile passes UPDATE_REPO (keys/release.conf, or a fork's secrets/fork.conf); this, as keys/release.conf, only
 * for a build outside it (an editor's) */
#ifndef REPO
#define REPO "Gamer92000/echo-dot-assist"
#endif
#define MAX_BUNDLE (16u << 20)          /* as for a push (ota.c) */
#define FIRST_LOOK 60                   /* s after start: the network may not be up before */
#define INTERVAL (6 * 3600)

const char *const update_channels[3] = { "off", "beta", "release" };

/* The owner's own server where the settings page names one (sources.c, issue #175: GitHub or a Forgejo/Gitea), else
 * this project's releases; tests point the default at a local server */
static const char *api_base(char *b, size_t cap)
{
    const char *e = getenv("HASSMIC_UPDATE_API");
    if (sources_update_urls(b, cap, NULL, 0)) return b;
    return e ? e : "https://api.github.com/repos/" REPO;
}
static const char *dl_base(char *b, size_t cap)
{
    const char *e = getenv("HASSMIC_UPDATE_DOWNLOAD");
    if (sources_update_urls(NULL, 0, b, cap)) return b;
    return e ? e : "https://github.com/" REPO "/releases/download";
}
/* what the summary and the log call it */
static const char *server_name(void) { return sources_update_urls(NULL, 0, NULL, 0) ? "the update server" : "GitHub"; }

static pthread_mutex_t ulock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ucond = PTHREAD_COND_INITIALIZER;
static int channel, want_check, want_install;           /* ulock */
static struct update_state st = { .current = CURRENT, .latest = CURRENT };      /* ulock */
static char tag[64];                                    /* ulock: tag of st.latest; "" until a look found one */
static void (*notify)(void);

/* ---------------------------------------------------------------- the fetch (http.c) */

struct buf { unsigned char *p; size_t n, cap, max; int progress, over, code; long long total;
             const char *const *hdrs; int lan; };      /* update_fetch_lan's: headers to send; plain HTTP, no redirect, 200 only */

/* The final answer's head: 4xx and 5xx end it (libcurl's FAILONERROR, as before), and another Echo's port answers 200
 * or nothing (a redirect's own answer is none) */
static int on_head(void *arg, int status, long long length)
{
    struct buf *b = arg;
    b->total = length;
    if (status >= 400 || (b->lan && status != 200)) { b->code = status; return 1; }
    return 0;
}

static int on_data(void *arg, const void *d, size_t len)
{
    struct buf *b = arg;
    if (b->n + len > b->max) { b->over = 1; return 1; } /* ends the transfer */
    if (b->n + len + 1 > b->cap) {
        size_t cap = (b->n + len + 1) * 2; unsigned char *p = realloc(b->p, cap);
        if (!p) return 1;
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, d, len); b->n += len; b->p[b->n] = 0;
    if (b->progress && b->total > 0) {
        float pct = 100.0f * (float)b->n / (float)b->total; int tell;
        pthread_mutex_lock(&ulock);
        tell = pct - st.progress >= 5 || (pct >= 100 && st.progress < 100);
        if (tell) st.progress = pct;
        pthread_mutex_unlock(&ulock);
        if (tell && notify) notify();
    }
    return 0;
}

/* GET URL into B (at most MAX bytes); 0, or -1 with the reason in ERR.  SECS: all the time the transfer has, from
 * the name lookup to the last byte (15: the page waits for the answer; LONG_S: a bundle over a slow link).  STOP set
 * by another thread ends it within 250 ms (http.c), but for a name lookup (getaddrinfo), which ends with the
 * resolver's timeouts */
#define LONG_S 900
/* UA: what the request says it is.  hassmic's own looks at GitHub's API name the version (GitHub refuses a request
 * without one, and the version is what an update is about); update_fetch's, the plugin index's site, only "hassmic"
 * (GitHub Pages needs none, and the index learns nothing of the Echo but its address and the file) */
static int fetch(const char *url, struct buf *b, size_t max, int progress, long secs, const volatile int *stop, const char *ua, char *err, size_t cap)
{
    char why[300];
    b->n = 0; b->max = max; b->progress = progress; b->over = 0; b->code = 0; b->total = -1;
    if (secs < 1) secs = 1;
    /* The first address may be plain HTTP (the tests' stand-ins; a site a fork names), a redirect only HTTPS: one to
     * HTTP took whatever answered there for GitHub's (a review, 2026-11-21; the lists and the bundles are signed, but
     * a package's bytes, the API's answer and what the page shows were not to come from anyone on the way).  GitHub's
     * own redirects, the release assets' to their CDN, are HTTPS.  Another Echo's settings port (b->lan): plain HTTP
     * and no redirect at all, through a plain socket, which the egress lock lets reach the local network only,
     * whatever address a member's word named (a review of 6631974, 6; and 2026-11-21: a redirect was followed to any
     * HTTPS address a member named) */
    struct http r = { .url = url, .agent = ua, .headers = b->hdrs, .verify = 1, .stop = stop,
                      .plain = b->lan, .lan = b->lan, .redirects = b->lan ? 0 : 5, .redirect_https = 1,
                      .connect_s = secs <= 15 ? (secs < 10 ? (int)secs : 10) : 20, .total_s = (int)secs,
                      .low_s = 60, .low_bytes = 512, .head = on_head, .data = on_data, .arg = b };
    int rc = http_run(&r, why, sizeof why);
    if (b->code) snprintf(err, cap, "HTTP %d from %s", b->code, url);
    else if (rc == HTTP_CANCELLED) snprintf(err, cap, "cancelled");
    else if (rc && b->over) snprintf(err, cap, "%s: more than %zu bytes", url, max);
    else if (rc) snprintf(err, cap, "%s: %s", url, why);
    return rc ? -1 : 0;
}

/* The same fetch for plugin_fetch.c (docs/plugins.md 5.6 has the two share it), as "hassmic" with no version: URL into *DATA (the caller's to
 * free, a NUL after its *LEN bytes), at most MAX bytes; 0, or -1 with why in ERR ("HTTP <code> from ..." when the
 * server answered: reached, as update_probe counts it).  SECS: all the time it has (fetch).
 * STOP (may be NULL): set by another thread, the transfer ends within 250 ms (fetch says when it does not).
 * Blocks; no lock held. */
int update_fetch(const char *url, size_t max, long secs, const volatile int *stop, unsigned char **data, size_t *len, char *err, size_t cap)
{
    struct buf b = { 0 }; int rc;
    rc = fetch(url, &b, max, 0, secs, stop, "hassmic", err, cap);
    if (rc || !b.p) { free(b.p); *data = NULL; *len = 0; if (!rc) snprintf(err, cap, "%s: an empty answer", url); return -1; }
    *data = b.p; *len = b.n;
    return 0;
}

int update_fetch_lan(const char *url, const char *const *hdrs, size_t max, long secs, unsigned char **data, size_t *len, char *err, size_t cap)
{
    struct buf b = { 0 }; int rc;
    b.hdrs = hdrs; b.lan = 1;
    rc = fetch(url, &b, max, 0, secs, NULL, "hassmic", err, cap);
    if (rc || !b.p) { free(b.p); *data = NULL; *len = 0; if (!rc) snprintf(err, cap, "%s: an empty answer", url); return -1; }
    *data = b.p; *len = b.n;
    return 0;
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
    char url[400], err[300], t[64], page[192], body[1024], *notes = body, ab[300]; struct buf b = { 0 }; int found;
    snprintf(url, sizeof url, ch == UPDATE_RELEASE ? "%s/releases/latest" : "%s/releases?per_page=10", api_base(ab, sizeof ab));
    if (fetch(url, &b, 1u << 20, 0, LONG_S, NULL, "hassmic/" CURRENT, err, sizeof err)) {
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
    if (!found) { char w[60]; snprintf(w, sizeof w, "unexpected answer from %s", server_name()); set_failed("could not look for updates", ch == UPDATE_BETA ? "nothing published yet" : w); return -1; }
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
    char url[400], err[300], t[64], res[256], db[300]; struct buf b = { 0 }, s = { 0 }; uint8_t pk[32]; int own, unchecked = sources_update_unchecked();
    if (look(ch)) return;
    pthread_mutex_lock(&ulock);
    snprintf(t, sizeof t, "%s", tag);
    int same = !strcmp(st.latest, st.current);
    if (!same) { st.in_progress = 1; st.progress = 0; }
    pthread_mutex_unlock(&ulock);
    if (same) { fprintf(stderr, "update: %s runs already, nothing to install\n", CURRENT); return; }
    if (notify) notify();
    fprintf(stderr, "update: downloading %s for %s\n", t, board.codename);
    snprintf(url, sizeof url, "%s/%s/hassmic-%s.bundle", dl_base(db, sizeof db), t, board.codename);
    if (fetch(url, &b, MAX_BUNDLE, 1, LONG_S, NULL, "hassmic/" CURRENT, err, sizeof err)) { set_failed("download failed", err); goto out; }
    if (unchecked) {
        /* No signature check (Sources, the owner's word with the action button): none is fetched, whatever the
         * server has; hassmic's own key signs what came, the key root was told to trust (sources.c) */
        if (!(s.p = malloc(64)) || sources_update_sign(b.p, b.n, s.p)) { set_failed("not installed", "hassmic's own key for updates without a signature check is missing"); goto out; }
        s.n = 64;
        fprintf(stderr, "update: %s downloaded and NOT CHECKED (Sources: no signature check), signed with hassmic's own key for root\n", t);
    } else {
        snprintf(url, sizeof url, "%s/%s/hassmic-%s.bundle.sig", dl_base(db, sizeof db), t, board.codename);
        if (fetch(url, &s, 64, 0, LONG_S, NULL, "hassmic/" CURRENT, err, sizeof err)) { set_failed("download failed", err); goto out; }
        if (s.n != 64) { set_failed("download failed", "the signature is not 64 bytes"); goto out; }
    }
    /* (nothing of the plugins' to do first: a hassmic update never breaks a plugin, sdk/PROTOCOL.md "Versions") */
    own = sources_update_key(pk);
    if (ota_handoff(b.p, b.n, s.p, own ? pk : NULL, res, sizeof res)) { set_failed("not installed", res + (strncmp(res, "FAILED ", 7) ? 0 : 7)); goto out; }
    pthread_mutex_lock(&ulock);
    /* root restarts us in 2 s */
    snprintf(st.summary, sizeof st.summary, "%s installed, restarting", st.latest);
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
    char url[400], why[300], ab[300]; struct buf b = { 0 }; int rc;
    snprintf(url, sizeof url, "%s/releases?per_page=1", api_base(ab, sizeof ab));
    rc = fetch(url, &b, 1u << 20, 0, 15, NULL, "hassmic/" CURRENT, why, sizeof why);
    free(b.p);
    if (rc && strstr(why, "HTTP ") == why) rc = 0;
    if (rc) { size_t ul = strlen(url); snprintf(err, cap, "%s", strncmp(why, url, ul) ? why : why + ul + 2); fprintf(stderr, "update: %s not reachable: %s\n", server_name(), why); }
    return rc ? -1 : 0;
}

void update_check(void) { pthread_mutex_lock(&ulock); want_check = 1; pthread_cond_signal(&ucond); pthread_mutex_unlock(&ulock); }
void update_install(void) { pthread_mutex_lock(&ulock); want_install = 1; pthread_cond_signal(&ucond); pthread_mutex_unlock(&ulock); }
void update_get(struct update_state *s) { pthread_mutex_lock(&ulock); *s = st; pthread_mutex_unlock(&ulock); }
