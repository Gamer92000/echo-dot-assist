/* RNNoise on the 16 kHz mic stream: see micdenoise.h.
 *
 * RNNoise runs at 48 kHz in frames of 480, so every 160 samples go up by 3 (zero stuffing + low-pass), through
 * rnnoise_process_frame, and down by 3 again (the same low-pass, every third sample).  Its bands above 8 kHz stay empty,
 * which it copes with: the gains are per band.
 *
 * Level: RNNoise was trained on speech between full scale and 40 dB below it and ours arrives at -55 dBFS, but raising
 * the stream ahead of it (by 12, 24, 36 dB, and back down behind it) changed nothing measurable, so it gets it as it is.
 *
 * Left alone RNNoise takes the floor between words to digital silence (-65 to below -108 dBFS on the captures), and it
 * cuts into the weak sounds of quiet speech.  So the reduction is capped: the output is RNNoise's plus the input of the
 * same moment (RNNoise delays by one frame) at minus the cap.  By ear (user, a quiet sentence over the Echo's room
 * noise, 2026-09-30): artefacts at 18 dB, fine at 6, 9 and 12, which became the three strengths. */
#include "micdenoise.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/rnnoise/rnnoise.h"

#define UP 3
#define F48 (MICDENOISE_FRAME * UP)
#define TAPS 96                         /* low-pass at 48 kHz: pass to 7 kHz, -60 dB from 9 kHz (Blackman) */

static float lp[TAPS];                  /* the same for every instance (init, once) */

struct micdenoise {
    float up_hist[TAPS / UP - 1];       /* the last input samples: the zero-stuffed signal only has every third */
    float down_hist[TAPS - 1];
    float dry[F48];                     /* the frame RNNoise was given before this one: what its output belongs to */
    int16_t pend[MICDENOISE_FRAME]; size_t npend;
    DenoiseState *st;
    float speech, keep;                 /* keep: how much of the input stays, 10^(-cap / 20) */
};
static struct micdenoise pipe;          /* the voice pipeline's (micdenoise_reset/_run/_speech); Drop In has its own */

static void init(void)
{
    double sum = 0;
    for (int i = 0; i < TAPS; i++) {
        double t = i - (TAPS - 1) / 2.0, x = M_PI * t / UP;                     /* cutoff 8 kHz = fs / (2 * UP) */
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (TAPS - 1)) + 0.08 * cos(4 * M_PI * i / (TAPS - 1));
        lp[i] = (float)((fabs(t) < 1e-9 ? 1.0 : sin(x) / x) * w);
        sum += lp[i];
    }
    for (int i = 0; i < TAPS; i++) lp[i] = (float)(lp[i] / sum);
}

void md_reset(struct micdenoise *d, float max_db)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init);          /* the pipeline's and Drop In's threads: whichever comes first */
    d->keep = powf(10.0f, -max_db / 20.0f);
    if (d->st) rnnoise_destroy(d->st);  /* rnnoise_init on a used state would leak its three buffers */
    d->st = rnnoise_create(NULL);
    memset(d->up_hist, 0, sizeof d->up_hist); memset(d->down_hist, 0, sizeof d->down_hist); memset(d->dry, 0, sizeof d->dry);
    d->npend = 0; d->speech = 0;
}

struct micdenoise *md_new(float max_db)
{
    struct micdenoise *d = calloc(1, sizeof *d);
    if (d) md_reset(d, max_db);
    return d;
}

void md_free(struct micdenoise *d)
{
    if (!d) return;
    if (d->st) rnnoise_destroy(d->st);
    free(d);
}

float md_speech(const struct micdenoise *d) { return d->speech; }
void micdenoise_reset(float max_db) { md_reset(&pipe, max_db); }
float micdenoise_speech(void) { return md_speech(&pipe); }

static void frame(struct micdenoise *d, const int16_t *in, int16_t *out)
{
    enum { H = TAPS / UP - 1 };
    float x[H + MICDENOISE_FRAME], a[F48], b[TAPS - 1 + F48];
    memcpy(x, d->up_hist, sizeof d->up_hist);
    for (int i = 0; i < MICDENOISE_FRAME; i++) x[H + i] = in[i];
    memcpy(d->up_hist, x + MICDENOISE_FRAME, sizeof d->up_hist);
    /* up: output sample 3 i + p is the sum over the input samples that fall under the filter, taps p, p + 3, ... */
    for (int i = 0; i < MICDENOISE_FRAME; i++)
        for (int p = 0; p < UP; p++) {
            float s = 0;
            for (int k = 0; k <= H; k++) s += lp[p + UP * k] * x[H + i - k];
            a[UP * i + p] = s * UP;
        }
    memcpy(b, d->down_hist, sizeof d->down_hist);
    d->speech = rnnoise_process_frame(d->st, b + TAPS - 1, a);
    for (int i = 0; i < F48; i++) b[TAPS - 1 + i] = b[TAPS - 1 + i] * (1 - d->keep) + d->dry[i] * d->keep;
    memcpy(d->dry, a, sizeof d->dry);
    memcpy(d->down_hist, b + F48, sizeof d->down_hist);
    if (!out) return;
    for (int i = 0; i < MICDENOISE_FRAME; i++) {
        float s = 0;
        for (int k = 0; k < TAPS; k++) s += lp[k] * b[UP * i + TAPS - 1 - k];
        out[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)lrintf(s);
    }
}

size_t md_run(struct micdenoise *d, const int16_t *in, size_t n, int16_t *out)
{
    size_t made = 0;
    if (!d->st) md_reset(d, 12);
    while (n) {
        size_t k = MICDENOISE_FRAME - d->npend; if (k > n) k = n;
        memcpy(d->pend + d->npend, in, k * sizeof *in);
        d->npend += k; in += k; n -= k;
        if (d->npend < MICDENOISE_FRAME) break;
        frame(d, d->pend, out ? out + made : NULL);
        made += MICDENOISE_FRAME; d->npend = 0;
    }
    return out ? made : 0;
}

size_t micdenoise_run(const int16_t *in, size_t n, int16_t *out) { return md_run(&pipe, in, n, out); }
