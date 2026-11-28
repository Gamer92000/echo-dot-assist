/* The application support sublayer (Zigbee spec 2.2, 4.4): data frames to endpoints (unicast, broadcast, group), APS
 * acks and their retries, duplicates dropped, link-key security, and the trust center's commands.
 * As the trust center (coordinator): the network key to every device that joins, sealed with the key-transport key
 * of its link key (the global "ZigBeeAlliance09", or its install code's), sent straight to a child of ours or
 * tunnelled through the router that announced it (update device); a key of its own to each device that asks (Zigbee
 * 3.0's request key), confirmed once it proves it has it (verify key).  As a joiner (router, end device): the network
 * key taken, then a key of our own asked for and verified, update devices sent for our children, tunnelled keys
 * passed on to them. */
#include "zb.h"
#include <stdlib.h>
#include <string.h>

#define AF_DATA 0
#define AF_CMD 1
#define AF_ACK 2
#define DM_UNICAST 0
#define DM_BROADCAST 2
#define DM_GROUP 3
#define AC_TRANSPORT_KEY 0x05
#define AC_UPDATE_DEVICE 0x06
#define AC_REMOVE_DEVICE 0x07
#define AC_REQUEST_KEY 0x08
#define AC_SWITCH_KEY 0x09
#define AC_TUNNEL 0x0E
#define AC_VERIFY_KEY 0x0F
#define AC_CONFIRM_KEY 0x10
#define KT_NETWORK 0x01
#define KT_TCLK 0x04
#define KID_DATA 0
#define KID_NWK 1
#define KID_TRANSPORT 2
#define KID_LOAD 3
#define ASDU_MAX 82
/* fragmentation (2.2.8.4.5): a unicast ASDU longer than a frame holds goes in blocks of FRAG_BLOCK, each acked
 * (a window of one, as EmberZNet's default), the first block's extended header naming how many follow */
#define FRAG_MAX 1024                   /* apsMaxSizeASDU, both ways (node descriptor's maximum transfer sizes) */
#define FRAG_BLOCK 64                   /* room left in a frame for a source route of several relays */
#define FRAG_BLOCKS (FRAG_MAX / FRAG_BLOCK)
#define FRAG_RX 4                       /* reassemblies at once */
#define FRAG_RX_MS 12000                /* a reassembly's time without a new block; a finished one's for late resends */

static struct dup { uint16_t src; uint8_t counter; int64_t until; } dups[48];
static struct await {
    uint8_t used, counter, tries, f[110], n; uint16_t dst; uint64_t dst_ieee;
    void (*done)(int, void *); void *arg; int64_t until;
} awaits[ZB_APS_WAIT];
static uint8_t pending_key[16]; static uint8_t pending_seq; static int have_pending;
static int tclk_asked;                  /* a joiner: we asked the trust center for a link key of our own */

static int dup_seen(uint16_t src, uint8_t counter)
{
    int64_t now = now_ms(); int free_ = -1;
    for (int i = 0; i < 48; i++) {
        if (dups[i].until > now && dups[i].src == src && dups[i].counter == counter) return 1;
        if (dups[i].until <= now && free_ < 0) free_ = i;
    }
    if (free_ < 0) free_ = (int)(zb_random() % 48);
    dups[free_] = (struct dup){ src, counter, now + 12000 };
    return 0;
}

/* the link key this node shares with peer (the TC with a device, a device with the TC) */
static const uint8_t *link_key(uint64_t peer)
{
    struct node *n = node_by_ieee(peer);
    return n && n->has_key ? n->key : zb.tclk;
}

/* an APS frame (header h, hl bytes; payload in place after it, pn bytes) secured with key and key id: the aux header
 * goes between, the MIC after.  The whole length */
static size_t secure(uint8_t *f, size_t hl, size_t pn, const uint8_t key[16], int kid)
{
    uint8_t pay[100], nonce[13], k[16];
    if (pn > sizeof pay) return 0;
    memcpy(pay, f + hl, pn);
    f[0] |= 1 << 5;
    uint8_t *aux = f + hl;
    uint32_t fc = zb.aps_fc++;
    aux[0] = (uint8_t)(kid << 3 | 0x20);
    wr32(aux + 1, fc); wr64(aux + 5, zb.ieee);
    size_t al = hl + 13;
    memcpy(f + al, pay, pn);
    if (kid == KID_TRANSPORT) keyed_hash(key, 0x00, k);
    else if (kid == KID_LOAD) keyed_hash(key, 0x02, k);
    else memcpy(k, key, 16);
    wr64(nonce, zb.ieee); wr32(nonce + 8, fc); nonce[12] = (uint8_t)(aux[0] | 5);
    aux[0] |= 5;
    ccm_encrypt(k, nonce, f, al, f + al, pn);
    aux[0] &= (uint8_t)~7;
    if (zb.aps_fc % 256 == 0) zb_save_soon();
    return al + pn + 4;
}

/* an APS frame secured by a peer: undone in place.  The payload's offset and length; -1 */
static uint8_t used_key[16];            /* the link key the last frame unsealed with */
static int unsecure(uint8_t *f, size_t n, size_t hl, uint64_t from, size_t *po, size_t *pn, uint64_t *sender)
{
    if (hl + 5 > n) return -1;
    uint8_t *aux = f + hl, sc = aux[0], nonce[13], k[16];
    int kid = sc >> 3 & 3;
    size_t al = hl + 5;
    uint64_t src = from;
    if (sc & 0x20) { if (al + 8 > n) return -1; src = rd64(aux + 5); al += 8; }
    if (kid == KID_NWK) al++;
    if (al + 4 > n || !src) return -1;
    /* the keys we may share with it: its own, then the global (a device that asks for its own has none yet).  Never
     * the global one for a peer whose own key is proven or was never on the air (an install code's): anyone in the
     * network could seal a frame as that peer with it (and ask for its key, or make it verify one) */
    struct node *pn_ = node_by_ieee(src);
    int own_only = pn_ && pn_->has_key && (pn_->key_verified || pn_->key_preconf);
    const uint8_t *keys[2] = { link_key(src), own_only ? link_key(src) : zb.tclk };
    for (int t = 0; t < 2; t++) {
        if (t && keys[1] == keys[0]) break;
        if (kid == KID_TRANSPORT) keyed_hash(keys[t], 0x00, k);
        else if (kid == KID_LOAD) keyed_hash(keys[t], 0x02, k);
        else if (kid == KID_NWK) memcpy(k, zb.nwk_key, 16);
        else memcpy(k, keys[t], 16);
        uint8_t copy[ZB_MAX_FRAME];
        memcpy(copy, f, n);
        wr64(nonce, src); wr32(nonce + 8, rd32(aux + 1)); nonce[12] = (uint8_t)((sc & ~7) | 5);
        copy[hl] = (uint8_t)((sc & ~7) | 5);
        if (!ccm_decrypt(k, nonce, copy, al, copy + al, n - al)) {
            memcpy(f, copy, n);
            memcpy(used_key, keys[t], 16);
            *po = al; *pn = n - al - 4; *sender = src;
            return 0;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- acks */

static void await_resend(void *arg);

static void await_done(struct await *w, int st)
{
    if (!w->used) return;
    w->used = 0;
    timer_cancel(await_resend, w);
    if (w->done) w->done(st, w->arg);
}

/* the MAC's word on a send: its arg names the slot and the transfer it was for (an indirect frame's comes up to
 * 7.68 s late, when the slot may hold another one) */
static uint16_t aw_gen[ZB_APS_WAIT];
static void *aw_tag(struct await *w) { int i = (int)(w - awaits); return (void *)(uintptr_t)(1u << 24 | (unsigned)aw_gen[i] << 8 | (unsigned)i); }
static void sent_nwk(int st, void *arg)
{
    unsigned v = (unsigned)(uintptr_t)arg, i = v & 0xFF;
    if (i >= ZB_APS_WAIT || (v >> 8 & 0xFFFF) != aw_gen[i]) return;
    struct await *w = &awaits[i];
    if (st && w->used && w->tries >= 3) await_done(w, st);
}

static void await_resend(void *arg)
{
    struct await *w = arg;
    if (!w->used) return;
    if (w->tries >= 3) { await_done(w, APS_ST_NO_ACK); return; }
    w->tries++;
    nwk_send(w->dst, w->dst_ieee, w->f, w->n, 1, 0, NULL, 0, sent_nwk, aw_tag(w));
    struct node *d = node_by_nwk(w->dst);
    int sleepy = d && !(d->cap & 0x08) && d->cap;
    timer_at(now_ms() + (sleepy ? 8000 : 1600), await_resend, w);
}

static void send_ack(uint16_t dst, const uint8_t *h, int cmd)
{
    uint8_t a[10], *p = a;
    if (cmd) { *p++ = AF_ACK | 1 << 4; *p++ = h[1]; }         /* a command's ack: the counter alone */
    else {
        /* ours to its: its source endpoint our destination's, cluster, profile, our endpoint, its counter */
        *p++ = AF_ACK;
        *p++ = h[6]; p = wr16(p, rd16(h + 2)); p = wr16(p, rd16(h + 4)); *p++ = h[1]; *p++ = h[7];
    }
    nwk_send(dst, 0, a, (size_t)(p - a), 1, 0, NULL, 0, NULL, NULL);
}

/* ---------------------------------------------------------------- fragmentation */

static struct ftx {
    uint8_t used, counter, block, blocks, tries, dst_ep, src_ep, radius, nrelays;
    uint16_t dst, cluster, profile, relays[10]; uint64_t dst_ieee;
    uint8_t data[FRAG_MAX]; size_t n;
    void (*done)(int, void *); void *arg;
} ftxs[2];

static void ftx_resend(void *arg);

static void ftx_end(struct ftx *t, int st)
{
    if (!t->used) return;
    t->used = 0;
    timer_cancel(ftx_resend, t);
    if (t->done) t->done(st, t->arg);
}

static uint16_t fx_gen[2];               /* per send: a late MAC word ends only the send it was for */
static void ftx_sent(int st, void *arg)
{
    unsigned v = (unsigned)(uintptr_t)arg, i = v & 0xFF;
    if (i >= 2 || (v >> 8 & 0xFFFF) != fx_gen[i]) return;
    struct ftx *t = &ftxs[i];
    if (st && t->used && t->tries >= 3) ftx_end(t, st);
}

static void ftx_send(struct ftx *t)
{
    uint8_t f[110], *p = f;
    size_t off = (size_t)t->block * FRAG_BLOCK, len = t->n - off < FRAG_BLOCK ? t->n - off : FRAG_BLOCK;
    *p++ = (uint8_t)(AF_DATA | DM_UNICAST << 2 | 1 << 6 | 0x80);       /* ack wanted, extended header */
    *p++ = t->dst_ep; p = wr16(p, t->cluster); p = wr16(p, t->profile); *p++ = t->src_ep; *p++ = t->counter;
    *p++ = t->block ? 2 : 1;                                            /* the first block, or one after it */
    *p++ = t->block ? t->block : t->blocks;                             /* the first names how many in all */
    memcpy(p, t->data + off, len); p += len;
    int ti = (int)(t - ftxs);
    fx_gen[ti]++;
    nwk_send(t->dst, t->dst_ieee, f, (size_t)(p - f), 1, t->radius, t->nrelays ? t->relays : NULL, t->nrelays, ftx_sent,
             (void *)(uintptr_t)(1u << 24 | (unsigned)fx_gen[ti] << 8 | (unsigned)ti));
    struct node *d = node_by_nwk(t->dst);
    int sleepy = d && !(d->cap & 0x08) && d->cap;
    timer_at(now_ms() + (sleepy ? 8000 : 1600), ftx_resend, t);
}

static void ftx_resend(void *arg)
{
    struct ftx *t = arg;
    if (!t->used) return;
    if (t->tries >= 3) { ftx_end(t, APS_ST_NO_ACK); return; }
    t->tries++;
    ftx_send(t);
}

/* an ack with an extended header: the blocks it names taken, the next one sent */
static void ftx_acked(uint16_t src, uint8_t counter, uint8_t first, uint8_t bits)
{
    for (int i = 0; i < 2; i++) {
        struct ftx *t = &ftxs[i];
        if (!t->used || t->counter != counter || t->dst != src) continue;
        if (t->block < first || t->block - first >= 8 || !(bits >> (t->block - first) & 1)) return;
        timer_cancel(ftx_resend, t);
        if (++t->block == t->blocks) { ftx_end(t, 0); return; }
        t->tries = 1;
        ftx_send(t);
        return;
    }
}

static int ftx_start(uint16_t dst, uint64_t dst_ieee, uint8_t dst_ep, uint8_t src_ep, uint16_t profile, uint16_t cluster,
                     const uint8_t *data, size_t n, uint8_t radius, const uint16_t *relays, int nrelays, void (*done)(int, void *), void *arg)
{
    struct ftx *t = !ftxs[0].used ? &ftxs[0] : !ftxs[1].used ? &ftxs[1] : NULL;
    if (!t) { if (done) done(0xA6, arg); return -1; }       /* busy */
    memset(t, 0, sizeof *t);
    t->used = 1; t->counter = zb.aps_seq++; t->blocks = (uint8_t)((n + FRAG_BLOCK - 1) / FRAG_BLOCK); t->tries = 1;
    t->dst = dst; t->dst_ieee = dst_ieee; t->dst_ep = dst_ep; t->src_ep = src_ep; t->profile = profile; t->cluster = cluster;
    t->radius = radius; t->done = done; t->arg = arg; t->n = n; memcpy(t->data, data, n);
    if (relays && nrelays > 0 && nrelays <= 10) { t->nrelays = (uint8_t)nrelays; memcpy(t->relays, relays, sizeof *relays * (size_t)nrelays); }
    LOG("aps: %zu bytes to %04x in %u blocks", n, dst, t->blocks);
    ftx_send(t);
    return 0;
}

/* the receiving side: blocks kept by sender and counter until all are in, each acked as it comes */
static struct frx {
    uint8_t used, done, counter, blocks; uint16_t src; uint32_t got; int64_t until;
    uint8_t len[FRAG_BLOCKS]; uint8_t data[FRAG_BLOCKS][FRAG_BLOCK + 32];
} frxs[FRAG_RX];

static void frag_ack(uint16_t dst, const uint8_t *h, uint8_t frag, uint8_t block)
{
    uint8_t a[12], *p = a;
    *p++ = AF_ACK | 0x80;
    *p++ = h[6]; p = wr16(p, rd16(h + 2)); p = wr16(p, rd16(h + 4)); *p++ = h[1]; *p++ = h[7];
    *p++ = frag; *p++ = block; *p++ = 0x01;           /* this block (a window of one) */
    nwk_send(dst, 0, a, (size_t)(p - a), 1, 0, NULL, 0, NULL, NULL);
}

/* a block (f: the data frame's header, unicast; ext: its extended frame control, blk its block number; the block's
 * bytes d, dn); 1 with *out the whole ASDU once complete */
static int frag_rx(uint16_t src, const uint8_t *f, uint8_t ext, uint8_t blk, const uint8_t *d, size_t dn, const uint8_t **out, size_t *on)
{
    static uint8_t whole[FRAG_MAX + FRAG_BLOCKS * 32];
    int64_t now = now_ms();
    uint8_t counter = f[7], frag = ext & 3;
    unsigned idx = frag == 1 ? 0 : blk;
    struct frx *r = NULL;
    for (int i = 0; i < FRAG_RX; i++)
        if (frxs[i].used && frxs[i].until > now && frxs[i].src == src && frxs[i].counter == counter) { r = &frxs[i]; break; }
    if (!r) {
        if (frag != 1 || !blk || blk > FRAG_BLOCKS || dn > FRAG_BLOCK + 32) return 0;   /* only a first block begins one */
        for (int i = 0; i < FRAG_RX && !r; i++) if (!frxs[i].used || frxs[i].until <= now) r = &frxs[i];
        if (!r) return 0;               /* all busy: no ack, the sender tries again */
        memset(r, 0, sizeof *r);
        r->used = 1; r->src = src; r->counter = counter; r->blocks = blk;
    }
    if (idx >= r->blocks || dn > FRAG_BLOCK + 32) return 0;
    r->until = now + FRAG_RX_MS;
    frag_ack(src, f, frag, (uint8_t)idx);
    if (r->done || r->got >> idx & 1) return 0;          /* a resend (our ack was lost): acked again only */
    memcpy(r->data[idx], d, dn); r->len[idx] = (uint8_t)dn; r->got |= 1u << idx;
    if (r->got != (r->blocks == 32 ? 0xFFFFFFFFu : (1u << r->blocks) - 1)) return 0;
    size_t k = 0;
    for (unsigned i = 0; i < r->blocks; i++) { memcpy(whole + k, r->data[i], r->len[i]); k += r->len[i]; }
    if (k > FRAG_MAX) { r->used = 0; return 0; }
    r->done = 1;                        /* kept a while, for resends of the last block */
    *out = whole; *on = k;
    return 1;
}

/* ---------------------------------------------------------------- data */

static void deliver(struct apsind *in)
{
    if (in->dst_ep == 0 && in->profile == 0x0000) {
        zdo_rx(in);
        if (zb.role == R_COORD) dz_indication(in);
        return;
    }
    if (zb.role == R_COORD) dz_indication(in);
    else zcl_rx(in);
}

/* the host's request to this node itself: answered here as the network would be */
static void loopback(uint8_t dst_ep, uint8_t src_ep, uint16_t profile, uint16_t cluster, const uint8_t *data, size_t n)
{
    struct apsind in = { .src = zb.nwk, .src_ieee = zb.ieee, .src_ep = src_ep, .dst = zb.nwk, .dst_mode = 2, .dst_ep = dst_ep,
                         .profile = profile, .cluster = cluster, .data = data, .n = n, .lqi = 255 };
    if (dst_ep == 0 && profile == 0) {
        uint8_t rsp[120]; size_t rn = 0; uint16_t rc;
        if (zdo_local(&in, rsp, &rn, &rc)) {
            struct apsind r = in; r.cluster = rc; r.data = rsp; r.n = rn;
            dz_indication(&r);
        }
        return;
    }
    dz_indication(&in);
}

int aps_send(int dst_mode, uint16_t dst, uint64_t dst_ieee, uint8_t dst_ep, uint8_t src_ep, uint16_t profile, uint16_t cluster,
             const uint8_t *data, size_t n, int ack, uint8_t radius, const uint16_t *relays, int nrelays,
             void (*done)(int, void *), void *arg)
{
    uint8_t f[110], *p = f;
    if (n > FRAG_MAX) { if (done) done(APS_ST_FRAME_TOO_LONG, arg); return -1; }
    if (dst_mode == 3) {
        if (dst_ieee == zb.ieee) dst = zb.nwk;
        else { struct node *d = node_by_ieee(dst_ieee); if (!d || d->nwk >= 0xFFF8) { if (done) done(APS_ST_NO_ROUTE, arg); return -1; } dst = d->nwk; }
        dst_mode = 2;
    }
    if (dst_mode == 2 && dst == zb.nwk) {
        loopback(dst_ep, src_ep, profile, cluster, data, n);
        if (done) done(0, arg);
        return 0;
    }
    int group = dst_mode == 1, bc = !group && dst >= 0xFFF8;
    if (n > ASDU_MAX) {
        /* more than a frame holds: in blocks, unicast only (broadcasts and groups are never fragmented) */
        if (bc || group) { if (done) done(APS_ST_FRAME_TOO_LONG, arg); return -1; }
        return ftx_start(dst, dst_ieee, dst_ep, src_ep, profile, cluster, data, n, radius, relays, nrelays, done, arg);
    }
    if (bc || group) ack = 0;
    uint8_t counter = zb.aps_seq++;
    *p++ = (uint8_t)(AF_DATA | (group ? DM_GROUP : bc ? DM_BROADCAST : DM_UNICAST) << 2 | (ack ? 1 << 6 : 0));
    if (group) p = wr16(p, dst); else *p++ = dst_ep;
    p = wr16(p, cluster); p = wr16(p, profile); *p++ = src_ep; *p++ = counter;
    memcpy(p, data, n); p += n;
    size_t fl = (size_t)(p - f);
    if (bc && dst_ep == 0 && profile == 0) {
        /* a ZDO broadcast this node belongs to (the host's permit joining, a channel change): ours too */
        if (dst == NWK_BCAST_ALL || dst == NWK_BCAST_RXON || dst == NWK_BCAST_ROUTERS) {
            struct apsind in = { .src = zb.nwk, .src_ieee = zb.ieee, .dst = dst, .dst_mode = 2, .profile = 0, .cluster = cluster, .data = data, .n = n };
            uint8_t rsp[120]; size_t rn = 0; uint16_t rc;
            zdo_local(&in, rsp, &rn, &rc);
        }
    }
    if (!ack) return nwk_send(group ? NWK_BCAST_RXON : dst, dst_ieee, f, fl, 1, radius, relays, nrelays, done, arg);
    int i;
    for (i = 0; i < ZB_APS_WAIT && awaits[i].used; i++) ;
    if (i == ZB_APS_WAIT) { if (done) done(0xA6, arg); return -1; }   /* busy */
    struct await *w = &awaits[i];
    w->used = 1; w->counter = counter; w->tries = 1; memcpy(w->f, f, fl); w->n = (uint8_t)fl; w->dst = dst; w->dst_ieee = dst_ieee;
    w->done = done; w->arg = arg;
    aw_gen[i]++;
    nwk_send(dst, dst_ieee, f, fl, 1, radius, relays, nrelays, sent_nwk, aw_tag(w));
    struct node *d = node_by_nwk(dst);
    int sleepy = d && !(d->cap & 0x08) && d->cap;
    timer_at(now_ms() + (sleepy ? 8000 : 1600), await_resend, w);
    return 0;
}

/* ---------------------------------------------------------------- the trust center */

/* a command frame, secured with the link key (kid) we share with peer, to dst: straight, or as the payload of a
 * tunnel to the router via (0xFFFF: none) */
static void send_cmd(uint16_t dst, uint64_t peer, const uint8_t *c, size_t cn, int kid, const uint8_t *key, int nwk_secure, uint16_t via, int ext_dst)
{
    uint8_t f[110];
    f[0] = AF_CMD; f[1] = zb.aps_seq++;
    memcpy(f + 2, c, cn);
    size_t fl = kid >= 0 ? secure(f, 2, cn, key, kid) : 2 + cn;
    if (!fl) return;
    if (via != 0xFFFF) {
        uint8_t t[120];
        t[0] = AF_CMD; t[1] = zb.aps_seq++; t[2] = AC_TUNNEL;
        wr64(t + 3, peer);
        memcpy(t + 11, f, fl);
        nwk_send(via, 0, t, 11 + fl, 1, 0, NULL, 0, NULL, NULL);
        return;
    }
    if (!nwk_secure) {
        /* a key to a joiner: the NWK frame unsecured, its EUI-64 in the header, one hop (it is our child) */
        uint8_t g[ZB_MAX_FRAME]; uint8_t *p = g;
        p = wr16(p, (uint16_t)(0 | 2 << 2 | (ext_dst ? 1 << 11 : 0) | 1 << 12));
        p = wr16(p, dst); p = wr16(p, zb.nwk); *p++ = 1; *p++ = zb.nwk_seq++;
        if (ext_dst) p = wr64(p, peer);
        p = wr64(p, zb.ieee);
        memcpy(p, f, fl); p += fl;
        nwk_deliver_unsecured(peer, dst, g, (size_t)(p - g));
        return;
    }
    nwk_send(dst, 0, f, fl, 1, 0, NULL, 0, NULL, NULL);
}

void aps_tc_joined(struct node *n, uint16_t parent, int rejoin)
{
    uint8_t c[1 + 1 + 16 + 1 + 8 + 8], *p = c;
    *p++ = AC_TRANSPORT_KEY; *p++ = KT_NETWORK;
    memcpy(p, zb.nwk_key, 16); p += 16; *p++ = zb.key_seq;
    p = wr64(p, n->ieee); p = wr64(p, zb.ieee);
    /* sealed with the key-transport key of what the device has: its install code's (preconfigured), the key it got
     * from us before (a trust center rejoin), or the global one.  Its frame counter stays as heard on a rejoin (it
     * kept it; reset, anyone who can make it rejoin could replay its frames once); a new join's is new (mac.c) */
    const uint8_t *key = n->has_key ? n->key : zb.tclk;
    LOG("aps: the network key to %016llx (%04x)%s%s", (unsigned long long)n->ieee, n->nwk,
        parent && parent != zb.nwk ? ", tunnelled through its parent" : "", rejoin ? ", rejoining" : "");
    if (parent && parent != zb.nwk && parent != 0xFFFF) send_cmd(n->nwk, n->ieee, c, (size_t)(p - c), KID_TRANSPORT, key, 1, parent, 0);
    else send_cmd(n->nwk, n->ieee, c, (size_t)(p - c), KID_TRANSPORT, key, 0, 0xFFFF, 1);
    if (!rejoin) n->fc_known = 0;
    dz_state_changed();
}

static int joining_open(void) { int64_t now = now_ms(); return zb.permit_until > now || zb.net_permit_until > now; }

/* joining open, or (a trust center rejoin) a device that holds a proven link key of its own: never the network key
 * sealed under the global key, or a key not verified, with joining closed (nwk.c rejoin_request says why) */
static int may_join(struct node *n, int rejoin)
{
    if (joining_open()) return 1;
    return rejoin && n && n->ieee && n->has_key && (n->key_verified || n->key_preconf);
}

/* a router's update device: Zigbee 3.0 has it sealed with that router's link key (APS security), from the router
 * that is the frame's NWK source.  One without APS security (older routers') only while joining is open: else any
 * member of the network could move a device's address and parent, or start a rejoin's key transport */
static void update_device(const uint8_t *c, size_t n, uint16_t router, int sec, uint64_t sender)
{
    if (n < 12 || zb.role != R_COORD) return;
    if (sec) { struct node *r = node_by_ieee(sender); if (!r || r->nwk != router) return; }
    else if (!joining_open()) { LOG("aps: an update device from %04x without APS security: refused (joining is closed)", router); return; }
    uint64_t ieee = rd64(c + 1);
    uint16_t nwk = rd16(c + 9);
    uint8_t st = c[11];
    struct node *d = node_by_ieee(ieee);
    switch (st) {
    case 0x00:                          /* standard secured rejoin: it has the key; where it is now */
        if ((d = node_add(ieee, nwk))) { d->parent = router; d->child = 0; }
        return;
    case 0x01: case 0x03:               /* unsecured join, trust center rejoin: the key, through that router */
        if (!may_join(d, st == 0x03)) { LOG("aps: %016llx at %04x: joining is closed", (unsigned long long)ieee, router); return; }
        if (!(d = node_add(ieee, nwk))) return;
        d->parent = router; d->child = 0;
        if (st == 0x01) {
            d->fc_known = 0; d->prev_fc_known = 0; d->aps_fc = 0;
            if (!d->key_preconf) { d->has_key = 0; d->key_verified = 0; }
            d->authed = 0; d->join_at = now_ms();
        }
        aps_tc_joined(d, router, st == 0x03);
        zb_save_soon();
        return;
    case 0x02:
        LOG("aps: %016llx left (through %04x)", (unsigned long long)ieee, router);
        return;
    }
}

static void request_key(uint64_t from, uint16_t src, const uint8_t *c, size_t n)
{
    if (zb.role != R_COORD || n < 2 || c[1] != KT_TCLK) return;
    struct node *d = node_by_ieee(from);
    /* the device asks for itself: the frame's NWK source is the EUI-64 it was sealed as (a member asking in another's
     * name would get that one's new key, and break its own) */
    struct node *s = node_by_nwk(src);
    if (!(s && s->ieee == from) && !(d && d->nwk == src)) {
        LOG("aps: a key request from %04x in the name of %016llx: refused", src, (unsigned long long)from); return;
    }
    if (d && d->has_key && (d->key_verified || d->key_preconf) && !memcmp(used_key, zb.tclk, 16)) return;   /* unsecure() never lets this by */
    if (!d) d = node_add(from, src);
    if (!d) return;
    /* the transport is sealed with what the device has now: the key its request was sealed with (the global one,
     * its install code's, or one we gave it before); a fresh key every time it asks: the one it verifies counts */
    uint8_t old[16], k[16];
    memcpy(old, used_key, 16);
    for (int i = 0; i < 16; i += 4) { uint32_t r = zb_random(); memcpy(k + i, &r, 4); }
    memcpy(d->key, k, 16);
    uint8_t r[1 + 1 + 16 + 8 + 8], *p = r;
    *p++ = AC_TRANSPORT_KEY; *p++ = KT_TCLK;
    memcpy(p, k, 16); p += 16; p = wr64(p, from); p = wr64(p, zb.ieee);
    send_cmd(src, from, r, (size_t)(p - r), KID_TRANSPORT, old, 1, 0xFFFF, 0);
    d->has_key = 1; d->key_preconf = 0; d->key_verified = 0;
    LOG("aps: a link key of its own for %016llx", (unsigned long long)from);
    zb_save_soon();
}

static void verify_key(uint64_t sender, uint16_t src, const uint8_t *c, size_t n)
{
    if (zb.role != R_COORD || n < 26 || c[1] != KT_TCLK) return;
    uint64_t ieee = rd64(c + 2);
    if (ieee != sender) return;         /* a device verifies its own key, sealed as itself */
    struct node *d = node_by_ieee(ieee);
    uint8_t h[16], st = 0xAD;           /* security failure */
    if (d && d->has_key) { keyed_hash(d->key, 0x03, h); if (!memcmp(h, c + 10, 16)) st = 0; }
    if (!st) d->key_verified = 1;
    uint8_t r[11] = { AC_CONFIRM_KEY, st, KT_TCLK };
    wr64(r + 3, ieee);
    LOG("aps: %016llx verified its link key: %s", (unsigned long long)ieee, st ? "it does not hold" : "confirmed");
    send_cmd(src, ieee, r, 11, KID_DATA, d && d->has_key ? d->key : zb.tclk, 1, 0xFFFF, 0);
    zb_save_soon();
}

/* The network key rotated (the coordinator's page button, the ctl's "rotate"): a new key, its sequence number one
 * up, to every device: unicast sealed with each one's own link key where it has one, and broadcast to all
 * (NWK-secured under the current key, no APS security: Zigbee 3.0's way for devices without a key of their own, as
 * Ember's trust centers do it); after nwkBroadcastDeliveryTime twice the switch key, broadcast; the trust center
 * switches with it.  A sleepy device that missed both rejoins (trust center rejoin: the current key) */
static struct { int on, next, sent; uint8_t key[16], seq; int64_t begun; } rot;
#define ROTATE_SWITCH_MS 20000         /* ZIGBEED_ROTATE_MS for the tests */
static int rotate_ms(void) { const char *e = getenv("ZIGBEED_ROTATE_MS"); int v = e ? atoi(e) : 0; return v > 0 ? v : ROTATE_SWITCH_MS; }

int aps_rotating(void) { return rot.on; }

/* the key a joiner holds for the coming switch: 1 with it where seq is its sequence number */
int aps_pending_key(uint8_t seq, uint8_t key[16])
{
    if (!have_pending || seq != pending_seq) return 0;
    memcpy(key, pending_key, 16);
    return 1;
}
void aps_pending_taken(void) { have_pending = 0; }

static void rotate_switch(void *unused)
{
    (void)unused;
    if (!rot.on) return;
    uint8_t c[2] = { AC_SWITCH_KEY, rot.seq };
    send_cmd(NWK_BCAST_ALL, 0, c, 2, -1, NULL, 1, 0xFFFF, 0);
    rot.on = 0;
    nwk_switch_key(rot.key, rot.seq);
    dz_state_changed();
}

/* the unicasts one every 100 ms after the broadcast (the radio's queue takes 24; each to a sleepy device holds an
 * indirect place): node by node from rot.next; the switch once the last went and the time is up, whichever later */
static void rotate_build(uint8_t *c, size_t *cn, uint64_t to)
{
    uint8_t *p = c;
    *p++ = AC_TRANSPORT_KEY; *p++ = KT_NETWORK;
    memcpy(p, rot.key, 16); p += 16; *p++ = rot.seq;
    p = wr64(p, to); p = wr64(p, zb.ieee);
    *cn = (size_t)(p - c);
}

static void rotate_unicast(void *unused)
{
    (void)unused;
    if (!rot.on) return;
    uint8_t c[40]; size_t cn;
    for (; rot.next < zb.nnodes; rot.next++) {
        struct node *n = &zb.nodes[rot.next];
        if (!n->used || !n->has_key || !n->authed || n->nwk >= 0xFFF8 || n->ieee == zb.ieee) continue;
        rotate_build(c, &cn, n->ieee);
        send_cmd(n->nwk, n->ieee, c, cn, KID_TRANSPORT, n->key, 1, 0xFFFF, 0);
        rot.sent++; rot.next++;
        timer_at(now_ms() + 100, rotate_unicast, NULL);
        return;
    }
    int64_t at = rot.begun + rotate_ms(), soon = now_ms() + 5000;
    LOG("aps: the new key went to %d devices by their own keys too", rot.sent);
    timer_at(at > soon ? at : soon, rotate_switch, NULL);
}

int aps_rotate_key(void)
{
    if (zb.role != R_COORD || zb.state != NS_CONNECTED || rot.on) return -1;
    for (int i = 0; i < 16; i += 4) { uint32_t r = zb_random(); memcpy(rot.key + i, &r, 4); }
    rot.seq = (uint8_t)(zb.key_seq + 1);
    rot.on = 1; rot.next = 0; rot.sent = 0; rot.begun = now_ms();
    uint8_t c[40]; size_t cn;
    rotate_build(c, &cn, 0);            /* to all, first */
    send_cmd(NWK_BCAST_ALL, 0, c, cn, -1, NULL, 1, 0xFFFF, 0);
    LOG("aps: a new network key (sequence %u) to all; to each device by its own key next; the switch in %d s at the earliest", rot.seq, rotate_ms() / 1000);
    timer_at(now_ms() + 200, rotate_unicast, NULL);
    return 0;
}

/* what a network left behind (zb_reset_network): nothing of it goes on being sent or taken */
void aps_reset(void)
{
    for (int i = 0; i < ZB_APS_WAIT; i++) { awaits[i].used = 0; timer_cancel(await_resend, &awaits[i]); aw_gen[i]++; }
    for (int i = 0; i < 2; i++) { ftxs[i].used = 0; timer_cancel(ftx_resend, &ftxs[i]); fx_gen[i]++; }
    memset(frxs, 0, sizeof frxs);
    have_pending = 0; tclk_asked = 0; rot.on = 0;
    timer_cancel(rotate_unicast, NULL); timer_cancel(rotate_switch, NULL); timer_cancel(aps_request_tclk, NULL);
}

/* ---------------------------------------------------------------- the joiner's side */

extern void nwk_joined(void);

void aps_request_tclk(void *unused)
{
    (void)unused;
    if (zb.role == R_COORD || !zb.joined) return;
    struct node *tc = node_by_ieee(zb.tc_ieee);
    if (tc && tc->has_key && tc->key_verified) return;
    uint8_t c[2] = { AC_REQUEST_KEY, KT_TCLK };
    tclk_asked = 1;
    send_cmd(0x0000, zb.tc_ieee, c, 2, KID_DATA, zb.tclk, 1, 0xFFFF, 0);
    timer_at(now_ms() + 8000, aps_request_tclk, NULL);           /* again, until a key holds (or 8 s after each) */
}

void aps_update_device(uint64_t ieee, uint16_t nwk, uint8_t status)
{
    uint8_t c[12] = { AC_UPDATE_DEVICE };
    wr64(c + 1, ieee); wr16(c + 9, nwk); c[11] = status;
    send_cmd(0x0000, zb.tc_ieee, c, 12, KID_DATA, link_key(zb.tc_ieee), 1, 0xFFFF, 0);
}

static void transport_key(const uint8_t *c, size_t n, uint64_t sender)
{
    if (zb.role == R_COORD || n < 2) return;
    /* once joined, keys come from our trust center only, sealed as it (with our proven key once we have one:
     * unsecure() then takes no other) */
    if (zb.state != NS_JOINING && sender != zb.tc_ieee) { LOG("aps: a key from %016llx, not our trust center: ignored", (unsigned long long)sender); return; }
    if (c[1] == KT_NETWORK && n >= 35) {
        uint64_t dst = rd64(c + 19), src = rd64(c + 27);
        if (dst && dst != zb.ieee) return;
        if (zb.state == NS_JOINING) {
            memcpy(zb.nwk_key, c + 2, 16); zb.key_seq = c[18];
            zb.tc_ieee = src ? src : sender;
            struct node *tc = node_add(zb.tc_ieee, 0x0000);
            if (tc) { tc->cap = 0x8F; tc->neighbor = zb.parent == 0x0000; }
            if (zb.parent != 0x0000) { struct node *pn = node_by_nwk(zb.parent); if (!pn) { (void)0; } }
            LOG("aps: the network key from %016llx (sequence %u)", (unsigned long long)zb.tc_ieee, zb.key_seq);
            nwk_joined();
        } else if (c[18] != zb.key_seq && (!have_pending || c[18] != pending_seq || memcmp(pending_key, c + 2, 16))) {
            /* a new network key, active at the switch key that follows */
            memcpy(pending_key, c + 2, 16); pending_seq = c[18]; have_pending = 1;
            LOG("aps: a new network key (sequence %u), waiting for the switch", pending_seq);
        }
        return;
    }
    if (c[1] == KT_TCLK && n >= 34) {
        uint64_t dst = rd64(c + 18);
        if (dst != zb.ieee || !tclk_asked) return;      /* only the key we asked for */
        tclk_asked = 0;
        struct node *tc = node_add(zb.tc_ieee, 0x0000);
        if (!tc) return;
        memcpy(tc->key, c + 2, 16); tc->has_key = 1; tc->key_verified = 0;
        uint8_t v[26] = { AC_VERIFY_KEY, KT_TCLK };
        wr64(v + 2, zb.ieee);
        keyed_hash(tc->key, 0x03, v + 10);
        LOG("aps: a link key of our own from the trust center: verifying it");
        send_cmd(0x0000, zb.tc_ieee, v, 26, -1, NULL, 1, 0xFFFF, 0);
        zb_save_soon();
    }
}

/* ---------------------------------------------------------------- receiving */

void aps_rx(const uint8_t *in, size_t n, uint16_t src, uint64_t src_ieee, uint16_t dst, int nwk_secured, const struct rxinfo *ri)
{
    uint8_t f[ZB_MAX_FRAME];
    if (n < 2 || n > sizeof f) return;
    memcpy(f, in, n);
    int type = f[0] & 3, dm = f[0] >> 2 & 3, sec = f[0] >> 5 & 1, ackreq = f[0] >> 6 & 1, ext = f[0] >> 7 & 1;
    if (ext && type != AF_DATA && type != AF_ACK) return;   /* an extended header is fragmentation's: data and its acks */
    if (type == AF_ACK) {
        uint8_t counter;
        if (f[0] & 1 << 4) counter = f[1];
        else { if (n < 8) return; counter = f[7]; }
        if (ext) {
            /* a block's ack: extended frame control, the window's first block, which of them came */
            if (f[0] & 1 << 4 || n < 11 || !(f[8] & 3)) return;
            ftx_acked(src, counter, f[9], f[10]);
            return;
        }
        for (int i = 0; i < ZB_APS_WAIT; i++)
            if (awaits[i].used && awaits[i].counter == counter && awaits[i].dst == src) { await_done(&awaits[i], 0); break; }
        return;
    }
    if (type == AF_CMD) {
        size_t po = 2, pn = n - 2;
        uint64_t sender = src_ieee;
        if (sec) {
            if (unsecure(f, n, 2, src_ieee, &po, &pn, &sender)) { LOG("aps: a command from %04x did not unseal", src); return; }
        } else if (!nwk_secured && !(zb.state == NS_JOINING)) return;
        if (!pn) return;
        if (dup_seen(src, f[1])) { if (ackreq) send_ack(src, f, 1); return; }   /* its ack was lost: again */
        if (ackreq) send_ack(src, f, 1);
        const uint8_t *c = f + po;
        switch (c[0]) {
        case AC_TRANSPORT_KEY:
            /* sealed with a link key; or, a new network key to all from the trust center (NWK-secured, no APS
             * security: a rotation's broadcast) once we are in the network */
            if (sec) transport_key(c, pn, sender);
            else if (nwk_secured && zb.state == NS_CONNECTED && src == 0x0000 && pn >= 35 && c[1] == KT_NETWORK) transport_key(c, pn, sender);
            return;
        case AC_UPDATE_DEVICE: if (nwk_secured) update_device(c, pn, src, sec, sender); return;
        case AC_REQUEST_KEY: if (sec && nwk_secured) request_key(sender, src, c, pn); return;
        case AC_VERIFY_KEY: if (nwk_secured) verify_key(sender, src, c, pn); return;
        case AC_CONFIRM_KEY:
            if (sec && pn >= 11 && rd64(c + 3) == zb.ieee && sender == zb.tc_ieee) {
                struct node *tc = node_by_ieee(zb.tc_ieee);
                if (tc && !c[1] && tc->has_key && !memcmp(used_key, tc->key, 16)) {   /* sealed with the key it confirms */ tc->key_verified = 1; timer_cancel(aps_request_tclk, NULL); zb_save_soon(); }
                LOG("aps: the trust center %s our link key", c[1] ? "refused" : "confirmed");
            }
            return;
        case AC_SWITCH_KEY:
            /* the trust center's (its key, pending, came from it alone: transport_key); sent NWK-secured, often
             * broadcast, without APS security (as Ember's and deCONZ's trust centers do) */
            if (zb.role != R_COORD && src == 0x0000 && pn >= 2 && have_pending && c[1] == pending_seq) {
                have_pending = 0;
                nwk_switch_key(pending_key, pending_seq);
            }
            return;
        case AC_TUNNEL:
            /* a key for our child from the trust center: passed on as it is, unsecured at the NWK layer */
            if (zb.role == R_ROUTER && nwk_secured && pn > 9) {
                uint64_t to = rd64(c + 1);
                struct node *ch = node_by_ieee(to);
                if (!ch || sender != zb.tc_ieee) return;        /* our trust center's, for a device it knows of here */
                uint8_t g[ZB_MAX_FRAME]; uint8_t *p = g;
                p = wr16(p, (uint16_t)(0 | 2 << 2 | 1 << 11 | 1 << 12));
                p = wr16(p, ch->nwk); p = wr16(p, zb.nwk); *p++ = 1; *p++ = zb.nwk_seq++;
                p = wr64(p, to); p = wr64(p, zb.ieee);
                if ((size_t)(p - g) + pn - 9 > ZB_MAX_FRAME - 11) return;
                memcpy(p, c + 9, pn - 9); p += pn - 9;
                nwk_deliver_unsecured(to, ch->nwk, g, (size_t)(p - g));
            }
            return;
        case AC_REMOVE_DEVICE:
            if (zb.role == R_ROUTER && pn >= 9 && sec && sender == zb.tc_ieee) {
                struct node *ch = node_by_ieee(rd64(c + 1));
                if (ch) { nwk_leave(ch->nwk, ch->ieee, 0, 0); ch->child = 0; }
            }
            return;
        }
        return;
    }
    /* data */
    if (!nwk_secured || sec) return;    /* data goes secured by the network key, never by a link key here */
    size_t k = 1;
    struct apsind ind = { .src = src, .src_ieee = src_ieee, .dst = dst, .lqi = ri->lqi, .rssi = ri->rssi };
    if (dm == DM_GROUP) { if (n < 3) return; ind.dst = rd16(f + 1); ind.dst_mode = 1; ind.dst_ep = 0xFF; k = 3; }
    else { ind.dst_mode = 2; ind.dst_ep = f[1]; k = 2; }
    if (k + 6 > n) return;
    ind.cluster = rd16(f + k); ind.profile = rd16(f + k + 2); ind.src_ep = f[k + 4];
    uint8_t counter = f[k + 5];
    k += 6;
    if (ext) {
        if (k + 1 > n) return;
        uint8_t xfc = f[k++];
        if (xfc & 3) {
            /* a block: unicast with an ack asked, never a duplicate check (every block has the same counter) */
            if (k + 1 > n || dm != DM_UNICAST || !ackreq || (xfc & 3) == 3) return;
            uint8_t blk = f[k++];
            const uint8_t *whole; size_t wn;
            if (!frag_rx(src, f, xfc, blk, f + k, n - k, &whole, &wn)) return;
            ind.data = whole; ind.n = wn;
            deliver(&ind);
            return;
        }
    }
    ind.data = f + k; ind.n = n - k;
    if (dup_seen(src, counter)) { if (ackreq && dm == DM_UNICAST) send_ack(src, f, 0); return; }
    if (ackreq && dm == DM_UNICAST) send_ack(src, f, 0);
    deliver(&ind);
}
