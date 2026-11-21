/*
 * RFCOMM, as much as a serial link to one device needs (Bluetooth RFCOMM 1.2, after ETSI TS 07.10): the multiplexer
 * on DLCI 0, one data channel of ours to the peer's server channel, credit based flow control, and polite answers to
 * the rest.  Written for the Echo Wall Clock (wallclock.c), which serves Alexa Gadgets on its RFCOMM channel 4: we open
 * that channel in a session of ours, as an Echo Show does (its HCI log, 2026-11-21).  We serve no channel ourselves: a peer's SABM for one gets DM, though either side may have started the
 * multiplexer (a peer's PN is still answered, with its parameters).
 *
 *   frame     address (DLCI, C/R, EA), control (type, P/F), length (1 or 2 bytes), [credits], information, FCS.  The FCS
 *             covers address and control for UIH, address, control and length for the rest (TS 07.10 5.2.1.6).
 *   C/R       commands carry 1 from the multiplexer's initiator and 0 from the responder, responses the other way;
 *             UIH frames always say who sends them (the initiator's 1).
 *   DLCI      server channel << 1, and 1 in the direction bit when the channel's server is the multiplexer's initiator.
 *   writes    each in frames of its own, never two in one (as a Linux RFCOMM socket sends them): a gadget may read
 *             one message per frame.
 *   CFC       asked for in PN (0xF0, answered 0xE0): each data frame costs a credit, granted in a byte after the length
 *             of a UIH frame with P/F set.  We grant 7 at a time (PN carries no more), again whenever half are used, on our
 *             next data frame or in an empty one.
 *
 * Everything runs on the controller thread (a2dp.c), one session per L2CAP channel; the tests drive it directly.
 */
#include "rfcomm.h"
#include <stdio.h>
#include <string.h>

enum { SABM = 0x2f, UA = 0x63, DM = 0x0f, DISC = 0x43, UIH = 0xef, PF = 0x10 };
enum { M_TEST = 0x08, M_FCON = 0x28, M_FCOFF = 0x18, M_MSC = 0x38, M_RPN = 0x24, M_RLS = 0x14, M_PN = 0x20, M_NSC = 0x04 };
#define CREDITS 7                       /* granted at a time: PN's field has 3 bits */
#define CMD_MS 20000                    /* a command unanswered that long: the peer is gone (TS 07.10's T1 is shorter, but
                                           Linux waits 20 s for a DISC and a paired gadget may be slow after its pairing) */
#define MSC_WAIT_MS 1000                /* data waits for the peer's modem status this long, at most */
#define V24 0x8d                        /* EA, RTC, RTR, DV: ready, as every stack says it */

static unsigned char crc[256];

static void crc_init(void)              /* CRC-8, x^8 + x^2 + x + 1, bits reversed (TS 07.10 annex B) */
{
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xe0 : c >> 1;
        crc[i] = c;
    }
}

unsigned char rfc_fcs(const unsigned char *p, size_t n)
{
    if (!crc[1]) crc_init();
    unsigned f = 0xff;
    while (n--) f = crc[f ^ *p++];
    return 0xff - f;
}

static int cr_cmd(const struct rfc *s) { return s->initiator == 1; }

/* one frame; credits >= 0: the credit byte (UIH with P/F) */
static void frame(struct rfc *s, int dlci, int cr, int ctrl, int credits, const unsigned char *info, size_t n)
{
    unsigned char b[8 + 1024]; size_t o = 0, h;
    if (n > sizeof b - 8) return;
    b[o++] = dlci << 2 | cr << 1 | 1;
    b[o++] = ctrl;
    if (n < 128) b[o++] = n << 1 | 1; else { b[o++] = (n & 0x7f) << 1; b[o++] = n >> 7; }
    h = o;
    if (credits >= 0) b[o++] = credits;
    if (n) memcpy(b + o, info, n);
    o += n;
    b[o] = rfc_fcs(b, (ctrl & ~PF) == UIH ? 2 : h); o++;
    s->out(s->ctx, b, o);
}

static void command(struct rfc *s, int dlci, int type, long long now) { frame(s, dlci, cr_cmd(s), type | PF, -1, NULL, 0); s->cmd_at = now ? now : 1; }
static void response(struct rfc *s, int dlci, int type) { frame(s, dlci, !cr_cmd(s), type | PF, -1, NULL, 0); }

/* a multiplexer control message on DLCI 0: type, C/R (1 command), value */
static void mcc(struct rfc *s, int type, int c, const unsigned char *v, size_t n)
{
    unsigned char b[2 + 125];                           /* a TEST echoed whole (one length byte: up to 127) */
    if (n > sizeof b - 2) n = sizeof b - 2;
    b[0] = type << 2 | c << 1 | 1; b[1] = n << 1 | 1;
    if (n) memcpy(b + 2, v, n);
    frame(s, 0, cr_cmd(s), UIH, -1, b, 2 + n);
}

static void pn(struct rfc *s, int c, int dlci, int flow, int credits, unsigned mtu)
{
    unsigned char v[8] = { dlci, flow, 7, 0, mtu & 0xff, mtu >> 8, 0, credits };   /* priority 7, as Linux; no ack timer, retries */
    mcc(s, M_PN, c, v, 8);
}

static unsigned mtu_max(const struct rfc *s) { return s->l2mtu > 6 + 1000 ? 1000 : s->l2mtu > 6 + 23 ? s->l2mtu - 6 : 23; }

static void dlc_open(struct rfc *s, long long now)
{
    s->dlci = s->chan << 1 | (s->initiator == 1 ? 0 : 1);
    s->dlc = 1;
    fprintf(stderr, "rfcomm: asking for channel %d as DLCI %d\n", s->chan, s->dlci); s->mtu = mtu_max(s); s->cfc = s->tx_credits = s->rx_credits = 0; s->msc_sent = 0; s->txn = 0; s->ntx = 0;
    pn(s, 1, s->dlci, 0xf0, CREDITS, s->mtu);
    s->cmd_at = now ? now : 1;
}

static void dlc_gone(struct rfc *s)
{
    int was = s->dlc;
    s->dlc = 0; s->txn = 0; s->ntx = 0; s->cmd_at = 0; s->msc_sent = 0; s->cfc = 0;
    if (was) s->state(s->ctx, 0);
}

static void grant(struct rfc *s)        /* credits back to the peer, in a frame of their own when no data goes out */
{
    if (!s->cfc || s->rx_credits > CREDITS / 2) return;
    int give = CREDITS - s->rx_credits;
    frame(s, s->dlci, cr_cmd(s), UIH | PF, give, NULL, 0);
    s->rx_credits += give;
}

static void flush(struct rfc *s)
{
    while (s->txn && s->ntx && s->dlc == 3 && s->msc_sent > 1 && (!s->cfc || s->tx_credits > 0)) {
        size_t k = s->txlen[0] < s->mtu ? s->txlen[0] : s->mtu;
        int give = s->cfc && s->rx_credits < CREDITS ? CREDITS - s->rx_credits : -1;      /* credits ride along, as an Echo's */
        frame(s, s->dlci, cr_cmd(s), give > 0 ? UIH | PF : UIH, give > 0 ? give : -1, s->txq, k);
        if (give > 0) s->rx_credits += give;
        if (s->cfc) s->tx_credits--;
        memmove(s->txq, s->txq + k, s->txn - k); s->txn -= k;
        if (!(s->txlen[0] -= k)) memmove(s->txlen, s->txlen + 1, sizeof *s->txlen * (size_t)--s->ntx);
    }
}

/* the channel is open and the modem status went both ways (or the peer never said its own): data may flow */
static void ready(struct rfc *s)
{
    if (s->msc_sent > 1) return;
    s->msc_sent = 2;
    s->state(s->ctx, 1);
    flush(s);
}

void rfc_init(struct rfc *s)
{
    if (!crc[1]) crc_init();
    s->initiator = -1; s->mux = 0; s->chan = s->dlci = s->dlc = 0; s->cfc = s->tx_credits = s->rx_credits = 0;
    s->mtu = 127; s->msc_sent = 0; s->cmd_at = 0; s->txn = 0; s->ntx = 0;
}

void rfc_connect(struct rfc *s, int channel, long long now)
{
    s->chan = channel;
    if (s->dlc) return;
    if (s->mux == 2) dlc_open(s, now);
    else if (!s->mux) { s->initiator = 1; s->mux = 1; command(s, 0, SABM, now); }
}

static void mcc_rx(struct rfc *s, const unsigned char *p, size_t n, long long now)
{
    while (n >= 2) {
        if (!(p[0] & 1)) return;                            /* types of more than a byte: none defined */
        int type = p[0] >> 2, c = p[0] >> 1 & 1; size_t len, h;
        if (p[1] & 1) { len = p[1] >> 1; h = 2; }
        else { if (n < 3) return; len = p[1] >> 1 | (size_t)p[2] << 7; h = 3; }
        if (h + len > n) return;
        const unsigned char *v = p + h;
        switch (type) {
        case M_PN:
            if (len < 8) break;
            if (c) {                                        /* the peer's channel to us: what it asks, CFC if it does */
                unsigned m = v[4] | v[5] << 8, max = mtu_max(s); int cfc = (v[1] & 0xf0) == 0xf0, dl = v[0] & 0x3f;
                if (m == 0 || m > max) m = max;
                if (m < 23) m = 23;                         /* RFCOMM's least */
                pn(s, 0, dl, cfc ? 0xe0 : 0, cfc ? CREDITS : 0, m);
            } else if ((v[0] & 0x3f) == s->dlci && s->dlc == 1) {         /* the answer to ours */
                unsigned m = v[4] | v[5] << 8;
                s->cfc = (v[1] & 0xf0) == 0xe0;
                s->tx_credits = s->cfc ? v[7] & 7 : 0; s->rx_credits = s->cfc ? CREDITS : 0;
                if (m >= 23 && m < s->mtu) s->mtu = m;
                fprintf(stderr, "rfcomm: channel %d: frames of %u bytes, %s\n", s->chan, s->mtu, s->cfc ? "credit flow control" : "no credits");
                s->dlc = 2; command(s, s->dlci, SABM, now);
            }
            break;
        case M_MSC:
            if (len < 2) break;
            if (c) {
                mcc(s, M_MSC, 0, v, len);                   /* its own, answered as it came */
                if ((v[0] >> 2) == s->dlci && s->dlc == 3) ready(s);
            }
            break;
        case M_RPN:                                         /* port settings: whatever it says (one byte: it asks ours) */
            if (c) {
                unsigned char r[8] = { v[0], 0x07, 0x03, 0x00, 0x11, 0x13, 0x7f, 0x3f };   /* 115200 8N1, no flow control */
                if (len >= 8) memcpy(r, v, 8);
                mcc(s, M_RPN, 0, r, 8);
            }
            break;
        case M_RLS: case M_TEST:
            if (c) mcc(s, type, 0, v, len);
            break;
        case M_FCON: case M_FCOFF:                          /* aggregate flow control: CFC makes it moot, but answer */
            if (c) mcc(s, type, 0, NULL, 0);
            break;
        case M_NSC:
            break;
        default:
            if (c) { unsigned char t = p[0]; mcc(s, M_NSC, 0, &t, 1); }
            break;
        }
        p += h + len; n -= h + len;
    }
}

void rfc_rx(struct rfc *s, const unsigned char *p, size_t n, long long now)
{
    if (n < 4) return;
    int dlci = p[0] >> 2, ctrl = p[1] & ~PF, pf = p[1] & PF; size_t h, len;
    if (p[2] & 1) { len = p[2] >> 1; h = 3; }
    else { if (n < 5) return; len = p[2] >> 1 | (size_t)p[3] << 7; h = 4; }
    int credit = ctrl == UIH && pf && dlci && dlci == s->dlci && s->cfc;
    if (h + credit + len + 1 != n) { fprintf(stderr, "rfcomm: frame of %zu bytes says %zu, ignored\n", n, len); return; }
    if (rfc_fcs(p, ctrl == UIH ? 2 : h) != p[n - 1]) { fprintf(stderr, "rfcomm: bad FCS on DLCI %d, ignored\n", dlci); return; }
    const unsigned char *info = p + h + credit;
    switch (ctrl) {
    case SABM:
        if (!dlci) {
            if (!s->mux) { s->initiator = 0; s->mux = 2; }  /* both at once (ours in flight): ours stands, theirs is answered */
            response(s, 0, UA);
            if (s->chan && !s->dlc && s->mux == 2) dlc_open(s, now);
        } else response(s, dlci, DM);                      /* we serve no channel */
        break;
    case UA:
        if (!dlci && s->mux == 1) { s->mux = 2; s->cmd_at = 0; if (s->chan) dlc_open(s, now); }
        else if (dlci && dlci == s->dlci && s->dlc == 2) {
            unsigned char v[2] = { s->dlci << 2 | 3, V24 };
            s->dlc = 3; s->cmd_at = now ? now : 1;          /* now the modem status's wait */
            mcc(s, M_MSC, 1, v, 2); s->msc_sent = 1;
        }
        break;
    case DM:
        if (!dlci && s->mux == 1) {
            fprintf(stderr, "rfcomm: the peer refused the multiplexer\n");
            s->mux = 0; s->initiator = -1; s->cmd_at = 0; s->state(s->ctx, 0);
        } else if (dlci && dlci == s->dlci && s->dlc) {
            if (s->dlc < 3) fprintf(stderr, "rfcomm: the peer refused channel %d\n", s->chan);
            dlc_gone(s);
        }
        break;
    case DISC:
        if (dlci && (dlci != s->dlci || !s->dlc)) { response(s, dlci, DM); break; }      /* not open: DM (07.10 5.4.4.3) */
        response(s, dlci, UA);
        if (!dlci) { s->mux = 0; s->initiator = -1; dlc_gone(s); }
        else dlc_gone(s);
        break;
    case UIH:
        if (!dlci) { mcc_rx(s, info, len, now); break; }
        if (dlci != s->dlci || s->dlc != 3) { if (len) fprintf(stderr, "rfcomm: %zu bytes on DLCI %d, not ours\n", len, dlci); break; }
        if (credit) { s->tx_credits += p[h]; if (s->tx_credits > 255) s->tx_credits = 255; }
        if (len) {
            if (s->msc_sent < 2) ready(s);                  /* data from the peer: it is ready, whatever its MSC */
            if (s->cfc && s->rx_credits > 0) s->rx_credits--;
            grant(s);
            s->data(s->ctx, info, len);
        }
        flush(s);
        break;
    }
}

int rfc_send(struct rfc *s, const void *p, size_t n)
{
    if (s->dlc != 3 || !n || s->txn + n > sizeof s->txq || s->ntx == (int)(sizeof s->txlen / sizeof *s->txlen)) return -1;
    memcpy(s->txq + s->txn, p, n); s->txn += n; s->txlen[s->ntx++] = n;
    flush(s);
    return 0;
}

int rfc_tick(struct rfc *s, long long now)
{
    if (s->dlc == 3 && s->msc_sent == 1 && now - s->cmd_at >= MSC_WAIT_MS) { s->cmd_at = 0; ready(s); return 0; }
    if (s->dlc == 3 && s->msc_sent > 1) s->cmd_at = 0;
    if (!s->cmd_at || now - s->cmd_at < CMD_MS) return 0;
    fprintf(stderr, "rfcomm: no answer from the peer (multiplexer %d, channel %d)\n", s->mux, s->dlc);
    s->cmd_at = 0;
    if (s->mux == 1) { s->mux = 0; s->initiator = -1; s->state(s->ctx, 0); }
    else dlc_gone(s);
    return -1;
}

int rfc_open(const struct rfc *s) { return s->dlc == 3 && s->msc_sent > 1; }

void rfc_close(struct rfc *s)
{
    if (s->dlc >= 2) frame(s, s->dlci, cr_cmd(s), DISC | PF, -1, NULL, 0);
    if (s->mux == 2) frame(s, 0, cr_cmd(s), DISC | PF, -1, NULL, 0);
    int had = s->dlc;
    rfc_init(s);
    if (had) s->state(s->ctx, 0);
}
