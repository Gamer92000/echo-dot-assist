/*
 * Sendspin player (synchronised multiroom audio, Music Assistant), in two dialects: spec 1.0 as aiosendspin 10.2.1 speaks
 * it (Music Assistant 2.10.8 on), and the older one of aiosendspin 9.1.1 (up to 2.10.6), which differs from the spec in
 * many places (docs/sendspin-digest.md section 0).  A 10.x server reads our 9.1.1-shaped hello as that dialect, logs
 * every difference and may refuse it (issue #64); 9.1.1 refuses the 1.0 shapes.  Which one a server speaks shows before
 * our client/hello: only 1.0 declares psk_category in Noise message 1 (`spec` in the session).
 *
 *   transport   the server finds us by mDNS (_sendspin._tcp) and dials our WebSocket; text frames carry the cleartext init
 *               exchange and the Noise KKpsk2 handshake (we are the responder), binary frames the encrypted session
 *   roles       player@v1: 48 kHz stereo as FLAC (dr_flac), Opus (the firmware's own libopus) or PCM, in that order of
 *               preference (HASSMIC_SENDSPIN_CODECS changes it); the server resamples to it
 *   roles       controller@v1: the action button pauses / resumes the group; the screen's music view (screen.h) sends
 *               play/pause/next/previous/volume, what the server's supported_commands allow
 *   roles       metadata@v1 and artwork@v1, where the model has a screen: title, artist, album, progress, and the album
 *               art as a 400 px JPEG (the server scales it), for the screen.  1.0: the channel in client/state, an image
 *               in parts after its announce, shown at its time (on_artwork); 9.1.1: the channel in client/hello, an image
 *               one message, beyond one Noise message in the library's fragments (types 2/3), put back together here
 *   trust       unpaired access under the published Sentinel PSK (the operator approves the device in Music Assistant),
 *               or paired: the operator pastes our pairing token, we hand over a fresh long-term PSK ("pairing_psk" flow),
 *               the server re-handshakes in band to it.  PIN pairing (CPace) is not implemented and is refused.
 *               Unpaired access is off unless switched on (sendspin_unpaired, on the settings page): anyone on the
 *               LAN knows the Sentinel PSK, so it can play here and sit between us and a real server.  Off, a server
 *               has to pair with the token first; the Sentinel session still carries that pairing.
 *   servers     every Sendspin server on the LAN dials us; one connection is admitted, ranked by declared activity
 *   sync        two-state Kalman filter on client/time exchanges; chunks are written to the mixer so that their first
 *               frame leaves the speaker at the server's timestamp (hard snap at start, then a rate of the music's own,
 *               steered by the error: resample.h)
 */
#include "sendspin.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "audio.h"
#include "board.h"
#include "btout.h"
#include "core.h"
#include "duck.h"
#include "hash.h"
#include "settings.h"
#include "netio.h"
#include "noise.h"
#include "resample.h"
#include "screen.h"
#include "mass.h"
#include "ws.h"
#include "statefile.h"
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
#define SNAP_US 20000                   /* beyond this error: one-shot resync instead of the rate catching up */

static int port; static const char *state_dir = "/data/local/hassmic/state";
static uint8_t id_priv[32], id_pub[32]; static char client_id[48];
/* Mixer + DAC + amp after our write.  Measured with src/tools/latency.c on the device: 83..88 ms from write to the click
 * arriving in micRaw, which includes the capture path (32 ms blocks, stamped on delivery).  Taking ~15 ms for that leaves
 * ~70 ms; the rest is trimmed by ear with the server's static delay.  Override: HASSMIC_OUTPUT_LATENCY_MS. */
static long long output_latency_us = 70000;
/* Playing to a Bluetooth speaker: its own buffer on top (the "Bluetooth speaker delay" setting, btout.c).  The step when
 * the route changes is caught up like any other: one snap. */
static long long out_latency_us(void) { return output_latency_us + btout_latency_us(); }

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
static void stream_set(int on) { atomic_store(&stream_on, on); core_music(MUSIC_SENDSPIN, on); }
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

static int load_file(const char *name, void *buf, size_t len);
static void save_file(const char *name, const void *buf, size_t len);

/* The drift learnt per route (resample.h), in state/sendspin.drift as "<Echo's ppm> <Bluetooth speaker's ppm>": learnt
 * again from nothing it took 20-60 s after every restart and update, a few ms off the other players meanwhile.  Saved
 * when the output closes and every 10 min while it plays, and only once it moved by 5 ppm: the flash spared. */
static void drift_load(double d[2])
{
    char buf[64]; int r = load_file("sendspin.drift", buf, sizeof buf - 1); double a, b;
    buf[r > 0 ? r : 0] = 0; d[0] = d[1] = 0;
    if (sscanf(buf, "%lf %lf", &a, &b) == 2 && fabs(a) <= RATE_DRIFT_MAX * 1e6 && fabs(b) <= RATE_DRIFT_MAX * 1e6) {
        d[0] = a * 1e-6; d[1] = b * 1e-6;
        fprintf(stderr, "sendspin: output clock as learnt before: %+.1f ppm (Echo), %+.1f (Bluetooth speaker)\n", a, b);
    }
}
static void drift_save(const double d[2], double kept[2])
{
    if (fabs(d[0] - kept[0]) < 5e-6 && fabs(d[1] - kept[1]) < 5e-6) return;
    char buf[64]; int n = snprintf(buf, sizeof buf, "%.1f %.1f\n", d[0] * 1e6, d[1] * 1e6);
    save_file("sendspin.drift", buf, (size_t)n); kept[0] = d[0]; kept[1] = d[1];
}

/* A fresh output: silence first (music_prime_us), then wait until the player took it, so the first chunk is timed against
 * the queue it will play behind.  On the Echo Shows AudioTrack takes its buffer's worth at once as it starts (the Show
 * 5's ~137 ms), which the queue leaves out from then on: timed before that, every stream's second chunk came 135-139 ms
 * early, a resync with that much silence after the first 25 ms of music (2026-11-04).  Done once the queue fell by
 * 100 ms, and 30 ms after that: the burst comes in one go.  The log says how much of it went at once, for other models. */
static void prime_output(void)
{
    static const int16_t zero[RATE / 50 * CHANNELS];           /* 20 ms: a slot of audio_android.c's each */
    long long p = music_prime_us();
    if (p <= 0) return;
    for (long long f = p * RATE / 1000000; f > 0; ) { size_t n = f > RATE / 50 ? RATE / 50 : (size_t)f; music_write(zero, n * FRAME); f -= n; }
    long long t0 = raw_us(), first = 0, q;
    while ((q = music_queued_us()) > p - 100000 && raw_us() - t0 < 1000000 && atomic_load(&stream_on)) {
        if (!first && q < p) first = raw_us();
        usleep(5000);
    }
    if (!first) first = raw_us();
    usleep(30000);
    q = music_queued_us();
    long long at_once = p - q - (raw_us() - first);
    fprintf(stderr, "sendspin: output primed with %lld ms of silence, %lld ms of it taken at once\n", p / 1000, at_once > 0 ? at_once / 1000 : 0);
}

/* Each chunk is due at its timestamp.  A snap (the start, an underrun, a new delay, an error past SNAP_US) pads with
 * silence or drops the chunk's late part; in between the music plays at a rate of its own, which the sync error steers
 * (resample.h): an output clock that is off, as the Echo Show 5's, is followed without a frame skipped or repeated. */
static void *player_thread(void *arg)
{
    static int16_t silence[RATE / 50 * CHANNELS]; int open = 0; long long stat_t = 0; int resyncs = 0, routed = -1, stats = 0;
    float duck_g = 1; double step = 1, rate_sum = 0, drift_of[2], drift_kept[2]; long rate_n = 0;   /* rate_sum: the log's mean */
    struct resampler rs; struct rate_servo servo = { 0 };    /* the drift kept across streams: the same output clock */
    int16_t *out = NULL; size_t out_cap = 0;
    (void)arg;
    rs_reset(&rs);
    drift_load(drift_of); memcpy(drift_kept, drift_of, sizeof drift_kept);
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
            if (open) {
                music_close(); open = 0; fprintf(stderr, "sendspin: output closed\n");
                if (routed >= 0) { drift_of[routed] = servo.drift; drift_save(drift_of, drift_kept); }
            }
            usleep(20000);
            continue;
        }
        if (!c) continue;
        if (!tf_synced(NULL)) { free(c); continue; }
        if (!open) { if (music_open(RATE, CHANNELS)) { free(c); usleep(200000); continue; } open = 1; atomic_store(&snap_next, 1); prime_output(); }

        while (music_queued_us() > MIXER_TARGET_US && atomic_load(&stream_on)) usleep(5000);

        /* On a Bluetooth speaker the output's clock is the speaker's: a drift learnt per route, the Echo's back after it */
        int route = btout_routed() != 0;
        if (route != routed) {
            if (routed >= 0) drift_of[routed] = servo.drift;
            routed = route; servo.drift = drift_of[route];
        }
        const uint8_t *pcm = c->pcm; size_t len = c->len;
        long long due = tf_client_time(c->ts) - atomic_load(&static_delay_ms) * 1000LL, at = raw_us() + music_queued_us() + out_latency_us();
        /* > 0: we are early.  The next frame out is still one of the chunk before's (what the resampler looks ahead) */
        long long err = due + llround(rs_lag(&rs) * 1e6 / RATE) - at;
        int fresh = atomic_exchange(&snap_next, 0);
        if (fresh || llabs(err) > SNAP_US) {
            if (!fresh) { resyncs++; fprintf(stderr, "sendspin: %+.1f ms off the schedule, resynced\n", err / 1000.0); }
            err = due - at;                                                             /* from this chunk's first frame */
            if (err > 0) {
                /* silence until the chunk is due, 20 ms at a time as it plays, the error read again before each: left
                 * to run dry while it waited (it used to, down to the last 200 ms), the Shows' AudioTrack emptied and
                 * took its buffer's worth at once again from the next writes, after the chunk was timed (prime_output) */
                for (long long q; atomic_load(&stream_on); ) {
                    q = music_queued_us(); err = due - (raw_us() + q + out_latency_us());
                    long long f = err * RATE / 1000000;
                    if (f <= 0) break;
                    if (q > MIXER_TARGET_US) { usleep(5000); continue; }
                    music_write(silence, (size_t)(f > RATE / 50 ? RATE / 50 : f) * FRAME);
                }
            } else {
                size_t drop = (size_t)(-err * RATE / 1000000) * FRAME;
                if (drop >= len) { free(c); atomic_store(&snap_next, 1); continue; }             /* whole chunk is late */
                pcm += drop; len -= drop;
            }
            rs_reset(&rs); step = rate_servo_snap(&servo);
        } else step = rate_servo(&servo, (double)err, (double)(len / FRAME) / RATE);

        size_t frames = len / FRAME, room = rs_out_max(frames), n;
        if (room > out_cap) {
            int16_t *o = realloc(out, room * FRAME);
            if (!o) { free(c); atomic_store(&snap_next, 1); continue; }
            out = o; out_cap = room;
        }
        n = rs_run(&rs, (const int16_t *)pcm, frames, step, out);
        if (atomic_load(&player_muted)) memset(out, 0, n * FRAME);
        else music_duck(out, n, RATE, &duck_g, duck_active() ? settings_duck_gain() : 1);          /* under the voice assistant, ours or another Echo's (duck.c) */
        music_write(out, n * FRAME);
        rate_sum += step - 1; rate_n++;
        if (raw_us() - stat_t > 10000000) {
            double e; tf_synced(&e); stat_t = raw_us();
            /* the rate: what the music plays at, the output's clock (the error's share of it besides: the queue's slots
             * read skewed); integral: what was learnt of it, kept */
            fprintf(stderr, "sendspin: sync err %+.2f ms, clock ±%.2f ms, queued %zu KB, rate %+.0f ppm (integral %+.0f), %d resyncs\n",
                    servo.smooth * 1e3, e / 1000, q_bytes / 1024, rate_sum / rate_n * 1e6, servo.drift * 1e6, resyncs);
            rate_sum = 0; rate_n = 0;
            if (++stats % 60 == 0) { drift_of[routed] = servo.drift; drift_save(drift_of, drift_kept); }      /* 10 min */
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
static atomic_int unpaired_ok;         /* unpaired access (Sentinel PSK) allowed; state file sendspin.unpaired, default off */
static char last_playback_server[48];
static atomic_int server_known;        /* a server held the player role here (this run, or played here before) */

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
    char path[256]; state_path(path, sizeof path, name);
    if (statefile_write(path, 0600, buf, len)) fprintf(stderr, "sendspin: cannot write %s\n", path);
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

/* Which of our PSKs does the server mean?  `declared`: the category it uses it as ("lt", "pr", "sn"; spec 1.0: a key we
 * hold only as another category is a miss), NULL from aiosendspin 9.x, which declares none.  -1: none (fall back to the
 * Sentinel on an initial handshake), -2: fail the handshake (an unknown category; 1.0: another server's record, a
 * misbinding rather than a miss). */
static int psk_select(const char *id, const char *declared, const char *server_id, uint8_t out[32])
{
    char cand[48]; int cat = -1;
    if (declared && strcmp(declared, "lt") && strcmp(declared, "pr") && strcmp(declared, "sn")) return -2;
    if (!declared || !strcmp(declared, "sn")) { psk_id(sentinel_psk, cand); if (!strcmp(cand, id)) { memcpy(out, sentinel_psk, 32); return PSK_SENTINEL; } }
    if (!declared || !strcmp(declared, "pr")) { psk_id(pairing_psk, cand);  if (!strcmp(cand, id)) { memcpy(out, pairing_psk, 32); return PSK_PAIRING; } }
    if (declared && strcmp(declared, "lt")) return -1;
    pthread_mutex_lock(&rec_lock);
    for (int i = 0; i < n_records && cat == -1; i++) {
        psk_id(records[i].psk, cand);
        if (strcmp(cand, id)) continue;
        if (!strcmp(records[i].server_id, server_id)) { memcpy(out, records[i].psk, 32); cat = PSK_LONGTERM; }
        else if (declared) cat = -2;
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

#define FRAG_MAX (1u << 20)                     /* far above anything we take: chunks are at most 150 ms, JSON a few KB */

struct session {
    struct ws ws; struct noise_cs tx, rx; pthread_mutex_t send_lock;
    uint8_t server_pub[32], hs_hash[32], new_psk[32]; char server_id[48]; int psk_cat;
    int spec;                          /* the server speaks spec 1.0 (aiosendspin 10.x), else 9.1.1's dialect */
    int activated, admitted, rank, player_active, controller_active, state_sent, available_sent, pairing, closing; long long activated_at;
    int pairing_index;                 /* pairing server/activates since the last Noise handshake (1.0: in client/pair-init) */
    int quiet, state_owed;             /* re-handshake: nothing but its own messages until the next server/activate (under
                                          send_lock); a client/state held back meanwhile, sent after it */
    char ctl_commands[256]; int group_playing; long long seek_max;   /* seek_max: the controller's seek_max_ms (0: none) */
    /* metadata@v1: what the server said so far (it sends changes), the track's position by our clock at m_at (raw_us) */
    char m_title[256], m_artist[256], m_album[256];
    long long m_pos, m_dur, m_at; int m_speed, m_known;
    uint8_t *frag; size_t frag_len; int frag_on;     /* a fragmented message being put together: its type byte, then data */
    /* artwork@v1, 1.0: an image comes as an announce (its time, its size) and parts; done, it waits for its time (the
     * server announces up to 20 s ahead: the next track's cover) in due, which the time thread shows (art_lock) */
    uint8_t *art_in; size_t art_size, art_len; long long art_ts; int art_on;
    pthread_mutex_t art_lock; uint8_t *due; size_t due_n; long long due_at; int due_on;
    /* metadata@v1, 1.0: the next track's comes ahead of its time as well, held in meta_due (the whole server/state)
     * until meta_at; meta_lock: these and the m_ fields above (the session's thread and the time thread) */
    pthread_mutex_t meta_lock; char *meta_due; long long meta_at;
    int meta_role, art_role;                         /* metadata@v1, artwork@v1 active: their state goes with them */
};

/* At most one admitted connection; the others are provisional until their first server/activate decides. */
static pthread_mutex_t adm_lock = PTHREAD_MUTEX_INITIALIZER;
static struct session *admitted;
static struct session *live[16];        /* sessions past the handshake (adm_lock): switching unpaired access closes the idle ones */
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

static int hex4(const char *p)                 /* four hex digits (it stops at the string's NUL) */
{
    for (int i = 0; i < 4; i++) if (!isxdigit((unsigned char)p[i])) return 0;
    return 1;
}

/* a JSON string value, unescaped into UTF-8 (\uXXXX and surrogate pairs too: titles in any script); 0 if the key is
 * missing or not a string (null) */
static int js_ustr(const char *j, const char *key, char *out, size_t outsz)
{
    char pat[64]; const char *p = j; size_t n = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    while ((p = strstr(p, pat))) {
        p += strlen(pat); while (*p == ' ') p++;
        if (*p != ':') continue;
        p++; while (*p == ' ') p++;
        if (*p != '"') return 0;
        for (p++; *p && *p != '"'; p++) {
            unsigned c = (unsigned char)*p; char u[4]; size_t k = 0;
            if (c == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': case 'r': case 't': c = ' '; break;
                case 'b': case 'f': continue;
                case 'u': {
                    unsigned v = 0;
                    if (!hex4(p + 1) || sscanf(p + 1, "%4x", &v) != 1) return 0;   /* all four there: a message cut in one ends */
                    p += 4;
                    if (v >= 0xD800 && v < 0xDC00 && p[1] == '\\' && p[2] == 'u' && hex4(p + 3)) {
                        unsigned lo = 0;
                        if (sscanf(p + 3, "%4x", &lo) == 1 && lo >= 0xDC00 && lo < 0xE000) { v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                    }
                    if (v < 0x80) u[k++] = (char)v;
                    else if (v < 0x800) { u[k++] = (char)(0xC0 | v >> 6); u[k++] = (char)(0x80 | (v & 63)); }
                    else if (v < 0x10000) { u[k++] = (char)(0xE0 | v >> 12); u[k++] = (char)(0x80 | (v >> 6 & 63)); u[k++] = (char)(0x80 | (v & 63)); }
                    else { u[k++] = (char)(0xF0 | v >> 18); u[k++] = (char)(0x80 | (v >> 12 & 63)); u[k++] = (char)(0x80 | (v >> 6 & 63)); u[k++] = (char)(0x80 | (v & 63)); }
                    if (n + k < outsz) { memcpy(out + n, u, k); n += k; }
                    continue; }
                default: c = (unsigned char)*p;
                }
            }
            if (n + 1 < outsz) out[n++] = (char)c;
        }
        out[n] = 0; return 1;
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
        int str = 0;                                     /* brackets inside strings ("Smile :-}") are text */
        do {
            if (str) { if (*p == '\\' && p[1]) { if (n + 1 < outsz) out[n++] = *p; p++; } else if (*p == '"') str = 0; }
            else if (*p == '"') str = 1;
            else if (*p == '[' || *p == '{') depth++;
            else if (*p == ']' || *p == '}') depth--;
            if (n + 1 < outsz) out[n++] = *p;
            p++;
        } while (*p && depth);
        out[n] = 0; return 1;
    }
    return 0;
}

/* 0 sent, -1 error, 1 held back: from Noise message 1 of a re-handshake to the next server/activate neither side may
 * start a message but the handshake's (1.0; 10.2.1 flags anything else, and strict it closes without coming back).
 * `forced`: such a message, past that gate.  `rekey`: the keys for every later message, swapped under the same lock
 * (message 2 still goes under the old ones, and nothing another thread sends may come between). */
static int vsend_json(struct session *s, int forced, const struct noise_cs *rekey, const char *fmt, va_list ap)
{
    static __thread char msg[2048]; uint8_t out[2048 + NOISE_TAG]; int rc;
    msg[0] = 0;                                         /* type byte 0 = JSON */
    int n = vsnprintf(msg + 1, sizeof msg - 1, fmt, ap);
    pthread_mutex_lock(&s->send_lock);                  /* nonce order must equal wire order */
    if (s->quiet && !forced) { pthread_mutex_unlock(&s->send_lock); return 1; }
    size_t cl = noise_encrypt(&s->tx, msg, n + 1, out);
    rc = ws_send(&s->ws, WS_BINARY, out, cl);
    if (rekey) s->tx = *rekey;
    pthread_mutex_unlock(&s->send_lock);
    return rc;
}

static int send_json(struct session *s, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); int rc = vsend_json(s, 0, NULL, fmt, ap); va_end(ap);
    return rc;
}

static int send_json_forced(struct session *s, const struct noise_cs *rekey, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); int rc = vsend_json(s, 1, rekey, fmt, ap); va_end(ap);
    return rc;
}

static void set_quiet(struct session *s, int on) { pthread_mutex_lock(&s->send_lock); s->quiet = on; pthread_mutex_unlock(&s->send_lock); }

/* 1.0: the delay is output_delay_ms / set_output_delay, and every command the server may send is listed here (9.1.1
 * takes only its set_static_delay here and the rest from the hello).  available only once the clock filter has
 * converged (spec: the server starts no stream before); 9.1.1 has no such rule. */
static void send_state_vol(struct session *s, int vol)
{
    int available = !s->spec || tf_synced(NULL);
    /* 1.0: the cover's channel in every client/state (the same one again changes nothing); 9.1.1: in client/hello */
    s->state_owed = 1 == send_json(s, "{\"type\":\"client/state\",\"payload\":{\"available\":%s,\"player\":{\"volume\":%d,\"muted\":%s,\"%s\":%d,"
                 "\"required_lead_time_ms\":%d,\"min_buffer_ms\":%d,\"supported_commands\":[%s]}%s}}",
              available ? "true" : "false", vol, atomic_load(&player_muted) ? "true" : "false",
              s->spec ? "output_delay_ms" : "static_delay_ms", atomic_load(&static_delay_ms), LEAD_MS, LEAD_MS,
              s->spec ? "\"volume\",\"mute\",\"set_output_delay\"" : "\"set_static_delay\"",
              s->spec && screen_present() ? ",\"artwork\":{\"channels\":[{\"source\":\"album\",\"format\":\"jpeg\",\"width\":400,\"height\":400}]}" : "");
    s->state_sent = 1; s->available_sent = available;
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

static void meta_tick(struct session *s);

/* a cover whose time came: to the screen (the time thread, every 50 ms) */
static void art_tick(struct session *s)
{
    pthread_mutex_lock(&s->art_lock);
    uint8_t *img = NULL; size_t n = 0; int show = s->due_on && raw_us() >= s->due_at;
    if (show) { img = s->due; n = s->due_n; s->due = NULL; s->due_n = 0; s->due_on = 0; }
    pthread_mutex_unlock(&s->art_lock);
    if (show && s->admitted) screen_music_art(img, n);
    free(img);
}

/* an image complete (n 0: the cover cleared) at server time ts: it replaces one still waiting (the server cancels
 * that first when the new one is not scheduled; a scheduled one replaces only a scheduled one) */
static void art_done(struct session *s, uint8_t *img, size_t n, long long ts)
{
    long long now = raw_us(), at = ts && tf_synced(NULL) ? tf_client_time(ts) : now;
    if (at > now + 30000000) at = now + 30000000;       /* the server's lead is 20 s at most */
    pthread_mutex_lock(&s->art_lock);
    free(s->due); s->due = img; s->due_n = n; s->due_at = at; s->due_on = 1;
    pthread_mutex_unlock(&s->art_lock);
    art_tick(s);
}

/* 1.0's artwork channel 0: [8][flags][...]: 2 announce + int64 time + uint32 size (0: clear), 0 a part, 1 cancel
 * (the transfer under way and an image still waiting).  Reserved flags or parts beyond the size: ignored. */
static void on_artwork(struct session *s, const uint8_t *m, size_t len)
{
    if (len < 2) return;
    if (m[1] == 1) {
        s->art_on = 0; free(s->art_in); s->art_in = NULL;
        pthread_mutex_lock(&s->art_lock); free(s->due); s->due = NULL; s->due_n = 0; s->due_on = 0; pthread_mutex_unlock(&s->art_lock);
    } else if (m[1] == 2 && len == 14) {
        long long ts = 0; size_t size = 0;
        for (int i = 0; i < 8; i++) ts = ts << 8 | m[2 + i];
        for (int i = 0; i < 4; i++) size = size << 8 | m[10 + i];
        free(s->art_in); s->art_in = NULL; s->art_on = 0;
        if (!size) { art_done(s, NULL, 0, ts); return; }
        if (size > FRAG_MAX || !(s->art_in = malloc(size))) return;    /* a cover is far smaller: its parts are ignored */
        s->art_size = size; s->art_len = 0; s->art_ts = ts; s->art_on = 1;
    } else if (m[1] == 0 && s->art_on) {
        if (len - 2 > s->art_size - s->art_len) { s->art_on = 0; free(s->art_in); s->art_in = NULL; return; }
        memcpy(s->art_in + s->art_len, m + 2, len - 2); s->art_len += len - 2;
        if (s->art_len == s->art_size) { uint8_t *img = s->art_in; s->art_in = NULL; s->art_on = 0; art_done(s, img, s->art_size, s->art_ts); }
    }
}

static void *time_thread(void *arg)
{
    struct session *s = arg;
    while (!s->closing) {
        double e; int ok = tf_synced(&e);
        art_tick(s); meta_tick(s);
        /* nothing but client/hello may precede the first server/activate; silent during pairing and re-handshake; only
         * the admitted connection drives the clock */
        if (!s->activated || !s->admitted || s->pairing) { usleep(50000); continue; }
        send_json(s, "{\"type\":\"client/time\",\"payload\":{\"client_transmitted\":%lld}}", raw_us());
        /* first client/state: once the clock is usable, but well inside the server's 5 s initial-state timer; sent
         * unavailable (1.0) when the filter was not there yet, then again once it is */
        if (s->player_active && !s->state_sent && ((ok && e < 5000) || raw_us() - s->activated_at > 3000000)) send_state(s);
        else if (s->player_active && !s->available_sent && ok) send_state(s);
        long long nap = !ok ? 200000 : e < 1000 ? 3000000 : e < 2000 ? 1000000 : e < 5000 ? 500000 : 200000;
        for (long long t = 0; t < nap && !s->closing; t += 50000) { usleep(50000); art_tick(s); meta_tick(s); }
    }
    return NULL;
}

/* First server/activate of a connection: may it replace the admitted one?  Rank: playback 2 > pairing 1 > nothing 0;
 * incoming >= current wins; two idle ones: only the server that last played here may take over. */
static int arbitrate(struct session *s)
{
    int win, gone = 0;
    pthread_mutex_lock(&adm_lock);
    struct session *cur = admitted;
    if (!cur || cur == s) win = 1;
    else if (s->rank == 0 && cur->rank == 0) win = !strcmp(s->server_id, last_playback_server) && strcmp(cur->server_id, last_playback_server);
    else win = s->rank >= cur->rank;
    if (win) {
        if (cur && cur != s) { fprintf(stderr, "sendspin: another server takes over\n"); cur->admitted = 0; goodbye(cur, "another_server"); gone = 1; }
        if (cur != s) { stream_set(0); q_flush(); tf_reset(); }
        admitted = s; s->admitted = 1;
    }
    pthread_mutex_unlock(&adm_lock);
    if (gone) screen_music_gone();                      /* the old server's track: the new one tells its own */
    if (!win) { fprintf(stderr, "sendspin: second server turned away, the current one is busier\n"); goodbye(s, "concurrent_attempt"); }
    return win;
}

static int rehandshake(struct session *s, const char *j);

/* supported_formats in order of preference: the server takes the first one it can encode.  flac or pcm always among
 * them (spec: every server makes both; 10.2.1 flags a list without either): pcm last if the override left both out */
static const char *formats_json(void)
{
    static char out[512]; const char *pref = getenv("HASSMIC_SENDSPIN_CODECS"); size_t o = 0; int any = 0;
    if (!pref || !*pref) pref = "flac,opus,pcm";
    out[0] = 0;
    for (const char *p = pref; *p; ) {
        size_t n = strcspn(p, ",");
        int flac = n == 4 && !strncmp(p, "flac", 4), pcm = n == 3 && !strncmp(p, "pcm", 3);
        if (flac || pcm || (n == 4 && !strncmp(p, "opus", 4)))
            o += snprintf(out + o, sizeof out - o, "%s{\"codec\":\"%.*s\",\"channels\":%d,\"sample_rate\":%d,\"bit_depth\":16}", o ? "," : "", (int)n, p, CHANNELS, RATE);
        any |= flac || pcm;
        p += n; if (*p == ',') p++;
    }
    if (!any) snprintf(out + o, sizeof out - o, "%s{\"codec\":\"pcm\",\"channels\":%d,\"sample_rate\":%d,\"bit_depth\":16}", o ? "," : "", CHANNELS, RATE);
    return out;
}

/* the track's position now, by our clock, for the screen (state 0 stopped, 1 playing, 2 paused; playback_speed 0 is
 * paused too) */
static void music_progress(struct session *s, int state)
{
    if (state == 1 && s->m_known && !s->m_speed) state = 2;
    if (!s->m_known) { screen_music_progress(state, 0, 0, 1000); return; }
    long long pos = s->m_pos + (state == 1 ? (raw_us() - s->m_at) / 1000 * s->m_speed / 1000 : 0);
    if (s->m_dur > 0 && pos > s->m_dur) pos = s->m_dur;
    screen_music_progress(state, pos, s->m_dur, s->m_speed);
}

/* "key": present (a string or null: 1, out set, "" for null) or left out (0) */
static int js_field(const char *j, const char *key, char *out, size_t n)
{
    char pat[64]; const char *p = j;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    while ((p = strstr(p, pat))) {
        p += strlen(pat); while (*p == ' ') p++;
        if (*p != ':') continue;
        if (!js_ustr(j, key, out, n)) out[0] = 0;
        return 1;
    }
    return 0;
}

/* server/state's metadata: what changed (aiosendspin diff_update: a field left out is unchanged, null clears it),
 * stamped with the server's time */
static void meta_apply(struct session *s, const char *j, long long t_recv)     /* meta_lock held */
{
    char sec[8192], prog[256], none[8]; long long ts = 0, a = 0;       /* each session's thread its own */
    if (!js_section(j, "metadata", sec, sizeof sec)) {
        /* "metadata": null: nothing plays any more (aiosendspin 10's set_metadata(None)) */
        if (js_field(j, "metadata", none, sizeof none) && (s->m_title[0] || s->m_artist[0] || s->m_album[0] || s->m_known)) {
            s->m_title[0] = s->m_artist[0] = s->m_album[0] = 0; s->m_known = 0;
            screen_music_meta("", "", ""); music_progress(s, 0);
        }
        return;
    }
    /* 9.1.1 sends what changed (diff_update: left out = unchanged); 1.0 the whole state each time (snapshot_update,
     * None left out): there a field not in it is empty, and only timestamp is everything cleared */
    char *f[3] = { s->m_title, s->m_artist, s->m_album }; static const char *k[3] = { "title", "artist", "album" };
    int changed = 0;
    for (int i = 0; i < 3; i++) {
        if (js_field(sec, k[i], f[i], sizeof s->m_title)) changed = 1;
        else if (s->spec && f[i][0]) { f[i][0] = 0; changed = 1; }
    }
    if (changed) screen_music_meta(s->m_title, s->m_artist, s->m_album);
    if (js_section(sec, "progress", prog, sizeof prog)) {
        js_i64(sec, "timestamp", &ts);
        long long at = ts && tf_synced(NULL) ? tf_client_time(ts) : t_recv;
        s->m_pos = js_i64(prog, "track_progress", &a) ? a : 0;
        s->m_dur = js_i64(prog, "track_duration", &a) ? a : 0;
        s->m_speed = js_i64(prog, "playback_speed", &a) ? (int)a : 1000;
        s->m_pos += (t_recv - at) / 1000 * s->m_speed / 1000;     /* where it was when the message came */
        s->m_at = t_recv; s->m_known = 1;
    } else if (js_field(sec, "progress", prog, sizeof prog) || s->spec) s->m_known = 0;   /* null (1.0: left out): no position any more */
    else if (!changed) return;
    music_progress(s, s->group_playing ? 1 : s->m_known ? 2 : 0);
}

/* 1.0: metadata whose timestamp is ahead is the next track's (sent up to 20 s early): held until then and applied by
 * the time thread (meta_tick); a newer one replaces it, one for now (or none: null) drops it, as the server's own
 * state goes.  A server/state without metadata changes nothing. */
static void on_metadata(struct session *s, const char *j, long long t_recv)
{
    char sec[8192], none[8]; long long ts = 0, now = raw_us();
    int obj = js_section(j, "metadata", sec, sizeof sec);
    if (!obj && !js_field(j, "metadata", none, sizeof none)) return;
    long long at = s->spec && obj && js_i64(sec, "timestamp", &ts) && ts && tf_synced(NULL) ? tf_client_time(ts) : now;
    char *hold = at > now + 100000 ? strdup(j) : NULL;
    pthread_mutex_lock(&s->meta_lock);
    free(s->meta_due); s->meta_due = hold; s->meta_at = at > now + 30000000 ? now + 30000000 : at;
    if (!hold) meta_apply(s, j, t_recv);
    pthread_mutex_unlock(&s->meta_lock);
}

static void meta_tick(struct session *s)
{
    pthread_mutex_lock(&s->meta_lock);
    if (s->meta_due && raw_us() >= s->meta_at) {
        if (s->admitted) meta_apply(s, s->meta_due, raw_us());
        free(s->meta_due); s->meta_due = NULL;
    }
    pthread_mutex_unlock(&s->meta_lock);
}

/* a role's state gone with the role (1.0 sends no null object for it) */
static void meta_gone(struct session *s)
{
    pthread_mutex_lock(&s->meta_lock);
    free(s->meta_due); s->meta_due = NULL;
    s->m_title[0] = s->m_artist[0] = s->m_album[0] = 0; s->m_known = 0;
    screen_music_meta("", "", ""); music_progress(s, 0);
    pthread_mutex_unlock(&s->meta_lock);
}

static void art_gone(struct session *s)
{
    s->art_on = 0; free(s->art_in); s->art_in = NULL;
    pthread_mutex_lock(&s->art_lock); free(s->due); s->due = NULL; s->due_n = 0; s->due_on = 0; pthread_mutex_unlock(&s->art_lock);
    screen_music_art(NULL, 0);
}

/* The address of a paired server that holds the player role here: the screen's music pages (mass.c) suggest Music
 * Assistant's own API there.  Only a suggestion: a token goes only to the address the settings page pinned (a pairing
 * session, or any host speaking Sendspin, never moves it).  Never an unpaired one's: with unpaired access on, that is
 * any host on the LAN, and the page would offer its address for the token */
static void suggest_peer(struct session *s)
{
    struct sockaddr_storage a; socklen_t al = sizeof a; char ip[64] = "";
    if (s->psk_cat != PSK_LONGTERM || getpeername(s->ws.fd, (struct sockaddr *)&a, &al)) return;
    if (a.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&a)->sin_addr, ip, sizeof ip);
    else if (a.ss_family == AF_INET6) {
        const struct in6_addr *v6 = &((struct sockaddr_in6 *)&a)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(v6)) inet_ntop(AF_INET, (const uint8_t *)v6 + 12, ip, sizeof ip);
        else { ip[0] = '['; inet_ntop(AF_INET6, v6, ip + 1, sizeof ip - 2); strcat(ip, "]"); }
    }
    mass_peer(ip);
}

static void on_json(struct session *s, char *j, long long t_recv)
{
    char type[48] = "", sec[512], str[64]; long long a, b, c;
    js_str(j, "type", type, sizeof type);
    if (!strcmp(type, "server/time")) {
        /* spec's filter input: integer halves; a round trip of nothing (or less) says nothing */
        long long z, e;
        if (s->admitted && js_i64(j, "client_transmitted", &a) && js_i64(j, "server_received", &b) && js_i64(j, "server_transmitted", &c) &&
            (z = ((b - a) + (c - t_recv)) / 2, e = ((t_recv - a) - (c - b)) / 2) > 0)
            tf_update((double)z, (double)e, t_recv);
    } else if (!strcmp(type, "noise/handshake")) {
        if (rehandshake(s, j)) { fprintf(stderr, "sendspin: re-handshake failed\n"); s->closing = 1; shutdown(s->ws.fd, SHUT_RDWR); }
    } else if (!strcmp(type, "server/hello")) {
        js_str(j, "name", str, sizeof str); fprintf(stderr, "sendspin: server \"%s\"\n", str);
        /* 1.0: the player's commands go to client/state, the pairing methods are an object keyed by method.  The
         * 9.1.1 shapes are what that library requires, and what marks a client as its dialect for 10.x.  Forced: 9.1.1
         * asks for it again after a re-handshake, before its server/activate (1.0 does not ask).  A model with a screen
         * also takes the track's metadata and its cover (screen.h). */
        int scr = screen_present();
        send_json_forced(s, NULL, "{\"type\":\"client/hello\",\"payload\":{\"name\":\"%s\",\"device_info\":{\"product_name\":\"%s\","
                     "\"manufacturer\":\"Amazon\",\"software_version\":\"" VERSION "\"},\"supported_roles\":[\"player@v1\",\"controller@v1\"%s],"
                     "\"player@v1_support\":{\"supported_formats\":[%s],\"buffer_capacity\":%u%s},%s"
                     "\"supported_pair_methods\":%s,\"unpaired_access\":{\"enabled\":%s}}}",
                  core_name, board.product, scr ? ",\"metadata@v1\",\"artwork@v1\"" : "", formats_json(), BUFFER_CAPACITY,
                  s->spec ? "" : ",\"supported_commands\":[\"volume\",\"mute\"]",
                  scr && !s->spec ? "\"artwork@v1_support\":{\"channels\":[{\"source\":\"album\",\"format\":\"jpeg\",\"media_width\":400,\"media_height\":400}]}," : "",
                  s->spec ? "{\"pairing_psk\":{\"locations\":[\"device\"]}}" : "[{\"method\":\"pairing_psk\",\"locations\":[\"device\"]}]",
                  atomic_load(&unpaired_ok) ? "true" : "false");
    } else if (!strcmp(type, "server/activate")) {
        int first = !s->activated, playback = 0, pairing = 0, rekeyed = s->quiet;
        if (rekeyed) set_quiet(s, 0);                   /* its receipt ends a re-handshake's quiet period */
        if (js_section(j, "activities", sec, sizeof sec)) { playback = strstr(sec, "playback") != NULL; pairing = strstr(sec, "pairing") != NULL; }
        s->rank = playback ? 2 : pairing ? 1 : 0;
        s->activated = 1;
        /* Unpaired access off: a Sentinel or pairing-key session may pair, but not play (spec: pairing_required, close).
         * Checked before arbitration, so a refused server does not push out the admitted one. */
        if (!atomic_load(&unpaired_ok) && s->psk_cat != PSK_LONGTERM &&
            (playback || (js_section(j, "active_roles", sec, sizeof sec) && strstr(sec, "@v")))) {
            fprintf(stderr, "sendspin: unpaired server turned away (paired servers only)\n");
            goodbye(s, "pairing_required"); return;
        }
        /* 1.0: a paired session never pairs (a server re-handshakes to the pairing key first) */
        if (s->spec && pairing && s->psk_cat == PSK_LONGTERM) { fprintf(stderr, "sendspin: paired server asked to pair, refused\n"); goodbye(s, "unauthorized"); return; }
        if (first && !arbitrate(s)) return;
        if (!s->admitted) return;
        if (playback && strcmp(last_playback_server, s->server_id)) {
            snprintf(last_playback_server, sizeof last_playback_server, "%s", s->server_id);
            save_file("sendspin.lastserver", last_playback_server, strlen(last_playback_server));
        }
        int had_player = s->player_active;
        if (js_section(j, "active_roles", sec, sizeof sec)) {                                   /* sticky when omitted */
            int on = strstr(sec, "player@v1") != NULL;
            if (on && !s->player_active) { s->activated_at = raw_us(); s->state_sent = 0; atomic_store(&server_known, 1); }
            if (!on && s->player_active) { stream_set(0); q_flush(); }
            if (!on && !pairing && first)
                fprintf(stderr, "sendspin: connected, no role yet: %s\n", atomic_load(&unpaired_ok) ? "approve the device in Music Assistant"
                        : "pair it with the token (or allow Music Assistant without pairing on the settings page)");
            s->player_active = on;
            int ctl = strstr(sec, "controller@v1") != NULL, meta = strstr(sec, "metadata@v1") != NULL, art = strstr(sec, "artwork@v1") != NULL;
            if (!ctl) { s->ctl_commands[0] = 0; s->group_playing = 0; }      /* its server/state goes with it */
            if (screen_present()) {                                          /* and what the screen showed of it */
                if (s->controller_active && !ctl) screen_music_controller(0, 0, "", NULL, -1);
                if (s->meta_role && !meta) meta_gone(s);
                if (s->art_role && !art) art_gone(s);
            }
            s->controller_active = ctl; s->meta_role = meta; s->art_role = art;
        }
        /* and again after each re-handshake: 1.0 keeps the role through pairing, the new key is what changed */
        if (s->player_active && (!had_player || first)) {
            fprintf(stderr, "sendspin: player role active (%s)\n", s->psk_cat == PSK_LONGTERM ? "paired" : "unpaired");
            suggest_peer(s);
        }
        if (rekeyed && had_player && s->player_active && s->state_owed) send_state(s);       /* a client/state held back */
        s->pairing = 0;
        if (pairing) {
            char method[32] = "";
            s->pairing_index++;
            if (js_section(j, "pairing", sec, sizeof sec)) js_str(sec, "method", method, sizeof method);
            if (!strcmp(method, "pairing_psk") && s->psk_cat == PSK_PAIRING) {
                /* The server proved it knows our token (the handshake used the pairing PSK).  Hand it a fresh long-term
                 * PSK; it is only stored once the server confirms.  1.0 opens the attempt with client/pair-init first,
                 * which 9.1.1 takes for a protocol error. */
                char b64[48];
                ws_random(s->new_psk, 32); b64_encode(s->new_psk, 32, b64, 1, 0);
                s->pairing = 1;
                fprintf(stderr, "sendspin: pairing with this server\n");
                if (s->spec) send_json(s, "{\"type\":\"client/pair-init\",\"payload\":{\"pairing_index\":%d}}", s->pairing_index);
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
        if (!s->player_active || !js_section(j, "player", sec, sizeof sec) || !js_str(sec, "command", str, sizeof str)) return;
        if (!strcmp(str, "volume") && js_i64(sec, "volume", &a)) { pthread_mutex_lock(&core_lock); core_set_volume((int)a); pthread_mutex_unlock(&core_lock); }
        else if (!strcmp(str, "mute") && js_i64(sec, "mute", &a)) atomic_store(&player_muted, a != 0);
        else if (!strcmp(str, s->spec ? "set_output_delay" : "set_static_delay") && js_i64(sec, s->spec ? "output_delay_ms" : "static_delay_ms", &a) &&
                 (s->spec || (a >= 0 && a <= 5000))) {                           /* 1.0: clamped, not refused */
            atomic_store(&static_delay_ms, a < 0 ? 0 : a > 5000 ? 5000 : (int)a); save_delay(); atomic_store(&snap_next, 1);
        }
        send_state(s);
    } else if (!strcmp(type, "server/state")) {
        if (js_section(j, "controller", sec, sizeof sec)) {
            js_section(sec, "supported_commands", s->ctl_commands, sizeof s->ctl_commands);
            if (!js_i64(sec, "seek_max_ms", &s->seek_max)) s->seek_max = 0;
            if (screen_present() && s->admitted) {                    /* the screen: the server that holds the player */
                char list[256] = ""; size_t o = 0; long long vol = 0, mu = 0;
                for (const char *c = s->ctl_commands; *c && o + 1 < sizeof list; c++)       /* ["play","pause"] -> play,pause */
                    if (*c != '[' && *c != ']' && *c != '"' && *c != ' ') list[o++] = *c;
                list[o] = 0;
                char rep[8] = ""; long long sh = -1;          /* the key's own, not the "shuffle" of supported_commands */
                js_i64(sec, "volume", &vol); js_i64(sec, "muted", &mu); js_str(sec, "repeat", rep, sizeof rep);
                if (!js_i64(sec, "shuffle", &sh)) sh = -1;
                screen_music_controller((int)vol, (int)mu, list, rep[0] ? rep : NULL, sh < 0 ? -1 : sh != 0);
            }
        }
        if (screen_present() && s->admitted) on_metadata(s, j, t_recv);
    } else if (!strcmp(type, "stream/start")) {
        if (!s->player_active || !js_section(j, "player", sec, sizeof sec)) return;
        static char hdr64[1024]; static uint8_t hdr[768]; long hl = 0;
        js_str(sec, "codec", str, sizeof str); js_i64(sec, "sample_rate", &a); js_i64(sec, "channels", &b); js_i64(sec, "bit_depth", &c);
        if (js_str(sec, "codec_header", hdr64, sizeof hdr64)) hl = b64_decode(hdr64, strlen(hdr64), hdr, sizeof hdr);
        if (a != RATE || b != CHANNELS || (strcmp(str, "opus") && c != 16) || decoder_open(str, hdr, hl < 0 ? 0 : (size_t)hl)) {
            fprintf(stderr, "sendspin: cannot play %s %lld Hz x%lld %lld bit, ignoring the stream\n", str, a, b, c); return;
        }
        fprintf(stderr, "sendspin: stream start (%s)\n", str);
        if (!atomic_load(&stream_on)) q_flush();
        stream_set(1);
        send_json(s, "{\"type\":\"client/time\",\"payload\":{\"client_transmitted\":%lld}}", raw_us());
    } else if (!strcmp(type, "stream/clear")) {
        if (!js_section(j, "roles", sec, sizeof sec) || strstr(sec, "player")) q_flush();
    } else if (!strcmp(type, "stream/end")) {
        int all = !js_section(j, "roles", sec, sizeof sec);
        if (all || strstr(sec, "player")) { stream_set(0); q_flush(); fprintf(stderr, "sendspin: stream end\n"); }
        if (s->spec && s->admitted && screen_present() && (all || strstr(sec, "artwork"))) art_gone(s);   /* 1.0: its images with it (9.1.1 ends every track) */
    } else if (!strcmp(type, "group/update")) {
        if (js_str(j, "playback_state", str, sizeof str)) {
            s->group_playing = !strcmp(str, "playing"); fprintf(stderr, "sendspin: group %s\n", str);
            if (screen_present() && s->admitted) {
                pthread_mutex_lock(&s->meta_lock); music_progress(s, !strcmp(str, "playing") ? 1 : !strcmp(str, "paused") ? 2 : 0); pthread_mutex_unlock(&s->meta_lock);
            }
        }
    }
}

/* Read Noise message 1, pick the PSK it names, answer with message 2.  `initial`: cleartext exchange, Sentinel fallback
 * allowed, and the dialect is decided.  Re-handshake: both messages travel as encrypted JSON under the old keys,
 * prologue = previous handshake hash. */
static int noise_respond(struct session *s, const uint8_t *prologue, size_t pl, const char *data_b64, int initial, char *out_b64, struct noise_cs *tx, struct noise_cs *rx)
{
    uint8_t msg[512], payload[512], psk[32]; char id[64] = "", declared[8]; struct noise_hs hs;
    long ml = b64_decode(data_b64, strlen(data_b64), msg, sizeof msg);
    noise_kk_responder_init(&hs, prologue, pl, id_priv, id_pub, s->server_pub);
    long pll = ml < 0 ? -1 : noise_kk_read_msg1(&hs, msg, ml, payload);
    if (pll < 0) { fprintf(stderr, "sendspin: handshake message 1 does not authenticate\n"); return -1; }
    payload[pll] = 0; js_str((char *)payload, "psk_id", id, sizeof id);
    int has_cat = js_str((char *)payload, "psk_category", declared, sizeof declared);
    if (initial) s->spec = has_cat;
    else if (has_cat != s->spec) return -1;                      /* a server does not change dialect mid-connection */
    int cat = psk_select(id, has_cat ? declared : NULL, s->server_id, psk);
    if (cat == -2) { fprintf(stderr, "sendspin: handshake message 1 names a key we hold for another server, or a key category we do not know\n"); return -1; }
    if (cat < 0) {
        if (!initial) return -1;
        fprintf(stderr, "sendspin: server refers to a key we do not have (remove this player's pairing in the server); trying the Sentinel key\n");
        memcpy(psk, sentinel_psk, 32); cat = PSK_SENTINEL;
    }
    s->psk_cat = cat; s->pairing_index = 0;
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
    s->activated = 0; set_quiet(s, 1);                  /* nothing else is sent until the next server/activate */
    if (noise_respond(s, prev, 32, data, 0, out_b64, &tx, &rx)) return -1;
    if (send_json_forced(s, &tx, "{\"type\":\"noise/handshake\",\"payload\":{\"data\":\"%s\"}}", out_b64)) return -1;  /* still under the old keys */
    s->rx = rx;
    fprintf(stderr, "sendspin: re-handshake done (%s)\n", s->psk_cat == PSK_LONGTERM ? "paired" : s->psk_cat == PSK_PAIRING ? "pairing key" : "sentinel");
    return 0;
}

/* One whole message, type byte first, with room for a NUL after it.  0: a protocol error, close. */
static int on_message(struct session *s, uint8_t *m, size_t len, long long t_recv)
{
    if (m[0] == 0) { m[len] = 0; on_json(s, (char *)m + 1, t_recv); }
    else if (m[0] == 4) {                       /* audio: int64 big-endian server time, (1.0) uint32 send_ahead, frames */
        size_t hl = s->spec ? 13 : 9;
        if (len < hl) return !s->spec;          /* 1.0: shorter than its header is an error */
        if (len == hl || !s->admitted || !s->player_active) return 1;
        long long ts = 0; for (int i = 0; i < 8; i++) ts = ts << 8 | m[1 + i];
        const uint8_t *pcm; size_t n = atomic_load(&stream_on) ? decode_chunk(m + hl, len - hl, &pcm) : 0;
        if (n) q_push(ts, pcm, n);
    } else if (m[0] == 8 && s->admitted) {      /* album art, channel 0 */
        if (s->spec) on_artwork(s, m, len);
        else if (len >= 9) screen_music_art(len > 9 ? m + 9 : NULL, len - 9);    /* 9.1.1: int64 time, the image (none: cleared) */
    }
    return 1;                                   /* other ids: roles we do not have, ignored */
}


/* The protocol's own fragmentation of a message beyond one Noise message (65518 bytes).  1.0: [1][flags][type][data],
 * then [1][flags][data]; flags bit 1 = first, bit 0 = last, the rest zero.  9.1.1: [2][type][data], [2][data] ...,
 * the last one [3][data].  0: a broken sequence, close (both). */
static int on_fragment(struct session *s, const uint8_t *p, size_t n, long long t_recv)
{
    int first, last; size_t off;
    if (s->spec) {
        if (n < 2 || (p[1] & ~3)) return 0;
        first = p[1] & 2; last = p[1] & 1; off = 2;
    } else { first = !s->frag_on && p[0] == 2; last = p[0] == 3; off = 1; }
    if (!!first == s->frag_on) return 0;        /* a first one while one is open, or a later one with none */
    if (first) {
        if (n < off + 1 || p[off] == 1 || (!s->spec && p[off] >= 2 && p[off] <= 3)) return 0;
        s->frag_len = 0; s->frag_on = 1;
    }
    if (s->frag_len + (n - off) > FRAG_MAX) return 0;
    if (!s->frag && !(s->frag = malloc(FRAG_MAX + 1))) return 0;
    memcpy(s->frag + s->frag_len, p + off, n - off); s->frag_len += n - off;
    if (!last) return 1;
    s->frag_on = 0;
    return on_message(s, s->frag, s->frag_len, t_recv);
}

static void *serve(void *arg)
{
    struct session *s = calloc(1, sizeof *s); uint8_t *plain = malloc(70000); char path[128]; int op, fd = (int)(long)arg; uint8_t *d; size_t n; pthread_t tt;
    struct timeval tv = { 75, 0 }, stv = { 5, 0 };      /* the server pings every 30 s: silence means it is gone; one */
    if (!s || !plain) { close(fd); free(s); free(plain); return NULL; }   /* that takes nothing for 5 s is gone too (the
                                                                   screen's commands, the state, wait on its sends) */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
    pthread_mutex_init(&s->send_lock, NULL); pthread_mutex_init(&s->art_lock, NULL); pthread_mutex_init(&s->meta_lock, NULL);
    if (ws_accept(&s->ws, fd, path, sizeof path)) { close(fd); free(s); free(plain); return NULL; }
    if (handshake(s)) { fprintf(stderr, "sendspin: handshake failed\n"); ws_close(&s->ws); free(s); free(plain); return NULL; }
    fprintf(stderr, "sendspin: encrypted session up (%s, %s)\n", s->psk_cat == PSK_LONGTERM ? "paired" : s->psk_cat == PSK_PAIRING ? "pairing key" : "sentinel key",
            s->spec ? "spec 1.0" : "aiosendspin 9 dialect");
    pthread_create(&tt, NULL, time_thread, s);
    pthread_mutex_lock(&adm_lock);
    for (int i = 0; i < 16; i++) if (!live[i]) { live[i] = s; break; }
    pthread_mutex_unlock(&adm_lock);

    while (!s->closing && ws_recv(&s->ws, &op, &d, &n) == 1) {
        if (op != WS_BINARY) break;                                     /* text after the handshake is a protocol error */
        long long t_recv = raw_us();
        long pl = n <= 70000 - 1 ? noise_decrypt(&s->rx, d, n, plain) : -1;
        if (pl < 1) { fprintf(stderr, "sendspin: message does not authenticate, closing\n"); break; }
        int frag = s->spec ? plain[0] == 1 : plain[0] == 2 || plain[0] == 3;
        if (frag ? !on_fragment(s, plain, pl, t_recv) : s->frag_on || !on_message(s, plain, pl, t_recv)) {
            fprintf(stderr, "sendspin: malformed binary message, closing\n"); break;
        }
    }
    s->closing = 1;
    pthread_join(tt, NULL);
    pthread_mutex_lock(&adm_lock);
    int was = admitted == s;
    if (was) { admitted = NULL; stream_set(0); q_flush(); }
    for (int i = 0; i < 16; i++) if (live[i] == s) live[i] = NULL;
    pthread_mutex_unlock(&adm_lock);
    if (was) screen_music_gone();
    ws_close(&s->ws); pthread_mutex_destroy(&s->send_lock); pthread_mutex_destroy(&s->art_lock); pthread_mutex_destroy(&s->meta_lock);
    fprintf(stderr, "sendspin: session closed\n");
    free(plain); free(s->frag); free(s->art_in); free(s->due); free(s->meta_due); free(s);
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
    if (admitted && admitted->activated && admitted->player_active && admitted->state_sent) send_state_vol(admitted, percent);
    pthread_mutex_unlock(&adm_lock);
}

/* The controller's pause while the group plays (stop where the server offers no pause).  adm_lock held; 1 = sent */
static int send_pause(struct session *s)
{
    if (!s || !s->activated || !s->controller_active || !s->group_playing || !atomic_load(&stream_on)) return 0;
    const char *cmd = strstr(s->ctl_commands, "\"pause\"") ? "pause" : strstr(s->ctl_commands, "\"stop\"") ? "stop" : NULL;
    if (!cmd) return 0;
    send_json(s, "{\"type\":\"client/command\",\"payload\":{\"controller\":{\"command\":\"%s\"}}}", cmd);
    return 1;
}

/* Action button: pause while the group plays; resume if it was this button (or "stop") that paused it.  1 = consumed. */
int sendspin_button(void)
{
    int used = 0;
    pthread_mutex_lock(&adm_lock);
    struct session *s = admitted;
    if (s && s->activated && s->controller_active) {      /* not during a re-handshake: only its own messages then */
        if (s->group_playing && atomic_load(&stream_on)) {
            if (send_pause(s)) { paused_by_button_at = raw_us(); used = 1; }
        } else if (paused_by_button_at && raw_us() - paused_by_button_at < 1800LL * 1000000 && strstr(s->ctl_commands, "\"play\"")) {
            send_json(s, "{\"type\":\"client/command\",\"payload\":{\"controller\":{\"command\":\"play\"}}}"); paused_by_button_at = 0; used = 1;
        }
    }
    pthread_mutex_unlock(&adm_lock);
    if (used) fprintf(stderr, "sendspin: button -> %s\n", paused_by_button_at ? "pause" : "play");
    return used;
}

/* the action button pressed twice or three times: next / previous track of what plays */
int sendspin_skip(int dir)
{
    int used = 0;
    pthread_mutex_lock(&adm_lock);
    struct session *s = admitted;
    if (s && s->activated && s->controller_active && s->group_playing && atomic_load(&stream_on) && strstr(s->ctl_commands, dir > 0 ? "\"next\"" : "\"previous\""))
        used = send_json(s, "{\"type\":\"client/command\",\"payload\":{\"controller\":{\"command\":\"%s\"}}}", dir > 0 ? "next" : "previous") == 0;
    pthread_mutex_unlock(&adm_lock);
    if (used) fprintf(stderr, "sendspin: button -> %s\n", dir > 0 ? "next" : "previous");
    return used;
}

/* "<wake word>, stop": pause as the button does, and the button resumes it */
int sendspin_stop(void)
{
    pthread_mutex_lock(&adm_lock);
    int sent = send_pause(admitted);
    if (sent) paused_by_button_at = raw_us();
    pthread_mutex_unlock(&adm_lock);
    if (sent) fprintf(stderr, "sendspin: paused, told to stop\n");
    return sent;
}

void sendspin_pause(void)
{
    pthread_mutex_lock(&adm_lock);
    struct session *s = admitted;
    int sent = send_pause(s);
    if (s && s->controller_active && s->group_playing && atomic_load(&stream_on))
        paused_by_button_at = 0;                        /* the button does not resume what another source paused or
                                                           took over, even where the server offers no pause */
    pthread_mutex_unlock(&adm_lock);
    if (sent) fprintf(stderr, "sendspin: paused, another source took over\n");
}

/* The screen's music view: a controller command for the group, if the server allows it.  0 sent */
int sendspin_control(const char *cmd, long long arg)
{
    char q[40]; int rc = -1;
    snprintf(q, sizeof q, "\"%s\"", cmd);
    pthread_mutex_lock(&adm_lock);
    struct session *s = admitted;
    if (s && s->controller_active && strstr(s->ctl_commands, q)) {
        if (!strcmp(cmd, "volume") && arg >= 0 && arg <= 100) rc = send_json(s, "{\"type\":\"client/command\",\"payload\":{\"controller\":{\"command\":\"volume\",\"volume\":%d}}}", (int)arg);
        else if (!strcmp(cmd, "mute") && arg >= 0) rc = send_json(s, "{\"type\":\"client/command\",\"payload\":{\"controller\":{\"command\":\"mute\",\"mute\":%s}}}", arg ? "true" : "false");
        else if (!strcmp(cmd, "seek") && arg >= 0)               /* the screen's progress bar: position_ms, nothing else;
                                                                    within seek_max_ms (past it the server drops it) */
            rc = send_json(s, "{\"type\":\"client/command\",\"payload\":{\"controller\":{\"command\":\"seek\",\"position_ms\":%lld}}}",
                           s->seek_max > 0 && arg > s->seek_max ? s->seek_max : arg);
        else if (!strcmp(cmd, "play") || !strcmp(cmd, "pause") || !strcmp(cmd, "next") || !strcmp(cmd, "previous") || !strcmp(cmd, "stop") ||
                 !strcmp(cmd, "repeat_off") || !strcmp(cmd, "repeat_one") || !strcmp(cmd, "repeat_all") || !strcmp(cmd, "shuffle") || !strcmp(cmd, "unshuffle"))
            rc = send_json(s, "{\"type\":\"client/command\",\"payload\":{\"controller\":{\"command\":\"%s\"}}}", cmd);
        if (!strcmp(cmd, "pause") || !strcmp(cmd, "play")) paused_by_button_at = 0;
    }
    pthread_mutex_unlock(&adm_lock);
    return rc ? -1 : 0;
}

const char *sendspin_client_id(void) { return client_id; }

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
    if (r > 0) atomic_store(&server_known, 1);
    r = load_file("sendspin.unpaired", buf, sizeof buf - 1); atomic_store(&unpaired_ok, r > 0 && buf[0] == '1');
    r = load_file("sendspin.delay", buf, sizeof buf - 1); buf[r > 0 ? r : 0] = 0;
    if (r > 0 && atoi(buf) >= 0 && atoi(buf) <= 5000) atomic_store(&static_delay_ms, atoi(buf));
}

void sendspin_mdns(int p, char *out, size_t outsz)
{
    snprintf(out, outsz, "  <service><type>_sendspin._tcp</type><port>%d</port><txt-record>path=/sendspin</txt-record>"
                         "<txt-record>name=%s</txt-record></service>\n", p, core_name);
}

void sendspin_init(void) { load_identity(); }

/* netaddr.c: while a server that played here is not connected, root announces the mDNS records again (issue #28).  A
 * server the operator removed for good keeps that going, at most every 30 min. */
int sendspin_linked(void) { pthread_mutex_lock(&adm_lock); int up = admitted != NULL; pthread_mutex_unlock(&adm_lock); return up; }
int sendspin_expected(void) { return atomic_load(&server_known); }

int sendspin_unpaired(int set)
{
    if (set < 0) return atomic_load(&unpaired_ok);
    int was = atomic_exchange(&unpaired_ok, set != 0);
    save_file("sendspin.unpaired", set ? "1" : "0", 1);
    fprintf(stderr, "sendspin: unpaired access %s\n", set ? "allowed" : "off, paired servers only");
    /* A server that connected and waits (Music Assistant: "no role yet", admitted without one) read the old answer in our
     * hello and does not ask again: what is unpaired and has no player role is closed, the server dials back and reads the
     * new one.  Only on a change: an import sets every setting, the same value too */
    pthread_mutex_lock(&adm_lock);
    for (int i = 0; was != (set != 0) && i < 16; i++) {
        struct session *l = live[i];
        if (l && l->psk_cat != PSK_LONGTERM && !l->player_active && !l->pairing && !l->closing) { l->closing = 1; shutdown(l->ws.fd, SHUT_RDWR); }
    }
    pthread_mutex_unlock(&adm_lock);
    if (!set) {                         /* an unpaired server playing now goes; it has to pair to come back */
        pthread_mutex_lock(&adm_lock);
        struct session *s = admitted; int gone = 0;
        if (s && s->psk_cat != PSK_LONGTERM && !s->pairing && !s->closing) { s->admitted = 0; admitted = NULL; stream_set(0); q_flush(); goodbye(s, "pairing_required"); gone = 1; }
        pthread_mutex_unlock(&adm_lock);
        if (gone) screen_music_gone();
    }
    return set != 0;
}

int sendspin_start(int p)
{
    pthread_t t;
    port = p;
    load_identity();
    pthread_create(&t, NULL, player_thread, NULL); pthread_detach(t);
    return pthread_create(&t, NULL, listen_thread, NULL) ? -1 : (pthread_detach(t), 0);
}
