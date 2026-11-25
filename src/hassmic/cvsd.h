/* Bluetooth's CVSD (Core spec Vol 2 Part B 9.2) in software, for the Echo Dot 2 (biscuit): its integrated Bluetooth hands
 * hassmic the air bitstream of a call (scoaudio.c, /dev/ebc; docs/re-hfp.md), which the stock HAL codes with
 * libcvsd_mtk.so.  64 kbit/s, one bit a sample at 64 kHz, least significant bit of each byte first (the air's order,
 * measured against the Echo Dot 5's chip both ways).  hassmic's side is 16 kHz (call.c): a byte is two samples.
 * And the FIR filters around it and the 8 kHz paths' 2:1 (fir_up/fir_down). */
#ifndef CVSD_H
#define CVSD_H
#include <stddef.h>
#include <stdint.h>

#define FIR_TAPS 128                    /* the most a filter here has */

/* Integer-ratio interpolation and decimation, L = 2 or 4: a windowed sinc (Hamming) of TAPS taps at the higher rate,
 * -6 dB at FC Hz.  Its transition is ~4 rate_high / taps wide: 8 <-> 16 kHz keeps speech's band (300..3400 Hz) and
 * stops what folds over 4 kHz with 3600 Hz and 96 taps; 16 <-> 64 kHz (CVSD) need only stop over 8 kHz, so 4400 Hz
 * and 128 taps keep 3.4 kHz flat */
struct fir { int L, taps; float h[FIR_TAPS]; float x[FIR_TAPS]; int pos; };
void   fir_init(struct fir *f, int L, float rate_high, float fc, int taps);
size_t fir_up(struct fir *f, const int16_t *in, size_t n, float *out);        /* n in -> n * L out */
size_t fir_down(struct fir *f, const float *in, size_t n, int16_t *out);     /* n in (a multiple of L) -> n / L out */

struct cvsd { float y, delta; unsigned hist; struct fir f; };
void   cvsd_init(struct cvsd *c);
/* 16 kHz s16 -> the bitstream: n samples (even) -> n / 2 bytes */
size_t cvsd_encode(struct cvsd *c, const int16_t *pcm, size_t n, uint8_t *out);
/* the bitstream -> 16 kHz s16: n bytes -> 2 n samples */
size_t cvsd_decode(struct cvsd *c, const uint8_t *in, size_t n, int16_t *pcm);
#endif
