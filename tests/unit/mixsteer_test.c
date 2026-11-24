/* mixsteer.h (issue #225): the Dots' music queue model steered towards the mixer's own count.  A mixer of the test's:
 * it drains at R (1 + ppm), stops for a while if told (an xrun), and takes the stream in blocks of 10 ms on its own
 * clock, K ahead of what it plays: MixerGetNumBytes reads the rest, in steps.  The writer is sendspin.c's:
 * polls every 5 ms, writes 25 ms once the model says 120 ms or less.  What counts is how far the model is from what
 * truly waits: constant from the reference on, whatever the output does. */
#include <math.h>
#include <stdio.h>
#include "mixsteer.h"

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

static unsigned lcg = 1;
static unsigned rnd(unsigned n) { lcg = lcg * 1103515245u + 12345u; return (lcg >> 8) % n; }

#define BPS 192000.0                  /* 48 kHz stereo s16 */

struct sim { double ppm; double stall_at, stall_s; int stuck; double T; };
struct res { double lock_at, dev_settled, dev_all, fastest, shift; int moved_early; };

static struct res run(struct sim s)
{
    struct res r = { -1, 0, 0, 0, 0, 0 };
    struct mixsteer m; long long t0 = 0, now = 0, written = 0; double consumed = 0, ref = NAN, prev_t0 = 0, prev_at = 0;
    const double K = 0.060, blk = 0.010;                                    /* held behind the count; the mixer's blocks */
    ms_reset(&m, 1);
    t0 = 1; prev_t0 = 1;
    long long was = 1;
    for (now = 1; now < (long long)(s.T * 1e6); was = now, now += 5000 + rnd(400)) {   /* usleep(5000) and the loop's own */
        double t = now / 1e6, dt = (now - was) / 1e6, rate = 1 + s.ppm * 1e-6;
        if (!(t >= s.stall_at && t < s.stall_at + s.stall_s)) consumed += BPS * rate * dt;
        if (consumed > written) consumed = written;                           /* ran dry: silence, nothing taken */
        long long q = (long long)(written * 1e6 / BPS) - (now - t0);
        if (ms_due(&m, now)) {
            /* the mixer has taken what it plays within K, in whole blocks: the count is what it has not taken */
            double truth = (written - consumed) / BPS, pulled = ceil((consumed / BPS + K) / blk) * blk * BPS;
            double count = (written - (pulled < written ? pulled : written)) / BPS;
            if (s.stuck) count = 0;
            int was = m.locked;
            long long d = ms_update(&m, (long long)(count * 1e6), q, now);
            if (!was && d) r.moved_early = 1;
            t0 += d; q += d;
            if (m.locked > 0 && !was) { r.lock_at = t; ref = truth * 1e6 - q; prev_at = t; prev_t0 = (double)t0; }
        }
        if (m.locked > 0) {
            double dev = fabs((written - consumed) / BPS * 1e6 - q - ref) / 1000;    /* ms the model is off since the reference */
            if (dev > r.dev_all) r.dev_all = dev;
            if (t > r.lock_at + 3 * MS_TAU_US / 1e6 && (s.stall_s == 0 || t > s.stall_at + s.stall_s + 8 * MS_TAU_US / 1e6) && dev > r.dev_settled) r.dev_settled = dev;
            if (t - prev_at >= 1) {
                double v = fabs(t0 - prev_t0) / 1000 / (t - prev_at);             /* ms moved a second */
                if (v > r.fastest) r.fastest = v;
                prev_at = t; prev_t0 = (double)t0;
            }
        }
        if (q <= 120000) written += 4800;                                       /* 25 ms */
    }
    r.shift = m.shift / 1000.0;
    return r;
}

static void drift(double ppm)
{
    struct res r = run((struct sim){ .ppm = ppm, .T = 1800 });
    double lag = fabs(ppm) * 1e-6 * MS_TAU_US / 1000;                          /* ms a first-order follower trails a ramp by */
    char what[200];
    snprintf(what, sizeof what, "output %+.0f ppm, 30 min: reference at %.1f s, nothing moved before, the model off by %.2f ms "
             "once settled (%.2f ms lag expected), moved %+.1f ms in all", ppm, r.lock_at, r.dev_settled, lag, r.shift);
    double want = -ppm * 1e-6 * (1800 - r.lock_at) * 1000;                     /* + ppm drains faster: less queued */
    check(r.lock_at >= 10 && r.lock_at < 11 && !r.moved_early && r.dev_settled < lag + 1 && fabs(r.shift - want) < lag + 1, what);
}

int main(void)
{
    struct res r = run((struct sim){ .T = 1800 });
    char what[200];
    snprintf(what, sizeof what, "an exact output (the Dot 2's): moved %+.2f ms in 30 min, the model off by %.2f ms at worst", r.shift, r.dev_all);
    check(fabs(r.shift) < 1 && r.dev_all < 1, what);       /* the reference: the mean of ~400 steps of 10 ms, 0.6 ms off at worst in 8 seeds */

    drift(25); drift(-25); drift(350); drift(-1500);

    r = run((struct sim){ .stall_at = 300, .stall_s = 0.080, .T = 900 });
    snprintf(what, sizeof what, "the mixer stalls 80 ms at 5 min: followed to %.2f ms 160 s on (moved %+.1f ms), %.1f ms a second at the fastest",
             r.dev_settled, r.shift, r.fastest);
    check(r.dev_settled < 1 && fabs(r.shift - 80) < 5 && r.fastest <= 1000.0 * MS_GAP_MAX / MS_TAU_US + 0.5, what);

    r = run((struct sim){ .stall_at = 300, .stall_s = 0.5, .T = 900 });
    snprintf(what, sizeof what, "a 500 ms stall: never faster than %.0f ms a second (%.1f), followed all the same (%+.1f ms)",
             1000.0 * MS_GAP_MAX / MS_TAU_US, r.fastest, r.shift);
    check(r.fastest <= 1000.0 * MS_GAP_MAX / MS_TAU_US + 0.5 && fabs(r.shift - 500) < 5, what);   /* + a reading's dt across a window's edge */

    r = run((struct sim){ .stuck = 1, .T = 1800 });
    snprintf(what, sizeof what, "a count stuck at 0: the model alone (moved %+.2f ms)", r.shift);
    check(r.shift == 0 && r.lock_at < 0, what);
    return fails ? 1 : 0;
}
