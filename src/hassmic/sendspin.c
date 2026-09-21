/*
 * Sendspin player (synchronised multiroom audio, Music Assistant), in the dialect aiosendspin 9.1.1 speaks; where that
 * differs from the published spec the library wins, see docs/sendspin-digest.md section 0.
 *
 *   transport   the server finds us by mDNS (_sendspin._tcp) and dials our WebSocket; text frames carry the cleartext init
 *               exchange and the Noise KKpsk2 handshake (we are the responder), binary frames the encrypted session
 *   roles       player@v1: 48 kHz stereo as FLAC (dr_flac), Opus (the firmware's own libopus) or PCM, in that order of
 *               preference (HASSMIC_SENDSPIN_CODECS changes it); the server resamples to it
 *   roles       controller@v1: the action button pauses / resumes the group
 *   trust       unpaired access under the published Sentinel PSK (the operator approves the device in Music Assistant),
 *               or paired: the operator pastes our pairing token, we hand over a fresh long-term PSK ("pairing_psk" flow),
 *               the server re-handshakes in band to it.  PIN pairing (CPace) is not implemented and is refused.
 *   servers     every Sendspin server on the LAN dials us; one connection is admitted, ranked by declared activity
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "audio.h"
#include "core.h"
#include "hash.h"
#include "netio.h"
#include "noise.h"
#include "ws.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "../third_party/dr_flac.h"

/* libopus: linked from the stock firmware on the device, from the system on the PC.  Only these three calls are used. */
typedef struct OpusDecoder OpusDecoder;
OpusDecoder *opus_decoder_create(int32_t Fs, int channels, int *error);
int opus_decode(OpusDecoder *st, const unsigned char *data, int32_t len, int16_t *pcm, int frame_size, int decode_fec);
void opus_decoder_destroy(OpusDecoder *st);

#define RATE 48000
#define CHANNELS 2
#define FRAME (2 * CHANNELS)
#define BUFFER_CAPACITY (2u << 20)      /* bytes of PCM the server may have in flight: ~10.9 s */
#define QUEUE_MAX (8u << 20)            /* decoded PCM we hold: the server's 30 s horizon plus margin */
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
static atomic_int stream_on, player_muted, snap_next;
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
    /* The server budgets BUFFER_CAPACITY in *compressed* bytes and at most 30 s ahead; this queue holds decoded PCM, so
     * it is sized for the horizon (30 s = 5.5 MB), not for the byte budget.  Dropping here means a gap: say so. */
    if (q_bytes + len > QUEUE_MAX) {
        static long long last; pthread_mutex_unlock(&q_lock); free(c);
        if (raw_us() - last > 5000000) { last = raw_us(); fprintf(stderr, "sendspin: queue full (%u MB), dropping audio\n", QUEUE_MAX >> 20); }
        return;
    }
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

static void save_delay(void)
{
    char path[256]; snprintf(path, sizeof path, "%s/sendspin.delay", state_dir);
    FILE *f = fopen(path, "w"); if (f) { fprintf(f, "%d\n", atomic_load(&static_delay_ms)); fclose(f); }
}

/* ---------------------------------------------------------------- decoders (receive thread of the admitted connection) */

enum { CODEC_PCM, CODEC_FLAC, CODEC_OPUS };
static int codec; static OpusDecoder *opus; static drflac *flac;
static struct { const uint8_t *p; size_t len, pos; } flac_in;       /* what dr_flac may read right now: the header, then one chunk */

static size_t flac_read(void *u, void *out, size_t n)
{
    (void)u;
    if (n > flac_in.len - flac_in.pos) n = flac_in.len - flac_in.pos;
    memcpy(out, flac_in.p + flac_in.pos, n); flac_in.pos += n;
    return n;
}
static drflac_bool32 flac_seek(void *u, int off, drflac_seek_origin o)
{
    (void)u;
    if (o != DRFLAC_SEEK_CUR || off < 0 || (size_t)off > flac_in.len - flac_in.pos) return DRFLAC_FALSE;
    flac_in.pos += off; return DRFLAC_TRUE;
}
static drflac_bool32 flac_tell(void *u, drflac_int64 *c) { (void)u; *c = (drflac_int64)flac_in.pos; return DRFLAC_TRUE; }

static void decoders_close(void)
{
    if (opus) { opus_decoder_destroy(opus); opus = NULL; }
    if (flac) { drflac_close(flac); flac = NULL; }
}

static int decoder_open(const char *name, const uint8_t *header, size_t hlen)
{
    int err = 0;
    decoders_close();
    if (!strcmp(name, "pcm")) { codec = CODEC_PCM; return 0; }
    if (!strcmp(name, "opus")) { codec = CODEC_OPUS; opus = opus_decoder_create(RATE, CHANNELS, &err); return opus ? 0 : -1; }
    if (!strcmp(name, "flac")) {                        /* header = "fLaC" + STREAMINFO block */
        codec = CODEC_FLAC; flac_in.p = header; flac_in.len = hlen; flac_in.pos = 0;
        flac = drflac_open(flac_read, flac_seek, flac_tell, NULL, NULL);
        return flac && flac->channels == CHANNELS && flac->sampleRate == RATE ? 0 : -1;
    }
    return -1;
}

/* One chunk in, PCM out (whole FLAC frames / one Opus packet per chunk).  Returns bytes, 0 if nothing came out. */
static size_t decode_chunk(const uint8_t *in, size_t len, const uint8_t **out)
{
    static int16_t pcm[RATE / 5 * CHANNELS];            /* up to 200 ms */
    if (codec == CODEC_PCM) { *out = in; return len; }
    *out = (const uint8_t *)pcm;
    if (codec == CODEC_OPUS) {
        int n = opus ? opus_decode(opus, in, (int32_t)len, pcm, 5760, 0) : -1;
        return n > 0 ? (size_t)n * FRAME : 0;
    }
    if (!flac) return 0;
    size_t frames = 0, cap = sizeof pcm / sizeof pcm[0] / CHANNELS;
    flac_in.p = in; flac_in.len = len; flac_in.pos = 0;
    for (drflac_uint64 n; frames < cap && (n = drflac_read_pcm_frames_s16(flac, cap - frames, pcm + frames * CHANNELS)) > 0; ) frames += n;
    return frames * FRAME;
}

/* ---------------------------------------------------------------- keys, pairing records, token */

enum { PSK_SENTINEL, PSK_PAIRING, PSK_LONGTERM };
#define MAX_RECORDS 8
struct record { uint8_t psk[32]; char server_id[48]; };
static pthread_mutex_t rec_lock = PTHREAD_MUTEX_INITIALIZER;
static struct record records[MAX_RECORDS]; static int n_records;
static uint8_t sentinel_psk[32], pairing_psk[32];
static char last_playback_server[48];

static void state_path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", state_dir, name); }

static int load_file(const char *name, void *buf, size_t len)
{
    char path[256]; state_path(path, sizeof path, name);
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    size_t r = fread(buf, 1, len, f); fclose(f);
    return (int)r;
}

static void save_file(const char *name, const void *buf, size_t len)
{
    char path[256], tmp[280]; state_path(path, sizeof path, name); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "sendspin: cannot write %s\n", path); return; }
    fwrite(buf, 1, len, f); fclose(f); chmod(tmp, 0600); rename(tmp, path);
}

static void psk_id(const uint8_t psk[32], char out[48])
{
    uint8_t in[18 + 32], h[32];
    memcpy(in, "sendspin-psk-id-v1", 18); memcpy(in + 18, psk, 32); sha256(in, sizeof in, h); b64_encode(h, 32, out, 1, 0);
}

static void records_save(void) { save_file("sendspin.records", records, sizeof records[0] * n_records); }

static void record_put(const uint8_t psk[32], const char *server_id)     /* one record per server, newest first */
{
    pthread_mutex_lock(&rec_lock);
    int k = 0;
    for (int i = 0; i < n_records; i++) if (strcmp(records[i].server_id, server_id)) records[k++] = records[i];
    if (k >= MAX_RECORDS) k = MAX_RECORDS - 1;                          /* evict the oldest */
    memmove(records + 1, records, sizeof records[0] * k);
    memcpy(records[0].psk, psk, 32); snprintf(records[0].server_id, sizeof records[0].server_id, "%s", server_id);
    n_records = k + 1; records_save();
    pthread_mutex_unlock(&rec_lock);
}

static void record_drop(const char *server_id)
{
    pthread_mutex_lock(&rec_lock);
    int k = 0;
    for (int i = 0; i < n_records; i++) if (strcmp(records[i].server_id, server_id)) records[k++] = records[i];
    n_records = k; records_save();
    pthread_mutex_unlock(&rec_lock);
}

/* Which of our PSKs does the server mean?  -1: none (spec: fall back to the Sentinel on an initial handshake). */
static int psk_select(const char *id, const char *server_id, uint8_t out[32])
{
    char cand[48]; int cat = -1;
    psk_id(sentinel_psk, cand); if (!strcmp(cand, id)) { memcpy(out, sentinel_psk, 32); return PSK_SENTINEL; }
    psk_id(pairing_psk, cand);  if (!strcmp(cand, id)) { memcpy(out, pairing_psk, 32); return PSK_PAIRING; }
    pthread_mutex_lock(&rec_lock);
    for (int i = 0; i < n_records && cat < 0; i++) {
        psk_id(records[i].psk, cand);
        if (!strcmp(cand, id) && !strcmp(records[i].server_id, server_id)) { memcpy(out, records[i].psk, 32); cat = PSK_LONGTERM; }
    }
    pthread_mutex_unlock(&rec_lock);
    return cat;
}

/* "SP:0" + base32(client_pub || pairing_psk), '=' stripped, every '2' written as '9' */
void sendspin_pairing_token(char *out, size_t outsz)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"; uint8_t in[64]; size_t o = 0; uint32_t acc = 0; int bits = 0;
    memcpy(in, id_pub, 32); memcpy(in + 32, pairing_psk, 32);
    o = snprintf(out, outsz, "SP:0");
    for (size_t i = 0; i < sizeof in && o + 2 < outsz; i++) {
        acc = acc << 8 | in[i]; bits += 8;
        while (bits >= 5 && o + 1 < outsz) { bits -= 5; char c = A[acc >> bits & 31]; out[o++] = c == '2' ? '9' : c; }
    }
    if (bits && o + 1 < outsz) { char c = A[acc << (5 - bits) & 31]; out[o++] = c == '2' ? '9' : c; }
    out[o] = 0;
}

/* ---------------------------------------------------------------- session */

struct session {
    struct ws ws; struct noise_cs tx, rx; pthread_mutex_t send_lock;
    uint8_t server_pub[32], hs_hash[32], new_psk[32]; char server_id[48]; int psk_cat;
    int activated, admitted, rank, player_active, controller_active, state_sent, pairing, closing; long long activated_at;
    char ctl_commands[256]; int group_playing;
};

/* At most one admitted connection; the others are provisional until their first server/activate decides. */
static pthread_mutex_t adm_lock = PTHREAD_MUTEX_INITIALIZER;
static struct session *admitted;
static long long paused_by_button_at;

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

/* the text of "key": [ ... ] or { ... }, for a substring test or a nested lookup */
static int js_section(const char *j, const char *key, char *out, size_t outsz)
{
    char pat[64]; const char *p = j; size_t n = 0; int depth = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    while ((p = strstr(p, pat))) {
        p += strlen(pat); while (*p == ' ') p++;
        if (*p != ':') continue;
        p++; while (*p == ' ') p++;
        if (*p != '[' && *p != '{') return 0;
        do { if (*p == '[' || *p == '{') depth++; else if (*p == ']' || *p == '}') depth--; if (n + 1 < outsz) out[n++] = *p; p++; } while (*p && depth);
        out[n] = 0; return 1;
    }
    return 0;
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

static void send_state_vol(struct session *s, int vol)
{
    send_json(s, "{\"type\":\"client/state\",\"payload\":{\"available\":true,\"player\":{\"volume\":%d,\"muted\":%s,\"static_delay_ms\":%d,"
                 "\"required_lead_time_ms\":%d,\"min_buffer_ms\":%d,\"supported_commands\":[\"set_static_delay\"]}}}",
              vol, atomic_load(&player_muted) ? "true" : "false", atomic_load(&static_delay_ms), LEAD_MS, LEAD_MS);
    s->state_sent = 1;
}

static void send_state(struct session *s)
{
    pthread_mutex_lock(&core_lock); int vol = core_volume(); pthread_mutex_unlock(&core_lock);
    send_state_vol(s, vol);
}

static void goodbye(struct session *s, const char *reason)
{
    send_json(s, "{\"type\":\"client/goodbye\",\"payload\":{\"reason\":\"%s\"}}", reason);
    s->closing = 1; shutdown(s->ws.fd, SHUT_RDWR);
}

static void *time_thread(void *arg)
{
    struct session *s = arg;
    while (!s->closing) {
        double e; int ok = tf_synced(&e);
        /* nothing but client/hello may precede the first server/activate; silent during pairing and re-handshake; only
         * the admitted connection drives the clock */
        if (!s->activated || !s->admitted || s->pairing) { usleep(50000); continue; }
        send_json(s, "{\"type\":\"client/time\",\"payload\":{\"client_transmitted\":%lld}}", raw_us());
        /* first client/state: once the clock is usable, but well inside the server's 5 s initial-state timer */
        if (s->player_active && !s->state_sent && ((ok && e < 5000) || raw_us() - s->activated_at > 3000000)) send_state(s);
        long long nap = !ok ? 200000 : e < 1000 ? 3000000 : e < 2000 ? 1000000 : e < 5000 ? 500000 : 200000;
        for (long long t = 0; t < nap && !s->closing; t += 50000) usleep(50000);
    }
    return NULL;
}

/* First server/activate of a connection: may it replace the admitted one?  Rank: playback 2 > pairing 1 > nothing 0;
 * incoming >= current wins; two idle ones: only the server that last played here may take over. */
static int arbitrate(struct session *s)
{
    int win;
    pthread_mutex_lock(&adm_lock);
    struct session *cur = admitted;
    if (!cur || cur == s) win = 1;
    else if (s->rank == 0 && cur->rank == 0) win = !strcmp(s->server_id, last_playback_server) && strcmp(cur->server_id, last_playback_server);
    else win = s->rank >= cur->rank;
    if (win) {
        if (cur && cur != s) { fprintf(stderr, "sendspin: another server takes over\n"); cur->admitted = 0; goodbye(cur, "another_server"); }
        if (cur != s) { atomic_store(&stream_on, 0); q_flush(); tf_reset(); }
        admitted = s; s->admitted = 1;
    }
    pthread_mutex_unlock(&adm_lock);
    if (!win) { fprintf(stderr, "sendspin: second server turned away, the current one is busier\n"); goodbye(s, "concurrent_attempt"); }
    return win;
}

static int rehandshake(struct session *s, const char *j);

/* supported_formats in order of preference: the server takes the first one it can encode */
static const char *formats_json(void)
{
    static char out[512]; const char *pref = getenv("HASSMIC_SENDSPIN_CODECS"); size_t o = 0;
    if (!pref || !*pref) pref = "flac,opus,pcm";
    out[0] = 0;
    for (const char *p = pref; *p; ) {
        size_t n = strcspn(p, ",");
        if ((n == 4 && !strncmp(p, "flac", 4)) || (n == 4 && !strncmp(p, "opus", 4)) || (n == 3 && !strncmp(p, "pcm", 3)))
            o += snprintf(out + o, sizeof out - o, "%s{\"codec\":\"%.*s\",\"channels\":%d,\"sample_rate\":%d,\"bit_depth\":16}", o ? "," : "", (int)n, p, CHANNELS, RATE);
        p += n; if (*p == ',') p++;
    }
    return out;
}

static void on_json(struct session *s, char *j, long long t_recv)
{
    char type[48] = "", sec[512], str[64]; long long a, b, c;
    js_str(j, "type", type, sizeof type);
    if (!strcmp(type, "server/time")) {
        if (s->admitted && js_i64(j, "client_transmitted", &a) && js_i64(j, "server_received", &b) && js_i64(j, "server_transmitted", &c))
            tf_update(((double)(b - a) + (double)(c - t_recv)) / 2, ((double)(t_recv - a) - (double)(c - b)) / 2, t_recv);
    } else if (!strcmp(type, "noise/handshake")) {
        if (rehandshake(s, j)) { fprintf(stderr, "sendspin: re-handshake failed\n"); s->closing = 1; shutdown(s->ws.fd, SHUT_RDWR); }
    } else if (!strcmp(type, "server/hello")) {
        js_str(j, "name", str, sizeof str); fprintf(stderr, "sendspin: server \"%s\"\n", str);
        send_json(s, "{\"type\":\"client/hello\",\"payload\":{\"name\":\"%s\",\"device_info\":{\"product_name\":\"Echo Dot 3 (hassmic)\","
                     "\"manufacturer\":\"Amazon\",\"software_version\":\"" VERSION "\"},\"supported_roles\":[\"player@v1\",\"controller@v1\"],"
                     "\"player@v1_support\":{\"supported_formats\":[%s],"
                     "\"buffer_capacity\":%u,\"supported_commands\":[\"volume\",\"mute\"]},"
                     "\"supported_pair_methods\":[{\"method\":\"pairing_psk\",\"locations\":[\"device\"]}],\"unpaired_access\":{\"enabled\":true}}}",
                  core_name, formats_json(), BUFFER_CAPACITY);
    } else if (!strcmp(type, "server/activate")) {
        int first = !s->activated, playback = 0, pairing = 0;
        if (js_section(j, "activities", sec, sizeof sec)) { playback = strstr(sec, "playback") != NULL; pairing = strstr(sec, "pairing") != NULL; }
        s->rank = playback ? 2 : pairing ? 1 : 0;
        s->activated = 1;
        if (first && !arbitrate(s)) return;
        if (!s->admitted) return;
        if (playback && strcmp(last_playback_server, s->server_id)) {
            snprintf(last_playback_server, sizeof last_playback_server, "%s", s->server_id);
            save_file("sendspin.lastserver", last_playback_server, strlen(last_playback_server));
        }
        if (js_section(j, "active_roles", sec, sizeof sec)) {                                   /* sticky when omitted */
            int on = strstr(sec, "player@v1") != NULL;
            if (on && !s->player_active) { s->activated_at = raw_us(); s->state_sent = 0; fprintf(stderr, "sendspin: player role active (%s)\n", s->psk_cat == PSK_LONGTERM ? "paired" : "unpaired"); }
            if (!on && s->player_active) { atomic_store(&stream_on, 0); q_flush(); }
            if (!on && !pairing && first) fprintf(stderr, "sendspin: connected, no role yet: approve the device in Music Assistant\n");
            s->player_active = on;
            s->controller_active = strstr(sec, "controller@v1") != NULL;
        }
        s->pairing = 0;
        if (pairing) {
            char method[32] = "";
            if (js_section(j, "pairing", sec, sizeof sec)) js_str(sec, "method", method, sizeof method);
            if (!strcmp(method, "pairing_psk") && s->psk_cat == PSK_PAIRING) {
                /* The server proved it knows our token (the handshake used the pairing PSK).  Hand it a fresh long-term
                 * PSK; it is only stored once the server confirms. */
                char b64[48];
                ws_random(s->new_psk, 32); b64_encode(s->new_psk, 32, b64, 1, 0);
                s->pairing = 1;
                fprintf(stderr, "sendspin: pairing with this server\n");
                send_json(s, "{\"type\":\"client/pair-finalize\",\"payload\":{\"long_term_psk\":\"%s\"}}", b64);
            } else {
                fprintf(stderr, "sendspin: pairing method \"%s\" refused (only the pairing token is supported)\n", method);
                send_json(s, "{\"type\":\"pair/abort\",\"payload\":{\"reason\":\"method_not_supported\"}}");
            }
        }
    } else if (!strcmp(type, "server/pair-finalize")) {
        if (s->pairing) { record_put(s->new_psk, s->server_id); s->pairing = 0; fprintf(stderr, "sendspin: paired, long-term key stored\n"); }
    } else if (!strcmp(type, "pair/abort")) {
        js_str(j, "reason", str, sizeof str); fprintf(stderr, "sendspin: pairing aborted by the server: %s\n", str); s->pairing = 0;
    } else if (!strcmp(type, "server/unpair")) {
        if (s->psk_cat == PSK_LONGTERM) { fprintf(stderr, "sendspin: unpaired by the server\n"); record_drop(s->server_id); goodbye(s, "unpaired"); }
    } else if (!s->admitted) {
        /* everything below belongs to the admitted connection */
    } else if (!strcmp(type, "server/command")) {
        if (!js_section(j, "player", sec, sizeof sec) || !js_str(sec, "command", str, sizeof str)) return;
        if (!strcmp(str, "volume") && js_i64(sec, "volume", &a)) { pthread_mutex_lock(&core_lock); core_set_volume((int)a); pthread_mutex_unlock(&core_lock); }
        else if (!strcmp(str, "mute") && js_i64(sec, "mute", &a)) atomic_store(&player_muted, a != 0);
        else if (!strcmp(str, "set_static_delay") && js_i64(sec, "static_delay_ms", &a) && a >= 0 && a <= 5000) { atomic_store(&static_delay_ms, (int)a); save_delay(); atomic_store(&snap_next, 1); }
        send_state(s);
    } else if (!strcmp(type, "server/state")) {
        if (js_section(j, "controller", sec, sizeof sec)) js_section(sec, "supported_commands", s->ctl_commands, sizeof s->ctl_commands);
    } else if (!strcmp(type, "stream/start")) {
        if (!js_section(j, "player", sec, sizeof sec)) return;
        static char hdr64[1024]; static uint8_t hdr[768]; long hl = 0;
        js_str(sec, "codec", str, sizeof str); js_i64(sec, "sample_rate", &a); js_i64(sec, "channels", &b); js_i64(sec, "bit_depth", &c);
        if (js_str(sec, "codec_header", hdr64, sizeof hdr64)) hl = b64_decode(hdr64, strlen(hdr64), hdr, sizeof hdr);
        if (a != RATE || b != CHANNELS || (strcmp(str, "opus") && c != 16) || decoder_open(str, hdr, hl < 0 ? 0 : (size_t)hl)) {
            fprintf(stderr, "sendspin: cannot play %s %lld Hz x%lld %lld bit, ignoring the stream\n", str, a, b, c); return;
        }
        fprintf(stderr, "sendspin: stream start (%s)\n", str);
        if (!atomic_load(&stream_on)) q_flush();
        atomic_store(&stream_on, 1);
        send_json(s, "{\"type\":\"client/time\",\"payload\":{\"client_transmitted\":%lld}}", raw_us());
    } else if (!strcmp(type, "stream/clear")) {
        if (!js_section(j, "roles", sec, sizeof sec) || strstr(sec, "player")) q_flush();
    } else if (!strcmp(type, "stream/end")) {
        if (!js_section(j, "roles", sec, sizeof sec) || strstr(sec, "player")) { atomic_store(&stream_on, 0); q_flush(); fprintf(stderr, "sendspin: stream end\n"); }
    } else if (!strcmp(type, "group/update")) {
        if (js_str(j, "playback_state", str, sizeof str)) { s->group_playing = !strcmp(str, "playing"); fprintf(stderr, "sendspin: group %s\n", str); }
    }
}

/* Read Noise message 1, pick the PSK it names, answer with message 2.  `initial`: cleartext exchange, Sentinel fallback
 * allowed.  Re-handshake: both messages travel as encrypted JSON under the old keys, prologue = previous handshake hash. */
static int noise_respond(struct session *s, const uint8_t *prologue, size_t pl, const char *data_b64, int initial, char *out_b64, struct noise_cs *tx, struct noise_cs *rx)
{
    uint8_t msg[512], payload[512], psk[32]; char id[64] = ""; struct noise_hs hs;
    long ml = b64_decode(data_b64, strlen(data_b64), msg, sizeof msg);
    noise_kk_responder_init(&hs, prologue, pl, id_priv, id_pub, s->server_pub);
    long pll = ml < 0 ? -1 : noise_kk_read_msg1(&hs, msg, ml, payload);
    if (pll < 0) { fprintf(stderr, "sendspin: handshake message 1 does not authenticate\n"); return -1; }
    payload[pll] = 0; js_str((char *)payload, "psk_id", id, sizeof id);
    int cat = psk_select(id, s->server_id, psk);
    if (cat < 0) {
        if (!initial) return -1;
        fprintf(stderr, "sendspin: server refers to a key we do not have (remove this player's pairing in the server); trying the Sentinel key\n");
        memcpy(psk, sentinel_psk, 32); cat = PSK_SENTINEL;
    }
    s->psk_cat = cat;
    size_t n = noise_kk_write_msg2(&hs, psk, "{}", 2, msg, tx, rx);
    memcpy(s->hs_hash, hs.hash_out, 32);
    b64_encode(msg, n, out_b64, 1, 0);
    return 0;
}

static int handshake(struct session *s)
{
    char init[160], data[512], out_b64[128], out[256]; uint8_t prologue[1024]; int op; uint8_t *d; size_t n;
    int il = snprintf(init, sizeof init, "{\"type\":\"client/init\",\"payload\":{\"client_id\":\"%s\",\"version\":1,\"suite\":\"25519_ChaChaPoly_SHA256\"}}", client_id);
    if (ws_send(&s->ws, WS_TEXT, init, il)) return -1;
    if (ws_recv(&s->ws, &op, &d, &n) != 1 || op != WS_TEXT || il + n > sizeof prologue) return -1;          /* server/init */
    if (!js_str((char *)d, "server_id", s->server_id, sizeof s->server_id) || b64_decode(s->server_id, strlen(s->server_id), s->server_pub, 32) != 32) return -1;
    memcpy(prologue, init, il); memcpy(prologue + il, d, n);                                                  /* raw bytes as sent / received */
    size_t pl = il + n;
    if (ws_recv(&s->ws, &op, &d, &n) != 1 || op != WS_TEXT || !js_str((char *)d, "data", data, sizeof data)) return -1;
    if (noise_respond(s, prologue, pl, data, 1, out_b64, &s->tx, &s->rx)) return -1;
    int ol = snprintf(out, sizeof out, "{\"type\":\"noise/handshake\",\"payload\":{\"data\":\"%s\"}}", out_b64);
    return ws_send(&s->ws, WS_TEXT, out, ol);
}

static int rehandshake(struct session *s, const char *j)
{
    char data[512], out_b64[128]; struct noise_cs tx, rx; uint8_t prev[32];
    if (!js_str(j, "data", data, sizeof data)) return -1;
    memcpy(prev, s->hs_hash, 32);
    s->activated = 0;                                   /* nothing else is sent until the next server/activate */
    if (noise_respond(s, prev, 32, data, 0, out_b64, &tx, &rx)) return -1;
    if (send_json(s, "{\"type\":\"noise/handshake\",\"payload\":{\"data\":\"%s\"}}", out_b64)) return -1;       /* still under the old keys */
    pthread_mutex_lock(&s->send_lock); s->tx = tx; pthread_mutex_unlock(&s->send_lock);
    s->rx = rx;
    fprintf(stderr, "sendspin: re-handshake done (%s)\n", s->psk_cat == PSK_LONGTERM ? "paired" : s->psk_cat == PSK_PAIRING ? "pairing key" : "sentinel");
    return 0;
}

static void *serve(void *arg)
{
    struct session *s = calloc(1, sizeof *s); uint8_t *plain = malloc(70000); char path[128]; int op, fd = (int)(long)arg; uint8_t *d; size_t n; pthread_t tt;
    struct timeval tv = { 75, 0 };                      /* the server pings every 30 s: silence means it is gone */
    if (!s || !plain) { close(fd); free(s); free(plain); return NULL; }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    pthread_mutex_init(&s->send_lock, NULL);
    if (ws_accept(&s->ws, fd, path, sizeof path)) { close(fd); free(s); free(plain); return NULL; }
    if (handshake(s)) { fprintf(stderr, "sendspin: handshake failed\n"); ws_close(&s->ws); free(s); free(plain); return NULL; }
    fprintf(stderr, "sendspin: encrypted session up (%s)\n", s->psk_cat == PSK_LONGTERM ? "paired" : s->psk_cat == PSK_PAIRING ? "pairing key" : "sentinel key");
    pthread_create(&tt, NULL, time_thread, s);

    while (!s->closing && ws_recv(&s->ws, &op, &d, &n) == 1) {
        if (op != WS_BINARY) break;                                     /* text after the handshake is a protocol error */
        long long t_recv = raw_us();
        long pl = n <= 70000 - 1 ? noise_decrypt(&s->rx, d, n, plain) : -1;
        if (pl < 1) { fprintf(stderr, "sendspin: message does not authenticate, closing\n"); break; }
        if (plain[0] == 0) { plain[pl] = 0; on_json(s, (char *)plain + 1, t_recv); }
        else if (plain[0] == 4 && pl > 9 && s->admitted) {              /* audio: int64 big-endian server time, then PCM */
            long long ts = 0; for (int i = 0; i < 8; i++) ts = ts << 8 | plain[1 + i];
            const uint8_t *pcm; size_t n2 = atomic_load(&stream_on) ? decode_chunk(plain + 9, pl - 9, &pcm) : 0;
            if (n2) q_push(ts, pcm, n2);
        }
    }
    s->closing = 1;
    pthread_join(tt, NULL);
    pthread_mutex_lock(&adm_lock);
    if (admitted == s) { admitted = NULL; atomic_store(&stream_on, 0); q_flush(); }
    pthread_mutex_unlock(&adm_lock);
    ws_close(&s->ws); pthread_mutex_destroy(&s->send_lock);
    fprintf(stderr, "sendspin: session closed\n");
    free(plain); free(s);
    return NULL;
}

static void *listen_thread(void *arg)
{
    int ls = net_listen(port); char token[160];
    (void)arg;
    if (ls < 0) { perror("sendspin: listen"); return NULL; }
    sendspin_pairing_token(token, sizeof token);
    fprintf(stderr, "sendspin: player %s on %d\nsendspin: pairing token %s\n", client_id, port, token);
    for (;;) {
        int c = net_accept(ls); pthread_t t;
        if (c < 0) break;
        if (pthread_create(&t, NULL, serve, (void *)(long)c)) close(c); else pthread_detach(t);
    }
    return NULL;
}

/* lock-free callers: core (buttons) */

void sendspin_volume_changed(int percent)
{
    pthread_mutex_lock(&adm_lock);
    if (admitted && admitted->player_active && admitted->state_sent) send_state_vol(admitted, percent);
    pthread_mutex_unlock(&adm_lock);
}

/* Action button: pause while the group plays; resume if it was this button that paused it.  1 = consumed. */
int sendspin_button(void)
{
    int used = 0;
    pthread_mutex_lock(&adm_lock);
    struct session *s = admitted;
    if (s && s->controller_active) {
        if (s->group_playing && atomic_load(&stream_on)) {
            const char *cmd = strstr(s->ctl_commands, "\"pause\"") ? "pause" : strstr(s->ctl_commands, "\"stop\"") ? "stop" : NULL;
            if (cmd) { send_json(s, "{\"type\":\"client/command\",\"payload\":{\"controller\":{\"command\":\"%s\"}}}", cmd); paused_by_button_at = raw_us(); used = 1; }
        } else if (paused_by_button_at && raw_us() - paused_by_button_at < 1800LL * 1000000 && strstr(s->ctl_commands, "\"play\"")) {
            send_json(s, "{\"type\":\"client/command\",\"payload\":{\"controller\":{\"command\":\"play\"}}}"); paused_by_button_at = 0; used = 1;
        }
    }
    pthread_mutex_unlock(&adm_lock);
    if (used) fprintf(stderr, "sendspin: button -> %s\n", paused_by_button_at ? "pause" : "play");
    return used;
}

static void load_identity(void)
{
    static const char LABEL[] = "sendspin-sentinel-psk-v1"; const char *e; char buf[64];
    if ((e = getenv("HASSMIC_STATE"))) state_dir = e;
    if ((e = getenv("HASSMIC_OUTPUT_LATENCY_MS"))) output_latency_us = atoll(e) * 1000;
    sha256(LABEL, sizeof LABEL - 1, sentinel_psk);
    /* the server pins client_id per address, and the pairing token contains both: keep them for ever */
    if (load_file("sendspin.key", id_priv, 32) != 32) { ws_random(id_priv, 32); save_file("sendspin.key", id_priv, 32); }
    if (load_file("sendspin.pairing", pairing_psk, 32) != 32) { ws_random(pairing_psk, 32); save_file("sendspin.pairing", pairing_psk, 32); }
    { extern void crypto_x25519_public_key(uint8_t *, const uint8_t *); crypto_x25519_public_key(id_pub, id_priv); }
    b64_encode(id_pub, 32, client_id, 1, 0);
    int r = load_file("sendspin.records", records, sizeof records); n_records = r > 0 ? r / (int)sizeof records[0] : 0;
    r = load_file("sendspin.lastserver", last_playback_server, sizeof last_playback_server - 1); last_playback_server[r > 0 ? r : 0] = 0;
    r = load_file("sendspin.delay", buf, sizeof buf - 1); buf[r > 0 ? r : 0] = 0;
    if (r > 0 && atoi(buf) >= 0 && atoi(buf) <= 5000) atomic_store(&static_delay_ms, atoi(buf));
}

void sendspin_mdns(int p, char *out, size_t outsz)
{
    snprintf(out, outsz, "  <service><type>_sendspin._tcp</type><port>%d</port><txt-record>path=/sendspin</txt-record>"
                         "<txt-record>name=%s</txt-record></service>\n", p, core_name);
}

void sendspin_init(void) { load_identity(); }

int sendspin_start(int p)
{
    pthread_t t;
    port = p;
    load_identity();
    pthread_create(&t, NULL, player_thread, NULL); pthread_detach(t);
    return pthread_create(&t, NULL, listen_thread, NULL) ? -1 : (pthread_detach(t), 0);
}
