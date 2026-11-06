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
#include "touchgate.h"
#include <errno.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
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
    t->gst = TG_IDLE; t->gpull = t->held = 0;
}

/* Which direct multitouch screen: the lowest event number, the same every boot (readdir has no order), and amazon_touch
 * only when there is no other.  That one is system_server's own (AmazonInputManagerJNI in libandroid_servers.so, every
 * Show's firmware: a "virtual touch device" on /dev/uinput, amazon_touch.idc makes it a touchscreen), which carries only
 * what Amazon's services inject, never a finger; the panel's driver (goodix-ts on the Show 5s, FocalTech or Goodix on
 * the Show 8s) is the one to read.  Rank: lower is better. */
static long rank(const char *nm, long num) { return (strcmp(nm, "amazon_touch") ? 0 : 1L << 20) + num; }

/* ---------------------------------------------------------------- the gate (touchgate.h) */

static atomic_int hold_on;
void touch_gate_hold(int on) { atomic_store(&hold_on, on); }

struct gate { int dev, u, out; const char *top; struct input_absinfo ax, ay; };

static int gate_write(void *ctx, const struct input_event *ev, int n)
{
    int u = *(int *)ctx;
    return write(u, ev, (size_t)n * sizeof *ev) == (ssize_t)((size_t)n * sizeof *ev) ? 0 : -1;
}

static int pass(int out, const struct input_event *e, int n)
{
    /* sendto, not send: android.c has a send() of its own (its lines to hassmic), which the library would call */
    return sendto(out, e, (size_t)n * sizeof *e, MSG_NOSIGNAL, NULL, 0) == (ssize_t)((size_t)n * sizeof *e) ? 0 : -1;
}

static void mark(int out, int state, int pulled)
{
    struct input_event m; memset(&m, 0, sizeof m);
    m.type = TOUCH_MARK; m.code = (unsigned short)state; m.value = pulled;
    pass(out, &m, 1);
}

/* The gate's own thread: Android's touches never wait for a frame of ours (a cover decoded, a screenshot), only for
 * the hold at the top.  Ends when the panel or the UI's end of the socket goes; the grab and the copy with it (and with
 * the process: the kernel lets go of both) */
static void *gate_thread(void *arg)
{
    struct gate *g = arg; struct tg st; tg_init(&st, &g->ax, &g->ay);
    struct input_event ev[64];
    for (;;) {
        int w = g->u >= 0 ? tg_wait_ms(&st, mono_ms()) : -1, bad = 0;
        struct pollfd p = { g->dev, POLLIN, 0 };
        int k = poll(&p, 1, w);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (k == 0) {                                            /* a held touch at rest: the app's (a long press) */
            int was = st.state;
            if (tg_tick(&st, mono_ms(), gate_write, &g->u) < 0) bad = 1;
            else if (st.state != was) mark(g->out, st.state, 0);
        } else {
            ssize_t r = read(g->dev, ev, sizeof ev);
            if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (r <= 0) break;
            for (size_t i = 0; i < (size_t)r / sizeof *ev; i++) {
                if (g->u >= 0 && !bad) {
                    float at; int v = tg_event(&st, &ev[i], mono_ms(), atomic_load(&hold_on) ? g->top : NULL, gate_write, &g->u, &at);
                    if (v < 0) bad = 1;
                    else if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) mark(g->out, st.state, v > 0);
                }
                if (pass(g->out, &ev[i], 1)) goto done;          /* the UI's end gone */
            }
        }
        if (bad) {
            /* Android's copy refused a write: the panel back to Android, read beside it from here on (no grip rows
             * either: a pull over an app is the app's too until the UI starts again) */
            fprintf(stderr, "touch: the gate's copy failed (%s): beside Android from now on\n", strerror(errno));
            ioctl(g->dev, EVIOCGRAB, 0); ioctl(g->u, UI_DEV_DESTROY); close(g->u); g->u = -1;
            mark(g->out, TG_PASS, 0);
        }
    }
done:
    if (g->u >= 0) { ioctl(g->dev, EVIOCGRAB, 0); ioctl(g->u, UI_DEV_DESTROY); close(g->u); }
    close(g->dev); close(g->out); free(g);
    return NULL;
}

/* the panel's edge that is the screen's top in this rotation (touch_map's turn): y min, x max, y max, x min */
static const char *top_edge(int rot) { static const char *e[4] = { "y0", "x1", "y1", "x0" }; return e[rot & 3]; }

/* the gate in front of dev: 0, its socket in t->fd; -1 (no copy, no grab: read dev beside Android) */
static int gate_start(struct touch *t, int dev)
{
    struct gate *g = calloc(1, sizeof *g); int sv[2] = { -1, -1 };
    if (!g) return -1;
    g->dev = dev; g->top = top_edge(t->rot);
    if (ioctl(dev, EVIOCGABS(ABS_MT_POSITION_X), &g->ax) || ioctl(dev, EVIOCGABS(ABS_MT_POSITION_Y), &g->ay)) goto fail;
    if ((g->u = tg_mirror(dev)) < 0) goto fail;
    if (ioctl(dev, EVIOCGRAB, 1)) goto fail;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) goto fail;
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    int fl = fcntl(dev, F_GETFL); fcntl(dev, F_SETFL, fl | O_NONBLOCK);
    g->out = sv[1];
    pthread_t th;
    if (pthread_create(&th, NULL, gate_thread, g)) goto fail;
    pthread_detach(th);
    t->fd = sv[0]; t->dev = dev;
    return 0;
fail:
    if (sv[0] >= 0) { close(sv[0]); close(sv[1]); }
    if (g->u >= 0) { ioctl(dev, EVIOCGRAB, 0); ioctl(g->u, UI_DEV_DESTROY); close(g->u); }
    free(g);
    return -1;
}

int touch_open(struct touch *t, char *name, int n)
{
    DIR *d = opendir("/dev/input"); struct dirent *e; long best = -1;
    t->fd = -1; if (n > 0) name[0] = 0;
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
    int want = t->gate; t->gate = 0; t->dev = t->fd;
    if (t->fd >= 0) { touch_resync(t); t->born = 0; for (int k = 0; k < TOUCH_SLOTS; k++) if (t->id[k] >= 0) t->rest = 1; }
    if (t->fd >= 0 && want) t->gate = !gate_start(t, t->fd);   /* else beside Android, as before */
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
    int dev = t->gate ? t->dev : t->fd;
    if (!ioctl(dev, EVIOCGABS(ABS_MT_SLOT), &a)) t->slot = a.value >= 0 && a.value < TOUCH_SLOTS ? a.value : -1;
    for (int k = 0; k < 3; k++) {                              /* (the kernel fills only the slots the panel has: the
                                                                   others keep what they had, -1 for no finger) */
        q.code = codes[k]; memcpy(q.v, dst[k], sizeof q.v);
        if (ioctl(dev, EVIOCGMTSLOTS(sizeof q), &q) >= 0) memcpy(dst[k], q.v, sizeof q.v);
    }
    if (t->p >= 0 && t->id[t->p] != was) {                    /* gone (the slot keeps where it lifted), or another there
                                                                   (its position: where the followed one was last seen) */
        t->lift = 1; t->lx = t->id[t->p] < 0 ? t->x[t->p] : px; t->ly = t->id[t->p] < 0 ? t->y[t->p] : py;
    }
    for (int k = 0; k < TOUCH_SLOTS; k++) if (t->id[k] >= 0 && t->id[k] != old[k]) t->born |= 1 << k;   /* (down meanwhile) */
    for (int k = 0; k < TOUCH_SLOTS; k++) if (t->id[k] < 0) t->later &= ~(1 << k);
}

void touch_gone(struct touch *t, struct scene *s, long long now)
{
    if (t->fd >= 0) close(t->fd);
    t->fd = -1;
    if (t->p >= 0 && !t->waking) { int x, y; touch_map(t, t->x[t->p], t->y[t->p], &x, &y); scene_touch(s, TOUCH_UP, x, y - t->grip, now); }
    t->p = -1;
}

void touch_read(struct touch *t, struct scene *s)
{
    struct input_event ev[64]; ssize_t r;
    while ((r = read(t->fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < (size_t)r / sizeof ev[0]; i++) {
            if (ev[i].type == TOUCH_MARK) { t->gst = ev[i].code; t->gpull |= ev[i].value; continue; }   /* the gate's word */
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
                    if (!t->waking && !t->held) scene_touch(s, TOUCH_UP, x, y - t->grip, at);
                    else if (t->waking) t->later = 0;            /* (the waking touch's other fingers: its too) */
                    t->held = 0;
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
                if (t->p < 0) { t->gpull = 0; continue; }
                touch_map(t, t->x[t->p], t->y[t->p], &x, &y);
                y -= t->grip;                                    /* the layer's rows start under the grip */
                /* the gate: a touch it holds from the app is not ours either, until it is a pull: then the drawer's,
                 * from above the layer's top (the scene's edge, as the grip's rows were) to where the finger is */
                if (fresh && t->gst == TG_HOLD) t->held = 1;
                if (t->gpull && t->held) {
                    int x0, y0; touch_map(t, t->x0[t->p], t->y0[t->p], &x0, &y0); (void)y0;
                    t->held = 0; t->gpull = 0;
                    scene_touch(s, TOUCH_DOWN, x0, -1, t->pt0);
                    scene_touch(s, TOUCH_MOVE, x, y, at);
                    continue;
                }
                t->gpull = 0;
                if (t->held) continue;
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
