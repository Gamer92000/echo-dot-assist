/* What Amazon's front end writes into the lowest bit of the micAsr samples (docs/re-vad.md): every 128-sample frame
 * carries a record, layout version 3.3 on both Dots (libasp.so's encoder 0xbcb90; stock's decoder
 * libAFEMetadataDecoder.so decodeMetadataVersion3_3).  hassmic reads its DNN VAD (vad_lite.tflite inside the front
 * end's "UED Fusion"): 2 bits, min(3, floor(4 p)), with no smoothing between the network and the record.  On the Echo
 * Dot 2's captures: 98-100 % of loud frames of a normal voice at >= 2, 82 % of a quiet one, 39 % whispered, 0-23 % for
 * sounds without words, 0 for the Echo's own replies.  It misses quiet distant speech (a sentence 7 dB over the floor
 * on the Dot 3 stayed at 0): it can tell that speech stopped, not that there was none. */
#ifndef AFEMETA_H
#define AFEMETA_H
#include <stdint.h>

#define AFEMETA_FRAME 128

struct afemeta {
    int vad;                            /* DNN VAD: 0 (p < .25) .. 3 (p >= .75) */
    int rms_db;                         /* the frame's level, dBFS */
    int playback;                       /* the Echo plays something */
    int muted;                          /* the mics are off */
};

/* 1 when the 128 samples at s are one frame carrying a 3.3 record (sync, version, magic and checksum), then filled in */
int afemeta_frame(const int16_t *s, struct afemeta *m);
#endif
