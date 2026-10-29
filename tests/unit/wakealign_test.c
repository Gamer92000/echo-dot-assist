/* wakealign.c on raw files, for tests/unit/wakealign_ref.py:
 *   wakealign_test <model.wa> <pcm.raw>                          the word's end (sample, -1 for none)
 *   wakealign_test adapt <model.wa> <stats out> <pcm.raw> ...     each in turn with the adaptation learnt so far, adding
 *                                                                 to it after, as main.c's worker does; then saved
 * exit 2 when the model does not load */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wakealign.h"

static struct wakealign_adapt ad;

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
