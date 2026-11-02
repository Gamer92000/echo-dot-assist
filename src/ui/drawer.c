/* See ui.h: the drawer, pulled down from the top edge as on Amazon's own Echo Show, over our own screen or any app.
 *
 * Over an app the layer is hidden and the touches are the app's, except in the grip: rows along the top that no app
 * has (android.c), where a pull reaches no app and is seen beside Android (scene.c reads the touch screen).  A picture
 * of the app is taken for under the drawer (SurfaceControl.screenshot), and hassmic's home activity is brought up, so
 * that the next touches land on it and come to us (a tap before it is in front reaches the app as well: the layer is
 * no window; it takes the window manager a frame or two).  Closed without a choice, the drawer brings that app back; closed
 * by a choice of app, it goes away first and then launches it.  Either way the picture stays until the app is in
 * front (scene.c leaving_until): no face, no black home activity in between.
 *
 *   top row   Home (the home app: Kiosk Satellite, or what the settings page picked), Face (where the home app is
 *             another), Music, Do not disturb, Screen off; the clock, or what plays
 *   then      the Echo's volume
 * No apps: the home app and our own pages are all a Show needs, and Android's own (Music, two Downloads, Settings, a
 * keyboard's settings) only confuse.
 */
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

#define PANEL  220                       /* the drawer's height, in units */

int drawer_open(const struct scene *s) { return s->dr.pos > 0.001f || s->dr.t > 0 || s->dr.pulling; }

/* closed: the app chosen, or the one it was pulled over, launched now; the picture of the latter stays under our
 * layer until that app is in front (scene_top lets go of it) */
static void done_closing(struct scene *s)
{
    struct drawer *d = &s->dr;
    if (d->go) launch(s, d->go_pkg, NULL);
    /* back to it, unless the page picked another home app meanwhile: that one (or the face) comes instead */
    else if (d->from_app && d->from[0] && !is_home(d->from) && !home_pending()) launch(s, d->from, NULL);
    else scene_backdrop(s, NULL, 0, 0);
    d->go = 0; d->from_app = 0; d->from[0] = 0;
}

void drawer_close(struct scene *s, int back_to_app)
{
    s->dr.t = 0; s->dr.pulling = 0; s->dr.go = 0;
    if (!back_to_app) { s->dr.from_app = 0; s->dr.from[0] = 0; }
}

void drawer_close_now(struct scene *s)
{
    struct drawer *d = &s->dr;
    d->pos = d->t = 0; d->pulling = 0; d->go = 0; d->from_app = 0; d->from[0] = 0;
}

/* the home app: the drawer goes, then the app comes (the one it was pulled over: back to it) */
static void drawer_go(struct scene *s, const char *pkg)
{
    struct drawer *d = &s->dr;
    drawer_close(s, 1);
    if (d->from_app && eq(pkg, d->from)) return;
    d->go = 1; snprintf(d->go_pkg, sizeof d->go_pkg, "%s", pkg);
    s->view = V_FACE;                                           /* what shows when that app is gone again */
    s->own_asked = 0;                                           /* (the home app asked for: back when it leaves, tried again) */
    scene_backdrop(s, NULL, 0, 0);                              /* not the other app's picture while this one comes: ours */
}

void drawer_pull(struct scene *s, int y, int ev)
{
    struct drawer *d = &s->dr; float H = U(PANEL);
    if (ev == TOUCH_DOWN) {
        if (!drawer_open(s)) {
            if (!scene_visible(s)) {                           /* over an app: its picture, and our home activity up */
                d->from_app = 1; snprintf(d->from, sizeof d->from, "%s", s->top);
                if (s->p->screenshot) s->p->screenshot(s->ctx);
                go_home(s, s->view);
            } else d->from_app = 0;
        }
        d->go = 0;                                              /* (grabbed again as it closed for a Home: that Home no more) */
        d->pulling = 1; d->grab = y - d->pos * H; d->vy = 0; d->ty = s->now;
        return;
    }
    float pos = clampf((y - d->grab) / H, 0, 1);
    if (ev == TOUCH_MOVE) {
        long long dt = s->now - d->ty;
        if (dt > 0) { float v = (pos - d->pos) * H / (float)dt; d->vy = d->vy * 0.5f + v * 0.5f; }
        d->ty = s->now; d->pos = d->t = pos;
        return;
    }
    d->pulling = 0;                                             /* TOUCH_UP: open or shut, by the flick or by how far */
    if (s->now - d->ty > 120) d->vy = 0;
    d->t = d->vy > 0.4f ? 1 : d->vy < -0.4f ? 0 : d->pos > 0.4f ? 1 : 0;
}

/* the home app's package (labelled Home, as on Amazon's own: what it is, the page says) */
static const char *home_label(struct scene *s, const char **pkg)
{
    *pkg = home_app(s);
    return "Home";
}

static void quick(struct scene *s, int id, float cx, float y, int ic, const char *label, int on, int act)
{
    float r = U(34);
    int p = pressed(s, act, 0);
    uint32_t bg = on ? ACCENT : p ? 0x3A3F47 : SURF2;
    rrect(s, id, cx - r, y, cx + r, y + 2 * r, r, bg, 255);
    icon(s, id + 1, ic, cx, y + r, U(15), on ? 0x0B0C0F : FG, bg);
    text(s, id + 9, label, U(16), 0, U(100), cx, y + 2 * r + U(8), 1, on ? FG : DIM, 255);
    hit(s, cx - U(48), y - U(4), cx + U(48), y + 2 * r + U(30), act, 0);
}

/* on towards where it goes; before the frame is built (scene.c): closed, it launches what was chosen */
int drawer_step(struct scene *s, float dt)
{
    struct drawer *d = &s->dr;
    if (d->pulling) return 2;
    float before = d->pos;
    d->pos += (d->t - d->pos) * (1 - expf(-dt / 55));
    if (fabsf(d->pos - d->t) < 0.003f) d->pos = d->t;
    if (d->pos == 0 && d->t == 0 && (d->from_app || d->go)) done_closing(s);
    return d->pos != before ? 2 : 0;
}

void drawer_draw(struct scene *s)
{
    struct drawer *d = &s->dr;
    if (d->pos <= 0.001f && !d->pulling) return;
    float H = U(PANEL), top = (d->pos - 1) * H, w = (float)s->w;
    rrect(s, ID_DRAWER, 0, 0, w, (float)s->h, 0, 0x000000, (int)(175 * clampf(d->pos * 1.4f, 0, 1)));
    hit(s, 0, 0, 0, 0, A_DR_SHADE, 0);
    rrect(s, ID_DRAWER + 1, 0, top - U(40), w, top + H, U(26), SURF, 255);
    hit(s, 0, top, w, top + H, A_NONE, 0);                      /* the panel itself: taps there do nothing */

    /* the top row: where to go, what the Echo does; then the clock or the music */
    const char *hpkg; const char *hl = home_label(s, &hpkg);
    float qy = top + U(26), qx = U(66), step = U(106);
    int k = 0;
    quick(s, ID_DRAWER + 10, qx + step * k++, qy, IC_HOME, hl, 0, A_DR_HOME);
    if (hpkg[0]) quick(s, ID_DRAWER + 20, qx + step * k++, qy, IC_FACE, "Face", 0, A_DR_FACE);
    quick(s, ID_DRAWER + 30, qx + step * k++, qy, IC_NOTE, "Music", 0, A_DR_MUSIC);
    quick(s, ID_DRAWER + 40, qx + step * k++, qy, IC_MOON, "Do not disturb", s->dnd, A_DR_DND);
    quick(s, ID_DRAWER + 50, qx + step * k++, qy, IC_POWER, "Screen off", 0, A_DR_SLEEP);
    float cx0 = qx + step * (k - 0.5f) + U(14), cx1 = w - U(28);
    if (s->music && (s->title[0] || s->mstate)) {
        float ch = U(96);
        rrect(s, ID_DRAWER + 60, cx0, qy - U(6), cx1, qy - U(6) + ch, U(18), pressed(s, A_DR_NOW, 0) ? 0x2A2E35 : SURF2, 255);
        float a = ch - U(20);
        if (!image(s, ID_DRAWER + 61, &s->art, cx0 + U(10), qy + U(4), a, a, U(10))) {
            rrect(s, ID_DRAWER + 61, cx0 + U(10), qy + U(4), cx0 + U(10) + a, qy + U(4) + a, U(10), DARK, 255);
            icon(s, ID_DRAWER + 62, IC_NOTE, cx0 + U(10) + a / 2, qy + U(4) + a / 2, U(18), DIM, DARK);
        }
        float tx = cx0 + U(22) + a, tw = cx1 - tx - U(70);
        text(s, ID_DRAWER + 65, s->title[0] ? s->title : "Music Assistant", U(19), 1, tw, tx, qy + U(16), 0, FG, 255);
        text(s, ID_DRAWER + 66, s->artist, U(16), 0, tw, tx, qy + U(44), 0, DIM, 255);
        hit(s, cx0, qy - U(6), cx1 - U(66), qy - U(6) + ch, A_DR_NOW, 0);
        if (can(s, s->mstate == 1 ? "pause" : "play"))
            round_button(s, ID_DRAWER + 67, cx1 - U(38), qy - U(6) + ch / 2, U(24), s->mstate == 1 ? IC_PAUSE : IC_PLAY, FG, SURF, A_DR_PLAY, 0);
    } else {
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); char clk[16], date[64];
        if (s->h12) snprintf(clk, sizeof clk, "%d:%02d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min);
        else snprintf(clk, sizeof clk, "%02d:%02d", tm.tm_hour, tm.tm_min);
        strftime(date, sizeof date, "%A, %e %B", &tm);
        text(s, ID_DRAWER + 65, clk, U(44), 0, cx1 - cx0, cx1, qy - U(4), 2, FG, 255);
        text(s, ID_DRAWER + 66, date, U(18), 0, cx1 - cx0, cx1, qy + U(52), 2, DIM, 255);
    }

    /* the Echo's own volume */
    float vy = top + U(168), vx0 = U(80), vx1 = w - U(48);
    icon(s, ID_DRAWER + 70, IC_SPEAKER, U(46), vy, U(14), DIM, SURF);
    float v = d->vol_drag ? d->vol_frac : s->evol >= 0 ? s->evol / 100.0f : 0.5f;
    slider(s, ID_DRAWER + 75, vx0, vx1, vy, v, ACCENT, A_DR_VOL, 0);

    rrect(s, ID_DRAWER + 90, w / 2 - U(26), top + H - U(14), w / 2 + U(26), top + H - U(9), U(3), 0x4A505A, 255);
}

int drawer_act(struct scene *s, int a, int arg, int ev)
{
    struct drawer *d = &s->dr; (void)arg;
    if (a == A_DR_VOL) {                                        /* a slider: the finger's moves */
        if (ev != EV_DRAG && ev != EV_DROP) return 0;
        d->vol_drag = ev == EV_DRAG; d->vol_frac = slider_value(s, U(80), s->w - U(48));
        int v = (int)lroundf(d->vol_frac * 100);
        if (ev == EV_DROP || s->now - d->vol_sent_at >= 150) { send(s, "vol %d", v); d->vol_sent_at = s->now; s->evol = v; }
        return 1;
    }
    if (ev != EV_TAP) return 0;
    switch (a) {
    case A_DR_SHADE: if (d->t > 0) drawer_close(s, 1); break;   /* (closing already, maybe for a Home chosen: as it is) */
    case A_DR_HOME: {
        const char *pkg; home_label(s, &pkg);
        if (pkg[0]) drawer_go(s, pkg);
        else { drawer_close(s, 0); go_home(s, V_FACE); }
    } break;
    case A_DR_FACE: drawer_close(s, 0); go_home(s, V_FACE); ask_own(s); break;
    case A_DR_MUSIC: case A_DR_NOW: drawer_close(s, 0); music_show(s, a == A_DR_NOW ? 0 : -1); break;
    case A_DR_DND: s->dnd = !s->dnd; send(s, "dnd %d", s->dnd); break;
    case A_DR_SLEEP: drawer_close(s, 1); if (s->p->sleep) s->p->sleep(s->ctx); break;
    case A_DR_PLAY: music_toggle(s); break;
    default: return 0;
    }
    return 1;
}
