/* Where the wake word ends, by aligning its phones to the audio (issues #72, #79; main.c word_tail_end).
 *
 * Pryon's end is where it was sure, not where the word ends, and how far off it is depends on the model: against the
 * end that the Montreal Forced Aligner (MFA 3.4.3, german_mfa) finds with the whole sentence known, 220 German requests
 * (OmniVoice, two voices, a room; stock Pryon under qemu, 2026-10-29): Alexa ~100 ms before it, Computer 25 ms before,
 * Amazon at it, Echo 75 ms and Ziggy 20 ms after it, so a replay from there loses the command's start.  The level of
 * the audio cannot tell the word's last syllable from the command's first (wordtail.h), nor can rules on its spectrum
 * for words other than Alexa.  Within 40 ms of that end (or in the pause after it): Pryon's end 80 of 220, wordtail.c's
 * rule (the release's) about as many, this 158.
 *
 * What this does is MFA's first pass with only the wake word known: the word's phones (their triphones from MFA's
 * decision tree, the first after silence or at the start, the last before silence or straight into the rest) between
 * optional silences, then "spn" (MFA's model for unknown speech) for whatever follows, Viterbi with Kaldi's scales.
 * Features as the model was trained: Kaldi's MFCC (13 of 23 mel bins, 20-7800 Hz, pre-emphasis 0.97, Povey window,
 * lifter 22), the mean taken out (over the window: MFA's per-speaker mean or a long-term one made no difference),
 * 7 frames spliced, the model's LDA to 40.  The models (.wa, one per wake word and language spoken, main.c
 * align_resolve) are cut out of MFA's acoustic models offline (tools/wakealign/): ~40 of its ~6000 GMMs, each cut to
 * its 16 heaviest Gaussians, in float16 (as many right as all of them in float32; 12: 153), kept once per acoustic
 * model in a .was the words of that model share (1.5-3 KB a word, 0.3-0.8 MB a .was; a file a word was 6.6 MB in
 * all, now 2.5 MB, the ends the same in all 496 of a check).  On the same
 * clips this gives MFA's word end in 71 of 77, within 40 ms in all.  The window: 1.5 s before Pryon's end to 0.5 s
 * after it (the wait wordtail.c had; 0.3 s after: 140 of 220).
 *
 * The language is the speakers', not the wake word's: 75 English requests in two German voices (OmniVoice), the
 * firmware's en-US Pryon (which ends 205 ms after the word for Alexa, 70 for Computer and Echo): 23 right with
 * english_mfa, 48 with german_mfa (release's rule 18).  Tried and dropped: a phone loop after the word instead of spn,
 * with MFA's phone bigram and the first phone in context (151-161 of 220, at seven times the size).  Speaker
 * adaptation (below) on top: 163 of 220, 53 of 75, learnt request by request. */
#ifndef WAKEALIGN_H
#define WAKEALIGN_H
#include <stddef.h>
#include <stdint.h>

#define WAKEALIGN_BEFORE 24000          /* samples before Pryon's end the window starts: 1.5 s */
#define WAKEALIGN_AFTER 8000            /* and after it, where it ends: 0.5 s */

struct wakealign;

/* NULL (and why on stderr) if the file is missing or not one */
struct wakealign *wakealign_load(const char *path);
void wakealign_free(struct wakealign *wa);
/* pcm: n samples at 16 kHz.  Returns the sample (0 .. n) where the word ends, -1 if no alignment came out */
long wakealign_end(const struct wakealign *wa, const int16_t *pcm, size_t n);

/* Speaker adaptation (fMLLR; Gales 1998, Kaldi's fmllr-diag-gmm.cc): what a household's requests say about its
 * voices, room and microphone in one model's feature space (every word of one language shares it), and the transform
 * of the features estimated from it.  On the 220 German requests, each estimated from the other requests of the same
 * voice: 158 right -> 166 (from 5), 169 (from 20).  A request counts FORGET of the one after it (~50 remembered); the
 * transform comes in from ADAPT_MIN requests on.  tools/wakealign/walign.py Adapt is the reference */
#define WAKEALIGN_ADIM 40
#define WAKEALIGN_FORGET 0.98
#define WAKEALIGN_ADAPT_MIN 5
struct wakealign_adapt {
    uint64_t space;                     /* wakealign_space() of the models it is for */
    int requests, have;                 /* requests in; have: xform holds a transform */
    double beta, k[WAKEALIGN_ADIM][WAKEALIGN_ADIM + 1], g[WAKEALIGN_ADIM][WAKEALIGN_ADIM + 1][WAKEALIGN_ADIM + 1];  /* g: upper half */
    float xform[WAKEALIGN_ADIM][WAKEALIGN_ADIM + 1];
};
/* the feature space a model's statistics belong to (its LDA's hash; 0: one adaptation cannot be used with) */
uint64_t wakealign_space(const struct wakealign *wa);
/* as wakealign_end, the features through use->xform when use->have; add: this window's statistics go into it (its
 * space must be the model's).  Either may be NULL */
long wakealign_end_adapt(const struct wakealign *wa, const int16_t *pcm, size_t n, const struct wakealign_adapt *use,
                         struct wakealign_adapt *add);
/* the transform from add's statistics (10 rounds from the last one); 1: estimated, 0: too few requests or no solution */
int wakealign_estimate(struct wakealign_adapt *ad);
/* a household's statistics on the flash and back (statefile.c); 0 / -1, or the wrong space or file */
int wakealign_adapt_save(const struct wakealign_adapt *ad, const char *path);
int wakealign_adapt_load(struct wakealign_adapt *ad, const char *path);
#endif
