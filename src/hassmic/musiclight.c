/*
 * The music for the live ring (musiclight.h).  Per 10 ms block of the mono mix: the energy of six band-pass filters
 * (RBJ biquads, constant 0 dB peak, Q 1.1, at 60, 150, 400 Hz, 1, 2.5 and 6 kHz) and of the whole, in dB.  Each is
 * shown relative to its own running peak (up at once, down 2 dB a second): the top 24 dB under it map to 0..1 (30 for
 * the whole), so a song mastered quiet fills the ring as a loud one does, and the volume does not matter (Sendspin's PCM
 * is taken before the mixer's volume; a phone's comes as the phone sends it).  Shown values rise at once and fall with
 * 140 ms, as a meter's.  A beat: the bass (the two lowest bands) more than 1.6 times its own average of the last ~0.7 s,
 * rising, at least 250 ms after the last (a kick drum at 240 bpm), over -45 dBFS.  Under -55 dBFS for a second, or no
 * audio for 300 ms: not active.  ~1.5 M multiply-adds a second at 48 kHz, only while an effect wants them.
 */
#include "musiclight.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define BLOCK_MS   10
#define HIST       256                  /* readings kept: 2.5 s, more than any output holds */
static const float centre[MUSIC_BANDS] = { 60, 150, 400, 1000, 2500, 6000 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int wanted;
static struct biquad { float b0, b1, b2, a1, a2, z1, z2; } bq[MUSIC_BANDS];
static unsigned at_rate;
static double acc[MUSIC_BANDS], acc_all;
static unsigned acc_n;
static float peak[MUSIC_BANDS], peak_all = -60, shown[MUSIC_BANDS], shown_all, bass_avg, bass_prev;
static long long quiet_ms, last_beat_heard;
static struct reading { long long at; struct music_feat f; } hist[HIST];
static int nhist, head;                 /* head: the next slot */
static long long beats[MUSIC_BEATS]; static float strengths[MUSIC_BEATS];

static long long mono_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void design(unsigned rate)
{
    for (int b = 0; b < MUSIC_BANDS; b++) {
        float w = 2 * (float)M_PI * centre[b] / rate, al = sinf(w) / (2 * 1.1f), a0 = 1 + al;
        bq[b] = (struct biquad){ al / a0, 0, -al / a0, -2 * cosf(w) / a0, (1 - al) / a0, 0, 0 };
    }
    at_rate = rate;
}

static float rel(float db, float pk, float span) { float v = (db - (pk - span)) / span; return v < 0 ? 0 : v > 1 ? 1 : v; }
static float meter(float was, float now, float fall) { return now >= was ? now : was + (now - was) * fall; }

/* lock held: one block done, heard at `heard` */
static void block(long long heard)
{
    float db[MUSIC_BANDS], all = 10 * log10f((float)(acc_all / acc_n) + 1e-12f);
    const float fall = 1 - expf(-(float)BLOCK_MS / 140);          /* the meters' release */
    for (int b = 0; b < MUSIC_BANDS; b++) {
        db[b] = 10 * log10f((float)(acc[b] / acc_n) + 1e-12f);
        peak[b] = db[b] > peak[b] ? db[b] : peak[b] - 0.02f;       /* 2 dB a second */
        if (peak[b] < -50) peak[b] = -50;
        acc[b] = 0;
    }
    peak_all = all > peak_all ? all : peak_all - 0.02f;
    if (peak_all < -50) peak_all = -50;
    acc_all = 0; acc_n = 0;
    int silent = all < -55;
    quiet_ms = silent ? quiet_ms + BLOCK_MS : 0;
    for (int b = 0; b < MUSIC_BANDS; b++) shown[b] = meter(shown[b], silent ? 0 : rel(db[b], peak[b], 24), fall);
    shown_all = meter(shown_all, silent ? 0 : rel(all, peak_all, 30), fall);
    /* a beat: the bass against its own recent average, rising */
    float bass = powf(10, db[0] / 10) + powf(10, db[1] / 10), bass_db = 10 * log10f(bass + 1e-12f);
    float ratio = bass / (bass_avg + 1e-12f);
    if (ratio > 1.6f && bass > bass_prev && bass_db > -45 && heard - last_beat_heard >= 250) {
        memmove(beats + 1, beats, sizeof beats[0] * (MUSIC_BEATS - 1));
        memmove(strengths + 1, strengths, sizeof strengths[0] * (MUSIC_BEATS - 1));
        beats[0] = heard; strengths[0] = 0.4f + 0.6f * fminf(1, (ratio - 1.6f) / 2.4f);
        last_beat_heard = heard;
    }
    bass_avg += (bass - bass_avg) * 0.015f;                        /* ~0.7 s */
    bass_prev = bass;
    struct reading *r = &hist[head];
    r->at = heard;
    r->f.active = quiet_ms < 1000;
    r->f.level = shown_all;
    memcpy(r->f.band, shown, sizeof shown);
    memcpy(r->f.beat_ms, beats, sizeof beats); memcpy(r->f.beat_strength, strengths, sizeof strengths);
    head = (head + 1) % HIST; if (nhist < HIST) nhist++;
}

void musiclight_want(int on) { atomic_store(&wanted, !!on); }

void musiclight_feed(const int16_t *pcm, size_t frames, int ch, unsigned rate, long long delay_us)
{
    if (!atomic_load(&wanted) || !pcm || ch < 1 || rate < 8000) return;
    long long now = mono_ms(), base = now + (delay_us > 0 ? delay_us / 1000 : 0);
    unsigned per = rate * BLOCK_MS / 1000;
    pthread_mutex_lock(&lock);
    if (!at_rate) for (int b = 0; b < MUSIC_BANDS; b++) peak[b] = -60;      /* the first music: its peaks found at once */
    if (rate != at_rate) { design(rate); acc_n = 0; acc_all = 0; memset(acc, 0, sizeof acc); }
    for (size_t i = 0; i < frames; i++) {
        float x = 0;
        for (int c = 0; c < ch; c++) x += pcm[i * ch + c];
        x /= ch * 32768.0f;
        acc_all += (double)x * x;
        for (int b = 0; b < MUSIC_BANDS; b++) {
            struct biquad *q = &bq[b];
            float y = q->b0 * x + q->z1;
            q->z1 = q->b1 * x - q->a1 * y + q->z2; q->z2 = q->b2 * x - q->a2 * y;
            acc[b] += (double)y * y;
        }
        if (++acc_n >= per) block(base + (long long)(i + 1) * 1000 / rate);
    }
    pthread_mutex_unlock(&lock);
}

void musiclight_now(long long now, struct music_feat *f)
{
    memset(f, 0, sizeof *f);
    pthread_mutex_lock(&lock);
    const struct reading *best = NULL; long long newest = 0;
    for (int k = 0; k < nhist; k++) {                   /* the latest reading heard by now */
        const struct reading *r = &hist[(head - 1 - k + HIST) % HIST];
        if (!newest) newest = r->at;
        if (r->at <= now) { best = r; break; }
    }
    if (best && now - newest < 300) {                   /* audio still coming: else stopped */
        *f = best->f;
        int k = 0; long long b[MUSIC_BEATS]; float s[MUSIC_BEATS];
        for (int i = 0; i < MUSIC_BEATS; i++) if (best->f.beat_ms[i] && best->f.beat_ms[i] <= now) { b[k] = best->f.beat_ms[i]; s[k++] = best->f.beat_strength[i]; }
        memset(f->beat_ms, 0, sizeof f->beat_ms); memset(f->beat_strength, 0, sizeof f->beat_strength);
        memcpy(f->beat_ms, b, sizeof b[0] * k); memcpy(f->beat_strength, s, sizeof s[0] * k);
    }
    pthread_mutex_unlock(&lock);
}

void musiclight_reset(void)
{
    pthread_mutex_lock(&lock);
    memset(bq, 0, sizeof bq); at_rate = 0; memset(acc, 0, sizeof acc); acc_all = 0; acc_n = 0;
    memset(peak, 0, sizeof peak); for (int b = 0; b < MUSIC_BANDS; b++) peak[b] = -60;
    peak_all = -60; memset(shown, 0, sizeof shown); shown_all = 0; bass_avg = 0; bass_prev = 0;
    quiet_ms = 0; last_beat_heard = 0; nhist = 0; head = 0; memset(beats, 0, sizeof beats); memset(strengths, 0, sizeof strengths);
    pthread_mutex_unlock(&lock);
}
