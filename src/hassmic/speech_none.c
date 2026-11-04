/* No voice of the Echo's own (speech.h): the Dots' firmware has no speech engine, the PC build none either.
 *
 * HASSMIC_FAKE_SPEECH=1 (tests) stands in for speech_ivona.c with its contract: installed and ready at once, voice
 * "Fake" (en-US), one sentence after the other in the order given, end for every sentence taken (also cancelled or
 * dropped from the queue), renders for others kept by speech_cancel.  A sentence is a 300 Hz tone of FAKE_MS_CHAR
 * per character (at least FAKE_MS_MIN) at FAKE_RATE, made 8x faster than real time in 20 ms pieces (Ivona: ~6x), so a
 * test knows each sentence's length in bytes: chars * 640 at 16 kHz.  It logs as speech_ivona.c does. */
#include "speech.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FAKE_RATE 16000
#define FAKE_MS_CHAR 20
#define FAKE_MS_MIN 200
#define QUEUE 32

static int fake(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("HASSMIC_FAKE_SPEECH"); on = e && *e == '1'; }
    return on;
}

static const struct speech_out *out;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static char *queue[QUEUE]; static int tags[QUEUE], q_head, q_n;     /* lk */
static const struct speech_sink *sinks[QUEUE];                      /* lk: NULL = played here */
static const struct speech_sink *sink_now;                          /* lk */
static atomic_int cancel_now;

static void *thread(void *arg)
{
    (void)arg;
    static int16_t pcm[FAKE_RATE / 50];
    for (;;) {
        pthread_mutex_lock(&lk);
        while (!q_n) pthread_cond_wait(&cv, &lk);
        char *text = queue[q_head]; int tag = tags[q_head]; const struct speech_sink *k = sinks[q_head];
        q_head = (q_head + 1) % QUEUE; q_n--;
        sink_now = k; atomic_store(&cancel_now, 0);
        pthread_mutex_unlock(&lk);
        long ms = (long)strlen(text) * FAKE_MS_CHAR; if (ms < FAKE_MS_MIN) ms = FAKE_MS_MIN;
        int pieces = (int)(ms / 20), cancelled = 0;
        for (int p = 0; p < pieces; p++) {
            if (atomic_load(&cancel_now)) { cancelled = 1; break; }
            for (size_t i = 0; i < sizeof pcm / sizeof *pcm; i++)
                pcm[i] = (int16_t)(6000 * sin(2 * M_PI * 300 * (double)(p * (FAKE_RATE / 50) + i) / FAKE_RATE));
            if (!p) { if (k) k->begin(k->arg, FAKE_RATE); else out->begin(FAKE_RATE, tag); }
            if (k) k->data(k->arg, pcm, sizeof pcm); else out->data(pcm, sizeof pcm);
            usleep(20000 / 8);
        }
        if (k) k->end(k->arg, !cancelled); else out->end(tag);
        fprintf(stderr, "speech: \"%.60s%s\" %s in %ld ms\n", text, strlen(text) > 60 ? "..." : "", cancelled ? "cancelled" : "said", ms / 8);
        pthread_mutex_lock(&lk); sink_now = NULL; pthread_mutex_unlock(&lk);
        free(text);
    }
    return NULL;
}

void speech_start(const struct speech_out *o)
{
    pthread_t t;
    if (!fake()) return;
    out = o;
    if (pthread_create(&t, NULL, thread, NULL) == 0) pthread_detach(t);
}
int  speech_installed(void) { return fake(); }
int  speech_ready(void) { return fake() && out; }
const char *speech_voice(void) { return fake() ? "Fake" : ""; }
const char *speech_language(void) { return fake() ? "en-US" : ""; }

static int enqueue(const char *text, int tag, const struct speech_sink *sink)
{
    if (!speech_ready() || !text || !*text) return -1;
    char *t = strdup(text);
    if (!t) return -1;
    pthread_mutex_lock(&lk);
    if (q_n == QUEUE) { pthread_mutex_unlock(&lk); free(t); return -1; }
    int i = (q_head + q_n) % QUEUE;
    queue[i] = t; tags[i] = tag; sinks[i] = sink; q_n++;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lk);
    return 0;
}
int  speech_say(const char *text, int tag) { return enqueue(text, tag, NULL); }
int  speech_render(const char *text, const struct speech_sink *sink) { return sink ? enqueue(text, 0, sink) : -1; }

void speech_cancel(void)
{
    if (!fake()) return;
    pthread_mutex_lock(&lk);
    int dropped[QUEUE], n = 0, keep = 0;
    char *kq[QUEUE]; int kt[QUEUE]; const struct speech_sink *ks[QUEUE];
    while (q_n) {                                               /* ours go, renders for others stay, in order */
        int i = q_head; q_head = (q_head + 1) % QUEUE; q_n--;
        if (sinks[i]) { kq[keep] = queue[i]; kt[keep] = tags[i]; ks[keep++] = sinks[i]; }
        else { free(queue[i]); dropped[n++] = tags[i]; }
    }
    for (int i = 0; i < keep; i++) { int j = (q_head + q_n) % QUEUE; queue[j] = kq[i]; tags[j] = kt[i]; sinks[j] = ks[i]; q_n++; }
    if (!sink_now) atomic_store(&cancel_now, 1);
    pthread_mutex_unlock(&lk);
    for (int i = 0; i < n; i++) out->end(dropped[i]);
}

static const char *const fake_names[] = { "fake" };
int  speech_voices(const char *const **names) { if (!fake()) { *names = 0; return 0; } *names = fake_names; return 1; }
int  speech_voice_index(void) { return fake() ? 0 : -1; }
void speech_voice_select(int i) { (void)i; }
