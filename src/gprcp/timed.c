/*
 * hassmic (gprcp): timed transmit on the GP712's own clock (PLAN.md "Thread 1.4" 2b.2; docs/re-gp712.md §5), in place of
 * OpenThread's software timing, which can only start a frame as the main loop wakes: Amazon's driver then takes 1.0 to
 * 1.3 ms to put it on the air (measured), and more when its thread is late.  The chip itself holds a time: a frame
 * staged (DataRequest's txOptions | 0x10) goes when ScheduleTimedTx (command 0x8E: packet buffer, chip time) says,
 * which no call of Qorvo's library makes.  Qorvo's gpMacDispatcher_DataRequest is wrapped at link time
 * (scripts/gprcp-build.sh: -Wl,--wrap) and stages instead of sending when a time is armed.
 *
 * The chip's clock is not the host's, and API 2.7 has no command that reads it; hassmic_gptime (src/kmod/), a module of
 * ours, keeps the pair the driver itself reads for every frame and confirm (chip, then CLOCK_MONOTONIC_RAW in us, the
 * clock of otPlatTimeGet) in /proc/hassmic_gptime.  The chip clock starts again whenever Qorvo's library opens the
 * driver (measured: 0.7 s into a run, with the host at 227 s), so only pairs the module counted after our own start
 * are ours.  The two clocks drift by some ppm (12 measured); that is followed between pairs some seconds apart.
 *
 * Measured with tools/gp712/gptimed.c: the chip's own timestamp of the frame comes +255 us (255 to 256) after the time
 * scheduled, and OpenThread's target (mTxDelayBaseTime + mTxDelay) is the end of the SFD at the antenna, so the chip is
 * asked for TX_LEAD_US earlier.  Whether the chip's timestamp is the end of the SFD is its own word: held to the second
 * radio in PLAN.md 2b.2.  Staging and 0x8E take 0.6 to 1.4 ms between them (more when the driver's thread is late); a
 * time already past is sent at once (measured, never a wait for the clock's wrap), as late as OpenThread's own would.
 * No CSMA for a timed frame (OpenThread: mMaxCsmaBackoffs ignored for one; the driver asserts on a staged frame with
 * CSMA, gpHal_MAC.c:1799), so no CCA either.
 *
 * Only where the module is there, hooked all three calls, and the dispatcher's ack is where this library (by its
 * checksum, gprcp-build.sh) has it: otherwise the caps stay Qorvo's and OpenThread times frames itself.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <openthread/platform/radio.h>
#include <openthread/platform/time.h>

#define TX_LEAD_US 255     /* the chip's timestamp after the time it was given (gptimed, 2026-11-27: 255 to 256) */
#define DRIFT_SPAN_US 10000000u   /* pairs at least this far apart give the drift */
#define DRIFT_KEEP_US 120000000u  /* and the older one is moved up after this long */
#define DRIFT_MAX_PPM 200

/* Qorvo's library (the Raspberry Pi's, by its checksum) */
typedef struct { uint8_t ch[3]; } mc_t;
typedef struct { uint16_t length, offset; uint8_t handle; } loh_t;
void __real_gpMacDispatcher_DataRequest(uint8_t src, void *dst, uint8_t tx, void *sec, mc_t mc, loh_t loh, uint8_t stack);
int gpCom_DataRequestAndWaitForAck(uint8_t module, uint16_t len, uint8_t *data, uint32_t comm, uint8_t *ack,
                                   uint32_t timeout, uint8_t ackid);
void gpMacDispatcher_SetPanId(uint16_t, uint8_t);
uint8_t gpMacDispatcher_GetCsmaMode(uint8_t stack);
void gpMacDispatcher_SetCsmaMode(uint8_t mode, uint8_t stack);
extern char __bss_start[], _end[];

static int sHw;                  /* the chip times frames */
static int sPairsAtStart;        /* the module's count when our library had opened the driver */
static uint8_t *sAckFlag, *sAckBuf;
static uint32_t sTimedAt;        /* the chip time the next DataRequest goes at; 0: at once */
static int sArmed;
static unsigned sOk, sFailed, sNoPair;

/* tests only (tools/gp712/csltest.py's sweep): GPRCP_TX_ADJUST names a file whose number (us, signed) moves every
 * timed frame, read again when it changes; GPRCP_TX_LOG=1 says each timed frame and how it ended on stderr.  Unset in
 * the product: root's radio_watch starts rcpbridge and gprcp without them. */
static int32_t sAdjust;
static int sLog = -1;
static struct timespec sAdjustAt;
static uint32_t sLastTarget, sLastHost;
static int sLastSeq = -1;     /* the timed frame in flight (its MAC sequence number), for its confirm's line */

static void adjust_read(void)
{
    const char *f = getenv("GPRCP_TX_ADJUST");
    struct stat st;
    if (sLog < 0) { const char *l = getenv("GPRCP_TX_LOG"); sLog = l && *l == '1'; }
    if (!f || stat(f, &st) || (st.st_mtim.tv_sec == sAdjustAt.tv_sec && st.st_mtim.tv_nsec == sAdjustAt.tv_nsec)) return;
    sAdjustAt = st.st_mtim;
    FILE *h = fopen(f, "r");
    long v = 0;
    if (h) { if (fscanf(h, "%ld", &v) != 1) v = 0; fclose(h); }
    sAdjust = (int32_t)v;
    fprintf(stderr, "gprcp: timed frames moved by %ld us (GPRCP_TX_ADJUST)\n", v);
}

struct pair { uint32_t chip, host; int n; };
static struct pair sAnchor;      /* for the drift; n 0: none */
static double sPpm;

static int pair_read(struct pair *p, int *hooked)
{
    FILE *f = fopen("/proc/hassmic_gptime", "r");
    char l[64];
    int got = 0;
    if (!f) return -1;
    while (fgets(l, sizeof l, f)) {
        unsigned v; int n;
        if (sscanf(l, "CHIP = %u", &v) == 1) { p->chip = v; got |= 1; }
        else if (sscanf(l, "HOST = %u", &v) == 1) { p->host = v; got |= 2; }
        else if (sscanf(l, "PAIRS = %d", &n) == 1) { p->n = n; got |= 4; }
        else if (sscanf(l, "HOOKED = %d", &n) == 1) { if (hooked) *hooked = n; got |= 8; }
    }
    fclose(f);
    return got == 15 ? 0 : (got & 8) ? 1 : -1;      /* 1: the module, no pair yet */
}

/* gpMacDispatcher_SetPanId's literal pool in this library: the dispatcher's ack buffer (+0xdc) and its flag (+0xec) */
static int find_ack(void)
{
    uint32_t *w = (uint32_t *)(void *)gpMacDispatcher_SetPanId;
    sAckBuf = (uint8_t *)(uintptr_t)w[0xdc / 4];
    sAckFlag = (uint8_t *)(uintptr_t)w[0xec / 4];
    return (char *)sAckBuf >= __bss_start && (char *)sAckBuf < _end && (char *)sAckFlag >= __bss_start &&
                   (char *)sAckFlag < _end ? 0 : -1;
}

/* after qorvoRadioInit (otSysInit): the driver is open, the chip's clock new */
void gprcpTimedInit(void)
{
    if (sLog < 0) { const char *l = getenv("GPRCP_TX_LOG"); sLog = l && *l == '1'; }
    struct pair p = { 0, 0, 0 };
    int hooked = 0, r = pair_read(&p, &hooked);
    if (r < 0) return;                              /* no module: OpenThread's own timing, said by nothing */
    if (hooked != 3 || find_ack()) {
        fprintf(stderr, "gprcp: hassmic_gptime hooked %d of 3 calls%s: timed transmit in software\n", hooked,
                find_ack() ? ", the library's ack not found" : "");
        return;
    }
    sPairsAtStart = r == 0 ? p.n : 0;
    sHw = 1;
    fprintf(stderr, "gprcp: timed transmit on the chip's clock (hassmic_gptime)\n");
}

otRadioCaps gprcpTimedCaps(otRadioCaps aCaps) { return sHw ? (otRadioCaps)(aCaps | OT_RADIO_CAPS_TRANSMIT_TIMING) : aCaps; }

/* the chip's time for a host time, by the latest pair of ours and the drift; -1: none yet */
static int to_chip(uint32_t aHost, uint32_t *aChip)
{
    struct pair p;
    if (pair_read(&p, NULL) || p.n <= sPairsAtStart) return -1;
    if (!sAnchor.n || (int32_t)(p.host - sAnchor.host) < 0) sAnchor = p;
    uint32_t dh = p.host - sAnchor.host;
    if (dh >= DRIFT_SPAN_US) {
        double ppm = ((double)(int32_t)((p.chip - sAnchor.chip) - dh)) * 1e6 / dh;
        if (ppm > DRIFT_MAX_PPM || ppm < -DRIFT_MAX_PPM) { sAnchor = p; sPpm = 0; }   /* the chip's clock began again */
        else { sPpm = ppm; if (dh >= DRIFT_KEEP_US) sAnchor = p; }
    }
    int32_t ahead = (int32_t)(aHost - p.host);
    *aChip = p.chip + (uint32_t)ahead + (uint32_t)(int32_t)(ahead * sPpm / 1e6);
    return 0;
}

/* otPlatRadioTransmit, before qorvoRadioTransmit: a frame with a target goes at it */
void gprcpTimedArm(const otRadioFrame *aFrame)
{
    sArmed = 0;
    if (!sHw || (!aFrame->mInfo.mTxInfo.mTxDelay && !aFrame->mInfo.mTxInfo.mTxDelayBaseTime)) return;
    uint32_t chip;
    adjust_read();
    sLastTarget = aFrame->mInfo.mTxInfo.mTxDelayBaseTime + aFrame->mInfo.mTxInfo.mTxDelay;
    sLastHost = (uint32_t)otPlatTimeGet();
    sLastSeq = aFrame->mLength > 2 ? aFrame->mPsdu[2] : -1;
    if (to_chip(sLastTarget + (uint32_t)sAdjust, &chip)) {
        if (!sNoPair++) fprintf(stderr, "gprcp: a timed frame before the first pair of this run: sent at once\n");
        return;
    }
    sTimedAt = chip - TX_LEAD_US;
    if (!sTimedAt) sTimedAt = 1;
    sArmed = 1;
}

void __wrap_gpMacDispatcher_DataRequest(uint8_t src, void *dst, uint8_t tx, void *sec, mc_t mc, loh_t loh, uint8_t stack)
{
    static unsigned said;
    if (sLog > 0 && said++ < 40)
        fprintf(stderr, "gprcp: DataRequest src mode %u, txOptions %02x, %u bytes at %u, %s\n", src, tx, loh.length,
                loh.offset, sArmed ? "timed" : "at once");
    if (!sArmed) { __real_gpMacDispatcher_DataRequest(src, dst, tx, sec, mc, loh, stack); return; }
    sArmed = 0;
    uint8_t csma = gpMacDispatcher_GetCsmaMode(stack);
    gpMacDispatcher_SetCsmaMode(0, stack);
    __real_gpMacDispatcher_DataRequest(src, dst, (uint8_t)(tx | 0x10), sec, mc, loh, stack);      /* staged */
    gpMacDispatcher_SetCsmaMode(csma, stack);
    uint8_t m[7] = { 0x8E, loh.handle, (uint8_t)sTimedAt, (uint8_t)(sTimedAt >> 8), (uint8_t)(sTimedAt >> 16),
                     (uint8_t)(sTimedAt >> 24), stack };
    sAckBuf[9] = 0xFF; sAckBuf[10] = 0; sAckBuf[11] = 0xFF;     /* judged by this ack, never a stale one */
    int r = gpCom_DataRequestAndWaitForAck(0x72, sizeof m, m, 0x80000040, sAckFlag, 10000000, 0xFE);
    if (sAckBuf[10] == 0x8E && !sAckBuf[9] && !sAckBuf[11]) {
        sOk++;
        if (sLog > 0)
            fprintf(stderr, "gprcp: timed seq %d: target %u, %d us ahead when asked, %d us ahead when scheduled\n",
                    sLastSeq, sLastTarget, (int32_t)(sLastTarget - sLastHost),
                    (int32_t)(sLastTarget - (uint32_t)otPlatTimeGet()));
        return;
    }
    /* not measured to happen: the staged frame waits for a time it was never given, and OpenThread's transmit
     * timeout ends the wait (the RCP host resets the radio after it) */
    if (sFailed++ < 10)
        fprintf(stderr, "gprcp: ScheduleTimedTx refused (%d: status %u, command %02x, result %u), %u of %u\n", r,
                sAckBuf[9], sAckBuf[10], sAckBuf[11], sFailed, sOk + sFailed);
}

/* cbQorvoRadioTransmitDone: how a timed frame ended (GPRCP_TX_LOG only) */
void gprcpTimedDone(const otRadioFrame *aFrame, otError aError)
{
    if (sLog <= 0 || sLastSeq < 0 || aFrame->mLength < 3 || aFrame->mPsdu[2] != sLastSeq) return;
    fprintf(stderr, "gprcp: timed seq %d: %s (FCF %02x%02x, %u bytes)\n", sLastSeq,
            aError == OT_ERROR_NONE ? "acked" : aError == OT_ERROR_NO_ACK ? "no ack" : "failed", aFrame->mPsdu[1],
            aFrame->mPsdu[0], aFrame->mLength);
    sLastSeq = -1;
}

