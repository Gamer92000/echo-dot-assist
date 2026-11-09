/* resample.c (make unit-resample): the resampler at step 1 from a reset hands every frame through as it is; any chunk
 * sizes give what one chunk gives; a sine resampled by 350 ppm is the true sine at the new rate; rs_lag says where the
 * next frame out lies; rs_out_max holds.  The servo against a model of the Echo Show 5's queue (20 ms slots, polled
 * every 5 ms, up to 5 ms of jitter on top) with the output's clock off, 6 min: the error stays within a few ms without
 * a resync, the drift is learnt, the rate never jumps. */
#include "resample.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R 48000
static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static unsigned lcg = 1;
static unsigned rnd(unsigned n) { lcg = lcg * 1103515245u + 12345u; return (lcg >> 8) % n; }

/* in -> out through chunks of 1..maxc frames (maxc 0: one chunk) at `step`; returns frames out */
static size_t run(const int16_t *in, size_t frames, double step, size_t maxc, int16_t *out)
{
    struct resampler rs; rs_reset(&rs);
    size_t done = 0, n = 0;
    while (done < frames) {
        size_t c = maxc ? 1 + rnd((unsigned)maxc) : frames - done;
        if (c > frames - done) c = frames - done;
        n += rs_run(&rs, in + done * 2, c, step, out + n * 2);
        done += c;
    }
    return n;
}

static void resampler(void)
{
    const size_t N = 2 * R;
    int16_t *in = malloc(N * 4), *a = malloc(rs_out_max(N) * 4), *b = malloc(rs_out_max(N) * 4);
    for (size_t i = 0; i < 2 * N; i++) in[i] = (int16_t)((int)rnd(65536) - 32768);

    size_t n = run(in, N, 1, 3000, a);
    check(n == N - 2 && !memcmp(a, in, n * 4), "step 1 from a reset: every frame as it is (the last two wait for more)");

    /* one chunk's position runs up to N, chunks' are taken back by whole frames: the same within one step of rounding */
    for (int k = 0; k < 2; k++) {
        double step = k ? 0.9996 : 1.0004; size_t maxc = k ? 2 : 3000;
        size_t n1 = run(in, N, step, 0, a), n2 = run(in, N, step, maxc, b);
        int same = n1 == n2;
        for (size_t i = 0; same && i < 2 * n1; i++) if (abs(a[i] - b[i]) > 1) same = 0;
        check(same, k ? "chunks of 1 and 2 frames at -400 ppm: what one chunk gives"
                      : "chunks of 1..3000 frames at +400 ppm: what one chunk gives");
    }

    /* a sine resampled: frame j out is the input at j * step */
    for (int hz = 1000; hz <= 8000; hz *= 8) {
        double step = 1.00035, worst = 0, amp = 16000;
        for (size_t i = 0; i < N; i++) in[2 * i] = in[2 * i + 1] = (int16_t)lrint(amp * sin(2 * M_PI * hz * (double)i / R));
        n = run(in, N, step, 1200, a);
        for (size_t j = 0; j < n; j++) {
            double want = amp * sin(2 * M_PI * hz * (double)j * step / R), d = fabs(a[2 * j] - want);
            if (d > worst) worst = d;
        }
        char what[160];
        double bound = hz == 1000 ? 2 : amp * 0.04;     /* 1 kHz: rounding; 8 kHz: the cubic's own error, -28 dB */
        snprintf(what, sizeof what, "%d Hz at +350 ppm: within %.0f of the true sine (%.1f, %.1f dB under it)", hz, bound, worst, 20 * log10(worst / amp));
        check(worst <= bound && n >= (size_t)(N / step) - 3 && n <= (size_t)(N / step) + 1, what);
    }

    /* a step of its own per call, as the Bluetooth sink's (10 ms blocks, up to 0.4 % off): each frame out where the
     * steps put it (the first of a call one of the call before's step on from the last) */
    {
        struct resampler rs; rs_reset(&rs);
        double amp = 16000, p = 0, worst = 0; size_t done = 0;
        for (size_t i = 0; i < N; i++) in[2 * i] = in[2 * i + 1] = (int16_t)lrint(amp * sin(2 * M_PI * 1000 * (double)i / R));
        for (int k = 0; done + 480 <= N; k++, done += 480) {
            double step = 1 + 0.004 * sin(2 * M_PI * k / 500);
            size_t m = rs_run(&rs, in + done * 2, 480, step, a);
            for (size_t j = 0; j < m; j++, p += step) {
                double d = fabs(a[2 * j] - amp * sin(2 * M_PI * 1000 * p / R));
                if (d > worst) worst = d;
            }
        }
        char what[120]; snprintf(what, sizeof what, "a step changing per call: 1 kHz within 2 of the true sine (%.1f)", worst);
        check(worst <= 2, what);
    }

    /* rs_lag: with a constant step from a reset, the next frame out is the input's frame (frames out) * step */
    {
        struct resampler rs; rs_reset(&rs);
        double step = 0.9993, worst = 0; size_t j0 = 0, outn = 0;
        check(rs_lag(&rs) == 0, "lag 0 after a reset");
        for (int k = 0; k < 2000; k++) {
            size_t c = 1 + rnd(1500);
            double d = fabs((double)j0 + rs_lag(&rs) - (double)outn * step);
            if (d > worst) worst = d;
            outn += rs_run(&rs, in, c, step, a);
            j0 += c;
        }
        char what[120]; snprintf(what, sizeof what, "rs_lag: the next frame out where it is (off by %.2g frames)", worst);
        check(worst < 1e-6, what);
    }

    /* rs_out_max, at either end of the steps and beyond (held to 0.98..1.02) */
    {
        int held = 1;
        for (double step = 0.95; step <= 1.05; step += 0.01) {
            struct resampler rs; rs_reset(&rs);
            for (int k = 0; k < 500; k++) {
                size_t c = 1 + rnd(5000), m = rs_run(&rs, in, c, step, a);
                if (m > rs_out_max(c)) held = 0;
            }
        }
        check(held, "never more frames out than rs_out_max");
    }
    free(in); free(a); free(b);
}

/* The Echo Show 5: the player waits until the queue it can see is at most 120 ms, which moves in 20 ms slots (polled
 * every 5 ms), reads the error, steers the rate and writes a 25 ms chunk through the resampler.  The output drains at
 * R * (1 + ppm) frames per second; the frame out at `written` plays at written / that. */
static void servo(double ppm)
{
    const double slot = 960, target = 0.120, poll = 0.005, rate_out = R * (1 + ppm * 1e-6), T = 360;
    static int16_t in[1200 * 2], out[1300 * 2];
    struct resampler rs; struct rate_servo sv = { 0 }; rs_reset(&rs);
    double t = 0, step = rate_servo_snap(&sv), lo = 1e9, hi = -1e9, worst_jump = 0, drift_sum = 0, rate_sum = 0, rate_sq = 0, prev = step;
    double steady = 0, far = 0; long long written = 0, j0 = 0; int late = 0, chunks = 0;
    double errs[16000]; int ne = 0;
    while (t < T) {
        double rep;
        for (;;) { rep = floor(rate_out * t / slot) * slot; if ((written - rep) / R <= target) break; t += poll; }
        double q = (written - rep) / R + rnd(5000) * 1e-6;
        double due = ((double)j0 + rs_lag(&rs)) / R, err = due - (t + q);
        if (chunks++) step = rate_servo(&sv, err * 1e6, 1200.0 / R);
        double truth = due - written / rate_out;                  /* when the next frame out is due against when it plays */
        if (ne < 16000 && chunks % 4 == 0) errs[ne++] = truth;
        if (fabs(step - prev) > worst_jump) worst_jump = fabs(step - prev);
        prev = step;
        if (t > T / 2) {
            if (truth < lo) lo = truth;
            if (truth > hi) hi = truth;
            drift_sum += sv.drift; rate_sum += 1 - step; late++;
            rate_sq += (step - 1 + sv.drift) * (step - 1 + sv.drift);
        }
        written += (long long)rs_run(&rs, in, 1200, step, out); j0 += 1200;
    }
    steady = (lo + hi) / 2;
    for (int i = 0; i < ne; i++) if (fabs(errs[i] - steady) > far) far = fabs(errs[i] - steady);
    /* followed: the rate's mean (the integral's share plus what the error's own share leaves on it, the slots' reading
     * being skewed) is the clock's, the error bounded */
    double rate = rate_sum / late * 1e6, learnt = drift_sum / late * 1e6, noise = sqrt(rate_sq / late) * 1e6;
    char what[240];
    snprintf(what, sizeof what, "output clock %+.0f ppm: followed at %+.0f (integral %+.0f), error within %.2f ms once it is (%.2f ms "
             "off at worst from the start), the rate's noise %.0f ppm rms, its largest step %.0f ppm",
             ppm, rate, learnt, (hi - lo) * 1e3, far * 1e3, noise, worst_jump * 1e6);
    /* far: the slots' skew settling in (~8 ms in this model at 0 ppm), and while a drift is learnt ~4 ms per 1000 ppm:
     * clear of a resync (SNAP_US, 20 ms) for any crystal (100 ppm) and the stress case of 1500 */
    check(fabs(rate - ppm) <= 0.05 * fabs(ppm) + 10 && hi - lo < 0.004 && far < 0.010 + fabs(ppm) * 4e-6 && noise < 600 && worst_jump < 3e-4, what);
}

/* music_duck: down to a sixth with a time constant of 30 ms, no step a click would be, exactly there and back */
#define DUCK (1.0f / 6)                                 /* the old fixed duck; any target does (setting music_duck) */
static void duck(void)
{
    static int16_t s[R * 2]; float g = 1; int smooth = 1, prev = 16000;
    for (size_t i = 0; i < R * 2; i++) s[i] = 16000;
    music_duck(s, R / 2, R, &g, DUCK);                                  /* 500 ms down */
    for (size_t i = 0; i < R / 2; i++) { if (abs(s[2 * i] - prev) > 12) smooth = 0; prev = s[2 * i]; }
    double at30 = s[2 * (R * 30 / 1000 - 1)] / 16000.0, want = DUCK + (1 - DUCK) * exp(-1);
    check(fabs(at30 - want) < 0.01 && g == DUCK && s[R - 2] == (int16_t)lrintf(16000 * DUCK),
          "music_duck: 30 ms in at 1/e of the way, at a sixth exactly after 500 ms");
    music_duck(s + R, R / 2, R, &g, 1);                                 /* and back up */
    prev = s[R - 2];
    for (size_t i = R / 2; i < R; i++) { if (abs(s[2 * i] - prev) > 12) smooth = 0; prev = s[2 * i]; }
    check(smooth, "music_duck: no frame more than 12 of 16000 from the last (a step clicked)");
    int16_t t[4] = { 123, -456, 32767, -32768 };
    music_duck(t, 2, R, &g, 1);
    check(g == 1 && t[0] == 123 && t[1] == -456 && t[2] == 32767 && t[3] == -32768, "music_duck: back at 1, the samples left as they are");
}

/* After a snap: what it missed by (the Echo Show 5: -4.8 ms a second after a stream's start) is no drift.  The drift
 * learnt stays through 4 s of it, the proportional share takes it out, and after the hold the integral learns again. */
static void hold(void)
{
    struct rate_servo sv = { 0 }; double dt = 0.025;
    for (int i = 0; i < 4000; i++) rate_servo(&sv, 0, dt);
    sv.drift = -350e-6;
    double step = rate_servo_snap(&sv), before = sv.drift;
    for (int i = 0; i < 160; i++) step = rate_servo(&sv, -4800, dt);           /* 4 s */
    char what[160];
    snprintf(what, sizeof what, "after a snap 4 s at -4.8 ms leave the drift alone (%+.0f -> %+.0f ppm), the rate catches up (%+.0f ppm)",
             before * 1e6, sv.drift * 1e6, (step - 1) * 1e6);
    check(sv.drift == before && step - 1 > 600e-6, what);
    for (int i = 0; i < 80; i++) rate_servo(&sv, -4800, dt);                   /* past the 5 s */
    check(sv.drift < before - 5e-6, "and after 5 s it learns again");
}

int main(void)
{
    resampler();
    hold();
    duck();
    servo(-350); servo(350); servo(0); servo(-1500);
    return fails ? 1 : 0;
}
