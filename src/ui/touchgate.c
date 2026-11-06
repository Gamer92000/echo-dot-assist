/* The touch screen between the panel and Android: touchgate.h says why.  Measured on the Show 5 (goodix-ts, 480 x 960,
 * multitouch protocol B: a frame is ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_TOUCH_MAJOR, positions, SYN_REPORT; ~100 Hz
 * while a finger moves). */
#include "touchgate.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <linux/uinput.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* the drawer's part of the top edge: a touch that starts within 10 % of it (48 px on the Show 5: a fingertip below the
 * edge, not only its very top, which 4 % asked for) and in its middle 80 % is held (the corners stay the app's at
 * once: its own menus and buttons up there; a tap anywhere still reaches the app on its lift); a pull once it moved 5 %
 * of that axis inwards, mostly straight; anything else once it went 3 % sideways, or rested 300 ms (a long press of the
 * app's own), or at the latest after 600 ms */
#define BAND    .10f
#define ALONG0  .10f
#define ALONG1  .90f
#define PULL    .05f
#define SIDE    .03f
#define REST_MS 300
#define REST    .02f
#define HOLD_MS 600

void tg_init(struct tg *g, const struct input_absinfo *ax, const struct input_absinfo *ay)
{
    memset(g, 0, sizeof *g);
    g->xmin = ax->minimum; g->xmax = ax->maximum; g->ymin = ay->minimum; g->ymax = ay->maximum;
    for (int i = 0; i < TG_SLOTS; i++) { g->id[i] = -1; g->x[i] = ax->value; g->y[i] = ay->value; }
}

static int fingers(const struct tg *g) { int n = 0; for (int i = 0; i < TG_SLOTS; i++) n += g->id[i] >= 0; return n; }

static void put(struct input_event *e, int *n, int type, int code, int value)
{
    memset(&e[*n], 0, sizeof e[*n]); e[*n].type = (unsigned short)type; e[*n].code = (unsigned short)code; e[*n].value = value; (*n)++;
}

/* a frame on to Android: its events, then every finger's position written again, since the panel leaves out a value
 * equal to its last and Android's copy may have heard another last one (a swallowed pull's end); the copy drops what
 * it already holds */
static int forward(struct tg *g, const struct input_event *ev, int n, tg_emit emit, void *ctx)
{
    struct input_event o[TG_FRAME + 3 * TG_SLOTS + 2]; int k = 0;
    for (int i = 0; i < n && k < TG_FRAME; i++) if (ev[i].type != EV_SYN) o[k++] = ev[i];
    int any = 0;
    for (int i = 0; i < TG_SLOTS; i++) if (g->id[i] >= 0) {
        put(o, &k, EV_ABS, ABS_MT_SLOT, i); put(o, &k, EV_ABS, ABS_MT_POSITION_X, g->x[i]); put(o, &k, EV_ABS, ABS_MT_POSITION_Y, g->y[i]); any = 1;
    }
    if (any) put(o, &k, EV_ABS, ABS_MT_SLOT, g->slot);
    put(o, &k, EV_SYN, SYN_REPORT, 0);
    return emit(ctx, o, k) ? -1 : 0;
}

/* the held touch's start on to Android after all: its first frame where it began (then the frame that decided, or its
 * fingers where they are now) */
static int release(struct tg *g, tg_emit emit, void *ctx)
{
    struct input_event o[TG_FRAME + 6]; int k = 0;
    for (int i = 0; i < g->firstn && k < TG_FRAME; i++) if (g->first[i].type != EV_SYN) o[k++] = g->first[i];
    put(o, &k, EV_ABS, ABS_MT_SLOT, g->hs); put(o, &k, EV_ABS, ABS_MT_POSITION_X, g->hx); put(o, &k, EV_ABS, ABS_MT_POSITION_Y, g->hy);
    put(o, &k, EV_ABS, ABS_MT_SLOT, g->slot);
    put(o, &k, EV_SYN, SYN_REPORT, 0);
    return emit(ctx, o, k) ? -1 : 0;
}

/* how far the held finger went in from its edge and along it, in panel units, and the inward axis's length */
static void moved(const struct tg *g, int *in, int *side, int *len)
{
    int dx = g->x[g->hs] - g->hx, dy = g->y[g->hs] - g->hy;
    switch (g->edge[0] == 'x' ? (g->edge[1] == '0' ? 0 : 1) : (g->edge[1] == '0' ? 2 : 3)) {
    case 0: *in = dx; *side = abs(dy); *len = g->xmax - g->xmin; break;
    case 1: *in = -dx; *side = abs(dy); *len = g->xmax - g->xmin; break;
    case 2: *in = dy; *side = abs(dx); *len = g->ymax - g->ymin; break;
    default: *in = -dy; *side = abs(dx); *len = g->ymax - g->ymin; break;
    }
}

static float along(const struct tg *g)
{
    return g->edge[0] == 'x' ? (float)(g->hy - g->ymin) / (float)(g->ymax - g->ymin) : (float)(g->hx - g->xmin) / (float)(g->xmax - g->xmin);
}

/* the finger in slot s starts in the bar's part of edge top */
static int in_band(const struct tg *g, int s, const char *top)
{
    if (!top || (top[0] != 'x' && top[0] != 'y') || (top[1] != '0' && top[1] != '1')) return 0;
    if (g->xmax <= g->xmin || g->ymax <= g->ymin) return 0;
    float fx = (float)(g->x[s] - g->xmin) / (float)(g->xmax - g->xmin), fy = (float)(g->y[s] - g->ymin) / (float)(g->ymax - g->ymin);
    float n = top[0] == 'x' ? fx : fy, a = top[0] == 'x' ? fy : fx;
    if (top[1] == '1') n = 1.f - n;
    return n < BAND && a >= ALONG0 && a <= ALONG1;
}

static int frame(struct tg *g, long long now, const char *top, tg_emit emit, void *ctx, float *at)
{
    int before[TG_SLOTS]; memcpy(before, g->id, sizeof before);
    int was = fingers(g);
    for (int i = 0; i < g->frn; i++) {
        const struct input_event *e = &g->fr[i];
        if (e->type != EV_ABS) continue;
        if (e->code == ABS_MT_SLOT) g->slot = e->value >= 0 && e->value < TG_SLOTS ? e->value : 0;
        else if (e->code == ABS_MT_TRACKING_ID) g->id[g->slot] = e->value;
        else if (e->code == ABS_MT_POSITION_X) g->x[g->slot] = e->value;
        else if (e->code == ABS_MT_POSITION_Y) g->y[g->slot] = e->value;
    }
    int now_n = fingers(g), fresh = -1, nfresh = 0;
    for (int i = 0; i < TG_SLOTS; i++) if (g->id[i] >= 0 && g->id[i] != before[i]) { fresh = i; nfresh++; }
    switch (g->state) {
    case TG_IDLE:
        if (!now_n) return forward(g, g->fr, g->frn, emit, ctx);
        if (!was && nfresh == 1 && now_n == 1 && in_band(g, fresh, top)) {
            g->state = TG_HOLD; memcpy(g->edge, top, 2); g->edge[2] = 0;
            memcpy(g->first, g->fr, (size_t)g->frn * sizeof *g->fr); g->firstn = g->frn;
            g->hs = fresh; g->hx = g->x[fresh]; g->hy = g->y[fresh]; g->t0 = now;
            return 0;
        }
        g->state = TG_PASS;
        return forward(g, g->fr, g->frn, emit, ctx);
    case TG_PASS:
        if (!now_n) g->state = TG_IDLE;
        return forward(g, g->fr, g->frn, emit, ctx);
    case TG_SWALLOW:
        if (!now_n) g->state = TG_IDLE;
        return 0;
    }
    /* TG_HOLD */
    int in, side, len; moved(g, &in, &side, &len);
    if (g->id[g->hs] >= 0 && now_n == 1 && in >= PULL * (float)len && in > 2 * side) {
        g->state = TG_SWALLOW;
        if (at) *at = along(g);
        return 1;
    }
    if (g->id[g->hs] < 0 || now_n > 1 || (side >= SIDE * (float)len && side >= in)) {
        /* lifted (a tap), a second finger, or sideways: the app's from its start */
        if (release(g, emit, ctx)) return -1;
        g->state = now_n ? TG_PASS : TG_IDLE;
        return forward(g, g->fr, g->frn, emit, ctx);
    }
    return 0;
}

int tg_event(struct tg *g, const struct input_event *ev, long long now, const char *top, tg_emit emit, void *ctx, float *at)
{
    if (ev->type == EV_SYN && ev->code == SYN_DROPPED) { g->frn = 0; return 0; }
    if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        int r = frame(g, now, top, emit, ctx, at);
        g->frn = 0;
        return r;
    }
    if (g->frn < TG_FRAME) g->fr[g->frn++] = *ev;
    return 0;
}

int tg_wait_ms(const struct tg *g, long long now)
{
    if (g->state != TG_HOLD) return -1;
    long long w = g->t0 + REST_MS - now;
    return w < 0 ? 0 : (int)w;
}

int tg_tick(struct tg *g, long long now, tg_emit emit, void *ctx)
{
    if (g->state != TG_HOLD || now - g->t0 < REST_MS) return 0;
    int in, side, len; moved(g, &in, &side, &len);
    if (now - g->t0 < HOLD_MS && in >= REST * (float)len) return 0;     /* coming down slowly: still maybe a pull */
    if (release(g, emit, ctx)) return -1;
    g->state = TG_PASS;
    return forward(g, NULL, 0, emit, ctx);
}

/* ---------------------------------------------------------------- the uinput copy */

#define BITS(n) (((n) + 8 * (int)sizeof(unsigned long) - 1) / (8 * (int)sizeof(unsigned long)))
static int bit(const unsigned long *b, int i) { return (int)((b[i / (8 * sizeof *b)] >> (i % (8 * sizeof *b))) & 1); }

int tg_mirror(int fd)
{
    const char *path = getenv("HASSMIC_UINPUT");
    int u = open(path ? path : "/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (u < 0) return -1;
    struct uinput_user_dev d; memset(&d, 0, sizeof d);
    /* the panel's name and ids, so Android takes it for the same internal touch screen (any .idc by name or ids too) */
    if (ioctl(fd, EVIOCGNAME(sizeof d.name - 1), d.name) < 0 || ioctl(fd, EVIOCGID, &d.id) < 0) goto fail;
    unsigned long types[BITS(EV_MAX + 1)], codes[BITS(KEY_MAX + 1)], props[BITS(INPUT_PROP_MAX + 1)];
    memset(types, 0, sizeof types);
    if (ioctl(fd, EVIOCGBIT(0, sizeof types), types) < 0) goto fail;
    static const struct { int type, max; unsigned long set; } kinds[] = {
        { EV_KEY, KEY_MAX, UI_SET_KEYBIT }, { EV_ABS, ABS_MAX, UI_SET_ABSBIT }, { EV_MSC, MSC_MAX, UI_SET_MSCBIT },
        { EV_REL, REL_MAX, UI_SET_RELBIT }, { EV_SW, SW_MAX, UI_SET_SWBIT } };
    for (unsigned k = 0; k < sizeof kinds / sizeof *kinds; k++) {
        if (!bit(types, kinds[k].type)) continue;
        if (ioctl(u, UI_SET_EVBIT, kinds[k].type) < 0) goto fail;
        memset(codes, 0, sizeof codes);
        if (ioctl(fd, EVIOCGBIT(kinds[k].type, sizeof codes), codes) < 0) goto fail;
        for (int c = 0; c <= kinds[k].max; c++) if (bit(codes, c)) {
            if (ioctl(u, kinds[k].set, c) < 0) goto fail;
            if (kinds[k].type == EV_ABS) {
                struct input_absinfo a;
                if (ioctl(fd, EVIOCGABS(c), &a) < 0) goto fail;
                d.absmin[c] = a.minimum; d.absmax[c] = a.maximum; d.absfuzz[c] = a.fuzz; d.absflat[c] = a.flat;
            }
        }
    }
    /* INPUT_PROP_DIRECT: without it Android makes a touch pad of it (a pointer on the screen) */
    memset(props, 0, sizeof props);
    if (ioctl(fd, EVIOCGPROP(sizeof props), props) >= 0)
        for (int p = 0; p <= INPUT_PROP_MAX; p++) if (bit(props, p) && ioctl(u, UI_SET_PROPBIT, p) < 0) goto fail;
    if (write(u, &d, sizeof d) != (ssize_t)sizeof d || ioctl(u, UI_DEV_CREATE) < 0) goto fail;
    if (path) return u;                                 /* tests: a stand-in, no node */
    /* Android reads the copy only through its node in /dev/input, which ueventd makes: wait for it (up to 2 s) and make
     * sure it is that device's (a stray file in its place, as a test once left, and Android never sees the copy; the
     * panel grabbed then, the screen would take no touch at all) */
    char sys[64] = "";
    if (ioctl(u, UI_GET_SYSNAME(sizeof sys - 1), sys) < 0) goto gone;
    for (int t = 0; t < 40; t++) {
        char p[400], dev[32] = ""; snprintf(p, sizeof p, "/sys/class/input/%s", sys);
        DIR *dir = opendir(p); struct dirent *e; char ev[300] = "";
        while (dir && (e = readdir(dir))) if (!strncmp(e->d_name, "event", 5)) snprintf(ev, sizeof ev, "%s", e->d_name);
        if (dir) closedir(dir);
        if (*ev) {
            snprintf(p, sizeof p, "/sys/class/input/%s/%s/dev", sys, ev);
            FILE *f = fopen(p, "r"); unsigned ma = 0, mi = 0;
            if (f) { if (fgets(dev, sizeof dev, f) && sscanf(dev, "%u:%u", &ma, &mi) == 2) { } fclose(f); }
            struct stat st; snprintf(p, sizeof p, "/dev/input/%s", ev);
            if (ma && !stat(p, &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == ma && minor(st.st_rdev) == mi) return u;
        }
        usleep(50000);
    }
gone:
    ioctl(u, UI_DEV_DESTROY);
fail:
    close(u);
    return -1;
}
