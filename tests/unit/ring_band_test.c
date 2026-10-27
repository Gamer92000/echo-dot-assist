/* ring_band.c on the PC: the light band's patterns as they reach the renderer.  A shell script stands in for ringbar.dex
 * (HASSMIC_FAKE_BAND: says "ready 960 <rows>", keeps every frame), the core is stubbed.  Checks what each pattern puts
 * in the middle of the band, the priorities, the volume fill, Home Assistant's light, designs from the settings page
 * (ringdesign.c, linked with it), the screen calls, and the band switched off; build/ring_band.ppm shows the frames one row each, top to bottom, for the eye. */
#define _GNU_SOURCE
#include "ring.h"
#include "ringdesign.h"
#include "board.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

const struct board board = { .volume_steps = 100 };
static int muted, dnd_on, spawns_wake, spawns_timeout, last_timeout;
int core_muted(void) { return muted; }
int core_dnd(int set) { (void)set; return dnd_on; }
static int brightness = 100;
int core_led_brightness(int set) { (void)set; return brightness; }
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
/* until cond holds, 3 s at most: the frames come through a pipe and cat, late on a busy PC (make -j unit) */
#define WAIT(cond) for (int w_ = 0; w_ < 150 && !(cond); w_++) ms(20)
static void rm_rf(const char *dir) { char c[320]; snprintf(c, sizeof c, "rm -rf '%s'", dir); if (system(c)) {} }

/* the stand-in renderer in dir, keeping its frames in dir/frames (px_at's) */
static void band_in(const char *dir, char *script, size_t n)
{
    snprintf(frames, sizeof frames, "%s/frames", dir);
    snprintf(script, n, "%s/ringbar", dir);
    FILE *f = fopen(script, "w");
    fprintf(f, "#!/bin/sh\nif [ \"$1\" = 0 ]; then echo off; echo off >> %s/log; exit 0; fi\n"
               "ls /proc/$$/fd > %s/fds; echo \"ready %d $1\"; echo \"rows $1\" >> %s/log; exec cat >> %s\n", dir, dir, W, dir, frames);
    fclose(f); chmod(script, 0755);
    setenv("HASSMIC_FAKE_BAND", script, 1);
}

static int same_files(const char *a, const char *b)
{
    char x[1 << 14], y[1 << 14]; FILE *f = fopen(a, "r"), *g = fopen(b, "r");
    size_t m = f ? fread(x, 1, sizeof x, f) : 0, n = g ? fread(y, 1, sizeof y, g) : 0;
    if (f) fclose(f);
    if (g) fclose(g);
    return f && g && m == n && !memcmp(x, y, m);
}

/* "again <state>": a start of hassmic on the state the first run left (its band in a folder of its own): the design
 * applied there shows, and a draft that was never installed goes back (rd_start) */
static int again(const char *st)
{
    char dir[] = "/tmp/ring_band_XXXXXX", script[300], a[320], b[320];
    if (!mkdtemp(dir)) return 1;
    band_in(dir, script, sizeof script);
    setenv("HASSMIC_STATE", st, 1);
    ring_band_rows(ROWS);
    ring_start(NULL, NULL); rd_start();
    ring_set("active-thinking");
    WAIT(px_at(440) == 0xff0000 && px_at(520) == 0x00ff00);
    check(px_at(440) == 0xff0000 && px_at(520) == 0x00ff00, "after a restart: the design applied before, from state/ring-installed");
    snprintf(a, sizeof a, "%s/ring-applied", st); snprintf(b, sizeof b, "%s/ring-installed", st);
    check(same_files(a, b), "after a restart: a draft never installed is back to what is");
    ring_band_rows(0); ms(300); rm_rf(dir);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "again")) return again(argv[2]);
    char dir[] = "/tmp/ring_band_XXXXXX", script[300];
    if (!mkdtemp(dir)) return 1;
    band_in(dir, script, sizeof script);
    char st[300]; snprintf(st, sizeof st, "%s/state", dir); mkdir(st, 0700);
    setenv("HASSMIC_STATE", st, 1);                 /* before ring_start: its first ring_reload reads it */
    FILE *f;
    int extra = open("/dev/null", O_RDONLY);        /* stands in for hassmic's sockets: none of it for the renderer */

    check(ring_band_rows(-1) == 10, "the band: 10 rows by default (stock's line)");
    ring_band_rows(ROWS);
    ring_start(NULL, NULL);
    for (int i = 0; i < 100 && px_at(480) < 0; i++) ms(20);
    check(px_at(480) == 0 && px_at(0) == 0, "nothing shows: black");
    check(ring_custom(), "Home Assistant gets the light");
    { char p[320], fdn[16], l[16]; int seen = 0; snprintf(p, sizeof p, "%s/fds", dir); snprintf(fdn, sizeof fdn, "%d\n", extra);
      f = fopen(p, "r"); while (f && fgets(l, sizeof l, f)) seen |= !strcmp(l, fdn); if (f) fclose(f);
      check(extra > 2 && f && !seen, "the renderer gets its stdin and stdout, none of hassmic's other files"); }
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

    struct ring_light l = { .on = 1, .brightness = 1, .color_brightness = 1, .r = 1, .g = 0.5f, .b = 0, .fill = 100 };  /* yellowish, no effect */
    ring_light_set(&l, 0); ms(200);
    c = px_at(10);
    check(r_(c) == 255 && g_(c) > 120 && g_(c) < 135 && b_(c) == 0 && px_at(950) == c, "Home Assistant's light: its colour across the band");
    brightness = 50;                                /* "LED brightness" while the picture stands still */
    WAIT(r_(px_at(10)) < 140);
    check(r_(px_at(10)) >= 126 && r_(px_at(10)) <= 129, "a still light at half brightness: dimmed too, not only a picture that moves");
    brightness = 100;
    WAIT(px_at(10) == c);
    ring_set("call_connected"); ms(600);
    int green = 0; for (int x = 40; x < W; x += 80) if (g_(px_at(x)) > 150 && r_(px_at(x)) < 30) green++;
    check(green >= 2 && r_(px_at(10)) < 30 && r_(px_at(950)) < 30, "a call over the light: green going round, the light gone meanwhile");
    ring_unset("call_connected"); ms(200);
    check(px_at(10) == c, "the light back after the call");
    l.on = 0; ring_light_set(&l, 0); ms(200);
    check(px_at(10) == 0, "light off: black");

    /* Designs (ringdesign.c), as the settings page makes them: on the band as soon as applied, flattened with LEDs 10
     * and 11 (where stock's listening ring points) in the middle */
    char err[200]; int id = 0, id24 = 0, end = 0; size_t sn = 0;
    const char *two = "name Two\nleds 12\nloop 0\n"
        "500:000000,000000,000000,000000,000000,000000,000000,000000,000000,000000,FF0000,00FF00\n"
        "500:0000FF,0000FF,0000FF,0000FF,0000FF,0000FF,0000FF,0000FF,0000FF,0000FF,0000FF,0000FF\n";
    const char *once = "name Once\nleds 12\nloop once\n300:FFFFFF,FFFFFF,FFFFFF,FFFFFF,FFFFFF,FFFFFF,FFFFFF,FFFFFF,FFFFFF,FFFFFF,FFFFFF,FFFFFF\n";
    check(!rd_save(0, two, strlen(two), &id, err, sizeof err) && !rd_save(0, once, strlen(once), &end, err, sizeof err),
          "designs saved");
    char as[80]; snprintf(as, sizeof as, "think %d\nvolume %d\nend %d\n", id, id, end);
    check(!rd_assign(as, err, sizeof err) && !rd_apply(err, sizeof err), "applied: nothing for root to install");
    char *js = malloc(1 << 16); rd_status_json(js, 1 << 16);
    check(strstr(js, "\"band\":true") && strstr(js, "\"state\":\"applied\""), "status: the band, applied at once");
    ring_set("active-thinking");
    WAIT(px_at(440) == 0xff0000 && px_at(520) == 0x00ff00 && px_at(40) == 0);
    check(px_at(440) == 0xff0000 && px_at(520) == 0x00ff00 && px_at(40) == 0 && px_at(920) == 0,
          "a design: LED 10 left of the middle, 11 right of it, the far side at the ends");
    WAIT(px_at(40) == 0x0000ff);
    check(px_at(40) == 0x0000ff && px_at(920) == 0x0000ff, "a design: its second frame");
    snprintf(as, sizeof as, "talk %d\n", end); rd_assign(as, err, sizeof err); rd_apply(err, sizeof err); ms(60);
    check(px_at(40) == 0x0000ff, "an Apply that leaves its design alone: thinking goes on, not from its start");
    ring_set("volume_step-50");
    WAIT(px_at(440) == 0xff0000 && px_at(360) == 0 && px_at(520) == 0);
    check(px_at(440) == 0xff0000 && px_at(360) == 0 && px_at(520) == 0, "volume: the fill in the design's first frame's colours");
    ring_unset("volume_step-50"); ring_unset("active-thinking");
    ring_set("active-end");
    WAIT(px_at(40) == 0xffffff);
    check(px_at(40) == 0xffffff, "a design played once: shows");
    WAIT(px_at(40) == 0);
    check(px_at(40) == 0, "... and is gone after its 300 ms");
    const char *held = "name Held\nleds 12\nloop 0\n200:00FF00,00FF00,00FF00,00FF00,00FF00,00FF00,00FF00,00FF00,00FF00,00FF00,00FF00,00FF00\n";
    int hid = 0; rd_save(0, held, strlen(held), &hid, err, sizeof err);
    snprintf(as, sizeof as, "end %d\n", hid); rd_assign(as, err, sizeof err); rd_apply(err, sizeof err);
    ring_set("active-end"); ms(900);
    check(px_at(40) == 0x00ff00, "a looping design for listening's end: still on after stock's 600 ms");
    ring_unset("active-end");
    WAIT(px_at(40) == 0);
    check(px_at(40) == 0, "... until it is unset");

    /* a restart: another process on this state; a draft left in state/ring-applied first, as a start after a failed
     * write would find it */
    char ap[320], in[320]; snprintf(ap, sizeof ap, "%s/ring-applied", st); snprintf(in, sizeof in, "%s/ring-installed", st);
    f = fopen(ap, "w"); fputs("hassmic ring applied 1\n", f); fclose(f);
    pid_t pid = fork();
    if (!pid) { execl("/proc/self/exe", "ring_band_test", "again", st, (char *)NULL); _exit(127); }
    int status = 0; waitpid(pid, &status, 0);
    check(WIFEXITED(status) && !WEXITSTATUS(status), "a restart on the same state (the checks above, \"after a restart\")");

    /* Apply when its files cannot be written: refused, saying why, the band as it was */
    unlink(in); mkdir(in, 0700);                    /* a folder where the file goes: the rename fails */
    snprintf(as, sizeof as, "end 0\n"); rd_assign(as, err, sizeof err);
    check(rd_apply(err, sizeof err) && strstr(err, "cannot write the designs"), "Apply that cannot write: refused, saying why");
    ring_set("active-thinking");
    WAIT(px_at(440) == 0xff0000);
    check(px_at(440) == 0xff0000, "... and the band keeps what it showed");
    ring_unset("active-thinking");
    rmdir(in); check(!rd_apply(err, sizeof err), "... and goes through once it can");
    const char *wide = "name Wide\nleds 24\nloop 0\n1000:FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,"
        "FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000,FF0000\n";
    rd_save(0, wide, strlen(wide), &id24, err, sizeof err);
    snprintf(as, sizeof as, "talk %d\n", id24); rd_assign(as, err, sizeof err);
    check(rd_apply(err, sizeof err) && strstr(err, "24 LEDs"), "a design for 24 LEDs: refused");
    snprintf(as, sizeof as, "talk 0\nthink 0\nvolume 0\nend 0\n"); rd_assign(as, err, sizeof err); rd_apply(err, sizeof err);
    ring_set("active-thinking");
    WAIT(b_(px_at(480)) > 100 && r_(px_at(480)) < 160);
    check(b_(px_at(480)) > 100 && r_(px_at(480)) < 160, "assignments taken back: the band's own thinking again");
    ring_unset("active-thinking"); ms(100);

    /* the band's patterns as files, for the page to start from: thinking's first 100 ms (its turns' own time) with
     * LED 0 (band position 7) cyan, LED 1 blue, then the other way round; the volume a step per LED */
    char *t = rd_stock("think", &sn);
    check(t && !strncmp(t, "loop\n100:00FFFF,0000FF,", 23) && strstr(t, "\n100:0000FF,00FFFF,"),
          "stock's thinking as a file, in the design's LED order, turning every 100 ms as it does");
    free(t);
    int most = 0, all_there = 1;                    /* every slot's: within what a design holds (the page cuts the rest) */
    for (int i = 0; i < ring_nslots; i++) {
        char *x = rd_stock(ring_slots[i].id, &sn); int frames = 0;
        if (!x) { all_there = 0; continue; }
        for (char *l = x; *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : l + strlen(l)) frames += *l >= '0' && *l <= '9';
        if (frames > most) most = frames;
        free(x);
    }
    check(all_there && most > 0 && most <= RD_FRAMES, "every pattern as a file, none longer than a design may be");
    printf("     (the longest: %d frames)\n", most);
    t = rd_stock("mute", &sn);                    /* its dark red, 0.6 as seen: the driver's duty 0x53 (gamma 2.2), which
                                                   * the page turns back into 153 (web/ring.js stockDesign) */
    const char *lp = t ? strstr(t, "loop\n") : NULL, *px = lp ? strchr(lp + 5, ':') : NULL;
    check(px && !strncmp(px, ":530000,530000,", 15) && fabs(255 * pow(0x53 / 255.0, 1 / 2.2) - 0.6 * 255) < 1,
          "stock's mute as a file: mid levels through gamma 2.2 and back");
    free(t);
    t = rd_stock("volume", &sn);
    check(t && !strncmp(t, "step 01\n1000:", 13) && strstr(t, "step 12\n"), "stock's volume: twelve steps");
    const char *s1 = t ? strchr(t, ':') : NULL;   /* step 1: band position 0 alone, the design's LED 5 */
    check(s1 && !strncmp(s1, ":000000,000000,000000,000000,000000,FFFFFF,000000,", 50), "... step 1 lights one LED, no fraction of the next");
    free(t); free(js);
    ring_set("hassmic_assist_50");
    WAIT(px_at(40) == 0x00ffff && px_at(600) == 0);
    check(px_at(40) == 0x00ffff && px_at(600) == 0, "the replies' own volume: cyan");
    ring_unset("hassmic_assist_50"); ms(200);

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
    rm_rf(dir);
    printf("%ld frames, build/ring_band.ppm\n", n);
    return fails ? 1 : 0;
}
