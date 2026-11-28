/* The 802.15.4 MAC as Zigbee uses it, above an RCP that does the PHY, CSMA, retries and acks: data frames, beacons and
 * beacon requests, association (both sides), data requests, and indirect frames for children that sleep (kept here
 * up to macTransactionPersistenceTime, 7.68 s; the RCP's source match tables set the frame pending bit in its ack to
 * a child's data request while one waits).  Frames are 2003-version (Zigbee's), the PAN ID compressed where both
 * addresses are in the network. */
#include "zb.h"
#include <string.h>

#define FT_BEACON 0
#define FT_DATA 1
#define FT_ACK 2
#define FT_CMD 3
#define AM_NONE 0
#define AM_SHORT 2
#define AM_EXT 3
#define C_ASSOC_REQ 0x01
#define C_ASSOC_RSP 0x02
#define C_DATA_REQ 0x04
#define C_BEACON_REQ 0x07
#define PERSIST_MS 7680

/* a frame for a child that sleeps */
static struct ind { uint8_t used, assoc; uint64_t ieee; uint16_t nwk; int64_t until; struct txf f; } ind[ZB_IND_MAX];
#define ASSOC_SLOTS 4                   /* of ind[]: association responses (anyone in range can ask for one) */
static int64_t expire_at;               /* when mac_indirect_expire runs next: 0 none set */

/* the expiry's timer, at the earliest it is wanted: never moved later (a steady stream of new frames would keep it
 * from ever running, and frames for sleepy children from ever expiring) */
static void expire_soon(int64_t when)
{
    if (expire_at && expire_at <= when) return;
    if (!timer_at(when, mac_indirect_expire, NULL)) expire_at = when;   /* only once it is set: else the next asks again */
}
static struct beacon scan[32];
static int nscan, scanning;

static void poll_again(void *unused);

static uint8_t *hdr(uint8_t *p, int type, int ack, int pending, int dmode, uint16_t dpan, uint64_t daddr, int smode, uint16_t span, uint64_t saddr)
{
    int comp = dmode && smode && dpan == span;
    uint16_t fcf = (uint16_t)(type | (pending ? 1 << 4 : 0) | (ack ? 1 << 5 : 0) | (comp ? 1 << 6 : 0) | dmode << 10 | smode << 14);
    p = wr16(p, fcf);
    *p++ = zb.mac_seq++;
    if (dmode) { p = wr16(p, dpan); p = dmode == AM_SHORT ? wr16(p, (uint16_t)daddr) : wr64(p, daddr); }
    if (smode) { if (!comp) p = wr16(p, span); p = smode == AM_SHORT ? wr16(p, (uint16_t)saddr) : wr64(p, saddr); }
    return p;
}

static int send(uint8_t *start, uint8_t *end, int unicast, void (*done)(int, void *), void *arg)
{
    struct txf t;
    size_t n = (size_t)(end - start);
    if (n + 2 > ZB_MAX_FRAME) { if (done) done(-3, arg); return -1; }
    memcpy(t.psdu, start, n); t.psdu[n] = t.psdu[n + 1] = 0;
    t.len = (uint8_t)(n + 2); t.retries = unicast ? 3 : 0; t.done = done; t.arg = arg;
    return rcp_tx(&t);
}

/* ---------------------------------------------------------------- indirect frames */

/* The radio acks a data request with frame pending for the addresses in its source match table: each entry's
 * child by its EUI-64 and by the short address the entry was queued under (a child told a new one polls by the old
 * one until it took it), each address in the table while one entry needs it */
static int ind_with(uint64_t ieee, uint16_t nwk)
{
    int n = 0;
    for (int i = 0; i < ZB_IND_MAX; i++) n += ind[i].used && (ieee ? ind[i].ieee == ieee : ind[i].nwk == nwk);
    return n;
}

static void sm_add(const struct ind *q)            /* q already in */
{
    if (ind_with(q->ieee, 0) == 1) rcp_src_match(1, 1, q->ieee);
    if (q->nwk < 0xFFF8 && ind_with(0, q->nwk) == 1) rcp_src_match(1, 0, q->nwk);
}

static void sm_del(const struct ind *q)            /* q already out */
{
    if (!ind_with(q->ieee, 0)) rcp_src_match(0, 1, q->ieee);
    if (q->nwk < 0xFFF8 && !ind_with(0, q->nwk)) rcp_src_match(0, 0, q->nwk);
}

static int queue_indirect(struct node *c, uint8_t *start, uint8_t *end, void (*done)(int, void *), void *arg, int assoc)
{
    int i;
    if (assoc) {
        /* one association response per joiner (a new one replaces its last), and only so many in all: the rest of
         * the queue stays the children's */
        int na = 0;
        for (i = 0; i < ZB_IND_MAX; i++) if (ind[i].used && ind[i].assoc) {
            if (ind[i].ieee == c->ieee) { ind[i].used = 0; sm_del(&ind[i]); if (ind[i].f.done) ind[i].f.done(-5, ind[i].f.arg); }
            else na++;
        }
        if (na >= ASSOC_SLOTS) { if (done) done(-4, arg); return -1; }
    }
    for (i = 0; i < ZB_IND_MAX && ind[i].used; i++) ;
    if (i == ZB_IND_MAX) { if (done) done(-4, arg); return -1; }
    size_t n = (size_t)(end - start);
    if (n + 2 > ZB_MAX_FRAME) { if (done) done(-3, arg); return -1; }
    struct ind *q = &ind[i];
    q->used = 1; q->assoc = (uint8_t)assoc; q->ieee = c->ieee; q->nwk = c->nwk; q->until = now_ms() + PERSIST_MS;
    memcpy(q->f.psdu, start, n); q->f.psdu[n] = q->f.psdu[n + 1] = 0;
    q->f.len = (uint8_t)(n + 2); q->f.retries = 3; q->f.done = done; q->f.arg = arg;
    sm_add(q);
    expire_soon(q->until);
    return 0;
}

void mac_indirect_expire(void *unused)
{
    (void)unused;
    int64_t now = now_ms(), next = 0;
    expire_at = 0;
    for (int i = 0; i < ZB_IND_MAX; i++) {
        if (!ind[i].used) continue;
        if (ind[i].until > now) { if (!next || ind[i].until < next) next = ind[i].until; continue; }
        struct ind q = ind[i];
        ind[i].used = 0;
        sm_del(&q);
        if (q.f.done) q.f.done(-5, q.f.arg);      /* the child did not poll in time: transaction expired */
    }
    if (next) expire_soon(next);
}

void mac_drop_indirect(uint64_t ieee)
{
    for (int i = 0; i < ZB_IND_MAX; i++) if (ind[i].used && ind[i].ieee == ieee) {
        struct ind q = ind[i];
        ind[i].used = 0;
        sm_del(&q);                     /* out of the radio's table, or it keeps the child polling */
        if (q.f.done) q.f.done(-5, q.f.arg);
    }
}

int mac_indirect_pending(uint64_t ieee) { return ind_with(ieee, 0); }
/* a frame for a child held under the short address it polls by now (not yet the one in the node: a new one it is
 * being told), by its EUI-64 at the MAC */
int mac_indirect_as(uint64_t ieee, uint16_t poll_nwk, const uint8_t *f, size_t n, int unproven)
{
    uint8_t b[ZB_MAX_FRAME + 16], *p = b;
    p = hdr(p, FT_DATA, 1, 0, AM_EXT, zb.pan, ieee, AM_SHORT, zb.pan, zb.nwk);
    if ((size_t)(p - b) + n + 2 > ZB_MAX_FRAME) return -1;
    memcpy(p, f, n); p += n;
    struct node tmp = { .ieee = ieee, .nwk = poll_nwk };
    return queue_indirect(&tmp, b, p, NULL, NULL, unproven);   /* one not yet in the network: a joiner's places */
}

int mac_indirect_free(void) { int n = 0; for (int i = 0; i < ZB_IND_MAX; i++) n += !ind[i].used; return n; }

/* a child's data request: its oldest frame, with frame pending set where more wait */
static void data_request(struct node *c)
{
    int first = -1;
    int64_t best = 0;
    for (int i = 0; i < ZB_IND_MAX; i++)
        if (ind[i].used && ind[i].ieee == c->ieee && (first < 0 || ind[i].until < best)) { first = i; best = ind[i].until; }
    if (first < 0) return;
    struct ind q = ind[first];
    struct txf t = q.f;
    ind[first].used = 0;
    int more = ind_with(c->ieee, 0);
    if (more) t.psdu[0] |= 1 << 4; else t.psdu[0] &= (uint8_t)~(1 << 4);
    sm_del(&q);
    rcp_tx(&t);
}

/* ---------------------------------------------------------------- sending */

int mac_data_direct(uint16_t dst, uint64_t ext, int use_ext, const uint8_t *f, size_t n, void (*done)(int, void *), void *arg)
{
    uint8_t b[ZB_MAX_FRAME + 16], *p = b;
    int bc = !use_ext && dst == 0xFFFF;
    if (zb.nwk >= 0xFFF8) p = hdr(p, FT_DATA, !bc, 0, use_ext ? AM_EXT : AM_SHORT, zb.pan, use_ext ? ext : dst, AM_EXT, zb.pan, zb.ieee);
    else p = hdr(p, FT_DATA, !bc, 0, use_ext ? AM_EXT : AM_SHORT, zb.pan, use_ext ? ext : dst, AM_SHORT, zb.pan, zb.nwk);
    if ((size_t)(p - b) + n + 2 > ZB_MAX_FRAME) { if (done) done(-3, arg); return -1; }
    memcpy(p, f, n); p += n;
    return send(b, p, !bc, done, arg);
}

int mac_data(uint16_t dst, uint64_t ext, int use_ext, const uint8_t *f, size_t n, void (*done)(int, void *), void *arg)
{
    struct node *c = use_ext ? node_by_ieee(ext) : (dst < 0xFFF8 ? node_by_nwk(dst) : NULL);
    if (c && c->child && !(c->cap & 0x08)) {          /* a child that sleeps: it gets it when it asks */
        uint8_t b[ZB_MAX_FRAME + 16], *p = b;
        p = hdr(p, FT_DATA, 1, 0, use_ext ? AM_EXT : AM_SHORT, zb.pan, use_ext ? ext : dst, AM_SHORT, zb.pan, zb.nwk);
        if ((size_t)(p - b) + n + 2 > ZB_MAX_FRAME) { if (done) done(-3, arg); return -1; }
        memcpy(p, f, n); p += n;
        return queue_indirect(c, b, p, done, arg, 0);
    }
    return mac_data_direct(dst, ext, use_ext, f, n, done, arg);
}

void mac_beacon_request(void)
{
    uint8_t b[16], *p = hdr(b, FT_CMD, 0, 0, AM_SHORT, 0xFFFF, 0xFFFF, AM_NONE, 0, 0);
    *p++ = C_BEACON_REQ;
    send(b, p, 0, NULL, NULL);
}

static void beacon(void)
{
    uint8_t b[40], *p = hdr(b, FT_BEACON, 0, 0, AM_NONE, 0, 0, AM_SHORT, zb.pan, zb.nwk);
    int permit = zb.permit_until > now_ms(), coord = zb.role == R_COORD;
    int children = 0;
    for (int i = 0; i < zb.nnodes; i++) children += zb.nodes[i].used && zb.nodes[i].child;
    p = wr16(p, (uint16_t)(0x0FFF | (coord ? 1 << 14 : 0) | (permit ? 1 << 15 : 0)));   /* BO 15, SO 15, final CAP 15 */
    *p++ = 0; *p++ = 0;                 /* GTS, pending addresses */
    *p++ = 0;                           /* Zigbee: protocol 0 */
    *p++ = 2 | 2 << 4;                  /* stack profile 2 (PRO), protocol version 2 */
    *p++ = (uint8_t)(1 << 2 | (zb.depth & 0x0F) << 3 | (children < 32 ? 1 << 7 : 0));   /* router capacity, depth, end device capacity */
    p = wr64(p, zb.epan);
    *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF;    /* tx offset */
    *p++ = zb.update_id;
    send(b, p, 0, NULL, NULL);
}

static void assoc_sent(int st, void *arg)
{
    struct node *n = arg;
    if (st) { LOG("mac: association response to %016llx not taken (%d)", (unsigned long long)n->ieee, st); return; }
    if (zb.role == R_COORD) aps_tc_joined(n, 0x0000, 0);
    else aps_update_device(n->ieee, n->nwk, 0x01);     /* standard unsecured join: the TC sends the key, through us */
}

static void association(uint64_t ieee, uint8_t cap, uint8_t lqi)
{
    uint8_t b[40], *p;
    struct node *n = node_by_ieee(ieee);
    int ok = zb.state == NS_CONNECTED && zb.permit_until > now_ms() && zb.role != R_ENDDEV;
    if (ok && !n && nwk_joiners() >= 16) ok = 0;    /* joiners not yet proven: so many at once at most */
    if (ok && !n) {
        n = node_add(ieee, nwk_new_address());
        if (!n) ok = 0;
    }
    if (ok && n->nwk >= 0xFFF8) n->nwk = nwk_new_address();
    LOG("mac: association request from %016llx (capability %02x, lqi %u): %s", (unsigned long long)ieee, cap, lqi, ok ? "taken" : "refused");
    if (ok) {
        n->cap = cap; n->seen = now_ms(); n->lqi = lqi; n->parent = zb.nwk;
        n->child = !(cap & 0x02);       /* an end device: our child; a router finds its own neighbors */
        /* a new join: its counters begin anew, a key of its own it had is gone (it was reset) unless the host gave it
         * (an install code's); it counts as a joiner until it proves the network key (nwk.c), else forgotten */
        n->fc_known = 0; n->prev_fc_known = 0; n->aps_fc = 0;
        if (!n->key_preconf) { n->has_key = 0; n->key_verified = 0; }
        n->authed = 0; n->join_at = now_ms();
        n->timeout_s = 256;
    }
    p = hdr(b, FT_CMD, 1, 0, AM_EXT, zb.pan, ieee, AM_EXT, zb.pan, zb.ieee);
    *p++ = C_ASSOC_RSP;
    p = wr16(p, ok ? n->nwk : 0xFFFF);
    *p++ = ok ? 0x00 : 0x02;            /* success, PAN access denied */
    /* the joiner polls for it (it may be an end device that sleeps): kept indirect, by its EUI-64 alone (it has no
     * short address yet) */
    struct node tmp = { .ieee = ieee, .nwk = 0xFFFF };
    if (ok) zb_save_soon();
    queue_indirect(&tmp, b, p, ok ? assoc_sent : NULL, ok ? n : NULL, 1);
}

/* ---------------------------------------------------------------- joining (router, end device) */

void mac_scan_begin(void) { nscan = 0; scanning = 1; }
int mac_scan_results(struct beacon *out, int max) { int n = nscan < max ? nscan : max; memcpy(out, scan, sizeof *out * (size_t)n); scanning = 0; return n; }

int mac_associate(uint16_t pan, uint16_t coord, uint8_t cap)
{
    uint8_t b[32], *p = hdr(b, FT_CMD, 1, 0, AM_SHORT, pan, coord, AM_EXT, 0xFFFF, zb.ieee);
    *p++ = C_ASSOC_REQ; *p++ = cap;
    return send(b, p, 1, NULL, NULL);
}

void mac_poll_then(void (*done)(int, void *))
{
    uint8_t b[32], *p;
    if (zb.nwk < 0xFFF8) p = hdr(b, FT_CMD, 1, 0, AM_SHORT, zb.pan, zb.parent, AM_SHORT, zb.pan, zb.nwk);
    else p = hdr(b, FT_CMD, 1, 0, AM_SHORT, zb.pan, zb.parent, AM_EXT, zb.pan, zb.ieee);
    *p++ = C_DATA_REQ;
    send(b, p, 1, done, NULL);
}

void mac_poll(void) { mac_poll_then(NULL); }
static void poll_again(void *unused) { (void)unused; mac_poll(); }

void nwk_assoc_result(uint16_t addr, uint8_t status);

/* ---------------------------------------------------------------- receiving */

uint8_t mac_rx_seq;

void mac_rx(const uint8_t *f, size_t len, const struct rxinfo *ri)
{
    if (len < 5) return;
    mac_rx_seq = f[2];
    size_t n = len - 2, k = 3;          /* the FCS is not ours to read */
    uint16_t fcf = rd16(f);
    int type = fcf & 7, sec = fcf >> 3 & 1, comp = fcf >> 6 & 1, dm = fcf >> 10 & 3, sm = fcf >> 14 & 3;
    uint16_t dpan = 0xFFFF, span = 0xFFFF, dshort = 0xFFFF, sshort = 0xFFFF;
    uint64_t dext = 0, sext = 0;
    if (sec || type == FT_ACK) return;  /* Zigbee uses no MAC security; acks are the RCP's */
    if (dm) {
        if (k + 2 > n) return;
        dpan = rd16(f + k); k += 2;
        if (dm == AM_SHORT) { if (k + 2 > n) return; dshort = rd16(f + k); k += 2; }
        else if (dm == AM_EXT) { if (k + 8 > n) return; dext = rd64(f + k); k += 8; }
        else return;
    }
    if (sm) {
        if (!comp) { if (k + 2 > n) return; span = rd16(f + k); k += 2; } else span = dpan;
        if (sm == AM_SHORT) { if (k + 2 > n) return; sshort = rd16(f + k); k += 2; }
        else if (sm == AM_EXT) { if (k + 8 > n) return; sext = rd64(f + k); k += 8; }
        else return;
    }
    const uint8_t *pl = f + k; size_t pn = n - k;
    (void)dext; (void)span;
    if (type == FT_BEACON) {
        if (pn < 4 + 15 || sm != AM_SHORT) return;
        uint16_t sf = rd16(pl);
        const uint8_t *z = pl + 4;
        if (z[0] != 0 || (z[1] & 0x0F) != 2) return;      /* Zigbee PRO */
        if (!scanning) {
            /* a beacon heard while in the network (another one answering a beacon request): only one of our PAN
             * ID passes the radio's filter; of another network, a PAN ID conflict (nwk.c) */
            if (zb.state == NS_CONNECTED && zb.role != R_ENDDEV && span == zb.pan) nwk_beacon_heard(span, rd64(z + 3));
            return;
        }
        for (int i = 0; i < nscan; i++) if (scan[i].pan == span && scan[i].src == sshort && scan[i].channel == zb.channel) return;
        if (nscan == 32) return;
        struct beacon *b = &scan[nscan++];
        b->pan = span; b->src = sshort; b->channel = (uint8_t)zb.channel; b->lqi = ri->lqi;
        b->permit = sf >> 15 & 1; b->router_cap = z[2] >> 2 & 1; b->depth = z[2] >> 3 & 0x0F; b->ed_cap = z[2] >> 7 & 1;
        b->epan = rd64(z + 3); b->update_id = pn >= 4 + 15 ? z[14] : 0; b->stack = z[1] & 0x0F;
        return;
    }
    if (scanning) return;
    if (type == FT_CMD) {
        if (!pn) return;
        switch (pl[0]) {
        case C_BEACON_REQ:
            if (zb.state == NS_CONNECTED && zb.role != R_ENDDEV) beacon();
            return;
        case C_ASSOC_REQ:
            if (pn >= 2 && sm == AM_EXT && dpan == zb.pan) association(sext, pl[1], ri->lqi);
            return;
        case C_ASSOC_RSP:
            if (pn >= 4 && zb.state == NS_JOINING) nwk_assoc_result(rd16(pl + 1), pl[3]);
            return;
        case C_DATA_REQ: {
            struct node *c = sm == AM_EXT ? node_by_ieee(sext) : node_by_nwk(sshort);
            if (sm == AM_EXT && !c) {
                /* a joiner polling for its association response: its frames are kept by its EUI-64 */
                struct node tmp = { .ieee = sext, .nwk = 0xFFFF };
                data_request(&tmp);
                return;
            }
            if (!c && sm == AM_SHORT) {
                /* a child polling by the address it had before we gave it another (address conflict): the rejoin
                 * response that tells it waits under the old one */
                for (int i = 0; i < ZB_IND_MAX; i++) if (ind[i].used && ind[i].nwk == sshort) {
                    struct node tmp = { .ieee = ind[i].ieee, .nwk = sshort };
                    data_request(&tmp);
                    return;
                }
            }
            if (c) { c->seen = now_ms(); c->lqi = ri->lqi; data_request(c); if (c->child) dz_mac_poll(c, ri); }
            return;
        }
        default:
            return;
        }
    }
    if (type == FT_DATA && pn) {
        if (zb.state == NS_OFFLINE) return;
        /* an end device: our parent holds more for us (frame pending): asked for at once, not at the next poll */
        if (zb.role == R_ENDDEV && fcf & 1 << 4 && zb.state == NS_CONNECTED) timer_at(now_ms() + 20, poll_again, NULL);
        if (dm == AM_SHORT && dshort != 0xFFFF && dshort != zb.nwk) return;
        nwk_rx(pl, pn, sshort, sext, sm == AM_EXT, ri);
    }
}
