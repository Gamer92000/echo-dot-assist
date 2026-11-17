/* wakealign.c on raw files, for tests/unit/wakealign_ref.py:
 *   wakealign_test <model.wa> <pcm.raw>                          the word's end (sample, -1 for none)
 *   wakealign_test adapt <model.wa> <stats out> <pcm.raw> ...     each in turn with the adaptation learnt so far, adding
 *                                                                 to it after, as main.c's worker does; then saved
 *   wakealign_test merge <model.wa> <pcm.raw> ...                 the same, each window's statistics gathered on their own
 *                                                                 and merged (main.c: once a request followed): "same"
 *                                                                 when ends and statistics agree with adding directly
 * exit 2 when the model does not load */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wakealign.h"

static struct wakealign_adapt ad, viam, one;

static int near(double a, double b) { double m = fabs(a) > fabs(b) ? fabs(a) : fabs(b); return fabs(a - b) <= 1e-9 * (m + 1e-6); }

int main(int argc, char **argv)
{
    if (argc >= 5 && !strcmp(argv[1], "adapt")) {
        struct wakealign *w = wakealign_load(argv[2]);
        if (!w) return 2;
        ad.space = wakealign_space(w);
        for (int i = 4; i < argc; i++) {
            static int16_t pcm[16000 * 10]; FILE *f = fopen(argv[i], "rb");
            if (!f) return 1;
            size_t n = fread(pcm, 2, sizeof pcm / 2, f); fclose(f);
            printf("%ld\n", wakealign_end_adapt(w, pcm, n, &ad, &ad));
            wakealign_estimate(&ad);
        }
        if (wakealign_adapt_save(&ad, argv[3])) return 1;
        struct wakealign_adapt back = { .space = ad.space };
        if (wakealign_adapt_load(&back, argv[3]) || memcmp(&back, &ad, sizeof ad)) return 3;
        wakealign_free(w);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "merge")) {
        struct wakealign *w = wakealign_load(argv[2]);
        if (!w) return 2;
        ad.space = viam.space = wakealign_space(w);
        int same = 1;
        for (int i = 3; i < argc; i++) {
            static int16_t pcm[16000 * 10]; FILE *f = fopen(argv[i], "rb");
            if (!f) return 1;
            size_t n = fread(pcm, 2, sizeof pcm / 2, f); fclose(f);
            long a = wakealign_end_adapt(w, pcm, n, &ad, &ad);
            memset(&one, 0, sizeof one); one.space = viam.space;
            long b = wakealign_end_adapt(w, pcm, n, &viam, &one);
            wakealign_adapt_merge(&viam, &one);
            wakealign_estimate(&ad); wakealign_estimate(&viam);
            same &= a == b && ad.requests == viam.requests && near(ad.beta, viam.beta);
            for (int r = 0; r < WAKEALIGN_ADIM; r++)
                for (int c = 0; c <= WAKEALIGN_ADIM; c++) {
                    same &= near(ad.k[r][c], viam.k[r][c]);
                    for (int l = c; l <= WAKEALIGN_ADIM; l++) same &= near(ad.g[r][c][l], viam.g[r][c][l]);
                }
        }
        printf("%s\n", same ? "same" : "different");
        wakealign_free(w);
        return !same;
    }
    if (argc != 3) return 1;
    struct wakealign *w = wakealign_load(argv[1]);
    if (!w) return 2;
    FILE *f = fopen(argv[2], "rb");
    if (!f) return 1;
    static int16_t pcm[16000 * 10];
    size_t n = fread(pcm, 2, sizeof pcm / 2, f);
    fclose(f);
    printf("%ld\n", wakealign_end(w, pcm, n));
    wakealign_free(w);
    return 0;
}
