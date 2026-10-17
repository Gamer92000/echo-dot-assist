/*
 * Media over HTTPS (issue #30).  Home Assistant builds the URLs of TTS replies, announcements and play_media with
 * get_url(): when it serves HTTPS itself (http: ssl_certificate), that prefers the external URL and never hands out a
 * plain http:// one.  proto_esphome.c's own HTTP/1.0 client stays for http://; https:// goes through the firmware's
 * libcurl 7.50.1 (dlopen'ed, as in update.c and davs.c: OpenSSL and the system CA store, /system/etc/security/cacerts).
 * A thread runs the transfer and writes the body into one end of a socketpair; the other end goes to the decoders, so
 * they read it as they read a plain fetch, and the media player's cancel, a shutdown() of that socket, ends the
 * transfer too (the next write fails, and a write callback that returns short stops curl).
 *
 * The certificate is checked by default (system CAs, and the name).  With HTTPS on, Home Assistant hands out no URL by
 * IP address (get_url: allow_ip false), so a public certificate passes as long as the Echo reaches that name; a
 * self-signed one does not: setting "https_verify" off accepts any certificate (the page says what that costs).
 */
#include "https.h"
#include "net.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

static atomic_int verify = 1;
int https_verify(int set) { if (set >= 0) atomic_store(&verify, !!set); return atomic_load(&verify); }

/* ---------------------------------------------------------------- libcurl, from the firmware */

enum { OPT_WRITEDATA = 10001, OPT_URL = 10002, OPT_USERAGENT = 10018, OPT_LOW_SPEED_LIMIT = 19, OPT_LOW_SPEED_TIME = 20,
       OPT_HEADERDATA = 10029, OPT_FAILONERROR = 45, OPT_FOLLOWLOCATION = 52, OPT_SSL_VERIFYPEER = 64,
       OPT_MAXREDIRS = 68, OPT_CONNECTTIMEOUT = 78, OPT_SSL_VERIFYHOST = 81, OPT_NOSIGNAL = 99, OPT_CAINFO = 10065,
       OPT_PROTOCOLS = 181, OPT_REDIR_PROTOCOLS = 182, OPT_RESOLVE = 10203, OPT_WRITEFUNCTION = 20011,
       OPT_HEADERFUNCTION = 20079, OPT_OPENSOCKETFUNCTION = 20163, OPT_OPENSOCKETDATA = 10164,
       INFO_RESPONSE_CODE = 0x200002, PROTO_HTTP_S = 1 | 2,
       CURLE_WRITE_ERROR = 23, CURLE_PEER_FAILED_VERIFICATION = 51, CURLE_SSL_CACERT = 60 };
struct curl_sockaddr { int family, socktype, protocol; unsigned addrlen; };     /* followed by the address; unused here */
struct curl_slist;

static struct {
    void *(*init)(void);
    int (*setopt)(void *, int, ...);
    int (*perform)(void *);
    void (*cleanup)(void *);
    int (*getinfo)(void *, int, ...);
    const char *(*strerror)(int);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
} curl;
static int curl_ok;

static void curl_load(void)
{
    void *h;
    if (!(h = dlopen("libcurl.so", RTLD_NOW)) && !(h = dlopen("libcurl.so.4", RTLD_NOW))) {    /* Echo; PC */
        fprintf(stderr, "media: no libcurl for https (%s)\n", dlerror()); return;
    }
    curl.init = (void *(*)(void))dlsym(h, "curl_easy_init");
    curl.setopt = (int (*)(void *, int, ...))dlsym(h, "curl_easy_setopt");
    curl.perform = (int (*)(void *))dlsym(h, "curl_easy_perform");
    curl.cleanup = (void (*)(void *))dlsym(h, "curl_easy_cleanup");
    curl.getinfo = (int (*)(void *, int, ...))dlsym(h, "curl_easy_getinfo");
    curl.strerror = (const char *(*)(int))dlsym(h, "curl_easy_strerror");
    curl.slist_append = (struct curl_slist *(*)(struct curl_slist *, const char *))dlsym(h, "curl_slist_append");
    curl.slist_free_all = (void (*)(struct curl_slist *))dlsym(h, "curl_slist_free_all");
    curl_ok = curl.init && curl.setopt && curl.perform && curl.cleanup && curl.getinfo && curl.strerror
              && curl.slist_append && curl.slist_free_all;
}

static int on_socket(void *arg, int purpose, struct curl_sockaddr *a)
{
    (void)arg; (void)purpose;
    return net_socket(a->family, a->socktype, a->protocol);   /* the egress lock lets this group out: an external URL */
}

/* ---------------------------------------------------------------- one transfer */

struct xfer {
    pthread_mutex_t m; pthread_cond_t c;
    int state;                          /* 0 connecting, 1 the body comes (https_open returns), -1 failed */
    int refs;                           /* https_open and the thread: the last one frees it */
    int wfd;                            /* our end of the socketpair */
    void *h; struct curl_slist *resolve;
    char url[1024];
};

static void set_state(struct xfer *x, int s)
{
    pthread_mutex_lock(&x->m);
    if (!x->state) { x->state = s; pthread_cond_signal(&x->c); }
    pthread_mutex_unlock(&x->m);
}

static void put(struct xfer *x)
{
    pthread_mutex_lock(&x->m); int last = !--x->refs; pthread_mutex_unlock(&x->m);
    if (!last) return;
    pthread_mutex_destroy(&x->m); pthread_cond_destroy(&x->c);
    free(x);
}

/* The end of a response's headers: a 2xx one is the body's (redirects have their own; FAILONERROR fails 4xx and up),
 * and https_open may return.  The caller learns of a failure before it has a socket, as with a plain fetch. */
static size_t on_header(char *d, size_t size, size_t n, void *arg)
{
    struct xfer *x = arg; long code = 0;
    if (size * n <= 2 && (d[0] == '\r' || d[0] == '\n')) {
        curl.getinfo(x->h, INFO_RESPONSE_CODE, &code);
        if (code >= 200 && code < 300) set_state(x, 1);
    }
    return size * n;
}

static size_t on_data(char *d, size_t size, size_t n, void *arg)
{
    struct xfer *x = arg; size_t len = size * n, off = 0;
    set_state(x, 1);
    while (off < len) {                 /* blocks while the decoders hold back: the TTS queue is full */
        ssize_t w = send(x->wfd, d + off, len - off, MSG_NOSIGNAL);
        if (w <= 0) return 0;           /* the reader hung up (cancel, or it played what it wanted): stop */
        off += (size_t)w;
    }
    return len;
}

static void *run(void *arg)
{
    struct xfer *x = arg;
    int rc = curl.perform(x->h);
    pthread_mutex_lock(&x->m); int began = x->state == 1; pthread_mutex_unlock(&x->m);
    if (rc && !(rc == CURLE_WRITE_ERROR && began))     /* a write error once the body came: the reader stopped it */
        fprintf(stderr, "media: %s: %s%s\n", x->url, curl.strerror(rc),
                rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT
                    ? " (the settings page's \"Check HTTPS certificates\" off accepts it)" : "");
    set_state(x, -1);
    close(x->wfd);                      /* the reader's EOF */
    curl.cleanup(x->h);
    if (x->resolve) curl.slist_free_all(x->resolve);
    put(x);
    return NULL;
}

/* A ".local" host (homeassistant.local) by our own mDNS lookup, as net_connect does for http://: neither the system
 * resolver nor this libcurl asks mDNS.  The certificate is still checked against the name. */
static struct curl_slist *resolve_local(const char *url)
{
    char host[256], entry[300], ip[INET_ADDRSTRLEN]; const char *p = url + 8, *end = p + strcspn(p, "/?#"), *colon;
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at) p = at + 1;
    if (*p == '[') return NULL;                         /* an IPv6 address */
    colon = memchr(p, ':', (size_t)(end - p));
    size_t hl = (size_t)((colon ? colon : end) - p);
    if (hl < 7 || hl >= sizeof host || strncasecmp(p + hl - 6, ".local", 6)) return NULL;
    memcpy(host, p, hl); host[hl] = 0;
    unsigned a = mdns_resolve4(host);
    if (!a || !inet_ntop(AF_INET, &a, ip, sizeof ip)) return NULL;     /* curl tries DNS: some servers serve .local */
    snprintf(entry, sizeof entry, "%s:%.*s:%s", host, colon ? (int)(end - colon - 1) : 3, colon ? colon + 1 : "443", ip);
    return curl.slist_append(NULL, entry);
}

int https_open(const char *url)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, curl_load);
    if (!curl_ok) { fprintf(stderr, "media: https is not available: %s\n", url); return -1; }
    int sv[2]; struct xfer *x = calloc(1, sizeof *x); pthread_t t; pthread_attr_t at;
    if (!x) return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) { free(x); return -1; }
    if (!(x->h = curl.init())) { close(sv[0]); close(sv[1]); free(x); return -1; }
    pthread_mutex_init(&x->m, NULL); pthread_cond_init(&x->c, NULL);
    x->refs = 2; x->wfd = sv[1];
    snprintf(x->url, sizeof x->url, "%s", url);
    void *h = x->h; int v = https_verify(-1);
    curl.setopt(h, OPT_URL, x->url);
    curl.setopt(h, OPT_USERAGENT, "hassmic/" VERSION);
    curl.setopt(h, OPT_PROTOCOLS, (long)PROTO_HTTP_S); curl.setopt(h, OPT_REDIR_PROTOCOLS, (long)PROTO_HTTP_S);
    curl.setopt(h, OPT_FOLLOWLOCATION, 1L); curl.setopt(h, OPT_MAXREDIRS, 5L);
    curl.setopt(h, OPT_FAILONERROR, 1L); curl.setopt(h, OPT_NOSIGNAL, 1L);
    /* as the plain fetch: 4 s per connect there (TLS adds its handshake here), 15 s without data (TTS may still be
     * rendering); no limit on the whole: play_media may be a long file */
    curl.setopt(h, OPT_CONNECTTIMEOUT, 10L);
    curl.setopt(h, OPT_LOW_SPEED_LIMIT, 1L); curl.setopt(h, OPT_LOW_SPEED_TIME, 15L);
    curl.setopt(h, OPT_SSL_VERIFYPEER, v ? 1L : 0L); curl.setopt(h, OPT_SSL_VERIFYHOST, v ? 2L : 0L);
    if (getenv("HASSMIC_CA_FILE")) curl.setopt(h, OPT_CAINFO, getenv("HASSMIC_CA_FILE"));     /* tests: their own CA */
    if ((x->resolve = resolve_local(url))) curl.setopt(h, OPT_RESOLVE, x->resolve);
    curl.setopt(h, OPT_HEADERFUNCTION, on_header); curl.setopt(h, OPT_HEADERDATA, x);
    curl.setopt(h, OPT_WRITEFUNCTION, on_data); curl.setopt(h, OPT_WRITEDATA, x);
    curl.setopt(h, OPT_OPENSOCKETFUNCTION, on_socket); curl.setopt(h, OPT_OPENSOCKETDATA, NULL);
    pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, run, x)) {
        pthread_attr_destroy(&at); close(sv[0]); close(sv[1]); curl.cleanup(h);
        if (x->resolve) curl.slist_free_all(x->resolve);
        pthread_mutex_destroy(&x->m); pthread_cond_destroy(&x->c); free(x);
        return -1;
    }
    pthread_attr_destroy(&at);
    pthread_mutex_lock(&x->m);
    while (!x->state) pthread_cond_wait(&x->c, &x->m);
    int ok = x->state == 1;
    pthread_mutex_unlock(&x->m);
    put(x);
    if (!ok) { close(sv[0]); return -1; }
    return sv[0];
}
