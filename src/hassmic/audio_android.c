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
#include "resample.h"
#include "audio.h"
#include "core.h"
#include "afe_parcel.h"
#include "board.h"
#include "btout.h"
#include "statefile.h"
#include <stdatomic.h>
#include <sys/system_properties.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <dlfcn.h>
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
 * queued, and each is queued again once cap_read's caller is done with it, so block k is always buf[k % CAP_NBUF].
 * With all of them full the recorder has nowhere to put what comes and it is lost: 8 blocks (160 ms) were too few for
 * the front end's reading after a wake word, 240-380 ms on the capture thread then (issue #77).  640 ms for any stall
 * of the caller, logged by cap_read. */
#define CAP_BLOCK 320                   /* samples: 20 ms */
#define CAP_NBUF  32
static SLObjectItf rec_obj;
static SLRecordItf rec;
static SLAndroidSimpleBufferQueueItf rec_q;
static short cap_buf[CAP_NBUF][CAP_BLOCK];
static pthread_mutex_t cap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cap_cond = PTHREAD_COND_INITIALIZER;
static unsigned cap_filled, cap_taken;
static int cap_held;
static long long cap_full_ms;           /* when the last free block was filled (cap_lock): from then on audio is lost */
static long long cap_lost_ms, cap_lost_said;    /* cap_read: lost since the last line, when that was */
/* The recorder's source: VOICE_RECOGNITION (the HAL's ASR pipeline), or VOICE_COMMUNICATION (its "Voice" pipeline) for a
 * call with Amazon's call processing (callmic_*, below).  Wanted by the call's side, switched by the capture thread,
 * the recorder's only owner, at its next cap_read */
enum { SRC_ASR, SRC_CALL, SRC_HOT };   /* want: the assistant's (SRC_ASR) or a call's switch; now: also SRC_HOT */
static atomic_int cap_want = SRC_ASR, cap_now = -1;
static int a9; static void tx_init(void);
/* HOTWORD for the assistant's wherever a call has Amazon's call processing beside it (board call_mic), as stock's
 * wake word; VOICE_RECOGNITION for good once HOTWORD failed here */
static atomic_int hot_failed;           /* for good: this firmware has no such recorder, or refuses the source */
static atomic_llong hot_later;          /* a failure of the moment (audioserver settling at boot, a dead recorder):
                                           VOICE_RECOGNITION until then, HOTWORD again after it (now_ms) */
#define HOT_RETRY_MS 10000
#define HOT_STALLS 3                    /* HOTWORD reopened this often in a row with nothing read: a failure of the moment */
int callmic_present(void);
static long long now_ms(void);
/* Another app's recording (Kiosk Satellite's microphone) does not take the input from a HOTWORD one here, as AOSP's
 * would: Fire OS's startInput asks startInput_fosExt_supportMultipleInputStreams (1) before it preempts a HOTWORD
 * input, then admits the newcomer beside it (checkers' libaudiopolicymanagerdefault.so, 2026-11-26) */
/* HOTWORD only for a call (callmic_open to _close), as the one source a call's recorder may run beside: outside calls the
 * assistant's records VOICE_RECOGNITION as it always did (the same ASR pipeline), so nothing changes for an Echo that
 * makes no calls, and a model whose policy treats HOTWORD otherwise risks only its calls */
static atomic_int hot_for_call;
static int want_hot(void) { return atomic_load(&hot_for_call) && !atomic_load(&hot_failed) && now_ms() >= atomic_load(&hot_later) && callmic_present(); }
#define HOT_TRIES 3                     /* failures of the moment in a row: then for good (each costs audio: a reopen) */
static atomic_int hot_tries;            /* reset by a HOTWORD read that brought audio */
static atomic_int cm_beside;            /* the call's recorder runs beside the HOTWORD capture (callmic_*) */
static atomic_int beside_fails;         /* HOTWORD opens refused while it runs (cap_open) */
static void beside_giveup(void);        /* three of them: the call's recorder goes, the one recorder switches (below) */
static void beside_fail(void) { if (atomic_fetch_add(&beside_fails, 1) + 1 >= HOT_TRIES) beside_giveup(); }
static void hot_later_by(const char *why)
{
    /* beside a call's VOICE_COMMUNICATION recorder the policy admits HOTWORD only: VOICE_RECOGNITION would be refused,
     * so HOTWORD is opened again, HOT_TRIES times, then the one recorder takes the call (beside_giveup) */
    if (atomic_load(&cm_beside)) { fprintf(stderr, "audio: HOTWORD %s beside a call's recorder\n", why); beside_fail(); return; }
    if (atomic_fetch_add(&hot_tries, 1) + 1 >= HOT_TRIES) {
        atomic_store(&hot_failed, 1);
        fprintf(stderr, "audio: HOTWORD %s, %d times in a row: VOICE_RECOGNITION for this run\n", why, HOT_TRIES);
        return;
    }
    atomic_store(&hot_later, now_ms() + HOT_RETRY_MS);
    fprintf(stderr, "audio: HOTWORD %s: VOICE_RECOGNITION, HOTWORD again in %d s\n", why, HOT_RETRY_MS / 1000);
}
/* what runs fits what is wanted: the assistant's is VOICE_RECOGNITION or HOTWORD (one pipeline, "ASR"), whichever
 * want_hot says */
static int cap_fits(void)
{
    int n = atomic_load(&cap_now), w = atomic_load(&cap_want);
    if (w == SRC_CALL) return n == SRC_CALL;
    return n == (want_hot() ? SRC_HOT : SRC_ASR);
}

/* --- the assistant's recording as stock's wake word has it: source HOTWORD (1999), the HAL's ASR pipeline as
 * VOICE_RECOGNITION's (getAspPipelineType: both 0), and the one Fire OS's policy lets a call's VOICE_COMMUNICATION run
 * beside (docs/re-checkers.md "Calls").  OpenSL ES has no such preset on Android 7, so there the firmware's own
 * android::AudioRecord (libmedia.so; one signature on checkers, cronos, crown, cupcake, SDK 25), found with dlsym:
 * AudioRecord(const String16& opPackageName), set(source, rate, format, channel mask, frame count, callback, user,
 * notification frames, threadCanCallJava, session, transfer, flags, uid, pid, attributes), start, read, stop, and
 * RefBase's strong count (the object is freed by its last decStrong: malloc'd, as bionic's operator new does).  No
 * callback (TRANSFER_SYNC), reads that never wait (a recorder the policy stopped would hold a waiting read for good).
 * Android 9 (athena) has Amazon's presets in OpenSL ES: 6 = HOTWORD.  Either failing: VOICE_RECOGNITION as before */
#define NAT_OBJ 4096                    /* sizeof(AudioRecord) is ~0x1c0 on SDK 25: room to spare */
static struct {
    int tried, ok;
    void (*ctor)(void *self, const void *pkg);
    int (*set)(void *self, int source, uint32_t rate, int format, uint32_t mask, size_t frames, void (*cbf)(int, void *, void *),
               void *user, uint32_t notify, int can_java, int session, int transfer, int flags, int uid, int pid, const void *attr);
    int (*start)(void *self, int event, int session);
    void (*stop)(void *self);
    long (*read)(void *self, void *buf, size_t n, int blocking);
    void (*s16)(void *self, const char *s);
    void (*s16_free)(void *self);
    void (*inc)(const void *self, const void *id);
    void (*dec)(const void *self, const void *id);
} nat;
static void *nrec;
static size_t nat_have;                 /* what of the block filling came already (kept over a read that gave up) */
static long long nat_t0, nat_got, nat_lost_said;   /* when it started, the samples read since, the last word of a loss */
static short nbuf[CAP_BLOCK];

static int nat_load(void)
{
    if (nat.tried) return nat.ok;
    nat.tried = 1;
    void *m = dlopen("libmedia.so", RTLD_NOW), *u = dlopen("libutils.so", RTLD_NOW);
    if (!m || !u) { fprintf(stderr, "audio: no libmedia.so for a HOTWORD capture\n"); return 0; }
    *(void **)&nat.ctor = dlsym(m, "_ZN7android11AudioRecordC1ERKNS_8String16E");
    *(void **)&nat.set = dlsym(m, "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjjPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tiiPK18audio_attributes_t");
    *(void **)&nat.start = dlsym(m, "_ZN7android11AudioRecord5startENS_11AudioSystem12sync_event_tE15audio_session_t");
    *(void **)&nat.stop = dlsym(m, "_ZN7android11AudioRecord4stopEv");
    *(void **)&nat.read = dlsym(m, "_ZN7android11AudioRecord4readEPvjb");
    *(void **)&nat.s16 = dlsym(u, "_ZN7android8String16C1EPKc");
    *(void **)&nat.s16_free = dlsym(u, "_ZN7android8String16D1Ev");
    *(void **)&nat.inc = dlsym(u, "_ZNK7android7RefBase9incStrongEPKv");
    *(void **)&nat.dec = dlsym(u, "_ZNK7android7RefBase9decStrongEPKv");
    nat.ok = nat.ctor && nat.set && nat.start && nat.stop && nat.read && nat.s16 && nat.s16_free && nat.inc && nat.dec;
    if (!nat.ok) fprintf(stderr, "audio: libmedia.so has not the AudioRecord this build knows: no HOTWORD capture\n");
    return nat.ok;
}

static void nat_close(void)
{
    nat_have = 0;
    if (!nrec) return;
    nat.stop(nrec);
    nat.dec(nrec, &nrec);
    nrec = NULL;
}

static int nat_open(void)
{
    if (!nat_load()) return -1;
    void *o = calloc(1, NAT_OBJ), *pkg = calloc(1, 16);
    if (!o || !pkg) { free(o); free(pkg); return -1; }
    nat.s16(pkg, "android");                    /* hassmic runs as system (device.conf): the package "android" */
    nat.ctor(o, pkg);
    nat.s16_free(pkg); free(pkg);
    nat.inc(o, &nrec);
    /* HOTWORD, 16 kHz, PCM 16 (1), AUDIO_CHANNEL_IN_MONO (0x10), 640 ms of frames as the OpenSL queue holds, no
     * callback, TRANSFER_SYNC (3), no flags, the caller's uid and pid */
    int st = nat.set(o, 1999, CAP_RATE, 1, 0x10, CAP_NBUF * CAP_BLOCK, NULL, NULL, 0, 0, 0, 3, 0, -1, -1, NULL);
    if (st == 0) st = nat.start(o, 0, 0);
    if (!st) { nat_t0 = now_ms(); nat_got = 0; }
    if (st) {
        /* PERMISSION_DENIED (-1) or BAD_VALUE (-22): the source is not this process's, for good; anything else (NO_INIT,
         * DEAD_OBJECT: audioserver busy or restarting) is tried again later */
        fprintf(stderr, "audio: HOTWORD capture refused (%d)\n", st); nrec = o; nat_close();
        return st == -1 || st == -22 ? -2 : -1;
    }
    nrec = o;
    return 0;
}

/* one block of CAP_BLOCK, polled (never a waiting read); 0 after 1.5 s without it */
static int nat_read(const void **data)
{
    size_t have = nat_have; long long t0 = now_ms();
    while (have < sizeof nbuf) {
        long n = nat.read(nrec, (char *)nbuf + have, sizeof nbuf - have, 0);
        /* nothing ready yet is WOULD_BLOCK (-EWOULDBLOCK), not an error (AudioRecord::read: obtainBuffer's
         * TIMED_OUT of a read that does not wait; seen on the Show 5, 2026-11-26) */
        if (n == -EWOULDBLOCK) n = 0;
        else if (n < 0) {               /* a dead recorder: not at once again (cap_read's three then reopen it) */
            static long said; if (n != said) { fprintf(stderr, "audio: HOTWORD read failed (%ld)\n", n); said = n; }
            nat_have = have; usleep(500000); return 0;
        }
        have += (size_t)n;
        if (have < sizeof nbuf) { if (now_ms() - t0 > 1500) { nat_have = have; return 0; } usleep(5000); }
    }
    nat_have = 0;
    /* AudioRecord's ring (640 ms) overran while the caller held us up: more time went by than samples came, said as
     * cap_read says it for the OpenSL queue (at most once a second; the count then starts anew) */
    nat_got += CAP_BLOCK;
    long long now = now_ms(), behind = (now - nat_t0) - nat_got * 1000 / CAP_RATE;
    if (behind > CAP_NBUF * CAP_BLOCK * 1000 / CAP_RATE + 200 && now - nat_lost_said >= 1000) {
        fprintf(stderr, "audio: HOTWORD capture held up: about %lld ms of the mic lost\n", behind - CAP_NBUF * CAP_BLOCK * 1000 / CAP_RATE);
        nat_lost_said = now; nat_t0 = now; nat_got = 0;
    }
    *data = nbuf;
    return sizeof nbuf;
}
static void cm_feed(const short *b, int n);
static void mode_watch_start(void);           /* a run that ended inside a call left Android's call mode on */

static void cap_cb(SLAndroidSimpleBufferQueueItf q, void *ctx)
{
    (void)q; (void)ctx;
    pthread_mutex_lock(&cap_lock);
    if (++cap_filled - cap_taken == CAP_NBUF) cap_full_ms = now_ms();
    pthread_cond_signal(&cap_cond);
    pthread_mutex_unlock(&cap_lock);
}

int cap_open(void)
{
    if (!engine_ok()) return -1;
    mode_watch_start();
    tx_init();
    int want = atomic_load(&cap_want);
    if (want == SRC_ASR && !a9 && want_hot()) {
        int r = nat_open();
        if (r == 0) {
            atomic_store(&cap_now, SRC_HOT);
            fprintf(stderr, "audio: capture HOTWORD (as stock's wake word), %u Hz mono\n", CAP_RATE);
            return 0;
        }
        if (atomic_load(&cm_beside)) { beside_giveup(); return -1; }   /* refused: VOICE_RECOGNITION would be too, beside it */
        if (r == -2 || !nat.ok) atomic_store(&hot_failed, 1); else hot_later_by("failed");
    }
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
    /* Android 9's own preset 6 (athena's libwilhelm: 6, 7, 8 = sources 1999, 2999, 3002): HOTWORD, else as before */
    int hot6 = want == SRC_ASR && a9 && want_hot();
    SLuint32 preset = want == SRC_CALL ? SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION : hot6 ? 6 : SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION;
    int got_cfg = (*rec_obj)->GetInterface(rec_obj, SL_IID_ANDROIDCONFIGURATION, &cfg) == SL_RESULT_SUCCESS;
    int cfg_ok = got_cfg && (*cfg)->SetConfiguration(cfg, SL_ANDROID_KEY_RECORDING_PRESET, &preset, sizeof preset) == SL_RESULT_SUCCESS;
    if ((!got_cfg || !cfg_ok) && hot6 && atomic_load(&cm_beside)) {   /* VOICE_RECOGNITION would be refused beside it */
        (*rec_obj)->Destroy(rec_obj); rec_obj = NULL;
        beside_giveup();
        return -1;
    }
    if (!got_cfg && (hot6 || want == SRC_CALL)) {      /* no preset can be set: neither HOTWORD nor a call's */
        fprintf(stderr, "audio: the recorder has no configuration interface\n");
        (*rec_obj)->Destroy(rec_obj); rec_obj = NULL;
        if (hot6) { atomic_store(&hot_failed, 1); return cap_open(); }
        return -1;
    }
    if (!cfg_ok && hot6) {
        fprintf(stderr, "audio: no HOTWORD preset here, VOICE_RECOGNITION\n");
        atomic_store(&hot_failed, 1); hot6 = 0; preset = SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION;
        (*cfg)->SetConfiguration(cfg, SL_ANDROID_KEY_RECORDING_PRESET, &preset, sizeof preset);
    } else if (!cfg_ok && want == SRC_CALL) {
        /* a recorder on the default preset is no call pipeline: call.c would send it without the assistant's gain */
        fprintf(stderr, "audio: the VOICE_COMMUNICATION preset refused\n");
        (*rec_obj)->Destroy(rec_obj); rec_obj = NULL;
        return -1;
    }
    /* Realize is where AudioFlinger decides about the recording rights (see the top) */
    if ((*rec_obj)->Realize(rec_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
        (*rec_obj)->GetInterface(rec_obj, SL_IID_RECORD, &rec) != SL_RESULT_SUCCESS ||
        (*rec_obj)->GetInterface(rec_obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &rec_q) != SL_RESULT_SUCCESS ||
        (*rec_q)->RegisterCallback(rec_q, cap_cb, NULL) != SL_RESULT_SUCCESS) {
        fprintf(stderr, "audio: recorder refused (RECORD_AUDIO for this uid? another capture running?)\n");
        (*rec_obj)->Destroy(rec_obj); rec_obj = NULL;
        if (hot6) {
            /* beside a call's recorder VOICE_RECOGNITION would be refused too: cap_read tries HOTWORD again in a second */
            if (atomic_load(&cm_beside)) {
                fprintf(stderr, "audio: HOTWORD refused beside a call's recorder\n");
                beside_giveup();
                return -1;
            }
            hot_later_by("refused"); return cap_open();       /* VOICE_RECOGNITION for now (want_hot is 0 meanwhile) */
        }
        return -1;
    }
    cap_filled = cap_taken = 0; cap_held = 0;
    for (int i = 0; i < CAP_NBUF; i++) (*rec_q)->Enqueue(rec_q, cap_buf[i], sizeof cap_buf[i]);
    if ((*rec)->SetRecordState(rec, SL_RECORDSTATE_RECORDING) != SL_RESULT_SUCCESS) {
        fprintf(stderr, "audio: recording does not start\n");
        cap_close();
        return -1;
    }
    atomic_store(&cap_now, hot6 ? SRC_HOT : want);
    fprintf(stderr, "audio: OpenSL ES capture, %s, %u Hz mono\n", want == SRC_CALL ? "VOICE_COMMUNICATION (a call)" : hot6 ? "HOTWORD (as stock's wake word)" : "VOICE_RECOGNITION", CAP_RATE);
    return 0;
}

/* A capture that stops delivering (the input taken by another AudioRecord, audioserver restarted) is reopened after
 * three empty waits, as audio_mixer.c does with a stalled micAsr. */
#define STALL_READS 3

int cap_read(const void **data)
{
    static int empty;
    if ((rec_obj || nrec) && !cap_fits()) cap_close();    /* a call's source, or back */
    int w0 = atomic_load(&cap_want);                      /* (beside_giveup may ask for the call's source in cap_open) */
    if (!rec_obj && !nrec && cap_open() < 0) {
        /* a call's source refused: the assistant's again, and the call keeps micAsr (callmic_open is told) */
        if (w0 == SRC_CALL && atomic_load(&cap_want) == SRC_CALL) { fprintf(stderr, "audio: no VOICE_COMMUNICATION capture, back to VOICE_RECOGNITION\n"); atomic_store(&cap_want, SRC_ASR); atomic_store(&cap_now, -1); return 0; }
        sleep(1); return 0;
    }
    static int stalls;                          /* a HOTWORD capture reopened in a row with nothing read */
    if (nrec) {
        int n = nat_read(data);
        if (n) { empty = 0; stalls = 0; atomic_store(&hot_tries, 0); atomic_store(&beside_fails, 0); return n; }
        if (++empty >= STALL_READS) {
            fprintf(stderr, "audio: capture delivers nothing, reopening\n"); cap_close(); empty = 0;
            if (++stalls >= HOT_STALLS) { stalls = 0; hot_later_by("delivers nothing"); }
        }
        return 0;
    }
    pthread_mutex_lock(&cap_lock);
    if (cap_held) {
        /* the block the caller had goes back to the recorder (were all of them full, it dropped audio meanwhile and
         * goes on with this one).  The recorder wanted a block one block after the last went full; AudioRecord's own
         * buffer may have kept some of it: "up to".  A line a second at most: a caller slower than the mic all along
         * would otherwise write one per block */
        long long now = now_ms(), full = cap_filled - cap_taken == CAP_NBUF ? now - cap_full_ms : 0;
        if (full >= CAP_BLOCK * 1000 / CAP_RATE) cap_lost_ms += full;
        if (cap_lost_ms && now - cap_lost_said >= 1000) {
            fprintf(stderr, "audio: capture held up, all %d blocks full: up to %lld ms of the mic lost\n", CAP_NBUF, cap_lost_ms);
            cap_lost_ms = 0; cap_lost_said = now;
        }
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
            int hot = atomic_load(&cap_now) == SRC_HOT;       /* (Android 9's preset 6 counts as the native one does) */
            cap_close(); empty = 0;
            if (hot && ++stalls >= HOT_STALLS) { stalls = 0; hot_later_by("delivers nothing"); }
        }
        return 0;
    }
    empty = 0;
    if (atomic_load(&cap_now) == SRC_HOT) { stalls = 0; atomic_store(&hot_tries, 0); atomic_store(&beside_fails, 0); }
    *data = cap_buf[cap_taken % CAP_NBUF]; cap_held = 1;
    pthread_mutex_unlock(&cap_lock);
    if (atomic_load(&cap_now) == SRC_CALL) cm_feed(cap_buf[cap_taken % CAP_NBUF], CAP_BLOCK);
    return sizeof cap_buf[0];
}

void cap_close(void)
{
    if (nrec) { nat_close(); atomic_store(&cap_now, -1); }
    if (!rec_obj) return;
    (*rec)->SetRecordState(rec, SL_RECORDSTATE_STOPPED);
    (*rec_obj)->Destroy(rec_obj);                   /* waits for a callback that is running */
    rec_obj = NULL; rec = NULL; rec_q = NULL; cap_held = 0;
    atomic_store(&cap_now, -1);
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

/* whole samples from every writer (audio_mixer.c says why), at a gain that glides (resample.c gain_glide) */
static float gain_of(atomic_int *q) { return q ? (float)atomic_load(q) / 65536 : 1; }

struct pstream {
    SLObjectItf obj; SLPlayItf play; SLAndroidSimpleBufferQueueItf q;
    atomic_int *gain;                           /* audio_*_gain's for this stream; NULL: none (earcons: main.c scales them) */
    float g;                                    /* the gain now, gliding towards *gain's */
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
                      bt = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .gain = &music_q16 },
                      plug = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .gain = &music_q16 };

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
 * and the state file (statefile.c: its fsyncs take tens of ms on the flash) are done on a thread of its own, the newest
 * value wins: vol_write is called under core_lock. */
#define TX_SET_STREAM_VOLUME_A7 "3"
#define TX_SET_STREAM_VOLUME_A9 "7"
#define TX_GET_STREAM_MAX_VOLUME_A7 "10"
#define TX_GET_STREAM_MAX_VOLUME_A9 "14"
#define STREAM_MUSIC "3"
static pthread_mutex_t vol_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vol_cond = PTHREAD_COND_INITIALIZER;
static int vols[2] = { -1, -1 }, vols_loaded, vol_want = -1, vol_pushed, vol_save[2] = { -1, -1 };    /* vol_save: to write */
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
static void tx_read(void)
{
    char sdk[PROP_VALUE_MAX] = "";
    __system_property_get("ro.build.version.sdk", sdk);
    a9 = atoi(sdk) >= 28;
}
static pthread_once_t tx_once = PTHREAD_ONCE_INIT;
static void tx_init(void) { pthread_once(&tx_once, tx_read); }     /* (asked from the capture, playout and mode threads) */

static void *vol_thread(void *arg)
{
    (void)arg;
    int max = 0;
    tx_init();
    for (;;) {
        pthread_mutex_lock(&vol_lock);
        while (vol_want < 0 && vol_save[0] < 0) pthread_cond_wait(&vol_cond, &vol_lock);
        int v = vol_want, m = vol_save[0], t = vol_save[1]; vol_want = vol_save[0] = vol_save[1] = -1;
        pthread_mutex_unlock(&vol_lock);
        if (m >= 0) {
            char line[16]; snprintf(line, sizeof line, "%d %d\n", m, t);
            if (statefile_write(vol_path(), 0666, line, strlen(line))) fprintf(stderr, "audio: cannot write %s\n", vol_path());
        }
        if (v < 0) continue;
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
static void vol_store(int m, int t)             /* under vol_lock: state/volume on vol_thread */
{
    pthread_once(&vol_once, vol_start);
    vol_save[0] = m; vol_save[1] = t;
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
    if (changed) vol_store(vols[VOL_MAIN] < 0 ? v : vols[VOL_MAIN], vols[VOL_TTS] < 0 ? v : vols[VOL_TTS]);
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

static atomic_int a2dp_out;             /* every player of ours on a Bluetooth speaker (voip_open) */

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
    if (on) { char line[20]; snprintf(line, sizeof line, "%s\n", a); statefile_write(route_path(), 0666, line, strlen(line)); }
    else unlink(route_path());
    atomic_store(&a2dp_out, on);                /* (as the policy confirmed it; audio_a2dp_lost clears it too) */
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
    if (st == 0) atomic_store(&a2dp_out, 0);    /* the policy dropped the speaker: our players are back on the Echo */
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
    s->busy = s->next = s->head = 0; s->written = s->consumed = 0; s->last_cb = now_ms(); s->g = gain_of(s->gain);
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
        gain_glide(slot, p, n, s->bps / 2, &s->g, gain_of(s->gain));
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
/* A fresh player, once it plays, takes AudioTrack's buffer's worth of slots at once (the Echo Show 5: ~137 ms, Sendspin's
 * second chunk timed 135-139 ms early in every stream, 2026-11-04), which s_queued_us leaves out from then on, as a
 * steady latency.  Timed before that, a stream's first chunk was off by it.  150 ms: more than that, and within the
 * slots (PLAY_NBUF x 20 ms), so writing it never waits. */
long long music_prime_us(void) { return 150000; }
long long music_steered_us(void) { return MUSIC_UNSTEERED; }   /* the queue is the output's own: nothing to steer */

/* The Bluetooth speaker stream: what a phone plays to the Echo (a2dp.c), the mixer's Bluetooth stream on the Dots.
 * Here a player of its own beside music, on the music stream like everything of ours.  Until it was there (-1) the
 * phone's audio filled a2dp.c's buffer and was dropped: paired and streaming, nothing heard (2026-10-13). */
int       bt_open(unsigned rate, unsigned channels) { return s_open(&bt, rate, channels, SL_ANDROID_STREAM_MEDIA); }
int       bt_write(const void *data, size_t len) { return s_write(&bt, data, len); }
long long bt_queued_us(void) { return s_queued_us(&bt); }
void      bt_close(void) { s_close(&bt, 0); }
/* a music plugin's (plugin.c): a player of its own on the music stream, as the phone's.  Untried on a Show */
int       plug_open(unsigned rate, unsigned channels) { return s_open(&plug, rate, channels, SL_ANDROID_STREAM_MEDIA); }
int       plug_write(const void *data, size_t len) { return s_write(&plug, data, len); }
long long plug_queued_us(void) { return s_queued_us(&plug); }
void      plug_close(void) { s_close(&plug, 0); }

/* --- a call as stock's calls have it (2026-11-26, static: docs/re-checkers.md "Calls").  The calling app of every
 * Android Echo (com.amazon.comms.multimodaltachyonarm, headlesstachyon on the Echo Input: AudioController, WebRTC's
 * voiceengine with OpenSL ES disallowed) puts Android into MODE_IN_COMMUNICATION for a call (setMode(3), the earlier
 * mode back after it), records AudioRecord source VOICE_COMMUNICATION (7) and plays on usage VOICE_COMMUNICATION (2).
 * The HAL (getAspPipelineType) gives source 7 libasp's pipeline 1, "Voice" in AFE.cfg: the call path of the Dots'
 * micHfp (echo canceller, residual echo suppressor, noise reduction, AGC), on all five models; VOICE_RECOGNITION and
 * HOTWORD get 0, "ASR".  Taken over here wherever the board has it (call_mic):
 *  - the mode through media.audio_policy's setPhoneState (transaction 3 on Android 7, 4 on 9: each firmware's
 *    BnAudioPolicyService::onTransact, the case that calls the slot AudioSystem::setPhoneState does): AudioService's
 *    setMode wants a binder of the caller's to watch, which a native daemon cannot hand it through the service tool.
 *    Below AudioService, as audio_a2dp_route is; set back as the call's stream closes, and at the next start if a run
 *    ended inside a call (state/call-mode);
 *  - the call's player on the music stream, not stock's voice stream (voip_open says why: its volume curve);
 *  - the recorder on VOICE_COMMUNICATION beside the assistant's, as stock's: Fire OS's policy
 *    (startInput_fosExt_allowMultipleInputStreams) lets a second recorder start only where every one running is
 *    HOTWORD (1999), 2999 or 3000, or the new one is, and stock's wake word records HOTWORD.  So does the assistant's
 *    here (cap_open: the firmware's AudioRecord, or Android 9's preset 6).
 *    Where HOTWORD cannot be had the one recorder changes source for the call and its blocks go to both: the wake
 *    word and a request during the call hear the call's path then. */
static int phone_state(int mode);
static int voip_moded;                  /* voip_open set the mode (mode_mu) */
/* The phone state is set from the playout thread (voip_open/_close) and settled from mode_watch's: one at a time,
 * so a late settle never sends MODE_NORMAL into a call that has just set MODE_IN_COMMUNICATION */
static pthread_mutex_t mode_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_int mode_settled; static atomic_llong mode_tried;
static void mode_back(void)             /* mode_mu held: MODE_NORMAL after a call; failed, mode_settle_once tries again */
{
    if (phone_state(0) != 0) { atomic_store(&mode_settled, 0); atomic_store(&mode_tried, 0); }
    voip_moded = 0;
}

static char mode_path[256];
static pthread_once_t mode_path_once = PTHREAD_ONCE_INIT;
static void mode_path_init(void) { const char *d = getenv("HASSMIC_STATE"); snprintf(mode_path, sizeof mode_path, "%s/call-mode", d ? d : "/data/local/hassmic/state"); }
static const char *mode_mark(void) { pthread_once(&mode_path_once, mode_path_init); return mode_path; }   /* (two threads ask) */

static int phone_state(int mode)        /* AUDIO_MODE_NORMAL 0, AUDIO_MODE_IN_COMMUNICATION 3; the status, -1000 none */
{
    struct afe_call c; char out[256]; uint32_t w[2];
    /* the mark before the mode (a run that dies in between still finds it), gone again if the mode did not take */
    if (mode && statefile_write(mode_mark(), 0666, "3\n", 2)) fprintf(stderr, "audio: cannot write %s\n", mode_mark());
    tx_init();
    svc_call_start(&c, "media.audio_policy", a9 ? "4" : "3");
    if (svc_i32(&c, mode)) { if (mode) unlink(mode_mark()); return -1000; }   /* (the mark only for a mode that took) */
    core_run(c.argv, out, sizeof out);
    int r = svc_reply_words(out, w, 2) == 1 ? (int)w[0] : -1000;
    /* the mark goes only once Android is back in MODE_NORMAL: else the next start (mode_settle) tries again */
    /* the mark gone where it is known: MODE_NORMAL taken, or MODE_IN_COMMUNICATION refused with a status.  A reply that
     * did not read (-1000) may have set it: kept, and taken for set (voip_open), so a reset follows */
    if (mode ? r != 0 && r != -1000 : r == 0) unlink(mode_mark());
    fprintf(stderr, "audio: phone state %s (%d)\n", mode ? "MODE_IN_COMMUNICATION" : "MODE_NORMAL", r);
    return r;
}

/* A run that ended inside a call, or a reset that failed: MODE_NORMAL again, from mode_watch's thread, 10 s apart and
 * then doubling up to 10 min until it took (audioserver may not answer yet at the start) */
static atomic_int mode_wait = 10000;     /* between tries: doubled after each failure, up to 10 min */
static void mode_settle_once(void)
{
    if (atomic_load(&mode_settled)) return;
    long long now = now_ms();
    if (atomic_load(&mode_tried) && now - atomic_load(&mode_tried) < atomic_load(&mode_wait)) return;
    pthread_mutex_lock(&mode_mu);
    /* only where the mark says a call of ours left it (another app's call keeps its mode); a factory reset, which
     * empties state/, sets it back itself first (root's main.sh) */
    if (!voip_moded && !atomic_load(&mode_settled)) {          /* (a call of ours holds the mode: its end sets it back) */
        atomic_store(&mode_tried, now);
        if (access(mode_mark(), F_OK)) atomic_store(&mode_settled, 1);
        else {
            if (atomic_load(&mode_wait) == 10000) fprintf(stderr, "audio: call mode left on (a run that ended in a call, or a failed reset)\n");
            if (phone_state(0) == 0) { atomic_store(&mode_settled, 1); atomic_store(&mode_wait, 10000); }
            else if (atomic_load(&mode_wait) < 600000) atomic_store(&mode_wait, atomic_load(&mode_wait) * 2);
        }
    }
    pthread_mutex_unlock(&mode_mu);
}

/* a thread of its own: the service tool is a process (~0.1 s), and the capture thread must never wait (issue #77) */
static void *mode_watch(void *arg) { (void)arg; for (;;) { mode_settle_once(); sleep(1); } return NULL; }
static pthread_once_t mode_watch_once = PTHREAD_ONCE_INIT;
static void mode_watch_go(void) { pthread_t t; if (!pthread_create(&t, NULL, mode_watch, NULL)) pthread_detach(t); }
static void mode_watch_start(void) { pthread_once(&mode_watch_once, mode_watch_go); }

/* the call's audio the stock way: wherever this model has Amazon's call processing */
static int call_stock(void) { return callmic_present(); }

int voip_open(unsigned rate, unsigned channels)
{
    /* Not while the Echo plays on a Bluetooth speaker (audio_a2dp_route): in a call AOSP's policy never routes the
     * phone strategy to A2DP, and the call would leave the speaker (on the Echo Input, which has none of its own, for a
     * line out with nothing in it).  The media stream follows the speaker; the call's recorder stays Amazon's.  Chosen
     * as the call's stream opens and kept for the call: a speaker that comes or goes during it moves nothing */
    int stock = call_stock() && !atomic_load(&a2dp_out);
    pthread_mutex_lock(&mode_mu);
    if (stock && !voip_moded) { int st = phone_state(3); if (st == 0 || st == -1000) voip_moded = 1; }
    else if (!stock && voip_moded) mode_back();
    pthread_mutex_unlock(&mode_mu);
    /* The music stream, not stock's voice stream: Fire OS gives both one index, but not one curve (the Show 5's
     * "dumpsys media.audio_policy", 2026-11-26: speaker VOICE_CALL -24..0 dB over the index, MUSIC -58..0), so at a low
     * volume a call on the voice stream played far louder than the volume set, and board.volume_curve, measured on the
     * music stream, no longer said what it plays at.  The echo reference is libasp's Playback pipeline, every stream's
     * (asp_open: pipeline 3 whatever the stream), and the call path is the recorder's source: neither needs it */
    int r = s_open(&voip, rate, channels, SL_ANDROID_STREAM_MEDIA);
    /* no player: the playout never calls voip_close (call.c), so the mode is set back here */
    if (r < 0) { pthread_mutex_lock(&mode_mu); if (voip_moded) mode_back(); pthread_mutex_unlock(&mode_mu); }
    return r;
}
long long voip_queued_us(void) { return s_queued_us(&voip); }
int       voip_write(const void *data, size_t len) { return s_write(&voip, data, len); }
void      voip_close(void) { s_close(&voip, 0); pthread_mutex_lock(&mode_mu); if (voip_moded) mode_back(); pthread_mutex_unlock(&mode_mu); }

/* The call microphone: what the capture thread reads while the recorder is on VOICE_COMMUNICATION, handed on in
 * 50 ms blocks as the Dots' micHfp comes */
#define CM_BLOCK 800
#define CM_RING  (CAP_RATE)             /* 1 s */
static pthread_mutex_t cm_lock = PTHREAD_MUTEX_INITIALIZER;
/* on the monotonic clock: root sets the wall clock when Home Assistant's time comes, and a wait across a step back
 * would outlast the call */
static pthread_cond_t cm_cond;
static pthread_once_t cm_once = PTHREAD_ONCE_INIT;
static void cm_init(void)
{
    pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&cm_cond, &a); pthread_condattr_destroy(&a);
}
static void cm_deadline(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000; ts->tv_nsec += (ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}
static short cm_ring[CM_RING]; static unsigned cm_w, cm_r; static int cm_on;

static void cm_feed(const short *b, int n)
{
    pthread_mutex_lock(&cm_lock);
    if (cm_on) {
        for (int i = 0; i < n; i++) cm_ring[cm_w++ % CM_RING] = b[i];
        if (cm_w - cm_r > CM_RING) cm_r = cm_w - CM_RING;          /* a reader that fell behind: the newest second */
        pthread_cond_signal(&cm_cond);
    }
    pthread_mutex_unlock(&cm_lock);
}

int callmic_present(void) { return board.call_mic; }
float callmic_echo_cap(void) { return board.call_echo_cap_db; }

/* Beside a HOTWORD capture, as stock's: a recorder of its own on VOICE_COMMUNICATION, its blocks into the ring from
 * its buffer queue's callback (each queued again at once).  Else (VOICE_RECOGNITION: the policy would refuse a second)
 * the one recorder changes source for the call and cap_read hands its blocks on (cm_feed) */
#define CREC_NBUF 8
static SLObjectItf crec_obj; static SLRecordItf crec; static SLAndroidSimpleBufferQueueItf crec_q;
static short crec_buf[CREC_NBUF][CAP_BLOCK]; static unsigned crec_k;

static void crec_cb(SLAndroidSimpleBufferQueueItf q, void *ctx)
{
    (void)ctx;
    short *b = crec_buf[crec_k++ % CREC_NBUF];
    pthread_mutex_lock(&cm_lock);
    if (cm_on) { for (int i = 0; i < CAP_BLOCK; i++) cm_ring[cm_w++ % CM_RING] = b[i]; if (cm_w - cm_r > CM_RING) cm_r = cm_w - CM_RING; pthread_cond_signal(&cm_cond); }
    pthread_mutex_unlock(&cm_lock);
    (*q)->Enqueue(q, b, CAP_BLOCK * sizeof(short));
}

static void crec_close(void)
{
    if (!crec_obj) return;
    (*crec)->SetRecordState(crec, SL_RECORDSTATE_STOPPED);
    (*crec_obj)->Destroy(crec_obj);
    crec_obj = NULL; crec = NULL; crec_q = NULL;
}

static int crec_open(void)
{
    SLDataLocator_IODevice dev = { SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT, SL_DEFAULTDEVICEID_AUDIOINPUT, NULL };
    SLDataSource src = { &dev, NULL };
    SLDataLocator_AndroidSimpleBufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, CREC_NBUF };
    SLDataFormat_PCM fmt = { SL_DATAFORMAT_PCM, 1, CAP_RATE * 1000, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                             SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN };
    SLDataSink sink = { &loc, &fmt };
    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
    const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
    SLAndroidConfigurationItf cfg; SLuint32 preset = SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION;
    if (!engine_ok() || (*engine)->CreateAudioRecorder(engine, &crec_obj, &src, &sink, 2, ids, req) != SL_RESULT_SUCCESS) { crec_obj = NULL; return -1; }
    /* the preset must take: a recorder on the default one is no call pipeline (call.c sends it without the gain) */
    if ((*crec_obj)->GetInterface(crec_obj, SL_IID_ANDROIDCONFIGURATION, &cfg) != SL_RESULT_SUCCESS ||
        (*cfg)->SetConfiguration(cfg, SL_ANDROID_KEY_RECORDING_PRESET, &preset, sizeof preset) != SL_RESULT_SUCCESS ||
        (*crec_obj)->Realize(crec_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
        (*crec_obj)->GetInterface(crec_obj, SL_IID_RECORD, &crec) != SL_RESULT_SUCCESS ||
        (*crec_obj)->GetInterface(crec_obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &crec_q) != SL_RESULT_SUCCESS ||
        (*crec_q)->RegisterCallback(crec_q, crec_cb, NULL) != SL_RESULT_SUCCESS) {
        (*crec_obj)->Destroy(crec_obj); crec_obj = NULL; return -1;
    }
    crec_k = 0;
    for (int i = 0; i < CREC_NBUF; i++) (*crec_q)->Enqueue(crec_q, crec_buf[i], sizeof crec_buf[i]);
    if ((*crec)->SetRecordState(crec, SL_RECORDSTATE_RECORDING) != SL_RESULT_SUCCESS) { crec_close(); return -1; }
    return 0;
}


/* the call's recorder is closed by its owner (callmic_close) or by the capture thread giving up on it: one at a time */
static pthread_mutex_t crec_mu = PTHREAD_MUTEX_INITIALIZER;

/* the capture thread (cap_open): HOTWORD keeps being refused beside the call's recorder.  As where HOTWORD is not to be
 * had: the call's recorder goes and the one recorder switches to VOICE_COMMUNICATION, its blocks to both (cm_feed);
 * callmic_read goes on reading the ring */
static void beside_giveup(void)
{
    pthread_mutex_lock(&crec_mu);
    if (atomic_load(&cm_beside)) {
        atomic_store(&cap_want, SRC_CALL);              /* first: callmic_read's cm_live stays true throughout */
        atomic_store(&cm_beside, 0);
        crec_close();
        fprintf(stderr, "audio: HOTWORD refused %d times beside the call's recorder: one recorder on VOICE_COMMUNICATION for the call\n", HOT_TRIES);
    }
    pthread_mutex_unlock(&crec_mu);
    atomic_store(&beside_fails, 0);
}

int callmic_open(void)
{
    if (!board.call_mic) return -1;
    pthread_once(&cm_once, cm_init);
    pthread_mutex_lock(&cm_lock); cm_on = 1; cm_w = cm_r = 0; pthread_mutex_unlock(&cm_lock);
    /* the assistant's to HOTWORD for the call (the capture thread switches at its next read), then the call's beside */
    atomic_store(&hot_for_call, 1);
    for (int i = 0; i < 100 && want_hot() && atomic_load(&cap_now) != SRC_HOT; i++) usleep(20000);
    if (atomic_load(&cap_now) == SRC_HOT) {
        if (crec_open() == 0) {
            /* a recorder the policy refused starts and delivers nothing: wait for its first block */
            for (int i = 0; i < 50; i++) { pthread_mutex_lock(&cm_lock); unsigned w = cm_w; pthread_mutex_unlock(&cm_lock); if (w) break; usleep(20000); }
            pthread_mutex_lock(&cm_lock); unsigned w = cm_w; pthread_mutex_unlock(&cm_lock);
            if (w) { atomic_store(&beside_fails, 0); atomic_store(&cm_beside, 1); fprintf(stderr, "audio: call capture VOICE_COMMUNICATION beside HOTWORD\n"); return 0; }
            crec_close();
        }
        fprintf(stderr, "audio: no VOICE_COMMUNICATION capture beside HOTWORD: the one capture switches for the call\n");
    }
    atomic_store(&cap_want, SRC_CALL);
    for (int i = 0; i < 100 && atomic_load(&cap_now) != SRC_CALL && atomic_load(&cap_want) == SRC_CALL; i++) usleep(20000);
    if (atomic_load(&cap_now) != SRC_CALL) {         /* refused (cap_read went back), or the capture thread is not reading */
        callmic_close();
        /* the call goes on micAsr (call.c): not in a call mode nobody measured that with; back with voip_close's rule */
        pthread_mutex_lock(&mode_mu); if (voip_moded) mode_back(); pthread_mutex_unlock(&mode_mu);
        return -1;
    }
    return 0;
}

static int cm_live(void) { return cm_on && (atomic_load(&cm_beside) || atomic_load(&cap_want) == SRC_CALL); }

int callmic_read(const void **data)
{
    static short out[CM_BLOCK];
    pthread_once(&cm_once, cm_init);
    struct timespec ts; cm_deadline(&ts, 1500);
    int rc = 0;
    pthread_mutex_lock(&cm_lock);
    while (cm_live() && cm_w - cm_r < CM_BLOCK && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&cm_cond, &cm_lock, &ts);
    if (!cm_live()) { pthread_mutex_unlock(&cm_lock); return -1; }   /* the source went back */
    if (cm_w - cm_r < CM_BLOCK) { pthread_mutex_unlock(&cm_lock); return 0; }
    for (int i = 0; i < CM_BLOCK; i++) out[i] = cm_ring[cm_r++ % CM_RING];
    pthread_mutex_unlock(&cm_lock);
    *data = out;
    return (int)sizeof out;
}

void callmic_close(void)
{
    pthread_mutex_lock(&cm_lock); cm_on = 0; pthread_cond_broadcast(&cm_cond); pthread_mutex_unlock(&cm_lock);
    pthread_mutex_lock(&crec_mu);
    if (atomic_exchange(&cm_beside, 0)) crec_close();
    pthread_mutex_unlock(&crec_mu);
    atomic_store(&hot_for_call, 0);                     /* the assistant's back on VOICE_RECOGNITION */
    atomic_store(&cap_want, SRC_ASR);
}

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
 * never heard (2026-10-14; the 0.5 s mute sounds were, clipped).  A cold earcon starts after board.cold_lead_ms of
 * silence: the Show 5 loses nothing without it (tools/cold-lead.py, 2026-11-07), so it is the model's own.  Without
 * a lead-in the open's own time lands in earsup.c's echo delay, which it searches up to D_MAX (400 ms): the Show 5's
 * open adds ~56 ms (createTrack to the HAL's first write), its echo ~140 ms after audio_earcon_started. */
#define COLD_MS 2500
#define LEAD_MAX_MS 500

static unsigned cold_lead_ms(void)
{
    int ms = board.cold_lead_ms ? board.cold_lead_ms : 350;
    return ms < 0 ? 0 : ms > LEAD_MAX_MS ? LEAD_MAX_MS : (unsigned)ms;
}

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
    unsigned want = cold_lead_ms();
    if (want && now_ms() - atomic_load(&last_out) > COLD_MS) {
        static short lead[48000 * LEAD_MAX_MS / 1000];
        size_t n = (size_t)rate * want / 1000 < sizeof lead / 2 ? (size_t)rate * want / 1000 : sizeof lead / 2;
        if (s_write(&earcon, lead, n * 2) == 0) lead_ms = (unsigned)(n * 1000 / rate);
        end += want;
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

int audio_sco_native(void) { return 1; }
