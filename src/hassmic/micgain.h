/* Automatic gain for the mic audio sent to the voice pipeline (not for the wake word, which is tuned to the stock level).
 *
 * Amazon's ASR path (AFE.cfg) ends in a fixed +5.2 dB "ASR Output Gain": no AGC, its cloud ASR was trained on that level.
 * Spoken wake words arrive at -50 to -62 dBFS (keyword rms, 10 detections on the installed Echo, 2026-09-29) over a
 * -67 dBFS floor; the STT engines and Home Assistant's VAD expect speech around -25 dBFS, as the Voice PE sends it.
 * Home Assistant does not help: its ESPHome satellite drops the audio settings of the request (core, assist_satellite
 * entity.py builds AudioSettings with silence_seconds only), so gain has to happen here. */
#ifndef MICGAIN_H
#define MICGAIN_H
#include <stddef.h>
#include <stdint.h>

#define MICGAIN_LEVEL -26               /* dBFS active speech level: ITU-T P.56's reference for speech (-26 dBov) */
#define MICGAIN_LEVEL_MIN -35
#define MICGAIN_LEVEL_MAX -15
#define MICGAIN_MAX_DB 36.0f            /* the floor (-67 dBFS) stays below -30 dBFS even for the quietest talker */
#define MICGAIN_MIN_DB -12.0f           /* shouting into it: the stock path reaches 0 dBFS a hand's width away */

struct micgain {
    int level;                          /* speech level wanted, dBFS (MICGAIN_LEVEL_MIN..MAX) */
    float speech_pow, noise_db, gain_db; /* talker's active speech level (mean power, full scale 1), floor, gain applied */
    float applied;                      /* linear gain at the end of the last frame: the next one ramps from there */
    int hold;                           /* set by the caller: what comes in is not the talker (our own sounds): the
                                         * levels and the gain stay as they are, the peak limit still works */
    float talker_min_db;                /* set by the caller: frames under this (dBFS, as they come in) are not the
                                         * talker's (micend.h): they neither feed its level nor lift the gain.  Else
                                         * other voices alone in the room are brought up to speech level within 2 s
                                         * (12 dB/s), a talker's 20 dB under too (tests/fake_ha_end.py).  -1000: all */
};

void micgain_init(struct micgain *g, int level);
/* A pipeline starts.  keyword_db: rms of the wake word that started it in dBFS, or > 0 when there was none (button,
 * Home Assistant): then the talker's level from before is kept. */
void micgain_start(struct micgain *g, float keyword_db);
void micgain_run(struct micgain *g, const int16_t *in, int16_t *out, size_t n);    /* 16 kHz mono; in == out is fine */
float micgain_talker_db(const struct micgain *g);                                   /* active speech level, dBFS */
#endif
