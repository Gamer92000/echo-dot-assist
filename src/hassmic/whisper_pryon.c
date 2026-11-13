/* Whisper detection on Amazon's libpryon.so (docs/re-whisper.md), called as stock's AHE does: one detector per request,
 * audio pushed with its running sample index, the end of utterance in 10 ms frames, one "result" event after it.
 * The model is DAVS's "whisper-static" (a DNN on 64 LFBE features behind a DNN speech detector), installed by
 * scripts/artifacts.sh into its own directory: it carries a stray pryon.manifest that the wake word scan of models/
 * would take for a wake word.  Measured on the Echo Dot 2 with German commands: whispered 995-999, normal and soft
 * voice 0-18, also with 0.5 s before and 1 s of silence after the speech.
 * What it is fed is main.c's: the request from the wake word's end (its tail left out as for Home Assistant), so
 * nothing is skipped here.  The start given with the end of utterance changes nothing (the whole detector's audio
 * counts).  Cut-off CUTOFF, not the 922 the result names (the model's default; its per-locale ones need a locale,
 * which nothing here sets): an Echo Dot 3 (2026-11-13, 7 whispered and 8 normal German requests, "Alexa" whispered
 * too, replayed from its capture dump) scored 890-993 whispered from the word's end and 0-1 normal, so 922 missed one
 * whispered request and 500 (AHE's own cut-off) misses none.  Sounds without speech (breath, rustling) score high,
 * but only the span up to Home Assistant's VAD end is scored. */
#include "whisper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "pryon_api.h"

#define MODEL_SET "hassmic-whisper-ms"
#define CUTOFF    500                   /* AHE's own cut-off, not the result's 922 (above) */

static whisper_cb callback;
static int opened, created, active;     /* created: this request has a detector; active: it takes audio */
static unsigned seq;
static char id[32];
static uint64_t fed;
static pthread_t closer;                /* finishes the last request's detector: its result, then gone */
static int closing;

static const char *model(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_WHISPER");
    snprintf(p, sizeof p, "%s/pryon_whisper.manifest", d ? d : "/data/local/hassmic/whisper");
    return p;
}

static int num(const char *json, const char *key)
{
    char pat[40]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    return p ? atoi(p + strlen(pat)) : -1;
}

/* "metadata": one confidence per 10 ms frame; logged as the mean of every 0.25 s, to see where a request scored low */
static void frames(const char *json)
{
    const char *p = strstr(json, "\"frame_confidences\":[");
    char line[1024]; int len = 0, n = 0, sum = 0;
    if (!p) return;
    p = strchr(p, '[') + 1;
    while (*p && *p != ']' && len < (int)sizeof line - 8) {
        sum += atoi(p);
        if (++n % 25 == 0) { len += snprintf(line + len, sizeof line - len, " %d", sum / 25); sum = 0; }
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
    line[len] = 0;
    fprintf(stderr, "whisper: per 0.25 s:%s\n", line);
}

/* type "result": {"whisper_results":{"confidence":8,...,"threshold":922,...}}; "metadata" is logged, "metrics" not used */
static void on_event(const char *det, const char *utt, const char *type, const char *json, uint32_t z0, uint32_t z1)
{
    (void)det; (void)utt; (void)z0; (void)z1;
    if (type && !strcmp(type, "metadata") && json) frames(json);
    if (!type || strcmp(type, "result") || !json) return;
    int c = num(json, "confidence"), t = num(json, "threshold");
    if (c < 0) { fprintf(stderr, "whisper: no confidence in %s\n", json); return; }
    (void)t;                            /* the model's 922: see the top */
    callback(c > CUTOFF, c, CUTOFF);
}

static void on_log(int level, const char *g, const char *msg)
{
    (void)g;
    if (level <= 2) fprintf(stderr, "whisper: %s\n", msg ? msg : "");
}

int whisper_open(whisper_cb cb)
{
    if (opened) return 0;
    callback = cb;
    WhisperApi_setLogEventHandler(on_log);
    if (WhisperApi_setEventHandler(on_event) || WhisperApi_loadWhisperModelset(MODEL_SET, model())) return -1;
    opened = 1;
    fprintf(stderr, "whisper: model %s loaded\n", model());
    return 0;
}

/* A request's detector is finished in a thread of its own, as whisper_test does it: its backlog waited for, then the
 * session end, then deleted.  The result does not always come with the end of utterance alone: on an Echo Dot 3 it
 * came now and then only when the next request's detector retired this one, seconds later, after the reply
 * (2026-11-13), so Home Assistant read the request before's.  backlogWait(-1) blocks: never on the capture thread */
static void finish_now(const char *d)
{
    WhisperApi_backlogWait(d, -1);
    WhisperApi_pushSessionEnd(d);
    WhisperApi_deleteWhisperDetector(d);
}

static void *finish(void *arg) { finish_now(arg); free(arg); return NULL; }

static void retire(void)
{
    char *d = strdup(id);
    if (d && pthread_create(&closer, NULL, finish, d) == 0) { closing = 1; return; }
    free(d); finish_now(id);                /* no thread: here */
}

static void closed(void) { if (closing) { pthread_join(closer, NULL); closing = 0; } }   /* done long before the next request */

void whisper_begin(void)
{
    if (!opened) return;
    closed();
    snprintf(id, sizeof id, "hassmic-whisper-%u", ++seq);
    WhisperAudioFormat fmt = { 0, 16000, 16, 1 };
    if (WhisperApi_createWhisperDetector(id, MODEL_SET, "pryon", fmt)) { fprintf(stderr, "whisper: cannot create a detector\n"); return; }
    created = active = 1; fed = 0;
}

void whisper_feed(const int16_t *samples, size_t count)
{
    if (!active || !count) return;
    if (WhisperApi_pushAudioEvent(id, samples, (uint32_t)count, fed)) { active = 0; return; }
    fed += count;
}

void whisper_end(int score)
{
    if (!created) return;
    const char *why = !score ? "the run ended without an end of speech" : !active ? "the detector refused audio"
                    : fed < 160 ? "no audio" : NULL;
    created = active = 0;
    /* every request says what came of it: a result line (main.c), or why there is none */
    if (why) fprintf(stderr, "whisper: not scored (%s, %.2f s)\n", why, fed / 16000.0);
    else if (WhisperApi_pushEndOfUtterance(id, "Utterance-1", 0, fed / 160)) fprintf(stderr, "whisper: not scored (end of utterance refused)\n");
    else fprintf(stderr, "whisper: scoring %.2f s\n", fed / 16000.0);
    retire();
}
