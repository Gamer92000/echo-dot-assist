/* Wake word clips: see wakeclip.h */
#include "wakeclip.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXC (WAKECLIP_KEEP * WAKECLIP_KINDS)

struct clip {
    struct wakeclip_info i;
    int16_t *pcm;
    uint64_t got;                   /* the next sample of the engine's index to copy in */
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct clip clips[MAXC];     /* pcm NULL: a free place */
static unsigned next_id = 1;
static atomic_int on, pending;      /* pending: clips still waiting for samples (the capture thread's quick look) */
static atomic_int after_ms = 1000;

static void drop(struct clip *c) { free(c->pcm); memset(c, 0, sizeof *c); }

static void drop_all(void)
{
    for (int i = 0; i < MAXC; i++) if (clips[i].pcm) drop(&clips[i]);
    atomic_store(&pending, 0);
}

void wakeclip_set(int enable, int ms)
{
    if (ms < 0) ms = 0;
    if (ms > WAKECLIP_AFTER_MAX_MS) ms = WAKECLIP_AFTER_MAX_MS;
    pthread_mutex_lock(&mu);
    atomic_store(&after_ms, ms);
    if (!enable && atomic_load(&on)) drop_all();
    atomic_store(&on, !!enable);
    pthread_mutex_unlock(&mu);
}

int wakeclip_enabled(void) { return atomic_load(&on); }
int wakeclip_after_ms(void) { return atomic_load(&after_ms); }

static float db_of(const int16_t *s, size_t n)
{
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (double)s[i] * s[i];
    return n ? (float)(10 * log10((sum / n + 1) / (32768.0 * 32768.0))) : -100.0f;
}

/* every sample is in: the levels the page lists */
static void finish(struct clip *c)
{
    int peak = 0;
    for (uint32_t k = 0; k < c->i.len; k++) { int a = abs(c->pcm[k]); if (a > peak) peak = a; }
    c->i.peak = peak > 32767 ? 32767 : peak;
    c->i.word_db = db_of(c->pcm + c->i.word_at, c->i.word_len);
    c->i.floor_db = db_of(c->pcm, c->i.word_at);
    c->i.done = 1;
}

void wakeclip_mark(int kind, const char *word, uint64_t begin, uint64_t end, int playing, long long wall, long long mono_ms)
{
    if (!atomic_load(&on) || kind < 0 || kind >= WAKECLIP_KINDS) return;
    const uint64_t pre = (uint64_t)WAKECLIP_RATE * WAKECLIP_PRE_MS / 1000, most = (uint64_t)WAKECLIP_RATE * WAKECLIP_WORD_MAX_MS / 1000;
    if (end <= begin) begin = end > WAKECLIP_RATE / 2 ? end - WAKECLIP_RATE / 2 : 0;    /* an engine without a begin: half a second */
    if (end - begin > most) begin = end - most;
    uint64_t from = begin > pre ? begin - pre : 0;
    uint32_t len = (uint32_t)(end - from) + (uint32_t)((uint64_t)WAKECLIP_RATE * (unsigned)atomic_load(&after_ms) / 1000);
    int16_t *pcm = calloc(len, sizeof *pcm);
    if (!pcm) { fprintf(stderr, "wake clip: no memory for %u samples\n", len); return; }

    pthread_mutex_lock(&mu);
    if (!atomic_load(&on)) { pthread_mutex_unlock(&mu); free(pcm); return; }
    /* the oldest of its kind goes when ten are kept: a run of near misses never pushes out the wake words */
    int n = 0, oldest = -1, free_at = -1;
    for (int k = 0; k < MAXC; k++) {
        if (!clips[k].pcm) { if (free_at < 0) free_at = k; continue; }
        if (clips[k].i.kind != kind) continue;
        n++;
        if (oldest < 0 || clips[k].i.id < clips[oldest].i.id) oldest = k;
    }
    if (n >= WAKECLIP_KEEP) {
        if (!clips[oldest].i.done) atomic_fetch_sub(&pending, 1);
        drop(&clips[oldest]); free_at = oldest;
    }
    struct clip *c = &clips[free_at];
    c->pcm = pcm; c->got = from;
    c->i.id = next_id++; if (!next_id) next_id = 1;
    c->i.kind = kind; c->i.playing = playing; c->i.wall = wall; c->i.mono_ms = mono_ms;
    /* the engine's own words: letters, digits, '-', '_' ("ALEXA", "hey_jarvis"); the list is JSON */
    size_t w = 0;
    for (const char *p = word ? word : ""; *p && w < sizeof c->i.word - 1; p++)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_') c->i.word[w++] = *p;
    c->i.from = from; c->i.len = len;
    c->i.word_at = (uint32_t)(begin - from); c->i.word_len = (uint32_t)(end - begin);
    atomic_fetch_add(&pending, 1);
    pthread_mutex_unlock(&mu);
}

void wakeclip_feed(const int16_t *ring, size_t cap, uint64_t ring_n)
{
    if (!atomic_load(&pending)) return;
    pthread_mutex_lock(&mu);
    uint64_t oldest = ring_n > cap ? ring_n - cap : 0;
    for (int k = 0; k < MAXC; k++) {
        struct clip *c = &clips[k];
        if (!c->pcm || c->i.done) continue;
        uint64_t to = c->i.from + c->i.len;
        if (c->got < oldest) c->got = oldest;           /* out of the ring before it was marked: stays silent */
        for (uint64_t s = c->got; s < to && s < ring_n; s++) c->pcm[s - c->i.from] = ring[s % cap];
        if (ring_n > c->got) c->got = ring_n < to ? ring_n : to;
        if (c->got >= to) { finish(c); atomic_fetch_sub(&pending, 1); }
    }
    pthread_mutex_unlock(&mu);
}

void wakeclip_outcome(const char *what, long long now_ms)
{
    if (!atomic_load(&on) || !what) return;
    pthread_mutex_lock(&mu);
    int newest = -1;
    for (int k = 0; k < MAXC; k++)          /* a "stop" is no run's wake word: it never has one */
        if (clips[k].pcm && clips[k].i.kind == WAKECLIP_HIT && strcmp(clips[k].i.word, "STOP") && (newest < 0 || clips[k].i.id > clips[newest].i.id)) newest = k;
    if (newest >= 0 && !clips[newest].i.outcome[0] && now_ms - clips[newest].i.mono_ms <= WAKECLIP_OUTCOME_MS)
        snprintf(clips[newest].i.outcome, sizeof clips[newest].i.outcome, "%s", what);
    pthread_mutex_unlock(&mu);
}

/* the clips' places, newest first */
static int order(int *at)
{
    int n = 0;
    for (int k = 0; k < MAXC; k++) if (clips[k].pcm) at[n++] = k;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && clips[at[b]].i.id > clips[at[b - 1]].i.id; b--) { int t = at[b]; at[b] = at[b - 1]; at[b - 1] = t; }
    return n;
}

int wakeclip_get(int i, struct wakeclip_info *out)
{
    int at[MAXC], n, r = -1;
    pthread_mutex_lock(&mu);
    n = order(at);
    if (i >= 0 && i < n) { *out = clips[at[i]].i; r = 0; }
    pthread_mutex_unlock(&mu);
    return r;
}

static struct clip *find(unsigned id)
{
    for (int k = 0; k < MAXC; k++) if (clips[k].pcm && clips[k].i.id == id) return &clips[k];
    return NULL;
}

int16_t *wakeclip_audio(unsigned id, size_t *n)
{
    int16_t *copy = NULL; struct clip *c;
    pthread_mutex_lock(&mu);
    if ((c = find(id)) && c->i.done && (copy = malloc(c->i.len * sizeof *copy))) {
        memcpy(copy, c->pcm, c->i.len * sizeof *copy); *n = c->i.len;
    }
    pthread_mutex_unlock(&mu);
    return copy;
}

int wakeclip_label(unsigned id, int label)
{
    struct clip *c; int r = -1;
    if (label < WAKECLIP_UNLABELLED || label > WAKECLIP_NOT_WAKE) return -1;
    pthread_mutex_lock(&mu);
    if ((c = find(id))) { c->i.label = label; r = 0; }
    pthread_mutex_unlock(&mu);
    return r;
}

int wakeclip_delete(unsigned id)
{
    struct clip *c; int r = 0;
    pthread_mutex_lock(&mu);
    if (!id) drop_all();
    else if ((c = find(id))) { if (!c->i.done) atomic_fetch_sub(&pending, 1); drop(c); }
    else r = -1;
    pthread_mutex_unlock(&mu);
    return r;
}

/* The page's waveform without the audio: the loudest sample of each of WAKECLIP_ENV stretches, 0..255 for -60..0 dBFS,
 * in hex.  Unsealing every clip to draw it took the page ~0.2 s a clip on a PC */
static void envelope(const struct clip *c, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (int b = 0; b < WAKECLIP_ENV; b++) {
        uint32_t s0 = (uint32_t)((uint64_t)c->i.len * b / WAKECLIP_ENV), s1 = (uint32_t)((uint64_t)c->i.len * (b + 1) / WAKECLIP_ENV);
        int peak = 0;
        for (uint32_t k = s0; k < s1; k++) { int a = abs(c->pcm[k]); if (a > peak) peak = a; }
        double db = peak ? 20 * log10(peak / 32768.0) : -60;
        int v = db <= -60 ? 0 : db >= 0 ? 255 : (int)((db + 60) / 60 * 255 + 0.5);
        out[2 * b] = hx[v >> 4]; out[2 * b + 1] = hx[v & 15];
    }
    out[2 * WAKECLIP_ENV] = 0;
}

static const char *const kinds[WAKECLIP_KINDS] = { "wake", "near_miss" };
static const char *const labels[] = { "", "wake", "not_wake" };

size_t wakeclip_json(char *out, size_t cap, long long now_ms)
{
    int at[MAXC], n; size_t o = 0;
    if (!cap) return 0;
#define PUT(...) do { int m_ = snprintf(out + o, o < cap ? cap - o : 0, __VA_ARGS__); if (m_ > 0) o += (size_t)m_; } while (0)
    pthread_mutex_lock(&mu);
    n = order(at);
    PUT("{\"on\":%s,\"now_ms\":%lld,\"after_ms\":%d,\"pre_ms\":%d,\"rate\":%d,\"keep\":%d,\"clips\":[", atomic_load(&on) ? "true" : "false", now_ms,
        atomic_load(&after_ms), WAKECLIP_PRE_MS, WAKECLIP_RATE, WAKECLIP_KEEP);
    for (int k = 0; k < n; k++) {
        const struct wakeclip_info *i = &clips[at[k]].i; char env[2 * WAKECLIP_ENV + 1] = "";
        if (i->done) envelope(&clips[at[k]], env);
        PUT("%s{\"id\":%u,\"kind\":\"%s\",\"word\":\"%s\",\"wall\":%lld,\"mono_ms\":%lld,\"from\":%llu,\"len\":%u,\"word_at\":%u,\"word_len\":%u,"
            "\"done\":%s,\"word_db\":%.1f,\"floor_db\":%.1f,\"peak\":%d,\"music\":%s,\"reply\":%s,\"ringing\":%s,\"sound\":%s,\"outcome\":\"%s\",\"label\":\"%s\",\"env\":\"%s\"}",
            k ? "," : "", i->id, kinds[i->kind], i->word, i->wall, i->mono_ms, (unsigned long long)i->from, i->len, i->word_at, i->word_len,
            i->done ? "true" : "false", i->done ? i->word_db : 0.0f, i->done ? i->floor_db : 0.0f, i->peak,
            i->playing & WAKECLIP_MUSIC ? "true" : "false", i->playing & WAKECLIP_REPLY ? "true" : "false",
            i->playing & WAKECLIP_RINGING ? "true" : "false", i->playing & WAKECLIP_SOUND ? "true" : "false", i->outcome, labels[i->label], env);
    }
    PUT("]}");
    pthread_mutex_unlock(&mu);
#undef PUT
    if (o >= cap) { out[cap - 1] = 0; return cap - 1; }
    return o;
}
