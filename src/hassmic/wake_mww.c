/* microWakeWord as a wake word engine (wake.h): features every 10 ms, an inference every stride (3) of them, and a
 * detection when the mean probability over the manifest's window passes its cutoff, as ESPHome decides it (the model's
 * uint8 output against cutoff x 255).  Runs in wake_feed, on the capture thread: 0.05 ms of CPU per 30 ms of audio on a
 * PC (okay_nabu, the largest); not measured on an Echo yet.  Features and probabilities come out bit for bit the same
 * on the Echo's bionic as on the PC (armv7 build under qemu, 13094 windows of a capture, 2026-10-07).
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

static struct mww_features fe;
static struct mww_model *model;
static wake_cb callback;
static char keyword[64];
static int stride, window, threshold, have, nprob, hold;
static uint8_t prob[MWW_WINDOW_MAX];
static uint16_t feat[16][MWW_CHANNELS];
static uint64_t fed;                    /* samples, on wake.h's clock */
static atomic_int reset_pending;

static void mww_close(void) { mww_model_free(model); model = NULL; }

static int mww_open(const char *manifest, wake_cb cb)
{
    struct mww_info i; char err[200], path[400], *slash; size_t n = 0; FILE *f; char *buf = NULL, *mf = NULL;
    mww_close();
    callback = cb; fed = wake_fed();
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
    if (!buf || !(model = mww_model_load(buf, n, err, sizeof err))) { fprintf(stderr, "microwakeword: %s: %s\n", path, buf ? err : "cannot read"); free(buf); return -1; }
    free(buf);
    if (mww_features_init(&fe)) { mww_close(); return -1; }
    snprintf(keyword, sizeof keyword, "%s", i.name);
    stride = mww_model_stride(model); window = i.window;
    threshold = (int)(i.cutoff * 255);
    have = nprob = 0; hold = HOLD_INFERENCES;
    fprintf(stderr, "microwakeword: \"%s\" loaded (cutoff %.2f over %d x %d ms)\n", keyword, i.cutoff, window, stride * 10);
    return 0;
}

static void mww_feed(const int16_t *s, size_t n)
{
    if (!model) { fed += n; return; }
    if (atomic_exchange(&reset_pending, 0)) { nprob = 0; hold = HOLD_INFERENCES; }
    while (n) {
        int done;
        size_t k = mww_features_run(&fe, s, n, feat[have], &done);
        s += k; n -= k; fed += k;
        if (!done || ++have < stride) continue;
        have = 0;
        int p = mww_model_run(model, feat[0]);
        if (p < 0) { fprintf(stderr, "microwakeword: the model broke, reloaded\n"); mww_model_reset(model); nprob = 0; continue; }
        memmove(prob + 1, prob, sizeof prob[0] * (MWW_WINDOW_MAX - 1));
        prob[0] = (uint8_t)p;
        if (nprob < window) nprob++;
        if (hold) { hold--; continue; }
        if (nprob < window) continue;
        int sum = 0;
        for (int i = 0; i < window; i++) sum += prob[i];
        if (sum <= threshold * window) continue;
        /* the window ends a feature window (30 ms) and up to window inferences after the word: begin a second before */
        fprintf(stderr, "wake: %s (microwakeword, mean %d/255)\n", keyword, sum / window);
        nprob = 0; hold = HOLD_INFERENCES;
        callback(keyword, fed > RATE ? fed - RATE : 0, fed);
    }
}

static void mww_reset(void) { atomic_store(&reset_pending, 1); }     /* any thread: the capture thread does it */
static void mww_property(const char *name, int value) { (void)name; (void)value; }
static int  mww_afe_times(long *start, long *end) { (void)start; (void)end; return 0; }

const struct wake_engine wake_mww = { mww_open, mww_feed, mww_reset, mww_property, mww_close, mww_afe_times };
