/*
 * Drop In: a two-way call between two Echos of one arbitration network, as Alexa's Drop In between the Echos of a
 * household.  Started from Home Assistant (the action "drop_in", a voice command through an automation) or the settings
 * page; the Echo called connects at once and opens its microphone, or with "drop_in_answer" set to ask, rings until
 * someone presses its action button.  The action button, "<wake word>, stop" or "hang up" ends it, on either side.
 *
 * Who may call: a member of our network (arb.c), i.e. an Echo Home Assistant vouched for or one paired with the volume
 * keys; nothing else on the LAN can ring an Echo or listen in.  The calls go through arb as signed messages (arb_tell):
 *   invite   caller -> callee   call id, the caller's audio port, its ephemeral X25519 key; again every INVITE_MS
 *                               until answered (Wi-Fi drops broadcasts), then every second while it rings
 *   ringing  callee -> caller   it rings (answer: ask); the caller keeps waiting
 *   accept   callee -> caller   its audio port and ephemeral key: both sides derive the call's key, audio flows
 *   refuse   callee -> caller   with a reason (busy, Drop In off, do not disturb, muted, not answered)
 *   bye      either             sent three times
 * payload: kind call_id[8] port[2] epk[32] len reason[len]
 * The call's key: BLAKE2b keyed with a key every member derives from K, over the X25519 of the two ephemeral keys, the
 * call id and both ids.  Recorded calls stay closed to whoever gets K later.
 *
 * Audio: Opus (the firmware's libopus), 16 kHz mono, 20 ms frames, inband FEC, straight between the two Echos over UDP:
 *   "HMD1" call_id[8] dir seq[4] enc(frame) mac[16]     XChaCha20-Poly1305, nonce = call_id dir seq, header as AD
 * dir 0 from the caller, 1 from the callee; seq counts frames.  Every frame goes out, silence included (no DTX): the
 * stream is the call's keepalive, LOST_MS without one ends it.  The other side's address comes from its message, then
 * from its authenticated audio (an address that changed mid-call follows).
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
 */
#include "dropin.h"
#include "arb.h"
#include "audio.h"
#include "core.h"
#include "micgain.h"
#include "ws.h"
#include "../third_party/monocypher.h"
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

#define RATE        16000
#define FRAME       320                 /* 20 ms */
#define MAXPKT      400                 /* an Opus frame at 24 kbit/s is ~60 bytes */
#define BITRATE     24000               /* wideband speech; FEC rides inside it */
#define LOSS_PERC   10                  /* what the encoder plans FEC for: Wi-Fi, no retries on a busy channel */
#define INVITE_MS   250
#define ANSWER_MS   4000                /* no word from the callee at all: not there */
#define RING_MS     30000               /* ask: how long it rings */
#define CALLER_MS   5000                /* ringing, the caller's invites stopped: it gave up */
#define LOST_MS     5000                /* live, no audio for this long: the other Echo is gone */
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
/* What we play, held after it stops and falling 20 dB/s: the echo's tail outlasts ECHO_MS.  In every pause of the other
 * side a tail 14 dB over the room came 0.3 to 1 s after its last word and passed for this side talking (trace,
 * 2026-10-08).  A reply 0.5 s after the other side stops passes at -48 dBFS in micAsr, a quiet one after a second */
#define FAR_FALL    0.4f
#define LISTEN_HANG_MS 1500

enum { M_INVITE = 1, M_ACCEPT, M_RINGING, M_REFUSE, M_BYE };
const char *const dropin_states[4] = { "idle", "calling", "ringing", "connected" };

static pthread_mutex_t dl = PTHREAD_MUTEX_INITIALIZER;   /* everything below; taken after core_lock, before arb's */
static pthread_cond_t live_cond = PTHREAD_COND_INITIALIZER;
static atomic_int notify;                                 /* the loop tells the core (core_dropin): callers may hold core_lock */
static int sock = -1, running, enabled = 1, ask;
static uint16_t my_port;
static uint8_t self_id[8];
static struct {
    int state, outgoing, gen, keyed, rang;              /* gen: one per call; rang: the callee said it rings */
    uint8_t id[8], peer[8];
    char node[64];
    uint32_t ip; uint16_t port;                         /* the other Echo's audio address (network byte order ip) */
    uint8_t esk[32], epk[32], key[32];
    long long since, sig_at, rx_at, live_at;
    uint32_t tx_seq;
    unsigned rx, lost, late, played;                    /* frames in; played without their packet; arrived after their turn */
    long long gap_max, arr_at;                          /* the longest wait between two packets, ms */
} c;
static uint32_t play_next;                              /* the playout's next frame, for counting late ones */
static struct { int left; long long at; uint8_t to[8]; uint8_t p[ARB_MSG_MAX]; size_t n; } bye;
/* Calls that ended: an invite the caller repeated before it heard of the end (a decline, a hang-up while it rang) must
 * not start the call again */
static uint8_t ended[4][8]; static unsigned ended_n;
static struct slot { uint32_t seq; int len; uint8_t d[MAXPKT]; } jb[JB];
static uint32_t jb_hi, jb_lo; static int jb_any;      /* the newest frame in, the first of the call */
static struct { long long t; float db; } ref[REF_N];
static unsigned ref_n;

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

/* ---------------------------------------------------------------- messages (dl held) */

static size_t msg(uint8_t *p, int kind, const char *reason)
{
    size_t n = 0, l = reason ? strlen(reason) : 0;
    if (l > 60) l = 60;
    p[n++] = (uint8_t)kind; memcpy(p + n, c.id, 8); n += 8;
    p[n++] = (uint8_t)(my_port & 0xff); p[n++] = (uint8_t)(my_port >> 8);
    memcpy(p + n, c.epk, 32); n += 32;
    p[n++] = (uint8_t)l;
    if (l) { memcpy(p + n, reason, l); n += l; }
    return n;
}

static void send_msg(int kind, const char *reason)
{
    uint8_t p[ARB_MSG_MAX]; size_t n = msg(p, kind, reason);
    if (arb_tell(c.peer, p, n)) fprintf(stderr, "drop in: cannot send (no network)\n");
}

/* A refusal or bye to an Echo whose call we do not take: with its call id, our ephemeral key left out */
static void send_to(const uint8_t to[8], const uint8_t id[8], int kind, const char *reason)
{
    uint8_t p[ARB_MSG_MAX], keep[8]; size_t n;
    memcpy(keep, c.id, 8); memcpy(c.id, id, 8);
    n = msg(p, kind, reason); memset(p + 11, 0, 32);
    memcpy(c.id, keep, 8);
    arb_tell(to, p, n);
}

static int derive(const uint8_t peer_epk[32])
{
    uint8_t kd[32], shared[32], zero[32] = { 0 }, m[32 + 8 + 16];
    if (arb_derive("hassmic drop in 1", kd)) return -1;
    crypto_x25519(shared, c.esk, peer_epk);
    if (!memcmp(shared, zero, 32)) { crypto_wipe(kd, sizeof kd); return -1; }      /* a low-order point: no secret */
    memcpy(m, shared, 32); memcpy(m + 32, c.id, 8);
    memcpy(m + 40, c.outgoing ? self_id : c.peer, 8); memcpy(m + 48, c.outgoing ? c.peer : self_id, 8);
    crypto_blake2b_keyed(c.key, 32, kd, 32, m, sizeof m);
    crypto_wipe(kd, sizeof kd); crypto_wipe(shared, sizeof shared); crypto_wipe(m, sizeof m);
    c.keyed = 1;
    return 0;
}

static void go_live(long long now)
{
    c.state = DROPIN_LIVE; c.live_at = c.rx_at = now; c.tx_seq = 0; c.rx = c.lost = c.late = c.played = 0; c.gap_max = c.arr_at = 0; play_next = 0;
    memset(jb, 0, sizeof jb); jb_any = 0; ref_n = 0; memset(ref, 0, sizeof ref);
    fprintf(stderr, "drop in: connected with %s\n", c.node);
    pthread_cond_broadcast(&live_cond);
}

static void end_call(const char *why, int tell)
{
    if (c.state == DROPIN_IDLE) return;
    if (c.state == DROPIN_LIVE) fprintf(stderr, "drop in: ended with %s after %lld s (%s); %u frames in, %u played, %u of them missing "
                                        "(%u came too late), longest gap %lld ms\n", c.node, (now_ms() - c.live_at) / 1000, why, c.rx, c.played,
                                        c.lost, c.late, c.gap_max);
    else fprintf(stderr, "drop in: %s %s: %s\n", c.outgoing ? "call to" : "call from", c.node, why);
    if (tell) {
        bye.n = msg(bye.p, M_BYE, why); memcpy(bye.to, c.peer, 8);
        arb_tell(bye.to, bye.p, bye.n); bye.left = 2; bye.at = now_ms();
    }
    memcpy(ended[ended_n++ % 4], c.id, 8);
    c.state = DROPIN_IDLE; c.gen++; c.keyed = 0; c.rang = 0;
    crypto_wipe(c.esk, sizeof c.esk); crypto_wipe(c.key, sizeof c.key);
    pthread_cond_broadcast(&live_cond);
}

static void fresh_call(const uint8_t peer[8], const char *node, uint32_t ip, int outgoing)
{
    memset(&c.id, 0, sizeof c.id);
    c.outgoing = outgoing; c.gen++; c.keyed = 0; c.rang = 0;
    memcpy(c.peer, peer, 8); snprintf(c.node, sizeof c.node, "%s", node); c.ip = ip; c.port = 0;
    ws_random(c.esk, 32); crypto_x25519_public_key(c.epk, c.esk);
    c.since = c.sig_at = now_ms();
}

/* ---------------------------------------------------------------- notifications (the loop's, not dl) */

static void changed(void) { atomic_store(&notify, 1); }

static void tell_core(void)
{
    char node[64]; int st, out;
    pthread_mutex_lock(&dl); st = c.state; out = c.outgoing; snprintf(node, sizeof node, "%s", st ? c.node : ""); pthread_mutex_unlock(&dl);
    core_dropin(st, node, out);
}

/* ---------------------------------------------------------------- interface */

int dropin_running(void) { return running; }

int dropin_enable(int set)
{
    int hang = 0;
    pthread_mutex_lock(&dl);
    if (set >= 0 && set != enabled) { enabled = set != 0; hang = !enabled && c.state; if (hang) end_call("Drop In switched off", 1); }
    int r = enabled;
    pthread_mutex_unlock(&dl);
    if (hang) changed();
    return r;
}

int dropin_ask(int set)
{
    pthread_mutex_lock(&dl);
    if (set >= 0) ask = set != 0;
    int r = ask;
    pthread_mutex_unlock(&dl);
    return r;
}

int dropin_status(char *peer, size_t cap)
{
    pthread_mutex_lock(&dl);
    int st = c.state;
    if (peer && cap) snprintf(peer, cap, "%s", st ? c.node : "");
    pthread_mutex_unlock(&dl);
    return st;
}

int dropin_call(const char *target, char *err, size_t errsz)
{
    char node[64]; uint8_t id[8]; unsigned ip;
    core_node_of(target, node);
    if (!running) { snprintf(err, errsz, "Drop In is not running on this Echo"); return -1; }
    if (!strcmp(node, core_node_name())) { snprintf(err, errsz, "that is this Echo"); return -1; }
    if (arb_member(node, id, &ip)) { snprintf(err, errsz, "no Echo \"%s\" in this Echo's network", node); return -1; }
    const char *no = core_dropin_refusal(0);
    pthread_mutex_lock(&dl);
    if (!enabled) { pthread_mutex_unlock(&dl); snprintf(err, errsz, "Drop In is off on this Echo"); return -1; }
    if (no) { pthread_mutex_unlock(&dl); snprintf(err, errsz, "%s", no); return -1; }
    if (c.state) { pthread_mutex_unlock(&dl); snprintf(err, errsz, "already in a Drop In with %s", c.node); return -1; }
    fresh_call(id, node, ip, 1);
    ws_random(c.id, 8);
    c.state = DROPIN_CALLING;
    fprintf(stderr, "drop in: calling %s\n", node);
    send_msg(M_INVITE, NULL);
    pthread_mutex_unlock(&dl);
    changed();
    return 0;
}

void dropin_hangup(const char *why)
{
    pthread_mutex_lock(&dl);
    int was = c.state;
    if (was == DROPIN_RINGING) { send_msg(M_REFUSE, "declined"); end_call(why, 0); }
    else end_call(why, 1);
    pthread_mutex_unlock(&dl);
    if (was) changed();
}

static void answer(void)              /* dl held: the callee takes a ringing call */
{
    send_msg(M_ACCEPT, NULL);
    go_live(now_ms());
}

int dropin_button(void)
{
    pthread_mutex_lock(&dl);
    int st = c.state;
    if (st == DROPIN_RINGING) answer();
    else if (st) end_call("action button", 1);
    pthread_mutex_unlock(&dl);
    if (st) changed();
    return st != DROPIN_IDLE;
}

void dropin_message(const unsigned char from[8], const char *node, unsigned ip, const unsigned char *p, size_t n)
{
    uint8_t id[8], epk[32]; char reason[64] = ""; unsigned port; int kind, tell = 0;
    if (n < 44 || n < (size_t)44 + p[43]) return;
    kind = p[0]; memcpy(id, p + 1, 8); port = p[9] | p[10] << 8; memcpy(epk, p + 11, 32);
    snprintf(reason, sizeof reason, "%.*s", p[43], (const char *)p + 44);
    for (char *q = reason; *q; q++) if ((unsigned char)*q < 0x20) *q = ' ';
    /* before dl: core_lock comes first */
    const char *no = kind == M_INVITE ? core_dropin_refusal(1) : NULL;
    long long now = now_ms();
    pthread_mutex_lock(&dl);
    int same = c.state && !memcmp(c.id, id, 8) && !memcmp(c.peer, from, 8);
    switch (kind) {
    case M_INVITE:
        if (same && !c.outgoing) {                      /* again: it missed our answer, or it rings and the caller waits */
            c.sig_at = now;
            if (c.state == DROPIN_LIVE) send_msg(M_ACCEPT, NULL); else if (c.state == DROPIN_RINGING) send_msg(M_RINGING, NULL);
            break;
        }
        { int old = 0; for (int i = 0; i < 4; i++) old |= !memcmp(ended[i], id, 8);
          if (old) break; }                             /* a late copy of an invite for a call that is over */
        if (!enabled || no || c.state || !port || !node[0]) {
            const char *why = !enabled ? "Drop In is off there" : no ? no : c.state ? "busy in another Drop In" : "no address";
            fprintf(stderr, "drop in: call from %s refused: %s\n", node[0] ? node : "an Echo", why);
            send_to(from, id, M_REFUSE, why);
            break;
        }
        fresh_call(from, node, ip, 0);
        memcpy(c.id, id, 8); c.port = htons((uint16_t)port);
        if (derive(epk)) { send_to(from, id, M_REFUSE, "no key"); c.state = DROPIN_IDLE; break; }
        if (ask) { c.state = DROPIN_RINGING; fprintf(stderr, "drop in: %s calls, ringing until the action button\n", node); send_msg(M_RINGING, NULL); }
        else { fprintf(stderr, "drop in: %s drops in\n", node); answer(); }
        tell = 1;
        break;
    case M_RINGING:
        if (same && c.outgoing && c.state == DROPIN_CALLING) { if (!c.rang) fprintf(stderr, "drop in: %s rings\n", c.node); c.rang = 1; c.sig_at = now; c.rx_at = now; tell = 1; }
        break;
    case M_ACCEPT:
        if (!same || !c.outgoing || c.state != DROPIN_CALLING) break;
        c.port = htons((uint16_t)port); c.ip = ip;
        if (!port || derive(epk)) { end_call("no key", 1); tell = 1; break; }
        go_live(now); tell = 1;
        break;
    case M_REFUSE:
        if (same && c.outgoing && c.state == DROPIN_CALLING) { char w[80]; snprintf(w, sizeof w, "refused: %s", reason[0] ? reason : "no reason"); end_call(w, 0); tell = 1; }
        break;
    case M_BYE:
        if (same) { char w[80]; snprintf(w, sizeof w, "%s hung up%s%s", c.node, reason[0] ? ": " : "", reason); end_call(w, 0); tell = 1; }
        break;
    }
    pthread_mutex_unlock(&dl);
    crypto_wipe(epk, sizeof epk);
    if (tell) changed();
}

/* ---------------------------------------------------------------- audio out: the mic */

/* The echo suppressor's and the gain's state, and the encoder: the capture thread's own */
static OpusEncoder *enc;
static int enc_gen = -1;
static struct micgain gain;
static float es_c, es_g;                /* echo against what we play, dB (peak held); the gain applied, linear */
static int dt_n;                        /* frames in a row over the echo estimate */
static float far_hold;                  /* what we play, held: falls FAR_FALL dB a frame */
static float floor_db;                  /* the room in micAsr */
static long long near_until;            /* this side talked until then (+ LISTEN_HANG_MS) */
static int listen_on;                   /* what we last asked of core_dropin_listen */
static long long listen_at;
static int16_t mic[FRAME]; static size_t mic_n;

static float far_db(long long now)       /* the loudest we played that can still be in the mic stream */
{
    float m = -100;
    for (unsigned i = 0; i < REF_N; i++) if (ref[i].t && ref[i].t <= now && now - ref[i].t < ECHO_MS && ref[i].db > m) m = ref[i].db;
    return m;
}

int dropin_far_talking(void)
{
    pthread_mutex_lock(&dl);
    int r = c.state == DROPIN_LIVE && far_db(now_ms()) > FAR_DB;
    pthread_mutex_unlock(&dl);
    return r;
}

static void send_frame(const int16_t *pcm, int own)
{
    uint8_t pkt[17 + MAXPKT + 16], op[MAXPKT], nonce[24] = { 0 }, key[32]; struct sockaddr_in to; int gen;
    pthread_mutex_lock(&dl);
    if (c.state != DROPIN_LIVE || !c.keyed || !c.port) { pthread_mutex_unlock(&dl); return; }
    gen = c.gen;
    float far = far_db(now_ms());
    memcpy(key, c.key, 32);
    memcpy(pkt, "HMD1", 4); memcpy(pkt + 4, c.id, 8); pkt[12] = (uint8_t)!c.outgoing;
    uint32_t seq = c.tx_seq++;
    for (int i = 0; i < 4; i++) pkt[13 + i] = (uint8_t)(seq >> 8 * i);
    memset(&to, 0, sizeof to); to.sin_family = AF_INET; to.sin_addr.s_addr = c.ip; to.sin_port = c.port;
    pthread_mutex_unlock(&dl);

    if (enc_gen != gen) {                           /* a new call: a fresh encoder, gain and suppressor */
        int e = 0;
        if (enc) opus_encoder_destroy(enc);
        enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &e);
        if (!enc) { fprintf(stderr, "drop in: no Opus encoder (%d)\n", e); crypto_wipe(key, sizeof key); return; }
        opus_encoder_ctl(enc, OPUS_SET_BITRATE, BITRATE); opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC, 1);
        opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC, LOSS_PERC); opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY, 5);
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL, OPUS_SIGNAL_VOICE);
        micgain_init(&gain, MICGAIN_LEVEL); micgain_start(&gain, 1);
        es_c = -30; es_g = 1; dt_n = 0; far_hold = -100; floor_db = -70; near_until = 0; listen_on = 0; listen_at = 0; enc_gen = gen;
    }
    int16_t f[FRAME];
    memcpy(f, pcm, sizeof f);
    /* echo suppression: the far end talks and the mic is not clearly louder than its echo has been */
    float m = level_db(f, FRAME), target = 1; long long now = now_ms(); int farend, near;
    far_hold = far > far_hold - FAR_FALL ? far : far_hold - FAR_FALL;
    farend = far_hold > FAR_DB;
    int over = farend && m >= far_hold + es_c + DOUBLE_TALK_DB;
    dt_n = over ? dt_n + 1 : 0;
    if (farend && !own && dt_n < DOUBLE_TALK_N) {       /* echo, as far as we can tell (not our own sounds): its peak */
        float x = m - far_hold;
        es_c = x > es_c ? x : es_c - 0.04f;
        if (es_c < -60) es_c = -60;
        if (es_c > 0) es_c = 0;
    }
    floor_db = m < floor_db ? (m > -90 ? m : -90) : floor_db + 0.03f;     /* drops at once, rises 1.5 dB/s */
    near = !own && m > floor_db + NEAR_DB && (!farend || dt_n >= DOUBLE_TALK_N);
    /* A gate: open while this side talks (and LISTEN_HANG_MS after, unless the far end plays), else ducked.  Ducked only
     * while the far end played (the first try), the gain lifted the room in its pauses from -72 to -48 dBFS while the
     * rest went out at -70: hiss that came and went with every sentence of the other side (2026-10-08) */
    if (own) target = powf(10, SUPPRESS_OWN_DB / 20);
    else if (!near && (farend || now >= near_until)) target = powf(10, SUPPRESS_DB / 20);
    /* the gain first: after the ducking it would give the 25 dB back (+24 dB at micAsr's level, measured) */
    gain.hold = own || farend;                          /* the talker's level is learnt while the other side is quiet */
    micgain_run(&gain, f, f, FRAME);
    float step = (target - es_g) / FRAME;              /* within a frame: a word's onset is not cut */
    for (int i = 0; i < FRAME; i++) { if ((step < 0 && es_g > target) || (step > 0 && es_g < target)) es_g += step; f[i] = (int16_t)(f[i] * es_g); }
    if (near) near_until = now + LISTEN_HANG_MS;
    int want = now < near_until;
    if (want != listen_on && now - listen_at >= 250) { listen_on = want; listen_at = now; core_dropin_listen(want); }
    /* HASSMIC_DROPIN_TRACE=<file>: the gate's inputs and decisions per frame, for tuning on a device (export it in
     * hassmic.conf): ms, mic dB, far dB, echo estimate, floor, this side talks, own sound, double talk run, gate, gain */
    { static FILE *tr; if (!tr && getenv("HASSMIC_DROPIN_TRACE")) tr = fopen(getenv("HASSMIC_DROPIN_TRACE"), "w");
      if (tr) { fprintf(tr, "%lld %.1f %.1f %.1f %.1f %d %d %d %.2f %.1f\n", now, m, far, es_c, floor_db, near, own, dt_n, target, gain.gain_db); fflush(tr); } }
    int32_t len = opus_encode(enc, f, FRAME, op, sizeof op);
    if (len <= 0) { crypto_wipe(key, sizeof key); return; }
    memcpy(nonce, pkt + 4, 13);
    crypto_aead_lock(pkt + 17, pkt + 17 + len, key, nonce, pkt, 17, op, (size_t)len);
    crypto_wipe(key, sizeof key);
    if (sendto(sock, pkt, (size_t)(17 + len + 16), 0, (struct sockaddr *)&to, sizeof to) < 0 && errno != EAGAIN) {
        static long long logged;
        if (now_ms() - logged > 5000) { logged = now_ms(); fprintf(stderr, "drop in: send: %s\n", strerror(errno)); }
    }
}

void dropin_mic(const int16_t *pcm, size_t n, int own)
{
    if (!running || dropin_status(NULL, 0) != DROPIN_LIVE) { mic_n = 0; return; }
    while (n) {
        size_t k = FRAME - mic_n < n ? FRAME - mic_n : n;
        memcpy(mic + mic_n, pcm, k * 2); mic_n += k; pcm += k; n -= k;
        if (mic_n == FRAME) { send_frame(mic, own); mic_n = 0; }
    }
}

/* ---------------------------------------------------------------- audio in */

static void on_audio(const uint8_t *p, size_t n, const struct sockaddr_in *from)
{
    uint8_t nonce[24] = { 0 }; uint32_t seq = 0;
    if (n < 17 + 1 + 16 || n > 17 + MAXPKT + 16 || memcmp(p, "HMD1", 4)) return;
    for (int i = 0; i < 4; i++) seq |= (uint32_t)p[13 + i] << 8 * i;
    pthread_mutex_lock(&dl);
    if (c.state != DROPIN_LIVE || !c.keyed || memcmp(p + 4, c.id, 8) || p[12] != (uint8_t)c.outgoing) { pthread_mutex_unlock(&dl); return; }
    if (jb_any && (int32_t)(seq - jb_hi) < -JB) { pthread_mutex_unlock(&dl); return; }          /* far too old */
    struct slot *s = &jb[seq % JB];
    memcpy(nonce, p + 4, 13);
    if (crypto_aead_unlock(s->d, p + n - 16, c.key, nonce, p, 17, p + 17, n - 17 - 16)) { pthread_mutex_unlock(&dl); return; }
    s->seq = seq; s->len = (int)(n - 17 - 16);
    if (!jb_any || (int32_t)(seq - jb_hi) > 0) jb_hi = seq;
    if (!jb_any || (int32_t)(seq - jb_lo) < 0) jb_lo = seq;
    long long now = now_ms();
    if (c.arr_at && now - c.arr_at > c.gap_max) c.gap_max = now - c.arr_at;
    if (jb_any && (int32_t)(seq - play_next) < 0) c.late++;
    jb_any = 1; c.rx++; c.rx_at = c.arr_at = now;
    c.ip = from->sin_addr.s_addr; c.port = from->sin_port;     /* authenticated: where it is now */
    pthread_mutex_unlock(&dl);
}

/* The playout thread: one frame per 20 ms into the Voip stream, from the jitter buffer, with Opus's FEC and loss
 * concealment for what is missing, silence before the first frame and after a long gap */
static void *playout(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&dl);
        while (c.state != DROPIN_LIVE) pthread_cond_wait(&live_cond, &dl);
        int gen = c.gen; long long t0 = now_ms();
        pthread_mutex_unlock(&dl);
        int e = 0, open = voip_open(RATE, 1) == 0, started = 0, missing = 0, skipped = 0, stretched = 0;
        OpusDecoder *dec = opus_decoder_create(RATE, 1, &e);
        if (!open) fprintf(stderr, "drop in: cannot open the Voip stream\n");
        if (!dec) fprintf(stderr, "drop in: no Opus decoder (%d)\n", e);
        uint32_t next = 0;
        for (;;) {
            uint8_t pkt[MAXPKT]; int len = 0, fec = 0; int16_t pcm[FRAME];
            while (open && voip_queued_us() > QUEUE_US) usleep(4000);
            pthread_mutex_lock(&dl);
            if (c.state != DROPIN_LIVE || c.gen != gen) { pthread_mutex_unlock(&dl); break; }
            if (!started && jb_any && (jb_hi - jb_lo >= PREBUF - 1 || now_ms() - t0 > 300)) { started = 1; next = jb_lo; }
            if (started && (int32_t)(jb_hi - next) > MAXBUF) { next = jb_hi - (PREBUF - 1); skipped++; }
            if (started) {
                struct slot *s = &jb[next % JB], *s1 = &jb[(next + 1) % JB];
                if (s->len && s->seq == next) { len = s->len; memcpy(pkt, s->d, (size_t)len); }
                else if ((int32_t)(jb_hi - next) <= 0) stretched++;         /* nothing newer yet: wait, concealing */
                else if (s1->len && s1->seq == next + 1) { len = s1->len; memcpy(pkt, s1->d, (size_t)len); fec = 1; }
                if (len || (int32_t)(jb_hi - next) > 0) {                  /* played, or lost for good: on to the next */
                    if (!len || fec) c.lost++;
                    s->len = 0;
                    next++; c.played++; play_next = next;
                }
            }
            pthread_mutex_unlock(&dl);
            int got = 0;
            if (dec && len) got = opus_decode(dec, pkt, len, pcm, FRAME, fec);
            else if (dec && started && missing < PLC_FRAMES) got = opus_decode(dec, NULL, 0, pcm, FRAME, 0);
            missing = len ? 0 : missing + 1;
            if (got != FRAME) memset(pcm, 0, sizeof pcm);
            long long q = open ? voip_queued_us() : 0;
            if (open && voip_write(pcm, sizeof pcm) < 0) { fprintf(stderr, "drop in: the Voip stream failed\n"); voip_close(); open = 0; }
            if (!open) usleep(20000);
            pthread_mutex_lock(&dl);
            ref[ref_n++ % REF_N].t = now_ms() + q / 1000; ref[(ref_n - 1) % REF_N].db = level_db(pcm, FRAME);   /* when it plays */
            pthread_mutex_unlock(&dl);
        }
        if (skipped || stretched) fprintf(stderr, "drop in: playout skipped ahead %d times (more than %d ms waiting), waited %d times for a late frame\n",
                                          skipped, MAXBUF * 20, stretched);
        if (open) voip_close();
        if (dec) opus_decoder_destroy(dec);
    }
    return NULL;
}

/* ---------------------------------------------------------------- the loop: audio in, retries, timeouts */

static void tick(void)
{
    long long now = now_ms(); int tell = 0;
    pthread_mutex_lock(&dl);
    switch (c.state) {
    case DROPIN_CALLING:
        if (!c.rang && now - c.since > ANSWER_MS) { end_call("no answer", 1); tell = 1; }
        else if (c.rang && now - c.since > RING_MS + ANSWER_MS) { end_call("not answered", 1); tell = 1; }
        else if (now - c.sig_at >= (c.rang ? 1000 : INVITE_MS)) { c.sig_at = now; send_msg(M_INVITE, NULL); }
        break;
    case DROPIN_RINGING:
        if (now - c.since > RING_MS) { send_msg(M_REFUSE, "not answered"); end_call("not answered", 0); tell = 1; }
        else if (now - c.sig_at > CALLER_MS) { end_call("the caller gave up", 0); tell = 1; }
        break;
    case DROPIN_LIVE:
        if (now - c.rx_at > LOST_MS) { end_call("lost: no audio for 5 s", 1); tell = 1; }
        break;
    }
    if (bye.left && now - bye.at >= 100) { arb_tell(bye.to, bye.p, bye.n); bye.at = now; bye.left--; }
    pthread_mutex_unlock(&dl);
    if (tell) changed();
}

static void *loop(void *arg)
{
    uint8_t buf[1500];
    (void)arg;
    for (;;) {
        struct pollfd pf = { sock, POLLIN, 0 }; struct sockaddr_in from; socklen_t fl = sizeof from; ssize_t n;
        if (poll(&pf, 1, 20) > 0 && (pf.revents & POLLIN) && (n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl)) > 0)
            on_audio(buf, (size_t)n, &from);
        tick();
        if (atomic_exchange(&notify, 0)) tell_core();
    }
    return NULL;
}

int dropin_start(int port)
{
    struct sockaddr_in a; pthread_t t; int tos = 0xb8;      /* DSCP EF: Wi-Fi's voice queue (WMM) where the AP honours it */
    if (!arb_running()) return -1;
    arb_self(self_id);
    if ((sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) { perror("drop in: socket"); return -1; }
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&a, sizeof a)) { perror("drop in: bind"); close(sock); sock = -1; return -1; }
    setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    my_port = (uint16_t)port;
    running = 1;
    if (pthread_create(&t, NULL, loop, NULL)) { running = 0; return -1; }
    pthread_detach(t);
    if (!pthread_create(&t, NULL, playout, NULL)) pthread_detach(t);
    fprintf(stderr, "drop in: port %d\n", port);
    return 0;
}
