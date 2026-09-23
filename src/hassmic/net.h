/* Outgoing TCP connections by host name, for URLs that Home Assistant or Music Assistant hand us. */
#ifndef NET_H
#define NET_H

/* Connected socket or -1.  Every address the name resolves to is tried in turn, so an unreachable one (a global IPv6
 * address behind the egress lock, say) does not hide a working one.  Names ending in ".local" are looked up by mDNS: the
 * system resolver does not do that.  timeout_s applies to each connect() and stays set as send and receive timeout. */
int net_connect(const char *host, const char *port, int timeout_s);

/* IPv4 address of a ".local" name by one-shot mDNS query (RFC 6762 5.1), in network byte order; 0 if nobody answered. */
unsigned mdns_resolve4(const char *name);
#endif
