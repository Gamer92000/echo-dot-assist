/* The Echo's own address changing under a running satellite (issue #17: new addresses handed out by the router).
 * Links accepted on an address the Echo no longer has are closed, and the mDNS records are announced again until Home
 * Assistant is back (root restarts avahi on state/mdns, main.sh netwatch).  netaddr.c says why both are needed. */
#ifndef NETADDR_H
#define NETADDR_H

/* An accepted API link (main.c's accept loop): on = 1 after accept, 0 right before its close() */
void netaddr_link(int fd, int on);

struct netaddr_hooks {
    int  (*linked)(void);               /* a client (Home Assistant) is connected now; called without any lock of ours */
    int  (*expected)(void);             /* Home Assistant has taken this Echo on (not the setup): it ought to come */
    void (*announce)(const char *why);  /* have root announce the mDNS records again (state/mdns) */
};
void netaddr_start(const struct netaddr_hooks *h);
#endif
