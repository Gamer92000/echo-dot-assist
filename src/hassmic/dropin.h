/* Drop In: a two-way call between two Echos of one arbitration network (dropin.c). */
#ifndef DROPIN_H
#define DROPIN_H
#include <stddef.h>
#include <stdint.h>

enum { DROPIN_IDLE, DROPIN_CALLING, DROPIN_RINGING, DROPIN_LIVE };
extern const char *const dropin_states[4];      /* "idle", "calling", "ringing", "connected": Home Assistant's text */
/* What came of a call this Echo made (Home Assistant's event entity "Drop In result", its event types): the other Echo
 * took it, rings (not the end: one of the others follows), refused it (busy, do not disturb, its microphones off, Drop In
 * off there, declined, not answered), said nothing (no_answer: off or out of reach), is not in this Echo's network, this
 * Echo cannot call now (in a call, microphones off), the call was cancelled here, or anything else */
enum { DROPIN_R_CONNECTED, DROPIN_R_RINGING, DROPIN_R_BUSY, DROPIN_R_DND, DROPIN_R_MUTED, DROPIN_R_OFF, DROPIN_R_DECLINED,
       DROPIN_R_NOT_ANSWERED, DROPIN_R_NO_ANSWER, DROPIN_R_NOT_FOUND, DROPIN_R_BUSY_HERE, DROPIN_R_MUTED_HERE,
       DROPIN_R_CANCELLED, DROPIN_R_FAILED, DROPIN_NRESULTS };
extern const char *const dropin_results[DROPIN_NRESULTS];

int  dropin_start(int port);                    /* the audio socket and threads; arb must run.  0 ok */
int  dropin_running(void);
/* Any lock but dropin's own (core_lock may be held): */
int  dropin_enable(int set);                    /* the "drop_in" setting: calls in and out; -1 reads */
int  dropin_ask(int set);                       /* "drop_in_answer": 0 connects at once, 1 waits for the action button */
/* Drop in on another Echo of the network, named by its node or its name ("Echo Dot 3 5695c4").  0: calling it; -1 and
 * why in err */
int  dropin_call(const char *target, char *err, size_t errsz);
void dropin_hangup(const char *why);            /* ends whatever runs: a call, a ringing one, a call going out */
int  dropin_button(void);                       /* the action button: answers a ringing call or ends one; 1 if it did */
int  dropin_status(char *peer, size_t cap);     /* DROPIN_*; peer: the other Echo's node ("" when idle) */
int  dropin_last(unsigned *attempt);            /* the last call's DROPIN_R_* (-1: none yet); attempt counts calls made */
/* The capture thread: 16 kHz mono micAsr as it comes, while a call runs (muted: zeros).  own: the Echo plays a sound or
 * a reply of its own: ducked like the far end's voice */
void dropin_mic(const int16_t *pcm, size_t n, int own);
int  dropin_far_talking(void);                  /* the other side's voice plays here now (or did, ECHO_MS ago) */
/* arb's hook: a member's message (arb.h) */
void dropin_message(const unsigned char from[8], const char *node, unsigned ip, const unsigned char *p, size_t n);
#endif
