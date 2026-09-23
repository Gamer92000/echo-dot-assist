/* PC backend for protocol tests: capture loops $HASSMIC_CAP (raw 16 kHz mono s16le, silence if unset) at real-time
 * speed; playback is appended to $HASSMIC_PLAY (default /tmp/hassmic_play.raw). */
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *cap, *play;
static short block[800];
static unsigned play_rate, play_ch;

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

int play_write(const void *data, size_t len)
{
    fwrite(data, 1, len, play);
    usleep((useconds_t)(len * 1e6 / (2.0 * play_ch * play_rate)));
    return 0;
}

void play_close(int drain) { (void)drain; if (play) fclose(play); play = NULL; }

void play_earcon(const short *pcm, size_t samples, unsigned rate)
{
    (void)pcm;
    fprintf(stderr, "earcon: %zu samples @ %u Hz\n", samples, rate);
}

/* Music streams on the PC: files, written at real-time speed through a modelled 200 ms device buffer. */
struct stream { FILE *f; unsigned bps; long long t0, written; };
static struct stream music, bt;
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

static int s_write(struct stream *s, const void *data, size_t len)
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
