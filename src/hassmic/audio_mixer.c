/* Device backend: capture micAsr and play on the TTS stream through libmixerAPI, so the AEC reference stays intact. */
#include "audio.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mixer_api.h"

static MixerHandle cap, play;
static int cap_held;

int cap_open(void)
{
    cap = MixerOpenRec(MIXER_REC_ASR);
    if (!cap) return -1;
    fprintf(stderr, "audio: micAsr rate=%u ch=%u bits=%u\n", MixerGetRate(cap), MixerGetNumCh(cap), MixerGetSampleSizeBits(cap));
    return 0;
}

int cap_read(const void **data)
{
    int status = 0; unsigned n = 0;
    if (cap_held) { MixerReleaseBufRec(cap); cap_held = 0; }
    void *p = MixerGetBufRec(cap, &status, &n);
    if (!p) return 0;
    cap_held = 1; *data = p;
    return n;
}

void cap_close(void)
{
    if (cap_held) MixerReleaseBufRec(cap);
    if (cap) MixerClose(cap);
    cap = NULL; cap_held = 0;
}

int play_open(unsigned rate, unsigned channels)
{
    play = MixerOpenPlay(rate, channels, 16, MIXER_PLAY_TTS);
    return play ? 0 : -1;
}

int play_write(const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        int status = 0; unsigned capb = 0;
        char *buf = MixerGetBufPlay(play, &status, &capb);
        if (!buf) return -1;
        unsigned n = len < capb ? len : capb;
        memcpy(buf, p, n);
        MixerReleaseBufPlay(play, n);
        p += n; len -= n;
    }
    return 0;
}

void play_close(int drain)
{
    if (!play) return;
    if (drain) MixerDrain(play); else MixerFlush(play);
    MixerClose(play);
    play = NULL;
}

/* Music stream.  The mixer's buffer fill is not exposed in a documented way, so the queue is modelled: bytes written
 * minus bytes that real time has consumed since the stream started draining (it drains at exactly the sample rate, and
 * MixerGetBufPlay blocks once its ring is full, which keeps the model honest). */
static MixerHandle music; static unsigned music_bps; static long long music_t0, music_written;

static long long raw_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int music_open(unsigned rate, unsigned channels)
{
    music = MixerOpenPlay(rate, channels, 16, MIXER_PLAY_MUSIC);
    music_bps = rate * channels * 2; music_t0 = 0; music_written = 0;
    return music ? 0 : -1;
}

long long music_queued_us(void)
{
    if (!music || !music_t0) return 0;
    long long q = music_written * 1000000 / music_bps - (raw_us() - music_t0);
    if (q < 0) { music_t0 = 0; music_written = 0; q = 0; }      /* ran dry: the model restarts with the next write */
    return q;
}

int music_write(const void *data, size_t len)
{
    const char *p = data;
    if (!music) return -1;
    music_queued_us();
    if (!music_t0) music_t0 = raw_us();
    while (len) {
        int status = 0; unsigned capb = 0;
        char *buf = MixerGetBufPlay(music, &status, &capb);
        if (!buf) return -1;
        unsigned n = len < capb ? len : capb;
        memcpy(buf, p, n);
        MixerReleaseBufPlay(music, n);
        p += n; len -= n; music_written += n;
    }
    return 0;
}

void music_close(void)
{
    if (!music) return;
    MixerFlush(music); MixerClose(music);
    music = NULL;
}

void play_earcon(const short *pcm, size_t samples, unsigned rate)
{
    MixerHandle h = MixerOpenPlay(rate, 1, 16, MIXER_PLAY_EARCON);
    const char *p = (const char *)pcm; size_t len = samples * 2;
    if (!h) return;
    while (len) {
        int status = 0; unsigned capb = 0;
        char *buf = MixerGetBufPlay(h, &status, &capb);
        if (!buf) break;
        unsigned n = len < capb ? len : capb;
        memcpy(buf, p, n);
        MixerReleaseBufPlay(h, n);
        p += n; len -= n;
    }
    MixerDrain(h);
    MixerClose(h);
}
