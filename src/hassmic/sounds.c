/* Stock UI sounds: WAV (48 kHz s16, mono or stereo) or MP3 (decoded with the vendored minimp3, whose implementation lives
 * in proto_esphome.c), mixed down to mono for the Earcon stream.  Loaded on first use, kept for the life of the process.
 * From board.earcon_dir first; where the firmware keeps them inside its apps instead (checkers), from board.earcon_zip:
 * the entry read straight out of the APK, stored uncompressed as aapt leaves audio, so no copy is made anywhere.
 * The timer sound of one's own (SND_CUSTOM) is the settings page's upload in state/, a WAV checked here first. */
#include "sounds.h"
#include "board.h"
#include "statefile.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../third_party/minimp3.h"

static const char *const names[SND_COUNT] = { "ui_wakesound", "ui_wakesound_touch", "state_privacy_mode_on", "state_privacy_mode_off",
                                              "state_volume_adjust_tone", "state_bluetooth_connected", "state_bluetooth_disconnected",
                                              "state_setup_discovery_beacon",       /* stock's "here I am" while in setup */
                                              "comms_drop_in_incoming", "comms_call_connected", "comms_call_disconnected",
                                              "comms_call_incoming_ringtone", "comms_outbound_ringtone",      /* Drop In (dropin.c) */
                                              /* PuffinApp's EarconsAudioProvider (libEarconAdapter.so): timerDefault and
                                               * alarmDefault; the Echo Shows' alarms app has the same files */
                                              "system_alerts_melodic_02", "system_alerts_melodic_01",
                                              "timer_sound" };      /* in state/, not with stock's */
static struct { short *pcm; size_t n; unsigned rate; int tried; } cache[SND_COUNT];
static atomic_int custom_gen;               /* sound_custom_save/_delete: the earcon thread reads SND_CUSTOM again */
static int custom_seen;                     /* the earcon thread's */

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
static void custom_path(char *out, size_t cap) { snprintf(out, cap, "%s/%s.wav", state_dir(), names[SND_CUSTOM]); }

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

/* the data chunk of a RIFF WAVE: its format into the rest, its length into *bytes.  NULL if there is none or a chunk
 * runs past the end */
static const unsigned char *wav_data(const unsigned char *in, size_t len, unsigned *fmt, unsigned *ch, unsigned *rate, unsigned *bits, size_t *bytes)
{
    *fmt = *ch = *bits = 0;
    if (len < 12 || memcmp(in, "RIFF", 4) || memcmp(in + 8, "WAVE", 4)) return NULL;
    for (size_t pos = 12; pos + 8 <= len;) {
        uint32_t n = le32(in + pos + 4);
        const unsigned char *body = in + pos + 8;
        if (n > len - pos - 8) return NULL;
        if (!memcmp(in + pos, "fmt ", 4) && n >= 16) {
            *fmt = le16(body); *ch = le16(body + 2); *rate = le32(body + 4); *bits = le16(body + 14);
        } else if (!memcmp(in + pos, "data", 4) && *ch) { *bytes = n; return body; }
        pos += 8 + n + (n & 1);
    }
    return NULL;
}

static short *wav_decode(const unsigned char *in, size_t len, size_t *samples, unsigned *rate)
{
    unsigned fmt, ch, bits; size_t n;
    const unsigned char *body = wav_data(in, len, &fmt, &ch, rate, &bits, &n);
    if (!body || bits != 16) return NULL;
    size_t frames = n / (2 * ch);
    short *raw = malloc(frames * ch * sizeof *raw), *out;
    if (!raw) return NULL;
    memcpy(raw, body, frames * ch * sizeof *raw);              /* little-endian on every Echo and PC */
    out = to_mono(raw, frames, ch);
    free(raw);
    *samples = out ? frames : 0;
    return out;
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

/* Where one entry of a zip (an APK) starts and how long it is, only if stored uncompressed: the end record in the last
 * 64 kB, the central directory for the name, the local header for where the data starts.  Every offset and size
 * checked against the file.  0 if it is there */
static int zip_find(FILE *f, const char *name, uint64_t *at, uint32_t *len)
{
    unsigned char tail[65536 + 22], *cd = NULL, lh[30];
    long size, from; size_t got, nl = strlen(name); uint32_t cdsize, cdoff; int r = -1;
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
            if (data + usize > (uint64_t)size) goto done;
            *at = data; *len = usize; r = 0;
            goto done;
        }
        p += 46 + fl + xl + cl;
    }
done:
    free(cd);
    return r;
}

static unsigned char *zip_entry(const char *path, const char *name, size_t *len)
{
    FILE *f = fopen(path, "rb"); unsigned char *out = NULL; uint64_t at; uint32_t n;
    if (!f) return NULL;
    if (!zip_find(f, name, &at, &n) && (out = malloc(n)) && (fseek(f, (long)at, SEEK_SET) || fread(out, 1, n, f) != n)) { free(out); out = NULL; }
    if (out) *len = n;
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

/* the APK of one of board.earcon_zip's entries; HASSMIC_EARCON_ROOT prefixes its path (tests: an unpacked firmware) */
static void zip_path(const struct board_sound *b, char *path, size_t cap)
{
    const char *root = getenv("HASSMIC_EARCON_ROOT");
    snprintf(path, cap, "%s%s", root ? root : "", b->zip);
    /* an app the install took off the partition (sysinstall.sh, STOCK_APPS_OUT) keeps its APK, and its sounds, in
     * /system/hassmic-removed under the same path below /system */
    if (access(path, R_OK) && !strncmp(b->zip, "/system/", 8)) snprintf(path, cap, "%s/system/hassmic-removed/%s", root ? root : "", b->zip + 8);
}

/* board.earcon_zip's entry for this sound */
static short *load_zip(const char *name, size_t *samples, unsigned *rate)
{
    for (const struct board_sound *b = board.earcon_zip; b && b->name; b++) {
        if (strcmp(b->name, name)) continue;
        char path[300]; size_t len; unsigned char *buf; short *pcm;
        zip_path(b, path, sizeof path);
        if (!(buf = zip_entry(path, b->entry, &len))) { fprintf(stderr, "sound %s: no stored %s in %s\n", name, b->entry, path); return NULL; }
        pcm = decode(b->entry, buf, len, samples, rate);
        free(buf);
        return pcm;
    }
    return NULL;
}

int sound_get(enum sound s, const short **pcm, size_t *samples, unsigned *rate)
{
    char path[300]; const char *dir = getenv("HASSMIC_EARCONS");    /* tests: stand-ins for stock's, "<dir>/" */
    if (s < 0 || s >= SND_COUNT) return 0;
    if (!dir) dir = board.earcon_dir;
    if (s == SND_CUSTOM && custom_seen != atomic_load(&custom_gen)) {      /* uploaded or deleted since */
        custom_seen = atomic_load(&custom_gen);
        free(cache[s].pcm); cache[s].pcm = NULL; cache[s].tried = 0;
    }
    if (!cache[s].tried) {
        cache[s].tried = 1;
        if (s == SND_CUSTOM) {
            custom_path(path, sizeof path);
            cache[s].pcm = load_file(path, &cache[s].n, &cache[s].rate);
            if (!cache[s].pcm) fprintf(stderr, "sound %s: none uploaded, using the built-in tone\n", names[s]);
            goto done;
        }
        snprintf(path, sizeof path, "%s%s.wav", dir, names[s]);
        cache[s].pcm = load_file(path, &cache[s].n, &cache[s].rate);
        if (!cache[s].pcm) { snprintf(path, sizeof path, "%s%s.mp3", dir, names[s]); cache[s].pcm = load_file(path, &cache[s].n, &cache[s].rate); }
        if (!cache[s].pcm) cache[s].pcm = load_zip(names[s], &cache[s].n, &cache[s].rate);
        if (!cache[s].pcm) fprintf(stderr, "sound %s: not on this image, using the built-in tone\n", names[s]);
    }
done:
    if (!cache[s].pcm) return 0;
    *pcm = cache[s].pcm; *samples = cache[s].n; *rate = cache[s].rate;
    return 1;
}

/* the system partition does not change while hassmic runs: what an APK has is looked up once (1 there, 2 not) */
static atomic_int in_zip[SND_COUNT];

int sound_present(enum sound s)
{
    char path[300]; const char *dir = getenv("HASSMIC_EARCONS");
    if (s < 0 || s >= SND_COUNT) return 0;
    if (s == SND_CUSTOM) { custom_path(path, sizeof path); return !access(path, R_OK); }
    if (!dir) dir = board.earcon_dir;
    snprintf(path, sizeof path, "%s%s.wav", dir, names[s]);
    if (!access(path, R_OK)) return 1;
    snprintf(path, sizeof path, "%s%s.mp3", dir, names[s]);
    if (!access(path, R_OK)) return 1;
    for (const struct board_sound *b = board.earcon_zip; b && b->name; b++) {     /* its entry, where load_zip reads it */
        if (strcmp(b->name, names[s])) continue;
        int k = atomic_load(&in_zip[s]);
        if (!k) {
            FILE *f; uint64_t at; uint32_t n;
            zip_path(b, path, sizeof path);
            k = (f = fopen(path, "rb")) && !zip_find(f, b->entry, &at, &n) ? 1 : 2;
            if (f) fclose(f);
            atomic_store(&in_zip[s], k);
        }
        return k == 1;
    }
    return 0;
}

/* Only what the page makes of an upload: anything else would need a resampler or a decoder on the Echo, and this is
 * decoded at every ring */
int sound_custom_save(const void *wav, size_t len, char *err, size_t errsz)
{
    unsigned fmt, ch, rate = 0, bits; size_t n = 0; char path[300];
    if (len > SOUND_CUSTOM_MAX) { snprintf(err, errsz, "too long: %d s at most", SOUND_CUSTOM_SECS); return -1; }
    if (!wav_data(wav, len, &fmt, &ch, &rate, &bits, &n)) { snprintf(err, errsz, "not a WAV file"); return -1; }
    if (fmt != 1 || ch != 1 || bits != 16 || rate != SOUND_CUSTOM_RATE) {
        snprintf(err, errsz, "a WAV of 16-bit mono PCM at %d Hz only (this one: format %u, %u channel%s, %u bit, %u Hz)", SOUND_CUSTOM_RATE, fmt, ch, ch == 1 ? "" : "s", bits, rate);
        return -1;
    }
    if (n < SOUND_CUSTOM_RATE / 10 * 2) { snprintf(err, errsz, "too short: a tenth of a second at least"); return -1; }
    custom_path(path, sizeof path);
    if (statefile_write(path, 0644, wav, len)) { snprintf(err, errsz, "cannot write %s", path); return -1; }
    atomic_fetch_add(&custom_gen, 1);
    fprintf(stderr, "sound %s: %u ms uploaded\n", names[SND_CUSTOM], (unsigned)(n / 2 * 1000 / SOUND_CUSTOM_RATE));
    return 0;
}

int sound_custom_delete(void)
{
    char path[300];
    custom_path(path, sizeof path);
    int r = unlink(path) && errno != ENOENT ? -1 : 0;
    atomic_fetch_add(&custom_gen, 1);
    return r;
}

/* from the file's size: what the page made has the 44-byte header only */
unsigned sound_custom_ms(void)
{
    char path[300]; struct stat st;
    custom_path(path, sizeof path);
    if (stat(path, &st) || st.st_size <= 44) return 0;
    return (unsigned)((unsigned long long)(st.st_size - 44) / 2 * 1000 / SOUND_CUSTOM_RATE);
}
