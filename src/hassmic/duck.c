/*
 * Music ducked on the other Echos that heard the wake word (issue #165).
 *
 * Two Echos in one open room playing the same music (a Sendspin group): the one that answers turns its music down
 * (music_duck, resample.h), the other played on at full volume, over the reply and over the command itself, which its
 * music reaches the answering Echo's microphones with.  A Home Assistant automation could set the other one's volume,
 * but HA hears of the run only once the round is decided and the pipeline runs (the window, 150-1000 ms, plus the
 * link), it sets the real volume (Music Assistant keeps it, the ring shows it going down and up, a change made
 * meanwhile is lost on the way back) and it has to be told which Echos share a room.  The round knows that better: an
 * Echo that took part in it heard the talker, so its music is heard where the talker is; one in another room never
 * claimed.  So:
 *
 *  - an Echo with a round of its own ducks from the wake word on (duck_round), won or lost: the winner's music goes
 *    down a window earlier than its state would take it, the loser's at once;
 *  - the one that answers tells the network "on" while its run lasts (copies at 0/100/300 ms: Wi-Fi drops broadcasts,
 *    then every KEEP_MS) and "off" when it ends, to every member at once (arb_tell(NULL)).  Home Assistant's media on
 *    it (play_media: SPEAKING for as long as a radio stream plays) is no run: "Alexa, play the radio" would otherwise
 *    hold the other Echos down for the whole stream;
 *  - a loser takes an "on" within ACCEPT_MS of its round, from the member whose "answers" came in that round
 *    (arb_round_answered; from any while none came: all three copies of it may be lost), and holds the duck HOLD_MS
 *    past each one, until that Echo's "off": a lost "off" costs HOLD_MS at most, a winner that says nothing (an older
 *    build) LOST_WAIT_MS after the round's decision.  After an "off" the same Echo's "on" counts for ACCEPT_MS more:
 *    a follow-up (continue_conversation) may start with a new run, and between the two runs it was idle;
 *
 * Each Echo ducks by its own music_duck.  The setting duck_for_others (on) gates the ducking side only: the answering
 * side always tells (a broadcast every 2 s while it is busy and has members), and each Echo decides for itself.  Our
 * protocol only, Kiosk Satellite's has no messages.  Older builds drop a T_MSG addressed to everyone (not their id).
 */
#include "duck.h"
#include "arb.h"
#include "core.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define M_DUCK       0x40           /* the message's kind: Drop In's are 1-5 (dropin.c), and 44 bytes at least */
#define KEEP_MS      2000           /* "on" again this often while our run lasts */
#define HOLD_MS      6500           /* the duck held past each "on": three in a row lost */
#define LOST_WAIT_MS 1500           /* after a round we lost: the winner's first "on" should be in by then */
#define ACCEPT_MS    5000           /* an "on" counts within this of our round's start (its keep-alives do too) */
static const int copy_ms[] = { 0, 100, 300 };
#define NCOPY ((int)(sizeof copy_ms / sizeof copy_ms[0]))

static pthread_mutex_t dk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;                  /* on the monotonic clock (HA sets the wall clock), made by duck_start: */
static atomic_int started;                  /* states and messages may come before it, and the loop reads them then */
static int busy, told, copy;                /* told: what we say now ("on" 1, "off" 0); copy: the copy due next */
static long long copy_from, next_at;        /* the first copy's time; the next one's, 0 none */
static long long heard_at;                  /* our last round's start: an "on" within ACCEPT_MS counts */
static long long round_until;               /* our round's own duck: an "off" of another Echo's leaves it */
static uint8_t for_id[8];                   /* for_on: we duck for that member (off_at: it said "off" then, 0 not; its */
static char for_node[64];                   /* "on" counts again until ACCEPT_MS after); for_node its name, for the log */
static int for_on;
static long long off_at;
static atomic_int on = 1;                   /* the setting */
static _Atomic long long until;             /* the music down until then (monotonic ms) */

static long long mono_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void tell(int what)                  /* not under dk: arb's lock */
{
    if (!arb_running() || arb_mode(-1) != ARB_HASSMIC || arb_peers() <= 0) return;
    uint8_t m[2] = { M_DUCK, (uint8_t)what };
    arb_tell(NULL, m, sizeof m);
}

static void *loop(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&dk);
    for (;;) {
        long long now = mono_ms(), wake = now + 60000;
        if (busy != told) { told = busy; copy = 0; copy_from = next_at = now; }
        if (for_on && now >= atomic_load(&until)) {
            char n[64]; snprintf(n, sizeof n, "%s", for_node); for_on = 0;
            pthread_mutex_unlock(&dk);
            fprintf(stderr, "duck: nothing from %s for %d ms, music back\n", n, HOLD_MS);
            pthread_mutex_lock(&dk);
            continue;
        }
        if (next_at && now >= next_at) {
            int what = told;
            if (++copy < NCOPY) next_at = copy_from + copy_ms[copy];
            else next_at = what ? now + KEEP_MS : 0;
            pthread_mutex_unlock(&dk);
            tell(what);
            pthread_mutex_lock(&dk);
            continue;
        }
        if (next_at && next_at < wake) wake = next_at;
        if (for_on && atomic_load(&until) < wake) wake = atomic_load(&until);
        struct timespec ts = { (time_t)(wake / 1000), (long)(wake % 1000) * 1000000 };
        pthread_cond_timedwait(&cv, &dk, &ts);
    }
    return NULL;
}

static void wake_loop(void) { if (atomic_load(&started)) pthread_cond_signal(&cv); }     /* dk held */

int duck_start(void)
{
    pthread_condattr_t ca; pthread_attr_t a; pthread_t t;
    pthread_mutex_lock(&dk);
    if (atomic_load(&started)) { pthread_mutex_unlock(&dk); return 0; }
    pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&cv, &ca); pthread_condattr_destroy(&ca);
    pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &a, loop, NULL);
    pthread_attr_destroy(&a);
    if (!r) atomic_store(&started, 1);
    pthread_mutex_unlock(&dk);
    return r ? -1 : 0;
}

int duck_for_others(int set)
{
    if (set >= 0) {
        atomic_store(&on, set != 0);
        if (!set) { pthread_mutex_lock(&dk); for_on = 0; heard_at = off_at = round_until = 0; atomic_store(&until, 0); pthread_mutex_unlock(&dk); }
    }
    return atomic_load(&on);
}

void duck_busy(int b)
{
    pthread_mutex_lock(&dk);
    if (busy != !!b) { busy = !!b; wake_loop(); }
    pthread_mutex_unlock(&dk);
}

void duck_round(long long due)
{
    if (!atomic_load(&on) || arb_mode(-1) != ARB_HASSMIC) return;
    long long now = mono_ms(), u = due + LOST_WAIT_MS;
    pthread_mutex_lock(&dk);
    heard_at = now; round_until = u;
    if (atomic_load(&until) < u) atomic_store(&until, u);
    pthread_mutex_unlock(&dk);
}

void duck_won(void)
{
    pthread_mutex_lock(&dk);
    heard_at = round_until = 0;
    if (!for_on) atomic_store(&until, 0);      /* ducking for another Echo still: its "off" or HOLD_MS ends that */
    pthread_mutex_unlock(&dk);
}

int duck_message(const unsigned char from[8], const char *node, const unsigned char *p, size_t n)
{
    if (n != 2 || p[0] != M_DUCK) return 0;
    if (!atomic_load(&on)) return 1;
    uint8_t w[8]; int winner = arb_round_answered(w) == 0;     /* before dk: arb's lock */
    long long now = mono_ms(); int news = 0, again = 0, done = 0;
    pthread_mutex_lock(&dk);
    /* after: its follow-up; a round of ours since its "off" is a new wake word, its winner may be another Echo */
    int same = !memcmp(for_id, from, 8), now_for = for_on && same;
    int after = !for_on && same && off_at && now - off_at < ACCEPT_MS && !(heard_at > off_at);
    if (p[1]) {
        if (now_for || after || (heard_at && now - heard_at < ACCEPT_MS && (!winner || !memcmp(w, from, 8)))) {
            news = !now_for; again = after; for_on = 1; heard_at = off_at = 0;
            memcpy(for_id, from, 8); snprintf(for_node, sizeof for_node, "%s", node[0] ? node : "another Echo");
            if (atomic_load(&until) < now + HOLD_MS) atomic_store(&until, now + HOLD_MS);
            wake_loop();
        }
    } else if (now_for) {
        done = 1; for_on = 0; off_at = now;
        atomic_store(&until, round_until > now ? round_until : 0);    /* a round of our own runs: its duck stays */
    }
    pthread_mutex_unlock(&dk);
    const char *who = node[0] ? node : "another Echo";
    if (news) fprintf(stderr, again ? "duck: %s answers again (a follow-up), music down\n" : "duck: %s answers what we heard too, music down until it is done\n", who);
    if (done) fprintf(stderr, "duck: %s is done, music back\n", who);
    return 1;
}

int duck_active(void)
{
    if (core_state() != IDLE) return 1;
    return atomic_load(&on) && mono_ms() < atomic_load(&until);
}
