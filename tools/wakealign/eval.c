/* The replay's start after a wake word, three ways, for tools/wakealign/corpus_eval.py: what the release did
 * (wordtail.c), the aligner alone (wakealign_end), and the aligner with the household's speaker adaptation as
 * main.c's align_thread runs it (the transform's end where it lies near Pryon's, else the plain one; what it settled
 * on added, the transform estimated anew).  Lines on stdin, one wake word each, a household's in the order it says them:
 *   <window.raw> <model.wa> <household> <pryon end in the window> <keyword dBFS>
 * the window being [Pryon's end - WAKEALIGN_BEFORE, + WAKEALIGN_AFTER] as align_ahead copies it.  Out, a line each:
 *   <release start> <aligner alone> <aligner adapted>       (samples in the window; -1: none)
 *   cc -O2 -Isrc/hassmic tools/wakealign/eval.c src/hassmic/wakealign.c src/hassmic/wordtail.c src/hassmic/statefile.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wakealign.h"
#include "wordtail.h"

#define NEAR_BEFORE 6400                /* main.c align_near: from 400 ms before Pryon's end to the window's end */

static struct { char who[64]; struct wakealign_adapt *ad; } homes[64];

static struct wakealign_adapt *home(const char *who, uint64_t space)
{
    for (int i = 0; i < 64; i++) {
        if (homes[i].ad && homes[i].ad->space == space && !strcmp(homes[i].who, who)) return homes[i].ad;
        if (!homes[i].ad) {
            snprintf(homes[i].who, sizeof homes[i].who, "%s", who);
            homes[i].ad = calloc(1, sizeof *homes[i].ad); homes[i].ad->space = space;
            return homes[i].ad;
        }
    }
    static int told;
    if (!told++) fprintf(stderr, "eval: more than 64 households and feature spaces, the rest without adaptation\n");
    return NULL;
}

int main(void)
{
    char line[1024], raw[512], wa[512], who[64]; long end; float kdb;
    static int16_t pcm[WAKEALIGN_BEFORE + WAKEALIGN_AFTER];
    char loaded[512] = ""; struct wakealign *w = NULL;
    while (fgets(line, sizeof line, stdin)) {
        if (sscanf(line, "%511s %511s %63s %ld %f", raw, wa, who, &end, &kdb) != 5) continue;
        FILE *f = fopen(raw, "rb"); size_t n = f ? fread(pcm, 2, sizeof pcm / 2, f) : 0;
        if (f) fclose(f);
        if (strcmp(wa, loaded)) { wakealign_free(w); w = wakealign_load(wa); snprintf(loaded, sizeof loaded, "%s", wa); }
        /* the release: wordtail_cut on the level after Pryon's end, all of its frames there */
        float db[WORDTAIL_FRAMES]; int k = 0; const char *why;
        for (; k < WORDTAIL_FRAMES && (size_t)(end + (k + 1) * WORDTAIL_FRAME) <= n; k++) {
            double s = 0;
            for (int i = 0; i < WORDTAIL_FRAME; i++) { double v = pcm[end + k * WORDTAIL_FRAME + i]; s += v * v; }
            db[k] = (float)(10 * log10(s / WORDTAIL_FRAME / (32768.0 * 32768.0) + 1e-10));
        }
        int cut = wordtail_cut(db, k, kdb, &why);
        long release = cut < 0 ? end : end + (long)cut * WORDTAIL_FRAME;
        long plain = -1, final = -1;
        if (w) {
            plain = wakealign_end(w, pcm, n);
            struct wakealign_adapt *ad = home(who, wakealign_space(w));
            long e = -1; int adapted = 0;
            #define NEAR(x) ((x) >= end - NEAR_BEFORE && (x) <= (long)n)
            if (ad && ad->have) { e = wakealign_end_adapt(w, pcm, n, ad, NULL); adapted = e >= 0 && NEAR(e); }
            if (!adapted) e = plain;
            final = e >= 0 && NEAR(e) ? e : -1;
            if (ad && final >= 0) { wakealign_end_adapt(w, pcm, n, adapted ? ad : NULL, ad); wakealign_estimate(ad); }
            if (plain >= 0 && !NEAR(plain)) plain = -1;
        }
        printf("%ld %ld %ld\n", release, plain, final);
        fflush(stdout);
    }
    wakealign_free(w);
    return 0;
}
