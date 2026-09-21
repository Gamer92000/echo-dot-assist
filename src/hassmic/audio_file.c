/* PC backend for protocol tests: capture loops $HASSMIC_CAP (raw 16 kHz mono s16le, silence if unset) at real-time
 * speed; playback is appended to $HASSMIC_PLAY (default /tmp/hassmic_play.raw). */
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
