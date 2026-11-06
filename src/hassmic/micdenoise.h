/* Noise reduction for the mic audio sent to the voice pipeline: RNNoise (src/third_party/rnnoise, v0.1.1), ahead of the
 * gain (micgain.h).  Not for the wake word, whose model is tuned to the stock stream.
 *
 * Why: even with the front end in listening mode a quietly spoken command sits only 6-8 dB over the noise floor of
 * micAsr (-65 dBFS; captures of 2026-09-30), and the gain lifts that floor with the speech to about -32 dBFS.  Amazon's
 * ASR path has no noise reduction of its own (AFE.cfg: the NoiseReductor belongs to the VoIP path). */
#ifndef MICDENOISE_H
#define MICDENOISE_H
#include <stddef.h>
#include <stdint.h>

#define MICDENOISE_FRAME 160            /* 10 ms at 16 kHz: what goes in comes out in blocks of this */

/* A new stretch of audio starts: forget the last one.  max_db: the most the noise is taken down by (6, 9, 12). */
void micdenoise_reset(float max_db);
/* 16 kHz mono in, the same out, 12 ms later plus the wait for a full frame: returns the samples written to out, a multiple of MICDENOISE_FRAME that
 * can be 0; out must hold n + MICDENOISE_FRAME.  out == NULL: only listen (to settle on the room before a command). */
size_t micdenoise_run(const int16_t *in, size_t n, int16_t *out);
float micdenoise_speech(void);          /* RNNoise's speech probability of the last frame, 0..1 */

/* The same as an instance of its own (the functions above are the voice pipeline's): Drop In's mic, its gate deciding by
 * the speech probability (dropin.c says why) */
struct micdenoise;
struct micdenoise *md_new(float max_db);
void md_reset(struct micdenoise *d, float max_db);
size_t md_run(struct micdenoise *d, const int16_t *in, size_t n, int16_t *out);
float md_speech(const struct micdenoise *d);
void md_free(struct micdenoise *d);
#endif
