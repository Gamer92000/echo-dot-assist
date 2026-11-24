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
 * A packet sent again passes the AEAD as it did the first time (its nonce is in it), and anyone on the LAN can capture
 * one and send it again from anywhere: so a frame is taken once (a window of the last RX_WIN numbers, as IPsec's,
 * RFC 4303 3.4.3), and only a frame newer than every other moves the address and keeps the call alive.  Replays had
 * the Echo play the far side's earlier words, sent this side's room to the replaying host, and kept a call open whose
 * other Echo had gone (D1 of a review of e8d52b9).  The receiving side's rule alone: the packets are the same as ever
 *
 * The audio itself (codec, gate, echo suppressor, jitter buffer, playout, the call's microphone) is call.c's.
 */
#include "dropin.h"
#include "arb.h"
#include "call.h"
#include "core.h"
#include "ws.h"
#include "../third_party/monocypher.h"
#include <arpa/inet.h>
#include <errno.h>
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

#define MAXPKT      CALL_PKT
#define INVITE_MS   250
#define ANSWER_MS   4000                /* no word from the callee at all: not there */
#define RING_MS     30000               /* ask: how long it rings */
#define CALLER_MS   5000                /* ringing, the caller's invites stopped: it gave up */
#define LOST_MS     5000                /* live, no audio for this long: the other Echo is gone */
/* HASSMIC_DROPIN_TIMES="<answer ms> <lost ms>" (tests: an Echo that vanishes or never answers, without the waits) */
static int answer_ms = ANSWER_MS, lost_ms = LOST_MS;

enum { M_INVITE = 1, M_ACCEPT, M_RINGING, M_REFUSE, M_BYE };
const char *const dropin_states[4] = { "idle", "calling", "ringing", "connected" };
const char *const dropin_results[DROPIN_NRESULTS] = { "connected", "ringing", "busy", "do_not_disturb", "microphones_off", "drop_in_off",
                                                      "declined", "not_answered", "no_answer", "not_found", "busy_here",
                                                      "microphones_off_here", "cancelled", "failed" };

static pthread_mutex_t dl = PTHREAD_MUTEX_INITIALIZER;   /* everything below; taken after core_lock, before arb's */
static atomic_int notify;                                 /* the loop tells the core (core_dropin): callers may hold core_lock */
static int sock = -1, running, enabled = 1, ask;
static uint16_t my_port;
static uint8_t self_id[8];
static struct {
    int state, outgoing, gen, keyed, rang;              /* gen: one per call; rang: the callee said it rings */
    int invited;                                        /* the first invite went out (once the core knew: tell_core) */
    uint8_t id[8], peer[8];
    char node[64];
    uint32_t ip; uint16_t port;                         /* the other Echo's audio address (network byte order ip) */
    uint8_t esk[32], epk[32], key[32];
    long long since, sig_at, rx_at, live_at;
    int media;                                          /* call.c's generation while connected, -1 before */
    /* the frames taken (on_audio): the newest number, and a bit per number RX_WIN back from it (bit 0 the newest) */
    uint32_t rx_hi, rx_seen; int rx_any;
} c = { .media = -1 };
/* call.c's jitter buffer is 32 frames: a frame further back is dropped there all the same */
#define RX_WIN 32
static struct { int left; long long at; uint8_t to[8]; uint8_t p[ARB_MSG_MAX]; size_t n; } bye;
/* Calls that ended: an invite the caller repeated before it heard of the end (a decline, a hang-up while it rang) must
 * not start the call again */
static uint8_t ended[4][8]; static unsigned ended_n;
/* What the core is told, in order, by the loop (tell_core): every state a call passes through and what came of every
 * call this Echo made.  With only the state the loop found then (the first version), a call refused within the loop's
 * 20 ms (the other Echo in do not disturb, on the same LAN) went from idle to idle: Home Assistant never saw "calling",
 * and the Echo made no sound.  state -1: a result alone (ringing, or a call refused here: here 1) */
#define EVQ 16                                          /* a call makes three or four */
static struct ev { int state, outgoing, result, here, gen, late; char node[64]; } evq[EVQ];
static unsigned ev_in, ev_out;
/* HASSMIC_DROPIN_LATE_IDLE=<file> (tests): while the file exists, a call's end lets go of the claim at once and the core
 * hears "idle" LATE_IDLE_MS later, the order before C1's fix (a review of fc43c40): a call begun in between is then kept
 * shown by core_call's own guard alone, which nothing else reaches since */
#define LATE_IDLE_MS 800
static const char *late_idle;
static int last_result = -1; static unsigned attempts;  /* for the settings page */

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* dl held: a state for the core (-1: none) and what came of the call this Echo made (result -1: nothing yet; here: it
 * was refused here, before it started) */
static void ev_push(int state, const char *node, int outgoing, int result, int here)
{
    /* Full (the core held up while Home Assistant or the page placed call after call): an event the core has not seen
     * goes, the oldest result alone first, else the oldest state of a call that is over but the last "idle".  Never the
     * call's own states (its "calling" sends the first invite: tell_core) nor the last "idle": the oldest went whatever
     * it was, and with the "idle" the core showed a call that was over (D5 of a review of e8d52b9).  Those are four at
     * most of EVQ.  Of a finished call's states its "calling"/"live" go before its "idle": without that "idle" the core
     * went from "live" to the next call's "calling" and never ran what an end does after "live" (music_unyield: a phone
     * without remote control stayed silent; a review of e10001c) */
    if (ev_in - ev_out == EVQ) {
        unsigned drop = ev_in, last_idle = ev_in;
        for (unsigned i = ev_out; i != ev_in; i++) if (evq[i % EVQ].state == DROPIN_IDLE) last_idle = i;
        for (int pass = 0; pass < 3 && drop == ev_in; pass++)
            for (unsigned i = ev_out; i != ev_in; i++) {
                const struct ev *o = &evq[i % EVQ];
                int finished = i != last_idle && !(o->state >= 0 && c.state != DROPIN_IDLE && o->gen == c.gen);
                if (pass == 0 ? o->state < 0 : pass == 1 ? finished && o->state != DROPIN_IDLE : finished) { drop = i; break; }
            }
        if (drop == ev_in) drop = ev_out;                /* (cannot be: kept are four at most) */
        for (unsigned i = drop; i != ev_out; i--) evq[i % EVQ] = evq[(i - 1) % EVQ];      /* the older ones up by one */
        ev_out++;
    }
    struct ev *e = &evq[ev_in++ % EVQ];
    e->state = state; e->outgoing = outgoing; e->result = result; e->here = here; e->gen = c.gen; e->late = 0;
    snprintf(e->node, sizeof e->node, "%s", node);
    if (result >= 0) last_result = result;
    atomic_store(&notify, 1);
}

/* The other Echo's reason (M_REFUSE: the same words in every build with Drop In) as a result */
static int refusal(const char *why)
{
    static const struct { const char *why; int r; } map[] = {
        { "busy in another Drop In", DROPIN_R_BUSY }, { "do not disturb is on", DROPIN_R_DND },
        { "the microphones are off", DROPIN_R_MUTED }, { "Drop In is off there", DROPIN_R_OFF },
        { "declined", DROPIN_R_DECLINED }, { "not answered", DROPIN_R_NOT_ANSWERED },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcmp(why, map[i].why)) return map[i].r;
    return DROPIN_R_FAILED;
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

static void send_audio(const uint8_t *frame, size_t len, uint32_t seq, void *arg);

static void go_live(long long now)
{
    ev_push(DROPIN_LIVE, c.node, c.outgoing, c.outgoing ? DROPIN_R_CONNECTED : -1, 0);
    c.state = DROPIN_LIVE; c.live_at = c.rx_at = now; c.rx_any = 0; c.rx_seen = 0;
    c.media = call_begin(CALL_DROPIN, CALL_CODEC_OPUS, send_audio, NULL);
    fprintf(stderr, "drop in: connected with %s\n", c.node);
}

/* result: what came of a call this Echo made that had not connected (ignored for any other) */
static void end_call(const char *why, int tell, int result)
{
    if (c.state == DROPIN_IDLE) return;
    ev_push(DROPIN_IDLE, "", c.outgoing, c.state == DROPIN_CALLING && c.outgoing ? result : -1, 0);
    if (c.state == DROPIN_LIVE) {
        struct call_stats st; call_stats(&st); call_end(c.media); c.media = -1;
        fprintf(stderr, "drop in: ended with %s after %lld s (%s); %u frames in, %u played, %u of them missing "
                "(%u came too late), longest gap %lld ms\n", c.node, (now_ms() - c.live_at) / 1000, why, st.rx, st.played,
                st.lost, st.late, st.gap_max);
    }
    else fprintf(stderr, "drop in: %s %s: %s\n", c.outgoing ? "call to" : "call from", c.node, why);
    if (tell) {
        bye.n = msg(bye.p, M_BYE, why); memcpy(bye.to, c.peer, 8);
        arb_tell(bye.to, bye.p, bye.n); bye.left = 2; bye.at = now_ms();
    }
    memcpy(ended[ended_n++ % 4], c.id, 8);
    c.state = DROPIN_IDLE; c.gen++; c.keyed = 0; c.rang = 0;
    /* The claim (call.h) stays Drop In's until the core has heard "idle" (tell_core): let go here, a plugin's call
     * could begin in the 20 ms before the loop tells the core, and this call's "idle" then took the ring, the sounds,
     * the screen and the music's pause away from it while it rang or heard the room (C1 of a review of fc43c40) */
    if (late_idle && !access(late_idle, F_OK)) { evq[(ev_in - 1) % EVQ].late = 1; call_release(CALL_DROPIN); }      /* (tests) */
    crypto_wipe(c.esk, sizeof c.esk); crypto_wipe(c.key, sizeof c.key);
}

static void fresh_call(const uint8_t peer[8], const char *node, uint32_t ip, int outgoing)
{
    memset(&c.id, 0, sizeof c.id);
    c.outgoing = outgoing; c.gen++; c.keyed = 0; c.rang = 0; c.invited = 0;
    memcpy(c.peer, peer, 8); snprintf(c.node, sizeof c.node, "%s", node); c.ip = ip; c.port = 0;
    ws_random(c.esk, 32); crypto_x25519_public_key(c.epk, c.esk);
    c.since = c.sig_at = now_ms();
}

/* ---------------------------------------------------------------- notifications (the loop's, not dl) */

/* Everything that happened, in order.  A call this Echo makes sends its first invite only once the core has been told:
 * Home Assistant then has "calling" before the other Echo can answer, so every call shows there, refused ones too */
static void tell_core(void)
{
    for (;;) {
        struct ev e;
        pthread_mutex_lock(&dl);
        if (ev_out == ev_in) {
            /* The core has heard all there is, the last of it "idle" or no call: the claim goes, if it is Drop In's.  Not
             * at each "idle": a call placed on the claim the last one left (dropin_call, an invite) before that call's
             * "idle" was told would lose it there, and its own "calling" and "idle" then came from a call without the
             * claim, which core_call does not show: Home Assistant never saw it and the Echo made no sound (D6 of a
             * review of e8d52b9).  Under dl, as dropin_call takes it: no call of Drop In's begins in between */
            if (c.state == DROPIN_IDLE) call_release(CALL_DROPIN);
            pthread_mutex_unlock(&dl);
            break;
        }
        e = evq[ev_out++ % EVQ];
        pthread_mutex_unlock(&dl);
        if (e.late) usleep(LATE_IDLE_MS * 1000);
        if (e.state >= 0) core_call(CALL_DROPIN, e.state, e.node, e.outgoing);
        if (e.result >= 0) core_dropin_result(e.result, e.node, e.here);
        if (e.state == DROPIN_CALLING && e.outgoing) {
            pthread_mutex_lock(&dl);
            if (c.state == DROPIN_CALLING && c.outgoing && c.gen == e.gen && !c.invited) {
                c.invited = 1; c.since = c.sig_at = now_ms();
                send_msg(M_INVITE, NULL);
            }
            pthread_mutex_unlock(&dl);
        }
    }
}

/* ---------------------------------------------------------------- interface */

int dropin_running(void) { return running; }

int dropin_enable(int set)
{
    pthread_mutex_lock(&dl);
    if (set >= 0 && set != enabled) { enabled = set != 0; if (!enabled && c.state) end_call("Drop In switched off", 1, DROPIN_R_CANCELLED); }
    int r = enabled;
    pthread_mutex_unlock(&dl);
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

int dropin_last(unsigned *attempt)
{
    pthread_mutex_lock(&dl);
    int r = last_result;
    if (attempt) *attempt = attempts;
    pthread_mutex_unlock(&dl);
    return r;
}

/* A call refused here has a result all the same, which the loop reports (callers may hold core_lock) */
int dropin_call(const char *target, char *err, size_t errsz)
{
    char node[64]; uint8_t id[8]; unsigned ip; int r = -1;
    core_node_of(target, node);
    if (!running) { snprintf(err, errsz, "Drop In is not running on this Echo"); return -1; }       /* no loop to tell */
    const char *no = core_call_refusal(0);
    int self = !strcmp(node, core_node_name()), member = !self && !arb_member(node, id, &ip);
    pthread_mutex_lock(&dl);
    attempts++;
    if (self) { snprintf(err, errsz, "that is this Echo"); r = DROPIN_R_FAILED; }
    else if (!member) { snprintf(err, errsz, "no Echo \"%s\" in this Echo's network", node); r = DROPIN_R_NOT_FOUND; }
    else if (!enabled) { snprintf(err, errsz, "Drop In is off on this Echo"); r = DROPIN_R_FAILED; }
    else if (no) { snprintf(err, errsz, "%s", no); r = DROPIN_R_MUTED_HERE; }
    else if (c.state) { snprintf(err, errsz, "already in a Drop In with %s", c.node); r = DROPIN_R_BUSY_HERE; }
    /* a plugin's call (call.h's claim): last, the claim is this call's from here on */
    else if (call_claim(CALL_DROPIN)) { snprintf(err, errsz, "already in another call"); r = DROPIN_R_BUSY_HERE; }
    if (r >= 0) { ev_push(-1, node, 1, r, 1); pthread_mutex_unlock(&dl); return -1; }
    fresh_call(id, node, ip, 1);
    ws_random(c.id, 8);
    c.state = DROPIN_CALLING;
    fprintf(stderr, "drop in: calling %s\n", node);
    ev_push(DROPIN_CALLING, node, 1, -1, 0);            /* the invite follows once the core knows (tell_core) */
    pthread_mutex_unlock(&dl);
    return 0;
}

void dropin_hangup(const char *why)
{
    pthread_mutex_lock(&dl);
    if (c.state == DROPIN_RINGING) { send_msg(M_REFUSE, "declined"); end_call(why, 0, -1); }
    else end_call(why, c.state != DROPIN_CALLING || c.invited, DROPIN_R_CANCELLED);    /* no bye for an invite never sent */
    pthread_mutex_unlock(&dl);
}

static void answer(void)              /* dl held: the callee takes a ringing call */
{
    send_msg(M_ACCEPT, NULL);
    go_live(now_ms());
}

int dropin_answer(void)
{
    pthread_mutex_lock(&dl);
    int st = c.state;
    if (st == DROPIN_RINGING) answer();
    pthread_mutex_unlock(&dl);
    return st == DROPIN_RINGING;
}

int dropin_button(void)
{
    pthread_mutex_lock(&dl);
    int st = c.state;
    if (st == DROPIN_RINGING) answer();
    else if (st) end_call("action button", 1, DROPIN_R_CANCELLED);
    pthread_mutex_unlock(&dl);
    return st != DROPIN_IDLE;
}

void dropin_message(const unsigned char from[8], const char *node, unsigned ip, const unsigned char *p, size_t n)
{
    uint8_t id[8], epk[32]; char reason[64] = ""; unsigned port; int kind;
    if (n < 44 || n < (size_t)44 + p[43]) return;
    kind = p[0]; memcpy(id, p + 1, 8); port = p[9] | p[10] << 8; memcpy(epk, p + 11, 32);
    snprintf(reason, sizeof reason, "%.*s", p[43], (const char *)p + 44);
    for (char *q = reason; *q; q++) if ((unsigned char)*q < 0x20) *q = ' ';
    /* before dl: core_lock comes first */
    const char *no = kind == M_INVITE ? core_call_refusal(1) : NULL;
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
        /* A plugin's call has the claim (call.h): busy as for another Drop In, in the words every build with Drop In maps
         * to "busy" (refusal()); words of its own would be "failed" on the Echos released before.  Drop In's own claim
         * with no call is the last call's, kept until the core heard its end (tell_core): this call takes it over */
        { int other = call_claimed() != CALL_NOBODY && call_claimed() != CALL_DROPIN;
        if (!enabled || no || c.state || !port || !node[0] || other) {
            const char *why = !enabled ? "Drop In is off there" : no ? no : c.state || other ? "busy in another Drop In" : "no address";
            fprintf(stderr, "drop in: call from %s refused: %s\n", node[0] ? node : "an Echo", why);
            send_to(from, id, M_REFUSE, why);
            break;
        } }
        fresh_call(from, node, ip, 0);
        memcpy(c.id, id, 8); c.port = htons((uint16_t)port);
        if (derive(epk)) { send_to(from, id, M_REFUSE, "no key"); c.state = DROPIN_IDLE; break; }
        /* (taken between the look above and here: refused as busy, as above) */
        if (call_claim(CALL_DROPIN)) { c.keyed = 0; crypto_wipe(c.key, sizeof c.key); send_to(from, id, M_REFUSE, "busy in another Drop In"); break; }
        if (ask) { c.state = DROPIN_RINGING; ev_push(DROPIN_RINGING, c.node, 0, -1, 0);
                   fprintf(stderr, "drop in: %s calls, ringing until the action button\n", node); send_msg(M_RINGING, NULL); }
        else { fprintf(stderr, "drop in: %s drops in\n", node); answer(); }
        break;
    case M_RINGING:
        if (same && c.outgoing && c.state == DROPIN_CALLING) {
            if (!c.rang) { fprintf(stderr, "drop in: %s rings\n", c.node); ev_push(-1, c.node, 1, DROPIN_R_RINGING, 0); }
            c.rang = 1; c.sig_at = now; c.rx_at = now;
        }
        break;
    case M_ACCEPT:
        if (!same || !c.outgoing || c.state != DROPIN_CALLING) break;
        c.port = htons((uint16_t)port); c.ip = ip;
        if (!port || derive(epk)) { end_call("no key", 1, DROPIN_R_FAILED); break; }
        go_live(now);
        break;
    case M_REFUSE:
        if (same && c.outgoing && c.state == DROPIN_CALLING) {
            char w[80]; snprintf(w, sizeof w, "refused: %s", reason[0] ? reason : "no reason");
            end_call(w, 0, refusal(reason));
        }
        break;
    case M_BYE:
        if (same) { char w[80]; snprintf(w, sizeof w, "%s hung up%s%s", c.node, reason[0] ? ": " : "", reason); end_call(w, 0, DROPIN_R_FAILED); }
        break;
    }
    pthread_mutex_unlock(&dl);
    crypto_wipe(epk, sizeof epk);
}

/* ---------------------------------------------------------------- audio */

/* call.c's sending side: a frame encoded, sealed and sent, from the capture thread or call.c's call microphone thread
 * with call.c's tx_mu held (call.h); takes dl, never calls into call.c */
static void send_audio(const uint8_t *frame, size_t len, uint32_t seq, void *arg)
{
    uint8_t pkt[17 + MAXPKT + 16], nonce[24] = { 0 }, key[32]; struct sockaddr_in to;
    (void)arg;
    if (len > MAXPKT) return;
    pthread_mutex_lock(&dl);
    if (c.state != DROPIN_LIVE || !c.keyed || !c.port) { pthread_mutex_unlock(&dl); return; }
    memcpy(key, c.key, 32);
    memcpy(pkt, "HMD1", 4); memcpy(pkt + 4, c.id, 8); pkt[12] = (uint8_t)!c.outgoing;
    for (int i = 0; i < 4; i++) pkt[13 + i] = (uint8_t)(seq >> 8 * i);
    memset(&to, 0, sizeof to); to.sin_family = AF_INET; to.sin_addr.s_addr = c.ip; to.sin_port = c.port;
    pthread_mutex_unlock(&dl);
    memcpy(nonce, pkt + 4, 13);
    crypto_aead_lock(pkt + 17, pkt + 17 + len, key, nonce, pkt, 17, frame, len);
    crypto_wipe(key, sizeof key);
    if (sendto(sock, pkt, 17 + len + 16, 0, (struct sockaddr *)&to, sizeof to) < 0 && errno != EAGAIN) {
        static long long logged;
        if (now_ms() - logged > 5000) { logged = now_ms(); fprintf(stderr, "drop in: send: %s\n", strerror(errno)); }
    }
}

static void on_audio(const uint8_t *p, size_t n, const struct sockaddr_in *from)
{
    uint8_t nonce[24] = { 0 }, frame[MAXPKT]; uint32_t seq = 0; int media;
    if (n < 17 + 1 + 16 || n > 17 + MAXPKT + 16 || memcmp(p, "HMD1", 4)) return;
    for (int i = 0; i < 4; i++) seq |= (uint32_t)p[13 + i] << 8 * i;
    pthread_mutex_lock(&dl);
    if (c.state != DROPIN_LIVE || !c.keyed || memcmp(p + 4, c.id, 8) || p[12] != (uint8_t)c.outgoing) { pthread_mutex_unlock(&dl); return; }
    memcpy(nonce, p + 4, 13);
    if (crypto_aead_unlock(frame, p + n - 16, c.key, nonce, p, 17, p + 17, n - 17 - 16)) { pthread_mutex_unlock(&dl); return; }
    /* taken once (the head's comment): only after the AEAD, so that nothing but the other Echo's own frames moves it */
    uint32_t ahead = seq - c.rx_hi, back = c.rx_hi - seq;
    if (!c.rx_any || (int32_t)ahead > 0) {              /* the newest yet: the other Echo is there, and where it sends from */
        c.rx_seen = !c.rx_any || ahead >= RX_WIN ? 1 : c.rx_seen << ahead | 1;
        c.rx_hi = seq; c.rx_any = 1;
        c.rx_at = now_ms();
        c.ip = from->sin_addr.s_addr; c.port = from->sin_port;
    } else if (back >= RX_WIN || (c.rx_seen >> back & 1)) { pthread_mutex_unlock(&dl); return; }   /* too old, or taken already */
    else c.rx_seen |= 1u << back;                       /* late, not seen yet: played if call.c still can */
    media = c.media;
    pthread_mutex_unlock(&dl);
    call_rx(media, seq, frame, n - 17 - 16);
}

/* ---------------------------------------------------------------- the loop: audio in, retries, timeouts */

static void tick(void)
{
    long long now = now_ms();
    pthread_mutex_lock(&dl);
    switch (c.state) {
    case DROPIN_CALLING:                                /* timed from the first invite (tell_core) */
        if (!c.invited) break;
        if (!c.rang && now - c.since > answer_ms) end_call("no answer", 1, DROPIN_R_NO_ANSWER);
        else if (c.rang && now - c.since > RING_MS + answer_ms) end_call("not answered", 1, DROPIN_R_NOT_ANSWERED);
        else if (now - c.sig_at >= (c.rang ? 1000 : INVITE_MS)) { c.sig_at = now; send_msg(M_INVITE, NULL); }
        break;
    case DROPIN_RINGING:
        if (now - c.since > RING_MS) { send_msg(M_REFUSE, "not answered"); end_call("not answered", 0, -1); }
        else if (now - c.sig_at > CALLER_MS) end_call("the caller gave up", 0, -1);
        break;
    case DROPIN_LIVE:
        if (now - c.rx_at > lost_ms) {
            char why[40]; snprintf(why, sizeof why, "lost: no audio for %d s", lost_ms / 1000);
            end_call(why, 1, -1);
        }
        break;
    }
    if (bye.left && now - bye.at >= 100) { arb_tell(bye.to, bye.p, bye.n); bye.at = now; bye.left--; }
    pthread_mutex_unlock(&dl);
}

static void *loop(void *arg)
{
    uint8_t buf[1500];
    (void)arg;
    for (;;) {
        struct pollfd pf = { sock, POLLIN, 0 }; struct sockaddr_in from; socklen_t fl = sizeof from; ssize_t n;
        if (poll(&pf, 1, 20) > 0 && (pf.revents & POLLIN) && (n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl)) > 0)
            on_audio(buf, (size_t)n, &from);
        if (atomic_exchange(&notify, 0)) tell_core();
        tick();
    }
    return NULL;
}

int dropin_start(int port)
{
    struct sockaddr_in a; pthread_t t; int tos = 0xb8;      /* DSCP EF: Wi-Fi's voice queue (WMM) where the AP honours it */
    const char *times = getenv("HASSMIC_DROPIN_TIMES");
    if (!arb_running()) return -1;
    if (times) sscanf(times, "%d %d", &answer_ms, &lost_ms);
    late_idle = getenv("HASSMIC_DROPIN_LATE_IDLE");
    arb_self(self_id);
    if ((sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) { perror("drop in: socket"); return -1; }
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&a, sizeof a)) { perror("drop in: bind"); close(sock); sock = -1; return -1; }
    setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    my_port = (uint16_t)port;
    running = 1;
    if (pthread_create(&t, NULL, loop, NULL)) { running = 0; return -1; }
    pthread_detach(t);
    if (call_start()) fprintf(stderr, "drop in: no playout thread\n");
    fprintf(stderr, "drop in: port %d\n", port);
    return 0;
}
