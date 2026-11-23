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

/* One call on the Echo at a time, whoever carries it (docs/plugins.md 4.8): Drop In (dropin.c) or a plugin with `call`
 * (plugin.c).  The claim is taken from calling or ringing on and let go when the call is over; whoever finds it taken
 * refuses "busy", Drop In against a plugin and the other way round.  Its states are Drop In's (dropin.h), which Home
 * Assistant's "Drop In" sensor has always said */
enum { CALL_IDLE, CALL_CALLING, CALL_RINGING, CALL_LIVE,
       CALL_ANSWERING };                /* a plugin's: a person answered here, its far side not in yet (never Drop In's,
                                         * whose states Home Assistant sees) */
enum { CALL_NOBODY = 0, CALL_DROPIN = 1, CALL_PLUGIN = 16 };      /* CALL_PLUGIN + the broker's entry: a plugin's */
/* Lock-free (one atomic): any lock or none.  call_claim: 0 WHO has it now (or had it), -1 another has it */
int  call_claim(int who);
void call_release(int who);             /* only WHO's claim goes */
int  call_claimed(void);                /* who has it, CALL_NOBODY none */

/* The media of a connected call, for WHO, which must have the claim (-1 if it has not): send gets each frame and its
 * number (from 0), in order, from the capture thread or call.c's call microphone thread, with call.c's sending lock
 * held (tx_mu, one sender at a time) and none of its others: send may take the owner's lock but must not call into
 * call.c.  The core's lock is taken under tx_mu too (core_call_listen), so the owner never calls call_mic holding its
 * own lock or core_lock.  CODEC: CALL_CODEC_OPUS, a frame encoded (CALL_PKT bytes at most, Drop In's); CALL_CODEC_PCM,
 * the frame as it is, CALL_FRAME samples of s16le after the gate and the gain (a plugin's: its codec, G.711 or Opus, is
 * its own, docs/plugins.md 4.8).  The generation it returns ties call_rx and call_end to this media */
enum { CALL_CODEC_OPUS, CALL_CODEC_PCM };
#define CALL_PCM_BYTES (CALL_FRAME * 2)
int  call_begin(int who, int codec, void (*send)(const uint8_t *frame, size_t len, uint32_t seq, void *arg), void *arg);
void call_end(int gen);
/* A frame that came in: seq numbers the frames from the far end in order.  Of the media's codec: Opus, or PCM of
 * exactly CALL_PCM_BYTES (anything else is dropped) */
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

#endif
