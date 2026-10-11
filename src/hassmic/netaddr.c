/* The Echo's own address changing under a running satellite (issue #17: the router hands out new addresses; after that
 * Home Assistant did not find the Echo again until a power cycle, sometimes two).  Two things go wrong:
 *
 * 1. Links over the old address stay.  Linux keeps a TCP socket whose local address was removed, and nothing leaves it
 *    any more (there is no route from an address the host lacks), so neither side hears of it.  Our keepalive (main.c)
 *    only probes while nothing waits for an acknowledgement; with a state update in flight the socket retransmits for
 *    some 15 min (tcp_retries2).  Meanwhile the dead link holds a client slot and, worse, the voice assistant: it goes
 *    to the first subscriber (proto_esphome.c SUBSCRIBE_VA), so Home Assistant back over the new address would get no
 *    pipelines.  So every poll closes the links whose local address is gone, once there is another one.
 *
 * 2. Home Assistant ignores the announcement of the new address.  Its ESPHome config flow takes a new host from mDNS
 *    only while the device is not connected (config_flow.py _async_validate_mac_abort_configured: "transient mDNS
 *    announcements during Wi-Fi mesh roaming" must not move a working entry), and aioesphomeapi gives up on a silent
 *    link only after 90 s (a ping every 20 s, KEEP_ALIVE_TIMEOUT_RATIO 4.5).  avahi announces the new address at once,
 *    while Home Assistant still holds the old link: ignored, and nothing announces again, so it dials the old address
 *    for ever.  A power cycle makes avahi announce again at boot, but an Echo is back on Wi-Fi within those 90 s more
 *    often than not: hence the second power cycle.
 *    So while Home Assistant should be there (the Echo is set up) and is not, root restarts avahi (state/mdns, main.sh
 *    netwatch): after 100 s without a client, then 200, 400, ... at most every 30 min.  A restart, not a repeat: avahi
 *    says goodbye to its records and announces them anew a few seconds later, so Home Assistant's zeroconf sees the
 *    service go and come and starts its discovery, which moves the entry to the new address.  The same records merely
 *    repeated change nothing in its cache and start nothing.  A new address is announced at once as well.
 *
 * Addresses: every IPv4 address the interfaces have (getifaddrs), up or not: a link over an interface that is down for a
 * moment is not stale, and dhcpcd -K leaves the address in place anyway.  HASSMIC_FAKE_ADDRS (tests) names a file with
 * one address per line instead, loopback not set apart.  HASSMIC_ANNOUNCE_TIMES="<first> <max>" (s). */
#define _GNU_SOURCE
#include "netaddr.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define POLL_S      2
#define MAX_ADDRS   16
#define MAX_LINKS   16          /* ESPHome takes 4 clients; the rest are refused at once */
static int FIRST_S = 100;       /* > 90 s: Home Assistant has given up the old link by then (see 2.) */
static int MAX_S = 1800;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int links[MAX_LINKS], nlinks;
static const struct netaddr_hooks *hooks;

void netaddr_link(int fd, int on)
{
    pthread_mutex_lock(&mu);
    if (on) { if (nlinks < MAX_LINKS) links[nlinks++] = fd; }
    else for (int i = 0; i < nlinks; i++) if (links[i] == fd) { links[i] = links[--nlinks]; break; }
    pthread_mutex_unlock(&mu);
}

struct addrs { int n, own; uint32_t a[MAX_ADDRS]; };     /* own: how many of a[] are not loopback (those come first) */

static int cmp(const void *x, const void *y) { uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y; return a < b ? -1 : a > b; }

/* 0, or -1 when it cannot tell (then nothing is decided on it) */
static int addrs_get(struct addrs *s)
{
    const char *fake = getenv("HASSMIC_FAKE_ADDRS");
    s->n = s->own = 0;
    if (fake) {
        FILE *f = fopen(fake, "r"); char l[64]; struct in_addr ia;
        if (!f) return 0;
        while (s->n < MAX_ADDRS && fgets(l, sizeof l, f)) { l[strcspn(l, " \r\n")] = 0; if (inet_pton(AF_INET, l, &ia) == 1) s->a[s->n++] = ia.s_addr; }
        fclose(f);
        s->own = s->n;
    } else {
        struct ifaddrs *ifs, *i; uint32_t lo[MAX_ADDRS]; int nlo = 0;
        if (getifaddrs(&ifs)) return -1;
        for (i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            uint32_t a = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
            if (i->ifa_flags & IFF_LOOPBACK) { if (nlo < MAX_ADDRS) lo[nlo++] = a; }
            else if (s->own < MAX_ADDRS) s->a[s->own++] = a;
        }
        freeifaddrs(ifs);
        s->n = s->own;
        for (int k = 0; k < nlo && s->n < MAX_ADDRS; k++) s->a[s->n++] = lo[k];
    }
    qsort(s->a, (size_t)s->own, sizeof s->a[0], cmp);
    return 0;
}

static int has(const struct addrs *s, uint32_t a) { for (int i = 0; i < s->n; i++) if (s->a[i] == a) return 1; return 0; }

static void addrs_text(const struct addrs *s, char *out, size_t cap)
{
    size_t k = 0; out[0] = 0;
    for (int i = 0; i < s->own && k + INET_ADDRSTRLEN + 1 < cap; i++) {
        struct in_addr ia = { s->a[i] };
        if (i) out[k++] = ' ';
        inet_ntop(AF_INET, &ia, out + k, (socklen_t)(cap - k)); k += strlen(out + k);
    }
    if (!s->own) snprintf(out, cap, "none");
}

/* Close the links whose local address is not among the Echo's; their readers clean up (shutdown, not close: the fd
 * stays theirs).  How many. */
static int drop_stale(const struct addrs *s)
{
    int n = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nlinks; i++) {
        struct sockaddr_in me; socklen_t l = sizeof me;
        if (getsockname(links[i], (struct sockaddr *)&me, &l) || me.sin_family != AF_INET || has(s, me.sin_addr.s_addr)) continue;
        char t[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &me.sin_addr, t, sizeof t);
        fprintf(stderr, "network: closing a link over %s, which this Echo no longer has\n", t);
        shutdown(links[i], SHUT_RDWR); n++;
    }
    pthread_mutex_unlock(&mu);
    return n;
}

static time_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }

static void *watch(void *arg)
{
    (void)arg;
    struct addrs last = { 0 }, cur; char was[160], now_t[160], why[64];
    time_t since = 0, next = 0; int wait = FIRST_S;
    for (;; sleep(POLL_S)) {
        if (addrs_get(&cur)) continue;
        time_t now = mono();
        if (cur.own) {
            /* A new address (not the first one, not the same one back after a gap: TCP links survive that) */
            if (last.own && (cur.own != last.own || memcmp(cur.a, last.a, sizeof cur.a[0] * (size_t)cur.own))) {
                addrs_text(&last, was, sizeof was); addrs_text(&cur, now_t, sizeof now_t);
                fprintf(stderr, "network: address %s now (was %s)\n", now_t, was);
                drop_stale(&cur);
                hooks->announce("new address");
                since = now; wait = FIRST_S; next = now + wait;
            } else drop_stale(&cur);
            last = cur;
        }
        /* Home Assistant should be there and is not: announce again, at growing intervals (see 2. above) */
        if (!cur.own || !hooks->expected() || hooks->linked()) { since = 0; wait = FIRST_S; continue; }
        if (!since) { since = now; next = now + wait; }
        if (now >= next) {
            snprintf(why, sizeof why, "no Home Assistant for %lld s", (long long)(now - since));
            hooks->announce(why);
            wait = wait * 2 > MAX_S ? MAX_S : wait * 2; next = now + wait;
        }
    }
    return NULL;
}

void netaddr_start(const struct netaddr_hooks *h)
{
    const char *t = getenv("HASSMIC_ANNOUNCE_TIMES"); pthread_t th;
    if (t) sscanf(t, "%d %d", &FIRST_S, &MAX_S);
    hooks = h;
    if (!pthread_create(&th, NULL, watch, NULL)) pthread_detach(th);
}
