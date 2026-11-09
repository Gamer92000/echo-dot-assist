/* See gfx.h.  Measured on the Echo Show 5 (MT8163, 2026-10-14): a full 960x470 fill 3.8 ms, a 200x200 disc with its
 * distance computed for every pixel 0.48 ms; the buffer is write-combined, so every pixel is written once, in order. */
#include "gfx.h"
#include <math.h>
#include <string.h>

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

uint32_t gfx_mix(uint32_t a, uint32_t b, float k)
{
    k = clampf(k, 0, 1);
    int r = (int)((a >> 16 & 255) + ((int)(b >> 16 & 255) - (int)(a >> 16 & 255)) * k + 0.5f);
    int g = (int)((a >> 8 & 255) + ((int)(b >> 8 & 255) - (int)(a >> 8 & 255)) * k + 0.5f);
    int bl = (int)((a & 255) + ((int)(b & 255) - (int)(a & 255)) * k + 0.5f);
    return (uint32_t)clampi(r, 0, 255) << 16 | (uint32_t)clampi(g, 0, 255) << 8 | (uint32_t)clampi(bl, 0, 255);
}

uint32_t gfx_scale(uint32_t rgb, float k) { return gfx_mix(0, rgb, k); }

struct rect rect_and(struct rect a, struct rect b)
{
    struct rect r = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };
    if (rect_empty(r)) r = (struct rect){ 0, 0, 0, 0 };
    return r;
}

struct rect rect_or(struct rect a, struct rect b)
{
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    return (struct rect){ a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
}

/* one pixel (0xXXBBGGRR) towards a colour (the same layout) by a of 256 */
static inline uint32_t blend(uint32_t d, uint32_t s, unsigned a)
{
    unsigned ia = 256 - a;                      /* two channels per multiply, 16 bits apart: no carry between them */
    uint32_t rb = ((d & 0x00FF00FF) * ia + (s & 0x00FF00FF) * a) >> 8 & 0x00FF00FF;
    uint32_t g = ((d & 0x0000FF00) * ia + (s & 0x0000FF00) * a) >> 8 & 0x0000FF00;
    return 0xFF000000u | rb | g;
}

void gfx_fill(struct gfx *g, struct rect r, uint32_t rgb)
{
    r = rect_and(r, g->clip);
    uint32_t c = gfx_px(rgb);
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *p = g->px + (size_t)y * g->stride + r.x0;
        for (int x = r.x0; x < r.x1; x++) *p++ = c;
    }
}

/* Rounded box by its signed distance (Inigo Quilez's sdRoundBox): negative inside.  A pixel's coverage is
 * 0.5 - distance at its centre, clamped: exact for straight edges, within a few percent on curves of 3 px and up. */
void gfx_rrect(struct gfx *g, float x0, float y0, float x1, float y1, float rad, uint32_t rgb, int alpha)
{
    if (x1 <= x0 || y1 <= y0 || alpha <= 0) return;
    float hw = (x1 - x0) / 2, hh = (y1 - y0) / 2, cx = x0 + hw, cy = y0 + hh;
    rad = clampf(rad, 0, hw < hh ? hw : hh);
    struct rect r = rect_and((struct rect){ (int)floorf(x0), (int)floorf(y0), (int)ceilf(x1), (int)ceilf(y1) }, g->clip);
    uint32_t c = gfx_px(rgb); unsigned A = (unsigned)clampi(alpha, 0, 255); A += A >> 7;      /* 0..256 */
    float ix = hw - rad, iy = hh - rad;                          /* the inner box the corners round */
    for (int y = r.y0; y < r.y1; y++) {
        float py = y + 0.5f - cy, qy = fabsf(py) - iy;
        uint32_t *row = g->px + (size_t)y * g->stride;
        /* where this row is wholly covered (distance <= -0.5): pixel centres within `span` of the middle */
        float span = -1;
        if (qy <= 0) { if (fabsf(py) <= hh - 0.5f) span = hw - 0.5f; }
        else { float e = (rad - 0.5f) * (rad - 0.5f) - qy * qy; if (e > 0 && rad > 0.5f) span = ix + sqrtf(e); }
        int fx0 = span >= 0 ? (int)ceilf(cx - span - 0.5f) : r.x1, fx1 = span >= 0 ? (int)floorf(cx + span - 0.5f) + 1 : r.x1;
        if (fx0 < r.x0) fx0 = r.x0;
        if (fx1 > r.x1) fx1 = r.x1;
        if (fx1 <= fx0) fx0 = fx1 = r.x1;
        for (int x = r.x0; x < r.x1; x++) {
            if (x == fx0) {                                      /* the certain inside: one fill */
                if (A >= 256) for (; x < fx1; x++) row[x] = c;
                else for (; x < fx1; x++) row[x] = blend(row[x], c, A);
                if (x >= r.x1) break;
            }
            float qx = fabsf(x + 0.5f - cx) - ix;
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - rad;
            float k = 0.5f - d;
            if (k <= 0) continue;
            unsigned a = k >= 1 ? A : (unsigned)(k * A);
            row[x] = a >= 256 ? c : blend(row[x], c, a);
        }
    }
}

/* Convex polygon: a pixel's coverage from its distance to the nearest edge (the inside of every edge's half-plane),
 * which smooths the corners a little.  Small glyphs only (buttons), so per pixel. */
void gfx_poly(struct gfx *g, const float *xy, int n, uint32_t rgb, int alpha)
{
    if (n < 3 || alpha <= 0) return;
    float a[16], b[16], cc[16], mnx = xy[0], mxx = xy[0], mny = xy[1], mxy = xy[1], area = 0;
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        float x = xy[2 * i], y = xy[2 * i + 1], nx = xy[2 * ((i + 1) % n)], ny = xy[2 * ((i + 1) % n) + 1];
        area += x * ny - nx * y;
        mnx = fminf(mnx, x); mxx = fmaxf(mxx, x); mny = fminf(mny, y); mxy = fmaxf(mxy, y);
    }
    float s = area > 0 ? 1 : -1;
    for (int i = 0; i < n; i++) {                                 /* inward unit normals: a x + b y + c >= 0 inside */
        float x = xy[2 * i], y = xy[2 * i + 1], nx = xy[2 * ((i + 1) % n)], ny = xy[2 * ((i + 1) % n) + 1];
        float dx = nx - x, dy = ny - y, l = sqrtf(dx * dx + dy * dy);
        if (l <= 0) l = 1;
        a[i] = -dy / l * s; b[i] = dx / l * s; cc[i] = -(a[i] * x + b[i] * y);
    }
    struct rect r = rect_and((struct rect){ (int)floorf(mnx) - 1, (int)floorf(mny) - 1, (int)ceilf(mxx) + 1, (int)ceilf(mxy) + 1 }, g->clip);
    uint32_t c = gfx_px(rgb); unsigned A = (unsigned)clampi(alpha, 0, 255); A += A >> 7;
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *row = g->px + (size_t)y * g->stride;
        for (int x = r.x0; x < r.x1; x++) {
            float px = x + 0.5f, py = y + 0.5f, d = 1e9f;
            for (int i = 0; i < n; i++) { float e = a[i] * px + b[i] * py + cc[i]; if (e < d) d = e; }
            float k = d + 0.5f;
            if (k <= 0) continue;
            unsigned al = k >= 1 ? A : (unsigned)(k * A);
            row[x] = al >= 256 ? c : blend(row[x], c, al);
        }
    }
}

void gfx_mask(struct gfx *g, int x, int y, const uint8_t *m, int w, int h, int stride, uint32_t rgb, int alpha)
{
    struct rect r = rect_and((struct rect){ x, y, x + w, y + h }, g->clip);
    uint32_t c = gfx_px(rgb); unsigned A = (unsigned)clampi(alpha, 0, 255); A += A >> 7;
    for (int yy = r.y0; yy < r.y1; yy++) {
        const uint8_t *mr = m + (size_t)(yy - y) * stride - x;
        uint32_t *row = g->px + (size_t)yy * g->stride;
        for (int xx = r.x0; xx < r.x1; xx++) {
            unsigned k = mr[xx];
            if (!k) continue;
            k = (k + (k >> 7)) * A >> 8;
            row[xx] = k >= 256 ? c : blend(row[xx], c, k);
        }
    }
}

void gfx_image(struct gfx *g, int x, int y, const uint32_t *src, int w, int h, int stride, int premul, int alpha)
{
    struct rect r = rect_and((struct rect){ x, y, x + w, y + h }, g->clip);
    unsigned A = (unsigned)clampi(alpha, 0, 255); A += A >> 7;
    for (int yy = r.y0; yy < r.y1; yy++) {
        const uint32_t *sr = src + (size_t)(yy - y) * stride - x;
        uint32_t *row = g->px + (size_t)yy * g->stride;
        for (int xx = r.x0; xx < r.x1; xx++) {
            uint32_t s = sr[xx];
            if (!premul) { row[xx] = A >= 256 ? (s | 0xFF000000u) : blend(row[xx], s, A); continue; }
            unsigned sa = s >> 24;                                /* premultiplied: d * (1 - sa) + s */
            if (!sa) continue;
            if (A >= 256 && sa == 255) { row[xx] = s | 0xFF000000u; continue; }
            uint32_t d = row[xx];
            unsigned k = A * (sa + (sa >> 7)) >> 8, inv = 256 - k;
            uint32_t rb = ((d & 0x00FF00FF) * inv >> 8 & 0x00FF00FF) + ((s & 0x00FF00FF) * A >> 8 & 0x00FF00FF);
            uint32_t gg = ((d & 0x0000FF00) * inv >> 8 & 0x0000FF00) + ((s & 0x0000FF00) * A >> 8 & 0x0000FF00);
            row[xx] = 0xFF000000u | (rb & 0x00FF00FF) | (gg & 0x0000FF00);
        }
    }
}

void gfx_image_scaled(struct gfx *g, int x, int y, int dw, int dh, const uint32_t *src, int w, int h, int stride, float rad)
{
    if (dw <= 0 || dh <= 0 || w <= 0 || h <= 0) return;
    struct rect r = rect_and((struct rect){ x, y, x + dw, y + dh }, g->clip);
    if (dw == w && dh == h) {                                   /* at its own size (the lists' images): rows copied, */
        float hw = dw / 2.0f, hh = dh / 2.0f, ix = hw - rad, iy = hh - rad;   /* the corners' coverage alone computed */
        for (int yy = r.y0; yy < r.y1; yy++) {
            const uint32_t *sr = src + (size_t)(yy - y) * stride;
            uint32_t *row = g->px + (size_t)yy * g->stride;
            float qy = fabsf(yy - y + 0.5f - hh) - iy;
            int c0 = r.x0, c1 = r.x1;                           /* outside the corner columns: plain copies */
            if (rad > 0 && qy > 0) { c0 = clampi(x + (int)ceilf(rad), r.x0, r.x1); c1 = clampi(x + dw - (int)ceilf(rad), c0, r.x1); }
            for (int xx = r.x0; xx < r.x1; xx++) {
                if (xx == c0) { for (; xx < c1; xx++) row[xx] = sr[xx - x] | 0xFF000000u; if (xx >= r.x1) break; }
                uint32_t p = sr[xx - x] | 0xFF000000u;
                if (rad > 0 && qy > 0) {
                    float qx = fabsf(xx - x + 0.5f - hw) - ix;
                    if (qx > 0) {
                        float k = 0.5f - (sqrtf(qx * qx + qy * qy) - rad);
                        if (k <= 0) continue;
                        if (k < 1) { row[xx] = blend(row[xx], p, (unsigned)(k * 256)); continue; }
                    }
                }
                row[xx] = p;
            }
        }
        return;
    }
    float sx = (float)w / dw, sy = (float)h / dh;
    float hw = dw / 2.0f, hh = dh / 2.0f, ix = hw - rad, iy = hh - rad;
    for (int yy = r.y0; yy < r.y1; yy++) {
        float fy = (yy - y + 0.5f) * sy - 0.5f; int y0 = (int)floorf(fy); float ty = fy - y0;
        int ya = clampi(y0, 0, h - 1), yb = clampi(y0 + 1, 0, h - 1);
        unsigned wy = (unsigned)(ty * 256);
        const uint32_t *ra = src + (size_t)ya * stride, *rb_ = src + (size_t)yb * stride;
        uint32_t *row = g->px + (size_t)yy * g->stride;
        float qy = fabsf(yy - y + 0.5f - hh) - iy;
        for (int xx = r.x0; xx < r.x1; xx++) {
            float fx = (xx - x + 0.5f) * sx - 0.5f; int x0 = (int)floorf(fx); float tx = fx - x0;
            int xa = clampi(x0, 0, w - 1), xb = clampi(x0 + 1, 0, w - 1);
            unsigned wx = (unsigned)(tx * 256);
            uint32_t top = blend(ra[xa], ra[xb], wx), bot = blend(rb_[xa], rb_[xb], wx), s = blend(top, bot, wy);
            if (rad > 0) {
                float qx = fabsf(xx - x + 0.5f - hw) - ix;
                if (qx > 0 && qy > 0) {
                    float k = 0.5f - (sqrtf(qx * qx + qy * qy) - rad);
                    if (k <= 0) continue;
                    if (k < 1) { row[xx] = blend(row[xx], s, (unsigned)(k * 256)); continue; }
                }
            }
            row[xx] = s;
        }
    }
}

/* every channel of premultiplied pixels a..b by k of 256, alpha too */
static inline uint32_t lerp4(uint32_t a, uint32_t b, unsigned k)
{
    unsigned ik = 256 - k;
    uint32_t rb = ((a & 0x00FF00FF) * ik + (b & 0x00FF00FF) * k) >> 8 & 0x00FF00FF;
    uint32_t ag = ((a >> 8 & 0x00FF00FF) * ik + (b >> 8 & 0x00FF00FF) * k) & 0xFF00FF00;
    return rb | ag;
}

void gfx_icon_scaled(struct gfx *g, int x, int y, int dw, int dh, const uint32_t *src, int w, int h, int stride)
{
    if (dw <= 0 || dh <= 0 || w <= 0 || h <= 0) return;
    struct rect r = rect_and((struct rect){ x, y, x + dw, y + dh }, g->clip);
    float sx = (float)w / dw, sy = (float)h / dh;
    for (int yy = r.y0; yy < r.y1; yy++) {
        float fy = (yy - y + 0.5f) * sy - 0.5f; int y0 = (int)floorf(fy);
        int ya = clampi(y0, 0, h - 1), yb = clampi(y0 + 1, 0, h - 1); unsigned wy = (unsigned)((fy - y0) * 256);
        const uint32_t *ra = src + (size_t)ya * stride, *rb_ = src + (size_t)yb * stride;
        uint32_t *row = g->px + (size_t)yy * g->stride;
        for (int xx = r.x0; xx < r.x1; xx++) {
            float fx = (xx - x + 0.5f) * sx - 0.5f; int x0 = (int)floorf(fx);
            int xa = clampi(x0, 0, w - 1), xb = clampi(x0 + 1, 0, w - 1); unsigned wx = (unsigned)((fx - x0) * 256);
            uint32_t p = lerp4(lerp4(ra[xa], ra[xb], wx), lerp4(rb_[xa], rb_[xb], wx), wy);
            unsigned sa = p >> 24;
            if (!sa) continue;
            if (sa == 255) { row[xx] = p | 0xFF000000u; continue; }
            uint32_t d = row[xx]; unsigned inv = 256 - (sa + (sa >> 7));
            uint32_t rb = ((d & 0x00FF00FF) * inv >> 8 & 0x00FF00FF) + (p & 0x00FF00FF);
            uint32_t gg = ((d & 0x0000FF00) * inv >> 8 & 0x0000FF00) + (p & 0x0000FF00);
            row[xx] = 0xFF000000u | (rb & 0x00FF00FF) | (gg & 0x0000FF00);
        }
    }
}

void gfx_vgrad(struct gfx *g, struct rect r, uint32_t top, uint32_t bottom)
{
    struct rect c = rect_and(r, g->clip);
    float hgt = (float)(r.y1 - r.y0 > 1 ? r.y1 - r.y0 - 1 : 1);
    for (int y = c.y0; y < c.y1; y++) {
        uint32_t px = gfx_px(gfx_mix(top, bottom, (y - r.y0) / hgt));
        uint32_t *p = g->px + (size_t)y * g->stride + c.x0;
        for (int x = c.x0; x < c.x1; x++) *p++ = px;
    }
}

/* the same distance as gfx_rrect's; covered where it is within t of the edge, inside */
void gfx_rrect_stroke(struct gfx *g, float x0, float y0, float x1, float y1, float rad, float t, uint32_t rgb, int alpha)
{
    if (x1 <= x0 || y1 <= y0 || alpha <= 0 || t <= 0) return;
    float hw = (x1 - x0) / 2, hh = (y1 - y0) / 2, cx = x0 + hw, cy = y0 + hh;
    rad = clampf(rad, 0, hw < hh ? hw : hh);
    struct rect r = rect_and((struct rect){ (int)floorf(x0), (int)floorf(y0), (int)ceilf(x1), (int)ceilf(y1) }, g->clip);
    uint32_t c = gfx_px(rgb); unsigned A = (unsigned)clampi(alpha, 0, 255); A += A >> 7;
    float ix = hw - rad, iy = hh - rad;
    for (int y = r.y0; y < r.y1; y++) {
        float qy = fabsf(y + 0.5f - cy) - iy;
        uint32_t *row = g->px + (size_t)y * g->stride;
        int inner = qy < -t - 1;                                 /* rows where only the sides are near an edge */
        for (int x = r.x0; x < r.x1; x++) {
            float qx = fabsf(x + 0.5f - cx) - ix;
            if (inner && qx < -t - 1) { x = (int)floorf(cx + ix - t - 1); if (x < r.x0) x = r.x0; continue; }
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - rad;   /* < 0 inside */
            float k = fminf(0.5f - d, d + t + 0.5f);
            if (k <= 0) continue;
            unsigned a = k >= 1 ? A : (unsigned)(k * A);
            row[x] = a >= 256 ? c : blend(row[x], c, a);
        }
    }
}

void gfx_arc(struct gfx *g, float cx, float cy, float r, float t, float a0, float a1, uint32_t rgb, int alpha)
{
    if (alpha <= 0 || t <= 0 || r <= 0) return;
    const float TAU = 6.2831853f;
    int whole = a1 - a0 >= TAU - 1e-4f;
    float e0x = cx + r * cosf(a0), e0y = cy + r * sinf(a0), e1x = cx + r * cosf(a1), e1y = cy + r * sinf(a1), ht = t / 2;
    float mid = (a0 + a1) / 2, half = (a1 - a0) / 2, mx = cosf(mid), my = sinf(mid), ch = cosf(half);
    struct rect b = rect_and((struct rect){ (int)floorf(cx - r - ht) - 1, (int)floorf(cy - r - ht) - 1,
                                            (int)ceilf(cx + r + ht) + 1, (int)ceilf(cy + r + ht) + 1 }, g->clip);
    uint32_t c = gfx_px(rgb); unsigned A = (unsigned)clampi(alpha, 0, 255); A += A >> 7;
    for (int y = b.y0; y < b.y1; y++) {
        uint32_t *row = g->px + (size_t)y * g->stride;
        for (int x = b.x0; x < b.x1; x++) {
            float px = x + 0.5f - cx, py = y + 0.5f - cy, l = sqrtf(px * px + py * py), d;
            /* inside the arc's angle: the cosine to its middle at least that of its half */
            if (whole || (l > 0 && (px * mx + py * my) / l >= ch)) d = fabsf(l - r) - ht;
            else {                                               /* outside: the nearer round end */
                float dx0 = x + 0.5f - e0x, dy0 = y + 0.5f - e0y, dx1 = x + 0.5f - e1x, dy1 = y + 0.5f - e1y;
                d = sqrtf(fminf(dx0 * dx0 + dy0 * dy0, dx1 * dx1 + dy1 * dy1)) - ht;
            }
            float k = 0.5f - d;
            if (k <= 0) continue;
            unsigned a = k >= 1 ? A : (unsigned)(k * A);
            row[x] = a >= 256 ? c : blend(row[x], c, a);
        }
    }
}
