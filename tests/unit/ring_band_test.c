/* ring_band.c on the PC: the light band's patterns as they reach the renderer.  A shell script stands in for ringbar.dex
 * (HASSMIC_FAKE_BAND: says "ready 960 <rows>", keeps every frame), the core is stubbed.  Checks what each pattern puts
 * in the middle of the band, the priorities, the volume fill, Home Assistant's light, the screen calls, and the band
 * switched off; build/ring_band.ppm shows the frames one row each, top to bottom, for the eye. */
#define _GNU_SOURCE
#include "ring.h"
#include "board.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const struct board board = { .volume_steps = 100 };
static int muted, dnd_on, spawns_wake, spawns_timeout, last_timeout;
int core_muted(void) { return muted; }
int core_dnd(int set) { (void)set; return dnd_on; }
int core_led_brightness(int set) { (void)set; return 100; }
void core_spawn(char *const argv[])
{
    if (!strcmp(argv[0], "/system/bin/service") && !strcmp(argv[3], "9")) spawns_wake++;
    if (!strcmp(argv[0], "/system/bin/settings")) { spawns_timeout++; last_timeout = atoi(argv[4]); }
}

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

#define W 960
#define ROWS 2
static char frames[300];

/* the newest frame's pixel at x: 0xRRGGBB, -1 none */
static long px_at(int x)
{
    FILE *f = fopen(frames, "rb"); struct stat st;
    if (!f || fstat(fileno(f), &st) || st.st_size < W * ROWS * 4) { if (f) fclose(f); return -1; }
    long off = (st.st_size / (W * ROWS * 4) - 1) * (long)(W * ROWS * 4) + x * 4L;
    unsigned char p[4]; fseek(f, off, SEEK_SET);
    long v = fread(p, 1, 4, f) == 4 ? (long)p[2] << 16 | p[1] << 8 | p[0] : -1;
    fclose(f);
    return v;
}
static long nframes(void) { struct stat st; return stat(frames, &st) ? 0 : st.st_size / (W * ROWS * 4); }
static int r_(long v) { return v >> 16 & 255; }
static int g_(long v) { return v >> 8 & 255; }
static int b_(long v) { return v & 255; }
static void ms(int n) { usleep(n * 1000); }

int main(void)
{
    char dir[] = "/tmp/ring_band_XXXXXX", script[300];
    if (!mkdtemp(dir)) return 1;
    snprintf(frames, sizeof frames, "%s/frames", dir);
    snprintf(script, sizeof script, "%s/ringbar", dir);
    FILE *f = fopen(script, "w");
    fprintf(f, "#!/bin/sh\nif [ \"$1\" = 0 ]; then echo off; echo off >> %s/log; exit 0; fi\n"
               "echo \"ready %d $1\"; echo \"rows $1\" >> %s/log; exec cat >> %s\n", dir, W, dir, frames);
    fclose(f); chmod(script, 0755);
    setenv("HASSMIC_FAKE_BAND", script, 1);

    check(ring_band_rows(-1) == 10, "the band: 10 rows by default (stock's line)");
    ring_band_rows(ROWS);
    ring_start(NULL);
    for (int i = 0; i < 100 && px_at(480) < 0; i++) ms(20);
    check(px_at(480) == 0 && px_at(0) == 0, "nothing shows: black");
    check(ring_custom(), "Home Assistant gets the light");
    check(spawns_timeout == 1 && last_timeout == 300000, "the screen sleeps after 5 min, as stock's");

    ring_set("active-start");
    ring_level(-200); ms(700);                      /* a loud talker */
    long c = px_at(480), e = px_at(30);
    check(g_(c) > 200 && b_(c) > 200 && r_(c) < 30, "listening: cyan at the talker (the centre), as the Dot's ring");
    check(b_(e) > 200 && g_(e) < 60, "listening: blue out to the ends, the whole width");
    check(g_(px_at(250)) > 150, "listening, loud: the cyan wider");
    check(spawns_wake >= 1, "a pattern wakes the screen");
    ring_level(-600); ms(1200);
    check(g_(px_at(250)) < 60 && g_(px_at(480)) > 200, "listening, quiet: cyan only in the middle");
    ring_unset("active-start"); ring_set("active-thinking"); ms(300);
    c = px_at(480);
    check(b_(c) > 100 && r_(c) < 160, "thinking: blue");
    ring_set("volume_step-40"); ms(200);
    check(px_at(100) == 0xffffff && px_at(250) == 0xffffff && px_at(600) == 0,
          "volume over thinking: 40 % of the LEDs white from the left, the rest dark");
    ring_unset("volume_step-40"); ring_unset("active-thinking"); ring_set("active-end"); ms(300);
    check(px_at(480) == 0 && b_(px_at(10)) > 100, "active-end: dark from the talker outwards");
    ms(500);
    check(px_at(10) == 0, "active-end: gone after 0.6 s");

    ring_set("mics-off_on"); muted = 1; ms(1300);
    c = px_at(480);
    check(r_(c) > 120 && g_(c) < 10 && b_(c) < 10 && px_at(5) == c, "mic off: the Dot's dark red, the whole width");
    check(last_timeout == 60000, "mic off: the screen sleeps after 1 min");
    ring_set("ready-timer"); ms(100);
    check(r_(px_at(480)) > 120, "mic off wins over a ringing timer");
    ring_unset("mics-off_on"); muted = 0; ms(300);
    c = px_at(480); long d = px_at(0);
    check(b_(c) > 100 && r_(c) < 50 && b_(d) > 100, "the timer: cyan and blue, the whole width");
    ring_unset("ready-timer"); ms(1200);
    check(last_timeout == 300000, "mic on again: 5 min");

    struct ring_light l = { 1, 1, 1, 1, 0.5f, 0, 0 };   /* on, full, yellowish, no effect */
    ring_light_set(&l, 0); ms(200);
    c = px_at(10);
    check(r_(c) == 255 && g_(c) > 120 && g_(c) < 135 && b_(c) == 0 && px_at(950) == c, "Home Assistant's light: its colour across the band");
    ring_set("call_connected"); ms(600);
    int green = 0; for (int x = 40; x < W; x += 80) if (g_(px_at(x)) > 150 && r_(px_at(x)) < 30) green++;
    check(green >= 2 && r_(px_at(10)) < 30 && r_(px_at(950)) < 30, "a call over the light: green going round, the light gone meanwhile");
    ring_unset("call_connected"); ms(200);
    check(px_at(10) == c, "the light back after the call");
    l.on = 0; ring_light_set(&l, 0); ms(200);
    check(px_at(10) == 0, "light off: black");

    long n = nframes(); ms(500);
    check(nframes() == n, "nothing moving: no frames sent");
    ring_band_rows(0); ms(500);
    f = fopen(script, "r"); fclose(f);
    char logp[300], line[64] = ""; snprintf(logp, sizeof logp, "%s/log", dir);
    f = fopen(logp, "r"); while (f && fgets(line, sizeof line, f)) {} if (f) fclose(f);
    check(!strncmp(line, "off", 3), "band switched off: the renderer runs with 0 (the overscan given back)");

    /* the frames as an image, one row of the band per frame */
    n = nframes(); f = fopen(frames, "rb"); FILE *o = fopen("build/ring_band.ppm", "wb");
    if (f && o) {
        fprintf(o, "P6\n%d %ld\n255\n", W, n);
        unsigned char in[W * 4], out[W * 3];
        for (long i = 0; i < n; i++) {
            if (fread(in, 1, sizeof in, f) != sizeof in) break;
            fseek(f, (ROWS - 1) * W * 4L, SEEK_CUR);
            for (int x = 0; x < W; x++) { out[3 * x] = in[4 * x + 2]; out[3 * x + 1] = in[4 * x + 1]; out[3 * x + 2] = in[4 * x]; }
            fwrite(out, 1, sizeof out, o);
        }
    }
    if (f) fclose(f);
    if (o) fclose(o);
    printf("%ld frames, build/ring_band.ppm\n", n);
    return fails ? 1 : 0;
}
