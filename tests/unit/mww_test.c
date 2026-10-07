/* microWakeWord's front end and interpreter (mww.h) on 16 kHz s16le audio from stdin, for tests/unit/mww_ref.py:
 *   mww_test features            one line of 40 features per 10 ms window
 *   mww_test model <x.tflite>    per inference: the quantized input, then "=" and the probability in 256ths
 *   mww_test load <x.tflite>     loads it: "ok", or the error */
#include "mww.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct mww_model *load(const char *path)
{
    FILE *f = fopen(path, "rb"); static char buf[1 << 20]; char err[200];
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    struct mww_model *m = mww_model_load(buf, n, err, sizeof err);
    if (!m) printf("error: %s\n", err);
    return m;
}

int main(int argc, char **argv)
{
    static struct mww_features fe; struct mww_model *m = NULL;
    int16_t buf[160]; uint16_t feat[16][MWW_CHANNELS]; int8_t q[16 * MWW_CHANNELS]; size_t n; int have = 0, stride = 1;
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "load")) { m = load(argv[2]); if (m) printf("ok %d\n", mww_model_stride(m)); mww_model_free(m); return !m; }
    if (!strcmp(argv[1], "model") && !(m = load(argv[2]))) return 1;
    if (m) stride = mww_model_stride(m);
    if (mww_features_init(&fe)) return 1;
    while ((n = fread(buf, 2, 160, stdin)) > 0)
        for (size_t at = 0; at < n; ) {
            int done;
            at += mww_features_run(&fe, buf + at, n - at, feat[have], &done);
            if (!done) continue;
            if (!m) { for (int i = 0; i < MWW_CHANNELS; i++) printf("%u ", feat[0][i]); printf("\n"); continue; }
            if (++have < stride) continue;
            have = 0;
            mww_model_quantize(m, feat[0], q);
            for (int i = 0; i < stride * MWW_CHANNELS; i++) printf("%d ", q[i]);
            printf("= %d\n", mww_model_run(m, feat[0]));
        }
    mww_model_free(m);
    return 0;
}
