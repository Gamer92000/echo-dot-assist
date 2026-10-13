/*
 * Playing to a Bluetooth speaker, the Echo's audio side.  Stock Alexa did it with Fluoride (inside btmanagerd on the
 * Dots, Android's Bluetooth app on the Echo Show); both firmwares have the whole route up to the A2DP HAL built in, so
 * hassmic stands in for Fluoride towards it (docs/re-a2dp-source.md, checked on an Echo Dot 2 with a2dpprobe; the Echo
 * Show's audioserver connects to the same sockets, docs/re-checkers.md):
 *
 *   route   the audio backend's audio_a2dp_route: the Dots' mixer moves its single output from the Echo's speaker to
 *           its A2DP HAL (audio.a2dp.default.so) on a LIPC property, Android's AudioPolicy moves the media strategy
 *           (every player of ours) when told an A2DP device is connected.  Everything mixed goes there (music,
 *           replies, earcons), with the Echo's volume applied, as with stock.
 *   HAL     AOSP's audio_a2dp_hw: it connects to two abstract sockets as Fluoride serves them, commands on .a2dp_ctrl (a
 *           byte each, a byte back: 0 done), 44.1 kHz stereo s16 on .a2dp_data once started.  The HAL writes as fast as
 *           it may: the reader sets the pace, like Fluoride's media timer.  The Dots' mixer sends START once when it
 *           opens the output and then plays all the time (silence included: its keep-alive for speakers that switch
 *           off); AudioFlinger puts the output in standby (SUSPEND) after 3 s of silence and STARTs it again with the
 *           next sound.
 *   AIPC    on every change of A2DPSourceConnect the Dots' mixer asks btmanagerd's service (uuid 0) for the speaker's
 *           name, and on BTUnpair (line out plugged while on the speaker) tells it to disconnect.  With btmanagerd gone
 *           each change waited ~20 s for a connect timeout; hassmic answers in its place (audio_a2dp_aipc).
 *   packets SBC (sbc.c) in RTP packets, as many frames as fit the media channel; a2dp.c sends them when the link has
 *           room.  A queue of at most QUEUE_MS: older packets go when the radio falls behind.
 *   awake   the mixer's silence is digital zero, and cheap speakers ("BT HIFI AUDIO" boards) mute their amplifier on it
 *           and lose the first ~0.5 s of a reply while unmuting (issue #24).  Optionally a 20 Hz tone at -50 dBFS goes
 *           under everything: not zero for them, inaudible for us (hearing needs ~78 dB SPL at 20 Hz, small speakers
 *           roll off far above it).  SBC keeps even a 1 LSB tone at bitpool 53 (libsbc decodes 0.94 rms from 0.82).
 *   standby with "Stay connected when off" (a2dp.c keeps the link while playing on the Echo, issue #25) the route is
 *           all that moves: "Play on Bluetooth speaker" then switches in well under a second instead of a reconnect.
 *           With keep-awake on as well, hal_thread makes that tone itself while the HAL is not started (not routed,
 *           or AudioFlinger's standby: zeros plus the tone, paced like the mixer, same RTP stream), so AVDTP stays
 *           started: the speaker neither mutes nor sees an idle link, and the HAL's START is acked at once.  The mixer has a
 *           single output, so this is all or nothing: music alone on the speaker would mean feeding it past the
 *           mixer, without the front end's echo reference for it (PLAN.md, issue #25).
 */
#define _GNU_SOURCE                     /* accept4 on the PC */
#include "btout.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "a2dp.h"
#include "aipc_api.h"
#include "audio.h"
#include "core.h"
#include "hci.h"
#include "sbc.h"

#define RATE 44100                      /* the HAL's output, fixed */
#define QUEUE_MS 200
#define QMAX 24
#define START_WAIT_MS 3000              /* for the speaker to start; the HAL waits up to 5 s for our answer */
#define AWAKE_HZ 20
#define AWAKE_AMP 100                   /* -50 dBFS */
#define RETRY_S 5                       /* a route the backend could not set, asked again */
#define CHECK_S 60                      /* while routed: whether the backend still has it (audioserver restarts) */

enum { CMD_CHECK_READY = 1, CMD_START, CMD_STOP, CMD_SUSPEND, CMD_GET_AUDIO_CONFIG };

static long long us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
/* m: */
static int ready, streaming, start_failed, cfg_gen;     /* a2dp.c's stream state; its configuration's generation */
static uint64_t ready_addr; static char ready_name[80]; static int bitpool; static unsigned per_packet;
static int want_route, routed_now; static uint64_t want_addr, routed_addr;
static int route_on = 1;                /* a2dp.c: play on the speaker; off, a link that stays is only kept awake */
static int recheck;                     /* while routed: 1 time to check the route, 2 the HAL let go of its control
                                           socket (audioserver gone?) */
static int want_abs, abs_pct, mode_now;  /* the speaker takes absolute volume, at that; core_speaker's mode as set */
static struct { size_t n; unsigned char b[1024]; } q[QMAX];
static int qh, qn; static long dropped;
static atomic_int hal_started, routed, keep_awake, idle_tone, abs_live;   /* abs_live: the Echo's volume is the speaker's */

/* ---------------------------------------------------------------- route */

static void deadline_s(struct timespec *ts, int s) { clock_gettime(CLOCK_REALTIME, ts); ts->tv_sec += s; }

/* One change at a time, in an order that never plays loud: the Echo's own volume comes back (and the mixer down from
 * full scale) before the route goes back to the Echo's speaker; the speaker's volume takes over only once routed.  A
 * move the backend could not make is tried again after RETRY_S (AudioPolicy while audioserver restarts); a route held
 * is checked when the HAL drops its control socket (audioserver gone) and every CHECK_S. */
static void *route_thread(void *arg)
{
    (void)arg;
    audio_a2dp_reset();
    int failed = 0;
    for (;;) {
        pthread_mutex_lock(&m);
        for (;;) {
            int off = routed_now && (!want_route || want_addr != routed_addr), on = !routed_now && want_route;
            int mode = routed_now && !off ? (want_abs ? SPEAKER_ABSOLUTE : SPEAKER_MIXER) : SPEAKER_NONE;
            if (off || (on && !failed) || (routed_now && !off && mode != mode_now) || (routed_now && recheck)) break;
            struct timespec ts; deadline_s(&ts, failed ? RETRY_S : CHECK_S);
            if (pthread_cond_timedwait(&cv, &m, &ts) == ETIMEDOUT) { if (failed) failed = 0; else if (routed_now) recheck = 1; }
        }
        int off = routed_now && (!want_route || want_addr != routed_addr);
        if (routed_now && !off && recheck) {
            uint64_t a = routed_addr; int why = recheck; recheck = 0;
            pthread_mutex_unlock(&m);
            /* the HAL gone: audioserver restarting, whose policy answers only once it is back (seen: >0.5 s) */
            int lost = audio_a2dp_lost(a);
            for (int i = 0; why == 2 && lost < 0 && i < 20; i++) { sleep(1); lost = audio_a2dp_lost(a); }
            pthread_mutex_lock(&m);
            if (lost > 0 && routed_now && routed_addr == a) {
                fprintf(stderr, "btout: the output is no longer on the speaker: moving it again\n");
                routed_now = 0; atomic_store(&routed, 0);
            }
            pthread_mutex_unlock(&m);
            continue;
        }
        if (routed_now) {                               /* volume first: down to the Echo's own before leaving */
            int mode = off ? SPEAKER_NONE : want_abs ? SPEAKER_ABSOLUTE : SPEAKER_MIXER, pct = abs_pct;
            if (mode != mode_now) {
                pthread_mutex_unlock(&m);
                atomic_store(&abs_live, 0); core_speaker(mode, pct); atomic_store(&abs_live, mode == SPEAKER_ABSOLUTE);
                pthread_mutex_lock(&m); mode_now = mode; pthread_mutex_unlock(&m);
                continue;
            }
        }
        int on = !routed_now; uint64_t a = on ? want_addr : routed_addr; char name[sizeof ready_name];
        snprintf(name, sizeof name, "%s", ready_name);
        pthread_mutex_unlock(&m);
        int r = audio_a2dp_route(on, a, name);
        pthread_mutex_lock(&m);
        /* a route that did not go back to the Echo is left for gone too: nothing better to do, and no loop over it */
        if (!r || !on) { routed_now = on; routed_addr = a; atomic_store(&routed, on); recheck = 0; failed = 0; }
        else failed = 1;
        pthread_mutex_unlock(&m);
    }
    return NULL;
}

/* ---------------------------------------------------------------- AIPC: btmanagerd's service, what the mixer asks of it */

static int aipc_handler(struct aipc_task *t)
{
    unsigned char *d = t->data; uint32_t len = t->len ? *t->len : 0; int32_t ok = 0;
    if (t->type != 0 || !d) return 0;                   /* events: clients coming and going */
    switch (t->function_id) {
    case AIPC_BT_SESSION_OPEN:
        if (len >= 11) { static uint32_t session; uint32_t s = ++session; memcpy(d + 1, &s, 4); memcpy(d + 7, &ok, 4); }
        break;
    case AIPC_BT_SESSION_CLOSE: if (len >= 9) memcpy(d + 5, &ok, 4); break;
    case AIPC_BT_GET_NAME:                              /* the mixer: its keep-alive table goes by name, and the event */
        if (len >= 0x0a + 249) {
            memcpy(d, &ok, 4); memset(d + 0x0a, 0, 249);
            pthread_mutex_lock(&m); snprintf((char *)d + 0x0a, 249, "%s", ready_name); pthread_mutex_unlock(&m);
        }
        break;
    case AIPC_BT_A2DP_SRC_DISCONNECT:
        if (len >= 12) { memcpy(d + 8, &ok, 4); fprintf(stderr, "btout: the mixer lets go of the speaker\n"); a2dp_out_enable(0); }
        break;
    default:                            /* ace_blemesh_service asks 0x0a every 2 s; we have no answer for it */
        if (t->status) *t->status = -1;
    }
    return 0;
}

/* The service as btmanagerd had it: directory and socket labelled btmanagerd_aipc_tmpfs, which the mixer may connect to
 * (created plainly they would be aipcd_tmpfs, which it may not).  The label is for what this thread creates. */
static void *aipc_thread(void *arg)
{
    (void)arg;
    void *lib = dlopen("libace_aipc.so", RTLD_NOW);
    aceAipc_start_fn start = lib ? (aceAipc_start_fn)dlsym(lib, "aceAipc_start") : NULL;
    if (!start) { fprintf(stderr, "btout: no AIPC: %s\n", dlerror()); return NULL; }
    static struct aipc_server_cfg cfg;
    cfg.uuid = AIPC_BT_UUID; cfg.handler = aipc_handler; cfg.max_payload = 0x2800; cfg.r10 = 10; cfg.r14 = 1; cfg.thread_option = 1;
    char path[64]; snprintf(path, sizeof path, "/proc/self/task/%ld/attr/fscreate", (long)syscall(SYS_gettid));
    for (int i = 0; ; i++) {
        static const char con[] = "u:object_r:btmanagerd_aipc_tmpfs:s0";
        int fd = open(path, O_WRONLY | O_CLOEXEC), h = -1;
        if (fd >= 0 && write(fd, con, sizeof con) < 0) { close(fd); fd = -1; }
        int r = start(&h, &cfg);
        if (fd >= 0) { if (write(fd, "", 0) < 0) {} close(fd); }
        if (!r) { fprintf(stderr, "btout: answering the mixer as the Bluetooth service\n"); return NULL; }
        /* btmanagerd's directory, until alexa-off.sh has removed it */
        if (!i) fprintf(stderr, "btout: AIPC service: error %d, trying again\n", r);
        sleep(i < 12 ? 5 : 60);
    }
}

/* ---------------------------------------------------------------- HAL sockets */

static int listen_abstract(const char *name)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };   /* sun_path[0] = 0, the name without its NUL: as the HAL connects */
    size_t n = strlen(name);
    memcpy(a.sun_path + 1, name, n);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, offsetof(struct sockaddr_un, sun_path) + 1 + n) < 0 || listen(fd, 5) < 0) {
        fprintf(stderr, "btout: %s: %s\n", name, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

static void ack(int fd, unsigned char v) { if (send(fd, &v, 1, MSG_NOSIGNAL) < 0) {} }

static unsigned char command(int fd, unsigned c)
{
    long long t;
    switch (c) {
    case CMD_CHECK_READY: pthread_mutex_lock(&m); c = ready; pthread_mutex_unlock(&m); return c ? 0 : 1;
    case CMD_START:
        pthread_mutex_lock(&m);
        if (!ready) { pthread_mutex_unlock(&m); fprintf(stderr, "btout: START without a speaker\n"); return 1; }
        start_failed = 0; atomic_store(&hal_started, 1);
        pthread_mutex_unlock(&m);
        hci_poke();                                     /* a2dp.c sends AVDTP START */
        for (t = us(); ; ) {
            pthread_mutex_lock(&m); int s = streaming, f = start_failed || !ready; pthread_mutex_unlock(&m);
            if (s) return 0;
            if (f || us() - t > START_WAIT_MS * 1000LL) { atomic_store(&hal_started, 0); fprintf(stderr, "btout: the speaker did not start\n"); return 1; }
            usleep(10000);
        }
    case CMD_STOP: case CMD_SUSPEND: atomic_store(&hal_started, 0); hci_poke(); return 0;
    case CMD_GET_AUDIO_CONFIG: {        /* the HAL's input side only, never used: the format anyway */
        uint32_t r = RATE; unsigned char ch = 2;
        ack(fd, 0); if (send(fd, &r, 4, MSG_NOSIGNAL) < 0 || send(fd, &ch, 1, MSG_NOSIGNAL) < 0) {}
        return 0xff; }
    default: return 0;                  /* ACL priority up / down: what the mixer plays is (not) silence */
    }
}

static void queue_push(const unsigned char *p, size_t n, unsigned frames)
{
    pthread_mutex_lock(&m);
    if (streaming && n <= sizeof q[0].b) {
        while (qn && (qn == QMAX || (long long)qn * frames * 1000 / RATE >= QUEUE_MS)) { qh = (qh + 1) % QMAX; qn--; dropped++; }
        int at = (qh + qn) % QMAX; memcpy(q[at].b, p, n); q[at].n = n; qn++;
    }
    pthread_mutex_unlock(&m);
    hci_poke();
}

/* the keep-awake tone over one block, in phase from the last; saturating, so music at full scale stays as it was */
static void awake_add(int16_t *pcm, unsigned frames)
{
    static int16_t tone[RATE / AWAKE_HZ]; static unsigned ph; static int made;
    if (!made) { for (unsigned i = 0; i < RATE / AWAKE_HZ; i++) tone[i] = (int16_t)lrint(AWAKE_AMP * sin(2 * M_PI * i / (RATE / AWAKE_HZ))); made = 1; }
    for (unsigned i = 0; i < frames; i++, ph = (ph + 1) % (RATE / AWAKE_HZ))
        for (int c = 0; c < 2; c++) { int v = pcm[2 * i + c] + tone[ph]; pcm[2 * i + c] = v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
}

/* SBC frames into RTP packets of per_packet frames each */
struct enc { struct sbc_enc sbc; int gen; unsigned k, nf; unsigned char pkt[1024]; size_t plen; uint16_t seq; uint32_t ts; };

static void enc_block(struct enc *e, int16_t *pcm)
{
    if (atomic_load(&keep_awake)) awake_add(pcm, SBC_ENC_FRAMES);
    pthread_mutex_lock(&m);
    if (e->gen != cfg_gen) { e->gen = cfg_gen; sbc_enc_init(&e->sbc, RATE, bitpool); e->k = per_packet; e->nf = 0; e->plen = 13; }
    pthread_mutex_unlock(&m);
    if (e->plen + sbc_enc_len(e->sbc.bitpool) > sizeof e->pkt) { e->nf = 0; e->plen = 13; }
    e->plen += sbc_encode(&e->sbc, pcm, e->pkt + e->plen);
    if (++e->nf < e->k) return;
    /* RTP: version 2, payload type 96, sequence, timestamp in samples, SSRC 1; then the SBC header: frame count */
    unsigned char *p = e->pkt;
    p[0] = 0x80; p[1] = 0x60; p[2] = e->seq >> 8; p[3] = e->seq; e->seq++;
    p[4] = e->ts >> 24; p[5] = e->ts >> 16; p[6] = e->ts >> 8; p[7] = e->ts; e->ts += e->nf * SBC_ENC_FRAMES;
    p[8] = p[9] = p[10] = 0; p[11] = 1; p[12] = e->nf;
    queue_push(p, e->plen, e->nf * SBC_ENC_FRAMES);
    e->nf = 0; e->plen = 13;
}

/* a speaker connected, keep-awake on, the HAL not started (the Echo plays itself, a2dp.c keeping the link; or routed,
 * AudioFlinger's output in standby): our tone.  A speaker that refused to start for it is not asked again (a2dp.c would
 * every upkeep) until the next stream. */
static int idle_wanted(void)
{
    pthread_mutex_lock(&m);
    int r = ready && !start_failed && !atomic_load(&hal_started) && atomic_load(&keep_awake);
    pthread_mutex_unlock(&m);
    return r;
}

static void *hal_thread(void *arg)
{
    int lc = ((int *)arg)[0], ld = ((int *)arg)[1], ctrl = -1, data = -1, gen = 0;
    static struct enc e = { .gen = -1, .k = 1, .plen = 13 };
    unsigned fill = 0; int16_t pcm[2 * SBC_ENC_FRAMES];
    long long since = 0, taken = 0, stat = us(); long got = 0; int peak = 0;
    for (;;) {
        int idle = data < 0 && idle_wanted();
        if (idle != gen) {
            gen = idle; atomic_store(&idle_tone, idle); hci_poke();      /* a2dp.c starts or suspends AVDTP by btout_want */
            if (idle) { since = us(); taken = 0; fill = 0; }
            fprintf(stderr, "btout: %s\n", idle ? "keeping the speaker awake while the Echo plays" : "keep-awake tone stopped");
        }
        /* the pace: what 44.1 kHz stereo s16 has delivered since the data socket opened (or the tone started).  Fallen
         * far behind (the mixer paused): start counting again rather than drain it in a burst */
        int paced = data >= 0 || gen;
        long long now = us(), allowed = (now - since) * (RATE * 4LL) / 1000000;
        if (paced && allowed - taken > RATE * 4LL / 5) { since = now - taken * 1000000 / (RATE * 4LL); allowed = taken; }
        int hold = paced && taken >= allowed;
        struct pollfd p[4] = { { lc, POLLIN, 0 }, { ld, POLLIN, 0 }, { ctrl, POLLIN, 0 }, { hold || data < 0 ? -1 : data, POLLIN, 0 } };
        if (poll(p, 4, hold ? 3 : gen ? 0 : 1000) < 0 && errno != EINTR) { usleep(100000); continue; }
        if (p[0].revents & POLLIN) {
            int fd = accept4(lc, NULL, NULL, SOCK_CLOEXEC);
            if (fd >= 0) { if (ctrl >= 0) close(ctrl); ctrl = fd; }
        }
        if (p[1].revents & POLLIN) {
            int fd = accept4(ld, NULL, NULL, SOCK_CLOEXEC);
            if (fd >= 0) { if (data >= 0) close(data); data = fd; since = us(); taken = 0; fill = 0; fprintf(stderr, "btout: mixer playing\n"); }
        }
        if (ctrl >= 0 && p[2].revents) {
            unsigned char c;
            if (recv(ctrl, &c, 1, 0) != 1) {
                close(ctrl); ctrl = -1; atomic_store(&hal_started, 0); hci_poke();
                pthread_mutex_lock(&m); if (routed_now) { recheck = 2; pthread_cond_signal(&cv); } pthread_mutex_unlock(&m);
            }
            else { unsigned char a = command(ctrl, c); if (a != 0xff) ack(ctrl, a); }
        }
        if (data >= 0 && p[3].revents) {
            ssize_t n = recv(data, (char *)pcm + fill, sizeof pcm - fill, 0);
            if (n <= 0) { close(data); data = -1; fprintf(stderr, "btout: mixer stopped\n"); continue; }
            fill += n; taken += n; got += n;
            if (fill < sizeof pcm) continue;
            fill = 0;
            for (int i = 0; i < 2 * SBC_ENC_FRAMES; i++) { int v = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (v > peak) peak = v; }
            enc_block(&e, pcm);                         /* the tone after the peak: that is the mixer's */
        } else if (gen && data < 0 && !hold) {
            memset(pcm, 0, sizeof pcm); taken += sizeof pcm;
            enc_block(&e, pcm);
        }
        if (us() - stat > 60000000LL) {             /* peak: whether the mixer sends more than its keep-alive silence */
            pthread_mutex_lock(&m); long d = dropped; dropped = 0; int s = streaming; pthread_mutex_unlock(&m);
            if ((s && !gen) || d) fprintf(stderr, "btout: %lld bytes/s from the mixer, peak %d, %ld packets dropped\n", got * 1000000LL / (us() - stat), peak, d);
            stat = us(); got = 0; peak = 0;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- API */

void btout_start(void)
{
    static int started; static int fds[2];
    if (started) return;
    started = 1;
    fds[0] = listen_abstract("/data/misc/bluedroid/.a2dp_ctrl"); fds[1] = listen_abstract("/data/misc/bluedroid/.a2dp_data");
    if (fds[0] < 0 || fds[1] < 0) return;
    pthread_t t;
    pthread_create(&t, NULL, hal_thread, fds); pthread_detach(t);
    pthread_create(&t, NULL, route_thread, NULL); pthread_detach(t);
    if (audio_a2dp_aipc()) { pthread_create(&t, NULL, aipc_thread, NULL); pthread_detach(t); }
}

void btout_ready(uint64_t addr, const char *name, int bp, unsigned mtu)
{
    pthread_mutex_lock(&m);
    ready = 1; streaming = 0; start_failed = 0; ready_addr = addr; snprintf(ready_name, sizeof ready_name, "%s", name);
    bitpool = bp; per_packet = (mtu - 13) / sbc_enc_len(bp);
    if (per_packet < 1) per_packet = 1;
    if (per_packet > 15) per_packet = 15;               /* the SBC header's frame count has 4 bits */
    cfg_gen++; qn = 0;
    want_route = route_on; want_addr = addr; pthread_cond_signal(&cv);
    pthread_mutex_unlock(&m);
    fprintf(stderr, "btout: \"%s\": SBC bitpool %d, %u frames per packet\n", name, bp, per_packet);
}

void btout_gone(void)
{
    pthread_mutex_lock(&m);
    ready = 0; streaming = 0; qn = 0; want_route = 0; want_abs = 0; pthread_cond_signal(&cv);
    pthread_mutex_unlock(&m);
}

void btout_route(int on) { pthread_mutex_lock(&m); route_on = on != 0; want_route = ready && route_on; pthread_cond_signal(&cv); pthread_mutex_unlock(&m); }
void btout_absvol(int on, int pct) { pthread_mutex_lock(&m); want_abs = on; abs_pct = pct; pthread_cond_signal(&cv); pthread_mutex_unlock(&m); }
void btout_streaming(int on) { pthread_mutex_lock(&m); streaming = on; if (!on) qn = 0; pthread_mutex_unlock(&m); }
void btout_start_failed(void) { pthread_mutex_lock(&m); start_failed = 1; pthread_mutex_unlock(&m); }
int  btout_want(void) { return atomic_load(&hal_started) || atomic_load(&idle_tone); }
int  btout_idle(void) { return atomic_load(&idle_tone); }
int  btout_speaker_volume(void) { return atomic_load(&abs_live); }
int  btout_routed(void) { return atomic_load(&routed); }
int  btout_moving(void)
{
    pthread_mutex_lock(&m);
    int r = want_route != routed_now || (routed_now && want_addr != routed_addr);
    pthread_mutex_unlock(&m);
    return r;
}

size_t btout_packet(unsigned char *buf, size_t max)
{
    size_t n = 0;
    pthread_mutex_lock(&m);
    if (qn && q[qh].n <= max) { n = q[qh].n; memcpy(buf, q[qh].b, n); }
    if (qn) { qh = (qh + 1) % QMAX; qn--; }
    pthread_mutex_unlock(&m);
    return n;
}

int btout_keep_awake(int set) { if (set >= 0) { atomic_store(&keep_awake, set != 0); hci_poke(); } return atomic_load(&keep_awake); }

long long btout_latency_us(void) { return atomic_load(&routed) ? a2dp_out_delay(-1) * 1000LL : 0; }
