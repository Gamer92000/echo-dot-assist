/* dropin.c on the PC (make unit-dropin), with call.c's media behind it and stand-ins for arb (one member, "echo-p"),
 * the core (core_call can be held up, as by a core_lock taken long) and the Voip stream.  This test is the other Echo:
 * it answers an invite as that Echo would, derives the call's key and sends and receives its audio over loopback UDP.
 *   D1 (a review of e8d52b9): packets of the call sent again by a host on the LAN pass the AEAD: each frame is taken once
 *      (a late one not seen yet still is), and only the newest moves the address the Echo sends its room to and keeps
 *      the call alive;
 *   D5: the core held up while calls are placed and refused: the event queue's "idle" is never what goes, the core
 *      ends on "idle" and the claim is let go;
 *   D6: a Drop In placed on the claim the one before left, and ended, before that one's "idle" was told: both calls'
 *      states reach the core while Drop In has the claim, and it goes after the last. */
#include "dropin.h"
#include "arb.h"
#include "call.h"
#include "audio.h"
#include "core.h"
#include "../third_party/monocypher.h"
#include <arpa/inet.h>
#include <fcntl.h>
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

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void ms(int n) { usleep((useconds_t)n * 1000); }

/* ---- arb: this Echo and one member */
static const uint8_t self_id[8] = { 1, 1, 1, 1, 1, 1, 1, 1 }, peer_id[8] = { 2, 2, 2, 2, 2, 2, 2, 2 };
static pthread_mutex_t am = PTHREAD_MUTEX_INITIALIZER;
static uint8_t told[16][ARB_MSG_MAX]; static size_t told_n[16]; static unsigned ntold;
int arb_running(void) { return 1; }
void arb_self(unsigned char id[8]) { memcpy(id, self_id, 8); }
int arb_derive(const char *label, unsigned char out[32]) { (void)label; memset(out, 0x42, 32); return 0; }
int arb_member(const char *node, unsigned char id[8], unsigned *ip)
{
    if (strcmp(node, "echo-p")) return -1;
    memcpy(id, peer_id, 8); *ip = inet_addr("127.0.0.1");
    return 0;
}
int arb_tell(const unsigned char to[8], const void *p, size_t n)
{
    (void)to;
    pthread_mutex_lock(&am);
    memcpy(told[ntold % 16], p, n); told_n[ntold % 16] = n; ntold++;
    pthread_mutex_unlock(&am);
    return 0;
}
static int last_told(int kind, uint8_t *out)        /* the newest message of that kind told the other Echo */
{
    int r = 0;
    pthread_mutex_lock(&am);
    for (unsigned i = ntold; i-- > 0 && i + 16 >= ntold;) if (told[i % 16][0] == kind) { memcpy(out, told[i % 16], told_n[i % 16]); r = 1; break; }
    pthread_mutex_unlock(&am);
    return r;
}

/* ---- the core: what it was told, and whether the one telling had the claim then (core_call's guard) */
static pthread_mutex_t cm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ccv = PTHREAD_COND_INITIALIZER;
static int held, holding;                           /* held: core_call waits; holding: one waits now */
#define TOLD_MAX 64
static struct { int st, shown; } heard[TOLD_MAX]; static atomic_int nheard;
static atomic_int results;
void core_call(int who, int st, const char *peer, int outgoing)
{
    (void)peer; (void)outgoing;
    pthread_mutex_lock(&cm);
    while (held) { holding = 1; pthread_cond_wait(&ccv, &cm); }
    holding = 0;
    pthread_mutex_unlock(&cm);
    int n = atomic_load(&nheard);
    if (n < TOLD_MAX) { heard[n].st = st; heard[n].shown = call_claimed() == who; atomic_store(&nheard, n + 1); }
}
void core_dropin_result(int r, const char *peer, int here) { (void)r; (void)peer; (void)here; atomic_fetch_add(&results, 1); }
const char *core_call_refusal(int incoming) { (void)incoming; return NULL; }
const char *core_node_name(void) { return "echo-self"; }
void core_node_of(const char *name, char node[64]) { snprintf(node, 64, "%s", name); }
static void hold(int on)
{
    pthread_mutex_lock(&cm); held = on; if (!on) pthread_cond_broadcast(&ccv); pthread_mutex_unlock(&cm);
}
static int waiting(void) { pthread_mutex_lock(&cm); int r = holding; pthread_mutex_unlock(&cm); return r; }
void ws_random(void *out, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, out, len) != (ssize_t)len) abort();
    close(fd);
}

/* ---- call.c's stand-ins: a Voip stream that takes everything, no call microphone, a core that is never muted */
int voip_open(unsigned rate, unsigned channels) { (void)rate; (void)channels; return 0; }
int voip_write(const void *data, size_t len) { (void)data; (void)len; usleep(20000); return 0; }
long long voip_queued_us(void) { return 0; }
void voip_close(void) {}
float callmic_echo_cap(void) { return -30; }
int callmic_present(void) { return 0; }
int callmic_open(void) { return -1; }
int callmic_read(const void **data) { (void)data; return -1; }
void callmic_close(void) {}
void core_call_listen(int on) { (void)on; }
int core_own_sound(void) { return 0; }
int core_muted(void) { return 0; }

/* ---- the other Echo's side of the call */
static int udp(uint16_t *port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = inet_addr("127.0.0.1") }; socklen_t l = sizeof a;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || getsockname(s, (struct sockaddr *)&a, &l)) abort();
    if (port) *port = ntohs(a.sin_port);
    return s;
}
static uint8_t call_id[8], key[32];
static uint16_t echo_port;
#define PKT (17 + 40 + 16)
static uint8_t pkts[64][PKT];                       /* the caller's frames as sent: what a host on the LAN captures */
static void seal(uint32_t seq)
{
    uint8_t *p = pkts[seq], nonce[24] = { 0 }, frame[40];
    memcpy(p, "HMD1", 4); memcpy(p + 4, call_id, 8); p[12] = 0;            /* from the caller */
    for (int i = 0; i < 4; i++) p[13 + i] = (uint8_t)(seq >> 8 * i);
    memset(frame, 0xf8 ^ (int)seq, sizeof frame); frame[0] = 0xf8;        /* (stored, not played: the Voip stream is a sink) */
    memcpy(nonce, p + 4, 13);
    crypto_aead_lock(p + 17, p + 17 + 40, key, nonce, p, 17, frame, 40);
}
static void send_from(int s, uint32_t seq)
{
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr.s_addr = inet_addr("127.0.0.1"), .sin_port = htons(echo_port) };
    sendto(s, pkts[seq], PKT, 0, (struct sockaddr *)&to, sizeof to);
    ms(3);                                          /* (the loop takes one packet a round) */
}
static int got(int s, int wait)                     /* packets the Echo sent to that socket within WAIT ms */
{
    int n = 0; uint8_t b[1500]; long long end = now_ms() + wait;
    for (;;) {
        long long left = end - now_ms();
        struct pollfd pf = { s, POLLIN, 0 };
        if (left <= 0 || poll(&pf, 1, (int)left) <= 0) break;
        if (recv(s, b, sizeof b, 0) > 0) n++;
    }
    return n;
}
static unsigned rx(void) { struct call_stats st; call_stats(&st); return st.rx; }
static int until(int (*cond)(void), int wait) { long long end = now_ms() + wait; while (!cond() && now_ms() < end) ms(5); return cond(); }
static int ended(void) { int n = atomic_load(&nheard); return n && heard[n - 1].st == DROPIN_IDLE; }
static int unclaimed(void) { return call_claimed() == CALL_NOBODY; }
static int live(void) { return dropin_status(NULL, 0) == DROPIN_LIVE && call_live(); }

int main(void)
{
    uint16_t pa, pb, port; int a = udp(&pa), b = udp(&pb), t = udp(&port);
    close(t);                                       /* (a free port for the Echo's audio) */
    setenv("HASSMIC_DROPIN_TIMES", "3000 1000", 1); /* lost: 1 s without a frame */
    CHECK(dropin_start(port) == 0, "Drop In runs (audio on %u)", port);
    echo_port = port;

    /* ---- D1: a call from the other Echo, answered at once (drop_in_answer auto) */
    { uint8_t p[ARB_MSG_MAX] = { 0 }, esk[32], epk[32], acc[ARB_MSG_MAX], kd[32], shared[32], m[56];
      ws_random(call_id, 8); ws_random(esk, 32); crypto_x25519_public_key(epk, esk);
      p[0] = 1; memcpy(p + 1, call_id, 8); p[9] = (uint8_t)(pa & 0xff); p[10] = (uint8_t)(pa >> 8); memcpy(p + 11, epk, 32); p[43] = 0;
      dropin_message(peer_id, "echo-p", inet_addr("127.0.0.1"), p, 44);
      CHECK(last_told(2, acc) && until(live, 1000), "the other Echo's invite: accepted, connected");
      arb_derive("", kd); crypto_x25519(shared, esk, acc + 11);
      memcpy(m, shared, 32); memcpy(m + 32, call_id, 8); memcpy(m + 40, peer_id, 8); memcpy(m + 48, self_id, 8);
      crypto_blake2b_keyed(key, 32, kd, 32, m, sizeof m); }
    for (uint32_t s = 0; s < 64; s++) seal(s);
    for (uint32_t s = 0; s < 50; s++) if (s != 47) send_from(a, s);
    ms(100);
    CHECK(rx() == 49, "49 frames of the other Echo taken (%u)", rx());
    send_from(a, 47); ms(60);
    CHECK(rx() == 50, "a late frame not taken before: taken (%u)", rx());
    unsigned before = rx();
    send_from(a, 49);                                                     /* the other Echo's own, twice */
    for (uint32_t s = 40; s < 50; s++) send_from(b, s);                  /* within the window, taken already */
    send_from(b, 30); for (uint32_t s = 5; s < 10; s++) send_from(b, s); /* further back */
    ms(100);
    CHECK(rx() == before, "D1: frames sent again (from another host and from the Echo's own address) are not taken (%u more)", rx() - before);
    got(a, 50); got(b, 1);
    { int16_t pcm[CALL_FRAME] = { 0 }; for (int k = 0; k < 5; k++) call_mic(pcm, CALL_FRAME, 0); }
    int to_a = got(a, 300), to_b = got(b, 100);
    CHECK(to_a == 5 && to_b == 0, "D1: the room goes on to the other Echo, not to the host that sent its frames again (%d there, %d to the other)", to_a, to_b);
    /* the other Echo moves (its address changed mid-call): its next frame moves the room with it */
    send_from(b, 50); ms(60);
    { int16_t pcm[CALL_FRAME] = { 0 }; for (int k = 0; k < 3; k++) call_mic(pcm, CALL_FRAME, 0); }
    to_b = got(b, 300); to_a = got(a, 100);
    CHECK(to_b == 3 && to_a == 0, "a newer frame from another address: the room follows it (%d, %d to the old one)", to_b, to_a);
    /* the other Echo is gone; a host sends its old frames again and again: the call ends as without them */
    long long t0 = now_ms(); int gone = 0;
    while (now_ms() - t0 < 3000) { send_from(a, (uint32_t)(now_ms() / 7 % 50)); ms(40); if (!live()) { gone = 1; break; } }
    CHECK(gone && now_ms() - t0 < 1800, "D1: no new frame for 1 s: the call ends, frames sent again do not keep it (after %lld ms of them)", now_ms() - t0);
    if (live()) dropin_hangup("test");             /* (each part on its own, also where one fails) */
    CHECK(until(ended, 500) && until(unclaimed, 500), "the core heard \"idle\", the claim is let go");

    /* ---- D5: the core held up ("calling" waits in core_call); the call hung up; 20 calls refused here meanwhile */
    atomic_store(&nheard, 0); hold(1);
    { char err[128]; int r = dropin_call("echo-p", err, sizeof err);
      CHECK(r == 0 && until(waiting, 1000), "a call placed; the core is held up telling it \"calling\"");
      dropin_hangup("test");
      for (int i = 0; i < 20; i++) dropin_call("echo-self", err, sizeof err);
      hold(0); }
    ms(300);
    { int n = atomic_load(&nheard);
      CHECK(n >= 2 && heard[n - 1].st == DROPIN_IDLE, "D5: the core's last word is \"idle\": it was not the event that went (%d told, last %d)", n, n ? heard[n - 1].st : -1); }
    CHECK(unclaimed(), "D5: and the claim is let go (%d)", call_claimed());

    /* ---- D6: call A, hung up; call B placed on the claim A left and hung up, all before A's "idle" was told */
    atomic_store(&nheard, 0); hold(1);
    { char err[128]; int ra = dropin_call("echo-p", err, sizeof err);
      int held_ = until(waiting, 1000);
      dropin_hangup("A");
      int rb = dropin_call("echo-p", err, sizeof err);
      dropin_hangup("B");
      CHECK(ra == 0 && held_ && rb == 0, "D6: calls A and B placed and hung up while the core was held up (%s)", rb ? err : "");
      hold(0); }
    ms(300);
    { int n = atomic_load(&nheard), shown = 0, seq_ok = n == 4;
      int want[4] = { DROPIN_CALLING, DROPIN_IDLE, DROPIN_CALLING, DROPIN_IDLE };
      for (int i = 0; i < n && i < 4; i++) { shown += heard[i].shown; seq_ok &= heard[i].st == want[i]; }
      CHECK(seq_ok && shown == 4, "D6: calling, idle, calling, idle, each from Drop In while it had the claim (%d told, %d with the claim)", n, shown); }
    CHECK(unclaimed(), "D6: the claim let go after B's \"idle\" (%d)", call_claimed());

    printf("dropin: %s\n", fails ? "FAILED" : "all good");
    return fails != 0;
}
