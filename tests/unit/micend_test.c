/* The end of a request heard on the Echo (micend.c) on synthetic audio: a talker's syllables (a 4 Hz envelope on a
 * 220 Hz tone, dipping 20 dB under its level between them) at the keyword's level over a -67 dBFS room, then other
 * voices in the room, quieter or as loud, and our own sounds. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "micend.h"

#define RATE 16000
static int bad;
static int16_t buf[RATE * 20];

static void check(int ok, const char *what, double v) { printf("%s %s: %.2f\n", ok ? "ok  " : "FAIL", what, v); if (!ok) bad = 1; }

static double noise(void) { return (rand() / (double)RAND_MAX - 0.5) * 2 * 32768 * pow(10, -67 / 20.0) * 1.73; }

static size_t make(size_t at, double sec, double rms_db)     /* syllables at rms_db (below -100: the room only) */
{
    size_t n = (size_t)(sec * RATE);
    double a = rms_db > -100 ? 32768 * pow(10, rms_db / 20) * 2 / sqrt(0.75) : 0;
    for (size_t i = 0; i < n; i++) {
        double t = (double)(at + i) / RATE, env = 0.5 + 0.5 * sin(2 * M_PI * 4 * t);
        double v = a * env * env * sin(2 * M_PI * 220 * t) + noise();
        buf[at + i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
    }
    return at + n;
}

/* Amazon's VAD as the stream would carry it: speech in [vad_from, vad_to) seconds; vad_from < 0: no record in the
 * stream (the PC, an audio HAL that drops it) */
static double vad_from = -1, vad_to = -1;
static int vad(uint64_t from, uint64_t to)
{
    if (vad_from < 0) return -1;
    double a = (double)from / RATE, b = (double)to / RATE;
    return b > vad_from && a < vad_to;
}

/* seconds into the audio when it ended, or -1; own: [own_from, own_to) seconds are our sounds */
static double word_to;                                  /* the wake word sent too: [0, word_to) seconds (e.from) */
static double run(int mode, float talker, size_t n, double own_from, double own_to)
{
    struct micend e; micend_start(&e, mode, talker, 0, vad);
    e.from = (uint64_t)(word_to * RATE);
    for (size_t i = 0; i < n; i += 800) {               /* 50 ms blocks, as the capture */
        double t = (double)i / RATE;
        if (micend_run(&e, buf + i, n - i < 800 ? n - i : 800, t >= own_from && t < own_to)) return (i + 800.0) / RATE;
    }
    return -1;
}

int main(void)
{
    srand(1);
    size_t n;
    /* a 2 s command at the keyword's level, then the room: ends after the mode's pause */
    n = make(0, 2, -45); n = make(n, 3, -200);
    for (int m = MICEND_RELAXED; m < MICEND_MODES; m++) {
        double t = run(m, -45, n, 0, 0), want = 2 + micend_hold_ms(m) / 1000.0;
        char what[80]; snprintf(what, sizeof what, "%s: the room after the command, ends at", micend_names[m]);
        check(t > want - 0.2 && t < want + 0.15, what, t);
    }
    check(run(MICEND_OFF, -45, n, 0, 0) < 0, "off: never ends", 0);
    /* then other voices 15 dB under: left out from 14 dB under (normal), counted up to 17 (relaxed) */
    n = make(0, 2, -45); n = make(n, 4, -60);
    check(fabs(run(MICEND_NORMAL, -45, n, 0, 0) - 2.7) < 0.2, "normal: quieter voices after it do not keep it open", run(MICEND_NORMAL, -45, n, 0, 0));
    check(run(MICEND_RELAXED, -45, n, 0, 0) < 0, "relaxed: 15 dB under still counts", 0);
    /* as loud as the talker: the level cannot tell */
    n = make(0, 2, -45); n = make(n, 4, -47);
    check(run(MICEND_AGGRESSIVE, -45, n, 0, 0) < 0, "aggressive: a voice as loud keeps it open", 0);
    /* a command 6 dB quieter than its wake word, with a 0.4 s pause in it: whole */
    n = make(0, 1, -51); n = make(n, 0.4, -200); n = make(n, 1, -51); n = make(n, 2, -200);
    check(fabs(run(MICEND_NORMAL, -45, n, 0, 0) - 3.1) < 0.2, "normal: a quieter command with a pause stays whole", run(MICEND_NORMAL, -45, n, 0, 0));
    /* nothing said yet (the talker hesitates): never ends here, Home Assistant's VAD does */
    n = make(0, 0.2, -45); n = make(n, 3, -200);
    check(run(MICEND_AGGRESSIVE, -45, n, 0, 0) < 0, "aggressive: 200 ms of speech is not a request yet", 0);
    /* our wake sound (as loud as the talker) for 0.7 s, then the room for 1 s, then the command: the sound counts as
     * neither, so its 300 ms do not make a request */
    n = make(0, 0.7, -45); n = make(n, 1.0, -200); n = make(n, 1.5, -45); n = make(n, 2, -200);
    check(fabs(run(MICEND_NORMAL, -45, n, 0, 0.7) - 3.9) < 0.2, "normal: our sound before the command does not count", run(MICEND_NORMAL, -45, n, 0, 0.7));
    /* the dishes as loud as the talker after the command: the level alone keeps the request open, Amazon's VAD (speech
     * only in the command) does not */
    { int16_t click[4000];
      n = make(0, 2, -45); size_t m = make(n, 3, -200);
      for (size_t t = n; t + 4000 <= m; t += 4000) {                   /* 4 clatters a second, as loud as the voice */
          for (int i = 0; i < 4000; i++) click[i] = (int16_t)(i < 1200 ? (rand() % 2 ? 1 : -1) * 32768 * pow(10, -42 / 20.0) * (rand() / (double)RAND_MAX) : 0);
          for (int i = 0; i < 4000; i++) buf[t + i] = (int16_t)(buf[t + i] + click[i]);
      }
      n = m;
      check(run(MICEND_NORMAL, -45, n, 0, 0) < 0, "normal, no VAD in the stream: the dishes keep it open", 0);
      vad_from = 0; vad_to = 2.05;
      check(fabs(run(MICEND_NORMAL, -45, n, 0, 0) - 2.6) < 0.2, "normal, Amazon's VAD: the dishes do not", run(MICEND_NORMAL, -45, n, 0, 0));
      /* a quiet far talker the VAD never hears: nothing ends here (Home Assistant's VAD does) */
      vad_from = vad_to = 0;
      n = make(0, 2, -45); n = make(n, 3, -200);
      check(run(MICEND_NORMAL, -45, n, 0, 0) < 0, "normal, a talker the VAD misses: never ends here", 0);
      vad_from = vad_to = -1; }
    /* the wake word sent too (wake_word_in_request), then 1 s of waiting for the tone, then the command: the word
     * itself is no request yet */
    n = make(0, 0.6, -45); n = make(n, 1.0, -200); n = make(n, 1.5, -45); n = make(n, 2, -200);
    check(run(MICEND_NORMAL, -45, n, 0, 0) < 1.5, "normal, the word counted: ends in the wait before the command", run(MICEND_NORMAL, -45, n, 0, 0));
    word_to = 0.6;
    check(fabs(run(MICEND_NORMAL, -45, n, 0, 0) - 3.7) < 0.2, "normal, the word sent too: ends after the command", run(MICEND_NORMAL, -45, n, 0, 0));
    word_to = 0;
    return bad;
}
