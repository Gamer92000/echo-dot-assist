/* call.c's G.711 and 2:1 rate change: ITU's code points (Sun's g711.c), every 16-bit value through each law and back
 * within its step, the round trip of speech-band tones at 16 kHz -> 8 kHz -> 16 kHz, and what the decimation leaves of a
 * tone above 4 kHz */
#include "call.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failed;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) failed = 1; }

/* stand-ins for what call.c's call side needs of the rest of hassmic: never called here */
void core_dropin_listen(int on) { (void)on; }
int voip_open(unsigned r, unsigned c) { (void)r; (void)c; return -1; }
int voip_write(const void *d, size_t n) { (void)d; (void)n; return -1; }
long long voip_queued_us(void) { return 0; }
void voip_close(void) {}
int callmic_present(void) { return 0; }
int callmic_open(void) { return -1; }
int callmic_read(const void **d) { (void)d; return -1; }
void callmic_close(void) {}
int core_own_sound(void) { return 0; }
int core_muted(void) { return 0; }

static double tone_db(const int16_t *x, size_t n, double f, double rate)    /* the tone's level, dBFS (rms) */
{
    double re = 0, im = 0;
    for (size_t i = 0; i < n; i++) { re += x[i] * cos(2 * M_PI * f * i / rate); im += x[i] * sin(2 * M_PI * f * i / rate); }
    return 20 * log10(sqrt(re * re + im * im) * 2 / n / sqrt(2) / 32768 + 1e-12);
}

int main(void)
{
    check(g711_alaw(0) == 0xd5 && g711_ulaw(0) == 0xff, "silence: A-law 0xd5, mu-law 0xff");
    check(g711_alaw_dec(0xd5) == 8 && g711_alaw_dec(0x55) == -8 && g711_ulaw_dec(0xff) == 0 && g711_ulaw_dec(0x7f) == 0, "their decodes");
    check(g711_alaw_dec(0xaa) == 32256 && g711_alaw_dec(0x2a) == -32256, "A-law's ends: +-32256");
    check(g711_ulaw_dec(0x80) == 32124 && g711_ulaw_dec(0x00) == -32124, "mu-law's ends: +-32124");
    check(g711_alaw(32767) == 0xaa && g711_alaw(-32768) == 0x2a && g711_ulaw(32767) == 0x80 && g711_ulaw(-32768) == 0x00, "full scale clips there");
    int worst_a = 0, worst_u = 0, mono = 1, prev_a = -40000, prev_u = -40000;
    for (int v = -32768; v <= 32767; v++) {
        int a = g711_alaw_dec(g711_alaw((int16_t)v)), u = g711_ulaw_dec(g711_ulaw((int16_t)v));
        /* half a step at most: 1/32 of the value (a segment's 16 steps; mu-law's segments start 132 under a power of
         * two, so 1/28 there, and a negative value, shifted to 14 bits before its sign is taken as Sun's code does, is
         * up to 8 further off), or the smallest steps near 0 */
        int ea = abs(a - v) - (abs(v) / 32 > 8 ? abs(v) / 32 : 8), eu = abs(u - v) - (abs(v) / 28 > 8 ? abs(v) / 28 : 8) - 8;
        if (ea > worst_a && abs(v) <= 32256) worst_a = ea;
        if (eu > worst_u && abs(v) <= 32124) worst_u = eu;
        if (a < prev_a || u < prev_u) mono = 0;
        prev_a = a; prev_u = u;
    }
    printf("     worst error past half a step: A-law %d, mu-law %d\n", worst_a, worst_u);
    check(worst_a <= 0 && worst_u <= 0, "every value within half its law's step");
    check(mono, "both laws keep the order of values");

    enum { N = 16000 };
    static int16_t in[N], mid[N / 2], out[N];
    for (int k = 0; k < 4; k++) {
        double f = (double[]){ 300, 1000, 2500, 3400 }[k];
        struct half d = { { 0 }, 0 }, u = { { 0 }, 0 };
        for (int i = 0; i < N; i++) in[i] = (int16_t)lrint(16000 * sin(2 * M_PI * f * i / 16000));
        for (int i = 0; i < N; i += 320) { half_down(&d, in + i, 320, mid + i / 2); half_up(&u, mid + i / 2, 160, out + i); }
        double l_in = tone_db(in + 4000, 8000, f, 16000), l_out = tone_db(out + 4000, 8000, f, 16000);
        char w[96]; snprintf(w, sizeof w, "%.0f Hz through 8 kHz and back: %+.2f dB", f, l_out - l_in);
        check(fabs(l_out - l_in) < (f > 3000 ? 1.0 : 0.3), w);
    }
    {
        struct half d = { { 0 }, 0 };
        for (int i = 0; i < N; i++) in[i] = (int16_t)lrint(16000 * sin(2 * M_PI * 5000 * i / 16000));
        for (int i = 0; i < N; i += 320) half_down(&d, in + i, 320, mid + i / 2);
        double alias = tone_db(mid + 2000, 4000, 3000, 8000) - tone_db(in + 4000, 8000, 5000, 16000);  /* 5 kHz folds onto 3 */
        char w[96]; snprintf(w, sizeof w, "5 kHz is gone at 8 kHz (its alias at 3 kHz: %.1f dB)", alias);
        check(alias < -50, w);
    }
    return failed;
}
