/* Whisper detection on Amazon's libpryon.so (docs/re-whisper.md), called as stock's AHE does: one detector per request,
 * audio pushed with its running sample index, the end of utterance in 10 ms frames, one "result" event after it.
 * The model is DAVS's "whisper-static" (a DNN on 64 LFBE features behind a DNN speech detector), installed by
 * scripts/artifacts.sh into its own directory: it carries a stray pryon.manifest that the wake word scan of models/
 * would take for a wake word.  The result names its own threshold (default 922; the model's per-locale ones are only
 * applied when a locale is set, which nothing here can).  Measured on the Echo Dot 2 with German commands: whispered
 * 995-999, normal and soft voice 0-18, also with 0.5 s before and 1 s of silence after the speech.
 * The stream starts when the wake word is recognised, before its last sound has died away: spoken normally, its tail
 * takes a whispered request down to 895-979 (live, 2026-10-04).  The start given with the end of utterance changes
 * nothing (the whole detector's audio counts), so the first SKIP of each request is not fed at all: then 996-998. */
#include "whisper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pryon_api.h"

#define MODEL_SET "hassmic-whisper-ms"
#define SKIP      8000                  /* samples: 0.5 s after the wake word */

static whisper_cb callback;
static int opened, active, pending;     /* active: a detector takes audio; pending: the last one may still answer */
static unsigned seq;
static char id[32];
static uint64_t fed, skipped;

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
    if (t <= 0) t = 500;                /* AHE's own cut-off */
    callback(c > t, c, t);
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

/* the detector of the request before goes now: deleting it right after its end of utterance could lose the result */
static void retire(void)
{
    if (!pending) return;
    WhisperApi_backlogWait(id, -1);
    WhisperApi_pushSessionEnd(id);
    WhisperApi_deleteWhisperDetector(id);
    pending = 0;
}

void whisper_begin(void)
{
    if (!opened) return;
    retire();
    snprintf(id, sizeof id, "hassmic-whisper-%u", ++seq);
    WhisperAudioFormat fmt = { 0, 16000, 16, 1 };
    if (WhisperApi_createWhisperDetector(id, MODEL_SET, "pryon", fmt)) { fprintf(stderr, "whisper: cannot create a detector\n"); return; }
    active = pending = 1; fed = skipped = 0;
}

void whisper_feed(const int16_t *samples, size_t count)
{
    if (!active || !count) return;
    if (skipped < SKIP) {
        size_t k = SKIP - skipped < count ? SKIP - skipped : count;
        skipped += k; samples += k; count -= k;
        if (!count) return;
    }
    if (WhisperApi_pushAudioEvent(id, samples, (uint32_t)count, fed)) { active = 0; return; }
    fed += count;
}

void whisper_end(int score)
{
    if (!active) return;
    active = 0;
    if (score && fed >= 160 && WhisperApi_pushEndOfUtterance(id, "Utterance-1", 0, fed / 160))
        fprintf(stderr, "whisper: end of utterance refused\n");
    else if (score) fprintf(stderr, "whisper: scoring %.2f s\n", fed / 16000.0);
    if (!score) retire();
}
