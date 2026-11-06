/*
 * The text layer's picture (textdraw.h): what was said and the reply, as subtitles above the light band.
 *
 * Fonts: stock's own screens are in Amazon Ember (/system/fonts/Amazon-Ember-*.ttf on the Echo Show 5 2nd gen's 8149, as
 * Fire OS has it), Roboto is Android's and on every model.  Rasterised with stb_truetype (src/third_party, public
 * domain): no FreeType on the device for us (the firmware's libft2 is the framework's, without a stable ABI), and the
 * text changes a few times a request, so drawing it here and sending pixels costs less than teaching ringbar.dex
 * Android's text layout through reflection.  Only on a change: fading is ring_band.c's, by the alpha of these pixels.
 *
 * Layout: the reply (white, Medium) as large as fits, from 12 % of the height above the band down to 6.5 %, the heard
 * text above it in 62 % of that size but never under 5.5 % (grey, Regular, three lines at most), the block in the
 * middle, a cyan bar (the band's listening colour) beside the reply; what does not fit at the smallest size ends in
 * "…".  The first try on an Echo Show 5 (2026-11-03), a strip of 55 % at the bottom with the reply at 15 % of it and
 * four lines at most, was small and cut short on a screen that was black above it.  Under the words a shade of 75 %
 * black over the whole layer, so they stay readable over whatever the screen shows.
 */
#include "textdraw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb_truetype.h"
#pragma GCC diagnostic pop

#define MAX_LINES    12
#define HEARD_LINES  3

static stbtt_fontinfo face[2];                  /* 0 Regular (heard), 1 Medium (reply) */
static unsigned char *data[2];
static int ready;

static unsigned char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = n > 0 && n < 32 << 20 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static int open_face(int k, const char *const *paths)
{
    for (int i = 0; paths[i]; i++) {
        unsigned char *b = slurp(paths[i]);
        if (!b) continue;
        int off = stbtt_GetFontOffsetForIndex(b, 0);
        if (off >= 0 && stbtt_InitFont(&face[k], b, off)) {
            data[k] = b;
            fprintf(stderr, "text: font %s\n", paths[i]);
            return 0;
        }
        free(b);
    }
    return -1;
}

int textdraw_init(void)
{
    if (ready) return ready > 0 ? 0 : -1;
    /* the PC: HASSMIC_FONTS for a firmware's font folder, HASSMIC_FONT for one file in every place */
    const char *env = getenv("HASSMIC_FONT"), *dir = getenv("HASSMIC_FONTS");
    if (!dir) dir = "/system/fonts";
    char p[4][512];
    const char *names[4] = { "Amazon-Ember-Regular.ttf", "Roboto-Regular.ttf", "Amazon-Ember-Medium.ttf", "Roboto-Medium.ttf" };
    for (int i = 0; i < 4; i++) snprintf(p[i], sizeof p[i], "%s/%s", dir, names[i]);
    const char *regular[] = { env, p[0], p[1], NULL };
    const char *medium[] = { env, p[2], p[3], p[0], p[1], NULL };
    const char *const *r = env ? regular : regular + 1, *const *m = env ? medium : medium + 1;
    if (open_face(0, r) < 0) { fprintf(stderr, "text: no font, nothing is drawn\n"); ready = -1; return -1; }
    if (open_face(1, m) < 0) { face[1] = face[0]; data[1] = NULL; }
    ready = 1;
    return 0;
}

/* the next code point of UTF-8 at *s (a broken byte counts as itself) */
static int next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int first = *p++, n = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : first >= 0xc0 ? 1 : 0, c = n ? first & (0x3f >> n) : first;
    for (int i = 0; i < n; i++, p++) {
        if ((*p & 0xc0) != 0x80) { *s += 1; return first; }    /* not UTF-8 after all: the byte alone */
        c = c << 6 | (*p & 0x3f);
    }
    *s = (const char *)p;
    return c;
}

/* width in pixels of [a, b) at scale, kerning included */
static float width(const stbtt_fontinfo *f, float scale, const char *a, const char *b)
{
    float x = 0; int prev = 0;
    for (const char *p = a; p < b; ) {
        int c = next_cp(&p), adv, lsb;
        if (prev) x += scale * stbtt_GetCodepointKernAdvance(f, prev, c);
        stbtt_GetCodepointHMetrics(f, c, &adv, &lsb);
        x += scale * adv; prev = c;
    }
    return x;
}

struct line { const char *a, *b; };

/* text wrapped to maxw: lines stored up to MAX_LINES, all of them counted */
static int wrap(const stbtt_fontinfo *f, float scale, const char *text, float maxw, struct line *out)
{
    int n = 0;
    const char *p = text;
    for (;;) {
        while (*p == ' ' || *p == '\n') p++;
        if (!*p) break;
        const char *start = p, *q = p, *space = NULL, *end = NULL;
        while (*q && *q != '\n') {
            const char *nx = q; int c = next_cp(&nx);
            if (c == ' ') space = q;
            if (width(f, scale, start, nx) > maxw) {
                if (space && space > start) { end = space; p = space + 1; }
                else { end = q > start ? q : nx; p = end; }      /* a word longer than the line: broken where it ends */
                break;
            }
            q = nx;
        }
        if (!end) { end = q; p = q; }
        while (end > start && end[-1] == ' ') end--;
        if (n < MAX_LINES) out[n] = (struct line){ start, end };
        n++;
    }
    return n;
}

/* px: draw one line at (x, baseline y) in colour rgb, over what is there (not premultiplied) */
static void draw_line(uint32_t *px, int w, int h, const stbtt_fontinfo *f, float scale, float x, int y,
                      const char *a, const char *b, uint32_t rgb)
{
    static unsigned char glyph[256 * 256];
    float cr = (rgb >> 16 & 255), cg = (rgb >> 8 & 255), cb = (rgb & 255);
    int prev = 0;
    for (const char *p = a; p < b; ) {
        int c = next_cp(&p), adv, lsb, x0, y0, x1, y1;
        if (prev) x += scale * stbtt_GetCodepointKernAdvance(f, prev, c);
        stbtt_GetCodepointHMetrics(f, c, &adv, &lsb);
        float sx = x - floorf(x);
        stbtt_GetCodepointBitmapBoxSubpixel(f, c, scale, scale, sx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw <= 256 && gh <= 256) {
            stbtt_MakeCodepointBitmapSubpixel(f, glyph, gw, gh, gw, scale, scale, sx, 0, c);
            int ox = (int)floorf(x) + x0, oy = y + y0;
            for (int j = 0; j < gh; j++) {
                int yy = oy + j;
                if (yy < 0 || yy >= h) continue;
                for (int i = 0; i < gw; i++) {
                    int xx = ox + i;
                    if (xx < 0 || xx >= w) continue;
                    float k = glyph[j * gw + i] / 255.f;
                    if (k <= 0) continue;
                    uint32_t d = px[(size_t)yy * w + xx];
                    float da = (d >> 24) / 255.f, oa = k + da * (1 - k);
                    float r = (cr * k + (d >> 16 & 255) * da * (1 - k)) / oa;
                    float g = (cg * k + (d >> 8 & 255) * da * (1 - k)) / oa;
                    float bl = (cb * k + (d & 255) * da * (1 - k)) / oa;
                    px[(size_t)yy * w + xx] = (uint32_t)lroundf(oa * 255) << 24 | (uint32_t)lroundf(r) << 16 |
                                              (uint32_t)lroundf(g) << 8 | (uint32_t)lroundf(bl);
                }
            }
        }
        x += scale * adv; prev = c;
    }
}

/* the line cut so that it and "…" fit maxw; the cut text is written into buf */
static struct line ellipsis(const stbtt_fontinfo *f, float scale, struct line l, float maxw, char *buf, size_t cap)
{
    const char *dots = stbtt_FindGlyphIndex(f, 0x2026) ? "\xe2\x80\xa6" : "...";
    float dw = width(f, scale, dots, dots + strlen(dots));
    const char *e = l.b;
    while (e > l.a && width(f, scale, l.a, e) + dw > maxw) {
        e--;
        while (e > l.a && (*e & 0xc0) == 0x80) e--;         /* whole code points */
    }
    while (e > l.a && e[-1] == ' ') e--;
    snprintf(buf, cap, "%.*s%s", (int)(e - l.a), l.a, dots);
    return (struct line){ buf, buf + strlen(buf) };
}

void textdraw_render(uint32_t *px, int w, int h, int keep, const char *heard, const char *reply)
{
    memset(px, 0, (size_t)w * h * 4);
    if (!heard) heard = "";
    if (!reply) reply = "";
    if (ready <= 0 || (!*heard && !*reply) || w < 64 || h - keep < 32) return;
    int avail = h - keep;
    float margin = avail * 0.08f, side = w * 0.05f, bar = fmaxf(3, w * 0.004f), gap = bar * 3;
    float x = side + bar + gap, maxw = w - x - side;
    struct line hl[MAX_LINES], rl[MAX_LINES];
    int nh = 0, nr = 0, hshown = 0, rshown = 0;
    float rs = 0, hs = 0, sr = 0, sh = 0, block = 0;
    /* the largest reply size whose block fits: the heard lines (three at most) + a gap + every reply line */
    for (float size = avail * 0.12f; ; size *= 0.93f) {
        int last = size < avail * 0.065f;
        if (last) size = avail * 0.065f;
        rs = size; hs = fmaxf(size * 0.62f, avail * 0.055f);
        sr = stbtt_ScaleForPixelHeight(&face[1], rs); sh = stbtt_ScaleForPixelHeight(&face[0], hs);
        nh = *heard ? wrap(&face[0], sh, heard, maxw, hl) : 0;
        nr = *reply ? wrap(&face[1], sr, reply, maxw, rl) : 0;
        hshown = nh < HEARD_LINES ? nh : HEARD_LINES;
        float room = avail - 2 * margin - (hshown ? hshown * hs * 1.25f + (nr ? hs * 0.6f : 0) : 0);
        rshown = (int)(room / (rs * 1.2f));
        if (rshown > nr) rshown = nr;
        if (rshown > MAX_LINES) rshown = MAX_LINES;
        if (rshown >= nr || last) break;
    }
    if (rshown < 1 && nr) rshown = 1;
    block = (hshown ? hshown * hs * 1.25f + (nr ? hs * 0.6f : 0) : 0) + rshown * rs * 1.2f;
    float top = margin + (avail - 2 * margin - block) / 2;  /* in the middle */
    if (top < margin) top = margin;
    for (int y = 0; y < avail; y++)                 /* the shade, down to the band */
        for (int i = 0; i < w; i++) px[(size_t)y * w + i] = 0xbfu << 24;
    char cut[1024];
    float y = top;
    int asc, desc, gapl;
    stbtt_GetFontVMetrics(&face[0], &asc, &desc, &gapl);
    for (int i = 0; i < hshown; i++) {
        struct line l = hl[i];
        if (i == hshown - 1 && nh > hshown) l = ellipsis(&face[0], sh, l, maxw, cut, sizeof cut);
        draw_line(px, w, h, &face[0], sh, x, (int)lroundf(y + asc * sh), l.a, l.b, 0xb4bcc8);
        y += hs * 1.25f;
    }
    if (hshown && nr) y += hs * 0.6f;
    stbtt_GetFontVMetrics(&face[1], &asc, &desc, &gapl);
    float r0 = y;
    for (int i = 0; i < rshown; i++) {
        struct line l = rl[i];
        if (i == rshown - 1 && nr > rshown) l = ellipsis(&face[1], sr, l, maxw, cut, sizeof cut);
        draw_line(px, w, h, &face[1], sr, x, (int)lroundf(y + asc * sr), l.a, l.b, 0xffffff);
        y += rs * 1.2f;
    }
    if (rshown) {                                   /* the cyan bar beside the reply, its ends rounded by the alpha */
        int bx0 = (int)side, bx1 = (int)(side + bar), by0 = (int)(r0 + rs * 0.1f), by1 = (int)(y - rs * 0.2f);
        for (int yy = by0; yy < by1 && yy < avail; yy++)
            for (int xx = bx0; xx < bx1 && xx < w; xx++) px[(size_t)yy * w + xx] = 0xff00caffu;
    }
}

/* ---------------------------------------------------------------- timers */

static void put(uint32_t *px, int w, int x0, int y0, int x1, int y1, uint32_t argb)
{
    for (int y = y0 < 0 ? 0 : y0; y < y1; y++) for (int x = x0 < 0 ? 0 : x0; x < x1 && x < w; x++) px[(size_t)y * w + x] = argb;
}

/* the time left and its unit, SI's symbols (the same in every language): "1:02:05" h, "4:59" min, under a minute the
 * seconds alone ("45" s: "0:45 min" reads slowly); ringing "0:00" and no unit */
static const char *clock_text(char *b, size_t cap, int s, int ringing)
{
    if (s < 0) s = 0;
    if (ringing) { snprintf(b, cap, "0:00"); return ""; }
    if (s >= 3600) { snprintf(b, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60); return "h"; }
    if (s >= 60) { snprintf(b, cap, "%d:%02d", s / 60, s % 60); return "min"; }
    snprintf(b, cap, "%d", s); return "s";
}

/* a line centred, with a smaller unit after it on the same baseline (unit "" for none): placed by the width of the
 * same text with every digit a 0, so a running clock keeps still */
static void centred(uint32_t *px, int w, int h, const stbtt_fontinfo *f, float scale, int y, const char *s, uint32_t rgb,
                    const char *unit, float uscale, uint32_t urgb)
{
    char z[96]; size_t n = 0;
    for (const char *p = s; *p && n + 1 < sizeof z; p++) z[n++] = *p >= '0' && *p <= '9' ? '0' : *p;
    z[n] = 0;
    float tw = width(f, scale, z, z + n), gap = unit[0] ? width(f, scale, z, z + 1) * 0.35f : 0;     /* a third of a digit */
    float uw = unit[0] ? width(&face[0], uscale, unit, unit + strlen(unit)) : 0, x = (w - tw - gap - uw) / 2;
    draw_line(px, w, h, f, scale, x, y, s, s + strlen(s), rgb);
    if (unit[0]) draw_line(px, w, h, &face[0], uscale, x + tw + gap, y, unit, unit + strlen(unit), urgb);
}

void textdraw_timers(uint32_t *px, int w, int h, int keep, const struct textdraw_timer *t, int n, int ringing)
{
    memset(px, 0, (size_t)w * h * 4);
    if (ready <= 0 || n <= 0 || w < 64 || h - keep < 32) return;
    if (n > 3) n = 3;                               /* the three that end first: more is not read off a screen */
    int avail = h - keep;
    float margin = avail * 0.08f, room = (avail - 2 * margin) / n;
    float ts = fminf(room * 0.50f, avail * 0.30f), ns = fmaxf(ts * 0.22f, avail * 0.05f);      /* time, name */
    float bh = fmaxf(4, avail * 0.012f), bw = w * 0.5f;
    float gap = n > 1 ? ns * 1.2f : 0, block = ns * 1.4f + ts * 1.05f + bh * 2 + gap;   /* a gap between timers */
    float top = margin + (avail - 2 * margin - block * n + gap) / 2;
    float st = stbtt_ScaleForPixelHeight(&face[1], ts), sn = stbtt_ScaleForPixelHeight(&face[0], ns);
    int at, dt, gt, an, dn, gn;
    stbtt_GetFontVMetrics(&face[1], &at, &dt, &gt); stbtt_GetFontVMetrics(&face[0], &an, &dn, &gn);
    put(px, w, 0, 0, w, avail, 0xbfu << 24);         /* the shade, as under words */
    for (int i = 0; i < n; i++) {
        float y = top + i * block;
        char clk[32]; const char *unit = clock_text(clk, sizeof clk, t[i].left, ringing);
        uint32_t col = ringing ? 0xff9f0a : t[i].active ? 0xffffff : 0x8a919c;     /* orange, white, paused grey */
        if (t[i].name && t[i].name[0]) centred(px, w, h, &face[0], sn, (int)lroundf(y + an * sn), t[i].name, 0xb4bcc8, "", 0, 0);
        y += ns * 1.4f;
        centred(px, w, h, &face[1], st, (int)lroundf(y + at * st), clk, col, unit, stbtt_ScaleForPixelHeight(&face[0], ts * 0.38f),
                t[i].active ? 0xb4bcc8 : 0x8a919c);
        y += ts * 1.05f + bh;
        float k = ringing || t[i].total <= 0 ? 1 : (float)t[i].left / t[i].total;
        if (k < 0) k = 0;
        if (k > 1) k = 1;
        int x0 = (int)((w - bw) / 2), y0 = (int)y, y1 = (int)(y + bh);
        put(px, w, x0, y0, (int)(x0 + bw), y1, 0xe0404650u);                     /* the track, on the shade */
        put(px, w, x0, y0, (int)(x0 + bw * k), y1, 0xff000000u | (ringing ? 0xff9f0au : t[i].active ? 0x00caffu : 0x8a919cu));
    }
}
