/* A call's audio (call.c), taken out of dropin.c: while a call is connected the media runs: the mic through the call's
 * gate, echo suppressor and gain, encoded 20 ms at a time (Opus) and handed to the owner's send(), which frames and
 * sends it; what comes back the owner hands to call_rx with its sequence number, into a jitter buffer that a playout
 * thread plays on the mixer's Voip stream. */
#ifndef CALL_H
#define CALL_H
#include <stddef.h>
#include <stdint.h>

#define CALL_RATE  16000
#define CALL_FRAME 320                  /* 20 ms at CALL_RATE: what the mic side encodes, the playout plays */
#define CALL_PKT   400                  /* the largest encoded frame (Opus at 24 kbit/s: ~60 bytes) */

/* The media of a connected call: send gets each encoded frame and its number (from 0), from the capture thread, no
 * lock of call.c's held.  The generation it returns ties call_rx and call_end to this media */
int  call_begin(void (*send)(const uint8_t *frame, size_t len, uint32_t seq, void *arg), void *arg);
void call_end(int gen);
/* A frame that came in: seq numbers the frames from the far end in order */
void call_rx(int gen, uint32_t seq, const uint8_t *frame, size_t len);
int  call_live(void);                   /* media runs */

/* What the media did, for the owner's log when it ends: frames in, played, played without their packet, came too late,
 * the longest wait between two packets (ms) */
struct call_stats { unsigned rx, played, lost, late; long long gap_max; };
void call_stats(struct call_stats *s);

/* The capture thread: 16 kHz mono micAsr as it comes, while a call runs (muted: zeros).  own: the Echo plays a sound or
 * a reply of its own: ducked like the far end's voice */
void call_mic(const int16_t *pcm, size_t n, int own);
float call_far_level(void);             /* the loudest of the far end that can be in the mic now, dBFS (-100: none) */
int  call_far_talking(void);            /* the other side's voice plays here now (or did, ECHO_MS ago) */

int  call_start(void);                  /* the playout and call microphone threads, once.  0 ok */
/* Where a call's microphone comes from: Amazon's call pipeline (audio.h callmic_*) where the backend has one, read by a
 * thread of call.c's own while the call's Voip stream is open, else the assistant's micAsr from call_mic.  set: 1 the
 * call pipeline (the default), 0 the assistant's, -1 reads; a call running keeps the one it started with until it ends,
 * except that switching to the assistant's takes effect at once */
int  call_mic_source(int set);
int  call_mic_hfp(void);                /* the call running now sends the call pipeline's */

#endif
