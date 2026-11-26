/*
 * hassmic's HTTP/1.1 client (http.h; issue #236), on its own TLS (tls.c).  It replaces the firmware's libcurl 7.50.1,
 * whose BoringSSL (Android 7's; athena's Android 9 has only TLS 1.3's drafts) cannot talk to a server that offers
 * TLS 1.3 alone: Home Assistant on its "Modern" SSL profile, and any server of the owner's for updates or plugins.
 *
 * What the callers needed of libcurl, and nothing more: GET and POST with headers of the caller's, redirects (HTTPS
 * only where the caller says; GitHub's release assets go to its CDN), bodies by Content-Length, chunks or the
 * connection's end, a time for each connection, one for the whole and libcurl's "low speed limit", a stop from
 * another thread, the sockets opened as the egress lock wants them (net_socket: any address; a plain socket(): the
 * local network only), ".local" names by our own mDNS (net.c: neither bionic's resolver nor libcurl asks it).
 *
 * One request per connection (Connection: close): none of the callers asks one server twice in a row often enough for
 * keeping it to matter, and a kept connection is state to get wrong.  A redirect to another origin (scheme, host or
 * port) carries none of the caller's headers: they are tokens (Amazon's) or for the first server alone (libcurl
 * 7.50.1 sent them along).
 */
#include "http.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include "net.h"
#include "tls.h"

#define SLICE_MS   250                  /* the longest wait between looks at STOP and the clocks */
#define LINE_MAX_  8192                 /* a header line kept (longer ones are read and dropped: we need none of them) */
#define HEAD_MAX   (64 << 10)           /* all of a response's header lines */
#define URL_MAX    2048

/* ---------------------------------------------------------------- addresses */

struct url { int https; char host[256], port[8], hostport[270]; char path[URL_MAX]; };

static int bad_char(const char *s, size_t n)            /* in an address: nothing that ends a request line */
{
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] <= ' ' || s[i] == 0x7f) return 1;
    return 0;
}

static int bad_header(const char *s)                    /* a header line of the caller's: no control character but tab */
{
    for (; *s; s++) if (((unsigned char)*s < ' ' && *s != '\t') || *s == 0x7f) return 1;
    return 0;
}

static int url_parse(const char *s, struct url *u, char *err, size_t cap)
{
    const char *p, *a, *e, *colon = NULL;
    memset(u, 0, sizeof *u);
    if (!strncasecmp(s, "https://", 8)) { u->https = 1; p = s + 8; }
    else if (!strncasecmp(s, "http://", 7)) p = s + 7;
    else { snprintf(err, cap, "not an http:// or https:// address"); return -1; }
    e = p + strcspn(p, "/?#");
    if (memchr(p, '@', (size_t)(e - p))) { snprintf(err, cap, "an address with a user or password in it"); return -1; }
    a = p;
    if (*a == '[') {                                    /* [v6]:port */
        const char *rb = memchr(a, ']', (size_t)(e - a));
        if (!rb || rb - a - 1 <= 0 || (size_t)(rb - a - 1) >= sizeof u->host) { snprintf(err, cap, "a bad IPv6 address"); return -1; }
        memcpy(u->host, a + 1, (size_t)(rb - a - 1));
        if (rb + 1 < e) { if (rb[1] != ':') { snprintf(err, cap, "a bad address"); return -1; } colon = rb + 1; }
    } else {
        colon = memchr(a, ':', (size_t)(e - a));
        size_t hl = (size_t)((colon ? colon : e) - a);
        if (!hl || hl >= sizeof u->host) { snprintf(err, cap, "no host in the address"); return -1; }
        memcpy(u->host, a, hl);
    }
    if (!u->host[0] || bad_char(u->host, strlen(u->host))) { snprintf(err, cap, "a bad host in the address"); return -1; }
    if (colon) {
        size_t pl = (size_t)(e - colon - 1); long v = 0;
        if (!pl || pl > 5) { snprintf(err, cap, "a bad port in the address"); return -1; }
        for (size_t i = 0; i < pl; i++) { if (!isdigit((unsigned char)colon[1 + i])) { snprintf(err, cap, "a bad port in the address"); return -1; } v = v * 10 + colon[1 + i] - '0'; }
        if (v < 1 || v > 65535) { snprintf(err, cap, "a bad port in the address"); return -1; }
        snprintf(u->port, sizeof u->port, "%ld", v);
    } else snprintf(u->port, sizeof u->port, "%s", u->https ? "443" : "80");
    int v6 = strchr(u->host, ':') != NULL, deflt = !strcmp(u->port, u->https ? "443" : "80");
    snprintf(u->hostport, sizeof u->hostport, "%s%s%s%s%s", v6 ? "[" : "", u->host, v6 ? "]" : "", deflt ? "" : ":", deflt ? "" : u->port);
    size_t pl = strcspn(e, "#");
    if (pl + 2 > sizeof u->path) { snprintf(err, cap, "the address is too long"); return -1; }
    if (bad_char(e, pl)) { snprintf(err, cap, "a space or control character in the address"); return -1; }
    snprintf(u->path, sizeof u->path, "%s%.*s", *e == '/' ? "" : "/", (int)pl, e);
    return 0;
}

/* LOC (a Location header's value) against the request's address BASE, into OUT */
static int url_join(const struct url *base, const char *loc, char *out, size_t cap)
{
    const char *scheme = base->https ? "https" : "http";
    if (!strncasecmp(loc, "https://", 8) || !strncasecmp(loc, "http://", 7)) return snprintf(out, cap, "%s", loc) >= (int)cap ? -1 : 0;
    if (loc[0] == '/' && loc[1] == '/') return snprintf(out, cap, "%s:%s", scheme, loc) >= (int)cap ? -1 : 0;
    if (loc[0] == '/') return snprintf(out, cap, "%s://%s%s", scheme, base->hostport, loc) >= (int)cap ? -1 : 0;
    size_t dir = strcspn(base->path, "?");                         /* relative: beside the request's own path */
    while (dir && base->path[dir - 1] != '/') dir--;
    return snprintf(out, cap, "%s://%s%.*s%s", scheme, base->hostport, (int)dir, base->path, loc) >= (int)cap ? -1 : 0;
}

/* ---------------------------------------------------------------- the clocks */

/* The low speed limit over the last LOW_S, as a moving window: what came in each quarter second, the last 64 s */
#define TICK_MS 250
#define TICKS   256

struct run {
    struct http *r;
    long long deadline;                 /* the whole's, 0 none */
    long long start;                    /* when it began (the window counts from there) */
    long long tick;                     /* the newest quarter second in ticks[] */
    long long ticks[TICKS];             /* bytes per quarter second, ring */
    char *err; size_t cap;
    int why;                            /* what ended it: HTTP_FAILED, HTTP_CANCELLED */
};

static void tick_to(struct run *x, long long now)      /* the ring moved on to NOW's quarter second */
{
    long long t = (now - x->start) / TICK_MS;
    if (t - x->tick >= TICKS) { memset(x->ticks, 0, sizeof x->ticks); x->tick = t; }
    while (x->tick < t) x->ticks[++x->tick % TICKS] = 0;
}

static void came(struct run *x, long long n) { tick_to(x, tls_now_ms()); x->ticks[x->tick % TICKS] += n; }

/* 0 to go on, -1 (ERR said) when STOP, the whole's time or the low speed limit says it is over */
static int clocks(struct run *x)
{
    long long now = tls_now_ms();
    if (x->r->stop && *x->r->stop) { snprintf(x->err, x->cap, "cancelled"); x->why = HTTP_CANCELLED; return -1; }
    if (x->deadline && now >= x->deadline) { snprintf(x->err, x->cap, "no answer within %d s", x->r->total_s); return -1; }
    int low = x->r->low_s > 64 ? 64 : x->r->low_s;
    if (low > 0 && now - x->start >= low * 1000LL) {
        long long sum = 0; int n = low * 1000 / TICK_MS;
        tick_to(x, now);
        for (int i = 0; i < n; i++) sum += x->ticks[(x->tick - i + TICKS) % TICKS];
        if (sum < x->r->low_bytes * low) {
            if (x->r->low_bytes <= 1) snprintf(x->err, x->cap, "nothing came for %d s", low);
            else snprintf(x->err, x->cap, "slower than %ld bytes a second for %d s", x->r->low_bytes, low);
            return -1;
        }
    }
    return 0;
}

static int slice(struct run *x)                         /* the next wait's length */
{
    long long ms = SLICE_MS, now = tls_now_ms();
    if (x->deadline && x->deadline - now < ms) ms = x->deadline - now;
    return ms < 1 ? 1 : (int)ms;
}

/* ---------------------------------------------------------------- the connection */

struct conn { int fd; struct tls *t; unsigned char buf[16384]; size_t off, have; int eof; char line[LINE_MAX_]; };

static int connect_one(struct run *x, const struct sockaddr *a, socklen_t al, int family)
{
    int fd = x->r->lan ? socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0) : net_socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { snprintf(x->err, x->cap, "socket: %s", strerror(errno)); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (!connect(fd, a, al)) return fd;
    if (errno != EINPROGRESS) { snprintf(x->err, x->cap, "connect: %s", strerror(errno)); close(fd); return -1; }
    long long until = tls_now_ms() + (x->r->connect_s > 0 ? x->r->connect_s : 30) * 1000LL;
    for (;;) {
        if (clocks(x)) { close(fd); return -1; }
        long long left = until - tls_now_ms();
        if (left <= 0) { snprintf(x->err, x->cap, "connect: no answer within %d s", x->r->connect_s); close(fd); return -1; }
        struct pollfd p = { .fd = fd, .events = POLLOUT }; int s = slice(x);
        int r = poll(&p, 1, left < s ? (int)left : s);
        if (r < 0 && errno != EINTR) { snprintf(x->err, x->cap, "connect: %s", strerror(errno)); close(fd); return -1; }
        if (r > 0) {
            int e = 0; socklen_t el = sizeof e;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) || e) { snprintf(x->err, x->cap, "connect: %s", strerror(e ? e : errno)); close(fd); return -1; }
            return fd;
        }
    }
}

static int conn_open(struct run *x, const struct url *u, struct conn *c)
{
    struct addrinfo hints = { 0 }, *ai = NULL, *p; struct sockaddr_in sin = { 0 }; int fd = -1, one = 1;
    size_t hl = strlen(u->host);
    c->fd = -1; c->t = NULL; c->off = c->have = 0; c->eof = 0;
    if (hl > 6 && !strcasecmp(u->host + hl - 6, ".local")) {    /* homeassistant.local: mDNS first */
        unsigned a = mdns_resolve4(u->host);
        if (a) { sin.sin_family = AF_INET; sin.sin_port = htons((unsigned short)atoi(u->port)); sin.sin_addr.s_addr = a;
                 fd = connect_one(x, (struct sockaddr *)&sin, sizeof sin, AF_INET); }
        if (fd < 0 && x->why == HTTP_CANCELLED) return -1;
    }
    if (fd < 0) {                       /* (also a ".local" nobody answered for: some DNS servers serve it) */
        hints.ai_socktype = SOCK_STREAM;
        int g = getaddrinfo(u->host, u->port, &hints, &ai);
        if (g) { snprintf(x->err, x->cap, "cannot resolve %s: %s", u->host, gai_strerror(g)); return -1; }
        /* every address in turn: an unreachable one (a global IPv6 behind the egress lock) must not hide one that works */
        for (p = ai; p && fd < 0; p = p->ai_next) {
            fd = connect_one(x, p->ai_addr, p->ai_addrlen, p->ai_family);
            if (fd < 0 && (x->why == HTTP_CANCELLED || (x->deadline && tls_now_ms() >= x->deadline))) break;
        }
        freeaddrinfo(ai);
        if (fd < 0) { char e[200]; snprintf(e, sizeof e, "%s", x->err); snprintf(x->err, x->cap, "%s:%s: %s", u->host, u->port, e); return -1; }
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    c->fd = fd;
    if (u->https) {
        long long until = tls_now_ms() + (x->r->connect_s > 0 ? x->r->connect_s : 30) * 1000LL;
        if (x->deadline && x->deadline < until) until = x->deadline;
        char e[300];
        if (!(c->t = tls_open(fd, u->host, x->r->verify, until, x->r->stop, e, sizeof e))) {
            if (x->r->stop && *x->r->stop) x->why = HTTP_CANCELLED;
            snprintf(x->err, x->cap, "%s", e);
            close(fd); c->fd = -1; return -1;
        }
    }
    return 0;
}

static void conn_close(struct conn *c)
{
    if (c->t) { tls_free(c->t); c->t = NULL; }
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
}

static int conn_write(struct run *x, struct conn *c, const void *d, size_t n)
{
    const char *p = d;
    while (n) {
        if (clocks(x)) return -1;
        ssize_t w;
        if (c->t) {
            w = tls_write(c->t, p, n, slice(x));
            if (w == -2) continue;                      /* nothing taken in this slice: the clocks decide */
            if (w < 0) { snprintf(x->err, x->cap, "%s", tls_error(c->t)); return -1; }
        } else {
            struct pollfd pf = { .fd = c->fd, .events = POLLOUT };
            int r = poll(&pf, 1, slice(x));
            if (r < 0 && errno != EINTR) { snprintf(x->err, x->cap, "send: %s", strerror(errno)); return -1; }
            if (r <= 0) continue;
            w = send(c->fd, p, n, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (w < 0) { if (errno == EAGAIN || errno == EINTR) continue; snprintf(x->err, x->cap, "send: %s", strerror(errno)); return -1; }
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* more into c->buf: 1 some came, 0 the end, -1 a failure (ERR) */
static int conn_fill(struct run *x, struct conn *c)
{
    if (c->eof) return 0;
    if (c->off == c->have) c->off = c->have = 0;
    else if (c->off) { memmove(c->buf, c->buf + c->off, c->have - c->off); c->have -= c->off; c->off = 0; }
    if (c->have == sizeof c->buf) return 1;
    for (;;) {
        if (clocks(x)) return -1;
        ssize_t r;
        if (c->t) {
            r = tls_read(c->t, c->buf + c->have, sizeof c->buf - c->have, slice(x));
            if (r == -2) continue;
            if (r < 0) { snprintf(x->err, x->cap, "%s", tls_error(c->t)); return -1; }
        } else {
            struct pollfd pf = { .fd = c->fd, .events = POLLIN };
            int pr = poll(&pf, 1, slice(x));
            if (pr < 0 && errno != EINTR) { snprintf(x->err, x->cap, "recv: %s", strerror(errno)); return -1; }
            if (pr <= 0) continue;
            r = recv(c->fd, c->buf + c->have, sizeof c->buf - c->have, MSG_DONTWAIT);
            if (r < 0) { if (errno == EAGAIN || errno == EINTR) continue; snprintf(x->err, x->cap, "recv: %s", strerror(errno)); return -1; }
        }
        if (!r) { c->eof = 1; return 0; }
        c->have += (size_t)r; came(x, r);
        return 1;
    }
}

/* One line (CRLF or LF, without it) into OUT, cut to CAP - 1 (then 1); 0, or -1 (ERR; the end before a line ended
 * counts) */
static int conn_line(struct run *x, struct conn *c, char *out, size_t cap, size_t *total)
{
    size_t n = 0; int cut = 0;
    for (;;) {
        while (c->off < c->have) {
            char ch = (char)c->buf[c->off++];
            if (++*total > HEAD_MAX) { snprintf(x->err, x->cap, "the answer's headers are longer than %d kB", HEAD_MAX >> 10); return -1; }
            if (ch == '\n') { if (n && out[n - 1] == '\r' && !cut) n--; out[n] = 0; return cut; }
            if (n + 1 < cap) out[n++] = ch; else cut = 1;
        }
        int f = conn_fill(x, c);
        if (f < 0) return -1;
        if (!f) { snprintf(x->err, x->cap, "the connection ended inside the answer's headers"); return -1; }
    }
}

/* ---------------------------------------------------------------- one exchange */

struct answer { int status; long long length; int chunked, location_cut; char location[URL_MAX]; };

static int read_head(struct run *x, struct conn *c, struct answer *a)
{
    char *line = c->line; const size_t lcap = sizeof c->line;
    size_t total = 0;
    for (;;) {                                          /* 1xx answers (100 Continue) come before the one */
        int maj, mnr, st;
        a->status = 0; a->length = -1; a->chunked = 0; a->location_cut = 0; a->location[0] = 0;
        if (conn_line(x, c, line, lcap, &total) < 0) return -1;
        if (sscanf(line, "HTTP/%d.%d %3d", &maj, &mnr, &st) != 3 || maj != 1 || st < 100 || st > 999)
            { snprintf(x->err, x->cap, "not an HTTP answer: %.60s", line); return -1; }
        a->status = st;
        for (;;) {
            int cut = conn_line(x, c, line, lcap, &total);   /* (a line cut is one we need none of, but a Location) */
            if (cut < 0) return -1;
            if (!line[0]) break;
            char *colon = strchr(line, ':'), *v;
            if (!colon) continue;
            *colon = 0; v = colon + 1; v += strspn(v, " \t");
            size_t vl = strlen(v); while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) v[--vl] = 0;
            if (!strcasecmp(line, "content-length")) {
                char *end; long long l = strtoll(v, &end, 10);
                if (end == v || *end || l < 0 || (a->length >= 0 && a->length != l)) { snprintf(x->err, x->cap, "a bad Content-Length"); return -1; }
                a->length = l;
            } else if (!strcasecmp(line, "transfer-encoding")) {
                /* chunked last is the only coding we take (no Accept-Encoding is sent, so nothing else is applied) */
                size_t k = strlen(v);
                if (k >= 7 && !strcasecmp(v + k - 7, "chunked")) a->chunked = 1;
                else { snprintf(x->err, x->cap, "a transfer coding this client does not read: %.40s", v); return -1; }
            } else if (!strcasecmp(line, "location"))
                a->location_cut = cut || snprintf(a->location, sizeof a->location, "%s", v) >= (int)sizeof a->location;
        }
        if (a->status >= 200 || a->status == 101) return 0;
    }
}

/* A reader that holds a piece back (https.c: the decoders' queue full) is no silence of the server's: the low speed
 * limit's window begins again after such a wait (libcurl's average took the wait in, too) */
static int deliver(struct run *x, const void *d, size_t n)
{
    if (!n || !x->r->data) return 0;
    long long t0 = tls_now_ms();
    if (x->r->data(x->r->arg, d, n)) { snprintf(x->err, x->cap, "ended by its reader"); x->why = HTTP_ENDED; return -1; }
    long long now = tls_now_ms();
    if (now - t0 >= TICK_MS) { x->start = now; x->tick = 0; memset(x->ticks, 0, sizeof x->ticks); }
    return 0;
}

static int read_body(struct run *x, struct conn *c, const struct answer *a, int has_body)
{
    if (!has_body) return 0;
    if (a->chunked) {
        char line[256]; size_t total = 0;
        for (;;) {
            total = 0;
            int k = conn_line(x, c, line, sizeof line, &total);
            if (k < 0) return -1;
            if (k) { snprintf(x->err, x->cap, "a bad chunk"); return -1; }      /* (no chunk's size line is that long) */
            char *end; unsigned long long n = strtoull(line, &end, 16);
            if (end == line || (*end && *end != ';' && *end != ' ' && *end != '\t')) { snprintf(x->err, x->cap, "a bad chunk"); return -1; }
            if (!n) {                                   /* the trailer's lines, to the empty one */
                do { total = 0; if (conn_line(x, c, line, sizeof line, &total) < 0) return -1; } while (line[0]);
                return 0;
            }
            while (n) {
                if (c->off == c->have) { int f = conn_fill(x, c); if (f < 0) return -1; if (!f) { snprintf(x->err, x->cap, "the connection ended inside a chunk"); return -1; } }
                size_t k = c->have - c->off; if (k > n) k = (size_t)n;
                if (deliver(x, c->buf + c->off, k)) return -1;
                c->off += k; n -= k;
            }
            total = 0;
            k = conn_line(x, c, line, sizeof line, &total);
            if (k < 0) return -1;
            if (k || line[0]) { snprintf(x->err, x->cap, "a bad chunk's end"); return -1; }
        }
    }
    long long left = a->length, got = 0;
    for (;;) {
        if (left == 0) return 0;
        if (c->off == c->have) {
            int f = conn_fill(x, c);
            if (f < 0) return -1;
            if (!f) {
                if (left < 0) return 0;                 /* no length: the connection's end is the body's */
                snprintf(x->err, x->cap, "cut off after %lld of %lld bytes", got, a->length);
                return -1;
            }
        }
        size_t k = c->have - c->off;
        if (left >= 0 && (long long)k > left) k = (size_t)left;
        if (deliver(x, c->buf + c->off, k)) return -1;
        c->off += k; got += (long long)k; if (left > 0) left -= (long long)k;
    }
}

static int send_request(struct run *x, struct conn *c, const struct url *u, const char *method, int headers, int with_body)
{
    struct http *r = x->r; size_t cap = 1024 + strlen(u->path), n;
    for (const char *const *h = headers ? r->headers : NULL; h && *h; h++) cap += strlen(*h) + 2;
    if (r->agent) cap += strlen(r->agent);
    char *q = malloc(cap);
    if (!q) { snprintf(x->err, x->cap, "out of memory"); return -1; }
    n = (size_t)snprintf(q, cap, "%s %s HTTP/1.1\r\nHost: %s\r\nAccept: */*\r\n", method, u->path, u->hostport);
    if (r->agent) n += (size_t)snprintf(q + n, cap - n, "User-Agent: %s\r\n", r->agent);
    for (const char *const *h = headers ? r->headers : NULL; h && *h; h++) n += (size_t)snprintf(q + n, cap - n, "%s\r\n", *h);
    if (with_body) n += (size_t)snprintf(q + n, cap - n, "Content-Length: %zu\r\n", r->body_len);
    n += (size_t)snprintf(q + n, cap - n, "Connection: close\r\n\r\n");
    int rc = conn_write(x, c, q, n);
    memset(q, 0, n); free(q);                           /* (Amazon's tokens are in it) */
    if (!rc && with_body && r->body_len) rc = conn_write(x, c, r->body, r->body_len);
    return rc;
}

static int exchange(struct http *r, char *err, size_t cap, struct conn *c)
{
    struct run x = { .r = r, .err = err, .cap = cap, .why = HTTP_FAILED };
    struct url u; struct answer a;
    char next[URL_MAX]; int hops = 0, headers = 1, origin_https;
    const char *method = r->method ? r->method : r->body ? "POST" : "GET";
    int with_body = r->body != NULL;
    char host0[256], port0[8];
    err[0] = 0; r->status = 0;
    x.deadline = r->total_s > 0 ? tls_now_ms() + r->total_s * 1000LL : 0;
    x.start = tls_now_ms();
    for (const char *const *h = r->headers; h && *h; h++)
        if (bad_header(*h)) { snprintf(err, cap, "a header with a control character in it"); return HTTP_FAILED; }
    if (url_parse(r->url, &u, err, cap)) return HTTP_FAILED;
    if (r->plain && u.https) { snprintf(err, cap, "only http:// here"); return HTTP_FAILED; }
    snprintf(host0, sizeof host0, "%s", u.host); snprintf(port0, sizeof port0, "%s", u.port); origin_https = u.https;
    for (;;) {
        if (conn_open(&x, &u, c)) return x.why;
        if (send_request(&x, c, &u, method, headers, with_body) || read_head(&x, c, &a)) { conn_close(c); return x.why; }
        int redirect = a.status >= 300 && a.status < 400 && a.status != 304 && a.location[0] && r->redirects > 0;
        if (redirect) {
            conn_close(c);
            if (++hops > r->redirects) { snprintf(err, cap, "more than %d redirects", r->redirects); return HTTP_FAILED; }
            if (a.location_cut) { snprintf(err, cap, "a redirect's address is longer than %d bytes", URL_MAX - 1); return HTTP_FAILED; }
            if (url_join(&u, a.location, next, sizeof next)) { snprintf(err, cap, "a redirect's address is too long"); return HTTP_FAILED; }
            struct url nu; char e[160];
            if (url_parse(next, &nu, e, sizeof e)) { snprintf(err, cap, "a redirect to a bad address: %s", e); return HTTP_FAILED; }
            if (r->plain && nu.https) { snprintf(err, cap, "a redirect away from the local network refused"); return HTTP_FAILED; }
            if (r->redirect_https && !nu.https) { snprintf(err, cap, "a redirect to plain http:// refused"); return HTTP_FAILED; }
            if (a.status == 303 || ((a.status == 301 || a.status == 302) && !strcmp(method, "POST"))) { method = "GET"; with_body = 0; }
            /* another origin: none of the caller's headers (tokens; or meant for the first server alone), and no body
             * (Amazon's carry tokens too: a 307 or 308 that would take one elsewhere is refused) */
            if (nu.https != origin_https || strcasecmp(nu.host, host0) || strcmp(nu.port, port0)) {
                if (with_body) { snprintf(err, cap, "a redirect that would take the request's body to another server refused"); return HTTP_FAILED; }
                headers = 0;
            }
            u = nu;
            continue;
        }
        r->status = a.status;
        int has_body = strcmp(method, "HEAD") && a.status != 204 && a.status != 304 && !(a.status >= 100 && a.status < 200);
        if (r->head && r->head(r->arg, a.status, has_body ? a.length : 0)) { conn_close(c); snprintf(err, cap, "ended at the answer's head"); return HTTP_ENDED; }
        int rc = read_body(&x, c, &a, has_body);
        conn_close(c);
        if (!rc) err[0] = 0;                            /* (an address that failed before the one that worked said so) */
        return rc ? x.why : HTTP_OK;
    }
}

int http_run(struct http *r, char *err, size_t cap)
{
    struct conn *c = malloc(sizeof *c);                 /* 24 kB: not on the stack of whichever thread asks */
    if (!c) { snprintf(err, cap, "out of memory"); r->status = 0; return HTTP_FAILED; }
    int rc = exchange(r, err, cap, c);
    free(c);
    return rc;
}

int http_cert_refused(const char *err) { return strstr(err, "does not check out") != NULL; }
