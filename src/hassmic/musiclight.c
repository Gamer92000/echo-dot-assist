/*
 * The music for the live ring (musiclight.h).  Every 10 ms block of what the Echo plays:
 *   - twelve band-passes (RBJ biquads, constant 0 dB peak, Q 1.8) at 50 Hz .. 12 kHz spaced evenly in log frequency,
 *     their energy in dB, and the whole's, left's and right's;
 *   - each shown against its own range: a peak that falls 1.5 dB a second and a floor that rises 2 dB a second (down at
 *     once), at least 18 dB apart (26 for the level): a song mastered loud and flat still swings, a quiet one fills;
 *     shown values rise at once and fall with 120 ms, as a meter's;
 *   - drums from the spectral flux: the rise of each band's log-compressed amplitude (log10(1 + 10 a): a loud rise
 *     counts, a quiet note out of silence little), rises only.  The kick: the lowest bands' rise less the mids', while
 *     the bass is within 3 dB of its own peak (falling 1.5 dB/s); the snare: bands 5-8 less half the hi-hat's, while
 *     they are within 3 dB of theirs (it comes with the kick in four-on-the-floor, so a kick does not rule it out);
 *     the hi-hat: the two highest.  An onset where the flux peaks above the mean + 1.4-1.5 standard deviations of its
 *     last second and twice its usual rise, after 150, 110, 70 ms; one block late, to see the peak.  Measured on
 *     tools/ringmusic.py's track (tools/ringviz.py): kick F 0.93, snare 0.64, hi-hat 0.92; a level-blind dB flux gave
 *     0.46, 0.28 and 0.87 (bass notes and chords out of silence counted as drums);
 *   - the tempo: the autocorrelation of all bands' flux over the last 6 s, every 250 ms, between 60 and 180 bpm,
 *     weighted towards 120 (an octave either side halves it) and helped by twice the lag; a new one taken when it agrees
 *     with itself three times; the beats on that grid, its phase pulled 30 % towards each kick near one; until the
 *     tempo is sure, the kicks are the beats.  The bar's first beat: the beat place (of 4) where kicks outweigh snares;
 *   - the energy: the level over 0.4 s against the last 8 s; how bright: the centroid of the shown bands.
 * Each reading is stamped with when it is heard: now + what the output holds + where in the piece it lies.
 * About 3 M multiply-adds a second at 48 kHz (1-2 % of a Dot 3's core), only while an effect wants them.
 * tools/ringmusic.py makes a track with its drums written down, tools/ringviz shows what this finds in it.
 */
#include "musiclight.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define BLOCK_MS 10
#define HIST     256                    /* readings kept: 2.5 s, more than any output holds */
#define ODF_N    600                    /* 6 s of flux for the tempo */
#define WIN      100                    /* 1 s for the drums' thresholds */
#define LAG_MIN  33                     /* 180 bpm in blocks */
#define LAG_MAX  100                    /* 60 bpm */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int wanted;
static long long (*clockfn)(void);
static void (*debugfn)(long long, const float *, const float *);

static struct biquad { float b0, b2, a1, a2, z1, z2; } bq[MUSIC_BANDS];     /* b1 = 0 for a band-pass */
static unsigned at_rate;
static double acc[MUSIC_BANDS], acc_l, acc_r;
static unsigned acc_n;
static long long blk;                   /* blocks analysed */

struct range { float peak, floor; };
static struct range rb[MUSIC_BANDS], rl;
static float c_prev[MUSIC_BANDS], shown[MUSIC_BANDS], shown_l, shown_r, shown_lv, low_pk = -60, mid_pk = -60;
static float e_short = -60, e_long = -60, bright_s, avg_all = -60, avg_l = -60, avg_r = -60;
static long long quiet_blocks;

struct det { float hist[WIN]; int n, at; double sum, sq; float prev2, prev1, usual; long long last; float k; int refr; struct music_hits hits; };
static struct det dk = { .k = 1.4f, .refr = 15 }, ds = { .k = 1.5f, .refr = 11 }, dh = { .k = 1.5f, .refr = 7 };

static float odf[ODF_N]; static int odf_at, odf_n;
static float period = 48.4f, cand; static int cand_n;           /* blocks a beat; 124 bpm to start with */
static float confidence;
static double next_beat; static long long beat_heard; static int beat_no, beats_since_kick;
static float place_kick[4], place_snare[4], place_harm[4], harm_recent[4]; static int downbeat;
static long long kick_blk[16]; static float kick_str[16]; static int nkick;      /* the last kicks, for the grid's phase */

static struct reading { long long at; struct music_feat f; } hist[HIST];
static int nhist, head;

static long long mono_ms(void)
{
    if (clockfn) return clockfn();
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static float centre(int b) { return 50 * powf(12000.0f / 50, (float)b / (MUSIC_BANDS - 1)); }

static void design(unsigned rate)
{
    for (int b = 0; b < MUSIC_BANDS; b++) {
        float f = centre(b); if (f > rate * 0.45f) f = rate * 0.45f;
        float w = 2 * (float)M_PI * f / rate, al = sinf(w) / (2 * 1.8f), a0 = 1 + al;
        bq[b] = (struct biquad){ al / a0, -al / a0, -2 * cosf(w) / a0, (1 - al) / a0, 0, 0 };
    }
    at_rate = rate;
}

/* a value against its range: the peak falls 1.5 dB a second, the floor rises 2 (falls at once), span at least min */
static float against(struct range *r, float db, float min)
{
    r->peak = db > r->peak ? db : r->peak - 0.015f;
    r->floor = db < r->floor ? db : r->floor + 0.02f;
    if (r->peak < -60) r->peak = -60;
    float span = r->peak - r->floor; if (span < min) span = min;
    float v = (db - (r->peak - span)) / span;
    return v < 0 ? 0 : v > 1 ? 1 : v;
}
static float meter(float was, float now) { const float fall = 1 - expf(-(float)BLOCK_MS / 120); return now >= was ? now : was + (now - was) * fall; }
static void hit(struct music_hits *h, long long at, float s)
{
    memmove(h->at + 1, h->at, sizeof h->at[0] * (MUSIC_HITS - 1)); memmove(h->strength + 1, h->strength, sizeof h->strength[0] * (MUSIC_HITS - 1));
    h->at[0] = at; h->strength[0] = s;
}

/* a drum's flux this block: an onset (its strength 0.4..1, 0 none) for the block before, one late to see the peak;
 * gate: the drum's band loud enough now */
static float detect(struct det *d, float flux, int gate)
{
    float mean = d->n ? (float)(d->sum / d->n) : 0, sd = d->n ? sqrtf(fmaxf(0, (float)(d->sq / d->n) - mean * mean)) : 0;
    float thr = mean + d->k * sd, s = 0, floor_ = 2 * d->usual;
    if (gate && d->prev1 > thr && d->prev1 > floor_ && d->prev1 >= d->prev2 && d->prev1 > flux && blk - 1 - d->last >= d->refr) {
        s = 0.4f + 0.6f * fminf(1, (d->prev1 - thr) / (2 * sd + floor_ + 1e-6f));
        d->last = blk - 1;
    }
    if (flux > 0) d->usual += (flux - d->usual) * 0.005f;        /* its usual rise, ~2 s */
    if (d->n == WIN) { d->sum -= d->hist[d->at]; d->sq -= (double)d->hist[d->at] * d->hist[d->at]; } else d->n++;
    d->hist[d->at] = flux; d->sum += flux; d->sq += (double)flux * flux; d->at = (d->at + 1) % WIN;
    d->prev2 = d->prev1; d->prev1 = flux;
    return s;
}

/* the tempo: autocorrelation of the flux over the last 6 s, lags of 60..180 bpm */
static void tempo(void)
{
    if (odf_n < ODF_N) return;
    float m = 0, x[ODF_N];
    for (int i = 0; i < ODF_N; i++) m += odf[i];
    m /= ODF_N;
    for (int i = 0; i < ODF_N; i++) x[i] = odf[(odf_at + i) % ODF_N] - m;
    double ac0 = 0, ac[2 * LAG_MAX + 2];
    for (int i = 0; i < ODF_N; i++) ac0 += (double)x[i] * x[i];
    if (ac0 <= 0) return;
    for (int l = LAG_MIN - 1; l <= 2 * LAG_MAX + 1 && l < ODF_N; l++) { double s = 0; for (int i = l; i < ODF_N; i++) s += (double)x[i] * x[i - l]; ac[l] = s / (ODF_N - l); }
    int best = 0; double bs = -1e30;
    for (int l = LAG_MIN; l <= LAG_MAX; l++) {
        float bpm = 6000.0f / l, w = expf(-0.5f * powf(log2f(bpm / 120) / 0.9f, 2));
        double s = (ac[l] + 0.5 * ac[2 * l]) * w;
        if (s > bs) { bs = s; best = l; }
    }
    double y0 = ac[best - 1], y1 = ac[best], y2 = ac[best + 1], den = y0 - 2 * y1 + y2;
    float lag = best + (den < 0 ? (float)(0.5 * (y0 - y2) / den) : 0);
    float conf = (float)(ac[best] / (ac0 / ODF_N)); conf = conf < 0 ? 0 : conf > 1 ? 1 : conf;
    confidence += (conf - confidence) * 0.3f;
    if (fabsf(lag - period) / period < 0.06f) { period += (lag - period) * 0.25f; cand_n = 0; }
    else if (cand && fabsf(lag - cand) / cand < 0.06f) { if (++cand_n >= 3) { period = lag; cand_n = 0; } }
    else { cand = lag; cand_n = 1; }
}

/* lock held: one block done, its end heard at `heard` */
static void block(long long heard)
{
    float db[MUSIC_BANDS], d[MUSIC_BANDS];
    for (int b = 0; b < MUSIC_BANDS; b++) { db[b] = 10 * log10f((float)(acc[b] / acc_n) + 1e-12f); acc[b] = 0; }
    float dl = 10 * log10f((float)(acc_l / acc_n) + 1e-12f), dr = 10 * log10f((float)(acc_r / acc_n) + 1e-12f);
    float dall = 10 * log10f((float)((acc_l + acc_r) / 2 / acc_n) + 1e-12f);
    acc_l = acc_r = 0; acc_n = 0;
    int silent = dall < -55;
    quiet_blocks = silent ? quiet_blocks + 1 : 0;

    float sum_v = 0, cen = 0;
    for (int b = 0; b < MUSIC_BANDS; b++) {
        float v = silent ? 0 : against(&rb[b], db[b], 18);
        shown[b] = meter(shown[b], v); sum_v += shown[b]; cen += b * shown[b];
        float cb = log10f(1 + 10 * powf(10, db[b] / 20));            /* log-compressed amplitude */
        d[b] = blk ? fmaxf(0, cb - c_prev[b]) : 0;
        c_prev[b] = cb;
    }
    if (debugfn) debugfn(heard, db, d);
    /* the level as punch: against its own last 1.5 s (0.28 = as loud as that, 1/16 a dB above or under; shown with
     * the meter's release, about half on the whole); against a range it sat at the top all the time in music mastered
     * loud (the meter was always full) */
    if (!silent && avg_all < -59) avg_all = avg_l = avg_r = e_short = e_long = dall;   /* the first music: its own means at once */
    if (!silent) { avg_all += (dall - avg_all) * 0.0067f; avg_l += (dl - avg_l) * 0.0067f; avg_r += (dr - avg_r) * 0.0067f; }
    (void)against(&rl, dall, 26);
    shown_lv = meter(shown_lv, silent ? 0 : fminf(1, fmaxf(0, 0.28f + (dall - avg_all) / 16)));
    shown_l = meter(shown_l, silent ? 0 : fminf(1, fmaxf(0, 0.28f + (dl - avg_all) / 16)));      /* both on the whole's mean: */
    shown_r = meter(shown_r, silent ? 0 : fminf(1, fmaxf(0, 0.28f + (dr - avg_all) / 16)));      /* their balance shows */
    if (sum_v > 0.05f) bright_s += (cen / sum_v / (MUSIC_BANDS - 1) - bright_s) * 0.02f;
    if (!silent) { e_short += (dall - e_short) * 0.025f; e_long += (dall - e_long) * 0.00125f; }

    /* drums; the onsets are of the block before (one late, to see the peak) */
    long long prev_heard = heard - BLOCK_MS;
    float fk = d[0] + d[1] + 0.6f * d[2] - 0.3f * (d[3] + d[4] + d[5] + d[6] + d[7]);
    float fs = d[5] + d[6] + d[7] + d[8] - 0.5f * (d[10] + d[11]), fh = d[10] + d[11], all = 0;
    for (int b = 0; b < MUSIC_BANDS; b++) all += d[b];
    float low = 10 * log10f(powf(10, db[0] / 10) + powf(10, db[1] / 10) + powf(10, db[2] / 10));
    float mid = 10 * log10f(powf(10, db[6] / 10) + powf(10, db[7] / 10) + powf(10, db[8] / 10));
    low_pk = low > low_pk ? low : low_pk - 0.015f; mid_pk = mid > mid_pk ? mid : mid_pk - 0.015f;
    float sk = silent ? 0 : detect(&dk, fk, low > low_pk - 3), ss = silent ? 0 : detect(&ds, fs, mid > mid_pk - 3), sh = silent ? 0 : detect(&dh, fh, 1);
    if (sk > 0) hit(&dk.hits, prev_heard, sk);
    if (ss > 0) hit(&ds.hits, prev_heard, ss);
    if (sh > 0) hit(&dh.hits, prev_heard, sh);
    memmove(harm_recent + 1, harm_recent, sizeof harm_recent[0] * 3);     /* the chords' change: the mids' rise */
    harm_recent[0] = d[3] + d[4] + d[5] + d[6] + d[7];
    odf[odf_at] = all; odf_at = (odf_at + 1) % ODF_N; if (odf_n < ODF_N) odf_n++;
    if (sk > 0) { memmove(kick_blk + 1, kick_blk, sizeof kick_blk[0] * 15); memmove(kick_str + 1, kick_str, sizeof kick_str[0] * 15); kick_blk[0] = blk - 1; kick_str[0] = sk; if (nkick < 16) nkick++; }
    if (blk % 25 == 0) {
        tempo();
        /* the grid's phase: the kicks of the last 4 s, as a circular mean over the beat; far off, the grid jumps there
         * (it started anywhere), near, the kicks pull it below */
        double cx = 0, cy = 0; int k = 0;
        for (int i = 0; i < nkick; i++) if (blk - kick_blk[i] < 400) {
            double a = 2 * M_PI * fmod((double)kick_blk[i], period) / period; cx += kick_str[i] * cos(a); cy += kick_str[i] * sin(a); k++;
        }
        if (k >= 4 && confidence > 0.2f && sqrt(cx * cx + cy * cy) / k > 0.5) {
            double want = atan2(cy, cx) / (2 * M_PI) * period; if (want < 0) want += period;
            double have = fmod(next_beat, period), e = want - have;
            if (e > period / 2) e -= period; else if (e < -period / 2) e += period;
            if (fabs(e) > 0.15 * period) next_beat += e;
        }
    }

    /* the beat */
    int sure = confidence > 0.2f;
    if (sure) {
        if (next_beat < blk - period) next_beat = blk;              /* lost (a pause): from here */
        while (blk >= next_beat) {
            beat_heard = heard - (long long)((blk - next_beat) * BLOCK_MS);
            beat_no++; beats_since_kick++;
            next_beat += period;
            float h = fmaxf(fmaxf(harm_recent[0], harm_recent[1]), fmaxf(harm_recent[2], harm_recent[3]));
            int place = ((beat_no % 4) + 4) % 4;
            for (int i = 0; i < 4; i++) place_harm[i] *= 0.98f;
            place_harm[place] += h;
        }
        if (sk > 0) {                                               /* pull the grid towards a kick near a beat */
            double o = blk - 1, prev = next_beat - period, e = fabs(o - prev) < fabs(o - next_beat) ? o - prev : o - next_beat;
            if (fabs(e) < 0.2 * period) { next_beat += 0.3 * e; beats_since_kick = 0; }
        }
    } else if (sk > 0) {                                            /* no tempo yet: the kicks are the beats */
        beat_heard = prev_heard; beat_no++; next_beat = blk - 1 + period;
    }
    /* the bar's first beat: the place of 4 with the kick rather than the snare (2 and 4), and where the chords change
     * (four on the floor has a kick on every beat: the change tells 1 from 3).  Each against its mean over the places */
    if (sk > 0 || ss > 0) {
        int place = ((beat_no % 4) + 4) % 4;
        for (int i = 0; i < 4; i++) { place_kick[i] *= 0.995f; place_snare[i] *= 0.995f; }
        if (sk > 0) place_kick[place] += sk;
        if (ss > 0) place_snare[place] += ss;
    }
    if (blk % 25 == 0) {
        float mk = 1e-6f, ms_ = 1e-6f, mh = 1e-6f, v[4]; int top = 0;
        for (int i = 0; i < 4; i++) { mk += place_kick[i] / 4; ms_ += place_snare[i] / 4; mh += place_harm[i] / 4; }
        for (int i = 0; i < 4; i++) { v[i] = place_kick[i] / mk - 0.8f * place_snare[i] / ms_ + place_harm[i] / mh; if (v[i] > v[top]) top = i; }
        if (v[top] > v[downbeat] + 0.3f) downbeat = top;          /* a clear winner only: the accent does not wander */
    }
    blk++;

    struct reading *r = &hist[head];
    struct music_feat *f = &r->f;
    r->at = heard;
    f->active = quiet_blocks < 100;
    f->level = shown_lv; f->left = shown_l; f->right = shown_r;
    memcpy(f->band, shown, sizeof shown);
    f->energy = fminf(1, fmaxf(0, 0.5f + (e_short - e_long) / 16));
    f->bright = bright_s;
    f->kick = dk.hits; f->snare = ds.hits; f->hat = dh.hits;
    f->beat_at = beat_heard; f->beat_no = beat_no - downbeat; f->beat_ms = period * BLOCK_MS; f->bpm = 6000.0f / period;
    f->confidence = confidence;
    head = (head + 1) % HIST; if (nhist < HIST) nhist++;
}

void musiclight_want(int on) { atomic_store(&wanted, !!on); }
void musiclight_debug(void (*fn)(long long, const float *, const float *)) { pthread_mutex_lock(&lock); debugfn = fn; pthread_mutex_unlock(&lock); }
void musiclight_clock(long long (*fn)(void)) { pthread_mutex_lock(&lock); clockfn = fn; pthread_mutex_unlock(&lock); }

void musiclight_feed(const int16_t *pcm, size_t frames, int ch, unsigned rate, long long delay_us)
{
    if (!atomic_load(&wanted) || !pcm || ch < 1 || rate < 8000) return;
    pthread_mutex_lock(&lock);
    long long base = mono_ms() + (delay_us > 0 ? delay_us / 1000 : 0);
    unsigned per = rate * BLOCK_MS / 1000;
    if (!at_rate) for (int b = 0; b < MUSIC_BANDS; b++) { rb[b].peak = -60; rb[b].floor = 0; }   /* the first music: found at once */
    if (!at_rate) { rl.peak = -60; rl.floor = 0; }
    if (rate != at_rate) { design(rate); acc_n = 0; acc_l = acc_r = 0; memset(acc, 0, sizeof acc); }
    for (size_t i = 0; i < frames; i++) {
        float l = pcm[i * ch] / 32768.0f, r = ch > 1 ? pcm[i * ch + 1] / 32768.0f : l, x = 0.5f * (l + r);
        acc_l += (double)l * l; acc_r += (double)r * r;
        for (int b = 0; b < MUSIC_BANDS; b++) {
            struct biquad *q = &bq[b];
            float y = q->b0 * x + q->z1;
            q->z1 = -q->a1 * y + q->z2; q->z2 = q->b2 * x - q->a2 * y;
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
        struct music_hits *hs[] = { &f->kick, &f->snare, &f->hat };      /* only what is heard by now */
        for (int h = 0; h < 3; h++) {
            struct music_hits o = *hs[h]; int k = 0; memset(hs[h], 0, sizeof *hs[h]);
            for (int i = 0; i < MUSIC_HITS; i++) if (o.at[i] && o.at[i] <= now) { hs[h]->at[k] = o.at[i]; hs[h]->strength[k++] = o.strength[i]; }
        }
    }
    pthread_mutex_unlock(&lock);
}

void musiclight_reset(void)
{
    pthread_mutex_lock(&lock);
    memset(bq, 0, sizeof bq); at_rate = 0; memset(acc, 0, sizeof acc); acc_l = acc_r = 0; acc_n = 0; blk = 0;
    memset(rb, 0, sizeof rb); memset(&rl, 0, sizeof rl); memset(c_prev, 0, sizeof c_prev); memset(shown, 0, sizeof shown); low_pk = mid_pk = -60;
    shown_l = shown_r = shown_lv = 0; e_short = e_long = -60; bright_s = 0; quiet_blocks = 0; avg_all = avg_l = avg_r = -60;
    struct det *ds_[] = { &dk, &ds, &dh };
    for (int i = 0; i < 3; i++) { struct det *d = ds_[i]; float k = d->k; int rf = d->refr; memset(d, 0, sizeof *d); d->k = k; d->refr = rf; }
    memset(odf, 0, sizeof odf); odf_at = odf_n = 0; period = 48.4f; cand = 0; cand_n = 0; confidence = 0;
    next_beat = 0; beat_heard = 0; beat_no = 0; beats_since_kick = 0; nkick = 0;
    memset(place_kick, 0, sizeof place_kick); memset(place_snare, 0, sizeof place_snare); memset(place_harm, 0, sizeof place_harm);
    memset(harm_recent, 0, sizeof harm_recent); downbeat = 0;
    nhist = 0; head = 0;
    pthread_mutex_unlock(&lock);
}
