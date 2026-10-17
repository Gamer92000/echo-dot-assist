/* The stock acoustic event detector (docs/re-aed.md) on Amazon's libpryon.so, beside the wake word decoder.
 * The model is the firmware's (/system/local/models/AED, the same file on donut, biscuit and radar), or the newer one
 * Amazon serves, where scripts/artifacts.sh installed it (/data/local/hassmic/aed): retrained weights and smoke/CO
 * thresholds 0.845 instead of 0.825, the same types; it scored the same on every test clip.  An installed one that
 * does not load leaves the firmware's.  Nothing is detected until the client properties PuffinApp pushes
 * are set: "AcousticEventDetectionEnabled" and "aed_<type>_enabled" per type.  The decoder scores ~10 s windows
 * (scorer.batch_scorer_reset_interval_msec 9980) and reports each one, detected or not, as JSON. */
#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pryon_api.h"

#define MODEL     "/system/local/models/AED/pryon.manifest"
#define MODEL_SET "hassmic-aed-ms"
#define DECODER   "hassmic-aed"
#define MAX_TYPES 16

static sound_cb callback;
static const char *types[MAX_TYPES];
static int ntypes, opened, newer_failed;       /* newer_failed: the installed one did not load once; it will not now */
static uint64_t sample_index;

static const char *installed(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_AED");
    snprintf(p, sizeof p, "%s/pryon.manifest", d ? d : "/data/local/hassmic/aed");
    return p;
}

/* "detected":true inside a type's object.  An object ends where the next type's begins: smokeSiren carries a JSON
 * string with braces in it ("detectionDescriptorProfile"), so the closing brace cannot be searched for. */
static int detected(const char *json, const char *type)
{
    char pat[48]; snprintf(pat, sizeof pat, "\"%s\":{", type);
    const char *o = strstr(json, pat), *end, *d;
    if (!o) return 0;
    o += strlen(pat);
    end = strstr(o, ":{\"aed_type\"");
    d = strstr(o, "\"detected\":true");
    return d && (!end || d < end);
}

static void on_result(const char *decoderId, PryonAcousticEventResult *r)
{
    const char *hit[MAX_TYPES]; int n = 0;
    (void)decoderId;
    if (r->resultType != PRYON_AED_RESULT_DETECTED || !r->json) return;
    for (int i = 0; i < ntypes; i++) if (detected(r->json, types[i])) hit[n++] = types[i];
    fprintf(stderr, "sound: window ending at %.1f s:", r->sampleIndex / 16000.0);
    for (int i = 0; i < n; i++) fprintf(stderr, " %s", hit[i]);
    fprintf(stderr, "\n");
    if (n) callback(hit, n);
}

int sound_open(const char *const *t, int n, sound_cb cb)
{
    PryonMultichannelAudioFormat fmt;
    PryonClientProperty props[MAX_TYPES + 1]; PryonClientEvent ev[MAX_TYPES + 1];
    static char names[MAX_TYPES][48];
    if (opened) return 0;
    callback = cb; ntypes = n > MAX_TYPES ? MAX_TYPES : n;
    for (int i = 0; i < ntypes; i++) types[i] = t[i];
    PryonApi_SetAcousticEventDetectionResultCallback(on_result);
    const char *m = installed();
    if (access(m, R_OK) || PryonModelSet_New(MODEL_SET, m, "")) {
        if (!access(m, R_OK)) { fprintf(stderr, "sound: cannot load %s, taking the firmware's\n", m); newer_failed = 1; }
        m = MODEL;
        if (PryonModelSet_New(MODEL_SET, m, "")) { fprintf(stderr, "sound: cannot load %s\n", m); return -1; }
    }
    PryonDecoder_NewPryonMultichannelAudioFormat_Default(&fmt);
    if (PryonDecoder_NewSpotterAudioDecoder(DECODER, MODEL_SET, "pryon", fmt, "{}")) { PryonModelSet_Delete(MODEL_SET); return -1; }
    props[0].name = "AcousticEventDetectionEnabled"; props[0].value = 1;
    for (int i = 0; i < ntypes; i++) {
        snprintf(names[i], sizeof names[i], "aed_%s_enabled", types[i]);
        props[i + 1].name = names[i]; props[i + 1].value = 1;
    }
    for (int i = 0; i <= ntypes; i++) { ev[i].one = 1; ev[i].property = &props[i]; }
    if (PryonDecoder_PushClientEvents(DECODER, ev, ntypes + 1)) {
        fprintf(stderr, "sound: client properties refused\n");
        PryonDecoder_Delete(DECODER); PryonModelSet_Delete(MODEL_SET); return -1;
    }
    sample_index = 0; opened = 1;
    fprintf(stderr, "sound detection: on (%s)\n", m);
    return 0;
}

enum sound_model sound_model(void)
{
    if (!newer_failed && !access(installed(), R_OK)) return SOUND_NEWER;
    return access(MODEL, R_OK) ? SOUND_NONE : SOUND_FIRMWARE;
}

void sound_feed(const int16_t *samples, size_t count)
{
    if (!opened) return;
    PryonDecoder_PushAudioEventSamples(DECODER, sample_index, samples, count);
    sample_index += count;
}

void sound_close(void)
{
    if (!opened) return;
    PryonDecoder_Delete(DECODER);
    PryonModelSet_Delete(MODEL_SET);
    opened = 0;
    fprintf(stderr, "sound detection: off\n");
}
