/* touchgate.c: which touches go on to Android at once, which are held, which are the bar's pull and swallowed; the
 * Show 5's panel (goodix-ts: 480 x 960, protocol B, top y0 in portrait, x1 in landscape) */
#include "touchgate.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }

static struct input_event out[512]; static int nout, frames;
static int emit(void *ctx, const struct input_event *ev, int n)
{
    (void)ctx;
    for (int i = 0; i < n && nout < 512; i++) out[nout++] = ev[i];
    frames++;
    return 0;
}
static void clear(void) { nout = frames = 0; }
/* the value Android's copy got last for code (in slot 0), -2: never */
static int last(int code)
{
    int v = -2;
    for (int i = 0; i < nout; i++) if (out[i].type == EV_ABS && out[i].code == code) v = out[i].value;
    return v;
}
static int has(int code, int value)
{
    for (int i = 0; i < nout; i++) if (out[i].type == EV_ABS && out[i].code == code && out[i].value == value) return 1;
    return 0;
}

static struct tg g; static long long t; static int id = 100;
static float at; static int pulls;
static void ev(int type, int code, int value, const char *top)
{
    struct input_event e; memset(&e, 0, sizeof e); e.type = (unsigned short)type; e.code = (unsigned short)code; e.value = value;
    int r = tg_event(&g, &e, t, top, emit, NULL, &at);
    if (r > 0) pulls++;
}
/* frames as the goodix sends them: the slot, the id on a finger down, positions (only those that changed), SYN */
static void down(int x, int y, const char *top)
{
    ev(EV_ABS, ABS_MT_SLOT, 0, top); ev(EV_ABS, ABS_MT_TRACKING_ID, ++id, top); ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 6, top);
    if (x >= 0) ev(EV_ABS, ABS_MT_POSITION_X, x, top);
    if (y >= 0) ev(EV_ABS, ABS_MT_POSITION_Y, y, top);
    ev(EV_SYN, SYN_REPORT, 0, top); t += 10;
}
static void move(int x, int y, const char *top)
{
    ev(EV_ABS, ABS_MT_SLOT, 0, top);
    if (x >= 0) ev(EV_ABS, ABS_MT_POSITION_X, x, top);
    if (y >= 0) ev(EV_ABS, ABS_MT_POSITION_Y, y, top);
    ev(EV_SYN, SYN_REPORT, 0, top); t += 10;
}
static void up(const char *top) { ev(EV_ABS, ABS_MT_SLOT, 0, top); ev(EV_ABS, ABS_MT_TRACKING_ID, -1, top); ev(EV_SYN, SYN_REPORT, 0, top); t += 10; }

int main(void)
{
    struct input_absinfo ax = { .value = 0, .minimum = 0, .maximum = 480 }, ay = { .value = 0, .minimum = 0, .maximum = 960 };
    tg_init(&g, &ax, &ay);

    clear(); down(240, 500, "y0");
    check(frames == 1 && last(ABS_MT_TRACKING_ID) == id && last(ABS_MT_POSITION_Y) == 500, "a touch in the middle: on to Android at once");
    move(250, 520, "y0"); up("y0");
    check(frames == 3 && last(ABS_MT_TRACKING_ID) == -1 && tg_wait_ms(&g, t) < 0, "its moves and its lift too");

    clear(); down(240, 10, "y0");
    check(frames == 0 && tg_wait_ms(&g, t) > 0, "a touch at the top's middle: held");
    up("y0");
    check(frames == 2 && has(ABS_MT_POSITION_X, 240) && has(ABS_MT_POSITION_Y, 10) && last(ABS_MT_TRACKING_ID) == -1 && pulls == 0,
          "lifted: a tap, the app's (its start where it was, then the lift)");

    clear(); down(240, 10, "y0"); move(241, 40, "y0");
    check(frames == 0 && pulls == 0, "coming down 30 of 960: still held");
    move(243, 70, "y0");
    check(pulls == 1 && at > .49f && at < .52f && frames == 0, "down 60: a pull from the middle of the top, the bar's");
    move(244, 300, "y0"); up("y0");
    check(frames == 0 && tg_wait_ms(&g, t) < 0, "the rest of the pull and its lift: never Android's");

    /* the panel leaves out a value equal to its last: Android's copy last heard y 10 (the tap), the panel 300 */
    clear(); down(-1, -1, "y0");
    check(frames == 1 && last(ABS_MT_POSITION_X) == 244 && last(ABS_MT_POSITION_Y) == 300, "after a swallowed pull, a touch where it ended: Android told where");
    up("y0");

    clear(); down(240, 10, "y0"); move(280, 20, "y0");
    check(frames == 2 && has(ABS_MT_POSITION_Y, 10) && last(ABS_MT_POSITION_X) == 280 && pulls == 1, "sideways from the top: the app's, from its start");
    move(300, 25, "y0"); up("y0");
    check(frames == 4 && last(ABS_MT_TRACKING_ID) == -1, "and the rest of it");

    clear(); down(240, 10, "y0"); t += 100;
    check(tg_tick(&g, t, emit, NULL) == 0 && frames == 0, "resting at the top 100 ms: held");
    t += 250;
    check(tg_tick(&g, t, emit, NULL) == 0 && frames == 2 && last(ABS_MT_TRACKING_ID) == id, "resting 300 ms: a long press, the app's");
    up("y0");

    clear(); down(240, 10, "y0"); move(240, 35, "y0"); t += 320;
    check(tg_tick(&g, t, emit, NULL) == 0 && frames == 0, "coming down slowly: held past 300 ms");
    t += 300;
    check(tg_tick(&g, t, emit, NULL) == 0 && frames == 2, "but no longer than 600 ms");
    up("y0");

    clear(); down(30, 10, "y0");
    check(frames == 1, "at the top but in its corner: the app's at once");
    up("y0");
    clear(); down(240, 10, NULL);
    check(frames == 1, "no bar (no shell, the kiosk off): nothing held");
    up(NULL);

    /* a second finger while one is held: both the app's */
    clear(); down(240, 10, "y0");
    ev(EV_ABS, ABS_MT_SLOT, 1, "y0"); ev(EV_ABS, ABS_MT_TRACKING_ID, ++id, "y0"); ev(EV_ABS, ABS_MT_POSITION_X, 100, "y0");
    ev(EV_ABS, ABS_MT_POSITION_Y, 400, "y0"); ev(EV_SYN, SYN_REPORT, 0, "y0");
    check(frames == 2 && has(ABS_MT_POSITION_Y, 10) && has(ABS_MT_POSITION_Y, 400), "a second finger: the held one and it, the app's");
    ev(EV_ABS, ABS_MT_TRACKING_ID, -1, "y0"); ev(EV_SYN, SYN_REPORT, 0, "y0"); up("y0");

    /* landscape: the screen's top is the panel's x max */
    clear(); pulls = 0; down(475, 480, "x1"); move(462, 482, "x1");
    check(frames == 0 && pulls == 0, "landscape: held at x 475, in by 13");
    move(440, 483, "x1");
    check(pulls == 1 && at > .49f && at < .51f, "landscape: in by 35 of 480, a pull");
    up("x1");
    clear(); down(475, 480, "y0");
    check(frames == 1, "the same touch in portrait (the panel's right edge): the app's");
    up("y0");
    /* not only the very top: a finger landing 40 px below it (of 480) still pulls; 60 px below, the app's at once */
    clear(); pulls = 0; down(440, 480, "x1"); move(410, 482, "x1");
    check(frames == 0 && pulls == 1, "landscape: a pull from 40 px below the top, the drawer's");
    up("x1");
    clear(); down(420, 480, "x1");
    check(frames == 1, "landscape: 60 px below the top, the app's at once");
    up("x1");

    printf("touchgate: %s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
