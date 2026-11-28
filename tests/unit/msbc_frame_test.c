/* msbc.c: mSBC packets through the air as the Echos' paths cut them (whole 60-byte packets, /dev/ebc's 30-byte pieces,
 * odd lengths), a lost stretch as zeros, a broken frame, a stream joined in mid-frame: frames found, the clock kept */
#include "msbc.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }

#define NF 200
static uint8_t air[NF * MSBC_PACKET];
static int16_t in[NF * MSBC_SAMPLES], out[(NF + 4) * MSBC_SAMPLES];

static double tone_db(const int16_t *x, size_t n, double f)       /* power at f against all of it, dB */
{
    double re = 0, im = 0, all = 0;
    for (size_t i = 0; i < n; i++) { re += x[i] * cos(2 * M_PI * f * i / 16000); im += x[i] * sin(2 * M_PI * f * i / 16000); all += (double)x[i] * x[i]; }
    return 10 * log10((re * re + im * im) * 2 / n / (all + 1e-9));
}

static size_t run(struct msbc_rx *r, const uint8_t *p, size_t n, size_t piece)
{
    size_t w = 0;
    for (size_t o = 0; o < n; o += piece) w += msbc_rx(r, p + o, n - o < piece ? n - o : piece, out + w);
    return w;
}

int main(void)
{
    struct msbc_tx t; msbc_tx_init(&t);
    for (int i = 0; i < NF * MSBC_SAMPLES; i++) in[i] = (int16_t)(10000 * sin(2 * M_PI * 1000 * i / 16000.0));
    for (int f = 0; f < NF; f++) msbc_tx(&t, in + f * MSBC_SAMPLES, air + f * MSBC_PACKET);
    check(air[0] == 1 && air[1] == 0x08 && air[61] == 0x38 && air[121] == 0xc8 && air[181] == 0xf8 && air[241] == 0x08 && air[2] == 0xad && !air[59],
          "H2 headers in sequence, syncword, padding");

    size_t pieces[] = { 60, 30, 7, 1, 1000 };
    for (size_t k = 0; k < sizeof pieces / sizeof *pieces; k++) {
        struct msbc_rx r; msbc_rx_init(&r);
        size_t w = run(&r, air, sizeof air, pieces[k]);
        char what[96]; snprintf(what, sizeof what, "pieces of %zu: %u frames, %u lost, %zu samples, 1 kHz %.1f dB", pieces[k], r.frames, r.lost, w,
                                tone_db(out + 1200, w - 1200, 1000));
        check(r.frames == NF && !r.lost && w == NF * MSBC_SAMPLES && tone_db(out + 1200, w - 1200, 1000) > -1, what);
    }

    {   /* 10 packets lost on the air (ebc: zeros), one frame broken (its CRC), then on */
        static uint8_t a2[sizeof air]; memcpy(a2, air, sizeof air);
        memset(a2 + 50 * MSBC_PACKET, 0, 10 * MSBC_PACKET);
        a2[100 * MSBC_PACKET + 7] ^= 0x40;          /* a scale factor: what the CRC covers */
        struct msbc_rx r; msbc_rx_init(&r);
        size_t w = run(&r, a2, sizeof a2, 30);
        char what[96]; snprintf(what, sizeof what, "10 packets lost, one broken: %u frames, %u lost, %zu samples", r.frames, r.lost, w);
        check(r.lost == 11 && r.frames == NF - 11 && w == NF * MSBC_SAMPLES, what);
    }
    {   /* joined in mid-frame: the next header found, the stretch before it no frame of ours */
        struct msbc_rx r; msbc_rx_init(&r);
        size_t w = run(&r, air + 23, sizeof air - 23, 30);
        char what[96]; snprintf(what, sizeof what, "joined 23 bytes in: %u frames, %u lost, %zu samples", r.frames, r.lost, w);
        check(r.frames == NF - 1 && !r.lost && w == (NF - 1) * MSBC_SAMPLES, what);
    }
    {   /* nothing but noise: no frame, a lost one per packet's length (the clock) */
        static uint8_t junk[20 * MSBC_PACKET]; for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 37 + 11);
        struct msbc_rx r; msbc_rx_init(&r);
        size_t w = run(&r, junk, sizeof junk, 30);
        char what[96]; snprintf(what, sizeof what, "noise: %u frames, %u lost, %zu samples", r.frames, r.lost, w);
        check(!r.frames && r.lost >= 18 && r.lost <= 20 && w == r.lost * MSBC_SAMPLES, what);
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
