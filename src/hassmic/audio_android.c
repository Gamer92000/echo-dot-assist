/* Device backend for Echos without Amazon's mixer daemon (checkers, Echo Show 5): Android's own audio through OpenSL ES.
 * There Amazon's front end runs inside the audio HAL (audio.primary.mt8163.so links libasp.so, docs/re-checkers.md):
 * an AudioRecord with source VOICE_RECOGNITION (6) or HOTWORD (1999) gets the ASR pipeline's output (16 kHz mono s16,
 * AEC + beams, the micAsr of donut), and every AudioTrack goes to the one "primary output" through the HAL's Speaker
 * pipeline, whose signal is the ASR pipeline's echo reference.  So each of our streams is a player of its own and
 * AudioFlinger mixes them.  OpenSL ES is the NDK's stable C API over AudioRecord/AudioTrack (libwilhelm): no C++ ABI of
 * the firmware's libmedia to match.
 *
 * Not tried on a real Echo Show yet.  What has to hold there, in order:
 *  - recording rights: recordingAllowed() (libserviceutility.so) wants RECORD_AUDIO, then lets uid 0 through and, as
 *    OpenSL ES names no package, looks up one for any other uid to note the app op against: hassmic runs as system
 *    (1000, package "android"; device.conf DAEMON_USER);
 *  - one capture at a time: Android 7 lets a newer AudioRecord take the input from a HOTWORD one, so amazon.speech.sim
 *    (which records 1999 and 2999 itself) must be disabled first;
 *  - the preset: VOICE_RECOGNITION is the closest OpenSL ES has to stock's HOTWORD source, both pick the ASR pipeline
 *    (getAspPipelineType in the HAL); HOTWORD itself is not reachable from OpenSL ES. */
#include "audio.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static SLObjectItf engine_obj, mix_obj;
static SLEngineItf engine;
static pthread_once_t engine_once = PTHREAD_ONCE_INIT;

static void engine_init(void)
{
    if (slCreateEngine(&engine_obj, 0, NULL, 0, NULL, NULL) != SL_RESULT_SUCCESS ||
        (*engine_obj)->Realize(engine_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
        (*engine_obj)->GetInterface(engine_obj, SL_IID_ENGINE, &engine) != SL_RESULT_SUCCESS) {
        fprintf(stderr, "audio: OpenSL ES engine failed\n");
        engine = NULL;
        return;
    }
    if ((*engine)->CreateOutputMix(engine, &mix_obj, 0, NULL, NULL) != SL_RESULT_SUCCESS ||
        (*mix_obj)->Realize(mix_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
        fprintf(stderr, "audio: OpenSL ES output mix failed\n");
        engine = NULL;
    }
}

static int engine_ok(void) { pthread_once(&engine_once, engine_init); return engine != NULL; }

static void deadline(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000; ts->tv_nsec += (ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}

/* --- capture: a ring of CAP_NBUF blocks, all queued at start.  The buffer queue fills them in the order they were
 * queued, and each is queued again once cap_read's caller is done with it, so block k is always buf[k % CAP_NBUF]. */
#define CAP_BLOCK 320                   /* samples: 20 ms */
#define CAP_NBUF  8
static SLObjectItf rec_obj;
static SLRecordItf rec;
static SLAndroidSimpleBufferQueueItf rec_q;
static short cap_buf[CAP_NBUF][CAP_BLOCK];
static pthread_mutex_t cap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cap_cond = PTHREAD_COND_INITIALIZER;
static unsigned cap_filled, cap_taken;
static int cap_held;

static void cap_cb(SLAndroidSimpleBufferQueueItf q, void *ctx)
{
    (void)q; (void)ctx;
    pthread_mutex_lock(&cap_lock);
    cap_filled++;
    pthread_cond_signal(&cap_cond);
    pthread_mutex_unlock(&cap_lock);
}

int cap_open(void)
{
    if (!engine_ok()) return -1;
    SLDataLocator_IODevice dev = { SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT, SL_DEFAULTDEVICEID_AUDIOINPUT, NULL };
    SLDataSource src = { &dev, NULL };
    SLDataLocator_AndroidSimpleBufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, CAP_NBUF };
    SLDataFormat_PCM fmt = { SL_DATAFORMAT_PCM, 1, CAP_RATE * 1000, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                             SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN };
    SLDataSink sink = { &loc, &fmt };
    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
    const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
    if ((*engine)->CreateAudioRecorder(engine, &rec_obj, &src, &sink, 2, ids, req) != SL_RESULT_SUCCESS) {
        fprintf(stderr, "audio: CreateAudioRecorder failed\n");
        return -1;
    }
    SLAndroidConfigurationItf cfg;
    SLuint32 preset = SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION;
    if ((*rec_obj)->GetInterface(rec_obj, SL_IID_ANDROIDCONFIGURATION, &cfg) == SL_RESULT_SUCCESS)
        (*cfg)->SetConfiguration(cfg, SL_ANDROID_KEY_RECORDING_PRESET, &preset, sizeof preset);
    /* Realize is where AudioFlinger decides about the recording rights (see the top) */
    if ((*rec_obj)->Realize(rec_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
        (*rec_obj)->GetInterface(rec_obj, SL_IID_RECORD, &rec) != SL_RESULT_SUCCESS ||
        (*rec_obj)->GetInterface(rec_obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &rec_q) != SL_RESULT_SUCCESS ||
        (*rec_q)->RegisterCallback(rec_q, cap_cb, NULL) != SL_RESULT_SUCCESS) {
        fprintf(stderr, "audio: recorder refused (RECORD_AUDIO for this uid? another capture running?)\n");
        (*rec_obj)->Destroy(rec_obj); rec_obj = NULL;
        return -1;
    }
    cap_filled = cap_taken = 0; cap_held = 0;
    for (int i = 0; i < CAP_NBUF; i++) (*rec_q)->Enqueue(rec_q, cap_buf[i], sizeof cap_buf[i]);
    if ((*rec)->SetRecordState(rec, SL_RECORDSTATE_RECORDING) != SL_RESULT_SUCCESS) {
        fprintf(stderr, "audio: recording does not start\n");
        cap_close();
        return -1;
    }
    fprintf(stderr, "audio: OpenSL ES capture, VOICE_RECOGNITION, %u Hz mono\n", CAP_RATE);
    return 0;
}

/* A capture that stops delivering (the input taken by another AudioRecord, audioserver restarted) is reopened after
 * three empty waits, as audio_mixer.c does with a stalled micAsr. */
#define STALL_READS 3

int cap_read(const void **data)
{
    static int empty;
    if (!rec_obj) { if (cap_open() < 0) { sleep(1); return 0; } }
    pthread_mutex_lock(&cap_lock);
    if (cap_held) {
        /* the block the caller had goes back to the recorder (were all of them full, it dropped audio meanwhile and
         * goes on with this one) */
        (*rec_q)->Enqueue(rec_q, cap_buf[cap_taken % CAP_NBUF], sizeof cap_buf[0]);
        cap_taken++; cap_held = 0;
    }
    struct timespec ts; deadline(&ts, 1500);
    int rc = 0;
    while (cap_filled == cap_taken && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&cap_cond, &cap_lock, &ts);
    if (cap_filled == cap_taken) {
        pthread_mutex_unlock(&cap_lock);
        if (++empty >= STALL_READS) {
            fprintf(stderr, "audio: capture delivers nothing, reopening\n");
            cap_close(); empty = 0;
        }
        return 0;
    }
    empty = 0;
    *data = cap_buf[cap_taken % CAP_NBUF]; cap_held = 1;
    pthread_mutex_unlock(&cap_lock);
    return sizeof cap_buf[0];
}

void cap_close(void)
{
    if (!rec_obj) return;
    (*rec)->SetRecordState(rec, SL_RECORDSTATE_STOPPED);
    (*rec_obj)->Destroy(rec_obj);                   /* waits for a callback that is running */
    rec_obj = NULL; rec = NULL; rec_q = NULL; cap_held = 0;
}

/* --- playback: one OpenSL ES player per stream.  Our copy of what is queued lives in PLAY_NBUF slots of 20 ms; a
 * write blocks while all of them are with the player, which keeps it at playback speed.  What is queued but not
 * played yet is the frames written minus the player's position (AudioTrack's, so the track's own buffer counts). */
#define PLAY_NBUF 8
struct pstream {
    SLObjectItf obj; SLPlayItf play; SLAndroidSimpleBufferQueueItf q;
    SLVolumeItf vol;                            /* the volume as a gain on this player (vol_write), NULL if refused */
    enum vol kind;
    pthread_mutex_t lock; pthread_cond_t cond;
    unsigned bps, slot, busy, next;             /* bytes per second, slot size, slots with the player, next free slot */
    long long written;                          /* bytes since open */
    char *buf;
};
static struct pstream voice = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .kind = VOL_TTS },
                      music = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER },
                      earcon = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER },
                      voip = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

/* --- volume: a gain on each player, as the Dots' mixer applies its own per stream type.  Android's stream volumes are
 * set to their maximum once per start (main.sh: wifictl.dex volume-max), so this is all that takes level off.  The
 * curve: 0 silent, then 0.4 dB per step down from 100 (50 = -20 dB, 1 = -39.6 dB; how it sounds on the Show's
 * speaker is for the device to tell).  Kept in state/volume ("main tts"): no mixer here to remember it across a restart. */
static pthread_mutex_t vol_lock = PTHREAD_MUTEX_INITIALIZER;
static int vols[2] = { -1, -1 }, vols_loaded;

static const char *vol_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/volume", d ? d : "/data/local/hassmic/state");
    return p;
}

static void vol_load(void)                      /* under vol_lock */
{
    if (vols_loaded) return;
    vols_loaded = 1;
    FILE *f = fopen(vol_path(), "r"); int m, t;
    if (f && fscanf(f, "%d %d", &m, &t) == 2 && m >= 0 && m <= 100 && t >= 0 && t <= 100) { vols[VOL_MAIN] = m; vols[VOL_TTS] = t; }
    if (f) fclose(f);
}

static SLmillibel vol_mb(int pct) { return pct <= 0 ? SL_MILLIBEL_MIN : (SLmillibel)((pct - 100) * 40); }

static void vol_apply(struct pstream *s)
{
    pthread_mutex_lock(&vol_lock);
    vol_load();
    if (s->obj && !s->vol && (*s->obj)->GetInterface(s->obj, SL_IID_VOLUME, &s->vol) != SL_RESULT_SUCCESS) s->vol = NULL;
    int v = vols[s->kind];
    if (s->vol && v >= 0) (*s->vol)->SetVolumeLevel(s->vol, vol_mb(v));
    pthread_mutex_unlock(&vol_lock);
}

int vol_read(enum vol which, int fallback)
{
    if (which == VOL_MUTE) return 0;            /* no global mute of ours here */
    pthread_mutex_lock(&vol_lock);
    vol_load();
    int v = vols[which];
    pthread_mutex_unlock(&vol_lock);
    return v < 0 ? fallback : v;
}

void vol_write(enum vol which, int v)
{
    if (which == VOL_MUTE) return;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    pthread_mutex_lock(&vol_lock);
    vol_load();
    int changed = vols[which] != v;
    vols[which] = v;
    if (changed) {
        char tmp[270]; FILE *f;
        snprintf(tmp, sizeof tmp, "%s.new", vol_path());
        if ((f = fopen(tmp, "w"))) {
            fprintf(f, "%d %d\n", vols[VOL_MAIN] < 0 ? v : vols[VOL_MAIN], vols[VOL_TTS] < 0 ? v : vols[VOL_TTS]);
            if (fclose(f) == 0) rename(tmp, vol_path()); else unlink(tmp);
        }
    }
    pthread_mutex_unlock(&vol_lock);
    struct pstream *all[] = { &voice, &music, &earcon, &voip };
    for (int i = 0; i < 4; i++) if (all[i]->kind == which) vol_apply(all[i]);
}

static void play_cb(SLAndroidSimpleBufferQueueItf q, void *ctx)
{
    struct pstream *s = ctx; (void)q;
    pthread_mutex_lock(&s->lock);
    if (s->busy) s->busy--;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static void s_close(struct pstream *s, int drain);
static void vol_apply(struct pstream *s);

static int s_open(struct pstream *s, unsigned rate, unsigned channels, SLint32 stream_type)
{
    if (!engine_ok() || !rate || channels < 1 || channels > 2) return -1;
    if (s->obj) s_close(s, 0);
    SLDataLocator_AndroidSimpleBufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, PLAY_NBUF };
    SLDataFormat_PCM fmt = { SL_DATAFORMAT_PCM, channels, rate * 1000, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                             channels == 2 ? SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT : SL_SPEAKER_FRONT_CENTER,
                             SL_BYTEORDER_LITTLEENDIAN };
    SLDataSource src = { &loc, &fmt };
    SLDataLocator_OutputMix out = { SL_DATALOCATOR_OUTPUTMIX, mix_obj };
    SLDataSink sink = { &out, NULL };
    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION, SL_IID_VOLUME };
    const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE };
    if ((*engine)->CreateAudioPlayer(engine, &s->obj, &src, &sink, 3, ids, req) != SL_RESULT_SUCCESS) {
        fprintf(stderr, "audio: CreateAudioPlayer failed (%u Hz, %u ch)\n", rate, channels);
        s->obj = NULL;
        return -1;
    }
    SLAndroidConfigurationItf cfg;
    if ((*s->obj)->GetInterface(s->obj, SL_IID_ANDROIDCONFIGURATION, &cfg) == SL_RESULT_SUCCESS)
        (*cfg)->SetConfiguration(cfg, SL_ANDROID_KEY_STREAM_TYPE, &stream_type, sizeof stream_type);
    if ((*s->obj)->Realize(s->obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
        (*s->obj)->GetInterface(s->obj, SL_IID_PLAY, &s->play) != SL_RESULT_SUCCESS ||
        (*s->obj)->GetInterface(s->obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &s->q) != SL_RESULT_SUCCESS ||
        (*s->q)->RegisterCallback(s->q, play_cb, s) != SL_RESULT_SUCCESS) {
        fprintf(stderr, "audio: player refused\n");
        (*s->obj)->Destroy(s->obj); s->obj = NULL;
        return -1;
    }
    s->bps = rate * channels * 2;
    s->slot = s->bps / 50 & ~3u;
    s->buf = malloc((size_t)s->slot * PLAY_NBUF);
    if (!s->buf) { (*s->obj)->Destroy(s->obj); s->obj = NULL; return -1; }
    s->busy = s->next = 0; s->written = 0;
    vol_apply(s);                               /* before the first sample: no burst at full scale */
    (*s->play)->SetPlayState(s->play, SL_PLAYSTATE_PLAYING);
    return 0;
}

static int s_write(struct pstream *s, const void *data, size_t len)
{
    const char *p = data;
    if (!s->obj) return -1;
    while (len) {
        size_t n = len < s->slot ? len : s->slot;
        pthread_mutex_lock(&s->lock);
        while (s->busy == PLAY_NBUF) {
            struct timespec ts; deadline(&ts, 2000);
            if (pthread_cond_timedwait(&s->cond, &s->lock, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&s->lock);
                fprintf(stderr, "audio: player does not consume\n");
                return -1;
            }
        }
        char *slot = s->buf + (size_t)s->next * s->slot;
        memcpy(slot, p, n);
        if ((*s->q)->Enqueue(s->q, slot, n) != SL_RESULT_SUCCESS) { pthread_mutex_unlock(&s->lock); return -1; }
        s->busy++; s->next = (s->next + 1) % PLAY_NBUF; s->written += n;
        pthread_mutex_unlock(&s->lock);
        p += n; len -= n;
    }
    return 0;
}

static long long s_queued_us(struct pstream *s)
{
    SLmillisecond pos = 0;
    if (!s->obj || !s->bps || (*s->play)->GetPosition(s->play, &pos) != SL_RESULT_SUCCESS) return 0;
    long long q = s->written * 1000000 / s->bps - (long long)pos * 1000;
    return q > 0 ? q : 0;
}

static void s_close(struct pstream *s, int drain)
{
    if (!s->obj) return;
    if (drain) {
        /* the queue running empty means AudioTrack has it all; then until the position catches up (+ a margin for the
         * HAL, whose buffer the position does not include) */
        pthread_mutex_lock(&s->lock);
        struct timespec ts; deadline(&ts, 5000);
        int rc = 0;
        while (s->busy && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&s->cond, &s->lock, &ts);
        pthread_mutex_unlock(&s->lock);
        long long q = s_queued_us(s);
        if (q > 2000000) q = 2000000;
        usleep(q + 50000);
    }
    (*s->play)->SetPlayState(s->play, SL_PLAYSTATE_STOPPED);
    (*s->q)->Clear(s->q);
    pthread_mutex_lock(&vol_lock);              /* vol_write may be setting its level right now */
    (*s->obj)->Destroy(s->obj);
    s->obj = NULL; s->play = NULL; s->q = NULL; s->vol = NULL;
    pthread_mutex_unlock(&vol_lock);
    free(s->buf); s->buf = NULL;
    s->busy = 0;
}

int  play_open(unsigned rate, unsigned channels) { return s_open(&voice, rate, channels, SL_ANDROID_STREAM_MEDIA); }
int  play_write(const void *data, size_t len) { return s_write(&voice, data, len); }
void play_close(int drain) { s_close(&voice, drain); }

int       music_open(unsigned rate, unsigned channels) { return s_open(&music, rate, channels, SL_ANDROID_STREAM_MEDIA); }
int       music_write(const void *data, size_t len) { return s_write(&music, data, len); }
long long music_queued_us(void) { return s_queued_us(&music); }
void      music_close(void) { s_close(&music, 0); }

/* The Bluetooth speaker stream is the mixer's own A2DP route on the Dots (btout.c stands in for btmanagerd towards it).
 * Here Android's Bluetooth stack owns the controller and routes AudioTracks to a speaker itself; until that is worked
 * out there is no such stream. */
int       bt_open(unsigned rate, unsigned channels) { (void)rate; (void)channels; return -1; }
int       bt_write(const void *data, size_t len) { (void)data; (void)len; return -1; }
long long bt_queued_us(void) { return 0; }
void      bt_close(void) {}

/* Drop In's call audio: the voice stream, its own volume and routing in Android */
int       voip_open(unsigned rate, unsigned channels) { return s_open(&voip, rate, channels, SL_ANDROID_STREAM_VOICE); }
long long voip_queued_us(void) { return s_queued_us(&voip); }
int       voip_write(const void *data, size_t len) { return s_write(&voip, data, len); }
void      voip_close(void) { s_close(&voip, 0); }

/* System stream: its own volume in Android, as stock's UI sounds.  go() says how long the sound is wanted: a
 * ringing timer ends when someone stops it, so what is left of the queue is dropped then, not drained.  Written a
 * slot at a time with go() asked in between, as audio_mixer.c does per buffer: s_write blocks while the queue is
 * full, and a long ringtone in one call would play on until its last slot is queued. */
static int always(void) { return 1; }
void play_earcon(const short *pcm, size_t samples, unsigned rate) { play_earcon_while(pcm, samples, rate, always); }
void play_earcon_while(const short *pcm, size_t samples, unsigned rate, int (*go)(void))
{
    if (s_open(&earcon, rate, 1, SL_ANDROID_STREAM_SYSTEM) < 0) { fprintf(stderr, "earcon: no player\n"); return; }
    const char *p = (const char *)pcm;
    size_t len = samples * 2;
    while (len && go()) {
        size_t n = len < earcon.slot ? len : earcon.slot;
        if (s_write(&earcon, p, n) < 0) break;
        p += n; len -= n;
    }
    while (s_queued_us(&earcon) > 0 && go()) usleep(20000);
    s_close(&earcon, go() ? 1 : 0);
}
