/* The end of a request heard on the Echo: see micend.h.
 *
 * The talker's own speech dips far under its level between words (stops, quiet consonants, breaths: 10-20 dB in
 * 10 ms frames), but its vowels come back within a few dB of it many times a second; a pause of HA's own 0.7 s
 * ("Finished speaking detection" default; relaxed 1.25 s, aggressive 0.25 s) between them is the end.  A frame is
 * compared with the keyword's rms, and voiced frames peak ~5 dB over their own rms (the unit test's syllables too):
 * other voices are left out from 17, 14 or 11 dB under the talker, and a command 6 dB quieter than its wake word
 * still reaches the keyword's rms within 1 dB.  Not measured on the user's kitchen yet: hence the modes, and the log
 * line with the talker's level for each request. */
#include "micend.h"
#include <math.h>

#define FRAME 160
#define SPEECH_MIN 30                   /* frames: the talker heard for 300 ms before anything ends */

const char *const micend_names[MICEND_MODES] = { "off", "relaxed", "normal", "aggressive" };
static const float margin[MICEND_MODES] = { 0, 12, 9, 6 };
static const int hold[MICEND_MODES] = { 0, 1250, 700, 450 };

float micend_margin(int mode) { return mode > 0 && mode < MICEND_MODES ? margin[mode] : 0; }
int micend_hold_ms(int mode) { return mode > 0 && mode < MICEND_MODES ? hold[mode] : 0; }

void micend_start(struct micend *e, int mode, float talker_db, uint64_t at, int (*speech)(uint64_t from, uint64_t to))
{
    e->mode = mode > 0 && mode < MICEND_MODES ? mode : MICEND_OFF;
    e->talker_db = talker_db; e->heard = e->quiet = e->ended = 0;
    e->acc = 0; e->acc_n = 0;
    e->at = e->from = at; e->speech = speech; e->vad_frames = e->vad_vetoed = 0;
}

int micend_run(struct micend *e, const int16_t *pcm, size_t n, int own)
{
    if (!e->mode) return 0;
    for (size_t i = 0; i < n && !e->ended; i++, e->at++) {
        e->acc += (double)pcm[i] * pcm[i];
        if (++e->acc_n < FRAME) continue;
        double db = 10 * log10(e->acc / FRAME / (32768.0 * 32768.0) + 1e-10);
        e->acc = 0; e->acc_n = 0;
        if (own || e->at + 1 <= e->from) continue;
        int loud = db >= e->talker_db - margin[e->mode], v = loud && e->speech ? e->speech(e->at + 1 - FRAME, e->at + 1) : -1;
        if (v >= 0) { e->vad_frames++; if (!v) e->vad_vetoed++; }
        if (loud && v) { e->heard++; e->quiet = 0; }
        else if (e->heard >= SPEECH_MIN && ++e->quiet * 10 >= hold[e->mode]) e->ended = 1;
    }
    return e->ended;
}
