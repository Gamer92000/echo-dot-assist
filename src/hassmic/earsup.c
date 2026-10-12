/* Wake sound removal: see earsup.h.  All numbers marked "guess" are to be measured on the Echos (PLAN.md). */
#include "earsup.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 16000
#define FR 512                              /* 32 ms frames, 50 % overlap, sqrt-Hann in and out */
#define HOP (FR / 2)
#define NB (FR / 2 + 1)
#define REF_MAX (RATE * 2)                  /* longer sounds (ringtones) are left alone: 2 s held would be too long */
/* The echo's delay after the mixer took the sound: the mixer's output buffer, speaker to mic, the front end, the capture
 * blocks.  Main.c's estimate is 85 ms; searched over this range (guess) */
#define D_MIN (-RATE * 50 / 1000)
#define D_MAX (RATE * 400 / 1000)
#define TAIL (RATE * 300 / 1000)            /* the room after the sound (guess) */
#define TAIL_DECAY 0.69f                    /* per hop (16 ms): -3.2 dB, 60 dB in 300 ms */
#define START_WAIT RATE                     /* the mixer can take ~0.4 s to pick a stream up (audio_mixer.c): give up after 1 s */
#define HOLD_MAX (START_WAIT + D_MAX + REF_MAX + TAIL + 2 * FR + 4096)
#define QSTEP 64                            /* 4 ms steps for the delay search: 32 ms frames hardly see less */
#define NQ ((REF_MAX + FR) / QSTEP + 2)
#define NBAND 96                            /* the sound's strongest bins, for the search */
#define KMAX (HOLD_MAX / HOP + 1)
/* below: no echo, or speech so much louder that it does not matter; nothing taken out.  The unit test's echo at -57
 * dBFS: 0.51 alone, 0.15 under speech at -35 dBFS (the delay still right), 0.11 at -30 (wrong); no echo: 0.03 at most */
#define MIN_MATCH 0.13f
#define QUANT 0.25f                         /* coupling = this quantile of mic over sound per bin: speech only adds */
#define ALPHA 2.0f                          /* the echo estimate counts double (3 dB) in the gain */
#define FLOOR 0.0316f                       /* a bin is taken down by 30 dB at most */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int enabled;
static struct { int on, started; uint64_t s0, s1; long len; float ref[REF_MAX]; } reg;    /* the sound, 16 kHz */
static int holding;                         /* audio from reg.s0 on is held: held_at .. + held_n */
static uint64_t held_at;
static size_t held_n;
static int16_t held[HOLD_MAX], out_buf[HOLD_MAX];
static float hf[HOLD_MAX], corr[HOLD_MAX];
static int last_delay; static float last_match, last_red; static int last_ok;

/* ---------------------------------------------------------------- FFT, 512 points */

static float cw[FR / 2], sw[FR / 2], win[FR];
static int rev[FR];

static void fft_init(void)
{
    static int done;
    if (done) return;
    for (int i = 0; i < FR / 2; i++) { cw[i] = (float)cos(2 * M_PI * i / FR); sw[i] = (float)-sin(2 * M_PI * i / FR); }
    for (int i = 0; i < FR; i++) {
        win[i] = (float)sqrt(0.5 - 0.5 * cos(2 * M_PI * i / FR));     /* periodic: in x out sums to 1 at 50 % */
        int r = 0;
        for (int b = 1, x = i; b < FR; b <<= 1, x >>= 1) r = r << 1 | (x & 1);
        rev[i] = r;
    }
    done = 1;
}

static void fft(float *re, float *im, int inv)
{
    for (int i = 0; i < FR; i++) if (i < rev[i]) { float t = re[i]; re[i] = re[rev[i]]; re[rev[i]] = t; t = im[i]; im[i] = im[rev[i]]; im[rev[i]] = t; }
    for (int len = 2; len <= FR; len <<= 1) {
        int half = len / 2, step = FR / len;
        for (int i = 0; i < FR; i += len)
            for (int k = 0; k < half; k++) {
                float wr = cw[k * step], wi = inv ? -sw[k * step] : sw[k * step];
                int a = i + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
            }
    }
    if (inv) for (int i = 0; i < FR; i++) { re[i] /= FR; im[i] /= FR; }
}

/* the windowed frame x[from .. from + FR), zeros outside [0, n) */
static void spec(const float *x, long n, long from, float *re, float *im)
{
    for (int i = 0; i < FR; i++) { long j = from + i; re[i] = j >= 0 && j < n ? x[j] * win[i] : 0; im[i] = 0; }
    fft(re, im, 0);
}

/* ---------------------------------------------------------------- the API */

void earsup_enable(int on) { pthread_mutex_lock(&mu); enabled = on != 0; pthread_mutex_unlock(&mu); }
int  earsup_enabled(void) { pthread_mutex_lock(&mu); int on = enabled; pthread_mutex_unlock(&mu); return on; }

void earsup_play(const int16_t *pcm, size_t n, unsigned rate, uint64_t now)
{
    pthread_mutex_lock(&mu);
    if (!enabled || holding || !rate || !n) { pthread_mutex_unlock(&mu); return; }    /* one at a time: the held one first */
    double step = rate / (double)RATE;
    long len = (long)(n / step);
    if (len > REF_MAX) { reg.on = 0; pthread_mutex_unlock(&mu); return; }
    for (long j = 0; j < len; j++) {                /* down: the mean over each output sample's span; up: linear */
        double t0 = j * step;
        if (step <= 1) {
            size_t i = (size_t)t0; double f = t0 - i;
            reg.ref[j] = (float)((pcm[i] * (1 - f) + (i + 1 < n ? pcm[i + 1] : 0) * f) / 32768);
        } else {
            size_t a = (size_t)t0, b = (size_t)(t0 + step); double s = 0;
            if (b > n) b = n;
            for (size_t i = a; i < b; i++) s += pcm[i];
            reg.ref[j] = (float)(b > a ? s / (b - a) / 32768 : 0);
        }
    }
    reg.len = len; reg.s0 = now; reg.started = 0; reg.on = 1;
    pthread_mutex_unlock(&mu);
}

void earsup_started(uint64_t now)
{
    pthread_mutex_lock(&mu);
    if (reg.on && !reg.started) { reg.started = 1; reg.s1 = now < reg.s0 ? reg.s0 : now; }
    pthread_mutex_unlock(&mu);
}

void earsup_reset(void) { pthread_mutex_lock(&mu); holding = 0; held_n = 0; pthread_mutex_unlock(&mu); }

int earsup_last(int *delay, float *match, float *reduction_db)
{
    pthread_mutex_lock(&mu);
    *delay = last_delay; *match = last_match; *reduction_db = last_red; int ok = last_ok;
    pthread_mutex_unlock(&mu);
    return ok;
}

/* where the held audio may end: the echo of the whole sound has passed, or the mixer never took it */
static uint64_t window_end(void) { return reg.started ? reg.s1 + D_MAX + reg.len + TAIL + FR : reg.s0 + START_WAIT; }

/* ---------------------------------------------------------------- the work, on the held audio (mu held) */

static void center(float (*m)[NBAND], int rows, int cols)  /* minus each row's mean and each column's */
{
    static float colm[NBAND];
    for (int j = 0; j < cols; j++) colm[j] = 0;
    for (int k = 0; k < rows; k++) {
        double s = 0;
        for (int j = 0; j < cols; j++) s += m[k][j];
        s /= cols;
        for (int j = 0; j < cols; j++) { m[k][j] -= (float)s; colm[j] += m[k][j]; }
    }
    for (int j = 0; j < cols; j++) colm[j] /= rows;
    for (int k = 0; k < rows; k++) for (int j = 0; j < cols; j++) m[k][j] -= colm[j];
}

static void process(void)
{
    static float re[FR], im[FR], rp[NB], rq[NQ][NBAND], ym[KMAX][NBAND], xd[KMAX][NBAND], xs[KMAX][NB], xr[KMAX][NB], ymag[KMAX][NB], g[NB], r[KMAX];
    static int band[NBAND];
    long n = (long)held_n, len = reg.len, base = (long)((int64_t)held_at - (int64_t)reg.s1);
    int K = n >= FR ? (int)((n - FR) / HOP + 1) : 0, nb = 0;
    for (long i = 0; i < n; i++) { hf[i] = held[i] / 32768.0f; corr[i] = 0; }
    last_ok = 0; last_match = 0; last_red = 0;

    /* the sound's bins: within 40 dB of its strongest, the strongest NBAND of them for the search */
    memset(rp, 0, sizeof rp);
    for (long q = 0; q < len; q += HOP) { spec(reg.ref, len, q, re, im); for (int b = 0; b < NB; b++) rp[b] += re[b] * re[b] + im[b] * im[b]; }
    float mx = 0; for (int b = 0; b < NB; b++) if (rp[b] > mx) mx = rp[b];
    for (int b = 0; b < NB && mx > 0; b++) {
        if (rp[b] < mx * 1e-4f) continue;
        if (nb < NBAND) band[nb++] = b;
        else { int w = 0; for (int j = 1; j < NBAND; j++) if (rp[band[j]] < rp[band[w]]) w = j; if (rp[b] > rp[band[w]]) band[w] = b; }
    }
    if (!nb || !K) { fprintf(stderr, "wake sound removal: nothing to compare (%ld samples held)\n", n); return; }

    /* the delay: log magnitudes in those bins, the sound against the mic, over the delay range in 2 ms steps.  Both
     * centred per frame and per bin before they are compared: speech raises whole frames (it is broadband) and is gone
     * then, the sound's pattern of tones over time is what is left to match.  Plain magnitudes lost the delay as soon as
     * speech was 7 dB louder than the echo (tests/unit/earsup_test.c) */
    int nq = (int)((len + FR) / QSTEP + 1);
    float xfl = 0;
    for (int i = 0; i < nq; i++) {
        spec(reg.ref, len, -FR + (long)i * QSTEP, re, im);
        for (int j = 0; j < nb; j++) { rq[i][j] = hypotf(re[band[j]], im[band[j]]); if (rq[i][j] > xfl) xfl = rq[i][j]; }
    }
    xfl *= 1e-3f;                                   /* 60 dB under the sound's peak counts as silence */
    for (int i = 0; i < nq; i++) for (int j = 0; j < nb; j++) rq[i][j] = logf(rq[i][j] > xfl ? rq[i][j] : xfl);
    float lfl = logf(xfl);
    for (int k = 0; k < K; k++) {                   /* the mic's magnitudes, kept for the coupling below */
        spec(hf, n, (long)k * HOP, re, im);
        for (int b = 0; b < NB; b++) ymag[k][b] = hypotf(re[b], im[b]);
        for (int j = 0; j < nb; j++) ym[k][j] = logf(ymag[k][band[j]] + 1e-7f);
    }
    center(ym, K, nb);
    float best = -2; long bd = 0;
    for (long d = D_MIN; d <= D_MAX; d += QSTEP) {
        for (int k = 0; k < K; k++) {
            long gi = (long)floor((base + (double)k * HOP - d + FR + 0.5 * QSTEP) / QSTEP);
            if (gi >= 0 && gi < nq) memcpy(xd[k], rq[gi], nb * sizeof **xd);
            else for (int j = 0; j < nb; j++) xd[k][j] = lfl;
        }
        center(xd, K, nb);
        double sxx = 0, syy = 0, sxy = 0;
        for (int k = 0; k < K; k++) for (int j = 0; j < nb; j++) { sxx += xd[k][j] * xd[k][j]; syy += ym[k][j] * ym[k][j]; sxy += xd[k][j] * ym[k][j]; }
        float r = sxx > 0 && syy > 0 ? (float)(sxy / sqrt(sxx * syy)) : 0;
        if (r > best) { best = r; bd = d; }
    }
    last_delay = (int)bd; last_match = best;
    if (best < MIN_MATCH) {
        fprintf(stderr, "wake sound removal: no echo of the sound found (match %.2f), %.2f s held\n", best, n / (double)RATE);
        return;
    }

    /* the sound as it reaches the mic, with the room after it */
    for (int k = 0; k < K; k++) {
        spec(reg.ref, len, base + (long)k * HOP - bd, re, im);
        for (int b = 0; b < NB; b++) { float m = xr[k][b] = hypotf(re[b], im[b]), t = k ? xs[k - 1][b] * TAIL_DECAY : 0; xs[k][b] = m > t ? m : t; }
    }
    /* coupling per bin: mic over sound in the frames where the sound itself is strong in it (not its modelled tail: a
     * short sound's tail frames pulled it down to a third, 7 dB taken off instead of 20 on the PC's 120 ms blip), the
     * low quantile (speech only adds) */
    float xall = 0; for (int k = 0; k < K; k++) for (int b = 0; b < NB; b++) if (xr[k][b] > xall) xall = xr[k][b];
    for (int b = 0; b < NB; b++) {
        float xm = 0; int c = 0;
        g[b] = 0;
        for (int k = 0; k < K; k++) if (xr[k][b] > xm) xm = xr[k][b];
        if (xm < xall * 0.01f) continue;                                    /* 40 dB under the sound's peak: left alone */
        for (int k = 0; k < K; k++) if (xr[k][b] >= 0.3f * xm) r[c++] = ymag[k][b] / xr[k][b];
        if (c < 2) continue;
        for (int i = 1; i < c; i++) for (int j = i; j > 0 && r[j - 1] > r[j]; j--) { float t = r[j]; r[j] = r[j - 1]; r[j - 1] = t; }
        g[b] = r[(int)(QUANT * (c - 1))];
    }

    /* the bins the echo dominates go down; only the change is added back (overlap-add of (gain - 1) x mic), so frames
     * left alone give back the held audio exactly */
    double before = 0, after = 0;
    for (int k = 0; k < K; k++) {
        int any = 0;
        spec(hf, n, (long)k * HOP, re, im);
        for (int b = 0; b < NB; b++) {
            float e = g[b] * xs[k][b], p = re[b] * re[b] + im[b] * im[b], gain = 1;
            if (e > 0) {
                gain = 1 - ALPHA * e * e / (p + 1e-12f);
                if (gain < FLOOR) gain = FLOOR;
                if (gain < 1) any = 1; else gain = 1;
                if (e * e > 0.1f * p) { before += p; after += gain * gain * p; }   /* where the echo counts */
            }
            float d = gain - 1;
            re[b] *= d; im[b] *= d;
            if (b && b < FR / 2) { re[FR - b] *= d; im[FR - b] *= d; }
        }
        if (!any) continue;
        fft(re, im, 1);
        for (int i = 0; i < FR; i++) { long j = (long)k * HOP + i; if (j < n) corr[j] += re[i] * win[i]; }
    }
    for (long i = 0; i < n; i++) {
        float v = (hf[i] + corr[i]) * 32768.0f;
        out_buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v));
    }
    last_red = before > 0 && after > 0 ? (float)(10 * log10(before / after)) : 0;
    last_ok = 1;
    fprintf(stderr, "wake sound removal: echo %.0f ms after the mixer took the sound (match %.2f), %.1f dB taken off where it "
            "dominates, %.2f s held\n", bd * 1000.0 / RATE, best, last_red, n / (double)RATE);
}

size_t earsup_run(const int16_t *in, size_t n, uint64_t at, const int16_t **out, int *held_sound)
{
    size_t pre = 0;
    *out = in; *held_sound = 0;
    pthread_mutex_lock(&mu);
    fft_init();
    if (!holding) {
        if (!reg.on || at + n <= reg.s0) { pthread_mutex_unlock(&mu); return n; }
        if (at >= window_end()) { reg.on = 0; pthread_mutex_unlock(&mu); return n; }   /* played while no audio went out */
        pre = reg.s0 > at ? (size_t)(reg.s0 - at) : 0;
        holding = 1; held_at = at + pre; held_n = 0;
    } else if (at != held_at + held_n || held_n + n > HOLD_MAX) {
        fprintf(stderr, "wake sound removal: the audio jumped, %.2f s held dropped\n", held_n / (double)RATE);
        holding = 0; reg.on = 0; pthread_mutex_unlock(&mu); return n;
    }
    memcpy(held + held_n, in + pre, (n - pre) * sizeof *in);
    held_n += n - pre;
    if (pre || held_at + held_n < window_end()) { pthread_mutex_unlock(&mu); return pre; }
    if (reg.started) process();
    else fprintf(stderr, "wake sound removal: the mixer never took the sound, %.2f s held\n", held_n / (double)RATE);
    if (!last_ok || !reg.started) memcpy(out_buf, held, held_n * sizeof *held);
    *out = out_buf; *held_sound = 1;
    size_t m = held_n;
    holding = 0; held_n = 0; reg.on = 0;
    pthread_mutex_unlock(&mu);
    return m;
}
