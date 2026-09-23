#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "net.h"

/* ---------------------------------------------------------------- mDNS */

static int put_name(uint8_t *p, size_t cap, const char *name)
{
    size_t o = 0;
    while (*name) {
        const char *dot = strchr(name, '.'); size_t l = dot ? (size_t)(dot - name) : strlen(name);
        if (!l || l > 63 || o + l + 2 > cap) return -1;
        p[o++] = l; memcpy(p + o, name, l); o += l;
        name += l + (dot ? 1 : 0);
    }
    p[o++] = 0;
    return o;
}

/* Name at *off (compression pointers followed) as dotted text; *off moves past it.  -1 on a malformed packet. */
static int get_name(const uint8_t *m, size_t len, size_t *off, char *out, size_t outsz)
{
    size_t o = *off, n = 0; int jumps = 0, moved = 0;
    for (;;) {
        if (o >= len) return -1;
        uint8_t l = m[o];
        if ((l & 0xc0) == 0xc0) {
            if (o + 1 >= len || ++jumps > 16) return -1;
            if (!moved) { *off = o + 2; moved = 1; }
            o = (l & 0x3f) << 8 | m[o + 1];
            continue;
        }
        if (l & 0xc0) return -1;
        if (!l) { if (!moved) *off = o + 1; break; }
        if (o + 1 + l > len || n + l + 2 > outsz) return -1;
        if (n) out[n++] = '.';
        memcpy(out + n, m + o + 1, l); n += l; o += 1 + l;
    }
    out[n] = 0;
    return 0;
}

static unsigned parse_answer(const uint8_t *m, size_t len, const char *name)
{
    char rn[256]; size_t off = 12;
    if (len < 12 || !(m[2] & 0x80)) return 0;                                  /* responses only */
    unsigned qd = m[4] << 8 | m[5], rr = (m[6] << 8 | m[7]) + (m[8] << 8 | m[9]) + (m[10] << 8 | m[11]);
    for (unsigned i = 0; i < qd; i++) { if (get_name(m, len, &off, rn, sizeof rn)) return 0; off += 4; }
    for (unsigned i = 0; i < rr; i++) {
        if (get_name(m, len, &off, rn, sizeof rn) || off + 10 > len) return 0;
        unsigned type = m[off] << 8 | m[off + 1], cls = (m[off + 2] << 8 | m[off + 3]) & 0x7fff, rdlen = m[off + 8] << 8 | m[off + 9];
        off += 10;
        if (off + rdlen > len) return 0;
        if (type == 1 && cls == 1 && rdlen == 4 && !strcasecmp(rn, name)) { unsigned a; memcpy(&a, m + off, 4); return a; }
        off += rdlen;
    }
    return 0;
}

/* Sent from a port other than 5353, so responders answer by unicast to it (RFC 6762 5.1, 6.7) and the avahi-daemon that
 * owns 5353 on the Echo is not in the way.  Not an ephemeral port: those start at 32768, and the stock firewall admits
 * inbound UDP on 16384-32767 only.  The query goes out on every IPv4 interface: multicast has no route to follow. */
unsigned mdns_resolve4(const char *host)
{
    char name[256]; uint8_t q[300], r[1500]; unsigned addr = 0;
    snprintf(name, sizeof name, "%s", host);
    size_t nl = strlen(name); if (nl && name[nl - 1] == '.') name[nl - 1] = 0;
    memset(q, 0, 12); q[1] = 0x42;                                              /* id; flags 0: standard query */
    q[5] = 1;                                                                   /* one question */
    int l = put_name(q + 12, sizeof q - 16, name);
    if (l < 0) return 0;
    size_t qlen = 12 + l;
    q[qlen++] = 0; q[qlen++] = 1;                                               /* type A */
    q[qlen++] = 0x80; q[qlen++] = 1;                                            /* class IN, unicast response wanted */
    int s = socket(AF_INET, SOCK_DGRAM, 0), bound = 0;
    if (s < 0) return 0;
    struct sockaddr_in me = { .sin_family = AF_INET };
    for (int i = 0; i < 20 && !bound; i++) {
        me.sin_port = htons(16384 + ((unsigned)getpid() * 7919u + i * 104729u) % 16384);
        bound = !bind(s, (struct sockaddr *)&me, sizeof me);
    }
    if (!bound) { close(s); return 0; }
    unsigned char ttl = 255; setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353) };
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    static const int wait_ms[] = { 250, 500, 1000 };
    for (int round = 0; round < 3 && !addr; round++) {
        struct ifaddrs *ifs = NULL; int sent = 0;
        if (!getifaddrs(&ifs)) {
            for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
                    !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_MULTICAST)) continue;
                struct in_addr via = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &via, sizeof via);
                if (sendto(s, q, qlen, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)qlen) sent++;
            }
            freeifaddrs(ifs);
        }
        if (!sent) sendto(s, q, qlen, 0, (struct sockaddr *)&to, sizeof to);
        struct pollfd pfd = { .fd = s, .events = POLLIN };
        while (!addr && poll(&pfd, 1, wait_ms[round]) > 0) {
            ssize_t n = recv(s, r, sizeof r, 0);
            if (n > 0) addr = parse_answer(r, n, name);
        }
    }
    close(s);
    return addr;
}

/* ---------------------------------------------------------------- TCP */

int net_connect(const char *host, const char *port, int timeout_s)
{
    struct addrinfo hints = { 0 }, *ai = NULL, local = { 0 }, *list = NULL; struct sockaddr_in sin = { 0 };
    size_t hl = strlen(host);
    if (hl > 6 && !strcasecmp(host + hl - 6, ".local")) {
        unsigned a = mdns_resolve4(host);
        if (a) {
            sin.sin_family = AF_INET; sin.sin_port = htons(atoi(port)); sin.sin_addr.s_addr = a;
            local.ai_family = AF_INET; local.ai_addr = (struct sockaddr *)&sin; local.ai_addrlen = sizeof sin;
            list = &local;
        }
    }
    hints.ai_socktype = SOCK_STREAM;
    if (!list) {                                        /* also for ".local" that nobody answered: some DNS servers serve it */
        if (getaddrinfo(host, port, &hints, &ai)) { fprintf(stderr, "net: cannot resolve %s\n", host); return -1; }
        list = ai;
    }
    int fd = -1;
    for (struct addrinfo *p = list; p && fd < 0; p = p->ai_next) {
        struct timeval tv = { timeout_s, 0 };           /* connect() obeys the send timeout */
        if ((fd = socket(p->ai_family, SOCK_STREAM, 0)) < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, p->ai_addr, p->ai_addrlen)) { close(fd); fd = -1; }
    }
    if (ai) freeaddrinfo(ai);
    if (fd < 0) fprintf(stderr, "net: cannot connect to %s:%s (not a local address? see lockdown.sh)\n", host, port);
    return fd;
}
