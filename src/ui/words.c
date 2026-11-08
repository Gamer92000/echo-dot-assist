/*
 * Words and timers over the screen (scene.h scene_words; issue #104): what was said and Home Assistant's reply, and
 * the running and ringing timers, as Alexa's own screens show them.  A layer of its own (android.c), see-through, over
 * the face, the music and any app, under the band, and hidden while the drawer is out: the drawer is what a finger is
 * on then.  Its picture is drawn here only when what it shows changes (the words, a timer's second); fading and a
 * ringing timer's pulse are the layer's own alpha (SurfaceControl.setAlpha), so they cost no pixels.
 *
 * One picture at a time: the words while they show (from speech to text until hold_s after the satellite is idle
 * again; a new request takes them away at once), else what rings, else the running timers (three, the one that ends
 * first on top).  Under them a shade of 75 % black over the whole layer, so they stay readable over whatever the
 * screen shows.  Text is the platform's (android.c: Skia, the system font, every script it covers), wrapped here at
 * spaces by the platform's own widths; what does not fit at the smallest size ends in the platform's ellipsis.
 *
 * Touches: the layer takes none (a SurfaceFlinger layer has no input), but over our own screen (the face, the music)
 * the scene gives them to it while it shows (scene_touch, words_tap), nothing under it gets them: a tap takes the words
 * away (hassmic too: "words clear"), hides the running timers until their list changes, or on a ringing one's Stop
 * stops it ("timer stop").  Over an app the panel is Android's (read beside it), so while they cover one the platform
 * grabs it for them (android.c, touch_grab, between touches only) and the same goes there: the app gets none of
 * those touches, and has them back once the words go, the timers are tapped away or the ring stops.  Where the panel
 * cannot be grabbed, a ring over an app brings our screen up, Stop and all, and the app back once it has stopped
 * ringing, as stock's alerts take the screen.  The top edge still pulls the drawer.
 *
 * Sizes in shares of the layer's height: the first try (an Echo Show 5, 2026-11-03, the text in a strip of 55 % at the
 * bottom, the reply at 15 % of it, four lines at most) was small and cut short on a screen black above it, so the
 * words take the whole layer, the reply from 14 % of its height down to 7.5 %, as many lines as fit, in the middle.
 */
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FADE_IN_MS   200
#define FADE_OUT_MS  600
#define STEP_MS      33                 /* while it fades: the layer's alpha only */
#define PULSE_STEP   66                 /* a ringing one's pulse: 15 transactions a second, each a composite of the
                                           whole screen; the 1.2 s cycle looks the same */
#define RING_AGAIN_MS 10000             /* our screen asked for again while the app keeps the front over a ring */
#define PULSE_MS     1200               /* a ringing timer: bright and dim by turns, a whole cycle */
#define PULSE_LOW    0.45f
#define SHADE        0xBF000000u        /* 75 % black, premultiplied (the layer is RGBA_8888) */
#define MAX_LINES    16
#define HEARD_LINES  3
#define SHOWN_TIMERS 3                  /* more is not read off a screen */
#define TEXT_MAX     1024

enum { W_NONE, W_WORDS, W_RING, W_TIMERS };

/* ---------------------------------------------------------------- messages (scene.h) */

int words_msg(struct scene *s, const char *cmd, const char *a)
{
    struct words *w = &s->wd;
    if (eq(cmd, "wo")) {
        int on = 0, t = 0, hold = 8;
        if (sscanf(a, "%d %d %d", &on, &t, &hold) >= 2) { w->on = on; w->timers_on = t; w->hold_s = hold < 0 ? 0 : hold; }
    } else if (eq(cmd, "wh")) snprintf(w->heard, sizeof w->heard, "%s", a);
    else if (eq(cmd, "wr")) snprintf(w->reply, sizeof w->reply, "%s", a);
    else if (eq(cmd, "ws")) { long long ago = atoll(a); w->idle = ago >= 0; w->idle_at = s->now - (ago > 0 ? ago : 0); }
    else if (eq(cmd, "tl")) {                                   /* <n>\t<left ms> <total s> <active> <name>... */
        w->nt = 0; w->timers_hidden = 0;                        /* (tapped away: back with a change) */
        const char *tab = strchr(a, '\t'); char hd[32]; int n_ = 0;   /* "<n> <seq>", up to the first record */
        snprintf(hd, sizeof hd, "%.*s", (int)(tab ? tab - a : (long)strlen(a)), a);
        w->tl_seq = 0; sscanf(hd, "%d %u", &n_, &w->tl_seq);
        for (const char *p = strchr(a, '\t'); p && w->nt < WTIMERS; ) {
            const char *e = strchr(++p, '\t');                  /* one record, to the next tab */
            char rec[160]; snprintf(rec, sizeof rec, "%.*s", (int)(e ? e - p : (long)strlen(p)), p);
            long long left = 0; int total = 0, active = 0, k = 0;
            if (sscanf(rec, "%lld %d %d%n", &left, &total, &active, &k) == 3) {
                struct wtimer *t = &w->t[w->nt++];
                snprintf(t->name, sizeof t->name, "%s", rec[k] == ' ' ? rec + k + 1 : rec + k);
                t->total = total; t->active = active; t->left = left < 0 ? 0 : left; t->end = s->now + t->left;
            }
            p = e;
        }
    } else return 0;
    w->ver++;
    return 1;
}

/* ---------------------------------------------------------------- what shows */

static long long left_ms(const struct scene *s, const struct wtimer *t)
{
    long long l = t->active ? t->end - s->now : t->left;
    return l < 0 ? 0 : l;
}

static int what(const struct scene *s)
{
    const struct words *w = &s->wd;
    if (w->on && (w->heard[0] || w->reply[0]) && (!w->idle || !w->hold_s || s->now - w->idle_at < w->hold_s * 1000LL)) return W_WORDS;
    if (w->timers_on && s->timers[0]) return W_RING;
    if (w->timers_on && w->nt && !w->timers_hidden) return W_TIMERS;
    return W_NONE;
}

static int by_left(const void *a, const void *b)
{
    const struct wtimer *x = a, *y = b;
    long long d = (x->active ? x->end : x->left + (1LL << 40)) - (y->active ? y->end : y->left + (1LL << 40));   /* paused last */
    return d < 0 ? -1 : d > 0;
}

/* the running timers in the order shown, their seconds as shown (rounded up: "5:00" as it starts) */
static int sorted(const struct scene *s, struct wtimer *t)
{
    int n = s->wd.nt;
    memcpy(t, s->wd.t, sizeof s->wd.t[0] * (size_t)n);
    qsort(t, (size_t)n, sizeof t[0], by_left);
    return n > SHOWN_TIMERS ? SHOWN_TIMERS : n;
}

int scene_words(struct scene *s, long long now, float *alpha, int *next_ms)
{
    struct words *w = &s->wd;
    s->now = now;
    float dt = w->alpha_at ? clampf((float)(now - w->alpha_at), 0, 2 * STEP_MS) : 0;   /* (a fade starts where it was, not
                                                                   from the last wait) */
    w->alpha_at = now;
    *next_ms = -1; *alpha = 0; w->shown = 0;
    if (!s->screen_on || drawer_open(s)) { w->alpha = 0; w->solid = 0; return 0; }   /* (back: it fades in again) */
    int k = what(s), redraw = 0;
    /* a ring over an app: our screen up, so that its Stop can be tapped; that app again once it has stopped ringing (not
     * when another took the screen meanwhile) */
    if (k == W_RING && w->grab < 0 && !scene_visible(s) && s->top[0] && (!w->ring_from[0] || now - w->ring_at >= RING_AGAIN_MS)) {
        if (!w->ring_from[0]) w->own_was = s->own_asked;       /* (asked again: the app that kept the front) */
        snprintf(w->ring_from, sizeof w->ring_from, "%s", s->top);
        send(s, "log a timer rings over %s: our screen up for its Stop", s->top);
        s->own_asked = 1; w->ring_at = now;                    /* (as ask_own, a choice the page made meanwhile kept) */
        go_home(s, V_FACE);
    } else if (k != W_RING && w->ring_from[0]) {
        s->own_asked = w->own_was;                              /* (the home app back after an app it was not) */
        if (is_home(s->top) && !on_its_way(s)) launch(s, w->ring_from, NULL);
        w->ring_from[0] = 0;
    }
    unsigned secs = 0;
    long long tick = 0;                                          /* a running timer's next second */
    if (k == W_TIMERS) {
        struct wtimer t[WTIMERS]; int n = sorted(s, t);
        for (int i = 0; i < n; i++) {
            long long l = left_ms(s, &t[i]);
            secs = secs * 31 + (unsigned)((l + 999) / 1000) * 2 + (unsigned)t[i].active;
            if (t[i].active && l > 0) { long long d = (l - 1) % 1000 + 1; if (!tick || d < tick) tick = d; }
        }
    }
    if (k != W_NONE && (k != w->pic || w->ver != w->pic_ver || secs != w->pic_secs)) {
        w->pic = k; w->pic_ver = w->ver; w->pic_secs = secs; redraw = 1;
    }
    if (k == W_NONE && w->pic == W_WORDS && !w->heard[0] && !w->reply[0]) w->alpha = 0;   /* a new request: gone at once */
    float target = k == W_NONE ? 0 : 1;
    if (k == W_RING) {
        float ph = (float)(now % PULSE_MS) / PULSE_MS;
        target = PULSE_LOW + (1 - PULSE_LOW) * (0.5f + 0.5f * cosf(6.2831853f * ph));
    }
    if (w->alpha < target) w->alpha = k == W_RING && w->alpha >= PULSE_LOW ? target : fminf(target, w->alpha + dt / FADE_IN_MS);
    else if (w->alpha > target) w->alpha = k == W_RING ? target : fmaxf(target, w->alpha - dt / FADE_OUT_MS);
    if (k == W_NONE && w->alpha <= 0) { w->pic = W_NONE; w->solid = 0; return 0; }
    *alpha = w->alpha; w->shown = k != W_NONE;
    if (!w->shown) w->solid = 0; else if (w->alpha >= 0.5f) w->solid = 1;   /* (a ring's pulse dips below: still solid) */
    if (k == W_RING) *next_ms = PULSE_STEP;
    else if (w->alpha != target) *next_ms = STEP_MS;
    else if (k == W_TIMERS && tick) *next_ms = (int)tick;
    else if (k == W_WORDS && w->idle && w->hold_s) *next_ms = (int)(w->idle_at + w->hold_s * 1000LL - now);
    if (*next_ms == 0) *next_ms = 1;
    if (k == W_NONE) *next_ms = STEP_MS;                        /* (fading out) */
    return redraw ? 2 : 1;
}

/* ---------------------------------------------------------------- touches */

/* only once faded in past half: a tap meant for what is under words just coming is still that; over an app only while
 * the panel is ours (else the app has the touch too) */
int words_shown(const struct scene *s)
{
    return s->wd.shown && s->wd.solid && !drawer_open(s) && s->screen_on && (scene_visible(s) || s->wd.grab == 1);
}

void scene_words_grab(struct scene *s, int state) { s->wd.grab = state; }
int  scene_words_wants_grab(const struct scene *s)
{
    return s->wd.grab >= 0 && s->wd.shown && s->wd.solid && !drawer_open(s) && s->screen_on && !scene_visible(s);
}

void words_tap(struct scene *s, int x, int y)
{
    struct words *w = &s->wd;
    if (w->pic == W_WORDS) {                                    /* gone, here and in hassmic (a new UI gets none) */
        w->heard[0] = w->reply[0] = 0; w->ver++;
        send(s, "words clear");
    } else if (w->pic == W_TIMERS) { w->timers_hidden = 1; send(s, "timers hide %u", w->tl_seq); }   /* (hassmic: the screen not held on) */
    else if (w->pic == W_RING && x >= w->stop.x0 && x < w->stop.x1 && y >= w->stop.y0 && y < w->stop.y1) send(s, "timer stop");
}

/* ---------------------------------------------------------------- drawing */

/* the width of n bytes of str as the platform draws them */
static float width(struct scene *s, const char *str, size_t n, float px, int bold)
{
    char b[TEXT_MAX];
    if (n >= sizeof b) n = sizeof b - 1;
    memcpy(b, str, n); b[n] = 0;
    return (float)text_measure(s, b, px, bold);
}

struct wline { const char *a; int n; };

/* text wrapped to maxw at spaces (a word longer than a line: broken between code points); lines stored up to max,
 * all of them counted */
static int wrap(struct scene *s, const char *t, float px, int bold, float maxw, struct wline *out, int max)
{
    int n = 0; const char *p = t;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p, *end = p, *q = p;
        while (*q) {
            const char *we = q;
            while (*we && *we != ' ') we++;
            if (width(s, start, (size_t)(we - start), px, bold) > maxw) break;
            end = we; q = we;
            while (*q == ' ') q++;
        }
        if (end == start) {                                     /* a word longer than the line */
            const char *c = start;
            while (*c && *c != ' ') {
                const char *nx = c + 1;
                while ((*nx & 0xC0) == 0x80) nx++;
                if (end > start && width(s, start, (size_t)(nx - start), px, bold) > maxw) break;
                end = c = nx;
            }
        }
        if (n < max) out[n] = (struct wline){ start, (int)(end - start) };
        n++; p = end;
    }
    return n;
}

static void put_text(struct scene *s, struct gfx *g, const char *str, size_t n, float px, int bold, float maxw, float x, float y, uint32_t rgb)
{
    char b[TEXT_MAX];
    if (n >= sizeof b) n = sizeof b - 1;
    while (n && ((unsigned char)str[n] & 0xC0) == 0x80) n--;    /* (a cut between code points) */
    memcpy(b, str, n); b[n] = 0;
    const struct ui_text *t = text_mask(s, b, px, bold, maxw);
    if (t) gfx_mask(g, (int)lroundf(x), (int)lroundf(y), t->m, t->w, t->h, t->stride, rgb, 255);
}

static void words_draw(struct scene *s, struct gfx *g)
{
    const char *heard = s->wd.heard, *reply = s->wd.reply;
    float H = (float)s->h, margin = H * 0.08f, side = s->w * 0.05f, bar = fmaxf(3, s->w * 0.004f), gap = bar * 3;
    float x = side + bar + gap, maxw = s->w - x - side;
    struct wline hl[MAX_LINES], rl[MAX_LINES];
    int nh = 0, nr = 0, hshown = 0, rshown = 0;
    float L = 0, Lh = 0, hblock = 0;
    /* the largest reply whose block fits: the heard lines (three at most), a gap, every reply line */
    for (L = H * 0.14f; ; L *= 0.93f) {
        int last = L <= H * 0.075f;
        if (last) L = H * 0.075f;
        Lh = fmaxf(L * 0.62f, H * 0.065f);
        nh = heard[0] ? wrap(s, heard, Lh, 0, maxw, hl, MAX_LINES) : 0;
        nr = reply[0] ? wrap(s, reply, L, 0, maxw, rl, MAX_LINES) : 0;
        hshown = nh < HEARD_LINES ? nh : HEARD_LINES;
        hblock = hshown ? hshown * Lh * 1.05f + (nr ? Lh * 0.5f : 0) : 0;
        rshown = (int)((H - 2 * margin - hblock) / (L * 1.05f));
        if (rshown > nr) rshown = nr;
        if (rshown > MAX_LINES) rshown = MAX_LINES;
        if (rshown >= nr || last) break;
    }
    if (nr && rshown < 1) rshown = 1;
    float block = hblock + rshown * L * 1.05f, y = margin + fmaxf(0, (H - 2 * margin - block) / 2);
    for (int i = 0; i < hshown; i++) {                          /* the last one shown, cut: the rest after it, ellipsised */
        size_t n = i == hshown - 1 && nh > hshown ? strlen(hl[i].a) : (size_t)hl[i].n;
        if (n > (size_t)hl[i].n + 64) n = (size_t)hl[i].n + 64;
        put_text(s, g, hl[i].a, n, Lh, 0, maxw, x, y, DIM);
        y += Lh * 1.05f;
    }
    if (hshown && nr) y += Lh * 0.5f;
    float r0 = y;
    for (int i = 0; i < rshown; i++) {
        size_t n = i == rshown - 1 && nr > rshown ? strlen(rl[i].a) : (size_t)rl[i].n;
        if (n > (size_t)rl[i].n + 64) n = (size_t)rl[i].n + 64;
        put_text(s, g, rl[i].a, n, L, 0, maxw, x, y, FG);
        y += L * 1.05f;
    }
    if (rshown) gfx_rrect(g, side, r0 + L * 0.15f, side + bar, y - L * 0.2f, bar / 2, CYAN, 255);   /* the band's listening colour */
}

/* the time left and its unit (SI's symbols, the same in every language): "1:02:05" h, "4:59" min, under a minute the
 * seconds alone ("45" s: "0:45 min" reads slowly); ringing "0:00" and none */
static const char *clock_text(char *b, size_t cap, long long ms, int ringing)
{
    long long t = (ms + 999) / 1000;
    if (ringing) { snprintf(b, cap, "0:00"); return ""; }
    if (t >= 3600) { snprintf(b, cap, "%lld:%02lld:%02lld", t / 3600, t / 60 % 60, t % 60); return "h"; }
    if (t >= 60) { snprintf(b, cap, "%lld:%02lld", t / 60, t % 60); return "min"; }
    snprintf(b, cap, "%lld", t); return "s";
}

static void timers_draw(struct scene *s, struct gfx *g, int ringing)
{
    struct wtimer t[WTIMERS]; int n;
    if (ringing) { memset(t, 0, sizeof t[0]); n = 1; }
    else n = sorted(s, t);
    if (n <= 0) return;
    float H = (float)s->h, W = (float)s->w, margin = H * 0.08f, room = (H - 2 * margin) / n;
    float ts = ringing ? H * 0.28f : fminf(room * 0.55f, H * 0.34f), ns = fmaxf(ts * 0.26f, H * 0.065f), us = ts * 0.42f;   /* time, name, unit */
    float bh = fmaxf(4, H * 0.012f), bw = W * 0.5f, between = n > 1 ? ns * 0.9f : 0;
    float ph = ringing ? H * 0.15f : 0, pgap = ringing ? H * 0.06f : 0;                         /* the Stop pill */
    float block = ns * 1.1f + ts + bh * 2 + between, y0 = margin + fmaxf(0, (H - 2 * margin - block * n + between - ph - pgap) / 2);
    s->wd.stop = (struct rect){ 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        float y = y0 + i * block;
        long long l = left_ms(s, &t[i]);
        char clk[32], zero[32]; const char *unit = clock_text(clk, sizeof clk, l, ringing);
        uint32_t col = ringing ? ORANGE : t[i].active ? FG : FAINT;
        const char *name = ringing ? s->timers : t[i].name;     /* (ringing: every one that rings, "tea, 5 min") */
        if (name[0]) {
            float nw = width(s, name, strlen(name), ns, 0);
            put_text(s, g, name, strlen(name), ns, 0, W * 0.9f, (W - fminf(nw, W * 0.9f)) / 2, y, DIM);
        }
        y += ns * 1.1f;
        /* placed by the width of the same text with every digit a 0, so a running clock keeps still */
        size_t k = 0; for (const char *c = clk; *c && k + 1 < sizeof zero; c++) zero[k++] = *c >= '0' && *c <= '9' ? '0' : *c;
        zero[k] = 0;
        float tw = width(s, zero, k, ts, 1), uw = unit[0] ? width(s, unit, strlen(unit), us, 0) : 0, sp = unit[0] ? ts * 0.12f : 0;
        float x = (W - tw - sp - uw) / 2;
        put_text(s, g, clk, strlen(clk), ts, 1, W, x, y, col);
        /* the unit on the same baseline: each line's text is 0.8 of its height, centred in it */
        if (unit[0]) put_text(s, g, unit, strlen(unit), us, 0, W, x + tw + sp, y + 0.78f * ts - 0.78f * us, t[i].active || ringing ? DIM : FAINT);
        y += ts + bh;
        float f = ringing || t[i].total <= 0 ? 1 : clampf((float)l / (t[i].total * 1000.0f), 0, 1);
        float bx = (W - bw) / 2;
        gfx_rrect(g, bx, y, bx + bw, y + bh, bh / 2, LINE, 255);   /* the track */
        if (f > 0) gfx_rrect(g, bx, y, bx + fmaxf(bh, bw * f), y + bh, bh / 2, ringing ? ORANGE : t[i].active ? CYAN : FAINT, 255);
        if (ringing) {                                          /* Stop, as the face's, large: tapped across the room's */
            float pw = fmaxf(W * 0.3f, ph * 3), px0 = (W - pw) / 2, py0 = y + bh + pgap;    /* reach */
            gfx_rrect(g, px0, py0, px0 + pw, py0 + ph, ph / 2, ORANGE, 255);
            float lw = width(s, "Stop", 4, ph * 0.6f, 1);
            put_text(s, g, "Stop", 4, ph * 0.6f, 1, pw, (W - lw) / 2, py0 + ph * 0.2f, 0x1A1D22);
            /* (a finger's margin around it: a tap that lands a little off still stops it) */
            s->wd.stop = (struct rect){ (int)(px0 - H * 0.04f), (int)(py0 - H * 0.04f), (int)(px0 + pw + H * 0.04f), (int)(py0 + ph + H * 0.04f) };
        }
    }
}

void scene_words_draw(struct scene *s, struct gfx *g)
{
    for (int y = 0; y < g->h; y++) {                            /* the shade: the layer's own alpha, not a blend */
        uint32_t *row = g->px + (size_t)y * g->stride;
        for (int x = 0; x < g->w; x++) row[x] = SHADE;
    }
    struct rect clip = g->clip;
    g->clip = (struct rect){ 0, 0, g->w, g->h };
    if (s->wd.pic == W_WORDS) words_draw(s, g);
    else if (s->wd.pic == W_RING) timers_draw(s, g, 1);
    else if (s->wd.pic == W_TIMERS) timers_draw(s, g, 0);
    g->clip = clip;
}

void scene_words_lost(struct scene *s) { s->wd.pic = W_NONE; }
