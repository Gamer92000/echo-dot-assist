/* The touchscreen's evdev node, turned as InputReader turns it: multitouch protocol B, the finger that came first followed
 * (in whichever slot the driver puts it).  android.c's; on its own so that the PC tests it (tests/unit/touch_test.c,
 * events through a pipe).
 *
 * Read beside Android, or (touch_open with gate set, where a uinput copy can be made) through the touch gate
 * (touchgate.h): a thread of its own grabs the panel, hands every touch on to Android's copy at once and these events
 * to us through a socket, each report with the gate's word before it (TOUCH_MARK).  A touch that starts in the middle
 * of the screen's top edge while an app is in front (touch_gate_hold) is held from the app until it shows what it is:
 * a pull is the drawer's alone (no grip rows taken from the apps for it), anything else the app's. */
#ifndef TOUCH_H
#define TOUCH_H

struct scene;

#define TOUCH_SLOTS 10
struct touch {
    int fd, rot, W, H, grip;            /* the display at start (rotation, size), the grip's rows above the layer */
    int gate, dev;                      /* through the gate (fd: its socket, dev: the panel's node, the gate thread's) */
    int gst, gpull, held;               /* the gate's word on this report (TG_*), a pull just now, this touch not ours */
    int xmin, xmax, ymin, ymax;         /* the device's range */
    int *screen_on;                     /* the platform's word: on or off */
    int (*interactive)(void);           /* the screen on, asked now; -1 unknown */
    void (*wake)(void);                 /* a touch on a dark screen: on, as stock */
    /* the slots and the finger followed (touch.c) */
    int slot, id[TOUCH_SLOTS], x[TOUCH_SLOTS], y[TOUCH_SLOTS], x0[TOUCH_SLOTS], y0[TOUCH_SLOTS];
    long long t0[TOUCH_SLOTS], pt0;     /* when each landed, and the one followed */
    int dropped, p, lift, waking, rest, lx, ly, born, later;
};

int  touch_open(struct touch *t, char *name, int n);   /* the panel's node (its name into name): fd, or -1; through the
                                                          gate when t->gate is set and the gate comes up (t->gate stays 1) */
void touch_gate_hold(int on);                          /* an app in front, the screen on, the drawer in: hold a touch at
                                                          the top's middle (any thread) */
#define TOUCH_MARK 0x1e                                 /* an event type no device has: the gate's word (code TG_*,
                                                           value 1: this report made a pull) */
void touch_reset(struct touch *t);                     /* no finger known (fd and the rest kept) */
void touch_read(struct touch *t, struct scene *s);     /* what came, into the scene */
void touch_gone(struct touch *t, struct scene *s, long long now);   /* the node went away: closed, the finger up with it */
#endif
