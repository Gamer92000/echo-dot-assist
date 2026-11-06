/* touch.c, the Echo Show's touchscreen as android.c reads it: protocol B reports through a pipe, what reaches the scene.
 * The finger that came first is the touch; a tap, a swipe; a lift and the next finger in one report; typing with two
 * thumbs (the second key once the first lifts); a two-finger tap (one tap); a two-finger swipe (the second finger never
 * a tap of its own where it lifts); a touch that wakes the screen (none of it, nor of its other fingers, a touch) */
#include "../../src/ui/touch.h"
#include "../../src/ui/scene.h"
#include "../../src/ui/touchgate.h"
#include <assert.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct { int ev, x, y; } got[256]; static int ngot, screens;
void scene_touch(struct scene *s, int ev, int x, int y, long long now) { (void)s; (void)now; if (ngot < 256) { got[ngot].ev = ev; got[ngot].x = x; got[ngot].y = y; ngot++; } }
void scene_screen(struct scene *s, int on, long long now) { (void)s; (void)on; (void)now; screens++; }

static int wr, screen_on = 1, really_on = 1, wakes;           /* screen_on: the platform's word; really_on: the panel's */
static int on_now(void) { return really_on; }
static void wake(void) { wakes++; really_on = 1; }

/* the events' times: a scenario's clock from 950 ms ago (touch.c takes no time ahead of now nor a second before it),
 * 10 ms a report on, gaps where a scenario says */
static long long clk;
static void at_start(void) { struct timespec n; clock_gettime(CLOCK_MONOTONIC, &n); clk = n.tv_sec * 1000LL + n.tv_nsec / 1000000 - 950; }
static void ev(int type, int code, int value)
{
    struct input_event e; memset(&e, 0, sizeof e); e.type = (unsigned short)type; e.code = (unsigned short)code; e.value = value;
    e.time.tv_sec = clk / 1000; e.time.tv_usec = clk % 1000 * 1000;
    assert(write(wr, &e, sizeof e) == sizeof e);
}
static void slot(int k) { ev(EV_ABS, ABS_MT_SLOT, k); }
static void down(int k, int id, int x, int y) { slot(k); ev(EV_ABS, ABS_MT_TRACKING_ID, id); ev(EV_ABS, ABS_MT_POSITION_X, x); ev(EV_ABS, ABS_MT_POSITION_Y, y); }
static void move(int k, int x, int y) { slot(k); ev(EV_ABS, ABS_MT_POSITION_X, x); ev(EV_ABS, ABS_MT_POSITION_Y, y); }
static void up(int k) { slot(k); ev(EV_ABS, ABS_MT_TRACKING_ID, -1); }
static void syn(struct touch *t) { ev(EV_SYN, SYN_REPORT, 0); touch_read(t, NULL); clk += 10; }

/* what came since the last look, as "D100,384 M.. U.." */
static const char *seen(void)
{
    static char b[2048]; size_t o = 0; b[0] = 0;
    for (int i = 0; i < ngot && o + 32 < sizeof b; i++) o += (size_t)snprintf(b + o, sizeof b - o, "%s%c%d,%d", i ? " " : "", "DMU"[got[i].ev], got[i].x, got[i].y);
    ngot = 0;
    return b;
}

int main(void)
{
    int fd[2]; assert(!pipe(fd)); fcntl(fd[0], F_SETFL, O_NONBLOCK); wr = fd[1];
    /* a panel of 960x600 raw units on a 960x600 display, not turned; the grip's 16 rows above the layer */
    struct touch t = { .fd = fd[0], .W = 960, .H = 600, .grip = 16, .xmin = 0, .xmax = 959, .ymin = 0, .ymax = 599,
                       .screen_on = &screen_on, .interactive = on_now, .wake = wake };
    touch_reset(&t); t.slot = 0;

    /* a tap; a swipe */
    at_start(); down(0, 1, 100, 216); syn(&t); up(0); syn(&t);
    assert(!strcmp(seen(), "D100,200 U100,200"));
    down(0, 2, 100, 216); syn(&t); move(0, 100, 316); syn(&t); up(0); syn(&t);
    assert(!strcmp(seen(), "D100,200 M100,300 U100,300"));
    /* a lift and the next finger in one report (another slot): the next tap */
    at_start(); down(0, 3, 100, 216); syn(&t); up(0); down(1, 4, 500, 416); syn(&t); up(1); syn(&t);
    assert(!strcmp(seen(), "D100,200 U100,200 D500,400 U500,400"));
    /* typing with two thumbs: the second down well after the first, before it lifts, held still: its key once the first
     * is up */
    at_start(); down(0, 5, 100, 416); syn(&t); clk += 150; down(1, 6, 600, 416); syn(&t); up(0); syn(&t); up(1); syn(&t);
    assert(!strcmp(seen(), "D100,400 M100,400 U100,400 D600,400 U600,400"));
    /* a two-finger tap: both down at once, or 30 ms apart, lifting a report apart: one tap, the first finger's */
    at_start(); down(0, 20, 300, 216); down(1, 21, 380, 216); syn(&t); up(0); syn(&t); up(1); syn(&t);
    assert(!strcmp(seen(), "D300,200 U300,200"));
    at_start(); down(0, 22, 300, 216); syn(&t); clk += 20; down(1, 23, 380, 216); syn(&t); up(0); syn(&t); up(1); syn(&t);
    assert(!strcmp(seen(), "D300,200 M300,200 U300,200"));
    /* a two-finger swipe down (each lifting in a report of its own): the second finger is part of it, no tap of its own */
    at_start(); down(0, 7, 400, 20); down(1, 8, 500, 20); syn(&t);
    for (int y = 60; y <= 340; y += 40) { move(0, 400, y); move(1, 500, y); syn(&t); }
    up(0); syn(&t); up(1); syn(&t);
    assert(!strcmp(seen(), "D400,4 M400,44 M400,84 M400,124 M400,164 M400,204 M400,244 M400,284 M400,324 U400,324"));
    /* a second finger that moved while the first was down (a gesture of two, a pinch): no touch of its own after it */
    at_start(); down(1, 9, 500, 316); syn(&t); clk += 150; down(0, 10, 100, 316); syn(&t); move(0, 100, 416); syn(&t); up(1); syn(&t);
    move(0, 100, 430); syn(&t); up(0); syn(&t);
    assert(!strcmp(seen(), "D500,300 M500,300 M500,300 U500,300"));
    /* a second finger that went with the first and came back to where it landed: still the gesture's */
    at_start(); down(0, 14, 100, 216); syn(&t); clk += 150; down(1, 15, 300, 216); syn(&t);
    for (int y = 256; y <= 416; y += 40) { move(0, 100, y); move(1, 300, y); syn(&t); }
    for (int y = 376; y >= 216; y -= 40) { move(0, 100, y); move(1, 300, y); syn(&t); }
    up(0); syn(&t); up(1); syn(&t);
    { const char *g = seen(); assert(g[0] == 'D' && !strstr(g, "D300")); }
    /* the screen off: the touch that wakes it is only that, and so is the other finger it came with */
    screen_on = really_on = 0;
    at_start(); down(0, 11, 100, 316); syn(&t); clk += 150; down(1, 12, 300, 316); syn(&t);
    assert(wakes == 1 && screen_on);
    up(0); syn(&t); up(1); syn(&t);
    assert(!strcmp(seen(), ""));
    /* the screen on by itself (a pattern's wake), the platform's word not yet: a touch, not a waking one */
    screen_on = 0; really_on = 1; { int sc = screens;
      at_start(); down(0, 24, 100, 216); syn(&t); up(0); syn(&t);
      assert(screen_on && screens == sc + 1 && wakes == 1 && !strcmp(seen(), "D100,200 U100,200")); }
    /* and after it all is as before */
    at_start(); down(0, 13, 100, 216); syn(&t); up(0); syn(&t);
    assert(!strcmp(seen(), "D100,200 U100,200"));
    /* through the gate (touchgate.h): its word before each report.  A touch it holds from the app at the top is none
     * of the scene's, and a pull from there the drawer's, from above the layer's top (as the grip's rows were) */
    t.grip = 0;
    at_start(); down(0, 30, 480, 16); ev(TOUCH_MARK, TG_HOLD, 0); syn(&t); move(0, 482, 40); ev(TOUCH_MARK, TG_HOLD, 0); syn(&t);
    assert(!strcmp(seen(), ""));
    move(0, 484, 80); ev(TOUCH_MARK, TG_SWALLOW, 1); syn(&t);
    assert(!strcmp(seen(), "D480,-1 M484,80"));
    move(0, 486, 200); ev(TOUCH_MARK, TG_SWALLOW, 0); syn(&t); up(0); ev(TOUCH_MARK, TG_IDLE, 0); syn(&t);
    assert(!strcmp(seen(), "M486,200 U486,200"));
    /* held and then handed to the app (a tap there, a swipe sideways): never the scene's */
    at_start(); down(0, 31, 480, 16); ev(TOUCH_MARK, TG_HOLD, 0); syn(&t); up(0); ev(TOUCH_MARK, TG_IDLE, 0); syn(&t);
    assert(!strcmp(seen(), ""));
    at_start(); down(0, 32, 480, 16); ev(TOUCH_MARK, TG_HOLD, 0); syn(&t); move(0, 540, 20); ev(TOUCH_MARK, TG_PASS, 0); syn(&t);
    move(0, 600, 20); ev(TOUCH_MARK, TG_PASS, 0); syn(&t); up(0); ev(TOUCH_MARK, TG_IDLE, 0); syn(&t);
    assert(!strcmp(seen(), ""));
    /* and a touch the gate let through at once is the scene's as ever */
    at_start(); down(0, 33, 100, 300); ev(TOUCH_MARK, TG_PASS, 0); syn(&t); up(0); ev(TOUCH_MARK, TG_IDLE, 0); syn(&t);
    assert(!strcmp(seen(), "D100,300 U100,300"));
    printf("touch_test: ok\n");
    return 0;
}
