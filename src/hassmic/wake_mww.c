/* microWakeWord as a wake word engine (wake.h): features every 10 ms, an inference every stride (3) of them, and a
 * detection when the mean probability over the manifest's window passes its cutoff, as ESPHome decides it (the model's
 * uint8 output against cutoff x 255).  Runs in wake_feed, on the capture thread: 0.05 ms of CPU per 30 ms of audio on a
 * PC (okay_nabu, the largest); not measured on an Echo yet.  Features and probabilities come out bit for bit the same
 * on the Echo's bionic as on the PC (armv7 build under qemu, 13094 windows of a capture, 2026-10-07).  Each slot
 * (wake.h) runs a front end and a model of its own: two models cost twice that.
 *
 * Fed the same micAsr as Amazon's engine, without gain: microWakeWord's front end normalises the level (PCAN), and its
 * models hear an espeak "Alexa" / "Okay Nabu" / "Hey Mycroft" put at the Echo's keyword levels (-35 to -62 dBFS over a
 * -67 dBFS floor) the same with 0, +12 or +24 dB in front (window mean 229-255 of 255 throughout, 2026-10-07). */
#include "wake.h"
#include "mww.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 16000
#define HOLD_INFERENCES 33              /* ~1 s after opening and after a detection: no detection (ESPHome waits too) */

static struct mww_slot {
    struct mww_features fe;
    struct mww_model *model;
    char keyword[64];
    int stride, window, threshold, have, nprob, hold;
    uint8_t prob[MWW_WINDOW_MAX];
    uint16_t feat[16][MWW_CHANNELS];
    uint64_t fed;                       /* samples, on wake.h's clock */
    atomic_int reset_pending;
} slots[WAKE_SLOTS];
static wake_cb callback;

static void mww_close(int slot) { mww_model_free(slots[slot].model); slots[slot].model = NULL; }

static int mww_open(int slot, const char *manifest, wake_cb cb)
{
    struct mww_slot *m = &slots[slot];
    struct mww_info i; char err[200], path[400], *slash; size_t n = 0; FILE *f; char *buf = NULL, *mf = NULL;
    mww_close(slot);
    callback = cb; m->fed = wake_fed();
    if ((f = fopen(manifest, "r"))) {
        if ((mf = malloc(8193))) { n = fread(mf, 1, 8192, f); mf[n] = 0; }
        fclose(f);
    }
    if (!mf || mww_manifest_parse(mf, n, &i, err, sizeof err)) { fprintf(stderr, "microwakeword: %s: %s\n", manifest, mf ? err : "cannot read"); free(mf); return -1; }
    free(mf);
    snprintf(path, sizeof path, "%s", manifest);
    if ((slash = strrchr(path, '/'))) slash[1] = 0; else path[0] = 0;
    strncat(path, "model.tflite", sizeof path - strlen(path) - 1);
    if ((f = fopen(path, "rb"))) {
        if ((buf = malloc(MWW_MODEL_MAX))) n = fread(buf, 1, MWW_MODEL_MAX, f);
        fclose(f);
    }
    if (!buf || !(m->model = mww_model_load(buf, n, err, sizeof err))) { fprintf(stderr, "microwakeword: %s: %s\n", path, buf ? err : "cannot read"); free(buf); return -1; }
    free(buf);
    if (mww_features_init(&m->fe)) { mww_close(slot); return -1; }
    snprintf(m->keyword, sizeof m->keyword, "%s", i.name);
    m->stride = mww_model_stride(m->model); m->window = i.window;
    m->threshold = (int)(i.cutoff * 255);
    m->have = m->nprob = 0; m->hold = HOLD_INFERENCES;
    atomic_store(&m->reset_pending, 0);
    fprintf(stderr, "microwakeword: \"%s\" loaded (cutoff %.2f over %d x %d ms)\n", m->keyword, i.cutoff, m->window, m->stride * 10);
    return 0;
}

static void mww_feed(int slot, const int16_t *s, size_t n)
{
    struct mww_slot *m = &slots[slot];
    if (!m->model) { m->fed += n; return; }
    if (atomic_exchange(&m->reset_pending, 0)) { m->nprob = 0; m->hold = HOLD_INFERENCES; }
    while (n) {
        int done;
        size_t k = mww_features_run(&m->fe, s, n, m->feat[m->have], &done);
        s += k; n -= k; m->fed += k;
        if (!done || ++m->have < m->stride) continue;
        m->have = 0;
        int p = mww_model_run(m->model, m->feat[0]);
        if (p < 0) { fprintf(stderr, "microwakeword: the model broke, reloaded\n"); mww_model_reset(m->model); m->nprob = 0; continue; }
        memmove(m->prob + 1, m->prob, sizeof m->prob[0] * (MWW_WINDOW_MAX - 1));
        m->prob[0] = (uint8_t)p;
        if (m->nprob < m->window) m->nprob++;
        if (m->hold) { m->hold--; continue; }
        if (m->nprob < m->window) continue;
        int sum = 0;
        for (int i = 0; i < m->window; i++) sum += m->prob[i];
        if (sum <= m->threshold * m->window) continue;
        /* the window ends a feature window (30 ms) and up to window inferences after the word: begin a second before */
        fprintf(stderr, "wake: %s (microwakeword, mean %d/255%s)\n", m->keyword, sum / m->window, slot ? ", second wake word" : "");
        m->nprob = 0; m->hold = HOLD_INFERENCES;
        callback(slot, m->keyword, m->fed > RATE ? m->fed - RATE : 0, m->fed);
    }
}

static void mww_reset(int slot) { atomic_store(&slots[slot].reset_pending, 1); }     /* any thread: the capture thread does it */
static void mww_property(const char *name, int value) { (void)name; (void)value; }
static int  mww_afe_times(int slot, long *start, long *end) { (void)slot; (void)start; (void)end; return 0; }

const struct wake_engine wake_mww = { mww_open, mww_feed, mww_reset, mww_property, mww_close, mww_afe_times };
