/*
 * The music's PCM on its way out (sendspin.c, a2dp.c's sink): its rate, and its duck under the voice assistant.
 *
 * Music played a hair faster or slower than it came, to stay in step with a clock that is not the output's (sendspin.c:
 * the server's schedule).  An output clock off by a constant (the Echo Show 5's: ~350 ppm, 2026-11-02) needs a constant
 * rate; corrected by frames skipped or repeated at one point it ticked several times a second, squeezed into one chunk
 * at a time (0.5 % for 25 ms) it was a pitch blip as often.  Here the rate follows the drift: a servo on the sync error
 * sets it, and a resampler plays at it continuously, its position and last frames carried from chunk to chunk.
 */
#ifndef HASSMIC_RESAMPLE_H
#define HASSMIC_RESAMPLE_H
#include <stddef.h>
#include <stdint.h>

/* Stereo s16 by cubic interpolation (Catmull-Rom: through every input frame; at 1 kHz within 2e-5 of the true sine, at
 * 8 kHz within 0.3 dB, where linear interpolation dips the treble by up to 1.3 dB each time the phase passes a half).
 * `step` = input frames per output frame: > 1 plays faster (fewer frames out), < 1 slower; held to 0.98..1.02.
 * From a reset at step 1 every frame comes out as it went in.  The last two frames in wait for the next input (the
 * interpolation looks two ahead): rs_lag says how far back the next frame out lies. */
struct resampler { int16_t hist[3 * 2]; int nhist; double pos; };
void   rs_reset(struct resampler *r);
#define RS_OUT_MAX(frames) ((frames) + (frames) / 48 + 4)                 /* the most frames rs_run makes of `frames`: */
size_t rs_out_max(size_t frames);                                          /* frames / 0.98 + 1, rounded up */
size_t rs_run(struct resampler *r, const int16_t *in, size_t frames, double step, int16_t *out);
double rs_lag(const struct resampler *r);       /* the next frame out against the next input's first, in input frames:
                                                   -2 < lag <= 0, 0 after a reset */

/* The rate from the sync error, a PI loop.  The error is read through the output's queue, which the Echo Shows count
 * in 20 ms slots taken back in bursts: ~10 ms of noise a reading on the Show 5, averaged over SERVO_SMOOTH_S (2 s) first.  `drift` is what
 * the integral learnt of the clocks' difference; it is kept across snaps and streams (same output, same clock;
 * sendspin.c keeps one per route, the Echo's speaker and a Bluetooth one, in state/sendspin.drift). */
struct rate_servo { double smooth, drift, hold; };
#define RATE_DRIFT_MAX 0.002                    /* 2000 ppm: a crystal is within 100; a phone's clock was 0.2 % off */
double rate_servo(struct rate_servo *s, double err_us, double dt_s);   /* err_us > 0: early.  Returns the step */
double rate_servo_snap(struct rate_servo *s);   /* after a jump in the schedule: the error starts from 0, the drift is
                                                   left alone for SERVO_HOLD_S (what the snap left over is no drift); the step */

/* Stereo s16 turned down to target (settings_duck_gain(): the setting music_duck, 16 dB by default) while a request
 * runs, glided into and out of with a time constant of 30 ms, frame by frame: a step from one block to the next clicked
 * as listening started and ended.  *g is the stream's gain now (start at 1); at 1 and target 1 the samples are left as
 * they are. */
void   music_duck(int16_t *s, size_t frames, unsigned rate, float *g, float target);

/* s16 bytes copied at a gain glided towards target with the duck's 30 ms time constant, sample by sample (`per_s`:
 * samples a second, rate x channels; the buffers the backends get are cut at whole samples, not frames): a volume made
 * in the PCM (even volume steps, separate volumes; audio_*_gain) stepped from one buffer to the next, a click on the
 * Echo, and the phase jump tests/fake_ma_sendspin.py found in a tone (0.049 rad, a volume change in it).  *g is the
 * stream's gain now: the target when it opens.  At 1 and target 1 a plain copy; a lone odd byte passes as it is. */
void   gain_glide(char *dst, const char *src, size_t bytes, unsigned per_s, float *g, float target);
#endif
