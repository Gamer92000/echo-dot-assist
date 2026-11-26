/*
 * A call's audio: call.h says what each side does.  Taken out of dropin.c as it was tuned there (PLAN.md, 2026-10-08 and
 * on), so that the call's microphone below can live beside it.
 *
 * Mic: micAsr, what the pipeline gets.  Playback on the mixer's Voip stream, which puts the front end in its call mode
 * (audio.h).  The front end's listening mode only while this side talks (main.c core_call_listen says why): its level
 * NEAR_DB over the room's floor and, while the far end plays, over the echo to expect; on with the first such frame,
 * off LISTEN_HANG_MS after the last.  What the echo canceller leaves of the far end would come back to the other side
 * as an echo, so an echo suppressor here ducks the mic by SUPPRESS_DB while the far end plays, unless this side is
 * clearly louder than the echo has been for DOUBLE_TALK_N frames in a row (double talk passes): the echo's level against
 * what we play is held at its peak while the far end talks, falling 2 dB/s.  A percentile of it (the first try) let the
 * echo's onsets through whenever the far end started again after a pause, and took them for this side talking: listening
 * mode then froze the canceller and more came through.  Our own sounds and replies are ducked the same way.  Then
 * the pipeline's gain (micgain.c) goes first, learning the talker only while the far end is quiet: micAsr runs some
 * 30 dB under speech level, and a gain that took the echo's onsets for a talker lifted them by up to 27 dB (2026-10-08).
 *
 * Codec: Opus (the firmware's libopus), 16 kHz mono, 20 ms, inband FEC.  Loss: Opus's FEC from the next packet, its
 * concealment for PLC_FRAMES.  Or none (CALL_CODEC_PCM, a plugin's call, docs/plugins.md 4.8): the same frames after the
 * gate and the gain go out as they are, s16le, and come in as such; a frame missing is the last one played again,
 * faded (PCM_FADE a frame, for PLC_FRAMES), where Opus conceals by itself.  G.711, its filter, RTP and SIP are the
 * plugin's; gate, echo suppressor, call microphone, listening mode and the Voip stream stay these.
 *
 * The call's own microphone (audio.h callmic_*), always where the model has one: on the Dots the mixer's micHfp carries Amazon's call
 * pipeline once the front end is in call mode, which our Voip stream puts it in.  It already has an echo canceller and
 * residual echo suppressor of its own, noise reduction and an AGC (speech at -20 dBFS against -43 in its pauses, where
 * micAsr had -48 and -65, 2026-11-13, donut: the same take on all streams), so micgain stays out of it and the gate
 * here only ducks by SUPPRESS_HFP_DB.  It cuts most of what is over 4 kHz.  callmic_thread reads it and call_mic
 * (micAsr) stands aside meanwhile; no stream, or one that stops delivering: back to micAsr for the rest of the call.
 * The mixer's read waits up to 1.5 s for a block that does not come (audio_mixer.c), and the far end ends a call after
 * LOST_MS (dropin.c, 5 s) without audio, so the capture thread does not wait for callmic_thread to notice: micHfp
 * HFP_STALE_MS without a block, and micAsr goes out until micHfp is back or given up (3 empty reads).
 */
#include "call.h"
#include "audio.h"
#include "core.h"
#include "micgain.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* libopus: linked from the stock firmware on the device (every model has the encoder), from the system on the PC */
typedef struct OpusEncoder OpusEncoder;
typedef struct OpusDecoder OpusDecoder;
OpusEncoder *opus_encoder_create(int32_t Fs, int channels, int application, int *error);
int32_t opus_encode(OpusEncoder *st, const int16_t *pcm, int frame_size, unsigned char *data, int32_t max_data_bytes);
int opus_encoder_ctl(OpusEncoder *st, int request, ...);
void opus_encoder_destroy(OpusEncoder *st);
OpusDecoder *opus_decoder_create(int32_t Fs, int channels, int *error);
int opus_decode(OpusDecoder *st, const unsigned char *data, int32_t len, int16_t *pcm, int frame_size, int decode_fec);
void opus_decoder_destroy(OpusDecoder *st);
enum { OPUS_APPLICATION_VOIP = 2048, OPUS_SET_BITRATE = 4002, OPUS_SET_COMPLEXITY = 4010, OPUS_SET_INBAND_FEC = 4012,
       OPUS_SET_PACKET_LOSS_PERC = 4014, OPUS_SET_SIGNAL = 4024, OPUS_SIGNAL_VOICE = 3001 };

#define RATE        CALL_RATE
#define FRAME       CALL_FRAME
#define MAXPKT      CALL_PKT
#define BITRATE     24000               /* wideband speech; FEC rides inside it */
#define LOSS_PERC   10                  /* what the encoder plans FEC for: Wi-Fi, no retries on a busy channel */
#define JB          32                  /* jitter buffer slots (frames) */
/* Frames held before playing.  micAsr comes in 50 ms blocks, so frames leave the other Echo two or three at a time
 * every 50 ms: three frames held (the first try) let the playout catch up with every burst and run a frame ahead of
 * arrivals for the rest of the call (2026-10-08, biscuit: 639 of 802 frames came after their turn, longest gap 64 ms).
 * Four from the oldest, and an underrun stretches (concealment without moving on) until the depth suits the link */
#define PREBUF      4
#define MAXBUF      12                  /* more than this waiting (clock drift, a stall): skip ahead */
#define QUEUE_US    40000               /* what may wait in the mixer's ring */
#define PLC_FRAMES  5                   /* loss concealment for this long, silence after */
#define REF_N       64                  /* played frames remembered for the echo suppressor */
/* How long what we play stays in the mic stream, from when our queue says it plays: the mixer's own latency, speaker,
 * room, canceller.  300 ms (the first guess) ended before the echo's tail did: the tail then counted as this side
 * talking and held the gate open through the other side's pauses (2026-10-08, biscuit, natural speech) */
#define ECHO_MS     600
#define FAR_DB      -55.0f              /* the far end talks */
#define SUPPRESS_DB -25.0f              /* the room, and the echo, while this side does not talk */
#define SUPPRESS_OWN_DB -40.0f          /* our own sounds (the connect chime came back at -50 dBFS ducked by 25) */
#define DOUBLE_TALK_DB 6.0f             /* the near end this much over the echo estimate passes... */
#define DOUBLE_TALK_N 3                 /* ...for this many frames in a row: an echo's onset is shorter */
#define NEAR_DB     9.0f                /* this side talks: this far over the room's floor */
/* ...and at least this loud in micAsr.  An empty room's floor is -74 dBFS: sounds of -65..-70 passed NEAR_DB, opened the
 * gate and got the speech gain (+24 dB) put on them: the far Echo sent -40..-54 dBFS of amplified room noise, which the
 * other end took for the far end talking (wake word ignored, its listening mode never on: STT heard nothing; trace of a
 * call with an empty master bedroom, 2026-10-15).  Quiet speech in micAsr is -55 and up */
#define NEAR_MIN_DB -58.0f
/* The far end talks when it plays this far over its own floor (what it sent in its pauses: the noise of its room
 * after its gain, -88..-38 dBFS in that call), and over FAR_DB: an older build on the other side still sends that noise */
#define FAR_OVER_DB 25.0f
/* What we play, held after it stops and falling 20 dB/s: the echo's tail outlasts ECHO_MS.  In every pause of the other
 * side a tail 14 dB over the room came 0.3 to 1 s after its last word and passed for this side talking (trace,
 * 2026-10-08).  A reply 0.5 s after the other side stops passes at -48 dBFS in micAsr, a quiet one after a second */
#define FAR_FALL    0.4f
#define LISTEN_HANG_MS 1500
/* The call pipeline's (micHfp): its own residual echo suppressor and noise reduction did most of the work, so ducked
 * less; its AGC puts speech near -20 dBFS and the room near -43 (2026-11-13) */
#define SUPPRESS_HFP_DB -12.0f
#define NEAR_MIN_HFP_DB -45.0f
/* ...and its echo canceller and residual echo suppressor took the far end out already, while its AGC brings this
 * side's voice up to the far end's playback level: speech at -10..-30 dBFS against playback at -6..-10 (a Dot 5 with a
 * phone, 2026-11-26, issue #233).  Measured as micAsr's is, every reply's first frames counted as echo (DOUBLE_TALK_N
 * of them each time), the echo estimate rose to its 0 dB cap within seconds and stayed, and no reply passed as double
 * talk again: what this side said within 1 s of the far end went out at full level 7..36 % of the time, within 2.5 s
 * 68..86 % (the hold falling 20 dB/s).  So on micHfp the estimate stops callmic_echo_cap() under what plays (the
 * board's call_echo_cap_db, -30 where measured: Amazon's canceller left the Dot 2's echo 15 dB under the bare mic's, its
 * RES below the room, PLAN.md), double talk passes
 * after DOUBLE_TALK_N_HFP frames and the hold falls FAR_FALL_HFP a frame */
/* ...not before the far end has played HFP_WARMUP_MS on micHfp, while Amazon's canceller still converges and leaves
 * more of the echo: the estimate learns up to 0 dB then, and is held to the cap after */
#define HFP_WARMUP_MS 2000
#define DOUBLE_TALK_N_HFP 2
#define FAR_FALL_HFP 1.0f
/* micHfp silent this long (its blocks come every 50 ms): micAsr fills in.  A gap this long plays as the far end's loss
 * concealment (PLC_FRAMES) and a moment of silence; the two sources swapping restarts the encoder and the gate */
#define HFP_STALE_MS 250

static pthread_mutex_t ml = PTHREAD_MUTEX_INITIALIZER;   /* everything below; taken after dropin.c's */
static pthread_cond_t live_cond = PTHREAD_COND_INITIALIZER;
static atomic_int running;
static struct {
    int live, gen, codec, who;          /* who: whose call (call.h's claim) */
    void (*send)(const uint8_t *, size_t, uint32_t, void *); void *arg;
    uint32_t tx_seq;
    struct call_stats st;
    long long arr_at;
} m;
static uint32_t play_next;                              /* the playout's next frame, for counting late ones */
/* a frame waiting: Opus's at most MAXPKT, PCM's CALL_PCM_BYTES */
#define SLOT_MAX (MAXPKT > CALL_PCM_BYTES ? MAXPKT : CALL_PCM_BYTES)
/* A frame missing from a PCM call: the last one again at this much of its level (-6 dB a frame), PLC_FRAMES times,
 * then silence.  Repeated at full level a 20 ms frame is a 50 Hz buzz; faded it is a short tail of the last sound */
#define PCM_FADE    0.5f
static struct slot { uint32_t seq; int len; uint8_t d[SLOT_MAX]; } jb[JB];
static uint32_t jb_hi, jb_lo; static int jb_any;      /* the newest frame in, the first of the call */
/* A plugin's call only: a new numbering.  Frames more than JUMP away from the newest, either way, RESYNC_N of them in a
 * row each following the one before (within JUMP_STEP), start the jitter buffer anew from there.  One stray number far
 * ahead became the newest, and every frame after it was "far too old": silence for the rest of the call; a numbering
 * that starts again (a plugin's 16-bit RTP numbers wrap after 22 minutes) the same (W3 of a review of fc43c40).  A
 * single stray frame is dropped; the frames of the new numbering before the third are lost (40 ms), and those of the
 * old one still waiting (~100 ms).  sdk/PROTOCOL.md says the rule.
 * Never Drop In's: its numbers are hassmic's own from 0 (32 bits, years), and a LAN host that replays packets it
 * captured passes the AEAD (the nonce is the call id, direction and number), so three old frames in a row started the
 * buffer anew from them and kept the far side's own frames out for the rest of the call: the Echo played its earlier
 * words again (D1 of a review of e8d52b9).  There a frame JB behind the newest is dropped, as before W3, and dropin.c
 * drops replays before they come here */
#define JUMP        (2 * JB)            /* 1.28 s: past any jitter this buffer holds */
#define JUMP_STEP   8
#define RESYNC_N    3
static uint32_t jump_last; static int jump_n;           /* the run of far frames so far */
static unsigned jb_epoch;                                /* one more at each new start: the playout starts again */
/* The line that says so: a plugin that alternates between two numberings resyncs every third packet, ~33 a second, and
 * one line each beat its own 50 lines a minute and wiped the 2 MB log in ~11 minutes (D2 of a review of e8d52b9).  One
 * line at once, then at most one per RESYNC_LOG_MS with the count since, and the call's count at its end */
#define RESYNC_LOG_MS 10000
static long long resync_said; static unsigned resync_unsaid, resync_calls;
static struct { long long t; float db; } ref[REF_N];
static unsigned ref_n;
static float far_floor;                 /* the quietest the far end has played lately (ml): drops at once, rises 1.5 dB/s */
static atomic_int src_hfp;              /* the call running now sends micHfp (callmic_thread), not micAsr */
static atomic_int voip_gen = -1;        /* the call whose Voip stream is open (call mode on): the playout's */
static atomic_llong hfp_at;             /* when callmic_thread last had a block of micHfp (now_ms) */
/* What went to the other side, for the line at the call's end (issue #233: "quiet" from across the room, and the log
 * said nothing of the microphone).  The sender's, read by call_end: a lock of its own, a leaf (call_end runs under
 * dropin.c's dl and the sender's send takes it: tx_mu must not be taken there) */
static pthread_mutex_t sl = PTHREAD_MUTEX_INITIALIZER;
static struct txstat {
    int gen; unsigned n, hfp, own, near, open, far;     /* frames: all, of micHfp, our own sounds, this side talked,
                                                           sent unducked, the far end talked */
    double near_pow, sent_pow;                          /* this side's frames as they came in and as they went out */
    float gain;                                         /* micgain's at the end (micAsr) */
} tx;

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static float level_db(const int16_t *s, size_t n)
{
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (double)s[i] * s[i];
    return (float)(10 * log10(sum / (n ? n : 1) / (32768.0 * 32768.0) + 1e-10));
}

/* ---------------------------------------------------------------- the call */

/* The claim (call.h): an atomic, so that dropin.c takes it under dl and plugin.c under its own without an order between
 * them, and the core reads it from any thread */
static atomic_int claimed;

int call_claim(int who)
{
    int none = CALL_NOBODY;
    return atomic_compare_exchange_strong(&claimed, &none, who) || none == who ? 0 : -1;
}

void call_release(int who)
{
    int mine = who;
    atomic_compare_exchange_strong(&claimed, &mine, CALL_NOBODY);
}

int call_claimed(void) { return atomic_load(&claimed); }



static int call_gen_is(int gen)
{
    pthread_mutex_lock(&ml);
    int r = m.live && m.gen == gen;
    pthread_mutex_unlock(&ml);
    return r;
}

int call_begin(int who, int codec, void (*send)(const uint8_t *, size_t, uint32_t, void *), void *arg)
{
    if (who == CALL_NOBODY || call_claimed() != who || (codec != CALL_CODEC_OPUS && codec != CALL_CODEC_PCM)) return -1;    /* (media only for the call that has the claim) */
    pthread_mutex_lock(&ml);
    m.gen++; m.live = 1; m.send = send; m.arg = arg; m.tx_seq = 0; m.codec = codec; m.who = who;
    resync_said = 0; resync_unsaid = 0; resync_calls = 0;
    memset(&m.st, 0, sizeof m.st); m.arr_at = 0; play_next = 0;
    memset(jb, 0, sizeof jb); jb_any = 0; jump_n = 0; ref_n = 0; memset(ref, 0, sizeof ref);
    int gen = m.gen;
    pthread_cond_broadcast(&live_cond);
    pthread_mutex_unlock(&ml);
    return gen;
}

static float pow_db(double p, unsigned n) { return n ? (float)(10 * log10(p / n + 1e-10)) : -100; }

void call_end(int gen)
{
    unsigned resyncs = 0; int was = 0;
    pthread_mutex_lock(&ml);
    if (m.live && m.gen == gen) { m.live = 0; m.gen++; m.send = NULL; pthread_cond_broadcast(&live_cond); resyncs = resync_calls; was = 1; }
    pthread_mutex_unlock(&ml);
    if (resyncs > 1) fprintf(stderr, "call: the far side's frames were numbered anew %u times in this call\n", resyncs);
    if (!was) return;
    /* What the other side got: of which microphone, how much of it this side's voice (by the gate: NEAR_DB over the
     * room and NEAR_MIN_DB), at what level as it came in and as it went out, and how much went out unducked */
    pthread_mutex_lock(&sl);
    struct txstat t = tx;
    pthread_mutex_unlock(&sl);
    if (t.gen != gen || !t.n) return;
    char g[32] = "", lv[64] = "";
    if (t.hfp < t.n) snprintf(g, sizeof g, ", gain %+.1f dB", t.gain);
    if (t.near) snprintf(lv, sizeof lv, " (%.1f dBFS in, %.1f out%s)", pow_db(t.near_pow, t.near), pow_db(t.sent_pow, t.near), g);
    fprintf(stderr, "call: sent %u frames (%u s), %u%% of micHfp; this side talked in %u%%%s, the other side in %u%%; "
            "sent unducked %u%%\n", t.n, t.n / 50, t.hfp * 100 / t.n, t.near * 100 / t.n, lv, t.far * 100 / t.n, t.open * 100 / t.n);
}

int call_live(void)
{
    pthread_mutex_lock(&ml);
    int r = m.live;
    pthread_mutex_unlock(&ml);
    return r;
}

void call_stats(struct call_stats *s)
{
    pthread_mutex_lock(&ml);
    *s = m.st;
    pthread_mutex_unlock(&ml);
}

void call_rx(int gen, uint32_t seq, const uint8_t *frame, size_t len)
{
    if (!len || len > SLOT_MAX) return;
    pthread_mutex_lock(&ml);
    if (!m.live || m.gen != gen) { pthread_mutex_unlock(&ml); return; }
    if (m.codec == CALL_CODEC_PCM ? len != CALL_PCM_BYTES : len > MAXPKT) { pthread_mutex_unlock(&ml); return; }     /* not a frame of this media */
    int32_t d = (int32_t)(seq - jb_hi);
    if (jb_any && m.who >= CALL_PLUGIN && (d > JUMP || d < -JUMP)) {     /* a plugin's, far away: a stray, or a new numbering (JUMP above) */
        int32_t step = (int32_t)(seq - jump_last);
        jump_n = jump_n && step > 0 && step <= JUMP_STEP ? jump_n + 1 : 1; jump_last = seq;
        if (jump_n < RESYNC_N) { pthread_mutex_unlock(&ml); return; }
        uint32_t hi = jb_hi; long long t = now_ms();
        memset(jb, 0, sizeof jb); jb_any = 0; jump_n = 0; jb_epoch++; play_next = seq;     /* from this frame on, as at a call's start */
        resync_calls++;
        if (!resync_said || t - resync_said >= RESYNC_LOG_MS) {
            if (resync_unsaid) fprintf(stderr, "call: the far side's frames numbered anew (%u after %u), %u more times since the last line: the jitter buffer starts again\n", seq, hi, resync_unsaid);
            else fprintf(stderr, "call: the far side's frames numbered anew (%u after %u): the jitter buffer starts again\n", seq, hi);
            resync_said = t; resync_unsaid = 0;
        } else resync_unsaid++;
    } else {
        jump_n = 0;
        /* far too old: its slot is a newer frame's.  JB behind is the newest's own slot (seq % JB): "<" let a frame
         * that late overwrite the newest (W4 of a review of fc43c40) */
        if (jb_any && d <= -JB) { pthread_mutex_unlock(&ml); return; }
    }
    struct slot *s = &jb[seq % JB];
    memcpy(s->d, frame, len); s->seq = seq; s->len = (int)len;
    if (!jb_any || (int32_t)(seq - jb_hi) > 0) jb_hi = seq;
    if (!jb_any || (int32_t)(seq - jb_lo) < 0) jb_lo = seq;
    long long now = now_ms();
    if (m.arr_at && now - m.arr_at > m.st.gap_max) m.st.gap_max = now - m.arr_at;
    if (jb_any && (int32_t)(seq - play_next) < 0) m.st.late++;
    jb_any = 1; m.st.rx++; m.arr_at = now;
    pthread_mutex_unlock(&ml);
}

/* ---------------------------------------------------------------- audio out: the mic */

/* The echo suppressor's and the gain's state, and the encoder: the sending thread's (the capture thread's, or
 * callmic_thread's), under tx_mu for the moment the two hand over */
static pthread_mutex_t tx_mu = PTHREAD_MUTEX_INITIALIZER;
static OpusEncoder *enc;
static int enc_gen = -1, enc_hfp = -1;    /* the media and the source the encoder, the gain and the suppressor are of */
static struct micgain gain;
static float es_c, es_g;                /* echo against what we play, dB (peak held); the gain applied, linear */
static int dt_n;                        /* frames in a row over the echo estimate */
static float far_hold;                  /* what we play, held: falls FAR_FALL dB a frame */
static float floor_db;                  /* the room in micAsr */
static long long near_until;            /* this side talked until then (+ LISTEN_HANG_MS) */
static int listen_on;                   /* what we last asked of core_call_listen */
static int hfp_warm;                    /* frames of this call's micHfp while the far end played (HFP_WARMUP_MS of them:
                                           Amazon's canceller converges on the far end's audio, not on the clock) */
static long long listen_at;
static int16_t mic[FRAME]; static size_t mic_n; static int mic_gen = -1;   /* the capture thread's: a frame filling, of which call */

static float far_db(long long now)       /* ml held: the loudest we played that can still be in the mic stream */
{
    float r = -100;
    for (unsigned i = 0; i < REF_N; i++) if (ref[i].t && ref[i].t <= now && now - ref[i].t < ECHO_MS && ref[i].db > r) r = ref[i].db;
    return r;
}

static int far_voice(float db, float floor)     /* a level of the far end that is a voice, not its room's noise */
{
    return db > FAR_DB && db > floor + FAR_OVER_DB;
}

float call_far_level(void)
{
    pthread_mutex_lock(&ml);
    float r = m.live ? far_db(now_ms()) : -100;
    pthread_mutex_unlock(&ml);
    return r;
}

int call_far_talking(void)
{
    pthread_mutex_lock(&ml);
    int r = m.live && far_voice(far_db(now_ms()), far_floor);
    pthread_mutex_unlock(&ml);
    return r;
}

static void send_frame_locked(const int16_t *pcm, int own, int hfp);

static void send_frame(const int16_t *pcm, int own, int hfp)
{
    pthread_mutex_lock(&tx_mu);
    send_frame_locked(pcm, own, hfp);
    pthread_mutex_unlock(&tx_mu);
}

static void send_frame_locked(const int16_t *pcm, int own, int hfp)
{
    uint8_t op[MAXPKT]; int gen, codec; void (*send)(const uint8_t *, size_t, uint32_t, void *); void *arg;
    pthread_mutex_lock(&ml);
    if (!m.live || !m.send) { pthread_mutex_unlock(&ml); return; }
    gen = m.gen; send = m.send; arg = m.arg; codec = m.codec;
    float far = far_db(now_ms()), ffloor = far_floor;
    uint32_t seq = m.tx_seq++;
    pthread_mutex_unlock(&ml);

    /* A new call or another source (micHfp stalled or back): a fresh encoder, gain and suppressor.
     * Listening mode is the front end's, told to the core: kept across a swap, or a side that talked when micHfp stalled
     * left it on for the rest of the call (the core hears of "off" only on a change of want) */
    if (enc_gen != gen) {                                /* the core cleared it as the last call ended */
        near_until = 0; listen_on = 0; listen_at = 0;
        pthread_mutex_lock(&sl); memset(&tx, 0, sizeof tx); tx.gen = gen; pthread_mutex_unlock(&sl);
        /* micHfp says so itself once it opens (callmic_thread), which takes a moment: until then micAsr goes */
        if (!hfp && !callmic_present()) fprintf(stderr, "call: the assistant's microphone (micAsr) through the call's gain and gate\n");
    }
    if (enc_gen != gen || enc_hfp != hfp) {
        int e = 0;
        if (enc) { opus_encoder_destroy(enc); enc = NULL; }
        if (codec == CALL_CODEC_OPUS) {                 /* (PCM: none; the gate and the gain are the same) */
            enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &e);
            if (!enc) { fprintf(stderr, "call: no Opus encoder (%d)\n", e); return; }
            opus_encoder_ctl(enc, OPUS_SET_BITRATE, BITRATE); opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC, 1);
            opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC, LOSS_PERC); opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY, 5);
            opus_encoder_ctl(enc, OPUS_SET_SIGNAL, OPUS_SIGNAL_VOICE);
        }
        micgain_init(&gain, MICGAIN_LEVEL); micgain_start(&gain, 1);
        es_c = -30; es_g = 1; dt_n = 0; far_hold = -100; floor_db = hfp ? -40 : -70;
        if (hfp) hfp_warm = 0;                        /* (a swap back to micHfp after a stall: its warm-up again) */
        enc_gen = gen; enc_hfp = hfp;
    }
    int16_t f[FRAME];
    memcpy(f, pcm, sizeof f);
    /* echo suppression: the far end talks and the mic is not clearly louder than its echo has been */
    float lv = level_db(f, FRAME), target = 1; long long now = now_ms(); int farend, near;
    /* the cap only where it was measured (callmic_echo_cap: the Dots); elsewhere the estimate learns as micAsr's does,
     * which ducks a quick reply's start but never lets an echo the model's canceller leaves through */
    float cap = hfp ? callmic_echo_cap() : 0;
    /* the three go together: a faster fall and a shorter wait for double talk only where the cap was measured too
     * (an unmeasured canceller's tail stays held as micAsr's is) */
    int tuned = cap < 0 && hfp_warm >= HFP_WARMUP_MS / 20;     /* (all three wait for the canceller's warm-up) */
    float fall = tuned ? FAR_FALL_HFP : FAR_FALL, es_max = tuned ? cap : 0;
    if (es_c > es_max) es_c = es_max;
    int dt_need = tuned ? DOUBLE_TALK_N_HFP : DOUBLE_TALK_N;
    far_hold = far > far_hold - fall ? far : far_hold - fall;
    farend = far_voice(far_hold, ffloor);
    if (hfp && farend && hfp_warm < HFP_WARMUP_MS / 20) hfp_warm++;
    int over = farend && lv >= far_hold + es_c + DOUBLE_TALK_DB;
    dt_n = over ? dt_n + 1 : 0;
    if (farend && !own && dt_n < dt_need) {             /* echo, as far as we can tell (not our own sounds): its peak */
        float x = lv - far_hold;
        es_c = x > es_c ? x : es_c - 0.04f;
        if (es_c < -60) es_c = -60;
        if (es_c > es_max) es_c = es_max;
    }
    floor_db = lv < floor_db ? (lv > -90 ? lv : -90) : floor_db + 0.03f;     /* drops at once, rises 1.5 dB/s */
    near = !own && lv > floor_db + NEAR_DB && lv > (hfp ? NEAR_MIN_HFP_DB : NEAR_MIN_DB) && (!farend || dt_n >= dt_need);
    /* A gate: open while this side talks (and LISTEN_HANG_MS after, unless the far end plays), else ducked.  Ducked only
     * while the far end played (the first try), the gain lifted the room in its pauses from -72 to -48 dBFS while the
     * rest went out at -70: hiss that came and went with every sentence of the other side (2026-10-08) */
    if (own) target = powf(10, SUPPRESS_OWN_DB / 20);
    else if (!near && (farend || now >= near_until)) target = powf(10, (hfp ? SUPPRESS_HFP_DB : SUPPRESS_DB) / 20);
    /* the gain first: after the ducking it would give the 25 dB back (+24 dB at micAsr's level, measured).  The call
     * pipeline has its own AGC */
    gain.hold = own || farend;                          /* the talker's level is learnt while the other side is quiet */
    if (!hfp) micgain_run(&gain, f, f, FRAME);
    float step = (target - es_g) / FRAME;              /* within a frame: a word's onset is not cut */
    for (int i = 0; i < FRAME; i++) { if ((step < 0 && es_g > target) || (step > 0 && es_g < target)) es_g += step; f[i] = (int16_t)(f[i] * es_g); }
    if (near) near_until = now + LISTEN_HANG_MS;
    {   float sent = level_db(f, FRAME);
        pthread_mutex_lock(&sl);
        if (tx.gen == gen) {
            tx.n++; tx.hfp += hfp; tx.own += own; tx.far += farend; tx.open += !own && target >= 1;
            if (near) { tx.near++; tx.near_pow += pow(10, lv / 10); tx.sent_pow += pow(10, sent / 10); }
            if (!hfp) tx.gain = gain.gain_db;
        }
        pthread_mutex_unlock(&sl);
    }
    int want = now < near_until;
    if (want != listen_on && now - listen_at >= 250) { listen_on = want; listen_at = now; core_call_listen(want); }
    /* HASSMIC_DROPIN_TRACE=<file>: the gate's inputs and decisions per frame, for tuning on a device (export it in
     * hassmic.conf): ms, mic dB, far dB, echo estimate, floor, this side talks, own sound, double talk run, gate, gain,
     * the call pipeline's microphone (1) or micAsr (0) */
    { static FILE *tr; if (!tr && getenv("HASSMIC_DROPIN_TRACE")) tr = fopen(getenv("HASSMIC_DROPIN_TRACE"), "w");
      if (tr) { fprintf(tr, "%lld %.1f %.1f %.1f %.1f %d %d %d %.2f %.1f %d\n", now, lv, far, es_c, floor_db, near, own, dt_n, target, hfp ? 0.0f : gain.gain_db, hfp); fflush(tr); } }
    /* PCM: the samples as they are in memory, which is s16le on every Echo (ARMv7 little-endian) and on the PC */
    if (codec == CALL_CODEC_PCM) { send((const uint8_t *)f, sizeof f, seq, arg); return; }
    int32_t len = enc ? opus_encode(enc, f, FRAME, op, sizeof op) : -1;
    if (len > 0) send(op, (size_t)len, seq, arg);
}

void call_mic(const int16_t *pcm, size_t n, int own)
{
    int gen;
    pthread_mutex_lock(&ml); gen = m.live ? m.gen : -1; pthread_mutex_unlock(&ml);
    if (!atomic_load(&running) || gen < 0) { mic_n = 0; return; }
    /* what was left of a 20 ms frame belongs to the call it came in: a call that ended without a call_mic after it
     * (the capture thread feeds only while one is live) left its last 10 ms to start the next one with, another
     * party's (W5 of a review of fc43c40) */
    if (gen != mic_gen) { mic_n = 0; mic_gen = gen; }
    if (atomic_load(&src_hfp) && now_ms() - atomic_load(&hfp_at) < HFP_STALE_MS) { mic_n = 0; return; }   /* micHfp goes out */
    while (n) {
        size_t k = FRAME - mic_n < n ? FRAME - mic_n : n;
        memcpy(mic + mic_n, pcm, k * 2); mic_n += k; pcm += k; n -= k;
        if (mic_n == FRAME) { send_frame(mic, own, 0); mic_n = 0; }
    }
}

static void wait_call_over(int gen)
{
    pthread_mutex_lock(&ml);
    while (m.live && m.gen == gen) pthread_cond_wait(&live_cond, &ml);
    pthread_mutex_unlock(&ml);
}

/* The call pipeline's microphone, while a call runs: opened once the playout has the Voip stream open (call mode; micHfp
 * is the bare mic before that), read 50 ms at a time and sent; the capture thread's micAsr stands aside meanwhile */
static void *callmic_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&ml);
        while (!m.live) pthread_cond_wait(&live_cond, &ml);
        int gen = m.gen;
        pthread_mutex_unlock(&ml);
        if (!callmic_present()) { wait_call_over(gen); continue; }
        long long t0 = now_ms();
        while (atomic_load(&voip_gen) != gen && call_gen_is(gen) && now_ms() - t0 < 1500) usleep(10000);
        if (!call_gen_is(gen)) continue;
        if (atomic_load(&voip_gen) != gen || callmic_open()) {
            fprintf(stderr, "call: no call microphone (%s), the assistant's for this call\n", atomic_load(&voip_gen) != gen ? "no call mode" : "micHfp does not open");
            wait_call_over(gen); continue;
        }
        atomic_store(&hfp_at, now_ms());           /* before src_hfp: the capture thread stands aside from now on */
        atomic_store(&src_hfp, 1);
        fprintf(stderr, "call: the call pipeline's microphone (micHfp)\n");
        int16_t f[FRAME]; size_t fn = 0; int empty = 0; const char *why = NULL;
        while (call_gen_is(gen)) {
            /* the Voip stream failed (the playout closed it): call mode off, micHfp the bare mic again on donut, and
             * without our gain.  The playout also clears voip_gen as a call ends: not this, then */
            if (atomic_load(&voip_gen) != gen) { if (call_gen_is(gen)) why = "call mode ended"; break; }
            const void *d; int n = callmic_read(&d);
            if (n < 0) { why = "micHfp is gone"; break; }
            if (n == 0) {                               /* the capture thread's micAsr has filled in since HFP_STALE_MS */
                if (!empty) fprintf(stderr, "call: micHfp stalls, the assistant's microphone meanwhile\n");
                fn = 0;
                if (++empty >= 3) { why = "micHfp delivers nothing"; break; }
                continue;
            }
            if (empty) fprintf(stderr, "call: micHfp is back\n");
            empty = 0; atomic_store(&hfp_at, now_ms());
            const int16_t *p = d; int own = core_own_sound(), muted = core_muted();
            for (int i = 0; i < n / 2; i++) { f[fn++] = muted ? 0 : p[i]; if (fn == FRAME) { send_frame(f, own, 1); fn = 0; } }
        }
        atomic_store(&src_hfp, 0);
        callmic_close();
        if (why) { fprintf(stderr, "call: %s: the assistant's microphone for the rest of this call\n", why); wait_call_over(gen); }
    }
    return NULL;
}

/* ---------------------------------------------------------------- audio in */

/* The playout thread: one frame per 20 ms into the Voip stream, from the jitter buffer, with Opus's FEC and loss
 * concealment for what is missing, silence before the first frame and after a long gap */
static void *playout(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&ml);
        while (!m.live) pthread_cond_wait(&live_cond, &ml);
        int gen = m.gen, pcm_codec = m.codec == CALL_CODEC_PCM; long long t0 = now_ms(); unsigned epoch = jb_epoch;
        far_floor = -70;
        pthread_mutex_unlock(&ml);
        int e = 0, open = voip_open(RATE, 1) == 0, started = 0, missing = 0, skipped = 0, stretched = 0;
        OpusDecoder *dec = pcm_codec ? NULL : opus_decoder_create(RATE, 1, &e);
        int16_t last[FRAME]; float fade = 0;           /* PCM: the frame played last, and its level for a concealment */
        if (!open) fprintf(stderr, "call: cannot open the Voip stream\n");
        else atomic_store(&voip_gen, gen);              /* call mode on: callmic_thread may open micHfp */
        if (!dec && !pcm_codec) fprintf(stderr, "call: no Opus decoder (%d)\n", e);
        memset(last, 0, sizeof last);
        uint32_t next = 0;
        for (;;) {
            uint8_t pkt[SLOT_MAX]; int len = 0, fec = 0; int16_t pcm[FRAME];
            while (open && voip_queued_us() > QUEUE_US) usleep(4000);
            pthread_mutex_lock(&ml);
            if (!m.live || m.gen != gen) { pthread_mutex_unlock(&ml); break; }
            if (epoch != jb_epoch) { epoch = jb_epoch; started = 0; t0 = now_ms(); }    /* numbered anew (call_rx): from its first frame */
            if (!started && jb_any && (jb_hi - jb_lo >= PREBUF - 1 || now_ms() - t0 > 300)) { started = 1; next = jb_lo; }
            if (started && (int32_t)(jb_hi - next) > MAXBUF) { next = jb_hi - (PREBUF - 1); skipped++; }
            if (started) {
                struct slot *s = &jb[next % JB], *s1 = &jb[(next + 1) % JB];
                if (s->len && s->seq == next) { len = s->len; memcpy(pkt, s->d, (size_t)len); }
                else if ((int32_t)(jb_hi - next) <= 0) stretched++;         /* nothing newer yet: wait, concealing */
                else if (!pcm_codec && s1->len && s1->seq == next + 1) { len = s1->len; memcpy(pkt, s1->d, (size_t)len); fec = 1; }   /* (Opus's FEC: PCM has none) */
                if (len || (int32_t)(jb_hi - next) > 0) {                  /* played, or lost for good: on to the next */
                    if (!len || fec) m.st.lost++;
                    s->len = 0;
                    next++; m.st.played++; play_next = next;
                }
            }
            pthread_mutex_unlock(&ml);
            int got = 0;
            if (pcm_codec && len == CALL_PCM_BYTES) { memcpy(pcm, pkt, sizeof pcm); memcpy(last, pcm, sizeof last); fade = 1; got = FRAME; }
            else if (pcm_codec && started && missing < PLC_FRAMES) {
                /* the last frame again, its level falling from where the one before left it to PCM_FADE of that */
                float to = fade * PCM_FADE;
                for (int i = 0; i < FRAME; i++) pcm[i] = (int16_t)(last[i] * (fade + (to - fade) * (float)i / FRAME));
                fade = to; got = FRAME;
            }
            else if (dec && len) got = opus_decode(dec, pkt, len, pcm, FRAME, fec);
            else if (dec && started && missing < PLC_FRAMES) got = opus_decode(dec, NULL, 0, pcm, FRAME, 0);
            missing = len ? 0 : missing + 1;
            if (got != FRAME) memset(pcm, 0, sizeof pcm);
            long long q = open ? voip_queued_us() : 0;
            if (open && voip_write(pcm, sizeof pcm) < 0) {
                fprintf(stderr, "call: the Voip stream failed\n"); voip_close(); open = 0;
                atomic_store(&voip_gen, -1);            /* call mode off: callmic_thread leaves micHfp */
            }
            if (!open) usleep(20000);
            pthread_mutex_lock(&ml);
            /* what plays, for the echo suppressor: nothing once the Voip stream failed, or the far end's voice, no longer
             * heard here, would duck this side's mic for the rest of the call */
            float pdb = open ? level_db(pcm, FRAME) : -100;
            ref[ref_n++ % REF_N].t = now_ms() + q / 1000; ref[(ref_n - 1) % REF_N].db = pdb;   /* when it plays */
            if (open) far_floor = pdb < far_floor ? (pdb > -90 ? pdb : -90) : far_floor + 0.03f;
            pthread_mutex_unlock(&ml);
        }
        if (skipped || stretched) fprintf(stderr, "call: playout skipped ahead %d times (more than %d ms waiting), waited %d times for a late frame\n",
                                          skipped, MAXBUF * 20, stretched);
        atomic_store(&voip_gen, -1);
        if (open) voip_close();
        if (dec) opus_decoder_destroy(dec);
    }
    return NULL;
}

int call_start(void)
{
    pthread_t t;
    if (atomic_exchange(&running, 1)) return 0;
    if (pthread_create(&t, NULL, playout, NULL)) { atomic_store(&running, 0); return -1; }
    pthread_detach(t);
    if (!pthread_create(&t, NULL, callmic_thread, NULL)) pthread_detach(t);
    else fprintf(stderr, "call: no call microphone thread, the assistant's microphone\n");
    return 0;
}
