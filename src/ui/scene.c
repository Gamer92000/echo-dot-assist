/* See scene.h and ui.h.  Layout is in units of the Echo Show 5's layer (U()), animation by values easing towards
 * targets (exponentially, per frame time): once every value has arrived, frames stop until something is due (a blink,
 * the next minute, the next second of a playing track) or a message or touch comes. */
#include "ui.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define EDGE_TOP   22                   /* the top edge's grip, in units: a pull from there is the drawer's */
#define BACK_PX    140                  /* along the band: Back */
#define FLING_TAU  325.0f               /* a fling slows as Android's scrollers do: e^(-t/325 ms) */

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static unsigned rnd(struct scene *s) { s->rnd ^= s->rnd << 13; s->rnd ^= s->rnd >> 17; s->rnd ^= s->rnd << 5; return s->rnd; }
float frand(struct scene *s) { return (rnd(s) & 0xFFFFFF) / (float)0x1000000; }
int eq(const char *a, const char *b) { return !strcmp(a, b); }

int ease(struct anim *a, float dt, float tau)
{
    float d = a->t - a->v;
    if (fabsf(d) < 0.002f) { a->v = a->t; return 0; }
    a->v += d * (1 - expf(-dt / tau));
    return 1;
}

void fmt_time(char *out, size_t n, long long ms)
{
    long long sec = ms / 1000;
    if (sec >= 3600) snprintf(out, n, "%lld:%02lld:%02lld", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(out, n, "%lld:%02lld", sec / 60, sec % 60);
}

/* ---------------------------------------------------------------- the display list */

struct item *emit(struct scene *s, int id, int kind)
{
    if (s->n[s->cur] >= MAXITEMS || id < 0 || id >= MAXID) return NULL;
    struct item *it = &s->list[s->cur][s->n[s->cur]++];
    memset(it, 0, sizeof *it);                               /* compared with memcmp: no stray padding */
    it->id = (uint16_t)id; it->kind = (uint8_t)kind; it->alpha = 255; it->clip = s->clip;
    return it;
}

/* an item's box as far as its clip lets it show: what the diff and the drawing go by */
static int place(struct scene *s, struct item *it, struct rect box)
{
    it->box = rect_and(box, it->clip);
    if (rect_empty(it->box)) { s->n[s->cur]--; return 0; }   /* wholly cut away: not there at all */
    return 1;
}

void fill(struct scene *s, int id, struct rect r, uint32_t rgb)
{
    struct item *it = emit(s, id, K_FILL);
    if (it) { it->rgb = rgb; place(s, it, r); }
}

void rrect(struct scene *s, int id, float x0, float y0, float x1, float y1, float rad, uint32_t rgb, int alpha)
{
    if (alpha <= 0 || x1 <= x0 || y1 <= y0) return;
    struct item *it = emit(s, id, K_RRECT);
    if (!it) return;
    it->f[0] = x0; it->f[1] = y0; it->f[2] = x1; it->f[3] = y1; it->f[4] = rad; it->rgb = rgb; it->alpha = alpha;
    place(s, it, (struct rect){ (int)floorf(x0) - 1, (int)floorf(y0) - 1, (int)ceilf(x1) + 1, (int)ceilf(y1) + 1 });
}

void stroke(struct scene *s, int id, float x0, float y0, float x1, float y1, float rad, float t, uint32_t rgb, int alpha)
{
    if (alpha <= 0 || x1 <= x0 || y1 <= y0) return;
    struct item *it = emit(s, id, K_STROKE);
    if (!it) return;
    it->f[0] = x0; it->f[1] = y0; it->f[2] = x1; it->f[3] = y1; it->f[4] = rad; it->f[5] = t; it->rgb = rgb; it->alpha = alpha;
    place(s, it, (struct rect){ (int)floorf(x0) - 1, (int)floorf(y0) - 1, (int)ceilf(x1) + 1, (int)ceilf(y1) + 1 });
}

void arc(struct scene *s, int id, float cx, float cy, float r, float t, float a0, float a1, uint32_t rgb, int alpha)
{
    if (alpha <= 0) return;
    struct item *it = emit(s, id, K_ARC);
    if (!it) return;
    it->f[0] = cx; it->f[1] = cy; it->f[2] = r; it->f[3] = t; it->f[4] = a0; it->f[5] = a1; it->rgb = rgb; it->alpha = alpha;
    float e = r + t / 2 + 2;
    place(s, it, (struct rect){ (int)floorf(cx - e), (int)floorf(cy - e), (int)ceilf(cx + e), (int)ceilf(cy + e) });
}

void poly(struct scene *s, int id, const float *xy, int n, uint32_t rgb, int alpha)
{
    if (alpha <= 0 || n < 3 || n > 4) return;
    struct item *it = emit(s, id, K_POLY);
    if (!it) return;
    float mnx = xy[0], mxx = xy[0], mny = xy[1], mxy = xy[1];
    for (int i = 0; i < n; i++) {
        it->f[2 * i] = xy[2 * i]; it->f[2 * i + 1] = xy[2 * i + 1];
        mnx = fminf(mnx, xy[2 * i]); mxx = fmaxf(mxx, xy[2 * i]); mny = fminf(mny, xy[2 * i + 1]); mxy = fmaxf(mxy, xy[2 * i + 1]);
    }
    it->n = (uint8_t)n; it->rgb = rgb; it->alpha = alpha;
    place(s, it, (struct rect){ (int)floorf(mnx) - 2, (int)floorf(mny) - 2, (int)ceilf(mxx) + 2, (int)ceilf(mxy) + 2 });
}

void grad(struct scene *s, int id, struct rect r, uint32_t top, uint32_t bottom)
{
    struct item *it = emit(s, id, K_GRAD);
    if (!it) return;
    it->rgb = top; it->rgb2 = bottom;
    it->f[0] = (float)r.y0; it->f[1] = (float)r.y1;               /* the gradient's own span, whatever the clip leaves */
    place(s, it, r);
}

static unsigned hash(const char *k) { unsigned h = 2166136261u; while (*k) h = (h ^ (unsigned char)*k++) * 16777619u; return h; }
unsigned ui_hash(const char *k) { return hash(k); }

static struct ui_text_e *text_get(struct scene *s, const char *str, int px, int bold, int maxw)
{
    char key[sizeof s->text[0].key]; struct ui_text_e *e, *old = &s->text[0];
    snprintf(key, sizeof key, "%d|%d|%d|%s", px, bold, maxw, str);
    unsigned h = hash(key);
    for (int i = 0; i < NTEXT; i++) {
        e = &s->text[i];
        if (e->used && e->hash == h && eq(e->key, key)) { e->used = s->now + 1; return e->ok ? e : NULL; }
        if (e->used < old->used) old = e;
    }
    e = old;                                                 /* the least recently used goes */
    free(e->t.m); memset(e, 0, sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", key); e->hash = h;
    e->used = s->now + 1; e->ver = ++s->text_ver;
    e->ok = s->p->text && maxw > 0 && !s->p->text(s->ctx, str, px, bold, maxw, &e->t) && e->t.m;
    return e->ok ? e : NULL;
}

int text_width(struct scene *s, const char *str, float px, int bold, float maxw)
{
    if (!str || !str[0]) return 0;
    struct ui_text_e *e = text_get(s, str, (int)lroundf(px), bold, (int)maxw);
    return e ? e->t.w : 0;
}

int text(struct scene *s, int id, const char *str, float px, int bold, float maxw, float x, float y, int align, uint32_t rgb, int alpha)
{
    if (!str || !str[0] || alpha <= 0 || maxw < 4) return 0;
    struct ui_text_e *e = text_get(s, str, (int)lroundf(px), bold, (int)maxw);
    if (!e) return 0;
    int ix = (int)lroundf(align == 1 ? x - e->t.w / 2.0f : align == 2 ? x - e->t.w : x), iy = (int)lroundf(y);
    struct item *it = emit(s, id, K_TEXT);
    if (!it) return 0;
    it->ref = e; it->ver = e->ver; it->rgb = rgb; it->alpha = alpha;
    it->f[0] = (float)ix; it->f[1] = (float)iy;                    /* where the mask starts, whatever the clip leaves */
    place(s, it, (struct rect){ ix, iy, ix + e->t.w, iy + e->t.h });
    return e->t.w;
}

int image(struct scene *s, int id, const struct ui_image *im, float x, float y, float w, float h, float rad)
{
    if (!im || !im->px || w < 1 || h < 1) return 0;
    struct item *it = emit(s, id, K_IMG);
    if (!it) return 0;
    it->ref = im; it->ver = im->ver; it->f[4] = rad;
    int ix = (int)lroundf(x), iy = (int)lroundf(y);
    it->f[0] = (float)ix; it->f[1] = (float)iy; it->f[2] = (float)lroundf(w); it->f[3] = (float)lroundf(h);
    place(s, it, (struct rect){ ix, iy, ix + (int)lroundf(w), iy + (int)lroundf(h) });
    return 1;
}

void app_icon(struct scene *s, int id, const struct ui_icon *ic, float x, float y, float w, float h)
{
    if (!ic->px || w < 1 || h < 1) return;
    struct item *it = emit(s, id, K_ICON);
    if (!it) return;
    it->ref = ic;
    int ix = (int)lroundf(x), iy = (int)lroundf(y);
    it->f[0] = (float)ix; it->f[1] = (float)iy; it->f[2] = (float)lroundf(w); it->f[3] = (float)lroundf(h);
    place(s, it, (struct rect){ ix, iy, ix + (int)lroundf(w), iy + (int)lroundf(h) });
}

void clip_push(struct scene *s, struct rect r, struct rect *saved) { *saved = s->clip; s->clip = rect_and(s->clip, r); }
void clip_pop(struct scene *s, struct rect saved) { s->clip = saved; }

void hit(struct scene *s, float x0, float y0, float x1, float y1, int act, int arg)
{
    if (s->nhits >= NHITS) return;
    struct rect r = rect_and((struct rect){ (int)x0, (int)y0, (int)x1, (int)y1 }, s->clip);
    if (rect_empty(r) && act != A_DR_SHADE && act != A_M_SHADE) return;
    s->hits[s->nhits++] = (struct hit){ act == A_DR_SHADE || act == A_M_SHADE ? (struct rect){ 0, 0, s->w, s->h + s->band } : r, act, arg, 0, 0 };
}

void hit_key(struct scene *s, float x0, float y0, float x1, float y1, int act, int arg, unsigned key)
{
    hit(s, x0, y0, x1, y1, act, arg);
    if (s->nhits && s->hits[s->nhits - 1].act == act && s->hits[s->nhits - 1].arg == arg) s->hits[s->nhits - 1].key = key;
}

void hit_drag(struct scene *s, float x0, float y0, float x1, float y1, int act, int arg)
{
    hit(s, x0, y0, x1, y1, act, arg);
    if (s->nhits && s->hits[s->nhits - 1].act == act && s->hits[s->nhits - 1].arg == arg) s->hits[s->nhits - 1].flags = HIT_DRAG;
}

int pressed(struct scene *s, int act, int arg)
{
    return s->pressed && s->pact == act && s->parg == arg;
}

void scroll_area(struct scene *s, struct rect r, struct scroller *sc, int axis, float content)
{
    float view = axis ? (float)(r.y1 - r.y0) : (float)(r.x1 - r.x0);
    sc->axis = axis; sc->max = fmaxf(0, content - view);
    /* with the hits drawn before it: a shade drawn after it (the drawer, a sheet) keeps touches from it */
    if (s->nscrolls < NSCROLL) s->scrolls[s->nscrolls++] = (struct scroll_area){ rect_and(r, s->clip), sc, s->nhits };
}

void scroll_to(struct scroller *sc, float pos) { sc->pos = pos; sc->vel = 0; sc->held = 0; sc->moving = 0; }

void forget_scroller(struct scene *s, const struct scroller *sc)
{
    for (int i = 0; i < s->nscrolls; i++) if (s->scrolls[i].sc == sc) { memmove(&s->scrolls[i], &s->scrolls[i + 1], (size_t)(s->nscrolls - i - 1) * sizeof s->scrolls[0]); s->nscrolls--; i--; }
    for (int i = 0; i < s->nprev; i++) if (s->prev[i] == sc) s->prev[i] = NULL;
    if (s->sc == sc) { s->sc = NULL; s->sc_drag = 0; }
}

void send(struct scene *s, const char *fmt, ...)
{
    char line[600]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    for (char *c = line; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
    if (s->p->send) s->p->send(s->ctx, line);
}

int can(const struct scene *s, const char *cmd)
{
    char q[40]; snprintf(q, sizeof q, ",%s,", cmd);
    char all[260]; snprintf(all, sizeof all, ",%s,", s->mcmds);
    return s->music && strstr(all, q) != NULL;
}

/* Play/pause shows at once: Sendspin's word comes back after a round trip through Music Assistant, a second at times
 * (checkers, 2026-11-02).  Until it agrees (2.5 s at most), an older word on its way does not flip it back; if none
 * agrees by then, what it last said shows */
#define PEND_MS 2500
void music_toggle(struct scene *s)
{
    long long pos = music_pos(s);
    int to = s->mstate == 1 ? 2 : 1;
    send(s, "cmd %s", to == 2 ? "pause" : "play");
    s->mstate = to; s->mpos = pos; s->mpos_at = s->now;
    s->pend_state = to; s->pend_until = s->now + PEND_MS;
}

long long music_pos(const struct scene *s)
{
    long long p = s->mpos;
    if (s->mstate == 1) p += (s->now - s->mpos_at) * s->mspeed / 1000;
    if (s->mdur > 0 && p > s->mdur) p = s->mdur;
    return p < 0 ? 0 : p;
}

void scene_draw(struct scene *s, struct gfx *g)
{
    const struct item *l = s->list[s->cur]; struct rect frame = g->clip;
    for (int i = 0; i < s->n[s->cur]; i++) {
        const struct item *it = &l[i];
        g->clip = rect_and(frame, it->clip);
        if (rect_empty(rect_and(it->box, g->clip))) continue;
        switch (it->kind) {
        case K_FILL: gfx_fill(g, it->box, it->rgb); break;
        case K_RRECT: gfx_rrect(g, it->f[0], it->f[1], it->f[2], it->f[3], it->f[4], it->rgb, it->alpha); break;
        case K_STROKE: gfx_rrect_stroke(g, it->f[0], it->f[1], it->f[2], it->f[3], it->f[4], it->f[5], it->rgb, it->alpha); break;
        case K_ARC: gfx_arc(g, it->f[0], it->f[1], it->f[2], it->f[3], it->f[4], it->f[5], it->rgb, it->alpha); break;
        case K_POLY: gfx_poly(g, it->f, it->n, it->rgb, it->alpha); break;
        case K_GRAD: {
            struct rect r = { it->box.x0, (int)it->f[0], it->box.x1, (int)it->f[1] };
            g->clip = rect_and(g->clip, it->box);
            gfx_vgrad(g, r, it->rgb, it->rgb2); } break;
        case K_TEXT: { const struct ui_text_e *e = it->ref;
            gfx_mask(g, (int)it->f[0], (int)it->f[1], e->t.m, e->t.w, e->t.h, e->t.stride, it->rgb, it->alpha); } break;
        case K_ICON: { const struct ui_icon *ic = it->ref;
            gfx_icon_scaled(g, (int)it->f[0], (int)it->f[1], (int)it->f[2], (int)it->f[3], ic->px, ic->w, ic->h, ic->w); } break;
        case K_IMG: { const struct ui_image *im = it->ref;
            gfx_image_scaled(g, (int)it->f[0], (int)it->f[1], (int)it->f[2], (int)it->f[3], im->px, im->w, im->h, im->w, it->f[4]); } break;
        }
    }
    g->clip = frame;
}

/* what differs from the last frame: the boxes of every item added, removed or changed, as one rectangle */
static struct rect diff(struct scene *s)
{
    const struct item *a = s->list[!s->cur], *b = s->list[s->cur];
    int na = s->n[!s->cur], nb = s->n[s->cur];
    struct rect d = { 0, 0, 0, 0 };
    static unsigned char seen[MAXITEMS];
    for (int i = 0; i < MAXID; i++) s->idx[i] = -1;
    for (int i = 0; i < na; i++) { s->idx[a[i].id] = (int16_t)i; seen[i] = 0; }
    for (int i = 0; i < nb; i++) {
        int j = s->idx[b[i].id];
        if (j >= 0 && !seen[j]) {                            /* an id drawn twice in a frame: its second goes by box */
            seen[j] = 1;
            if (!memcmp(&a[j], &b[i], sizeof a[j])) continue;
            d = rect_or(d, a[j].box);
        }
        d = rect_or(d, b[i].box);
    }
    for (int j = 0; j < na; j++) if (!seen[j]) d = rect_or(d, a[j].box);
    return d;
}

/* ---------------------------------------------------------------- images */

const struct ui_image *img_get(struct scene *s, const char *key, int px)
{
    if (!key || !key[0] || px <= 0) return NULL;
    struct ui_img *e, *old = NULL;
    for (int i = 0; i < NIMG; i++) {
        e = &s->img[i];
        if (e->state != IMG_FREE && e->px == px && eq(e->key, key)) {
            e->used = s->now + 1;
            if (e->state == IMG_OK) return &e->im;
            if ((e->state == IMG_ASKED && s->now - e->asked > 20000) || (e->state == IMG_FAILED && s->now - e->asked > 120000)) {
                e->state = IMG_ASKED; e->asked = s->now;            /* lost on the way, or worth another try */
                if (s->p->image) s->p->image(s->ctx, key, px);
            }
            return NULL;
        }
        if (e->used <= s->now && (!old || e->used < old->used)) old = e;   /* none drawn in this frame */
    }
    if (!old) return NULL;
    free(old->im.px); memset(old, 0, sizeof *old);
    snprintf(old->key, sizeof old->key, "%s", key); old->px = px;
    old->state = IMG_ASKED; old->asked = s->now; old->used = s->now + 1;
    if (s->p->image) s->p->image(s->ctx, key, px);
    return NULL;
}

void scene_image(struct scene *s, const char *key, int px, uint32_t *pixels, int w, int h)
{
    for (int i = 0; i < NIMG; i++) {
        struct ui_img *e = &s->img[i];
        if (e->state == IMG_FREE || e->px != px || !eq(e->key, key)) continue;
        free(e->im.px);
        e->im.px = pixels; e->im.w = w; e->im.h = h; e->im.ver++;
        e->state = pixels ? IMG_OK : IMG_FAILED;
        return;
    }
    free(pixels);                                                /* asked for, then pushed out: not wanted now */
}

/* the art's average colour, for the backdrop behind it */
static uint32_t average(const struct ui_image *im)
{
    unsigned long long r = 0, g = 0, b = 0, n = 0;
    for (int y = 0; y < im->h; y += 8)
        for (int x = 0; x < im->w; x += 8) { uint32_t p = im->px[(size_t)y * im->w + x]; r += p & 255; g += p >> 8 & 255; b += p >> 16 & 255; n++; }
    if (!n) return 0;
    return (uint32_t)(r / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(b / n);
}

void scene_art(struct scene *s, uint32_t *px, int w, int h)
{
    free(s->art.px);
    s->art.px = px; s->art.w = px ? w : 0; s->art.h = px ? h : 0; s->art.ver++;
    s->art_avg = px ? average(&s->art) : 0;
}

void scene_backdrop(struct scene *s, uint32_t *px, int w, int h)
{
    free(s->backdrop.px);
    s->backdrop.px = px; s->backdrop.w = px ? w : 0; s->backdrop.h = px ? h : 0; s->backdrop.ver++;
}

/* ---------------------------------------------------------------- home, apps, what is in front */

int is_home(const char *pkg) { return !pkg[0] || eq(pkg, UI_HOME_PKG); }

static char kiosk[96];
static int home_chosen, home_known, home_new;     /* home_new: the page picked another while the drawer was out */

/* the app the Echo comes back to: the one chosen, else Kiosk Satellite where it is installed; "" the face */
const char *home_app(const struct scene *s) { return home_chosen ? s->home : kiosk; }


int scene_drawer(const struct scene *s) { return drawer_open(s); }

/* an app on its way and not in front yet (one launched while it was still in front, our home not reported there yet,
 * is in front: no scene_top tells it) */
static int on_its_way(const struct scene *s) { return s->leaving_until > s->now && !eq(s->leaving_pkg, s->top); }

int scene_waiting(const struct scene *s) { return on_its_way(s) || s->want_home_until > s->now; }

void go_home(struct scene *s, int view)
{
    if (!is_home(s->top)) {                                     /* our home asked for: shown meanwhile (getTasks */
        s->want_home_until = s->now + 3000;                     /* says the app a while longer); in front already, no */
        if (s->p->launch) s->p->launch(s->ctx, NULL, NULL);    /* grace, and an app that comes takes the screen (first: */
    }                                                           /* a launch that fails at once ends it) */
    s->leaving_until = 0;
    s->view = view;
}

void launch(struct scene *s, const char *pkg, const char *cls)
{
    s->leaving_until = s->now + 4000; s->leaving_drawn = 0; s->want_home_until = 0;   /* until the app is in front */
    snprintf(s->leaving_pkg, sizeof s->leaving_pkg, "%s", pkg ? pkg : UI_HOME_PKG);
    if (s->p->launch) s->p->launch(s->ctx, pkg, cls);           /* (last: a failure it tells, scene_launch_failed, stands) */
}

int home_pending(void) { return home_new; }
void ask_own(struct scene *s) { s->own_asked = 1; home_new = 0; }

/* the platform could not start it (no launcher activity: uninstalled, say; the system starting again, not up yet):
 * nothing on its way; the home app that does not start is not tried again on every return of our home, until the
 * choice changes, but now and then while our own screen is what shows (scene_prepare) */
void scene_launch_failed(struct scene *s, const char *pkg, long long now)
{
    s->now = now;
    if (eq(s->leaving_pkg, pkg ? pkg : UI_HOME_PKG)) s->leaving_until = 0;
    if (!pkg && s->want_home_until) {                           /* our own did not start: the app keeps the screen, */
        s->want_home_until = 0;                                 /* and a drawer pulled over it goes */
        if (s->dr.from_app && drawer_open(s)) { drawer_close_now(s); s->edge = 0; s->pressed = 0; scene_backdrop(s, NULL, 0, 0); }
    }
    if (pkg && eq(pkg, home_app(s))) {
        s->home_broken = 1;
        if (!s->home_retry_at) { s->home_retry_ms = s->home_retry_ms ? (s->home_retry_ms >= 300000 ? 600000 : s->home_retry_ms * 2) : 30000; s->home_retry_at = now + s->home_retry_ms; }
    }
}

int scene_visible(const struct scene *s)
{
    return is_home(s->top) || s->now < s->want_home_until;
}

/* The activity in front.  An app gone while one is the home app (it ended, crashed, or Back left it): it comes back,
 * three times a minute at most, unless our own screen was asked for. */
void scene_top(struct scene *s, const char *pkg, long long now)
{
    s->now = now;
    int was_home = is_home(s->top);
    if (eq(pkg, s->top)) return;
    snprintf(s->top, sizeof s->top, "%s", pkg);
    if (!is_home(pkg)) {                                        /* the app is in front: what held its place goes */
        s->leaving_until = 0;
        if (eq(pkg, home_app(s))) s->own_asked = 0;             /* (the home app back: it comes back again after this) */
        if (eq(pkg, home_app(s))) { s->home_broken = 0; s->home_retry_at = 0; s->home_retry_ms = 0; }           /* (it starts after all: an update had it away) */
        /* a drawer still out when an app takes the screen, not one pulled over it that waits for our home: it goes */
        if (drawer_open(s) && (!s->dr.from_app || now >= s->want_home_until)) {   /* (the grace is a pull's over one) */
            int go = s->dr.go; char gp[sizeof s->dr.go_pkg]; snprintf(gp, sizeof gp, "%s", s->dr.go_pkg);
            drawer_close_now(s); s->edge = 0; s->pressed = 0;   /* (that finger: no new pull over the app) */
            if (go && !eq(gp, pkg)) launch(s, gp, NULL);        /* (a Home chosen as it closed: that app all the same) */
        }
        if (!drawer_open(s)) { s->dr.from_app = 0; scene_backdrop(s, NULL, 0, 0); }
        return;
    }
    const char *h = home_app(s);
    if (!was_home && h[0] && !s->home_broken && !s->own_asked && now >= s->want_home_until && now >= s->leaving_until && !drawer_open(s)) {   /* (not over an app on its way) */
        if (now - s->relaunch[2] > 60000) {
            s->relaunch[2] = s->relaunch[1]; s->relaunch[1] = s->relaunch[0]; s->relaunch[0] = now;
            launch(s, h, NULL);
        } else {                                                /* the face meanwhile; again once a minute is past */
            send(s, "log %s keeps leaving: the face stays a while", h);
            if (!s->home_retry_at) s->home_retry_at = s->relaunch[2] + 60001;
        }
    }
    s->want_home_until = 0;                                   /* ours came: an app that comes next takes the screen */
}

static int by_label(const void *a, const void *b) { return strcasecmp(((const struct ui_app *)a)->label, ((const struct ui_app *)b)->label); }

void scene_apps(struct scene *s, struct ui_app *apps, int n)
{
    for (int i = 0; i < s->napps; i++) free(s->apps[i].icon.px);
    free(s->apps);
    if (n > 0) qsort(apps, (size_t)n, sizeof *apps, by_label);
    s->apps = apps; s->napps = n;
    if (!apps) return;
    for (int i = 0; i < n; i++) {                               /* for the settings page, which picks the home app */
        char l[64]; snprintf(l, sizeof l, "%s", apps[i].label);  /* an app's own words: no line end, no tab in them */
        for (char *c = l; *c; c++) if ((unsigned char)*c < 0x20 || *c == 0x7F) *c = ' ';
        if (strpbrk(apps[i].pkg, "\t\n\r ")) continue;
        send(s, "app %s\t%s\t%d", apps[i].pkg, l, apps[i].system);
    }
    send(s, "apps.");
}

void scene_screen(struct scene *s, int on, long long now) { s->now = now; s->screen_on = on; }

/* ---------------------------------------------------------------- touches */

static int act(struct scene *s, int a, int arg, int ev)        /* 1: the hit took it */
{
    if (a >= A_M_TAB) return music_act(s, a, arg, ev);
    if (a >= A_DR_SHADE) return drawer_act(s, a, arg, ev);
    return face_act(s, a, arg, ev);
}

/* what the finger is on is no more (gone from what shows, or our screen no longer in front): that touch does nothing */
static void let_go(struct scene *s)
{
    s->pressed = s->dragging = s->slide = 0; s->longd = 1;
    s->m.seek_drag = s->m.vol_drag = 0; s->m.spk_drag = 0; s->dr.vol_drag = 0;
}

static int blocker(const struct hit *h) { return h->act == A_DR_SHADE || h->act == A_M_SHADE; }

static int hit_at(struct scene *s, int x, int y)
{
    for (int i = s->nhits - 1; i >= 0; i--) {
        struct rect r = s->hits[i].r;
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) return i;
    }
    return -1;
}

/* the scroller under the finger, unless a shade over it (the drawer, a sheet) came later: the scroll areas are listed in
 * the order drawn, as the hits are, and one drawn before the topmost shade is under it */
static struct scroller *scroll_at(struct scene *s, int x, int y)
{
    int shade = -1;
    for (int i = s->nhits - 1; i >= 0; i--) if (blocker(&s->hits[i])) { shade = i; break; }
    for (int i = s->nscrolls - 1; i >= 0; i--) {
        struct rect r = s->scrolls[i].r;
        if (x < r.x0 || x >= r.x1 || y < r.y0 || y >= r.y1) continue;
        return s->scrolls[i].hseq > shade ? s->scrolls[i].sc : NULL;
    }
    return NULL;
}

static void sample(struct scroller *sc, long long t)
{
    memmove(sc->sv + 1, sc->sv, sizeof sc->sv - sizeof sc->sv[0]); memmove(sc->st + 1, sc->st, sizeof sc->st - sizeof sc->st[0]);
    sc->sv[0] = sc->pos; sc->st[0] = t;
}

void scene_touch(struct scene *s, int ev, int x, int y, long long now)
{
    s->now = now;
    if (ev == TOUCH_DOWN && !s->screen_on) { s->down = 0; return; }   /* the screen off: no gesture (android.c wakes it) */
    int vis = scene_visible(s) && s->screen_on;
    if (ev == TOUCH_DOWN) {
        s->down = 1; s->x0 = s->x = x; s->y0 = s->y = y; s->t0 = now; s->fired = s->longd = s->dragging = s->slide = 0;
        s->sc = NULL; s->sc_drag = 0; s->pressed = 0;
        /* the grip above the layer (y < 0: rows no app has, android.c) is the drawer's whatever shows; over our own
         * screen also the top of the layer, where a tap is still a tap on what is drawn.  Over an app a pull from
         * below the grip is the app's (its own pull to refresh), never the drawer's */
        s->edge = y >= s->h ? 2 : ((y < 0 || (vis && y < U(EDGE_TOP))) && !s->dr.t && !s->dr.pulling) ? 1 : 0;   /* (closed, or
                                                                   on its way in: grabbed again where it is) */
        if (s->edge == 2 || !vis || y < 0) return;
        /* a finger on a list that still moves (a fling, the way back into bounds) stops it, and is only that, as on a
         * phone: no tap on the row that was passing under it, no slider grabbed; a drag after it scrolls on */
        struct scroller *sc = scroll_at(s, x, y);
        if (sc && (sc->vel != 0 || sc->moving)) { sc->vel = 0; sc->moving = 0; s->sc = sc; return; }
        int h = hit_at(s, x, y);
        if (h >= 0) { s->pressed = 1; s->pact = s->hits[h].act; s->parg = s->hits[h].arg; s->pkey = s->hits[h].key; }
        s->sc = sc;
        /* a slider: decided on the first move, as Android's SeekBar in a list: along it the slider's, across it the
         * list's or the drawer's (a swipe that starts on a volume changes no volume); let go without one, a tap there */
        if (h >= 0 && (s->hits[h].flags & HIT_DRAG)) s->slide = 1;
        return;
    }
    if (!s->down) return;
    if (!vis && (s->pressed || s->dragging || s->slide)) let_go(s);   /* (an app came up under the finger: none of ours) */
    s->x = x; s->y = y;
    int dx = x - s->x0, dy = y - s->y0, moved = abs(dx) > TAP_PX || abs(dy) > TAP_PX;
    if (ev == TOUCH_MOVE) {
        if (s->slide) {
            if (!moved) return;
            s->slide = 0;
            if (abs(dx) >= abs(dy)) { s->dragging = 1; s->sc = NULL; act(s, s->pact, s->parg, EV_DRAG); return; }
            s->pressed = 0;                                     /* across: as any other touch, below */
        }
        if (s->edge == 1 && !s->dragging && !s->sc_drag) {     /* the top edge: the drawer comes with the finger */
            if (s->dr.pulling) { drawer_pull(s, y, TOUCH_MOVE); return; }
            if (on_its_way(s)) return;                          /* an app on its way: its place is held, no drawer */
            if (dy > fmaxf(U(10), TAP_PX) && dy > abs(dx)) { s->pressed = 0; s->sc = NULL; drawer_pull(s, y, TOUCH_DOWN); return; }   /* (a tap that
                                                                   slips is still a tap) */
        }
        if (s->edge == 2) {                                     /* along the band over an app: Back */
            if (!vis && !s->fired && abs(dx) >= BACK_PX && abs(dy) < 50) { s->fired = 1; if (s->p->back) s->p->back(s->ctx); }
            return;
        }
        if (s->dragging) { act(s, s->pact, s->parg, EV_DRAG); return; }
        /* up on the open drawer, not along its apps: it goes with the finger */
        if (drawer_open(s) && !s->sc_drag && dy < -TAP_PX && abs(dy) > abs(dx) && (!s->sc || s->sc->axis)) {
            s->edge = 1; s->pressed = 0; drawer_pull(s, s->y0, TOUCH_DOWN); drawer_pull(s, y, TOUCH_MOVE);
            return;
        }
        if (s->sc) {
            struct scroller *sc = s->sc; float d = (float)(sc->axis ? dy : dx);
            if (!s->sc_drag && fabsf(d) > TAP_PX && fabsf(d) >= (float)(sc->axis ? abs(dx) : abs(dy))) {
                /* from where the finger would have it: a list stopped past an end goes on from there, not by a jump */
                float raw = sc->pos < 0 ? sc->pos / 0.35f : sc->pos > sc->max ? sc->max + (sc->pos - sc->max) / 0.35f : sc->pos;
                s->sc_drag = 1; s->pressed = 0; sc->held = 1; sc->pos0 = raw + d; memset(sc->st, 0, sizeof sc->st);
            }
            if (s->sc_drag) {
                float p = sc->pos0 - d;                         /* past either end: a third as far as the finger */
                if (p < 0) p *= 0.35f;
                else if (p > sc->max) p = sc->max + (p - sc->max) * 0.35f;
                sc->pos = p; sample(sc, now);
                return;
            }
        }
        if (moved) s->pressed = 0;
        return;
    }
    s->down = 0;                                                 /* TOUCH_UP */
    if (s->edge == 1 && s->dr.pulling) { drawer_pull(s, y, TOUCH_UP); return; }
    if (s->edge == 2) return;
    if (s->slide) { s->slide = 0; s->pressed = 0; act(s, s->pact, s->parg, EV_DRAG); act(s, s->pact, s->parg, EV_DROP); return; }   /* tapped */
    if (s->dragging) { act(s, s->pact, s->parg, EV_DROP); s->pressed = 0; s->dragging = 0; return; }
    if (s->sc_drag) {
        struct scroller *sc = s->sc; float v = 0;
        for (int i = 3; i >= 1; i--)                            /* over the last 120 ms of the move at most */
            if (sc->st[i] && sc->st[0] > sc->st[i] && now - sc->st[i] <= 120) { v = (sc->sv[0] - sc->sv[i]) / (float)(sc->st[0] - sc->st[i]); break; }
        if (sc->st[0] && now - sc->st[0] > 80) v = 0;           /* held still before letting go */
        sc->vel = clampf(v, -6, 6); sc->held = 0; sc->moving = 1; s->sc_drag = 0;
        return;
    }
    int p = s->pressed; s->pressed = 0;
    int here = 0;                                                /* (where it lifts or where it landed: a slip off an edge) */
    for (int k = 0; k < 2 && !here; k++) {
        int h = k ? hit_at(s, s->x0, s->y0) : hit_at(s, x, y);
        here = h >= 0 && s->hits[h].act == s->pact && s->hits[h].arg == s->parg && s->hits[h].key == s->pkey;
    }
    if (vis && !s->fired && s->longd != 1 && !moved && p && here) act(s, s->pact, s->parg, EV_TAP);   /* (held long, nothing took
                                                                   it: a tap; on what is under the finger now, not one that moved away) */
}

/* flings and the way back into bounds; 2 while any still moves (frames at DRAG_MS) */
static int scrollers(struct scene *s, float dt)
{
    int moving = 0;
    for (int i = 0; i < s->nscrolls; i++) {
        struct scroller *sc = s->scrolls[i].sc; int dup = 0, m = 0;
        for (int j = 0; j < i; j++) if (s->scrolls[j].sc == sc) dup = 1;
        if (dup) continue;
        if (sc->held) { moving |= s->sc_drag ? 2 : 0; continue; }
        if (s->down && sc == s->sc) continue;                   /* a finger resting on it (a fling or the way back stopped
                                                                   under it): where it is, on once it lifts */
        if (sc->vel != 0) {
            sc->pos += sc->vel * dt;
            float k = expf(-dt / FLING_TAU);
            if (sc->pos < 0 || sc->pos > sc->max) k *= expf(-dt / 40);   /* past an end it stops at once */
            sc->vel *= k;
            if (fabsf(sc->vel) < 0.02f) sc->vel = 0;
            m = 1;
        }
        float want = clampf(sc->pos, 0, sc->max);
        if (sc->pos != want && fabsf(sc->vel) < 0.3f) {        /* back within bounds, as a spring would */
            sc->pos += (want - sc->pos) * (1 - expf(-dt / 70));
            if (fabsf(sc->pos - want) < 0.5f) sc->pos = want;
            m = 1;
        }
        sc->moving = m;
        if (m) moving = 2;
    }
    return moving;
}

/* ---------------------------------------------------------------- frames */

static long long until_minute(void)
{
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    return 60000 - ((long long)t.tv_sec % 60) * 1000 - t.tv_nsec / 1000000 + 5;
}

static void home_apply(struct scene *s);

struct rect scene_prepare(struct scene *s, long long now, int *next_ms)
{
    float dt = s->last ? clampf((float)(now - s->last), 1, 100) : 16;
    s->last = now; s->now = now;
    long long wake = 0;
    #define SOON(t) do { long long t_ = (t); if (t_ > now && (!wake || t_ < wake)) wake = t_; } while (0)
    if (s->down && !s->fired && !s->longd && !s->dragging && !s->slide && !s->sc_drag && s->edge != 2 && !s->dr.pulling &&
        scene_visible(s) && s->screen_on) {                    /* a long press is held (on what shows: not under an app) */
        if (now - s->t0 >= LONG_MS && abs(s->x - s->x0) <= TAP_PX && abs(s->y - s->y0) <= TAP_PX) {
            s->longd = 2;                                       /* held long: 1 once what it is on took it (a sheet came), */
            if (s->pressed && act(s, s->pact, s->parg, EV_LONG)) { s->longd = 1; s->sc = NULL; s->sc_drag = 0; }   /* and that */
                                                                /* finger then scrolls nothing; else still a tap or a scroll */
        } else SOON(s->t0 + LONG_MS);
    }
    if (s->want_home_until) SOON(s->want_home_until);
    if (s->leaving_until) SOON(s->leaving_until);
    if (s->home_retry_at && now >= s->home_retry_at) {         /* the home app that did not start, or kept leaving: again */
        const char *h = home_app(s); s->home_retry_at = 0;
        if (h[0] && is_home(s->top) && !s->own_asked) {
            if (!drawer_open(s) && !on_its_way(s) && now >= s->want_home_until) {
                s->relaunch[2] = s->relaunch[1]; s->relaunch[1] = s->relaunch[0]; s->relaunch[0] = now;
                launch(s, h, NULL);
            } else s->home_retry_at = now + 1000;               /* (not now: the drawer out, an app on its way: soon) */
        }
        if (s->home_broken && !s->home_retry_at) {             /* (not set again by a launch that failed at once) */
            s->home_retry_ms = s->home_retry_ms >= 300000 ? 600000 : s->home_retry_ms ? s->home_retry_ms * 2 : 30000; s->home_retry_at = now + s->home_retry_ms;
        }
    }
    if (s->home_retry_at) SOON(s->home_retry_at);
    long long due = face_due(s); if (due) SOON(due);
    if (s->pend_until && now >= s->pend_until) {                /* a tap Sendspin never agreed with: what it said */
        s->pend_until = 0;
        s->mstate = s->srv_state; s->mpos = s->srv_pos + (s->srv_state == 1 ? (now - s->srv_at) * s->srv_speed / 1000 : 0);
        s->mpos_at = now; s->mspeed = s->srv_speed;
    } else if (s->pend_until) SOON(s->pend_until);
    int moving = drawer_step(s, dt);                            /* first: closed now, it may launch the app it held */
    /* out over an app that kept the screen (our home activity never came, or the app took it back): the drawer goes,
     * or the grip would stay its top and pull nothing */
    if (drawer_open(s) && !scene_visible(s)) {                  /* (a Home chosen as it closed: that app all the same) */
        int go = s->dr.go; char pkg[sizeof s->dr.go_pkg]; snprintf(pkg, sizeof pkg, "%s", s->dr.go_pkg);
        drawer_close_now(s); s->edge = 0; s->pressed = 0;       /* (that finger: no new pull) */
        if (go) launch(s, pkg, NULL);
    }
    if (home_new) home_apply(s);
    /* the app's picture: while the drawer is out over it, and while the app launched from it is on its way; never
     * again under a later one */
    if (s->backdrop.px && !(s->dr.from_app && drawer_open(s)) && s->leaving_until <= now) scene_backdrop(s, NULL, 0, 0);
    if (s->leaving_until > now && s->leaving_drawn) {           /* that app on its way: this frame stays as it is */
        *next_ms = (int)(s->leaving_until - now);
        return (struct rect){ 0, 0, 0, 0 };
    }
    if (!s->screen_on || !scene_visible(s)) {                   /* nothing to draw: the last frame stays as it was, its */
        for (int i = 0; i < s->nscrolls; i++) {                 /* lists where they are (no fling on when it comes back) */
            struct scroller *sc = s->scrolls[i].sc; sc->vel = 0; sc->moving = 0; sc->held = 0; sc->pos = clampf(sc->pos, 0, sc->max);
        }
        s->sc = NULL; s->sc_drag = 0;
        *next_ms = wake ? (int)(wake - now) : -1;
        return (struct rect){ 0, 0, 0, 0 };
    }
    if (s->leaving_until > now) s->leaving_drawn = 1;           /* (one more first, drawn: the drawer gone from it) */
    moving |= scrollers(s, dt);                                 /* last frame's areas: their flings move on */
    s->nprev = s->nscrolls; for (int i = 0; i < s->nscrolls; i++) s->prev[i] = s->scrolls[i].sc;
    s->cur = !s->cur; s->n[s->cur] = 0; s->nhits = 0; s->nscrolls = 0;
    s->clip = (struct rect){ 0, 0, s->w, s->h };
    int drawer_only = s->backdrop.px && ((s->dr.from_app && drawer_open(s)) || s->leaving_until > now);
    if (drawer_only) {                                          /* pulled over an app: its picture under the drawer */
        fill(s, 0, (struct rect){ 0, 0, s->w, s->h }, BG);
        if (s->backdrop.px) image(s, 1, &s->backdrop, 0, 0, (float)s->backdrop.w, (float)s->backdrop.h, 0);
    } else if (s->view == V_MUSIC) {
        long long mdue = 0;
        music_view(s, dt, &moving, &mdue);
        if (mdue) SOON(mdue);
    } else {
        moving |= face_view(s, dt);
        SOON(now + until_minute());
    }
    drawer_draw(s);
    if (drawer_open(s)) SOON(now + until_minute());             /* its clock */
    if (s->down && (s->pressed || s->dragging || s->slide)) {   /* what the finger is on: gone from what shows now (a
                                                                   request brought the face): that touch does nothing */
        int there = 0;
        for (int i = 0; i < s->nhits && !there; i++) there = s->hits[i].act == s->pact && s->hits[i].arg == s->parg && s->hits[i].key == s->pkey;
        if (!there) { let_go(s); moving |= 1; }                 /* (a frame again at once: not its highlight on the row
                                                                   that came under the finger) */
    }
    for (int i = 0; i < s->nprev; i++) {                        /* a list left (another tab, view): still where it was */
        int here = 0;
        for (int k = 0; k < s->nscrolls && !here; k++) here = s->scrolls[k].sc == s->prev[i];
        if (s->prev[i] && !here) {
            struct scroller *sc = s->prev[i]; sc->vel = 0; sc->moving = 0; sc->held = 0; sc->pos = clampf(sc->pos, 0, sc->max);
            if (s->sc == sc) { s->sc = NULL; s->sc_drag = 0; }       /* (a finger on it as it went: no fling after) */
        }
    }
    for (int i = 0; i < s->nscrolls; i++) {                     /* a list shorter now than where it was: back into it */
        struct scroller *sc = s->scrolls[i].sc;
        if (!sc->held && (sc->pos < 0 || sc->pos > sc->max)) moving |= 1;
    }
    struct rect d = s->full ? (struct rect){ 0, 0, s->w, s->h } : diff(s);
    s->full = 0;
    int fast = s->down || moving > 1;
    *next_ms = moving ? (fast || s->sc_drag ? DRAG_MS : FRAME_MS) : wake ? (int)(wake - now) : -1;
    for (int i = 0; i < s->nscrolls; i++) if (s->scrolls[i].sc->vel != 0) *next_ms = DRAG_MS;
    if (*next_ms == 0) *next_ms = 1;
    return rect_and(d, (struct rect){ 0, 0, s->w, s->h });
    #undef SOON
}

void scene_invalidate(struct scene *s) { s->full = 1; }

/* ---------------------------------------------------------------- messages */

static void copy_field(char *out, size_t n, const char **p)       /* up to the next tab */
{
    size_t k = 0;
    while (**p && **p != '\t') { if (k + 1 < n) out[k++] = **p; (*p)++; }
    out[k] = 0;
    if (**p == '\t') (*p)++;
}

/* the page's new choice, now: its app in front, or the face (once the drawer is in) */
static void home_apply(struct scene *s)
{
    if (drawer_open(s)) return;
    const char *h = home_app(s);
    home_new = 0; s->own_asked = 0;                             /* (a new choice: its app, and back when it leaves) */
    if (h[0] && !eq(s->top, h)) launch(s, h, NULL);
    else if (!h[0] && !is_home(s->top)) go_home(s, V_FACE);
}

static void start_home(struct scene *s)
{
    const char *h = home_app(s);
    if (h[0] && is_home(s->top) && !drawer_open(s) && s->view == V_FACE) {   /* at start: the home app first */
        s->relaunch[0] = s->now; launch(s, h, NULL);
    }
}

void scene_msg(struct scene *s, const char *line, long long now)
{
    char cmd[16] = ""; const char *a = line; size_t k = 0;
    s->now = now;
    while (*a && *a != ' ' && k + 1 < sizeof cmd) cmd[k++] = *a++;
    cmd[k] = 0;
    if (*a == ' ') a++;
    if (eq(cmd, "p+") || eq(cmd, "p-")) {
        char name[48] = ""; long long ago = 0;
        sscanf(a, "%47s %lld", name, &ago);
        int p = pat_find(name);
        if (p < 0) return;
        face_pattern(s, p, cmd[1] == '+', ago < 0 ? 0 : ago);
    }
    else if (eq(cmd, "ev")) s->evol = atoi(a);
    else if (eq(cmd, "lv")) s->level = atoi(a) < 0 ? 0 : atoi(a) > 100 ? 100 : atoi(a);
    else if (eq(cmd, "mute")) s->mute = atoi(a);
    else if (eq(cmd, "dnd")) s->dnd = atoi(a);
    else if (eq(cmd, "oobe")) s->oobe = atoi(a);
    else if (eq(cmd, "h12")) s->h12 = atoi(a);
    else if (eq(cmd, "name")) snprintf(s->name, sizeof s->name, "%s", a);
    else if (eq(cmd, "act")) snprintf(s->act, sizeof s->act, "%s", a);
    else if (eq(cmd, "web")) snprintf(s->web, sizeof s->web, "%s", a);
    else if (eq(cmd, "kiosk")) {                               /* installed (or gone) while the UI runs: the home app now */
        char was[96]; snprintf(was, sizeof was, "%s", home_app(s));
        snprintf(kiosk, sizeof kiosk, "%s", a);
        if (!eq(was, home_app(s))) { s->home_broken = 0; s->home_retry_at = 0; s->home_retry_ms = 0; }
        if (home_known && !home_chosen && !eq(was, home_app(s))) { home_new = 1; home_apply(s); }
    }
    else if (eq(cmd, "timers")) snprintf(s->timers, sizeof s->timers, "%s", a);
    else if (eq(cmd, "peer")) snprintf(s->peer, sizeof s->peer, "%s", a);
    else if (eq(cmd, "tz")) { if (a[0]) setenv("TZ", a, 1); else unsetenv("TZ"); tzset(); }
    else if (eq(cmd, "ping")) send(s, "pong %.20s", a);             /* hassmic: this UI's main loop goes on */
    else if (eq(cmd, "mt")) {
        copy_field(s->title, sizeof s->title, &a); copy_field(s->artist, sizeof s->artist, &a); copy_field(s->album, sizeof s->album, &a);
        s->music = 1;
    } else if (eq(cmd, "mp")) {
        long long pos = 0, dur = 0; int st = 0, sp = 1000;
        if (sscanf(a, "%d %lld %lld %d", &st, &pos, &dur, &sp) >= 1) {
            s->srv_state = st; s->srv_pos = pos; s->srv_dur = dur; s->srv_speed = sp; s->srv_at = now; s->music = 1;
            if (s->pend_until && st != s->pend_state) s->mdur = dur;     /* older than the tap: the state stays as tapped */
            else { s->mstate = st; s->mpos = pos; s->mdur = dur; s->mspeed = sp; s->mpos_at = now; }   /* (the tap still
                                                                   pending: an older word that agrees, then its own answer) */
        }
    } else if (eq(cmd, "mc")) {
        int n = 0; char cmds[256] = "", rep[8] = ""; int sh = -1;
        s->mcmds[0] = 0;
        if (sscanf(a, "%d %d %n", &s->mvol, &s->mmuted, &n) >= 2) {
            sscanf(a + n, "%255s %7s %d", cmds, rep, &sh);
            snprintf(s->mcmds, sizeof s->mcmds, "%s", cmds);
            s->mrepeat = eq(rep, "all") ? 2 : eq(rep, "one") ? 1 : 0;
            if (sh >= 0) s->mshuffle = sh;
        }
        s->music = 1;
    } else if (eq(cmd, "mx")) {
        s->music = 0; s->title[0] = s->artist[0] = s->album[0] = s->mcmds[0] = 0; s->mstate = 0; s->mdur = 0; s->pend_until = 0;
    } else if (eq(cmd, "home")) {                              /* at start, and whenever the page picks another */
        char was[96]; snprintf(was, sizeof was, "%s", home_app(s));
        home_chosen = !eq(a, "-");
        snprintf(s->home, sizeof s->home, "%s", home_chosen ? a : "");
        const char *h = home_app(s);
        if (!eq(was, h)) { s->home_broken = 0; s->home_retry_at = 0; s->home_retry_ms = 0; }
        if (!home_known) { home_known = 1; start_home(s); }
        else if (!eq(was, h)) { home_new = 1; home_apply(s); }  /* picked on the page: it applies now */
        else if (s->home_broken) { s->home_broken = 0; s->home_retry_at = 0; s->home_retry_ms = 0; home_new = 1; home_apply(s); }   /* the same again: tried again */
    } else music_msg(s, cmd, a);
}

void scene_resize(struct scene *s, int w, int h, int band)
{
    s->w = w; s->h = h; s->band = band; s->full = 1;
    s->u = fminf(h / 470.0f, w / 960.0f);
    if (s->u <= 0) s->u = 1;
}

struct scene *scene_new(int w, int h, int band, const struct ui_platform *p, void *ctx, long long now)
{
    struct scene *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->p = p; s->ctx = ctx; kiosk[0] = 0; home_chosen = home_known = home_new = 0;
    scene_resize(s, w, h, band);
    s->rnd = 0x9E3779B9u ^ (unsigned)now; if (!s->rnd) s->rnd = 1;
    s->screen_on = 1; s->pressed = 0; s->view = V_FACE; s->evol = -1;
    for (int i = 0; i < 3; i++) s->relaunch[i] = -3600000;     /* none yet (the clock may be under a minute) */
    s->now = s->last = 0;
    face_init(s, now);
    music_init(s);
    s->m.req_next = (int)((unsigned long long)now % 1000000) * 100;   /* not 1 again after a restart of the UI */
    return s;
}

void scene_free(struct scene *s)
{
    if (!s) return;
    for (int i = 0; i < NTEXT; i++) free(s->text[i].t.m);
    for (int i = 0; i < NIMG; i++) free(s->img[i].im.px);
    scene_apps(s, NULL, 0);
    music_free(s);
    free(s->art.px); free(s->backdrop.px); free(s);
}
