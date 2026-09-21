/* Output latency of the mixer's Music stream: play clicks inside a continuous stream, find them in micRaw.
 * Result = (time the click is heard) - (time it would play if everything after MixerReleaseBufPlay took no time),
 * i.e. the value for HASSMIC_OUTPUT_LATENCY_MS.  Includes the capture path's own latency (unknown, additive), so it
 * is an upper bound; the raw capture block size is printed to judge that.  Run with the room quiet and music paused. */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mixer_api.h"

#define CAP_RATE 16000
#define MAXS (CAP_RATE * 12)
static int16_t cap[MAXS]; static long long cap_t[MAXS / 64 + 1]; static size_t cap_n; static volatile int stop; static unsigned block;

static long long raw_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts); return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }

static void *capture(void *arg)
{
    MixerHandle h = arg;
    while (!stop && cap_n < MAXS - 8192) {
        int st = 0; unsigned n = 0; int16_t *p = MixerGetBufRec(h, &st, &n);
        if (!p) continue;
        long long t = raw_us(); n /= 2; if (n > block) block = n;
        for (unsigned i = 0; i < n; i++) { if ((cap_n & 63) == 0) cap_t[cap_n / 64] = t - (long long)(n - i) * 1000000 / CAP_RATE; cap[cap_n++] = p[i]; }
        MixerReleaseBufRec(h);
    }
    return NULL;
}

int main(void)
{
    enum { RATE = 48000, CLICKS = 5 };
    MixerHandle rec = MixerOpenRec("micRaw"), play = MixerOpenPlay(RATE, 2, 16, MIXER_PLAY_MUSIC);
    static int16_t piece[RATE / 100 * 2]; long long expect[CLICKS], t0 = 0, written = 0; pthread_t t; int k = 0;
    if (!rec || !play) { fprintf(stderr, "latency: cannot open streams (run as puffin via runas)\n"); return 1; }
    pthread_create(&t, NULL, capture, rec);
    for (int i = 0; i < 700 && k <= CLICKS; i++) {                     /* 10 ms pieces, 7 s */
        int click = i >= 150 && (i - 150) % 100 == 0 && k < CLICKS;
        for (int s = 0; s < RATE / 100; s++) piece[2 * s] = piece[2 * s + 1] = click && s < 240 ? (int16_t)(20000 * sin(2 * M_PI * 2000 * s / RATE)) : 0;
        if (!t0) t0 = raw_us();
        long long queued = written * 1000000 / (RATE * 4) - (raw_us() - t0);
        if (queued < 0) { t0 = raw_us(); written = 0; queued = 0; }
        if (click) expect[k++] = raw_us() + queued;
        for (size_t off = 0, len = sizeof piece; off < len; ) {
            int st = 0; unsigned capb = 0; char *b = MixerGetBufPlay(play, &st, &capb);
            if (!b) return 1;
            unsigned n = len - off < capb ? len - off : capb; memcpy(b, (char *)piece + off, n); MixerReleaseBufPlay(play, n); off += n;
        }
        written += sizeof piece;
        while (written * 1000000 / (RATE * 4) - (raw_us() - t0) > 120000) usleep(2000);    /* same 120 ms target as hassmic */
        if (i == 150 + 100 * CLICKS + 60) break;
    }
    usleep(400000); stop = 1; pthread_join(t, NULL);

    double noise = 0; for (size_t i = 0; i < CAP_RATE / 2 && i < cap_n; i++) noise += fabs((double)cap[i]); noise /= CAP_RATE / 2;
    double res[CLICKS]; int got = 0;
    for (int c = 0; c < k; c++) {
        for (size_t i = 0; i < cap_n; i++) {
            long long ti = cap_t[i / 64] + (long long)(i & 63) * 1000000 / CAP_RATE;
            if (ti < expect[c] - 20000 || ti > expect[c] + 600000) continue;
            if (fabs((double)cap[i]) > noise * 8 + 300) { res[got++] = (ti - expect[c]) / 1000.0; break; }
        }
    }
    printf("capture block %u samples (%.1f ms), noise floor %.0f, clicks found %d/%d\n", block, block * 1000.0 / CAP_RATE, noise, got, k);
    for (int i = 0; i < got; i++) printf("  click %d: %.1f ms\n", i + 1, res[i]);
    if (got) { for (int i = 0; i < got; i++) for (int j = i + 1; j < got; j++) if (res[j] < res[i]) { double x = res[i]; res[i] = res[j]; res[j] = x; }
               printf("median output latency: %.0f ms\n", res[got / 2]); }
    MixerClose(play); MixerClose(rec);
    return got ? 0 : 1;
}
