/* Sockets for plugins (docs/plugins.md 4.6): a plugin makes none and owns none.  The broker (plugin.c) checks a
 * CONNECT against what the owner accepted and entered, and hands it here; worker threads (one for each plugin that may
 * run, each plugin's requests one at a time) resolve and connect, since both may wait (a name, a server that does not
 * answer), and the result goes back through DONE.  What reaches
 * the plugin is the connected socket and nothing else: hassmic keeps no copy. */
#ifndef PLUGIN_NET_H
#define PLUGIN_NET_H
#include <stdint.h>

struct pnet_req {
    int who; unsigned gen;          /* the broker's: which plugin entry, which of its processes */
    uint32_t id;                    /* the CONNECT's id */
    char plugin[25];                /* for the log */
    char host[254];                 /* net.server: what the owner entered, a name or an IPv4 address; net.lan: the
                                     * plugin's IPv4 address (never a name); net.internet: the plugin's name or address
                                     * (a public one: what resolves to the network is refused) */
    uint16_t port; int udp;
    int kind;                       /* PM_CAP_NET_SERVER, PM_CAP_NET_LAN (private and link-local addresses only, given
                                     * as one: no lookup; never DNS's ports, PS_PORT_RELAY), PM_CAP_NET_INTERNET (public
                                     * addresses only) */
    const char *mark;               /* the log's mark for its lines (LOG_PRIVATE_MARK for a plugin that hears
                                     * requests: where it connects is its choice), NULL none */
};
/* On a worker's thread, no lock held: FD the connected socket (the callee's to hand on and close), or -1 and ERR a
 * PM_ERR_* (DENIED: an address no plugin may reach, or not of its kind; NOT_FOUND: no address; FAILED: no connection
 * in time; BUSY: its name's lookup not begun, two of the plugin's own under way or the Echo's at its bound) */
typedef void (*pnet_done)(const struct pnet_req *r, int fd, int err);
/* On a worker's thread, no lock held: whether the process that asked still runs.  Asked before a request is worked on
 * and between its addresses: one that ended has its request dropped (DONE with FAILED), so that the next start's
 * waits behind nothing of the last one's */
typedef int (*pnet_live)(const struct pnet_req *r);

/* Queued: 0; -1 the queue is full (16 waiting in all), or R->who is no entry of the broker's */
int pnet_ask(const struct pnet_req *r, pnet_done done, pnet_live live);
/* the whole of a request: the name's lookup and every address's connect, PNET_DEADLINE_MS from its start */
#define PNET_DEADLINE_MS 10000

/* Whether a plugin may reach ADDR (IPv4, network order) at PORT at all, whatever its entry says: never loopback,
 * 0.0.0.0/8, multicast, broadcast, 240/4, an address of this Echo's own (or its network's broadcast), port 5555, and
 * only an address the kernel routes as unicast out of an interface that is not loopback (a "local" route delivers an
 * address no interface has to this Echo itself, over lo).  Loopback matters most: `-o lo` passes the firewall, and
 * behind it are hassmic's own ports and Amazon's daemons */
int pnet_address_ok(uint32_t addr, uint16_t port);
/* a private or link-local address (lockdown.sh's LOCAL4 without multicast and broadcast): the firewall lets a plain
 * socket of hassmic's reach it; anything else needs the group the firewall lets out (net.c net_socket) */
int pnet_local(uint32_t addr);
#endif
