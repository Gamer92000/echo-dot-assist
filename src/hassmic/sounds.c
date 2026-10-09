/* Stock UI sounds: WAV (48 kHz s16, mono or stereo) or MP3 (decoded with the vendored minimp3, whose implementation lives
 * in proto_esphome.c), mixed down to mono for the Earcon stream.  Loaded on first use, kept for the life of the process.
 * From board.earcon_dir first; where the firmware keeps them inside its apps instead (checkers), from board.earcon_zip:
 * the entry read straight out of the APK, stored uncompressed as aapt leaves audio, so no copy is made anywhere. */
#include "sounds.h"
#include "board.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/minimp3.h"

static const char *const names[SND_COUNT] = { "ui_wakesound", "ui_wakesound_touch", "state_privacy_mode_on", "state_privacy_mode_off",
                                              "state_volume_adjust_tone", "state_bluetooth_connected", "state_bluetooth_disconnected",
                                              "state_setup_discovery_beacon",       /* stock's "here I am" while in setup */
                                              "comms_drop_in_incoming", "comms_call_connected", "comms_call_disconnected",
                                              "comms_call_incoming_ringtone", "comms_outbound_ringtone" };    /* Drop In (dropin.c) */
static struct { short *pcm; size_t n; unsigned rate; int tried; } cache[SND_COUNT];

#define SOUND_MAX (4u << 20)                /* the longest stock sound (a ringtone) is ~180 kB */

static short *to_mono(const short *in, size_t frames, unsigned ch)
{
    short *out = malloc(frames * sizeof *out);
    if (!out) return NULL;
    for (size_t i = 0; i < frames; i++) { int s = 0; for (unsigned c = 0; c < ch; c++) s += in[i * ch + c]; out[i] = (short)(s / (int)ch); }
    return out;
}

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned le16(const unsigned char *p) { return p[0] | p[1] << 8; }

static short *wav_decode(const unsigned char *in, size_t len, size_t *samples, unsigned *rate)
{
    unsigned ch = 0, bits = 0;
    if (len < 12 || memcmp(in, "RIFF", 4) || memcmp(in + 8, "WAVE", 4)) return NULL;
    for (size_t pos = 12; pos + 8 <= len;) {
        uint32_t n = le32(in + pos + 4);
        const unsigned char *body = in + pos + 8;
        if (n > len - pos - 8) return NULL;
        if (!memcmp(in + pos, "fmt ", 4) && n >= 16) {
            ch = le16(body + 2); *rate = le32(body + 4); bits = le16(body + 14);
        } else if (!memcmp(in + pos, "data", 4) && ch && bits == 16) {
            size_t frames = n / (2 * ch);
            short *raw = malloc(frames * ch * sizeof *raw), *out;
            if (!raw) return NULL;
            memcpy(raw, body, frames * ch * sizeof *raw);              /* little-endian on every Echo and PC */
            out = to_mono(raw, frames, ch);
            free(raw);
            *samples = out ? frames : 0;
            return out;
        }
        pos += 8 + n + (n & 1);
    }
    return NULL;
}

static short *mp3_decode(const unsigned char *in, size_t len, size_t *samples, unsigned *rate)
{
    short *out = NULL; size_t cap = 0, n = 0; unsigned ch = 0;
    static mp3dec_t dec; mp3dec_frame_info_t info; mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_init(&dec);
    for (size_t pos = 0; pos < len;) {
        int got = mp3dec_decode_frame(&dec, in + pos, (int)(len - pos), pcm, &info);
        if (!info.frame_bytes) break;
        pos += info.frame_bytes;
        if (!got) continue;
        if (!ch) { ch = info.channels; *rate = info.hz; }
        if (n + (size_t)got * ch > cap) { cap = (cap + got * ch) * 2; short *p = realloc(out, cap * sizeof *p); if (!p) { free(out); out = NULL; break; } out = p; }
        memcpy(out + n, pcm, (size_t)got * ch * sizeof *pcm); n += (size_t)got * ch;
    }
    if (!out || !ch) { free(out); return NULL; }
    if (ch > 1) { short *m = to_mono(out, n / ch, ch); free(out); out = m; n /= ch; }
    *samples = out ? n : 0;
    return out;
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); unsigned char *buf; long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || (unsigned long)n > SOUND_MAX || !(buf = malloc(n))) { fclose(f); return NULL; }
    if (fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* One entry of a zip (an APK), only if stored uncompressed: the end record in the last 64 kB, the central directory
 * for the name, the local header for where the data starts.  Every offset and size checked against the file. */
static unsigned char *zip_entry(const char *path, const char *name, size_t *len)
{
    FILE *f = fopen(path, "rb"); unsigned char tail[65536 + 22], *cd = NULL, *out = NULL, lh[30];
    long size, from; size_t got, nl = strlen(name); uint32_t cdsize, cdoff;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 22) goto done;
    from = size > (long)sizeof tail ? size - (long)sizeof tail : 0;
    fseek(f, from, SEEK_SET);
    got = fread(tail, 1, (size_t)(size - from), f);
    for (long i = (long)got - 22; ; i--) {
        if (i < 0) goto done;
        if (!memcmp(tail + i, "PK\5\6", 4)) { cdsize = le32(tail + i + 12); cdoff = le32(tail + i + 16); break; }
    }
    if (cdoff > (uint64_t)size || cdsize > (uint64_t)size - cdoff || cdsize > 16u << 20 || !(cd = malloc(cdsize))) goto done;
    if (fseek(f, cdoff, SEEK_SET) || fread(cd, 1, cdsize, f) != cdsize) goto done;
    for (uint32_t p = 0; p + 46 <= cdsize;) {
        unsigned fl = le16(cd + p + 28), xl = le16(cd + p + 30), cl = le16(cd + p + 32);
        if (memcmp(cd + p, "PK\1\2", 4) || p + 46 + fl + xl + cl > cdsize) goto done;
        if (fl == nl && !memcmp(cd + p + 46, name, nl)) {
            uint32_t csize = le32(cd + p + 20), usize = le32(cd + p + 24), lo = le32(cd + p + 42);
            if (le16(cd + p + 10) != 0 || csize != usize || usize == 0 || usize > SOUND_MAX) goto done;     /* stored only */
            if (lo > (uint64_t)size - 30 || fseek(f, lo, SEEK_SET) || fread(lh, 1, 30, f) != 30 || memcmp(lh, "PK\3\4", 4)) goto done;
            uint64_t data = (uint64_t)lo + 30 + le16(lh + 26) + le16(lh + 28);
            if (data + usize > (uint64_t)size || !(out = malloc(usize))) goto done;
            if (fseek(f, (long)data, SEEK_SET) || fread(out, 1, usize, f) != usize) { free(out); out = NULL; goto done; }
            *len = usize;
            goto done;
        }
        p += 46 + fl + xl + cl;
    }
done:
    free(cd);
    fclose(f);
    return out;
}

static short *decode(const char *what, const unsigned char *buf, size_t len, size_t *samples, unsigned *rate)
{
    size_t l = strlen(what);
    if (l > 4 && !strcmp(what + l - 4, ".wav")) return wav_decode(buf, len, samples, rate);
    return mp3_decode(buf, len, samples, rate);
}

static short *load_file(const char *path, size_t *samples, unsigned *rate)
{
    size_t len; unsigned char *buf = read_file(path, &len); short *pcm;
    if (!buf) return NULL;
    pcm = decode(path, buf, len, samples, rate);
    free(buf);
    return pcm;
}

/* board.earcon_zip's entry for this sound; HASSMIC_EARCON_ROOT prefixes its path (tests: an unpacked firmware) */
static short *load_zip(const char *name, size_t *samples, unsigned *rate)
{
    const char *root = getenv("HASSMIC_EARCON_ROOT");
    for (const struct board_sound *b = board.earcon_zip; b && b->name; b++) {
        if (strcmp(b->name, name)) continue;
        char path[300]; size_t len; unsigned char *buf; short *pcm;
        snprintf(path, sizeof path, "%s%s", root ? root : "", b->zip);
        if (!(buf = zip_entry(path, b->entry, &len))) { fprintf(stderr, "sound %s: no stored %s in %s\n", name, b->entry, path); return NULL; }
        pcm = decode(b->entry, buf, len, samples, rate);
        free(buf);
        return pcm;
    }
    return NULL;
}

int sound_get(enum sound s, const short **pcm, size_t *samples, unsigned *rate)
{
    char path[160];
    if (s < 0 || s >= SND_COUNT) return 0;
    if (!cache[s].tried) {
        cache[s].tried = 1;
        snprintf(path, sizeof path, "%s%s.wav", board.earcon_dir, names[s]);
        cache[s].pcm = load_file(path, &cache[s].n, &cache[s].rate);
        if (!cache[s].pcm) { snprintf(path, sizeof path, "%s%s.mp3", board.earcon_dir, names[s]); cache[s].pcm = load_file(path, &cache[s].n, &cache[s].rate); }
        if (!cache[s].pcm) cache[s].pcm = load_zip(names[s], &cache[s].n, &cache[s].rate);
        if (!cache[s].pcm) fprintf(stderr, "sound %s: not on this image, using the built-in tone\n", names[s]);
    }
    if (!cache[s].pcm) return 0;
    *pcm = cache[s].pcm; *samples = cache[s].n; *rate = cache[s].rate;
    return 1;
}
