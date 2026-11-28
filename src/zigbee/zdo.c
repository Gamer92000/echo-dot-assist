/* The ZDO server (Zigbee spec 2.4): what every Zigbee node answers about itself (addresses, node, power and simple
 * descriptors, active endpoints), for its sleeping children (their addresses), the network manager's requests
 * (neighbors, routes, permit joining, energy scan and channel change, leave, system server discovery); and, as a router
 * or end device, a ZCL Basic cluster on endpoint 1 (manufacturer, model, power source: what Home Assistant's
 * interview reads).  As the coordinator the host's endpoints are what deCONZ's "configure endpoint" slots hold, and
 * every ZDO frame for us goes to the host as well (deconz.c): it answers match descriptor requests itself. */
#include "zb.h"
#include <string.h>

#define ST_SUCCESS 0x00
#define ST_INV_REQUESTTYPE 0x80
#define ST_DEVICE_NOT_FOUND 0x81
#define ST_INVALID_EP 0x82
#define ST_NOT_ACTIVE 0x83
#define ST_NOT_SUPPORTED 0x84

static const uint8_t router_ep1[] = {   /* endpoint 1, Home Automation, range extender, Basic and Identify in */
    1, 0x04, 0x01, 0x08, 0x00, 0x00, 2, 0x00, 0x00, 0x03, 0x00, 0
};

static const uint8_t router_ep242[] = {  /* endpoint 242, Green Power, a basic proxy: the GP cluster's client */
    242, 0xE0, 0xA1, 0x61, 0x00, 0x00, 0, 1, 0x21, 0x00
};

static int nep(void)
{
    if (zb.role == R_ROUTER) return 2;
    if (zb.role != R_COORD) return 1;
    int n = 0;
    for (int i = 0; i < ZB_EP_SLOTS; i++) n += zb.ep_len[i] > 0;
    return n;
}

static const uint8_t *ep_desc(uint8_t ep, size_t *len)
{
    if (zb.role == R_ROUTER && ep == 242) { *len = sizeof router_ep242; return router_ep242; }
    if (zb.role != R_COORD) { if (ep != 1) return NULL; *len = sizeof router_ep1; return router_ep1; }
    for (int i = 0; i < ZB_EP_SLOTS; i++) if (zb.ep_len[i] && zb.ep[i][0] == ep) { *len = zb.ep_len[i]; return zb.ep[i]; }
    return NULL;
}

static uint8_t *node_desc(uint8_t *p)
{
    int coord = zb.role == R_COORD, ed = zb.role == R_ENDDEV;
    *p++ = coord ? 0 : ed ? 2 : 1;
    *p++ = 0x40;                        /* 2.4 GHz */
    *p++ = coord ? 0x8F : ed ? 0x80 : 0x8E;
    p = wr16(p, coord ? 0x1135 : 0x0000);   /* the coordinator as a ConBee (dresden elektronik): what the host expects */
    *p++ = ZB_MAX_FRAME - 45;           /* max buffer */
    p = wr16(p, 1024);                  /* max incoming transfer: fragmented (aps.c) */
    p = wr16(p, (uint16_t)(22 << 9 | (coord ? 0x0041 : 0)));   /* stack revision 22; primary TC, network manager */
    p = wr16(p, 1024);                  /* max outgoing transfer */
    *p++ = 0;
    return p;
}

static uint8_t ed_value(int8_t rssi)
{
    /* RSSI to Zigbee's energy (0..255, -100..-20 dBm), a third of it: the host reads the coordinator as a ConBee II,
     * whose values it multiplies by 3 (zigpy-deconz energy_scan) */
    int e = rssi == 127 ? 0 : (rssi + 100) * 255 / 80;
    if (e < 0) e = 0;
    if (e > 255) e = 255;
    return (uint8_t)(e / 3);
}

static int new_channel;                 /* the one a channel change moves to (one timer, whatever asks) */
static void channel_change(void *unused)
{
    (void)unused;
    int ch = new_channel;
    LOG("zdo: moving to channel %d", ch);
    zb.channel = ch;
    rcp_set_u8(SP_PHY_CHAN, (uint8_t)ch);
    zb_save();
}

static void leave_now(void *unused)
{
    (void)unused;
    extern void zb_reset_network(int rejoin);
    nwk_leave(zb.nwk, zb.ieee, 0, 0);
    zb_reset_network(0);
}

/* an energy scan's answer, once the scan ended (spinel.c runs it from timers): to the host (its own request, through
 * the loopback) or to the network manager that asked */
static struct { int busy; uint16_t to; uint8_t tsn; uint32_t ch; int host; int64_t last, gap, began; } es;

static void scan_done(int st, const int8_t *e, void *unused)
{
    (void)unused;
    uint8_t r[40], *p = r;
    es.busy = 0;
    /* the network's next scan after a pause five times as long as this one took, a minute at least: scans hold
     * the radio (nothing sent, our channel not heard) */
    if (!es.host) { es.last = now_ms(); es.gap = 5 * (es.last - es.began); if (es.gap < 60000) es.gap = 60000; }
    *p++ = es.tsn;
    if (st) { *p++ = ST_NOT_SUPPORTED; p = wr32(p, es.ch); p = wr16(p, 0); p = wr16(p, 0); *p++ = 0; }
    else {
        *p++ = ST_SUCCESS; p = wr32(p, es.ch); p = wr16(p, (uint16_t)(zb.tx_ok + zb.tx_fail)); p = wr16(p, (uint16_t)zb.tx_fail);
        uint8_t *cp = p++, k = 0;
        for (int c = 11; c <= 26; c++) if (es.ch & 1u << c) { *p++ = ed_value(e[c]); k++; }
        *cp = k;
    }
    if (es.host) {
        struct apsind a = { .src = zb.nwk, .src_ieee = zb.ieee, .dst = zb.nwk, .dst_mode = 2, .profile = 0, .cluster = 0x8038,
                            .data = r, .n = (size_t)(p - r), .lqi = 255 };
        if (zb.role == R_COORD) dz_indication(&a);
    } else aps_send(2, es.to, 0, 0, 0, 0x0000, 0x8038, r, (size_t)(p - r), 0, 0, NULL, 0, NULL, NULL);
}

int zdo_local(const struct apsind *in, uint8_t *r, size_t *rn, uint16_t *rc)
{
    const uint8_t *d = in->data;
    size_t n = in->n;
    uint8_t *p = r;
    int bcast = in->dst >= 0xFFF8;
    if (n < 1) return 0;
    *p++ = d[0];                        /* the request's sequence number */
    *rc = (uint16_t)(in->cluster | 0x8000);
    switch (in->cluster) {
    case 0x0000: {                      /* NWK_addr_req */
        if (n < 10) return 0;
        uint64_t ie = rd64(d + 1);
        struct node *c = node_by_ieee(ie);
        if (ie != zb.ieee && !(c && c->child)) return 0;
        *p++ = ST_SUCCESS; p = wr64(p, ie); p = wr16(p, ie == zb.ieee ? zb.nwk : c->nwk);
        if (d[9] == 1) { *p++ = 0; *p++ = 0; }
        break;
    }
    case 0x0001: {                      /* IEEE_addr_req */
        if (n < 4) return 0;
        uint16_t a = rd16(d + 1);
        struct node *c = node_by_nwk(a);
        if (a != zb.nwk && !(c && c->child)) return 0;
        *p++ = ST_SUCCESS; p = wr64(p, a == zb.nwk ? zb.ieee : c->ieee); p = wr16(p, a);
        if (d[3] == 1) { *p++ = 0; *p++ = 0; }
        break;
    }
    case 0x0002:                        /* Node_Desc_req */
        if (n < 3 || rd16(d + 1) != zb.nwk) return 0;
        *p++ = ST_SUCCESS; p = wr16(p, zb.nwk); p = node_desc(p);
        break;
    case 0x0003:                        /* Power_Desc_req: receiver on, mains, full */
        if (n < 3 || rd16(d + 1) != zb.nwk) return 0;
        *p++ = ST_SUCCESS; p = wr16(p, zb.nwk); *p++ = 0x10; *p++ = 0xC1;
        break;
    case 0x0004: {                      /* Simple_Desc_req */
        if (n < 4 || rd16(d + 1) != zb.nwk) return 0;
        size_t l; const uint8_t *e = ep_desc(d[3], &l);
        if (!e) { *p++ = ST_NOT_ACTIVE; p = wr16(p, zb.nwk); *p++ = 0; break; }
        *p++ = ST_SUCCESS; p = wr16(p, zb.nwk); *p++ = (uint8_t)l; memcpy(p, e, l); p += l;
        break;
    }
    case 0x0005:                        /* Active_EP_req */
        if (n < 3 || rd16(d + 1) != zb.nwk) return 0;
        *p++ = ST_SUCCESS; p = wr16(p, zb.nwk); *p++ = (uint8_t)nep();
        if (zb.role != R_COORD) { *p++ = 1; if (zb.role == R_ROUTER) *p++ = 242; }
        else for (int i = 0; i < ZB_EP_SLOTS; i++) if (zb.ep_len[i]) *p++ = zb.ep[i][0];
        break;
    case 0x0006: {                      /* Match_Desc_req: the coordinator's host answers; a router for its endpoint 1 */
        if (zb.role == R_COORD || n < 6) return 0;
        uint16_t prof = rd16(d + 3);
        size_t k = 5; int hit = 0;
        for (int pass = 0; pass < 2 && k < n; pass++) {
            uint8_t cnt = d[k++];
            for (int i = 0; i < cnt && k + 2 <= n; i++, k += 2) { uint16_t cl = rd16(d + k); if (!pass && (cl == 0x0000 || cl == 0x0003)) hit = 1; }
        }
        if (prof != 0x0104 || !hit) { if (bcast) return 0; *p++ = ST_SUCCESS; p = wr16(p, zb.nwk); *p++ = 0; break; }
        *p++ = ST_SUCCESS; p = wr16(p, zb.nwk); *p++ = 1; *p++ = 1;
        break;
    }
    case 0x0013:                        /* Device_annce: an address learnt */
        if (n >= 12) nwk_device_announced(rd16(d + 1), rd64(d + 3), d[11]);
        return 0;
    case 0x0015:                        /* System_Server_Discovery_req: the coordinator is the trust center */
        if (n < 3 || zb.role != R_COORD) return 0;
        *p++ = ST_SUCCESS; p = wr16(p, (uint16_t)(rd16(d + 1) & 0x0041));
        break;
    case 0x0031: {                      /* Mgmt_Lqi_req: neighbors and children, three a time */
        if (n < 2 || bcast) return 0;
        struct node *list[ZB_MAX_NODES]; int total = 0;
        for (int i = 0; i < zb.nnodes; i++) {
            struct node *x = &zb.nodes[i];
            if (x->used && (x->child || (x->neighbor && now_ms() - x->seen < 60000))) list[total++] = x;
        }
        uint8_t start = d[1], cnt = 0;
        *p++ = ST_SUCCESS; *p++ = (uint8_t)total; *p++ = start;
        uint8_t *cp = p++;
        for (int i = start; i < total && cnt < 3; i++, cnt++) {
            struct node *x = list[i];
            int dtype = x->nwk == 0x0000 ? 0 : (x->cap & 0x02) ? 1 : 2;
            p = wr64(p, zb.epan); p = wr64(p, x->ieee); p = wr16(p, x->nwk);
            *p++ = (uint8_t)(dtype | ((x->cap & 0x08) ? 1 : 0) << 2 | (x->child ? 1 : 2) << 4);
            *p++ = 2;                   /* permit joining: unknown */
            *p++ = x->child ? (uint8_t)(zb.depth + 1) : 0x0F;
            *p++ = x->lqi;
        }
        *cp = cnt;
        break;
    }
    case 0x0032: {                      /* Mgmt_Rtg_req: the routes we hold, ten a time */
        if (n < 2 || bcast) return 0;
        struct node *list[ZB_MAX_NODES]; int total = 0;
        for (int i = 0; i < zb.nnodes; i++) if (zb.nodes[i].used && zb.nodes[i].next_hop != 0xFFFF) list[total++] = &zb.nodes[i];
        uint8_t start = d[1], cnt = 0;
        *p++ = ST_SUCCESS; *p++ = (uint8_t)total; *p++ = start;
        uint8_t *cp = p++;
        for (int i = start; i < total && cnt < 10; i++, cnt++) { p = wr16(p, list[i]->nwk); *p++ = 0; p = wr16(p, list[i]->next_hop); }
        *cp = cnt;
        break;
    }
    case 0x0034: {                      /* Mgmt_Leave_req: ourselves (a router), the coordinator never */
        if (n < 10 || bcast) return 0;
        uint64_t ie = rd64(d + 1);
        if (ie && ie != zb.ieee) { *p++ = ST_NOT_SUPPORTED; break; }
        if (zb.role == R_COORD) { *p++ = ST_NOT_SUPPORTED; break; }
        *p++ = ST_SUCCESS;
        LOG("zdo: asked to leave the network");
        timer_at(now_ms() + 600, leave_now, NULL);
        break;
    }
    case 0x0036: {                      /* Mgmt_Permit_Joining_req */
        if (n < 3) return 0;
        uint8_t dur = d[1];
        if (dur == 0xFF) dur = 0xFE;    /* R21: never for good (anyone in the network could open it for ever) */
        int64_t until = dur ? now_ms() + dur * 1000LL : 0;
        zb.permit_until = until;
        if (until > zb.net_permit_until || !dur) zb.net_permit_until = until;
        LOG("zdo: joining %s", dur ? "open" : "closed");
        if (bcast) return 0;
        *p++ = ST_SUCCESS;
        break;
    }
    case 0x0038: {                      /* Mgmt_NWK_Update_req */
        if (n < 6) return 0;
        uint32_t ch = rd32(d + 1);
        uint8_t dur = d[5];
        if (dur <= 5) {
            /* a scan: unicast only (a broadcast one is not valid, and would hold every router's radio at once); the
             * network's own requests one per 10 s (each holds our radio for up to 16 channels' time); the answer
             * comes at the scan's end (scan_done) */
            int host = in->src == zb.nwk;
            int ms = (int)(((1 << dur) + 1) * 15360 / 1000);
            if (bcast) return 0;
            int64_t now = now_ms();
            if (zb.role == R_ENDDEV || es.busy || (!host && es.last && now - es.last < es.gap) ||
                rcp_energy_scan_async(ch, ms, scan_done, NULL)) {
                /* the whole notify all the same, nothing in it: hosts read its fields whatever the status */
                *p++ = ST_NOT_SUPPORTED; p = wr32(p, ch); p = wr16(p, 0); p = wr16(p, 0); *p++ = 0;
                break;
            }
            es.busy = 1; es.to = in->src; es.tsn = d[0]; es.ch = ch; es.host = host; es.began = now;
            return 0;
        }
        /* the network manager's (the coordinator: its host's own request, looped back; elsewhere 0x0000's) */
        int manager = zb.role == R_COORD ? in->src == zb.nwk : in->src == 0x0000;
        if (!manager) return 0;
        if (dur == 0xFE) {              /* channel change, at nwkBroadcastDeliveryTime */
            int c;
            for (c = 11; c <= 26 && !(ch & 1u << c); c++) ;
            if (c <= 26 && n >= 7) { zb.update_id = d[6]; new_channel = c; timer_at(now_ms() + 9000, channel_change, NULL); }
            return 0;
        }
        if (dur == 0xFF && n >= 7) { zb.update_id = d[6]; zb.channel_mask = ch; }
        return 0;
    }
    default:
        /* a request we do not serve: a router says so (unicast); the coordinator's host may answer it */
        if (zb.role == R_COORD || bcast || in->cluster & 0x8000) return 0;
        *p++ = ST_NOT_SUPPORTED;
        break;
    }
    *rn = (size_t)(p - r);
    return 1;
}

void zdo_rx(const struct apsind *in)
{
    uint8_t r[100]; size_t rn = 0; uint16_t rc;
    if (in->cluster & 0x8000) return;   /* a response: the host's (coordinator), nobody's else */
    if (zdo_local(in, r, &rn, &rc) && in->src != zb.nwk)
        aps_send(2, in->src, 0, 0, 0, 0x0000, rc, r, rn, 0, 0, NULL, 0, NULL, NULL);
}

void zdo_device_announce(void)
{
    uint8_t a[12];
    a[0] = zb.zdo_seq++;
    wr16(a + 1, zb.nwk); wr64(a + 3, zb.ieee);
    a[11] = zb.role == R_ROUTER ? 0x8E : 0x80;
    aps_send(2, NWK_BCAST_RXON, 0, 0, 0, 0x0000, 0x0013, a, 12, 0, 0, NULL, 0, NULL, NULL);
}

/* ---------------------------------------------------------------- ZCL: Basic on endpoint 1 (router, end device) */

static uint8_t *zstr(uint8_t *p, const char *s) { size_t l = strlen(s); *p++ = 0x42; *p++ = (uint8_t)l; memcpy(p, s, l); return p + l; }

void zcl_rx(const struct apsind *in)
{
    if (in->dst_ep == 242 && in->profile == 0xA1E0 && in->cluster == 0x0021) { gp_zcl_rx(in); return; }   /* the sink's (gp.c) */
    if (in->dst_ep != 1 || in->profile != 0x0104 || in->n < 3 || in->dst_mode != 2 || in->dst >= 0xFFF8) return;
    const uint8_t *d = in->data;
    uint8_t fc = d[0];
    size_t k = (fc & 0x04) ? 3 : 1;     /* a manufacturer code */
    if (k + 2 > in->n) return;
    uint8_t tsn = d[k], cmd = d[k + 1];
    k += 2;
    uint8_t r[200], *p = r;             /* more than a frame holds: aps.c fragments a long answer */
    int cluster_specific = fc & 1, from_server = fc >> 3 & 1;
    if (from_server) return;
    if (!cluster_specific && cmd == 0x00 && in->cluster == 0x0000) {
        *p++ = 0x18; *p++ = tsn; *p++ = 0x01;          /* read attributes response */
        for (; k + 2 <= in->n && (size_t)(p - r) + 2 + 1 + 2 + 24 <= sizeof r; k += 2) {   /* room for the longest */
            uint16_t a = rd16(d + k);
            p = wr16(p, a);
            switch (a) {
            case 0x0000: *p++ = 0; *p++ = 0x20; *p++ = 8; break;                /* ZCL version 8 */
            case 0x0001: *p++ = 0; *p++ = 0x20; *p++ = 1; break;
            case 0x0002: *p++ = 0; *p++ = 0x20; *p++ = 22; break;
            case 0x0003: *p++ = 0; *p++ = 0x20; *p++ = 1; break;
            case 0x0004: *p++ = 0; p = zstr(p, "hassmic"); break;
            case 0x0005: *p++ = 0; p = zstr(p, zb.role == R_ROUTER ? "Echo 4 Zigbee router" : "hassmic end device"); break;
            case 0x0006: *p++ = 0; p = zstr(p, "20261127"); break;
            case 0x0007: *p++ = 0; *p++ = 0x30; *p++ = zb.role == R_ROUTER ? 0x01 : 0x03; break;   /* mains, battery */
            case 0x4000: *p++ = 0; p = zstr(p, "zigbeed 1"); break;
            default: *p++ = 0x86; break;                                   /* unsupported attribute */
            }
        }
    } else if (!cluster_specific && cmd == 0x0C && in->cluster == 0x0000) {
        /* discover attributes: what read answers */
        static const uint16_t attrs[] = { 0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x4000 };
        static const uint8_t types[] = { 0x20, 0x20, 0x20, 0x20, 0x42, 0x42, 0x42, 0x30, 0x42 };
        *p++ = 0x18; *p++ = tsn; *p++ = 0x0D; *p++ = 1;
        for (unsigned i = 0; i < sizeof attrs / sizeof *attrs; i++) { p = wr16(p, attrs[i]); *p++ = types[i]; }
    } else if (in->cluster == 0x0003 && cluster_specific && cmd == 0x01) {
        *p++ = 0x19; *p++ = tsn; *p++ = 0x00; p = wr16(p, 0);   /* identify query: not identifying */
    } else {
        if (fc & 0x10) return;          /* no default response wanted */
        *p++ = 0x18; *p++ = tsn; *p++ = 0x0B; *p++ = cmd;
        *p++ = in->cluster == 0x0000 || in->cluster == 0x0003 ? (cluster_specific && in->cluster == 0x0003 ? 0x00 : 0x82) : 0xC3;
    }
    aps_send(2, in->src, 0, in->src_ep, 1, 0x0104, in->cluster, r, (size_t)(p - r), 0, 0, NULL, 0, NULL, NULL);
}
