/*
 * Recovered subset of Amazon's libpryon.so C API (Fire OS NS65741, donut_puffin, armv7 Thumb).
 * Source: static analysis, see docs/re-pryon.md for evidence addresses.
 * The call sequence, PryonEnumeratedResult fields used below and detectionType values were then
 * confirmed by running build/pryon_test under qemu-arm (tools/qrun.sh) on 2026-09-21.
 * Anything still tagged UNVERIFIED is inferred and has not been exercised.
 *
 * The API is a process-wide singleton. Model sets and decoders are addressed by
 * caller-chosen string ids, not by handles. All functions return 0 on success,
 * non-zero on failure (C++ exceptions are caught inside the wrappers and turned into 1).
 */
#ifndef PRYON_API_H
#define PRYON_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRYON_MAX_CHANNELS 32

/* Values seen in PryonDecoder_NewPryonMultichannelAudioFormat_Default.
 * Names are UNVERIFIED; 0 is what the single input channel uses, 5 fills unused slots.
 * nemort_scorer_config.json maps the model input to channel_type "beamformed". */
enum {
    PRYON_CHANNEL_TYPE_PRIMARY = 0, /* UNVERIFIED name */
    PRYON_CHANNEL_TYPE_UNUSED  = 5  /* UNVERIFIED name */
};

/* 0x90 bytes, passed BY VALUE to the decoder constructors.
 * Default = { 0, 16000, 16, 1, { 0, 5, 5, ... 5 } }. */
typedef struct PryonMultichannelAudioFormat {
    int32_t encoding;        /* default 0. UNVERIFIED meaning (PCM?) */
    int32_t sampleRate;      /* default 16000 */
    int32_t bitsPerSample;   /* default 16 */
    int32_t numChannels;     /* default 1 */
    int32_t channelTypes[PRYON_MAX_CHANNELS];
} PryonMultichannelAudioFormat;

/* Partial layout, 32-bit ARM. Only the named fields are confirmed by two independent
 * readers (libWakeWordManager, libAmazonKWD). Do not allocate or copy this struct;
 * read through the pointer inside the callback only. */
typedef struct _PryonEnumeratedResult {
    uint8_t     _unknown00[0x08];
    uint64_t    beginSampleIndex;   /* +0x08 keyword start, in pushed-sample index space (confirmed) */
    uint8_t     _unknown10[0x0c];
    const char *keyword;            /* +0x1c e.g. "ALEXA", "STOP" */
    uint8_t     _unknown20[0x04];
    int32_t     detectionType;      /* +0x24: 0 = NearMiss, 2 = Accept (confirmed against library log). 1 and 3 not observed */
    uint32_t    metadataSize;       /* +0x28 */
    const void *metadata;           /* +0x2c opaque blob Alexa uploads with the utterance; not needed */
    uint8_t     _unknown30[0x10];
    uint64_t    endSampleIndex;     /* +0x40 */
} PryonEnumeratedResult;

#define PRYON_DETECTION_TYPE_NEAR_MISS 0
#define PRYON_DETECTION_TYPE_ACCEPT    2 /* libAmazonKWD also acts on 3 (flagged variant, not observed) */

typedef void (*PryonLoggingCallback)(int level, const char *tag, const char *message); /* arg names UNVERIFIED */
typedef void (*PryonEnumeratedResultCallback)(const char *decoderId, PryonEnumeratedResult *result);

/* Returns a JSON string owned by the library. */
const char *PryonApi_GetAttributes(void);

int PryonApi_SetLoggingCallback(PryonLoggingCallback cb);
int PryonApi_SetEnumeratedResultCallback(PryonEnumeratedResultCallback cb);

/* manifestPath is the pryon.manifest FILE, not the directory.
 * situationList must be non-NULL; stock callers pass "". */
int PryonModelSet_New(const char *modelSetId, const char *manifestPath, const char *situationList);
int PryonModelSet_Delete(const char *modelSetId);

void PryonDecoder_NewPryonMultichannelAudioFormat_Default(PryonMultichannelAudioFormat *out);

/* modelId is the key under "models" in the manifest: "pryon" for the stock keyword model.
 * configJson: stock caller passes "{}". Spawns the decoder thread. */
int PryonDecoder_NewSpotterAudioDecoder(const char *decoderId,
                                        const char *modelSetId,
                                        const char *modelId,
                                        PryonMultichannelAudioFormat format,
                                        const char *configJson);

/* sampleIndex: running index of samples[0] since stream start. sampleCount is in samples, not bytes.
 * Asynchronous: samples are queued for the decoder thread; the result callback fires on that thread. */
int PryonDecoder_PushAudioEventSamples(const char *decoderId,
                                       uint64_t sampleIndex,
                                       const int16_t *samples,
                                       uint32_t sampleCount);

/* Marks a discontinuity in the audio (stock: wwm::PryonDecoder::endContiguousAudio). */
int PryonDecoder_SessionEnd(const char *decoderId);

/* Stock caller passes -1. Observed to return before the queue is drained, so do not rely on it:
 * push at real-time rate instead. The decoder drops audio once its backlog passes ~7 s. */
int PryonDecoder_BacklogWait(const char *decoderId, int32_t timeout);

int PryonDecoder_Delete(const char *decoderId);

#ifdef __cplusplus
}
#endif

#endif /* PRYON_API_H */
