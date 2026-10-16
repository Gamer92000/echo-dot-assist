/*
 * The light ring beyond stock's animations (ring.h).  What ledcontroller does, measured on a Dot 2 (biscuit, 2026-10-12,
 * the ring's frame read back from /sys/bus/i2c/devices/0-003f/frame; PLAN.md "Light ring"):
 *   - At its start it loads every file of /system/etc/led-resources (~1 s for stock's 266) and plays nothing else:
 *     a file changed later plays as it was.  So colours are a palette made beforehand, not written on demand.
 *   - Names must be in a layer config.  Every layer_config*.json there gets a compositor of its own, and two of them
 *     overwrite each other's frames, so ours go into stock's layer_config_common.json (the stock file kept beside it).
 *   - Layers compose per LED, black is transparent; in one layer only the first listed of the active ones shows, the
 *     others wait.  An animation whose loop is black (or that has none) finishes by itself; one with a lit loop shows
 *     until unset.
 *   - Animation lines are "<ms>:<pixel>,<pixel>,..." with 3- or 6-digit hex pixels; "loop" marks where the loop starts.
 *   - Stock turns only "active-start" and "active-end" towards the talker (com.amazon.ledd BeamDirection, kept from
 *     the front end's beam_index), and only while one animation is active (issue #22).  Copies turned by each beam
 *     (ours) keep the turn when more are active: checked against stock's own turn, beside a ringing timer.
 * Home Assistant's light lives in layer 0, under all of stock's, and is off while any pattern of the Echo's own shows
 * (they are told here, ring_set): the issue asked for the Echo's states first, and per-LED composition would mix the
 * light into the gaps of the listening ring or the volume steps.  Its colour is the nearest of the palette: 24 hues,
 * full or half saturation or white, at 3 levels, each as solid, pulse, spin and blink, and a rainbow at the 3 levels.
 * Themes (setting ring_theme): the Echo's listening, thinking and talking rings in other colours.  Made here from stock's own animations, as the turned copies are: stock's ring is
 * blue with a light-blue highlight (00F..0FF: blue full, green 0..F), so the green of each pixel is how much it is
 * highlight.  The theme's colour goes by LED, not by the frame, so it stays put while the highlight turns towards the
 * talker (turning the coloured frame would carry the rainbow round with the beam).  Red (mute), orange (setup), the
 * volume steps and the timers are stock's in every theme.
 */
#include "ring.h"
#include "hash.h"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HUES     24
#define LEVELS   3
#define MAX_LEDS 24
#define FORMAT   "hassmic ring 3"           /* in the digest: a change of the palette's layout changes it */
/* As seen: 100, 66 and 38 %.  The LED driver takes linear duty, so every channel goes through gamma 2.2 (colour_rgb):
 * 1, 0.4 and 0.12 in the file.  A dim level any lower vanishes under auto brightness in a dark room, where the ring
 * itself runs at a tenth */
static const float level_gain[LEVELS] = { 1.0f, 0.66f, 0.382f };
#define GAMMA 2.2f
enum { E_NONE, E_PULSE, E_SPIN, E_BLINK, E_RAINBOW, E_COUNT };
const char *const ring_effects[] = { "None", "Pulse", "Spin", "Blink", "Rainbow" };
const int ring_neffects = E_COUNT;
static const char *const effect_ids[] = { "solid", "pulse", "spin", "blink", "rainbow" };

/* The themes: the rainbow is hsv(i * 360 / n, 1, 0.75) per LED, the solid ones a listening
 * colour and a spinner's head and trail.  Written to the files as they are, without the light's gamma: the values a
 * test on the kitchen's Dot 2 approved (2026-10-16); through gamma 2.2 the rainbow's BF would be 87, visibly darker */
struct theme { int rainbow; unsigned char listen[3], head[3], trail[3]; };
static const struct theme themes[] = {
    { 0, { 0 }, { 0 }, { 0 } },                                         /* stock's own */
    { 1, { 0 }, { 0 }, { 0 } },
    { 0, { 0, 180, 0 }, { 0, 200, 0 }, { 0, 60, 0 } },
    { 0, { 80, 150, 200 }, { 150, 205, 255 }, { 25, 45, 70 } },
    { 0, { 110, 0, 45 }, { 210, 45, 0 }, { 55, 8, 0 } },                /* not pure red: that is the mute ring's */
};
#define NTHEMES  ((int)(sizeof themes / sizeof themes[0]))
const char *const ring_theme_ids[NTHEMES] = { "stock", "rainbow", "green", "sky", "crimson" };
const char *const ring_theme_names[NTHEMES] = { "Stock", "Rainbow", "Green", "Sky blue", "Crimson" };
const int ring_nthemes = NTHEMES;
#define THEME_V  0.75                       /* the rainbow below full, so yellow and cyan do not swamp the rest */
#define THEME_HL 150                        /* the highlight towards the talker: +150 on each channel */
#define THEME_TALK_FLOOR 0.3                /* the talking ring's darkest, as a share of the theme's colour */
#define THEME_SPIN_MS 80                    /* the spinner: one LED per 80 ms */

/* The system partition's directory, HASSMIC_LED_ROOT on the PC */
static const char *root(void) { const char *e = getenv("HASSMIC_LED_ROOT"); return e ? e : "/system/etc"; }
static void path(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", root(), rel); }

/* ---------------------------------------------------------------- text buffers */

struct buf { char *p; size_t n, cap; };
static void bput(struct buf *b, const char *fmt, ...)
{
    va_list ap; int k;
    for (;;) {
        va_start(ap, fmt); k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap); va_end(ap);
        if (k < 0) return;
        if (b->p && b->n + (size_t)k < b->cap) { b->n += k; return; }
        size_t cap = (b->cap ? b->cap * 2 : 4096) + k;
        char *p = realloc(b->p, cap);
        if (!p) return;
        b->p = p; b->cap = cap;
    }
}

/* the hot path: ~110 000 pixels per set, too many for printf on the Echo (0.6 s; this way 0.1 s) */
static void bcat(struct buf *b, const char *s, size_t k)
{
    if (b->n + k + 1 > b->cap) {
        size_t cap = (b->cap ? b->cap * 2 : 4096) + k;
        char *p = realloc(b->p, cap);
        if (!p) return;
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, s, k); b->n += k; b->p[b->n] = 0;
}

static char *slurp(const char *file)
{
    FILE *f = fopen(file, "r"); struct buf b = { 0 }; char chunk[4096]; size_t k;
    if (!f) return NULL;
    while ((k = fread(chunk, 1, sizeof chunk, f)) > 0) bput(&b, "%.*s", (int)k, chunk);
    fclose(f);
    if (!b.p) bput(&b, "%s", "");
    return b.p;
}

/* ---------------------------------------------------------------- the palette */

struct colour { int white, hue, soft, level; };

static void colour_rgb(const struct colour *c, float k, char px[8])
{
    float rgb[3] = { 1, 1, 1 };
    if (!c->white) {
        float h = c->hue * 6.0f / HUES, s = c->soft ? 0.5f : 1.0f, f = h - floorf(h);
        float p = 1 - s, q = 1 - s * f, t = 1 - s * (1 - f);
        switch ((int)h % 6) {
        case 0: rgb[0] = 1; rgb[1] = t; rgb[2] = p; break;
        case 1: rgb[0] = q; rgb[1] = 1; rgb[2] = p; break;
        case 2: rgb[0] = p; rgb[1] = 1; rgb[2] = t; break;
        case 3: rgb[0] = p; rgb[1] = q; rgb[2] = 1; break;
        case 4: rgb[0] = t; rgb[1] = p; rgb[2] = 1; break;
        default: rgb[0] = 1; rgb[1] = p; rgb[2] = q; break;
        }
    }
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 3; i++) {
        /* Gamma: Home Assistant's colours are as seen, the driver's duty is linear.  Half a channel unmapped is three
         * quarters as bright to the eye, and a pale colour came out nearly white (issue #26, coral 255,110,84).
         * Amazon's own colours are mapped so: its solid_orange is F30, its other orange ff6000; Home Assistant's
         * orange 255,165,0 maps to 255,98,0 */
        int v = (int)lroundf(powf(rgb[i] * level_gain[c->level] * k, GAMMA) * 255);
        if (v > 255) v = 255;
        px[2 * i] = hex[v >> 4]; px[2 * i + 1] = hex[v & 15];
    }
    px[6] = 0;
}

static void colour_code(const struct colour *c, char *out, size_t n)
{
    if (c->white) snprintf(out, n, "w%d", c->level);
    else snprintf(out, n, "%02d%c%d", c->hue, c->soft ? 's' : 'f', c->level);
}

static void light_pattern(int effect, const struct colour *c, char *out, size_t n)
{
    char code[8];
    if (effect == E_RAINBOW) { snprintf(out, n, "hassmic_rainbow_%d", c->level); return; }
    colour_code(c, code, sizeof code);
    snprintf(out, n, "hassmic_%s_%s", effect_ids[effect], code);
}

static void frame(struct buf *b, int ms, char px[][8], int n)
{
    char d[16]; int k = snprintf(d, sizeof d, "%d:", ms);
    bcat(b, d, k);
    for (int i = 0; i < n; i++) { if (i) bcat(b, ",", 1); bcat(b, px[i], strlen(px[i])); }
    bcat(b, "\n", 1);
}

/* One of Home Assistant's effects in one colour (as seen; colour_rgb maps to duty).  Pulse: 2.4 s from 35 % up and
 * down again.  Spin: two comets with short tails round the ring once a second, over the colour at 25 %, as stock's
 * thinking ring.  Blink: 0.5 s on and off. */
static void light_anim(struct buf *b, int effect, const struct colour *c, int n)
{
    char px[MAX_LEDS][8];
    bput(b, "loop\n");
    switch (effect) {
    case E_NONE:
        for (int i = 0; i < n; i++) colour_rgb(c, 1, px[i]);
        frame(b, 1000, px, n);
        break;
    case E_PULSE:
        for (int f = 0; f < 24; f++) {
            float k = 0.35f + 0.65f * (0.5f - 0.5f * cosf(2 * (float)M_PI * f / 24));
            for (int i = 0; i < n; i++) colour_rgb(c, k, px[i]);
            frame(b, 100, px, n);
        }
        break;
    case E_SPIN:
        for (int p = 0; p < n; p++) {
            for (int i = 0; i < n; i++) {
                int d1 = (p - i + n) % n, d2 = (p + n / 2 - i + n) % n, d = d1 < d2 ? d1 : d2;   /* behind a head */
                colour_rgb(c, d == 0 ? 1 : d == 1 ? 0.65f : d == 2 ? 0.42f : 0.25f, px[i]);
            }
            frame(b, 1000 / n, px, n);
        }
        break;
    case E_BLINK:
        for (int i = 0; i < n; i++) colour_rgb(c, 1, px[i]);
        frame(b, 500, px, n);
        for (int i = 0; i < n; i++) strcpy(px[i], "000000");
        frame(b, 500, px, n);
        break;
    case E_RAINBOW:                         /* the colour wheel round the ring, turning once in 2 s */
        for (int f = 0; f < HUES; f++) {
            for (int i = 0; i < n; i++) {
                struct colour w = { 0, (i * HUES / n + f) % HUES, 0, c->level };
                colour_rgb(&w, 1, px[i]);
            }
            frame(b, 2000 / HUES, px, n);
        }
        break;
    }
}

/* ---------------------------------------------------------------- stock's animations, turned */

/* Pixels of an animation line "<ms>:<p>,<p>,...": count, and where they start; 0 for other lines */
static int line_pixels(const char *l, const char **px)
{
    const char *c = l;
    if (!isdigit((unsigned char)*c)) return 0;
    while (isdigit((unsigned char)*c)) c++;
    if (*c != ':') return 0;
    *px = ++c;
    int n = 1;
    for (; *c && *c != '\n'; c++) if (*c == ',') n++;
    return n;
}

/* ---------------------------------------------------------------- themes */

/* A pixel of stock's listening, thinking or talking ring: how much it is highlight, 0..1 (its green: those rings are
 * 00F..0FF, measured on the Dot 2's), -1 black, -2 not of that scale (another model's stock: no themes there) */
static float stock_highlight(const char *px)
{
    size_t w = strlen(px); unsigned v[3];
    for (size_t i = 0; i < w; i++) if (!isxdigit((unsigned char)px[i])) return -2;
    if (w == 3) for (int i = 0; i < 3; i++) { char d[2] = { px[i], 0 }; v[i] = 17 * (unsigned)strtoul(d, NULL, 16); }
    else if (w == 6) for (int i = 0; i < 3; i++) { char d[3] = { px[2 * i], px[2 * i + 1], 0 }; v[i] = (unsigned)strtoul(d, NULL, 16); }
    else return -2;
    if (!v[0] && !v[1] && !v[2]) return -1;
    if (v[0] > 8 || v[2] < 0xC0) return -2;
    return v[1] / 255.0f;
}

static void rgb_px(const double c[3], char px[8])
{
    int v[3];
    for (int i = 0; i < 3; i++) { long x = lround(c[i]); v[i] = x < 0 ? 0 : x > 255 ? 255 : (int)x; }
    snprintf(px, 8, "%02X%02X%02X", v[0], v[1], v[2]);
}

/* The theme's colour of LED i of n.  The rainbow as Python's colorsys makes it, then floor (the values approved);
 * talking: the solid ones throb their spinner's head colour, the rainbow its wheel */
static void theme_base(const struct theme *th, int i, int n, int talking, double c[3])
{
    if (th->rainbow) {
        double h = (double)(i % n) / n * 6, f = h - floor(h), v = THEME_V, q = v * (1 - f), t = v * f;
        double rgb[6][3] = { { v, t, 0 }, { q, v, 0 }, { 0, v, t }, { 0, q, v }, { t, 0, v }, { v, 0, q } };
        for (int k = 0; k < 3; k++) c[k] = floor(rgb[(int)h % 6][k] * 255);
    } else {
        const unsigned char *s = talking ? th->head : th->listen;
        for (int k = 0; k < 3; k++) c[k] = s[k];
    }
}

/* stock's pixel on LED i in the theme: listening and end, the colour with the highlight brightened towards white by
 * up to THEME_HL; talking, the colour throbbing as stock's ring does.  -1: off scale */
static int theme_px(const struct theme *th, int i, int n, int talking, char px[8])
{
    float hl = stock_highlight(px); double c[3];
    if (hl == -2) return -1;
    if (hl < 0) { strcpy(px, "000000"); return 0; }
    theme_base(th, i, n, talking, c);
    for (int k = 0; k < 3; k++) c[k] = talking ? c[k] * (THEME_TALK_FLOOR + (1 - THEME_TALK_FLOOR) * hl) : c[k] + THEME_HL * hl;
    rgb_px(c, px);
    return 0;
}

/* Thinking, instead of stock's twinkle: the rainbow turning one LED per step, else the head and trail
 * of a spinner over a dark ring */
static void theme_think(struct buf *b, const struct theme *th, int n)
{
    char px[MAX_LEDS][8]; double c[3];
    bput(b, "loop\n");
    for (int pos = 0; pos < n; pos++) {
        for (int i = 0; i < n; i++) {
            if (th->rainbow) theme_base(th, ((i - pos) % n + n) % n, n, 0, c);
            else {
                const unsigned char *s = i == pos ? th->head : i == (pos + n - 1) % n ? th->trail : NULL;
                for (int k = 0; k < 3; k++) c[k] = s ? s[k] : 0;
            }
            rgb_px(c, px[i]);
        }
        frame(b, THEME_SPIN_MS, px, n);
    }
}

/* text turned by t LEDs: LED i shows what LED i - t did, as LedDevice turns stock's (PLAN.md, issue #22).  hold: only
 * the last frame, looping (for a beam that moves while the ring is already open).  th: in that theme's colours, by
 * the LED each pixel lands on (talking: as the talking ring) */
static int turned(struct buf *out, const char *text, int t, int hold, const struct theme *th, int talking)
{
    const char *l = text, *last = NULL; int n = 0;
    for (; *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : l + strlen(l)) {
        const char *px; int k = line_pixels(l, &px);
        if (k) { if (n && k != n) return -1; n = k; last = l; }
    }
    if (!n || n > MAX_LEDS) return -1;
    if (hold) bput(out, "loop\n");
    for (l = hold ? last : text; l && *l; ) {
        const char *e = strchr(l, '\n'); size_t len = e ? (size_t)(e - l) : strlen(l);
        const char *px; int k = line_pixels(l, &px);
        if (!k) { if (!hold) bput(out, "%.*s\n", (int)len, l); }
        else {
            char p[MAX_LEDS][8]; const char *c = px;
            for (int i = 0; i < n; i++) {
                size_t w = strcspn(c, ",\n\r");
                snprintf(p[(i + t) % n], sizeof p[0], "%.*s", (int)(w < 7 ? w : 7), c);
                c += w + (c[w] == ',');
            }
            if (th) for (int i = 0; i < n; i++) if (theme_px(th, i, n, talking, p[i])) return -1;
            frame(out, (int)strtol(l, NULL, 10), p, n);
        }
        if (hold || !e) break;
        l = e + 1;
    }
    return n;
}

static int leds_of(const char *text)
{
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        const char *px; int k = line_pixels(l, &px);
        if (k) return k <= MAX_LEDS ? k : 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- the layer config */

/* just after the "[" of layer `layer`, or NULL */
static const char *layer_open(const char *t, const char *layer)
{
    char key[8]; const char *p = t;
    snprintf(key, sizeof key, "\"%s\"", layer);
    while ((p = strstr(p, key))) {
        const char *q = p + strlen(key);
        while (isspace((unsigned char)*q)) q++;
        if (*q == ':') { q++; while (isspace((unsigned char)*q)) q++; if (*q == '[') return q + 1; }
        p++;
    }
    return NULL;
}

/* stock's "active-thinking" in layer 4: where the themes' thinking and talking rings go, just before it, so that they
 * wait behind what stock's waits behind (the short alarm and timer rings come before it there); NULL if not there */
static const char *thinking_at(const char *stock)
{
    const char *l4 = layer_open(stock, "4"), *e4 = l4 ? strchr(l4, ']') : NULL, *t = l4 ? strstr(l4, "\"active-thinking\"") : NULL;
    return t && e4 && t < e4 ? t : NULL;
}

/* stock's config with names4 first in layer 4 (stock's "active-start" is there; first: they never wait behind another
 * of that layer, as stock's turned pair waits behind none of what hassmic plays with it), names4t just before stock's
 * "active-thinking" (thinking_at) and names0 last in layer 0 (the light: under everything of stock's) */
static int merge_config(struct buf *out, const char *stock, const char *names4, const char *names4t, const char *names0)
{
    const char *l4 = layer_open(stock, "4"), *l0 = layer_open(stock, "0"), *t4 = *names4t ? thinking_at(stock) : NULL;
    if (!l4 || !l0 || strstr(stock, "hassmic_") || (*names4t && !t4)) return -1;
    const char *e0 = strchr(l0, ']');
    if (!e0) return -1;
    const char *last0 = e0;
    while (last0 > l0 && isspace((unsigned char)last0[-1])) last0--;
    const char *n4 = l4; while (isspace((unsigned char)*n4)) n4++;
    struct ins { const char *at; int kind; } ins[3] = { { l4, 4 }, { last0, 0 }, { t4, 5 } };
    int nins = t4 ? 3 : 2;
    for (int i = 0; i < nins; i++)                      /* in the order they come in the text */
        for (int j = i + 1; j < nins; j++)
            if (ins[j].at < ins[i].at) { struct ins x = ins[i]; ins[i] = ins[j]; ins[j] = x; }
    const char *p = stock;
    for (int i = 0; i < nins; i++) {
        bput(out, "%.*s", (int)(ins[i].at - p), p);
        if (ins[i].kind == 4) { if (*names4) bput(out, " %s%s", names4, *n4 == ']' ? "" : ","); }
        else if (ins[i].kind == 5) bput(out, "%s, ", names4t);
        else if (*names0) bput(out, "%s %s", last0 == l0 ? "" : ",", names0);
        p = ins[i].at;
    }
    bput(out, "%s", p);
    return 0;
}

/* ---------------------------------------------------------------- the set */

struct gen {
    const char *dir;                        /* NULL: only the digest */
    struct sha256 h;
    int files, failed, turned;              /* turned: the listening ring's copies are in it */
    int themed;                             /* the themes' rings are in it */
};
static int leds = 12;                       /* the ring's, from stock's listening ring (every model's has 12) */

static void emit(struct gen *g, const char *name, const struct buf *b)
{
    const char *p = b->p ? b->p : "";
    sha256_update(&g->h, name, strlen(name) + 1);
    sha256_update(&g->h, p, b->n + 1);
    g->files++;
    if (!g->dir) return;
    char file[512]; FILE *f;
    snprintf(file, sizeof file, "%s/%s", g->dir, name);
    if (!(f = fopen(file, "w")) || fwrite(p, 1, b->n, f) != b->n || fclose(f)) {
        if (f) fclose(f);
        fprintf(stderr, "ring: cannot write %s: %s\n", file, strerror(errno)); g->failed = 1;
    }
}

static void listnames(struct buf *b, const char *name) { bput(b, "%s\"%s\"", b->n ? ", " : "", name); }

/* "hassmic_theme_<id>_<kind>[_<turn>]": under 40 characters for every theme (struct shown, visible_ms) */
static void theme_pattern(char *out, size_t n, int th, const char *kind, int turn)
{
    if (turn >= 0) snprintf(out, n, "hassmic_theme_%s_%s_%d", ring_theme_ids[th], kind, turn);
    else snprintf(out, n, "hassmic_theme_%s_%s", ring_theme_ids[th], kind);
}

static const char *const kinds[] = { "listen", "hold", "end" };

/* Every theme's rings into the set: names4 gets their listening rings, names4t their thinking rings, then their
 * talking rings (stock's talking ring comes after its thinking one).  Made twice: first only to see that stock's rings
 * are of the scale the themes read (theme_px), then for real, so that a set never holds half a theme */
static int build_themes(struct gen *g, const char *start, const char *end, const char *talking, struct buf *names4,
                        struct buf *names4t, int n)
{
    struct buf b = { 0 }, think = { 0 }, talk = { 0 }; char name[64]; int ok = 1;
    for (int pass = 0; pass < 2 && ok; pass++)
        for (int th = 1; th < NTHEMES && ok; th++) {
            for (int k = 0; k < 3 && ok; k++)
                for (int t = 0; t < n && ok; t++) {
                    b.n = 0;
                    if (turned(&b, k == 2 ? end : start, t, k == 1, &themes[th], 0) != n) { ok = 0; break; }
                    if (!pass) continue;
                    theme_pattern(name, sizeof name, th, kinds[k], t);
                    listnames(names4, name);
                    strcat(name, ".animation"); emit(g, name, &b);
                }
            b.n = 0;
            if (ok && turned(&b, talking, 0, 0, &themes[th], 1) != n) ok = 0;
            if (!ok || !pass) continue;
            theme_pattern(name, sizeof name, th, "talk", -1);
            listnames(&talk, name);
            strcat(name, ".animation"); emit(g, name, &b);
            b.n = 0; theme_think(&b, &themes[th], n);
            theme_pattern(name, sizeof name, th, "think", -1);
            listnames(&think, name);
            strcat(name, ".animation"); emit(g, name, &b);
        }
    if (ok) bput(names4t, "%s, %s", think.p, talk.p);
    free(b.p); free(think.p); free(talk.p);
    return ok;
}

static int build(struct gen *g)
{
    char file[512], name[64];
    struct buf names4 = { 0 }, names4t = { 0 }, names0 = { 0 }, b = { 0 };
    sha256_init(&g->h);
    sha256_update(&g->h, FORMAT, sizeof FORMAT);
    path(file, sizeof file, "led-resources/active-start.animation");
    char *start = slurp(file);
    path(file, sizeof file, "led-resources/active-end.animation");
    char *end = slurp(file);
    path(file, sizeof file, "led-resources/active-talking.animation");
    char *talking = slurp(file);
    /* stock's config: the copy kept from before our first install, else the one in place (still stock's then) */
    path(file, sizeof file, "layer_config_common.json.pre-hassmic");
    char *stock = slurp(file);
    if (!stock) { path(file, sizeof file, "led-resources/layer_config_common.json"); stock = slurp(file); }
    int n = start ? leds_of(start) : 0;
    if (!n) n = 12;
    leds = n;
    g->turned = g->themed = 0;
    if (start && end && leds_of(end) == n) {
        int ok = 1;
        for (int k = 0; k < 3 && ok; k++)
            for (int t = 0; t < n && ok; t++) {
                b.n = 0;
                if (turned(&b, k == 2 ? end : start, t, k == 1, NULL, 0) != n) { ok = 0; break; }
                snprintf(name, sizeof name, "hassmic_%s_%d", kinds[k], t);
                listnames(&names4, name);
                strcat(name, ".animation"); emit(g, name, &b);
            }
        g->turned = ok;
        if (!ok) { fprintf(stderr, "ring: stock's listening ring has a shape we do not know: not turned\n"); names4.n = 0; }
        else if (talking && leds_of(talking) == n && stock && thinking_at(stock)) {
            g->themed = build_themes(g, start, end, talking, &names4, &names4t, n);
            if (!g->themed) fprintf(stderr, "ring: stock's rings are not the blue we know: no themes\n");
        }
    }
    for (int e = 0; e < E_COUNT; e++)
        for (int w = 0; w <= (e == E_RAINBOW ? 0 : 1); w++)
            for (int hue = 0; hue < (w || e == E_RAINBOW ? 1 : HUES); hue++)
                for (int soft = 0; soft <= (w || e == E_RAINBOW ? 0 : 1); soft++)
                    for (int lv = 0; lv < LEVELS; lv++) {
                        struct colour c = { w, hue, soft, lv };
                        b.n = 0; light_anim(&b, e, &c, n);
                        light_pattern(e, &c, name, sizeof name);
                        listnames(&names0, name);
                        strcat(name, ".animation"); emit(g, name, &b);
                    }
    b.n = 0;
    int rc = !stock || merge_config(&b, stock, names4.p ? names4.p : "", g->themed && names4t.p ? names4t.p : "",
                                    names0.p ? names0.p : "") ? -1 : 0;
    if (rc) fprintf(stderr, "ring: no layer config of stock's to add ours to (%s)\n", file);
    else emit(g, "layer_config_common.json", &b);
    free(start); free(end); free(talking); free(stock); free(names4.p); free(names4t.p); free(names0.p); free(b.p);
    return rc || g->failed ? -1 : 0;
}

static void digest_hex(struct gen *g, char hex[65])
{
    uint8_t d[32];
    sha256_final(&g->h, d);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
}

int ring_generate(const char *dir)
{
    struct gen g; char hex[65], file[512]; FILE *f;
    memset(&g, 0, sizeof g); g.dir = strcmp(dir, "-") ? dir : NULL;
    if (build(&g)) return 1;
    digest_hex(&g, hex);
    if (!g.dir) { printf("%s\n", hex); return 0; }       /* "-": the digest only, for root to compare */
    snprintf(file, sizeof file, "%s/digest", dir);
    if (!(f = fopen(file, "w")) || fprintf(f, "%s\n", hex) < 0 || fclose(f)) { fprintf(stderr, "ring: cannot write %s\n", file); return 1; }
    fprintf(stderr, "ring: %d files, digest %.16s%s\n", g.files, hex, g.turned ? "" : " (listening ring not turned)");
    return 0;
}

/* ---------------------------------------------------------------- at run time */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void (*run_fn)(const char *op, const char *name);
static int custom, have_turned, have_themes, theme;   /* theme: index in themes, 0 stock's */

/* The Echo's own patterns that hassmic played and did not stop: how long each shows, from its file */
#define SHOWN_MAX 24
/* ms < 0: until unset; th: the theme it shows in (a theme picked meanwhile shows from the next pattern on) */
static struct shown { char logical[40], actual[40]; long long at; int ms, turn, th; } shown[SHOWN_MAX];
static int nshown;
static struct ring_light light = { 0, 1, 1, 1, 1, 1, E_NONE };
static struct ring_light flash; static long long flash_until;
static char light_shown[48];                /* what of ours is on the ring for the light, "" none */

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void run(const char *op, const char *name) { if (run_fn) run_fn(op, name); }

/* How long a pattern shows once played: until unset (-1) when its loop has light in it, else to the end of its last
 * lit frame (ledcontroller lets it finish there).  Read once per pattern; 1 s when there is no file to read */
static int visible_ms(const char *name)
{
    static struct { char name[40]; int ms; } cache[48]; static int ncache;
    for (int i = 0; i < ncache; i++) if (!strcmp(cache[i].name, name)) return cache[i].ms;
    char file[512]; path(file, sizeof file, "led-resources/");
    snprintf(file + strlen(file), sizeof file - strlen(file), "%s.animation", name);
    char *t = slurp(file); int ms = 1000;
    if (t) {
        int at = 0, lit_end = 0, looping = 0, loop_lit = 0;
        for (const char *l = t; *l; ) {
            const char *e = strchr(l, '\n'), *px; size_t len = e ? (size_t)(e - l) : strlen(l);
            if (!strncmp(l, "loop", 4)) looping = 1;
            else if (line_pixels(l, &px)) {
                int lit = 0;
                for (const char *c = px; c < l + len; c++) if (isxdigit((unsigned char)*c) && *c != '0') lit = 1;
                at += atoi(l);
                if (lit && looping) loop_lit = 1;
                if (lit && !looping) lit_end = at;
            }
            if (!e) break;
            l = e + 1;
        }
        ms = loop_lit ? -1 : lit_end;
        free(t);
    }
    if (ncache < 48) { snprintf(cache[ncache].name, sizeof cache[0].name, "%s", name); cache[ncache++].ms = ms; }
    return ms;
}

/* The beam the front end last reported, as ledcontroller keeps it (com.amazon.ledd BeamDirection: the same handler as
 * the beam_index event), read through liblipc in-process: lipc-get-prop costs a fork and ~75 ms.  -1 unknown */
static int beam_now(void)
{
    static void *h; static int (*get)(void *, const char *, const char *, int *); static int tried;
    const char *fake = getenv("HASSMIC_FAKE_BEAM");
    if (fake) { FILE *f = fopen(fake, "r"); int v = -1; if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); } return v; }
    if (!tried) {
        tried = 1;
        void *lib = dlopen("liblipc.so", RTLD_NOW);
        void *(*open_)(void) = lib ? (void *(*)(void))dlsym(lib, "LipcOpenNoName") : NULL;
        get = lib ? (int (*)(void *, const char *, const char *, int *))dlsym(lib, "LipcGetIntProperty") : NULL;
        if (open_ && get) h = open_();
        if (!h) fprintf(stderr, "ring: no LIPC, the listening ring is stock's (turned only while nothing else shows)\n");
    }
    int v;
    if (!h || get(h, "com.amazon.ledd", "BeamDirection", &v) != 0 || v < 0) return -1;
    return v;
}

static int busy(long long now)
{
    for (int i = 0; i < nshown; i++) if (shown[i].ms < 0 || now < shown[i].at + shown[i].ms) return 1;
    return 0;
}

static struct shown *find(const char *logical)
{
    for (int i = 0; i < nshown; i++) if (!strcmp(shown[i].logical, logical)) return &shown[i];
    return NULL;
}

/* Remembered across a restart of hassmic: ledcontroller keeps playing what was left on */
static void light_remember(void)
{
    char file[300]; const char *d = getenv("HASSMIC_STATE");
    snprintf(file, sizeof file, "%s/ring-light", d ? d : "/data/local/hassmic/state");
    if (!light_shown[0]) { unlink(file); return; }
    FILE *f = fopen(file, "w");
    if (f) { fprintf(f, "%s\n", light_shown); fclose(f); }
}

static void quantize(const struct ring_light *l, struct colour *c)
{
    float r = l->r, g = l->g, b = l->b, mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
    float v = l->brightness * l->color_brightness * mx, s = mx > 0 ? (mx - mn) / mx : 0, h = 0;
    if (mx > mn) {
        if (mx == r) h = fmodf((g - b) / (mx - mn) + 6, 6);
        else if (mx == g) h = (b - r) / (mx - mn) + 2;
        else h = (r - g) / (mx - mn) + 4;
    }
    c->white = s < 0.2f;
    c->soft = s < 0.7f;
    c->hue = (int)lroundf(h * HUES / 6) % HUES;
    c->level = v >= 0.75f ? 0 : v >= 0.4f ? 1 : 2;
}

/* lock held: the light on the ring as it should be now */
static void light_apply(long long now)
{
    char want[48] = "";
    int flashing = flash_until && now < flash_until;
    if (!flashing) flash_until = 0;
    const struct ring_light *l = flashing ? &flash : &light;
    if (custom && (l->on || flashing) && !busy(now)) {
        struct colour c; quantize(l, &c);
        light_pattern(flashing ? E_BLINK : l->effect, &c, want, sizeof want);
    }
    if (!strcmp(want, light_shown)) return;
    if (want[0]) fprintf(stderr, "light ring: Home Assistant's light %s%s\n", want, flashing ? " (flash)" : "");
    else fprintf(stderr, "light ring: Home Assistant's light %s\n", l->on || flashing ? "waits for the Echo's own" : "off");
    if (want[0]) run("-s", want);           /* the new first: the ring is never dark between two colours */
    if (light_shown[0]) run("-u", light_shown);
    snprintf(light_shown, sizeof light_shown, "%s", want);
    light_remember();
}

void ring_start(void (*fn)(const char *op, const char *name))
{
    struct gen g; char hex[65], file[512], *inst;
    memset(&g, 0, sizeof g);
    run_fn = fn;
    path(file, sizeof file, "hassmic-leds.digest");
    inst = slurp(file);
    if (!build(&g)) {
        digest_hex(&g, hex);
        custom = inst && !strncmp(inst, hex, 64);
        have_turned = custom && g.turned;
        have_themes = have_turned && g.themed;
    }
    if (custom) fprintf(stderr, "ring: our animations are installed (%d files)%s%s\n", g.files, have_turned ? "" : ", the listening ring not turned",
                        have_themes ? ", themes" : "");
    else fprintf(stderr, "ring: %s: stock's animations only, no light in Home Assistant\n",
                 inst ? "the installed animations are another build's" : "our animations are not installed");
    free(inst);
    if (have_turned) {                      /* LIPC opened now: a failure is in the log at the start, not at a wake word */
        pthread_mutex_lock(&lock); int b = beam_now(); pthread_mutex_unlock(&lock);
        if (b >= 0) fprintf(stderr, "ring: the front end's beam is %d\n", b);
    }
    const char *d = getenv("HASSMIC_STATE"); char left[48] = ""; FILE *f;   /* this light from before a restart */
    snprintf(file, sizeof file, "%s/ring-light", d ? d : "/data/local/hassmic/state");
    if ((f = fopen(file, "r"))) { if (fscanf(f, "%47s", left) == 1 && !strncmp(left, "hassmic_", 8)) run("-u", left); fclose(f); unlink(file); }
}

int ring_custom(void) { return custom; }
int ring_themes(void) { return have_themes; }

int ring_theme(int v)
{
    pthread_mutex_lock(&lock);
    if (v >= 0 && v < NTHEMES) theme = v;
    int r = theme;
    pthread_mutex_unlock(&lock);
    return r;
}

void ring_set(const char *pattern)
{
    char actual[40]; int turn = -1, th;
    int ring = !strcmp(pattern, "active-start") || !strcmp(pattern, "active-end");
    snprintf(actual, sizeof actual, "%s", pattern);
    pthread_mutex_lock(&lock);
    th = have_themes ? theme : 0;
    if (have_turned && ring && (turn = beam_now()) >= 0) turn %= leds;
    if (th && ring) {                       /* beam not known yet: at 0, ring_tick turns it once it is */
        if (turn < 0) turn = 0;
        theme_pattern(actual, sizeof actual, th, pattern[7] == 's' ? "listen" : "end", turn);
    }
    else if (th && !strcmp(pattern, "active-thinking")) theme_pattern(actual, sizeof actual, th, "think", -1);
    else if (th && !strcmp(pattern, "active-talking")) theme_pattern(actual, sizeof actual, th, "talk", -1);
    else if (turn >= 0) snprintf(actual, sizeof actual, "hassmic_%s_%d", pattern[7] == 's' ? "listen" : "end", turn);
    struct shown *s = find(pattern);
    if (s && strcmp(s->actual, actual)) run("-u", s->actual);
    if (!s && nshown < SHOWN_MAX) s = &shown[nshown++];
    if (s) {
        snprintf(s->logical, sizeof s->logical, "%s", pattern); snprintf(s->actual, sizeof s->actual, "%s", actual);
        s->at = now_ms(); s->ms = visible_ms(pattern); s->turn = turn; s->th = th;
    }
    light_apply(now_ms());                  /* the light goes first: never mixed into the Echo's */
    run("-s", actual);
    pthread_mutex_unlock(&lock);
}

void ring_unset(const char *pattern)
{
    pthread_mutex_lock(&lock);
    struct shown *s = find(pattern);
    run("-u", s ? s->actual : pattern);
    if (s) *s = shown[--nshown];
    pthread_mutex_unlock(&lock);
}

void ring_tick(void)
{
    long long now = now_ms();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nshown; i++)        /* finished ones: ledcontroller let them go itself */
        if (shown[i].ms >= 0 && now >= shown[i].at + shown[i].ms) shown[i--] = shown[--nshown];
    /* The talker moved while the ring is open (or the front end reported the beam after the wake word): the open ring
     * at the new turn, without its opening again */
    struct shown *s = find("active-start");
    if (s && s->turn >= 0) {
        int b = beam_now();
        if (b >= 0 && b % leds != s->turn) {
            char was[40]; snprintf(was, sizeof was, "%s", s->actual);
            s->turn = b % leds;
            if (s->th) theme_pattern(s->actual, sizeof s->actual, s->th, "hold", s->turn);
            else snprintf(s->actual, sizeof s->actual, "hassmic_hold_%d", s->turn);
            run("-s", s->actual); run("-u", was);
        }
    }
    light_apply(now);
    pthread_mutex_unlock(&lock);
}

void ring_light_get(struct ring_light *l) { pthread_mutex_lock(&lock); *l = light; pthread_mutex_unlock(&lock); }

void ring_light_set(const struct ring_light *l, unsigned flash_ms)
{
    pthread_mutex_lock(&lock);
    if (flash_ms) { flash = *l; flash.on = 1; flash_until = now_ms() + flash_ms; }
    else { light = *l; flash_until = 0; }
    if (light.effect < 0 || light.effect >= E_COUNT) light.effect = E_NONE;
    long long now = now_ms();
    light_apply(now);
    if (custom && (light.on || flash_until) && !light_shown[0])
        for (int i = 0; i < nshown; i++)
            if (shown[i].ms < 0 || now < shown[i].at + shown[i].ms) {
                fprintf(stderr, "light ring: Home Assistant's light waits for the Echo's own (%s)\n", shown[i].logical); break;
            }
    pthread_mutex_unlock(&lock);
}
