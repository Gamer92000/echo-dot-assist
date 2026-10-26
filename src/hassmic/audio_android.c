/* Device backend for Echos without Amazon's mixer daemon (checkers, Echo Show 5): Android's own audio through OpenSL ES.
 * There Amazon's front end runs inside the audio HAL (audio.primary.mt8163.so links libasp.so, docs/re-checkers.md):
 * an AudioRecord with source VOICE_RECOGNITION (6) or HOTWORD (1999) gets the ASR pipeline's output (16 kHz mono s16,
 * AEC + beams, the micAsr of donut), and every AudioTrack goes to the one "primary output" through the HAL's Speaker
 * pipeline, whose signal is the ASR pipeline's echo reference.  So each of our streams is a player of its own and
 * AudioFlinger mixes them.  OpenSL ES is the NDK's stable C API over AudioRecord/AudioTrack (libwilhelm): no C++ ABI of
 * the firmware's libmedia to match.
 *
 * What has to hold there (all of it does on the first Echo Show, 2026-10-13):
 *  - recording rights: recordingAllowed() (libserviceutility.so) wants RECORD_AUDIO, then lets uid 0 through and, as
 *    OpenSL ES names no package, looks up one for any other uid to note the app op against: hassmic runs as system
 *    (1000, package "android"; device.conf DAEMON_USER);
 *  - one capture at a time: Android 7 lets a newer AudioRecord take the input from a HOTWORD one, so amazon.speech.sim
 *    (which records 1999 and 2999 itself) must be disabled first;
 *  - the preset: VOICE_RECOGNITION is the closest OpenSL ES has to stock's HOTWORD source, both pick the ASR pipeline
 *    (getAspPipelineType in the HAL); HOTWORD itself is not reachable from OpenSL ES. */
#include "audio.h"
#include "core.h"
#include "afe_parcel.h"
#include "btout.h"
#include <stdatomic.h>
#include <sys/system_properties.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
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

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

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
/* audio_*_gain (separate volumes, main.c): our own PCM turned down, as audio_mixer.c does, before the player gets it.
 * Not the player's volume: Amazon's audio stack puts a track back to 1.0 (vol_write's note below).  1 = 65536 */
static atomic_int music_q16 = 65536, tts_q16 = 65536, call_q16 = 65536;
static int q16(float g) { return (int)lroundf((g < 0 ? 0 : g > 1 ? 1 : g) * 65536); }
void audio_music_gain(float g) { atomic_store(&music_q16, q16(g)); }
void audio_tts_gain(float g) { atomic_store(&tts_q16, q16(g)); }
void audio_call_gain(float g) { atomic_store(&call_q16, q16(g)); }

/* whole samples from every writer (audio_mixer.c's copy_gain says why); a lone odd byte passes unscaled */
static void copy_gain(char *dst, const char *src, size_t n, int q)
{
    if (q >= 65536) { memcpy(dst, src, n); return; }
    for (size_t i = 0; i + 1 < n; i += 2) {
        short v; memcpy(&v, src + i, 2); v = (short)((v * q + 32768) >> 16); memcpy(dst + i, &v, 2);   /* rounded */
    }
    if (n & 1) dst[n - 1] = src[n - 1];
}

struct pstream {
    SLObjectItf obj; SLPlayItf play; SLAndroidSimpleBufferQueueItf q;
    atomic_int *gain;                           /* audio_*_gain's for this stream; NULL: none (earcons: main.c scales them) */
    pthread_mutex_t lock; pthread_cond_t cond;
    unsigned bps, slot, busy, next;             /* bytes per second, slot size, slots with the player, next free slot */
    unsigned head, lens[PLAY_NBUF];             /* the oldest slot with the player, and each slot's bytes */
    long long written, consumed, last_cb;       /* bytes since open, bytes the player has taken, when it last took (ms) */
    char *buf;
};
static struct pstream voice = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .gain = &tts_q16 },
                      music = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .gain = &music_q16 },
                      earcon = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER },
                      voip = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .gain = &call_q16 },
                      bt = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .gain = &music_q16 };

/* --- volume: Android's own, the music stream's index (0..its maximum, 30 on the Show), set through IAudioService with
 * the firmware's service tool as the front end is reached (afe_parcel.c): setStreamVolume(stream, index, flags,
 * callingPackage) = transaction 3, getStreamMaxVolume(stream) = 10 (8149's framework: IAudioService$Stub's TRANSACTION_*
 * constants in its boot oat) up to Android 7; Android 9 (athena, SDK 28) has four more methods before them (the player
 * tracking calls), so setStreamVolume = 7 and getStreamMaxVolume = 14 (14 answers 30 as "dumpsys audio" says; 3 is a
 * one-way call there, which gets no reply at all, and 10 answers 0; read off the replies' types and the audio log,
 * 2026-10-15).  Every player of ours is on the music stream, and Fire OS aliases system, voice call and TTS
 * to it ("dumpsys audio": one index for all of them), so it is the one volume, as MainVolume is the Dots' mixer's.  Not
 * the player's own volume (SLVolumeItf, AudioTrack::setVolume): Amazon's audio stack put a track back to 1.0 some 70 ms
 * after it started on the first Echo Show ("TrackVolumeChanged ... track_volume=1.0"), so a sound began at our level and
 * jumped to full.  hassmic's 0..100 is kept in state/volume ("main tts"; the index has 31 steps); TTS has no stream of
 * its own and follows main.  hassmic runs as system: the calling package is "android".  The call (a process, ~0.1 s)
 * runs on a thread of its own, the newest value wins: vol_write is called under core_lock. */
#define TX_SET_STREAM_VOLUME_A7 "3"
#define TX_SET_STREAM_VOLUME_A9 "7"
#define TX_GET_STREAM_MAX_VOLUME_A7 "10"
#define TX_GET_STREAM_MAX_VOLUME_A9 "14"
#define STREAM_MUSIC "3"
static pthread_mutex_t vol_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vol_cond = PTHREAD_COND_INITIALIZER;
static int vols[2] = { -1, -1 }, vols_loaded, vol_want = -1, vol_pushed;
static pthread_once_t vol_once = PTHREAD_ONCE_INIT;

static const char *vol_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/volume", d ? d : "/data/local/hassmic/state");
    return p;
}

/* "Result: Parcel(00000000 0000001e   '........')": no exception, then the int; a void method's reply is the exception
 * word alone ("Parcel(00000000    '....')"), v NULL */
static int parcel_int(const char *out, int *v)
{
    unsigned ex, x; const char *s = strstr(out, "Parcel(");
    if (!s || sscanf(s, "Parcel(%8x", &ex) != 1 || ex) return -1;
    if (!v) return 0;
    if (sscanf(s, "Parcel(%8x %8x", &ex, &x) != 2) return -1;
    *v = (int)x;
    return 0;
}

/* the transaction numbers follow the Android release: Android 7.1 (SDK 25: checkers, crown) and 9 (SDK 28: athena) are the
 * ones verified; SDK 28 and newer take 9's, older ones 7's.  A wrong number is somebody else's method: a model on another
 * release checks them first (service call audio, as above) */
static int a9;
static void tx_init(void)
{
    char sdk[PROP_VALUE_MAX] = "";
    __system_property_get("ro.build.version.sdk", sdk);
    a9 = atoi(sdk) >= 28;
}

static void *vol_thread(void *arg)
{
    (void)arg;
    int max = 0;
    tx_init();
    for (;;) {
        pthread_mutex_lock(&vol_lock);
        while (vol_want < 0) pthread_cond_wait(&vol_cond, &vol_lock);
        int v = vol_want; vol_want = -1;
        pthread_mutex_unlock(&vol_lock);
        char out[256], idx[12];
        if (max <= 0) {
            char *get[] = { "/system/bin/service", "call", "audio", a9 ? TX_GET_STREAM_MAX_VOLUME_A9 : TX_GET_STREAM_MAX_VOLUME_A7, "i32", STREAM_MUSIC, NULL };
            core_run(get, out, sizeof out);
            if (parcel_int(out, &max) || max <= 0 || max > 100) { fprintf(stderr, "audio: no maximum for the music stream (%s)\n", out); max = 0; continue; }
        }
        snprintf(idx, sizeof idx, "%d", (v * max + 50) / 100);
        char *set[] = { "/system/bin/service", "call", "audio", a9 ? TX_SET_STREAM_VOLUME_A9 : TX_SET_STREAM_VOLUME_A7, "i32", STREAM_MUSIC, "i32", idx,
                        "i32", "0", "s16", "android", NULL };
        core_run(set, out, sizeof out);
        if (parcel_int(out, NULL)) fprintf(stderr, "audio: setting the music stream to %s of %d failed (%s)\n", idx, max, out);
    }
    return NULL;
}
static void vol_start(void) { pthread_t t; if (!pthread_create(&t, NULL, vol_thread, NULL)) pthread_detach(t); }

static void vol_push(int v)                     /* under vol_lock */
{
    pthread_once(&vol_once, vol_start);
    vol_want = v;
    pthread_cond_signal(&vol_cond);
}

static void vol_load(void)                      /* under vol_lock */
{
    if (vols_loaded) return;
    vols_loaded = 1;
    FILE *f = fopen(vol_path(), "r"); int m, t;
    if (f && fscanf(f, "%d %d", &m, &t) == 2 && m >= 0 && m <= 100 && t >= 0 && t <= 100) { vols[VOL_MAIN] = m; vols[VOL_TTS] = t; }
    if (f) fclose(f);
    /* not pushed from here: on a Bluetooth speaker that sets its own volume the file holds the full scale the mixer
     * played at, and the core puts the Echo's own volume back just after (core_volume); pushed first, the Echo's
     * speaker had a moment at full volume.  The core's first write of it pushes, changed or not (vol_write). */
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
    if (which == VOL_MAIN && (changed || !vol_pushed)) { vol_pushed = 1; vol_push(v); }   /* Android's index as hassmic has it */
    pthread_mutex_unlock(&vol_lock);
}

/* --- the Bluetooth speaker's route (btout.c): AudioPolicy's connection state of an A2DP device, which Android's own
 * Bluetooth service has AudioService set; here straight on media.audio_policy (IAudioPolicyService, a native interface:
 * setDeviceConnectionState = 1 (device, state, address and name as C strings; status back), getDeviceConnectionState
 * = 2 (device, address; state back)).  Connected, AudioFlinger opens an output on the A2DP HAL, whose CHECK_READY
 * btout.c acks only with a stream to the speaker configured (else the policy drops the device again: -38), and the
 * media strategy, i.e. every player of ours, plays there alone: the Echo's speaker is silent, as on the Dots.  All
 * seen on the first Echo Show (2026-10-13): audioserver connects to btout.c's abstract sockets, and policy and HAL
 * speak AOSP's protocol.  AudioService keeps a volume index per output device and sets the one of the device the music
 * stream plays on, so after a move ours is set again, for the device it now is.  The policy forgets the device when
 * audioserver restarts (audio_a2dp_lost); getDeviceConnectionState needs the address, so the one connected is kept in
 * state/a2dp-route, which also tells a restarted hassmic what an earlier one left. */
#define POLICY_A2DP 0x80                /* AUDIO_DEVICE_OUT_BLUETOOTH_A2DP */

static const char *route_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/a2dp-route", d ? d : "/data/local/hassmic/state");
    return p;
}

static void addr_text(uint64_t a, char *s)      /* as BluetoothDevice.getAddress() gives it AudioService */
{
    sprintf(s, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)(a >> 40 & 0xff), (unsigned)(a >> 32 & 0xff),
            (unsigned)(a >> 24 & 0xff), (unsigned)(a >> 16 & 0xff), (unsigned)(a >> 8 & 0xff), (unsigned)(a & 0xff));
}

/* the reply's one word, -1000 when there was none (no tool, audioserver gone) */
static int policy_call(const char *code, int state, const char *addr, const char *name)
{
    struct afe_call c; char out[256]; uint32_t w[2];
    svc_call_start(&c, "media.audio_policy", code);
    if (svc_i32(&c, POLICY_A2DP) || (state >= 0 && svc_i32(&c, state)) || svc_cstr(&c, addr) || (name && svc_cstr(&c, name)))
        return -1000;
    core_run(c.argv, out, sizeof out);
    return svc_reply_words(out, w, 2) == 1 ? (int)w[0] : -1000;
}

static int policy_state(const char *addr) { return policy_call("2", -1, addr, NULL); }

int audio_a2dp_route(int on, uint64_t addr, const char *name)
{
    char a[18], n[64]; long long t = now_ms();
    addr_text(addr, a);
    snprintf(n, sizeof n, "%s", name ? name : "");     /* the policy only shows it (dumpsys) */
    int r = policy_call("1", on ? 1 : 0, a, n), now = policy_state(a);
    if (now != (on ? 1 : 0)) {
        fprintf(stderr, "btout: AudioPolicy did not move the output %s (status %d, state %d)\n", on ? "to the speaker" : "back", r, now);
        return -1;
    }
    if (on) { FILE *f = fopen(route_path(), "w"); if (f) { fprintf(f, "%s\n", a); fclose(f); } }
    else unlink(route_path());
    pthread_mutex_lock(&vol_lock);
    vol_load();
    if (vols[VOL_MAIN] >= 0) vol_push(vols[VOL_MAIN]);
    pthread_mutex_unlock(&vol_lock);
    fprintf(stderr, "btout: output %s (AudioPolicy, %lld ms)\n", on ? "to the speaker" : "to the Echo", now_ms() - t);
    return 0;
}

int audio_a2dp_lost(uint64_t addr)
{
    char a[18]; addr_text(addr, a);
    int st = policy_state(a);
    return st == 0 ? 1 : st == 1 ? 0 : -1;
}
int audio_a2dp_aipc(void) { return 0; }

void audio_a2dp_reset(void)
{
    char a[32] = ""; FILE *f = fopen(route_path(), "r");
    if (!f) return;
    if (!fgets(a, sizeof a, f)) a[0] = 0;
    fclose(f);
    a[strcspn(a, "\r\n")] = 0;
    if (a[0] && policy_state(a) == 1) {
        policy_call("1", 0, a, "");
        fprintf(stderr, "btout: %s left connected in AudioPolicy by an earlier run: back to the Echo\n", a);
    }
    unlink(route_path());
}

static atomic_llong last_out;           /* when any of our players last had audio (the earcon's lead-in, below) */

static void play_cb(SLAndroidSimpleBufferQueueItf q, void *ctx)
{
    struct pstream *s = ctx; (void)q;
    pthread_mutex_lock(&s->lock);
    if (s->busy) { s->busy--; s->consumed += s->lens[s->head]; s->head = (s->head + 1) % PLAY_NBUF; s->last_cb = now_ms(); }
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static void s_close(struct pstream *s, int drain);

/* A player opened while the Bluetooth speaker's route moves waits for it (1.5 s at most).  AudioFlinger puts a track on
 * the output the policy names when it is created and moves it, from wherever it got to, when that output closes: a
 * sound started as the speaker went away began on the dead A2DP output and was moved to the Echo's speaker mid-sound,
 * with an audible pop (the first Echo Show, 2026-10-13: "Bluetooth disconnected" and Home Assistant's announcement of
 * it come within 30 ms of the disconnect, the route ~200 ms later; the route moved alone, with nothing playing, made no
 * sound). */
static void route_settle(void) { for (int i = 0; i < 75 && btout_moving(); i++) usleep(20000); }

static int s_open(struct pstream *s, unsigned rate, unsigned channels, SLint32 stream_type)
{
    if (!engine_ok() || !rate || channels < 1 || channels > 2) return -1;
    route_settle();
    if (s->obj) s_close(s, 0);
    SLDataLocator_AndroidSimpleBufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, PLAY_NBUF };
    SLDataFormat_PCM fmt = { SL_DATAFORMAT_PCM, channels, rate * 1000, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                             channels == 2 ? SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT : SL_SPEAKER_FRONT_CENTER,
                             SL_BYTEORDER_LITTLEENDIAN };
    SLDataSource src = { &loc, &fmt };
    SLDataLocator_OutputMix out = { SL_DATALOCATOR_OUTPUTMIX, mix_obj };
    SLDataSink sink = { &out, NULL };
    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
    const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
    if ((*engine)->CreateAudioPlayer(engine, &s->obj, &src, &sink, 2, ids, req) != SL_RESULT_SUCCESS) {
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
    s->busy = s->next = s->head = 0; s->written = s->consumed = 0; s->last_cb = now_ms();
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
        copy_gain(slot, p, n, s->gain ? atomic_load(s->gain) : 65536);
        if ((*s->q)->Enqueue(s->q, slot, n) != SL_RESULT_SUCCESS) { pthread_mutex_unlock(&s->lock); return -1; }
        atomic_store(&last_out, now_ms());
        s->lens[s->next] = n; s->busy++; s->next = (s->next + 1) % PLAY_NBUF; s->written += n;
        pthread_mutex_unlock(&s->lock);
        p += n; len -= n;
    }
    return 0;
}

/* What is queued: the slots the player has not taken back.  Not from the player's position: on the first Echo Show
 * GetPosition stayed at 0 for these players, every sound counted as never played (2026-10-13).  AudioTrack's and the
 * HAL's own buffers behind the slots are a steady latency, as the mixer's is on the Dots, and not counted: counted
 * from each callback (120 ms) they made "queued" never fall below ~100 ms while a stream played, and a2dp.c, which
 * writes below 60 ms, starved its player (a phone's audio in pieces, the jitter buffer at a second).  Where the end of
 * a sound matters (earcons, a draining close) s_drained adds TAIL_MS after the last slot was taken. */
#define TAIL_MS 120
static long long s_queued_us(struct pstream *s)
{
    if (!s->obj || !s->bps) return 0;
    pthread_mutex_lock(&s->lock);
    long long q = (s->written - s->consumed) * 1000000 / s->bps;
    pthread_mutex_unlock(&s->lock);
    return q;
}
static int s_drained(struct pstream *s)          /* every slot taken back, and the last one played out */
{
    if (!s->obj) return 1;
    pthread_mutex_lock(&s->lock);
    int d = s->written == s->consumed && now_ms() - s->last_cb >= TAIL_MS;
    pthread_mutex_unlock(&s->lock);
    return d;
}

static void s_close(struct pstream *s, int drain)
{
    if (!s->obj) return;
    if (drain) {
        /* the queue running empty means AudioTrack has it all; then what AudioTrack and the HAL still hold (TAIL_MS) */
        pthread_mutex_lock(&s->lock);
        struct timespec ts; deadline(&ts, 5000);
        int rc = 0;
        while (s->busy && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&s->cond, &s->lock, &ts);
        pthread_mutex_unlock(&s->lock);
        for (int i = 0; i < 50 && !s_drained(s); i++) usleep(10000);
    }
    (*s->play)->SetPlayState(s->play, SL_PLAYSTATE_STOPPED);
    (*s->q)->Clear(s->q);
    (*s->obj)->Destroy(s->obj);
    s->obj = NULL; s->play = NULL; s->q = NULL;
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

/* The Bluetooth speaker stream: what a phone plays to the Echo (a2dp.c), the mixer's Bluetooth stream on the Dots.
 * Here a player of its own beside music, on the music stream like everything of ours.  Until it was there (-1) the
 * phone's audio filled a2dp.c's buffer and was dropped: paired and streaming, nothing heard (2026-10-13). */
int       bt_open(unsigned rate, unsigned channels) { return s_open(&bt, rate, channels, SL_ANDROID_STREAM_MEDIA); }
int       bt_write(const void *data, size_t len) { return s_write(&bt, data, len); }
long long bt_queued_us(void) { return s_queued_us(&bt); }
void      bt_close(void) { s_close(&bt, 0); }

/* Drop In's call audio: the voice stream, its own volume and routing in Android */
int       voip_open(unsigned rate, unsigned channels) { return s_open(&voip, rate, channels, SL_ANDROID_STREAM_MEDIA); }
long long voip_queued_us(void) { return s_queued_us(&voip); }
int       voip_write(const void *data, size_t len) { return s_write(&voip, data, len); }
void      voip_close(void) { s_close(&voip, 0); }

/* On the music stream with everything else of ours (the volume above).  go() says how long the sound is wanted: a
 * ringing timer ends when someone stops it, so what is left of the queue is dropped then, not drained.  Written a
 * slot at a time with go() asked in between, as audio_mixer.c does per buffer: s_write blocks while the queue is
 * full, and a long ringtone in one call would play on until its last slot is queued.  The player is PLAYING from
 * s_open on, so it consumes from the first Enqueue: audio_earcon_started then (earsup.c looks for the echo from there
 * on, so early is safe, late is not). */
static int always(void) { return 1; }
void (*audio_earcon_started)(unsigned lead_ms);

/* The output is closed after ~3 s without audio and opening it takes ~170 ms (AudioALSAPlaybackHandlerNormal: open
 * pcm 27.644 -> 27.811) plus the speaker amplifier switching on: the 120 ms wake blip, written to a cold output, was
 * never heard (2026-10-14; the 0.5 s mute sounds were, clipped).  A cold earcon starts after this much silence. */
#define COLD_MS 2500
#define LEAD_MS 350

void play_earcon(const short *pcm, size_t samples, unsigned rate) { play_earcon_while(pcm, samples, rate, always); }
void play_earcon_while(const short *pcm, size_t samples, unsigned rate, int (*go)(void))
{
    if (s_open(&earcon, rate, 1, SL_ANDROID_STREAM_MEDIA) < 0) { fprintf(stderr, "earcon: no player\n"); return; }
    /* the wait for the queue to run out ends at the sound's length + 1 s whatever the position says, as audio_mixer.c's:
     * on the first Echo Show the position stopped short of what was written after the mute sound (0.4 s), and this
     * thread waited for ever, every later sound queued behind it (2026-10-13) */
    long long end = now_ms() + (long long)(samples * 1000ULL / rate) + 1000;
    const char *p = (const char *)pcm;
    size_t len = samples * 2;
    unsigned lead_ms = 0;
    if (now_ms() - atomic_load(&last_out) > COLD_MS) {
        static short lead[48000 * LEAD_MS / 1000];
        size_t n = (size_t)rate * LEAD_MS / 1000 < sizeof lead / 2 ? (size_t)rate * LEAD_MS / 1000 : sizeof lead / 2;
        if (s_write(&earcon, lead, n * 2) == 0) lead_ms = (unsigned)(n * 1000 / rate);
        end += LEAD_MS;
    }
    while (len && go()) {
        size_t n = len < earcon.slot ? len : earcon.slot;
        if (s_write(&earcon, p, n) < 0) break;
        /* the sound starts after the lead-in: earsup.c's echo search (up to 400 ms after this) would miss it otherwise */
        if (p == (const char *)pcm && audio_earcon_started) audio_earcon_started(lead_ms);
        p += n; len -= n;
    }
    while (!s_drained(&earcon) && go() && now_ms() < end) usleep(20000);
    atomic_store(&last_out, now_ms());
    if (now_ms() >= end) fprintf(stderr, "earcon: the player's position stopped %lld ms short\n", s_queued_us(&earcon) / 1000);
    s_close(&earcon, go() && now_ms() < end ? 1 : 0);
}

/* --- the front end: libasp inside the audio HAL, its binder service "audiosignalprocessor" (docs/re-checkers.md).
 * Command numbers are the ASP ones donut's LIPC names map to (libgenericaspclient.so; aspclient.odex has 146 and 23 by
 * name): SET_LISTENING_MODE 146 (int32), SET_WAKEWORD_METADATA 114 (the JSON; libasp refuses 0 or over 4095 bytes),
 * REQUEST_ARBITRATION_JSON 23 (int32 256 in, 256 bytes out), NOTIFY_ASR_STREAM_STOPPED 14 (int32).  Through the
 * firmware's own service tool (afe_parcel.c): no binder library of ours. */

static void afe_send(int cmd, const void *in, size_t len)
{
    struct afe_call c;
    if (afe_call_build(&c, cmd, in, len, 4) == 0) core_spawn(c.argv);
}

void afe_listening(int on) { int32_t v = on ? 1 : 0; afe_send(146, &v, sizeof v); }
void afe_stream_stopped(void) { int32_t v = 1; afe_send(14, &v, sizeof v); }

int afe_arbitration(long ts, long te, char *json, size_t n)
{
    char meta[96], out[2048]; struct afe_call c; int32_t size = 256;
    snprintf(meta, sizeof meta, "{\"timestamp_before_ww_start\":%ld,\"timestamp_before_ww_end\":%ld}", ts, te);
    if (afe_call_build(&c, 114, meta, strlen(meta), 4)) return 0;
    core_run(c.argv, out, sizeof out);                  /* the times first: the JSON's energies are measured on them */
    if (afe_call_build(&c, 23, &size, sizeof size, 256)) return 0;
    core_run(c.argv, out, sizeof out);
    return afe_call_reply(out, (unsigned char *)json, n) > 0;
}

/* The user equalizer: SET_USER_EQ_INFO 21 and GET_USER_EQ_INFO 29 (donut's LIPC names paired with them; this libasp has
 * the UserEq code and its JSON).  Payloads as LIPC's strings: the JSON in for a set; for a get the getter convention of
 * REQUEST_ARBITRATION_JSON (an int32 reply size in, the JSON out), which is inferred, not traced to UserEq's handler. */
int afe_eq_get(char *json, size_t n)
{
    char out[8192]; struct afe_call c; int32_t size = 512;     /* the JSON is ~110 bytes; 512 dumps to ~2.3 kB */
    if (afe_call_build(&c, 29, &size, sizeof size, size)) return 0;
    core_run(c.argv, out, sizeof out);
    return afe_call_reply(out, (unsigned char *)json, n) > 0;
}

void afe_eq_set(const char *json) { afe_send(21, json, strlen(json)); }
