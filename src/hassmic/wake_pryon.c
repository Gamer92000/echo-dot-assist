/* Amazon's stock wake-word engine (libpryon.so) with the stock "Alexa" model. */
#include "wake.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "pryon_api.h"

#define MODEL_SET "hassmic-ms"
#define DECODER   "hassmic-dec"

static wake_cb callback;
static uint64_t sample_index;
static int opened;

/* Client properties the model's thresholds depend on.  Kept here so that they can be pushed again after a session end,
 * in case the engine forgets them with the audio.  Set from the playback, alarm and Sendspin threads: under props_lock. */
static PryonClientProperty props[8];
static unsigned nprops;
static pthread_mutex_t props_lock = PTHREAD_MUTEX_INITIALIZER;

static void push_props(void)
{
    PryonClientEvent ev[8];
    for (unsigned i = 0; i < nprops; i++) { ev[i].one = 1; ev[i].property = &props[i]; }
    if (nprops && PryonDecoder_PushClientEvents(DECODER, ev, nprops)) fprintf(stderr, "pryon: client properties refused\n");
}

static void on_log(int level, const char *tag, const char *msg)
{
    if (level <= 3) fprintf(stderr, "pryon %s: %s\n", tag ? tag : "", msg ? msg : "");
}

static void on_result(const char *decoderId, PryonEnumeratedResult *r)
{
    (void)decoderId;
    fprintf(stderr, "wake: %s type=%d\n", r->keyword ? r->keyword : "?", r->detectionType);
    if (r->detectionType == PRYON_DETECTION_TYPE_ACCEPT && r->keyword) callback(r->keyword);
}

int wake_open(const char *manifest, wake_cb cb)
{
    PryonMultichannelAudioFormat fmt;
    callback = cb;
    PryonApi_SetLoggingCallback(on_log);
    PryonApi_SetEnumeratedResultCallback(on_result);
    if (PryonModelSet_New(MODEL_SET, manifest, "")) return -1;
    PryonDecoder_NewPryonMultichannelAudioFormat_Default(&fmt);
    if (PryonDecoder_NewSpotterAudioDecoder(DECODER, MODEL_SET, "pryon", fmt, "{}")) return -1;
    pthread_mutex_lock(&props_lock);
    opened = 1;
    push_props();
    pthread_mutex_unlock(&props_lock);
    return 0;
}

void wake_feed(const int16_t *samples, size_t count)
{
    PryonDecoder_PushAudioEventSamples(DECODER, sample_index, samples, count);
    sample_index += count;
}

void wake_reset(void)
{
    PryonDecoder_SessionEnd(DECODER);
    pthread_mutex_lock(&props_lock); push_props(); pthread_mutex_unlock(&props_lock);
}

void wake_property(const char *name, int value)
{
    unsigned i;
    pthread_mutex_lock(&props_lock);
    for (i = 0; i < nprops && strcmp(props[i].name, name); i++) ;
    if (i == nprops && nprops < sizeof props / sizeof *props) props[nprops++].name = name;
    if (i < nprops && props[i].value != value) { props[i].value = value; if (opened) push_props(); }
    pthread_mutex_unlock(&props_lock);
}

void wake_close(void)
{
    PryonDecoder_Delete(DECODER);
    PryonModelSet_Delete(MODEL_SET);
}
