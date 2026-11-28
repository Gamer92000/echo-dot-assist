/* The RCP over Spinel in HDLC-lite (OpenThread's RCP; on the Echo 4 Amazon's ot-rcp, served by root's rcpbridge on a
 * unix socket).  What a Zigbee stack needs of it: the channel, the PAN ID, short and long address for its address
 * filter and auto-acks, the source match tables (an ack's frame pending bit for the children with frames waiting),
 * raw frames both ways (STREAM_RAW: the RCP does CSMA, retries and the ack wait), an energy scan.  Formats as
 * OpenThread's host writes and reads them (src/lib/spinel/radio_spinel.cpp, BSD): a frame to send is the PSDU (FCS
 * bytes included, their value the radio's), channel, CSMA backoffs, retries, CSMA on, header updated, retransmission,
 * security processed, delay, delay base, rx channel after; one received is the PSDU, RSSI, noise floor, flags, then
 * (channel, LQI, timestamp) and (receive error) as length-prefixed structs.  Extended addresses in frames go in air
 * order (little-endian); Spinel's EUI-64 properties (the long address, the source match entries, the hardware
 * address) are big-endian, the order an EUI-64 is written: the RCP reverses them for the radio (OpenThread's
 * Radio::SetExtendedAddress, AddSrcMatchExtEntry).  Written in air order, the address filter held our address
 * reversed: the radio dropped every frame to our EUI-64 (an association response: no router ever joined) and gave
 * children polling by theirs no frame pending. */
#include "zb.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CMD_RESET 1
#define CMD_GET 2
#define CMD_SET 3
#define CMD_INSERT 4
#define CMD_REMOVE 5
#define CMD_IS 6
#define CMD_INSERTED 7
#define CMD_REMOVED 8
#define P_LAST_STATUS 0
#define P_PROTOCOL_VERSION 1
#define P_NCP_VERSION 2
#define P_HWADDR 8
#define P_SCAN_STATE 0x30
#define P_SCAN_MASK 0x31
#define P_SCAN_PERIOD 0x32
#define P_ENERGY_RESULT 0x39
#define P_SRC_SHORT 0x1304
#define P_SRC_EXT 0x1305
#define P_STREAM_RAW 0x71
#define P_RCP_API 0xB0
#define TX_TID 15                       /* transmissions; property requests take 1..14 */
#define TXQ 24

static uint8_t inbuf[2048];
static size_t inlen;
static struct txf txq[TXQ];
static int txh, txn, tx_busy;
static int64_t tx_started;
static uint8_t sync_tid = 1;
/* a property request's answer, filled by the frame handler while rcp_prop waits */
static struct { int want_tid, got; unsigned cmd, prop; uint8_t data[300]; size_t n; } wait_;
/* energy scan results */
static int8_t *escan; static int escan_done;
/* What the RCP says unasked (frames received, a transmission's end) waits here and is acted on from the main loop
 * only (rcp_dispatch): acting on it can ask the RCP something (a source match entry), and that waits for its answer
 * by reading more, which must never happen inside the reading that found the first */
#define EVQ 64
static struct ev { uint8_t type, len; int8_t rssi; uint8_t lqi; int status; uint8_t psdu[ZB_MAX_FRAME]; } evq[EVQ];
static int evh, evn;
static void ev_push(int type, int status, const uint8_t *psdu, size_t len, int8_t rssi, uint8_t lqi)
{
    if (evn == EVQ) { if (type == 0) zb.rx_bad++; else LOG("rcp: events overflow"); return; }
    struct ev *e = &evq[(evh + evn++) % EVQ];
    e->type = (uint8_t)type; e->status = status; e->rssi = rssi; e->lqi = lqi;
    e->len = (uint8_t)(len > ZB_MAX_FRAME ? ZB_MAX_FRAME : len);
    if (psdu) memcpy(e->psdu, psdu, e->len);
}

static uint16_t fcs16(const uint8_t *d, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) { c ^= *d++; for (int i = 0; i < 8; i++) c = c & 1 ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1); }
    return (uint16_t)~c;
}

static size_t pui(uint8_t *o, unsigned v) { size_t n = 0; do { uint8_t b = v & 0x7F; v >>= 7; o[n++] = (uint8_t)(b | (v ? 0x80 : 0)); } while (v); return n; }
static size_t rpui(const uint8_t *p, size_t n, unsigned *v)
{
    unsigned r = 0; size_t i = 0; int s = 0;
    while (i < n && i < 4) { r |= (unsigned)(p[i] & 0x7F) << s; s += 7; if (!(p[i++] & 0x80)) { *v = r; return i; } }
    return 0;
}

static int send_frame(const uint8_t *d, size_t n)
{
    uint8_t out[700]; size_t k = 0;
    uint16_t f = fcs16(d, n);
    uint8_t tmp[340];
    if (n + 2 > sizeof tmp) return -1;
    memcpy(tmp, d, n); tmp[n] = (uint8_t)f; tmp[n + 1] = (uint8_t)(f >> 8);
    out[k++] = 0x7E;
    for (size_t i = 0; i < n + 2; i++) {
        uint8_t b = tmp[i];
        if (b == 0x7E || b == 0x7D || b == 0x11 || b == 0x13 || b == 0xF8) { out[k++] = 0x7D; out[k++] = b ^ 0x20; }
        else out[k++] = b;
    }
    out[k++] = 0x7E;
    for (size_t o = 0; o < k; ) {
        ssize_t w = write(zb.rcp, out + o, k - o);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) { struct pollfd p = { zb.rcp, POLLOUT, 0 }; poll(&p, 1, 100); continue; }
        if (w <= 0) return -1;
        o += (size_t)w;
    }
    return 0;
}

static void handle(const uint8_t *f, size_t n);

/* frames out of what came: each one checked, handled */
static void drain(void)
{
    size_t s = 0;
    for (;;) {
        while (s < inlen && inbuf[s] != 0x7E) s++;
        size_t e = s + 1;
        while (e < inlen && inbuf[e] != 0x7E) e++;
        if (e >= inlen) break;
        uint8_t d[400]; size_t k = 0; int esc = 0, ok = 1;
        for (size_t i = s + 1; i < e; i++) {
            if (k >= sizeof d) { ok = 0; break; }
            if (esc) { d[k++] = inbuf[i] ^ 0x20; esc = 0; }
            else if (inbuf[i] == 0x7D) esc = 1;
            else d[k++] = inbuf[i];
        }
        if (ok && k >= 3 && fcs16(d, k - 2) == rd16(d + k - 2)) handle(d, k - 2);
        s = e;
    }
    if (s) { memmove(inbuf, inbuf + s, inlen - s); inlen -= s; }
    if (inlen == sizeof inbuf) inlen = 0;          /* noise without a flag: dropped */
}

static int read_some(int ms)
{
    struct pollfd p = { zb.rcp, POLLIN, 0 };
    int r = poll(&p, 1, ms);
    if (r <= 0) return r;
    ssize_t g = read(zb.rcp, inbuf + inlen, sizeof inbuf - inlen);
    if (g <= 0) { LOG("rcp: the radio's connection ended"); _exit(2); }   /* root starts us again; the RCP too */
    inlen += (size_t)g;
    drain();
    return 1;
}

static void tx_finish(int status);
void rcp_dispatch(void)
{
    static int busy;
    if (busy) return;
    busy = 1;
    while (evn) {
        struct ev e = evq[evh]; evh = (evh + 1) % EVQ; evn--;
        if (e.type == 1) tx_finish(e.status);
        else { struct rxinfo ri = { e.rssi, e.lqi }; mac_rx(e.psdu, e.len, &ri); }
    }
    busy = 0;
}

void rcp_input(void) { read_some(0); rcp_dispatch(); }

/* a property request: the answer's data into out, 0; -1 (the RCP's status, or no answer within a second) */
static int rcp_prop(unsigned cmd, unsigned prop, const uint8_t *data, size_t n, uint8_t *out, size_t *on)
{
    uint8_t f[320]; size_t k = 0;
    uint8_t tid = sync_tid; sync_tid = sync_tid % 14 + 1;
    f[k++] = (uint8_t)(0x80 | tid);
    k += pui(f + k, cmd); k += pui(f + k, prop);
    if (n > sizeof f - k) return -1;
    memcpy(f + k, data, n); k += n;
    wait_.want_tid = tid; wait_.got = 0;
    if (send_frame(f, k)) return -1;
    int64_t until = now_ms() + 1000;
    while (!wait_.got && now_ms() < until) read_some((int)(until - now_ms()));
    wait_.want_tid = 0;
    if (!wait_.got) { LOG("rcp: no answer to property 0x%x", prop); return -1; }
    if (wait_.prop == P_LAST_STATUS && prop != P_LAST_STATUS) {
        unsigned st = 0; rpui(wait_.data, wait_.n, &st);
        if (st) { LOG("rcp: property 0x%x: status %u", prop, st); return -1; }
    }
    if (out && on) { size_t c = wait_.n < *on ? wait_.n : *on; memcpy(out, wait_.data, c); *on = c; }
    return 0;
}

int rcp_set_u8(unsigned prop, uint8_t v) { return rcp_prop(CMD_SET, prop, &v, 1, NULL, NULL); }
int rcp_set_u16(unsigned prop, uint16_t v) { uint8_t b[2]; wr16(b, v); return rcp_prop(CMD_SET, prop, b, 2, NULL, NULL); }
static void wr64be(uint8_t *b, uint64_t v) { for (int i = 7; i >= 0; i--, v >>= 8) b[i] = (uint8_t)v; }
int rcp_set_eui(unsigned prop, uint64_t v) { uint8_t b[8]; wr64be(b, v); return rcp_prop(CMD_SET, prop, b, 8, NULL, NULL); }
void rcp_promiscuous(int on) { rcp_set_u8(SP_MAC_PROMISC, on ? 2 : 0); }

int rcp_src_match(int add, int ext, uint64_t addr)
{
    uint8_t b[8];
    if (ext) wr64be(b, addr); else wr16(b, (uint16_t)addr);
    return rcp_prop(add ? CMD_INSERT : CMD_REMOVE, ext ? P_SRC_EXT : P_SRC_SHORT, b, ext ? 8 : 2, NULL, NULL);
}

int rcp_src_match_clear(void)
{
    int r = rcp_prop(CMD_SET, P_SRC_SHORT, NULL, 0, NULL, NULL);
    return r | rcp_prop(CMD_SET, P_SRC_EXT, NULL, 0, NULL, NULL);
}

int rcp_open(const char *path)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
    zb.rcp = s;
    return 0;
}

int rcp_init(void)
{
    uint8_t f[4] = { 0x80, CMD_RESET, 2 }, out[100]; size_t n;
    /* a software reset (stack), then what it is; the reset's own "last status" comes unasked */
    send_frame(f, 3);
    for (int64_t until = now_ms() + 600; now_ms() < until; ) read_some((int)(until - now_ms()));
    n = sizeof out;
    if (rcp_prop(CMD_GET, P_PROTOCOL_VERSION, NULL, 0, out, &n) || n < 2) return -1;
    if (out[0] != 4) { LOG("rcp: Spinel %u.%u, not 4.x", out[0], out[1]); return -1; }
    n = sizeof out - 1;
    if (!rcp_prop(CMD_GET, P_NCP_VERSION, NULL, 0, out, &n)) { out[n] = 0; LOG("rcp: %s", (char *)out); }
    n = 8;
    if (rcp_prop(CMD_GET, P_HWADDR, NULL, 0, out, &n) || n != 8) return -1;
    /* the RCP says its EUI-64 most significant byte first */
    zb.hw_ieee = 0;
    for (int i = 0; i < 8; i++) zb.hw_ieee = zb.hw_ieee << 8 | out[i];
    n = sizeof out;
    if (!rcp_prop(CMD_GET, P_RCP_API, NULL, 0, out, &n)) { unsigned v = 0; rpui(out, n, &v); LOG("rcp: RCP API %u, EUI-64 %016llx", v, (unsigned long long)zb.hw_ieee); }
    return 0;
}

/* ---------------------------------------------------------------- transmissions: one at a time */

static void tx_next(void);

static void tx_finish(int status)
{
    if (!tx_busy) return;
    struct txf t = txq[txh];
    txh = (txh + 1) % TXQ; txn--; tx_busy = 0;
    if (status) zb.tx_fail++; else zb.tx_ok++;
    if (t.done) t.done(status, t.arg);
    tx_next();
}

/* A transmission the RCP never confirmed is given up; its confirm may still come.  For a while after, the next
 * frame waits, and a confirm that comes is that one's and dropped: one tid serves every transmission, so a late
 * confirm taken for the next frame's would shift every done callback after it by one */
static int64_t tx_late_until;
static void tx_next_timer(void *unused) { (void)unused; tx_next(); }
static void tx_timeout(void *unused)
{
    (void)unused;
    if (tx_busy && now_ms() - tx_started >= 900) {
        LOG("rcp: a transmission was never confirmed");
        tx_late_until = now_ms() + 400;
        ev_push(1, -1, NULL, 0, 0, 0); rcp_dispatch();
    }
}

static int scanning;                    /* an energy scan holds the radio: transmissions wait for its end */

static void tx_next(void)
{
    if (tx_busy || !txn || scanning) return;
    if (tx_late_until && now_ms() < tx_late_until) { timer_at(tx_late_until, tx_next_timer, NULL); return; }
    tx_late_until = 0;
    struct txf *t = &txq[txh];
    uint8_t f[200]; size_t k = 0;
    f[k++] = 0x80 | TX_TID;
    k += pui(f + k, CMD_SET); k += pui(f + k, P_STREAM_RAW);
    k = (size_t)(wr16(f + k, t->len) - f);
    memcpy(f + k, t->psdu, t->len); k += t->len;
    f[k++] = (uint8_t)zb.channel;
    f[k++] = 4;                         /* CSMA backoffs */
    f[k++] = t->retries;
    f[k++] = 1;                         /* CSMA */
    f[k++] = 1;                         /* header updated: our sequence numbers and security are in */
    f[k++] = 0;                         /* not a retransmission of the host's */
    f[k++] = 1;                         /* security processed (by us) */
    k = (size_t)(wr32(f + k, 0) - f); k = (size_t)(wr32(f + k, 0) - f);
    f[k++] = (uint8_t)zb.channel;
    tx_busy = 1; tx_started = now_ms();
    if (send_frame(f, k)) { ev_push(1, -1, NULL, 0, 0, 0); return; }
    timer_at(tx_started + 1000, tx_timeout, NULL);
}

int rcp_tx(struct txf *f)
{
    if (txn == TXQ) { if (f->done) f->done(-2, f->arg); return -1; }
    txq[(txh + txn) % TXQ] = *f; txn++;
    tx_next();
    return 0;
}

int rcp_tx_idle(void) { return !tx_busy && !txn; }

/* ---------------------------------------------------------------- energy scan */

/* one channel a request: an RCP scans the one channel of its mask and refuses more (Amazon's ot-rcp answers status 3,
 * invalid argument, to a mask of sixteen; OpenThread's own host asks channel by channel too) */
int rcp_energy_scan(uint32_t mask, int ms, int8_t out[27])
{
    int any = 0;
    for (int c = 11; c <= 26; c++) out[c] = 127;
    if (!rcp_tx_idle() || scanning) return -1;    /* the blocking one is for formation, before anything is sent */
    if (rcp_set_u16(P_SCAN_PERIOD, (uint16_t)ms)) return -1;
    for (int c = 11; c <= 26; c++) {
        if (!(mask & 1u << c)) continue;
        uint8_t ch = (uint8_t)c;
        if (rcp_prop(CMD_SET, P_SCAN_MASK, &ch, 1, NULL, NULL)) continue;
        escan = out; escan_done = 0;
        if (rcp_set_u8(P_SCAN_STATE, 2)) { escan = NULL; continue; }
        for (int64_t until = now_ms() + ms + 500; !escan_done && now_ms() < until; ) read_some(20);
        escan = NULL;
        any |= escan_done;
    }
    rcp_set_u8(SP_PHY_CHAN, (uint8_t)zb.channel);
    return any ? 0 : -1;
}

/* The scan a running network asks for (Mgmt_NWK_Update_req: the host's, a network manager's): channel by channel
 * from timers, the loop going on meanwhile (a blocking wait here held every frame, and with a transmission queued
 * never ended: its confirm is acted on only from the loop).  The transmission on the air finishes first; the ones
 * queued wait for the scan's end.  One at a time */
static struct { int on, ch, ms; uint32_t mask; int8_t e[27]; int any; int64_t until; void (*done)(int, const int8_t *, void *); void *arg; } as;

static void as_step(void *unused);
static void as_end(void)
{
    as.on = 0; scanning = 0; escan = NULL;
    rcp_set_u8(SP_PHY_CHAN, (uint8_t)zb.channel);
    void (*d)(int, const int8_t *, void *) = as.done; void *a = as.arg;
    tx_next();
    if (d) d(as.any ? 0 : -1, as.e, a);
}

static void as_step(void *unused)
{
    (void)unused;
    if (!as.on) return;
    int64_t now = now_ms();
    if (as.ch == 10) {                  /* waiting for the frame on the air */
        if (tx_busy && now < as.until) { timer_at(now + 20, as_step, NULL); return; }
        if (rcp_set_u16(P_SCAN_PERIOD, (uint16_t)as.ms)) { as_end(); return; }
        as.ch = 11; escan = NULL;
    }
    if (escan) {                        /* a channel under way */
        if (!escan_done && now < as.until) { timer_at(now + 20, as_step, NULL); return; }
        as.any |= escan_done; escan = NULL; as.ch++;
    }
    for (; as.ch <= 26; as.ch++) {
        if (!(as.mask & 1u << as.ch)) continue;
        uint8_t ch = (uint8_t)as.ch;
        if (rcp_prop(CMD_SET, P_SCAN_MASK, &ch, 1, NULL, NULL)) continue;
        escan = as.e; escan_done = 0;
        if (rcp_set_u8(P_SCAN_STATE, 2)) { escan = NULL; continue; }
        as.until = now_ms() + as.ms + 500;
        timer_at(now_ms() + as.ms, as_step, NULL);
        return;
    }
    as_end();
}

int rcp_energy_scan_async(uint32_t mask, int ms, void (*done)(int ok, const int8_t *e, void *arg), void *arg)
{
    if (as.on) return -1;
    memset(&as, 0, sizeof as);
    for (int c = 0; c <= 26; c++) as.e[c] = 127;
    as.on = 1; as.mask = mask; as.ms = ms; as.ch = 10; as.done = done; as.arg = arg; as.until = now_ms() + 1500;
    scanning = 1;
    timer_at(now_ms(), as_step, NULL);
    return 0;
}

/* ---------------------------------------------------------------- what the RCP says */

static void handle(const uint8_t *f, size_t n)
{
    if (n < 3 || (f[0] & 0xC0) != 0x80) return;
    unsigned tid = f[0] & 0x0F, cmd, prop;
    size_t k = 1, u;
    if (!(u = rpui(f + k, n - k, &cmd))) return;
    k += u;
    if (!(u = rpui(f + k, n - k, &prop))) return;
    k += u;
    const uint8_t *d = f + k; size_t dn = n - k;
    if (tid == TX_TID && tid && prop == P_LAST_STATUS && cmd == CMD_IS) {
        unsigned st = 0; rpui(d, dn, &st);
        if (!tx_busy) { tx_late_until = 0; return; }   /* the confirm of one given up (tx_timeout) */
        ev_push(1, (int)st, NULL, 0, 0, 0);     /* 0 ok; 17 no ack; 18 channel access failure */
        return;
    }
    if (tid && tid == (unsigned)wait_.want_tid) {
        wait_.cmd = cmd; wait_.prop = prop;
        wait_.n = dn < sizeof wait_.data ? dn : sizeof wait_.data;
        memcpy(wait_.data, d, wait_.n);
        wait_.got = 1;
        return;
    }
    if (cmd == CMD_IS && prop == P_STREAM_RAW && dn >= 2) {
        uint16_t len = rd16(d);
        if (len < 5 || (size_t)len + 2 + 4 > dn) return;
        struct rxinfo ri = { (int8_t)d[2 + len], 0 };
        size_t m = 2 + len + 1 + 1;
        uint16_t flags = rd16(d + m); m += 2;
        if (m + 2 + 2 <= dn) { uint16_t sl = rd16(d + m); if (sl >= 2 && m + 2 + sl <= dn) ri.lqi = d[m + 3]; }
        if (flags & 0x0004) { zb.rx_bad++; return; }      /* bad FCS */
        zb.rx_frames++;
        if (len <= ZB_MAX_FRAME) ev_push(0, 0, d + 2, len, ri.rssi, ri.lqi);
        return;
    }
    if (cmd == CMD_IS && prop == P_ENERGY_RESULT && dn >= 2) { if (escan && d[0] <= 26) escan[d[0]] = (int8_t)d[1]; return; }
    if (cmd == CMD_IS && prop == P_SCAN_STATE && dn >= 1 && d[0] == 0) { escan_done = 1; return; }
    if (cmd == CMD_IS && prop == P_LAST_STATUS) {
        unsigned st = 0; rpui(d, dn, &st);
        if (st >= 112 && st < 128) LOG("rcp: reset (%u)", st);
    }
}
