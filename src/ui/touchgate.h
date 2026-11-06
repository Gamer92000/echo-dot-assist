/* The touch screen between the panel and Android (display_screen.c touch_thread), so that a swipe down from the top
 * edge opens the shell's bar and never reaches the app below, as Android's own status bar takes it, while a tap there
 * is still the app's.  hassmic grabs the panel's evdev device (EVIOCGRAB: Android's reader gets nothing more from it)
 * and writes every touch on to a uinput copy of it, which Android takes for the touch screen.  A touch that starts in
 * the middle of the screen's top edge is held back until it shows what it is: a pull (swallowed whole: the app never
 * saw its start, so nothing has to be cancelled) or anything else (handed on from its start).  Every other touch goes
 * on at once.  docs/screen-apps.md "The shell".
 *
 * The decision is tg_frame/tg_tick, pure (tests/unit/touchgate_test.c); tg_mirror makes the uinput copy. */
#ifndef HASSMIC_TOUCHGATE_H
#define HASSMIC_TOUCHGATE_H
#include <linux/input.h>

#define TG_SLOTS 16
#define TG_FRAME 96                 /* events of one frame (to SYN_REPORT); the Show 5's goodix sends 6 a finger */

struct tg {
    /* the panel's axes (EVIOCGABS: min, max, and the values it holds now, since evdev drops a value equal to the last) */
    int xmin, xmax, ymin, ymax;
    /* the panel as Android last heard of it: the slot written to, each slot's tracking id and position */
    int slot, id[TG_SLOTS], x[TG_SLOTS], y[TG_SLOTS];
    struct input_event fr[TG_FRAME]; int frn;           /* the frame being read */
    int state;                                          /* TG_IDLE, TG_PASS, TG_HOLD, TG_SWALLOW */
    char edge[3];                                       /* the held touch's edge (the top then) */
    struct input_event first[TG_FRAME + 4]; int firstn; /* the held touch's first frame */
    int hs, hx, hy;                                     /* its slot and where it started */
    long long t0;                                       /* when it started (ms) */
};
enum { TG_IDLE, TG_PASS, TG_HOLD, TG_SWALLOW };

/* events for Android (the uinput copy); 0 when written */
typedef int (*tg_emit)(void *ctx, const struct input_event *ev, int n);

void tg_init(struct tg *g, const struct input_absinfo *ax, const struct input_absinfo *ay);
/* one event from the panel at time now (ms).  top: the panel's edge that is the screen's top now ("x0" "x1" "y0" "y1"),
 * NULL while no bar would open (no shell, kiosk off): then nothing is held.  Returns 1 when this frame made a held
 * touch a pull (*at: where along the edge, 0..1), -1 when emit failed, else 0 */
int  tg_event(struct tg *g, const struct input_event *ev, long long now, const char *top, tg_emit emit, void *ctx, float *at);
/* time passing with no events: a held touch that stays where it was goes on to the app (a long press of its own).
 * -1 when emit failed */
int  tg_tick(struct tg *g, long long now, tg_emit emit, void *ctx);
/* ms until tg_tick has something to do, -1: nothing held */
int  tg_wait_ms(const struct tg *g, long long now);

/* a uinput device like the panel on fd (name, ids, events, axes, properties); its fd, or -1 */
int  tg_mirror(int fd);
#endif
