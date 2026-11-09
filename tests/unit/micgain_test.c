/* The pipeline's mic gain on synthetic speech at the levels the Echo delivers: syllables (a 4 Hz envelope on a 220 Hz
 * tone) over a -67 dBFS noise floor, a quiet talker at -55 dBFS rms as the wake words arrive, a loud one close by.
 * Levels are active speech levels: power mean over the 10 ms frames well above the floor, as the gain measures them. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "micgain.h"

#define RATE 16000
static int bad;
static void check(int ok, const char *what, double v) { printf("%s %s: %.1f\n", ok ? "ok  " : "FAIL", what, v); if (!ok) bad = 1; }

static float noise(void) { return (rand() / (float)RAND_MAX - 0.5f) * 2 * 32768 * powf(10, -67 / 20.0f) * 1.73f; }

/* seconds of speech at rms_db (0 amplitude: floor only) into buf, from sample at */
static size_t make(int16_t *buf, size_t at, double sec, double rms_db)
{
    size_t n = (size_t)(sec * RATE);
    double a = rms_db > -100 ? 32768 * pow(10, rms_db / 20) * 2 / sqrt(0.75) : 0;  /* sine x (0.5 + 0.5 sin) envelope: rms a/2 * sqrt(3/4) */
    for (size_t i = 0; i < n; i++) {
        double t = (double)(at + i) / RATE, env = 0.5 + 0.5 * sin(2 * M_PI * 4 * t);
        double v = a * env * sin(2 * M_PI * 220 * t) + noise();
        buf[at + i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
    }
    return at + n;
}

static double level(const int16_t *s, size_t a, size_t b)     /* plain rms */
{
    double sum = 0;
    for (size_t i = a; i < b; i++) sum += (double)s[i] * s[i];
    return 10 * log10(sum / (b - a) / (32768.0 * 32768.0) + 1e-12);
}

static double active(const int16_t *s, const int16_t *ref, size_t a, size_t b)   /* frames where ref (the input) is speech */
{
    double sum = 0; size_t n = 0;
    for (size_t i = a; i + 160 <= b; i += 160)
        if (level(ref, i, i + 160) > -67 + 9) { for (size_t k = i; k < i + 160; k++) sum += (double)s[k] * s[k]; n += 160; }
    return n ? 10 * log10(sum / n / (32768.0 * 32768.0) + 1e-12) : -200;
}

static double frame_gain(const int16_t *i, const int16_t *o)     /* of a 10 ms frame, by mean magnitudes */
{
    double a = 1, b = 0;
    for (int k = 0; k < 160; k++) { a += abs(i[k]); b += abs(o[k]); }
    return b / a;
}

static int16_t in[RATE * 12], out[RATE * 12];

static void run(struct micgain *g, size_t n)     /* in blocks of odd size, as the mixer's are not frame aligned */
{
    for (size_t i = 0; i < n; i += 250) micgain_run(g, in + i, out + i, n - i < 250 ? n - i : 250);
}

int main(void)
{
    struct micgain g;
    size_t t = 0, q0, q1, p1, l0, l1;
    q0 = t; t = make(in, t, 3, -55);             /* quiet talker */
    q1 = t; t = make(in, t, 2, -200);            /* pause */
    p1 = t; l0 = t; t = make(in, t, 3, -16);     /* loud talker close by */
    l1 = t; t = make(in, t, 1, -200);

    micgain_init(&g, MICGAIN_LEVEL); micgain_start(&g, -55);
    run(&g, t);
    double want = MICGAIN_LEVEL;
    check(fabs(active(out, in, q0, q0 + RATE / 4) - want) < 3, "quiet talker, first 250 ms (gain from the wake word), dBFS", active(out, in, q0, q0 + RATE / 4));
    check(fabs(active(out, in, q0 + RATE, q1) - want) < 1.5, "quiet talker after 1 s, dBFS", active(out, in, q0 + RATE, q1));
    check(level(out, p1 - RATE / 2, p1) - level(out, q1 + RATE / 4, q1 + RATE * 3 / 4) < 2, "pause: the floor does not pump up, dB", level(out, p1 - RATE / 2, p1) - level(out, q1 + RATE / 4, q1 + RATE * 3 / 4));
    check(level(out, q1 + RATE / 4, p1) - level(in, q1 + RATE / 4, p1) <= MICGAIN_MAX_DB + 0.5, "gain in the pause, dB", level(out, q1 + RATE / 4, p1) - level(in, q1 + RATE / 4, p1));
    check(fabs(active(out, in, l0 + RATE, l1) - want) < 1.5, "loud talker after 1 s, dBFS", active(out, in, l0 + RATE, l1));
    int peak = 0; for (size_t i = 0; i < t; i++) if (abs(out[i]) > peak) peak = abs(out[i]);
    check(peak <= 29000, "peak, loud talker included", peak);

    micgain_init(&g, MICGAIN_LEVEL); micgain_start(&g, -55);         /* held while our own wake sound is in the stream */
    float before = g.gain_db;
    g.hold = 1; micgain_run(&g, in + l0, out, RATE / 2);             /* half a second as loud as the loud talker */
    check(g.gain_db == before, "gain unchanged by a loud sound while held, dB", g.gain_db - before);
    g.hold = 0; micgain_run(&g, in + q0 + RATE, out, RATE);
    check(fabs(active(out, in + q0 + RATE, 0, RATE) - want) < 1.5, "quiet talker right after it, dBFS", active(out, in + q0 + RATE, 0, RATE));

    /* someone talking over the sound (issue #82), 25 dB over the wake word */
    size_t m0 = t; make(in, m0, 2, -30);
    micgain_init(&g, MICGAIN_LEVEL); micgain_start(&g, -55);
    g.limit_stays = 1; g.hold = 1; micgain_run(&g, in + m0, out, RATE / 2);
    int rose = 0; peak = 0; double low = 1e9;
    for (size_t i = 0; i + 160 <= RATE / 2; i += 160) {
        double f = frame_gain(in + m0 + i, out + i);
        if (f > low * 1.02) rose++;
        if (f < low) low = f;
        for (size_t k = i; k < i + 160; k++) if (abs(out[k]) > peak) peak = abs(out[k]);
    }
    check(!rose, "held: the limit only ever takes the gain down, frames turned up again", rose);
    check(peak <= 29000, "held: peak over the sound", peak);
    g.hold = 0; micgain_run(&g, in + m0 + RATE / 2, out, RATE / 10);  /* the hold ends, the talker goes on */
    double high = 0;
    for (size_t i = 0; i + 160 <= RATE / 10; i += 160) { double f = frame_gain(in + m0 + RATE / 2 + i, out + i); if (f > high) high = f; }
    check(high <= low * 1.15, "the hold over: the level there at once, no jump in mid-syllable, dB", 20 * log10(high / low));
    micgain_run(&g, in + m0 + RATE * 6 / 10, out, RATE);
    check(fabs(active(out, in + m0 + RATE * 6 / 10, RATE / 2, RATE) - want) < 1.5, "and the talker's level found, dBFS", active(out, in + m0 + RATE * 6 / 10, RATE / 2, RATE));

    micgain_init(&g, MICGAIN_LEVEL); micgain_start(&g, -55);         /* a sound alone at the limit, a quiet talker after */
    g.limit_stays = 1; g.hold = 1; micgain_run(&g, in + l0, out, RATE / 2);
    g.hold = 0; micgain_run(&g, in + q0 + RATE, out, RATE);
    check(fabs(active(out, in + q0 + RATE, 0, RATE) - want) < 1.5, "a sound's peaks do not stay in the gain, dBFS", active(out, in + q0 + RATE, 0, RATE));

    micgain_init(&g, MICGAIN_LEVEL_MIN); micgain_start(&g, -55);     /* another level from the slider */
    run(&g, q1);
    check(fabs(active(out, in, q0 + RATE, q1) - MICGAIN_LEVEL_MIN) < 1.5, "quiet talker at the lowest mic level, dBFS", active(out, in, q0 + RATE, q1));
    return bad;
}
