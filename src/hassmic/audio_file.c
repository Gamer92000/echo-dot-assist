/* PC backend for protocol tests: capture loops $HASSMIC_CAP (raw 16 kHz mono s16le, silence if unset) at real-time
 * speed; playback is appended to $HASSMIC_PLAY (default /tmp/hassmic_play.raw).
 * HASSMIC_FAKE_ECHO=<ms>:<dB>: our sounds come back in the capture <ms> after they start, <dB> under their level, with
 * two reflections: what Amazon's echo canceller leaves of them on an Echo (earsup.h). */
#include "audio.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *cap, *play;
static short block[800];
static unsigned play_rate, play_ch;
void (*audio_earcon_started)(void);

#define ECHO_LEN (16000 * 4)
static pthread_mutex_t echo_mu = PTHREAD_MUTEX_INITIALIZER;
static float echo[ECHO_LEN];                /* by capture sample, modulo */
static unsigned long long cap_pos;          /* samples cap_read has handed out */

static void echo_add(const short *pcm, size_t samples, unsigned rate)
{
    const char *e = getenv("HASSMIC_FAKE_ECHO"); int ms; float db;
    if (!e || sscanf(e, "%d:%f", &ms, &db) != 2 || !rate) return;
    float g = powf(10, db / 20) / 32768;
    pthread_mutex_lock(&echo_mu);
    unsigned long long at = cap_pos + (unsigned long long)ms * 16;
    size_t n = (size_t)(samples * 16000ULL / rate);
    for (size_t j = 0; j < n && j < ECHO_LEN / 2; j++) {
        float v = pcm[j * rate / 16000] * g;            /* every rate/16000-th sample: the earcons are tones far under 8 kHz */
        echo[(at + j) % ECHO_LEN] += v;
        echo[(at + j + 48) % ECHO_LEN] += 0.5f * v;       /* reflections at 3 and 10 ms */
        echo[(at + j + 160) % ECHO_LEN] += 0.25f * v;
    }
    pthread_mutex_unlock(&echo_mu);
}

int cap_open(void)
{
    const char *p = getenv("HASSMIC_CAP");
    cap = p ? fopen(p, "rb") : NULL;
    return p && !cap ? -1 : 0;
}

int cap_read(const void **data)
{
    size_t n = 0;
    memset(block, 0, sizeof block);
    if (cap) {
        n = fread(block, 2, 800, cap);
        if (n < 800) rewind(cap);
    }
    usleep(50000);
    pthread_mutex_lock(&echo_mu);
    for (int i = 0; i < 800; i++) {
        float *e = &echo[(cap_pos + i) % ECHO_LEN], v = block[i] + *e * 32768;
        block[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v)); *e = 0;
    }
    cap_pos += 800;
    pthread_mutex_unlock(&echo_mu);
    *data = block;
    return sizeof block;
}

void cap_close(void) { if (cap) fclose(cap); cap = NULL; }

int play_open(unsigned rate, unsigned channels)
{
    const char *p = getenv("HASSMIC_PLAY");
    play = fopen(p ? p : "/tmp/hassmic_play.raw", "ab");
    play_rate = rate; play_ch = channels;
    return play ? 0 : -1;
}

static float tts_gain = 1;                  /* tests: as the mixer would get it, and in the log */
void audio_tts_gain(float g)
{
    if (fabsf(g - tts_gain) > 0.0005f) fprintf(stderr, "audio: tts gain %.3f\n", g);
    tts_gain = g;
}

int play_write(const void *data, size_t len)
{
    if (tts_gain < 0.9995f)
        for (size_t i = 0; i + 1 < len; i += 2) { short v; memcpy(&v, (const char *)data + i, 2); v = (short)lrintf(v * tts_gain); fwrite(&v, 2, 1, play); }
    else fwrite(data, 1, len, play);
    usleep((useconds_t)(len * 1e6 / (2.0 * play_ch * play_rate)));
    return 0;
}

void play_close(int drain) { (void)drain; if (play) fclose(play); play = NULL; }

void play_earcon(const short *pcm, size_t samples, unsigned rate)
{
    int peak = 0;                               /* for the tests: how loud (separate volumes turn our sounds down) */
    for (size_t i = 0; i < samples; i++) if (abs(pcm[i]) > peak) peak = abs(pcm[i]);
    echo_add(pcm, samples, rate);
    if (audio_earcon_started) audio_earcon_started();
    fprintf(stderr, "earcon: %zu samples @ %u Hz, peak %d\n", samples, rate, peak);
}

void play_earcon_while(const short *pcm, size_t samples, unsigned rate, int (*go)(void))
{
    int peak = 0;
    for (size_t i = 0; i < samples; i++) if (abs(pcm[i]) > peak) peak = abs(pcm[i]);
    fprintf(stderr, "earcon: %zu samples @ %u Hz, until stopped, peak %d\n", samples, rate, peak);
    for (long ms = 0; ms < (long)(samples * 1000ULL / (rate ? rate : 48000)) && go(); ms += 20) usleep(20000);
}

/* Music streams on the PC: files, written at real-time speed through a modelled 200 ms device buffer. */
struct stream { FILE *f; unsigned bps; long long t0, written; };
static struct stream music, bt, voip;
static long long raw_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts); return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }

static int s_open(struct stream *s, const char *env, const char *def, unsigned rate, unsigned channels)
{
    const char *p = getenv(env);
    s->f = fopen(p ? p : def, "ab");
    s->bps = rate * channels * 2; s->t0 = 0; s->written = 0;
    return s->f ? 0 : -1;
}

static long long s_queued_us(struct stream *s)
{
    if (!s->f || !s->t0) return 0;
    long long q = s->written * 1000000 / s->bps - (raw_us() - s->t0);
    if (q < 0) { s->t0 = 0; s->written = 0; q = 0; }
    return q;
}

static int s_write_raw(struct stream *s, const void *data, size_t len);
/* tests: the gain into the file as the mixer would get it, and into the log when it changes */
static float music_gain = 1;
void audio_music_gain(float g)
{
    if (fabsf(g - music_gain) > 0.0005f) fprintf(stderr, "audio: music gain %.3f\n", g);
    music_gain = g;
}
static float call_gain = 1;
void audio_call_gain(float g)
{
    if (fabsf(g - call_gain) > 0.0005f) fprintf(stderr, "audio: call gain %.3f\n", g);
    call_gain = g;
}

static int s_write(struct stream *s, const void *data, size_t len)
{
    if (!s->f) return -1;
    short buf[4096]; float gain = s == &voip ? call_gain : music_gain;
    if (gain < 0.9995f) {
        for (size_t off = 0; off < len; ) {
            size_t n = len - off < sizeof buf ? len - off : sizeof buf;
            for (size_t i = 0; i < n / 2; i++) { short v; memcpy(&v, (const char *)data + off + 2 * i, 2); buf[i] = (short)lrintf(v * gain); }
            if (s_write_raw(s, buf, n)) return -1;
            off += n;
        }
        return 0;
    }
    return s_write_raw(s, data, len);
}

static int s_write_raw(struct stream *s, const void *data, size_t len)
{
    if (!s->f) return -1;
    s_queued_us(s);
    if (!s->t0) s->t0 = raw_us();
    fwrite(data, 1, len, s->f); fflush(s->f); s->written += len;
    while (s_queued_us(s) > 200000) usleep(2000);
    return 0;
}

static void s_close(struct stream *s) { if (s->f) fclose(s->f); s->f = NULL; }

int       music_open(unsigned rate, unsigned channels) { return s_open(&music, "HASSMIC_MUSIC", "/tmp/hassmic_music.raw", rate, channels); }
long long music_queued_us(void) { return s_queued_us(&music); }
int       music_write(const void *data, size_t len) { return s_write(&music, data, len); }
void      music_close(void) { s_close(&music); }
int       bt_open(unsigned rate, unsigned channels) { return s_open(&bt, "HASSMIC_BT", "/tmp/hassmic_bt.raw", rate, channels); }
long long bt_queued_us(void) { return s_queued_us(&bt); }
int       bt_write(const void *data, size_t len) { return s_write(&bt, data, len); }
void      bt_close(void) { s_close(&bt); }
int       voip_open(unsigned rate, unsigned channels) { return s_open(&voip, "HASSMIC_VOIP", "/tmp/hassmic_voip.raw", rate, channels); }
long long voip_queued_us(void) { return s_queued_us(&voip); }
int       voip_write(const void *data, size_t len) { return s_write(&voip, data, len); }
void      voip_close(void) { s_close(&voip); }

/* Volumes: in memory; unset = what the caller expects (as a mixer that cannot be asked).  Tests: the mixer's volumes
 * as files in HASSMIC_FAKE_MIXER instead, one per property as audio_mixer.c names them */
static int vols[3] = { -1, -1, -1 };
static const char *const vol_props[] = { "MainVolume", "TTSVolume", "Mute" };

static const char *fake_mixer(enum vol which, char *file, size_t n)
{
    const char *d = getenv("HASSMIC_FAKE_MIXER");
    if (!d) return NULL;
    snprintf(file, n, "%s/%s", d, vol_props[which]);
    return file;
}

int vol_read(enum vol which, int fallback)
{
    char file[300]; int v = fallback, x;
    if (fake_mixer(which, file, sizeof file)) {
        FILE *f = fopen(file, "r");
        if (f) { if (fscanf(f, "%d", &x) == 1 && x >= 0 && x <= 100) v = x; fclose(f); }
        return v;
    }
    return vols[which] < 0 ? fallback : vols[which];
}

void vol_write(enum vol which, int v)
{
    char file[300];
    if (fake_mixer(which, file, sizeof file)) { FILE *f = fopen(file, "w"); if (f) { fprintf(f, "%d\n", v); fclose(f); } return; }
    vols[which] = v;
}

/* No front end on the PC */
void afe_listening(int on) { (void)on; }
void afe_stream_stopped(void) {}
int  afe_arbitration(long ts, long te, char *json, size_t n) { (void)ts; (void)te; if (n) json[0] = 0; return 0; }
