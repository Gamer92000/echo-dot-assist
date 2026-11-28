/* zigbeed's parsers under libFuzzer, ASan and UBSan (make unit-zb_fuzz; clang): what anyone in radio range can send
 * (a frame into mac_rx: MAC, NWK, APS, ZDO, ZCL, joining and the trust center's commands) and what the deCONZ host
 * sends (deconz.c's commands), with the node in each role and state.  Built with -DZB_FUZZ: CCM*'s MIC check passes
 * whatever it is (crypto.c), so the fuzzer reaches what lies behind the network key and the link keys.  The radio
 * (spinel.c) is a stand-in here that takes every transmission and answers every property. */
#include "../../src/zigbee/zb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct zb zb;
static int64_t clock_ms = 1000000;
int64_t now_ms(void) { return clock_ms; }
void logf_(const char *fmt, ...) { (void)fmt; }
uint32_t zb_random(void) { static uint32_t x = 12345; x = x * 1103515245 + 12345; return x; }
void zb_save(void) {}
void zb_save_soon(void) {}
int zb_form_network(void) { nwk_start(); return 0; }
void zb_stop_network(void) { nwk_stop(); }
void zb_reset_network(int rejoin) { (void)rejoin; zb.joined = 0; }
void store_save(void) {}
int store_load(void) { return 0; }

/* timers: kept, and fired a few times per input (they send frames of their own) */
#define TIMERS 96
static struct { int used; int64_t when; void (*fn)(void *); void *arg; } timers[TIMERS];
int timer_at(int64_t when, void (*fn)(void *), void *arg)
{
    for (int i = 0; i < TIMERS; i++) if (timers[i].used && timers[i].fn == fn && timers[i].arg == arg) { timers[i].when = when; return 0; }
    for (int i = 0; i < TIMERS; i++) if (!timers[i].used) { timers[i].used = 1; timers[i].when = when; timers[i].fn = fn; timers[i].arg = arg; return 0; }
    return -1;
}
void timer_cancel(void (*fn)(void *), void *arg) { for (int i = 0; i < TIMERS; i++) if (timers[i].used && timers[i].fn == fn && timers[i].arg == arg) timers[i].used = 0; }
static void run_timers(void)
{
    for (int i = 0; i < TIMERS; i++) if (timers[i].used && timers[i].when <= clock_ms) { void (*f)(void *) = timers[i].fn; void *a = timers[i].arg; timers[i].used = 0; f(a); }
}

/* the radio: every transmission taken at once and acked (or not), every property answered */
static unsigned ntx;
int rcp_open(const char *p) { (void)p; return 0; }
int rcp_init(void) { return 0; }
int rcp_set_u8(unsigned p, uint8_t v) { (void)p; (void)v; return 0; }
int rcp_set_u16(unsigned p, uint16_t v) { (void)p; (void)v; return 0; }
int rcp_set_eui(unsigned p, uint64_t v) { (void)p; (void)v; return 0; }
int rcp_src_match(int a, int e, uint64_t x) { (void)a; (void)e; (void)x; return 0; }
int rcp_src_match_clear(void) { return 0; }
void rcp_input(void) {}
void rcp_dispatch(void) {}
void rcp_promiscuous(int on) { (void)on; }
int rcp_tx_idle(void) { return 1; }
int rcp_tx(struct txf *f)
{
    if (f->len < 3 || f->len > ZB_MAX_FRAME) abort();      /* nothing bigger than a PSDU ever goes to the radio */
    ntx++;
    if (f->done) f->done(ntx % 3 == 0 ? 17 : 0, f->arg);
    return 0;
}
int rcp_energy_scan(uint32_t m, int ms, int8_t out[27]) { (void)m; (void)ms; for (int c = 0; c < 27; c++) out[c] = -80; return 0; }
static void (*scan_cb)(int, const int8_t *, void *); static void *scan_arg;
static void scan_fire(void *u) { (void)u; int8_t e[27]; for (int c = 0; c < 27; c++) e[c] = -70; void (*f)(int, const int8_t *, void *) = scan_cb; scan_cb = NULL; if (f) f(0, e, scan_arg); }
int rcp_energy_scan_async(uint32_t m, int ms, void (*done)(int, const int8_t *, void *), void *arg)
{ (void)m; if (scan_cb) return -1; scan_cb = done; scan_arg = arg; timer_at(clock_ms + ms, scan_fire, NULL); return 0; }

static int host[2] = { -1, -1 };

static void setup(uint8_t how)
{
    memset(&zb, 0, sizeof zb);
    memset(timers, 0, sizeof timers); scan_cb = NULL;
    zb.role = how % 3 == 0 ? R_COORD : how % 3 == 1 ? R_ROUTER : R_ENDDEV;
    zb.state = how & 4 ? NS_JOINING : NS_CONNECTED;
    zb.ieee = 0x00155F0000000001ULL; zb.nwk = zb.role == R_COORD ? 0 : 0x1234; zb.pan = 0x1A62; zb.epan = 0xDDDDDDDDDDDDDDDDULL;
    zb.channel = 15; zb.parent = 0; zb.tc_ieee = 0x00155F0000000009ULL; zb.joined = zb.state == NS_CONNECTED;
    memcpy(zb.tclk, "ZigBeeAlliance09", 16); memset(zb.nwk_key, 0x5A, 16);
    zb.channel_mask = 0x03FFF800; zb.mto_next = 0xFFFF;
    zb.permit_until = how & 8 ? clock_ms + 60000 : 0;
    zb.nwk_fc_saved = 1u << 30;
    zb.dir = "/nonexistent";
    /* some nodes: a neighbor router, a sleepy child, the trust center */
    struct node *n = node_add(0x00155F0000000002ULL, 0x2222); n->cap = 0x8E; n->neighbor = 1; n->seen = clock_ms; n->lqi = 200;
    n = node_add(0x00155F0000000003ULL, 0x3333); n->cap = 0x80; n->child = 1; n->seen = clock_ms; n->timeout_s = 256;
    n = node_add(zb.tc_ieee, 0x0000); n->cap = 0x8F;
    if (zb.role == R_COORD) {
        if (host[0] < 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, host)) abort();
        dz_client = host[1];
        uint8_t ep[] = { 1, 0x04, 0x01, 0x05, 0x00, 0x01, 1, 0x00, 0x00, 1, 0x19, 0x00 };
        memcpy(zb.ep[0], ep, sizeof ep); zb.ep_len[0] = sizeof ep;
    }
    if (zb.state == NS_CONNECTED) nwk_start();
}

/* the host's side drained, so the socket never fills */
static void drain_host(void)
{
    char b[4096];
    if (host[0] < 0) return;
    while (recv(host[0], b, sizeof b, MSG_DONTWAIT) > 0) ;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 2) return 0;
    setup(d[0]);
    d++; n--;
    /* several frames in one input: a length byte each; the high bit of the length: the host's bytes (SLIP), not air */
    while (n > 1) {
        size_t l = d[0] & 0x7F; int to_host = d[0] & 0x80;
        d++; n--;
        if (l > n) l = n;
        if (to_host && zb.role == R_COORD) {
            uint8_t f[300]; size_t k = 0;
            f[k++] = 0xC0;
            for (size_t i = 0; i < l && k < sizeof f - 4; i++) f[k++] = d[i];
            f[k++] = 0xC0;
            if (write(host[0], f, k) > 0) dz_input(host[1]);
            if (dz_client < 0) {            /* the host's connection ended (dz_input closed it): a new one */
                close(host[0]); host[0] = host[1] = -1;
                if (socketpair(AF_UNIX, SOCK_STREAM, 0, host)) abort();
                dz_client = host[1];
            }
        } else if (l) {
            struct rxinfo ri = { -60, (uint8_t)(l * 7) };
            mac_rx(d, l, &ri);
        }
        d += l; n -= l;
        clock_ms += 700;
        run_timers();
        drain_host();
    }
    clock_ms += 10000; run_timers(); drain_host();
    return 0;
}
