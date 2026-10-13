/* spkgain.c against a fake control (HASSMIC_FAKE_SPEAKER_GAIN) in the shapes of the Echo Dot 2's codec (1 dB a step,
 * 6..35 above stock), the Echo Dot 3's amplifier (0.5 dB a step, 16..20) and the Echo Show 5's codec (1.5 dB a step,
 * 29..39): dB to steps, the top, the watcher putting stock back and leaving other values alone.  Each shape in a child
 * of its own: the control is probed once. */
#include "board.h"
#include "spkgain.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

const struct board board;                      /* the fake decides, whatever the board */

static char file[64];
static int fails;

static int ctl(void) { FILE *f = fopen(file, "r"); int v = -1; if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); } return v; }
static void put(int v) { FILE *f = fopen(file, "w"); if (f) { fprintf(f, "%d\n", v); fclose(f); } }
/* the control as wanted within 5 s: the thread on the card's events writes it back (on a loaded machine not within a
 * fixed 0.6 s) */
static int becomes(int want) { for (int i = 0; i < 100 && ctl() != want; i++) usleep(50000); return ctl() == want; }
static void check(int ok, const char *shape, const char *what) { printf("%s %s: %s\n", ok ? "ok  " : "FAIL", shape, what); if (!ok) fails++; }

/* one, at_top: the control at +1 dB and at the top */
static int shape(const char *name, int stock, int steps, int hi, int per, int top, int one, int at_top)
{
    char env[128];
    snprintf(env, sizeof env, "%s:%d:%d:%d:%d", file, stock, steps, hi, per);
    setenv("HASSMIC_FAKE_SPEAKER_GAIN", env, 1);
    put(stock);
    check(spkgain_present() == (top > 0) && spkgain_max() == top, name, "present, and its top in whole dB");
    if (!top) { spkgain_set(1); check(ctl() == stock, name, "absent: the control untouched"); return fails; }
    spkgain_set(1);
    check(spkgain_get() == 1 && ctl() == one, name, "+1 dB: the nearest step over stock");
    spkgain_set(top + 5);
    check(spkgain_get() == top && ctl() == at_top, name, "above the top: the top");
    put(stock);
    check(becomes(at_top), name, "stock written by someone else: set again");
    put(stock - 1); usleep(600000);
    check(ctl() == stock - 1, name, "another value: left alone");
    put(stock);
    check(becomes(at_top), name, "stock again: set again");
    spkgain_set(0);
    check(spkgain_get() == 0 && ctl() == stock, name, "+0 dB: stock");
    put(stock); usleep(600000);
    check(ctl() == stock, name, "at +0 dB stock stays");
    return fails;
}

int main(void)
{
    static const struct { const char *name; int stock, steps, hi, per, top, one, at_top; } shapes[] = {
        { "Echo Dot 2 (HP Driver Gain Volume)", 6, 1, 35, 1, SPKGAIN_MAX, 7, 18 },
        { "Echo Dot 3 (DAC Playback Volume)", 16, 2, 20, 1, 2, 18, 20 },
        { "Echo Show 5 (OUT Playback Volume)", 29, 2, 39, 3, SPKGAIN_MAX, 30, 37 },
        { "half a dB of headroom", 16, 2, 17, 1, 0, 0, 0 },
    };
    snprintf(file, sizeof file, "/tmp/spkgain_test.%d", (int)getpid());
    for (unsigned i = 0; i < sizeof shapes / sizeof *shapes; i++) {
        pid_t p = fork(); int st = 1;
        if (!p) {
            int f = shape(shapes[i].name, shapes[i].stock, shapes[i].steps, shapes[i].hi, shapes[i].per, shapes[i].top,
                          shapes[i].one, shapes[i].at_top);
            fflush(stdout); _exit(f ? 1 : 0);
        }
        if (p < 0 || waitpid(p, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) fails++;
    }
    unlink(file);
    printf("spkgain: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
