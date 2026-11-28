/* Minimal RFC 6455 WebSocket endpoint, both directions, blocking sockets.  No extensions, no subprotocols.  A client
 * may run over TLS (ws_connect_tls: Music Assistant behind a proxy that serves wss://; tls.c): then every read and
 * write goes through tls.c, which lets the one reader and the writers share the connection. */
#include "ws.h"
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "hash.h"
#include "net.h"
#include "netio.h"
#include "tls.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MAX_MESSAGE (4u << 20)

void ws_random(void *out, size_t len)
{
    int f = open("/dev/urandom", O_RDONLY);
    if (f < 0 || read(f, out, len) != (ssize_t)len) abort();        /* no randomness: nothing sane to do */
    close(f);
}

/* The socket, or TLS over it: N bytes (fewer: the end, an error or the receive timeout) */
static ssize_t w_read_full(int fd, struct tls *t, int ms, void *buf, size_t n)
{
    if (!t) return read_full(fd, buf, n);
    size_t got = 0;
    while (got < n) {
        ssize_t r = tls_read(t, (char *)buf + got, n - got, ms > 0 ? ms : -1);
        if (r <= 0) break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static int w_write_all(int fd, struct tls *t, const void *buf, size_t n)
{
    if (!t) return write_all(fd, buf, n);
    /* 5 s, as the plain socket's SO_SNDTIMEO (net_connect): every send runs under mass.c's wslk, which mass_set and
     * mass_stop wait for */
    return tls_write(t, buf, n, 5000) == (ssize_t)n ? 0 : -1;
}

static int read_headers(int fd, struct tls *t, char *buf, size_t cap)      /* up to and including the blank line */
{
    size_t n = 0;
    while (n < cap - 1) {
        if (w_read_full(fd, t, 5000, buf + n, 1) != 1) return -1;
        n++;
        if (n >= 4 && !memcmp(buf + n - 4, "\r\n\r\n", 4)) { buf[n] = 0; return 0; }
    }
    return -1;
}

static const char *header(const char *hdrs, const char *name, char *out, size_t outsz)
{
    size_t nl = strlen(name);
    for (const char *p = hdrs; (p = strstr(p, "\r\n")); ) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1; size_t i = 0;
            while (*v == ' ') v++;
            while (*v && *v != '\r' && i < outsz - 1) out[i++] = *v++;
            out[i] = 0;
            return out;
        }
    }
    return NULL;
}

static void accept_key(const char *key, char *out)
{
    char cat[128]; uint8_t d[20];
    snprintf(cat, sizeof cat, "%s" WS_GUID, key);
    sha1(cat, strlen(cat), d);
    b64_encode(d, 20, out, 0, 1);
}

static void ws_init(struct ws *w, int fd, int client, struct tls *t)
{
    memset(w, 0, sizeof *w);
    w->fd = fd; w->client = client; w->tls = t;
    pthread_mutex_init(&w->wlock, NULL);
}

int ws_accept(struct ws *w, int fd, char *path, size_t pathsz)
{
    char h[4096], key[128], acc[64], resp[256], origin[8];
    if (read_headers(fd, NULL, h, sizeof h) || strncmp(h, "GET ", 4) || !header(h, "Sec-WebSocket-Key", key, sizeof key)) return -1;
    /* Only a browser sends Origin (Sendspin servers never do; the spec has clients refuse it): a page open anywhere on
     * the LAN would otherwise reach the endpoint */
    if (header(h, "Origin", origin, sizeof origin)) {
        static const char no[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        write_all(fd, no, sizeof no - 1);
        return -1;
    }
    if (path) { size_t i = 0; for (const char *p = h + 4; *p && *p != ' ' && i < pathsz - 1; p++) path[i++] = *p; path[i] = 0; }
    accept_key(key, acc);
    int n = snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    if (write_all(fd, resp, n) < 0) return -1;
    ws_init(w, fd, 0, NULL);
    return 0;
}

static int connect_as(struct ws *w, const char *host, int port, const char *path, int secure, int verify, char *err, size_t cap)
{
    char ports[8], req[512], h[4096], key[32], want[64], got[64]; uint8_t nonce[16]; struct tls *t = NULL;
    snprintf(ports, sizeof ports, "%d", port);
    if (err && cap) err[0] = 0;
    int fd = net_connect(host, ports, 5);
    if (fd < 0) { if (err) snprintf(err, cap, "%s:%d does not answer", host, port); return -1; }
    if (secure) {
        char e[300];
        if (!(t = tls_open(fd, host, verify, tls_now_ms() + 10000, NULL, e, sizeof e))) { if (err) snprintf(err, cap, "%s", e); close(fd); return -1; }
    }
    ws_random(nonce, sizeof nonce); b64_encode(nonce, 16, key, 0, 1);
    int v6 = strchr(host, ':') != NULL;                         /* Host: [v6]:port */
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s%s%s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", path, v6 ? "[" : "", host, v6 ? "]" : "", port, key);
    accept_key(key, want);
    if (w_write_all(fd, t, req, n) < 0 || read_headers(fd, t, h, sizeof h) || !strstr(h, " 101") ||
        !header(h, "Sec-WebSocket-Accept", got, sizeof got) || strcmp(got, want)) {
        if (err) snprintf(err, cap, "%s:%d is no WebSocket server", host, port);
        tls_free(t); close(fd); return -1;
    }
    struct timeval none = { 0, 0 };                     /* an idle connection is fine once it is up */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
    ws_init(w, fd, 1, t);
    return 0;
}

int ws_connect(struct ws *w, const char *host, int port, const char *path) { return connect_as(w, host, port, path, 0, 0, NULL, 0); }

int ws_connect_tls(struct ws *w, const char *host, int port, const char *path, int verify, char *err, size_t cap)
{
    return connect_as(w, host, port, path, 1, verify, err, cap);
}

void ws_timeout(struct ws *w, int secs)
{
    struct timeval tv = { secs, 0 };
    w->rcv_ms = secs * 1000;
    setsockopt(w->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

static int send_frame(struct ws *w, int opcode, const void *data, size_t len)
{
    uint8_t h[14], mask[4]; size_t hl = 2; int rc = 0;
    h[0] = 0x80 | opcode;
    if (len < 126) h[1] = len; else if (len < 65536) { h[1] = 126; h[2] = len >> 8; h[3] = len; hl = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (uint64_t)len >> (56 - 8 * i); hl = 10; }
    pthread_mutex_lock(&w->wlock);
    if (w->client) {                                    /* client frames are masked */
        uint8_t *m = malloc(len ? len : 1);
        if (!m) { pthread_mutex_unlock(&w->wlock); return -1; }
        ws_random(mask, 4); h[1] |= 0x80; memcpy(h + hl, mask, 4); hl += 4;
        for (size_t i = 0; i < len; i++) m[i] = ((const uint8_t *)data)[i] ^ mask[i & 3];
        rc = w_write_all(w->fd, w->tls, h, hl) < 0 || (len && w_write_all(w->fd, w->tls, m, len) < 0) ? -1 : 0;
        free(m);
    } else rc = w_write_all(w->fd, w->tls, h, hl) < 0 || (len && w_write_all(w->fd, w->tls, data, len) < 0) ? -1 : 0;
    /* A frame not sent whole leaves the stream broken: the connection ends here (the reader sees its end), and no
     * later frame follows the piece (tls.c refuses one while a record is pending, a review 2026-11-26) */
    if (rc && w->fd >= 0) shutdown(w->fd, SHUT_RDWR);
    pthread_mutex_unlock(&w->wlock);
    return rc;
}

int ws_send(struct ws *w, int opcode, const void *data, size_t len) { return send_frame(w, opcode, data, len); }

#define WS_HEAD 256                                     /* what is kept of a message too big: enough for its id */

/* n bytes of the socket read and dropped: 0, or -1 */
static int skip(struct ws *w, uint64_t n)
{
    uint8_t b[4096];
    while (n) { size_t k = n < sizeof b ? (size_t)n : sizeof b; if (w_read_full(w->fd, w->tls, w->rcv_ms, b, k) != (ssize_t)k) return -1; n -= k; }
    return 0;
}

int ws_recv(struct ws *w, int *opcode, uint8_t **data, size_t *len)
{
    size_t total = 0; int first_op = 0, over = 0;
    for (;;) {
        uint8_t h[2], ext[8], mask[4]; uint64_t n;
        if (w_read_full(w->fd, w->tls, w->rcv_ms, h, 2) != 2) return 0;
        int fin = h[0] & 0x80, op = h[0] & 0x0f, masked = h[1] & 0x80;
        n = h[1] & 0x7f;
        if (n == 126) { if (w_read_full(w->fd, w->tls, w->rcv_ms, ext, 2) != 2) return 0; n = ext[0] << 8 | ext[1]; }
        else if (n == 127) { if (w_read_full(w->fd, w->tls, w->rcv_ms, ext, 8) != 8) return 0; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | ext[i]; }
        if (masked && w_read_full(w->fd, w->tls, w->rcv_ms, mask, 4) != 4) return 0;
        if (op >= 8 && n > 125) return -1;                /* a control frame that long is no frame */
        if (op < 8 && (over || n > WS_MAX_MESSAGE || total + n > WS_MAX_MESSAGE)) {   /* too big: read to its end, its head kept */
            if (n > 16 * (uint64_t)WS_MAX_MESSAGE || total + n > 16 * (uint64_t)WS_MAX_MESSAGE) return -1;   /* (no frame is that; the message may be any length) */
            if (total > WS_HEAD) total = WS_HEAD;       /* earlier fragments may have brought up to 4 MB: only the head stays */
            size_t keep = total < WS_HEAD ? WS_HEAD - total : 0; if (keep > n) keep = (size_t)n;
            if (total + keep + 1 > w->cap) { uint8_t *nb = realloc(w->buf, total + keep + 1); if (!nb) return -1; w->buf = nb; w->cap = total + keep + 1; }
            if (keep && w_read_full(w->fd, w->tls, w->rcv_ms, w->buf + total, keep) != (ssize_t)keep) return 0;
            if (masked) for (size_t i = 0; i < keep; i++) w->buf[total + i] ^= mask[i & 3];
            if (skip(w, n - keep)) return 0;
            total += keep; over = 1;
            if (op) first_op = op;
            if (fin) { w->buf[total] = 0; *opcode = first_op; *data = w->buf; *len = total; return 2; }
            continue;
        }
        if (total + n + 1 > w->cap) {
            size_t cap = total + n + 1 + 4096; uint8_t *nb = realloc(w->buf, cap);
            if (!nb) return -1;
            w->buf = nb; w->cap = cap;
        }
        uint8_t *dst = w->buf + total;
        if (n && w_read_full(w->fd, w->tls, w->rcv_ms, dst, n) != (ssize_t)n) return 0;
        if (masked) for (uint64_t i = 0; i < n; i++) dst[i] ^= mask[i & 3];

        if (op >= 8) {                                  /* control frames may interleave with a fragmented message */
            if (op == WS_PING) send_frame(w, WS_PONG, dst, n);
            else if (op == WS_CLOSE) { send_frame(w, WS_CLOSE, dst, n < 2 ? 0 : 2); return 0; }
            continue;
        }
        if (op) first_op = op;
        total += n;
        if (fin) { w->buf[total] = 0; *opcode = first_op; *data = w->buf; *len = total; return 1; }
    }
}

void ws_close(struct ws *w)
{
    if (w->fd >= 0) {
        send_frame(w, WS_CLOSE, "\x03\xe8", 2);
        tls_free(w->tls); w->tls = NULL;
        shutdown(w->fd, SHUT_RDWR); close(w->fd); w->fd = -1;
    }
    free(w->buf); w->buf = NULL; w->cap = 0;
    pthread_mutex_destroy(&w->wlock);
}
