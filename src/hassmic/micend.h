/* The end of a request heard on the Echo (issue #66; setting end_of_request, experimental, off by default).
 *
 * Home Assistant's VAD ends a request on silence, so in a family kitchen any voice in the room keeps it open (up to
 * its 15 s), and speech-to-text writes down the others' talk after the command: "Next. Eat some food please Now"
 * for "Alexa, next" (issue #66, Dot 2, Scribe v2: the command words are all there, the rest is quieter voices).
 * The gain then brings those quieter voices up to speech level too (micgain.c learns from any speech).
 *
 * Here the talker is the one who said the wake word: its level is the keyword's rms (after a button or a follow-up,
 * the gain's talker level from before).  10 ms frames within the mode's margin of it are the talker (relaxed 12 dB,
 * normal 9, aggressive 6), unless Amazon's front end is sure there is no speech in them (afemeta.h: its DNN VAD 0,
 * where the stream carries it): a kettle, the dishes or a door as loud as the voice do not count.  Once the talker has spoken for
 * 300 ms, a pause without such a frame (1.25, 0.7, 0.45 s) ends the request (end of audio: Home Assistant goes on with
 * what it has).  Quieter voices then neither keep it open nor follow the command into speech-to-text.  Home
 * Assistant's VAD stays on: the Echo can only end a request sooner, never later.  Our own sounds (the wake sound
 * after the echo canceller is as loud as a quiet talker) stop the clock: they count as neither.
 *
 * What it cannot tell: another voice as loud as the talker's (the level is all it has: the VAD hears speech, not
 * whose), and a talker who turns away or drops the voice by more than the margin mid-sentence (cut short: hence off by
 * default, "relaxed" first).  Speech the VAD misses (quiet and far) never ends a request here: Home Assistant does. */
#ifndef MICEND_H
#define MICEND_H
#include <stddef.h>
#include <stdint.h>

enum { MICEND_OFF, MICEND_RELAXED, MICEND_NORMAL, MICEND_AGGRESSIVE, MICEND_MODES };
extern const char *const micend_names[MICEND_MODES];         /* "off", "relaxed", "normal", "aggressive" */

struct micend {
    int mode;
    float talker_db;                    /* the talker's level, dBFS */
    int heard, quiet;                   /* 10 ms frames: the talker so far; since the talker was last heard */
    int ended;
    double acc; int acc_n;              /* the frame being summed */
    uint64_t at;                        /* the stream's sample index of the next sample */
    uint64_t from;                      /* set by the caller: frames ending before it are the wake word (sent too):
                                         * neither the talker nor a pause.  micend_start: the first sample */
    int (*speech)(uint64_t from, uint64_t to);   /* may be NULL: speech in samples [from, to)? 1, 0, -1 not known */
    int vad_frames, vad_vetoed;         /* loud frames the VAD had a say on; those it said were no speech */
};

/* at: the stream's sample index of the first sample micend_run will get, for speech() */
void micend_start(struct micend *e, int mode, float talker_db, uint64_t at, int (*speech)(uint64_t from, uint64_t to));
/* 16 kHz mono, the level as the Echo hears it (before the gain); own: our sounds are in it.  1 once the talker has
 * finished (and from then on), 0 while not or when off */
int micend_run(struct micend *e, const int16_t *pcm, size_t n, int own);
float micend_margin(int mode);          /* dB under the talker that still counts as the talker */
int micend_hold_ms(int mode);           /* without the talker this long: the end */
#endif
