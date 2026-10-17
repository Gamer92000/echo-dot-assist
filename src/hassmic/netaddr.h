/* The Echo's own address changing under a running satellite (issue #17: new addresses handed out by the router).
 * Links accepted on an address the Echo no longer has are closed, and the mDNS records are announced again until Home
 * Assistant is back (root restarts avahi on state/mdns, main.sh netwatch); the same for a Sendspin server that played
 * here (issue #28).  netaddr.c says why. */
#ifndef NETADDR_H
#define NETADDR_H

/* An accepted API link (main.c's accept loop): on = 1 after accept, 0 right before its close() */
void netaddr_link(int fd, int on);

/* Something that finds the Echo by mDNS and ought to be connected */
struct netaddr_peer {
    const char *what;                   /* for the log and state/mdns: "no <what> for <n> s" */
    int  first_s;                       /* first announcement after this long without it, then doubling */
    int  (*linked)(void);               /* it is connected now; called without any lock of ours */
    int  (*expected)(void);             /* it ought to come (Home Assistant: the Echo is set up) */
};

struct netaddr_hooks {
    const struct netaddr_peer *peers; int npeers;
    void (*announce)(const char *why);  /* have root announce the mDNS records again (state/mdns) */
};
void netaddr_start(const struct netaddr_hooks *h);
#endif
