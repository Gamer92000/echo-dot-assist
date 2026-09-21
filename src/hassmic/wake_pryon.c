/* Amazon's stock wake-word engine (libpryon.so) with the stock "Alexa" model. */
#include "wake.h"
#include <stdio.h>
#include "pryon_api.h"

#define MODEL_SET "hassmic-ms"
#define DECODER   "hassmic-dec"

static wake_cb callback;
static uint64_t sample_index;

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
    return 0;
}

void wake_feed(const int16_t *samples, size_t count)
{
    PryonDecoder_PushAudioEventSamples(DECODER, sample_index, samples, count);
    sample_index += count;
}

void wake_reset(void) { PryonDecoder_SessionEnd(DECODER); }

void wake_close(void)
{
    PryonDecoder_Delete(DECODER);
    PryonModelSet_Delete(MODEL_SET);
}
