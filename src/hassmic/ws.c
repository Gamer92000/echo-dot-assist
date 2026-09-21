/* Minimal RFC 6455 WebSocket endpoint, both directions, blocking sockets.  No extensions, no subprotocols. */
#include "ws.h"
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include "hash.h"
#include "netio.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MAX_MESSAGE (4u << 20)

void ws_random(void *out, size_t len)
{
    int f = open("/dev/urandom", O_RDONLY);
    if (f < 0 || read(f, out, len) != (ssize_t)len) abort();        /* no randomness: nothing sane to do */
    close(f);
}

static int read_headers(int fd, char *buf, size_t cap)      /* up to and including the blank line */
{
    size_t n = 0;
    while (n < cap - 1) {
        if (read(fd, buf + n, 1) != 1) return -1;
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

static void ws_init(struct ws *w, int fd, int client)
{
    memset(w, 0, sizeof *w);
    w->fd = fd; w->client = client;
    pthread_mutex_init(&w->wlock, NULL);
}

int ws_accept(struct ws *w, int fd, char *path, size_t pathsz)
{
    char h[4096], key[128], acc[64], resp[256];
    if (read_headers(fd, h, sizeof h) || strncmp(h, "GET ", 4) || !header(h, "Sec-WebSocket-Key", key, sizeof key)) return -1;
    if (path) { size_t i = 0; for (const char *p = h + 4; *p && *p != ' ' && i < pathsz - 1; p++) path[i++] = *p; path[i] = 0; }
    accept_key(key, acc);
    int n = snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    if (write_all(fd, resp, n) < 0) return -1;
    ws_init(w, fd, 0);
    return 0;
}

int ws_connect(struct ws *w, const char *host, int port, const char *path)
{
    char ports[8], req[512], h[4096], key[32], want[64], got[64]; uint8_t nonce[16]; struct addrinfo hints = { 0 }, *ai;
    snprintf(ports, sizeof ports, "%d", port);
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ports, &hints, &ai)) return -1;
    int fd = socket(ai->ai_family, SOCK_STREAM, 0);
    struct timeval tv = { 5, 0 };
    if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen)) { if (fd >= 0) close(fd); freeaddrinfo(ai); return -1; }
    freeaddrinfo(ai);
    ws_random(nonce, sizeof nonce); b64_encode(nonce, 16, key, 0, 1);
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", path, host, port, key);
    accept_key(key, want);
    if (write_all(fd, req, n) < 0 || read_headers(fd, h, sizeof h) || !strstr(h, " 101") ||
        !header(h, "Sec-WebSocket-Accept", got, sizeof got) || strcmp(got, want)) { close(fd); return -1; }
    ws_init(w, fd, 1);
    return 0;
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
        rc = write_all(w->fd, h, hl) < 0 || (len && write_all(w->fd, m, len) < 0) ? -1 : 0;
        free(m);
    } else rc = write_all(w->fd, h, hl) < 0 || (len && write_all(w->fd, data, len) < 0) ? -1 : 0;
    pthread_mutex_unlock(&w->wlock);
    return rc;
}

int ws_send(struct ws *w, int opcode, const void *data, size_t len) { return send_frame(w, opcode, data, len); }

int ws_recv(struct ws *w, int *opcode, uint8_t **data, size_t *len)
{
    size_t total = 0; int first_op = 0;
    for (;;) {
        uint8_t h[2], ext[8], mask[4]; uint64_t n;
        if (read_full(w->fd, h, 2) != 2) return 0;
        int fin = h[0] & 0x80, op = h[0] & 0x0f, masked = h[1] & 0x80;
        n = h[1] & 0x7f;
        if (n == 126) { if (read_full(w->fd, ext, 2) != 2) return 0; n = ext[0] << 8 | ext[1]; }
        else if (n == 127) { if (read_full(w->fd, ext, 8) != 8) return 0; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | ext[i]; }
        if (masked && read_full(w->fd, mask, 4) != 4) return 0;
        if (n > WS_MAX_MESSAGE || total + n > WS_MAX_MESSAGE) return -1;
        if (total + n + 1 > w->cap) {
            size_t cap = total + n + 1 + 4096; uint8_t *nb = realloc(w->buf, cap);
            if (!nb) return -1;
            w->buf = nb; w->cap = cap;
        }
        uint8_t *dst = w->buf + total;
        if (n && read_full(w->fd, dst, n) != (ssize_t)n) return 0;
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
    if (w->fd >= 0) { send_frame(w, WS_CLOSE, "\x03\xe8", 2); shutdown(w->fd, SHUT_RDWR); close(w->fd); w->fd = -1; }
    free(w->buf); w->buf = NULL; w->cap = 0;
    pthread_mutex_destroy(&w->wlock);
}
