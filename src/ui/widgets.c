/* See ui.h: icons drawn from the display list's shapes (no image files: they scale with the screen and cost nothing
 * to keep), buttons, sliders, the keyboard, the action sheet, toasts. */
#include "ui.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265f

/* a line t thick with round ends: a quad and two discs (ids id, id + 1, id + 2) */
static void seg(struct scene *s, int id, float x0, float y0, float x1, float y1, float t, uint32_t col)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l <= 0) return;
    float nx = -dy / l * t / 2, ny = dx / l * t / 2;
    float q[] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny };
    poly(s, id, q, 4, col, 255);
    rrect(s, id + 1, x0 - t / 2, y0 - t / 2, x0 + t / 2, y0 + t / 2, t / 2, col, 255);
    rrect(s, id + 2, x1 - t / 2, y1 - t / 2, x1 + t / 2, y1 + t / 2, t / 2, col, 255);
}

static void tri(struct scene *s, int id, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col)
{
    float t[] = { x0, y0, x1, y1, x2, y2 };
    poly(s, id, t, 3, col, 255);
}

/* an icon in a box of 2r around (cx, cy); bg: what it is drawn on (the moon's cut) */
void icon(struct scene *s, int id, int which, float cx, float cy, float r, uint32_t col, uint32_t bg)
{
    float t = fmaxf(1.6f, r * 0.17f);                           /* the stroke of outline icons */
    switch (which) {
    case IC_PLAY: tri(s, id, cx - 0.55f * r, cy - 0.8f * r, cx + 0.85f * r, cy, cx - 0.55f * r, cy + 0.8f * r, col); break;
    case IC_PAUSE:
        rrect(s, id, cx - 0.62f * r, cy - 0.75f * r, cx - 0.17f * r, cy + 0.75f * r, 0.12f * r, col, 255);
        rrect(s, id + 1, cx + 0.17f * r, cy - 0.75f * r, cx + 0.62f * r, cy + 0.75f * r, 0.12f * r, col, 255);
        break;
    case IC_NEXT: case IC_PREV: {
        float d = which == IC_NEXT ? 1 : -1;
        tri(s, id, cx - 0.65f * d * r, cy - 0.7f * r, cx + 0.45f * d * r, cy, cx - 0.65f * d * r, cy + 0.7f * r, col);
        float bx = cx + 0.45f * d * r;
        rrect(s, id + 1, fminf(bx, bx + 0.24f * d * r), cy - 0.7f * r, fmaxf(bx, bx + 0.24f * d * r), cy + 0.7f * r, 0.06f * r, col, 255);
    } break;
    case IC_SHUFFLE:
        seg(s, id, cx - 0.8f * r, cy + 0.5f * r, cx + 0.45f * r, cy - 0.5f * r, t, col);
        seg(s, id + 3, cx - 0.8f * r, cy - 0.5f * r, cx + 0.45f * r, cy + 0.5f * r, t, col);
        tri(s, id + 6, cx + 0.35f * r, cy - 0.85f * r, cx + 0.9f * r, cy - 0.5f * r, cx + 0.35f * r, cy - 0.15f * r, col);
        tri(s, id + 7, cx + 0.35f * r, cy + 0.15f * r, cx + 0.9f * r, cy + 0.5f * r, cx + 0.35f * r, cy + 0.85f * r, col);
        break;
    case IC_REPEAT: case IC_REPEAT1:
        stroke(s, id, cx - 0.85f * r, cy - 0.55f * r, cx + 0.85f * r, cy + 0.55f * r, 0.4f * r, t, col, 255);
        rrect(s, id + 1, cx - 0.2f * r, cy - 0.55f * r - t, cx + 0.3f * r, cy - 0.55f * r + t, 0, bg, 255);   /* the gap */
        tri(s, id + 2, cx + 0.05f * r, cy - 0.95f * r, cx + 0.5f * r, cy - 0.55f * r, cx + 0.05f * r, cy - 0.15f * r, col);
        if (which == IC_REPEAT1) rrect(s, id + 3, cx - 0.1f * r, cy - 0.28f * r, cx + 0.12f * r, cy + 0.3f * r, 0.05f * r, col, 255);
        break;
    case IC_NOTE:
        rrect(s, id, cx - 0.62f * r, cy + 0.12f * r, cx + 0.02f * r, cy + 0.68f * r, 0.32f * r, col, 255);
        rrect(s, id + 1, cx - 0.11f * r, cy - 0.8f * r, cx + 0.03f * r, cy + 0.42f * r, 0.03f * r, col, 255);
        { float f[] = { cx - 0.11f * r, cy - 0.8f * r, cx + 0.55f * r, cy - 0.45f * r, cx + 0.55f * r, cy - 0.18f * r, cx - 0.11f * r, cy - 0.5f * r };
          poly(s, id + 2, f, 4, col, 255); }
        break;
    case IC_QUEUE: case IC_LIST:
        for (int i = 0; i < 3; i++) {
            float y = cy + (i - 1) * 0.55f * r, x1 = which == IC_QUEUE && i == 2 ? cx + 0.1f * r : cx + 0.8f * r;
            rrect(s, id + i, cx - 0.8f * r, y - t / 2, x1, y + t / 2, t / 2, col, 255);
        }
        if (which == IC_QUEUE) tri(s, id + 3, cx + 0.32f * r, cy + 0.3f * r, cx + 0.88f * r, cy + 0.62f * r, cx + 0.32f * r, cy + 0.94f * r, col);
        break;
    case IC_LIBRARY:
        rrect(s, id, cx - 0.8f * r, cy - 0.75f * r, cx - 0.45f * r, cy + 0.75f * r, 0.08f * r, col, 255);
        rrect(s, id + 1, cx - 0.3f * r, cy - 0.75f * r, cx + 0.05f * r, cy + 0.75f * r, 0.08f * r, col, 255);
        { float b[] = { cx + 0.2f * r, cy - 0.62f * r, cx + 0.52f * r, cy - 0.72f * r, cx + 0.88f * r, cy + 0.66f * r, cx + 0.56f * r, cy + 0.76f * r };
          poly(s, id + 2, b, 4, col, 255); }
        break;
    case IC_SEARCH:
        arc(s, id, cx - 0.15f * r, cy - 0.15f * r, 0.5f * r, t * 1.1f, 0, 2 * PI, col, 255);
        seg(s, id + 1, cx + 0.24f * r, cy + 0.24f * r, cx + 0.75f * r, cy + 0.75f * r, t * 1.3f, col);
        break;
    case IC_SPEAKER:
        rrect(s, id, cx - 0.85f * r, cy - 0.3f * r, cx - 0.45f * r, cy + 0.3f * r, 0.06f * r, col, 255);
        { float c[] = { cx - 0.5f * r, cy - 0.3f * r, cx - 0.02f * r, cy - 0.72f * r, cx - 0.02f * r, cy + 0.72f * r, cx - 0.5f * r, cy + 0.3f * r };
          poly(s, id + 1, c, 4, col, 255); }
        arc(s, id + 2, cx - 0.05f * r, cy, 0.42f * r, t, -0.8f, 0.8f, col, 255);
        arc(s, id + 3, cx - 0.05f * r, cy, 0.8f * r, t, -0.8f, 0.8f, col, 255);
        break;
    case IC_SPEAKERS: case IC_GROUP:
        for (int i = 0; i < 2; i++) {
            float x = cx + (i ? 0.38f : -0.38f) * r, h = i ? 0.8f : 0.62f;
            stroke(s, id + 3 * i, x - 0.34f * r, cy - h * r, x + 0.34f * r, cy + h * r, 0.12f * r, t, col, 255);
            arc(s, id + 3 * i + 1, x, cy + (h - 0.42f) * r, 0.17f * r, t, 0, 2 * PI, col, 255);
            rrect(s, id + 3 * i + 2, x - 0.06f * r, cy - (h - 0.24f) * r - 0.06f * r, x + 0.06f * r, cy - (h - 0.24f) * r + 0.06f * r, 0.06f * r, col, 255);
        }
        break;
    case IC_HOME:
        tri(s, id, cx - 0.92f * r, cy - 0.05f * r, cx, cy - 0.88f * r, cx + 0.92f * r, cy - 0.05f * r, col);
        rrect(s, id + 1, cx - 0.62f * r, cy - 0.2f * r, cx + 0.62f * r, cy + 0.8f * r, 0.08f * r, col, 255);
        rrect(s, id + 2, cx - 0.18f * r, cy + 0.25f * r, cx + 0.18f * r, cy + 0.8f * r, 0.04f * r, bg, 255);
        break;
    case IC_MOON:
        rrect(s, id, cx - 0.75f * r, cy - 0.75f * r, cx + 0.75f * r, cy + 0.75f * r, 0.75f * r, col, 255);
        rrect(s, id + 1, cx - 0.35f * r, cy - 1.0f * r, cx + 1.0f * r, cy + 0.35f * r, 0.68f * r, bg, 255);
        break;
    case IC_POWER:
        arc(s, id, cx, cy + 0.08f * r, 0.66f * r, t * 1.15f, -PI / 2 + 0.75f, 3 * PI / 2 - 0.75f, col, 255);
        seg(s, id + 1, cx, cy - 0.85f * r, cx, cy - 0.05f * r, t * 1.15f, col);
        break;
    case IC_BACK:
        seg(s, id, cx + 0.3f * r, cy - 0.7f * r, cx - 0.4f * r, cy, t * 1.3f, col);
        seg(s, id + 3, cx - 0.4f * r, cy, cx + 0.3f * r, cy + 0.7f * r, t * 1.3f, col);
        break;
    case IC_CLOSE:
        seg(s, id, cx - 0.6f * r, cy - 0.6f * r, cx + 0.6f * r, cy + 0.6f * r, t * 1.2f, col);
        seg(s, id + 3, cx + 0.6f * r, cy - 0.6f * r, cx - 0.6f * r, cy + 0.6f * r, t * 1.2f, col);
        break;
    case IC_PLUS:
        rrect(s, id, cx - 0.7f * r, cy - t * 0.6f, cx + 0.7f * r, cy + t * 0.6f, t * 0.6f, col, 255);
        rrect(s, id + 1, cx - t * 0.6f, cy - 0.7f * r, cx + t * 0.6f, cy + 0.7f * r, t * 0.6f, col, 255);
        break;
    case IC_CHECK:
        seg(s, id, cx - 0.65f * r, cy + 0.02f * r, cx - 0.18f * r, cy + 0.5f * r, t * 1.3f, col);
        seg(s, id + 3, cx - 0.18f * r, cy + 0.5f * r, cx + 0.7f * r, cy - 0.5f * r, t * 1.3f, col);
        break;
    case IC_RADIO:
        rrect(s, id, cx - 0.18f * r, cy - 0.18f * r, cx + 0.18f * r, cy + 0.18f * r, 0.18f * r, col, 255);
        arc(s, id + 1, cx, cy, 0.48f * r, t, -0.8f, 0.8f, col, 255);
        arc(s, id + 2, cx, cy, 0.48f * r, t, PI - 0.8f, PI + 0.8f, col, 255);
        arc(s, id + 3, cx, cy, 0.85f * r, t, -0.75f, 0.75f, col, 255);
        arc(s, id + 4, cx, cy, 0.85f * r, t, PI - 0.75f, PI + 0.75f, col, 255);
        break;
    case IC_MIC:
        rrect(s, id, cx - 0.28f * r, cy - 0.85f * r, cx + 0.28f * r, cy + 0.2f * r, 0.28f * r, col, 255);
        arc(s, id + 1, cx, cy - 0.12f * r, 0.55f * r, t, 0.1f, PI - 0.1f, col, 255);
        rrect(s, id + 2, cx - t / 2, cy + 0.42f * r, cx + t / 2, cy + 0.85f * r, t / 2, col, 255);
        break;
    case IC_PERSON:
        rrect(s, id, cx - 0.36f * r, cy - 0.85f * r, cx + 0.36f * r, cy - 0.13f * r, 0.36f * r, col, 255);
        rrect(s, id + 1, cx - 0.75f * r, cy + 0.05f * r, cx + 0.75f * r, cy + 1.2f * r, 0.6f * r, col, 255);
        rrect(s, id + 2, cx - r, cy + 0.85f * r, cx + r, cy + 1.3f * r, 0, bg, 255);
        break;
    case IC_ALBUM: case IC_DISC:
        arc(s, id, cx, cy, 0.7f * r, t, 0, 2 * PI, col, 255);
        rrect(s, id + 1, cx - 0.17f * r, cy - 0.17f * r, cx + 0.17f * r, cy + 0.17f * r, 0.17f * r, col, 255);
        break;
    case IC_BACKSPACE: {
        float b[] = { cx - 0.95f * r, cy, cx - 0.45f * r, cy - 0.62f * r, cx + 0.9f * r, cy - 0.62f * r, cx + 0.9f * r, cy + 0.62f * r };
        poly(s, id, b, 4, col, 255);
        tri(s, id + 1, cx - 0.95f * r, cy, cx + 0.9f * r, cy + 0.62f * r, cx - 0.45f * r, cy + 0.62f * r, col);
        seg(s, id + 2, cx - 0.05f * r, cy - 0.25f * r, cx + 0.45f * r, cy + 0.25f * r, t, bg);
        seg(s, id + 5, cx + 0.45f * r, cy - 0.25f * r, cx - 0.05f * r, cy + 0.25f * r, t, bg);
    } break;
    case IC_SHIFT:
        tri(s, id, cx - 0.75f * r, cy + 0.05f * r, cx, cy - 0.8f * r, cx + 0.75f * r, cy + 0.05f * r, col);
        rrect(s, id + 1, cx - 0.32f * r, cy, cx + 0.32f * r, cy + 0.7f * r, 0.05f * r, col, 255);
        break;
    case IC_FACE:                                               /* the face's own eyes, wide apart, and a smile */
        for (int e = 0; e < 2; e++) {
            float x = cx + (e ? 0.45f : -0.45f) * r;
            rrect(s, id + e, x - 0.17f * r, cy - 0.62f * r, x + 0.17f * r, cy + 0.08f * r, 0.17f * r, col, 255);
        }
        arc(s, id + 2, cx, cy + 0.05f * r, 0.55f * r, t, 0.55f, 3.14159f - 0.55f, col, 255);
        break;
    case IC_MORE:
        for (int i = 0; i < 3; i++) {
            float x = cx + (i - 1) * 0.6f * r;
            rrect(s, id + i, x - 0.15f * r, cy - 0.15f * r, x + 0.15f * r, cy + 0.15f * r, 0.15f * r, col, 255);
        }
        break;
    }
}

void button(struct scene *s, int id, float x0, float y0, float x1, float y1, const char *label, int ic, uint32_t bg, uint32_t fg, int act, int arg)
{
    int on = pressed(s, act, arg); float h = y1 - y0, px = h * 0.42f;
    rrect(s, id, x0, y0, x1, y1, h / 2, on ? gfx_mix(bg, 0xFFFFFF, 0.18f) : bg, 255);
    float lw = label && label[0] ? (float)text_width(s, label, px, 1, x1 - x0 - h * (ic >= 0 ? 1.4f : 0.6f)) : 0;
    float iw = ic >= 0 ? h * 0.5f : 0, gap = ic >= 0 && lw > 0 ? h * 0.18f : 0, total = iw + gap + lw;
    float x = (x0 + x1) / 2 - total / 2;
    if (ic >= 0) icon(s, id + 1, ic, x + iw / 2, (y0 + y1) / 2, h * 0.24f, fg, bg);
    if (lw > 0) text(s, id + 9, label, px, 1, x1 - x0 - h * (ic >= 0 ? 1.4f : 0.6f), x + iw + gap, y0 + (h - px) / 2, 0, fg, 255);
    hit(s, x0 - U(4), y0 - U(6), x1 + U(4), y1 + U(6), act, arg);
}

void round_button(struct scene *s, int id, float cx, float cy, float r, int ic, uint32_t bg, uint32_t fg, int act, int arg)
{
    int on = pressed(s, act, arg);
    uint32_t b = on ? gfx_mix(bg, 0xFFFFFF, 0.2f) : bg;
    if (bg != BG || on) rrect(s, id, cx - r, cy - r, cx + r, cy + r, r, on && bg == BG ? SURF2 : b, 255);
    icon(s, id + 1, ic, cx, cy, r * 0.45f, fg, b);
    hit(s, cx - r - U(6), cy - r - U(6), cx + r + U(6), cy + r + U(6), act, arg);
}

void slider(struct scene *s, int id, float x0, float x1, float y, float v, uint32_t col, int act, int arg)
{
    float th = U(6), kr = U(pressed(s, act, arg) ? 11 : 9);
    v = clampf(v, 0, 1);
    float kx = x0 + (x1 - x0) * v;
    rrect(s, id, x0, y - th / 2, x1, y + th / 2, th / 2, 0x3A3F47, 255);
    rrect(s, id + 1, x0, y - th / 2, fmaxf(kx, x0 + th), y + th / 2, th / 2, col, 255);
    rrect(s, id + 2, kx - kr, y - kr, kx + kr, y + kr, kr, FG, 255);
    hit_drag(s, x0 - U(14), y - U(22), x1 + U(14), y + U(22), act, arg);
}

float slider_value(struct scene *s, float x0, float x1) { return clampf((s->x - x0) / (x1 - x0), 0, 1); }

/* QWERTY on four rows; the symbols' page instead of letters.  Keys a tenth of the width */
void keyboard(struct scene *s, int id, struct rect r, int shift, int sym, int act)
{
    static const char *letters[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    static const char *symbols[3] = { "1234567890", "-/:;()&@\"", ".,?!'#+" };
    const char *const *rows = sym ? symbols : letters;
    float W = (float)(r.x1 - r.x0), H = (float)(r.y1 - r.y0), kw = W / 10, kh = H / 4, g = U(3);
    fill(s, id, r, 0x0B0C0F);
    int n = 1;
    for (int row = 0; row < 3; row++) {
        int len = (int)strlen(rows[row]);
        float x = r.x0 + (W - len * kw) / 2, y = r.y0 + row * kh;
        for (int i = 0; i < len; i++, n += 2) {
            char c = rows[row][i]; char lab[2] = { (char)(shift && !sym && c >= 'a' && c <= 'z' ? c - 32 : c), 0 };
            float k0 = x + i * kw;
            int on = pressed(s, act, (unsigned char)lab[0]);
            rrect(s, id + n, k0 + g, y + g, k0 + kw - g, y + kh - g, U(7), on ? 0x4A505A : 0x2A2E35, 255);
            text(s, id + n + 1, lab, kh * 0.42f, 0, kw, k0 + kw / 2, y + kh * 0.27f, 1, FG, 255);
            hit(s, k0, y, k0 + kw, y + kh, act, (unsigned char)lab[0]);
        }
    }
    float y3 = r.y0 + 2 * kh, y4 = r.y0 + 3 * kh;
    /* shift (letters only) and backspace beside the third row */
    if (!sym) {
        int on = pressed(s, act, K_SHIFT);
        rrect(s, id + 200, r.x0 + g, y3 + g, r.x0 + 1.4f * kw - g, y3 + kh - g, U(7), shift ? FG : on ? 0x4A505A : 0x3A3F47, 255);
        icon(s, id + 201, IC_SHIFT, r.x0 + 0.7f * kw, y3 + kh / 2, kh * 0.22f, shift ? 0x0B0C0F : FG, shift ? FG : 0x3A3F47);
        hit(s, r.x0, y3, r.x0 + 1.4f * kw, y3 + kh, act, K_SHIFT);
    }
    { int on = pressed(s, act, K_BACKSPACE);
      rrect(s, id + 210, r.x1 - 1.4f * kw + g, y3 + g, r.x1 - g, y3 + kh - g, U(7), on ? 0x4A505A : 0x3A3F47, 255);
      icon(s, id + 211, IC_BACKSPACE, r.x1 - 0.7f * kw, y3 + kh / 2, kh * 0.22f, FG, on ? 0x4A505A : 0x3A3F47);
      hit(s, r.x1 - 1.4f * kw, y3, r.x1, y3 + kh, act, K_BACKSPACE); }
    /* ?123 / ABC, space, done */
    struct { int k; float a, b; const char *l; } bot[3] = {
        { K_SYM, 0, 1.6f, sym ? "ABC" : "?123" }, { K_SPACE, 1.6f, 7.8f, "space" }, { K_DONE, 7.8f, 10, "Done" } };
    for (int i = 0; i < 3; i++) {
        float k0 = r.x0 + bot[i].a * kw, k1 = r.x0 + bot[i].b * kw; int on = pressed(s, act, bot[i].k);
        uint32_t bg = bot[i].k == K_DONE ? (on ? 0x1E8FBF : ACCENT) : on ? 0x4A505A : bot[i].k == K_SPACE ? 0x2A2E35 : 0x3A3F47;
        rrect(s, id + 220 + 2 * i, k0 + g, y4 + g, k1 - g, y4 + kh - g, U(7), bg, 255);
        text(s, id + 221 + 2 * i, bot[i].l, kh * 0.36f, bot[i].k == K_DONE, k1 - k0, (k0 + k1) / 2, y4 + kh * 0.31f, 1,
             bot[i].k == K_DONE ? 0x0B0C0F : bot[i].k == K_SPACE ? DIM : FG, 255);
        hit(s, k0, y4, k1, y4 + kh, act, bot[i].k);
    }
}

void sheet(struct scene *s, int id, const char *title, const char *const *rows, int n, int act, int shade)
{
    float w = fminf(U(440), s->w - U(40)), rh = U(54), th = title && title[0] ? U(56) : U(12);
    float h = th + n * rh + U(12), x0 = (s->w - w) / 2, y0 = fmaxf(U(16), (s->h - h) / 2);
    rrect(s, id, 0, 0, (float)s->w, (float)s->h, 0, 0x000000, 150);
    hit(s, 0, 0, 0, 0, shade, -1);                              /* everything else: a tap there closes it */
    rrect(s, id + 1, x0, y0, x0 + w, y0 + h, U(18), SURF, 255);
    hit(s, x0, y0, x0 + w, y0 + h, A_NONE, 0);                  /* the panel itself: taps there do nothing */
    if (th > U(20)) text(s, id + 2, title, U(20), 1, w - U(40), x0 + U(24), y0 + U(18), 0, FG, 255);
    for (int i = 0; i < n && i < 12; i++) {
        float y = y0 + th + i * rh;
        if (pressed(s, act, i)) rrect(s, id + 10 + 3 * i, x0 + U(8), y, x0 + w - U(8), y + rh, U(12), SURF2, 255);
        text(s, id + 11 + 3 * i, rows[i], U(20), 0, w - U(48), x0 + U(24), y + (rh - U(20)) / 2, 0, FG, 255);
        hit(s, x0, y, x0 + w, y + rh, act, i);
    }
}

void toast(struct scene *s, int id, const char *msg)
{
    float px = U(18), tw = (float)text_width(s, msg, px, 0, s->w * 0.8f), h = U(44), w = tw + U(48);
    float x0 = (s->w - w) / 2, y0 = s->h - h - U(18);
    rrect(s, id, x0, y0, x0 + w, y0 + h, h / 2, 0x2A2E35, 245);
    text(s, id + 1, msg, px, 0, s->w * 0.8f, s->w / 2.0f, y0 + (h - px) / 2, 1, FG, 255);
}
