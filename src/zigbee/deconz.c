/* The coordinator's host interface: dresden elektronik's deCONZ serial protocol (the ConBee's), which Home Assistant's
 * ZHA (zigpy-deconz) and Zigbee2MQTT (zigbee-herdsman's deconz adapter) both speak.  Frames in SLIP (0xC0 ends one,
 * 0xDB escapes) with a two's-complement 16-bit sum after them; each is command, sequence number, status, frame length
 * and the command's payload, little-endian.  Formats as zigpy-deconz's COMMAND_SCHEMAS and NETWORK_PARAMETER_TYPES
 * have them (api.py), the firmware version a ConBee II's (platform 0x07), protocol version 0x010C.  Indications and
 * confirms wait in queues for the host to fetch them, as the firmware's do: the device state's flags say what waits,
 * and "device state changed" tells the host when that changes.  One host at a time. */
#define _GNU_SOURCE
#include "zb.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define C_APS_CONFIRM 0x04
#define C_DEVICE_STATE 0x07
#define C_CHANGE_STATE 0x08
#define C_READ_PARAM 0x0A
#define C_WRITE_PARAM 0x0B
#define C_VERSION 0x0D
#define C_STATE_CHANGED 0x0E
#define C_APS_REQUEST 0x12
#define C_APS_INDICATION 0x17
#define C_GREEN_POWER 0x19
#define C_UPDATE_NEIGHBOR 0x1D
#define C_MAC_POLL 0x1C
#define S_SUCCESS 0
#define S_FAILURE 1
#define S_BUSY 2
#define S_UNSUPPORTED 4
#define S_ERROR 5
#define S_INVALID 7
#define FW_VERSION 0x26780700u          /* 0x26780700: major 0x26, minor 0x78, platform 0x07 (ConBee II), reserved */
#define PROTOCOL 0x010C
#define INDQ 32
#define CONFQ 16
#define INFLIGHT 16

int dz_client = -1;
static uint8_t rx[4096]; static size_t rxn;
static struct indq { uint8_t b[1100]; uint16_t n; } inds[INDQ];   /* up to a fragmented ASDU's 1024 bytes */
static int indh, indn;
static struct conf { uint8_t req_id, mode, ep, src_ep, status; uint16_t nwk; uint64_t ieee; } confs[CONFQ];
static int confh, confn, inflight;
static uint8_t last_state = 0xFF;
int dz_want_connected;                   /* the host's last CHANGE_NETWORK_STATE: the network runs over restarts */
extern int zb_form_network(void);
extern void zb_stop_network(void);
extern void zb_params_changed(void);

static int64_t watchdog_until;          /* ms: the host's watchdog runs out; 0 none set */

static void host_gone(void);
static void dz_watchdog(void *unused)
{
    (void)unused;
    if (!watchdog_until || dz_client < 0) return;
    if (now_ms() < watchdog_until) { timer_at(watchdog_until + 100, dz_watchdog, NULL); return; }
    LOG("deconz: the host's watchdog ran out: its connection ends (a ConBee would reset)");
    close(dz_client);
    host_gone();
}

static uint8_t devstate(void)
{
    uint8_t s = zb.state == NS_CONNECTED ? 2 : zb.state == NS_JOINING ? 1 : 0;
    if (confn) s |= 0x04;
    if (indn) s |= 0x08;
    if (inflight < INFLIGHT) s |= 0x20;
    return s;
}

static void out_frame(const uint8_t *d, size_t n)
{
    if (dz_client < 0) return;
    uint8_t o[2 * 1200 + 8]; size_t k = 0;   /* every byte escaped at worst */
    uint16_t sum = 0;
    for (size_t i = 0; i < n; i++) sum = (uint16_t)(sum + d[i]);
    sum = (uint16_t)(~sum + 1);
    uint8_t cs[2] = { (uint8_t)sum, (uint8_t)(sum >> 8) };
    o[k++] = 0xC0;
    for (size_t i = 0; i < n + 2 && k < sizeof o - 3; i++) {
        uint8_t b = i < n ? d[i] : cs[i - n];
        if (b == 0xC0) { o[k++] = 0xDB; o[k++] = 0xDC; }
        else if (b == 0xDB) { o[k++] = 0xDB; o[k++] = 0xDD; }
        else o[k++] = b;
    }
    o[k++] = 0xC0;
    for (size_t w = 0; w < k; ) {
        ssize_t r = send(dz_client, o + w, k - w, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == EAGAIN) { struct pollfd p = { dz_client, POLLOUT, 0 }; if (poll(&p, 1, 500) <= 0) break; continue; }
        if (r <= 0) break;
        w += (size_t)r;
    }
}

/* a reply: command, sequence, status, the frame's length (the whole, checksum excluded), payload */
static void reply(uint8_t cmd, uint8_t seq, uint8_t status, const uint8_t *pl, size_t pn)
{
    uint8_t f[1200];
    if (pn > sizeof f - 5) return;
    f[0] = cmd; f[1] = seq; f[2] = status;
    wr16(f + 3, (uint16_t)(5 + pn));
    if (pn) memcpy(f + 5, pl, pn);
    out_frame(f, 5 + pn);
}

void dz_state_changed(void)
{
    uint8_t s = devstate();
    if (s == last_state || dz_client < 0) return;
    last_state = s;
    uint8_t pl[2] = { s, 0 };
    reply(C_STATE_CHANGED, 0, S_SUCCESS, pl, 2);
}

/* ---------------------------------------------------------------- indications and confirms */

static uint8_t *addr_out(uint8_t *p, uint8_t mode, uint16_t nwk, uint64_t ieee)
{
    *p++ = mode;
    if (mode == 3) p = wr64(p, ieee);
    else { p = wr16(p, nwk); if (mode == 4) p = wr64(p, ieee); }
    return p;
}

void dz_indication(const struct apsind *in)
{
    if (dz_client < 0) return;
    if (indn == INDQ) { LOG("deconz: the host is not fetching: an indication dropped"); return; }
    struct indq *q = &inds[(indh + indn) % INDQ];
    uint8_t *p = q->b;
    /* kept without the device state and length (filled at the fetch): destination, source, profile, cluster, data */
    if (in->n > sizeof q->b - 40) return;
    p = addr_out(p, in->dst_mode == 1 ? 1 : 2, in->dst, 0);
    *p++ = in->dst_ep;
    p = addr_out(p, 4, in->src, in->src_ieee);    /* both: the fetch's flags pick */
    *p++ = in->src_ep;
    p = wr16(p, in->profile); p = wr16(p, in->cluster);
    p = wr16(p, (uint16_t)in->n); memcpy(p, in->data, in->n); p += in->n;
    *p++ = 0; *p++ = 0; *p++ = in->lqi; *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0; *p++ = (uint8_t)in->rssi;
    q->n = (uint16_t)(p - q->b);
    indn++;
    dz_state_changed();
}

/* a child's data request, as a ConBee tells its host (zigpy-deconz's mac_poll: the child's address, lqi, rssi, its
 * time left and timeout); once a minute per child at most: hosts take it as a sign of life, nothing more */
void dz_mac_poll(struct node *c, const struct rxinfo *ri)
{
    if (dz_client < 0 || zb.role != R_COORD) return;
    int64_t now = now_ms();
    if (c->poll_told && now - c->poll_told < 60000) return;
    c->poll_told = now;
    uint8_t pl[20], *p = pl + 2;
    *p++ = 2; p = wr16(p, c->nwk);      /* the address, by NWK */
    *p++ = ri->lqi; *p++ = (uint8_t)ri->rssi;
    p = wr32(p, c->timeout_s); p = wr32(p, c->timeout_s);
    wr16(pl, (uint16_t)(p - pl - 2));
    reply(C_MAC_POLL, 0, S_SUCCESS, pl, (size_t)(p - pl));
}

void dz_green_power(const uint8_t *gpdf, size_t n)
{
    if (dz_client < 0 || n > 120) return;
    uint8_t pl[130];
    wr16(pl, (uint16_t)n); memcpy(pl + 2, gpdf, n);
    reply(C_GREEN_POWER, 0, S_SUCCESS, pl, n + 2);
}

struct req { uint8_t id, mode, ep, src_ep; uint16_t nwk; uint64_t ieee; };
static struct req reqs[INFLIGHT];

static void confirmed(int st, void *arg)
{
    struct req *r = arg;
    inflight--;
    if (confn < CONFQ) {
        struct conf *c = &confs[(confh + confn++) % CONFQ];
        c->req_id = r->id; c->mode = r->mode; c->nwk = r->nwk; c->ieee = r->ieee; c->ep = r->ep; c->src_ep = r->src_ep;
        c->status = st < 0 ? APS_ST_MAC_NO_ACK : (uint8_t)st;
    }
    r->mode = 0;
    dz_state_changed();
}

/* ---------------------------------------------------------------- parameters */

static int param_read(uint8_t id, const uint8_t *arg, size_t an, uint8_t *o, size_t *on)
{
    uint8_t *p = o;
    *p++ = id;
    switch (id) {
    case 0x01: p = wr64(p, zb.ieee); break;
    case 0x05: p = wr16(p, zb.pan); break;
    case 0x07: p = wr16(p, zb.nwk); break;
    case 0x08: p = wr64(p, zb.state == NS_CONNECTED ? zb.epan : 0); break;
    case 0x09: *p++ = zb.role == R_COORD; break;
    case 0x0A: p = wr32(p, zb.channel_mask); break;
    case 0x0B: p = wr64(p, zb.epan); break;
    case 0x0E: p = wr64(p, zb.tc_ieee ? zb.tc_ieee : zb.ieee); break;
    case 0x10: *p++ = 0x03; break;      /* only the TC link key */
    case 0x13:
        if (an < 1 || arg[0] >= ZB_EP_SLOTS) return S_UNSUPPORTED;
        *p++ = arg[0];
        if (zb.ep_len[arg[0]]) { memcpy(p, zb.ep[arg[0]], zb.ep_len[arg[0]]); p += zb.ep_len[arg[0]]; }
        else { /* an empty slot: endpoint 0, nothing in it */ memset(p, 0, 8); p += 8; }
        break;
    case 0x15: *p++ = zb.predefined_pan; break;
    case 0x18: *p++ = an ? arg[0] : 0; memcpy(p, zb.nwk_key, 16); p += 16; break;
    case 0x19: {
        if (an < 8) return S_INVALID;
        uint64_t ie = rd64(arg);
        p = wr64(p, ie);
        struct node *n = ie == zb.ieee || ie == zb.tc_ieee ? NULL : node_by_ieee(ie);
        memcpy(p, n && n->has_key ? n->key : zb.tclk, 16); p += 16;
        break;
    }
    case 0x1C: *p++ = (uint8_t)(zb.state == NS_CONNECTED ? zb.channel : 0); break;
    case 0x21: { int64_t s = (zb.permit_until - now_ms()) / 1000; *p++ = (uint8_t)(s > 0 ? (s > 254 ? 254 : s) : 0); break; }
    case 0x22: p = wr16(p, PROTOCOL); break;
    case 0x24: *p++ = zb.update_id; break;
    case 0x26: { int64_t l = watchdog_until ? (watchdog_until - now_ms()) / 1000 : 0; p = wr32(p, (uint32_t)(l > 0 ? l : 0)); break; }
    case 0x27: p = wr32(p, zb.nwk_fc); break;
    case 0x28: p = wr16(p, 0); break;
    default: return S_UNSUPPORTED;
    }
    *on = (size_t)(p - o);
    return S_SUCCESS;
}

static int param_write(uint8_t id, const uint8_t *v, size_t n)
{
    switch (id) {
    case 0x01: if (n < 8) return S_INVALID; zb.ieee = rd64(v); break;
    case 0x05: if (n < 2) return S_INVALID; zb.pan = rd16(v); break;
    case 0x07: if (n < 2) return S_INVALID; zb.nwk = rd16(v); break;
    case 0x09: if (n < 1) return S_INVALID; break;      /* always the coordinator */
    case 0x0A: if (n < 4) return S_INVALID; zb.channel_mask = rd32(v); break;
    case 0x0B: if (n < 8) return S_INVALID; zb.epan = rd64(v); break;
    case 0x0E: if (n < 8) return S_INVALID; zb.tc_ieee = rd64(v); break;
    case 0x10: break;
    case 0x13:
        if (n < 1 || v[0] >= ZB_EP_SLOTS || n - 1 > sizeof zb.ep[0]) return S_INVALID;
        memcpy(zb.ep[v[0]], v + 1, n - 1); zb.ep_len[v[0]] = (uint8_t)(n - 1);
        break;
    case 0x15: if (n < 1) return S_INVALID; zb.predefined_pan = v[0]; break;
    case 0x18: if (n < 17) return S_INVALID; memcpy(zb.nwk_key, v + 1, 16); break;
    case 0x19: {
        if (n < 24) return S_INVALID;
        uint64_t ie = rd64(v);
        if (ie == zb.ieee || ie == zb.tc_ieee || !ie) { memcpy(zb.tclk, v + 8, 16); break; }
        /* a device's own key, given before it joins (an install code's): sealed with it, its network key goes */
        struct node *nd = node_add(ie, 0xFFFF);
        if (!nd) return S_FAILURE;
        memcpy(nd->key, v + 8, 16); nd->has_key = 1; nd->key_preconf = 1; nd->key_verified = 0;
        break;
    }
    case 0x21:
        if (n < 1) return S_INVALID;
        zb.permit_until = v[0] ? now_ms() + v[0] * 1000LL : 0;
        if (zb.permit_until > zb.net_permit_until || !v[0]) zb.net_permit_until = zb.permit_until;
        LOG("deconz: joining %s for %u s", v[0] ? "open" : "closed", v[0]);
        break;
    case 0x24: if (n < 1) return S_INVALID; zb.update_id = v[0]; break;
    case 0x26:
        /* the watchdog: the host writes its time to live again and again (zigpy-deconz: 60 s every 30 s); left to run
         * out, a ConBee resets itself, and its host reconnects.  Here the network runs on and only the host's
         * connection ends (dz_watchdog): the host's side of a hang is undone the same way */
        if (n < 4) return S_INVALID;
        watchdog_until = rd32(v) ? now_ms() + rd32(v) * 1000LL : 0;
        if (watchdog_until) timer_at(watchdog_until + 100, dz_watchdog, NULL);
        break;
    case 0x27: if (n < 4) return S_INVALID; zb.nwk_fc = rd32(v); zb_save(); break;
    case 0x28: break;
    default: return S_UNSUPPORTED;
    }
    zb_save_soon();
    return S_SUCCESS;
}

/* ---------------------------------------------------------------- the host's requests */

static void command(const uint8_t *f, size_t n)
{
    if (n < 5) return;
    uint8_t cmd = f[0], seq = f[1];
    const uint8_t *p = f + 5;
    size_t pn = n - 5;
    uint8_t o[300]; size_t on = 0;
    switch (cmd) {
    case C_VERSION: wr32(o, FW_VERSION); reply(cmd, seq, S_SUCCESS, o, 4); return;
    case C_DEVICE_STATE: o[0] = devstate(); o[1] = 0; o[2] = 0; last_state = o[0]; reply(cmd, seq, S_SUCCESS, o, 3); return;
    case C_CHANGE_STATE:
        if (pn < 1) { reply(cmd, seq, S_INVALID, NULL, 0); return; }
        if (p[0] == 2) { dz_want_connected = 1; if (zb.state != NS_CONNECTED && zb_form_network()) { o[0] = p[0]; reply(cmd, seq, S_FAILURE, o, 1); return; } }
        else if (p[0] == 0) { dz_want_connected = 0; if (zb.state != NS_OFFLINE) zb_stop_network(); }
        zb_save_soon();
        o[0] = p[0]; reply(cmd, seq, S_SUCCESS, o, 1);
        dz_state_changed();
        return;
    case C_READ_PARAM: {
        if (pn < 3) { reply(cmd, seq, S_INVALID, NULL, 0); return; }
        int st = param_read(p[2], p + 3, pn - 3, o + 2, &on);
        if (st) { wr16(o, 1); o[2] = p[2]; reply(cmd, seq, (uint8_t)st, o, 3); return; }
        wr16(o, (uint16_t)on);
        reply(cmd, seq, S_SUCCESS, o, on + 2);
        return;
    }
    case C_WRITE_PARAM: {
        if (pn < 3) { reply(cmd, seq, S_INVALID, NULL, 0); return; }
        int st = param_write(p[2], p + 3, pn - 3);
        wr16(o, 1); o[2] = p[2];
        reply(cmd, seq, (uint8_t)st, o, 3);
        return;
    }
    case C_APS_INDICATION: {
        uint8_t flags = pn >= 3 ? p[2] : 0;
        if (!indn) { o[0] = 0; o[1] = 0; reply(cmd, seq, S_FAILURE, NULL, 0); return; }
        struct indq *q = &inds[indh];
        indh = (indh + 1) % INDQ; indn--;
        /* the source as the host asked: NWK (1), NWK and IEEE (4), else NWK */
        uint8_t b[1150], *w = b + 2;
        *w++ = devstate();
        const uint8_t *s = q->b;
        size_t dl = s[0] == 1 || s[0] == 2 ? 3 : 9;
        memcpy(w, s, dl + 1); w += dl + 1; s += dl + 1;        /* destination and its endpoint */
        uint16_t src = rd16(s + 1); uint64_t sie = rd64(s + 3);
        if (flags & 4) w = addr_out(w, 4, src, sie);
        else w = addr_out(w, 2, src, 0);
        s += 11;
        size_t rest = q->n - (size_t)(s - q->b);
        memcpy(w, s, rest); w += rest;
        wr16(b, (uint16_t)(w - b - 2));
        reply(cmd, seq, S_SUCCESS, b, (size_t)(w - b));
        dz_state_changed();
        return;
    }
    case C_APS_CONFIRM: {
        if (!confn) { reply(cmd, seq, S_FAILURE, NULL, 0); return; }
        struct conf c = confs[confh]; confh = (confh + 1) % CONFQ; confn--;
        uint8_t b[40], *w = b + 2;
        *w++ = devstate();
        *w++ = c.req_id;
        *w++ = c.mode;
        if (c.mode == 3) w = wr64(w, c.ieee); else w = wr16(w, c.nwk);
        if (c.mode != 1) *w++ = c.ep;
        *w++ = c.src_ep; *w++ = c.status;
        *w++ = 0; *w++ = 0; *w++ = 0; *w++ = 0;
        wr16(b, (uint16_t)(w - b - 2));
        reply(cmd, seq, S_SUCCESS, b, (size_t)(w - b));
        dz_state_changed();
        return;
    }
    case C_APS_REQUEST: {
        if (pn < 6) { reply(cmd, seq, S_INVALID, NULL, 0); return; }
        size_t k = 2;
        uint8_t id = p[k++], flags = p[k++], mode = p[k++];
        uint16_t dnwk = 0; uint64_t dieee = 0; uint8_t dep = 0;
        if (mode == 1) { if (k + 2 > pn) goto bad; dnwk = rd16(p + k); k += 2; }
        else if (mode == 2) { if (k + 3 > pn) goto bad; dnwk = rd16(p + k); dep = p[k + 2]; k += 3; }
        else if (mode == 3) { if (k + 9 > pn) goto bad; dieee = rd64(p + k); dep = p[k + 8]; k += 9; }
        else if (mode == 4) { if (k + 11 > pn) goto bad; dnwk = rd16(p + k); dieee = rd64(p + k + 2); dep = p[k + 10]; k += 11; mode = 2; }
        else goto bad;
        if (k + 7 > pn) goto bad;
        uint16_t prof = rd16(p + k), clus = rd16(p + k + 2); uint8_t sep = p[k + 4];
        uint16_t al = rd16(p + k + 5); k += 7;
        if (k + al + 2 > pn) goto bad;
        const uint8_t *asdu = p + k; k += al;
        uint8_t txo = p[k], radius = p[k + 1]; k += 2;
        uint16_t relays[10]; int nr = 0;
        if (flags & 2 && k < pn) {
            nr = p[k++];
            if (nr > 10 || k + 2u * nr > pn) goto bad;
            /* the host's relays run from us towards the destination; ours from the destination's side */
            for (int i = 0; i < nr; i++) relays[nr - 1 - i] = rd16(p + k + 2 * i);
        }
        if (zb.state != NS_CONNECTED) { reply(cmd, seq, S_ERROR, NULL, 0); return; }
        int slot = -1;
        for (int i = 0; i < INFLIGHT; i++) if (!reqs[i].mode) { slot = i; break; }
        if (slot < 0 || inflight >= INFLIGHT) { reply(cmd, seq, S_BUSY, NULL, 0); return; }
        struct req *r = &reqs[slot];
        *r = (struct req){ id, mode, dep, sep, dnwk, dieee };
        inflight++;
        uint8_t b[4]; wr16(b, 2); b[2] = devstate(); b[3] = id;
        reply(cmd, seq, S_SUCCESS, b, 4);
        /* a ZDO permit joining through us: the network takes joiners for that long (the trust center's part) */
        if (prof == 0 && clus == 0x0036 && al >= 2) {
            int64_t until = asdu[1] ? now_ms() + asdu[1] * 1000LL : 0;
            if (until > zb.net_permit_until || !asdu[1]) zb.net_permit_until = until;
        }
        aps_send(mode, dnwk, dieee, dep, sep, prof, clus, asdu, al, txo & 0x04, radius, nr ? relays : NULL, nr, confirmed, r);
        dz_state_changed();
        return;
bad:
        reply(cmd, seq, S_INVALID, NULL, 0);
        return;
    }
    case C_UPDATE_NEIGHBOR: {
        /* the host's restore: an end device that was our child before a restart */
        if (pn < 14 || p[2] != 1) { reply(cmd, seq, S_INVALID, NULL, 0); return; }
        struct node *nd = node_add(rd64(p + 5), rd16(p + 3));
        if (nd) { nd->cap = p[13]; nd->child = !(p[13] & 0x02); nd->seen = now_ms(); nd->timeout_s = nd->timeout_s ? nd->timeout_s : 256; }
        memcpy(o, p, 14); reply(cmd, seq, nd ? S_SUCCESS : S_FAILURE, o, 14);
        zb_save_soon();
        return;
    }
    default:
        reply(cmd, seq, S_UNSUPPORTED, NULL, 0);
        return;
    }
}

void dz_input(int fd)
{
    ssize_t r = read(fd, rx + rxn, sizeof rx - rxn);
    if (r <= 0) {
        LOG("deconz: the host left");
        close(fd);
        host_gone();
        return;
    }
    rxn += (size_t)r;
    size_t s = 0;
    for (;;) {
        size_t e = s;
        while (e < rxn && rx[e] != 0xC0) e++;
        if (e >= rxn) break;
        uint8_t f[1400]; size_t k = 0; int esc = 0;
        for (size_t i = s; i < e && k < sizeof f; i++) {
            if (esc) { f[k++] = rx[i] == 0xDC ? 0xC0 : rx[i] == 0xDD ? 0xDB : rx[i]; esc = 0; }
            else if (rx[i] == 0xDB) esc = 1;
            else f[k++] = rx[i];
        }
        if (k >= 7) {
            uint16_t sum = 0;
            for (size_t i = 0; i < k - 2; i++) sum = (uint16_t)(sum + f[i]);
            if ((uint16_t)(~sum + 1) == rd16(f + k - 2)) command(f, k - 2);
            else LOG("deconz: a frame with a wrong checksum");
        }
        s = e + 1;
    }
    if (s) { memmove(rx, rx + s, rxn - s); rxn -= s; }
    if (rxn == sizeof rx) rxn = 0;
}

/* the host's connection gone (it left, or its watchdog ran out): what waited for it dropped */
static void host_gone(void)
{
    dz_client = -1; rxn = 0; indn = 0; confn = 0; last_state = 0xFF; watchdog_until = 0;
}

void dz_accept(int ls)
{
    int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (c < 0) return;
    if (dz_client >= 0) {
        /* a new host takes over: the old one's connection is one it gave up (Home Assistant restarted) */
        LOG("deconz: a new host: the old one's connection ends");
        close(dz_client);
    }
    dz_client = c; rxn = 0; indn = 0; confn = 0; last_state = 0xFF; watchdog_until = 0;
    LOG("deconz: a host connected");
}

int dz_listen(const char *path)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    unlink(path);
    mode_t m = umask(007);
    int r = s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 2);
    umask(m);
    if (r) { if (s >= 0) close(s); return -1; }
    return s;
}
