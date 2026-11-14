/*
 * A call's audio: what Drop In (dropin.c) and the phone (sip.c) share; call.h says what each side does.  Taken out of
 * dropin.c as it was tuned there (PLAN.md, 2026-10-08 and on): the measurements below are Drop In's.
 *
 * Mic: micAsr, what the pipeline gets.  Playback on the mixer's Voip stream, which puts the front end in its call mode
 * (audio.h).  The front end's listening mode only while this side talks (main.c core_dropin_listen says why): its level
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
 * Codecs: Opus (the firmware's libopus), 16 kHz mono, 20 ms, inband FEC; G.711 A-law and mu-law at 8 kHz for phones,
 * the mic halved and what plays doubled by a 63-tap FIR (half_down, half_up: Kaiser, cut at 3.85 kHz, -63 dB from 4.4).
 * Loss: Opus's FEC from the next packet, its concealment for PLC_FRAMES; G.711 the last frame fading, then silence.
 *
 * The call's own microphone (call_mic_source, audio.h callmic_*): on the Dots the mixer's micHfp carries Amazon's call
 * pipeline once the front end is in call mode, which our Voip stream puts it in.  It already has an echo canceller and
 * residual echo suppressor of its own, noise reduction and an AGC (speech at -20 dBFS against -43 in its pauses, where
 * micAsr had -48 and -65, 2026-11-13, donut: the same take on all streams), so micgain stays out of it and the gate
 * here only ducks by SUPPRESS_HFP_DB.  It cuts most of what is over 4 kHz.  callmic_thread reads it and call_mic
 * (micAsr) stands aside meanwhile; no stream, or one that stops delivering: back to micAsr for the rest of the call.
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

static pthread_mutex_t ml = PTHREAD_MUTEX_INITIALIZER;   /* everything below; taken after dropin.c's and sip.c's */
static pthread_cond_t live_cond = PTHREAD_COND_INITIALIZER;
static atomic_int claimed, running;
static struct {
    int live, gen, who, codec;
    void (*send)(const uint8_t *, size_t, uint32_t, void *); void *arg;
    uint32_t tx_seq;
    struct call_stats st;
    long long arr_at;
} m;
static uint32_t play_next;                              /* the playout's next frame, for counting late ones */
static struct slot { uint32_t seq; int len; uint8_t d[MAXPKT]; } jb[JB];
static uint32_t jb_hi, jb_lo; static int jb_any;      /* the newest frame in, the first of the call */
static struct { long long t; float db; } ref[REF_N];
static unsigned ref_n;
static float far_floor;                 /* the quietest the far end has played lately (ml): drops at once, rises 1.5 dB/s */
static atomic_int use_hfp = 1;          /* call_mic_source */
static atomic_int src_hfp;              /* the call running now sends micHfp (callmic_thread), not micAsr */
static atomic_int voip_gen = -1;        /* the call whose Voip stream is open (call mode on): the playout's */

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

/* ---------------------------------------------------------------- G.711 (ITU-T G.711, as Sun's g711.c) */

uint8_t g711_alaw(int16_t pcm)
{
    static const int end[8] = { 0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff };
    int v = pcm >> 3, mask = 0xd5, seg = 0;                 /* 13 bits */
    if (v < 0) { v = -v - 1; mask = 0x55; }
    if (v > 0xfff) v = 0xfff;
    while (v > end[seg]) seg++;
    return (uint8_t)((seg << 4 | ((seg < 2 ? v >> 1 : v >> seg) & 0x0f)) ^ mask);
}

int16_t g711_alaw_dec(uint8_t a)
{
    int t, seg;
    a ^= 0x55;
    t = (a & 0x0f) << 4; seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8; else if (seg == 1) t += 0x108; else { t += 0x108; t <<= seg - 1; }
    return (int16_t)(a & 0x80 ? t : -t);
}

uint8_t g711_ulaw(int16_t pcm)
{
    static const int end[8] = { 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff, 0x1fff };
    int v = pcm >> 2, mask = 0xff, seg = 0;                 /* 14 bits */
    if (v < 0) { v = -v; mask = 0x7f; }
    if (v > 8159) v = 8159;
    v += 0x21;
    while (seg < 8 && v > end[seg]) seg++;
    if (seg == 8) return (uint8_t)(0x7f ^ mask);
    return (uint8_t)((seg << 4 | ((v >> (seg + 1)) & 0x0f)) ^ mask);
}

int16_t g711_ulaw_dec(uint8_t u)
{
    int t;
    u = (uint8_t)~u;
    t = ((u & 0x0f) << 3) + 0x84; t <<= (u & 0x70) >> 4;
    return (int16_t)(u & 0x80 ? 0x84 - t : t - 0x84);
}

/* ---------------------------------------------------------------- 16 kHz <-> 8 kHz */

#define TAPS 63
static float fir[TAPS];
static pthread_once_t fir_once = PTHREAD_ONCE_INIT;

static double bessel_i0(double x)
{
    double sum = 1, t = 1;
    for (int k = 1; k < 30; k++) { t *= x / (2 * k); sum += t * t; }
    return sum;
}

static void fir_make(void)              /* windowed sinc (Kaiser, beta 6: -63 dB), cut at 3.85 kHz of 16, unity gain */
{
    double sum = 0, wc = 2 * M_PI * 3850.0 / 16000.0, beta = 6;
    for (int i = 0; i < TAPS; i++) {
        double k = i - (TAPS - 1) / 2.0, s = k == 0 ? wc / M_PI : sin(wc * k) / (M_PI * k), r = 2.0 * i / (TAPS - 1) - 1;
        fir[i] = (float)(s * bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta)); sum += fir[i];
    }
    for (int i = 0; i < TAPS; i++) fir[i] = (float)(fir[i] / sum);
}

static float fir_push(struct half *h, float v)          /* one sample in at 16 kHz, the filter's output */
{
    float acc = 0;
    memmove(h->x, h->x + 1, (TAPS - 1) * sizeof h->x[0]); h->x[TAPS - 1] = v;
    for (int i = 0; i < TAPS; i++) acc += fir[i] * h->x[i];
    return acc;
}

static int16_t clip(float v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v)); }

void half_down(struct half *h, const int16_t *in, size_t n, int16_t *out)
{
    pthread_once(&fir_once, fir_make);
    for (size_t i = 0; i < n; i++) { float y = fir_push(h, in[i]); if (h->n++ & 1) out[i / 2] = clip(y); }
}

void half_up(struct half *h, const int16_t *in, size_t n, int16_t *out)
{
    pthread_once(&fir_once, fir_make);
    for (size_t i = 0; i < n; i++) { out[2 * i] = clip(2 * fir_push(h, in[i])); out[2 * i + 1] = clip(2 * fir_push(h, 0)); }
}

/* ---------------------------------------------------------------- the call */

int call_claim(int who)
{
    int none = CALL_NONE;
    return atomic_compare_exchange_strong(&claimed, &none, who) || none == who ? 0 : -1;
}

void call_release(int who)
{
    int w = who;
    atomic_compare_exchange_strong(&claimed, &w, CALL_NONE);
}

int call_claimed(void) { return atomic_load(&claimed); }

int call_mic_source(int set)
{
    if (set >= 0) atomic_store(&use_hfp, set != 0);
    return atomic_load(&use_hfp);
}

int call_mic_hfp(void) { return atomic_load(&src_hfp); }

static int call_gen_is(int gen)
{
    pthread_mutex_lock(&ml);
    int r = m.live && m.gen == gen;
    pthread_mutex_unlock(&ml);
    return r;
}

int call_begin(int who, int codec, void (*send)(const uint8_t *, size_t, uint32_t, void *), void *arg)
{
    pthread_mutex_lock(&ml);
    if (m.live && m.who != who) { pthread_mutex_unlock(&ml); return -1; }
    m.gen++; m.live = 1; m.who = who; m.codec = codec; m.send = send; m.arg = arg; m.tx_seq = 0;
    memset(&m.st, 0, sizeof m.st); m.arr_at = 0; play_next = 0;
    memset(jb, 0, sizeof jb); jb_any = 0; ref_n = 0; memset(ref, 0, sizeof ref);
    int gen = m.gen;
    pthread_cond_broadcast(&live_cond);
    pthread_mutex_unlock(&ml);
    return gen;
}

void call_end(int gen)
{
    pthread_mutex_lock(&ml);
    if (m.live && m.gen == gen) { m.live = 0; m.gen++; m.send = NULL; pthread_cond_broadcast(&live_cond); }
    pthread_mutex_unlock(&ml);
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
    if (!len || len > MAXPKT) return;
    pthread_mutex_lock(&ml);
    if (!m.live || m.gen != gen || (jb_any && (int32_t)(seq - jb_hi) < -JB)) { pthread_mutex_unlock(&ml); return; }   /* far too old */
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
static int enc_gen = -1, enc_hfp = -1;
static struct micgain gain;
static struct half down;
static float es_c, es_g;                /* echo against what we play, dB (peak held); the gain applied, linear */
static int dt_n;                        /* frames in a row over the echo estimate */
static float far_hold;                  /* what we play, held: falls FAR_FALL dB a frame */
static float floor_db;                  /* the room in micAsr */
static long long near_until;            /* this side talked until then (+ LISTEN_HANG_MS) */
static int listen_on;                   /* what we last asked of core_dropin_listen */
static long long listen_at;
static int16_t mic[FRAME]; static size_t mic_n;

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
    gen = m.gen; codec = m.codec; send = m.send; arg = m.arg;
    float far = far_db(now_ms()), ffloor = far_floor;
    uint32_t seq = m.tx_seq++;
    pthread_mutex_unlock(&ml);

    if (enc_gen != gen || enc_hfp != hfp) {         /* a new call or another source: a fresh encoder, gain and suppressor */
        int e = 0;
        if (enc) { opus_encoder_destroy(enc); enc = NULL; }
        if (codec == CODEC_OPUS) {
            enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &e);
            if (!enc) { fprintf(stderr, "call: no Opus encoder (%d)\n", e); return; }
            opus_encoder_ctl(enc, OPUS_SET_BITRATE, BITRATE); opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC, 1);
            opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC, LOSS_PERC); opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY, 5);
            opus_encoder_ctl(enc, OPUS_SET_SIGNAL, OPUS_SIGNAL_VOICE);
        }
        memset(&down, 0, sizeof down);
        micgain_init(&gain, MICGAIN_LEVEL); micgain_start(&gain, 1);
        es_c = -30; es_g = 1; dt_n = 0; far_hold = -100; floor_db = hfp ? -40 : -70; near_until = 0; listen_on = 0; listen_at = 0;
        enc_gen = gen; enc_hfp = hfp;
    }
    int16_t f[FRAME];
    memcpy(f, pcm, sizeof f);
    /* echo suppression: the far end talks and the mic is not clearly louder than its echo has been */
    float lv = level_db(f, FRAME), target = 1; long long now = now_ms(); int farend, near;
    far_hold = far > far_hold - FAR_FALL ? far : far_hold - FAR_FALL;
    farend = far_voice(far_hold, ffloor);
    int over = farend && lv >= far_hold + es_c + DOUBLE_TALK_DB;
    dt_n = over ? dt_n + 1 : 0;
    if (farend && !own && dt_n < DOUBLE_TALK_N) {       /* echo, as far as we can tell (not our own sounds): its peak */
        float x = lv - far_hold;
        es_c = x > es_c ? x : es_c - 0.04f;
        if (es_c < -60) es_c = -60;
        if (es_c > 0) es_c = 0;
    }
    floor_db = lv < floor_db ? (lv > -90 ? lv : -90) : floor_db + 0.03f;     /* drops at once, rises 1.5 dB/s */
    near = !own && lv > floor_db + NEAR_DB && lv > (hfp ? NEAR_MIN_HFP_DB : NEAR_MIN_DB) && (!farend || dt_n >= DOUBLE_TALK_N);
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
    int want = now < near_until;
    if (want != listen_on && now - listen_at >= 250) { listen_on = want; listen_at = now; core_dropin_listen(want); }
    /* HASSMIC_DROPIN_TRACE=<file>: the gate's inputs and decisions per frame, for tuning on a device (export it in
     * hassmic.conf): ms, mic dB, far dB, echo estimate, floor, this side talks, own sound, double talk run, gate, gain,
     * the call pipeline's microphone (1) or micAsr (0) */
    { static FILE *tr; if (!tr && getenv("HASSMIC_DROPIN_TRACE")) tr = fopen(getenv("HASSMIC_DROPIN_TRACE"), "w");
      if (tr) { fprintf(tr, "%lld %.1f %.1f %.1f %.1f %d %d %d %.2f %.1f %d\n", now, lv, far, es_c, floor_db, near, own, dt_n, target, hfp ? 0.0f : gain.gain_db, hfp); fflush(tr); } }
    int32_t len;
    if (codec == CODEC_OPUS) len = enc ? opus_encode(enc, f, FRAME, op, sizeof op) : -1;
    else {
        int16_t h[FRAME / 2];
        half_down(&down, f, FRAME, h);
        for (int i = 0; i < FRAME / 2; i++) op[i] = codec == CODEC_PCMA ? g711_alaw(h[i]) : g711_ulaw(h[i]);
        len = FRAME / 2;
    }
    if (len > 0) send(op, (size_t)len, seq, arg);
}

void call_mic(const int16_t *pcm, size_t n, int own)
{
    if (!atomic_load(&running) || !call_live() || atomic_load(&src_hfp)) { mic_n = 0; return; }
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
        if (!atomic_load(&use_hfp) || !callmic_present()) { wait_call_over(gen); continue; }
        long long t0 = now_ms();
        while (atomic_load(&voip_gen) != gen && call_gen_is(gen) && now_ms() - t0 < 1500) usleep(10000);
        if (!call_gen_is(gen)) continue;
        if (atomic_load(&voip_gen) != gen || callmic_open()) {
            fprintf(stderr, "call: no call microphone (%s), the assistant's for this call\n", atomic_load(&voip_gen) != gen ? "no call mode" : "micHfp does not open");
            wait_call_over(gen); continue;
        }
        atomic_store(&src_hfp, 1);
        fprintf(stderr, "call: the call pipeline's microphone (micHfp)\n");
        int16_t f[FRAME]; size_t fn = 0; int empty = 0; const char *why = NULL;
        while (call_gen_is(gen)) {
            if (!atomic_load(&use_hfp)) { why = "switched to the assistant's"; break; }
            const void *d; int n = callmic_read(&d);
            if (n < 0) { why = "micHfp is gone"; break; }
            if (n == 0) { if (++empty >= 3) { why = "micHfp delivers nothing"; break; } continue; }
            empty = 0;
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

/* One frame of G.711 into 16 kHz (fewer bytes than a frame: the rest silence) */
static void g711_frame(int codec, struct half *up, const uint8_t *p, int len, int16_t *pcm)
{
    int16_t h[FRAME / 2] = { 0 };
    for (int i = 0; i < len && i < FRAME / 2; i++) h[i] = codec == CODEC_PCMA ? g711_alaw_dec(p[i]) : g711_ulaw_dec(p[i]);
    half_up(up, h, FRAME / 2, pcm);
}

/* The playout thread: one frame per 20 ms into the Voip stream, from the jitter buffer, with Opus's FEC and loss
 * concealment for what is missing, silence before the first frame and after a long gap */
static void *playout(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&ml);
        while (!m.live) pthread_cond_wait(&live_cond, &ml);
        int gen = m.gen, codec = m.codec; long long t0 = now_ms();
        far_floor = -70;
        pthread_mutex_unlock(&ml);
        int e = 0, open = voip_open(RATE, 1) == 0, started = 0, missing = 0, skipped = 0, stretched = 0;
        OpusDecoder *dec = codec == CODEC_OPUS ? opus_decoder_create(RATE, 1, &e) : NULL;
        struct half up; int16_t last[FRAME]; float fade = 0;
        memset(&up, 0, sizeof up); memset(last, 0, sizeof last);
        if (!open) fprintf(stderr, "call: cannot open the Voip stream\n");
        else atomic_store(&voip_gen, gen);              /* call mode on: callmic_thread may open micHfp */
        if (codec == CODEC_OPUS && !dec) fprintf(stderr, "call: no Opus decoder (%d)\n", e);
        uint32_t next = 0;
        for (;;) {
            uint8_t pkt[MAXPKT]; int len = 0, fec = 0; int16_t pcm[FRAME];
            while (open && voip_queued_us() > QUEUE_US) usleep(4000);
            pthread_mutex_lock(&ml);
            if (!m.live || m.gen != gen) { pthread_mutex_unlock(&ml); break; }
            if (!started && jb_any && (jb_hi - jb_lo >= PREBUF - 1 || now_ms() - t0 > 300)) { started = 1; next = jb_lo; }
            if (started && (int32_t)(jb_hi - next) > MAXBUF) { next = jb_hi - (PREBUF - 1); skipped++; }
            if (started) {
                struct slot *s = &jb[next % JB], *s1 = &jb[(next + 1) % JB];
                if (s->len && s->seq == next) { len = s->len; memcpy(pkt, s->d, (size_t)len); }
                else if ((int32_t)(jb_hi - next) <= 0) stretched++;         /* nothing newer yet: wait, concealing */
                else if (codec == CODEC_OPUS && s1->len && s1->seq == next + 1) { len = s1->len; memcpy(pkt, s1->d, (size_t)len); fec = 1; }
                if (len || (int32_t)(jb_hi - next) > 0) {                  /* played, or lost for good: on to the next */
                    if (!len || fec) m.st.lost++;
                    s->len = 0;
                    next++; m.st.played++; play_next = next;
                }
            }
            pthread_mutex_unlock(&ml);
            int got = 0;
            if (codec == CODEC_OPUS) {
                if (dec && len) got = opus_decode(dec, pkt, len, pcm, FRAME, fec);
                else if (dec && started && missing < PLC_FRAMES) got = opus_decode(dec, NULL, 0, pcm, FRAME, 0);
            } else if (len) { g711_frame(codec, &up, pkt, len, pcm); memcpy(last, pcm, sizeof last); fade = 1; got = FRAME; }
            else if (started && missing < PLC_FRAMES && fade > 0) {        /* the last frame again, fading out */
                fade *= 0.5f; for (int i = 0; i < FRAME; i++) pcm[i] = (int16_t)(last[i] * fade); got = FRAME;
            }
            missing = len ? 0 : missing + 1;
            if (got != FRAME) memset(pcm, 0, sizeof pcm);
            long long q = open ? voip_queued_us() : 0;
            if (open && voip_write(pcm, sizeof pcm) < 0) { fprintf(stderr, "call: the Voip stream failed\n"); voip_close(); open = 0; }
            if (!open) usleep(20000);
            pthread_mutex_lock(&ml);
            float pdb = level_db(pcm, FRAME);
            ref[ref_n++ % REF_N].t = now_ms() + q / 1000; ref[(ref_n - 1) % REF_N].db = pdb;   /* when it plays */
            far_floor = pdb < far_floor ? (pdb > -90 ? pdb : -90) : far_floor + 0.03f;
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
