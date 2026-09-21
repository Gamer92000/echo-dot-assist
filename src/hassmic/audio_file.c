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

/* Music stream on the PC: a file, written at real-time speed through a modelled 200 ms device buffer. */
static FILE *music; static unsigned music_bps; static long long music_t0, music_written;
static long long raw_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts); return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }

int music_open(unsigned rate, unsigned channels)
{
    const char *p = getenv("HASSMIC_MUSIC");
    music = fopen(p ? p : "/tmp/hassmic_music.raw", "ab");
    music_bps = rate * channels * 2; music_t0 = 0; music_written = 0;
    return music ? 0 : -1;
}

long long music_queued_us(void)
{
    if (!music || !music_t0) return 0;
    long long q = music_written * 1000000 / music_bps - (raw_us() - music_t0);
    if (q < 0) { music_t0 = 0; music_written = 0; q = 0; }
    return q;
}

int music_write(const void *data, size_t len)
{
    if (!music) return -1;
    music_queued_us();
    if (!music_t0) music_t0 = raw_us();
    fwrite(data, 1, len, music); fflush(music); music_written += len;
    while (music_queued_us() > 200000) usleep(2000);
    return 0;
}

void music_close(void) { if (music) fclose(music); music = NULL; }
