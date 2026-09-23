/* SBC decoder (A2DP specification, appendix B / section 12), for the Bluetooth speaker. */
#ifndef SBC_H
#define SBC_H
#include <stddef.h>
#include <stdint.h>

struct sbc {
    unsigned rate, channels;                    /* of the last frame decoded */
    int nsb;                                    /* subbands the filter state belongs to; 0 = fresh */
    float v[2][160];                            /* synthesis filter state per channel (10 * 2M values, newest first) */
};

void sbc_init(struct sbc *s);
/* One frame from p (n bytes available).  Writes blocks * subbands samples per channel, interleaved, to out (room for
 * 16 * 8 * 2).  Returns the frame's length in bytes and sets *samples (per channel); 0 = not a whole valid frame. */
size_t sbc_decode(struct sbc *s, const uint8_t *p, size_t n, int16_t *out, unsigned *samples);
#endif
