/* cvsd.c: a tone through the coder comes back as that tone at its level; the filters pass speech's band and stop what
 * would alias (make unit-cvsd).  The bit order and the coder itself were held to real chips (docs/re-hfp.md): the Echo
 * Dot 5's CVSD decoded what this encodes, and what its chip encoded decodes here as the tone it was. */
#include "cvsd.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL "); } else printf("ok   "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* level (dBFS) and the share of the power within 30 Hz of f, over n samples at rate */
static void measure(const int16_t *x, size_t n, float rate, float f, float *db, float *share)
{
    double re = 0, im = 0, all = 0;
    for (size_t i = 0; i < n; i++) {
        double v = x[i] / 32768.0, w = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1));
        re += v * w * cos(2 * M_PI * f * i / rate); im += v * w * sin(2 * M_PI * f * i / rate); all += v * v;
    }
    *db = 10 * log10(all / n + 1e-20);
    *share = (float)(8 * (re * re + im * im) / ((double)n * n) / (all / n + 1e-20));   /* a sine of amplitude A: |X| = A n / 4 */
}

static void tone(int16_t *x, size_t n, float rate, float f, float amp)
{
    for (size_t i = 0; i < n; i++) x[i] = (int16_t)(amp * sin(2 * M_PI * f * i / rate));
}

int main(int argc, char **argv)
{
    enum { N = 16000 };
    static int16_t in[N], out[2 * N]; static uint8_t bits[N / 2];
    float db, share;

    for (float f = 300; f <= 3300; f += 1000) {                 /* speech's band through CVSD at -15 dBFS */
        struct cvsd e, d; cvsd_init(&e); cvsd_init(&d);
        tone(in, N, 16000, f, 5800);
        size_t nb = cvsd_encode(&e, in, N, bits), ns = cvsd_decode(&d, bits, nb, out);
        CHECK(nb == N / 2 && ns == N, "%.0f Hz: %zu bytes, %zu samples", f, nb, ns);
        float idb; measure(in + 4000, 8000, 16000, f, &idb, &share);
        measure(out + 4000, 8000, 16000, f, &db, &share);
        CHECK(fabsf(db - idb) < 1.5f && share > 0.85f, "%.0f Hz: in %.1f dBFS, out %.1f dBFS, %.0f %% the tone", f, idb, db, 100 * share);
    }
    {   /* silence codes as CVSD's idle pattern, alternating bits, and decodes quiet */
        struct cvsd e, d; cvsd_init(&e); cvsd_init(&d);
        memset(in, 0, sizeof in);
        size_t nb = cvsd_encode(&e, in, N, bits); int alt = 0;
        for (size_t i = 100; i < nb; i++) alt += bits[i] == 0x55 || bits[i] == 0xaa;
        cvsd_decode(&d, bits, nb, out); measure(out + 4000, 8000, 16000, 1000, &db, &share);
        CHECK(alt > (int)(nb - 100) * 9 / 10 && db < -60, "silence: %d of %zu bytes idle, out %.1f dBFS", alt, nb - 100, db);
    }
    {   /* 2:1 both ways at 8 kHz: 1 kHz passes, 6 kHz (over the new Nyquist) does not come back */
        struct fir up, dn; fir_init(&up, 2, 16000, 3600, 96); fir_init(&dn, 2, 16000, 3600, 96);
        static float mid[2 * N]; static int16_t n8[N], back[N];
        tone(n8, 8000, 8000, 1000, 9000);
        size_t m = fir_up(&up, n8, 8000, mid);
        for (size_t i = 0; i < m; i++) out[i] = (int16_t)lrintf(mid[i]);
        measure(out + 4000, 8000, 16000, 1000, &db, &share);
        CHECK(m == 16000 && fabsf(db - -14.2f) < 1.0f && share > 0.95f, "8 -> 16 kHz: 1 kHz at %.1f dBFS, %.0f %%", db, 100 * share);
        tone(in, N, 16000, 6000, 9000);
        for (int i = 0; i < N; i++) mid[i] = in[i];
        size_t k = fir_down(&dn, mid, N, back);
        measure(back + 1000, 6000, 8000, 2000, &db, &share);    /* 6 kHz folds to 2 kHz */
        CHECK(k == N / 2 && db < -50, "16 -> 8 kHz: 6 kHz left at %.1f dBFS", db);
    }
    if (argc > 1) {   /* the air's CVSD from the Echo Dot 5's chip (a 500 Hz tone at -15 dBFS), as the Dot 2's /dev/ebc gave it */
        FILE *f = fopen(argv[1], "rb"); unsigned char u[32]; size_t ns = 0; int bad = 0;
        struct cvsd d; cvsd_init(&d);
        while (f && fread(u, 1, 32, f) == 32 && ns + 60 <= 2 * N) { bad += u[30] != 1; cvsd_decode(&d, u, 30, out + ns); ns += 60; }
        if (f) fclose(f);
        measure(out + 2000, ns - 2000, 16000, 500, &db, &share);
        CHECK(f && ns > 8000 && !bad && share > 0.9f && fabsf(db - -15.5f) < 2, "a real chip's CVSD: %zu samples, %d packets lost, 500 Hz %.0f %% at %.1f dBFS", ns, bad, 100 * share, db);
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
