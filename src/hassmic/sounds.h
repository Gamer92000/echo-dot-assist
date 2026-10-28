/* Amazon's own UI sounds, left on the system image by the stock firmware (/system/local/share/earcon/base). */
#ifndef SOUNDS_H
#define SOUNDS_H
#include <stddef.h>
enum sound { SND_WAKE, SND_TOUCH, SND_MICS_OFF, SND_MICS_ON, SND_VOLUME, SND_BT_ON, SND_BT_OFF, SND_IDENTIFY,
             SND_DROPIN, SND_CALL_ON, SND_CALL_OFF, SND_RING_IN, SND_RING_OUT,      /* Drop In: stock's comms sounds */
             SND_TIMER, SND_ALARM,          /* stock's timer and alarm tones (issue #63) */
             SND_CUSTOM,                    /* the timer sound uploaded on the settings page: state/timer_sound.wav */
             SND_COUNT };
/* Decoded to mono s16 at its own rate.  0 = not available (file missing or undecodable); result is cached.  The earcon
 * thread's only (SND_CUSTOM is read again there after sound_custom_save/_delete). */
int sound_get(enum sound s, const short **pcm, size_t *samples, unsigned *rate);
/* Whether the sound's file is there, without decoding it: any thread */
int sound_present(enum sound s);

/* The timer sound of one's own: a WAV of 16-bit mono at 48 kHz (the Earcon stream's rate: no resampler on the Echo; the
 * settings page converts whatever the browser decodes), at most SOUND_CUSTOM_SECS long */
#define SOUND_CUSTOM_RATE 48000
#define SOUND_CUSTOM_SECS 30
#define SOUND_CUSTOM_MAX (SOUND_CUSTOM_RATE * 2 * SOUND_CUSTOM_SECS + 4096)    /* the WAV's bytes, header and chunks included */
int sound_custom_save(const void *wav, size_t len, char *err, size_t errsz);   /* checked, then kept: 0, -1 (err says why) */
int sound_custom_delete(void);
unsigned sound_custom_ms(void);             /* its length, 0: none */
#endif
