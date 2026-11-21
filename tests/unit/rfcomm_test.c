/* rfcomm.c against a peer written here from the RFCOMM spec (a gadget's side: it answers, it never opens a channel):
 * the FCS on frames from Bluetooth traces, the multiplexer and a channel opened from either side, C/R and direction
 * bits, PN with credit based flow control, credits running out and coming back, frames cut to the MTU, the modem
 * status before data, a channel of ours asked for (none: DM), refusals and an unanswered command. */
#include "rfcomm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, what) do { if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)

/* what rfcomm.c sent */
static unsigned char wire[128][1100]; static size_t wlen[128]; static int nwire;
static unsigned char got[4096]; static size_t ngot; static int ups, downs;

static void out(void *c, const unsigned char *p, size_t n) { (void)c; if (nwire < 128) { memcpy(wire[nwire], p, n); wlen[nwire++] = n; } }
static void data(void *c, const unsigned char *p, size_t n) { (void)c; memcpy(got + ngot, p, n); ngot += n; }
static void state(void *c, int up) { (void)c; if (up) ups++; else downs++; }

static void fresh(struct rfc *s)
{
    memset(s, 0, sizeof *s);
    s->out = out; s->data = data; s->state = state; s->l2mtu = 672;
    rfc_init(s);
    nwire = 0; ngot = 0; ups = downs = 0;
}

/* a frame as the peer sends it */
static void peer(struct rfc *s, int dlci, int cr, int ctrl, int credits, const void *info, size_t n, long long now)
{
    unsigned char b[1100]; size_t o = 0, h;
    b[o++] = dlci << 2 | cr << 1 | 1; b[o++] = ctrl;
    if (n < 128) b[o++] = n << 1 | 1; else { b[o++] = (n & 0x7f) << 1; b[o++] = n >> 7; }
    h = o;
    if (credits >= 0) b[o++] = credits;
    if (n) memcpy(b + o, info, n);
    o += n;
    b[o] = rfc_fcs(b, (ctrl & 0xef) == 0xef ? 2 : h); o++;
    rfc_rx(s, b, o, now);
}

static void peer_mcc(struct rfc *s, int cr, int type, int c, const unsigned char *v, size_t n, long long now)
{
    unsigned char b[32] = { type << 2 | c << 1 | 1, n << 1 | 1 };
    if (n) memcpy(b + 2, v, n);
    peer(s, 0, cr, 0xef, -1, b, 2 + n, now);
}

/* frame i of ours, taken apart; 0 if it is malformed */
struct fr { int dlci, cr, ctrl, pf, credits; const unsigned char *info; size_t len; };
static int parse(int i, struct fr *f, int cfc)
{
    const unsigned char *p = wire[i]; size_t n = wlen[i], h;
    if (n < 4) return 0;
    f->dlci = p[0] >> 2; f->cr = p[0] >> 1 & 1; f->ctrl = p[1] & 0xef; f->pf = !!(p[1] & 0x10);
    if (p[2] & 1) { f->len = p[2] >> 1; h = 3; } else { f->len = p[2] >> 1 | p[3] << 7; h = 4; }
    int cr = f->ctrl == 0xef && f->pf && f->dlci && cfc;
    f->credits = cr ? p[h] : -1;
    if (h + cr + f->len + 1 != n) return 0;
    if (rfc_fcs(p, f->ctrl == 0xef ? 2 : h) != p[n - 1]) return 0;
    f->info = p + h + cr;
    return 1;
}

int main(void)
{
    struct rfc s; struct fr f;

    /* ---- the FCS, on frames seen in Bluetooth traces */
    CHECK(rfc_fcs((const unsigned char[]){ 0x03, 0x3f, 0x01 }, 3) == 0x1c, "FCS: SABM on DLCI 0 from the initiator (03 3f 01 1c)");
    CHECK(rfc_fcs((const unsigned char[]){ 0x03, 0x73, 0x01 }, 3) == 0xd7, "FCS: UA on DLCI 0 from the responder (03 73 01 d7)");
    CHECK(rfc_fcs((const unsigned char[]){ 0x03, 0xef }, 2) == 0x70, "FCS: UIH on DLCI 0 from the initiator, address and control only (70)");

    /* ---- we start the multiplexer and open the clock's channel 4 */
    fresh(&s);
    rfc_connect(&s, 4, 1000);
    CHECK(nwire == 1 && wlen[0] == 4 && !memcmp(wire[0], "\x03\x3f\x01\x1c", 4), "connect: SABM on DLCI 0, C/R 1, P/F (03 3f 01 1c)");
    peer(&s, 0, 1, 0x73, -1, NULL, 0, 1010);                        /* UA: from the responder, C/R 1 in a response */
    CHECK(nwire == 2 && parse(1, &f, 0) && f.dlci == 0 && f.ctrl == 0xef && f.cr == 1 && f.len == 10 && f.info[0] == 0x83 && f.info[1] == 0x11,
          "multiplexer up: PN command on DLCI 0 (type 0x83, 8 bytes)");
    CHECK(f.info[2] == 8 && f.info[3] == 0xf0 && f.info[9] == 7 && (f.info[6] | f.info[7] << 8) == 666,
          "PN: DLCI 8 (channel 4, our side started), CFC asked (F0), 7 credits, MTU 672 - 6");
    { unsigned char pn[8] = { 8, 0xe0, 7, 0, 127, 0, 0, 2 };        /* CFC on, MTU 127, 2 credits for us */
      peer_mcc(&s, 0, 0x20, 0, pn, 8, 1020); }
    CHECK(nwire == 3 && parse(2, &f, 0) && f.dlci == 8 && f.ctrl == 0x2f && f.cr == 1 && f.pf, "PN answered with CFC: SABM on DLCI 8");
    CHECK(rfc_send(&s, "x", 1) < 0, "nothing sent before the channel is open");
    peer(&s, 8, 0, 0x73, -1, NULL, 0, 1030);
    CHECK(nwire == 4 && parse(3, &f, 0) && f.dlci == 0 && f.info[0] == 0xe3 && f.info[2] == (8 << 2 | 3) && f.info[3] == 0x8d,
          "channel open: our MSC (DLCI 8, RTC RTR DV)");
    CHECK(!rfc_open(&s) && ups == 0, "not open for data before the modem status");
    CHECK(rfc_send(&s, "hello", 5) == 0 && nwire == 4, "data queued while the modem status is pending");
    { unsigned char msc[2] = { 8 << 2 | 3, 0x8d }; peer_mcc(&s, 0, 0x38, 1, msc, 2, 1040); }
    CHECK(ups == 1 && rfc_open(&s), "the peer's MSC: open");
    CHECK(nwire == 6 && parse(4, &f, 0) && f.dlci == 0 && f.info[0] == 0xe1, "its MSC answered (type 0xe1: a response)");
    CHECK(parse(5, &f, 1) && f.dlci == 8 && f.ctrl == 0xef && f.len == 5 && !memcmp(f.info, "hello", 5) && f.cr == 1,
          "the queued data went in a UIH frame, C/R 1 (we started the multiplexer)");

    /* credits: the peer gave 2, one went with "hello"; a big write is cut to 127 and waits */
    { unsigned char big[300]; memset(big, 'a', sizeof big); big[0] = 'A'; rfc_send(&s, big, sizeof big); }
    CHECK(nwire == 7 && parse(6, &f, 1) && f.len == 127 && f.info[0] == 'A', "a write of 300: 127 sent on the last credit");
    peer(&s, 8, 0, 0xff, 5, NULL, 0, 1050);                         /* UIH with P/F: 5 credits, no data */
    CHECK(nwire == 9 && parse(7, &f, 1) && f.len == 127 && parse(8, &f, 1) && f.len == 46, "5 credits back: the rest, 127 + 46");
    { int all = 1; for (int i = 6; i < 9; i++) { parse(i, &f, 1); if (f.len > 127) all = 0; } CHECK(all, "no frame over the MTU the peer gave"); }

    /* two writes waiting for credits leave in frames of their own */
    s.tx_credits = 0; nwire = 0;
    rfc_send(&s, "AAA", 3); rfc_send(&s, "BBBB", 4);
    CHECK(nwire == 0, "no credits: both writes wait");
    peer(&s, 8, 0, 0xff, 5, NULL, 0, 1055);
    CHECK(nwire == 2 && parse(0, &f, 1) && f.len == 3 && !memcmp(f.info, "AAA", 3) && parse(1, &f, 1) && f.len == 4 && !memcmp(f.info, "BBBB", 4),
          "credits back: two frames, one per write, not one of both");

    /* data from the peer, and our credits going back to it */
    nwire = 0;
    for (int i = 0; i < 4; i++) peer(&s, 8, 0, 0xef, -1, "abc", 3, 1060 + i);
    CHECK(ngot == 12 && !memcmp(got, "abcabcabcabc", 12), "the peer's data passed on");
    { int granted = 0; for (int i = 0; i < nwire; i++) if (parse(i, &f, 1) && f.dlci == 8 && f.credits > 0) granted += f.credits;
      CHECK(granted >= 4 && granted <= 7, "credits granted back to the peer as it used them"); }

    /* the peer closes the channel */
    nwire = 0;
    peer(&s, 8, 1, 0x53, -1, NULL, 0, 1100);                        /* DISC */
    CHECK(nwire == 1 && parse(0, &f, 0) && f.ctrl == 0x63 && f.dlci == 8 && downs == 1 && !rfc_open(&s), "DISC answered with UA, channel down");

    /* ---- the peer started the multiplexer (it called us): our channel to its 4 is DLCI 9, our commands C/R 0 */
    fresh(&s);
    peer(&s, 0, 1, 0x3f, -1, NULL, 0, 2000);                        /* SABM from the initiator */
    CHECK(nwire == 1 && parse(0, &f, 0) && f.dlci == 0 && f.ctrl == 0x63 && f.cr == 1, "its SABM on DLCI 0: UA with C/R 1 (responder's response)");
    rfc_connect(&s, 4, 2010);
    CHECK(nwire == 2 && parse(1, &f, 0) && f.cr == 0 && f.info[0] == 0x83 && f.info[2] == 9, "connect: PN for DLCI 9, our frames C/R 0");
    { unsigned char pn[8] = { 9, 0x00, 7, 0, 0x00, 0x01, 0, 0 };    /* no CFC, MTU 256 */
      peer_mcc(&s, 1, 0x20, 0, pn, 8, 2020); }
    CHECK(nwire == 3 && parse(2, &f, 0) && f.dlci == 9 && f.ctrl == 0x2f && f.cr == 0, "SABM on DLCI 9 with C/R 0");
    peer(&s, 9, 1, 0x73, -1, NULL, 0, 2030);
    CHECK(ups == 0 && rfc_tick(&s, 2500) == 0 && ups == 0, "open, waiting for the peer's MSC");
    CHECK(rfc_tick(&s, 3100) == 0 && ups == 1 && rfc_open(&s), "no MSC from the peer within a second: open anyway");
    { unsigned char big[300]; memset(big, 'b', sizeof big); nwire = 0; rfc_send(&s, big, sizeof big); }
    CHECK(nwire == 2 && parse(0, &f, 0) && f.len == 256 && !f.pf && parse(1, &f, 0) && f.len == 44, "no CFC: data flows without credits, cut to 256");
    CHECK(parse(0, &f, 0) && f.cr == 0 && wire[0][2] == 0 && wire[0][3] == 2, "a length over 127 in two bytes");

    /* ---- we serve no channel: the peer's SABM for one gets DM, its data on one is dropped */
    nwire = 0;
    peer(&s, 2, 1, 0x3f, -1, NULL, 0, 3200);
    CHECK(nwire == 1 && parse(0, &f, 0) && f.dlci == 2 && f.ctrl == 0x0f, "the peer asks for a channel of ours: DM");
    size_t before = ngot;
    peer(&s, 2, 1, 0xef, -1, "zz", 2, 3220);
    CHECK(ngot == before, "data on a channel not ours is dropped");
    { unsigned char t[3] = { 1, 2, 3 }; nwire = 0; peer_mcc(&s, 1, 0x08, 1, t, 3, 3230); }
    CHECK(nwire == 1 && parse(0, &f, 0) && f.info[0] == 0x21 && f.len == 5 && !memcmp(f.info + 2, "\x01\x02\x03", 3), "TEST echoed back");
    nwire = 0; peer(&s, 6, 1, 0x53, -1, NULL, 0, 3235);
    CHECK(nwire == 1 && parse(0, &f, 0) && f.dlci == 6 && f.ctrl == 0x0f, "DISC for a channel not open: DM");
    { nwire = 0; peer_mcc(&s, 1, 0x3e, 1, NULL, 0, 3240); }
    CHECK(nwire == 1 && parse(0, &f, 0) && f.info[0] == 0x11 && f.info[2] == 0xfb, "an unknown command: NSC naming it");

    /* ---- refusals and silence */
    fresh(&s);
    rfc_connect(&s, 4, 4000);
    peer(&s, 0, 1, 0x0f, -1, NULL, 0, 4010);
    CHECK(downs == 1 && s.mux == 0, "DM to our SABM: refused, told");
    fresh(&s);
    rfc_connect(&s, 4, 5000);
    peer(&s, 0, 1, 0x73, -1, NULL, 0, 5010);
    { unsigned char pn[8] = { 8, 0xe0, 7, 0, 127, 0, 0, 7 }; peer_mcc(&s, 0, 0x20, 0, pn, 8, 5020); }
    peer(&s, 8, 0, 0x0f, -1, NULL, 0, 5030);
    CHECK(downs == 1 && !s.dlc && s.mux == 2, "DM to the channel's SABM: refused, the multiplexer stays");
    fresh(&s);
    rfc_connect(&s, 4, 6000);
    CHECK(rfc_tick(&s, 6000 + 19999) == 0 && rfc_tick(&s, 6000 + 20001) < 0 && downs == 1, "no UA in 20 s: the tick says so");
    { unsigned char bad[4] = { 0x03, 0x73, 0x01, 0x00 }; fresh(&s); rfc_connect(&s, 4, 7000); rfc_rx(&s, bad, 4, 7010);
      CHECK(s.mux == 1, "a frame with a wrong FCS is ignored"); }

    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
