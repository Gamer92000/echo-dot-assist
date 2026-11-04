/* resample.h: a continuous rate for music (Sendspin's sync) */
#include "resample.h"
#include <math.h>
#include <string.h>

void rs_reset(struct resampler *r) { memset(r, 0, sizeof *r); }

size_t rs_out_max(size_t frames) { return RS_OUT_MAX(frames); }

double rs_lag(const struct resampler *r) { return r->nhist ? r->pos - r->nhist : 0; }

/* The input as one row: the frames kept from before (hist), then `in`.  pos is the next frame out's place in that row,
 * 1 <= pos < 2 between calls, so the frame before it (the cubic's first tap) is always there. */
size_t rs_run(struct resampler *r, const int16_t *in, size_t frames, double step, int16_t *out)
{
    if (!frames) return 0;
    step = step < 0.98 ? 0.98 : step > 1.02 ? 1.02 : step;
    if (!r->nhist) { r->hist[0] = in[0]; r->hist[1] = in[1]; r->nhist = 1; r->pos = 1; }   /* the first frame as its own tap */
    const long h = r->nhist, end = h + (long)frames;
#define AT(i, c) ((float)((i) < h ? r->hist[(i) * 2 + (c)] : in[((i) - h) * 2 + (c)]))
    size_t n = 0;
    for (long k; (k = (long)r->pos) + 2 < end; r->pos += step, n++) {
        float f = (float)(r->pos - (double)k);
        for (int c = 0; c < 2; c++) {
            float x0 = AT(k - 1, c), x1 = AT(k, c), x2 = AT(k + 1, c), x3 = AT(k + 2, c);
            float y = x1 + 0.5f * f * (x2 - x0 + f * (2 * x0 - 5 * x1 + 4 * x2 - x3 + f * (3 * (x1 - x2) + x3 - x0)));
            out[n * 2 + c] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : lrintf(y));   /* a cubic overshoots */
        }
    }
    /* keep from the frame before pos on: 2 or 3 frames (pos stopped within the last 2, or never got that far) */
    long k = (long)r->pos, keep = end - (k - 1);
    int16_t kept[3 * 2];
    for (long i = 0; i < keep; i++) { kept[i * 2] = (int16_t)AT(k - 1 + i, 0); kept[i * 2 + 1] = (int16_t)AT(k - 1 + i, 1); }
#undef AT
    memcpy(r->hist, kept, (size_t)keep * 2 * sizeof kept[0]);
    r->nhist = (int)keep; r->pos -= (double)(k - 1);
    return n;
}

/* Gains from a simulation of the Echo Show 5's queue (20 ms slots, polled every 5 ms, 5 ms of jitter on top) with its
 * clock 350 ppm off, 2026-11-04: the drift learnt to 10 % within a minute (20 s the Show 5's way, its output slow), the
 * error swinging over 2 ms once it is (the frame nudging before: 5.3 ms, and 0.5 % rate jumps six times a second, even
 * with no drift at all: the noise crossed its 400 us threshold), the rate's own noise ~280 ppm rms (0.5 cent), below
 * 1 Hz: inaudible.  tests/unit/resample_test.c runs the same model. */
#define SERVO_SMOOTH_S 0.25                     /* the error averaged over this long first */
#define SERVO_KP 0.2                            /* per s: 1 ms early plays 200 ppm slower */
#define SERVO_KI (SERVO_KP * SERVO_KP / 4)      /* critically damped: the drift learnt without overshoot */
#define RATE_MAX 0.005                          /* drift and error together: 0.5 % (8.6 cents), only while catching up */
/* After a snap the error is what the snap missed by, not drift: on the Echo Show 5 -4.8 ms a second after a stream's
 * start, which took the integral from -127 to -328 ppm in 4 s (2026-11-04); the proportional share alone takes it out */
#define SERVO_HOLD_S 5.0

double rate_servo(struct rate_servo *s, double err_us, double dt)
{
    double e = err_us * 1e-6;
    s->smooth += dt / (SERVO_SMOOTH_S + dt) * (e - s->smooth);
    if (s->hold > 0) s->hold -= dt;
    else s->drift += SERVO_KI * s->smooth * dt;
    s->drift = s->drift > RATE_DRIFT_MAX ? RATE_DRIFT_MAX : s->drift < -RATE_DRIFT_MAX ? -RATE_DRIFT_MAX : s->drift;
    double c = SERVO_KP * s->smooth + s->drift;             /* > 0: early, so slower: fewer input frames per frame out */
    c = c > RATE_MAX ? RATE_MAX : c < -RATE_MAX ? -RATE_MAX : c;
    return 1 - c;
}

double rate_servo_snap(struct rate_servo *s) { s->smooth = 0; s->hold = SERVO_HOLD_S; return 1 - s->drift; }

void music_duck(int16_t *s, size_t frames, unsigned rate, float *g, float target)
{
    const float k = 1.0f / (0.030f * (float)rate);
    if (*g == 1 && target == 1) return;
    for (size_t i = 0; i < frames; i++) {
        *g += (target - *g) * k;
        if (fabsf(*g - target) < 1e-4f) *g = target;                /* there: exactly, so 1 leaves the samples alone */
        s[2 * i] = (int16_t)lrintf(s[2 * i] * *g); s[2 * i + 1] = (int16_t)lrintf(s[2 * i + 1] * *g);
    }
}
