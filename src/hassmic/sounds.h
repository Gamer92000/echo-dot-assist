/* Amazon's own UI sounds, left on the system image by the stock firmware (/system/local/share/earcon/base), or one's
 * own in their place. */
#ifndef SOUNDS_H
#define SOUNDS_H
#include <stddef.h>
enum sound { SND_WAKE, SND_TOUCH, SND_MICS_OFF, SND_MICS_ON, SND_VOLUME, SND_BT_ON, SND_BT_OFF, SND_IDENTIFY,
             SND_DROPIN, SND_CALL_ON, SND_CALL_OFF, SND_RING_IN, SND_RING_OUT,      /* Drop In: stock's comms sounds */
             SND_TIMER, SND_ALARM,          /* stock's timer and alarm tones (issue #63) */
             SND_CUSTOM,                    /* the timer sound uploaded on the settings page: state/timer_sound.wav */
             SND_COUNT };
/* Decoded to mono s16 at its own rate.  0 = not available (file missing or undecodable); result is cached.  The earcon
 * thread's only (a sound is read again there after sound_own_save/_delete). */
int sound_get(enum sound s, const short **pcm, size_t *samples, unsigned *rate);
void sound_played(enum sound s);            /* the earcon thread's, its pcm no longer used: a long one is not kept */
/* Whether the sound's file is there, without decoding it: any thread */
int sound_present(enum sound s);

/* The timer sound of one's own: a WAV of 16-bit mono at 48 kHz (the Earcon stream's rate: no resampler on the Echo; the
 * settings page converts whatever the browser decodes), at most SOUND_CUSTOM_SECS long */
#define SOUND_CUSTOM_RATE 48000
#define SOUND_CUSTOM_SECS 30
#define SOUND_CUSTOM_MAX (SOUND_CUSTOM_RATE * 2 * SOUND_CUSTOM_SECS + 4096)    /* the WAV's bytes, header and chunks included */

/* Sounds of one's own in place of stock's (the settings page's "Sounds" card): SND_WAKE to SND_RING_OUT, each a WAV
 * as the timer's in state/sounds/<id>.wav, played wherever stock's would be; deleted, stock's again; SND_CUSTOM is the
 * timer's, in state/.  Any thread */
const char *sound_id(enum sound s);         /* the page's name for it ("wake"), NULL: not replaceable */
int sound_by_id(const char *id);            /* -1: no such replaceable sound */
unsigned sound_own_secs(enum sound s);      /* how long one may be */
int sound_own_save(enum sound s, const void *wav, size_t len, char *err, size_t errsz);   /* checked, then kept: 0, -1 (err says why) */
int sound_own_check(enum sound s, const void *wav, size_t len, char *err, size_t errsz);   /* what save takes: 0, -1 */
int sound_own_delete(enum sound s);
unsigned char *sound_own_read(enum sound s, size_t *len);   /* its WAV, the caller's to free; NULL: none */
unsigned sound_own_ms(enum sound s);        /* its length, 0: none (stock's plays) */
int sound_stock_present(enum sound s);      /* stock's is on this image */
/* stock's, decoded (mono s16 at its own rate) and the caller's to free: the page measures its loudness to match */
short *sound_stock(enum sound s, size_t *samples, unsigned *rate);
#endif
