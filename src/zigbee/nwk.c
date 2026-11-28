/* The Zigbee PRO network layer (Zigbee spec 3.4-3.6, 4.3): frames and their security (the network key, CCM* level 5;
 * every hop secures anew with its own frame counter and EUI-64, as relays do), the address map, unicast routing (a
 * child or neighbor direct; else a route table entry, a source route, or a route discovery first: route request and
 * reply, AODV as Zigbee has it), many-to-one routes to the concentrator (the coordinator's every minute) and the route
 * records that give it source routes back, broadcasts (the broadcast transaction table, relayed once with jitter and
 * repeated twice), link status (every 15 s: which routers hear us at what cost), end device children (association
 * in mac.c, end device timeout, their frames held for their polls), rejoin, leave; and joining a network as a router
 * or an end device (scan, associate, the key from the trust center: aps.c). */
#include "zb.h"
#include <string.h>

#define NF_DATA 0
#define NF_CMD 1
#define PROTO 2
#define NC_RREQ 0x01
#define NC_RREP 0x02
#define NC_STATUS 0x03
#define NC_LEAVE 0x04
#define NC_RREC 0x05
#define NC_REJOIN_REQ 0x06
#define NC_REJOIN_RSP 0x07
#define NC_LINK_STATUS 0x08
#define NC_NWK_REPORT 0x09
#define NC_NWK_UPDATE 0x0A
#define NC_ED_TIMEOUT_REQ 0x0B
#define ST_ADDR_CONFLICT 0x0D
#define BCAST_DELIVERY_MS 9000          /* nwkNetworkBroadcastDeliveryTime */
#define NC_ED_TIMEOUT_RSP 0x0C
#define MAX_RADIUS 30
#define LINK_STATUS_MS 15000
#define MTO_MS 60000
#define JOINER_MS 60000                 /* a joiner that has not proven the network key by then is forgotten */
#define ROUTE_MS 300000                 /* a route not used for 5 min is gone */

static struct bt { uint16_t src; uint8_t seq; int64_t until; } btt[ZB_BCAST_SLOTS];
static struct disc { uint16_t orig, dst; uint8_t id; uint16_t back; uint8_t cost; int64_t until; int ours; } disc[ZB_DISC_SLOTS];
static struct pend {
    uint8_t used; uint16_t dst; uint64_t dst_ieee; uint8_t a[110]; uint8_t n; int secure; uint8_t radius;
    void (*done)(int, void *); void *arg; int64_t until;
} pend[ZB_PENDING_MAX];
static struct join_state { int phase, ch; int64_t at; struct beacon best; int tries; } js;

/* ---------------------------------------------------------------- the address map */

struct node *node_by_nwk(uint16_t nwk)
{
    for (int i = 0; i < zb.nnodes; i++) if (zb.nodes[i].used && zb.nodes[i].nwk == nwk) return &zb.nodes[i];
    return NULL;
}

struct node *node_by_ieee(uint64_t ieee)
{
    if (!ieee) return NULL;
    for (int i = 0; i < zb.nnodes; i++) if (zb.nodes[i].used && zb.nodes[i].ieee == ieee) return &zb.nodes[i];
    return NULL;
}

struct node *node_add(uint64_t ieee, uint16_t nwk)
{
    struct node *n = node_by_ieee(ieee);
    if (!n && nwk < 0xFFF8 && (n = node_by_nwk(nwk)) && n->ieee && n->ieee != ieee) {
        /* the address moved to another device (it rejoined elsewhere and got the old one's): the old entry loses it */
        n->nwk = 0xFFFE; n = NULL;
    }
    if (n && !n->ieee) n->ieee = ieee;  /* an entry by address alone (a route reply's) learns whose it is */
    if (!n) {
        int i;
        for (i = 0; i < zb.nnodes && zb.nodes[i].used; i++) ;
        if (i == zb.nnodes) { if (zb.nnodes == ZB_MAX_NODES) return NULL; zb.nnodes++; }
        n = &zb.nodes[i];
        memset(n, 0, sizeof *n);
        n->used = 1; n->ieee = ieee; n->nwk = 0xFFFE; n->next_hop = 0xFFFF; n->parent = 0xFFFF;
    }
    if (nwk < 0xFFF8 && n->nwk != nwk) {
        struct node *o = node_by_nwk(nwk);
        if (o && o != n) o->nwk = 0xFFFE;
        n->nwk = nwk; n->next_hop = 0xFFFF; n->nrelay = 0;
    }
    return n;
}

void node_forget(struct node *n)
{
    mac_drop_indirect(n->ieee);
    memset(n, 0, sizeof *n);
}

uint16_t nwk_new_address(void)
{
    for (;;) {
        uint16_t a = (uint16_t)(zb_random() % 0xFFF6 + 1);
        if (a != zb.nwk && !node_by_nwk(a)) return a;
    }
}

uint8_t lqi_cost(uint8_t lqi) { return lqi >= 200 ? 1 : lqi >= 150 ? 2 : lqi >= 110 ? 3 : lqi >= 80 ? 4 : lqi >= 50 ? 5 : 7; }

static int link_up(struct node *n) { return n && n->neighbor && now_ms() - n->seen < 3 * LINK_STATUS_MS + 2000; }

/* ---------------------------------------------------------------- frames */

struct nhdr {
    uint16_t fcf, dst, src; uint8_t radius, seq;
    uint64_t dst_ieee, src_ieee;
    uint8_t mcast;                      /* the multicast control byte, where the flag says */
    uint8_t nrelay, relay_idx; uint16_t relay[10];
    size_t len;                         /* the header's bytes */
};

static int parse_hdr(const uint8_t *f, size_t n, struct nhdr *h)
{
    size_t k = 8;
    if (n < 8) return -1;
    memset(h, 0, sizeof *h);
    h->fcf = rd16(f); h->dst = rd16(f + 2); h->src = rd16(f + 4); h->radius = f[6]; h->seq = f[7];
    if (h->fcf & 1 << 11) { if (k + 8 > n) return -1; h->dst_ieee = rd64(f + k); k += 8; }
    if (h->fcf & 1 << 12) { if (k + 8 > n) return -1; h->src_ieee = rd64(f + k); k += 8; }
    if (h->fcf & 1 << 8) { if (k + 1 > n) return -1; h->mcast = f[k++]; }    /* multicast control */
    if (h->fcf & 1 << 10) {
        if (k + 2 > n) return -1;
        h->nrelay = f[k]; h->relay_idx = f[k + 1]; k += 2;
        if (h->nrelay > 10 || k + 2u * h->nrelay > n) return -1;
        for (int i = 0; i < h->nrelay; i++) h->relay[i] = rd16(f + k + 2 * i);
        k += 2u * h->nrelay;
    }
    h->len = k;
    return 0;
}

static size_t put_hdr(uint8_t *f, const struct nhdr *h)
{
    uint8_t *p = wr16(f, h->fcf);
    p = wr16(p, h->dst); p = wr16(p, h->src); *p++ = h->radius; *p++ = h->seq;
    if (h->fcf & 1 << 11) p = wr64(p, h->dst_ieee);
    if (h->fcf & 1 << 12) p = wr64(p, h->src_ieee);
    if (h->fcf & 1 << 8) *p++ = h->mcast;
    if (h->fcf & 1 << 10) {
        *p++ = h->nrelay; *p++ = h->relay_idx;
        for (int i = 0; i < h->nrelay; i++) p = wr16(p, h->relay[i]);
    }
    return (size_t)(p - f);
}

/* the NWK header's bytes as put_hdr writes them */
static size_t hdr_len(const struct nhdr *h)
{
    return 8 + (h->fcf & 1 << 11 ? 8 : 0) + (h->fcf & 1 << 12 ? 8 : 0) + (h->fcf & 1 << 8 ? 1 : 0) + (h->fcf & 1 << 10 ? 2 + 2u * h->nrelay : 0);
}

/* what a NWK frame may carry: the PSDU less the MAC's header at its shortest (short addresses, the PAN ID once: 9
 * bytes) and FCS; mac.c refuses what a longer MAC header leaves no room for */
#define NWK_ROOM (ZB_MAX_FRAME - 2 - 9)

/* the NWK header h with payload pl: secured with the network key (aux header: level 5 sent as 0, key id 1, extended
 * nonce; our counter and EUI-64; the key's sequence number) into out (ZB_MAX_FRAME bytes).  Its length; 0 when it
 * does not fit (a source route of many relays and a long payload): nothing written past out */
static size_t frame(uint8_t *out, struct nhdr *h, const uint8_t *pl, size_t pn, int secure)
{
    if (secure) h->fcf |= 1 << 9; else h->fcf &= (uint16_t)~(1 << 9);
    if (hdr_len(h) + (secure ? 14 + 4 : 0) + pn > NWK_ROOM) return 0;
    size_t k = put_hdr(out, h);
    if (!secure) { memcpy(out + k, pl, pn); return k + pn; }
    uint8_t *aux = out + k, nonce[13];
    uint32_t fc = zb.nwk_fc++;
    if (zb.nwk_fc >= zb.nwk_fc_saved) zb_save();
    aux[0] = 0x28;                      /* key id 1 (network), extended nonce */
    wr32(aux + 1, fc); wr64(aux + 5, zb.ieee); aux[13] = zb.key_seq;
    k += 14;
    memcpy(out + k, pl, pn);
    wr64(nonce, zb.ieee); wr32(nonce + 8, fc); nonce[12] = 0x2D;
    aux[0] = 0x2D;                      /* the level counts in the MIC, not on the air */
    ccm_encrypt(zb.nwk_key, nonce, out, k, out + k, pn);
    aux[0] = 0x28;
    return k + pn + 4;
}

/* the frame's security undone in place: the payload's offset and length, -1 when it does not hold */
static int unsecure(uint8_t *f, size_t n, const struct nhdr *h, uint64_t *sender, size_t *po, size_t *pn)
{
    size_t k = h->len;
    if (k + 14 + 4 > n) return -1;
    uint8_t *aux = f + k, sc = aux[0], nonce[13];
    if ((sc & 0x18) != 0x08 || !(sc & 0x20)) return -1;   /* the network key, an extended nonce */
    uint32_t fc = rd32(aux + 1);
    uint64_t src = rd64(aux + 5);
    uint8_t alt[16];
    int pending = aux[13] != zb.key_seq && aps_pending_key(aux[13], alt);
    int prev = !pending && aux[13] != zb.key_seq && zb.have_prev_key && aux[13] == zb.prev_key_seq && now_ms() < zb.prev_key_until;
    /* another sequence number for what may be our key: the deCONZ protocol carries none, so a coordinator restored
     * from ZHA's backup after the key was changed has the key and 0; a frame that holds under it tells the number
     * (only the key's holders can make one) */
    static int seq_heard, learned;      /* our number held by a frame of the network's; one taken already */
    int learn = aux[13] != zb.key_seq && !pending && !prev && !seq_heard && !learned;
    if (src == zb.ieee || !src) return -1;  /* ours: a frame of ours sent back (a replay of a broadcast we relayed) */
    /* a counter per sender, always: an unknown one gets an entry (none left: the frame is refused) */
    struct node *s = node_by_ieee(src);
    int cur = aux[13] == zb.key_seq || pending || learn;   /* else the key before a switch: counters of their own (they began at 0 anew) */
    if (s && !pending && (cur ? s->fc_known && fc <= s->nwk_fc : s->prev_fc_known && fc <= s->prev_fc)) return -2;   /* a replay */
    const uint8_t *key = pending ? alt : cur ? zb.nwk_key : zb.prev_key;
    wr64(nonce, src); wr32(nonce + 8, fc); nonce[12] = (uint8_t)((sc & ~7) | 5);
    aux[0] = (uint8_t)((sc & ~7) | 5);
    uint8_t keep[ZB_MAX_FRAME];
    if (learn || prev) memcpy(keep, f, n);
    int bad = ccm_decrypt(key, nonce, f, k + 14, f + k + 14, n - k - 14);
    if (bad && prev && !(s && s->fc_known && fc <= s->nwk_fc)) {
        /* the old key's number on a frame under the current key: a coordinator restored from a backup made after a
         * new key (deCONZ carries no number) sends 0 until it hears ours; taken under the current key's counters */
        memcpy(f, keep, n);
        bad = ccm_decrypt(zb.nwk_key, nonce, f, k + 14, f + k + 14, n - k - 14);
        if (!bad) cur = 1;
    }
    aux[0] = sc;
    if (bad) { if (learn || prev) memcpy(f, keep, n); return -1; }
    if (learn) {
        /* once a start, and never once our own number was heard: a member flipping numbers moves nothing */
        LOG("nwk: our network key is the network's key number %u: taken (it had %u)", aux[13], zb.key_seq);
        zb.key_seq = aux[13]; learned = 1;
        zb_save();
    }
    if (aux[13] == zb.key_seq) seq_heard = 1;
    if (pending) {
        /* a frame under the key the trust center sent us to switch to: the network switched before its switch key
         * reached us (a sleepy device's poll came late); so do we (the spec's alternate key made active) */
        LOG("nwk: a frame under the new network key %u before its switch: switching", aux[13]);
        aps_pending_taken();
        nwk_switch_key(alt, aux[13]);
        s = node_by_ieee(src);
    }
    if (!s && !(s = node_add(src, 0xFFFF))) return -2;
    if (cur) { s->nwk_fc = fc; s->fc_known = 1; } else { s->prev_fc = fc; s->prev_fc_known = 1; }
    if (!s->authed) { s->authed = 1; s->join_at = 0; }
    *sender = src; *po = k + 14; *pn = n - k - 14 - 4;
    return 0;
}

/* ---------------------------------------------------------------- the network key's switch */

/* the new key active from now; the old one still taken for frames under it a while (sleepy devices that have not
 * polled for the switch yet, frames on the way), with its own counters: every device counts anew from 0 under a new
 * key (the spec's outgoing frame counter reset at the switch), ours too */
#define PREV_KEY_MS (30 * 60000)
void nwk_switch_key(const uint8_t key[16], uint8_t seq)
{
    if (seq == zb.key_seq && !memcmp(key, zb.nwk_key, 16)) return;
    memcpy(zb.prev_key, zb.nwk_key, 16); zb.prev_key_seq = zb.key_seq;
    zb.have_prev_key = 1; zb.prev_key_until = now_ms() + PREV_KEY_MS;
    memcpy(zb.nwk_key, key, 16); zb.key_seq = seq;
    for (int i = 0; i < zb.nnodes; i++) {
        struct node *n = &zb.nodes[i];
        n->prev_fc = n->nwk_fc; n->prev_fc_known = n->fc_known; n->fc_known = 0;
    }
    zb.nwk_fc = 0;
    LOG("nwk: switched to network key %u", seq);
    zb_save();
}

/* ---------------------------------------------------------------- the broadcast transaction table */

static int btt_seen(uint16_t src, uint8_t seq)
{
    int64_t now = now_ms(); int free_ = -1;
    for (int i = 0; i < ZB_BCAST_SLOTS; i++) {
        if (btt[i].until > now && btt[i].src == src && btt[i].seq == seq) return 1;
        if (btt[i].until <= now && free_ < 0) free_ = i;
    }
    if (free_ < 0) free_ = (int)(zb_random() % ZB_BCAST_SLOTS);
    btt[free_] = (struct bt){ src, seq, now + 9000 };
    return 0;
}

/* a broadcast repeated: the frame kept, sent twice more 500 ms apart (passive acks are not counted) */
struct rebcast { uint8_t f[ZB_MAX_FRAME]; uint8_t n, left; };
static struct rebcast rebs[8];
static void rebcast_fire(void *arg)
{
    struct rebcast *r = arg;
    if (!r->left) return;
    mac_data_direct(0xFFFF, 0, 0, r->f, r->n, NULL, NULL);
    if (--r->left) timer_at(now_ms() + 500 + zb_random() % 64, rebcast_fire, r);
}
static void bcast(const uint8_t *f, size_t n, int repeat)
{
    mac_data_direct(0xFFFF, 0, 0, f, n, NULL, NULL);
    if (n >= 4 && rd16(f + 2) == NWK_BCAST_ALL && zb.role != R_ENDDEV) {
        /* to all devices: our children that sleep never hear the air's copy; each gets it at its next poll, held
         * as a unicast to it (as Zigbee's parents keep broadcasts for them).  Only while the queue has room to
         * spare: their own frames come first */
        uint16_t from = n >= 6 ? rd16(f + 4) : 0xFFFF;   /* the broadcast's originator: not back to it */
        for (int i = 0; i < zb.nnodes; i++) {
            struct node *c = &zb.nodes[i];
            if (c->used && c->child && !(c->cap & 0x08) && c->nwk < 0xFFF8 && c->nwk != from && mac_indirect_free() > ZB_IND_MAX / 4)
                mac_data(c->nwk, 0, 0, f, n, NULL, NULL);
        }
    }
    if (!repeat || zb.role == R_ENDDEV) return;
    for (int i = 0; i < 8; i++) if (!rebs[i].left) {
        memcpy(rebs[i].f, f, n); rebs[i].n = (uint8_t)n; rebs[i].left = 2;
        timer_at(now_ms() + 500 + zb_random() % 64, rebcast_fire, &rebs[i]);
        return;
    }
}

/* ---------------------------------------------------------------- sending */

/* a unicast on its way to its next hop: what to tell when the MAC is done with it.  Held as long as the frame is
 * (an indirect one up to 7.68 s): taken from the free ones, never one in use */
struct hop { uint8_t used; void (*done)(int, void *); void *arg; uint16_t dst; uint16_t next; };
#define HOPS (ZB_IND_MAX + 32)
static struct hop hops[HOPS];

static void hop_done(int st, void *arg)
{
    struct hop *h = arg;
    if (st && h->next != 0xFFFF) {
        /* the next hop did not take it: that route is gone, and the next frame asks for a new one */
        struct node *d = node_by_nwk(h->dst);
        if (d && d->next_hop == h->next) d->next_hop = 0xFFFF;
        if (d && d->nrelay) d->nrelay = 0;
        if (h->dst == 0x0000 && zb.mto_next == h->next) zb.mto_next = 0xFFFF;
    }
    void (*d)(int, void *) = h->done; void *a = h->arg;
    h->used = 0; h->done = NULL;
    if (d) d(st ? APS_ST_MAC_NO_ACK : 0, a);
}

static struct hop *hop_new(void (*done)(int, void *), void *arg, uint16_t dst, uint16_t next)
{
    for (int i = 0; i < HOPS; i++) if (!hops[i].used) {
        struct hop *h = &hops[i];
        h->used = 1; h->done = done; h->arg = arg; h->dst = dst; h->next = next;
        return h;
    }
    return NULL;
}

static void route_discovery(uint16_t dst, uint64_t dst_ieee);
static void route_record(void);

/* where a unicast to dst goes next: 0xFFFF none known (then the caller asks); *srcroute set where it is source routed */
static uint16_t next_hop(uint16_t dst, struct node **dn)
{
    struct node *d = node_by_nwk(dst);
    *dn = d;
    if (zb.role == R_ENDDEV) return zb.parent;
    if (d && d->child) return dst;
    if (d && link_up(d)) return dst;
    if (dst == 0x0000 && zb.mto_next != 0xFFFF) return zb.mto_next;
    if (d && d->next_hop != 0xFFFF && now_ms() - d->route_at < ROUTE_MS) return d->next_hop;
    if (zb.role == R_ROUTER && dst == zb.parent && zb.parent != 0xFFFF) return zb.parent;   /* until links are known */
    return 0xFFFF;
}

int nwk_send(uint16_t dst, uint64_t dst_ieee, const uint8_t *a, size_t n, int secure, uint8_t radius,
             const uint16_t *relays, int nrelays, void (*done)(int, void *), void *arg)
{
    uint8_t f[ZB_MAX_FRAME];
    struct nhdr h = { 0 };
    int bc = dst >= 0xFFF8;
    h.fcf = (uint16_t)(NF_DATA | PROTO << 2 | (bc ? 0 : 1 << 6));
    h.dst = dst; h.src = zb.nwk; h.radius = radius ? radius : MAX_RADIUS; h.seq = zb.nwk_seq++;
    if (zb.role == R_ENDDEV && zb.state == NS_CONNECTED) h.fcf |= 1 << 13;     /* end device initiator */
    if (bc) {
        size_t m = frame(f, &h, a, n, secure);
        if (!m) { if (done) done(APS_ST_FRAME_TOO_LONG, arg); return -1; }
        btt_seen(zb.nwk, h.seq);
        bcast(f, m, 1);
        if (done) done(0, arg);
        return 0;
    }
    struct node *d;
    uint16_t next = 0xFFFF;
    if (relays && nrelays > 0 && nrelays <= 10) {
        h.fcf |= 1 << 10; h.nrelay = (uint8_t)nrelays; h.relay_idx = (uint8_t)(nrelays - 1);
        memcpy(h.relay, relays, sizeof *relays * (size_t)nrelays);
        next = relays[nrelays - 1];
        d = node_by_nwk(dst);
    } else {
        next = next_hop(dst, &d);
        if (next == 0xFFFF && d && d->nrelay && zb.role == R_COORD) {
            /* the concentrator's way: the route the device's route record took, back */
            h.fcf |= 1 << 10; h.nrelay = d->nrelay; h.relay_idx = (uint8_t)(d->nrelay - 1);
            memcpy(h.relay, d->relay, sizeof *d->relay * d->nrelay);
            next = d->relay[d->nrelay - 1];
        }
    }
    if (next == 0xFFFF) {
        /* no route: kept until a route discovery finds one, 4 s at most */
        int i;
        for (i = 0; i < ZB_PENDING_MAX && pend[i].used; i++) ;
        if (i == ZB_PENDING_MAX || n > sizeof pend[i].a) { if (done) done(APS_ST_NO_ROUTE, arg); return -1; }
        pend[i] = (struct pend){ 1, dst, dst_ieee, { 0 }, (uint8_t)n, secure, h.radius, done, arg, now_ms() + 4000 };
        memcpy(pend[i].a, a, n);
        route_discovery(dst, dst_ieee);
        return 0;
    }
    if (dst == 0x0000 && zb.role == R_ROUTER && zb.rr_needed && !(h.fcf & 1 << 10)) route_record();
    if (d && d->next_hop == next) d->route_at = now_ms();
    size_t m = frame(f, &h, a, n, secure);
    if (!m && (h.fcf & 1 << 10) && !relays) {
        /* the concentrator's source route leaves no room for this one: along the route table, or found anew */
        h.fcf &= (uint16_t)~(1 << 10); h.nrelay = 0;
        m = frame(f, &h, a, n, secure);
    }
    if (!m) { if (done) done(APS_ST_FRAME_TOO_LONG, arg); return -1; }
    struct hop *hp = hop_new(done, arg, dst, next == dst ? 0xFFFF : next);
    if (!hp) { if (done) done(APS_ST_BUSY, arg); return -1; }
    return mac_data(next, 0, 0, f, m, hop_done, hp);
}

/* our own command frames: dst a node (or a broadcast), with its EUI-64 in the header where asked */
int nwk_send_cmd(uint16_t dst, const uint8_t *c, size_t n, int secure, uint8_t radius, int use_ext_dst, uint64_t ext)
{
    uint8_t f[ZB_MAX_FRAME];
    struct nhdr h = { 0 };
    h.fcf = (uint16_t)(NF_CMD | PROTO << 2 | 1 << 12 | (use_ext_dst ? 1 << 11 : 0));
    h.dst = dst; h.src = zb.nwk; h.radius = radius; h.seq = zb.nwk_seq++;
    h.src_ieee = zb.ieee; h.dst_ieee = ext;
    size_t m = frame(f, &h, c, n, secure);
    if (!m) return -1;
    if (dst >= 0xFFF8) { btt_seen(zb.nwk, h.seq); bcast(f, m, radius > 1); return 0; }
    struct node *d;
    uint16_t next = next_hop(dst, &d);
    if (next == 0xFFFF) next = dst;     /* commands go where they can; a route request first would wait on them */
    return mac_data(next, ext, use_ext_dst && next == dst && d && d->child, f, m, NULL, NULL);
}

/* ---------------------------------------------------------------- routing */

static void flush_pending(uint16_t dst)
{
    for (int i = 0; i < ZB_PENDING_MAX; i++) if (pend[i].used && pend[i].dst == dst) {
        struct pend p = pend[i];
        pend[i].used = 0;
        struct node *d;
        if (next_hop(dst, &d) == 0xFFFF && !(d && d->nrelay)) { if (p.done) p.done(APS_ST_NO_ROUTE, p.arg); continue; }
        nwk_send(p.dst, p.dst_ieee, p.a, p.n, p.secure, p.radius, NULL, 0, p.done, p.arg);
    }
}

/* the expiry's timer at the earliest wanted, never moved later (frames sent more often than it waits kept it off) */
static int64_t pend_at;
static void pending_expire(void *unused);
static void pending_soon(int64_t when)
{
    if (pend_at && pend_at <= when) return;
    if (!timer_at(when, pending_expire, NULL)) pend_at = when;   /* only once it is set: else the next asks again */
}

static void pending_expire(void *unused)
{
    (void)unused;
    int64_t now = now_ms(), next = 0;
    pend_at = 0;
    for (int i = 0; i < ZB_PENDING_MAX; i++) if (pend[i].used) {
        if (pend[i].until <= now) { pend[i].used = 0; if (pend[i].done) pend[i].done(APS_ST_NO_ROUTE, pend[i].arg); }
        else if (!next || pend[i].until < next) next = pend[i].until;
    }
    if (next) pending_soon(next);
}

static struct disc *disc_find(uint16_t orig, uint8_t id)
{
    int64_t now = now_ms();
    for (int i = 0; i < ZB_DISC_SLOTS; i++) if (disc[i].until > now && disc[i].orig == orig && disc[i].id == id) return &disc[i];
    return NULL;
}

static struct disc *disc_new(void)
{
    int64_t now = now_ms(); int old = 0;
    for (int i = 0; i < ZB_DISC_SLOTS; i++) { if (disc[i].until <= now) return &disc[i]; if (disc[i].until < disc[old].until) old = i; }
    return &disc[old];
}

static void route_discovery(uint16_t dst, uint64_t dst_ieee)
{
    int64_t now = now_ms();
    for (int i = 0; i < ZB_DISC_SLOTS; i++)
        if (disc[i].ours && disc[i].until > now && disc[i].dst == dst) { pending_soon(now + 4000); return; }
    uint8_t c[16], *p = c;
    *p++ = NC_RREQ;
    *p++ = dst_ieee ? 1 << 5 : 0;
    *p++ = ++zb.rreq_id;
    p = wr16(p, dst); *p++ = 0;
    if (dst_ieee) p = wr64(p, dst_ieee);
    struct disc *d = disc_new();
    *d = (struct disc){ zb.nwk, dst, zb.rreq_id, 0xFFFF, 0, now + 10000, 1 };
    nwk_send_cmd(NWK_BCAST_ROUTERS, c, (size_t)(p - c), 1, MAX_RADIUS, 0, 0);
    pending_soon(now + 4000);
}

static void mto_send(void *unused)
{
    (void)unused;
    if (zb.state != NS_CONNECTED || zb.role != R_COORD) return;
    uint8_t c[6], *p = c;
    *p++ = NC_RREQ;
    *p++ = 1 << 3;                      /* many-to-one, route record table: routers send us route records */
    *p++ = ++zb.rreq_id;
    p = wr16(p, zb.nwk); *p++ = 0;
    nwk_send_cmd(NWK_BCAST_ROUTERS, c, (size_t)(p - c), 1, MAX_RADIUS, 0, 0);
    timer_at(now_ms() + MTO_MS, mto_send, NULL);
}

static void route_record(void)
{
    uint8_t c[2] = { NC_RREC, 0 };
    zb.rr_needed = 0;
    nwk_send_cmd(0x0000, c, 2, 1, MAX_RADIUS, 0, 0);
}

/* ---------------------------------------------------------------- link status */

static void link_status(void *unused)
{
    (void)unused;
    if (zb.state != NS_CONNECTED || zb.role == R_ENDDEV) return;
    uint16_t addr[64]; int na = 0;
    for (int i = 0; i < zb.nnodes && na < 64; i++) {
        struct node *n = &zb.nodes[i];
        if (n->used && link_up(n)) addr[na++] = n->nwk;
    }
    for (int i = 1; i < na; i++) for (int j = i; j > 0 && addr[j] < addr[j - 1]; j--) { uint16_t t = addr[j]; addr[j] = addr[j - 1]; addr[j - 1] = t; }
    int first = 1, k = 0;
    do {
        /* 26 entries a frame at most: MAC 9 + NWK 16 + aux 14 + 2 + 3 x 26 + MIC 4 + FCS 2 = 125 of 127 */
        uint8_t c[2 + 3 * 26], *p = c + 2;
        int m = 0;
        while (k < na && m < 26) {
            struct node *n = node_by_nwk(addr[k++]);
            p = wr16(p, n->nwk);
            *p++ = (uint8_t)(lqi_cost(n->lqi) | (n->out_cost ? n->out_cost : 7) << 4);
            m++;
        }
        c[0] = NC_LINK_STATUS;
        c[1] = (uint8_t)(m | (first ? 1 << 5 : 0) | (k >= na ? 1 << 6 : 0));
        uint8_t f[ZB_MAX_FRAME];
        struct nhdr h = { .fcf = NF_CMD | PROTO << 2 | 1 << 12, .dst = NWK_BCAST_ROUTERS, .src = zb.nwk, .radius = 1, .seq = zb.nwk_seq++, .src_ieee = zb.ieee };
        size_t fl = frame(f, &h, c, (size_t)(p - c), 1);
        if (fl) mac_data_direct(0xFFFF, 0, 0, f, fl, NULL, NULL);
        first = 0;
    } while (k < na);
    /* neighbors and children not heard for long */
    int64_t now = now_ms();
    for (int i = 0; i < zb.nnodes; i++) {
        struct node *n = &zb.nodes[i];
        if (!n->used) continue;
        if (n->neighbor && now - n->seen > 6 * LINK_STATUS_MS) { n->neighbor = 0; n->out_cost = 0; }
        if (n->join_at && !n->authed && now - n->join_at > JOINER_MS && (n->key_preconf || n->key_verified)) n->join_at = 0;   /* a key of its own stays */
        if (n->join_at && !n->authed && now - n->join_at > JOINER_MS) {
            /* it associated (or was announced) and never proved the network key: gone, so a stream of made-up
             * joiners cannot fill the address map (they are never saved either) */
            LOG("nwk: %016llx never finished joining: forgotten", (unsigned long long)n->ieee);
            node_forget(n);
            continue;
        }
        if (n->child && n->timeout_s && now - n->seen > (int64_t)n->timeout_s * 1000 + 30000) {
            LOG("nwk: end device %04x (%016llx) not heard for %u s: no longer our child", n->nwk, (unsigned long long)n->ieee, n->timeout_s);
            n->child = 0; mac_drop_indirect(n->ieee);
        }
    }
    /* the frame counters heard (replays refused over a restart) on the flash every 10 min, nothing more often */
    static int64_t saved;
    if (now - saved > 600000) { saved = now; zb_save_soon(); }
    timer_at(now + LINK_STATUS_MS - 1000 + zb_random() % 2000, link_status, NULL);
}

/* ---------------------------------------------------------------- the network */

void nwk_start(void)
{
    zb.state = NS_CONNECTED;
    if (zb.role != R_ENDDEV) timer_at(now_ms() + 1000 + zb_random() % 1000, link_status, NULL);
    if (zb.role == R_COORD) timer_at(now_ms() + 2000, mto_send, NULL);
    dz_state_changed();
}

void nwk_stop(void)
{
    zb.state = NS_OFFLINE;
    timer_cancel(link_status, NULL); timer_cancel(mto_send, NULL);
    for (int i = 0; i < ZB_PENDING_MAX; i++) if (pend[i].used) { pend[i].used = 0; if (pend[i].done) pend[i].done(APS_ST_NO_ROUTE, pend[i].arg); }
    dz_state_changed();
}

void nwk_leave(uint16_t nwk, uint64_t ieee, int rejoin, int remove_children)
{
    uint8_t c[2] = { NC_LEAVE, (uint8_t)((rejoin ? 1 << 5 : 0) | (remove_children ? 1 << 7 : 0)) };
    if (nwk == zb.nwk) { nwk_send_cmd(NWK_BCAST_RXON, c, 2, 1, 1, 0, 0); return; }   /* ourselves */
    c[1] |= 1 << 6;                     /* a request: that device leaves */
    nwk_send_cmd(nwk, c, 2, 1, MAX_RADIUS, 1, ieee);
}

void nwk_device_announced(uint16_t nwk, uint64_t ieee, uint8_t cap)
{
    nwk_address_seen(nwk, ieee);
    struct node *n = node_add(ieee, nwk);
    if (!n) return;
    n->cap = cap;
    if (n->child && n->nwk != nwk) n->child = 0;
    zb_save_soon();
}

/* Unsecured rejoins (trust center rejoins) waiting for the device to prove the network key: anyone in range can send
 * one in a known device's name (its EUI-64 is in every frame it sends), so nothing of a member's entry changes for
 * it (address, capability, child, authed); the device is told the address it has with us, gets the key (sealed with
 * a key of its own, never the global one with joining closed: aps.c may_join) and, once a secured frame of its own
 * comes from it, its place here (child) as its rejoin said */
#define PRJ 8
#define PRJ_MS 15000
#define JOINERS_MAX 16                  /* entries of joiners not yet proven at once (associations and rejoins) */
static struct prj { uint64_t ieee; uint16_t poll; uint8_t cap; int64_t at; } prj[PRJ];

static struct prj *prj_find(uint64_t ieee)
{
    int64_t now = now_ms();
    for (int i = 0; i < PRJ; i++) if (prj[i].ieee == ieee && now - prj[i].at < PRJ_MS) return &prj[i];
    return NULL;
}

static struct prj *prj_put(uint64_t ieee, uint16_t poll, uint8_t cap)
{
    struct prj *p = prj_find(ieee);
    if (!p) { p = &prj[0]; for (int i = 1; i < PRJ; i++) if (prj[i].at < p->at) p = &prj[i]; }
    p->ieee = ieee; p->poll = poll; p->cap = cap; p->at = now_ms();
    return p;
}

/* a secured frame of the device's own (the aux header's source, the last hop: it came to us straight) */
static void prj_proved(uint64_t ieee, uint16_t mac_src)
{
    struct prj *p = prj_find(ieee);
    if (!p) return;
    struct node *n = node_by_ieee(ieee);
    p->ieee = 0;
    if (!n) return;
    n->cap = p->cap; n->child = !(p->cap & 0x02); n->parent = zb.nwk; n->seen = now_ms();
    n->timeout_s = n->timeout_s ? n->timeout_s : 256;
    if (mac_src < 0xFFF8 && mac_src != n->nwk) node_add(ieee, mac_src);
    LOG("nwk: %016llx proved the key after its rejoin: %s", (unsigned long long)ieee, n->child ? "our child" : "a router");
    zb_save_soon();
}

int nwk_joiners(void)
{
    int k = 0;
    for (int i = 0; i < zb.nnodes; i++) k += zb.nodes[i].used && zb.nodes[i].join_at && !zb.nodes[i].authed;
    return k;
}

/* an unsecured NWK frame to a device joining or rejoining through us (a rejoin response, the network key; nwk the
 * address it is told to use): to a sleepy one held for the poll it sends by the address it has now, in the places
 * joiners have (mac.c: one each, four in all); to one awake straight, by its EUI-64 */
int nwk_deliver_unsecured(uint64_t ieee, uint16_t nwk, const uint8_t *f, size_t n)
{
    struct prj *p = prj_find(ieee);
    struct node *d = node_by_ieee(ieee);
    uint8_t cap = p ? p->cap : d ? d->cap : 0x80;
    uint16_t poll = p ? p->poll : d ? d->nwk : nwk;
    if (!(cap & 0x08)) return mac_indirect_as(ieee, poll, f, n, 1);
    return mac_data_direct(nwk, ieee, 1, f, n, NULL, NULL);
}

static void rejoin_reply(uint64_t ieee, uint16_t nwk, int secured)
{
    uint8_t c[4] = { NC_REJOIN_RSP }, f[ZB_MAX_FRAME];
    wr16(c + 1, nwk); c[3] = 0;
    struct nhdr h = { .fcf = NF_CMD | PROTO << 2 | 1 << 11 | 1 << 12, .dst = nwk, .src = zb.nwk, .radius = 1, .seq = zb.nwk_seq++,
                      .dst_ieee = ieee, .src_ieee = zb.ieee };
    size_t m = frame(f, &h, c, 4, secured);
    if (!m) return;
    if (secured) mac_data(nwk, ieee, 1, f, m, NULL, NULL);
    else nwk_deliver_unsecured(ieee, nwk, f, m);
}

static void rejoin_request(const struct nhdr *h, uint64_t mac_ext, int src_ext, uint8_t cap, int secured, uint8_t lqi)
{
    uint64_t ieee = h->src_ieee ? h->src_ieee : src_ext ? mac_ext : 0;
    if (!ieee || zb.role == R_ENDDEV || ieee == zb.ieee) return;
    struct node *n = node_by_ieee(ieee);
    int64_t now = now_ms();
    int open_ = zb.net_permit_until > now || zb.permit_until > now;
    uint16_t was = h->src;
    if (!secured) {
        /* An unsecured rejoin (the device lost the key, or missed a new one) is the trust center's to allow: while
         * joining is open, or for a device whose link key of its own is proven (verified: Zigbee 3.0's request
         * key) or was never on the air (an install code's).  A router passes a member's on to the trust center
         * (update device), which decides so; a stranger's only while joining is open */
        if (!n && !open_) return;
        if (!n && nwk_joiners() >= JOINERS_MAX) return;
        if (zb.role == R_COORD && !open_ && !(n->key_verified || n->key_preconf)) {
            LOG("nwk: unsecured rejoin of %016llx refused: joining is closed and it has no proven link key of its own", (unsigned long long)ieee);
            return;
        }
        struct prj *p = prj_find(ieee);
        if (p && now - p->at < 2000) return;              /* its repeats (and anyone's flood in its name) */
        if (n && n->authed) {
            /* a member: told the address it has with us; its entry as it is until it proves the key */
            uint16_t addr = n->nwk < 0xFFF8 ? n->nwk : was < 0xFFF8 && !node_by_nwk(was) ? was : nwk_new_address();
            if (n->nwk >= 0xFFF8) node_add(ieee, addr);
            prj_put(ieee, was, cap);
            LOG("nwk: unsecured (trust center) rejoin of %016llx: told %04x, waiting for it to prove the key", (unsigned long long)ieee, addr);
            rejoin_reply(ieee, addr, 0);
            if (zb.role == R_COORD) aps_tc_joined(n, 0x0000, 1);
            else aps_update_device(ieee, addr, 0x03);
            return;
        }
        prj_put(ieee, was, cap);                          /* a joiner's: where its frames wait */
    }
    uint16_t addr = was;
    struct node *o = node_by_nwk(addr);
    if (addr >= 0xFFF8 || addr == zb.nwk || (o && o->ieee && o->ieee != ieee)) addr = nwk_new_address();
    n = node_add(ieee, addr);
    if (!n) return;
    n->cap = cap; n->seen = now; n->lqi = lqi; n->parent = zb.nwk;
    n->child = !(cap & 0x02); n->timeout_s = n->timeout_s ? n->timeout_s : 256;
    if (!secured) { n->authed = 0; n->join_at = now; }
    LOG("nwk: %s rejoin of %016llx as %04x", secured ? "secured" : "unsecured (trust center)", (unsigned long long)ieee, n->nwk);
    if (secured && n->child && !(cap & 0x08) && was < 0xFFF8 && was != n->nwk) {
        /* a sleepy child with a new address: the response waits under the one it polls by */
        uint8_t c[4] = { NC_REJOIN_RSP }, f[ZB_MAX_FRAME];
        wr16(c + 1, n->nwk); c[3] = 0;
        struct nhdr rh = { .fcf = NF_CMD | PROTO << 2 | 1 << 11 | 1 << 12, .dst = was, .src = zb.nwk, .radius = 1, .seq = zb.nwk_seq++,
                           .dst_ieee = ieee, .src_ieee = zb.ieee };
        size_t m = frame(f, &rh, c, 4, 1);
        if (m) mac_indirect_as(ieee, was, f, m, 0);
    } else rejoin_reply(ieee, n->nwk, secured);
    if (zb.role == R_COORD) { if (!secured) aps_tc_joined(n, 0x0000, 1); }
    else aps_update_device(ieee, n->nwk, secured ? 0x00 : 0x03);
    zb_save_soon();
}

/* ---------------------------------------------------------------- address conflicts (3.6.1.9) */

/* a new address for us: a router or end device whose address another device has too; announced as a device
 * announce (the coordinator keeps 0x0000: the other one moves) */
static void change_address(void *unused)
{
    (void)unused;
    if (zb.role == R_COORD || zb.state != NS_CONNECTED) return;
    uint16_t old = zb.nwk;
    zb.nwk = nwk_new_address();
    rcp_set_u16(SP_MAC_SADDR, zb.nwk);
    LOG("nwk: address conflict: %04x is another device's too; ours is now %04x", old, zb.nwk);
    zb_save();
    zdo_device_announce();
}

/* our sleepy child's address is another's too: a new one, told as an unsolicited rejoin response held for its next
 * poll under the address it polls by */
static void child_new_address(struct node *c)
{
    uint16_t old = c->nwk, nw = nwk_new_address();
    uint8_t m[4] = { NC_REJOIN_RSP }, f[ZB_MAX_FRAME];
    wr16(m + 1, nw); m[3] = 0;
    struct nhdr h = { .fcf = NF_CMD | PROTO << 2 | 1 << 11 | 1 << 12, .dst = old, .src = zb.nwk, .radius = 1, .seq = zb.nwk_seq++,
                      .dst_ieee = c->ieee, .src_ieee = zb.ieee };
    size_t k = frame(f, &h, m, 4, 1);
    LOG("nwk: address conflict: our child %016llx at %04x moves to %04x", (unsigned long long)c->ieee, old, nw);
    mac_drop_indirect(c->ieee);
    c->nwk = nw;
    if (k) mac_indirect_as(c->ieee, old, f, k, 0);
    zb_save_soon();
}

static void conflict_report(uint16_t addr)
{
    static int64_t last; static uint16_t last_addr;
    int64_t now = now_ms();
    if (addr == last_addr && now - last < 30000) return;     /* once per address and half minute */
    last = now; last_addr = addr;
    uint8_t c[4] = { NC_STATUS, ST_ADDR_CONFLICT };
    wr16(c + 2, addr);
    nwk_send_cmd(NWK_BCAST_RXON, c, 4, 1, MAX_RADIUS, 0, 0);
}

/* an address and the EUI-64 a secured frame (or device announce) gives for it: the map moved, or two devices hold
 * one address: then ours changes, our child's is changed, or the network hears of it (network status 0x0D) */
void nwk_address_seen(uint16_t nwk, uint64_t ieee)
{
    if (!ieee || nwk >= 0xFFF8) return;
    if (ieee == zb.ieee) return;
    if (nwk == zb.nwk && zb.state == NS_CONNECTED) {
        if (zb.role == R_COORD) conflict_report(nwk);         /* the other one moves */
        else timer_at(now_ms() + zb_random() % 500, change_address, NULL);
        return;
    }
    struct node *o = node_by_nwk(nwk);
    int64_t now = now_ms();
    if (o && o->ieee && o->ieee != ieee && o->used && (now - o->seen < 120000 || now - o->heard < 120000) && zb.role != R_ENDDEV) {
        /* both heard lately: not a move but a conflict.  A sleepy child of ours: we give it a new address; else
         * everybody is told and each holder that is not the coordinator moves */
        if (o->child && !(o->cap & 0x08)) { child_new_address(o); node_add(ieee, nwk); return; }
        struct node *x = node_by_ieee(ieee);
        if (x && x->child && !(x->cap & 0x08)) { node_add(ieee, nwk); child_new_address(x); return; }
        conflict_report(nwk);
    }
    struct node *s = node_by_ieee(ieee);
    if (!s || s->nwk != nwk) s = node_add(ieee, nwk);
    if (s) s->heard = now;
}

/* ---------------------------------------------------------------- PAN ID conflicts (3.6.1.13) */

static struct { int count; int64_t first, last_fix; uint16_t pan; uint64_t epan; } pc;

static uint16_t new_pan;                /* the one a PAN ID change moves to (one timer, whatever asks) */
static void pan_change(void *unused)
{
    (void)unused;
    uint16_t np = new_pan;
    LOG("nwk: PAN ID %04x -> %04x (update %u)", zb.pan, np, zb.update_id);
    zb.pan = np;
    rcp_set_u16(SP_MAC_PANID, np);
    zb_save();
    dz_state_changed();
}

/* a beacon with our PAN ID and another extended PAN ID: two networks share the PAN ID.  Counted (three beacons
 * over half a minute at least: one made-up beacon moves nothing), then the network manager (our coordinator) picks
 * another and broadcasts a network update, all switch after the broadcast's time; a router reports it to it */
void nwk_beacon_heard(uint16_t pan, uint64_t epan)
{
    int64_t now = now_ms();
    if (pan != zb.pan || epan == zb.epan || !epan) return;
    if (now - pc.first > 300000 || pc.epan != epan) { pc.count = 0; pc.first = now; pc.epan = epan; }
    if (++pc.count < 3 || now - pc.first < 30000) return;
    if (pc.last_fix && now - pc.last_fix < 3600000) return;   /* once an hour at most */
    pc.last_fix = now; pc.count = 0;
    LOG("nwk: PAN ID conflict: %016llx uses our PAN ID %04x too", (unsigned long long)epan, pan);
    if (zb.role == R_COORD) {
        uint16_t np;
        do np = (uint16_t)(zb_random() % 0xFFFE + 1); while (np == zb.pan);
        zb.update_id++;
        uint8_t c[14], *p = c;
        *p++ = NC_NWK_UPDATE; *p++ = 1;                       /* one entry, type 0: PAN identifier update */
        p = wr64(p, zb.epan); *p++ = zb.update_id; p = wr16(p, np);
        nwk_send_cmd(NWK_BCAST_ALL, c, (size_t)(p - c), 1, MAX_RADIUS, 0, 0);
        new_pan = np; timer_at(now + BCAST_DELIVERY_MS, pan_change, NULL);
    } else {
        uint8_t c[12], *p = c;
        *p++ = NC_NWK_REPORT; *p++ = 1;                       /* one entry, type 0: PAN identifier conflict */
        p = wr64(p, zb.epan); p = wr16(p, pan);
        nwk_send_cmd(0x0000, c, (size_t)(p - c), 1, MAX_RADIUS, 0, 0);
    }
}

/* ---------------------------------------------------------------- joining (router, end device) */

void nwk_assoc_result(uint16_t addr, uint8_t status)
{
    if (js.phase != 2) return;
    if (status) { LOG("nwk: association refused by %04x (status %u)", js.best.src, status); js.phase = 0; timer_at(now_ms() + 3000, nwk_join_found, NULL); return; }
    zb.nwk = addr; zb.parent = js.best.src;
    rcp_set_u16(SP_MAC_SADDR, addr);
    LOG("nwk: associated with %04x on channel %u, PAN %04x: address %04x; waiting for the network key", js.best.src, js.best.channel, js.best.pan, addr);
    js.phase = 3; js.at = now_ms();
    timer_at(now_ms() + 300, nwk_join_found, NULL);
}

/* the key came (aps.c): the network is ours */
void nwk_joined(void)
{
    js.phase = 0;
    zb.joined = 1;
    zb.depth = (uint8_t)(js.best.depth + 1);
    nwk_start();
    zb_save();
    LOG("nwk: joined the network %016llx as %04x (%s)", (unsigned long long)zb.epan, zb.nwk, zb.role == R_ROUTER ? "router" : "end device");
    zdo_device_announce();
    timer_at(now_ms() + 600, aps_request_tclk, NULL);
    if (zb.role == R_ENDDEV) {
        uint8_t c[3] = { NC_ED_TIMEOUT_REQ, 8, 0 };      /* 256 min */
        nwk_send_cmd(zb.parent, c, 3, 1, 1, 0, 0);
    }
}

/* the networks a scan heard, for the page's choice: one entry per extended PAN ID (the best beacon), kept 10 min */
static void nets_note(const struct beacon *b, int nb)
{
    int64_t now = now_ms();
    for (int i = 0; i < nb; i++) {
        if (b[i].stack != 2 || !b[i].epan) continue;
        int k = -1, old = 0;
        for (int j = 0; j < 16; j++) {
            if (zb.nets[j].at && zb.nets[j].epan == b[i].epan) { k = j; break; }
            if (zb.nets[j].at < zb.nets[old].at) old = j;
        }
        if (k < 0) { k = old; zb.nets[k].lqi = 0; zb.nets[k].permit = 0; zb.nets[k].room = 0; zb.nets[k].at = 0; }
        struct seen_net *e = &zb.nets[k];
        int fresh = now - e->at > 30000;
        e->epan = b[i].epan;
        if (fresh || b[i].lqi > e->lqi) { e->pan = b[i].pan; e->channel = b[i].channel; e->lqi = b[i].lqi; }
        e->permit = (uint8_t)((fresh ? 0 : e->permit) | b[i].permit);
        e->room = (uint8_t)((fresh ? 0 : e->room) | (zb.role == R_ROUTER ? b[i].router_cap : b[i].ed_cap));
        e->at = now;
    }
    for (int j = 0; j < 16; j++) if (zb.nets[j].at && now - zb.nets[j].at > 600000) zb.nets[j].at = 0;
}

void nwk_join_found(void *unused)
{
    (void)unused;
    if (zb.state != NS_JOINING) return;
    int64_t now = now_ms();
    switch (js.phase) {
    case 0:                             /* scan from channel 11 */
        rcp_promiscuous(1);
        rcp_set_u16(SP_MAC_PANID, 0xFFFF);
        rcp_set_u16(SP_MAC_SADDR, 0xFFFE);
        zb.nwk = 0xFFFF;
        mac_scan_begin();
        js.ch = 11; js.phase = 1;
        /* fall through */
    case 1:
        if (js.ch <= 25 && (zb.channel_mask & 1u << js.ch)) {
            zb.channel = js.ch;
            rcp_set_u8(SP_PHY_CHAN, (uint8_t)js.ch);
            mac_beacon_request();
            js.ch++;
            timer_at(now + 250, nwk_join_found, NULL);
            return;
        }
        if (js.ch <= 25) { js.ch++; timer_at(now, nwk_join_found, NULL); return; }
        {
            struct beacon b[32]; int nb = mac_scan_results(b, 32), best = -1;
            rcp_promiscuous(0);
            nets_note(b, nb);
            for (int i = 0; i < nb; i++) {
                if (!b[i].permit || b[i].stack != 2) continue;
                if (zb.role == R_ROUTER ? !b[i].router_cap : !b[i].ed_cap) continue;
                if (zb.epan && zb.epan != b[i].epan) continue;
                if (zb.prefer_epan && zb.prefer_epan != b[i].epan) continue;     /* the owner's choice of network */
                if (best < 0 || b[i].lqi > b[best].lqi) best = i;
            }
            if (best < 0) {
                if (js.tries++ % 12 == 0) LOG("nwk: no network open for joining%s (%d beacons heard): looking again", zb.prefer_epan ? " of the chosen one" : "", nb);
                js.phase = 0;
                timer_at(now + 5000, nwk_join_found, NULL);
                return;
            }
            js.best = b[best];
            LOG("nwk: joining %016llx through %04x on channel %u (lqi %u)", (unsigned long long)b[best].epan, b[best].src, b[best].channel, b[best].lqi);
            zb.channel = b[best].channel; zb.pan = b[best].pan; zb.epan = b[best].epan; zb.update_id = b[best].update_id;
            rcp_set_u8(SP_PHY_CHAN, (uint8_t)zb.channel);
            rcp_set_u16(SP_MAC_PANID, zb.pan);
            zb.parent = b[best].src;
            mac_associate(zb.pan, b[best].src, zb.role == R_ROUTER ? 0x8E : 0x80);
            js.phase = 2; js.at = now; js.tries = 0;
            timer_at(now + 500, nwk_join_found, NULL);
            return;
        }
    case 2:                             /* the association response is kept for our poll */
        if (now - js.at > 3000) { LOG("nwk: no association response"); js.phase = 0; timer_at(now + 2000, nwk_join_found, NULL); return; }
        mac_poll();
        timer_at(now + 400, nwk_join_found, NULL);
        return;
    case 3:                             /* the key: a router hears it straight, an end device polls for it */
        if (now - js.at > 6000) { LOG("nwk: the network key never came"); zb.nwk = 0xFFFF; js.phase = 0; timer_at(now + 2000, nwk_join_found, NULL); return; }
        if (zb.role == R_ENDDEV) mac_poll();
        timer_at(now + 300, nwk_join_found, NULL);
        return;
    }
}

/* An end device whose parent stops acking its polls (it moved to another address, or went away) rejoins: a beacon
 * request on our channel, then a secured rejoin request to the best router of our network that answered, then polls
 * to it for the rejoin response (3.6.1.4.2).  Again every few seconds until one takes us */
static struct { int phase, fails; int64_t at; uint16_t to; } rj;

static void rejoin_step(void *unused)
{
    (void)unused;
    if (zb.role != R_ENDDEV || zb.state != NS_CONNECTED) { rj.phase = 0; return; }
    int64_t now = now_ms();
    if (rj.phase == 1) {                /* the scan's beacons in: ask the best of ours */
        struct beacon b[32]; int nb = mac_scan_results(b, 32), best = -1;
        for (int i = 0; i < nb; i++) if (b[i].epan == zb.epan && b[i].ed_cap && (best < 0 || b[i].lqi > b[best].lqi)) best = i;
        if (best < 0) { rj.phase = 0; timer_at(now + 5000, rejoin_step, NULL); return; }
        rj.to = b[best].src;
        uint8_t c[2] = { NC_REJOIN_REQ, 0x80 }, f[ZB_MAX_FRAME];
        struct nhdr h = { .fcf = NF_CMD | PROTO << 2 | 1 << 12, .dst = rj.to, .src = zb.nwk, .radius = 1, .seq = zb.nwk_seq++, .src_ieee = zb.ieee };
        size_t m = frame(f, &h, c, 2, 1);
        LOG("nwk: rejoining through %04x (lqi %u)", rj.to, b[best].lqi);
        zb.parent = rj.to;
        if (m) mac_data_direct(rj.to, 0, 0, f, m, NULL, NULL);
        rj.phase = 2; rj.at = now;
        timer_at(now + 300, rejoin_step, NULL);
        return;
    }
    if (rj.phase == 2) {                /* its response is held for our poll */
        if (now - rj.at > 3000) { rj.phase = 0; timer_at(now + 2000, rejoin_step, NULL); return; }
        mac_poll();
        timer_at(now + 400, rejoin_step, NULL);
        return;
    }
    rj.phase = 1;
    mac_scan_begin();
    mac_beacon_request();
    timer_at(now + 400, rejoin_step, NULL);
}

static void poll_done(int st, void *unused)
{
    (void)unused;
    if (!st) { rj.fails = 0; return; }
    if (++rj.fails >= 4 && !rj.phase && zb.role == R_ENDDEV && zb.state == NS_CONNECTED) {
        LOG("nwk: our parent %04x does not answer our polls: rejoining", zb.parent);
        rj.fails = 0;
        rejoin_step(NULL);
    }
}

static void ed_poll(void *unused)
{
    (void)unused;
    if (zb.role != R_ENDDEV) return;
    if (zb.state == NS_CONNECTED && !rj.phase) mac_poll_then(poll_done);
    timer_at(now_ms() + 1000, ed_poll, NULL);
}

void nwk_begin_join(void)
{
    zb.state = NS_JOINING; js.phase = 0; js.tries = 0;
    timer_at(now_ms(), nwk_join_found, NULL);
    if (zb.role == R_ENDDEV) timer_at(now_ms() + 1000, ed_poll, NULL);
}

void nwk_resume_enddev(void) { if (zb.role == R_ENDDEV) timer_at(now_ms() + 1000, ed_poll, NULL); }

/* ---------------------------------------------------------------- receiving */

static void forward(uint8_t *f, size_t n, struct nhdr *h, const uint8_t *pl, size_t pn, int secured);
static void command(uint8_t *f, size_t fl, struct nhdr *h, const uint8_t *c, size_t n, uint16_t mac_src, uint64_t mac_ext, int src_ext, int secured, const struct rxinfo *ri);

void nwk_rx(const uint8_t *in, size_t n, uint16_t mac_src, uint64_t mac_ext, int src_ext, const struct rxinfo *ri)
{
    uint8_t f[ZB_MAX_FRAME];
    struct nhdr h;
    if (n > sizeof f) return;
    memcpy(f, in, n);
    if (n >= 1 && (f[0] >> 2 & 0x0F) == 3) {
        /* Green Power (a GPDF: no NWK header of Zigbee's): the coordinator's host's sink; a router's proxy */
        if (zb.role == R_COORD) dz_green_power(f, n);
        else gp_rx(f, n, mac_rx_seq, ri);
        return;
    }
    if (parse_hdr(f, n, &h)) return;
    int ver = h.fcf >> 2 & 0x0F, type = h.fcf & 3;
    if (ver != PROTO || type > NF_CMD) return;
    if (h.src == zb.nwk && (!h.src_ieee || h.src_ieee == zb.ieee)) return;   /* our own broadcast, relayed back */
    /* the last hop: a neighbor heard (its link costs come from its link status) */
    size_t po = h.len, pn = n - h.len;
    uint64_t sender = 0;
    int secured = h.fcf >> 9 & 1;
    if (secured) {
        int r = unsecure(f, n, &h, &sender, &po, &pn);
        if (r) { if (r == -1) zb.rx_bad++; return; }
        /* the last hop: a neighbor heard, only once its frame held (garbage with its address keeps nothing alive).
         * The aux header names the EUI-64 that secured it, the last hop's */
        prj_proved(sender, mac_src);
        struct node *last = node_by_ieee(sender);
        if (last && mac_src < 0xFFF8 && mac_src != 0xFFFE && last->nwk != mac_src) last = node_add(sender, mac_src);
        if (last) { last->seen = now_ms(); last->lqi = ri->lqi; }
    } else if (zb.joined || zb.role == R_COORD) {
        /* only what joining needs goes unsecured: a joiner's rejoin request, a key to us while we join.  Nothing in
         * such a frame moves an address: anyone can send one (the rejoin moves it once the trust center allowed it) */
        if (type == NF_CMD ? pn < 1 || (f[po] != NC_REJOIN_REQ && f[po] != NC_REJOIN_RSP) : zb.state != NS_JOINING) return;
    }
    if (secured && h.src_ieee && h.src < 0xFFF8) nwk_address_seen(h.src, h.src_ieee);
    if (secured) {
        /* the frame's originator is in the network: a frame of its own got here under the network key (each hop
         * secures anew, so a device behind a router never shows its own aux header to us; the relay that passed it
         * on had to hold the key) */
        struct node *o = h.src_ieee ? node_by_ieee(h.src_ieee) : h.src < 0xFFF8 ? node_by_nwk(h.src) : NULL;
        if (o && !o->authed) { o->authed = 1; o->join_at = 0; zb_save_soon(); }
    }
    int bc = h.dst >= 0xFFF8;
    if (bc && btt_seen(h.src, h.seq)) return;
    int mine = h.dst == zb.nwk || (bc && (h.dst == NWK_BCAST_ALL || h.dst == NWK_BCAST_RXON || (h.dst == NWK_BCAST_ROUTERS && zb.role != R_ENDDEV)))
             || (zb.state == NS_JOINING && h.dst_ieee == zb.ieee);
    if (h.fcf & 1 << 10 && !bc && h.dst != zb.nwk) mine = 0;
    if (bc && zb.role != R_ENDDEV && h.radius > 1 && zb.state == NS_CONNECTED) {
        /* relayed once, as it came, secured anew */
        h.radius--;
        uint8_t g[ZB_MAX_FRAME];
        size_t m = frame(g, &h, f + po, pn, secured);
        if (m) bcast(g, m, 1);
    }
    if (mine) {
        if (type == NF_DATA) aps_rx(f + po, pn, h.src, h.src_ieee ? h.src_ieee : (node_by_nwk(h.src) ? node_by_nwk(h.src)->ieee : 0), h.dst, secured, ri);
        else if (pn) command(f, n, &h, f + po, pn, mac_src, mac_ext, src_ext, secured, ri);
        return;
    }
    if (!bc && zb.role != R_ENDDEV && zb.state == NS_CONNECTED) forward(f, n, &h, f + po, pn, secured);
}

static void status_to(uint16_t to, uint8_t code, uint16_t about)
{
    uint8_t c[4] = { NC_STATUS, code };
    wr16(c + 2, about);
    nwk_send_cmd(to, c, 4, 1, MAX_RADIUS, 0, 0);
}

/* a unicast through us: down a source route, to a child, along a route; else the source hears there is none */
static void forward(uint8_t *f, size_t n, struct nhdr *h, const uint8_t *pl, size_t pn, int secured)
{
    (void)f; (void)n;
    uint8_t g[ZB_MAX_FRAME];
    uint16_t next;
    if (h->radius <= 1) return;
    h->radius--;
    if (h->fcf & 1 << 10) {
        if (h->relay_idx >= h->nrelay || h->relay[h->relay_idx] != zb.nwk) return;
        if (h->relay_idx == 0) next = h->dst;
        else { h->relay_idx--; next = h->relay[h->relay_idx]; }
    } else {
        struct node *d;
        next = next_hop(h->dst, &d);
        if (next == 0xFFFF) {
            status_to(h->src, 0x00, h->dst);                 /* no route available */
            route_discovery(h->dst, h->dst_ieee);
            return;
        }
        if (h->dst == 0x0000 && pl && pn && (h->fcf & 3) == NF_CMD && pl[0] == NC_RREC) {
            /* a route record on its way to the concentrator: we are one of its relays */
            uint8_t c[2 + 2 * 12];
            if (pn < 2 || pl[1] >= 10 || pn != 2u + 2u * pl[1] || pn + 2 > sizeof c) return;
            memcpy(c, pl, pn);
            c[1]++;
            wr16(c + pn, zb.nwk);
            size_t m = frame(g, h, c, pn + 2, secured);
            if (m) mac_data(next, 0, 0, g, m, NULL, NULL);
            return;
        }
    }
    size_t m = frame(g, h, pl, pn, secured);
    if (m) mac_data(next, 0, 0, g, m, NULL, NULL);
}

static void command(uint8_t *f, size_t fl, struct nhdr *h, const uint8_t *c, size_t n, uint16_t mac_src, uint64_t mac_ext, int src_ext, int secured, const struct rxinfo *ri)
{
    (void)f; (void)fl;
    int64_t now = now_ms();
    uint8_t link = lqi_cost(ri->lqi);
    switch (c[0]) {
    case NC_RREQ: {
        if (n < 6 || !secured || zb.role == R_ENDDEV) return;
        uint8_t opt = c[1], id = c[2], cost = (uint8_t)(c[5] + link);
        uint16_t dst = rd16(c + 3);
        if (opt & 0x18) {
            /* many-to-one: the way to the concentrator, through whoever brought it cheapest */
            if (dst == zb.nwk) return;
            struct disc *d = disc_find(h->src, id);
            if (d && d->cost <= cost) return;
            if (!d) d = disc_new();
            *d = (struct disc){ h->src, dst, id, mac_src, cost, now + 10000, 0 };
            if (dst == 0x0000) { zb.mto_next = mac_src; zb.mto_cost = cost; zb.rr_needed = (opt & 0x18) == 0x08; }
            struct node *cn = node_by_nwk(dst);
            if (cn) { cn->next_hop = mac_src; cn->route_cost = cost; cn->route_at = now; }
            if (h->radius > 1) {
                uint8_t r[16]; memcpy(r, c, n < 16 ? n : 16); r[5] = cost;
                struct nhdr hh = *h; hh.radius--;
                uint8_t g[ZB_MAX_FRAME]; size_t m = frame(g, &hh, r, n < 16 ? n : 16, 1);
                if (m) bcast(g, m, 0);
            }
            return;
        }
        struct node *dn = node_by_nwk(dst);
        int ours = dst == zb.nwk || (dn && dn->child);
        struct disc *d = disc_find(h->src, id);
        if (d && d->cost <= cost) return;
        if (!d) d = disc_new();
        *d = (struct disc){ h->src, dst, id, mac_src, cost, now + 10000, 0 };
        struct node *on = node_by_nwk(h->src);
        if (!on && h->src_ieee) on = node_add(h->src_ieee, h->src);
        if (on) { on->next_hop = mac_src; on->route_cost = cost; on->route_at = now; }
        if (ours) {
            uint8_t r[8] = { NC_RREP, 0, id };
            wr16(r + 3, h->src); wr16(r + 5, dst); r[7] = (uint8_t)(dn && dn->child ? 1 : 0);
            /* a route reply goes hop by hop: to the one the request came from, which builds it again towards the
             * originator (and sets up its route to us on the way) */
            uint8_t g[ZB_MAX_FRAME];
            struct nhdr hh = { .fcf = NF_CMD | PROTO << 2 | 1 << 12, .dst = mac_src, .src = zb.nwk, .radius = 1, .seq = zb.nwk_seq++, .src_ieee = zb.ieee };
            size_t m = frame(g, &hh, r, 8, 1);
            if (m) mac_data(mac_src, 0, 0, g, m, NULL, NULL);
        } else if (h->radius > 1) {
            uint8_t r[16]; memcpy(r, c, n < 16 ? n : 16); r[5] = cost;
            struct nhdr hh = *h; hh.radius--;
            uint8_t g[ZB_MAX_FRAME]; size_t m = frame(g, &hh, r, n < 16 ? n : 16, 1);
            if (m) bcast(g, m, 0);
        }
        return;
    }
    case NC_RREP: {
        if (n < 8 || !secured) return;
        uint8_t id = c[2], cost = (uint8_t)(c[7] + link);
        uint16_t orig = rd16(c + 3), resp = rd16(c + 5);
        struct node *rn = node_by_nwk(resp);
        if (!rn) { uint64_t ie = (c[1] & 1 << 5) && n >= 8u + 8u + ((c[1] & 1 << 4) ? 8u : 0u) ? rd64(c + 8 + ((c[1] & 1 << 4) ? 8 : 0)) : 0; rn = ie ? node_add(ie, resp) : NULL; }
        if (!rn) {
            /* a node we do not know by EUI-64: an entry by address alone, for its route */
            int i; for (i = 0; i < zb.nnodes && zb.nodes[i].used; i++) ;
            if (i == zb.nnodes && zb.nnodes < ZB_MAX_NODES) zb.nnodes++;
            if (i < zb.nnodes) { rn = &zb.nodes[i]; memset(rn, 0, sizeof *rn); rn->used = 1; rn->nwk = resp; rn->parent = 0xFFFF; }
        }
        if (rn && (rn->next_hop == 0xFFFF || cost <= rn->route_cost || now - rn->route_at > ROUTE_MS)) {
            rn->next_hop = mac_src; rn->route_cost = cost; rn->route_at = now;
        }
        if (orig == zb.nwk) { flush_pending(resp); return; }
        struct disc *d = disc_find(orig, id);
        if (!d || d->back == 0xFFFF) return;
        uint8_t r[24]; size_t rl = n < 24 ? n : 24; memcpy(r, c, rl); r[7] = cost;
        uint8_t g[ZB_MAX_FRAME];
        /* built anew, one hop on: from us to the one the request came through */
        struct nhdr hh = { .fcf = NF_CMD | PROTO << 2 | 1 << 12, .dst = d->back, .src = zb.nwk, .radius = 1, .seq = zb.nwk_seq++, .src_ieee = zb.ieee };
        size_t m = frame(g, &hh, r, rl, 1);
        if (m) mac_data(d->back, 0, 0, g, m, NULL, NULL);
        return;
    }
    case NC_STATUS:
        if (n >= 4) {
            uint16_t about = rd16(c + 2);
            struct node *d = node_by_nwk(about);
            if (c[1] <= 0x02 || c[1] == 0x0B || c[1] == 0x0C) {
                if (d) { d->next_hop = 0xFFFF; d->nrelay = 0; }
                if (about == 0x0000 && zb.role != R_COORD) zb.mto_next = 0xFFFF;
            }
            LOG("nwk: network status %02x about %04x from %04x", c[1], about, h->src);
            if (c[1] == ST_ADDR_CONFLICT && secured && zb.state == NS_CONNECTED) {
                if (about == zb.nwk && zb.role != R_COORD) timer_at(now + zb_random() % 500, change_address, NULL);
                else if (d && d->child && !(d->cap & 0x08) && d->parent == zb.nwk) child_new_address(d);
            }
        }
        return;
    case NC_NWK_REPORT:
        /* a router heard another network on our PAN ID: the network manager (us) takes it as heard */
        if (n >= 12 && secured && zb.role == R_COORD && (c[1] >> 5) == 0 && rd64(c + 2) == zb.epan) {
            uint16_t other = rd16(c + 10);
            if (other == zb.pan) { pc.count = 3; pc.first = now - 30000; pc.epan = ~zb.epan; nwk_beacon_heard(zb.pan, ~zb.epan); }
        }
        return;
    case NC_NWK_UPDATE:
        /* the network manager's new PAN ID, taken after the broadcast's time */
        if (n >= 13 && secured && zb.role != R_COORD && h->src == 0x0000 && (c[1] >> 5) == 0 && rd64(c + 2) == zb.epan &&
            (uint8_t)(c[10] - zb.update_id) < 0x80 && c[10] != zb.update_id) {
            zb.update_id = c[10];
            new_pan = rd16(c + 11); timer_at(now + BCAST_DELIVERY_MS, pan_change, NULL);
        }
        return;
    case NC_LEAVE: {
        if (n < 2 || !secured) return;
        uint8_t opt = c[1];
        if (opt & 1 << 6) {
            /* a request: we are to leave (the trust center or our parent asks) */
            if (zb.role == R_COORD || (h->dst != zb.nwk && h->dst_ieee != zb.ieee)) return;
            LOG("nwk: asked to leave the network by %04x", h->src);
            nwk_leave(zb.nwk, zb.ieee, 0, 0);
            extern void zb_reset_network(int rejoin);
            zb_reset_network(opt >> 5 & 1);
            return;
        }
        uint64_t ie = h->src_ieee;
        struct node *s = ie ? node_by_ieee(ie) : node_by_nwk(h->src);
        LOG("nwk: %04x (%016llx) left the network%s", h->src, (unsigned long long)(s ? s->ieee : ie), opt & 1 << 5 ? " to rejoin" : "");
        if (s) {
            if (zb.role == R_ROUTER && s->child) aps_update_device(s->ieee, s->nwk, 0x02);   /* device left */
            if (!(opt & 1 << 5)) { s->child = 0; s->neighbor = 0; mac_drop_indirect(s->ieee); }
        }
        return;
    }
    case NC_RREC:
        if (n < 2 || h->dst != zb.nwk || zb.role != R_COORD) return;
        {
            uint8_t cnt = c[1];
            if (cnt > 10 || n < 2u + 2u * cnt) return;
            struct node *s = node_by_nwk(h->src);
            if (!s && h->src_ieee) s = node_add(h->src_ieee, h->src);
            if (!s) return;
            s->nrelay = cnt;
            for (int i = 0; i < cnt; i++) s->relay[i] = rd16(c + 2 + 2 * i);
            if (!cnt) { s->next_hop = 0xFFFF; }
        }
        return;
    case NC_REJOIN_REQ:
        if (n >= 2 && (h->dst == zb.nwk)) rejoin_request(h, mac_ext, src_ext, c[1], secured, ri->lqi);
        return;
    case NC_REJOIN_RSP:
        /* the answer to our rejoin (an end device that lost its parent): our address, perhaps a new one */
        /* or our parent's, unasked: it gave us a new address (an address conflict) */
        if (n >= 4 && secured && zb.role == R_ENDDEV && !c[3] &&
            ((rj.phase == 2 && h->src == rj.to) || (!rj.phase && h->src == zb.parent && h->dst_ieee == zb.ieee))) {
            uint16_t a = rd16(c + 1);
            rj.phase = 0; timer_cancel(rejoin_step, NULL);
            if (a != zb.nwk && a < 0xFFF8) { zb.nwk = a; rcp_set_u16(SP_MAC_SADDR, a); }
            zb.parent = h->src;
            LOG("nwk: rejoined through %04x as %04x", zb.parent, zb.nwk);
            zb_save();
            zdo_device_announce();
        }
        return;
    case NC_LINK_STATUS: {
        if (n < 2 || !secured || zb.role == R_ENDDEV || h->radius != 1 || mac_src >= 0xFFF8) return;
        struct node *s = node_by_nwk(mac_src);
        if (!s) return;
        s->neighbor = 1; s->seen = now; s->lqi = ri->lqi;
        if (!(s->cap & 0x02)) s->cap |= 0x02 | 0x04 | 0x08;  /* a router */
        int cnt = c[1] & 0x1F;
        uint8_t out = 0;
        for (int i = 0; i < cnt && 2u + 3u * i + 3 <= n; i++)
            if (rd16(c + 2 + 3 * i) == zb.nwk) out = c[4 + 3 * i] & 7;   /* the cost it has for us, its incoming */
        s->out_cost = out;
        return;
    }
    case NC_ED_TIMEOUT_REQ: {
        if (n < 3 || !secured || zb.role == R_ENDDEV) return;
        struct node *s = node_by_nwk(h->src);
        if (!s) return;
        uint8_t e = c[1] > 14 ? 14 : c[1];
        s->timeout_s = e == 0 ? 10 : 60u << e;
        s->child = 1; s->seen = now;
        uint8_t r[3] = { NC_ED_TIMEOUT_RSP, 0, 0x03 };    /* success; keepalive by data poll and by this request */
        nwk_send_cmd(h->src, r, 3, 1, 1, 0, 0);
        return;
    }
    default:
        return;
    }
}
