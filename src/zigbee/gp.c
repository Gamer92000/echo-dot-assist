/* A Green Power basic proxy (Zigbee Green Power 1.1.2, A.3), for the router role: battery-less switches (EnOcean's
 * PTM 215Z in Friends of Hue switches, the Hue Tap) send GPDFs (NWK protocol version 3, no association, no network
 * key) that only nodes in their range hear; a proxy tunnels them to the network's sink (Home Assistant's ZHA through
 * zigpy's Green Power sink, zigpy/zgp) as ZCL commands of the Green Power cluster (0x0021, endpoint 242, profile
 * 0xA1E0).  In commissioning mode (the sink's GP Proxy Commissioning Mode, while joining is open in ZHA) the
 * commissioning GPDFs go out as GP Commissioning Notifications; the sink answers with a GP Pairing that puts the GPD
 * into our proxy table; from then on that GPD's GPDFs go out as GP Notifications, groupcast to the sink's group (what
 * zigpy asks for), after their security is checked (CCM* with the GPD's key, A.1.5.4) and their frame counter is
 * newer than the last.  Unidirectional only: we never transmit to a GPD (no gpTxQueue: bidirectional capability 0),
 * so a GPD that needs a Commissioning Reply from a proxy commissions through the sink's own radio or not at all.
 * SrcID GPDs only (application id 0; IEEE-addressed ones are rare). */
#include "zb.h"
#include <stdio.h>
#include <string.h>

#define GP_EP 242
#define GP_PROFILE 0xA1E0
#define GP_CLUSTER 0x0021
#define GP_GROUP 0x0B84                 /* the sink's groupcast group (zigpy GP_GROUP_ID) */
#define C_NOTIFICATION 0x00
#define C_COMM_NOTIFICATION 0x04
#define C_PAIRING 0x01
#define C_PROXY_COMM_MODE 0x02
#define GPD_COMMISSIONING 0xE0
#define GPD_SUCCESS 0xE2
#define GPD_CHANNEL_REQUEST 0xE3
#define PROXY_TABLE 16
#define DUPS 16

static struct entry {
    uint8_t used, sec, key_type, has_key, comm_mode, mac_seq_cap, fc_known;
    uint32_t src_id, fc;
    uint8_t key[16];
    uint16_t group;
} table[PROXY_TABLE];

static struct { int on; int64_t until; uint8_t exit_on_pairing; uint16_t sink; int unicast; } comm;
static struct { uint32_t src_id, fc; int64_t until; } dups[DUPS];
static uint8_t zcl_seq;

static struct entry *find(uint32_t src_id)
{
    for (int i = 0; i < PROXY_TABLE; i++) if (table[i].used && table[i].src_id == src_id) return &table[i];
    return NULL;
}

int gp_entries(void) { int n = 0; for (int i = 0; i < PROXY_TABLE; i++) n += table[i].used; return n; }
int gp_commissioning(void) { return comm.on && now_ms() < comm.until; }

/* the same GPDF heard twice (a GPD repeats each one, often on several channels' worth of tries) */
static int dup_seen(uint32_t src_id, uint32_t fc)
{
    int64_t now = now_ms(); int free_ = 0;
    for (int i = 0; i < DUPS; i++) {
        if (dups[i].until > now && dups[i].src_id == src_id && dups[i].fc == fc) return 1;
        if (dups[i].until < dups[free_].until) free_ = i;
    }
    dups[free_].src_id = src_id; dups[free_].fc = fc; dups[free_].until = now + 2000;
    return 0;
}

/* the GPP-GPD link byte (A.3.3.4.1, Figure 27): RSSI capped to -109..+8 dBm, +110, halved; the link quality in 2 bits */
static uint8_t link_byte(const struct rxinfo *ri)
{
    int r = ri->rssi < -109 ? -109 : ri->rssi > 8 ? 8 : ri->rssi;
    int q = ri->lqi >= 200 ? 3 : ri->lqi >= 150 ? 2 : ri->lqi >= 100 ? 1 : 0;
    return (uint8_t)(((r + 110) / 2 & 0x3F) | q << 6);
}

static void send_zcl(uint8_t cmd, const uint8_t *pl, size_t n, uint16_t group, uint16_t to)
{
    uint8_t z[120];
    if (n + 3 > sizeof z) return;
    z[0] = 0x11;                        /* cluster specific, client to server, no default response */
    z[1] = zcl_seq++; z[2] = cmd;
    memcpy(z + 3, pl, n);
    if (group) aps_send(1, group, 0, GP_EP, GP_EP, GP_PROFILE, GP_CLUSTER, z, n + 3, 0, 0, NULL, 0, NULL, NULL);
    else aps_send(2, to, 0, GP_EP, GP_EP, GP_PROFILE, GP_CLUSTER, z, n + 3, 0, 0, NULL, 0, NULL, NULL);
}

/* CCM* of a SrcID GPD's GPDF (A.1.5.4): nonce srcID | srcID | frame counter | 0x05; level 2 authenticates header
 * and payload (the MIC alone), level 3 encrypts the payload (command id and its bytes).  0: holds, in place */
/* a GPD's key: its own from the pairing, or the network key as it is now (key type 1: never a copy, which a new
 * network key would leave behind) */
static const uint8_t *gp_key(const struct entry *e)
{
    if (e->has_key) return e->key;
    return e->key_type == 1 ? zb.nwk_key : NULL;
}

/* so many frames a second, a burst of as many: 1 taken */
struct bucket { int milli; int64_t at; };
static int take(struct bucket *b, int per_s)
{
    int64_t now = now_ms();
    int64_t add = (now - b->at) * per_s;
    b->at = now;
    b->milli = (int)(b->milli + add > per_s * 1000LL ? per_s * 1000LL : b->milli + add);
    if (b->milli < 1000) return 0;
    b->milli -= 1000;
    return 1;
}
static struct bucket comm_bucket, all_bucket;

static int unseal(const uint8_t *key, int level, const uint8_t *hdr, size_t hl, uint8_t *pl, size_t pn, uint32_t src_id, uint32_t fc)
{
    uint8_t nonce[13], a[127], m[127];
    wr32(nonce, src_id); wr32(nonce + 4, src_id); wr32(nonce + 8, fc); nonce[12] = 0x05;
    if (pn < 4 || hl + pn > sizeof a) return -1;
    if (level == 2) {
        memcpy(a, hdr, hl); memcpy(a + hl, pl, pn - 4);
        memcpy(m, pl + pn - 4, 4);
        return ccm_decrypt(key, nonce, a, hl + pn - 4, m, 4);
    }
    memcpy(m, pl, pn);                  /* decrypted in a copy: one that does not hold is forwarded as heard */
    if (ccm_decrypt(key, nonce, hdr, hl, m, pn)) return -1;
    memcpy(pl, m, pn - 4);
    return 0;
}

/* a GPDF heard (nwk.c: protocol version 3).  f: from the NWK frame control on; mac_seq: the MAC's sequence number
 * (an unprotected GPD's counter) */
void gp_rx(const uint8_t *f, size_t n, uint8_t mac_seq, const struct rxinfo *ri)
{
    if (zb.role != R_ROUTER || zb.state != NS_CONNECTED || n < 2) return;
    uint8_t fc0 = f[0];
    int type = fc0 & 3, auto_comm = fc0 >> 6 & 1, ext = fc0 >> 7 & 1;
    if ((fc0 >> 2 & 0x0F) != 3 || type > 1) return;
    uint8_t xfc = 0;
    size_t k = 1;
    if (ext) xfc = f[k++];
    int app = xfc & 7, level = xfc >> 3 & 3, key_bit = xfc >> 5 & 1, rx_after = xfc >> 6 & 1, dir = xfc >> 7 & 1;
    if (dir || app != 0 || level == 1) return;      /* to a GPD; not SrcID; reserved */
    if (type == 1 && !ext) return;                  /* a maintenance frame without a SrcID */
    if (k + 4 > n) return;
    uint32_t src_id = rd32(f + k); k += 4;
    uint32_t fc = mac_seq;
    if (level >= 2) { if (k + 4 > n) return; fc = rd32(f + k); k += 4; }
    size_t hl = k;                      /* the header the MIC covers */
    if (k + 1 + (level >= 2 ? 4 : 0) > n) return;
    uint8_t pl[127]; size_t pn = n - k;
    memcpy(pl, f + k, pn);
    struct entry *e = find(src_id);
    int in_comm = gp_commissioning();
    /* A paired GPD's frames at its pairing's security level and kind of key only (A.3.5.2.4): another is a forgery
     * (a level 0 frame in a protected GPD's name) or, in commissioning mode, the GPD commissioning anew */
    if (e && (level != e->sec || (level >= 2 && key_bit != (e->key_type == 4 || e->key_type == 7)))) {
        if (!in_comm) return;
        e = NULL;
    }
    /* its security before anything is read of it or remembered (level 3: the command itself is encrypted) */
    int failed = 0;
    if (level >= 2) {
        const uint8_t *key = e ? gp_key(e) : NULL;
        if (key && !unseal(key, level, f, hl, pl, pn, src_id, fc)) pn -= 4;
        else if (in_comm && level == 2) failed = 1; /* forwarded as heard, its MIC along: the sink checks it */
        else {
            static int64_t said;
            if (e && now_ms() - said > 10000) { said = now_ms(); LOG("gp: a GPDF of %08x that did not hold: dropped", src_id); }
            return;
        }
    }
    uint8_t cmd = pl[0];
    int commissioning = in_comm && (cmd == GPD_COMMISSIONING || cmd == GPD_SUCCESS || cmd == GPD_CHANNEL_REQUEST || auto_comm);
    if (!commissioning && (!e || failed)) return;   /* operational: only the paired GPDs' frames that held */
    if (dup_seen(src_id, fc)) return;               /* a GPD repeats each frame: once (after the check: a forged
                                                     * one with its counter never hides the real one) */
    /* bounds on what goes out (each a broadcast in the network): commissioning frames 4 a second, all 20 */
    if (commissioning ? !take(&comm_bucket, 4) : !take(&all_bucket, 20)) return;
    if (e && !failed && !commissioning) {
        /* a replay: a protected GPD's counter only grows; an unprotected one's MAC sequence number (where it says it
         * counts) wraps at 256 */
        if (e->fc_known && (level >= 2 ? fc <= e->fc : e->mac_seq_cap && (uint8_t)(fc - e->fc) - 1u >= 0x7F)) return;
        if (level >= 2 || e->mac_seq_cap) { e->fc = fc; e->fc_known = 1; }
    }
    uint8_t key_type = e ? e->key_type : (uint8_t)(key_bit ? 4 : 0);
    uint8_t o[127], *p = o;
    size_t cl = failed ? pn - 4 : pn;   /* the command id and its payload (the MIC apart) */
    if (cl < 1 || cl - 1 > 64) return;
    if (commissioning) {
        /* GP Commissioning Notification (A.3.3.4.3): options, SrcID, counter, command, payload, our address and
         * link, the MIC where its check failed here */
        uint16_t opt = (uint16_t)(app | rx_after << 3 | level << 4 | key_type << 6 | failed << 9 | 0 << 10 | 1 << 11);
        p = wr16(p, opt); p = wr32(p, src_id); p = wr32(p, fc);
        *p++ = cmd; *p++ = (uint8_t)(cl - 1); memcpy(p, pl + 1, cl - 1); p += cl - 1;
        p = wr16(p, zb.nwk); *p++ = link_byte(ri);
        if (failed) { memcpy(p, pl + pn - 4, 4); p += 4; }
        LOG("gp: commissioning GPDF %02x of %08x: to the sink", cmd, src_id);
        send_zcl(C_COMM_NOTIFICATION, o, (size_t)(p - o), 0, comm.unicast ? comm.sink : NWK_BCAST_RXON);
        return;
    }
    /* GP Notification (A.3.3.4.1): to the sink's group, as the pairing asked (or the sink itself) */
    uint16_t opt = (uint16_t)(app | (e->comm_mode == 2 ? 1 << 5 : e->comm_mode == 1 ? 1 << 4 : 1 << 3) | level << 6 |
                              e->key_type << 8 | rx_after << 11 | 0 << 12 | 0 << 13 | 1 << 14);
    p = wr16(p, opt); p = wr32(p, src_id); p = wr32(p, fc);
    *p++ = cmd; *p++ = (uint8_t)(cl - 1); memcpy(p, pl + 1, cl - 1); p += cl - 1;
    p = wr16(p, zb.nwk); *p++ = link_byte(ri);
    send_zcl(C_NOTIFICATION, o, (size_t)(p - o), e->comm_mode == 1 || e->comm_mode == 2 ? e->group : 0, 0x0000);
}

static void comm_end(void *unused) { (void)unused; if (comm.on) LOG("gp: commissioning mode over"); comm.on = 0; }

/* the sink's commands to proxies (server to client, endpoint 242): commissioning mode, pairing */
void gp_zcl_rx(const struct apsind *in)
{
    if (zb.role != R_ROUTER || in->n < 3) return;
    const uint8_t *d = in->data;
    if ((d[0] & 0x07) != 0x01 || !(d[0] & 0x08)) return;   /* cluster specific, server to client, no manufacturer */
    uint8_t cmd = d[2];
    const uint8_t *c = d + 3; size_t n = in->n - 3;
    if (cmd == C_PROXY_COMM_MODE) {
        if (n < 1) return;
        uint8_t opt = c[0];
        if (!(opt & 1)) { comm.on = 0; timer_cancel(comm_end, NULL); LOG("gp: commissioning mode left (the sink)"); return; }
        uint16_t window = 180;
        if (opt & 2) { if (n < 3) return; window = rd16(c + 1); }
        comm.on = 1; comm.until = now_ms() + window * 1000LL; comm.exit_on_pairing = opt >> 2 & 1;
        comm.sink = in->src; comm.unicast = opt >> 5 & 1;
        timer_at(comm.until, comm_end, NULL);
        LOG("gp: commissioning mode for %u s (sink %04x)", window, in->src);
        return;
    }
    if (cmd != C_PAIRING || n < 3 + 4) return;
    uint32_t opt = (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16;
    int app = opt & 7, add = opt >> 3 & 1, remove = opt >> 4 & 1, mode = opt >> 5 & 3, mac_seq = opt >> 8 & 1;
    int level = opt >> 9 & 3, key_type = opt >> 11 & 7, fc_present = opt >> 14 & 1, key_present = opt >> 15 & 1;
    int alias = opt >> 16 & 1, radius = opt >> 17 & 1;
    if (app != 0) return;
    size_t k = 3;
    uint32_t src_id = rd32(c + k); k += 4;
    uint16_t group = 0;
    if (!remove && (mode == 0 || mode == 3)) { if (k + 10 > n) return; k += 10; }       /* the sink's EUI-64 and address */
    if (!remove && (mode == 1 || mode == 2)) { if (k + 2 > n) return; group = rd16(c + k); k += 2; }
    struct entry *e = find(src_id);
    if (remove || !add) {
        if (e) { e->used = 0; LOG("gp: %08x out of the proxy table", src_id); }
        return;
    }
    if (k + 1 > n) return;
    k++;                                /* the device id */
    uint32_t fc = 0; uint8_t key[16]; int has_key = 0;
    if (fc_present) { if (k + 4 > n) return; fc = rd32(c + k); k += 4; }
    if (key_present) { if (k + 16 > n) return; memcpy(key, c + k, 16); k += 16; has_key = 1; }
    if (alias) k += 2;
    if (radius) k += 1;
    if (k > n) return;
    if (!e) for (int i = 0; i < PROXY_TABLE && !e; i++) if (!table[i].used) e = &table[i];
    if (!e) { LOG("gp: the proxy table is full: %08x not taken", src_id); return; }
    memset(e, 0, sizeof *e);
    e->used = 1; e->src_id = src_id; e->sec = (uint8_t)level; e->key_type = (uint8_t)key_type; e->comm_mode = (uint8_t)mode;
    e->mac_seq_cap = (uint8_t)mac_seq; e->fc = fc; e->fc_known = (uint8_t)fc_present; e->group = group ? group : GP_GROUP;
    if (has_key) { memcpy(e->key, key, 16); e->has_key = 1; }   /* key type 1 without one: the network key as it is (gp_key) */
    LOG("gp: %08x paired (security level %d, key type %d%s)", src_id, level, key_type, e->has_key ? ", its key" : "");
    if (comm.on && comm.exit_on_pairing) comm_end(NULL);
    zb_save_soon();
}

/* the proxy table in the state file: one line an entry */
void gp_save(void *file)
{
    for (int i = 0; i < PROXY_TABLE; i++) {
        struct entry *e = &table[i];
        if (!e->used) continue;
        char hx[33];
        for (int j = 0; j < 16; j++) { static const char h[] = "0123456789abcdef"; hx[2 * j] = h[e->key[j] >> 4]; hx[2 * j + 1] = h[e->key[j] & 15]; }
        hx[32] = 0;
        fprintf((FILE *)file, "gp %08x %u %u %u %u %u %u %04x %s %u\n", e->src_id, e->sec, e->key_type, e->has_key, e->comm_mode, e->mac_seq_cap, e->fc, e->group, hx, e->fc_known);
    }
}

void gp_load(uint32_t src_id, unsigned sec, unsigned key_type, unsigned has_key, unsigned mode, unsigned mac_seq, uint32_t fc, unsigned group, const uint8_t key[16], unsigned fc_known)
{
    struct entry *e = find(src_id);
    if (!e) for (int i = 0; i < PROXY_TABLE && !e; i++) if (!table[i].used) e = &table[i];
    if (!e) return;
    e->used = 1; e->src_id = src_id; e->sec = (uint8_t)sec; e->key_type = (uint8_t)key_type; e->has_key = (uint8_t)!!has_key;
    e->comm_mode = (uint8_t)mode; e->mac_seq_cap = (uint8_t)!!mac_seq; e->fc = fc; e->group = (uint16_t)group; e->fc_known = (uint8_t)!!fc_known;
    memcpy(e->key, key, 16);
}

void gp_reset(void) { memset(table, 0, sizeof table); comm.on = 0; }
