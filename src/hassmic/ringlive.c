/*
 * The light ring live (ringlive.h).  What the driver does, measured on a Dot 3 (donut, 2026-11-12, scripts/led-probe.sh;
 * PLAN.md "Light ring live"):
 *   - ISSI's IS31FL3236 at I2C 0-003f.  Its frame file is text, 12 x "RRGGBB" and a newline, LED 0 first, in the order of
 *     the animation files; libacehal_led.so writes it "%02X%02X%02X" per LED.  The driver scales each channel by the
 *     unit's calibration as it takes a frame (ledpwmscaling, root's: 0000FD written reads back 0000A9), so what reads
 *     back is not what was written; a frame is compared with what read back after our own write.
 *   - ledcontroller writes the frame only when its picture changes: idle, and over a static pattern, never (0 writes in
 *     10 s).  A frame written by someone else stays until ledcontroller's next change; over a moving pattern its next
 *     frame replaced ours after 40 ms.  On every set and unset it writes again (the last unset: dark).
 *   - What ledcontroller writes is the file's colour x its brightness table's pwm for the ring's level
 *     (led_mute_ring_config.json rgbTable, by the cap colour, level = index 0..100: solid_blue at 100, 50, 25, 10, 1 read
 *     back A9, 76, 31, 1D, 15, each the table's pwm x 255 through the calibration), truncated (scaleDutyCycle).  It
 *     sets the chip's current (led_current) with the level too; that file stays ledcontroller's.
 *   - A write takes ~15 ms, nearly all of it the I2C bus (1.3 ms of CPU): 65 a second at most.  RINGLIVE_FPS keeps us at 30.
 * So hassmic writes colours as seen through the same gamma ring.c maps designs with and the same pwm ledcontroller uses:
 * a design played live looks as installed, and nothing is brighter than ledcontroller would make it at that level (at
 * level 100 FD, its own most).  Whatever comes from Home Assistant goes through that, and a pixel never above 255 x pwm.
 */
#include "ringlive.h"
#include "ring.h"
#include "board.h"
#include "json.h"
#include "statefile.h"
#include <ctype.h>
#include <signal.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define MAX_LEDS   RING_LEDS_MAX
#define GAMMA      2.2f                 /* as ring.c's designs and light */
#define TEXT_MAX   (MAX_LEDS * 6 + 8)
#define CHECK_MS   200                  /* how often the frame is read back while ours shows */
#define RESUME_MS  150                  /* after the Echo's own pattern: ledcontroller's unset writes first (ledq) */
#define FIGHT_N    3                    /* read back changed this often within FIGHT_MS: something else animates */
#define FIGHT_MS   3000
#define BACKOFF_MS 5000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static atomic_int avail;                /* set once by ringlive_start; read from the protocols' threads */
static int leds = 12, dirty, enabled, quitting;      /* enabled, quitting: under lock */
static pid_t our_pid;                   /* SIGTERM's handler acts only in hassmic itself, not in a child between fork and
                                         * exec (ledq.c's ledctrl) */
static _Atomic int writes, owned_now;   /* owned_now: ours is on the ring (SIGTERM's handler writes the dark frame) */
static atomic_int yield_gen;            /* the Echo's own pattern was set (ringlive_yield): owned_now waits for the writer */
static atomic_int yielding;             /* ringlive_yield .. _end: no frame of ours written meanwhile */
static atomic_int owned_gen;            /* yield_gen our last frame was written under: after any yield since, the ring is
                                         * not ours until we write again (SIGTERM's handler checks it) */
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;     /* a frame write in flight (writer only) */
static atomic_int leftover;             /* the marker: 1 the last run of this boot left the ring lit, 2 ringlive_begin came */
static char marker_path[300];
static char dark_text[TEXT_MAX];
static float shown[MAX_LEDS][3], fade_from[MAX_LEDS][3];   /* the last picture as seen, and a fade's start */
static long long fade_t0; static int fade_ms;
static char frame_path[256];
static float pwm[101];
static int (*hold_fn)(void), (*level_fn)(void);
static void (*changed_fn)(void);

static struct live_led led[MAX_LEDS];
static long long off_at[MAX_LEDS];      /* a frame's timeout: that LED dark then; 0 none */
static struct { struct rd_design d; int on, drawn, repeat; long long t0; float bri; char name[RD_NAME]; } anim;

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static float clamp01(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; }

/* ---------------------------------------------------------------- pure parts */

void ringlive_duty(const float seen[3], float p, unsigned char out[3])
{
    p = clamp01(p);
    int cap = (int)(255 * p);
    for (int c = 0; c < 3; c++) {
        int v = (int)lroundf(powf(clamp01(seen[c]), GAMMA) * 255);      /* ring.c colour_rgb's mapping */
        int o = (int)(v * p);                                           /* the HAL's: multiplied, truncated */
        out[c] = (unsigned char)(o < 0 ? 0 : o > cap ? cap : o);
    }
}

void ringlive_text(const unsigned char (*px)[3], int n, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    char *o = out;
    for (int i = 0; i < n; i++) for (int c = 0; c < 3; c++) { *o++ = hex[px[i][c] >> 4]; *o++ = hex[px[i][c] & 15]; }
    *o++ = '\n'; *o = 0;
}

int ringlive_table(const char *s, size_t n, int capcolor, float out[101])
{
    struct json j = { 0 }; char want[16] = "";
    if (json_parse(&j, s, n)) { json_free(&j); return -1; }
    int rgb = json_get(&j, 0, "rgbTable"), caps = json_get(&j, 0, "cap_color"), rc = -1, ntab = 0;
    if (capcolor >= 0 && json_type(&j, caps) == J_ARR)
        JSON_EACH(&j, caps, k) if (json_int(&j, json_get(&j, k, "cap_color_number"), -1) == capcolor) json_str(&j, json_get(&j, k, "color"), want, sizeof want);
    if (json_type(&j, rgb) == J_OBJ) {
        for (int i = 0; i <= 100; i++) out[i] = 2;
        for (int t = json_first(&j, rgb); t >= 0; t = json_next(&j, rgb, t)) {
            int tab = json_next(&j, rgb, t);    /* an object's key, then its value */
            char key[16] = ""; json_str(&j, t, key, sizeof key);
            if (json_type(&j, tab) != J_ARR || (want[0] && strcmp(key, want))) { t = tab; continue; }
            if (json_len(&j, tab) < 101) { ntab = -1; break; }
            int i = 0;
            JSON_EACH(&j, tab, e) {
                if (i > 100) break;
                double p = json_num(&j, json_get(&j, e, "pwm"), -1);
                if (!(p > 0 && p <= 1)) { ntab = -1; break; }
                if (p < out[i]) out[i] = (float)p;      /* the cap colour unknown: the dimmer of the tables */
                i++;
            }
            if (ntab < 0) break;
            ntab++;
            t = tab;
        }
        if (ntab > 0) rc = 0;
    }
    json_free(&j);
    return rc;
}

/* ---------------------------------------------------------------- the frame file */

static int frame_write(const char *t)
{
    int fd = open(frame_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = strlen(t);
    ssize_t k = write(fd, t, n);
    int rc = k == (ssize_t)n ? 0 : -1;                  /* sysfs takes a frame in one write */
    writes++;
    int e = k >= 0 && rc ? EIO : errno;                 /* a short write sets no errno of its own */
    close(fd);
    errno = e;
    return rc;
}

/* A frame write that never lands after ledcontroller's for a pattern being set (ringlive_yield): read yield_gen,
 * look at hold_fn, then this.  -2: a yield came or the Echo's own shows, nothing written */
static int frame_write_guarded(const char *t, int gen, int *err)
{
    int rc = -2;
    pthread_mutex_lock(&write_lock);
    if (!atomic_load(&yielding) && atomic_load(&yield_gen) == gen) { rc = frame_write(t); *err = errno; }
    pthread_mutex_unlock(&write_lock);
    return rc;
}

static int frame_read(char *out, size_t cap)
{
    int fd = open(frame_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, out, cap - 1);
    close(fd);
    if (k < 0) return -1;
    out[k] = 0;
    return 0;
}

/* The marker: this run lit the ring in this boot.  init's "stop hassmic" (an update, a new wake word engine, a model
 * installed) kills outright, and ledcontroller draws again only on a change of its own: what showed stays, a moving
 * effect frozen.  So the next start in the same boot makes the ring dark, once the Echo's own state is known
 * (ringlive_begin: the mute latch, the mics-off ring, the setup spinner) and only while none of it shows: then
 * ledcontroller, idle, shows dark too.  After a reboot or a power cut the chip lost it anyway and ledcontroller drew the
 * ring anew: the marker is only deleted.
 * Written once a run, when ours first shows, and deleted when the run ends cleanly; never per frame or colour: /data
 * is the Echo's flash, and the first version wrote the frame (fsync'd) on every change Home Assistant made and every
 * 5 s of an effect, ~17000 times a day with one running (2026-11-12) */
static void boot_id(char out[48])
{
    const char *f = getenv("HASSMIC_BOOT_ID"); FILE *b = fopen(f ? f : "/proc/sys/kernel/random/boot_id", "r");
    out[0] = 0;
    if (b) { if (!fgets(out, 48, b)) out[0] = 0; fclose(b); }
    out[strcspn(out, "\r\n")] = 0;
}
static void marker_write(void)
{
    char t[64], id[48];
    boot_id(id);
    int n = snprintf(t, sizeof t, "%s\n", id);
    statefile_write(marker_path, 0600, t, (size_t)n);
}

/* ---------------------------------------------------------------- effects */

static unsigned hash2(unsigned a, unsigned b)
{
    unsigned h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
static float noise(unsigned j, float x)         /* smooth value noise in 0..1 */
{
    unsigned i = (unsigned)floorf(x); float f = x - floorf(x), u = f * f * (3 - 2 * f);
    float a = (hash2(j, i) & 0xFFFF) / 65535.0f, b = (hash2(j, i + 1) & 0xFFFF) / 65535.0f;
    return a + (b - a) * u;
}
static void hue_rgb(float h, float out[3])      /* a fully saturated hue, h in turns */
{
    h = (h - floorf(h)) * 6; float f = h - floorf(h);
    switch ((int)h % 6) {
    case 0: out[0] = 1; out[1] = f; out[2] = 0; break;
    case 1: out[0] = 1 - f; out[1] = 1; out[2] = 0; break;
    case 2: out[0] = 0; out[1] = 1; out[2] = f; break;
    case 3: out[0] = 0; out[1] = 1 - f; out[2] = 1; break;
    case 4: out[0] = f; out[1] = 0; out[2] = 1; break;
    default: out[0] = 1; out[1] = 0; out[2] = 1 - f; break;
    }
}
static float around(float a, float b, int n)    /* how far a is behind b round a ring of n */
{
    float d = fmodf(b - a, (float)n);
    return d < 0 ? d + n : d;
}

void ringlive_effect(int effect, const float rgb[3], float level, int n, long long t, float out[][3])
{
    const float tau = 2 * (float)M_PI;
    for (int j = 0; j < n; j++) {
        float k = 1, c[3] = { rgb[0], rgb[1], rgb[2] };
        switch (effect) {
        case RING_E_PULSE: k = 0.35f + 0.65f * (0.5f - 0.5f * cosf(tau * (t % 2400) / 2400.0f)); break;     /* as the palette's */
        case RING_E_SPIN: {                         /* two comets round the ring once a second over the colour at 25 % */
            float head = (t % 1000) / 1000.0f * n, d = fminf(around(j, head, n), around(j, head + n / 2.0f, n));
            k = fmaxf(0.25f, 1 - 0.35f * d); } break;
        case RING_E_BLINK: k = (t / 500) % 2 ? 0 : 1; break;
        case RING_E_RAINBOW: hue_rgb((float)j / n - (t % 2000) / 2000.0f, c); break;                         /* turning once in 2 s */
        case RING_E_BREATHE: k = 0.5f - 0.5f * cosf(tau * (t % 4000) / 4000.0f); break;                      /* down to dark, 4 s */
        case RING_E_COLORLOOP: hue_rgb((t % 12000) / 12000.0f, c); break;                                    /* the whole ring, 12 s */
        case RING_E_COMET: {                        /* one head, a long tail, round in 1.5 s */
            float head = (t % 1500) / 1500.0f * n, d = around(j, head, n);
            k = expf(-d / 2.2f); } break;
        case RING_E_WAVE: k = 0.15f + 0.85f * (0.5f + 0.5f * sinf(tau * ((float)j / n - (t % 2500) / 2500.0f))); break;
        case RING_E_SPARKLE: {                      /* the colour at 30 %, an LED now and then flaring white-hot */
            long long slot = t / 150; float f = (t % 150) / 150.0f;
            k = 0.3f;
            if (hash2((unsigned)j, (unsigned)slot) % 9 == 0) { k = 1 - 0.7f * f; for (int i = 0; i < 3; i++) c[i] = c[i] + (1 - c[i]) * 0.5f * (1 - f); }
            } break;
        case RING_E_FIRE: {                         /* embers: orange to yellow, flickering, the colour ignored */
            float v = noise((unsigned)j, t / 90.0f) * 0.6f + noise((unsigned)j + 97, t / 31.0f) * 0.4f;
            k = 0.25f + 0.75f * v; c[0] = 1; c[1] = 0.18f + 0.32f * v; c[2] = 0.02f * v; } break;
        default: break;
        }
        for (int i = 0; i < 3; i++) out[j][i] = clamp01(c[i] * k * level);
    }
}

/* ---------------------------------------------------------------- music */

static float rgb_hue(const float c[3])          /* the hue of a colour in turns; 0 for a grey */
{
    float mx = fmaxf(c[0], fmaxf(c[1], c[2])), mn = fminf(c[0], fminf(c[1], c[2])), d = mx - mn, h;
    if (d < 1e-4f) return 0;
    if (mx == c[0]) h = fmodf((c[1] - c[2]) / d + 6, 6); else if (mx == c[1]) h = (c[2] - c[0]) / d + 2; else h = (c[0] - c[1]) / d + 4;
    return h / 6;
}
static float from_front(int j, int n) { return (float)(j <= n / 2 ? j : n - j); }     /* LEDs from LED 1, either way round */

static void hsv_rgb(float h, float sat, float v, float out[3])
{
    hue_rgb(h, out);
    for (int i = 0; i < 3; i++) out[i] = v * (1 - sat + sat * out[i]);
}
static float since(long long t, long long at) { return at && t >= at ? (float)(t - at) : 1e9f; }

/* What the music effects carry from frame to frame: each LED as last shown (they fall back smoothly), the meter's
 * peaks, the orbit's turn while there is no tempo, the effect they were for */
static struct { long long t; int effect; float led[RING_LEDS_MAX][3], peak[2], peak_at[2], turn; } mus;

void ringlive_music_effect(int effect, const float rgb[3], float level, int n, long long t, const struct music_feat *m, float out[][3])
{
    const float tau = 2 * (float)M_PI;
    float dt = mus.t && t > mus.t ? fminf(0.2f, (t - mus.t) / 1000.0f) : 0, c[3] = { clamp01(rgb[0]), clamp01(rgb[1]), clamp01(rgb[2]) };
    if (mus.effect != effect) { memset(mus.led, 0, sizeof mus.led); mus.peak[0] = mus.peak[1] = 0; mus.effect = effect; }
    mus.t = t;
    /* the light's colour as hue and saturation; white (or near it) takes the effect's own colours */
    float mx = fmaxf(c[0], fmaxf(c[1], c[2])), mn = fminf(c[0], fminf(c[1], c[2])), sat = mx > 0 ? (mx - mn) / mx : 0, h0 = rgb_hue(c);
    int white = sat < 0.25f;
    float release = effect == RING_E_MUSIC_AURORA ? 0.35f : effect == RING_E_MUSIC_ORBIT ? 0.06f : effect == RING_E_MUSIC_DRUMS ? 0.12f : 0.09f;     /* s, as seen */
    float tgt[RING_LEDS_MAX][3]; memset(tgt, 0, sizeof tgt);
    if (!m || !m->active) {                     /* nothing playing: a quiet breath (the aurora drifts), so it shows it is on */
        float k = 0.03f + 0.06f * (0.5f - 0.5f * cosf(tau * (t % 6000) / 6000.0f));
        for (int j = 0; j < n; j++) {
            float col[3];
            if (effect == RING_E_MUSIC_AURORA) hsv_rgb((white ? 0.45f : h0) + 0.06f * sinf(tau * ((float)j / n + (t % 20000) / 20000.0f)), white ? 0.8f : sat, 1, col);
            else if (effect == RING_E_MUSIC_SPECTRUM && white) hsv_rgb((float)j / n * 0.8f, 1, 1, col);
            else memcpy(col, c, sizeof col);
            for (int i = 0; i < 3; i++) tgt[j][i] = col[i] * k;
        }
        mus.peak[0] = mus.peak[1] = 0;
        release = 0.5f;
    }
    else {
        float half = n / 2.0f, lv = m->level;
        float age = since(t, m->beat_at), beat = m->beat_ms > 0 ? m->beat_ms : 500;
        int place = ((m->beat_no % 4) + 4) % 4, down = place == 0;
        /* the colours: the light's own, or for white a palette of the effect's (sky blue, with its accents) */
        float hb = white ? 0.58f : h0, sb = white ? 0.75f : fmaxf(0.55f, sat);
        float kick_near = m->kick.at[0] && fabsf((float)(m->kick.at[0] - m->beat_at)) < 70 ? m->kick.strength[0] : 0.6f;
        for (int j = 0; j < n; j++) {
            float p = from_front(j, n), col[3], k = 0;
            hsv_rgb(hb, sb, 1, col);
            switch (effect) {
            case RING_E_MUSIC_BEAT: {           /* each beat blooms from LED 1 round to the back and settles to a glow: the
                                                   ring breathes in time; each beat of the bar a shade apart, the 1 whiter */
                float a = age - p * 22, bloom = a >= 0 ? expf(-a / (0.32f * beat)) : 0;
                float glow = 0.16f + 0.24f * lv;
                k = glow + (1 - glow) * bloom * (0.55f + 0.45f * kick_near);
                hsv_rgb(hb + 0.035f * (place - 1.5f), sb, 1, col);
                float w = (down ? 0.45f : 0.15f) * bloom;
                for (int i = 0; i < 3; i++) col[i] += (1 - col[i]) * w;
                } break;
            case RING_E_MUSIC_SPECTRUM: {       /* LED by LED the twelve bands, bass at LED 1 round to the highs at LED 12 */
                float v = m->band[j * MUSIC_BANDS / n];
                k = 0.07f + 0.93f * powf(v, 1.25f);
                hsv_rgb(white ? (float)j / (n - 1) * 0.75f : h0 - 0.12f + (float)j / (n - 1) * 0.3f, 1, 1, col);
                if (v > 0.9f) for (int i = 0; i < 3; i++) col[i] += (1 - col[i]) * 0.35f * (v - 0.9f) * 10;
                } break;
            case RING_E_MUSIC_VU: {             /* stereo: right from LED 1 the clock's way, left the other; peaks hold, then fall */
                int side = j == 0 ? -1 : j <= n / 2 ? 1 : 0;   /* 1 right, 0 left, -1 LED 1: both */
                float fill_r = clamp01(m->right * (half + 1) - p), fill_l = clamp01(m->left * (half + 1) - p);
                float fill = side == 1 ? fill_r : side == 0 ? fill_l : fmaxf(fill_l, fill_r);
                if (p == half) fill = fmaxf(fill_l, fill_r);
                float g = p / half;             /* green, yellow, red */
                col[0] = g < 0.55f ? g / 0.55f : 1; col[1] = g < 0.55f ? 1 : 1 - (g - 0.55f) / 0.45f; col[2] = 0.1f * (1 - g);
                k = 0.05f + 0.95f * fill;
                for (int s = 0; s < 2; s++) if ((s == side || side < 0) && mus.peak[s] > 1 && mus.peak[s] < half && fabsf(p - floorf(mus.peak[s] - 0.01f)) < 0.5f && fill < 0.3f) {
                    col[0] = col[1] = col[2] = 1; k = 0.65f;
                }
                } break;
            case RING_E_MUSIC_ORBIT: {          /* two comets, one round a bar, the other the other way; they flare on the kick */
                float turn = mus.turn;
                if (m->confidence > 0.2f) turn = (place + fminf(1, age / beat)) / 4;
                float ha = turn * n, hb_ = (0.5f - turn) * n, len = 1.4f + 3 * m->energy;
                float da = around((float)j, ha, n), db = around(hb_, (float)j, n);
                float flare = 0.5f * expf(-since(t, m->kick.at[0]) / 140) * m->kick.strength[0];
                float ka = expf(-da / len), kb = expf(-db / len), head = 0.55f + 0.45f * lv + flare;
                float ca[3], cb[3];
                hsv_rgb(white ? 0.07f : h0, white ? 0.7f : sb, 1, ca); hsv_rgb(white ? 0.58f : h0 + 0.5f, sb, 1, cb);
                for (int i = 0; i < 3; i++) col[i] = (ca[i] * ka + cb[i] * kb + 0.3f * (ca[i] + cb[i]) * 0.05f) / fmaxf(1e-3f, fmaxf(ka, kb) + 0.03f);
                k = 0.07f + fmaxf(ka, kb) * head;
                } break;
            case RING_E_MUSIC_DRUMS: {          /* the kick a wave from LED 1, the snare one from the back, hi-hats sparks */
                float acc[3], cs[3], ch[3], glow = 0.10f + 0.12f * lv;
                hsv_rgb(hb + 0.03f * sinf(tau * (t % 16000) / 16000.0f), sb, 1, col);
                hsv_rgb(hb + 0.5f, white ? 0.6f : sb * 0.8f, 1, cs);
                for (int i = 0; i < 3; i++) { cs[i] += (1 - cs[i]) * 0.3f; ch[i] = 0.75f + 0.25f * col[i]; acc[i] = col[i] * glow; }
                for (int h = 0; h < MUSIC_HITS; h++) {
                    float ak = since(t, m->kick.at[h]), as = since(t, m->snare.at[h]), ah = since(t, m->hat.at[h]);
                    if (ak < 500) {
                        float x = (p - ak / 280 * half) / 1.6f, w = 1.1f * expf(-x * x) * m->kick.strength[h] * (1 - ak / 500);
                        if (p < 1.5f) w += m->kick.strength[h] * expf(-ak / 150);
                        for (int i = 0; i < 3; i++) acc[i] += col[i] * w;
                    }
                    if (as < 450) { float x = ((half - p) - as / 240 * half) / 1.4f, w = expf(-x * x) * m->snare.strength[h] * (1 - as / 450); for (int i = 0; i < 3; i++) acc[i] += cs[i] * w; }
                    if (ah < 220) {
                        unsigned spot = (unsigned)(m->hat.at[h] / 7) % (unsigned)n;
                        if ((unsigned)j == spot || (unsigned)j == (spot + 5) % (unsigned)n) { float w = 0.9f * m->hat.strength[h] * expf(-ah / 90); for (int i = 0; i < 3; i++) acc[i] += ch[i] * w; }
                    }
                }
                float mxa = fmaxf(acc[0], fmaxf(acc[1], acc[2]));
                k = fminf(1, mxa);
                for (int i = 0; i < 3; i++) col[i] = mxa > 0 ? acc[i] / mxa : 0;
                } break;
            case RING_E_MUSIC_AURORA: {         /* light flowing round, one drift in two bars; louder brighter, the beat a swell
                                                   running round; brighter music turns it a little cooler */
                float cycle = m->confidence > 0.2f ? 8 * beat : 6000, x = (float)j / n, T = (float)(t % 3600000) / cycle;
                float f = 0.5f + 0.25f * sinf(tau * (x + 0.6f * T)) + 0.15f * sinf(tau * (2 * x - T) + 1) + 0.10f * sinf(tau * (3 * x + 0.4f * T) + 2);
                float g = 0.5f + 0.5f * sinf(tau * (x - 1.3f * T) + 0.7f);
                float base_h = white ? 0.42f : h0;
                hsv_rgb(base_h + 0.16f * (f - 0.5f) + 0.15f * (m->bright - 0.4f) + 0.05f * sinf(tau * 0.13f * T), white ? 0.8f : sb, 1, col);
                if (f > 0.7f) { float ac[3]; hsv_rgb(base_h + 0.3f, white ? 0.7f : sb, 1, ac); for (int i = 0; i < 3; i++) col[i] += (ac[i] - col[i]) * (f - 0.7f) / 0.3f * 0.6f; }
                float swell = expf(-age / 520) * (0.4f + 0.6f * m->energy);
                k = 0.18f + 0.5f * f * (0.45f + 0.55f * lv) + 0.32f * swell * g;
                } break;
            }
            k = clamp01(k);                     /* never past the light's own level */
            for (int i = 0; i < 3; i++) tgt[j][i] = clamp01(col[i]) * k;
        }
        if (effect == RING_E_MUSIC_VU)          /* the peaks: held 0.4 s, then falling 6 LEDs a second */
            for (int s = 0; s < 2; s++) {
                float top = (s ? m->right : m->left) * (half + 1);
                if (top >= mus.peak[s]) { mus.peak[s] = top; mus.peak_at[s] = 0; }
                else if ((mus.peak_at[s] += dt) > 0.4f) mus.peak[s] = fmaxf(0, mus.peak[s] - 6 * dt);
            }
        if (effect == RING_E_MUSIC_ORBIT && m->confidence <= 0.2f) mus.turn = fmodf(mus.turn + dt * (0.25f + 0.5f * lv), 1);
    }
    /* as seen: up at once, back down smoothly */
    float fall = dt > 0 ? 1 - expf(-dt / release) : 1;
    for (int j = 0; j < n; j++) for (int i = 0; i < 3; i++) {
        float v = tgt[j][i], *s_ = &mus.led[j][i];
        *s_ = v >= *s_ ? v : *s_ + (v - *s_) * fall;
        out[j][i] = clamp01(*s_ * level);
    }
}

/* ---------------------------------------------------------------- the picture */

static int hw_of(int j, int rot) { return ((leds - 1 + rot + j) % leds + leds) % leds; }     /* ring.c's fill: volume_step-01's LED first */

/* Home Assistant's "Light ring" as the bottom layer (ring.c keeps its state): its colour or effect on its fill, LED by
 * LED from light_ring_leds, a flash blinking it whole */
static void base(const struct ring_light *l, int flashing, long long now, int rot, const struct music_feat *m, float seen[][3], int *next)
{
    float lv = clamp01(l->brightness) * clamp01(l->color_brightness), rgb[3] = { clamp01(l->r), clamp01(l->g), clamp01(l->b) };
    if (flashing) {
        if ((now / 500) % 2 == 0) for (int h = 0; h < leds; h++) for (int c = 0; c < 3; c++) seen[h][c] = rgb[c] * lv;
        int left = 500 - (int)(now % 500); if (left < *next) *next = left;
        return;
    }
    if (!l->on) return;
    if (l->nleds > 0) {
        for (int j = 0; j < l->nleds && j < leds; j++) for (int c = 0; c < 3; c++) seen[hw_of(j, rot)][c] = l->led_rgb[j][c] / 255.0f * lv;
        return;
    }
    float out[MAX_LEDS][3]; int lit = (l->fill * leds + 99) / 100;
    if (RING_E_IS_MUSIC(l->effect)) ringlive_music_effect(l->effect, rgb, lv, leds, now, m, out);
    else ringlive_effect(l->effect, rgb, lv, leds, now, out);
    for (int j = 0; j < lit && j < leds; j++) memcpy(seen[hw_of(j, rot)], out[j], sizeof out[j]);
    if (l->effect != RING_E_NONE && *next > 1000 / RINGLIVE_FPS) *next = 1000 / RINGLIVE_FPS;
}

/* lock held: what the ring shows now, as seen, per LED of the driver: the light, the LEDs over it, an animation over
 * all, a fade between the last picture and this one; *next: ms until it changes by itself.  1 if the LEDs' states or
 * what plays changed by itself (a timeout, an animation's end) */
static int picture(long long now, int rot, const struct ring_light *rl, int flashing, const struct music_feat *m, float seen[][3], int *next)
{
    int ch = 0;
    *next = CHECK_MS;
    memset(seen, 0, sizeof(float) * 3 * MAX_LEDS);
    for (int j = 0; j < leds; j++)
        if (off_at[j] && now >= off_at[j]) { off_at[j] = 0; led[j].on = 0; ch = 1; }
        else if (off_at[j] && off_at[j] - now < *next) *next = (int)(off_at[j] - now);
    int drawn = 0;
    if (anim.on) {
        const struct rd_design *d = &anim.d; long long total = 0, loop_at = 0, e = now - anim.t0;
        for (int i = 0; i < d->nframes; i++) { if (anim.drawn && i == d->loop) loop_at = total; total += d->f[i].ms; }
        if (total <= 0) total = 1;
        int done = 0;
        if (anim.repeat > 0) { if (e >= total * anim.repeat) done = 1; else e %= total; }
        else if (!anim.drawn) e %= total;                                   /* Home Assistant's: until stopped */
        else if (d->loop < 0) { if (e >= total) done = 1; }                  /* a design "once" */
        else if (e >= total) e = loop_at + (e - total) % (total - loop_at > 0 ? total - loop_at : 1);
        if (done) { rd_free(&anim.d); memset(&anim, 0, sizeof anim); ch = 1; }
        else {
            int i = 0; long long at = 0;
            while (i < d->nframes - 1 && at + d->f[i].ms <= e) at += d->f[i++].ms;
            long long left = at + d->f[i].ms - e;
            if (left < *next) *next = (int)(left > 0 ? left : 1);
            for (int k = 0; k < leds && k < d->leds; k++) {
                int h = anim.drawn ? k : hw_of(k, rot);
                for (int c = 0; c < 3; c++) seen[h][c] = d->f[i].px[k][c] / 255.0f * anim.bri;
            }
            drawn = 1;
        }
    }
    if (!drawn) {
        if (enabled) base(rl, flashing, now, rot, m, seen, next);
        for (int j = 0; j < leds; j++) if (led[j].on) {
            int h = hw_of(j, rot);
            seen[h][0] = led[j].r * led[j].brightness; seen[h][1] = led[j].g * led[j].brightness; seen[h][2] = led[j].b * led[j].brightness;
        }
    }
    if (fade_ms > 0) {                          /* a light's transition: from what showed to this, as seen */
        float k = (float)(now - fade_t0) / fade_ms;
        if (k >= 1) fade_ms = 0;
        else {
            k = k < 0 ? 0 : k;
            for (int h = 0; h < leds; h++) for (int c = 0; c < 3; c++) seen[h][c] = fade_from[h][c] + (seen[h][c] - fade_from[h][c]) * k;
            if (*next > 1000 / RINGLIVE_FPS) *next = 1000 / RINGLIVE_FPS;
        }
    }
    memcpy(shown, seen, sizeof(float) * 3 * MAX_LEDS);
    return ch;
}

/* lock held: anything of ours to show (the picture just made has light in it, or a fade still goes to dark) */
static int active_locked(void)
{
    if (anim.on || fade_ms > 0) return 1;
    for (int h = 0; h < leds; h++) if (shown[h][0] > 0 || shown[h][1] > 0 || shown[h][2] > 0) return 1;
    for (int j = 0; j < leds; j++) if (led[j].on && (led[j].r > 0 || led[j].g > 0 || led[j].b > 0) && led[j].brightness > 0) return 1;
    return 0;
}

/* ---------------------------------------------------------------- the writer */

static void *writer(void *arg)
{
    (void)arg;
    char text[TEXT_MAX] = "", seen_back[TEXT_MAX] = "", now_back[TEXT_MAX];
    long long last_write = 0, last_check = 0, resume_at = 0, backoff_until = 0, fight_t0 = 0;
    int owned = 0, held = 0, fights = 0, failed = 0, marked = 0;
    for (;;) {
        if (atomic_load(&leftover) == 2) {      /* what the last run of this boot left lit (the marker) */
            atomic_store(&leftover, 0);
            int g = atomic_load(&yield_gen), e = 0, rc = -2;
            if (!owned && !(hold_fn && hold_fn())) rc = frame_write_guarded(dark_text, g, &e);
            if (owned) {}
            else if (rc == -2) fprintf(stderr, "light ring live: the last run left the ring lit, but the Echo's own shows (or comes) now: left to it\n");
            else if (!rc) fprintf(stderr, "light ring live: what the last run left on the ring is dark again\n");
            else fprintf(stderr, "light ring live: cannot darken what the last run left (%s)\n", strerror(e));
            if (!marked && rc != -1) unlink(marker_path);     /* a failed write: the next start tries again */
        }
        int rot = ring_rotation(-1), lvl = level_fn ? level_fn() : 100, next, flashing = 0;
        float seen[MAX_LEDS][3]; struct ring_light rl; memset(&rl, 0, sizeof rl);
        if (lvl < 0) lvl = 0;
        if (lvl > 100) lvl = 100;
        pthread_mutex_lock(&lock); int en = enabled; pthread_mutex_unlock(&lock);
        if (en) flashing = ring_light_now(&rl);             /* outside our lock: ring.c's own */
        /* the music: analysed only while a music effect shows it */
        int music = en && rl.on && !flashing && rl.nleds <= 0 && RING_E_IS_MUSIC(rl.effect);
        struct music_feat mf; memset(&mf, 0, sizeof mf);
        musiclight_want(music);
        if (music) musiclight_now(now_ms(), &mf);
        pthread_mutex_lock(&lock);
        long long now = now_ms();
        int ch = picture(now, rot, &rl, flashing, &mf, seen, &next), quit = quitting, content = active_locked() && !quit;
        pthread_mutex_unlock(&lock);
        if (ch && changed_fn) changed_fn();

        if (content || owned) {
            if (quit && held) owned = 0;    /* ending while the Echo's own shows: never written over */
            else if (hold_fn && hold_fn() && !quit) {    /* the Echo's own pattern, or muted: hands off, ledcontroller writes */
                if (!held) fprintf(stderr, "light ring live: the Echo's own pattern shows, live waits\n");
                held = 1; next = next < CHECK_MS ? next : CHECK_MS;
                owned_now = 0;                  /* the ring is ledcontroller's meanwhile: SIGTERM leaves it alone */
            }
            else if (held) {                    /* ledcontroller wrote last: ours again, not counted as a fight */
                held = 0; resume_at = now + RESUME_MS; next = RESUME_MS; text[0] = 0; fights = 0;
            }
            else if (now < resume_at && !quit) next = (int)(resume_at - now);
            else if (now < backoff_until && !quit) next = (int)(backoff_until - now);
            else {
                unsigned char px[MAX_LEDS][3]; char want[TEXT_MAX];
                for (int h = 0; h < leds; h++) {
                    if (content) ringlive_duty(seen[h], pwm[lvl], px[h]); else memset(px[h], 0, 3);
                }
                ringlive_text((const unsigned char (*)[3])px, leds, want);
                int need = strcmp(want, text) || !owned;
                if (!need && now - last_check >= CHECK_MS) {        /* ledcontroller wrote over it? */
                    last_check = now;
                    if (!frame_read(now_back, sizeof now_back) && strcmp(now_back, seen_back)) {
                        need = 1;
                        if (now - fight_t0 > FIGHT_MS) { fight_t0 = now; fights = 0; }
                        if (++fights >= FIGHT_N) {
                            fprintf(stderr, "light ring live: something else keeps writing the ring (a pattern hassmic did not set?), live waits %d s\n", BACKOFF_MS / 1000);
                            backoff_until = now + BACKOFF_MS; fights = 0; need = 0; next = BACKOFF_MS;
                        }
                    }
                }
                if (need && !quit && now - last_write < 1000 / RINGLIVE_FPS) { next = (int)(1000 / RINGLIVE_FPS - (now - last_write)); need = 0; }
                /* The last look, then the write under write_lock, which ringlive_yield waits for: a yield begun after
                 * yg2 was read changed yield_gen (or still counts in yielding), one ended before it left ring_set's
                 * ring_busy() for hold_fn to see.  So ledcontroller's frame for that pattern always lands after ours.
                 * Only the write itself is under the lock (no other lock taken there): yield waits ~15 ms at most */
                int yg2 = atomic_load(&yield_gen), wrc = 0, werr = 0;
                if (need && hold_fn && hold_fn()) need = 0;     /* the Echo's own came meanwhile: its frame wins */
                if (need && (wrc = frame_write_guarded(want, yg2, &werr)) == -2) { need = 0; next = 5; }
                if (need && !wrc) atomic_store(&owned_gen, yg2);
                if (need) {
                    last_write = last_check = now;
                    if (wrc) {
                        if (!failed) fprintf(stderr, "light ring live: cannot write %s (%s), tries again in 10 s\n", frame_path, strerror(werr));
                        failed = 1; backoff_until = now + 10000;
                    }
                    else {
                        if (failed) fprintf(stderr, "light ring live: %s written again\n", frame_path);
                        failed = 0;
                        snprintf(text, sizeof text, "%s", want);
                        if (frame_read(seen_back, sizeof seen_back)) seen_back[0] = 0;
                        if (content && !owned) fprintf(stderr, "light ring live: on the ring (level %d)\n", lvl);
                        if (content && !marked) { marker_write(); marked = 1; }     /* once a run (see marker_write) */
                        if (!content) {
                            fprintf(stderr, "light ring live: dark, the ring is ledcontroller's again\n"); text[0] = 0;
                            if (quit) { unlink(marker_path); marked = 0; }      /* a clean end: nothing left */
                        }
                        owned = content;
                    }
                }
            }
        }
        /* SIGTERM's handler darkens the ring only while ours shows: not during a yield (yielding), nor after one since
         * our last write (yield_gen moved past owned_gen) until we write again; else the listening ring or the mute's
         * red one would go dark.  Store, then look again: a yield between the two has its owned_now = 0 win */
        int og = atomic_load(&owned_gen);
        owned_now = !atomic_load(&yielding) && atomic_load(&yield_gen) == og && owned && !held
                    && now_ms() >= backoff_until;   /* backing off: what shows is someone else's */
        if (atomic_load(&yielding) || atomic_load(&yield_gen) != og) owned_now = 0;    /* one between the look and the store */
        if (quit && !owned) {
            if (marked && !held) unlink(marker_path);   /* a clean end with the ring dark: nothing for the next start */
            pthread_mutex_lock(&lock); quitting = 2; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&lock); return NULL; }
        if (next < 5) next = 5;
        if (next > CHECK_MS) next = CHECK_MS;
        if (!content && !owned) next = 1000;    /* nothing of ours: a change pokes the thread awake anyway */
        pthread_mutex_lock(&lock);
        if (!dirty) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (long)next * 1000000L; ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L;
            pthread_cond_timedwait(&cv, &lock, &ts);
        }
        dirty = 0;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static void poke(void) { dirty = 1; pthread_cond_signal(&cv); }       /* lock held */

/* ---------------------------------------------------------------- start */

static char *slurp(const char *file, size_t max, size_t *n)
{
    FILE *f = fopen(file, "r"); char *t; size_t k = 0;
    if (!f) return NULL;
    if (!(t = malloc(max + 1))) { fclose(f); return NULL; }
    k = fread(t, 1, max, f); fclose(f);
    t[k] = 0; if (n) *n = k;
    return t;
}

/* A SIGTERM (kill, a test): the dark frame if ours is on the ring, then as before (the default ends the process).  open,
 * write and close are async-signal-safe.  init's "stop hassmic" kills outright (measured on the Dot 3: the LED stayed
 * lit); then the next start's unsets have ledcontroller write its own frame, and the marker darkens ours if it is still
 * there */
static void on_term(int sig)
{
    /* Not under write_lock (an async handler may not wait): a writer's write already past its guard can still land
     * after this one, and leave ours lit with the marker gone; one write's window, ~15 ms */
    if (owned_now && !atomic_load(&yielding) && atomic_load(&yield_gen) == atomic_load(&owned_gen) && getpid() == our_pid) {
        int fd = open(frame_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
        if (fd >= 0) { if (write(fd, dark_text, strlen(dark_text)) == (ssize_t)strlen(dark_text)) unlink(marker_path); close(fd); }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static int capcolor(void)               /* idme alsparams "... capcolor=0": the table's colour; -1 unknown */
{
    const char *f = getenv("HASSMIC_FAKE_ALSPARAMS");
    char *t = slurp(f ? f : "/proc/idme/alsparams", 512, NULL), *c = t ? strstr(t, "capcolor=") : NULL;
    int v = c && isdigit((unsigned char)c[9]) ? atoi(c + 9) : -1;
    free(t);
    return v;
}

int ringlive_start(int (*hold)(void), int (*level)(void), void (*changed)(void))
{
    const char *fake = getenv("HASSMIC_FAKE_FRAME"), *p = fake ? fake : board.ring_frame, *why = NULL;
    char t[TEXT_MAX + 64], table[300]; size_t n;
    hold_fn = hold; level_fn = level; changed_fn = changed;
    leds = ring_leds();
    snprintf(table, sizeof table, "%s/led_mute_ring_config.json", getenv("HASSMIC_LED_ROOT") ? getenv("HASSMIC_LED_ROOT") : "/system/etc");
    char *js = NULL;
    if (!p || !*p) return -1;                                       /* not on this model (yet): nothing to say */
    snprintf(frame_path, sizeof frame_path, "%s", p);
    if (leds < 2 || leds > MAX_LEDS) why = "not a ring of LEDs";
    else if (access(frame_path, W_OK)) why = "hassmic may not write the ring's frame (root's main.sh gives it at the satellite's start; an older one does not)";
    else if (frame_read(t, sizeof t) || strlen(t) != (size_t)leds * 6 + 1 || strspn(t, "0123456789abcdefABCDEF") != (size_t)leds * 6)
        why = "the ring's frame is not 12 x RRGGBB as measured";
    else if (!(js = slurp(table, 1 << 16, &n)) || ringlive_table(js, n, capcolor(), pwm)) why = "no brightness table (led_mute_ring_config.json)";
    free(js);
    if (why) { fprintf(stderr, "light ring live: not here: %s\n", why); return -1; }
    int cc = capcolor();
    fprintf(stderr, "light ring live: %s, %d LEDs, at most %02X at level 100 (cap colour %d)\n", frame_path, leds, (int)(255 * pwm[100]), cc);
    {   /* the marker of the last run: lit in this boot, darkened once ringlive_begin came (the writer) */
        const char *d = getenv("HASSMIC_STATE"); char id[48], m[64] = ""; FILE *f;
        snprintf(marker_path, sizeof marker_path, "%s/ring-live", d ? d : "/data/local/hassmic/state");
        if ((f = fopen(marker_path, "r"))) {
            if (!fgets(m, sizeof m, f)) m[0] = 0;
            fclose(f); m[strcspn(m, "\r\n")] = 0;
            boot_id(id);
            if (id[0] && !strcmp(m, id)) atomic_store(&leftover, 1);
            else { fprintf(stderr, "light ring live: the last run's picture was of an earlier boot: the ring left as it is\n"); unlink(marker_path); }
        }
    }
    { unsigned char z[MAX_LEDS][3]; memset(z, 0, sizeof z); ringlive_text((const unsigned char (*)[3])z, leds, dark_text); }
    if (avail) return 0;                    /* once: one writer, whatever calls this again */
    our_pid = getpid();
    signal(SIGTERM, on_term);
    pthread_t th;
    if (pthread_create(&th, NULL, writer, NULL)) { fprintf(stderr, "light ring live: no thread\n"); return -1; }
    pthread_detach(th);
    avail = 1;
    return 0;
}

int ringlive_available(void) { return avail; }

void ringlive_begin(void)
{
    if (!avail) return;
    int one = 1;
    if (atomic_compare_exchange_strong(&leftover, &one, 2)) { pthread_mutex_lock(&lock); poke(); pthread_mutex_unlock(&lock); }
}

void ringlive_yield(void)              /* counted whether or not the ring is live: a pair that straddles the start stays a pair */
{
    atomic_fetch_add(&yielding, 1);
    atomic_fetch_add(&yield_gen, 1); owned_now = 0;
    if (!avail) return;
    pthread_mutex_lock(&write_lock); pthread_mutex_unlock(&write_lock);     /* a write in flight is done */
    pthread_mutex_lock(&lock); poke(); pthread_mutex_unlock(&lock);
}

void ringlive_yield_end(void)
{
    atomic_fetch_add(&yield_gen, 1);        /* a writer that looked at hold_fn before ring_set: its write is off */
    atomic_fetch_sub(&yielding, 1);
    if (!avail) return;
    pthread_mutex_lock(&lock); poke(); pthread_mutex_unlock(&lock);
}

void ringlive_enable(int on)
{
    pthread_mutex_lock(&lock);
    int was = enabled;
    enabled = on && avail;
    int now_on = enabled;
    if (!enabled) { rd_free(&anim.d); memset(&anim, 0, sizeof anim); for (int i = 0; i < leds; i++) { led[i].on = 0; off_at[i] = 0; } }
    poke();
    pthread_mutex_unlock(&lock);
    if (was != now_on || !on) ring_light_live(now_on);    /* ring.c's palette steps aside for us, or comes back */
}

int ringlive_enabled(void) { pthread_mutex_lock(&lock); int e = enabled; pthread_mutex_unlock(&lock); return e; }

void ringlive_fade(int ms)
{
    pthread_mutex_lock(&lock);
    if (ms > 0) { memcpy(fade_from, shown, sizeof shown); fade_t0 = now_ms(); fade_ms = ms > 10000 ? 10000 : ms; }
    poke();
    pthread_mutex_unlock(&lock);
}

void ringlive_shutdown(void)
{
    if (!avail) return;
    pthread_mutex_lock(&lock);
    quitting = 1; poke();
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
    while (quitting != 2 && !pthread_cond_timedwait(&cv, &lock, &ts)) {}
    pthread_mutex_unlock(&lock);
}
int ringlive_writes(void) { return writes; }
int ringlive_leds(void) { return leds; }

/* ---------------------------------------------------------------- what Home Assistant asks */

void ringlive_led_get(int i, struct live_led *l)
{
    pthread_mutex_lock(&lock);
    if (i >= 0 && i < leds) *l = led[i]; else memset(l, 0, sizeof *l);
    pthread_mutex_unlock(&lock);
}

void ringlive_led_set(int i, const struct live_led *l)
{
    if (i < 0 || i >= leds) return;
    pthread_mutex_lock(&lock);
    led[i] = *l;
    led[i].brightness = clamp01(l->brightness); led[i].r = clamp01(l->r); led[i].g = clamp01(l->g); led[i].b = clamp01(l->b);
    off_at[i] = 0;
    poke();
    pthread_mutex_unlock(&lock);
}

/* "#rrggbb" or "rrggbb" into 0..1 a channel, the largest 1, and its level (the largest / 255) */
static int colour(const char *s, struct live_led *l)
{
    s += *s == '#';
    if (!*s || !strcasecmp(s, "off") || !strcasecmp(s, "black")) { l->on = 0; return 0; }
    if (strlen(s) != 6 || strspn(s, "0123456789abcdefABCDEF") != 6) return -1;
    unsigned long v = strtoul(s, NULL, 16);
    float r = (v >> 16 & 255) / 255.0f, g = (v >> 8 & 255) / 255.0f, b = (v & 255) / 255.0f, mx = fmaxf(r, fmaxf(g, b));
    l->on = mx > 0;
    l->brightness = mx;
    l->r = mx > 0 ? r / mx : 1; l->g = mx > 0 ? g / mx : 1; l->b = mx > 0 ? b / mx : 1;
    return 0;
}

int ringlive_frame(char colors[][16], int n, float brightness, int timeout_s, char *err, size_t cap)
{
    struct live_led want[MAX_LEDS]; memset(want, 0, sizeof want);
    if (n > leds) { snprintf(err, cap, "%d colours for %d LEDs", n, leds); return -1; }
    for (int i = 0; i < n; i++)
        if (colour(colors[i], &want[i])) { snprintf(err, cap, "\"%s\" is no colour (#rrggbb)", colors[i]); return -1; }
    brightness = clamp01(brightness);
    pthread_mutex_lock(&lock);
    long long off = timeout_s > 0 ? now_ms() + (long long)timeout_s * 1000 : 0;
    for (int i = 0; i < leds; i++) {
        if (i < n) {
            if (want[i].on) want[i].brightness *= brightness;
            if (!(want[i].brightness > 0)) want[i].on = 0;      /* brightness 0: dark (below: a level kept for later) */
            led[i] = want[i];
        }
        else memset(&led[i], 0, sizeof led[i]);
        if (!led[i].brightness) led[i].brightness = 1;          /* an LED off keeps a level for when it comes on */
        if (!led[i].r && !led[i].g && !led[i].b) led[i].r = led[i].g = led[i].b = 1;
        off_at[i] = led[i].on ? off : 0;
    }
    poke();
    pthread_mutex_unlock(&lock);
    return 0;
}

static int play(struct rd_design *d, int drawn, int repeat, float brightness, const char *name, char *err, size_t cap)
{
    if (d->leds != leds) { snprintf(err, cap, "made for %d LEDs, the ring has %d", d->leds, leds); rd_free(d); return -1; }
    pthread_mutex_lock(&lock);
    rd_free(&anim.d);
    anim.d = *d; anim.on = 1; anim.drawn = drawn; anim.repeat = repeat < 0 ? 0 : repeat; anim.t0 = now_ms();
    anim.bri = clamp01(brightness);
    snprintf(anim.name, sizeof anim.name, "%s", name);
    poke();
    pthread_mutex_unlock(&lock);
    return 0;
}

int ringlive_animate(const char *lines, size_t n, int repeat, float brightness, char *err, size_t cap)
{
    struct rd_design d; char head[32];
    int k = snprintf(head, sizeof head, "leds %d\n", leds);
    char *t = malloc((size_t)k + n + 1);
    if (!t) { snprintf(err, cap, "no memory"); return -1; }
    memcpy(t, head, (size_t)k); memcpy(t + k, lines, n); t[k + n] = 0;
    int rc = rd_parse(t, (size_t)k + n, &d, err, cap);
    free(t);
    if (rc) return -1;
    return play(&d, 0, repeat, brightness, "animation", err, cap);
}

int ringlive_play_text(const char *text, size_t n, char *err, size_t cap)
{
    struct rd_design d;
    if (rd_parse(text, n, &d, err, cap)) return -1;
    char nm[RD_NAME]; snprintf(nm, sizeof nm, "%s", d.name[0] ? d.name : "draft");
    return play(&d, 1, 0, 1, nm, err, cap);
}

int ringlive_design(const char *name, int repeat, float brightness, char *err, size_t cap)
{
    struct rd_design d;
    if (rd_find(name, &d)) { snprintf(err, cap, "no design \"%s\" in the library", name); return -1; }
    char nm[RD_NAME]; snprintf(nm, sizeof nm, "%s", d.name);
    return play(&d, 1, repeat, brightness, nm, err, cap);
}

void ringlive_stop(void)
{
    pthread_mutex_lock(&lock);
    rd_free(&anim.d); memset(&anim, 0, sizeof anim);
    poke();
    pthread_mutex_unlock(&lock);
}

void ringlive_clear(void)
{
    pthread_mutex_lock(&lock);
    rd_free(&anim.d); memset(&anim, 0, sizeof anim);
    for (int i = 0; i < leds; i++) { led[i].on = 0; off_at[i] = 0; }
    poke();
    pthread_mutex_unlock(&lock);
}

int ringlive_active(void) { pthread_mutex_lock(&lock); int a = active_locked(); pthread_mutex_unlock(&lock); return a; }

void ringlive_playing(char *out, size_t cap)
{
    pthread_mutex_lock(&lock);
    snprintf(out, cap, "%s", anim.on ? anim.name : "");
    pthread_mutex_unlock(&lock);
}
