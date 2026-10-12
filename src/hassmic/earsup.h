/* Wake sound removal (experimental, setting "wake_sound_removal", off by default): takes what Amazon's echo canceller
 * leaves of our own UI sounds out of the mic audio sent to the voice pipeline, ahead of noise reduction and gain.
 *
 * Why: the canceller takes ~15 dB off the wake sound, and what is left in micAsr (-57 dBFS, 4-8 dB over the floor;
 * PLAN.md "Wake sound in the pipeline's audio") is as loud as a quiet talker.  Since the pipeline gets the audio from
 * the wake word's end (main.c stream_ring), people speak on without waiting, right over the sound.
 *
 * How: we know the samples we play, and roughly when.  The audio from the moment a sound is handed to the mixer is held
 * back until its echo has passed; then the delay is found (correlation of magnitude spectra), the echo's coupling per
 * frequency bin is read from this very sound (a low quantile over its frames: speech only adds energy), and the bins
 * the echo dominates are taken down (Wiener-like gain with a floor).  A linear canceller on top would have to adapt
 * during the talker's voice and could not model what the stock one leaves (mostly non-linear).  The wake sound is
 * tonal (stock ui_wakesound: 90 % of its energy in 8 of 5178 FFT bins), so speech keeps nearly all of its spectrum.
 * The held audio then goes out at once: Home Assistant buffers it, and the stream is live again after ~1 s.
 *
 * Threads: earsup_play/earsup_started from the earcon thread, the rest from the capture thread (core_lock held). */
#ifndef EARSUP_H
#define EARSUP_H
#include <stddef.h>
#include <stdint.h>

/* positions are on the capture clock: samples of micAsr since the start (main.c's ring counts so) */
void   earsup_enable(int on);
int    earsup_enabled(void);
/* a sound is about to be handed to the mixer; now: the capture clock.  pcm: mono s16 at rate, copied */
void   earsup_play(const int16_t *pcm, size_t n, unsigned rate, uint64_t now);
void   earsup_started(uint64_t now);        /* the mixer took it (audio.h audio_earcon_started) */
void   earsup_reset(void);                  /* a new pipeline: audio held for the last one is dropped */
/* 16 kHz mono in, at = its position; returns how many samples go on now, at *out (in itself, or held audio released
 * at once).  *held_sound: what goes on had one of our sounds in it (the gain holds for it).  n: at most 4096 */
size_t earsup_run(const int16_t *in, size_t n, uint64_t at, const int16_t **out, int *held_sound);
/* the last sound's result, for tests: delay of its echo after earsup_started in samples, match 0..1, dB taken off */
int    earsup_last(int *delay, float *match, float *reduction_db);
#endif
