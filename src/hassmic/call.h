/* A call's audio (call.c): what Drop In (dropin.c) and the phone (sip.c) share.  One call at a time on the Echo: whoever
 * starts one claims the call first (call_claim), from calling or ringing until it is over, so the other side refuses.
 * While it is connected the media runs: the mic through the call's gate, echo suppressor and gain, encoded 20 ms at a
 * time and handed to the owner's send(), which frames and sends it; what comes back the owner hands to call_rx with its
 * sequence number, into a jitter buffer that a playout thread plays on the mixer's Voip stream. */
#ifndef CALL_H
#define CALL_H
#include <stddef.h>
#include <stdint.h>

enum { CALL_NONE, CALL_DROPIN, CALL_PHONE };
/* Opus at 16 kHz (Drop In's, and the phone's wideband one), G.711 A-law and mu-law at 8 kHz (any phone) */
enum { CODEC_OPUS, CODEC_PCMA, CODEC_PCMU };
#define CALL_RATE  16000
#define CALL_FRAME 320                  /* 20 ms at CALL_RATE: what the mic side encodes, the playout plays */
#define CALL_PKT   400                  /* the largest encoded frame (Opus at 24 kbit/s: ~60 bytes; G.711: 160) */

/* Any lock but call.c's own (dropin.c's, core_lock may be held).  0: the call is who's now (or was already) */
int  call_claim(int who);
void call_release(int who);
int  call_claimed(void);                /* CALL_* */

/* The media of a connected call: send gets each encoded frame and its number (from 0), from the capture thread, no
 * lock of call.c's held.  The generation it returns ties call_rx and call_end to this media; -1: none (another runs) */
int  call_begin(int who, int codec, void (*send)(const uint8_t *frame, size_t len, uint32_t seq, void *arg), void *arg);
void call_end(int gen);
/* A frame that came in: seq numbers the frames from the far end in order (the owner widens RTP's 16 bits) */
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

/* G.711 and the 2:1 rate change around it (call.c; tests/unit/call_test.c) */
uint8_t g711_alaw(int16_t s);
uint8_t g711_ulaw(int16_t s);
int16_t g711_alaw_dec(uint8_t a);
int16_t g711_ulaw_dec(uint8_t u);
struct half { float x[64]; unsigned n; };   /* a FIR's history (call.c: 63 taps) */
void half_down(struct half *h, const int16_t *in, size_t n, int16_t *out);    /* n at 16 kHz -> n/2 at 8 kHz */
void half_up(struct half *h, const int16_t *in, size_t n, int16_t *out);      /* n at 8 kHz -> 2n at 16 kHz */
#endif
