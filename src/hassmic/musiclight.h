/* What the music sounds like, for the live ring's music effects (ringlive.c): the PCM the Echo plays (Music Assistant's
 * Sendspin, a phone over Bluetooth, Home Assistant's media) taken apart as it goes to the output, every 10 ms, each
 * reading stamped with when it is heard (what the output still holds in front of it), so the ring moves with what
 * comes out of the speaker, not with what was written:
 *   - the level, left and right, and twelve bands from 50 Hz to 12 kHz (one an LED), each against its own range (a
 *     running peak and floor), so a quiet song and a loud one, compressed or not, fill the ring alike;
 *   - drums: kick, snare and hi-hat onsets from the spectral flux of their bands, each against an adaptive threshold;
 *   - the beat: the tempo from the onsets' autocorrelation (60-180 bpm), a phase locked to the kicks, the bar's first
 *     beat, so effects run in time even where a beat is quiet;
 *   - the energy against the last seconds (a drop against a breakdown) and how bright the sound is (its centroid).
 * Nothing is computed unless an effect wants it (musiclight_want).  tools/ringviz renders the effects from it. */
#ifndef MUSICLIGHT_H
#define MUSICLIGHT_H
#include <stddef.h>
#include <stdint.h>

#define MUSIC_BANDS 12                  /* 50 Hz .. 12 kHz, a third of an octave and more apart */
#define MUSIC_HITS  4                   /* the last onsets of each drum kept (ripples) */

struct music_hits { long long at[MUSIC_HITS]; float strength[MUSIC_HITS]; };    /* newest first; at 0: none */

struct music_feat {
    int   active;                       /* music heard now (not silence, not stopped) */
    float level, left, right;           /* 0..1 */
    float band[MUSIC_BANDS];            /* 0..1 each, bass first */
    float energy;                       /* 0..1: 0.5 as loud as the last seconds, more in a drop, less in a breakdown */
    float bright;                       /* 0..1: dark (bass) .. bright (highs) */
    struct music_hits kick, snare, hat;
    /* the beat: the last one heard (on the tempo's grid once it is known, else a kick), its number (the bar's first: a
     * multiple of 4), the beat's length; confidence 0..1 how sure the tempo is */
    long long beat_at; int beat_no; float beat_ms, bpm, confidence;
};

void musiclight_want(int on);           /* an effect shows the music: analyse what plays */
/* PCM as it is written to an output: s16 interleaved, `delay_us` what that output still holds in front of it */
void musiclight_feed(const int16_t *pcm, size_t frames, int channels, unsigned rate, long long delay_us);
/* The music as it is heard at `now_ms` (CLOCK_MONOTONIC ms) */
void musiclight_now(long long now_ms, struct music_feat *f);
void musiclight_reset(void);            /* tests: forget everything */
/* tests and tools/ringviz: the clock readings are stamped with, instead of CLOCK_MONOTONIC (NULL: that again) */
void musiclight_clock(long long (*now_ms)(void));
/* tools/ringviz: every block's band levels (dB) and their rises, heard when */
void musiclight_debug(void (*fn)(long long heard, const float db[MUSIC_BANDS], const float rise[MUSIC_BANDS]));
#endif
