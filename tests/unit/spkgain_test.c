/* spkgain.c against a fake control (HASSMIC_FAKE_SPEAKER_GAIN) in the shapes of the Echo Dot 2's codec (1 dB a step,
 * 6..35 above stock) and the Echo Dot 3's amplifier (0.5 dB a step, 16..20): dB to steps, the top, the watcher putting
 * stock back and leaving other values alone.  Each shape in a child of its own: the control is probed once. */
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
static void check(int ok, const char *shape, const char *what) { printf("%s %s: %s\n", ok ? "ok  " : "FAIL", shape, what); if (!ok) fails++; }

static int shape(const char *name, int stock, int steps, int hi, int top)
{
    char env[128];
    snprintf(env, sizeof env, "%s:%d:%d:%d", file, stock, steps, hi);
    setenv("HASSMIC_FAKE_SPEAKER_GAIN", env, 1);
    put(stock);
    check(spkgain_present() == (top > 0) && spkgain_max() == top, name, "present, and its top in whole dB");
    if (!top) { spkgain_set(1); check(ctl() == stock, name, "absent: the control untouched"); return fails; }
    spkgain_set(1);
    check(spkgain_get() == 1 && ctl() == stock + steps, name, "+1 dB: one dB of steps over stock");
    spkgain_set(top + 5);
    check(spkgain_get() == top && ctl() == stock + top * steps, name, "above the top: the top");
    put(stock); usleep(600000);
    check(ctl() == stock + top * steps, name, "stock written by someone else: set again");
    put(stock - 1); usleep(600000);
    check(ctl() == stock - 1, name, "another value: left alone");
    put(stock); usleep(600000);
    check(ctl() == stock + top * steps, name, "stock again: set again");
    spkgain_set(0);
    check(spkgain_get() == 0 && ctl() == stock, name, "+0 dB: stock");
    put(stock); usleep(600000);
    check(ctl() == stock, name, "at +0 dB stock stays");
    return fails;
}

int main(void)
{
    static const struct { const char *name; int stock, steps, hi, top; } shapes[] = {
        { "Echo Dot 2 (HP Driver Gain Volume)", 6, 1, 35, SPKGAIN_MAX },
        { "Echo Dot 3 (DAC Playback Volume)", 16, 2, 20, 2 },
        { "half a dB of headroom", 16, 2, 17, 0 },
    };
    snprintf(file, sizeof file, "/tmp/spkgain_test.%d", (int)getpid());
    for (unsigned i = 0; i < sizeof shapes / sizeof *shapes; i++) {
        pid_t p = fork(); int st = 1;
        if (!p) { int f = shape(shapes[i].name, shapes[i].stock, shapes[i].steps, shapes[i].hi, shapes[i].top); fflush(stdout); _exit(f ? 1 : 0); }
        if (p < 0 || waitpid(p, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) fails++;
    }
    unlink(file);
    printf("spkgain: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
