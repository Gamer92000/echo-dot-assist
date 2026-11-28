/* Timed transmit on the GP712's own clock, held to what the chip says it did (PLAN.md "Thread 1.4" 2b.2; docs/re-gp712.md
 * §5): Qorvo's library (with the byte of build-probe.sh) sends a frame as always, but its gpMacDispatcher_DataRequest
 * is wrapped at link time (-Wl,--wrap): the frame staged (txOptions 0x10) and ScheduleTimedTx (command 0x8E) sent for
 * its packet buffer at a chip time, from /proc/hassmic_gptime's pair (src/kmod/hassmic_gptime.c), the host's target
 * plus the pair's difference.  The module's TX (the chip's own timestamp of the frame it confirmed) against that
 * target is the error.  Immediate frames beside them, as the baseline.
 * The time the two calls take (staging and 0x8E) is printed too: a timed frame needs at least that much ahead.
 *   gptimed [frames] [ahead us]        root, radio otherwise idle, hassmic_gptime loaded */
#include <openthread/platform/radio.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "radio_qorvo.h"
#include "platform_qorvo.h"
#include "alarm_qorvo.h"

int backtrace(void **b, int n) { (void)b; (void)n; return 0; }
char **backtrace_symbols(void *const *b, int n) { (void)b; (void)n; return NULL; }

static long long raw_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC_RAW, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }
static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* /proc/hassmic_gptime: CHIP, HOST, TX, TXN */
static int gptime(uint32_t *chip, uint32_t *host, uint32_t *tx, int *txn)
{
    FILE *f = fopen("/proc/hassmic_gptime", "r"); char l[64]; int got = 0;
    if (!f) return -1;
    while (fgets(l, sizeof l, f)) {
        unsigned v; int n;
        if (sscanf(l, "CHIP = %u", &v) == 1) { *chip = v; got |= 1; }
        else if (sscanf(l, "HOST = %u", &v) == 1) { *host = v; got |= 2; }
        else if (sscanf(l, "TX = %u", &v) == 1) { *tx = v; got |= 4; }
        else if (sscanf(l, "TXN = %d", &n) == 1) { *txn = n; got |= 8; }
    }
    fclose(f);
    return got == 15 ? 0 : -1;
}

/* the library's own: DataRequest (wrapped), and the call its commands are sent with (gpCom), the dispatcher's ack */
typedef struct { uint8_t ch[3]; } mc_t;
typedef struct { uint16_t length, offset; uint8_t handle; } loh_t;
void __real_gpMacDispatcher_DataRequest(uint8_t src, void *dst, uint8_t tx, void *sec, mc_t mc, loh_t loh, uint8_t stack);
int gpCom_DataRequestAndWaitForAck(uint8_t module, uint16_t len, uint8_t *data, uint32_t comm, uint8_t *ack, uint32_t timeout, uint8_t ackid);
void gpMacDispatcher_SetPanId(uint16_t, uint8_t);
uint8_t gpMacDispatcher_GetCsmaMode(uint8_t stack);
void gpMacDispatcher_SetCsmaMode(uint8_t mode, uint8_t stack);
extern char __bss_start[], _end[];

static uint32_t timed_at;           /* the chip time the next frame goes at; 0: at once */
static int timed_ok, timed_fail;
static long stage_min = 1L << 30, stage_max, sched_min = 1L << 30, sched_max;
static uint8_t *ack_flag, *ack_buf;

/* gpMacDispatcher_SetPanId's literals (this library, by its checksum): the ack buffer (+0xdc) and its flag (+0xec) */
static int find_ack(void)
{
    uint32_t *w = (uint32_t *)(void *)gpMacDispatcher_SetPanId;
    ack_buf = (uint8_t *)(uintptr_t)w[0xdc / 4]; ack_flag = (uint8_t *)(uintptr_t)w[0xec / 4];
    return (char *)ack_buf >= __bss_start && (char *)ack_buf < _end && (char *)ack_flag >= __bss_start && (char *)ack_flag < _end ? 0 : -1;
}

void __wrap_gpMacDispatcher_DataRequest(uint8_t src, void *dst, uint8_t tx, void *sec, mc_t mc, loh_t loh, uint8_t stack)
{
    if (!timed_at) { __real_gpMacDispatcher_DataRequest(src, dst, tx, sec, mc, loh, stack); return; }
    /* a timed frame goes at its time, no CSMA: the driver asserts otherwise (gpHal_DataRequest, gpHal_MAC.c:1799) */
    long long t0 = raw_us();
    uint8_t csma = gpMacDispatcher_GetCsmaMode(stack);
    gpMacDispatcher_SetCsmaMode(0, stack);
    __real_gpMacDispatcher_DataRequest(src, dst, (uint8_t)(tx | 0x10), sec, mc, loh, stack);       /* staged */
    gpMacDispatcher_SetCsmaMode(csma, stack);
    long long t1 = raw_us();
    uint8_t m[7] = { 0x8E, loh.handle, (uint8_t)timed_at, (uint8_t)(timed_at >> 8), (uint8_t)(timed_at >> 16), (uint8_t)(timed_at >> 24), stack };
    ack_buf[9] = 0xFF; ack_buf[10] = 0; ack_buf[11] = 0xFF;      /* judged by the ack itself, never a stale one */
    int r = gpCom_DataRequestAndWaitForAck(0x72, sizeof m, m, 0x80000040, ack_flag, 10000000, 0xFE);
    long long t2 = raw_us();
    if (t1 - t0 < stage_min) stage_min = t1 - t0;
    if (t1 - t0 > stage_max) stage_max = t1 - t0;
    if (t2 - t1 < sched_min) sched_min = t2 - t1;
    if (t2 - t1 > sched_max) sched_max = t2 - t1;
    if (ack_buf[10] == 0x8E && !ack_buf[9] && !ack_buf[11]) timed_ok++;
    else { timed_fail++; printf("ScheduleTimedTx: r %d status %u cmd %02x result %u\n", r, ack_buf[9], ack_buf[10], ack_buf[11]); }
    timed_at = 0;
}

static int done; static otError done_err;
void cbQorvoRadioReceiveDone(otRadioFrame *f, otError e) { (void)f; (void)e; }
void cbQorvoRadioTransmitDone(otRadioFrame *f, bool pending, otError e) { (void)f; (void)pending; done = 1; done_err = e; }
void cbQorvoRadioTransmitDone_AckFrame(otRadioFrame *f, otRadioFrame *ack, otError e) { (void)f; (void)ack; done = 1; done_err = e; }
void cbQorvoRadioTxStarted(otRadioFrame *f) { (void)f; }
void cbQorvoRadioEnergyScanDone(int8_t v) { (void)v; }
static uint8_t gotosleep(void) { return 1; }

static int send_wait(otRadioFrame *f, int ms)
{
    done = 0;
    if (qorvoRadioTransmit(f)) return -1;
    for (long long until = mono_ms() + ms; !done && mono_ms() < until; ) qorvoPlatMainLoop(1);
    return done ? (int)done_err : -2;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 20, ahead = argc > 2 ? atoi(argv[2]) : 30000;
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint32_t chip, host, tx, tx0; int txn, txn0;
    if (find_ack()) { printf("the dispatcher's ack is not where this library has it\n"); return 1; }
    qorvoPlatInit((qorvoPlatGotoSleepCheckCallback_t)gotosleep);
    qorvoAlarmInit();
    qorvoRadioInit();
    qorvoRadioSetPanId(0xFFFF); qorvoRadioSetShortAddress(0xFFFE); qorvoRadioSetRxOnWhenIdle(true);
    qorvoRadioSetCurrentChannel(26);            /* the quietest here; a beacon request nobody answers */
    static uint8_t psdu[127];
    otRadioFrame f; memset(&f, 0, sizeof f); f.mPsdu = psdu; f.mChannel = 26;
    f.mInfo.mTxInfo.mMaxCsmaBackoffs = 0; f.mInfo.mTxInfo.mMaxFrameRetries = 0;
    /* a first frame: its confirm gives the module its first pair (an idle radio has none) */
    { uint8_t br[] = { 0x03, 0x08, 0xEE, 0xff, 0xff, 0xff, 0xff, 0x07, 0, 0 }; memcpy(psdu, br, sizeof br); f.mLength = sizeof br; send_wait(&f, 500); }
    if (gptime(&chip, &host, &tx, &txn)) { printf("no /proc/hassmic_gptime (or no pair yet)\n"); return 1; }
    long sum_t = 0, sum_i = 0, min_t = 1L << 30, max_t = -(1L << 30); int n_t = 0, n_i = 0;
    for (int i = 0; i < 2 * frames; i++) {
        uint8_t br[] = { 0x03, 0x08, (uint8_t)i, 0xff, 0xff, 0xff, 0xff, 0x07, 0, 0 };
        memcpy(psdu, br, sizeof br); f.mLength = sizeof br;
        if (gptime(&chip, &host, &tx0, &txn0)) { printf("no pair\n"); return 1; }
        int timed = i % 2;
        uint32_t want = 0;
        long long now = raw_us();
        if (timed) { want = chip + (uint32_t)((uint32_t)(now + ahead) - host); timed_at = want; }
        else want = chip + (uint32_t)((uint32_t)now - host);
        int r = send_wait(&f, ahead / 1000 + 500);
        if (gptime(&chip, &host, &tx, &txn) || txn == txn0) { printf("frame %d: no confirm seen by the module (%d)\n", i, r); continue; }
        long err = (long)(int32_t)(tx - want);
        if (timed) { sum_t += err; n_t++; if (err < min_t) min_t = err; if (err > max_t) max_t = err; }
        else { sum_i += err; n_i++; }
        printf("%s frame %d: sent %+ld us %s (err %d)\n", timed ? "timed" : "now  ", i, err, timed ? "from its target" : "after the call", r);
    }
    if (n_t) printf("timed: %d frames, mean %+ld us, %+ld .. %+ld; ScheduleTimedTx %d ok, %d failed\n", n_t, sum_t / n_t, min_t, max_t, timed_ok, timed_fail);
    if (n_t) printf("staging %ld .. %ld us, 0x8E %ld .. %ld us\n", stage_min, stage_max, sched_min, sched_max);
    if (n_i) printf("at once: %d frames, mean %+ld us after the call\n", n_i, sum_i / n_i);
    return 0;
}
