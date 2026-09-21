#include "wyoming.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

int wy_reader_init(struct wy_reader *r, int fd)
{
    r->fd = fd;
    r->buf = malloc(WY_MAX_PAYLOAD);
    return r->buf ? 0 : -1;
}

void wy_reader_free(struct wy_reader *r) { free(r->buf); r->buf = NULL; }

static int read_n(int fd, void *dst, size_t n)
{
    char *p = dst;
    while (n) {
        ssize_t got = read(fd, p, n);
        if (got < 0) { if (errno == EINTR) continue; return -1; }
        if (got == 0) return 0;
        p += got; n -= got;
    }
    return 1;
}

int wy_read(struct wy_reader *r, struct wy_event *ev)
{
    size_t len = 0;
    for (;;) {                                   /* header line; byte-wise is fine at this message rate */
        char c; int rc = read_n(r->fd, &c, 1);
        if (rc <= 0) return rc;
        if (c == '\n') break;
        if (len + 1 >= WY_MAX_JSON) return -1;
        ev->json[len++] = c;
    }
    ev->json[len] = 0;
    if (!wy_json_str(ev->json, "type", ev->type, sizeof ev->type)) return -1;

    long data_len = 0, payload_len = 0;
    wy_json_int(ev->json, "data_length", &data_len);
    wy_json_int(ev->json, "payload_length", &payload_len);
    if (data_len < 0 || payload_len < 0 || payload_len > WY_MAX_PAYLOAD) return -1;
    if (data_len) {
        if (len + 1 + data_len >= WY_MAX_JSON) return -1;
        ev->json[len++] = '\n';
        int rc = read_n(r->fd, ev->json + len, data_len);
        if (rc <= 0) return rc;
        ev->json[len + data_len] = 0;
    }
    ev->payload = r->buf; ev->payload_len = payload_len;
    if (payload_len) { int rc = read_n(r->fd, r->buf, payload_len); if (rc <= 0) return rc; }
    return 1;
}

int wy_write(int fd, const char *type, const char *data, const void *payload, size_t payload_len)
{
    char head[256]; size_t data_len = data ? strlen(data) : 0;
    int n = snprintf(head, sizeof head, "{\"type\":\"%s\",\"version\":\"1.5.4\"", type);
    if (data_len)    n += snprintf(head + n, sizeof head - n, ",\"data_length\":%zu", data_len);
    if (payload_len) n += snprintf(head + n, sizeof head - n, ",\"payload_length\":%zu", payload_len);
    n += snprintf(head + n, sizeof head - n, "}\n");

    struct iovec iov[3] = { { head, n }, { (void *)data, data_len }, { (void *)payload, payload_len } };
    int cnt = 3, i = 0;
    while (i < cnt) {
        if (!iov[i].iov_len) { i++; continue; }
        ssize_t w = writev(fd, iov + i, cnt - i);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        while (w > 0 && i < cnt) {
            if ((size_t)w >= iov[i].iov_len) { w -= iov[i].iov_len; iov[i].iov_len = 0; i++; }
            else { iov[i].iov_base = (char *)iov[i].iov_base + w; iov[i].iov_len -= w; w = 0; }
        }
    }
    return 0;
}

static const char *find_key(const char *json, const char *key)
{
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat))) {
        p += strlen(pat);
        while (*p == ' ') p++;
        if (*p == ':') { p++; while (*p == ' ') p++; return p; }
    }
    return NULL;
}

int wy_json_int(const char *json, const char *key, long *out)
{
    const char *p = find_key(json, key); char *end;
    if (!p) return 0;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = v; return 1;
}

int wy_json_str(const char *json, const char *key, char *out, size_t outsz)
{
    const char *p = find_key(json, key); size_t n = 0;
    if (!p || *p != '"') return 0;
    for (p++; *p && *p != '"' && n + 1 < outsz; p++) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p;
    }
    out[n] = 0; return 1;
}
