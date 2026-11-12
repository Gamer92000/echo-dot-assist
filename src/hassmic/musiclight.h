/* What the music sounds like, for the live ring's music effects (ringlive.c): the PCM the Echo plays (Music Assistant's
 * Sendspin, a phone over Bluetooth, Home Assistant's media) taken apart as it goes to the output, every 10 ms: the
 * level, six bands from the bass up, and the beats; each reading stamped with when it is heard (what the output still
 * holds in front of it), so the ring moves with what comes out of the speaker, not with what was written.
 * Levels are relative to the music itself (a running peak a band, falling 2 dB a second): a quiet song and a loud one
 * fill the ring alike, whatever the volume.  Nothing is computed unless an effect wants it (musiclight_want). */
#ifndef MUSICLIGHT_H
#define MUSICLIGHT_H
#include <stddef.h>
#include <stdint.h>

#define MUSIC_BANDS 6                   /* 60, 150, 400 Hz, 1, 2.5, 6 kHz */
#define MUSIC_BEATS 4                   /* the last beats kept (ripples) */

struct music_feat {
    int   active;                       /* music heard now (not silence, not stopped) */
    float level;                        /* 0..1, the whole of it */
    float band[MUSIC_BANDS];            /* 0..1 each */
    long long beat_ms[MUSIC_BEATS];     /* when the last beats are heard, newest first (monotonic ms; 0 none) */
    float beat_strength[MUSIC_BEATS];   /* 0.4..1 */
};

void musiclight_want(int on);           /* an effect shows the music: analyse what plays */
/* PCM as it is written to an output: s16 interleaved, `delay_us` what that output still holds in front of it */
void musiclight_feed(const int16_t *pcm, size_t frames, int channels, unsigned rate, long long delay_us);
/* The music as it is heard at `now_ms` (CLOCK_MONOTONIC ms) */
void musiclight_now(long long now_ms, struct music_feat *f);
void musiclight_reset(void);            /* tests: forget everything */
#endif
