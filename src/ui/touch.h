/* The touchscreen's evdev node, read beside Android, turned as InputReader turns it: multitouch protocol B, the finger
 * that came first followed (in whichever slot the driver puts it).  Grabbed (EVIOCGRAB) only while the words and timers
 * cover an app (words.c), so that Android gets none of those touches; let go of when they go, between touches always.  android.c's; on its own so that the PC
 * tests it (tests/unit/touch_test.c, events through a pipe). */
#ifndef TOUCH_H
#define TOUCH_H

struct scene;

#define TOUCH_SLOTS 10
struct touch {
    int fd, rot, W, H, grip;            /* the display at start (rotation, size), the grip's rows above the layer */
    int xmin, xmax, ymin, ymax;         /* the device's range */
    int *screen_on;                     /* the platform's word: on or off */
    int (*interactive)(void);           /* the screen on, asked now; -1 unknown */
    void (*wake)(void);                 /* a touch on a dark screen: on, as stock */
    /* the slots and the finger followed (touch.c) */
    int slot, id[TOUCH_SLOTS], x[TOUCH_SLOTS], y[TOUCH_SLOTS], x0[TOUCH_SLOTS], y0[TOUCH_SLOTS];
    long long t0[TOUCH_SLOTS], pt0;     /* when each landed, and the one followed */
    int dropped, p, lift, waking, rest, lx, ly, born, later;
    int grabbed;                        /* EVIOCGRAB held on fd */
};

int  touch_open(struct touch *t, char *name, int n);   /* the panel's node (its name into name): fd, or -1 */
void touch_reset(struct touch *t);                     /* no finger known (fd and the rest kept) */
void touch_read(struct touch *t, struct scene *s);     /* what came, into the scene */
void touch_gone(struct touch *t, struct scene *s, long long now);   /* the node went away: closed, the finger up with it */
int  touch_fingers(const struct touch *t);             /* fingers down now (grabs change only at 0) */
int  touch_grab(struct touch *t, int on);              /* the node to us alone, or Android's again: 0, -1 refused */
#endif
