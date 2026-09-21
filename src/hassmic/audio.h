/* Audio backend: Amazon mixer daemon on the device, files on the PC (for protocol tests). */
#ifndef AUDIO_H
#define AUDIO_H
#include <stddef.h>

#define CAP_RATE 16000

/* Blocks until the next block of 16 kHz mono s16le is available.  Returns bytes, 0 on timeout, -1 on fatal error.
 * *data stays valid until the next call. */
int  cap_open(void);
int  cap_read(const void **data);
void cap_close(void);

int  play_open(unsigned rate, unsigned channels);
int  play_write(const void *data, size_t len);      /* blocks at playback speed */
void play_close(int drain);

/* Short UI sound on its own stream; mixes with whatever else plays.  Blocks for the length of the sound. */
void play_earcon(const short *pcm, size_t samples, unsigned rate);
#endif
