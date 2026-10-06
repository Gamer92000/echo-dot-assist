/* Amazon's stock wake-word engine (libpryon.so) with the stock "Alexa" model. */
#include "wake.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <zlib.h>
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

/* Not logged: the two warnings about the audio fingerprint in the result's metadata ("Bitmask frame indices ..",
 * "Invalid bitmask frame indices (currentFrameIdx < wwEndFrameIdx) .."): its extractor is a few 10 ms frames behind the
 * keyword's end when the result is built.  158 of 235 accepted wake words on the Dot 3 came with one of them, each
 * accepted as usual; nothing here uses the fingerprint (Alexa's cloud did). */
static void on_log(int level, const char *tag, const char *msg)
{
    if (level > 3 || (msg && strstr(msg, "itmask frame indices (currentFrameIdx < wwEndFrameIdx)"))) return;
    fprintf(stderr, "pryon %s: %s\n", tag ? tag : "", msg ? msg : "");
}

/* The front end writes its clock into the lowest bit of the micAsr samples (the first 23 samples of each 128-sample
 * frame carry a fixed pattern, data follows), the engine reads it back and reports where the keyword lies on that clock
 * in the result's metadata: a header starting "JSON_GZ_AND_FP", a gzip stream with JSON ("audioMetadataDuringDetection":
 * {.."timestamp_before_ww_end":9559,"timestamp_before_ww_start":8823}, "has_audio_lsb_metadata":1), a fingerprint.
 * Stock hands those two numbers back to the front end before it asks for the wake word's energies (main.c). */
static long afe_start, afe_end; static atomic_int afe_known;

static void read_metadata(const unsigned char *m, size_t n)
{
    static char js[1 << 16];
    size_t at = 0; z_stream z; const char *a, *b;
    atomic_store(&afe_known, 0);
    while (m && at + 3 < n && at < 64 && !(m[at] == 0x1f && m[at + 1] == 0x8b && m[at + 2] == 8)) at++;
    if (!m || at >= 64 || at + 3 >= n) return;
    memset(&z, 0, sizeof z);
    z.next_in = (Bytef *)(m + at); z.avail_in = n - at; z.next_out = (Bytef *)js; z.avail_out = sizeof js - 1;
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return;
    inflate(&z, Z_FINISH); inflateEnd(&z);
    js[sizeof js - 1 - z.avail_out] = 0;
    if (!strstr(js, "\"has_audio_lsb_metadata\":1") || !(a = strstr(js, "\"timestamp_before_ww_start\":"))
        || !(b = strstr(js, "\"timestamp_before_ww_end\":"))) return;
    afe_start = atol(strchr(a, ':') + 1); afe_end = atol(strchr(b, ':') + 1);
    atomic_store(&afe_known, 1);
}

int wake_afe_times(long *start, long *end)
{
    if (!atomic_load(&afe_known)) return 0;
    *start = afe_start; *end = afe_end;
    return 1;
}

static void on_result(const char *decoderId, PryonEnumeratedResult *r)
{
    (void)decoderId;
    fprintf(stderr, "wake: %s type=%d samples %llu-%llu (fed %llu)\n", r->keyword ? r->keyword : "?", r->detectionType,
            (unsigned long long)r->beginSampleIndex, (unsigned long long)r->endSampleIndex, (unsigned long long)sample_index);
    if (r->detectionType == PRYON_DETECTION_TYPE_ACCEPT) read_metadata(r->metadata, r->metadataSize);
    if (r->detectionType == PRYON_DETECTION_TYPE_ACCEPT && r->keyword) callback(r->keyword, r->beginSampleIndex, r->endSampleIndex);
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

const char *wake_attributes(void) { return PryonApi_GetAttributes(); }
