/*
 * hassmic's TLS client (tls.h; issue #236), on mbedTLS 3.6 (src/third_party/mbedtls, configured by its
 * hassmic_config.h: a TLS 1.2 and 1.3 client and nothing else).
 *
 * Why not the firmware's any more: every Echo's libcurl runs on Android's BoringSSL of its firmware, Android 7's on
 * donut, biscuit, radar, cannoli, checkers, cronos, crown and cupcake (libssl.so has no TLS 1.3 code at all) and
 * Android 9's on athena (the draft variants only: SSL_set_tls13_variant, SSL_is_draft_downgrade).  A Home Assistant
 * on the "Modern" SSL profile (TLS 1.3 alone) refused every reply's audio with "SSL connect error"; a server of the
 * owner's for updates or the plugin index (sources.c) can be the same.  And that BoringSSL is 2016's, fixed by no one
 * since.
 *
 * The certificates: the system's CA folder (/system/etc/security/cacerts: Android's, one PEM file a root, 136 on the
 * Dots, with ISRG Root X1, USERTrust, DigiCert Global G2 and Amazon's roots valid past 2031; the stock image's own
 * list, as BoringSSL had it), else the PC's bundle; HASSMIC_CA_FILE (tests) in place of all of them.  Read once, at
 * the first connection that checks, and kept (~300 kB): parsing them is the work, not the checks.  Read-only after
 * that, so any thread verifies against them.
 *
 * Each connection is used by one reader and one writer at once (ws.c: Music Assistant's session thread reads while
 * the UI's requests are sent, one at a time under its wlock), which mbedTLS does not allow on one context.  So the socket is
 * non-blocking, every mbedTLS call is made under the connection's lock and returns at once (WANT_READ/WANT_WRITE),
 * and the waits for the socket are poll()s without the lock.  A shutdown() of the socket by another thread (mass.c
 * when its settings change) wakes the poll, and the next read sees the end.
 */
#include "tls.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
/* out_left (tls_write): whether the record a write left pending is still pending, which no public function says */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

/* mbedTLS's own words for a socket's failure (net_sockets.h, which is not built: the sockets are ours) */
#define ERR_SEND -0x004E
#define ERR_RECV -0x004C

#define SLICE_MS 250                    /* a wait's longest piece: STOP is looked at between them */

long long tls_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* ---------------------------------------------------------------- randomness: the kernel's */

static int urandom_fd = -1;
static pthread_once_t urandom_once = PTHREAD_ONCE_INIT;
static void urandom_open(void) { urandom_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); }

static int urandom(unsigned char *out, size_t n)
{
    pthread_once(&urandom_once, urandom_open);
    while (n) {
        ssize_t r = urandom_fd < 0 ? -1 : read(urandom_fd, out, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        out += r; n -= (size_t)r;
    }
    return 0;
}

/* PSA's (MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG): no entropy pool and DRBG of mbedTLS's own to seed and to lock */
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *ctx, uint8_t *out, size_t size, size_t *len)
{
    (void)ctx;
    if (urandom(out, size)) { *len = 0; return PSA_ERROR_INSUFFICIENT_ENTROPY; }
    *len = size;
    return PSA_SUCCESS;
}

/* the TLS layer's (mbedtls_ssl_conf_rng) */
static int rng(void *p, unsigned char *out, size_t n) { (void)p; return urandom(out, n) ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : 0; }

static pthread_once_t psa_once = PTHREAD_ONCE_INIT;
static psa_status_t psa_rc = PSA_ERROR_BAD_STATE;
static void psa_start(void) { psa_rc = psa_crypto_init(); }

/* ---------------------------------------------------------------- the CAs */

static pthread_mutex_t ca_lock = PTHREAD_MUTEX_INITIALIZER;
static mbedtls_x509_crt cas;
static int cas_loaded;                  /* ca_lock: 1 once some were read (then for good) */
static long long cas_tried;             /* ca_lock: when the last try found none (tried again a minute later) */
static char cas_from[160];

/* The chain to check against, or NULL (none readable: said once a minute) */
static mbedtls_x509_crt *ca_chain(void)
{
    static const char *const places[] = {
        "/system/etc/security/cacerts",             /* the Echos (Android's folder) */
        "/etc/ssl/certs/ca-certificates.crt",       /* the PC: Debian, Ubuntu, Arch */
        "/etc/pki/tls/certs/ca-bundle.crt",         /* Fedora */
        "/etc/ssl/cert.pem",                        /* Alpine, macOS */
    };
    mbedtls_x509_crt *r = NULL;
    pthread_mutex_lock(&ca_lock);
    if (cas_loaded) r = &cas;
    else if (!cas_tried || tls_now_ms() - cas_tried >= 60000) {
        const char *only = getenv("HASSMIC_CA_FILE");                   /* tests: their own CA, alone */
        cas_tried = tls_now_ms(); if (!cas_tried) cas_tried = 1;
        mbedtls_x509_crt_init(&cas);
        for (size_t i = 0; i < (only ? 1 : sizeof places / sizeof *places) && !cas_loaded; i++) {
            const char *p = only ? only : places[i]; struct stat st;
            if (stat(p, &st)) continue;
            /* (a positive answer counts the certificates left out, a negative one is a failure; what was read stays) */
            int rc = S_ISDIR(st.st_mode) ? mbedtls_x509_crt_parse_path(&cas, p) : mbedtls_x509_crt_parse_file(&cas, p);
            if (cas.raw.p) { cas_loaded = 1; snprintf(cas_from, sizeof cas_from, "%s%s", p, rc > 0 ? " (some left out)" : ""); }
        }
        if (cas_loaded) { r = &cas; fprintf(stderr, "tls: certificates from %s\n", cas_from); }
        else { mbedtls_x509_crt_free(&cas); fprintf(stderr, "tls: no CA certificates readable%s%s: no server can be checked\n", only ? " in " : "", only ? only : ""); }
    }
    pthread_mutex_unlock(&ca_lock);
    return r;
}

/* ---------------------------------------------------------------- one connection */

struct tls {
    int fd;
    pthread_mutex_t m;                  /* every mbedTLS call on ssl */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    int sys_errno;                      /* the socket's, when bio_send/bio_recv failed */
    int unclean;                        /* the peer closed the connection without close_notify */
    size_t wpend;                       /* m: the payload of the record a write left pending (WANT_WRITE), 0 none */
    const unsigned char *wptr;          /* m: where that payload began in the caller's buffer */
    char err[200];
};

static int bio_send(void *arg, const unsigned char *b, size_t n)
{
    struct tls *t = arg;
    ssize_t w = send(t->fd, b, n, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (w >= 0) return (int)w;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
    t->sys_errno = errno;
    return ERR_SEND;
}

static int bio_recv(void *arg, unsigned char *b, size_t n)
{
    struct tls *t = arg;
    ssize_t r = recv(t->fd, b, n, MSG_DONTWAIT);
    if (r >= 0) return (int)r;          /* 0: the connection's end (mbedTLS: MBEDTLS_ERR_SSL_CONN_EOF) */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    t->sys_errno = errno;
    return ERR_RECV;
}

static void say(struct tls *t, char *out, size_t cap, int rc, const char *what)
{
    char e[120];
    if (rc == ERR_SEND || rc == ERR_RECV) snprintf(e, sizeof e, "%s", strerror(t->sys_errno));
    else mbedtls_strerror(rc, e, sizeof e);
    snprintf(out, cap, "%s: %s (-0x%04x)", what, e, (unsigned)-rc);
}

/* Wait for the socket to be readable (want 1) or writable (2): 1 ready (or its end: the next call says which), 0
 * not within MS (-1: no limit), -1 an error */
static int wait_fd(int fd, int want, int ms)
{
    struct pollfd p = { .fd = fd, .events = want == 2 ? POLLOUT : POLLIN };
    for (;;) {
        int r = poll(&p, 1, ms);
        if (r < 0 && errno == EINTR) continue;
        return r < 0 ? -1 : r > 0;
    }
}

static int is_address(const char *h)
{
    unsigned char b[16];
    return inet_pton(AF_INET, h, b) == 1 || inet_pton(AF_INET6, h, b) == 1;
}

struct tls *tls_open(int fd, const char *host, int verify, long long deadline_ms, const volatile int *stop, char *err, size_t cap)
{
    struct tls *t; int rc; mbedtls_x509_crt *ca = NULL;
    /* An address has no name to send (RFC 6066 3: no literal addresses in SNI) and mbedTLS checks the name only
     * where one was set, so an address's certificate is checked here after the handshake, against its IP entries */
    int addr = is_address(host);
    pthread_once(&psa_once, psa_start);
    if (psa_rc != PSA_SUCCESS) { snprintf(err, cap, "TLS: PSA crypto did not start (%d)", (int)psa_rc); return NULL; }
    if (verify && !(ca = ca_chain())) { snprintf(err, cap, "TLS: no CA certificates to check %s against", host); return NULL; }
    if (!(t = calloc(1, sizeof *t))) { snprintf(err, cap, "TLS: out of memory"); return NULL; }
    t->fd = fd;
    pthread_mutex_init(&t->m, NULL);
    mbedtls_ssl_init(&t->ssl); mbedtls_ssl_config_init(&t->conf);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if ((rc = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)))
        { say(t, err, cap, rc, "TLS setup"); goto fail; }
    mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&t->conf, rng, NULL);
    mbedtls_ssl_conf_authmode(&t->conf, !verify ? MBEDTLS_SSL_VERIFY_NONE : addr ? MBEDTLS_SSL_VERIFY_OPTIONAL : MBEDTLS_SSL_VERIFY_REQUIRED);
    if (ca) mbedtls_ssl_conf_ca_chain(&t->conf, ca, NULL);
    if ((rc = mbedtls_ssl_setup(&t->ssl, &t->conf))) { say(t, err, cap, rc, "TLS setup"); goto fail; }
    if ((rc = mbedtls_ssl_set_hostname(&t->ssl, addr ? NULL : host))) { say(t, err, cap, rc, "TLS: the server's name"); goto fail; }
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
    for (;;) {
        rc = mbedtls_ssl_handshake(&t->ssl);       /* (no other thread has t yet: no lock) */
        if (!rc) break;
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* (BAD_CERTIFICATE: one mbedTLS refused before the chain, its key usage or serverAuth missing; the
             * reason is in the verify result too) */
            uint32_t vr = mbedtls_ssl_get_verify_result(&t->ssl);
            if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || (rc == MBEDTLS_ERR_SSL_BAD_CERTIFICATE && vr && vr != (uint32_t)-1)) {
                char info[160]; uint32_t f = vr;
                if (mbedtls_x509_crt_verify_info(info, sizeof info, "", f) < 0) info[0] = 0;
                info[strcspn(info, "\n")] = 0;
                snprintf(err, cap, "the certificate of %s does not check out: %s", host, info[0] ? info : "?");
            } else say(t, err, cap, rc, "TLS handshake");
            goto fail;
        }
        if (stop && *stop) { snprintf(err, cap, "cancelled"); goto fail; }
        long long left = deadline_ms - tls_now_ms();
        if (left <= 0) { snprintf(err, cap, "TLS handshake with %s: no answer in time", host); goto fail; }
        if (wait_fd(fd, rc == MBEDTLS_ERR_SSL_WANT_WRITE ? 2 : 1, left < SLICE_MS ? (int)left : SLICE_MS) < 0)
            { snprintf(err, cap, "TLS handshake: %s", strerror(errno)); goto fail; }
    }
    if (verify && addr) {                       /* the chain again, with the address as the name to find */
        const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&t->ssl); uint32_t f = 0;
        /* mbedTLS's own result under VERIFY_OPTIONAL as well: key usage and serverAuth are checked there, not in
         * mbedtls_x509_crt_verify (with no name set, a good certificate has no flag in it) */
        f = mbedtls_ssl_get_verify_result(&t->ssl);
        if (!peer || f || mbedtls_x509_crt_verify((mbedtls_x509_crt *)peer, ca, NULL, host, &f, NULL, NULL) || f) {
            char info[160] = "";
            if (!peer) snprintf(info, sizeof info, "none sent");
            else if (mbedtls_x509_crt_verify_info(info, sizeof info, "", f) < 0) info[0] = 0;
            info[strcspn(info, "\n")] = 0;
            snprintf(err, cap, "the certificate of %s does not check out: %s", host, info[0] ? info : "?");
            goto fail;
        }
    }
    return t;
fail:
    mbedtls_ssl_free(&t->ssl); mbedtls_ssl_config_free(&t->conf);
    pthread_mutex_destroy(&t->m); free(t);
    return NULL;
}

ssize_t tls_read(struct tls *t, void *buf, size_t n, int timeout_ms)
{
    long long until = timeout_ms < 0 ? 0 : tls_now_ms() + timeout_ms;
    if (n > 1 << 30) n = 1 << 30;
    for (;;) {
        pthread_mutex_lock(&t->m);
        int rc = mbedtls_ssl_read(&t->ssl, buf, n);
        if (rc < 0 && rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE && rc != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY &&
            rc != MBEDTLS_ERR_SSL_CONN_EOF && rc != MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            say(t, t->err, sizeof t->err, rc, "TLS read");
        pthread_mutex_unlock(&t->m);
        if (rc > 0) return rc;
        /* the end: close_notify, or the connection closed without it (HTTP knows its length where it matters) */
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF) { t->unclean = rc != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY; return 0; }
        if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;     /* a TLS 1.3 ticket: not kept */
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) return -1;
        int ms = -1;
        if (timeout_ms >= 0) { long long left = until - tls_now_ms(); if (left <= 0) return -2; ms = (int)left; }
        int w = wait_fd(t->fd, rc == MBEDTLS_ERR_SSL_WANT_WRITE ? 2 : 1, ms);
        if (w < 0) { snprintf(t->err, sizeof t->err, "TLS read: %s", strerror(errno)); return -1; }
        if (!w) return -2;
    }
}

/* A record mbedTLS could not send at once stays in its buffer (out_left), and the next mbedtls_ssl_write with the same
 * data sends the rest and counts the record's payload as written.  But the reader may send it first: mbedtls_ssl_read
 * flushes what is pending before it answers a TLS 1.3 ticket or sends an alert.  A write that then came again with
 * the same data would find nothing pending and send that payload a second time (a review, 2026-11-26).  So the
 * payload of a pending record is kept (wpend, wptr), and once out_left is 0 it is counted as written without asking
 * mbedTLS again.  Only for the same write: the caller goes on from where that record begins (http.c) or gives the
 * connection up (ws.c); any other write while one is pending is refused, else a shorter one would be counted past its
 * end (a review, 2026-11-26: the next ping's 6 bytes against a 4 kB record). */
ssize_t tls_write(struct tls *t, const void *buf, size_t n, int timeout_ms)
{
    const unsigned char *p = buf; size_t left = n;
    long long until = timeout_ms < 0 ? 0 : tls_now_ms() + timeout_ms;
    while (left) {
        int rc;
        pthread_mutex_lock(&t->m);
        if (t->wpend && (p != t->wptr || t->wpend > left)) {
            snprintf(t->err, sizeof t->err, "TLS write: an earlier write was left unfinished");
            pthread_mutex_unlock(&t->m);
            return -1;
        }
        if (t->wpend && !t->ssl.MBEDTLS_PRIVATE(out_left)) rc = (int)t->wpend;   /* sent by the reader meanwhile */
        else if (!t->wpend && t->ssl.MBEDTLS_PRIVATE(out_left)) {
            /* a record the reader left half sent (an alert it queued): mbedtls_ssl_write would take it for ours, flush
             * it and answer our length with nothing of ours framed (ssl_write_real's out_left branch).  Out first,
             * with a write of nothing (that branch flushes and answers 0), then ours */
            rc = mbedtls_ssl_write(&t->ssl, p, 0);
            if (rc == 0) { pthread_mutex_unlock(&t->m); continue; }
        }
        else {
            size_t max = (size_t)mbedtls_ssl_get_max_out_record_payload(&t->ssl), had = t->wpend;
            rc = mbedtls_ssl_write(&t->ssl, p, left);
            /* a pending record flushed answers the length asked now, though only what was framed then went out */
            if (had && rc > (int)had) rc = (int)had;
            if (rc == MBEDTLS_ERR_SSL_WANT_WRITE && !t->wpend) { t->wpend = left < max ? left : max; t->wptr = p; }
        }
        if (rc > 0) { t->wpend = 0; t->wptr = NULL; }
        if (rc < 0 && rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) say(t, t->err, sizeof t->err, rc, "TLS write");
        pthread_mutex_unlock(&t->m);
        if (rc > 0) { p += rc; left -= (size_t)rc; until = timeout_ms < 0 ? 0 : tls_now_ms() + timeout_ms; continue; }
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) return -1;
        int ms = -1;
        if (timeout_ms >= 0) { long long l = until - tls_now_ms(); if (l <= 0) return n - left ? (ssize_t)(n - left) : -2; ms = (int)l; }
        int w = wait_fd(t->fd, rc == MBEDTLS_ERR_SSL_WANT_READ ? 1 : 2, ms);
        if (w < 0) { snprintf(t->err, sizeof t->err, "TLS write: %s", strerror(errno)); return -1; }
        if (!w) return n - left ? (ssize_t)(n - left) : -2;
    }
    return (ssize_t)n;
}

const char *tls_error(struct tls *t) { return t->err; }

const char *tls_version(struct tls *t) { return mbedtls_ssl_get_version(&t->ssl); }

void tls_free(struct tls *t)
{
    if (!t) return;
    pthread_mutex_lock(&t->m);
    mbedtls_ssl_close_notify(&t->ssl);          /* non-blocking: what fits now, else nothing */
    pthread_mutex_unlock(&t->m);
    mbedtls_ssl_free(&t->ssl); mbedtls_ssl_config_free(&t->conf);
    pthread_mutex_destroy(&t->m);
    free(t);
}

int tls_closed_clean(struct tls *t) { return !t->unclean; }
