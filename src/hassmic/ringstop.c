/*
 * "Stop" said near one Echo ends a timer ringing on another (issue #161).
 *
 * A ringing Echo claims a round with priority (main.c wake_heard: prio 2), and every Echo that heard "<wake word>,
 * stop" runs the keyword itself (stop_word), round won or not.  Neither helps when the ringing Echo never heard the
 * wake word: the talker is in the next room, or across a big one, or its own ring covered the word.  Then an Echo that
 * has nothing ringing gets the "stop", pauses its music if it plays (or hands "stop" to Home Assistant, which forgot
 * the finished timer), and the ring goes on.  So, with the setting timer_stop_from_others on the Echo that rings:
 *
 *  - while a timer rings it tells every member "ring on" (copies at 0/100/300 ms: Wi-Fi drops broadcasts, then every
 *    KEEP_MS) and "ring off" when it stops (three copies).  Every Echo keeps who says it rings, HOLD_MS past each
 *    "on": a lost "off" costs a forward to an Echo that no longer rings, which ignores it;
 *  - an Echo whose "stop" (Pryon's keyword, or a bare "stop" transcript) ended nothing of its own (no ring, reply,
 *    call, nor a run its wake word cut) and knows a member that rings sends "stop" to every member (three copies, one
 *    id: each Echo stops once) and treats the "stop" as one that ended something: its music plays on (the ring comes
 *    first, as on a stock Echo) and the wake word's run is dropped, as after a ring of its own.  Every Echo that heard
 *    the keyword does this, the round's loser too: the first copy to arrive ends the ring;
 *  - a member that rings, with the setting on, stops on it.
 *
 * Off (the default), an Echo says nothing of its rings, so nobody forwards to it, and a "stop" from elsewhere is
 * ignored: no Echo behaves differently until one has it on.  Every forwarding Echo needs this build, the ringing one
 * the setting; the setting is the ringing Echo's because it is that Echo's ring others may end.  Whole house: an Echo
 * cannot tell how near a member that did not hear the talker is (it took no part in the round), so a "stop" anywhere
 * ends every ring that says it may be ended.  timer_stop_any (any request stops a ring) stays local: "Alexa, what's the
 * weather" in the bedroom is no reason to end the kitchen's.  Our protocol only: Kiosk Satellite's has no messages.
 * Builds before issue #165 drop a T_MSG addressed to everyone (not their id), #165's take only the duck's kind.
 */
#include "ringstop.h"
#include "arb.h"
#include "core.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define M_RING   0x41               /* the messages' kinds (duck.c has 0x40): { M_RING, 1 on / 0 off } */
#define M_STOP   0x42               /* { M_STOP, id (4 bytes, little endian) } */
#define KEEP_MS  2000               /* "on" again this often while it rings */
#define HOLD_MS  6500               /* a member rings this long past its last "on": three in a row lost */
#define NRING    8                  /* members that ring at once, kept */
#define NSEEN    8                  /* stops taken, by sender and id: their copies end nothing more */
static const int copy_ms[] = { 0, 100, 300 };
#define NCOPY ((int)(sizeof copy_ms / sizeof copy_ms[0]))

static pthread_mutex_t rs = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;                   /* on the monotonic clock, made by ringstop_start */
static atomic_int started, on;              /* on: the setting, off by default */
/* the ringing side: ringing as main.c says; told what we say ("on" 1, "off" 0), copy: the copy due next of it */
static int ringing, told, copy;
static long long copy_from, next_at;
/* the forwarding side: the stop to send (its id; scopy: the copy due next, sfrom: when it was asked for) */
static uint32_t sid; static int scopy = NCOPY; static long long sfrom;
/* the receiving side: a stop taken, carried out by the loop (core_lock: not on arb's thread, whose socket waits meanwhile) */
static char stop_from[64]; static int stop_due;
/* who says it rings (until 0: a free slot), stops taken */
static struct { uint8_t id[8]; char node[64]; long long until; } rg[NRING];
static struct { uint8_t from[8]; uint32_t id; } seen[NSEEN]; static int nseen;

static long long mono_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int ours(void) { return arb_running() && arb_mode(-1) == ARB_HASSMIC && arb_peers() > 0; }   /* not under rs: arb's lock */

static void wake_loop(void) { if (atomic_load(&started)) pthread_cond_signal(&cv); }     /* rs held */

static void *loop(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&rs);
    for (;;) {
        long long now = mono_ms(), wake = now + 60000;
        int want = ringing && atomic_load(&on);
        if (want != told) { told = want; copy = 0; copy_from = next_at = now; }
        if (stop_due) {
            char why[96]; snprintf(why, sizeof why, "stop heard by %s", stop_from);
            stop_due = 0;
            pthread_mutex_unlock(&rs);                  /* core_lock comes first (alarm_set -> ringstop_ringing) */
            pthread_mutex_lock(&core_lock);
            core_alarm_stop_elsewhere(why);
            pthread_mutex_unlock(&core_lock);
            pthread_mutex_lock(&rs);
            continue;
        }
        if (next_at && now >= next_at) {
            uint8_t m[2] = { M_RING, (uint8_t)told };
            if (++copy < NCOPY) next_at = copy_from + copy_ms[copy];
            else next_at = told ? now + KEEP_MS : 0;
            pthread_mutex_unlock(&rs);
            if (ours()) arb_tell(NULL, m, sizeof m);
            pthread_mutex_lock(&rs);
            continue;
        }
        if (scopy < NCOPY && now >= sfrom + copy_ms[scopy]) {
            uint8_t m[5] = { M_STOP, (uint8_t)sid, (uint8_t)(sid >> 8), (uint8_t)(sid >> 16), (uint8_t)(sid >> 24) };
            scopy++;
            pthread_mutex_unlock(&rs);
            if (ours()) arb_tell(NULL, m, sizeof m);
            pthread_mutex_lock(&rs);
            continue;
        }
        if (next_at && next_at < wake) wake = next_at;
        if (scopy < NCOPY && sfrom + copy_ms[scopy] < wake) wake = sfrom + copy_ms[scopy];
        struct timespec ts = { (time_t)(wake / 1000), (long)(wake % 1000) * 1000000 };
        pthread_cond_timedwait(&cv, &rs, &ts);
    }
    return NULL;
}

int ringstop_start(void)
{
    pthread_condattr_t ca; pthread_attr_t a; pthread_t t;
    pthread_mutex_lock(&rs);
    if (atomic_load(&started)) { pthread_mutex_unlock(&rs); return 0; }
    pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&cv, &ca); pthread_condattr_destroy(&ca);
    pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &a, loop, NULL);
    pthread_attr_destroy(&a);
    if (!r) atomic_store(&started, 1);
    pthread_mutex_unlock(&rs);
    return r ? -1 : 0;
}

int ringstop_from_others(int set)
{
    if (set >= 0) {
        atomic_store(&on, set != 0);
        pthread_mutex_lock(&rs); wake_loop(); pthread_mutex_unlock(&rs);   /* ringing now: "on" or "off" at once */
    }
    return atomic_load(&on);
}

void ringstop_ringing(int r)
{
    pthread_mutex_lock(&rs);
    if (ringing != !!r) { ringing = !!r; wake_loop(); }
    pthread_mutex_unlock(&rs);
}

int ringstop_forward(char *who, size_t cap)
{
    long long now = mono_ms(); int n = 0;
    if (cap) who[0] = 0;
    if (!atomic_load(&started) || !ours()) return 0;   /* nothing would be sent: the "stop" stays this Echo's.  arb's lock
                                                        * under core_lock, as proto_esphome.c's arb_peers() */
    pthread_mutex_lock(&rs);
    for (int i = 0; i < NRING; i++) if (rg[i].until > now) { if (!n++ && cap) snprintf(who, cap, "%s", rg[i].node); }
    if (n) {
        sid = (uint32_t)now ^ (uint32_t)(now >> 32);    /* a new one per stop: the receivers take each once */
        scopy = 0; sfrom = now;
        wake_loop();
    }
    pthread_mutex_unlock(&rs);
    if (n > 1 && cap) { size_t l = strlen(who); snprintf(who + l, cap - l, " and %d more", n - 1); }
    return n;
}

void ringstop_message(const unsigned char from[8], const char *node, const unsigned char *p, size_t n)
{
    const char *who = node[0] ? node : "another Echo";
    long long now = mono_ms();
    if (n < 2 || (p[0] != M_RING && p[0] != M_STOP) || arb_mode(-1) != ARB_HASSMIC) return;   /* not arb's lock: its hooks run outside it */
    if (n == 2 && p[0] == M_RING) {
        int i, k = -1, news = 0, done = 0;
        pthread_mutex_lock(&rs);
        for (i = 0; i < NRING; i++) if (rg[i].until && !memcmp(rg[i].id, from, 8)) break;
        if (i < NRING && rg[i].until <= now) { rg[i].until = 0; i = NRING; }     /* ran out: a new ring */
        if (p[1]) {
            if (i == NRING) {
                for (int j = 0; j < NRING; j++) if (k < 0 || rg[j].until < rg[k].until) k = j;   /* free, else the oldest */
                i = k; news = 1;
                memcpy(rg[i].id, from, 8); snprintf(rg[i].node, sizeof rg[i].node, "%s", who);
            }
            rg[i].until = now + HOLD_MS;
        } else if (i < NRING) { rg[i].until = 0; done = 1; }
        pthread_mutex_unlock(&rs);
        if (news) fprintf(stderr, "ringstop: %s rings, a stop here ends it\n", who);
        if (done) fprintf(stderr, "ringstop: %s no longer rings\n", who);
        return;
    }
    if (n != 5 || p[0] != M_STOP || !atomic_load(&on)) return;
    uint32_t id = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
    pthread_mutex_lock(&rs);
    int again = 0;
    for (int i = 0; i < NSEEN; i++) if (seen[i].id == id && !memcmp(seen[i].from, from, 8)) again = 1;
    if (!again) {
        memcpy(seen[nseen].from, from, 8); seen[nseen].id = id; nseen = (nseen + 1) % NSEEN;
        snprintf(stop_from, sizeof stop_from, "%s", who); stop_due = 1; wake_loop();
    }
    pthread_mutex_unlock(&rs);
}
