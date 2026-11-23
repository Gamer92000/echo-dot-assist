/* plugin_net.h: the sockets a plugin gets by CONNECT (docs/plugins.md 4.6: `net.server`, `net.lan` and
 * `net.internet`).
 *
 * What the broker has checked before a request comes here: the plugin has the capability, the entry is one of its
 * manifest's, the port is one the entry lists (any but 5555 for "any"; for net.server also the one the owner wrote
 * into its port row), and the plugin has no more than two under way and thirty in a minute.  The host: for net.server
 * what the owner wrote into that entry's row (a plugin cannot name another: there is no lookup of its own, so it
 * cannot spell data into DNS queries either); for net.lan an IPv4 address the plugin names, written a.b.c.d, private
 * or link-local (never a name: the same reason), and never at DNS's ports 53 and 853 (PS_PORT_RELAY: the home's
 * router answers there by asking the internet, the way out a name would have been); for net.internet whatever the
 * plugin names (it has the internet anyway, a query spells nothing it could not send).
 *
 * What is checked here, whatever the entry: the address the name resolves to (pnet_address_ok); for net.lan that it
 * is private or link-local, for net.internet that it is not (the network is net.lan's, asked for and shown on a line
 * of its own: "Connects to the internet" reached every device of the home, and a name its author points at one of
 * them, DNS rebinding, did so after any check of the name; N3 of a review of 7be9343).  A name is resolved only here;
 * ".local" by mDNS as net.c does for media.
 *
 * Which socket: a private address gets a plain socket of hassmic's, which the firewall holds to local addresses
 * (lockdown.sh hassmic_out); any other one a socket made as the group the firewall lets out (net_socket), as hassmic's
 * own media fetches are: for net.server the owner entered that address, for net.internet the owner accepted "the
 * internet".  net.lan never reaches that branch, net.internet never the first.  The socket stays hassmic's user's:
 * the plugin's users own none (the firewall drops whatever they would own, a wall behind the filter that refuses
 * socket and accept).
 *
 * TCP: connected within PNET_DEADLINE_MS of the request, its name's lookup (bounded by it: a thread of its own the
 * worker does not wait for past it) and every address it has together, then
 * handed over non-blocking and whole: hassmic keeps no copy, so nothing a plugin does to it can hold a thread of
 * hassmic's (3.2).  UDP: connected, so that send() reaches that server and no other (sendto with an address is refused
 * by the filter); handed over the same way.  Either is checked once more as connected: a socket whose own address is
 * the server's went to this Echo itself, whatever the route said. */
#define _GNU_SOURCE
#include "plugin_net.h"
#include "logread.h"
#include "net.h"
#include "plugin_store.h"
#include "plugmsg.h"
#include "plugrun.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Requests waiting for a worker, all plugins together: NET_PENDING (plugin.c) under way per plugin, eight plugins at
 * once (RUN_MAX); a full queue answers BUSY.  A worker for each plugin that may run (made at the first request), and a
 * worker takes the oldest request of a plugin none of the others works on: one plugin's requests are worked on one
 * at a time, so that its connects to servers that never answer (10 s each) delay its own and nobody else's.  Two
 * workers shared by all in turn, and a plugin's two SYNs into nothing held every other plugin's CONNECT for 10 s, and
 * ten such for a minute (N2 of a review of 7be9343).  A worker that works on a process's request after that process
 * ended (between its addresses it asks, live()) keeps its plugin's turn until it is done: at most PNET_DEADLINE_MS,
 * and the next process of that plugin is the only one that waits for it */
#define QUEUE       16
#define WORKERS     PLUGRUN_SLOTS
_Static_assert(QUEUE >= WORKERS, "a request for every worker");

static pthread_mutex_t ql = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;
static struct { struct pnet_req r; pnet_done done; pnet_live live; } q[QUEUE];

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static int qn, workers;
static unsigned char working[PS_MAX];       /* (ql) by the broker's entry: a request of that plugin in a worker's hands */

static int in(uint32_t a, const char *net, int bits)
{
    struct in_addr n; uint32_t mask = bits ? htonl(~0u << (32 - bits)) : 0;
    inet_pton(AF_INET, net, &n);
    return (a & mask) == (n.s_addr & mask);
}

int pnet_local(uint32_t a) { return in(a, "10.0.0.0", 8) || in(a, "172.16.0.0", 12) || in(a, "192.168.0.0", 16) || in(a, "169.254.0.0", 16); }

/* How the kernel routes A (RTM_GETROUTE, as `ip route get`): 1 unicast out of an interface that is not loopback, 0
 * anything else (local, broadcast, unreachable, ... or over lo), and 0 when it cannot be asked: what is not known to
 * leave the Echo is not reached */
static int route_out(uint32_t a)
{
    struct { struct nlmsghdr h; struct rtmsg r; struct rtattr at; uint32_t dst; } req; char b[4096]; int fd, ok = 0; ssize_t n;
    struct sockaddr_nl nl = { .nl_family = AF_NETLINK };
    if ((fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)) < 0) return 0;
    { struct timeval tv = { 1, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); }
    memset(&req, 0, sizeof req);
    req.h.nlmsg_len = sizeof req; req.h.nlmsg_type = RTM_GETROUTE; req.h.nlmsg_flags = NLM_F_REQUEST; req.h.nlmsg_seq = 1;
    req.r.rtm_family = AF_INET; req.r.rtm_dst_len = 32;
    req.at.rta_type = RTA_DST; req.at.rta_len = RTA_LENGTH(4); req.dst = a;
    if (sendto(fd, &req, sizeof req, 0, (struct sockaddr *)&nl, sizeof nl) == (ssize_t)sizeof req) {
        while ((n = recv(fd, b, sizeof b, 0)) < 0 && errno == EINTR) ;
        for (struct nlmsghdr *h = (struct nlmsghdr *)b; n > 0 && NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
            struct rtmsg *r; int len, oif = -1;
            if (h->nlmsg_type != RTM_NEWROUTE) break;           /* (an error: no route) */
            r = NLMSG_DATA(h); len = (int)RTM_PAYLOAD(h);
            for (struct rtattr *x = RTM_RTA(r); RTA_OK(x, len); x = RTA_NEXT(x, len)) if (x->rta_type == RTA_OIF && RTA_PAYLOAD(x) >= 4) memcpy(&oif, RTA_DATA(x), 4);
            if (r->rtm_type == RTN_UNICAST && oif > 0) {
                struct ifreq ifr; int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
                memset(&ifr, 0, sizeof ifr);
                if (s >= 0 && if_indextoname((unsigned)oif, ifr.ifr_name) && !ioctl(s, SIOCGIFFLAGS, &ifr)) ok = !(ifr.ifr_flags & IFF_LOOPBACK);
                if (s >= 0) close(s);
            }
            break;
        }
    }
    close(fd);
    return ok;
}

int pnet_address_ok(uint32_t a, uint16_t port)
{
    struct ifaddrs *ifs = NULL; int ok = 1;
    if (port == 0 || port == 5555 || in(a, "0.0.0.0", 8) || in(a, "127.0.0.0", 8) || in(a, "224.0.0.0", 4) || in(a, "240.0.0.0", 4)) return 0;
    /* the Echo's own addresses go over lo as well; and its networks' broadcast */
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs *i = ifs; i && ok; i = i->ifa_next) {
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == a) ok = 0;
            if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr && i->ifa_broadaddr->sa_family == AF_INET && ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr.s_addr == a) ok = 0;
        }
        freeifaddrs(ifs);
    } else ok = 0;                                      /* (what is the Echo's own cannot be known: nothing) */
    /* an address no interface has can still be this Echo's: a "local" route (`ip route add local ...`) delivers it
     * over lo, past the firewall's `-o lo` (a review, 2026-11-21) */
    return ok && route_out(a);
}

/* The name's lookup, within the request's deadline.  getaddrinfo has no deadline of its own (bionic's resolver
 * tries each server in turn with its own timeouts: 20 s and more for a name nobody answers), and it held one of the
 * two workers every plugin shares past the 10 s the request has in all (R7 of a review, 2026-11-21).  So it runs in a
 * thread of its own, and the worker waits for it until the deadline, no longer: one that answers late finds nobody
 * waiting and frees what it had (the lookup's own refcount).  A name that never answers holds its lookup until the
 * resolver gives up, so they are counted until each thread ends, and past a bound one is not begun (BUSY, said so).
 * The bound was four for all plugins together: one plugin with net.internet asking for names under a server that
 * never answers (two every ten seconds keep four busy) and every other plugin's lookups failed, net.server's for the
 * server its owner entered too, with words that blamed the name (N2 of a review of 7be9343).  So: LOOKUPS_EACH for
 * each plugin (by the broker's entry), LOOKUPS_EACH times RUN_MAX of names a plugin chose (net.internet's), and
 * LOOKUPS_OWNER more that only the names an owner entered take (net.server's): eight plugins at their bound and the
 * entries of stopped ones still waiting on theirs leave the owners' servers places of their own */
#define LOOKUPS_EACH  2
#define LOOKUPS_OWNER 4
#define LOOKUPS_CHOSEN (LOOKUPS_EACH * PLUGRUN_SLOTS)
struct lookup { pthread_mutex_t mu; pthread_cond_t cv; int refs, done, n, who, owner; char host[254]; uint32_t a[8]; };
static pthread_mutex_t lkm = PTHREAD_MUTEX_INITIALIZER;
static int lk_each[PS_MAX], lk_chosen, lk_all;      /* (lkm) lookups whose thread runs: by entry, chosen names, all */
/* a lookup for entry WHO (OWNER: the owner's name) may begin, and is counted: 0; -2 its plugin has LOOKUPS_EACH under
 * way, -3 the Echo has all it takes */
static int lookup_take(int who, int owner)
{
    int r = 0;
    pthread_mutex_lock(&lkm);
    if (who < 0 || who >= PS_MAX || lk_each[who] >= LOOKUPS_EACH) r = -2;
    else if (lk_all >= LOOKUPS_CHOSEN + LOOKUPS_OWNER || (!owner && lk_chosen >= LOOKUPS_CHOSEN)) r = -3;
    else { lk_each[who]++; lk_all++; lk_chosen += !owner; }
    pthread_mutex_unlock(&lkm);
    return r;
}
static void lookup_give(int who, int owner)
{
    pthread_mutex_lock(&lkm);
    lk_each[who]--; lk_all--; lk_chosen -= !owner;
    pthread_mutex_unlock(&lkm);
}
static void lookup_put(struct lookup *l)
{
    int last;
    pthread_mutex_lock(&l->mu); last = !--l->refs; pthread_mutex_unlock(&l->mu);
    if (last) { pthread_mutex_destroy(&l->mu); pthread_cond_destroy(&l->cv); free(l); }
}
static void *lookup_thread(void *arg)
{
    struct lookup *l = arg; struct addrinfo hints = { 0 }, *ai = NULL; uint32_t a[8]; int n = 0; size_t hl = strlen(l->host);
    const char *slow = getenv("HASSMIC_PLUGIN_LOOKUP_DELAY");        /* tests: a resolver that takes this long (ms) */
    if (slow && atoi(slow) > 0) { struct timespec d = { atoi(slow) / 1000, atoi(slow) % 1000 * 1000000L }; nanosleep(&d, NULL); }
    if (hl > 6 && !strcasecmp(l->host + hl - 6, ".local") && (a[0] = mdns_resolve4(l->host))) n = 1;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (!n && !getaddrinfo(l->host, NULL, &hints, &ai)) {
        for (struct addrinfo *p = ai; p && n < 8; p = p->ai_next) if (p->ai_family == AF_INET) a[n++] = ((struct sockaddr_in *)p->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(ai);
    }
    pthread_mutex_lock(&l->mu); memcpy(l->a, a, sizeof a); l->n = n; l->done = 1; pthread_cond_signal(&l->cv); pthread_mutex_unlock(&l->mu);
    lookup_give(l->who, l->owner);
    lookup_put(l);
    return NULL;
}

/* R's host as addresses (network order), up to MAX (8), by UNTIL (the monotonic clock): the count, 0 none, -1 no
 * answer in time, -2 / -3 not begun (lookup_take's bounds) */
static int resolve(const struct pnet_req *r, uint32_t *out, int max, long long until)
{
    struct in_addr a; struct lookup *l; pthread_t t; int n = 0, owner = r->kind == PM_CAP_NET_SERVER;
    if (inet_pton(AF_INET, r->host, &a) == 1) { out[0] = a.s_addr; return 1; }
    if ((n = lookup_take(r->who, owner))) return n;
    if (!(l = calloc(1, sizeof *l))) { lookup_give(r->who, owner); return -1; }
    pthread_mutex_init(&l->mu, NULL);
    { pthread_condattr_t ca; pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC); pthread_cond_init(&l->cv, &ca); pthread_condattr_destroy(&ca); }
    snprintf(l->host, sizeof l->host, "%s", r->host); l->refs = 2; l->who = r->who; l->owner = owner;
    if (pthread_create(&t, NULL, lookup_thread, l)) { lookup_give(r->who, owner); l->refs = 1; lookup_put(l); return -1; }
    pthread_detach(t);
    pthread_mutex_lock(&l->mu);
    while (!l->done) {
        struct timespec ts = { (time_t)(until / 1000), (long)(until % 1000) * 1000000L };
        if (mono_ms() >= until || pthread_cond_timedwait(&l->cv, &l->mu, &ts) == ETIMEDOUT) break;
    }
    if (l->done) { n = l->n < max ? l->n : max; memcpy(out, l->a, sizeof out[0] * (size_t)n); } else n = -1;
    pthread_mutex_unlock(&l->mu);
    lookup_put(l);
    return n;
}

/* connected by UNTIL (the monotonic clock), or -1.  A socket whose own address is the server's went to this Echo
 * itself (whatever route led there): closed, -1 with *self set */
static int connect_one(uint32_t addr, uint16_t port, int udp, long long until, int *self)
{
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) }, me; socklen_t ml = sizeof me; int fd, type = (udp ? SOCK_DGRAM : SOCK_STREAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, ok = 0;
    sa.sin_addr.s_addr = addr;
    if ((fd = pnet_local(addr) ? socket(AF_INET, type, 0) : net_socket(AF_INET, type, 0)) < 0) return -1;
    if (!connect(fd, (struct sockaddr *)&sa, sizeof sa)) ok = 1;
    else if (errno == EINPROGRESS) {
        struct pollfd pf = { fd, POLLOUT, 0 }; int e = 0, r; socklen_t el = sizeof e; long long left;
        /* (a signal does not end the wait: what is left of it is waited for) */
        while ((left = until - mono_ms()) > 0 && (r = poll(&pf, 1, (int)left)) < 0 && errno == EINTR) ;
        ok = left > 0 && pf.revents && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) && !e;
    }
    if (ok && !getsockname(fd, (struct sockaddr *)&me, &ml) && me.sin_addr.s_addr == addr) { ok = 0; *self = 1; }
    if (ok) return fd;
    close(fd);
    return -1;
}

/* One request, PNET_DEADLINE_MS as a whole: the lookup, then the addresses in turn, each with what is left (it was 10 s
 * each, eight addresses 80 s, and two workers shared by every plugin; a review, 2026-11-21) */
static void work(const struct pnet_req *r, pnet_done done, pnet_live live)
{
    const char *e = getenv("HASSMIC_PLUGIN_NET_MS");                 /* tests: a shorter deadline */
    uint32_t a[8]; long long until = mono_ms() + (e && atoi(e) > 0 && atoi(e) < PNET_DEADLINE_MS ? atoi(e) : PNET_DEADLINE_MS); int n, fd = -1, allowed = 0, self = 0, gone = 0, late = 0; char at[16] = "";
    const char *what = r->kind == PM_CAP_NET_SERVER ? "the server the owner entered" : r->kind == PM_CAP_NET_LAN ? "a device on the network" : "the internet";
    if (live && !live(r)) { done(r, -1, PM_ERR_FAILED); return; }       /* its process ended while it waited in the queue */
    /* (the broker refused it already: a request from before the rule, or one that came past it, is refused here too) */
    if (r->kind == PM_CAP_NET_LAN && PS_PORT_RELAY(r->port)) {
        log_marked_f(r->mark, "plugins: %s: no connection to %s, port %u: DNS's port, where a device on the network asks the internet", r->plugin, what, r->port);
        done(r, -1, PM_ERR_DENIED); return;
    }
    if ((n = resolve(r, a, 8, until)) < -1) {
        log_marked_f(r->mark, "plugins: %s: no connection to %s, port %u (%s): %s", r->plugin, what, r->port, r->udp ? "UDP" : "TCP",
                     n == -2 ? "two of its name lookups are under way already (one whose name nobody answers holds its place until the resolver gives up, 20 s and more)"
                             : "this Echo has as many name lookups under way as it takes, for all plugins together");
        done(r, -1, PM_ERR_BUSY); return;
    }
    if (n < 0) { n = 0; late = 1; }
    for (int i = 0; i < n && fd < 0 && mono_ms() < until; i++) {
        if (live && !live(r)) { gone = 1; break; }
        if (!pnet_address_ok(a[i], r->port) || (r->kind == PM_CAP_NET_LAN && !pnet_local(a[i])) || (r->kind == PM_CAP_NET_INTERNET && pnet_local(a[i]))) continue;
        allowed++;
        inet_ntop(AF_INET, &a[i], at, sizeof at);
        fd = connect_one(a[i], r->port, r->udp, until, &self);
        if (self) { allowed--; self = 0; }                       /* (this Echo's own after all: not reachable) */
    }
    /* (a plugin that hears requests: its lines private, where it connects is its choice under net.lan and
     * net.internet, r->mark) */
    if (gone) ;
    else if (fd >= 0) log_marked_f(r->mark, "plugins: %s: connected to %s:%u (%s%s)", r->plugin, at, r->port, r->udp ? "UDP" : "TCP",
                                   r->kind == PM_CAP_NET_LAN ? ", a device on the network (net.lan)" : r->kind == PM_CAP_NET_INTERNET ? ", outside the local network: net.internet"
                                   : pnet_local(inet_addr(at)) ? "" : ", outside the local network: the owner's server");
    else log_marked_f(r->mark, "plugins: %s: no connection to %s, port %u (%s): %s", r->plugin, what, r->port, r->udp ? "UDP" : "TCP",
                      late ? "its name's lookup did not answer in 10 s" : !n ? "its name has no address" : !allowed ? (r->kind == PM_CAP_NET_LAN ? "an address no plugin may reach on the network (not a private one, this Echo's own, loopback, multicast or broadcast)"
                                                                                                                        : r->kind == PM_CAP_NET_INTERNET ? "an address no plugin may reach on the internet (one on the local network: that is net.lan's; this Echo's own, loopback, multicast or broadcast)"
                                                                                                                        : "an address no plugin may reach (this Echo's own, loopback, multicast or broadcast)") : "it did not answer in 10 s or refused");
    done(r, fd, fd >= 0 ? 0 : late ? PM_ERR_FAILED : !n ? PM_ERR_NOT_FOUND : !allowed ? PM_ERR_DENIED : PM_ERR_FAILED);
}

/* (ql held) the oldest request of a plugin no worker works on, or -1 */
static int next_free(void)
{
    for (int k = 0; k < qn; k++) if (!working[q[k].r.who]) return k;
    return -1;
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&ql);
    for (;;) {
        struct pnet_req r; pnet_done done; pnet_live live; int k;
        while ((k = next_free()) < 0) pthread_cond_wait(&qcv, &ql);
        r = q[k].r; done = q[k].done; live = q[k].live;
        memmove(q + k, q + k + 1, sizeof q[0] * (size_t)(--qn - k));
        working[r.who] = 1;
        pthread_mutex_unlock(&ql);
        work(&r, done, live);
        pthread_mutex_lock(&ql);
        working[r.who] = 0;
        pthread_cond_broadcast(&qcv);   /* (its plugin's next request may be waiting for it) */
    }
    return NULL;
}

int pnet_ask(const struct pnet_req *r, pnet_done done, pnet_live live)
{
    if (r->who < 0 || r->who >= PS_MAX) return -1;
    pthread_mutex_lock(&ql);
    if (qn == QUEUE) { pthread_mutex_unlock(&ql); return -1; }
    q[qn].r = *r; q[qn].done = done; q[qn].live = live; qn++;
    while (workers < WORKERS) { pthread_t t; if (pthread_create(&t, NULL, worker, NULL)) break; pthread_detach(t); workers++; }
    if (!workers) { qn--; pthread_mutex_unlock(&ql); return -1; }
    pthread_cond_broadcast(&qcv);
    pthread_mutex_unlock(&ql);
    return 0;
}
