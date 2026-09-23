/* Device backend: capture micAsr and play on the TTS stream through libmixerAPI, so the AEC reference stays intact. */
#include "audio.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
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

/* Music streams (Sendspin, Bluetooth speaker).  The mixer's buffer fill is not exposed in a documented way, so the queue
 * is modelled: bytes written minus bytes that real time has consumed since the stream started draining (it drains at
 * exactly the sample rate, and MixerGetBufPlay blocks once its ring is full, which keeps the model honest). */
struct stream { MixerHandle h; unsigned bps; long long t0, written; };
static struct stream music, bt;

static long long raw_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int s_open(struct stream *s, unsigned rate, unsigned channels)
{
    s->h = MixerOpenPlay(rate, channels, 16, MIXER_PLAY_MUSIC);
    s->bps = rate * channels * 2; s->t0 = 0; s->written = 0;
    return s->h ? 0 : -1;
}

static long long s_queued_us(struct stream *s)
{
    if (!s->h || !s->t0) return 0;
    long long q = s->written * 1000000 / s->bps - (raw_us() - s->t0);
    if (q < 0) { s->t0 = 0; s->written = 0; q = 0; }            /* ran dry: the model restarts with the next write */
    return q;
}

static int s_write(struct stream *s, const void *data, size_t len)
{
    const char *p = data;
    if (!s->h) return -1;
    s_queued_us(s);
    if (!s->t0) s->t0 = raw_us();
    while (len) {
        int status = 0; unsigned capb = 0;
        char *buf = MixerGetBufPlay(s->h, &status, &capb);
        if (!buf) return -1;
        unsigned n = len < capb ? len : capb;
        memcpy(buf, p, n);
        MixerReleaseBufPlay(s->h, n);
        p += n; len -= n; s->written += n;
    }
    return 0;
}

static void s_close(struct stream *s)
{
    if (!s->h) return;
    MixerFlush(s->h); MixerClose(s->h);
    s->h = NULL;
}

int       music_open(unsigned rate, unsigned channels) { return s_open(&music, rate, channels); }
long long music_queued_us(void) { return s_queued_us(&music); }
int       music_write(const void *data, size_t len) { return s_write(&music, data, len); }
void      music_close(void) { s_close(&music); }
int       bt_open(unsigned rate, unsigned channels) { return s_open(&bt, rate, channels); }
long long bt_queued_us(void) { return s_queued_us(&bt); }
int       bt_write(const void *data, size_t len) { return s_write(&bt, data, len); }
void      bt_close(void) { s_close(&bt); }

void play_earcon(const short *pcm, size_t samples, unsigned rate)
{
    /* MixerDrain returns at once, it does not wait for the queue.  Closing right after it unlinks the ring while the mixer's
     * discovery thread (inotify on /data/mixer_streams) may not have opened it yet: the mixer then logs shmOpenFailed and the
     * sound is lost (seen on ~40 % of button sounds).  So wait for the mixer to start consuming, then for the queue to run
     * out, and only then close.  A stream the mixer never picks up is closed and opened again. */
    long duration_ms = (long)(samples * 1000ULL / (rate ? rate : 48000));
    for (int attempt = 0; attempt < 4; attempt++) {
        MixerHandle h = NULL; const char *p = (const char *)pcm; size_t len = samples * 2; int tries; long t = 0;
        for (tries = 0; tries < 25 && !(h = MixerOpenPlay(rate, 1, 16, MIXER_PLAY_EARCON)); tries++) usleep(20000);
        if (!h) { fprintf(stderr, "earcon: MixerOpenPlay failed\n"); return; }
        while (len) {
            int status = 0; unsigned capb = 0;
            char *buf = MixerGetBufPlay(h, &status, &capb);
            if (!buf) { fprintf(stderr, "earcon: MixerGetBufPlay failed, status %d\n", status); MixerClose(h); return; }
            unsigned n = len < capb ? len : capb;
            memcpy(buf, p, n);
            MixerReleaseBufPlay(h, n);
            p += n; len -= n;
        }
        unsigned q0 = MixerGetNumBytes(h);
        while (t < 400 && MixerGetNumBytes(h) >= q0) { usleep(20000); t += 20; }
        if (MixerGetNumBytes(h) >= q0) {
            fprintf(stderr, "earcon: the mixer did not pick up the stream, opening again\n");
            MixerClose(h); usleep(50000);
            continue;
        }
        while (t < duration_ms + 1000 && MixerGetNumBytes(h) > 0) { usleep(20000); t += 20; }
        MixerClose(h);
        return;
    }
    fprintf(stderr, "earcon: giving up\n");
}
