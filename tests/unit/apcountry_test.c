/* apcountry.c's parsing: the Country element in an IE list, and the associated BSS's in GET_SCAN dump datagrams as
 * cfg80211 sends them (built here), lengths that lie included.  Under ASan/UBSan (Makefile unit-apcountry). */
#include "apcountry.h"
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }

static uint8_t buf[4096]; static size_t at;
static size_t put_attr(uint16_t type, const void *d, size_t l)  /* returns its offset */
{
    size_t o = at; struct nlattr a = { (uint16_t)(NLA_HDRLEN + l), type };
    memcpy(buf + at, &a, sizeof a); memcpy(buf + at + NLA_HDRLEN, d, l); at += NLA_ALIGN(NLA_HDRLEN + l);
    return o;
}
static size_t begin(uint16_t type)
{
    size_t o = at; struct nlmsghdr n = { 0, type, NLM_F_MULTI, 1, 0 }; struct genlmsghdr g = { 34, 1, 0 };
    memcpy(buf + at, &n, sizeof n); at += NLMSG_HDRLEN; memcpy(buf + at, &g, sizeof g); at += GENL_HDRLEN;
    return o;
}
static void end(size_t o) { uint32_t l = (uint32_t)(at - o); memcpy(buf + o, &l, 4); }
static size_t nest_begin(uint16_t type) { size_t o = at; struct nlattr a = { 0, type }; memcpy(buf + at, &a, sizeof a); at += NLA_HDRLEN; return o; }
static void nest_end(size_t o) { uint16_t l = (uint16_t)(at - o); memcpy(buf + o, &l, 2); }
static void bss(uint32_t status, const uint8_t *ie, size_t il, const uint8_t *beacon, size_t bl, int with_status)
{
    size_t m = begin(28), b = nest_begin(47);            /* NL80211_ATTR_BSS */
    uint8_t bssid[6] = { 0xba, 0xfb, 0xe4, 0x4e, 0x74, 0xfe };
    put_attr(1, bssid, 6);
    if (ie) put_attr(6, ie, il);
    if (with_status) put_attr(9, &status, 4);
    if (beacon) put_attr(11, beacon, bl);
    nest_end(b); end(m);
}
static void done(void) { size_t m = at; struct nlmsghdr n = { NLMSG_HDRLEN + 4, NLMSG_DONE, NLM_F_MULTI, 1, 0 }; memcpy(buf + at, &n, sizeof n); at += NLMSG_HDRLEN + 4; (void)m; }

int main(void)
{
    char cc[3];
    /* the Dot 2's AP, as it sent it: SSID, rates, DS, TIM-less, Country "DE " with its channel triplets */
    const uint8_t de[] = { 0, 2, 'X', 'Y', 1, 2, 0x8c, 0x12, 3, 1, 100, 7, 6, 'D', 'E', ' ', 36, 4, 23 };
    memset(cc, 0, sizeof cc);
    check(apcountry_ies(de, sizeof de, cc) && !strcmp(cc, "DE"), "IEs: DE after SSID, rates, DS");
    const uint8_t xx[] = { 7, 3, 'X', 'X', ' ' }, low[] = { 7, 3, 'd', 'e', ' ' }, shortc[] = { 7, 2, 'D', 'E' };
    check(!apcountry_ies(xx, sizeof xx, cc) && !apcountry_ies(low, sizeof low, cc) && !apcountry_ies(shortc, sizeof shortc, cc),
          "IEs: XX, lower case, an element of 2 bytes: none");
    const uint8_t cut[] = { 0, 30, 'a', 7, 3, 'D', 'E', ' ' }, cut2[] = { 7, 9, 'D', 'E' };
    check(!apcountry_ies(cut, sizeof cut, cc) && !apcountry_ies(cut2, sizeof cut2, cc) && !apcountry_ies(de, 1, cc) && !apcountry_ies(NULL, 0, cc),
          "IEs: lengths past the end: none, nothing read beyond");

    at = 0; bss(0, xx, sizeof xx, NULL, 0, 0); bss(1, de, sizeof de, NULL, 0, 1); done();
    memset(cc, 0, sizeof cc);
    check(apcountry_parse(buf, at, cc) == (APC_FOUND | APC_END) && !strcmp(cc, "DE"), "dump: the associated BSS of two, and the end in the same datagram");
    at = 0; bss(1, de, sizeof de, NULL, 0, 0); bss(2, de, sizeof de, NULL, 0, 1);
    check(apcountry_parse(buf, at, cc) == 0, "dump: no status, or joined an IBSS: not ours, read on");
    const uint8_t fr[] = { 7, 3, 'F', 'R', 'I' }, none[] = { 0, 1, 'x' };
    at = 0; bss(1, none, sizeof none, fr, sizeof fr, 1);
    memset(cc, 0, sizeof cc);
    check(apcountry_parse(buf, at, cc) == APC_FOUND && !strcmp(cc, "FR"), "dump: none in the probe response's IEs, the beacon's then");
    at = 0; done();
    check(apcountry_parse(buf, at, cc) == APC_END, "dump: done");
    at = 0; { struct nlmsghdr n = { NLMSG_HDRLEN + 4, NLMSG_ERROR, 0, 1, 0 }; memcpy(buf, &n, sizeof n); at = NLMSG_HDRLEN + 4; }
    check(apcountry_parse(buf, at, cc) == APC_END, "dump: an error (no wireless there): ended");
    /* lies: every cut of a datagram with the associated BSS, a message longer than the datagram, an attribute longer
     * than its message, a nested length past its parent */
    at = 0; bss(1, de, sizeof de, NULL, 0, 1); size_t whole = at; int bad = 0;
    for (size_t l = 0; l < whole; l++) if (apcountry_parse(buf, l, cc) & APC_FOUND) bad++;
    check(!bad, "dump: every cut of the datagram: nothing found, nothing read beyond");
    uint32_t big = 9999; memcpy(buf, &big, 4);
    check(apcountry_parse(buf, whole, cc) == APC_END, "dump: a message longer than the datagram: ended");
    at = 0; bss(1, de, sizeof de, NULL, 0, 1);
    uint16_t huge = 2000; memcpy(buf + NLMSG_HDRLEN + GENL_HDRLEN, &huge, 2);
    check(apcountry_parse(buf, at, cc) == 0, "dump: a BSS attribute longer than its message: skipped");
    at = 0; bss(1, de, sizeof de, NULL, 0, 1);
    memcpy(buf + NLMSG_HDRLEN + GENL_HDRLEN + NLA_HDRLEN + NLA_HDRLEN + 8, &huge, 2);   /* the IEs' length */
    check(apcountry_parse(buf, at, cc) == 0, "dump: the IEs' length past the BSS: not read");
    return fails ? 1 : 0;
}
