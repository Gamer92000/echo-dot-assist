/*
 * Sendspin player (synchronised multiroom audio, Music Assistant), in the dialect aiosendspin 9.1.1 speaks; where that
 * differs from the published spec the library wins, see docs/sendspin-digest.md section 0.
 *
 *   transport   the server finds us by mDNS (_sendspin._tcp) and dials our WebSocket; text frames carry the cleartext init
 *               exchange and the Noise KKpsk2 handshake (we are the responder), binary frames the encrypted session
 *   roles       player@v1: PCM 48 kHz stereo only, so the server resamples and no decoder is needed
 *   trust       unpaired access under the published Sentinel PSK; the operator approves the device in Music Assistant
 *   sync        two-state Kalman filter on client/time exchanges; chunks are written to the mixer so that their first
 *               frame leaves the speaker at the server's timestamp (hard snap at start, then whole-frame drop/duplicate)
 */
#include "sendspin.h"
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "audio.h"
#include "core.h"
#include "hash.h"
#include "netio.h"
#include "noise.h"
#include "ws.h"

#define RATE 48000
#define CHANNELS 2
#define FRAME (2 * CHANNELS)
#define BUFFER_CAPACITY (2u << 20)      /* bytes of PCM the server may have in flight: ~10.9 s */
#define LEAD_MS 300                     /* required_lead_time_ms and min_buffer_ms we report */
#define MIXER_TARGET_US 120000          /* keep about this much queued in the mixer: reacts fast to stop / duck / volume */
#define SNAP_US 20000                   /* beyond this error: one-shot resync instead of frame nudging */

static int port; static const char *state_dir = "/data/local/hassmic/state";
static uint8_t id_priv[32], id_pub[32]; static char client_id[48];
/* Mixer + DAC + amp after our write.  Measured with src/tools/latency.c on the device: 83..88 ms from write to the click
 * arriving in micRaw, which includes the capture path (32 ms blocks, stamped on delivery).  Taking ~15 ms for that leaves
 * ~70 ms; the rest is trimmed by ear with the server's static delay.  Override: HASSMIC_OUTPUT_LATENCY_MS. */
static long long output_latency_us = 70000;

static long long raw_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---------------------------------------------------------------- time filter (aiosendspin client/time_sync.py) */

static pthread_mutex_t tf_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { long long last; int count; double offset, drift, poo, pod, pdd; int use_drift; } tf;

static void tf_reset(void) { pthread_mutex_lock(&tf_lock); memset(&tf, 0, sizeof tf); tf.poo = INFINITY; pthread_mutex_unlock(&tf_lock); }

static void tf_update(double z, double max_error, long long t)
{
    pthread_mutex_lock(&tf_lock);
    if (t > tf.last) {
        double dt = (double)(t - tf.last), r = (max_error * 0.5) * (max_error * 0.5);
        tf.last = t;
        if (tf.count == 0) { tf.count = 1; tf.offset = z; tf.poo = r; tf.drift = 0; }
        else if (tf.count == 1) { tf.count = 2; tf.drift = (z - tf.offset) / dt; tf.offset = z; tf.pdd = (tf.poo + r) / (dt * dt); tf.poo = r; }
        else {
            double pred = tf.offset + tf.drift * dt, npdd = tf.pdd + dt * 1e-22, npod = tf.pod + tf.pdd * dt;
            double npoo = tf.poo + 2 * tf.pod * dt + tf.pdd * dt * dt, resid = z - pred;
            if (tf.count < 100) tf.count++;
            else if (fabs(resid) > 3.0 * max_error) { npdd *= 4; npod *= 4; npoo *= 4; }       /* adaptive forgetting */
            double u = 1 / fmax(npoo + r, 1e-9), ko = npoo * u, kd = npod * u;
            tf.offset = pred + ko * resid; tf.drift += kd * resid;
            tf.pdd = npdd - kd * npod; tf.pod = npod - kd * npoo; tf.poo = npoo - ko * npoo;
            tf.use_drift = tf.drift * tf.drift > 4.0 * tf.pdd;
        }
    }
    pthread_mutex_unlock(&tf_lock);
}

static int tf_synced(double *error_us)
{
    pthread_mutex_lock(&tf_lock);
    int ok = tf.count >= 2 && isfinite(tf.poo);
    if (error_us) *error_us = ok ? sqrt(tf.poo) : 1e9;
    pthread_mutex_unlock(&tf_lock);
    return ok;
}

static long long tf_client_time(long long server_us)
{
    pthread_mutex_lock(&tf_lock);
    double d = tf.use_drift ? tf.drift : 0;
    long long t = llround(((double)server_us - tf.offset + d * (double)tf.last) / (1 + d));
    pthread_mutex_unlock(&tf_lock);
    return t;
}

/* ---------------------------------------------------------------- chunk queue + scheduler */

struct chunk { struct chunk *next; long long ts; size_t len; uint8_t pcm[]; };
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static struct chunk *q_head, *q_tail; static size_t q_bytes;
static atomic_int stream_on, player_muted, snap_next, session_gen;
static atomic_int static_delay_ms;

static void q_flush(void)
{
    pthread_mutex_lock(&q_lock);
    while (q_head) { struct chunk *c = q_head; q_head = c->next; free(c); }
    q_tail = NULL; q_bytes = 0;
    pthread_mutex_unlock(&q_lock);
    atomic_store(&snap_next, 1);
}

static void q_push(long long ts, const uint8_t *pcm, size_t len)
{
    struct chunk *c;
    len -= len % FRAME;
    if (!len || !atomic_load(&stream_on) || !(c = malloc(sizeof *c + len))) return;
    c->next = NULL; c->ts = ts; c->len = len; memcpy(c->pcm, pcm, len);
    pthread_mutex_lock(&q_lock);
    if (q_bytes + len > BUFFER_CAPACITY + (1u << 20)) { pthread_mutex_unlock(&q_lock); free(c); return; }   /* server over-ran its budget */
    if (q_tail) q_tail->next = c; else q_head = c;
    q_tail = c; q_bytes += len;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
}

static void *player_thread(void *arg)
{
    static int16_t silence[RATE / 100 * CHANNELS]; int open = 0; double smooth = 0; long long stat_t = 0; long adj_total = 0;
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (!q_head && atomic_load(&stream_on)) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 50000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            if (pthread_cond_timedwait(&q_cond, &q_lock, &ts) && open && music_queued_us() == 0) atomic_store(&snap_next, 1);   /* underrun */
        }
        struct chunk *c = q_head;
        if (c) { q_head = c->next; if (!q_head) q_tail = NULL; q_bytes -= c->len; }
        pthread_mutex_unlock(&q_lock);

        if (!atomic_load(&stream_on)) {
            if (c) free(c);
            if (open) { music_close(); open = 0; fprintf(stderr, "sendspin: output closed\n"); }
            usleep(20000);
            continue;
        }
        if (!c) continue;
        if (!tf_synced(NULL)) { free(c); continue; }
        if (!open) { if (music_open(RATE, CHANNELS)) { free(c); usleep(200000); continue; } open = 1; atomic_store(&snap_next, 1); smooth = 0; }

        while (music_queued_us() > MIXER_TARGET_US && atomic_load(&stream_on)) usleep(5000);

        uint8_t *pcm = c->pcm; size_t len = c->len;
        long long due = tf_client_time(c->ts) - atomic_load(&static_delay_ms) * 1000LL;
        long long err = due - (raw_us() + music_queued_us() + output_latency_us);      /* > 0: we are early */
        if (atomic_exchange(&snap_next, 0) || llabs(err) > SNAP_US) {
            if (err > 0) {
                while (err > 200000 && atomic_load(&stream_on)) { usleep(20000); err = due - (raw_us() + music_queued_us() + output_latency_us); }
                for (long long frames = err * RATE / 1000000; frames > 0; ) {
                    size_t n = frames > RATE / 100 ? RATE / 100 : (size_t)frames;
                    music_write(silence, n * FRAME); frames -= n;
                }
            } else {
                size_t drop = (size_t)(-err * RATE / 1000000) * FRAME;
                if (drop >= len) { free(c); atomic_store(&snap_next, 1); continue; }             /* whole chunk is late */
                pcm += drop; len -= drop;
            }
            smooth = 0;
        } else {
            smooth = 0.9 * smooth + 0.1 * (double)err;
            if (fabs(smooth) > 400) {                                                           /* nudge: at most 6 frames per 25 ms chunk */
                int n = (int)fmin(6, fmax(1, fabs(smooth) * RATE / 1e6));
                if (smooth > 0) { for (int i = 0; i < n; i++) music_write(pcm, FRAME); adj_total += n; }       /* early: repeat a frame */
                else if (len > (size_t)n * FRAME) { pcm += n * FRAME; len -= n * FRAME; adj_total -= n; }     /* late: skip frames */
                smooth -= (smooth > 0 ? 1 : -1) * n * 1e6 / RATE;
            }
        }
        if (atomic_load(&player_muted)) memset(pcm, 0, len);
        else if (core_state() != IDLE) { int16_t *s = (int16_t *)pcm; for (size_t i = 0; i < len / 2; i++) s[i] /= 6; }    /* duck under the voice assistant */
        music_write(pcm, len);
        if (raw_us() - stat_t > 10000000) {
            double e; tf_synced(&e); stat_t = raw_us();
            fprintf(stderr, "sendspin: sync err %+.2f ms, clock ±%.2f ms, queued %zu KB, nudged %+ld frames\n", smooth / 1000, e / 1000, q_bytes / 1024, adj_total);
        }
        free(c);
    }
    return NULL;
}

/* ---------------------------------------------------------------- session */

struct session { struct ws ws; struct noise_cs tx, rx; pthread_mutex_t send_lock; int gen, activated, player_active, state_sent; long long activated_at; };

static int js_str(const char *j, const char *key, char *out, size_t outsz)
{
    char pat[64]; const char *p = j; size_t n = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    while ((p = strstr(p, pat))) {
        p += strlen(pat); while (*p == ' ') p++;
        if (*p != ':') continue;
        p++; while (*p == ' ') p++;
        if (*p != '"') return 0;
        for (p++; *p && *p != '"' && n + 1 < outsz; p++) { if (*p == '\\' && p[1]) p++; out[n++] = *p; }
        out[n] = 0; return 1;
    }
    return 0;
}

static int js_i64(const char *j, const char *key, long long *out)
{
    char pat[64]; const char *p = j; char *end;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    while ((p = strstr(p, pat))) {
        p += strlen(pat); while (*p == ' ') p++;
        if (*p != ':') continue;
        p++; while (*p == ' ') p++;
        if (!strncmp(p, "true", 4)) { *out = 1; return 1; }
        if (!strncmp(p, "false", 5)) { *out = 0; return 1; }
        long long v = strtoll(p, &end, 10);
        if (end == p) return 0;
        *out = v; return 1;
    }
    return 0;
}

/* the text of "key": [ ... ] or { ... }, for a substring test */
static int js_section(const char *j, const char *key, char *out, size_t outsz)
{
    char pat[64]; const char *p; size_t n = 0; int depth = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    if (!(p = strstr(j, pat))) return 0;
    p += strlen(pat); while (*p == ' ' || *p == ':') p++;
    if (*p != '[' && *p != '{') return 0;
    do { if (*p == '[' || *p == '{') depth++; else if (*p == ']' || *p == '}') depth--; if (n + 1 < outsz) out[n++] = *p; p++; } while (*p && depth);
    out[n] = 0; return 1;
}

static int send_json(struct session *s, const char *fmt, ...)
{
    static __thread char msg[2048]; uint8_t out[2048 + NOISE_TAG]; va_list ap; int rc;
    msg[0] = 0;                                         /* type byte 0 = JSON */
    va_start(ap, fmt); int n = vsnprintf(msg + 1, sizeof msg - 1, fmt, ap); va_end(ap);
    pthread_mutex_lock(&s->send_lock);                  /* nonce order must equal wire order */
    size_t cl = noise_encrypt(&s->tx, msg, n + 1, out);
    rc = ws_send(&s->ws, WS_BINARY, out, cl);
    pthread_mutex_unlock(&s->send_lock);
    return rc;
}

static void send_state(struct session *s)
{
    pthread_mutex_lock(&core_lock); int vol = core_volume(); pthread_mutex_unlock(&core_lock);
    send_json(s, "{\"type\":\"client/state\",\"payload\":{\"available\":true,\"player\":{\"volume\":%d,\"muted\":%s,\"static_delay_ms\":%d,"
                 "\"required_lead_time_ms\":%d,\"min_buffer_ms\":%d,\"supported_commands\":[\"set_static_delay\"]}}}",
              vol, atomic_load(&player_muted) ? "true" : "false", atomic_load(&static_delay_ms), LEAD_MS, LEAD_MS);
    s->state_sent = 1;
}

static void save_delay(void)
{
    char path[256]; snprintf(path, sizeof path, "%s/sendspin.delay", state_dir);
    FILE *f = fopen(path, "w"); if (f) { fprintf(f, "%d\n", atomic_load(&static_delay_ms)); fclose(f); }
}

static void *time_thread(void *arg)
{
    struct session *s = arg; int gen = s->gen;
    while (atomic_load(&session_gen) == gen) {
        double e; int ok = tf_synced(&e);
        if (!s->activated) { usleep(50000); continue; }     /* nothing but client/hello may precede the first server/activate */
        send_json(s, "{\"type\":\"client/time\",\"payload\":{\"client_transmitted\":%lld}}", raw_us());
        /* first client/state: once the clock is usable, but well inside the server's 5 s initial-state timer */
        if (s->player_active && !s->state_sent && ((ok && e < 5000) || raw_us() - s->activated_at > 3000000)) send_state(s);
        usleep(!ok ? 200000 : e < 1000 ? 3000000 : e < 2000 ? 1000000 : e < 5000 ? 500000 : 200000);
    }
    return NULL;
}

static void on_json(struct session *s, char *j, long long t_recv)
{
    char type[48] = "", sec[512], str[64]; long long a, b, c;
    js_str(j, "type", type, sizeof type);
    if (!strcmp(type, "server/time")) {
        if (js_i64(j, "client_transmitted", &a) && js_i64(j, "server_received", &b) && js_i64(j, "server_transmitted", &c))
            tf_update(((double)(b - a) + (double)(c - t_recv)) / 2, ((double)(t_recv - a) - (double)(c - b)) / 2, t_recv);
    } else if (!strcmp(type, "server/hello")) {
        js_str(j, "name", str, sizeof str); fprintf(stderr, "sendspin: server \"%s\"\n", str);
        send_json(s, "{\"type\":\"client/hello\",\"payload\":{\"name\":\"%s\",\"device_info\":{\"product_name\":\"Echo Dot 3 (hassmic)\","
                     "\"manufacturer\":\"Amazon\",\"software_version\":\"" VERSION "\"},\"supported_roles\":[\"player@v1\"],"
                     "\"player@v1_support\":{\"supported_formats\":[{\"codec\":\"pcm\",\"channels\":%d,\"sample_rate\":%d,\"bit_depth\":16}],"
                     "\"buffer_capacity\":%u,\"supported_commands\":[\"volume\",\"mute\"]},\"unpaired_access\":{\"enabled\":true}}}",
                  core_name, CHANNELS, RATE, BUFFER_CAPACITY);
    } else if (!strcmp(type, "server/activate")) {
        s->activated = 1;
        if (js_section(j, "activities", sec, sizeof sec) && strstr(sec, "pairing"))
            send_json(s, "{\"type\":\"pair/abort\",\"payload\":{\"reason\":\"method_not_supported\"}}");
        if (js_section(j, "active_roles", sec, sizeof sec)) {                                   /* sticky when omitted */
            int on = strstr(sec, "player@v1") != NULL;
            if (on && !s->player_active) { s->activated_at = raw_us(); s->state_sent = 0; fprintf(stderr, "sendspin: player role active\n"); }
            if (!on && s->player_active) { atomic_store(&stream_on, 0); q_flush(); fprintf(stderr, "sendspin: waiting for approval in Music Assistant (unpaired device)\n"); }
            if (!on && !s->player_active) fprintf(stderr, "sendspin: connected, no role yet: approve the device in Music Assistant\n");
            s->player_active = on;
        }
    } else if (!strcmp(type, "server/command")) {
        if (!js_section(j, "player", sec, sizeof sec) || !js_str(sec, "command", str, sizeof str)) return;
        if (!strcmp(str, "volume") && js_i64(sec, "volume", &a)) { pthread_mutex_lock(&core_lock); core_set_volume((int)a); pthread_mutex_unlock(&core_lock); }
        else if (!strcmp(str, "mute") && js_i64(sec, "mute", &a)) atomic_store(&player_muted, a != 0);
        else if (!strcmp(str, "set_static_delay") && js_i64(sec, "static_delay_ms", &a) && a >= 0 && a <= 5000) { atomic_store(&static_delay_ms, (int)a); save_delay(); atomic_store(&snap_next, 1); }
        send_state(s);
    } else if (!strcmp(type, "stream/start")) {
        if (!js_section(j, "player", sec, sizeof sec)) return;
        js_str(sec, "codec", str, sizeof str); js_i64(sec, "sample_rate", &a); js_i64(sec, "channels", &b); js_i64(sec, "bit_depth", &c);
        if (strcmp(str, "pcm") || a != RATE || b != CHANNELS || c != 16) { fprintf(stderr, "sendspin: unexpected format %s %lld/%lld/%lld, ignoring stream\n", str, a, b, c); return; }
        fprintf(stderr, "sendspin: stream start\n");
        if (!atomic_load(&stream_on)) q_flush();
        atomic_store(&stream_on, 1);
        send_json(s, "{\"type\":\"client/time\",\"payload\":{\"client_transmitted\":%lld}}", raw_us());
    } else if (!strcmp(type, "stream/clear")) {
        if (!js_section(j, "roles", sec, sizeof sec) || strstr(sec, "player")) q_flush();
    } else if (!strcmp(type, "stream/end")) {
        if (!js_section(j, "roles", sec, sizeof sec) || strstr(sec, "player")) { atomic_store(&stream_on, 0); q_flush(); fprintf(stderr, "sendspin: stream end\n"); }
    } else if (!strcmp(type, "group/update")) {
        if (js_str(j, "playback_state", str, sizeof str)) fprintf(stderr, "sendspin: group %s\n", str);
    }
}

static int handshake(struct session *s)
{
    static const uint8_t SENTINEL_LABEL[] = "sendspin-sentinel-psk-v1";
    char init[160], data[512], want[48]; uint8_t prologue[1024], msg[512], payload[512], server_pub[32], sentinel[32], idin[18 + 32], idh[32];
    int op; uint8_t *d; size_t n, pl; struct noise_hs hs;

    int il = snprintf(init, sizeof init, "{\"type\":\"client/init\",\"payload\":{\"client_id\":\"%s\",\"version\":1,\"suite\":\"25519_ChaChaPoly_SHA256\"}}", client_id);
    if (ws_send(&s->ws, WS_TEXT, init, il)) return -1;
    if (ws_recv(&s->ws, &op, &d, &n) != 1 || op != WS_TEXT || il + n > sizeof prologue) return -1;          /* server/init */
    if (!js_str((char *)d, "server_id", data, sizeof data) || b64_decode(data, strlen(data), server_pub, 32) != 32) return -1;
    memcpy(prologue, init, il); memcpy(prologue + il, d, n); pl = il + n;                                      /* raw bytes as sent / received */

    if (ws_recv(&s->ws, &op, &d, &n) != 1 || op != WS_TEXT || !js_str((char *)d, "data", data, sizeof data)) return -1;
    long ml = b64_decode(data, strlen(data), msg, sizeof msg);
    noise_kk_responder_init(&hs, prologue, pl, id_priv, id_pub, server_pub);
    long pll = ml < 0 ? -1 : noise_kk_read_msg1(&hs, msg, ml, payload);
    if (pll < 0) { fprintf(stderr, "sendspin: handshake message 1 does not authenticate\n"); return -1; }
    payload[pll] = 0;

    sha256(SENTINEL_LABEL, sizeof SENTINEL_LABEL - 1, sentinel);
    memcpy(idin, "sendspin-psk-id-v1", 18); memcpy(idin + 18, sentinel, 32); sha256(idin, sizeof idin, idh); b64_encode(idh, 32, want, 1, 0);
    if (!js_str((char *)payload, "psk_id", data, sizeof data) || strcmp(data, want))
        fprintf(stderr, "sendspin: server refers to a pairing key we do not have (remove this device's pairing in Music Assistant); trying the Sentinel key\n");

    n = noise_kk_write_msg2(&hs, sentinel, "{}", 2, msg, &s->tx, &s->rx);
    b64_encode(msg, n, data, 1, 0);
    char out[768]; int ol = snprintf(out, sizeof out, "{\"type\":\"noise/handshake\",\"payload\":{\"data\":\"%s\"}}", data);
    return ws_send(&s->ws, WS_TEXT, out, ol);
}

static void serve(int fd)
{
    static struct session s; static uint8_t plain[70000]; char path[128]; int op; uint8_t *d; size_t n; pthread_t tt;
    memset(&s, 0, sizeof s); pthread_mutex_init(&s.send_lock, NULL);
    if (ws_accept(&s.ws, fd, path, sizeof path)) { close(fd); return; }
    if (handshake(&s)) { fprintf(stderr, "sendspin: handshake failed\n"); ws_close(&s.ws); return; }
    fprintf(stderr, "sendspin: encrypted session up\n");
    s.gen = atomic_fetch_add(&session_gen, 1) + 1;
    tf_reset();
    pthread_create(&tt, NULL, time_thread, &s);

    while (ws_recv(&s.ws, &op, &d, &n) == 1) {
        if (op != WS_BINARY) break;                                     /* text after the handshake is a protocol error */
        long long t_recv = raw_us();
        long pl = n <= sizeof plain ? noise_decrypt(&s.rx, d, n, plain) : -1;
        if (pl < 1) { fprintf(stderr, "sendspin: message does not authenticate, closing\n"); break; }
        if (plain[0] == 0) { plain[pl] = 0; on_json(&s, (char *)plain + 1, t_recv); }
        else if (plain[0] == 4 && pl > 9) {                             /* audio: int64 big-endian server time, then PCM */
            long long ts = 0; for (int i = 0; i < 8; i++) ts = ts << 8 | plain[1 + i];
            q_push(ts, plain + 9, pl - 9);
        }
    }
    atomic_fetch_add(&session_gen, 1);                                  /* stops the time thread */
    atomic_store(&stream_on, 0); q_flush();
    pthread_join(tt, NULL);
    ws_close(&s.ws); pthread_mutex_destroy(&s.send_lock);
    fprintf(stderr, "sendspin: session closed\n");
}

static void *listen_thread(void *arg)
{
    int ls = net_listen(port);
    (void)arg;
    if (ls < 0) { perror("sendspin: listen"); return NULL; }
    fprintf(stderr, "sendspin: player %s on %d\n", client_id, port);
    for (;;) { int c = net_accept(ls); if (c < 0) break; serve(c); }    /* one server at a time */
    return NULL;
}

static void load_identity(void)
{
    char path[256]; FILE *f; const char *e;
    if ((e = getenv("HASSMIC_STATE"))) state_dir = e;
    if ((e = getenv("HASSMIC_OUTPUT_LATENCY_MS"))) output_latency_us = atoll(e) * 1000;
    snprintf(path, sizeof path, "%s/sendspin.key", state_dir);
    if (!(f = fopen(path, "rb")) || fread(id_priv, 1, 32, f) != 32) {   /* the server pins client_id per address: keep it for ever */
        if (f) fclose(f);
        noise_keypair(id_priv, id_pub);
        if ((f = fopen(path, "wb"))) { fwrite(id_priv, 1, 32, f); fclose(f); chmod(path, 0600); }
        else fprintf(stderr, "sendspin: cannot store identity in %s, a new one is made at every start\n", path);
        f = NULL;
    }
    if (f) fclose(f);
    { extern void crypto_x25519_public_key(uint8_t *, const uint8_t *); crypto_x25519_public_key(id_pub, id_priv); }
    b64_encode(id_pub, 32, client_id, 1, 0);
    snprintf(path, sizeof path, "%s/sendspin.delay", state_dir);
    if ((f = fopen(path, "r"))) { int v; if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 5000) atomic_store(&static_delay_ms, v); fclose(f); }
}

void sendspin_mdns(int p, char *out, size_t outsz)
{
    snprintf(out, outsz, "  <service><type>_sendspin._tcp</type><port>%d</port><txt-record>path=/sendspin</txt-record>"
                         "<txt-record>name=%s</txt-record></service>\n", p, core_name);
}

int sendspin_start(int p)
{
    pthread_t t;
    port = p;
    load_identity();
    pthread_create(&t, NULL, player_thread, NULL); pthread_detach(t);
    return pthread_create(&t, NULL, listen_thread, NULL) ? -1 : (pthread_detach(t), 0);
}
