/*
 * Media over HTTPS (issue #30).  Home Assistant builds the URLs of TTS replies, announcements and play_media with
 * get_url(): when it serves HTTPS itself (http: ssl_certificate), that prefers the external URL and never hands out a
 * plain http:// one.  proto_esphome.c's own HTTP/1.0 client stays for http://; https:// goes through http.c on
 * hassmic's own TLS (tls.c, mbedTLS: TLS 1.2 and 1.3).  It went through the firmware's libcurl until issue #236:
 * that one's BoringSSL (Android 7's) has no TLS 1.3, and a Home Assistant on the "Modern" SSL profile refused every
 * reply's audio.
 * A thread runs the transfer and writes the body into one end of a socketpair; the other end goes to the decoders, so
 * they read it as they read a plain fetch, and the media player's cancel, a shutdown() of that socket, ends the
 * transfer too (the next write fails, and the data callback ends http_run).
 *
 * The certificate is checked by default (system CAs, and the name).  With HTTPS on, Home Assistant hands out no URL by
 * IP address (get_url: allow_ip false), so a public certificate passes as long as the Echo reaches that name; a
 * self-signed one does not: setting "https_verify" off accepts any certificate (the page says what that costs).
 */
#include "logread.h"
#include "https.h"
#include "http.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static atomic_int verify = 1;
int https_verify(int set) { if (set >= 0) atomic_store(&verify, !!set); return atomic_load(&verify); }

/* ---------------------------------------------------------------- one transfer */

struct xfer {
    pthread_mutex_t m; pthread_cond_t c;
    int state;                          /* 0 connecting, 1 the body comes (https_open returns), -1 failed */
    int refs;                           /* https_open and the thread: the last one frees it */
    int wfd;                            /* our end of the socketpair */
    int code;                           /* a final answer that was not 2xx */
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

/* The final response's head (redirects are followed before it): a 2xx one is the body's, and https_open may return.
 * The caller learns of a failure before it has a socket, as with a plain fetch. */
static int on_head(void *arg, int status, long long length)
{
    struct xfer *x = arg; (void)length;
    if (status < 200 || status >= 300) { x->code = status; return 1; }
    set_state(x, 1);
    return 0;
}

static int on_data(void *arg, const void *d, size_t len)
{
    struct xfer *x = arg; size_t off = 0;
    while (off < len) {                 /* blocks while the decoders hold back: the TTS queue is full */
        ssize_t w = send(x->wfd, (const char *)d + off, len - off, MSG_NOSIGNAL);
        if (w <= 0) return 1;           /* the reader hung up (cancel, or it played what it wanted): stop */
        off += (size_t)w;
    }
    return 0;
}

static void *run(void *arg)
{
    struct xfer *x = arg; char err[300];
    /* as the plain fetch: 4 s per connect there (TLS adds its handshake here), 15 s without data (TTS may still be
     * rendering); no limit on the whole: play_media may be a long file */
    struct http r = { .url = x->url, .agent = "hassmic/" VERSION, .redirects = 5, .verify = https_verify(-1),
                      .connect_s = 10, .low_s = 15, .low_bytes = 1, .head = on_head, .data = on_data, .arg = x };
    int rc = http_run(&r, err, sizeof err);
    pthread_mutex_lock(&x->m); int began = x->state == 1; pthread_mutex_unlock(&x->m);
    if (x->code)                                    /* (an address with its token: logread.h) */
        log_private("media: %.*s%s: HTTP %d", url_shown(x->url), x->url, x->url[url_shown(x->url)] ? "..." : "", x->code);
    else if (rc && !(rc == HTTP_ENDED && began))    /* ended by the reader once the body came: it stopped it */
        log_private("media: %.*s%s: %s%s", url_shown(x->url), x->url, x->url[url_shown(x->url)] ? "..." : "", err,
                http_cert_refused(err) ? " (the settings page's \"Check HTTPS certificates\" off accepts it)" : "");
    set_state(x, -1);
    close(x->wfd);                      /* the reader's EOF */
    put(x);
    return NULL;
}

int https_open(const char *url)
{
    int sv[2]; struct xfer *x = calloc(1, sizeof *x); pthread_t t; pthread_attr_t at;
    if (!x) return -1;
    if (strlen(url) >= sizeof x->url) { log_private("media: an address too long: %.*s...", url_shown(url) < 200 ? url_shown(url) : 200, url); free(x); return -1; }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) { free(x); return -1; }
    pthread_mutex_init(&x->m, NULL); pthread_cond_init(&x->c, NULL);
    x->refs = 2; x->wfd = sv[1];
    snprintf(x->url, sizeof x->url, "%s", url);
    pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, run, x)) {
        pthread_attr_destroy(&at); close(sv[0]); close(sv[1]);
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
