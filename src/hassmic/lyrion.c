/*
 * Lyrion Music Server player: SlimProto as Squeezelite speaks it, so that Lyrion (formerly Logitech Media Server) lists
 * the Echo among its players, plays to it, groups it with its other players and sets its volume.  Off unless the
 * setting "lyrion" is on (settings.c keeps it); beside Sendspin, one music source at a time (core_music).
 *
 *   finding     the page's address (state/lyrion.server), else Lyrion's own discovery: "e" and TLVs broadcast to UDP
 *               3483, the answer "E" and TLVs (Slim::Networking::Discovery: NAME, IPAD, JSON = its web port, VERS).
 *               Sent from DISC_PORT: the firewall admits UDP in 16384-32767 only, and an answer to a broadcast is no
 *               reply conntrack knows (lockdown.sh)
 *   control     TCP 3483 out.  HELO as Squeezelite (device 12) with the Echo's Wi-Fi MAC as the player's id and the
 *               model as its name; then strm/aude/audg/setd/serv/cont/codc in, STAT/RESP/DSCO/SETD/META out.  A server
 *               silent for 35 s is gone (it sends strm 't' every few seconds)
 *   stream      each strm 's' is an HTTP request Lyrion wrote for us, to the address it names (its own, as a rule):
 *               FLAC (dr_flac), MP3 (minimp3) or PCM (raw, or WAV/AIFF with their header), what Lyrion transcodes all
 *               else to (HELO's "flc,pcm,mp3"; MaxSampleRate 48000: the mixer's highest).  Decoded on the stream's own
 *               thread into a queue of s16 stereo at the track's rate, the next track behind the current one: Lyrion
 *               sends it once STMd said this one is decoded, and the output plays on across the join (gapless)
 *   output      the queue into the music stream (audio.h), opened at the track's rate (the mixer resamples, as for a
 *               phone's 44.1 kHz).  Autostart, or STMl and a wait for strm 'u' (a group starts together), at a time in
 *               our jiffies; strm 'p'/'a' with a time insert silence or skip music (how Lyrion keeps a group in step:
 *               it compares the play points, AccuratePlayPoints, each STAT's elapsed time against its jiffies)
 *   volume      Lyrion's gain (audg, 16.16) back to its 0..100 by SqueezePlay's curve (lyrion.h) as the Echo's volume
 *               (core_set_volume, as Sendspin's) once it stays put; its fades (pause, stop, skip) and its mute only on
 *               this stream (fade_q16).  The Echo's volume goes to Lyrion by its
 *               JSON-RPC ("mixer volume"), as the action button, "stop" and skips do: SlimProto has no way for a
 *               Squeezelite to say them.  On a new link the Echo's volume goes first: Lyrion keeps the last one it set
 *               per player, which would otherwise change the Echo's every time it connects
 */
#include "lyrion.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "audio.h"
#include "board.h"
#include "btout.h"
#include "core.h"
#include "duck.h"
#include "musiclight.h"
#include "net.h"
#include "netio.h"
#include "resample.h"
#include "settings.h"
#include "statefile.h"
#include "../third_party/dr_flac.h"         /* the implementations: sendspin.c (dr_flac), proto_esphome.c (minimp3) */
#include "../third_party/minimp3.h"

#define SLIM_PORT 3483
#define DISC_PORT 28933                 /* in the firewall's 16384-32767, beside Drop In's 28932 */
#define CH 2
#define FRAME (2 * CH)
#define CHUNK_FRAMES 2048               /* decoded per chunk: ~46 ms at 44.1 kHz */
#define Q_MAX (4u << 20)                /* decoded PCM held: ~24 s at 44.1 kHz; the stream waits (TCP holds Lyrion back) */
#define MIXER_TARGET_US 120000          /* as sendspin.c: stop, pause and the duck reach the speaker this soon */
#define SILENCE_MS 20
#define LINK_SILENT_S 35                /* Squeezelite's: Lyrion sends every 5 s, mysqueezebox.com every 30 */
#define RPC_SLOTS 8

/* Mixer + DAC + amp after our write, as sendspin.c measured it (src/tools/latency.c: 83..88 ms write to micRaw, ~70 ms
 * of it to the speaker; HASSMIC_OUTPUT_LATENCY_MS as there); a Bluetooth speaker adds its own.  Lyrion reads elapsed
 * time as heard, and a group starts as heard: this goes out of both */
static long long output_latency_us = 70000;
static long long out_latency_us(void) { return output_latency_us + btout_latency_us(); }

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
static void state_path(char *out, size_t n, const char *f) { snprintf(out, n, "%s/%s", state_dir(), f); }

static uint32_t jiffies(void)                           /* SlimProto's clock: ms, wrapping, ours alone */
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((unsigned long long)ts.tv_sec * 1000 + (unsigned long long)ts.tv_nsec / 1000000);
}
static long long mono_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---------------------------------------------------------------- state */

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;   /* the queue, the output's state, the stream, the setting */
static atomic_int enabled, relink;                      /* relink: the setting or the address changed, drop the link */
static atomic_int started;
static int trace;                                       /* HASSMIC_LYRION_TRACE: every message in and out in the log */

enum { L_OFF, L_SEARCHING, L_CONNECTING, L_UP, L_UNREACHABLE };
static const char *const lstates[] = { "off", "searching", "connecting", "up", "unreachable" };
/* lk */
static int lstate; static char given[160], srv_name[64], srv_version[24]; static struct in_addr srv_ip; static int http_port = 9000;
static uint8_t mac[6]; static char mac_s[18];

/* What the control thread sends next (the others only set these: a send can block, the output must not) */
enum { P_STMS = 1, P_STMD = 2, P_STML = 4, P_STMU = 8, P_STMO = 16, P_STMN = 32, P_DSCO = 64, P_RESP = 128, P_META = 256 };
static atomic_int pend;
static char resp[4096]; static size_t resp_len;          /* lk: the stream's HTTP header, for RESP */
static char meta[4096]; static size_t meta_len;          /* lk: ICY metadata, for META */
static int dsco_reason;                                  /* lk */

/* The stream: one at a time (Lyrion sends the next strm 's' only after STMd) */
static struct {
    int gen;                    /* strm 's' counted: chunks carry it, and the output sees a new track by it */
    int fd;                     /* the HTTP connection, -1 */
    int stop;                   /* 'f', 'q', a new link: the thread ends */
    int running;                /* a stream thread is out there */
    int done, decoded;          /* the body ended; the decoder took all of it (STMd) */
    int autostart;              /* 0 wait for 'u' (after STMl), 1 start at the threshold; 2, 3 the same after 'cont' */
    int cont; unsigned metaint;
    unsigned long long bytes, threshold;
    char fmt, size, rate, chans, endian;
    struct in_addr ip; int port;
    char req[2048]; size_t reqlen;
    float gain;                 /* the track's replay gain: strm's 16.16, 1 without */
} st = { .fd = -1 };

/* The decoded PCM, s16 stereo, at its track's rate */
struct chunk { struct chunk *next; int gen; unsigned rate; float gain; size_t frames, off; int16_t pcm[]; };
static struct chunk *qh, *qt; static size_t qbytes;

enum { O_STOPPED, O_BUFFER, O_WAIT, O_START_AT, O_RUNNING, O_PAUSED };
static struct {
    int state; uint32_t start_at; long long pause_frames, skip_frames;
    int threshold;              /* strm's output_threshold: tenths of a second buffered before an autostart */
    int cur_gen; unsigned cur_rate; unsigned long long track_frames;   /* the track the output plays, frames written */
    int stml_sent, stmo_sent;
    long long heard_ms, heard_at; /* the track's position as heard, and when that was so (the output's last write) */
    int epoch;                  /* a flush: what the output thread had in hand is stale */
} out;
/* Lyrion fades by volume: on pause, stop and skip it steps the gain down in 50 ms steps (and up again on play), muting is
 * gain 0.  Those reach the Echo's volume only once the gain stays put for VOL_SETTLE_MS; until then they are a gain on
 * this stream alone (fade, 16.16, against the volume Lyrion and the Echo agree on): the Echo's volume, which Home
 * Assistant shows and the replies follow, never went down to 2 and back for a pause (2026-11-17, Lyrion 9.2.0) */
#define VOL_SETTLE_MS 500
static atomic_int fade_q16 = 65536;
static atomic_int lms_vol = -1;                 /* Lyrion's volume as it last said, 0..100; -1 not yet on this link */
static atomic_int vol_pending = -1;             /* the control thread's: a volume waiting to settle, and since when */
static long long vol_pending_at; static uint32_t vol_gain;
static atomic_int vol_first;                    /* a new link: the Echo's volume goes to Lyrion before Lyrion's is taken */
static atomic_int vol_fixed;                    /* audg without adjust: Lyrion's "fixed volume", the Echo keeps its own */

static void queue_free_locked(int keep_gen)     /* every chunk, or (keep_gen >= 0) those after keep_gen's */
{
    struct chunk **pp = &qh, *c; qt = NULL;
    while ((c = *pp)) {
        if (keep_gen >= 0 && c->gen <= keep_gen) { qt = c; pp = &c->next; continue; }
        *pp = c->next; qbytes -= c->frames * FRAME; free(c);
    }
    pthread_cond_broadcast(&cv);
}

/* ---------------------------------------------------------------- the setting, the address, the page */

static void load_given(void)
{
    char p[300]; FILE *f; state_path(p, sizeof p, "lyrion.server");
    given[0] = 0;
    if ((f = fopen(p, "r"))) { if (!fgets(given, sizeof given, f)) given[0] = 0; fclose(f); }
    given[strcspn(given, "\r\n \t")] = 0;
}

int lyrion_enable(int set)
{
    if (set < 0) return atomic_load(&enabled);
    if (atomic_exchange(&enabled, !!set) != !!set) {
        atomic_store(&relink, 1);
        if (atomic_load(&started)) fprintf(stderr, "lyrion: %s\n", set ? "on" : "off");
        pthread_mutex_lock(&lk); pthread_cond_broadcast(&cv); pthread_mutex_unlock(&lk);
    }
    return !!set;
}

/* a host name, an IPv4 address or [IPv6], and an optional :port; nothing that could end the file's line or the JSON */
int lyrion_set_server(const char *host)
{
    char p[300], v[sizeof given]; size_t n = strlen(host);
    if (n >= sizeof given) return -1;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)host[i]) && !strchr(".-_:[]", host[i])) return -1;
    snprintf(v, sizeof v, "%s", host);
    state_path(p, sizeof p, "lyrion.server");
    if (n) { char line[sizeof given + 1]; int l = snprintf(line, sizeof line, "%s\n", v); if (statefile_write(p, 0666, line, (size_t)l)) return -1; }
    else unlink(p);
    pthread_mutex_lock(&lk); snprintf(given, sizeof given, "%s", v); pthread_cond_broadcast(&cv); pthread_mutex_unlock(&lk);
    atomic_store(&relink, 1);
    fprintf(stderr, "lyrion: server %s\n", n ? v : "found on the network");
    return 0;
}

static void jesc(char *o, size_t cap, const char *s)
{
    size_t k = 0;
    for (; *s && k + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c < 0x20) k += (size_t)snprintf(o + k, cap - k, "\\u%04x", c);
        else o[k++] = (char)c;
    }
    o[k] = 0;
}

size_t lyrion_status_json(char *o, size_t cap)
{
    char name[sizeof srv_name * 6 + 1], ver[sizeof srv_version * 6 + 1], g[sizeof given * 6 + 1], where[48] = "";
    pthread_mutex_lock(&lk);
    int ls = atomic_load(&enabled) ? lstate : L_OFF, playing = out.state == O_RUNNING || out.state == O_START_AT;
    jesc(name, sizeof name, srv_name); jesc(ver, sizeof ver, srv_version); jesc(g, sizeof g, given);
    if (srv_ip.s_addr && (ls == L_UP || ls == L_CONNECTING)) snprintf(where, sizeof where, "%s:%d", inet_ntoa(srv_ip), http_port);
    pthread_mutex_unlock(&lk);
    int n = snprintf(o, cap, "{\"on\":%s,\"state\":\"%s\",\"server\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"given\":\"%s\",\"playing\":%s,\"id\":\"%s\"}",
                     atomic_load(&enabled) ? "true" : "false", lstates[ls], where, ls == L_UP ? name : "", ls == L_UP ? ver : "", g,
                     playing ? "true" : "false", mac_s);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

/* The player's id: the Echo's Wi-Fi MAC, as Lyrion keys a player's settings by it (a reset keeps it, as Home
 * Assistant's name does).  The PC has none: HASSMIC_LYRION_MAC, else one made of the name, locally administered */
static void mac_load(void)
{
    char s[32] = ""; const char *e = getenv("HASSMIC_LYRION_MAC"); unsigned b[6];
    if (e) snprintf(s, sizeof s, "%s", e);
    else { FILE *f = fopen("/sys/class/net/wlan0/address", "r"); if (f) { if (fscanf(f, "%31s", s) != 1) s[0] = 0; fclose(f); } }
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6 && (b[0] | b[1] | b[2] | b[3] | b[4] | b[5]))
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
    else {
        uint32_t h = 2166136261u;
        for (const char *p = core_name ? core_name : "hassmic"; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        mac[0] = 0x02; mac[1] = 0x68; put32(mac + 2, h);
    }
    snprintf(mac_s, sizeof mac_s, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* The name Lyrion shows: the Echo's own (core_name) until someone renames the player in Lyrion, which sends it (setd 0)
 * and gets it back from us after every restart, as Squeezelite's -N file does */
static void name_get(char *out, size_t n)
{
    char p[300]; FILE *f; state_path(p, sizeof p, "lyrion.name");
    out[0] = 0;
    if ((f = fopen(p, "r"))) { if (!fgets(out, (int)n, f)) out[0] = 0; fclose(f); }
    out[strcspn(out, "\r\n")] = 0;
    if (!out[0]) snprintf(out, n, "%s", core_name ? core_name : board.default_name);
}

/* ---------------------------------------------------------------- JSON-RPC: what the Echo's controls ask of Lyrion */

static pthread_cond_t rpc_cv = PTHREAD_COND_INITIALIZER;
static char rpcq[RPC_SLOTS][96]; static int rpc_n;      /* lk */

static void rpc(const char *cmd)                         /* cmd: the command's JSON array, as Lyrion's CLI words it */
{
    pthread_mutex_lock(&lk);
    if (lstate == L_UP && rpc_n < RPC_SLOTS) { snprintf(rpcq[rpc_n++], sizeof rpcq[0], "%s", cmd); pthread_cond_signal(&rpc_cv); }
    pthread_mutex_unlock(&lk);
}

static void *rpc_thread(void *a)
{
    (void)a;
    for (;;) {
        char cmd[sizeof rpcq[0]], host[INET_ADDRSTRLEN], port[8], id[sizeof mac_s];
        pthread_mutex_lock(&lk);
        while (!rpc_n) pthread_cond_wait(&rpc_cv, &lk);
        snprintf(cmd, sizeof cmd, "%s", rpcq[0]); memmove(rpcq[0], rpcq[1], sizeof rpcq[0] * (size_t)--rpc_n);
        inet_ntop(AF_INET, &srv_ip, host, sizeof host); snprintf(port, sizeof port, "%d", http_port); snprintf(id, sizeof id, "%s", mac_s);
        pthread_mutex_unlock(&lk);
        char body[256], req[512], ans[128] = "";
        int bl = snprintf(body, sizeof body, "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"%s\",%s]}", id, cmd);
        int rl = snprintf(req, sizeof req, "POST /jsonrpc.js HTTP/1.0\r\nHost: %s:%s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s", host, port, bl, body);
        int fd = net_connect(host, port, 3);
        if (fd < 0) { fprintf(stderr, "lyrion: %s not sent: no answer on %s:%s\n", cmd, host, port); continue; }
        ssize_t got = write(fd, req, (size_t)rl) == rl ? read(fd, ans, sizeof ans - 1) : -1;
        close(fd);
        if (got > 0) ans[got] = 0;
        int code = 0;
        if (got <= 0 || sscanf(ans, "HTTP/%*s %d", &code) != 1 || code != 200)
            fprintf(stderr, "lyrion: %s refused (%d%s)\n", cmd, code, code == 401 ? ": Lyrion asks for a password, which the Echo cannot give" : "");
        else fprintf(stderr, "lyrion: sent %s\n", cmd);
    }
    return NULL;
}

static int loaded(void) { int s = out.state; return s != O_STOPPED; }       /* lk: a track is here, playing or paused */

int lyrion_button(void)
{
    pthread_mutex_lock(&lk); int l = lstate == L_UP && loaded(), paused = out.state == O_PAUSED; pthread_mutex_unlock(&lk);
    if (!l) return 0;
    rpc(paused ? "[\"pause\",\"0\"]" : "[\"pause\",\"1\"]");
    return 1;
}

int lyrion_skip(int dir)
{
    pthread_mutex_lock(&lk); int p = lstate == L_UP && (out.state == O_RUNNING || out.state == O_START_AT); pthread_mutex_unlock(&lk);
    if (!p) return 0;
    rpc(dir > 0 ? "[\"playlist\",\"index\",\"+1\"]" : "[\"playlist\",\"index\",\"-1\"]");
    return 1;
}

int lyrion_stop(void)
{
    pthread_mutex_lock(&lk); int p = lstate == L_UP && (out.state == O_RUNNING || out.state == O_START_AT || out.state == O_BUFFER); pthread_mutex_unlock(&lk);
    if (!p) return 0;
    rpc("[\"pause\",\"1\"]");
    return 1;
}

void lyrion_pause(void) { lyrion_stop(); }

void lyrion_volume_changed(int percent)
{
    if (!atomic_load(&started) || atomic_load(&vol_fixed) || atomic_load(&vol_first) || percent == atomic_load(&lms_vol)) return;
    char c[48]; snprintf(c, sizeof c, "[\"mixer\",\"volume\",\"%d\"]", percent);
    atomic_store(&lms_vol, percent);            /* what it will say back: no second round */
    rpc(c);
}

/* ---------------------------------------------------------------- the control link */

static int ctl_fd = -1;                                 /* the control thread's; sends under send_lk */
static pthread_mutex_t send_lk = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long link_bytes;                   /* stream bytes since the link came (HELO's on a reconnect) */

static int send_pkt(const char op[4], const void *a, size_t an, const void *b, size_t bn)
{
    uint8_t h[8]; int r = -1;
    memcpy(h, op, 4); put32(h + 4, (uint32_t)(an + bn));
    pthread_mutex_lock(&send_lk);
    if (ctl_fd >= 0) {
        struct iovec v[3] = { { h, 8 }, { (void *)a, an }, { (void *)b, bn } }; size_t want = 8 + an + bn;
        struct msghdr m = { .msg_iov = v, .msg_iovlen = 3 };
        ssize_t w = sendmsg(ctl_fd, &m, MSG_NOSIGNAL);
        r = w == (ssize_t)want ? 0 : -1;
        if (r) { shutdown(ctl_fd, SHUT_RDWR); }                     /* the read loop sees it and links again */
    }
    pthread_mutex_unlock(&send_lk);
    return r;
}

/* STAT (Slim::Networking::Slimproto _stat_handler): 53 bytes after the header.  Elapsed: the track as heard now */
static void send_stat(const char ev[4], uint32_t server_ts)
{
    uint8_t s[53] = { 0 }; uint32_t now = jiffies();
    if (trace && memcmp(ev, "STMt", 4)) fprintf(stderr, "lyrion: > %.4s\n", ev);
    pthread_mutex_lock(&lk);
    long long ms = out.heard_ms;
    if (out.state == O_RUNNING && out.heard_at) ms += (mono_us() - out.heard_at) / 1000;
    long long cap = out.cur_rate ? (long long)(out.track_frames * 1000 / out.cur_rate) : 0;
    if (ms > cap) ms = cap;
    if (ms < 0) ms = 0;
    unsigned long long bytes = st.bytes; size_t qb = qbytes;
    pthread_mutex_unlock(&lk);
    memcpy(s, ev, 4);
    put32(s + 7, Q_MAX); put32(s + 11, (uint32_t)(qb < Q_MAX ? qb : Q_MAX));          /* stream buffer: the queue */
    put32(s + 15, (uint32_t)(bytes >> 32)); put32(s + 19, (uint32_t)bytes);
    put16(s + 23, 0xffff);                                                               /* signal: wired, as Squeezelite */
    put32(s + 25, now);
    put32(s + 29, Q_MAX); put32(s + 33, (uint32_t)(qb < Q_MAX ? qb : Q_MAX));
    put32(s + 37, (uint32_t)(ms / 1000));
    put32(s + 43, (uint32_t)ms);
    memcpy(s + 47, &server_ts, 4);                                                       /* as it came */
    send_pkt("STAT", s, sizeof s, NULL, 0);
}

static void send_helo(int again)
{
    char cap[320];
    int cl = snprintf(cap, sizeof cap, "Model=squeezelite,AccuratePlayPoints=1,HasDigitalOut=1,Firmware=%s,ModelName=%s,MaxSampleRate=48000,flc,pcm,mp3",
                      VERSION, board.product);
    uint8_t h[36] = { 0 };
    h[0] = 12;                                          /* SqueezePlay's id, as Squeezelite: Slim::Player::SqueezePlay */
    memcpy(h + 2, mac, 6);
    put16(h + 24, again ? 0x4000 : 0);                  /* reconnect: Lyrion keeps the player's state */
    put32(h + 26, (uint32_t)(link_bytes >> 32)); put32(h + 30, (uint32_t)link_bytes);
    send_pkt("HELO", h, sizeof h, cap, (size_t)(cl < (int)sizeof cap ? cl : (int)sizeof cap - 1));
}

static void send_name(void)
{
    char n[96]; uint8_t id = 0; name_get(n, sizeof n);
    send_pkt("SETD", &id, 1, n, strlen(n) + 1);
}

/* ---------------------------------------------------------------- the stream thread: HTTP in, PCM out */

struct src { int fd, gen; unsigned metaint, to_meta; uint8_t buf[16384]; size_t len, pos; };

static int stream_live(int gen) { pthread_mutex_lock(&lk); int ok = !st.stop && st.gen == gen; pthread_mutex_unlock(&lk); return ok; }

static ssize_t raw_read(struct src *s, void *o, size_t n)       /* the socket: 0 at the end or when told to stop */
{
    for (;;) {
        struct pollfd p = { .fd = s->fd, .events = POLLIN };
        int r = poll(&p, 1, 500);
        if (!stream_live(s->gen)) return 0;
        if (r == 0) continue;
        ssize_t got = recv(s->fd, o, n, 0);
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (got > 0) { pthread_mutex_lock(&lk); st.bytes += (unsigned long long)got; link_bytes += (unsigned long long)got; pthread_mutex_unlock(&lk); }
        return got < 0 ? -1 : got;
    }
}

/* The body, ICY metadata taken out (every metaint bytes: a length byte, then 16 x that of "StreamTitle='...';"), which
 * goes to Lyrion as META: it shows a station's title from it */
static size_t src_read(struct src *s, void *o, size_t n)
{
    size_t got = 0;
    while (got < n) {
        if (s->pos == s->len) {
            ssize_t r = raw_read(s, s->buf, sizeof s->buf);
            if (r <= 0) break;
            s->len = (size_t)r; s->pos = 0;
        }
        if (s->metaint && !s->to_meta) {
            uint8_t l = s->buf[s->pos++]; size_t m = (size_t)l * 16, k = 0; char text[16 * 255 + 1];
            while (k < m) {
                if (s->pos == s->len) { ssize_t r = raw_read(s, s->buf, sizeof s->buf); if (r <= 0) return got; s->len = (size_t)r; s->pos = 0; }
                size_t take = s->len - s->pos < m - k ? s->len - s->pos : m - k;
                memcpy(text + k, s->buf + s->pos, take); k += take; s->pos += take;
            }
            text[m] = 0;
            if (m) {
                pthread_mutex_lock(&lk); meta_len = strlen(text); memcpy(meta, text, meta_len); pthread_mutex_unlock(&lk);
                atomic_fetch_or(&pend, P_META);
            }
            s->to_meta = s->metaint;
            continue;
        }
        size_t take = s->len - s->pos;
        if (take > n - got) take = n - got;
        if (s->metaint && take > s->to_meta) take = s->to_meta;
        memcpy((uint8_t *)o + got, s->buf + s->pos, take);
        s->pos += take; got += take;
        if (s->metaint) s->to_meta -= (unsigned)take;
    }
    return got;
}

/* s16 stereo onto the queue; waits while it is full.  -1: the stream is to stop */
static int push(int gen, unsigned rate, float gain, const int16_t *pcm, size_t frames)
{
    if (!frames) return 0;
    struct chunk *c = malloc(sizeof *c + frames * FRAME);
    if (!c) return -1;
    c->next = NULL; c->gen = gen; c->rate = rate; c->gain = gain; c->frames = frames; c->off = 0;
    memcpy(c->pcm, pcm, frames * FRAME);
    pthread_mutex_lock(&lk);
    while (qbytes + frames * FRAME > Q_MAX && !st.stop && st.gen == gen) pthread_cond_wait(&cv, &lk);
    if (st.stop || st.gen != gen) { pthread_mutex_unlock(&lk); free(c); return -1; }
    if (qt) qt->next = c; else qh = c;
    qt = c; qbytes += frames * FRAME;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&lk);
    return 0;
}

/* n frames of `ch` channels into stereo (mono doubled, beyond two the first two) */
static void to_stereo(const int16_t *in, int ch, size_t n, int16_t *o)
{
    for (size_t i = 0; i < n; i++) { o[2 * i] = in[i * (size_t)ch]; o[2 * i + 1] = in[i * (size_t)ch + (ch > 1)]; }
}

/* dr_flac reads the stream through this: first what decode_flac put in front of it (pre), then the socket */
struct flac_in { struct src *s; uint8_t pre[64]; size_t pre_len, pre_pos; };
static size_t flac_read_cb(void *u, void *o, size_t n)
{
    struct flac_in *f = u; size_t k = 0;
    if (f->pre_pos < f->pre_len) {
        k = f->pre_len - f->pre_pos < n ? f->pre_len - f->pre_pos : n;
        memcpy(o, f->pre + f->pre_pos, k); f->pre_pos += k;
    }
    return k + (k < n ? src_read(f->s, (uint8_t *)o + k, n - k) : 0);
}
static drflac_bool32 flac_seek_cb(void *u, int off, drflac_seek_origin o)
{
    uint8_t skip[256]; struct flac_in *f = u;          /* forward only (an ID3 tag): read past it */
    if (o != DRFLAC_SEEK_CUR || off < 0) return DRFLAC_FALSE;
    while (off > 0) { size_t t = off < (int)sizeof skip ? (size_t)off : sizeof skip; if (flac_read_cb(f, skip, t) != t) return DRFLAC_FALSE; off -= (int)t; }
    return DRFLAC_TRUE;
}
static drflac_bool32 flac_tell_cb(void *u, drflac_int64 *c) { (void)u; (void)c; return DRFLAC_FALSE; }

/* A stream Lyrion starts in the middle of a file (a seek, and a resume after the whole track had come: _JumpToTime) begins
 * at a frame, without "fLaC" and STREAMINFO: dr_flac's relaxed open gave up on those (Lyrion 9.2.0, 2026-11-17).  The
 * first frame's header has what STREAMINFO would (rate, channels, bits): one is made of it and put in front */
static int flac_streaminfo(const uint8_t h[4], uint8_t *o)
{
    static const unsigned rates[16] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
    static const int sizes[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };
    if (h[0] != 0xff || (h[1] & 0xfe) != 0xf8) return -1;
    unsigned rate = rates[h[2] & 15]; int ca = h[3] >> 4, ch = ca < 8 ? ca + 1 : ca <= 10 ? 2 : 0, bits = sizes[(h[3] >> 1) & 7];
    if (!rate || !ch || !bits) return -1;                  /* in STREAMINFO only, or at the header's end: not made up */
    memcpy(o, "fLaC", 4);
    o[4] = 0x80; o[5] = 0; o[6] = 0; o[7] = 34;            /* the last block: STREAMINFO, 34 bytes */
    uint8_t *b = o + 8; memset(b, 0, 34);
    put16(b, 16); put16(b + 2, 65535);                     /* block sizes: any; frame sizes and length unknown (0) */
    b[10] = (uint8_t)(rate >> 12); b[11] = (uint8_t)(rate >> 4);
    b[12] = (uint8_t)((rate & 15) << 4 | (ch - 1) << 1 | (bits - 1) >> 4); b[13] = (uint8_t)(((bits - 1) & 15) << 4);
    return 42;
}

static int decode_flac(struct src *s, float gain)
{
    struct flac_in in = { .s = s };
    if (src_read(s, in.pre, 4) != 4) return -1;
    if (memcmp(in.pre, "fLaC", 4) && memcmp(in.pre, "ID3", 3)) {
        uint8_t h[4]; int k;
        memcpy(h, in.pre, 4);
        if ((k = flac_streaminfo(h, in.pre)) < 0) { fprintf(stderr, "lyrion: not a FLAC stream (%02x %02x %02x %02x)\n", h[0], h[1], h[2], h[3]); return -1; }
        memcpy(in.pre + k, h, 4); in.pre_len = (size_t)k + 4;
    } else in.pre_len = 4;
    drflac *f = drflac_open(flac_read_cb, flac_seek_cb, flac_tell_cb, &in, NULL);
    if (!f) { fprintf(stderr, "lyrion: not a FLAC stream\n"); return -1; }
    int ch = f->channels; unsigned rate = f->sampleRate; int r = 0;
    int16_t *pcm = malloc((size_t)CHUNK_FRAMES * (size_t)ch * 2), *o = malloc((size_t)CHUNK_FRAMES * FRAME);
    fprintf(stderr, "lyrion: FLAC, %u Hz, %d channels, %d bits%s\n", rate, ch, f->bitsPerSample, in.pre_len > 4 ? ", from the middle" : "");
    for (drflac_uint64 n; pcm && o && (n = drflac_read_pcm_frames_s16(f, CHUNK_FRAMES, pcm)) > 0; ) {
        to_stereo(pcm, ch, (size_t)n, o);
        if (push(s->gen, rate, gain, o, (size_t)n)) { r = -1; break; }
    }
    free(pcm); free(o); drflac_close(f);
    return r;
}

static int decode_mp3(struct src *s, float gain)
{
    struct { mp3dec_t dec; mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; int16_t o[MINIMP3_MAX_SAMPLES_PER_FRAME]; uint8_t buf[32768]; } *m = malloc(sizeof *m);
    size_t have = 0; int eof = 0, said = 0;
    if (!m) return -1;
    mp3dec_t *dec = &m->dec; mp3d_sample_t *pcm = m->pcm; int16_t *o = m->o; uint8_t *buf = m->buf;
    mp3dec_init(dec);
    for (;;) {
        if (!eof && have < 16384) { size_t g = src_read(s, buf + have, 32768 - have); if (!g) eof = 1; have += g; }
        if (!have) break;
        mp3dec_frame_info_t info; int n = mp3dec_decode_frame(dec, buf, (int)have, pcm, &info);
        if (!info.frame_bytes) { if (eof) break; if (have == 32768) have = 0; continue; }       /* no frame in it: more */
        memmove(buf, buf + info.frame_bytes, have - (size_t)info.frame_bytes); have -= (size_t)info.frame_bytes;
        if (n <= 0) continue;
        if (!said++) fprintf(stderr, "lyrion: MP3, %d Hz, %d channels, %d kbit/s\n", info.hz, info.channels, info.bitrate_kbps);
        to_stereo(pcm, info.channels, (size_t)n, o);
        if (push(s->gen, (unsigned)info.hz, gain, o, (size_t)n)) { free(m); return -1; }
    }
    free(m);
    return said ? 0 : -1;
}

static const unsigned pcm_rates[] = { 11025, 22050, 32000, 44100, 48000, 8000, 12000, 16000, 24000, 96000, 88200, 176400, 192000 };

/* WAV ("RIFF"/"WAVE": fmt, then data) or AIFF ("FORM"/"AIFF": COMM, then SSND) header off the stream, for strm's '?' */
static int pcm_header(struct src *s, unsigned *rate, int *ch, int *bits, int *big, int *unsign)
{
    uint8_t h[12], c[8], b[40];
    if (src_read(s, h, 12) != 12) return -1;
    int aiff = !memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4));
    if (!aiff && (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))) return -1;
    for (;;) {
        if (src_read(s, c, 8) != 8) return -1;
        uint32_t len = aiff ? get32(c + 4) : (uint32_t)c[4] | (uint32_t)c[5] << 8 | (uint32_t)c[6] << 16 | (uint32_t)c[7] << 24;
        if (!aiff && !memcmp(c, "fmt ", 4) && len >= 16 && len <= sizeof b) {
            if (src_read(s, b, len) != len) return -1;
            *ch = b[2] | b[3] << 8; *rate = (unsigned)(b[4] | b[5] << 8 | b[6] << 16 | (unsigned)b[7] << 24); *bits = b[14] | b[15] << 8;
            *big = 0; *unsign = *bits == 8;
            if (len & 1) src_read(s, b, 1);
        } else if (aiff && !memcmp(c, "COMM", 4) && len >= 18 && len <= sizeof b) {
            if (src_read(s, b, len) != len) return -1;
            *ch = b[0] << 8 | b[1]; *bits = b[6] << 8 | b[7];
            int e = ((b[8] & 0x7f) << 8 | b[9]) - 16383 - 63; unsigned long long m = 0;
            for (int i = 0; i < 8; i++) m = m << 8 | b[10 + i];
            *rate = (unsigned)llround(ldexp((double)m, e)); *big = 1; *unsign = 0;
            if (len & 1) src_read(s, b, 1);
        } else if (!aiff && !memcmp(c, "data", 4)) return 0;
        else if (aiff && !memcmp(c, "SSND", 4)) { if (src_read(s, b, 8) != 8) return -1; for (uint32_t k = get32(b); k; k--) if (src_read(s, b, 1) != 1) return -1; return 0; }
        else for (uint32_t k = len + (len & 1); k; ) { size_t t = k < sizeof b ? k : sizeof b; if (src_read(s, b, t) != t) return -1; k -= (uint32_t)t; }
    }
}

static int decode_pcm(struct src *s, float gain, char size, char rate_c, char chans, char endian)
{
    unsigned rate = 0; int ch = 0, bits = 0, big = 0, unsign = 0;
    if (size == '?' || rate_c == '?' || chans == '?') {
        if (pcm_header(s, &rate, &ch, &bits, &big, &unsign)) { fprintf(stderr, "lyrion: PCM without a WAV or AIFF header\n"); return -1; }
    } else {
        static const int sizes[] = { 8, 16, 24, 32 };
        int si = size - '0', ri = rate_c - '0';
        if (si < 0 || si > 3 || ri < 0 || ri >= (int)(sizeof pcm_rates / sizeof *pcm_rates)) return -1;
        bits = sizes[si]; rate = pcm_rates[ri]; ch = chans - '0'; big = endian == '0';
    }
    if (ch < 1 || ch > 8 || (bits != 8 && bits != 16 && bits != 24 && bits != 32) || rate < 8000 || rate > 384000) return -1;
    fprintf(stderr, "lyrion: PCM, %u Hz, %d channels, %d bits\n", rate, ch, bits);
    int bps = bits / 8, fb = bps * ch; uint8_t *in = malloc((size_t)CHUNK_FRAMES * (size_t)fb); int16_t *mono = malloc((size_t)CHUNK_FRAMES * (size_t)ch * 2), *o = malloc((size_t)CHUNK_FRAMES * FRAME);
    int r = 0; size_t have = 0;
    while (in && mono && o) {
        size_t g = src_read(s, in + have, (size_t)CHUNK_FRAMES * (size_t)fb - have);
        have += g;
        size_t n = have / (size_t)fb;
        if (!n) break;
        for (size_t i = 0; i < n * (size_t)ch; i++) {
            const uint8_t *p = in + i * (size_t)bps;
            int v = bps == 1 ? (unsign ? (p[0] - 128) : (int8_t)p[0]) << 8                 /* the top 16 bits of each sample */
                  : big ? (int16_t)(p[0] << 8 | p[1]) : (int16_t)(p[bps - 1] << 8 | p[bps - 2]);
            mono[i] = (int16_t)v;
        }
        to_stereo(mono, ch, n, o);
        if (push(s->gen, rate, gain, o, n)) { r = -1; break; }
        memmove(in, in + n * (size_t)fb, have - n * (size_t)fb); have -= n * (size_t)fb;
        if (!g) break;
    }
    free(in); free(mono); free(o);
    return r;
}

static void stream_end(int gen, int reason, int decoded)
{
    pthread_mutex_lock(&lk);
    if (st.gen == gen) {
        if (st.fd >= 0) { close(st.fd); st.fd = -1; }
        if (!st.done && !st.stop) { dsco_reason = reason; atomic_fetch_or(&pend, P_DSCO); }
        st.done = 1;
        if (!st.stop) { st.decoded = 1; atomic_fetch_or(&pend, decoded ? P_STMD : P_STMN); }
    }
    if (st.gen == gen) st.running = 0;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&lk);
}

static void *stream_thread(void *a)
{
    struct src *s = a; char ip[INET_ADDRSTRLEN], port[8], h[sizeof resp]; size_t hl = 0;
    pthread_mutex_lock(&lk);
    int gen = st.gen; char fmt = st.fmt, size = st.size, rate = st.rate, chans = st.chans, endian = st.endian; float gain = st.gain;
    inet_ntop(AF_INET, &st.ip, ip, sizeof ip); snprintf(port, sizeof port, "%d", st.port);
    char req[sizeof st.req]; size_t rl = st.reqlen; memcpy(req, st.req, rl);
    pthread_mutex_unlock(&lk);
    s->gen = gen; s->fd = net_connect(ip, port, 5);
    if (s->fd < 0) { free(s); stream_end(gen, 3, 0); return NULL; }                       /* 3: unreachable */
    { int on = 1; setsockopt(s->fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on); }
    pthread_mutex_lock(&lk); if (st.gen == gen && !st.stop) st.fd = s->fd; pthread_mutex_unlock(&lk);
    if (write(s->fd, req, rl) != (ssize_t)rl) { stream_end(gen, 1, 0); free(s); return NULL; }
    /* the header, for RESP: Lyrion reads the status and, for a stream it did not proxy, the type and metadata interval */
    while (hl < 4 || memcmp(h + hl - 4, "\r\n\r\n", 4)) {
        if (hl == sizeof h - 1) { stream_end(gen, 2, 0); free(s); return NULL; }
        ssize_t r = raw_read(s, h + hl, 1);
        if (r <= 0) { stream_end(gen, r < 0 ? 2 : 0, 0); free(s); return NULL; }
        hl++;
    }
    pthread_mutex_lock(&lk);
    if (st.gen == gen) { memcpy(resp, h, hl); resp_len = hl; atomic_fetch_or(&pend, P_RESP); }
    /* direct streams (autostart 2, 3): Lyrion answers RESP with cont (the metadata interval), or codc for an unknown type */
    while (st.gen == gen && !st.stop && st.autostart >= 2 && !st.cont) pthread_cond_wait(&cv, &lk);
    if (trace) {
        fprintf(stderr, "lyrion: request %.*s", (int)rl, req);
        fprintf(stderr, "lyrion: answer %.*s", (int)hl, h);
    }
    s->metaint = s->to_meta = st.metaint; fmt = st.fmt; size = st.size; rate = st.rate; chans = st.chans; endian = st.endian;
    int live = st.gen == gen && !st.stop;
    pthread_mutex_unlock(&lk);
    int r = !live ? -1 : fmt == 'f' ? decode_flac(s, gain) : fmt == 'm' ? decode_mp3(s, gain) : fmt == 'p' ? decode_pcm(s, gain, size, rate, chans, endian) : -1;
    if (live && fmt != 'f' && fmt != 'm' && fmt != 'p') fprintf(stderr, "lyrion: format '%c' not supported\n", fmt);
    stream_end(gen, 0, r == 0);
    free(s);
    return NULL;
}

/* ---------------------------------------------------------------- strm and the rest of the server's commands */

static void wait_ms_locked(int ms)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000; ts.tv_sec += ts.tv_nsec / 1000000000; ts.tv_nsec %= 1000000000;
    pthread_cond_timedwait(&cv, &lk, &ts);
}

static void stream_stop_locked(void)                    /* the stream thread told to end, and waited for (2 s at most) */
{
    st.stop = 1;
    if (st.fd >= 0) shutdown(st.fd, SHUT_RDWR);
    pthread_cond_broadcast(&cv);
    for (int i = 0; i < 40 && st.running; i++) wait_ms_locked(50);
    if (st.running) fprintf(stderr, "lyrion: the stream thread is slow to end\n");
}

static void flush_all_locked(void)
{
    stream_stop_locked();
    queue_free_locked(-1);
    out.state = O_STOPPED; out.epoch++; out.pause_frames = out.skip_frames = 0;
    out.track_frames = 0; out.heard_ms = 0; out.heard_at = 0;
}

static unsigned rate_now_locked(void) { return out.cur_rate ? out.cur_rate : qh ? qh->rate : 44100; }

/* strm (Slim::Player::Squeezebox stream_s): the command, then autostart, format, PCM size, rate, channels, endianness,
 * threshold (KB), S/PDIF, transition period and type, flags, output threshold (0.1 s), slaves, replay gain or a time
 * (16.16 / ms), server port and IPv4 address, and for 's' the HTTP request */
static void strm(const uint8_t *p, size_t n)
{
    if (n < 24) return;
    uint32_t arg = get32(p + 14);
    switch (p[0]) {
    case 't': { uint32_t ts; memcpy(&ts, p + 14, 4); send_stat("STMt", ts); break; }     /* the server's stamp as it came */
    case 'q':
        pthread_mutex_lock(&lk); flush_all_locked(); pthread_mutex_unlock(&lk);
        send_stat("STMf", 0);
        break;
    case 'f': {
        /* the stream, and of the queue what has not started playing: Lyrion takes back a next track it sent */
        pthread_mutex_lock(&lk);
        int streaming = st.running && !st.done, had = 0;
        stream_stop_locked();
        int playing = out.state == O_RUNNING || out.state == O_PAUSED || out.state == O_START_AT;
        for (struct chunk *c = qh; c; c = c->next) if (!playing || c->gen != out.cur_gen) had = 1;
        if (playing) queue_free_locked(out.cur_gen);
        else { queue_free_locked(-1); out.state = O_STOPPED; out.epoch++; }
        pthread_mutex_unlock(&lk);
        if (streaming || had) send_stat("STMf", 0);
        break;
    }
    case 'p':                                           /* a time: that much silence (a group's sync); none: pause */
        pthread_mutex_lock(&lk);
        if (arg) out.pause_frames += (long long)arg * rate_now_locked() / 1000;
        else if (out.state != O_STOPPED) out.state = O_PAUSED;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&lk);
        if (!arg) send_stat("STMp", 0);
        break;
    case 'a':                                           /* skip that much ahead (a group's sync) */
        pthread_mutex_lock(&lk); out.skip_frames += (long long)arg * rate_now_locked() / 1000; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&lk);
        break;
    case 'u':                                           /* go on, or start, at a time in our jiffies (a group starts together) */
        pthread_mutex_lock(&lk);
        if (out.state != O_STOPPED || qh) { out.state = arg ? O_START_AT : O_RUNNING; out.start_at = arg; }
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&lk);
        send_stat("STMr", 0);
        break;
    case 's': {
        size_t rl = n - 24;
        if (rl >= sizeof st.req) { fprintf(stderr, "lyrion: request too long (%zu bytes)\n", rl); return; }
        struct src *s = calloc(1, sizeof *s);
        if (!s) return;
        pthread_mutex_lock(&lk);
        stream_stop_locked();
        st.gen++; st.stop = 0; st.done = 0; st.decoded = 0; st.cont = 0; st.metaint = 0; st.bytes = 0; st.fd = -1;
        st.autostart = p[1] - '0'; st.fmt = (char)p[2]; st.size = (char)p[3]; st.rate = (char)p[4]; st.chans = (char)p[5]; st.endian = (char)p[6];
        st.threshold = (unsigned long long)p[7] * 1024; st.gain = arg ? (float)arg / 65536 : 1;
        st.port = p[18] << 8 | p[19]; memcpy(&st.ip.s_addr, p + 20, 4);
        if (!st.ip.s_addr) st.ip = srv_ip;                  /* 0: the server's own address */
        memcpy(st.req, p + 24, rl); st.reqlen = rl;
        out.threshold = p[12];
        if (out.state == O_STOPPED) { out.state = st.autostart & 1 ? O_BUFFER : O_WAIT; out.stml_sent = 0; }
        st.running = 1;
        int gen = st.gen;
        pthread_mutex_unlock(&lk);
        fprintf(stderr, "lyrion: track %d, format '%c', autostart %c, from %s:%d\n", gen, p[2], p[1], inet_ntoa(st.ip), st.port);
        send_stat("STMf", 0);                           /* as Squeezelite: the last one flushed */
        pthread_t t;
        if (pthread_create(&t, NULL, stream_thread, s)) { free(s); stream_end(gen, 1, 0); }
        else pthread_detach(t);
        send_stat("STMc", 0);
        break;
    }
    default: fprintf(stderr, "lyrion: strm '%c' not known\n", p[0]);
    }
}

static void fade_set(uint32_t g, int vol)               /* the stream's gain: Lyrion's against the volume agreed on */
{
    double f = vol > 0 ? (double)g / lyrion_gain16(vol) : g ? 1 : 0;      /* 1 exactly while they agree */
    atomic_store(&fade_q16, (int)lround((f > 1 ? 1 : f) * 65536));
}

static void audg(const uint8_t *p, size_t n)
{
    if (n < 18) return;
    uint32_t l = get32(p + 10), r = get32(p + 14), g = l > r ? l : r;        /* the balance: the louder side */
    uint32_t ol = get32(p), orr = get32(p + 4), old = ol > orr ? ol : orr;
    if (!p[8]) {                                        /* "Volume control: fixed": the Echo's own volume stays */
        if (!atomic_exchange(&vol_fixed, 1)) fprintf(stderr, "lyrion: volume fixed in Lyrion, the Echo keeps its own\n");
        atomic_store(&fade_q16, 65536); atomic_store(&vol_first, 0); atomic_store(&vol_pending, -1);
        return;
    }
    atomic_store(&vol_fixed, 0);
    vol_gain = g;
    if (!g) { atomic_store(&fade_q16, 0); atomic_store(&vol_pending, -1); return; }     /* muted, or the end of a fade */
    int v = lyrion_volume_of_gain(g, old);
    if (atomic_exchange(&vol_first, 0)) {
        /* the link's first: the Echo's volume to Lyrion instead, which then says it back */
        pthread_mutex_lock(&core_lock); int now = core_volume();
        if (v != now) { fprintf(stderr, "lyrion: Lyrion had volume %d, the Echo's %d goes to it\n", v, now); lyrion_volume_changed(now); }
        else atomic_store(&lms_vol, v);
        pthread_mutex_unlock(&core_lock);
        fade_set(g, v);
        return;
    }
    int agreed = atomic_load(&lms_vol);
    fade_set(g, agreed > 0 ? agreed : v);
    if (v != agreed) { atomic_store(&vol_pending, v); vol_pending_at = mono_us(); }
    else atomic_store(&vol_pending, -1);
}

/* serve_link: a volume of Lyrion's that stayed put is the Echo's */
static void vol_settle(long long now)
{
    int v = atomic_load(&vol_pending);
    if (v < 0 || now - vol_pending_at < VOL_SETTLE_MS * 1000LL) return;
    atomic_store(&vol_pending, -1);
    atomic_store(&lms_vol, v);
    pthread_mutex_lock(&core_lock);
    if (core_volume() != v) core_set_volume(v);
    pthread_mutex_unlock(&core_lock);
    fade_set(vol_gain, v);
}

static struct in_addr serv_to;                          /* the control thread's: serv asked for another server */

static void process(const uint8_t *m, size_t n)
{
    const uint8_t *p = m + 4; size_t pn = n - 4;
    if (trace && !(!memcmp(m, "strm", 4) && pn && p[0] == 't')) {
        fprintf(stderr, "lyrion: < %.4s", (const char *)m);
        if (!memcmp(m, "strm", 4) && pn >= 24) fprintf(stderr, " %c autostart %c format %c arg %u", p[0], p[1], p[2], get32(p + 14));
        if (!memcmp(m, "audg", 4) && pn >= 18) fprintf(stderr, " adjust %u gain %u", p[8], get32(p + 10));
        fprintf(stderr, " (%zu bytes)\n", pn);
    }
    if (!memcmp(m, "strm", 4)) strm(p, pn);
    else if (!memcmp(m, "audg", 4)) audg(p, pn);
    else if (!memcmp(m, "aude", 4) && pn >= 2) { if (!p[0] && !p[1]) fprintf(stderr, "lyrion: switched off in Lyrion\n"); }
    else if (!memcmp(m, "setd", 4) && pn >= 1) {
        if (p[0] == 0 && pn > 1) {                      /* renamed in Lyrion: kept, and said back */
            char name[96], path[300]; size_t l = strnlen((const char *)p + 1, pn - 1);
            if (l >= sizeof name) l = sizeof name - 1;
            memcpy(name, p + 1, l); name[l] = 0;
            char cur[96]; name_get(cur, sizeof cur);
            if (l && strcmp(name, cur)) {               /* Lyrion says the name back after every query: only a new one is kept */
                char line[100]; int k = snprintf(line, sizeof line, "%s\n", name); state_path(path, sizeof path, "lyrion.name"); statefile_write(path, 0666, line, (size_t)k);
                fprintf(stderr, "lyrion: named \"%s\" in Lyrion\n", name);
            }
            send_name();
        } else if (p[0] == 0) send_name();
    } else if (!memcmp(m, "serv", 4) && pn >= 4) {
        memcpy(&serv_to.s_addr, p, 4);
        if (ntohl(serv_to.s_addr) <= 1) serv_to.s_addr = 0;      /* 0.0.0.1: mysqueezebox.com, which is gone */
        else { fprintf(stderr, "lyrion: told to go to %s\n", inet_ntoa(serv_to)); atomic_store(&relink, 1); }
    } else if (!memcmp(m, "cont", 4) && pn >= 4) {
        pthread_mutex_lock(&lk); st.metaint = get32(p); st.cont = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&lk);
    } else if (!memcmp(m, "codc", 4) && pn >= 5) {
        pthread_mutex_lock(&lk); st.fmt = (char)p[0]; st.size = (char)p[1]; st.rate = (char)p[2]; st.chans = (char)p[3]; st.endian = (char)p[4]; pthread_mutex_unlock(&lk);
    }
    /* vers, grfe, grfb, visu, ledc, i2cc, knob, body, ...: a display's or another model's, nothing for us */
}

/* ---------------------------------------------------------------- finding the server, the link */

/* Lyrion's discovery: "e" + TLVs (tag, length, value) asked for, "E" + TLVs back.  0: found */
static int discover(struct in_addr *ip, int *port, char *name, size_t nn, char *ver, size_t vn)
{
    static const char ask[] = "eIPAD\0NAME\0JSON\0VERS\0";
    const char *to = getenv("HASSMIC_LYRION_DISCOVERY");        /* tests: a unicast address, where nothing broadcasts */
    const char *dp = getenv("HASSMIC_LYRION_DISC_PORT");
    int s = net_socket(AF_INET, SOCK_DGRAM, 0), on = 1, found = -1;
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(dp ? atoi(dp) : DISC_PORT) }, d = { .sin_family = AF_INET, .sin_port = htons(SLIM_PORT) };
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on); setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (bind(s, (struct sockaddr *)&me, sizeof me)) { fprintf(stderr, "lyrion: discovery: cannot bind UDP %d: %s\n", ntohs(me.sin_port), strerror(errno)); close(s); return -1; }
    d.sin_addr.s_addr = to ? inet_addr(to) : htonl(INADDR_BROADCAST);
    for (int round = 0; round < 3 && found && atomic_load(&enabled) && !atomic_load(&relink); round++) {
        sendto(s, ask, sizeof ask - 1, 0, (struct sockaddr *)&d, sizeof d);
        struct pollfd pf = { .fd = s, .events = POLLIN }; uint8_t r[1500]; struct sockaddr_in from; socklen_t fl = sizeof from;
        while (found && poll(&pf, 1, 1500) > 0) {
            ssize_t k = recvfrom(s, r, sizeof r, 0, (struct sockaddr *)&from, &fl);
            if (k < 1 || r[0] != 'E') continue;
            *ip = from.sin_addr; *port = 9000; name[0] = ver[0] = 0;
            for (ssize_t i = 1; i + 5 <= k; ) {
                size_t l = r[i + 4]; char v[256];
                if (i + 5 + (ssize_t)l > k) break;
                memcpy(v, r + i + 5, l); v[l] = 0;
                if (!memcmp(r + i, "NAME", 4)) { size_t c = l < nn ? l : nn - 1; memcpy(name, v, c); name[c] = 0; }
                else if (!memcmp(r + i, "VERS", 4)) { size_t c = l < vn ? l : vn - 1; memcpy(ver, v, c); ver[c] = 0; }
                else if (!memcmp(r + i, "JSON", 4) && atoi(v) > 0 && atoi(v) < 65536) *port = atoi(v);
                else if (!memcmp(r + i, "IPAD", 4)) { struct in_addr a; if (inet_pton(AF_INET, v, &a) == 1 && a.s_addr) *ip = a; }
                i += 5 + (ssize_t)l;
            }
            found = 0;
        }
    }
    close(s);
    return found;
}

/* The link's loop: messages in (two bytes of length, then the opcode and its data), what the other threads want sent out */
static void serve_link(int fd)
{
    uint8_t *m = malloc(65536); long long last_rx = mono_us(), last_t = 0, since = mono_us();
    if (!m) return;
    while (atomic_load(&enabled) && !atomic_load(&relink)) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, 50);
        if (r > 0) {
            uint8_t h[2];
            if (read_full(fd, h, 2) != 2) { fprintf(stderr, "lyrion: link closed by the server\n"); break; }
            size_t n = (size_t)h[0] << 8 | h[1];
            if (n < 4 || read_full(fd, m, n) != (ssize_t)n) { fprintf(stderr, "lyrion: link lost\n"); break; }
            last_rx = mono_us();
            process(m, n);
        } else if (r < 0 && errno != EINTR) break;
        long long now = mono_us();
        if (now - last_rx > LINK_SILENT_S * 1000000LL) { fprintf(stderr, "lyrion: nothing from the server for %d s, linking again\n", LINK_SILENT_S); break; }
        if (atomic_load(&vol_first) && now - since > 3000000) {         /* no audg came: the Echo's volume all the same */
            atomic_store(&vol_first, 0);
            pthread_mutex_lock(&core_lock); int v = core_volume(); pthread_mutex_unlock(&core_lock);
            lyrion_volume_changed(v);
        }
        vol_settle(now);
        int f = atomic_exchange(&pend, 0);
        if (f & (P_RESP | P_META)) {                    /* the header first: a short stream ends in the same tick */
            char b[sizeof resp]; size_t bl;
            if (f & P_RESP) { pthread_mutex_lock(&lk); bl = resp_len; memcpy(b, resp, bl); pthread_mutex_unlock(&lk); send_pkt("RESP", b, bl, NULL, 0); }
            if (f & P_META) { pthread_mutex_lock(&lk); bl = meta_len; memcpy(b, meta, bl); pthread_mutex_unlock(&lk); send_pkt("META", b, bl, NULL, 0); }
        }
        if (f & P_DSCO) { pthread_mutex_lock(&lk); uint8_t why = (uint8_t)dsco_reason; pthread_mutex_unlock(&lk); send_pkt("DSCO", &why, 1, NULL, 0); }
        if (f & P_STMS) send_stat("STMs", 0);
        if (f & P_STMD) send_stat("STMd", 0);
        if (f & P_STML) send_stat("STMl", 0);
        if (f & P_STMU) send_stat("STMu", 0);
        if (f & P_STMO) send_stat("STMo", 0);
        if (f & P_STMN) send_stat("STMn", 0);
        pthread_mutex_lock(&lk); int running = out.state == O_RUNNING; pthread_mutex_unlock(&lk);
        if (running && now - last_t >= 1000000) { last_t = now; send_stat("STMt", 0); }     /* the play point every second */
    }
    free(m);
}

static void set_lstate(int s) { pthread_mutex_lock(&lk); lstate = s; pthread_mutex_unlock(&lk); }

static void pause_s(int s)                              /* a wait the setting or a new address cuts short */
{
    pthread_mutex_lock(&lk);
    for (int i = 0; i < s * 4 && atomic_load(&enabled) && !atomic_load(&relink); i++) wait_ms_locked(250);
    pthread_mutex_unlock(&lk);
}

static void *control_thread(void *a)
{
    int again = 0, said_none = 0; (void)a;
    for (;;) {
        pthread_mutex_lock(&lk);
        flush_all_locked();
        while (!atomic_load(&enabled)) { lstate = L_OFF; again = 0; pthread_cond_wait(&cv, &lk); }
        char host[sizeof given]; snprintf(host, sizeof host, "%s", given);
        pthread_mutex_unlock(&lk);
        atomic_store(&relink, 0);
        char target[sizeof given] = "", name[64] = "", ver[24] = ""; int port = 9000;
        if (serv_to.s_addr) { inet_ntop(AF_INET, &serv_to, target, sizeof target); serv_to.s_addr = 0; }
        else if (host[0]) {                             /* host, host:port, [IPv6]:port; the port is Lyrion's web port */
            char *b = strchr(host, ']'), *c = strrchr(host, ':');
            if (host[0] == '[' && b) { *b = 0; if (b[1] == ':') port = atoi(b + 2); snprintf(target, sizeof target, "%s", host + 1); }
            else if (c && c == strchr(host, ':')) { *c = 0; port = atoi(c + 1); snprintf(target, sizeof target, "%s", host); }
            else snprintf(target, sizeof target, "%s", host);
            if (port <= 0 || port > 65535) port = 9000;
        } else {
            struct in_addr a2;
            set_lstate(L_SEARCHING);
            if (discover(&a2, &port, name, sizeof name, ver, sizeof ver)) {
                if (!said_none++) fprintf(stderr, "lyrion: no server answers on the network (give its address on the settings page)\n");
                pause_s(5); continue;
            }
            inet_ntop(AF_INET, &a2, target, sizeof target);
            fprintf(stderr, "lyrion: found \"%s\" %s at %s (web port %d)\n", name, ver, target, port);
        }
        said_none = 0;
        set_lstate(L_CONNECTING);
        int fd = net_connect(target, "3483", 5);
        if (fd < 0) { set_lstate(L_UNREACHABLE); pause_s(5); continue; }
        { struct sockaddr_in pa; socklen_t pl = sizeof pa; int on = 1, idle = 10, intvl = 5, cnt = 3;
          struct timeval tv = { 5, 0 };
          setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on); setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
          setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle); setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
          setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
          pthread_mutex_lock(&lk);
          if (!getpeername(fd, (struct sockaddr *)&pa, &pl) && pa.sin_family == AF_INET) srv_ip = pa.sin_addr;
          http_port = port; snprintf(srv_name, sizeof srv_name, "%s", name); snprintf(srv_version, sizeof srv_version, "%s", ver);
          lstate = L_UP; rpc_n = 0;
          pthread_mutex_unlock(&lk); }
        pthread_mutex_lock(&send_lk); ctl_fd = fd; pthread_mutex_unlock(&send_lk);
        atomic_store(&vol_first, 1); atomic_store(&lms_vol, -1); atomic_store(&vol_pending, -1); atomic_store(&fade_q16, 65536); atomic_store(&pend, 0);
        fprintf(stderr, "lyrion: player %s linked to %s%s\n", mac_s, inet_ntoa(srv_ip), again ? " again" : "");
        send_helo(again);
        serve_link(fd);
        pthread_mutex_lock(&send_lk); ctl_fd = -1; pthread_mutex_unlock(&send_lk);
        close(fd);
        set_lstate(L_CONNECTING);
        again = 1;
        if (atomic_load(&enabled) && !atomic_load(&relink)) pause_s(1);
    }
    return NULL;
}

/* ---------------------------------------------------------------- the output thread: the queue into the music stream */

static int ready_locked(void)                           /* enough of the track is here to start it */
{
    if (!qh) return st.done && st.decoded;
    size_t f = 0; unsigned rate = qh->rate;
    for (struct chunk *c = qh; c; c = c->next) f += c->frames - c->off;
    size_t need = (size_t)out.threshold * rate / 10;
    if (need < rate / 5) need = rate / 5;
    return f >= need || st.done || qbytes > Q_MAX / 2;
}

static void track_check_locked(const struct chunk *c)  /* the first frames of a new track go out: STMs */
{
    if (c->gen == out.cur_gen) return;
    out.cur_gen = c->gen; out.cur_rate = c->rate; out.track_frames = 0; out.heard_ms = 0; out.heard_at = 0; out.stmo_sent = 0;
    atomic_fetch_or(&pend, P_STMS);
}

static void *output_thread(void *a)
{
    static const int16_t zero[48000 / 50 * CH];
    int open = 0, music_on = 0; unsigned open_rate = 0, bad_rate = 0; float duck_g = 1, fade = 1;
    int16_t *work = NULL; size_t work_cap = 0;
    (void)a;
    for (;;) {
        struct chunk *c = NULL; size_t sil = 0; unsigned need = 0; int close_it = 0, epoch, playing;
        pthread_mutex_lock(&lk);
        for (;;) {
            int s = out.state;
            if (s == O_STOPPED || s == O_PAUSED) { if (s == O_STOPPED && open) { close_it = 1; break; } wait_ms_locked(200); continue; }
            if (s == O_BUFFER || s == O_WAIT) {
                if (!qh && st.done && st.decoded) { out.state = O_STOPPED; continue; }    /* nothing came (STMn said so) */
                if (ready_locked()) {
                    if (s == O_BUFFER) { out.state = O_RUNNING; continue; }
                    if (!out.stml_sent) { out.stml_sent = 1; atomic_fetch_or(&pend, P_STML); }
                }
                wait_ms_locked(50); continue;
            }
            if (!qh) {
                if (s == O_RUNNING && st.done && st.decoded && !st.running) {
                    if (open && music_queued_us() > 0) { wait_ms_locked(20); continue; }
                    out.state = O_STOPPED; atomic_fetch_or(&pend, P_STMU);  /* played out: Lyrion's next, or the end */
                    continue;
                }
                if (s == O_RUNNING && !out.stmo_sent && out.track_frames) { out.stmo_sent = 1; atomic_fetch_or(&pend, P_STMO); }
                wait_ms_locked(20); continue;
            }
            if (qh->rate == bad_rate) {                 /* the mixer would not take it: that track is dropped */
                struct chunk *d = qh; qh = d->next; if (!qh) qt = NULL; qbytes -= d->frames * FRAME; free(d);
                pthread_cond_broadcast(&cv); continue;
            }
            if (!open || open_rate != qh->rate) { need = qh->rate; break; }
            long long q = music_queued_us();
            if (q > MIXER_TARGET_US) { wait_ms_locked(5); continue; }
            unsigned rate = qh->rate;
            if (s == O_START_AT) {
                long long d = (long long)(int32_t)(out.start_at - jiffies()) * 1000 - q - out_latency_us();
                if (d <= 0 || d > 10000000) { out.state = O_RUNNING; continue; }
                sil = (size_t)(d * rate / 1000000); break;
            }
            if (out.pause_frames > 0) { sil = (size_t)out.pause_frames; break; }
            if (out.skip_frames > 0) {
                track_check_locked(qh);
                size_t take = qh->frames - qh->off;
                if ((long long)take > out.skip_frames) take = (size_t)out.skip_frames;
                qh->off += take; out.skip_frames -= (long long)take; out.track_frames += take;
                if (qh->off == qh->frames) { struct chunk *d = qh; qh = d->next; if (!qh) qt = NULL; qbytes -= d->frames * FRAME; free(d); pthread_cond_broadcast(&cv); }
                continue;
            }
            c = qh; qh = c->next; if (!qh) qt = NULL; qbytes -= c->frames * FRAME;
            pthread_cond_broadcast(&cv);
            track_check_locked(c);
            break;
        }
        if (sil) {
            unsigned r = qh ? qh->rate : 44100;
            if (sil > (size_t)r / 50) sil = r / 50;
            if (sil > sizeof zero / FRAME) sil = sizeof zero / FRAME;
            if (out.state == O_RUNNING && out.pause_frames > 0) out.pause_frames -= (long long)sil;
        }
        epoch = out.epoch;
        playing = out.state == O_RUNNING || out.state == O_START_AT;
        pthread_mutex_unlock(&lk);

        if (playing != music_on) { music_on = playing; core_music(MUSIC_LYRION, playing); }
        if (close_it) { music_close(); open = 0; fprintf(stderr, "lyrion: output closed\n"); continue; }
        if (need) {
            if (open) music_close();
            open = !music_open(need, CH);
            if (open) { open_rate = need; fprintf(stderr, "lyrion: output open, %u Hz\n", need); }
            else { bad_rate = need; fprintf(stderr, "lyrion: the output does not take %u Hz\n", need); }
            continue;
        }
        if (sil) { music_write(zero, sil * FRAME); continue; }
        if (!c) continue;
        size_t n = c->frames - c->off;
        if (n * CH > work_cap) { int16_t *w = realloc(work, n * FRAME); if (!w) { free(c); continue; } work = w; work_cap = n * CH; }
        memcpy(work, c->pcm + c->off * CH, n * FRAME);
        /* the track's replay gain, and Lyrion's fade glided to over the chunk (a step would click) */
        float to = (float)atomic_load(&fade_q16) / 65536, from = fade;
        if (from != 1 || to != 1 || c->gain != 1)
            for (size_t i = 0; i < n; i++) {
                float g = (from + (to - from) * (float)(i + 1) / (float)n) * c->gain;
                for (int k = 0; k < CH; k++) { float v = work[i * CH + k] * g; work[i * CH + k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
            }
        fade = to;
        music_duck(work, n, c->rate, &duck_g, duck_active() ? settings_duck_gain() : 1);     /* under the voice assistant (duck.c) */
        musiclight_feed(work, n, CH, c->rate, music_queued_us());   /* the ring's music effects (ringlive.c), as heard */
        music_write(work, n * FRAME);
        long long q = music_queued_us();
        pthread_mutex_lock(&lk);
        if (epoch == out.epoch && c->gen == out.cur_gen) {
            out.track_frames += n;
            out.heard_ms = (long long)(out.track_frames * 1000 / c->rate) - (q + out_latency_us()) / 1000;
            out.heard_at = mono_us();
        }
        pthread_mutex_unlock(&lk);
        free(c);
    }
    return NULL;
}

void lyrion_start(void)
{
    pthread_t t;
    if (atomic_exchange(&started, 1)) return;
    mac_load();
    trace = getenv("HASSMIC_LYRION_TRACE") != NULL;
    if (getenv("HASSMIC_OUTPUT_LATENCY_MS")) output_latency_us = atoll(getenv("HASSMIC_OUTPUT_LATENCY_MS")) * 1000;
    pthread_mutex_lock(&lk); load_given(); out.cur_gen = 0; pthread_mutex_unlock(&lk);
    if (pthread_create(&t, NULL, control_thread, NULL) == 0) pthread_detach(t);
    if (pthread_create(&t, NULL, output_thread, NULL) == 0) pthread_detach(t);
    if (pthread_create(&t, NULL, rpc_thread, NULL) == 0) pthread_detach(t);
    if (atomic_load(&enabled)) fprintf(stderr, "lyrion: on, player %s\n", mac_s);
}
