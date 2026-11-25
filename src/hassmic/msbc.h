/* mSBC on the air (HFP 1.7, 5.7.2): each 57-byte frame (sbc.h) behind a two-byte H2 header (0x01, then 0x08 0x38 0xc8
 * 0xf8 for the sequence 0..3) and one byte of padding: 60 bytes, 7.5 ms, 120 samples at 16 kHz; one 2-EV3 packet, or
 * any cut of the stream (the Dot 2's /dev/ebc hands it out in 30-byte pieces).  scoaudio.c's wideband calls. */
#ifndef MSBC_H
#define MSBC_H
#include "sbc.h"

#define MSBC_PACKET 60

struct msbc_tx { struct sbc_enc e; unsigned seq; };
void msbc_tx_init(struct msbc_tx *t);
void msbc_tx(struct msbc_tx *t, const int16_t *pcm, uint8_t *out);          /* MSBC_SAMPLES in -> MSBC_PACKET bytes */

/* What comes off the air, in pieces of any length; a stretch the link lost given as that many zero bytes (or left out:
 * then nothing stands in for it).  Frames are found by their H2 header and syncword wherever they start; every
 * MSBC_PACKET bytes without one, and every frame whose CRC fails, is a lost frame: the one before, fading (6 dB a
 * frame), so the call's clock keeps its pace */
struct msbc_rx { struct sbc s; uint8_t b[2 * MSBC_PACKET]; size_t n, skipped; int16_t last[MSBC_SAMPLES]; unsigned frames, lost; };
void   msbc_rx_init(struct msbc_rx *r);
/* out: room for (n / MSBC_PACKET + 2) * MSBC_SAMPLES samples; returns how many it wrote */
size_t msbc_rx(struct msbc_rx *r, const uint8_t *p, size_t n, int16_t *out);
#endif
