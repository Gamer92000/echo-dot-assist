/*
 * The country the Echo is in, from the Wi-Fi: the access point's Country element (802.11d, element 7: two letters of
 * ISO 3166 and an environment byte), which an AP that offers 5 GHz must send in Europe and most others send too.  It
 * picks the time zone's name among the zones of Home Assistant's rule (tz.c, clock.c): Home Assistant sends only the
 * rule (CET-1CEST,M3.5.0,M10.5.0/3), never the name, and the name is what says the language (main.c speech_locale, the
 * Echo Show's date).
 *
 * Read over nl80211 (generic netlink) as cfg80211 keeps it: a dump of the scan results (NL80211_CMD_GET_SCAN, which
 * needs no privilege) per interface, the BSS whose status is "associated", its IEs (the probe response's, else the
 * beacon's).  Measured on an Echo Dot 2 (biscuit, kernel 3.18, MediaTek's gen2 driver) as the daemon's user: the AP's
 * "DE" in both.  No root, no wpa_supplicant (whose socket differs per model and is root's).  Where the driver keeps no
 * IEs, or the AP sends none, there is no country and tz.c goes on as before.
 *
 * HASSMIC_FAKE_COUNTRY (tests) names a file with the two letters instead ("" or no file: none).
 */
#include "apcountry.h"
#include "compat.h"
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* linux/nl80211.h, not in every NDK's sysroot: the values are ABI */
#define NL80211_CMD_GET_SCAN 32
#define NL80211_ATTR_IFINDEX 3
#define NL80211_ATTR_BSS 47
#define NL80211_BSS_INFORMATION_ELEMENTS 6
#define NL80211_BSS_STATUS 9
#define NL80211_BSS_BEACON_IES 11
#define NL80211_BSS_STATUS_ASSOCIATED 1

static int letter(uint8_t c) { return c >= 'A' && c <= 'Z'; }

int apcountry_ies(const uint8_t *ie, size_t len, char cc[3])
{
    for (size_t i = 0; i + 2 <= len && i + 2 + ie[i + 1] <= len; i += 2 + (size_t)ie[i + 1])
        if (ie[i] == 7 && ie[i + 1] >= 3) {
            const uint8_t *c = ie + i + 2;
            if (!letter(c[0]) || !letter(c[1]) || (c[0] == 'X' && c[1] == 'X')) return 0;  /* XX: "unknown" */
            cc[0] = (char)c[0]; cc[1] = (char)c[1]; cc[2] = 0;
            return 1;
        }
    return 0;
}

/* the attributes from a to a + len: each whole, or the walk stops */
#define NLA_EACH(a, start, len) \
    for (const struct nlattr *a = (const void *)(start); (const char *)a + NLA_HDRLEN <= (const char *)(start) + (len) && \
         a->nla_len >= NLA_HDRLEN && (const char *)a + a->nla_len <= (const char *)(start) + (len); \
         a = (const void *)((const char *)a + NLA_ALIGN(a->nla_len)))

static int bss(const struct nlattr *b, char cc[3])
{
    const uint8_t *ie = NULL, *beacon = NULL; size_t iel = 0, bl = 0; uint32_t status = ~0u;
    NLA_EACH(s, (const char *)b + NLA_HDRLEN, b->nla_len - NLA_HDRLEN) {
        const uint8_t *d = (const uint8_t *)s + NLA_HDRLEN; size_t dl = s->nla_len - NLA_HDRLEN;
        switch (s->nla_type & NLA_TYPE_MASK) {
        case NL80211_BSS_STATUS: if (dl >= 4) memcpy(&status, d, 4); break;
        case NL80211_BSS_INFORMATION_ELEMENTS: ie = d; iel = dl; break;
        case NL80211_BSS_BEACON_IES: beacon = d; bl = dl; break;
        }
    }
    if (status != NL80211_BSS_STATUS_ASSOCIATED) return 0;
    return (ie && apcountry_ies(ie, iel, cc)) || (beacon && apcountry_ies(beacon, bl, cc));
}

int apcountry_parse(const void *buf, size_t len, char cc[3])
{
    int r = 0;
    for (size_t off = 0; off + sizeof(struct nlmsghdr) <= len;) {
        const struct nlmsghdr *n = (const void *)((const char *)buf + off);
        if (n->nlmsg_len < sizeof *n || n->nlmsg_len > len - off) return r | APC_END;     /* cut: nothing more to read */
        if (n->nlmsg_type == NLMSG_DONE || n->nlmsg_type == NLMSG_ERROR) return r | APC_END;
        if (!(r & APC_FOUND) && n->nlmsg_len >= NLMSG_HDRLEN + GENL_HDRLEN) {
            const char *at = (const char *)NLMSG_DATA(n) + GENL_HDRLEN;
            NLA_EACH(a, at, n->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN)
                if ((a->nla_type & NLA_TYPE_MASK) == NL80211_ATTR_BSS && bss(a, cc)) { r |= APC_FOUND; break; }
        }
        off += NLMSG_ALIGN(n->nlmsg_len);
    }
    return r;
}

/* a generic netlink request with one attribute */
static int request(int sk, uint16_t family, uint16_t flags, uint8_t cmd, uint16_t type, const void *v, uint16_t vl)
{
    struct { struct nlmsghdr n; struct genlmsghdr g; char a[64]; } m;
    struct nlattr *a = (void *)m.a;
    if ((size_t)NLA_HDRLEN + vl > sizeof m.a) return -1;
    memset(&m, 0, sizeof m);
    a->nla_type = type; a->nla_len = (uint16_t)(NLA_HDRLEN + vl); memcpy(m.a + NLA_HDRLEN, v, vl);
    m.n.nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN + NLA_ALIGN(a->nla_len); m.n.nlmsg_type = family;
    m.n.nlmsg_flags = NLM_F_REQUEST | flags; m.n.nlmsg_seq = 1;
    m.g.cmd = cmd; m.g.version = 1;
    return send(sk, &m, m.n.nlmsg_len, 0) == (ssize_t)m.n.nlmsg_len ? 0 : -1;
}

static int family_id(int sk)
{
    static char b[4096] __attribute__((aligned(4))); int fam = -1;
    if (request(sk, GENL_ID_CTRL, 0, CTRL_CMD_GETFAMILY, CTRL_ATTR_FAMILY_NAME, "nl80211", 8)) return -1;
    ssize_t r;
    do r = recv(sk, b, sizeof b, 0); while (r < 0 && errno == EINTR);
    for (size_t off = 0, len = r > 0 ? (size_t)r : 0; off + sizeof(struct nlmsghdr) <= len;) {
        const struct nlmsghdr *n = (const void *)(b + off);
        if (n->nlmsg_len < sizeof *n || n->nlmsg_len > len - off) break;
        if (n->nlmsg_type == GENL_ID_CTRL && n->nlmsg_len >= NLMSG_HDRLEN + GENL_HDRLEN) {
            const char *at = (const char *)NLMSG_DATA(n) + GENL_HDRLEN;
            NLA_EACH(a, at, n->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN)
                if (a->nla_type == CTRL_ATTR_FAMILY_ID && a->nla_len >= NLA_HDRLEN + 2) { uint16_t id; memcpy(&id, (const char *)a + NLA_HDRLEN, 2); fam = id; }
        }
        off += NLMSG_ALIGN(n->nlmsg_len);
    }
    return fam;
}

int apcountry_get(char cc[3])
{
    const char *fake = getenv("HASSMIC_FAKE_COUNTRY");
    cc[0] = 0;
    if (fake) {
        FILE *f = fopen(fake, "r"); char l[8] = "";
        if (f) { if (!fgets(l, sizeof l, f)) l[0] = 0; fclose(f); }
        uint8_t ie[5] = { 7, 3, (uint8_t)l[0], (uint8_t)(l[0] ? l[1] : 0), ' ' };
        return apcountry_ies(ie, sizeof ie, cc);
    }
    struct if_nameindex *ifs = if_nameindex(); int found = 0;
    int sk = ifs ? socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC) : -1;
    struct timeval tv = { 1, 0 };                       /* a second a wait at most (clock.c's thread, not under a lock) */
    if (sk >= 0) setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int fam = sk >= 0 ? family_id(sk) : -1;
    static char b[32768] __attribute__((aligned(4)));   /* a dump datagram (cfg80211 fills up to a page or so each) */
    for (struct if_nameindex *i = ifs; fam > 0 && i && i->if_index && !found; i++) {
        uint32_t idx = i->if_index; int end = 0;
        if (request(sk, (uint16_t)fam, NLM_F_DUMP, NL80211_CMD_GET_SCAN, NL80211_ATTR_IFINDEX, &idx, 4)) break;
        while (!end) {
            ssize_t r;
            do r = recv(sk, b, sizeof b, 0); while (r < 0 && errno == EINTR);   /* SO_RCVTIMEO: never restarted */
            if (r <= 0) { end = 1; fam = -1; break; }   /* timed out or broken: give up, the socket's state is unknown */
            int p = apcountry_parse(b, (size_t)r, found ? (char[3]){ 0 } : cc);
            if (p & APC_FOUND) found = 1;
            if (p & APC_END) end = 1;                   /* read the dump to its end even once found */
        }
    }
    if (sk >= 0) close(sk);
    if (ifs) if_freenameindex(ifs);
    if (!found) cc[0] = 0;
    return found;
}
