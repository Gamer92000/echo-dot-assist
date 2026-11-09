/* Automatic gain for the pipeline's mic audio: see micgain.h.
 *
 * Per 10 ms frame: a floor tracker (follows drops at once, rises 3 dB/s); frames 9 dB above the floor count as speech
 * and feed the talker's active speech level: the power mean over speech frames only (pauses left out), like ITU-T P.56,
 * averaged over ~0.5 s.  The gain brings that level to the target, so the slider's value is what the pipeline gets.
 *
 * Not an upper envelope of the frames, as first written: how far such an envelope sits above the active level depends on
 * how peaky the speech is, +4 dB on the synthetic syllables of the unit test, -7 to +2 dB on the Echo's captures, so no
 * fixed offset could make the slider mean the output level.  A power mean needs no attack of its own: frames 20 dB
 * over it lift the mean by 5 dB each, 13 dB within 100 ms.  A capture that starts cold (no wake word to start from) with
 * its loudest word and fades by 15 dB within 2.5 s still ends up 6 dB under the target.
 *
 * The gain falls at once but rises only during speech and at 12 dB/s, so pauses and the room never pump up.  A peak
 * limiter per frame keeps loud close speech from clipping while the level catches up. */
#include "micgain.h"
#include <math.h>

#define FRAME 160
#define SPEECH_OVER_FLOOR 9.0f
#define FLOOR_MIN_DB -75.0f             /* digital silence (muted mics) is not a floor to aim at */
#define PEAK_MAX 29000.0f               /* about -1 dBFS */
#define LEVEL_AVG 0.02f                 /* per frame of speech: ~0.5 s.  On the captures at -26: 1 s -25.2..-27.0, 0.5 s
                                         * -26.2..-26.5 dBFS; shorter starts to even out the words of a sentence */

static float db_to_lin(float db) { return powf(10.0f, db / 20.0f); }
static float speech_db(const struct micgain *g) { return 10.0f * log10f(g->speech_pow + 1e-12f); }

static float wanted(const struct micgain *g)
{
    float d = (float)g->level - speech_db(g);
    return d < MICGAIN_MIN_DB ? MICGAIN_MIN_DB : d > MICGAIN_MAX_DB ? MICGAIN_MAX_DB : d;
}

void micgain_init(struct micgain *g, int level)
{
    g->level = level; g->hold = 0; g->limit_stays = 0; g->limit = 0; g->talker_min_db = -1000;
    g->speech_pow = 1e-5f; g->noise_db = -67.0f;               /* -50 dBFS */
    g->gain_db = wanted(g);
    g->applied = db_to_lin(g->gain_db);
}

float micgain_talker_db(const struct micgain *g) { return speech_db(g); }

void micgain_start(struct micgain *g, float keyword_db)
{
    if (keyword_db <= 0) g->speech_pow = powf(10.0f, keyword_db / 10.0f);      /* the keyword is nearly all speech */
    g->limit = 0;
    g->gain_db = wanted(g);
    g->applied = db_to_lin(g->gain_db);
}

static void frame(struct micgain *g, const int16_t *in, int16_t *out, size_t n)
{
    float sum = 0, peak = 1;
    for (size_t i = 0; i < n; i++) { float v = in[i]; sum += v * v; if (fabsf(v) > peak) peak = fabsf(v); }
    float p = sum / n / (32768.0f * 32768.0f), db = 10.0f * log10f(p + 1e-10f), step = (float)n / FRAME;

    if (g->hold) ;
    else if (db < g->noise_db) g->noise_db = db > FLOOR_MIN_DB ? db : FLOOR_MIN_DB;
    else g->noise_db += 0.03f * step;
    if (!g->hold && db > g->noise_db + SPEECH_OVER_FLOOR && db >= g->talker_min_db) {
        g->speech_pow += (p - g->speech_pow) * LEVEL_AVG * step;
        float w = wanted(g);
        if (w < g->gain_db) g->gain_db = w;
        else if (g->gain_db + 0.12f * step < w) g->gain_db += 0.12f * step;
        else g->gain_db = w;
    }
    float to = db_to_lin(g->gain_db);
    /* held, the level cannot bring the gain down, and a limit frame by frame would turn it up and down with every
     * syllable of someone talking over the sound (issue #82): limit_stays keeps the lowest, and only until the hold
     * ends, so a sound's own peak leaves nothing behind.  A talker then is on the level within 2 frames: a syllable's
     * dips at most stand more than 3 dB over the gain held (talkers 5-40 dB over the wake word, every phase) */
    if (!g->hold) g->limit = 0;
    else if (g->limit > 0 && g->limit < to) to = g->limit;
    if (peak * to > PEAK_MAX) {
        to = PEAK_MAX / peak;
        if (g->hold && g->limit_stays) g->limit = to;
    }
    /* up: ramp from the last frame's gain; down at once, or the frame's first samples clip (measured on the captures) */
    float from = to < g->applied ? to : g->applied, d = (to - from) / n;
    for (size_t i = 0; i < n; i++) {
        float v = in[i] * (from + d * (i + 1));
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)lrintf(v);
    }
    g->applied = to;
}

void micgain_run(struct micgain *g, const int16_t *in, int16_t *out, size_t n)
{
    while (n) {
        size_t k = n < FRAME ? n : FRAME;
        frame(g, in, out, k);
        in += k; out += k; n -= k;
    }
}
