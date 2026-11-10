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
 *  - the one that answers, for a run that a round it won started, tells the network "on" while the run lasts (copies
 *    at 0/100/300 ms: Wi-Fi drops broadcasts, then every KEEP_MS) and "off" when it ends, to every member at once
 *    (arb_tell(NULL)).  A button press or an announcement tells nobody: no round, nobody who heard it.  A follow-up
 *    question (continue_conversation) or a wake word that cut the reply starts the next run from the IDLE that ends
 *    this one (main.c core_pipeline_finish): duck_follow bridges it, else the thread may wake between the two and say
 *    "off" and "on" again.  Home Assistant's media (play_media: SPEAKING for as long as a radio plays) is no run:
 *    "Alexa, play the radio" would otherwise hold the others down for the whole stream;
 *  - a loser takes an "on" within ACCEPT_MS of its round from a member that claimed in that round
 *    (arb_round_claimed: it heard the talker too; one that answered a wake word of its own elsewhere did not), up to
 *    NFOR of them (two may both answer, each sure of itself: Home Assistant takes one, the other ends soon), and holds
 *    the duck HOLD_MS past each one's last "on", until its "off": a lost "off" costs HOLD_MS at most, a winner that
 *    says nothing (an older build) LOST_WAIT_MS after the round's decision.
 *
 * Each Echo ducks by its own music_duck.  The setting duck_for_others (on) gates the ducking side: the answering side
 * always tells, and each Echo decides for itself.  Our protocol only, Kiosk Satellite's has no messages.  Older builds
 * drop a T_MSG addressed to everyone (not their id), and Drop In never sees one (main.c arb_message).
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

#define M_DUCK         0x40         /* the message's kind */
#define KEEP_MS        2000         /* "on" again this often while our run lasts */
#define HOLD_MS        6500         /* the duck held past each "on": three in a row lost */
#define LOST_WAIT_MS   1500         /* after a round we lost: the winner's first "on" should be in by then */
#define ACCEPT_MS      5000         /* an "on" counts within this of our round's start (its keep-alives do too) */
#define WON_MS         3000         /* a run that starts within this of a round we won is that round's */
#define DUCK_BRIDGE_MS 5000         /* duck_follow: the next run is LISTENING in the same call (measured: 0 ms) */
#define NFOR           4
static const int copy_ms[] = { 0, 100, 300 };
#define NCOPY ((int)(sizeof copy_ms / sizeof copy_ms[0]))

static pthread_mutex_t dk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;                  /* on the monotonic clock (HA sets the wall clock), made by duck_start: */
static atomic_int started;                  /* states and messages may come before it, and the loop reads them then */
/* the sending side: busy as main.c says, bridge_until across a follow-up; run: busy or bridged, tells: it is a run a
 * round we won started (won_at); told: what we say now ("on" 1, "off" 0); copy: the copy due next of it */
static int busy, run, tells, told, copy;
static long long bridge_until, won_at, copy_from, next_at;
/* the ducking side: our last round's start and its own duck; the members we duck for (until 0: a free slot) */
static long long heard_at, round_until;
static struct { uint8_t id[8]; char node[64]; long long until; } fr[NFOR];
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

static void recompute(long long now)        /* dk held */
{
    long long u = round_until > now ? round_until : 0;
    for (int i = 0; i < NFOR; i++) if (fr[i].until > u) u = fr[i].until;
    atomic_store(&until, u);
}

static const char *still(void)              /* dk held: a member we still duck for, or NULL */
{
    for (int i = 0; i < NFOR; i++) if (fr[i].until) return fr[i].node;
    return NULL;
}

static void wake_loop(void) { if (atomic_load(&started)) pthread_cond_signal(&cv); }     /* dk held */

static void *loop(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&dk);
    for (;;) {
        long long now = mono_ms(), wake = now + 60000;
        int r = busy || now < bridge_until;
        if (r && !run) { tells = won_at && now - won_at < WON_MS; won_at = 0; }
        run = r;
        int want = run && tells;
        if (want != told) { told = want; copy = 0; copy_from = next_at = now; }
        int gone = -1;
        for (int i = 0; i < NFOR; i++) if (fr[i].until && now >= fr[i].until) { gone = i; break; }
        if (gone >= 0) {
            char n[64]; snprintf(n, sizeof n, "%s", fr[gone].node); fr[gone].until = 0; recompute(now);
            const char *s = still(); char left[64]; snprintf(left, sizeof left, "%s", s ? s : "");
            pthread_mutex_unlock(&dk);
            if (left[0]) fprintf(stderr, "duck: nothing from %s for %d ms, music stays down for %s\n", n, HOLD_MS, left);
            else fprintf(stderr, "duck: nothing from %s for %d ms, music back\n", n, HOLD_MS);
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
        if (!busy && bridge_until > now && bridge_until < wake) wake = bridge_until;
        for (int i = 0; i < NFOR; i++) if (fr[i].until && fr[i].until < wake) wake = fr[i].until;
        struct timespec ts = { (time_t)(wake / 1000), (long)(wake % 1000) * 1000000 };
        pthread_cond_timedwait(&cv, &dk, &ts);
    }
    return NULL;
}

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
        if (!set) {
            pthread_mutex_lock(&dk);
            memset(fr, 0, sizeof fr); heard_at = round_until = 0; atomic_store(&until, 0);
            pthread_mutex_unlock(&dk);
        }
    }
    return atomic_load(&on);
}

void duck_busy(int b)
{
    pthread_mutex_lock(&dk);
    if (busy != !!b) { busy = !!b; if (busy) bridge_until = 0; wake_loop(); }
    pthread_mutex_unlock(&dk);
}

void duck_follow(void)
{
    pthread_mutex_lock(&dk);
    bridge_until = mono_ms() + DUCK_BRIDGE_MS;
    pthread_mutex_unlock(&dk);
}

void duck_round(long long due)
{
    if (!atomic_load(&on) || arb_mode(-1) != ARB_HASSMIC) return;
    long long now = mono_ms();
    pthread_mutex_lock(&dk);
    heard_at = now; round_until = due + LOST_WAIT_MS;
    recompute(now);
    pthread_mutex_unlock(&dk);
}

void duck_won(int round)
{
    long long now = mono_ms();
    pthread_mutex_lock(&dk);
    heard_at = round_until = 0;
    recompute(now);                             /* ducking for another Echo still: its "off" or HOLD_MS ends that */
    if (round) {                                /* a run of ours already going (a reply this wake word cut) tells now */
        won_at = now;
        if (run) { tells = 1; won_at = 0; wake_loop(); }
    }
    pthread_mutex_unlock(&dk);
}

void duck_message(const unsigned char from[8], const char *node, const unsigned char *p, size_t n)
{
    if (n != 2 || p[0] != M_DUCK || !atomic_load(&on)) return;
    int claimed = arb_round_claimed(from);      /* before dk: arb's lock */
    long long now = mono_ms(); int news = 0, done = 0, i, k = -1;
    const char *who = node[0] ? node : "another Echo"; char left[64] = "";
    pthread_mutex_lock(&dk);
    for (i = 0; i < NFOR; i++) if (fr[i].until && !memcmp(fr[i].id, from, 8)) break;
    if (p[1]) {
        if (i == NFOR && heard_at && now - heard_at < ACCEPT_MS && claimed) {
            for (int j = 0; j < NFOR; j++) if (k < 0 || fr[j].until < fr[k].until) k = j;   /* free, else the oldest */
            i = k; news = 1;
            memcpy(fr[i].id, from, 8); snprintf(fr[i].node, sizeof fr[i].node, "%s", who);
        }
        if (i < NFOR) { fr[i].until = now + HOLD_MS; recompute(now); wake_loop(); }
    } else if (i < NFOR) {
        done = 1; fr[i].until = 0; recompute(now);
        const char *s = still(); if (s) snprintf(left, sizeof left, "%s", s);
    }
    pthread_mutex_unlock(&dk);
    if (news) fprintf(stderr, "duck: %s answers what we heard too, music down until it is done\n", who);
    if (done && left[0]) fprintf(stderr, "duck: %s is done, music stays down for %s\n", who, left);
    else if (done) fprintf(stderr, "duck: %s is done, music back\n", who);
}

int duck_active(void)
{
    if (core_state() != IDLE) return 1;
    return atomic_load(&on) && mono_ms() < atomic_load(&until);
}
