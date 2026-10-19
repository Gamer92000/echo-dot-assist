/* Amazon's stock wake-word engine (libpryon.so) with the stock "Alexa" model, or a model set from Amazon.  One decoder
 * per slot (wake.h), each on a model set of its own: libpryon is a process-wide singleton that runs any number of
 * named decoders, each on a thread of its own, and hands every keyword result to one callback with the decoder's name
 * (sound_pryon.c runs the acoustic event decoder beside them the same way). */
#include "wake.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <zlib.h>
#include <stdio.h>
#include <string.h>
#include "pryon_api.h"

/* slot 0 keeps the names of the one decoder there was before */
static const char *const model_set[WAKE_SLOTS] = { "hassmic-ms", "hassmic-ms2" };
static const char *const decoder[WAKE_SLOTS] = { "hassmic-dec", "hassmic-dec2" };

static wake_cb callback;
static uint64_t sample_index[WAKE_SLOTS];
static int opened[WAKE_SLOTS];

/* Client properties the model's thresholds depend on.  Kept here so that they can be pushed again after a session end,
 * in case the engine forgets them with the audio.  Set from the playback, alarm and Sendspin threads: under props_lock. */
static PryonClientProperty props[8];
static unsigned nprops;
static pthread_mutex_t props_lock = PTHREAD_MUTEX_INITIALIZER;

static void push_props(int slot)
{
    PryonClientEvent ev[8];
    for (unsigned i = 0; i < nprops; i++) { ev[i].one = 1; ev[i].property = &props[i]; }
    if (nprops && PryonDecoder_PushClientEvents(decoder[slot], ev, nprops)) fprintf(stderr, "pryon: client properties refused\n");
}

/* Not logged: the two warnings about the audio fingerprint in the result's metadata ("Bitmask frame indices ..",
 * "Invalid bitmask frame indices (currentFrameIdx < wwEndFrameIdx) .."): its extractor is a few 10 ms frames behind the
 * keyword's end when the result is built.  158 of 235 accepted wake words on the Dot 3 came with one of them, each
 * accepted as usual; nothing here uses the fingerprint (Alexa's cloud did).  Nor "WindowAverageTracker request for
 * average is before existing history": the NTT fusion sets (every en-US one, alexa-de-DE) ask their window averages
 * for the second before the model was loaded, ~390 lines in the first second after each load (biscuit, 2026-10-08),
 * none after. */
static void on_log(int level, const char *tag, const char *msg)
{
    if (level > 3 || (msg && (strstr(msg, "itmask frame indices (currentFrameIdx < wwEndFrameIdx)")
                              || strstr(msg, "WindowAverageTracker request for average is before existing history")))) return;
    fprintf(stderr, "pryon %s: %s\n", tag ? tag : "", msg ? msg : "");
}

/* The front end writes its clock into the lowest bit of the micAsr samples (the first 23 samples of each 128-sample
 * frame carry a fixed pattern, data follows), the engine reads it back and reports where the keyword lies on that clock
 * in the result's metadata: a header starting "JSON_GZ_AND_FP", a gzip stream with JSON ("audioMetadataDuringDetection":
 * {.."timestamp_before_ww_end":9559,"timestamp_before_ww_start":8823}, "has_audio_lsb_metadata":1), a fingerprint.
 * Stock hands those two numbers back to the front end before it asks for the wake word's energies (main.c). */
static long afe_start[WAKE_SLOTS], afe_end[WAKE_SLOTS]; static atomic_int afe_known[WAKE_SLOTS];

static void read_metadata(int slot, const unsigned char *m, size_t n)
{
    static char jss[WAKE_SLOTS][1 << 16];          /* each decoder calls back on its own thread */
    char *js = jss[slot]; const size_t cap = sizeof jss[0];
    size_t at = 0; z_stream z; const char *a, *b;
    atomic_store(&afe_known[slot], 0);
    while (m && at + 3 < n && at < 64 && !(m[at] == 0x1f && m[at + 1] == 0x8b && m[at + 2] == 8)) at++;
    if (!m || at >= 64 || at + 3 >= n) return;
    memset(&z, 0, sizeof z);
    z.next_in = (Bytef *)(m + at); z.avail_in = n - at; z.next_out = (Bytef *)js; z.avail_out = cap - 1;
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return;
    inflate(&z, Z_FINISH); inflateEnd(&z);
    js[cap - 1 - z.avail_out] = 0;
    if (!strstr(js, "\"has_audio_lsb_metadata\":1") || !(a = strstr(js, "\"timestamp_before_ww_start\":"))
        || !(b = strstr(js, "\"timestamp_before_ww_end\":"))) return;
    afe_start[slot] = atol(strchr(a, ':') + 1); afe_end[slot] = atol(strchr(b, ':') + 1);
    atomic_store(&afe_known[slot], 1);
}

static int pryon_afe_times(int slot, long *start, long *end)
{
    if (!atomic_load(&afe_known[slot])) return 0;
    *start = afe_start[slot]; *end = afe_end[slot];
    return 1;
}

static void on_result(const char *decoderId, PryonEnumeratedResult *r)
{
    int slot = 0;
    while (slot < WAKE_SLOTS && (!decoderId || strcmp(decoderId, decoder[slot]))) slot++;
    if (slot == WAKE_SLOTS) return;
    fprintf(stderr, "wake: %s type=%d samples %llu-%llu (fed %llu%s)\n", r->keyword ? r->keyword : "?", r->detectionType,
            (unsigned long long)r->beginSampleIndex, (unsigned long long)r->endSampleIndex, (unsigned long long)sample_index[slot],
            slot ? ", second wake word" : "");
    if (r->detectionType == PRYON_DETECTION_TYPE_ACCEPT) read_metadata(slot, r->metadata, r->metadataSize);
    if (r->detectionType == PRYON_DETECTION_TYPE_ACCEPT && r->keyword) callback(slot, r->keyword, r->beginSampleIndex, r->endSampleIndex);
    else if (r->detectionType == PRYON_DETECTION_TYPE_NEAR_MISS && r->keyword) wake_near_miss(slot, r->keyword);
}

static int pryon_open(int slot, const char *manifest, wake_cb cb)
{
    PryonMultichannelAudioFormat fmt;
    callback = cb; sample_index[slot] = wake_fed();
    PryonApi_SetLoggingCallback(on_log);
    PryonApi_SetEnumeratedResultCallback(on_result);
    if (PryonModelSet_New(model_set[slot], manifest, "")) return -1;
    PryonDecoder_NewPryonMultichannelAudioFormat_Default(&fmt);
    if (PryonDecoder_NewSpotterAudioDecoder(decoder[slot], model_set[slot], "pryon", fmt, "{}")) { PryonModelSet_Delete(model_set[slot]); return -1; }
    pthread_mutex_lock(&props_lock);
    opened[slot] = 1;
    push_props(slot);
    pthread_mutex_unlock(&props_lock);
    return 0;
}

static void pryon_feed(int slot, const int16_t *samples, size_t count)
{
    PryonDecoder_PushAudioEventSamples(decoder[slot], sample_index[slot], samples, count);
    sample_index[slot] += count;
}

static void pryon_reset(int slot)
{
    PryonDecoder_SessionEnd(decoder[slot]);
    pthread_mutex_lock(&props_lock); push_props(slot); pthread_mutex_unlock(&props_lock);
}

static void pryon_property(const char *name, int value)
{
    unsigned i;
    pthread_mutex_lock(&props_lock);
    for (i = 0; i < nprops && strcmp(props[i].name, name); i++) ;
    if (i == nprops && nprops < sizeof props / sizeof *props) props[nprops++].name = name;
    if (i < nprops && props[i].value != value) {
        props[i].value = value;
        for (int s = 0; s < WAKE_SLOTS; s++) if (opened[s]) push_props(s);
    }
    pthread_mutex_unlock(&props_lock);
}

static void pryon_close(int slot)
{
    pthread_mutex_lock(&props_lock); opened[slot] = 0; pthread_mutex_unlock(&props_lock);     /* no hints to a decoder that is gone */
    atomic_store(&afe_known[slot], 0);
    PryonDecoder_Delete(decoder[slot]);
    PryonModelSet_Delete(model_set[slot]);
}

const char *wake_attributes(void) { return PryonApi_GetAttributes(); }

const struct wake_engine wake_vendor = { pryon_open, pryon_feed, pryon_reset, pryon_property, pryon_close, pryon_afe_times };
