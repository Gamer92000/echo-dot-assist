/* The Dots' music queue (audio_mixer.c): written minus what real time drained at the nominal rate, a model, steered
 * towards what the mixer says it still holds (issue #225).
 *
 * The model alone cannot see the output fall behind or run ahead of CLOCK_MONOTONIC_RAW: an output clock of its own, or
 * the mixer stalling for a moment (an xrun), and every Echo of a Sendspin group drifts by what its own does while its
 * servo reads "sync err 0.00".  MixerGetNumBytes counts what the mixer has not taken of the stream yet, so it shows both;
 * but it leaves out what the mixer holds behind it, and reads in steps.  Measured on a Dot 2 (biscuit, 2026-11-24,
 * 25 min, 48 kHz stereo written as sendspin.c does): it sat 51-125 ms against the model's 95-145, their difference
 * -58..-68 ms at every reading and no trend, the mixer draining at 0.0 ppm (±0.05) against CLOCK_MONOTONIC_RAW.  So the
 * difference is taken as the stream's own once it settled (MS_SKIP_US after the first write, its mean over
 * MS_LEARN_US), and from then on the model's start is moved so the difference stays there, with a time constant of
 * MS_TAU_US: the steps average out (the reference within 0.6 ms of the truth, a constant), a drift of 25 ppm lags by
 * 0.5 ms, a stall is followed at most MS_GAP_MAX / MS_TAU_US fast (10 ms a second; past 20 ms of error sendspin.c
 * resyncs).  Where the mixer is exact, as on the Dot 2, it moves nothing; where its count never moves, nothing is
 * steered.  Pure: tests/unit/mixsteer_test.c. */
#ifndef MIXSTEER_H
#define MIXSTEER_H

#include <string.h>

#define MS_SKIP_US   2000000LL        /* the mixer picks the stream up (inotify, then the ring) and the start's prime/snap */
#define MS_LEARN_US  8000000LL        /* the reference's mean: steps of 10-40 ms, a writer's sawtooth of 25 */
#define MS_MIN_N     100              /* readings it takes at least */
#define MS_EVERY_US  20000LL          /* one reading per 20 ms on average: 10-30 at random (below) */
#define MS_TAU_US    20000000.0
#define MS_GAP_MAX   200000LL         /* a reading further off than this from the reference counts as this */

struct mixsteer {
    long long start, last;            /* the stream's first write; the last reading */
    double sum, base, acc;            /* the reference's sum, the reference; what is not moved yet (< 1 us) */
    int n, locked;                    /* locked: 1 steering, -1 never (the count did not move while the reference was taken) */
    long long lo, hi;                 /* the count's range meanwhile */
    long long shift;                  /* moved in all since the reference */
    long long next;                   /* the next reading's time */
    unsigned lcg;
};

/* The readings' times at random: the writer polls every 5 ms and writes every 25, the mixer takes the stream in blocks
 * of its own clock, which is the CPU's (the Dot 2: exact).  Read every 20 ms, the count's steps fall on a few phases of
 * that pattern, their mean is off by a share of a step and moves as the output drifts across them (the test: 2-4 ms at
 * 25 ppm); read at random times it is the steps' plain mean, the same all along, in the reference. */
static inline void ms_next(struct mixsteer *m, long long now)
{
    m->lcg = m->lcg * 1103515245u + 12345u;
    m->next = now + MS_EVERY_US / 2 + (long long)((m->lcg >> 8) % (unsigned)MS_EVERY_US);
}

static inline void ms_reset(struct mixsteer *m, long long now)
{
    unsigned seed = m->lcg + (unsigned)now;
    memset(m, 0, sizeof *m); m->start = now; m->last = now; m->lcg = seed; ms_next(m, now + MS_SKIP_US);
}

/* whether a reading is wanted now (MixerGetNumBytes is asked only then) */
static inline int ms_due(const struct mixsteer *m, long long now) { return m->start && now >= m->next; }

/* count: what the mixer says it holds, model: what the model says (us), at now.  Returns how far to move the model's
 * start (us; > 0: more is queued than the model said).  m->locked turns 1 at the reading that took the reference, -1
 * where the count never moved meanwhile (a mixer that does not count: the model alone, as before). */
static inline long long ms_update(struct mixsteer *m, long long count, long long model, long long now)
{
    long long dt = now - m->last, gap = count - model;
    m->last = now; ms_next(m, now);
    if (m->locked < 0) return 0;
    if (!m->locked) {
        if (!m->n || count < m->lo) m->lo = count;
        if (!m->n || count > m->hi) m->hi = count;
        m->sum += (double)gap; m->n++;
        if (now - m->start >= MS_SKIP_US + MS_LEARN_US && m->n >= MS_MIN_N) { m->base = m->sum / m->n; m->locked = m->hi > m->lo ? 1 : -1; }
        return 0;
    }
    double err = (double)gap - m->base;
    if (err > MS_GAP_MAX) err = MS_GAP_MAX;
    if (err < -MS_GAP_MAX) err = -MS_GAP_MAX;
    if (dt > 100000) dt = 100000;                                  /* a pause in the readings is no longer weight */
    m->acc += err * (double)dt / MS_TAU_US;
    long long d = (long long)m->acc;
    m->acc -= (double)d; m->shift += d;
    return d;
}

#endif
