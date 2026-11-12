/*
 * A card on the words layer (words.c; hassmic's cards.c says what one is): what Home Assistant pushed, over whatever the
 * screen shows, in place of the request's words (a ringing timer comes first).  Four kinds:
 *
 *   entities  a panel per row (up to 8: one column to 4, two beyond), its name, its state as words, and its control on
 *             the right: a switch (t), a switch with the brightness as a bar under the name (s: a tap on the bar sets
 *             it), open/stop/close (c), the setpoint down/up (h), play/pause (p), Run (b); r rows only show.  The focus
 *             row (what the request was about) stands out with the band's listening colour along its left
 *   weather   the condition as a drawing and the temperature large, a line under it, then a column a day
 *   list      a panel per row (up to 12: one column to 6, two beyond), its text and a detail under it (a calendar's
 *             events, a shopping list); no controls
 *   message   the text as large as it fits; with a picture (a doorbell's snapshot) the picture as large as it fits on the
 *             left at its own shape, the text beside it
 *
 * The title on top, the subtitle (Home Assistant's, else the reply as said) under it.  Icons are drawn from shapes as the
 * rest of the UI's are (widgets.c): no image files.
 *
 * Taps (words_tap while the card shows): on a control, "card tap <seq> <row> <op> [<arg>]" to hassmic, which runs the
 * Home Assistant action and tells the result; a button along the bottom (any kind's), "card tap <seq> <i> press";
 * anywhere else the card goes ("card clear <seq>"), as tapped words do.
 */
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_TOGGLE = 1, OP_LEVEL, OP_OPEN, OP_STOP, OP_CLOSE, OP_DOWN, OP_UP, OP_PLAY, OP_RUN, OP_PRESS };
static const char *const op_names[] = { "", "toggle", "level", "open", "stop", "close", "down", "up", "play", "run", "press" };
#define PANEL   0x1E2228
#define PANEL_F 0x262B33                /* the focus row's */
#define SUN     0xFFC83D
#define CLOUD   0xC9D0DA
#define CARD_IMG_PX 800                 /* a picture's longer side as decoded: the Show 8's card area is ~1150 x 600 */

/* ---------------------------------------------------------------- messages (scene.h) */

/* the fields of a, tab-separated after its first space-separated ones: copies of at most n-1 bytes */
static const char *field(const char *a, int k, char *out, size_t n)
{
    const char *p = a;
    for (int i = 0; i < k && p; i++) { p = strchr(p, '\t'); if (p) p++; }
    if (!p) { out[0] = 0; return out; }
    const char *e = strchr(p, '\t'); size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l >= n) l = n - 1;
    memcpy(out, p, l); out[l] = 0;
    return out;
}

int card_msg(struct scene *s, const char *cmd, const char *a)
{
    struct card *c = &s->cd;
    if (eq(cmd, "cx")) { c->ready = 0; c->n = 0; free(c->img.px); memset(&c->img, 0, sizeof c->img); c->ikey[0] = c->ikey_shown[0] = 0; }
    else if (eq(cmd, "cb")) {                                   /* <seq> <kind> <n> <voice>\t<title>\t<subtitle> */
        char k = 'm'; int n = 0, old = c->seq;
        c->ready = 0; c->n = 0; c->nhit = 0; c->voice = 0;
        if (sscanf(a, "%d %c %d %d", &c->seq, &k, &n, &c->voice) < 2) return 1;
        if (c->seq != old) { free(c->img.px); memset(&c->img, 0, sizeof c->img); c->ikey_shown[0] = 0; }   /* the same card told
                                                                   again (a row changed, its picture fetched again) keeps it */
        c->ikey[0] = 0;
        int most = k == 'l' ? CITEMS : k == 'w' ? CDAYS : CROWS;
        c->kind = k; c->n = n < 0 ? 0 : n > most ? most : n;
        field(a, 1, c->title, sizeof c->title); field(a, 2, c->subtitle, sizeof c->subtitle);
        memset(c->r, 0, sizeof c->r); memset(c->d, 0, sizeof c->d); memset(c->it, 0, sizeof c->it); c->nbtn = 0;
        c->w_icon[0] = c->w_temp[0] = c->w_detail[0] = c->msg[0] = 0;
    } else if (eq(cmd, "cr")) {                                 /* <i> <ctl> <on> <level> <focus>\t<name>\t<value> */
        int i = -1, on = -1, lv = -1, f = 0; char ctl = 'r';
        if (sscanf(a, "%d %c %d %d %d", &i, &ctl, &on, &lv, &f) < 5 || i < 0 || i >= CROWS) return 1;
        struct crow *r = &c->r[i];
        r->ctl = ctl; r->on = on; r->level = lv; r->focus = f;
        field(a, 1, r->name, sizeof r->name); field(a, 2, r->value, sizeof r->value);
    } else if (eq(cmd, "cw")) {                                 /* <icon>\t<temperature>\t<detail> */
        char ic[sizeof c->w_icon]; field(a, 0, ic, sizeof ic);
        snprintf(c->w_icon, sizeof c->w_icon, "%s", eq(ic, "-") ? "" : ic);
        field(a, 1, c->w_temp, sizeof c->w_temp); field(a, 2, c->w_detail, sizeof c->w_detail);
    } else if (eq(cmd, "cf")) {                                 /* <i> <icon>\t<label>\t<temps>\t<precip> */
        int i = -1, k = 0;
        if (sscanf(a, "%d %n", &i, &k) < 1 || i < 0 || i >= CDAYS) return 1;
        struct cday *d = &c->d[i]; const char *b = a + k; char ic[sizeof d->icon];
        field(b, 0, ic, sizeof ic); snprintf(d->icon, sizeof d->icon, "%s", eq(ic, "-") ? "" : ic);
        field(b, 1, d->label, sizeof d->label); field(b, 2, d->temps, sizeof d->temps); field(b, 3, d->precip, sizeof d->precip);
    } else if (eq(cmd, "cm")) snprintf(c->msg, sizeof c->msg, "%s", a);
    else if (eq(cmd, "cl")) {                                   /* <i>\t<text>\t<detail> */
        int i = atoi(a);
        if (i < 0 || i >= CITEMS || !strchr(a, '\t')) return 1;
        field(a, 1, c->it[i].text, sizeof c->it[i].text); field(a, 2, c->it[i].detail, sizeof c->it[i].detail);
    } else if (eq(cmd, "cu")) {                                 /* <i>\t<label> */
        int i = atoi(a);
        if (i < 0 || i >= CBTNS || !strchr(a, '\t')) return 1;
        field(a, 1, c->btn[i], sizeof c->btn[i]);
        if (i >= c->nbtn) c->nbtn = i + 1;
    }
    else if (eq(cmd, "ci")) {                                   /* <key>: a picture, asked for when it is a new one */
        if (!a[0] || strlen(a) >= sizeof c->ikey) return 1;
        snprintf(c->ikey, sizeof c->ikey, "%s", a);
        if (!eq(c->ikey, c->ikey_shown) && s->p->image) s->p->image(s->ctx, c->ikey, CARD_IMG_PX);
    }
    else if (eq(cmd, "ce")) c->ready = 1;
    else return 0;
    return 1;
}

static int key_of(const char *k, int *seq, int *n) { return sscanf(k, "card%d.%d", seq, n) == 2; }

/* a picture for this card's key, or a late one of the same card newer than what shows: a camera slower than its
 * refresh (a 2.5 s snapshot every 2 s) answers each ask after the next was made, and would otherwise never show */
static int wanted(const struct card *c, const char *key)
{
    int ks, kn, cs, cn, ss, sn;
    if (eq(key, c->ikey)) return 1;
    if (!key_of(key, &ks, &kn) || !key_of(c->ikey, &cs, &cn) || ks != cs || kn > cn) return 0;
    return !(key_of(c->ikey_shown, &ss, &sn) && ss == ks && sn >= kn);
}

int card_image(struct scene *s, const char *key, uint32_t *px, int w, int h)
{
    struct card *c = &s->cd;
    if (strncmp(key, "card", 4)) return 0;
    if (!px || w <= 0 || h <= 0 || !wanted(c, key)) { free(px); return 1; }   /* failed (the last one stays), or stale */
    free(c->img.px);
    c->img.px = px; c->img.w = w; c->img.h = h; c->img.ver++;
    snprintf(c->ikey_shown, sizeof c->ikey_shown, "%s", key);
    s->wd.ver++;                                                /* the words layer draws again */
    return 1;
}

/* ---------------------------------------------------------------- drawing helpers */

static void chit_add(struct card *c, float x0, float y0, float x1, float y1, int row, int op, float pad)
{
    if (c->nhit >= CHITS) return;
    c->hit[c->nhit++] = (struct chit){ { (int)(x0 - pad), (int)(y0 - pad), (int)(x1 + pad), (int)(y1 + pad) }, row, op, (int)x0, (int)x1 };
}

static void text1(struct scene *s, struct gfx *g, const char *t, float px, int bold, float maxw, float x, float y, uint32_t rgb)
{
    if (t[0]) words_put(s, g, t, strlen(t), px, bold, maxw, x, y, rgb);
}

static void centred(struct scene *s, struct gfx *g, const char *t, float px, int bold, float cx, float maxw, float y, uint32_t rgb)
{
    if (!t[0]) return;
    float w = fminf(words_width(s, t, strlen(t), px, bold), maxw);
    words_put(s, g, t, strlen(t), px, bold, maxw, cx - w / 2, y, rgb);
}

static void tri(struct gfx *g, float cx, float cy, float r, int up, uint32_t rgb)          /* up 1, down 0, right 2 */
{
    float xy[6];
    if (up == 2) { float v[6] = { cx - r * 0.6f, cy - r, cx - r * 0.6f, cy + r, cx + r, cy }; memcpy(xy, v, sizeof v); }
    else { float d = up ? -1 : 1; float v[6] = { cx - r, cy - d * r * 0.55f, cx + r, cy - d * r * 0.55f, cx, cy + d * r * 0.75f }; memcpy(xy, v, sizeof v); }
    gfx_poly(g, xy, 3, rgb, 255);
}

/* a round button with its glyph */
static void cbutton(struct gfx *g, float cx, float cy, float r, int glyph, int lit)
{
    gfx_rrect(g, cx - r, cy - r, cx + r, cy + r, r, lit ? CYAN : SURF2, 255);
    uint32_t fg = lit ? 0x0B0D10 : FG; float q = r * 0.42f;
    switch (glyph) {
    case OP_OPEN: tri(g, cx, cy, q, 1, fg); break;
    case OP_CLOSE: tri(g, cx, cy, q, 0, fg); break;
    case OP_STOP: gfx_rrect(g, cx - q * 0.75f, cy - q * 0.75f, cx + q * 0.75f, cy + q * 0.75f, q * 0.15f, fg, 255); break;
    case OP_DOWN: gfx_rrect(g, cx - q, cy - q * 0.14f, cx + q, cy + q * 0.14f, q * 0.14f, fg, 255); break;
    case OP_UP:
        gfx_rrect(g, cx - q, cy - q * 0.14f, cx + q, cy + q * 0.14f, q * 0.14f, fg, 255);
        gfx_rrect(g, cx - q * 0.14f, cy - q, cx + q * 0.14f, cy + q, q * 0.14f, fg, 255); break;
    case OP_PLAY:                                               /* lit: playing, so pause shows */
        if (lit) { gfx_rrect(g, cx - q * 0.7f, cy - q, cx - q * 0.2f, cy + q, q * 0.12f, fg, 255); gfx_rrect(g, cx + q * 0.2f, cy - q, cx + q * 0.7f, cy + q, q * 0.12f, fg, 255); }
        else tri(g, cx + q * 0.1f, cy, q, 2, fg);
        break;
    }
}

static void toggle(struct gfx *g, float x1, float cy, float h, int on)    /* right edge at x1 */
{
    float w = h * 1.8f, x0 = x1 - w, k = h * 0.4f;
    gfx_rrect(g, x0, cy - h / 2, x1, cy + h / 2, h / 2, on > 0 ? CYAN : LINE, 255);
    float kx = on > 0 ? x1 - h / 2 : x0 + h / 2;
    gfx_rrect(g, kx - k, cy - k, kx + k, cy + k, k, on > 0 ? 0xFFFFFF : DIM, 255);
}

/* the weather's drawing, in a box of size z at x, y, by Home Assistant's condition names */
static void cloud(struct gfx *g, float x, float y, float z, uint32_t rgb)
{
    gfx_rrect(g, x + z * 0.10f, y + z * 0.42f, x + z * 0.90f, y + z * 0.72f, z * 0.15f, rgb, 255);
    gfx_rrect(g, x + z * 0.18f, y + z * 0.30f, x + z * 0.52f, y + z * 0.64f, z * 0.17f, rgb, 255);
    gfx_rrect(g, x + z * 0.36f, y + z * 0.16f, x + z * 0.78f, y + z * 0.58f, z * 0.21f, rgb, 255);
}

static void sun(struct gfx *g, float cx, float cy, float r)
{
    for (int i = 0; i < 8; i++) {                               /* rays */
        float a = (float)i * 0.785398f, c = cosf(a), sn = sinf(a), r0 = r * 1.3f, r1 = r * 1.75f, t = r * 0.12f;
        float xy[8] = { cx + c * r0 - sn * t, cy + sn * r0 + c * t, cx + c * r1 - sn * t, cy + sn * r1 + c * t,
                        cx + c * r1 + sn * t, cy + sn * r1 - c * t, cx + c * r0 + sn * t, cy + sn * r0 - c * t };
        gfx_poly(g, xy, 4, SUN, 255);
    }
    gfx_rrect(g, cx - r, cy - r, cx + r, cy + r, r, SUN, 255);
}

static void weather_icon(struct gfx *g, const char *c, float x, float y, float z)
{
    if (!c[0]) return;
    int rain = strstr(c, "rain") || eq(c, "pouring"), snow = strstr(c, "snow") != NULL, bolt = strstr(c, "lightning") != NULL;
    if (eq(c, "sunny")) { sun(g, x + z / 2, y + z / 2, z * 0.22f); return; }
    if (eq(c, "clear-night")) {                                 /* a crescent: a disc, another of the card's black over it */
        float cx = x + z * 0.5f, cy = y + z * 0.5f, r = z * 0.28f, o = r * 0.55f;
        gfx_rrect(g, cx - r, cy - r, cx + r, cy + r, r, FG, 255);
        gfx_rrect(g, cx - r + o, cy - r - o * 0.35f, cx + r + o, cy + r - o * 0.35f, r, BG, 255);
        return;
    }
    if (eq(c, "fog")) { for (int i = 0; i < 3; i++) gfx_rrect(g, x + z * 0.15f, y + z * (0.32f + i * 0.16f), x + z * 0.85f, y + z * (0.40f + i * 0.16f), z * 0.04f, CLOUD, 255); return; }
    if (eq(c, "partlycloudy")) sun(g, x + z * 0.36f, y + z * 0.34f, z * 0.15f);
    cloud(g, x, y - (rain || snow || bolt ? z * 0.08f : 0), z, eq(c, "partlycloudy") ? FG : CLOUD);
    for (int i = 0; i < 3 && rain; i++) {
        float bx = x + z * (0.30f + i * 0.18f), by = y + z * 0.70f, t = z * 0.035f;
        float xy[8] = { bx, by, bx + t * 2, by, bx - z * 0.06f + t * 2, by + z * 0.18f, bx - z * 0.06f, by + z * 0.18f };
        gfx_poly(g, xy, 4, CYAN, 255);
    }
    for (int i = 0; i < 3 && snow; i++) { float bx = x + z * (0.30f + i * 0.20f), by = y + z * 0.80f, r = z * 0.04f; gfx_rrect(g, bx - r, by - r, bx + r, by + r, r, FG, 255); }
    if (bolt) { float xy[8] = { x + z * 0.52f, y + z * 0.60f, x + z * 0.42f, y + z * 0.80f, x + z * 0.52f, y + z * 0.78f, x + z * 0.46f, y + z * 0.96f };
                gfx_poly(g, xy, 4, ORANGE, 255); }
}

/* ---------------------------------------------------------------- the three kinds */

static void entities_draw(struct scene *s, struct gfx *g, float top, float H, float W, float side)
{
    struct card *c = &s->cd;
    int n = c->n, cols = n > 4 ? 2 : 1, per = (n + cols - 1) / cols;
    float gap = H * 0.025f, area = H - top - H * 0.06f, rh = fminf(H * 0.17f, (area - gap * (per - 1)) / per);
    float cw = (W - 2 * side - gap * (cols - 1)) / cols;
    for (int i = 0; i < n; i++) {
        const struct crow *r = &c->r[i];
        int col = i / per, k = i % per;
        float x0 = side + col * (cw + gap), y0 = top + k * (rh + gap), x1 = x0 + cw, y1 = y0 + rh, pad = rh * 0.18f;
        gfx_rrect(g, x0, y0, x1, y1, rh * 0.18f, r->focus ? PANEL_F : PANEL, 255);
        if (r->focus) gfx_rrect(g, x0, y0 + rh * 0.2f, x0 + fmaxf(3, rh * 0.05f), y1 - rh * 0.2f, rh * 0.03f, CYAN, 255);
        float np = rh * 0.30f, vp = rh * 0.22f, tx = x0 + rh * 0.28f, cy = (y0 + y1) / 2;
        /* the control: its width first, so the text knows its room */
        float ctrl = 0, bh = rh * 0.40f, br = rh * 0.25f;
        switch (r->ctl) {
        case 't': case 's': ctrl = bh * 1.8f; toggle(g, x1 - pad, cy, bh, r->on); chit_add(c, x1 - pad - bh * 1.8f, cy - bh / 2, x1 - pad, cy + bh / 2, i, OP_TOGGLE, pad); break;
        case 'c': ctrl = br * 6.8f;
            for (int b = 0; b < 3; b++) { float bx = x1 - pad - br - b * br * 2.4f; int op = b == 0 ? OP_CLOSE : b == 1 ? OP_STOP : OP_OPEN;
                cbutton(g, bx, cy, br, op, 0); chit_add(c, bx - br, cy - br, bx + br, cy + br, i, op, br * 0.15f); }
            break;
        case 'h': ctrl = br * 4.4f;
            for (int b = 0; b < 2; b++) { float bx = x1 - pad - br - b * br * 2.4f; int op = b == 0 ? OP_UP : OP_DOWN;
                cbutton(g, bx, cy, br, op, 0); chit_add(c, bx - br, cy - br, bx + br, cy + br, i, op, br * 0.15f); }
            break;
        case 'p': ctrl = br * 2; cbutton(g, x1 - pad - br, cy, br, OP_PLAY, r->on > 0); chit_add(c, x1 - pad - br * 2, cy - br, x1 - pad, cy + br, i, OP_PLAY, pad); break;
        case 'b': { float lw = words_width(s, "Run", 3, vp, 1) + rh * 0.5f; ctrl = lw;
            gfx_rrect(g, x1 - pad - lw, cy - bh / 2, x1 - pad, cy + bh / 2, bh / 2, CYAN, 255);
            text1(s, g, "Run", vp, 1, lw, x1 - pad - lw + rh * 0.25f, cy - vp * 0.5f, 0x0B0D10);
            chit_add(c, x1 - pad - lw, cy - bh / 2, x1 - pad, cy + bh / 2, i, OP_RUN, pad); } break;
        }
        float room = x1 - tx - (ctrl ? ctrl + pad * 1.6f : pad);
        if (r->ctl == 's') {                                    /* the name, the brightness bar, the state */
            float ny = y0 + rh * 0.10f, by = y0 + rh * 0.52f, th = fmaxf(4, rh * 0.07f), lv = r->level < 0 ? 0 : r->level / 100.0f;
            text1(s, g, r->name, np, 0, room, tx, ny, FG);
            gfx_rrect(g, tx, by, tx + room, by + th, th / 2, LINE, 255);
            if (r->on > 0 && lv > 0) gfx_rrect(g, tx, by, tx + fmaxf(th, room * lv), by + th, th / 2, CYAN, 255);
            chit_add(c, tx, by - rh * 0.12f, tx + room, by + th + rh * 0.12f, i, OP_LEVEL, 0);
            text1(s, g, r->value, vp, 0, room, tx, y0 + rh * 0.66f, DIM);
        } else {
            text1(s, g, r->name, np, 0, room, tx, y0 + rh * 0.14f, FG);
            text1(s, g, r->value, vp, 0, room, tx, y0 + rh * 0.56f, r->on > 0 ? CYAN : DIM);
        }
    }
}

static void weather_draw(struct scene *s, struct gfx *g, float top, float H, float W, float side)
{
    struct card *c = &s->cd;
    int nd = c->n;
    float fh = nd ? H * 0.30f : 0, room = H - top - fh - H * 0.05f;
    float z = fminf(room, H * 0.36f), ts = z * 0.62f;
    float tw = c->w_temp[0] ? words_width(s, c->w_temp, strlen(c->w_temp), ts, 1) : 0;
    float bx = (W - (c->w_icon[0] ? z : 0) - tw - z * 0.15f) / 2;
    if (c->w_icon[0]) { weather_icon(g, c->w_icon, bx, top, z); bx += z + z * 0.15f; }
    text1(s, g, c->w_temp, ts, 1, W, bx, top + (z - ts) / 2, FG);
    centred(s, g, c->w_detail, H * 0.055f, 0, W / 2, W - 2 * side, top + z + H * 0.01f, DIM);
    if (!nd) return;
    float y = H - fh - H * 0.04f, colw = (W - 2 * side) / nd, iz = fh * 0.40f;
    for (int i = 0; i < nd; i++) {
        const struct cday *d = &c->d[i];
        float cx = side + colw * (i + 0.5f);
        centred(s, g, d->label, fh * 0.15f, 0, cx, colw * 0.95f, y, DIM);
        weather_icon(g, d->icon, cx - iz / 2, y + fh * 0.18f, iz);
        centred(s, g, d->temps, fh * 0.15f, 1, cx, colw * 0.95f, y + fh * 0.60f, FG);
        centred(s, g, d->precip, fh * 0.13f, 0, cx, colw * 0.95f, y + fh * 0.80f, CYAN);
    }
}

static void list_draw(struct scene *s, struct gfx *g, float top, float H, float W, float side)
{
    struct card *c = &s->cd;
    int n = c->n, cols = n > 6 ? 2 : 1, per = (n + cols - 1) / cols;
    if (!n) return;
    float gap = H * 0.02f, area = H - top - H * 0.06f, rh = fminf(H * 0.14f, (area - gap * (per - 1)) / per);
    float cw = (W - 2 * side - gap * (cols - 1)) / cols;
    for (int i = 0; i < n; i++) {
        const struct citem *it = &c->it[i];
        int col = i / per, k = i % per;
        float x0 = side + col * (cw + gap), y0 = top + k * (rh + gap), tx = x0 + rh * 0.28f, room = cw - rh * 0.5f;
        gfx_rrect(g, x0, y0, x0 + cw, y0 + rh, rh * 0.18f, PANEL, 255);
        if (it->detail[0]) {
            float tp = rh * 0.36f, dp = rh * 0.26f, y = y0 + (rh - tp - dp * 1.1f) / 2;
            text1(s, g, it->text, tp, 0, room, tx, y, FG);
            text1(s, g, it->detail, dp, 0, room, tx, y + tp * 1.08f, DIM);
        } else text1(s, g, it->text, rh * 0.40f, 0, room, tx, y0 + rh * 0.30f, FG);
    }
}

/* the buttons along the bottom, the first the main one (lit), right to left from the right edge */
static float buttons_draw(struct scene *s, struct gfx *g, float H, float W, float side)
{
    struct card *c = &s->cd;
    float bh = H * 0.11f, y1 = H - H * 0.05f, y0 = y1 - bh, x1 = W - side, fp = bh * 0.40f, gap = bh * 0.25f;
    for (int i = 0; i < c->nbtn; i++) {
        const char *l = c->btn[i];
        float w = fminf(words_width(s, l, strlen(l), fp, 1) + bh * 0.9f, (W - 2 * side) / c->nbtn - gap), x0 = x1 - w;
        gfx_rrect(g, x0, y0, x1, y1, bh / 2, i == 0 ? CYAN : SURF2, 255);
        float tw = fminf(words_width(s, l, strlen(l), fp, 1), w - bh * 0.6f);
        text1(s, g, l, fp, 1, w - bh * 0.6f, x0 + (w - tw) / 2, y0 + (bh - fp) / 2 - fp * 0.08f, i == 0 ? 0x0B0D10 : FG);
        chit_add(c, x0, y0, x1, y1, i, OP_PRESS, bh * 0.12f);
        x1 = x0 - gap;
    }
    return c->nbtn ? bh + H * 0.08f : 0;                        /* what the content above leaves free */
}

static void message_draw(struct scene *s, struct gfx *g, float top, float H, float W, float side)
{
    struct card *c = &s->cd;
    const char *t = c->msg;
    float x0 = side, maxw = W - 2 * side;
    if (c->ikey[0]) {                                           /* the picture on the left, as large as fits */
        float bh = H - top - H * 0.06f, a = c->img.px ? (float)c->img.w / (float)c->img.h : 4.0f / 3, room = maxw * (t[0] ? 0.64f : 1);
        float bw = bh * a;
        if (bw > room) { bw = room; bh = bw / a; }
        float bx = t[0] ? side : (W - bw) / 2, rad = H * 0.02f;
        if (c->img.px) gfx_image_scaled(g, (int)lroundf(bx), (int)lroundf(top), (int)lroundf(bw), (int)lroundf(bh), c->img.px, c->img.w, c->img.h, c->img.w, rad);
        else gfx_rrect(g, bx, top, bx + bw, top + bh, rad, PANEL, 255);   /* on its way */
        x0 = bx + bw + side * 0.8f; maxw = W - side - x0;
        if (!t[0] || maxw < W * 0.15f) return;
    }
    if (!t[0]) return;
    struct wline l[12]; float L;
    int n = 0, shown;
    for (L = c->ikey[0] ? H * 0.08f : H * 0.12f; ; L *= 0.92f) {  /* the largest size whose lines fit (beside a picture
                                                                   smaller: its narrow column would take a word a line) */
        n = words_wrap(s, t, L, 0, maxw, l, 12);
        shown = (int)((H - top - H * 0.06f) / (L * 1.1f));
        if (n <= shown || L <= H * 0.06f) break;
    }
    if (shown > 12) shown = 12;
    if (n < shown) shown = n;
    float y = top + fmaxf(0, (H - top - H * 0.06f - shown * L * 1.1f) / 2);   /* in the middle of what is left */
    for (int i = 0; i < shown; i++) {
        size_t k = i == shown - 1 && n > shown ? strlen(l[i].a) : (size_t)l[i].n;
        words_put(s, g, l[i].a, k, L, 0, maxw, x0, y + i * L * 1.1f, FG);
    }
}

void card_draw(struct scene *s, struct gfx *g)
{
    struct card *c = &s->cd;
    float H = (float)s->h, W = (float)s->w, side = W * 0.05f, y = H * 0.06f;
    c->nhit = 0;
    const char *sub = c->subtitle[0] || !c->voice ? c->subtitle : s->wd.reply;   /* a request's: the reply as said where
                                                                   Home Assistant gave none */
    if (c->title[0]) { text1(s, g, c->title, H * 0.085f, 1, W - 2 * side, side, y, FG); y += H * 0.085f * 1.15f; }
    if (sub[0]) { text1(s, g, sub, H * 0.05f, 0, W - 2 * side, side, y, DIM); y += H * 0.05f * 1.25f; }
    y += H * 0.02f;
    float low = buttons_draw(s, g, H, W, side);                 /* (their room comes off the bottom: the content's own
                                                                   layout ends higher) */
    if (c->kind == 'e') entities_draw(s, g, y, H - low, W, side);
    else if (c->kind == 'w') weather_draw(s, g, y, H - low, W, side);
    else if (c->kind == 'l') list_draw(s, g, y, H - low, W, side);
    else message_draw(s, g, y, H - low, W, side);
}

/* ---------------------------------------------------------------- taps */

void card_tap(struct scene *s, int x, int y)
{
    struct card *c = &s->cd;
    for (int i = 0; i < c->nhit; i++) {
        const struct chit *h = &c->hit[i];
        if (x < h->r.x0 || x >= h->r.x1 || y < h->r.y0 || y >= h->r.y1) continue;
        if (h->op == OP_LEVEL) {
            int lv = h->x1 > h->x0 ? (int)lroundf(clampf((float)(x - h->x0) / (float)(h->x1 - h->x0), 0, 1) * 100) : 100;
            if (lv < 1) lv = 1;
            send(s, "card tap %d %d level %d", c->seq, h->row, lv);
        } else send(s, "card tap %d %d %s", c->seq, h->row, op_names[h->op]);
        return;
    }
    send(s, "card clear %d", c->seq);                         /* elsewhere: away, here at once and in hassmic */
    c->ready = 0; s->wd.ver++;
}
