/* Amazon's own UI sounds, left on the system image by the stock firmware (/system/local/share/earcon/base). */
#ifndef SOUNDS_H
#define SOUNDS_H
#include <stddef.h>
enum sound { SND_WAKE, SND_TOUCH, SND_MICS_OFF, SND_MICS_ON, SND_VOLUME, SND_BT_ON, SND_BT_OFF, SND_COUNT };
/* Decoded to mono s16 at its own rate.  0 = not available (file missing or undecodable); result is cached. */
int sound_get(enum sound s, const short **pcm, size_t *samples, unsigned *rate);
#endif
