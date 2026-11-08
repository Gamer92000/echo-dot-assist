/* The touchscreen (touch.h): protocol B, the finger that came first followed; p its slot (-1 none), lift: it went in
 * this report, waking: its touch woke the screen, none of that touch is a gesture.
 *
 * rest: a finger lifted, the next only once none is down: a second finger left down is no touch of its own, unless it
 * is new in that report (born, by slot: a quick next tap) or came down well after the one followed and stayed where
 * it landed (later, by slot: typing with two thumbs, the next key).  One that landed with the first (a two-finger tap:
 * one tap, as Android makes it) or moved with it (a two-finger swipe or fling, lifting in two reports) is part of its
 * gesture: never a touch of its own after it, which would be a second tap where it lifts, on whatever the first one
 * brought up (the drawer's shade under a swipe that opened it); nor is anything of a waking touch. */
#include "touch.h"
#include "scene.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define SLOP 14                         /* px: a finger that moved less stayed where it landed (the scene's TAP_PX) */
#define LATER_MS 100                    /* a finger down this long after the one followed: a touch of its own, next */

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

static void touch_resync(struct touch *t);

void touch_reset(struct touch *t)
{
    for (int k = 0; k < TOUCH_SLOTS; k++) t->id[k] = -1;
    t->p = -1; t->dropped = t->lift = t->waking = t->rest = t->born = t->later = 0;
}

/* Which direct multitouch screen: the lowest event number, the same every boot (readdir has no order), and amazon_touch
 * only when there is no other.  That one is system_server's own (AmazonInputManagerJNI in libandroid_servers.so, every
 * Show's firmware: a "virtual touch device" on /dev/uinput, amazon_touch.idc makes it a touchscreen), which carries only
 * what Amazon's services inject, never a finger; the panel's driver (goodix-ts on the Show 5s, FocalTech or Goodix on
 * the Show 8s) is the one to read.  Rank: lower is better. */
static long rank(const char *nm, long num) { return (strcmp(nm, "amazon_touch") ? 0 : 1L << 20) + num; }

int touch_open(struct touch *t, char *name, int n)
{
    DIR *d = opendir("/dev/input"); struct dirent *e; long best = -1;
    t->fd = -1; t->grabbed = 0; if (n > 0) name[0] = 0;
    while (d && (e = readdir(d))) {
        char p[300], nm[64] = "", *end; struct input_absinfo ax, ay;
        if (strncmp(e->d_name, "event", 5)) continue;
        long num = strtol(e->d_name + 5, &end, 10);
        if (end == e->d_name + 5 || *end || num < 0 || num >= 1L << 20) continue;
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long props = 0;
        if (ioctl(fd, EVIOCGNAME(sizeof nm - 1), nm) >= 0 && ioctl(fd, EVIOCGPROP(sizeof props), &props) >= 0 &&
            (props & (1 << INPUT_PROP_DIRECT)) && !ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) && !ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) &&
            ax.maximum > ax.minimum && ay.maximum > ay.minimum && (best < 0 || rank(nm, num) < best)) {
            if (t->fd >= 0) close(t->fd);                        /* (a worse one found first) */
            t->fd = fd; best = rank(nm, num);
            t->xmin = ax.minimum; t->xmax = ax.maximum; t->ymin = ay.minimum; t->ymax = ay.maximum;
            if (n > 0) snprintf(name, (size_t)n, "%s (%s)", nm, p);
            continue;
        }
        close(fd);
    }
    if (d) closedir(d);
    if (t->fd >= 0) {
        int mono = CLOCK_MONOTONIC; ioctl(t->fd, EVIOCSCLOCKID, &mono);   /* the events' own times on our clock (mono_ms) */
        struct input_absinfo sl; t->slot = !ioctl(t->fd, EVIOCGABS(ABS_MT_SLOT), &sl) ? sl.value : 0;
        if (t->slot < 0 || t->slot >= TOUCH_SLOTS) t->slot = -1;
    }
    touch_reset(t);
    /* the slots as they are: the kernel sends a slot's value only when it changes, so a first touch where that slot's
     * last one was (before this open) would come without it; a finger down already is no touch of ours */
    if (t->fd >= 0) { touch_resync(t); t->born = 0; for (int k = 0; k < TOUCH_SLOTS; k++) if (t->id[k] >= 0) t->rest = 1; }
    return t->fd;
}

/* raw -> the logical display, as AOSP 7's TouchInputMapper::cookPointerData turns it by the display's rotation */
static void touch_map(const struct touch *t, int rx, int ry, int *x, int *y)
{
    float xr = t->xmax - t->xmin + 1, yr = t->ymax - t->ymin + 1;
    int nw = t->rot & 1 ? t->H : t->W, nh = t->rot & 1 ? t->W : t->H;      /* the panel's natural size */
    float xs = nw / xr, ys = nh / yr;
    switch (t->rot) {
    case 1: *x = (int)((ry - t->ymin) * ys); *y = (int)((t->xmax - rx) * xs); break;
    case 2: *x = (int)((t->xmax - rx) * xs); *y = (int)((t->ymax - ry) * ys); break;
    case 3: *x = (int)((t->ymax - ry) * ys); *y = (int)((rx - t->xmin) * xs); break;
    default: *x = (int)((rx - t->xmin) * xs); *y = (int)((ry - t->ymin) * ys);
    }
}

/* slot k's finger where it landed, give or take a tap's slop */
static int stayed(const struct touch *t, int k)
{
    int x, y, x0, y0;
    touch_map(t, t->x[k], t->y[k], &x, &y); touch_map(t, t->x0[k], t->y0[k], &x0, &y0);
    return abs(x - x0) <= SLOP && abs(y - y0) <= SLOP;
}

/* events lost when the kernel's buffer ran over (this thread busy: a cover decoded, a screenshot): the slots as the
 * device has them now (EVIOCGMTSLOTS, the kernel's way back in step), else a lift would be lost and the next touch a move */
static void touch_resync(struct touch *t)
{
    struct input_absinfo a; struct { uint32_t code; int32_t v[TOUCH_SLOTS]; } q;
    static const unsigned codes[3] = { ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y };
    int *dst[3] = { t->id, t->x, t->y }, was = t->p >= 0 ? t->id[t->p] : -1, old[TOUCH_SLOTS];
    int px = t->p >= 0 ? t->x[t->p] : 0, py = t->p >= 0 ? t->y[t->p] : 0;   /* (where the followed one was last seen) */
    memcpy(old, t->id, sizeof old);
    if (!ioctl(t->fd, EVIOCGABS(ABS_MT_SLOT), &a)) t->slot = a.value >= 0 && a.value < TOUCH_SLOTS ? a.value : -1;
    for (int k = 0; k < 3; k++) {                              /* (the kernel fills only the slots the panel has: the
                                                                   others keep what they had, -1 for no finger) */
        q.code = codes[k]; memcpy(q.v, dst[k], sizeof q.v);
        if (ioctl(t->fd, EVIOCGMTSLOTS(sizeof q), &q) >= 0) memcpy(dst[k], q.v, sizeof q.v);
    }
    if (t->p >= 0 && t->id[t->p] != was) {                    /* gone (the slot keeps where it lifted), or another there
                                                                   (its position: where the followed one was last seen) */
        t->lift = 1; t->lx = t->id[t->p] < 0 ? t->x[t->p] : px; t->ly = t->id[t->p] < 0 ? t->y[t->p] : py;
    }
    for (int k = 0; k < TOUCH_SLOTS; k++) if (t->id[k] >= 0 && t->id[k] != old[k]) t->born |= 1 << k;   /* (down meanwhile) */
    for (int k = 0; k < TOUCH_SLOTS; k++) if (t->id[k] < 0) t->later &= ~(1 << k);
}

int touch_fingers(const struct touch *t)
{
    int n = 0;
    for (int k = 0; k < TOUCH_SLOTS; k++) n += t->id[k] >= 0;
    return n;
}

/* A grab begun or ended with a finger down would leave Android half a touch (a down never lifted, or a lift it never
 * saw the down of): the caller does it only with none (touch_fingers).  The kernel lets go of it when the fd closes,
 * so a UI that dies or is killed as hung never leaves the panel taken */
int touch_grab(struct touch *t, int on)
{
    if (t->fd < 0) return -1;
    if (ioctl(t->fd, EVIOCGRAB, on ? 1 : 0) < 0) return -1;
    t->grabbed = !!on;
    return 0;
}

void touch_gone(struct touch *t, struct scene *s, long long now)
{
    if (t->fd >= 0) close(t->fd);
    t->fd = -1; t->grabbed = 0;
    if (t->p >= 0 && !t->waking) { int x, y; touch_map(t, t->x[t->p], t->y[t->p], &x, &y); scene_touch(s, TOUCH_UP, x, y - t->grip, now); }
    t->p = -1;
}

void touch_read(struct touch *t, struct scene *s)
{
    struct input_event ev[64]; ssize_t r;
    while ((r = read(t->fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < (size_t)r / sizeof ev[0]; i++) {
            if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED) { t->dropped = 1; continue; }
            if (t->dropped) {                                    /* up to the next report: what is left of the lost ones */
                if (ev[i].type != EV_SYN || ev[i].code != SYN_REPORT) continue;
                t->dropped = 0; touch_resync(t);
            }
            if (ev[i].type == EV_ABS) {
                int k = t->slot;
                if (ev[i].code == ABS_MT_SLOT) t->slot = ev[i].value >= 0 && ev[i].value < TOUCH_SLOTS ? ev[i].value : -1;
                else if (k < 0) continue;                        /* a slot past ours: not followed */
                else if (ev[i].code == ABS_MT_TRACKING_ID) {
                    if (k == t->p && ev[i].value != t->id[k]) { t->lift = 1; t->lx = t->x[k]; t->ly = t->y[k]; }   /* that finger gone
                                                                   (-1, or a new one at once: where it was, before the new one's) */
                    if (ev[i].value >= 0 && ev[i].value != t->id[k]) t->born |= 1 << k;
                    if (ev[i].value < 0) t->later &= ~(1 << k);
                    t->id[k] = ev[i].value;
                }
                else if (ev[i].code == ABS_MT_POSITION_X) t->x[k] = ev[i].value;
                else if (ev[i].code == ABS_MT_POSITION_Y) t->y[k] = ev[i].value;
            } else if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
                /* when it happened, not when this thread came to read it (a frame's vsync, a cover decoded, a
                 * screenshot: events come in bursts), for the fling's speed; never ahead of the scene's clock */
                long long at = ev[i].time.tv_sec * 1000LL + ev[i].time.tv_usec / 1000, nw = mono_ms();
                if (at > nw || at < nw - 1000) at = nw;
                int x, y;
                for (int k = 0; k < TOUCH_SLOTS; k++) if (t->born >> k & 1) { t->x0[k] = t->x[k]; t->y0[k] = t->y[k]; t->t0[k] = at; }   /* (where, when) */
                for (int k = 0; k < TOUCH_SLOTS; k++) if ((t->later >> k & 1) && !stayed(t, k)) t->later &= ~(1 << k);   /* moved once: for good
                                                                   (back where it landed, still the gesture's) */
                if (t->p >= 0 && t->lift) {                      /* the finger followed lifted (and maybe another down) */
                    touch_map(t, t->lx, t->ly, &x, &y);
                    if (!t->waking) scene_touch(s, TOUCH_UP, x, y - t->grip, at);
                    else t->later = 0;                           /* (the waking touch's other fingers: its too) */
                    t->p = -1; t->waking = 0; t->rest = 1;
                }
                t->lift = 0;
                int fresh = 0, any = 0;
                for (int k = 0; k < TOUCH_SLOTS; k++) any |= t->id[k] >= 0;
                if (!any) t->rest = 0;
                for (int k = 0; t->p < 0 && k < TOUCH_SLOTS; k++)
                    if (t->id[k] >= 0 && (!t->rest || (t->born >> k & 1) || ((t->later >> k & 1) && stayed(t, k)))) { t->p = k; fresh = 1; }
                if (fresh) t->pt0 = t->t0[t->p];
                if (t->p >= 0 && at - t->pt0 >= LATER_MS) t->later |= t->born;   /* (others down well after it: next) */
                if (t->p >= 0) t->later &= ~(1 << t->p);
                t->born = 0;
                if (t->p < 0) continue;
                touch_map(t, t->x[t->p], t->y[t->p], &x, &y);
                y -= t->grip;                                    /* the layer's rows start under the grip */
                if (fresh && t->interactive) {                   /* on or off by itself since the platform looked */
                    int on = t->interactive();
                    if (on >= 0 && on != *t->screen_on) { *t->screen_on = on; scene_screen(s, on, at); }
                }
                if (fresh && !*t->screen_on) {                   /* a touch wakes the screen, as on stock: that touch is
                                                                    only that, whatever it does next (no tap, no Back) */
                    if (t->wake) t->wake();
                    *t->screen_on = 1; scene_screen(s, 1, at); t->waking = 1;
                } else if (fresh) scene_touch(s, TOUCH_DOWN, x, y, at);
                else if (!t->waking) scene_touch(s, TOUCH_MOVE, x, y, at);
            }
        }
    }
}
