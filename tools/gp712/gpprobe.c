/* Qorvo's public GP712 library against Amazon's driver on the Echo 4 (/dev/gp; docs/re-gp712.md, PLAN.md "Thread 1.4"
 * phase 0.1): init, the EUI-64, on each channel a beacon request sent, what is heard, an energy scan.  Built by
 * tools/gp712/build-probe.sh; run as root with the radio off (nothing else holding the driver): gpprobe [listen ms] */
#include <openthread/platform/radio.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include "radio_qorvo.h"
#include "platform_qorvo.h"
#include "alarm_qorvo.h"
#include "random_qorvo.h"

/* glibc names the library was built against */

int backtrace(void **b, int n) { (void)b; (void)n; return 0; }
char **backtrace_symbols(void *const *b, int n) { (void)b; (void)n; return NULL; }

static int rx_n, tx_n, tx_err, ed_done; static int8_t ed_val;
static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

void cbQorvoRadioReceiveDone(otRadioFrame *f, otError e)
{
    if (e || !f) return;
    rx_n++;
    if (rx_n <= 40) {
        printf("rx ch %u len %u rssi %d lqi %u ts %llu:", f->mChannel, f->mLength, f->mInfo.mRxInfo.mRssi, f->mInfo.mRxInfo.mLqi,
               (unsigned long long)f->mInfo.mRxInfo.mTimestamp);
        for (int i = 0; i < f->mLength && i < 16; i++) printf(" %02x", f->mPsdu[i]);
        printf("\n");
    }
}
void cbQorvoRadioTransmitDone(otRadioFrame *f, bool pending, otError e) { (void)f; (void)pending; tx_n++; if (e) tx_err++; printf("tx done err %d\n", e); }
void cbQorvoRadioTransmitDone_AckFrame(otRadioFrame *f, otRadioFrame *ack, otError e) { (void)f; (void)ack; tx_n++; if (e) tx_err++; printf("tx done (ack frame) err %d\n", e); }
void cbQorvoRadioTxStarted(otRadioFrame *f) { (void)f; }
void cbQorvoRadioEnergyScanDone(int8_t v) { ed_val = v; ed_done = 1; }
uint8_t gotosleep(void) { return 1; }
/* the platform's own hooks the library may call */

static void run(int ms) { long long until = now_ms() + ms; while (now_ms() < until) qorvoPlatMainLoop(1); }

int main(int argc, char **argv)
{
    int listen_ms = argc > 1 ? atoi(argv[1]) : 400;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("init\n");
    qorvoPlatInit((qorvoPlatGotoSleepCheckCallback_t)gotosleep);
    qorvoAlarmInit();

    qorvoRadioInit();
    uint8_t eui[8]; qorvoRadioGetIeeeEui64(eui);
    printf("eui64"); for (int i = 0; i < 8; i++) printf(" %02x", eui[i]); printf("\ncaps %x\n", qorvoRadioGetCaps());
    qorvoRadioSetPanId(0xFFFF);
    qorvoRadioSetShortAddress(0xFFFE);
    qorvoRadioSetPromiscuous(true);
    qorvoRadioSetRxOnWhenIdle(true);
    static uint8_t psdu[127];
    otRadioFrame f; memset(&f, 0, sizeof f); f.mPsdu = psdu;
    for (int ch = 11; ch <= 26; ch++) {
        qorvoRadioSetCurrentChannel((uint8_t)ch);
        /* a beacon request: networks on this channel answer */
        uint8_t br[] = { 0x03, 0x08, (uint8_t)(0x40 + ch), 0xff, 0xff, 0xff, 0xff, 0x07, 0, 0 };
        memcpy(psdu, br, sizeof br); f.mLength = sizeof br; f.mChannel = (uint8_t)ch;
        f.mInfo.mTxInfo.mMaxCsmaBackoffs = 4; f.mInfo.mTxInfo.mMaxFrameRetries = 0;
        int before = rx_n;
        otError e = qorvoRadioTransmit(&f);
        if (e) printf("ch %d transmit refused %d\n", ch, e);
        run(listen_ms);
        ed_done = 0;
        e = qorvoRadioEnergyScan((uint8_t)ch, 100);
        run(300);
        printf("ch %d: %d frames, energy %s%d dBm\n", ch, rx_n - before, ed_done ? "" : "(none) ", ed_val);
    }
    printf("done: rx %d tx %d (errors %d)\n", rx_n, tx_n, tx_err);
    return 0;
}
