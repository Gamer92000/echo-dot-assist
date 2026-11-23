/* PulseAudio backend for Xiaomi L09G. Uses the system's processed microphone
 * and an independently configured sink, not the stopped Harman server. */
#include "audio.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

typedef struct pa_simple pa_simple;
typedef struct { int format; uint32_t rate; uint8_t channels; } pa_sample_spec;
typedef struct { uint32_t maxlength, tlength, prebuf, minreq, fragsize; } pa_buffer_attr;
extern pa_simple *pa_simple_new(const char *, const char *, int, const char *, const char *, const pa_sample_spec *, const void *, const void *, int *);
extern void pa_simple_free(pa_simple *);
extern int pa_simple_read(pa_simple *, void *, size_t, int *);
extern int pa_simple_write(pa_simple *, const void *, size_t, int *);
extern int pa_simple_drain(pa_simple *, int *);
extern int pa_simple_flush(pa_simple *, int *);
extern uint64_t pa_simple_get_latency(pa_simple *, int *);
extern const char *pa_strerror(int);

enum { PA_SAMPLE_S16LE = 3, PA_STREAM_PLAYBACK = 1, PA_STREAM_RECORD = 2 };
static pa_simple *capture, *speech, *music, *bt, *voip, *callcap;
static int16_t cap_block[800], call_block[800];
static _Atomic float tts_gain = 1, music_gain = 1, call_gain = 1;
static _Atomic int volume_main = 40, volume_tts = 100, muted;
void (*audio_earcon_started)(unsigned lead_ms);

static pa_simple *stream(int dir, const char *name, unsigned rate, unsigned channels)
{
    pa_sample_spec spec = { PA_SAMPLE_S16LE, rate, (uint8_t)channels };
    int err = 0;
    const char *device = dir == PA_STREAM_RECORD ? "speex" : "l09g_direct";
    /* For calls, bound PulseAudio's buffer: pa_simple_write then paces the 20 ms
     * frames. Its reported latency includes the hardware sink and can stay above
     * the Echo playout loop's 40 ms threshold even when no samples are queued. */
    pa_buffer_attr call_attr = { UINT32_MAX, rate * channels * 2 / 10, 0, rate * channels * 2 / 50, UINT32_MAX };
    const pa_buffer_attr *attr = dir == PA_STREAM_PLAYBACK && !strcmp(name, "call") ? &call_attr : NULL;
    pa_simple *s = pa_simple_new(NULL, "l09g-assist", dir, device, name, &spec, NULL, attr, &err);
    if (!s) fprintf(stderr, "pulse: %s: %s\n", name, pa_strerror(err));
    return s;
}
static int read_block(pa_simple *s, int16_t *block, const void **data)
{
    int err = 0;
    if (!s || pa_simple_read(s, block, 1600, &err) < 0) return -1;
    *data = block;
    return 1600;
}
static int write_pcm(pa_simple *s, const void *data, size_t len, float gain)
{
    const int16_t *in = data;
    int16_t buffer[2048];
    int err = 0;
    if (!s || (len & 1)) return -1;
    gain *= muted ? 0.0f : (float)volume_main / 100.0f;
    while (len) {
        size_t count = len / 2;
        if (count > sizeof buffer / sizeof *buffer) count = sizeof buffer / sizeof *buffer;
        for (size_t i = 0; i < count; i++) {
            float v = in[i] * gain;
            buffer[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        if (pa_simple_write(s, buffer, count * 2, &err) < 0) return -1;
        in += count; len -= count * 2;
    }
    return 0;
}
static void close_stream(pa_simple **s, int drain)
{
    if (!*s) return;
    if (drain) pa_simple_drain(*s, NULL); else pa_simple_flush(*s, NULL);
    pa_simple_free(*s); *s = NULL;
}
static long long latency(pa_simple *s)
{
    uint64_t n = s ? pa_simple_get_latency(s, NULL) : 0;
    return n == UINT64_MAX ? 0 : (long long)n;
}
int cap_open(void) { capture = stream(PA_STREAM_RECORD, "microphone", 16000, 1); return capture ? 0 : -1; }
int cap_read(const void **data) { return read_block(capture, cap_block, data); }
void cap_close(void) { if (capture) pa_simple_free(capture); capture = NULL; }
int play_open(unsigned rate, unsigned channels) { speech = stream(PA_STREAM_PLAYBACK, "assistant", rate, channels); return speech ? 0 : -1; }
int play_write(const void *data, size_t len) { return write_pcm(speech, data, len, tts_gain * volume_tts / 100.0f); }
void play_close(int drain) { close_stream(&speech, drain); }
void audio_tts_gain(float gain) { tts_gain = gain; }
void audio_music_gain(float gain) { music_gain = gain; }
void audio_call_gain(float gain) { call_gain = gain; }
int music_open(unsigned r, unsigned c) { music = stream(PA_STREAM_PLAYBACK, "music", r, c); return music ? 0 : -1; }
int music_write(const void *d, size_t n) { return write_pcm(music, d, n, music_gain); }
long long music_queued_us(void) { return latency(music); }
long long music_prime_us(void) { return 0; }
void music_close(void) { close_stream(&music, 0); }
int bt_open(unsigned r, unsigned c) { bt = stream(PA_STREAM_PLAYBACK, "bluetooth", r, c); return bt ? 0 : -1; }
int bt_write(const void *d, size_t n) { return write_pcm(bt, d, n, music_gain); }
long long bt_queued_us(void) { return latency(bt); }
void bt_close(void) { close_stream(&bt, 0); }
int voip_open(unsigned r, unsigned c) { voip = stream(PA_STREAM_PLAYBACK, "call", r, c); return voip ? 0 : -1; }
int voip_write(const void *d, size_t n) { return write_pcm(voip, d, n, call_gain); }
long long voip_queued_us(void) { return 0; } /* pa_simple_write provides backpressure for the call stream. */
void voip_close(void) { close_stream(&voip, 0); }
/* Xiaomi exposes the same processed microphone for both streams, not Echo's
 * separately amplified call pipeline; use the assistant stream and its gain. */
int callmic_present(void) { return 0; }
int callmic_open(void) { callcap = stream(PA_STREAM_RECORD, "call microphone", 16000, 1); return callcap ? 0 : -1; }
int callmic_read(const void **d) { return read_block(callcap, call_block, d); }
void callmic_close(void) { if (callcap) pa_simple_free(callcap); callcap = NULL; }
int audio_a2dp_route(int on, uint64_t a, const char *n) { (void)on; (void)a; (void)n; return -1; }
int audio_a2dp_lost(uint64_t a) { (void)a; return -1; }
void audio_a2dp_reset(void) {}
int audio_a2dp_aipc(void) { return 0; }
int vol_read(enum vol which, int fallback) { (void)fallback; return which == VOL_MAIN ? volume_main : which == VOL_TTS ? volume_tts : muted; }
void vol_write(enum vol which, int v) { if (v < 0) v = 0; if (v > 100) v = 100; if (which == VOL_MAIN) volume_main = v; else if (which == VOL_TTS) volume_tts = v; else muted = !!v; }
void afe_listening(int on) { (void)on; }
int afe_arbitration(long s, long e, char *json, size_t n) { (void)s; (void)e; if (n) *json = 0; return 0; }
void afe_stream_stopped(void) {}
int afe_eq_get(char *json, size_t n) { if (n) *json = 0; return 0; }
void afe_eq_set(const char *json) { (void)json; }
void play_earcon(const short *pcm, size_t samples, unsigned rate)
{
    pa_simple *s = stream(PA_STREAM_PLAYBACK, "earcon", rate, 1);
    if (!s) return;
    if (audio_earcon_started) audio_earcon_started(0);
    write_pcm(s, pcm, samples * 2, tts_gain); close_stream(&s, 1);
}
void play_earcon_while(const short *pcm, size_t samples, unsigned rate, int (*go)(void))
{
    pa_simple *s = stream(PA_STREAM_PLAYBACK, "earcon", rate, 1);
    if (!s) return;
    if (audio_earcon_started) audio_earcon_started(0);
    for (size_t p = 0; p < samples && go();) {
        size_t n = samples - p > 800 ? 800 : samples - p;
        if (write_pcm(s, pcm + p, n * 2, tts_gain)) break;
        p += n;
    }
    close_stream(&s, 0);
}
