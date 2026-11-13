/* ringlive.c on the PC: the light ring live against a stand-in frame file (HASSMIC_FAKE_FRAME) and a brightness table
 * (HASSMIC_LED_ROOT).  The pure parts against what a Dot 3 measured (PLAN.md "Light ring live", 2026-11-12): stock's
 * solid_blue (00F) at levels 100/50/25/10/1 is what the driver was written, never above 255 x pwm, the frame's text, the
 * table by cap colour.  Then the writer: a frame, waiting for the Echo's own pattern and taking the ring back after it,
 * a frame ledcontroller wrote over put back, the rate under an animation of 1 ms frames, a timeout, a design of the
 * library, the dark frame at the end, the marker a crash left: what it lit made dark once the Echo's own state is known
 * and none of it shows, the marker written once a run (it is flash), gone after a clean end. */
#define _GNU_SOURCE
#include "ringlive.h"
#include "musiclight.h"
#include "ring.h"
#include "board.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const struct board board = { .volume_steps = 30 };
/* ring.c's side, as ringdesign.c and ringlive.c need it */
const struct ring_slot ring_slots[] = { { "listen", "active-start", RING_LISTEN } };
const char *const ring_effects[] = { "None", "Pulse", "Spin", "Blink", "Rainbow", RING_LIVE_EFFECTS };
const int ring_neffects = 5 + RING_LIVE_NEFFECTS;
const int ring_nslots = 1;
int ring_leds(void) { return 12; }
static int rotation;
int ring_rotation(int set) { (void)set; return rotation; }
int ring_check(const char *a, size_t n, char *e, size_t c) { (void)a; (void)n; (void)e; (void)c; return 0; }
int ring_slot_available(int i) { (void)i; return 1; }
int ring_slot_live(int i) { (void)i; return 0; }
int ring_designs_live(void) { return 0; }
char *ring_stock(int i, size_t *n) { (void)i; (void)n; return NULL; }
void ring_reload(void) {}
int ring_band_rows(int set) { (void)set; return -1; }
/* ring.c's light, as Home Assistant set it: the bottom layer while the live ring draws it */
static struct ring_light light = { .brightness = 1, .color_brightness = 1, .r = 1, .g = 1, .b = 1, .fill = 100 };
static int flashing, light_live;
int ring_light_now(struct ring_light *l) { *l = light; return flashing; }
void ring_light_live(int on) { light_live = on; }
int ring_custom(void) { return 1; }

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

static volatile int hold, level = 100, changes;
static int hold_fn(void) { return hold; }
static int level_fn(void) { return level; }
static void changed_fn(void) { changes++; }

static char dir[256], frame[300];
static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static void put(const char *file, const char *text) { FILE *f = fopen(file, "w"); fputs(text, f); fclose(f); }
static void get(char *out, size_t cap) { FILE *f = fopen(frame, "r"); size_t k = f ? fread(out, 1, cap - 1, f) : 0; if (f) fclose(f); out[k] = 0; }
static const char *dark = "000000000000000000000000000000000000000000000000000000000000000000000000\n";

/* led_mute_ring_config.json's shape: black's pwm as donut's at the measured levels, white's lower */
static void table(const char *file, int entries)
{
    FILE *f = fopen(file, "w");
    fprintf(f, "{ \"cap_color\": [ { \"color\": \"black\", \"cap_color_number\": 0 }, { \"color\": \"white\", \"cap_color_number\": 1 } ],\n"
               "  \"muteTable\": { \"black\": [ 1, 2 ] },\n  \"rgbTable\": {\n");
    for (int t = 0; t < 2; t++) {
        fprintf(f, "    \"%s\": [", t ? "white" : "black");
        for (int i = 0; i < entries; i++) {
            float p = t ? 0.5f * (0.055f + i * 0.0094f) : i == 100 || i == 101 ? 0.993f : i == 50 ? 0.697f : i == 25 ? 0.292f : i == 10 ? 0.173f : i == 1 ? 0.127f : 0.122f + i * 0.0087f;
            fprintf(f, "%s{ \"pwm\": %.3f, \"current\": %d }", i ? ", " : "", p, i > 59 ? 0 : 3);
        }
        fprintf(f, "]%s\n", t ? "" : ",");
    }
    fprintf(f, "  }\n}\n");
    fclose(f);
}


/* musiclight.c on made-up music on a clock of its own (as an output plays it, 200 ms of it in front of the speaker): a
 * kick drum (an 808's pitch drop, 80 ms) at 120 bpm under a quiet hi-hat, then a 6 kHz tone, then silence.  Its finer
 * measure is make unit-ringviz (a whole track with its drums written down) */
static long long sim;
static long long sim_now(void) { return sim; }
static void music(void)
{
    enum { RATE = 48000, CHUNK = 960 };
    static int16_t pcm[CHUNK * 2]; struct music_feat f;
    musiclight_clock(sim_now); musiclight_reset();
    sim = 1000000;
    musiclight_feed(pcm, CHUNK, 2, RATE, 0); musiclight_now(sim + 50, &f);
    check(!f.active, "music: nothing analysed until an effect wants it");
    musiclight_want(1);
    long long t0 = sim; int kicks = 0, beats = 0, on_kick = 0; long long seen_k = 0, seen_b = 0; float bpm = 0;
    unsigned seed = 1; double ph = 0;
    for (int k = 0; k < 500; k++) {                     /* 10 s */
        for (int i = 0; i < CHUNK; i++) {
            double ts = (k * CHUNK + i) / (double)RATE, in = fmod(ts, 0.5), v = 0;
            if (in < 0.3) { ph += (45 + 110 * exp(-in / 0.03)) / RATE; v += 0.8 * sin(2 * M_PI * ph) * fmin(1, in / 0.002) * exp(-in / 0.12); }
            seed = seed * 1103515245u + 12345u; v += 0.02 * (((seed >> 16) & 0x7fff) / 16384.0 - 1);
            pcm[2 * i] = pcm[2 * i + 1] = (int16_t)(v * 32767);
        }
        musiclight_feed(pcm, CHUNK, 2, RATE, 200000);
        sim += 20;
        musiclight_now(sim + 200, &f);                  /* what is heard now: what was fed 200 ms ago */
        if (f.kick.at[0] && f.kick.at[0] != seen_k) { seen_k = f.kick.at[0]; kicks++; }
        if (k > 400 && f.beat_at && f.beat_at != seen_b) {
            seen_b = f.beat_at; beats++;
            double off = fmod((f.beat_at - t0 - 200) / 1000.0 + 0.25, 0.5) - 0.25;      /* to the nearest kick, s */
            if (fabs(off) < 0.06) on_kick++;
        }
        bpm = f.bpm;
    }
    char w[128];
    snprintf(w, sizeof w, "music: kicks at 120 bpm: %d in 10 s (19 to 21)", kicks); check(kicks >= 19 && kicks <= 21, w);
    snprintf(w, sizeof w, "music: the tempo found: %.1f bpm, confidence %.2f", bpm, f.confidence); check(fabsf(bpm - 120) < 2 && f.confidence > 0.2f, w);
    snprintf(w, sizeof w, "music: the last 2 s: %d of %d beats on a kick, heard 200 ms after it was fed", on_kick, beats); check(beats >= 3 && on_kick == beats, w);
    check(f.active, "music: active while it plays");
    for (int k = 0; k < 100; k++) {                     /* 2 s of 6 kHz */
        for (int i = 0; i < CHUNK; i++) pcm[2 * i] = pcm[2 * i + 1] = (int16_t)(0.3 * 32767 * sin(2 * M_PI * 6000 * (k * CHUNK + i) / (double)RATE));
        musiclight_feed(pcm, CHUNK, 2, RATE, 0); sim += 20;
    }
    musiclight_now(sim, &f);
    snprintf(w, sizeof w, "music: a 6 kHz tone lights the bands next to it (%.2f %.2f) over the bass (%.2f)", f.band[9], f.band[10], f.band[0]);
    check(f.band[9] + f.band[10] > 1.2f && f.band[0] < 0.4f, w);
    for (int k = 0; k < 100; k++) {                     /* 2 s, the tone on the left only */
        for (int i = 0; i < CHUNK; i++) { pcm[2 * i] = (int16_t)(0.3 * 32767 * sin(2 * M_PI * 440 * (k * CHUNK + i) / (double)RATE)); pcm[2 * i + 1] = 0; }
        musiclight_feed(pcm, CHUNK, 2, RATE, 0); sim += 20;
    }
    musiclight_now(sim, &f);
    snprintf(w, sizeof w, "music: left only: left %.2f over right %.2f", f.left, f.right); check(f.left > f.right + 0.3f, w);
    memset(pcm, 0, sizeof pcm);
    for (int k = 0; k < 60; k++) { musiclight_feed(pcm, CHUNK, 2, RATE, 0); sim += 20; }      /* 1.2 s of silence */
    musiclight_now(sim, &f); check(!f.active, "music: silence, not active");
    musiclight_now(sim + 2000, &f); check(!f.active, "music: nothing fed for a while: stopped");
    musiclight_want(0); musiclight_clock(NULL);

    /* the effects on readings made up here */
    float rgb[3] = { 0, 0.4f, 1 }, out[12][3]; struct music_feat m; memset(&m, 0, sizeof m);
    int over = 0;
    for (int e = RING_E_MUSIC_BEAT; e <= RING_E_MUSIC_AURORA; e++) {
        for (int k = 0; k < 400; k++) {
            m.active = 1; m.level = (k % 37) / 36.0f; m.left = m.level; m.right = 1 - m.level; m.energy = (k % 11) / 10.0f; m.bright = 0.5f;
            for (int b = 0; b < MUSIC_BANDS; b++) m.band[b] = ((k + b * 7) % 23) / 22.0f;
            m.beat_at = 1000000 + (k / 15) * 500; m.beat_no = k / 15; m.beat_ms = 500; m.confidence = k > 200;
            m.kick.at[0] = m.beat_at; m.kick.strength[0] = 1; m.snare.at[0] = m.beat_at + 250; m.snare.strength[0] = 0.8f;
            m.hat.at[0] = 1000000 + (k / 4) * 125; m.hat.strength[0] = 0.6f;
            ringlive_music_effect(e, rgb, 0.7f, 12, 1000000 + k * 33, &m, out);
            for (int j = 0; j < 12; j++) for (int c = 0; c < 3; c++) if (out[j][c] < 0 || out[j][c] > 0.7f + 1e-5f) over++;
        }
    }
    check(!over, "music effects: always within the light's level");
    ringlive_music_effect(RING_E_MUSIC_BEAT, rgb, 1, 12, 2000000, NULL, out);
    ringlive_music_effect(RING_E_MUSIC_BEAT, rgb, 1, 12, 2003000, NULL, out);
    check(out[0][2] > 0 && out[0][2] <= 0.1f, "music effects: nothing playing, a quiet breath");
    memset(&m, 0, sizeof m); m.active = 1; m.left = m.right = 1;
    for (int k = 0; k < 3; k++) ringlive_music_effect(RING_E_MUSIC_VU, rgb, 1, 12, 3000000 + k * 33, &m, out);
    int lit = 0; for (int j = 0; j < 12; j++) lit += out[j][0] + out[j][1] > 0.5f;
    check(lit == 12 && out[0][1] > 0.9f && out[6][0] > 0.9f && out[6][1] < 0.1f, "VU meter: full, green at LED 1 to red opposite");
    m.right = 0; for (int k = 0; k < 20; k++) ringlive_music_effect(RING_E_MUSIC_VU, rgb, 1, 12, 3000100 + k * 33, &m, out);
    check(out[3][0] + out[3][1] < 0.2f && out[9][0] + out[9][1] > 0.8f, "VU meter: the left alone, only LED 1's left side lit");
    memset(m.band, 0, sizeof m.band); m.band[MUSIC_BANDS - 1] = 1;
    for (int k = 0; k < 20; k++) ringlive_music_effect(RING_E_MUSIC_SPECTRUM, rgb, 1, 12, 4000000 + k * 33, &m, out);
    check(out[0][0] + out[0][1] + out[0][2] < 0.15f && out[11][0] + out[11][1] + out[11][2] > 0.8f, "spectrum: only the highs, LED 12 lit, LED 1 dark");
    memset(&m, 0, sizeof m); m.active = 1; m.kick.at[0] = 5000000; m.kick.strength[0] = 1;
    ringlive_music_effect(RING_E_MUSIC_DRUMS, rgb, 1, 12, 5000000, &m, out); float a0 = out[0][2], a6 = out[6][2];
    for (int k = 1; k <= 9; k++) ringlive_music_effect(RING_E_MUSIC_DRUMS, rgb, 1, 12, 5000000 + k * 31, &m, out);
    check(a0 > 0.8f && a6 < 0.3f && out[6][2] > 0.4f, "drums: a kick starts at LED 1, 0.28 s later it is opposite");
    memset(&m, 0, sizeof m); m.active = 1; m.beat_at = 6000000; m.beat_ms = 500; m.level = 0.3f;
    ringlive_music_effect(RING_E_MUSIC_BEAT, rgb, 1, 12, 6000000, &m, out); float on = out[0][2];
    for (int k = 1; k <= 12; k++) ringlive_music_effect(RING_E_MUSIC_BEAT, rgb, 1, 12, 6000000 + k * 33, &m, out);
    snprintf(w, sizeof w, "beat: blooms on the beat (%.2f), settles to a glow, never dark (%.2f)", on, out[0][2]);
    check(on > 0.8f && out[0][2] < 0.5f && out[0][2] > 0.15f, w);
}

static void pure(void)
{
    float blue[3] = { 0, 0, 1 }, white[3] = { 1, 1, 1 }, black[3] = { 0, 0, 0 }; unsigned char o[3];
    /* the driver's input for stock's 00F, which the Dot 3 read back as A9, 76, 31, 1D, 15 through its calibration */
    static const struct { float pwm; int want; } lv[] = { { 0.993f, 0xFD }, { 0.697f, 177 }, { 0.292f, 74 }, { 0.173f, 44 }, { 0.127f, 32 } };
    for (size_t i = 0; i < sizeof lv / sizeof lv[0]; i++) {
        ringlive_duty(blue, lv[i].pwm, o);
        char w[80]; snprintf(w, sizeof w, "blue at pwm %.3f is %02X as ledcontroller writes it (%02X)", lv[i].pwm, o[2], lv[i].want);
        check(o[0] == 0 && o[1] == 0 && o[2] == lv[i].want, w);
    }
    ringlive_duty(white, 0.993f, o); check(o[0] == 0xFD && o[1] == 0xFD && o[2] == 0xFD, "white at level 100: FD, ledcontroller's most");
    ringlive_duty(black, 0.993f, o); check(!o[0] && !o[1] && !o[2], "black is dark");
    /* as ring.c maps a design: HA's orange 255,165,0 is 255,98,0 at full duty */
    float orange[3] = { 1, 165 / 255.0f, 0 }; ringlive_duty(orange, 1, o);
    check(o[0] == 255 && o[1] == 98 && o[2] == 0, "gamma 2.2 as ring.c's (orange 255,165,0 -> FF6200 at pwm 1)");
    int over = 0;
    for (int k = 0; k < 20000; k++) {
        float s[3] = { (float)rand() / RAND_MAX * 1.5f - 0.2f, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX * 3 };
        float p = (float)rand() / RAND_MAX * 1.4f - 0.2f, pc = p < 0 ? 0 : p > 1 ? 1 : p;
        ringlive_duty(s, p, o);
        for (int c = 0; c < 3; c++) if (o[c] > (int)(255 * pc)) over++;
    }
    check(!over, "never above 255 x pwm, whatever comes in");
    unsigned char px[12][3]; memset(px, 0, sizeof px); px[0][2] = 0xFD; px[11][0] = 0x0A;
    char t[80]; ringlive_text((const unsigned char (*)[3])px, 12, t);
    check(!strcmp(t, "0000FD0000000000000000000000000000000000000000000000000000000000000A0000\n"), "the frame's text: 12 x RRGGBB, LED 0 first, a newline");

    /* the effects: each in 0..level, the ones that move moving, Rainbow and Color loop not the light's colour */
    float rgbw[3] = { 1, 0.5f, 0 }, a[12][3], b[12][3];
    for (int e = 0; e < RING_E_MUSIC_BEAT; e++) {            /* the music's: music() */
        int in = 1, moved = 0;
        for (long long t = 0; t < 12000; t += 37) {
            ringlive_effect(e, rgbw, 0.8f, 12, t, a); ringlive_effect(e, rgbw, 0.8f, 12, t + 120, b);
            for (int j = 0; j < 12; j++) for (int c = 0; c < 3; c++) { if (a[j][c] < 0 || a[j][c] > 0.8f + 1e-5f) in = 0; if (fabsf(a[j][c] - b[j][c]) > 1e-3f) moved = 1; }
        }
        char w[96]; snprintf(w, sizeof w, "effect %s: within the level, %s", ring_effects[e], e ? "moves" : "still");
        check(in && moved == (e != RING_E_NONE), w);
    }
    ringlive_effect(RING_E_NONE, rgbw, 0.5f, 12, 0, a);
    check(fabsf(a[3][0] - 0.5f) < 1e-6f && fabsf(a[3][1] - 0.25f) < 1e-6f && a[3][2] == 0, "None: the colour at the level");

    music();

    char file[300]; snprintf(file, sizeof file, "%s/t.json", dir); table(file, 102);
    FILE *f = fopen(file, "r"); char *js = calloc(1, 1 << 16); size_t n = fread(js, 1, (1 << 16) - 1, f); fclose(f);
    float pw[101];
    check(!ringlive_table(js, n, 0, pw) && fabsf(pw[100] - 0.993f) < 1e-4f && fabsf(pw[50] - 0.697f) < 1e-4f, "table: black for cap colour 0");
    check(!ringlive_table(js, n, 1, pw) && pw[100] < 0.6f, "table: white for cap colour 1");
    float lo[101]; ringlive_table(js, n, -1, lo);
    check(lo[100] == pw[100] && lo[0] <= pw[0], "table: the cap colour unknown, the dimmer of both at each level");
    check(ringlive_table("{ \"rgbTable\": { \"black\": [ { \"pwm\": 0.5 } ] } }", 46, 0, pw) == -1, "table: too short refused");
    check(ringlive_table("{ \"rgbTable\": 3 }", 17, 0, pw) == -1, "table: not one refused");
    free(js);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(dir, sizeof dir, "/tmp/ringlive_test.%d", (int)getpid());
    mkdir(dir, 0700);
    char p[300];
    snprintf(p, sizeof p, "%s/state", dir); mkdir(p, 0700); setenv("HASSMIC_STATE", p, 1);
    snprintf(p, sizeof p, "%s/state/ring", dir); mkdir(p, 0700);
    pure();

    /* the writer */
    snprintf(frame, sizeof frame, "%s/frame", dir); setenv("HASSMIC_FAKE_FRAME", frame, 1);
    setenv("HASSMIC_LED_ROOT", dir, 1);
    snprintf(p, sizeof p, "%s/led_mute_ring_config.json", dir); table(p, 102);
    snprintf(p, sizeof p, "%s/alsparams", dir); put(p, "alsgain=03 alstime=690 capcolor=0\n"); setenv("HASSMIC_FAKE_ALSPARAMS", p, 1);
    put(frame, "0000FF\n");
    check(ringlive_start(hold_fn, level_fn, changed_fn) == -1, "a frame not 12 x RRGGBB: no live ring");
    const char *left = "00000000000000000000000000000000000000000000000000000000000000000000A900\n";
    put(frame, left);
    char t[200]; char marker[300], m[200]; snprintf(marker, sizeof marker, "%s/state/ring-live", dir);
    snprintf(p, sizeof p, "%s/boot_id", dir); put(p, "boot-2\n"); setenv("HASSMIC_BOOT_ID", p, 1);
    snprintf(m, sizeof m, "boot-1\n%s", left); put(marker, m);              /* ours, of an earlier boot (a power cut) */
    check(ringlive_start(hold_fn, level_fn, changed_fn) == 0, "started after a reboot");
    get(t, sizeof t);
    check(!strcmp(t, left) && access(marker, F_OK), "  the last boot's picture: the ring left as it is (the chip was redrawn), the marker gone");
    put(marker, "boot-2\n");                                               /* this boot: init's stop killed it */
    check(ringlive_start(hold_fn, level_fn, changed_fn) == 0 && ringlive_available(), "started again in the same boot");
    msleep(150); get(t, sizeof t);
    check(!strcmp(t, left), "  the ring left until the Echo's own state is known");
    hold = 1; ringlive_begin(); msleep(150); get(t, sizeof t);
    check(!strcmp(t, left) && access(marker, F_OK), "  muted or the Echo's own showing then: left to it, the marker gone");
    hold = 0; put(marker, "boot-2\n");
    ringlive_start(hold_fn, level_fn, changed_fn); ringlive_begin(); msleep(150); get(t, sizeof t);
    check(!strcmp(t, dark) && access(marker, F_OK), "  else what a crash left on the ring (a frozen effect too) is dark again, the marker gone");

    char c[12][16] = { "#0000ff", "", "ff0000" }, e[120];
    check(ringlive_frame(c, 3, 1, 0, e, sizeof e) == 0 && ringlive_active(), "a frame from Home Assistant");
    msleep(120); get(t, sizeof t);
    /* LED 1 = the fill's start, LED 11 of the driver; LED 3 = driver's 1 */
    check(strlen(t) == 73 && !strcmp(t + 66, "0000FD\n") && !strncmp(t + 6, "FD0000", 6),
          "written: LED 1 (blue) on the driver's 11, LED 3 (red) on its 1, at level 100's most");
    struct stat ms0; check(!stat(marker, &ms0), "  the marker says this run lit the ring");

    level = 50; msleep(300); get(t, sizeof t);
    check(!strcmp(t + 66, "0000B1\n"), "level 50: B1, stock's solid_blue there");
    level = 100; msleep(300);

    /* the Echo's own pattern: hands off, then ours again */
    hold = 1; msleep(100); int w0 = ringlive_writes();
    put(frame, "00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF\n");     /* ledcontroller's */
    msleep(500); get(t, sizeof t);
    check(ringlive_writes() == w0 && !strncmp(t, "00FFFF", 6), "while the Echo's own shows: not one write");
    hold = 0; put(frame, dark);                                         /* its unset: dark */
    msleep(400); get(t, sizeof t);
    check(!strcmp(t + 66, "0000FD\n"), "after it: ours back");

    /* main.c led(): yield before ring_set, its end after.  In between, with hold not yet set (ring_set's busy comes
     * later), not one frame of ours: else one in flight could land after ledcontroller's static pattern and stay */
    ringlive_yield(); w0 = ringlive_writes();
    put(frame, dark); char cy[12][16] = { "#00ff00" }; ringlive_frame(cy, 1, 1, 0, e, sizeof e);
    msleep(400);
    check(ringlive_writes() == w0, "between ringlive_yield and its end: not one write");
    hold = 1; ringlive_yield_end(); msleep(300);
    check(ringlive_writes() == w0, "  its end with the pattern shown: still none");
    hold = 0; msleep(400); get(t, sizeof t);
    check(ringlive_writes() > w0 && !strcmp(t + 66, "00FD00\n"), "  the pattern gone: ours back");
    /* nested (the mute's yield around led()'s own): only the outer end lets ours back, and the count comes back to 0 */
    ringlive_yield(); ringlive_yield(); w0 = ringlive_writes();
    put(frame, dark); ringlive_frame(c, 3, 1, 0, e, sizeof e); ringlive_yield_end(); msleep(400);
    check(ringlive_writes() == w0, "nested yields: the inner end lets nothing through");
    ringlive_yield_end(); msleep(400); get(t, sizeof t);
    check(ringlive_writes() > w0 && !strcmp(t + 66, "0000FD\n"), "  the outer end: ours back");
    ringlive_frame(c, 3, 1, 0, e, sizeof e); msleep(300);

    /* ledcontroller writes over ours without hassmic knowing: put back within a check */
    put(frame, dark); msleep(350); get(t, sizeof t);
    check(!strcmp(t + 66, "0000FD\n"), "written over: put back");
    w0 = ringlive_writes();
    for (int k = 0; k < 12; k++) { put(frame, dark); msleep(60); }      /* something animates: no fight for ever */
    msleep(300); int w1 = ringlive_writes();
    msleep(2000);
    check(w1 - w0 <= 6 && ringlive_writes() - w1 <= 1, "something else keeps writing: live waits instead of fighting");
    msleep(3500);

    /* an animation of 1 ms frames: at most RINGLIVE_FPS a second */
    char lines[4000] = ""; for (int k = 0; k < 20; k++) strcat(lines, k % 2 ? "1:FF0000,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000\n"
                                                                         : "1:00FF00,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000\n");
    check(ringlive_animate(lines, strlen(lines), 0, 1, e, sizeof e) == 0, "an animation");
    msleep(100); w0 = ringlive_writes(); msleep(2000); int rate = (ringlive_writes() - w0) / 2;
    char w[80]; snprintf(w, sizeof w, "  %d frames a second (at most %d)", rate, RINGLIVE_FPS); check(rate <= RINGLIVE_FPS + 1 && rate >= RINGLIVE_FPS / 2, w);
    check(ringlive_animate("1:FF0000\n", 9, 0, 1, e, sizeof e) == -1, "a frame for one LED refused");
    char nm[RD_NAME]; ringlive_playing(nm, sizeof nm); check(!strcmp(nm, "animation"), "  the one before plays on");
    ringlive_stop(); msleep(200); get(t, sizeof t);
    check(!strcmp(t + 66, "0000FD\n"), "stopped: the LEDs' own frame again");

    /* twice, three frames 100 ms each: the animation's end said */
    changes = 0;
    const char *two = "100:FFFFFF,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000\n"
                      "100:000000,FFFFFF,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000\n";
    check(ringlive_animate(two, strlen(two), 2, 0.5f, e, sizeof e) == 0, "an animation twice");
    msleep(600); ringlive_playing(nm, sizeof nm); check(!nm[0] && changes > 0, "  over after twice, said");

    /* a design of the library, as drawn (LED k = the driver's k), by name */
    snprintf(p, sizeof p, "%s/state/ring/1", dir);
    put(p, "name Sunrise\nleds 12\nloop once\n500:FF8000,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000\n");
    check(ringlive_design("sunrise", 0, 1, e, sizeof e) == 0, "the design \"Sunrise\" by name");
    msleep(150); get(t, sizeof t);
    check(!strncmp(t, "FD3700", 6), "  its first LED on the driver's first, FF8000 through gamma and pwm");
    check(ringlive_design("Nothing", 0, 1, e, sizeof e) == -1, "  a name not in the library refused");
    msleep(600); ringlive_playing(nm, sizeof nm); check(!nm[0], "  once: over");

    /* a timeout */
    changes = 0;
    char c2[12][16] = { "ffffff" };
    ringlive_frame(c2, 1, 1, 1, e, sizeof e);
    msleep(1400); struct live_led l; ringlive_led_get(0, &l);
    check(!l.on && changes > 0 && !ringlive_active(), "a frame's timeout: dark after it, said");
    msleep(300); get(t, sizeof t);
    check(!strcmp(t, dark), "nothing left: the dark frame, the ring ledcontroller's again");
    char b0[12][16] = { "ff0000", "00ff00" };
    check(ringlive_frame(b0, 2, 0, 0, e, sizeof e) == 0, "a frame at brightness 0");
    msleep(200); get(t, sizeof t); ringlive_led_get(0, &l);
    check(!strcmp(t, dark) && !l.on && !ringlive_active(), "  dark, its LEDs off (not at their colour's own level)");
    w0 = ringlive_writes(); put(frame, "00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF\n");
    msleep(600); check(ringlive_writes() == w0, "handed back: ledcontroller's frames left alone");

    /* "Light ring live" on: Home Assistant's light drawn as the bottom layer, the LEDs over it */
    check(!ringlive_enabled(), "the light not drawn until \"Light ring live\" is on");
    ringlive_enable(1); check(ringlive_enabled() && light_live, "on: ring.c told to leave the light to us");
    light.on = 1; light.r = 0; light.g = 1; light.b = 0; light.fill = 50;
    msleep(250); get(t, sizeof t);
    /* fill 50 %: 6 LEDs from the fill's start (driver's 11, 0, 1 .. 4) green at FD */
    check(!strncmp(t + 66, "00FD00", 6) && !strncmp(t, "00FD00", 6) && !strncmp(t + 24, "00FD00", 6) && !strncmp(t + 30, "000000", 6),
          "the light at 50 %: green on the fill's 6 LEDs, the rest dark");
    struct live_led one = { 1, 1, 1, 0, 0 }; ringlive_led_set(1, &one);       /* LED 2 = the driver's 0 */
    msleep(150); get(t, sizeof t);
    check(!strncmp(t, "FD0000", 6) && !strncmp(t + 66, "00FD00", 6), "an LED light over it: red on its LED, the light around");
    one.on = 0; ringlive_led_set(1, &one);
    /* a fade: the light off over 400 ms, half way about half (as seen), then dark and handed back */
    ringlive_fade(400); light.on = 0;
    msleep(200); get(t, sizeof t);
    unsigned g = 0; sscanf(t + 68, "%2x", &g);
    char fw[96]; snprintf(fw, sizeof fw, "a fade to dark: half way at %02X (between 10 and F0)", g); check(g > 0x10 && g < 0xF0, fw);
    msleep(500); get(t, sizeof t);
    check(!strcmp(t, dark), "  then dark");
    light.on = 1; light.effect = RING_E_RAINBOW; light.fill = 100;
    msleep(200); w0 = ringlive_writes(); msleep(1000); rate = ringlive_writes() - w0;
    snprintf(w, sizeof w, "an effect moves on its own: %d frames in 1 s (at most %d)", rate, RINGLIVE_FPS); check(rate >= RINGLIVE_FPS / 2 && rate <= RINGLIVE_FPS + 1, w);
    flashing = 1; light.r = 1; light.g = 0; light.b = 0; msleep(1100);
    flashing = 0; light.effect = RING_E_NONE;
    /* off: everything off, ring.c's palette back */
    ringlive_enable(0); msleep(300); get(t, sizeof t);
    check(!ringlive_enabled() && !light_live && !strcmp(t, dark), "\"Light ring live\" off: dark, the palette's light back");
    /* the end: ours on the ring, then hassmic stops: the dark frame */
    ringlive_enable(1); light.on = 1; light.r = light.g = light.b = 1; msleep(250); get(t, sizeof t);
    check(!strncmp(t, "FDFDFD", 6), "white on");
    struct stat ms1; check(!stat(marker, &ms1) && ms1.st_ino == ms0.st_ino && ms1.st_mtime == ms0.st_mtime,
                           "the marker written once in all of it (frames, effects, fades, levels): /data is flash");
    ringlive_shutdown(); get(t, sizeof t);
    check(!strcmp(t, dark) && access(marker, F_OK), "shut down: the dark frame before it ends, the marker gone");

    char cmd[320]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
    printf("%s\n", fails ? "ringlive: FAILED" : "ringlive: all ok");
    return fails ? 1 : 0;
}
