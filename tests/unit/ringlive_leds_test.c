/* ringlive.c on the PC with a file per LED (board.ring_led_color, the Echo 4's LP5036) against stand-in files
 * (HASSMIC_FAKE_RING_LEDS=<dir>: <dir>/0 .. <dir>/23 and <dir>/power0, power1, the chips' ledhven) and a brightness
 * table of laser's shape (every entry pwm 1, the level in "current", which stays ledcontroller's).  What the HAL does,
 * read out of libacehal_led.so: "%d %d %d" per LED, then "1"/"0" into both supplies when the ring goes lit or dark.
 * Checked: no files, no live ring; a frame from Home Assistant on the right LEDs as "R G B" at full (pwm 1), the supply
 * on; only the LEDs that changed written again; what ledcontroller wrote while its own pattern showed put back whole
 * after it; a second start while live changes nothing; dark at the end: every LED 0 0 0 and the supply off.  Then, in a
 * child, files that do not read back (laser's are write-only for ledcontroller): what another writer put on a LED
 * written over within a second or so, and SIGTERM's dark frame, every LED and the supply whatever was written before */
#include "ringlive.h"
#include "ring.h"
#include "ringdesign.h"
#include "board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const struct board board = { .volume_steps = 30 };
const struct ring_slot ring_slots[] = { { "listen", "active-start", RING_LISTEN } };
const char *const ring_effects[] = { "None", "Pulse", "Spin", "Blink", "Rainbow", RING_LIVE_EFFECTS };
const int ring_neffects = 5 + RING_LIVE_NEFFECTS;
const int ring_nslots = 1;
int ring_leds(void) { return 24; }
int ring_rotation(int set) { (void)set; return 0; }
int ring_check(const char *a, size_t n, char *e, size_t c) { (void)a; (void)n; (void)e; (void)c; return 0; }
int ring_slot_available(int i) { (void)i; return 1; }
int ring_slot_live(int i) { (void)i; return 0; }
int ring_designs_live(void) { return 0; }
char *ring_stock(int i, size_t *n) { (void)i; (void)n; return NULL; }
void ring_reload(void) {}
int ring_band_rows(int set) { (void)set; return -1; }
static struct ring_light light = { .brightness = 1, .color_brightness = 1, .r = 1, .g = 1, .b = 1, .fill = 100 };
int ring_light_now(struct ring_light *l) { *l = light; return 0; }
void ring_light_live(int on) { (void)on; }
int ring_live_acs(void) { return 0; }
int ring_live_put(const char *rgb) { (void)rgb; return -1; }
void ring_live_show(int on) { (void)on; }
int ring_live_on(void) { return 0; }
int ring_custom(void) { return 1; }

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static volatile int hold;
static int hold_fn(void) { return hold; }
static int level_fn(void) { return 100; }
static char dir[256], leds_dir[300];
static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static void put(const char *file, const char *text) { FILE *f = fopen(file, "w"); fputs(text, f); fclose(f); }
static void get(const char *file, char *out, size_t cap) { FILE *f = fopen(file, "r"); size_t k = f ? fread(out, 1, cap - 1, f) : 0; if (f) fclose(f); out[k] = 0; }
static void led(int i, char *out) { char p[340]; snprintf(p, sizeof p, "%s/%d", leds_dir, i); get(p, out, 32); }
static void power(int i, char *out) { char p[340]; snprintf(p, sizeof p, "%s/power%d", leds_dir, i); get(p, out, 8); }
static int all_dark(void) { char t[32]; for (int i = 0; i < 24; i++) { led(i, t); if (strcmp(t, "0 0 0")) return 0; } return 1; }

/* the child: a process of its own (fork alone would inherit a live ring without its writer thread), its environment the
 * parent's; lights LEDs 1 and 3, says so on fd 3, waits for SIGTERM */
static int child(void)
{
    char k[24][16] = { "#0000ff", "", "ff0000" }, ke[120];
    if (ringlive_start(hold_fn, level_fn, NULL)) return 2;
    ringlive_begin();
    ringlive_frame(k, 3, 1, 0, ke, sizeof ke);
    msleep(200);
    if (write(3, "s", 1) != 1) return 3;
    for (;;) pause();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1 && !strcmp(argv[1], "child")) return child();
    snprintf(dir, sizeof dir, "/tmp/ringlive_leds_test.%d", (int)getpid());
    mkdir(dir, 0700);
    char p[400], t[32], a[32], b[32], e[120];
    snprintf(p, sizeof p, "%s/state", dir); mkdir(p, 0700); setenv("HASSMIC_STATE", p, 1);
    snprintf(p, sizeof p, "%s/boot_id", dir); put(p, "boot-1\n"); setenv("HASSMIC_BOOT_ID", p, 1);
    setenv("HASSMIC_LED_ROOT", dir, 1);
    snprintf(p, sizeof p, "%s/alsparams", dir); put(p, "capcolor=0\n"); setenv("HASSMIC_FAKE_ALSPARAMS", p, 1);
    /* laser's table: pwm 1 at every level, the current in its own field */
    snprintf(p, sizeof p, "%s/led_mute_ring_config.json", dir);
    FILE *f = fopen(p, "w");
    fprintf(f, "{ \"cap_color\": [ { \"color\": \"black\", \"cap_color_number\": 0 } ], \"muteTable\": { \"black\": [ 470 ] },\n  \"rgbTable\": { \"black\": [");
    for (int i = 0; i < 102; i++) fprintf(f, "%s{ \"pwm\": 1, \"current\": %d }", i ? ", " : "", 43 + i * 2 > 255 ? 255 : 43 + i * 2);
    fprintf(f, "] } }\n"); fclose(f);

    snprintf(leds_dir, sizeof leds_dir, "%s/leds", dir); mkdir(leds_dir, 0700);
    setenv("HASSMIC_FAKE_RING_LEDS", leds_dir, 1);
    check(ringlive_start(hold_fn, level_fn, NULL) == -1, "no colour files: no live ring");
    for (int i = 0; i < 24; i++) { snprintf(p, sizeof p, "%s/%d", leds_dir, i); put(p, "0 0 0"); }
    for (int i = 0; i < 2; i++) { snprintf(p, sizeof p, "%s/power%d", leds_dir, i); put(p, "0"); }
    check(ringlive_start(hold_fn, level_fn, NULL) == 0 && ringlive_available() && ringlive_leds() == 24, "24 colour files and two supplies: live");
    ringlive_begin();

    char c[24][16] = { "#0000ff", "", "ff0000" };
    check(ringlive_frame(c, 3, 1, 0, e, sizeof e) == 0, "a frame from Home Assistant");
    for (int i = 0; i < 200; i++) { msleep(10); power(0, t); if (!strcmp(t, "1")) break; }
    led(23, a); led(1, b);
    char w[200]; snprintf(w, sizeof w, "written: LED 1 (blue) on the driver's 23 as \"%s\", LED 3 (red) on its 1 as \"%s\" (pwm 1: full)", a, b);
    check(!strcmp(a, "0 0 255") && !strcmp(b, "255 0 0"), w);
    led(5, t); check(!strcmp(t, "0 0 0"), "  the others 0 0 0");
    power(0, a); power(1, b); check(!strcmp(a, "1") && !strcmp(b, "1"), "  both supplies on (ledhven 1)");

    /* only what changed: a mark in LED 5's file stays while LED 3 changes colour */
    snprintf(p, sizeof p, "%s/5", leds_dir); put(p, "mark");
    char c2[24][16] = { "#0000ff", "", "00ff00" };
    ringlive_frame(c2, 3, 1, 0, e, sizeof e); msleep(300);
    led(1, a); led(5, b);
    check(!strcmp(a, "0 255 0") && !strcmp(b, "mark"), "a change: only that LED written again (LED 5 untouched)");

    /* the Echo's own pattern: ledcontroller writes all of them, then the dark frame and the supply off; ours back whole */
    hold = 1; msleep(300);
    for (int i = 0; i < 24; i++) { snprintf(p, sizeof p, "%s/%d", leds_dir, i); put(p, "0 0 0"); }
    snprintf(p, sizeof p, "%s/power0", leds_dir); put(p, "0"); snprintf(p, sizeof p, "%s/power1", leds_dir); put(p, "0");
    hold = 0; msleep(500);
    led(23, a); led(1, b); power(0, t);
    check(!strcmp(a, "0 0 255") && !strcmp(b, "0 255 0") && !strcmp(t, "1"), "after the Echo's own: every LED of ours and the supply back");
    led(5, t); check(!strcmp(t, "0 0 0"), "  LED 5 dark again (written whole, not only what changed)");

    check(ringlive_start(hold_fn, level_fn, NULL) == 0, "started again while live: 0, nothing reopened");
    ringlive_frame(c2, 3, 1, 0, e, sizeof e); msleep(300); led(1, a);
    check(!strcmp(a, "0 255 0"), "  and the ring still written");

    ringlive_shutdown();
    power(0, a); power(1, b);
    check(all_dark() && !strcmp(a, "0") && !strcmp(b, "0"), "shut down: every LED 0 0 0, both supplies off");

    /* write-only files: opened before they turn readable again for the checks (the child keeps its descriptors) */
    for (int i = 0; i < 24; i++) { snprintf(p, sizeof p, "%s/%d", leds_dir, i); put(p, "0 0 0"); chmod(p, 0200); }
    for (int i = 0; i < 2; i++) { snprintf(p, sizeof p, "%s/power%d", leds_dir, i); put(p, "0"); chmod(p, 0200); }
    snprintf(p, sizeof p, "%s/state/ring-live", dir); unlink(p);
    int up[2]; if (pipe(up)) return 1;
    pid_t kid = fork();
    if (kid == 0) {
        close(up[0]); if (up[1] != 3) { dup2(up[1], 3); close(up[1]); }
        execl("/proc/self/exe", argv[0], "child", (char *)NULL);
        _exit(4);
    }
    close(up[1]); char go; int started = read(up[0], &go, 1) == 1;
    msleep(400);
    for (int i = 0; i < 24; i++) { snprintf(p, sizeof p, "%s/%d", leds_dir, i); chmod(p, 0600); }
    for (int i = 0; i < 2; i++) { snprintf(p, sizeof p, "%s/power%d", leds_dir, i); chmod(p, 0600); }
    led(23, a); power(0, b);
    check(started && !strcmp(a, "0 0 255") && !strcmp(b, "1"), "write-only files: written LED by LED, the supply on");
    snprintf(p, sizeof p, "%s/5", leds_dir); put(p, "9 9 9"); snprintf(p, sizeof p, "%s/power1", leds_dir); put(p, "0");
    msleep(1500); led(5, a); power(1, b);
    check(!strcmp(a, "0 0 0") && !strcmp(b, "1"), "  another writer's LED and supply written over within the second, nothing to read back");
    kill(kid, SIGTERM); int st = 0; waitpid(kid, &st, 0);
    power(0, a); power(1, b);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM && all_dark() && !strcmp(a, "0") && !strcmp(b, "0"),
          "SIGTERM: every LED 0 0 0 and both supplies off before it ends");

    char cmd[320]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
    printf("%s\n", fails ? "ringlive_leds: FAILED" : "ringlive_leds: all ok");
    return fails ? 1 : 0;
}
