/* ringlive.c on the PC: the light ring live against a stand-in frame file (HASSMIC_FAKE_FRAME) and a brightness table
 * (HASSMIC_LED_ROOT).  The pure parts against what a Dot 3 measured (PLAN.md "Light ring live", 2026-11-12): stock's
 * solid_blue (00F) at levels 100/50/25/10/1 is what the driver was written, never above 255 x pwm, the frame's text, the
 * table by cap colour.  Then the writer: a frame, waiting for the Echo's own pattern and taking the ring back after it,
 * a frame ledcontroller wrote over put back, the rate under an animation of 1 ms frames, a timeout, a design of the
 * library, the dark frame and the marker at the end, the marker a crash left. */
#define _GNU_SOURCE
#include "ringlive.h"
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
    put(frame, "00000000000000000000000000000000000000000000000000000000000000000000A900\n");
    char marker[300]; snprintf(marker, sizeof marker, "%s/state/ring-live", dir); put(marker, "1\n");
    check(ringlive_start(hold_fn, level_fn, changed_fn) == 0 && ringlive_available(), "started");
    char t[200]; get(t, sizeof t);
    check(!strcmp(t, dark) && access(marker, F_OK), "what a crash left on the ring is dark again, the marker gone");

    char c[12][16] = { "#0000ff", "", "ff0000" }, e[120];
    check(ringlive_frame(c, 3, 1, 0, e, sizeof e) == 0 && ringlive_active(), "a frame from Home Assistant");
    msleep(120); get(t, sizeof t);
    /* LED 1 = the fill's start, LED 11 of the driver; LED 3 = driver's 1 */
    check(strlen(t) == 73 && !strcmp(t + 66, "0000FD\n") && !strncmp(t + 6, "FD0000", 6),
          "written: LED 1 (blue) on the driver's 11, LED 3 (red) on its 1, at level 100's most");
    check(!access(marker, F_OK), "  the marker says ours is on the ring");

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
    check(!strcmp(t, dark) && access(marker, F_OK), "nothing left: the dark frame, the ring ledcontroller's again (no marker)");
    w0 = ringlive_writes(); put(frame, "00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF00FFFF\n");
    msleep(600); check(ringlive_writes() == w0, "handed back: ledcontroller's frames left alone");

    char cmd[320]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
    printf("%s\n", fails ? "ringlive: FAILED" : "ringlive: all ok");
    return fails ? 1 : 0;
}
