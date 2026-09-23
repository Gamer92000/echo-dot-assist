/* Stock UI sounds: WAV (48 kHz s16, mono or stereo) or MP3 (decoded with the vendored minimp3, whose implementation lives
 * in proto_esphome.c), mixed down to mono for the Earcon stream.  Loaded on first use, kept for the life of the process. */
#include "sounds.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/minimp3.h"

#ifndef SOUND_DIR
#define SOUND_DIR "/system/local/share/earcon/base/"      /* overridable for a PC check against the extracted image */
#endif
#define DIR SOUND_DIR
static const char *const names[SND_COUNT] = { "ui_wakesound", "ui_wakesound_touch", "state_privacy_mode_on", "state_privacy_mode_off",
                                              "state_volume_adjust_tone", "state_bluetooth_connected", "state_bluetooth_disconnected" };
static struct { short *pcm; size_t n; unsigned rate; int tried; } cache[SND_COUNT];

static short *to_mono(const short *in, size_t frames, unsigned ch)
{
    short *out = malloc(frames * sizeof *out);
    if (!out) return NULL;
    for (size_t i = 0; i < frames; i++) { int s = 0; for (unsigned c = 0; c < ch; c++) s += in[i * ch + c]; out[i] = (short)(s / (int)ch); }
    return out;
}

static short *load_wav(const char *path, size_t *samples, unsigned *rate)
{
    unsigned char h[8]; unsigned ch = 0, bits = 0; short *out = NULL; FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fread(h, 1, 8, f) != 8 || memcmp(h, "RIFF", 4) || fread(h, 1, 4, f) != 4 || memcmp(h, "WAVE", 4)) { fclose(f); return NULL; }
    while (fread(h, 1, 8, f) == 8) {
        uint32_t len = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
        if (!memcmp(h, "fmt ", 4) && len >= 16) {
            unsigned char fmt[16]; if (fread(fmt, 1, 16, f) != 16) break;
            ch = fmt[2] | fmt[3] << 8; *rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (unsigned)fmt[7] << 24; bits = fmt[14] | fmt[15] << 8;
            fseek(f, len - 16 + (len & 1), SEEK_CUR);
        } else if (!memcmp(h, "data", 4) && ch && bits == 16) {
            size_t frames = len / (2 * ch); short *raw = malloc(len);
            if (raw && fread(raw, 1, len, f) == len) { out = to_mono(raw, frames, ch); *samples = out ? frames : 0; }
            free(raw); break;
        } else fseek(f, len + (len & 1), SEEK_CUR);
    }
    fclose(f);
    return out;
}

static short *load_mp3(const char *path, size_t *samples, unsigned *rate)
{
    FILE *f = fopen(path, "rb"); unsigned char *in; long len; short *out = NULL; size_t cap = 0, n = 0; unsigned ch = 0;
    static mp3dec_t dec; mp3dec_frame_info_t info; mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4 << 20 || !(in = malloc(len))) { fclose(f); return NULL; }
    if (fread(in, 1, len, f) != (size_t)len) { free(in); fclose(f); return NULL; }
    fclose(f);
    mp3dec_init(&dec);
    for (long pos = 0; pos < len;) {
        int got = mp3dec_decode_frame(&dec, in + pos, (int)(len - pos), pcm, &info);
        if (!info.frame_bytes) break;
        pos += info.frame_bytes;
        if (!got) continue;
        if (!ch) { ch = info.channels; *rate = info.hz; }
        if (n + (size_t)got * ch > cap) { cap = (cap + got * ch) * 2; short *p = realloc(out, cap * sizeof *p); if (!p) { free(out); out = NULL; break; } out = p; }
        memcpy(out + n, pcm, (size_t)got * ch * sizeof *pcm); n += (size_t)got * ch;
    }
    free(in);
    if (!out || !ch) return NULL;
    if (ch > 1) { short *m = to_mono(out, n / ch, ch); free(out); out = m; n /= ch; }
    *samples = out ? n : 0;
    return out;
}

int sound_get(enum sound s, const short **pcm, size_t *samples, unsigned *rate)
{
    char path[160];
    if (s < 0 || s >= SND_COUNT) return 0;
    if (!cache[s].tried) {
        cache[s].tried = 1;
        snprintf(path, sizeof path, DIR "%s.wav", names[s]);
        cache[s].pcm = load_wav(path, &cache[s].n, &cache[s].rate);
        if (!cache[s].pcm) { snprintf(path, sizeof path, DIR "%s.mp3", names[s]); cache[s].pcm = load_mp3(path, &cache[s].n, &cache[s].rate); }
        if (!cache[s].pcm) fprintf(stderr, "sound %s: not on this image, using the built-in tone\n", names[s]);
    }
    if (!cache[s].pcm) return 0;
    *pcm = cache[s].pcm; *samples = cache[s].n; *rate = cache[s].rate;
    return 1;
}
