/*
 * Receiving end of scripts/ota-push.sh.  Runs inside hassmic, i.e. unprivileged and network-facing, so it decides nothing:
 * it checks the bundle's signature (to refuse junk before it touches the disk), stores bundle + signature in state/ota/
 * and drops a "request" file.  The root-side loop in main.sh verifies again with the tool and key from the read-only
 * system partition, installs, and writes "result", which is relayed to the pusher.
 *
 *   -> "HMOTA-PUSH1 <bundle bytes>\n"  <64 byte signature>  <bundle>
 *   <- one line: "OK <version>" | "FAILED <why>"
 *
 * The same key opens adb over Wi-Fi (adbwifi.c) without Home Assistant: the way back in when the Echo is not adopted,
 * has lost its key, or runs Wyoming, and its USB wires are gone.  Challenge and response, so that a recorded exchange
 * cannot be played again; what is signed starts differently from every bundle ("HMOTA1\n"), so neither passes for the
 * other.
 *   -> "HMOTA-ADB1\n"
 *   <- "NONCE <64 hex digits>\n"
 *   -> <64 byte signature over "HMOTA-ADB1\n" + the 32 nonce bytes>
 *   <- one line: "OK ..." once root's firewall watcher has opened it | "FAILED <why>"
 *
 * A listed plugin's package (scripts/ota-push.sh --plugin): an Echo without internet gets its plugins' updates so, as
 * it gets hassmic's.
 *   -> "HMOTA-PLUG1 <package bytes> <the package's BLAKE2b-256, 64 hex digits>\n"
 *   <- "NONCE <64 hex digits>\n"
 *   -> <64 byte signature over that line + the 32 nonce bytes>
 *   <- "SEND\n" | "FAILED <why>"     (the package only once it is wanted: a refusal sent while megabytes are
 *                                     still coming in is thrown away with the connection's reset)
 *   -> <package>
 *   <- one line: "OK <id> version <n> installed" | "FAILED <why>"
 * Two keys, each for its own question.  The plugin index's listing inside the package is what makes it installable
 * at all, and with the pinned author key and the accepted section it is the whole of what decides (web.c
 * web_plugin_push: listed, that author, nothing beyond what the owner accepted; else refused, and the PC is told
 * why).  The update key's signature is not about the package's worth: without it anyone on the LAN could make this
 * Echo take megabytes, write them to flash and restart its wake word engine at will, and could choose the moment
 * and the version of an update that the settings page gives an approved browser only.  So the port asks who pushes,
 * as it does for adb, by challenge (a recorded push cannot be played again), over the package's digest (what is
 * installed is what was signed for).  5.9 says the same, of the release key that listed official plugins then: "the
 * port's challenge is the update key's, the package's signature the release key's"; the index key has its place now.
 *
 * An update that passes its self test (main.c) becomes the factory copy on the system partition, the one the Echo falls
 * back to: ota_healthy() tells root's installer loop, which checks that it is the installed update and that the running
 * hassmic is its binary, then writes it (scripts/system/sysinstall.sh).
 *
 * Online updates (update.c) come the same way to root, signed with the project's release key instead of the owner's:
 * ota_handoff() checks that one and leaves the bundle where a push leaves it.  This port takes the owner's key only.
 *
 * What someone on the LAN without the key can cost.  Anyone can connect, and until a signature has been checked
 * ("before the key" below) a connection is anyone's.  Three things bound it:
 * Time.  Every step has a time of its own as a whole, not per read: LINE_MS for the request line, SIG_MS for the
 * answer to a challenge, and for a body a floor (body_read).  Before, each read() had 30 s: a request line sent a
 * byte at a time held the port for an hour (127 bytes).  The body's floor was then one line from the start
 * (BODY_RATE times the time so far), which a burst banks: 16 MB less one byte at once, and the line let the sender
 * be silent for 8.7 minutes (16 MB / 32 kB/s + the grace) with the buffer and the bundle's one place held.  Now
 * nothing is banked: BODY_STEP bytes within every BODY_STEP_MS, counted anew from where the last step ended
 * however much came with it, so silence ends a body after ten seconds at most; and the whole of it within the
 * grace plus its size at BODY_RATE, so the steps' own rate (6.4 kB/s) is no rate to drip megabytes at.
 * Connections.  One thread each, CONNS_MAX at once.  Four that asked for adb and sat out the challenge's ten
 * seconds, opened again as they ended, took all four places there were, and the fifth was closed without a word:
 * the owner's push or adb got through 0 times of 39 in 40 s, and otatool could not say why (a review, 2026-11-20).
 * So before the key a source address has PRE_ADDR connections and all of them together PRE_MAX; a newcomer that
 * finds PRE_MAX taken ends the oldest of those that hold no bundle's place (below), the one that has sat longest
 * without signing, and that one is told ("FAILED busy ...") before its connection is shut down.  The oldest of
 * all it was at first, and that ended the owner's bundle: adb and a package sign within milliseconds of their
 * connect, but "HMOTA-PUSH1" is before the key for all of its body (its signature is over every byte), seconds
 * for a few megabytes, and idle connections from two other addresses, three connects a second, made it the oldest
 * and had it shut down each time (a review, 2026-11-20).  A connection that holds a bundle's place is therefore
 * not ended for a newcomer's connection, and who holds a place is decided by the places' own rule (Memory, below),
 * where sitting idle loses.  "Holds a place" has to be true at that moment for this to leave room: pconn.body was
 * at first set when the place was taken and cleared only by the connection's own thread, which after a takeover
 * still sat in its signature's read (SIG_MS, the generation not looked at there), so four addresses that each
 * sent "HMOTA-PUSH1 16777216" and nothing more were four "bundles on their way", none to be ended, and every
 * newcomer was refused for as long as they kept coming back: the owner 0 of 39 again (a review, 2026-11-20).  Now
 * a place names its connection (ubody.who) and a takeover clears that connection's `body` under cl in the same
 * breath, so at most BODIES_MAX connections are spared at any moment, fewer than PRE_MAX, and room is always
 * made; the signature's read looks at the generation as the body's does (read_by's U), so the one that lost its
 * place is told within 200 ms and goes.
 * Connections in their request line's or their challenge's time are not spared: four that sit there, opened
 * again as they end, would be the first paragraph's lockout again, and the owner's own passes those two steps in
 * milliseconds.  One address alone therefore keeps no other address out: it has PRE_ADDR connections before the
 * key and one bundle's place.  Many addresses that each keep opening connections can still end the owner's
 * between its connect and its signature (four connects within those milliseconds, each time), and can take a
 * bundle's place from an owner who sends slower than BODY_FAST (Memory, below): what anyone who can flood the LAN
 * can do to any port.
 * A connection refused is told so, "FAILED busy ...", and kept half open for a moment so that the line arrives
 * (refuse, park): closed at once with the request unread, the kernel answers with a reset and the line is gone.
 * Memory.  A plugin's package is allocated only after the challenge is answered.  A bundle's cannot wait for that:
 * "HMOTA-PUSH1" sends the signature and then the bundle, EdDSA's check needs every byte it signs, and the line is
 * what scripts/otatool.py and the Echo's own otatool of every version installed speak.  Written to the flash
 * unchecked instead, junk would wear what the header above keeps it from touching.  So it stays in memory, at
 * MAX_BUNDLE, pages touched only as bytes really arrive.  One at a time, as it was, meant that whoever held that
 * one place kept every signed push out for as long as he kept to the floor.  Now BODIES_MAX of them, 32 MB that no
 * signature has paid for, one per source address; and when those are taken a newcomer takes the place of the one
 * that came in slowest over the last second or two (never one whose bundle is whole and being checked: body_whole),
 * unless that one too is faster than BODY_FAST (256 kB/s: a
 * push over any Wi-Fi the Echo is usable on is; the 4 MB of a bundle are then seconds).  A place just taken has
 * shown no speed yet (its sender's first bytes are in the socket, its thread has not read them): judged at once,
 * it was the slowest there is, and the next stranger's connect took the owner's push out in its first moment.  So
 * a place is not given away in its first BODY_YOUNG_MS, once per address: an address whose last bundle ended
 * without a signature that held has no such grace for BODY_AGAIN_MS (spent[]), else two addresses that open a
 * bundle every two seconds and send nothing held both places for good.  "Ended" there is the sender's own doing:
 * it fell under the floor, closed, its signature did not verify, or its place was taken while nothing at all had
 * come from it for the window (it sat).  A bundle that was arriving when a newcomer took its place is not held
 * against its address: that is the owner on a slow link, whose next push needs the grace most.  Holding the places against
 * the owner so takes two addresses that each send at that speed for as long as it is to last, or some ten that
 * take turns with the grace, a flood again, and each of them ends at its 16 MB at the latest with a signature
 * that does not verify.  What it costs the other way: an owner
 * pushing over a link slower than that can lose his place to a stranger's push, while one attacks; he is told
 * ("another push took this one's place") and pushes again.  The two ways not taken: the newest always wins (a
 * stranger connecting ten times a second ends every push of the owner's), the oldest always wins (as it was).
 */
#include "ota.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../third_party/monocypher.h"
#include "../include/edkey.h"
#include "adbwifi.h"
#include "core.h"
#include "netio.h"
#include "statefile.h"
#include "update.h"
#include "plugin.h"
#include "plugin_store.h"
#include "web.h"

#define MAX_BUNDLE (16u << 20)
#define MAX_PLUGIN (4u << 20)           /* PS_PACKAGE_MAX: no package is larger */
#define CONNS_MAX  8                    /* threads at once: PRE_MAX before the key, and the owner's own after it (a push waits a minute for root) */
#define PRE_MAX    4                    /* ... of them before the key, */
#define PRE_ADDR   2                    /* ... and of those from one address: a push, and adb asked for beside it */
#define BODIES_MAX 2                    /* bundles in memory that no signature has been checked for yet */
#define LINE_MS    5000                 /* the request line, whole: the PC sends it in one packet */
#define SIG_MS     10000                /* a challenge's answer: the PC signs 100 bytes */
/* A body (a bundle, a package): BODY_STEP bytes in every BODY_STEP_MS (ten seconds cover a link that stalls), and
 * all of it within BODY_GRACE_MS plus its size at BODY_RATE: 32 kB/s is a link at 256 kbit/s, an Echo at the edge
 * of its Wi-Fi, where a 4 MB bundle still arrives in two minutes.  Whoever sends slower is not pushing an update */
#define BODY_GRACE_MS 10000
#define BODY_RATE  32768
#define BODY_STEP  65536
#define BODY_STEP_MS 10000
#define BODY_FAST  (256 << 10)          /* bytes in the last one to two seconds below which an unsigned body gives way */
#define BODY_YOUNG_MS 2000              /* ... but not in its first two seconds: the window above has to fill first */
#define BODY_AGAIN_MS 10000             /* ... unless that address's last bundle, within this, ended unsigned */
#define PARK_MS    1000                 /* a refused connection stays half open this long, for its answer to arrive */
static int port;
/* cl: who is connected (pcs) and whose unsigned bundle is in memory (bodies).  Never held over a read or the flash */
static pthread_mutex_t cl = PTHREAD_MUTEX_INITIALIZER;
/* pre: before the key (no signature of this connection's has been checked yet); body: it holds an unsigned
 * bundle's place now (the listener ends none of those for a newcomer): set and cleared with the place itself,
 * under cl, by whoever takes or gives it (body_take for a place taken over, body_done), never left standing by a
 * thread that has not looked yet; fd -1: a free place.  The fd is the thread's until it takes its place out
 * (conn), so the listener may write to it and shut it down under cl, and no other */
static struct pconn { int fd, pre, body, ended; uint32_t addr; long long at; } pcs[CONNS_MAX];   /* ended: the listener shut it down for a newcomer */
/* w_at, w_now, w_prev: bytes since w_at and in the second before it, for body_take; since: when the place was
 * taken, grace: not given away while younger than BODY_YOUNG_MS; gen: new with every holder, so the thread whose
 * place a newcomer took finds it is no longer its own when it next looks; whole: every byte of the bundle is here
 * and its signature is being checked: not given away (body_whole) */
static struct ubody { int used, grace, whole; unsigned gen; uint32_t addr; long long since, w_at; size_t w_now, w_prev; struct pconn *who; } bodies[BODIES_MAX];
/* cl: addresses whose bundle ended without a signature that held, each until when its next has no grace; the one
 * that ends soonest makes room */
static struct { uint32_t addr; long long until; } spent[8];

static pthread_mutex_t handoff_lock = PTHREAD_MUTEX_INITIALIZER;     /* one bundle in state/ota/ at a time: push or download */
/* A bundle is with root's installer (stage(): a push or an online update), and this build's self test passing now
 * makes nothing the fallback: root installs the next and restarts hassmic within seconds.  The self test held by the
 * wake word plugin asks every 10 s for as long as hassmic runs (main.c selftest_thread), and a late pass in those
 * seconds left `healthy` for root's next look, which came after the install and made the untested update the
 * factory copy (a review of 1d704e9, U1).  Root compares the version too (main.sh healthy); this keeps the build
 * from saying it at all.  Set under hand_lock before the request is written, so ota_healthy, which looks and writes
 * under it, either wrote before the request (root takes it before it installs: this build passed, it becomes the
 * fallback) or writes nothing.  Cleared where the installer said no, or nothing was handed over: the self test asks
 * again.  Where root did not answer within the minute it may install it still: `unanswered`, and ota_handed reads
 * root's answer once root has taken the request (main.sh ota_watch deletes `request` and `result` as it takes one,
 * and writes `result` when it is done; stage() deletes an old `result` before the request goes in): a "no" clears
 * it as one in time would have.  It stayed set for the run: a root busy for a minute (a light ring set made, a
 * reset's stop) that then refused had the self test never pass and the other plugins stopped until a restart (a
 * review of 0119caf, V2).  That late answer is read every 2 s by a thread of its own (late_thread) for as long as it
 * is outstanding, not only by the self test, which asks nothing once it passed (a review of 9bbfd66, W1).
 * Root's answer is this handoff's only where it carries the request's nonce (stage(): 16 random bytes, "1 <hex>" in
 * `request`; main.sh ota_watch echoes "nonce <hex>" as the result's second line): a root that took a request after
 * hassmic stopped waiting wrote its answer while the next handoff waited, and that one took it for its own (told the
 * pusher the other's words: a review of 9bbfd66, W3).  A result without
 * that line is an older main.sh's, which echoes none, and is taken as before (W3's race stays for that pairing): boot.sh
 * picks the copy per service as it starts, so the update's hassmic runs beside the factory copy's main.sh where the
 * boot started both from the factory copy (three failed starts) and the satellite starts again later (`tries` set to 0
 * after a minute up), and the factory copy's hassmic beside the update's main.sh after five quick exits (it ignores
 * the second line). */
static pthread_mutex_t hand_lock = PTHREAD_MUTEX_INITIALIZER;
static int handed, unanswered, late_on;
static char hand_nonce[33];
static void handed_off(void) { pthread_mutex_lock(&hand_lock); handed = unanswered = 0; pthread_mutex_unlock(&hand_lock); }
static void *late_thread(void *arg);
static void late_read(void);
static void handed_late(void)
{
    pthread_t t;
    pthread_mutex_lock(&hand_lock);
    unanswered = handed;
    if (unanswered && !late_on) { late_on = 1; if (pthread_create(&t, NULL, late_thread, NULL)) late_on = 0; else pthread_detach(t); }
    pthread_mutex_unlock(&hand_lock);
}

static const char *pub_path(void) { const char *e = getenv("HASSMIC_UPDATE_PUB"); return e ? e : "/system/hassmic/update.pub"; }
/* main.sh names the release key of the copy that runs (an update may bring a new one), else the factory copy's */
static const char *release_pub_path(void) { const char *e = getenv("HASSMIC_RELEASE_PUB"); return e && *e ? e : "/system/hassmic/release.pub"; }
static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* Up to N bytes, all of them by UNTIL (monotonic ms).  U (or NULL): the unsigned bundle's place this read is made
 * from, taken as GEN; once a newcomer has it the read ends, looked for every 200 ms as body_read does.  The count:
 * short when the time ran out, the peer closed or the place is gone */
static size_t read_by(int fd, void *buf, size_t n, long long until, struct ubody *u, unsigned gen)
{
    size_t got = 0;
    while (got < n) {
        struct pollfd p = { fd, POLLIN, 0 }; long long left; ssize_t k; int r, out = 0, step = u ? 200 : 1000;
        if (u) { pthread_mutex_lock(&cl); out = u->gen != gen; pthread_mutex_unlock(&cl); }
        if (out || (left = until - now_ms()) <= 0) break;
        r = poll(&p, 1, left > step ? step : (int)left);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        k = recv(fd, (char *)buf + got, n - got, MSG_DONTWAIT);
        if (k < 0) { if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; break; }
        if (k == 0) break;
        got += (size_t)k;
    }
    return got;
}

/* cl held: what U took in over the last one to two seconds */
static size_t body_recent(const struct ubody *u, long long now)
{
    long long age = now - u->w_at;
    return age >= 2000 ? 0 : age >= 1000 ? u->w_now : u->w_now + u->w_prev;
}

/* A body of N bytes under the floor the header describes.  U: an unsigned bundle's place (NULL: a package after its
 * challenge) taken as GEN, which counts what comes for body_take.  The count: short when the sender fell under the
 * floor, closed, or lost its place to a newcomer */
static size_t body_read(int fd, void *buf, size_t n, struct ubody *u, unsigned gen)
{
    size_t got = 0, step_got = 0; long long start = now_ms(), step_at = start, whole = start + BODY_GRACE_MS + (long long)n * 1000 / BODY_RATE;
    while (got < n) {
        struct pollfd p = { fd, POLLIN, 0 }; long long now = now_ms(), until, left; ssize_t k; int r, out = 0;
        /* a step done is where the next begins: what came beyond it buys no time */
        if (got - step_got >= BODY_STEP) { step_got = got; step_at = now; }
        until = step_at + BODY_STEP_MS < whole ? step_at + BODY_STEP_MS : whole;
        if (u) { pthread_mutex_lock(&cl); out = u->gen != gen; pthread_mutex_unlock(&cl); }
        if (out || (left = until - now) <= 0) break;
        r = poll(&p, 1, left > 200 ? 200 : (int)left);      /* (200 ms: how soon the buffer of a place a newcomer took is freed) */
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        k = recv(fd, (char *)buf + got, n - got > 65536 ? 65536 : n - got, MSG_DONTWAIT);
        if (k < 0) { if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; break; }
        if (k == 0) break;
        got += (size_t)k;
        if (u) {
            pthread_mutex_lock(&cl);
            now = now_ms();
            if (u->gen != gen) { pthread_mutex_unlock(&cl); break; }
            if (now - u->w_at >= 1000) { u->w_prev = now - u->w_at >= 2000 ? 0 : u->w_now; u->w_now = 0; u->w_at = now; }
            u->w_now += (size_t)k;
            pthread_mutex_unlock(&cl);
        }
    }
    return got;
}

/* cl held: ADDR's next bundle has no grace for BODY_AGAIN_MS */
static void spend(uint32_t addr, long long now)
{
    int at = 0;
    for (int i = 0; i < 8; i++) { if (spent[i].addr == addr) { at = i; break; } if (spent[i].until < spent[at].until) at = i; }
    spent[at].addr = addr; spent[at].until = now + BODY_AGAIN_MS;
}

/* A place for C's unsigned bundle and its generation, or NULL and WHY.  The header says who gives way to whom.
 * The one that gives way is no longer counted as holding a place from this moment (its `body` cleared here: the
 * listener may end it for a newcomer like any connection that has not signed) and keeps its buffer until its
 * thread next looks (200 ms, in the signature's read as in the body's).  Its address is spent only if it sat:
 * nothing from it in the window.  Buffers at once: one per connection before the key at the worst (PRE_MAX: a
 * newcomer without grace can lose its place to the next within those 200 ms, and so on), of which only what
 * really arrived is memory in use; two of them go on receiving */
static struct ubody *body_take(struct pconn *c, unsigned *gen, const char **why)
{
    struct ubody *u = NULL, *slow = NULL; long long now = now_ms(); int same = 0, grace = 1; uint32_t addr = c->addr;
    pthread_mutex_lock(&cl);
    for (int i = 0; i < BODIES_MAX; i++) {
        struct ubody *b = &bodies[i];
        if (!b->used) { if (!u) u = b; continue; }
        if (b->addr == addr) same = 1;
        if (b->whole || (b->grace && now - b->since < BODY_YOUNG_MS) || body_recent(b, now) >= BODY_FAST) continue;    /* keeps its place */
        if (!slow || body_recent(b, now) < body_recent(slow, now)) slow = b;
    }
    /* a connection the listener has ended for a newcomer (just after its request line: its thread has not seen the
     * read fail yet) takes no place: it took one, lost it to its dead socket a moment later, and its address was
     * spent for what the listener had done */
    if (c->ended) { u = NULL; *why = "FAILED busy: others connected while this connection waited without a signature: connect again"; }
    else if (same) { u = NULL; *why = "FAILED busy: an update from this address is being pushed just now"; }
    else if (!u && slow) u = slow;
    else if (!u) *why = "FAILED busy: other updates are being pushed just now";
    if (u) {
        unsigned g = u->gen + 1;
        if (u->used) {
            if (u->who) u->who->body = 0;
            if (!body_recent(u, now)) spend(u->addr, now);
        }
        for (int i = 0; i < 8; i++) if (spent[i].addr == addr && now < spent[i].until) grace = 0;
        memset(u, 0, sizeof *u); u->used = 1; u->gen = *gen = g; u->addr = addr; u->since = u->w_at = now; u->grace = grace;
        u->who = c; c->body = 1;
    }
    pthread_mutex_unlock(&cl);
    return u;
}

/* Every byte of C's bundle has arrived and GEN still has its place: from here it is not given away (0), until
 * body_done.  A bundle is done receiving but before the key for as long as its signature's check takes (EdDSA
 * hashes all of it twice: tenths of a second for megabytes on an Echo), it shows no speed any more, and a newcomer
 * took its place in that time: `body` 0 with `pre` still 1, so the listener might shut the connection down for the
 * next one, and the thread went on all the same and handed the bundle to the installer, installed while the PC
 * read "FAILED busy" or a reset (a review, 2026-11-20).  The check is CPU on bytes already here: nothing a sender
 * can stretch.  1: the place was taken before the last byte was counted, the bundle is not looked at.
 * (HASSMIC_OTA_CHECK_DELAY=<ms>:<file> for the tests: while the file exists the check takes that much longer) */
static int body_whole(struct ubody *u, unsigned gen)
{
    int out;
    pthread_mutex_lock(&cl);
    if (!(out = u->gen != gen)) u->whole = 1;
    pthread_mutex_unlock(&cl);
    return out;
}

/* C's place given back.  GOOD 1: its bundle's signature held, and C is past the key from the same hold of cl on:
 * cleared one after the other, `body` first, there was a moment in which the listener saw a connection that had
 * not signed and held no place, and could end the verified push for a newcomer.  Not GOOD and the place still its
 * own (the floor, a close, a signature that does not verify): its address has no grace for the next one,
 * BODY_AGAIN_MS long (-1: given back before anything was asked of the sender, nothing held against it).  The
 * listener's ending a connection for a newcomer is no case here: it ends only one that holds no place (`body` 0,
 * under cl), and one it ended gets none (body_take), so a place that is still its own was never the listener's to
 * cut short; a test of `ended` stood here and could not be reached (a review, 2026-11-20).  A place a
 * newcomer took is not this end's doing (body_take decided whether the address is spent, by whether it sat); a
 * place that is `whole` is never taken, so after body_whole this is always the place's own end.
 * 1: it was no longer GEN's */
static int body_done(struct pconn *c, struct ubody *u, unsigned gen, int good)
{
    int out;
    pthread_mutex_lock(&cl);
    if (!(out = u->gen != gen)) { u->used = 0; u->whole = 0; u->who = NULL; c->body = 0; if (!good) spend(c->addr, now_ms()); }
    if (good > 0) c->pre = 0;
    pthread_mutex_unlock(&cl);
    return out;
}

static void reply(int fd, const char *line) { char b[300]; int n = snprintf(b, sizeof b, "%s\n", line); write_all(fd, b, n); fprintf(stderr, "update: %s\n", line); }

/* An answer to a sender that may still be sending (megabytes of a bundle nobody will read): said, our side closed
 * for writing, and what still comes thrown away for PARK_MS, so that the line is read before the close resets */
static void refuse(int fd, const char *line)
{
    char junk[4096]; long long until = now_ms() + PARK_MS;
    reply(fd, line); shutdown(fd, SHUT_WR);
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 }; long long left = until - now_ms();
        if (left <= 0 || poll(&p, 1, (int)left) <= 0 || recv(fd, junk, sizeof junk, MSG_DONTWAIT) <= 0) break;
    }
}

/* this connection's signature verified: no longer one of those before the key */
static void keyed(struct pconn *c) { pthread_mutex_lock(&cl); c->pre = 0; pthread_mutex_unlock(&cl); }

static int store(const char *dir, const char *name, const void *data, size_t len)
{
    char path[300]; snprintf(path, sizeof path, "%s/%s", dir, name);
    return statefile_write(path, 0666, data, len);
}

/* (a key under which any signature holds is none: edkey.h; the release key, the update key) */
static int load_key(const char *path, uint8_t pk[32])
{
    FILE *f = fopen(path, "rb");
    int ok = f && fread(pk, 1, 32, f) == 32;
    if (f) fclose(f);
    if (ok && ed_key_weak(pk)) { fprintf(stderr, "ota: %s is a key under which any signature holds (small order): taken as none\n", path); ok = 0; }
    return ok;
}

/* 0 if the peer signs "<line>\n" + a fresh nonce with the update key; otherwise it has been answered already. */
static int challenge(int fd, const uint8_t pk[32], const char *line)
{
    uint8_t msg[160], sig[64]; char hex[80]; size_t ll = strlen(line); int r = open("/dev/urandom", O_RDONLY);
    memcpy(msg, line, ll); msg[ll++] = '\n';
    if (r < 0 || read(r, msg + ll, 32) != 32) { if (r >= 0) close(r); reply(fd, "FAILED no randomness"); return -1; }
    close(r);
    int n = sprintf(hex, "NONCE ");
    for (int i = 0; i < 32; i++) n += sprintf(hex + n, "%02x", msg[ll + i]);
    hex[n++] = '\n';
    if (write_all(fd, hex, n) || read_by(fd, sig, 64, now_ms() + SIG_MS, NULL, 0) != 64) { reply(fd, "FAILED no signature"); return -1; }
    if (crypto_eddsa_check(sig, pk, msg, ll + 32)) { reply(fd, "FAILED signature does not verify against this device's update key"); return -1; }
    return 0;
}

/* Root's answer in DIR/result as it stands, for the handoff of NONCE: 0 and its first line in RES (what the pusher
 * hears); 1 none; 2 another handoff's (its "nonce" line names another, or is not one) */
static int result_read(const char *dir, const char *nonce, char *res, size_t cap)
{
    char path[300], line[80] = ""; FILE *f;
    snprintf(path, sizeof path, "%s/result", dir);
    if (!(f = fopen(path, "r"))) return 1;
    /* (a first line longer than RES: the rest of it skipped, not read as the nonce's line: a review of 06683b6, X3) */
    res[0] = 0;
    if (fgets(res, cap, f)) { size_t n = strcspn(res, "\n"); int c; if (!res[n]) while ((c = getc(f)) != EOF && c != '\n') {} res[n] = 0; }
    if (!fgets(line, sizeof line, f)) line[0] = 0;          /* (none: an older main.sh's, which echoes none) */
    fclose(f);
    line[strcspn(line, "\n")] = 0;
    if (line[0] && (strncmp(line, "nonce ", 6) || strcmp(line + 6, nonce))) return 2;
    if (!res[0]) snprintf(res, cap, "FAILED empty result");
    return 0;
}

/* Root's answer to the handoff of NONCE (the installer loop looks every 2 s), waited for up to SECS
 * (HASSMIC_OTA_ANSWER_S for the tests: a root that answers after it).  0; -1: none came (root may still install it:
 * handed_late) */
static int answer(const char *dir, const char *nonce, int secs, char *res, size_t cap)
{
    const char *e = getenv("HASSMIC_OTA_ANSWER_S"); int other = 0, r;
    if (e && atoi(e) > 0) secs = atoi(e);
    for (int t = 0; t < secs * 2; t++) {
        if (!(r = result_read(dir, nonce, res, cap))) return 0;
        if (r == 2 && !other++) fprintf(stderr, "update: an answer of root's to an earlier handoff left alone: %s\n", res);
        usleep(500000);
    }
    snprintf(res, cap, "FAILED the installer did not answer (is the hassmic_fw service running?)");
    return -1;
}

/* Bundle and signature into state/ota/ with a request for root's installer, which names a nonce of its own (root
 * echoes it under its answer); 0 if stored.  The handoff is this one's (handed, a new nonce, an earlier one's late
 * answer no longer waited for) only once bundle and signature are stored, and as it was again where the request
 * could not be: a store that failed (no room while root unpacks the push before, unanswered after its minute) took
 * that one's handoff away and ended the reading of its late answer (a review of 06683b6, X1).
 * HASSMIC_OTA_STAGE_DELAY=<ms>:<file> for the tests: while the file exists (up to MS) the handoff waits before it is
 * marked, the gap the release key's check or another push's handoff makes on an Echo */
static int stage(const uint8_t *b, size_t len, const uint8_t sig[64], char *dir, size_t dircap, char nonce[33])
{
    char path[300], req[40], was_nonce[33]; uint8_t r[16] = { 0 }; int fd, ok, was_handed, was_unanswered;
    { const char *e = getenv("HASSMIC_OTA_STAGE_DELAY"); int ms = 0; char file[200];
      if (e && sscanf(e, "%d:%199s", &ms, file) == 2) for (; ms > 0 && !access(file, F_OK); ms -= 100) usleep(100000); }
    ok = (fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC)) >= 0 && read(fd, r, sizeof r) == (ssize_t)sizeof r;
    if (fd >= 0) close(fd);
    if (!ok) { fprintf(stderr, "update: no randomness for the request\n"); return -1; }
    for (int i = 0; i < 16; i++) sprintf(nonce + 2 * i, "%02x", r[i]);
    snprintf(dir, dircap, "%s/ota", state_dir()); mkdir(dir, 0700);
    if (store(dir, "bundle", b, len) || store(dir, "bundle.sig", sig, 64)) return -1;
    /* Marked before the request is written (ota_healthy, above).  An earlier handoff's late answer read first: the
     * result goes below (an older main.sh's answer names no request) */
    pthread_mutex_lock(&hand_lock);
    late_read();
    was_handed = handed; was_unanswered = unanswered; snprintf(was_nonce, sizeof was_nonce, "%s", hand_nonce);
    handed = 1; unanswered = 0;
    snprintf(hand_nonce, sizeof hand_nonce, "%s", nonce);
    pthread_mutex_unlock(&hand_lock);
    snprintf(path, sizeof path, "%s/result", dir); unlink(path);
    snprintf(req, sizeof req, "1 %s\n", nonce);
    if (!store(dir, "request", req, strlen(req))) return 0;
    pthread_mutex_lock(&hand_lock);
    handed = was_handed; unanswered = was_unanswered; snprintf(hand_nonce, sizeof hand_nonce, "%s", was_nonce);
    pthread_mutex_unlock(&hand_lock);
    if (was_unanswered) handed_late();                  /* (its reader ended as this one took the handoff) */
    return -1;
}

int ota_handoff(const uint8_t *b, size_t len, const uint8_t sig[64], const uint8_t *own, char *res, size_t cap)
{
    uint8_t pk[32]; char dir[280], nonce[33]; int none = 0, bad, rel = load_key(release_pub_path(), pk);
    const char *by = "the release key";
    /* the release key, or the key the owner's Sources named (root checks against the same: main.sh ota_watch) */
    if (rel && !crypto_eddsa_check(sig, pk, b, len)) goto check;
    if (own && !ed_key_weak(own) && !crypto_eddsa_check(sig, own, b, len)) { by = "the key of Sources (settings page)"; goto check; }
    if (!rel && !own) snprintf(res, cap, "FAILED no release key on this device (%s): an update with it pushed or installed first", release_pub_path());
    else snprintf(res, cap, "FAILED signature does not verify against the release key%s", own ? " nor the key of Sources (settings page)" : "");
    return -1;
check:
    pthread_mutex_lock(&handoff_lock);
    if ((bad = stage(b, len, sig, dir, sizeof dir, nonce))) snprintf(res, cap, "FAILED cannot store the bundle");
    if (!bad) {
        fprintf(stderr, "update: %zu bytes downloaded, signed with %s, handed to the installer\n", len, by);
        none = answer(dir, nonce, 60, res, cap);
        if (strncmp(res, "OK ", 3) && !none) handed_off();
        if (none) handed_late();
    }
    pthread_mutex_unlock(&handoff_lock);
    fprintf(stderr, "update: %s\n", res);
    return !strncmp(res, "OK ", 3) ? 0 : none ? 1 : -1;
}

/* `healthy` names this build (core.h VERSION_NAME, the bundle's VERSION): root makes the installed update the
 * factory copy only when that is the one that passed (main.sh healthy).  It said "1" before, and a build that passed
 * spoke for whichever was installed when root looked (U1 above) */
int ota_healthy(void)
{
    char dir[280]; int r = 1;
    pthread_mutex_lock(&hand_lock);
    if (!handed) {
        snprintf(dir, sizeof dir, "%s/ota", state_dir()); mkdir(dir, 0700);
        r = store(dir, "healthy", VERSION_NAME "\n", strlen(VERSION_NAME "\n")) ? -1 : 0;
    }
    pthread_mutex_unlock(&hand_lock);
    if (r < 0) fprintf(stderr, "self test: cannot tell the installer\n");
    return r;
}

/* hand_lock held: a handoff root did not answer within the minute, its answer once it took the request (above) */
static void late_read(void)
{
    char path[300], res[200];
    snprintf(path, sizeof path, "%s/ota/request", state_dir());
    if (handed && unanswered && access(path, F_OK) && errno == ENOENT) {
        snprintf(path, sizeof path, "%s/ota", state_dir());
        if (!result_read(path, hand_nonce, res, sizeof res)) {
            unanswered = 0;
            if (strncmp(res, "OK ", 3)) handed = 0;     /* (as a refusal in time does: handed_off) */
            fprintf(stderr, "update: the installer answered late: %s%s\n", res, handed ? "" : "; the self test asks again");
        }
    }
}

int ota_handed(void)
{
    int r;
    pthread_mutex_lock(&hand_lock);
    late_read();
    r = handed;
    pthread_mutex_unlock(&hand_lock);
    return r;
}

/* Reads root's late answer while one is outstanding (handed_late starts it; one at a time, late_on).  Ends when it
 * came or another handoff began (its own answer is waited for by its caller), else with the process */
static void *late_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(2000000);
        ota_handed();
        pthread_mutex_lock(&hand_lock);
        if (!handed || !unanswered) { late_on = 0; pthread_mutex_unlock(&hand_lock); return NULL; }
        pthread_mutex_unlock(&hand_lock);
    }
}

static void adb_open(struct pconn *c, const uint8_t pk[32])
{
    int fd = c->fd;
    if (challenge(fd, pk, "HMOTA-ADB1")) return;
    keyed(c);
    fprintf(stderr, "update: adb over Wi-Fi asked for with the update key\n");
    adbwifi_ask(1);
    for (int t = 0; t < 30; t++) {                      /* the firewall watcher looks every 5 s */
        if (adbwifi_granted()) { reply(fd, "OK adb over Wi-Fi open for 30 min"); return; }
        usleep(500000);
    }
    reply(fd, "FAILED the firewall service did not answer (is hassmic_fw running?)");
}

static void plugin_push(struct pconn *c, const uint8_t pk[32], const char *line, unsigned long len, const char *dg)
{
    uint8_t *b, have[32]; char hex[65], res[300]; int fd = c->fd;
    if (challenge(fd, pk, line)) return;
    keyed(c);
    if (!(b = malloc(len))) { reply(fd, "FAILED out of memory"); return; }
    if (write_all(fd, "SEND\n", 5) || body_read(fd, b, len, NULL, 0) != len) { reply(fd, "FAILED upload incomplete"); free(b); return; }
    crypto_blake2b(have, 32, b, len);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", have[i]);
    if (strcmp(hex, dg)) { reply(fd, "FAILED the package is not the one that was signed for"); free(b); return; }
    fprintf(stderr, "update: a plugin package of %lu bytes pushed with the update key\n", len);
    web_plugin_push(b, len, res, sizeof res);
    reply(fd, res);
    free(b);
}

static void handle(struct pconn *c)
{
    char line[128], dir[280]; size_t i = 0; unsigned long len = 0; uint8_t sig[64], pk[32], *b; int eol = 0, fd = c->fd;
    long long until = now_ms() + LINE_MS; struct ubody *u; unsigned gen = 0; const char *why = "";
    struct timeval tv = { 10, 0 }; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);       /* (a peer that does not read its answer) */
    /* a byte at a time, since what follows the line is not the line's; all of it within LINE_MS */
    while (i < sizeof line - 1 && read_by(fd, line + i, 1, until, NULL, 0) == 1) { if (line[i] == '\n') { eol = 1; break; } i++; }
    line[i] = 0;
    if (!eol) { reply(fd, "FAILED bad request"); return; }
    int adb = !strcmp(line, "HMOTA-ADB1"), plug = 0, k = 0; char dg[65] = "";
    /* the line as it is signed: one form only (the length without sign or leading zero, the digest in lower case) */
    if (!adb && sscanf(line, "HMOTA-PLUG1 %lu %64[0-9a-f]%n", &len, dg, &k) == 2) {
        char again[128]; snprintf(again, sizeof again, "HMOTA-PLUG1 %lu %s", len, dg);
        plug = strlen(dg) == 64 && !line[k] && !strcmp(again, line) && len >= 1024 && len <= MAX_PLUGIN;
        if (!plug) { reply(fd, "FAILED bad request"); return; }
    }
    if (!adb && !plug && (sscanf(line, "HMOTA-PUSH1 %lu", &len) != 1 || !len || len > MAX_BUNDLE)) { reply(fd, "FAILED bad request"); return; }
    if (!load_key(pub_path(), pk)) { reply(fd, "FAILED this device has no update key (install-system.sh installs it)"); return; }
    if (adb) { adb_open(c, pk); return; }
    if (plug) { plugin_push(c, pk, line, len, dg); return; }
    /* the bundle before its signature can be checked (the header says why, and who gets a place) */
    if (!(u = body_take(c, &gen, &why))) { refuse(fd, why); return; }
    if (!(b = malloc(len))) { body_done(c, u, gen, -1); reply(fd, "FAILED out of memory"); return; }
    if (read_by(fd, sig, 64, now_ms() + SIG_MS, u, gen) != 64 || body_read(fd, b, len, u, gen) != len || body_whole(u, gen)) {
        free(b);
        if (body_done(c, u, gen, 0)) refuse(fd, "FAILED busy: another push took this one's place (this one came in slowly): push again");
        else reply(fd, "FAILED upload incomplete");
        return;
    }
    { const char *e = getenv("HASSMIC_OTA_CHECK_DELAY"); int ms = 0; char file[200];
      if (e && sscanf(e, "%d:%199s", &ms, file) == 2 && ms > 0 && !access(file, F_OK)) usleep((useconds_t)ms * 1000); }
    i = (size_t)crypto_eddsa_check(sig, pk, b, len);
    body_done(c, u, gen, !i);                   /* signed (and past the key, in one) or freed: not what the places count any more */
    if (i) { reply(fd, "FAILED signature does not verify against this device's update key"); free(b); return; }
    /* (no plugin needs anything of a hassmic update: one never breaks a plugin, sdk/PROTOCOL.md "Versions") */
    char res[256], nonce[33]; int none = 0, bad;
    pthread_mutex_lock(&handoff_lock);
    if ((bad = stage(b, len, sig, dir, sizeof dir, nonce))) snprintf(res, sizeof res, "FAILED cannot store the bundle");
    if (!bad) {
        fprintf(stderr, "update: %lu bytes received, signature good, handed to the installer\n", len);
        none = answer(dir, nonce, 60, res, sizeof res);
        /* (root refused it: no longer with root, handed_off.  Not where root never answered: it may install it still;
         * its later "no" says so, ota_handed.  OK restarts us) */
        if (strncmp(res, "OK ", 3) && !none) handed_off();
        if (none) handed_late();
    }
    reply(fd, res);
    pthread_mutex_unlock(&handoff_lock);
    free(b);
}

static void *conn(void *arg)
{
    struct pconn *c = arg; int fd = c->fd;
    handle(c);
    pthread_mutex_lock(&cl); c->fd = -1; pthread_mutex_unlock(&cl);     /* out of the table first: the fd is nobody's to shut down any more */
    close(fd);
    return NULL;
}

/* Connections the listener refused, half open until their answer has had time to arrive (refuse() says why).  The
 * listener's own, no lock: it parks and reaps between accepts.  All places taken: the oldest is closed for it */
static struct { int fd; long long until; } parked[8];

static void park_reap(int all)
{
    long long now = now_ms();
    for (int i = 0; i < 8; i++) if (parked[i].until && (all || now >= parked[i].until)) { close(parked[i].fd); parked[i].until = 0; }
}

static void park(int fd, const char *line)
{
    static long long said; static int unsaid;
    char b[200]; int n = snprintf(b, sizeof b, "%s\n", line), at = -1; long long now = now_ms();
    /* the log: a line per ten seconds, however many are refused (the log is on the flash) */
    unsaid++;
    if (now - said >= 10000) { fprintf(stderr, "update: %d connection%s refused: %s\n", unsaid, unsaid == 1 ? "" : "s", line); said = now; unsaid = 0; }
    send(fd, b, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL); shutdown(fd, SHUT_WR);
    for (int i = 0; i < 8; i++) { if (!parked[i].until) { at = i; break; } if (at < 0 || parked[i].until < parked[at].until) at = i; }
    if (parked[at].until) close(parked[at].fd);
    parked[at].fd = fd; parked[at].until = now + PARK_MS;
}

static void *listener(void *arg)
{
    int ls = net_listen(port);
    (void)arg;
    if (ls < 0) { perror("update: listen"); return NULL; }
    fprintf(stderr, "update: push port %d\n", port);
    for (int i = 0; i < CONNS_MAX; i++) pcs[i].fd = -1;
    for (;;) {
        struct pollfd p = { ls, POLLIN, 0 }; struct sockaddr_in sa; socklen_t sl = sizeof sa; uint32_t addr = 0;
        struct pconn *c = NULL, *old = NULL; int fd, mine = 0, pre = 0, r; pthread_t t; pthread_attr_t at;
        r = poll(&p, 1, 250); park_reap(0);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        if ((fd = net_accept(ls)) < 0) break;
        if (!getpeername(fd, (struct sockaddr *)&sa, &sl) && sa.sin_family == AF_INET) addr = sa.sin_addr.s_addr;
        pthread_mutex_lock(&cl);
        for (int i = 0; i < CONNS_MAX; i++) {
            if (pcs[i].fd < 0) { if (!c) c = &pcs[i]; continue; }
            if (!pcs[i].pre) continue;
            pre++; mine += pcs[i].addr == addr;
            if (!pcs[i].body && (!old || pcs[i].at < old->at)) old = &pcs[i];
        }
        if (mine >= PRE_ADDR) c = NULL;
        else if (c && pre >= PRE_MAX && !old) c = NULL;     /* (every one holds a bundle's place: not with BODIES_MAX < PRE_MAX, `body` being the place's own word) */
        else if (c && pre >= PRE_MAX) {
            /* the oldest of those that have not signed and receive no bundle makes room (the header says why not
             * the oldest of all): told, as far as its socket takes the line now, and shut down; its thread's read
             * ends and it goes.  Under cl, so the fd is still that thread's and no other connection's */
            static const char gone[] = "FAILED busy: others connected while this connection waited without a signature: connect again\n";
            old->pre = 0; old->ended = 1; send(old->fd, gone, sizeof gone - 1, MSG_DONTWAIT | MSG_NOSIGNAL); shutdown(old->fd, SHUT_RDWR);
        }
        if (c) { c->fd = fd; c->pre = 1; c->body = 0; c->ended = 0; c->addr = addr; c->at = now_ms(); }
        pthread_mutex_unlock(&cl);
        if (!c) { park(fd, mine >= PRE_ADDR ? "FAILED busy: this address has two connections here that have not signed yet" : "FAILED busy: too many connections just now, try again in a moment"); continue; }
        pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, conn, c)) { pthread_mutex_lock(&cl); c->fd = -1; pthread_mutex_unlock(&cl); close(fd); }
        pthread_attr_destroy(&at);
    }
    park_reap(1);
    return NULL;
}

int ota_start(int p)
{
    pthread_t t;
    port = p;
    return pthread_create(&t, NULL, listener, NULL) ? -1 : (pthread_detach(t), 0);
}
