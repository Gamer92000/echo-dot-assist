/*
 * The light ring beyond stock's animations (ring.h).  What ledcontroller does, measured on a Dot 2 (biscuit, 2026-10-12,
 * the ring's frame read back from /sys/bus/i2c/devices/0-003f/frame; PLAN.md "Light ring"):
 *   - At its start it loads every file of /system/etc/led-resources (~1 s for stock's 266) and plays nothing else:
 *     a file changed later plays as it was.  So colours are a palette made beforehand, not written on demand.
 *   - Names must be in a layer config.  Every layer_config*.json there gets a compositor of its own, and two of them
 *     overwrite each other's frames, so ours go into stock's layer_config_common.json (the stock file kept beside it).
 *   - Layers compose per LED, black is transparent; in one layer only the first listed of the active ones shows, the
 *     others wait.  An animation whose loop is black finishes by itself; one with a lit loop shows until unset, and so
 *     does one without "loop": it plays again from the start (stock's talking ring, 3.4 s, lasts a reply of any length;
 *     the call rings and zzz_* have none either).
 *   - Animation lines are "<ms>:<pixel>,<pixel>,..." with 3- or 6-digit hex pixels; "loop" marks where the loop starts.
 *   - Stock turns only "active-start" and "active-end" towards the talker (com.amazon.ledd BeamDirection, kept from
 *     the front end's beam_index), and only while one animation is active (issue #22).  Copies turned by each beam
 *     (ours) keep the turn when more are active: checked against stock's own turn, beside a ringing timer.
 * Home Assistant's light lives in layer 0, under all of stock's, and is off while any pattern of the Echo's own shows
 * (they are told here, ring_set): the issue asked for the Echo's states first, and per-LED composition would mix the
 * light into the gaps of the listening ring or the volume steps.  Its colour is the nearest of the palette: 24 hues,
 * full or half saturation or white, at 3 levels, each as solid, pulse, spin and blink, and a rainbow at the 3 levels.
 * Each LED also has a layer of its own under stock's (issue #57: a fill, the ring LED by LED): ledcontroller mixes
 * the layers per LED by brightness (a pixel weighs its brightest channel / 255, top first, what is left goes below), so
 * black lets the layer below through and nothing dark covers it: a part of the ring lit takes patterns that light
 * just that part.  One pattern per LED and colour of the palette (solid), each LED in a layer of its own (0..11),
 * stock's moved up above them (12..17).  9.7 MB of the system partition: where it has no room for them, root installs
 * the set without them (HASSMIC_RING_LEDS=0) and there is no fill (ring_per_led).
 * Stock's white volume steps once more in cyan, for the Assistant's own volume (separate volumes, main.c), just before
 * stock's in their layer.
 * Designs (ringdesign.h, issue #38): what people drew on the settings page in place of stock's patterns, one slot per
 * pattern the core plays.  Each goes into the layer config just before the stock name it replaces, so it waits behind
 * and covers what stock's would; listening's and its end's turned copies go first in layer 4 with stock's turned ones.
 * Colours are as seen, through the light's gamma.  Made from state/ring-applied, which only changes when the page
 * applies (root then installs the set again and restarts ledcontroller): a design edited meanwhile does not change it.
 */
#include "ring.h"
#include "ringdesign.h"
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
#define MAX_LEDS RING_LEDS_MAX
#define FORMAT   "hassmic ring 4"           /* in the digest: a change of the palette's layout changes it */
/* As seen: 100, 66 and 38 %.  The LED driver takes linear duty, so every channel goes through gamma 2.2 (colour_rgb):
 * 1, 0.4 and 0.12 in the file.  A dim level any lower vanishes under auto brightness in a dark room, where the ring
 * itself runs at a tenth */
static const float level_gain[LEVELS] = { 1.0f, 0.66f, 0.382f };
#define GAMMA 2.2f
enum { E_NONE, E_PULSE, E_SPIN, E_BLINK, E_RAINBOW, E_COUNT };
const char *const ring_effects[] = { "None", "Pulse", "Spin", "Blink", "Rainbow" };
const int ring_neffects = E_COUNT;
static const char *const effect_ids[] = { "solid", "pulse", "spin", "blink", "rainbow" };

/* Every pattern main.c plays (led()), by what it shows.  The ids are the settings page's and state/ring-applied's: keep
 * them.  setup-mode, factory-reset and authenticated_setup_mode are in layer 3/5 like stock's, so they still cover
 * what they covered */
const struct ring_slot ring_slots[] = {
    { "listen", "active-start", RING_LISTEN }, { "end", "active-end", RING_END },
    { "think", "active-thinking", RING_PLAIN }, { "talk", "active-talking", RING_PLAIN },
    { "error", "anim_start_error_short", RING_PLAIN },
    { "timer", "ready-timer", RING_PLAIN }, { "timer_short", "ready-timer-short", RING_PLAIN },
    { "volume", "volume_step-", RING_VOLUME }, { "assist_volume", "hassmic_assist_", RING_ASSIST },
    { "mute", "mics-off_on", RING_PLAIN }, { "unmute", "mics-off_end", RING_PLAIN },
    { "dnd", "do_not_disturb", RING_PLAIN }, { "identify", "zzz_rainbow", RING_PLAIN },
    { "bt_pairing", "scone-setup", RING_PLAIN },
    { "call_out", "call_outbound_ringing", RING_PLAIN }, { "call_in", "call_incoming", RING_PLAIN },
    { "call_live", "call_connected", RING_PLAIN }, { "call_end", "call_connected_end", RING_PLAIN },
    { "setup", "setup-mode", RING_PLAIN }, { "reset", "factory-reset", RING_PLAIN },
    { "login", "authenticated_setup_mode", RING_PLAIN },
};
const int ring_nslots = (int)(sizeof ring_slots / sizeof ring_slots[0]);
enum { S_LISTEN, S_END };                 /* ring_slots' first two */
_Static_assert(sizeof ring_slots / sizeof ring_slots[0] <= RING_SLOTS_MAX, "RING_SLOTS_MAX");
/* What the designs may add to the system partition: the light's set is ~0.75 MB (9.7 MB in 4 kB blocks), stock's
 * 1.9 MB; a design for listening is there 36 times (each turn, opening, held, closing) */
#define DESIGN_BYTES_MAX (3 << 20)

/* The system partition's directory, HASSMIC_LED_ROOT on the PC */
static const char *root(void) { const char *e = getenv("HASSMIC_LED_ROOT"); return e ? e : "/system/etc"; }
static void path(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", root(), rel); }

/* ---------------------------------------------------------------- text buffers */

struct buf { char *p; size_t n, cap; int bad; };     /* bad: memory ran out, the text is not whole */
static void bput(struct buf *b, const char *fmt, ...)
{
    va_list ap; int k;
    for (;;) {
        va_start(ap, fmt); k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap); va_end(ap);
        if (k < 0) { b->bad = 1; return; }
        if (b->p && b->n + (size_t)k < b->cap) { b->n += k; return; }
        size_t cap = (b->cap ? b->cap * 2 : 4096) + k;
        char *p = realloc(b->p, cap);
        if (!p) { b->bad = 1; return; }
        b->p = p; b->cap = cap;
    }
}

/* the hot path: ~110 000 pixels per set, too many for printf on the Echo (0.6 s; this way 0.1 s) */
static void bcat(struct buf *b, const char *s, size_t k)
{
    if (b->n + k + 1 > b->cap) {
        size_t cap = (b->cap ? b->cap * 2 : 4096) + k;
        char *p = realloc(b->p, cap);
        if (!p) { b->bad = 1; return; }       /* nothing written past what there is */
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
    if (b.bad) { free(b.p); return NULL; }
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

/* The palette's colours: 24 hues full and half saturated, and white, at each level; 147 */
#define NCOLOURS ((HUES * 2 + 1) * LEVELS)
static struct colour colour_at(int i)
{
    struct colour c = { 0, 0, 0, i % LEVELS };
    i /= LEVELS;
    if (i == HUES * 2) c.white = 1; else { c.hue = i / 2; c.soft = i % 2; }
    return c;
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

/* text turned by t LEDs: LED i shows what LED i - t did, as LedDevice turns stock's (PLAN.md, issue #22).  hold: only
 * the last frame, looping (for a beam that moves while the ring is already open) */
static int turned(struct buf *out, const char *text, int t, int hold)
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

/* the opening quote of layer `layer`'s key, and just after its "[" in *open; NULL */
static const char *layer_key(const char *t, const char *layer, const char **open)
{
    char key[8]; const char *p = t;
    snprintf(key, sizeof key, "\"%s\"", layer);
    while ((p = strstr(p, key))) {
        const char *q = p + strlen(key);
        while (isspace((unsigned char)*q)) q++;
        if (*q == ':') { q++; while (isspace((unsigned char)*q)) q++; if (*q == '[') { if (open) *open = q + 1; return p; } }
        p++;
    }
    return NULL;
}

/* just after the "[" of layer `layer`, or NULL */
static const char *layer_open(const char *t, const char *layer)
{
    const char *open;
    return layer_key(t, layer, &open) ? open : NULL;
}

/* T (stock's config with ours added) with stock's layers moved up by N and N layers under them, layer i the names of
 * LED i's patterns (perled[i]).  ledcontroller wants numberOfLayers and every layer below it as an array */
static int relayer(struct buf *out, const char *t, int n, const struct buf *perled)
{
    struct edit { const char *at, *end; int kind, k; } ed[2 + 100], tmp;
    int ne = 0, layers;
    const char *nl = strstr(t, "\"numberOfLayers\""), *lay = strstr(t, "\"Layers\""), *v, *brace;
    char *e;
    if (!nl || !lay || !(v = strchr(nl, ':')) || !(brace = strchr(lay, '{'))) return -1;
    for (v++; isspace((unsigned char)*v); v++) {}
    layers = (int)strtol(v, &e, 10);
    if (e == v || layers < 1 || layers > 99) return -1;
    ed[ne++] = (struct edit){ v, e, 'n', 0 };
    ed[ne++] = (struct edit){ brace + 1, brace + 1, '{', 0 };
    for (int k = 0; k < layers; k++) {
        char key[8]; const char *at;
        snprintf(key, sizeof key, "%d", k);
        if (!(at = layer_key(t, key, NULL))) return -1;
        ed[ne++] = (struct edit){ at, at + strlen(key) + 2, 'k', k };
    }
    for (int i = 1; i < ne; i++)
        for (int j = i; j > 0 && ed[j].at < ed[j - 1].at; j--) { tmp = ed[j]; ed[j] = ed[j - 1]; ed[j - 1] = tmp; }
    const char *p = t;
    for (int i = 0; i < ne; i++) {
        bput(out, "%.*s", (int)(ed[i].at - p), p);
        if (ed[i].kind == 'n') bput(out, "%d", layers + n);
        else if (ed[i].kind == 'k') bput(out, "\"%d\"", ed[i].k + n);
        else for (int l = 0; l < n; l++) bput(out, "\n    \"%d\": [%s],", l, perled[l].p ? perled[l].p : "");
        p = ed[i].end;
    }
    bput(out, "%s", p);
    return 0;
}

/* stock's config with names4 first in layer 4 (stock's "active-start" is there; first: they never wait behind another
 * of that layer, as stock's turned pair waits behind none of what hassmic plays with it), names0 last in layer 0 (the
 * light: under everything of stock's) and namesv just before stock's first volume step (the Assistant's steps show
 * as stock's would) */
/* where stock's config lists a pattern ("name" quoted), NULL if nowhere */
static const char *listed_at(const char *stock, const char *name)
{
    char q[64]; snprintf(q, sizeof q, "\"%s\"", name);
    return strstr(stock, q);
}

/* ... and each design for a pattern of stock's just before that name (before[i]: names, at[i]: where): it waits behind
 * what stock's waited behind, and covers what stock's covered */
static int merge_config(struct buf *out, const char *stock, const char *names4, const char *names0, const char *namesv,
                        const char *const *at, const char *const *before, int nbefore)
{
    const char *l4 = layer_open(stock, "4"), *l0 = layer_open(stock, "0"), *v1 = strstr(stock, "\"volume_step-");
    if (!l4 || !l0 || strstr(stock, "hassmic_")) return -1;
    const char *e0 = strchr(l0, ']');
    if (!e0) return -1;
    const char *last0 = e0;
    while (last0 > l0 && isspace((unsigned char)last0[-1])) last0--;
    const char *n4 = l4; while (isspace((unsigned char)*n4)) n4++;
    struct ins { const char *at, *names; int kind; } ins[3 + RING_SLOTS_MAX], t;
    int k = 0;
    ins[k++] = (struct ins){ l4, names4, 4 }; ins[k++] = (struct ins){ last0, names0, 0 };
    if (*namesv && v1) ins[k++] = (struct ins){ v1, namesv, 'v' };
    for (int i = 0; i < nbefore && k < (int)(sizeof ins / sizeof ins[0]); i++) ins[k++] = (struct ins){ at[i], before[i], 'v' };
    for (int i = 1; i < k; i++)             /* in the order they come in the file, the same place in the order given */
        for (int j = i; j > 0 && ins[j].at < ins[j - 1].at; j--) { t = ins[j]; ins[j] = ins[j - 1]; ins[j - 1] = t; }
    const char *p = stock;
    for (int i = 0; i < k; i++) {
        bput(out, "%.*s", (int)(ins[i].at - p), p);
        if (ins[i].kind == 4) { if (*names4) bput(out, " %s%s", names4, *n4 == ']' ? "" : ","); }
        else if (ins[i].kind == 'v') bput(out, "%s, ", ins[i].names);
        else if (*names0) bput(out, "%s %s", last0 == l0 ? "" : ",", names0);
        p = ins[i].at;
    }
    bput(out, "%s", p);
    return 0;
}

/* stock's animation with every lit LED in cyan, as bright as it was (its brightest channel) */
static void cyan(struct buf *out, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const char *l = text; *l; ) {
        const char *e = strchr(l, '\n'), *px; size_t len = e ? (size_t)(e - l) : strlen(l);
        if (!line_pixels(l, &px)) bput(out, "%.*s\n", (int)len, l);
        else {
            bput(out, "%.*s", (int)(px - l), l);
            for (const char *c = px; c < l + len; ) {
                size_t w = strcspn(c, ",\n\r"); int v = 0;
                for (int ch = 0; ch < 3; ch++) {        /* 3 digits: one per channel, 6: two */
                    char d[3] = { 0 };
                    if (w == 3) d[0] = d[1] = c[ch]; else if (w == 6) memcpy(d, c + 2 * ch, 2);
                    int x = (int)strtol(d, NULL, 16);
                    if (x > v) v = x;
                }
                bput(out, "%s00%c%c%c%c", c == px ? "" : ",", hex[v >> 4], hex[v & 15], hex[v >> 4], hex[v & 15]);
                c += w; if (*c == ',') c++; else break;
            }
            bput(out, "\n");
        }
        if (!e) break;
        l = e + 1;
    }
}

/* ---------------------------------------------------------------- designs */

/* A colour as seen to the driver's duty, as colour_rgb maps the light's */
static int duty(int seen) { return (int)lroundf(powf(seen / 255.0f, GAMMA) * 255); }

static void duty_px(const unsigned char c[3], char px[8])
{
    static const char hex[] = "0123456789ABCDEF";
    for (int k = 0; k < 3; k++) { int v = duty(c[k]); px[2 * k] = hex[v >> 4]; px[2 * k + 1] = hex[v & 15]; }
    px[6] = 0;
}

/* A design as ledcontroller's text, turned by t (LED i shows what LED i - t did, as turned()).  from > 0: only from
 * that frame on, all of it looping (listening held at a new turn: its loop).  Played once: its frames, then a black
 * loop, so that ledcontroller lets it finish */
static void design_anim(struct buf *b, const struct rd_design *d, int t, int from)
{
    char px[MAX_LEDS][8]; int n = d->leds;
    if (from) bput(b, "loop\n");
    for (int k = from; k < d->nframes; k++) {
        if (!from && k == d->loop) bput(b, "loop\n");
        for (int i = 0; i < n; i++) duty_px(d->f[k].px[i], px[(i + t) % n]);
        frame(b, d->f[k].ms, px, n);
    }
    if (d->loop < 0) {
        for (int i = 0; i < n; i++) strcpy(px[i], "000000");
        bput(b, "loop\n"); frame(b, 1000, px, n);
    }
}

/* stock's volume step with each lit LED in the design's colour there (its first frame), as bright as stock's was */
static void tinted(struct buf *out, const char *text, const struct rd_design *d)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const char *l = text; *l; ) {
        const char *e = strchr(l, '\n'), *px; size_t len = e ? (size_t)(e - l) : strlen(l);
        if (!line_pixels(l, &px)) bput(out, "%.*s\n", (int)len, l);
        else {
            bput(out, "%.*s", (int)(px - l), l);
            int i = 0;
            for (const char *c = px; c < l + len; i++) {
                size_t w = strcspn(c, ",\n\r"); int v = 0;
                for (int ch = 0; ch < 3; ch++) {
                    char dd[3] = { 0 };
                    if (w == 3) dd[0] = dd[1] = c[ch]; else if (w == 6) memcpy(dd, c + 2 * ch, 2);
                    int x = (int)strtol(dd, NULL, 16);
                    if (x > v) v = x;
                }
                const unsigned char *col = d->f[0].px[i < d->leds ? i : 0];
                bput(out, "%s", c == px ? "" : ",");
                for (int ch = 0; ch < 3; ch++) { int y = duty(col[ch]) * v / 255; bput(out, "%c%c", hex[y >> 4], hex[y & 15]); }
                c += w; if (*c == ',') c++; else break;
            }
            bput(out, "\n");
        }
        if (!e) break;
        l = e + 1;
    }
}

/* ---------------------------------------------------------------- the set */

struct gen {
    const char *dir;                        /* NULL: only the digest */
    struct sha256 h;
    int files, failed, turned;              /* turned: the listening ring's copies are in it */
    int assist;                             /* the Assistant's volume steps are in it */
    const char *applied; size_t applied_n;  /* the designs to make: this text, or NULL for state/ring-applied */
    int avail[RING_SLOTS_MAX], on[RING_SLOTS_MAX];      /* stock lists the slot's pattern; a design of it is in the set */
    size_t design_bytes;                    /* what the designs add */
    char why[200];                          /* a design left out, and why */
    int leds;                               /* the ring's, from stock's listening ring */
    int noleds;                             /* in: without the LEDs' own layers (no room for them) */
};
static int leds = 12;                       /* of the set installed (take(), under the lock); every model's has 12 */

static void emit(struct gen *g, const char *name, const struct buf *b)
{
    if (b->bad) { fprintf(stderr, "ring: out of memory making %s\n", name); g->failed = 1; return; }
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

/* A design's file into the set and its name into names */
static void emit_design(struct gen *g, struct buf *names, const char *name, const struct buf *b)
{
    char file[80];
    listnames(names, name);
    snprintf(file, sizeof file, "%s.animation", name); emit(g, file, b);
    g->design_bytes += b->n;
}

/* The designs of state/ring-applied (or g->applied), each where its slot is: g->on.  A design is left out (stock's
 * then, and g->why says so) when stock's config does not list its pattern, it is for another number of LEDs, or it
 * would take the designs past DESIGN_BYTES_MAX */
static void build_designs(struct gen *g, const char *stock, struct buf *names4, struct buf *namesv, struct buf *before,
                          const char **at, int *nat, struct rd_design *ds, int n)
{
    char name[64]; struct buf b = { 0 };
    for (int i = 0; i < ring_nslots; i++) {
        const struct ring_slot *sl = &ring_slots[i];
        g->avail[i] = sl->kind == RING_VOLUME || sl->kind == RING_ASSIST ? strstr(stock, "\"volume_step-") != NULL : listed_at(stock, sl->stock) != NULL;
        g->on[i] = 0;
        const struct rd_design *d = &ds[i];
        if (!d->nframes) continue;
        if (!g->avail[i]) { snprintf(g->why, sizeof g->why, "%s: stock's config has no %s here", sl->id, sl->stock); continue; }
        if (d->leds != n) { snprintf(g->why, sizeof g->why, "%s: drawn for %d LEDs, the ring has %d", sl->id, d->leds, n); continue; }
        b.n = 0; design_anim(&b, d, 0, 0);
        size_t est = sl->kind == RING_LISTEN ? 2 * (size_t)n * b.n : sl->kind == RING_END ? (size_t)n * b.n : b.n;
        if (g->design_bytes + est > DESIGN_BYTES_MAX) {
            snprintf(g->why, sizeof g->why, "%s: the designs would be %zu kB on the system partition, %d at most: shorten them",
                     sl->id, (g->design_bytes + est) >> 10, DESIGN_BYTES_MAX >> 10);
            continue;
        }
        g->on[i] = 1;
        if (sl->kind == RING_LISTEN || sl->kind == RING_END)
            for (int k = 0; k < (sl->kind == RING_LISTEN ? 2 : 1); k++)
                for (int t = 0; t < n; t++) {
                    b.n = 0;
                    if (k == 0 || d->loop >= 0) design_anim(&b, d, t, k ? d->loop : 0);   /* held: its loop */
                    else {                  /* held, of one played once: over (a black loop finishes) */
                        char px[MAX_LEDS][8];
                        for (int i = 0; i < n; i++) strcpy(px[i], "000000");
                        bput(&b, "loop\n"); frame(&b, 1000, px, n);
                    }
                    snprintf(name, sizeof name, "hassmic_s_%s_%d", sl->kind == RING_END ? "end" : k ? "hold" : "listen", t);
                    emit_design(g, names4, name, &b);
                }
        else if (sl->kind == RING_PLAIN) {
            struct buf one = { 0 };
            snprintf(name, sizeof name, "hassmic_s_%s", sl->id);
            emit_design(g, &one, name, &b);
            before[*nat] = one;
            at[(*nat)++] = listed_at(stock, sl->stock);
        }
        else if (sl->kind == RING_VOLUME)
            for (int st = 1; st <= 99; st++) {
                char quoted[40], file[512], *t;
                snprintf(quoted, sizeof quoted, "\"volume_step-%02d\"", st);
                if (!strstr(stock, quoted)) continue;
                snprintf(name, sizeof name, "led-resources/volume_step-%02d.animation", st); path(file, sizeof file, name);
                if (!(t = slurp(file))) continue;
                b.n = 0; tinted(&b, t, d); free(t);
                snprintf(name, sizeof name, "hassmic_s_volume_%02d", st);
                emit_design(g, namesv, name, &b);
            }
        /* RING_ASSIST: the Assistant's steps themselves are made in its colours (build) */
    }
    free(b.p);
}

static int build(struct gen *g)
{
    char file[512], name[64];
    struct buf names4 = { 0 }, names0 = { 0 }, namesv = { 0 }, b = { 0 }, before[RING_SLOTS_MAX], perled[MAX_LEDS];
    const char *at[RING_SLOTS_MAX]; int nat = 0;
    struct rd_design ds[RING_SLOTS_MAX]; int ids[RING_SLOTS_MAX];
    memset(before, 0, sizeof before); memset(perled, 0, sizeof perled);
    sha256_init(&g->h);
    sha256_update(&g->h, FORMAT, sizeof FORMAT);
    if (g->noleds) sha256_update(&g->h, "noleds", 7);
    g->why[0] = 0; g->design_bytes = 0;
    path(file, sizeof file, "led-resources/active-start.animation");
    char *start = slurp(file);
    path(file, sizeof file, "led-resources/active-end.animation");
    char *end = slurp(file);
    int n = start ? leds_of(start) : 0;
    if (!n) n = 12;
    g->leds = n;
    g->turned = 0;
    if (start && end && leds_of(end) == n) {
        static const char *const kinds[] = { "listen", "hold", "end" };
        int ok = 1;
        for (int k = 0; k < 3 && ok; k++)
            for (int t = 0; t < n && ok; t++) {
                b.n = 0;
                if (turned(&b, k == 2 ? end : start, t, k == 1) != n) { ok = 0; break; }
                snprintf(name, sizeof name, "hassmic_%s_%d", kinds[k], t);
                listnames(&names4, name);
                strcat(name, ".animation"); emit(g, name, &b);
            }
        g->turned = ok;
        if (!ok) { fprintf(stderr, "ring: stock's listening ring has a shape we do not know: not turned\n"); names4.n = 0; }
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
    /* each LED alone in each colour, solid: "hassmic_led_<led>_<colour>" */
    for (int l = 0; l < (g->noleds ? 0 : n); l++)
        for (int i = 0; i < NCOLOURS; i++) {
            struct colour c = colour_at(i); char px[MAX_LEDS][8], code[8];
            for (int k = 0; k < n; k++) strcpy(px[k], "000000");
            colour_rgb(&c, 1, px[l]);
            b.n = 0; bput(&b, "loop\n"); frame(&b, 1000, px, n);
            colour_code(&c, code, sizeof code);
            snprintf(name, sizeof name, "hassmic_led_%d_%s", l, code);
            listnames(&perled[l], name);
            strcat(name, ".animation"); emit(g, name, &b);
        }
    /* stock's config: the copy kept from before our first install, else the one in place (still stock's then) */
    path(file, sizeof file, "layer_config_common.json.pre-hassmic");
    char *stock = slurp(file);
    if (!stock) { path(file, sizeof file, "led-resources/layer_config_common.json"); stock = slurp(file); }
    /* the designs applied: none when there is no file, or one that is not ours */
    size_t an = g->applied_n; char *own = g->applied ? NULL : rd_applied_read(&an);
    const char *atext = g->applied ? g->applied : own;
    memset(ds, 0, sizeof ds);
    if (atext && rd_applied_parse(atext, an, ds, ids)) for (int i = 0; i < ring_nslots; i++) rd_free(&ds[i]);   /* not ours: none */
    free(own);
    const struct rd_design *assist_design = NULL;
    if (stock) build_designs(g, stock, &names4, &namesv, before, at, &nat, ds, n);
    for (int i = 0; i < ring_nslots; i++) if (g->on[i] && ring_slots[i].kind == RING_ASSIST) assist_design = &ds[i];
    if (g->why[0]) fprintf(stderr, "ring: a design left out, stock's there: %s\n", g->why);
    /* the Assistant's volume steps: each of stock's that its config lists, in cyan (or in its design's colours) */
    g->assist = 0;
    for (int i = 1; stock && i <= 99; i++) {
        char quoted[40], *t;
        snprintf(quoted, sizeof quoted, "\"volume_step-%02d\"", i);
        if (!strstr(stock, quoted)) continue;
        snprintf(name, sizeof name, "led-resources/volume_step-%02d.animation", i); path(file, sizeof file, name);
        if (!(t = slurp(file))) continue;
        b.n = 0;
        if (assist_design) tinted(&b, t, assist_design); else cyan(&b, t);
        free(t);
        snprintf(name, sizeof name, "hassmic_assist_%02d", i);
        listnames(&namesv, name);
        strcat(name, ".animation"); emit(g, name, &b);
        g->assist++;
    }
    b.n = 0;
    const char *bnames[RING_SLOTS_MAX];
    for (int i = 0; i < nat; i++) bnames[i] = before[i].p ? before[i].p : "";
    int oom = names4.bad || names0.bad || namesv.bad;
    for (int i = 0; i < nat; i++) oom |= before[i].bad;
    for (int l = 0; l < n; l++) oom |= perled[l].bad;
    if (oom) { fprintf(stderr, "ring: out of memory making the layer config\n"); g->failed = 1; }
    int rc = !stock || merge_config(&b, stock, names4.p ? names4.p : "", names0.p ? names0.p : "", namesv.p ? namesv.p : "",
                                    at, bnames, nat) ? -1 : 0;
    struct buf conf = { 0 };
    if (!rc && !g->noleds && (relayer(&conf, b.p, n, perled) || conf.bad)) { rc = -1; fprintf(stderr, "ring: stock's layer config has a shape we do not know\n"); }
    else if (rc) fprintf(stderr, "ring: no layer config of stock's to add ours to (%s)\n", file);
    if (!rc) emit(g, "layer_config_common.json", g->noleds ? &b : &conf);
    free(conf.p);
    for (int l = 0; l < n; l++) free(perled[l].p);
    for (int i = 0; i < ring_nslots; i++) rd_free(&ds[i]);
    for (int i = 0; i < nat; i++) free(before[i].p);
    free(start); free(end); free(stock); free(names4.p); free(names0.p); free(namesv.p); free(b.p);
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
    const char *e = getenv("HASSMIC_RING_LEDS");
    memset(&g, 0, sizeof g); g.dir = strcmp(dir, "-") ? dir : NULL;
    g.noleds = e && !strcmp(e, "0");        /* root's leds(): the partition has no room for the LEDs' own */
    if (build(&g)) return 1;
    digest_hex(&g, hex);
    if (!g.dir) { printf("%s\n", hex); return 0; }       /* "-": the digest only, for root to compare */
    snprintf(file, sizeof file, "%s/digest", dir);
    if (!(f = fopen(file, "w")) || fprintf(f, "%s\n", hex) < 0 || fclose(f)) { fprintf(stderr, "ring: cannot write %s\n", file); return 1; }
    int nd = 0; for (int i = 0; i < ring_nslots; i++) nd += g.on[i];
    fprintf(stderr, "ring: %d files, digest %.16s%s, designs in %d place%s\n", g.files, hex, g.turned ? "" : " (listening ring not turned)",
            nd, nd == 1 ? "" : "s");
    return 0;
}

/* ---------------------------------------------------------------- at run time */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void (*run_fn)(const char *op, const char *name);
static void (*reloaded_fn)(int custom_changed);
static int custom, have_turned, have_assist, have_leds;
static int slot_avail[RING_SLOTS_MAX], slot_on[RING_SLOTS_MAX];         /* of the set installed */

/* The Echo's own patterns that hassmic played and did not stop: how long each shows, from its file */
#define SHOWN_MAX 24
static struct shown { char logical[40], actual[40]; long long at; int ms, turn; } shown[SHOWN_MAX];   /* ms < 0: until unset */
static int nshown;
static struct ring_light light = { .brightness = 1, .color_brightness = 1, .r = 1, .g = 1, .b = 1, .effect = E_NONE, .fill = 100 };
static struct ring_light flash; static long long flash_until;
static char light_shown[48];                /* what of ours is on the ring for the light, "" none */
static char led_shown[MAX_LEDS][32];        /* and on each LED's own layer (a fill), "" none */
static int rotation;                        /* the fill's start, in LEDs from stock's */

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void run(const char *op, const char *name) { if (run_fn) run_fn(op, name); }

/* How long a pattern shows once played: until unset (-1) when its loop has light in it or it has no loop (it plays
 * again from the start), else to the end of its last lit frame (ledcontroller lets it finish there).  Taking a file
 * without "loop" for finished let Home Assistant's light into the gaps of a Drop In call's ring after one pass, and
 * forgot what played for it.  Read once per pattern; 1 s when there is no file to read */
static struct { char name[40]; int ms; } cache[48]; static int ncache;      /* emptied when root installs a set */
static int visible_ms(const char *name)
{
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
        ms = loop_lit || (!looping && lit_end) ? -1 : lit_end;
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

/* Remembered across a restart of hassmic: ledcontroller keeps playing what was left on.  One name per line */
static void light_remember(void)
{
    char file[300]; const char *d = getenv("HASSMIC_STATE"); int any = light_shown[0];
    snprintf(file, sizeof file, "%s/ring-light", d ? d : "/data/local/hassmic/state");
    for (int i = 0; i < MAX_LEDS; i++) any |= led_shown[i][0];
    if (!any) { unlink(file); return; }
    FILE *f = fopen(file, "w");
    if (!f) return;
    if (light_shown[0]) fprintf(f, "%s\n", light_shown);
    for (int i = 0; i < MAX_LEDS; i++) if (led_shown[i][0]) fprintf(f, "%s\n", led_shown[i]);
    fclose(f);
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
    char want[48] = "", wled[MAX_LEDS][32];
    int flashing = flash_until && now < flash_until, changed = 0;
    if (!flashing) flash_until = 0;
    const struct ring_light *l = flashing ? &flash : &light;
    memset(wled, 0, sizeof wled);
    /* The fill: that share of the LEDs, rounded up (1 % one LED, all of them only at 100 %), from where stock's volume
     * steps start (volume_step-01 lights LED 11, -05 LEDs 11 and 0, -10 LEDs 11 to 2) turned by the rotation.  A part
     * of the ring is each LED's own solid pattern, so the effects are the whole ring's only; a flash blinks it whole */
    int lit = (l->fill * leds + 99) / 100, start = (leds - 1 + rotation) % leds;
    if (custom && have_leds && l->on && !flashing && !busy(now) && l->nleds > 0) {     /* LED by LED, each in the palette's nearest */
        for (int j = 0; j < l->nleds && j < leds; j++) {
            struct ring_light t = *l; struct colour c; char code[8]; int i = (start + j) % leds;
            const unsigned char *p = l->led_rgb[j];
            if (!p[0] && !p[1] && !p[2]) continue;
            t.r = p[0] / 255.0f; t.g = p[1] / 255.0f; t.b = p[2] / 255.0f;
            quantize(&t, &c); colour_code(&c, code, sizeof code);
            snprintf(wled[i], sizeof wled[i], "hassmic_led_%d_%s", i, code);
        }
    }
    else if (custom && (l->on || flashing) && !busy(now) && lit > 0) {
        struct colour c; quantize(l, &c);
        if (lit >= leds || flashing || !have_leds) light_pattern(flashing ? E_BLINK : l->effect, &c, want, sizeof want);
        else {
            char code[8]; colour_code(&c, code, sizeof code);
            for (int j = 0; j < lit; j++) { int i = (start + j) % leds; snprintf(wled[i], sizeof wled[i], "hassmic_led_%d_%s", i, code); }
        }
    }
    int nled = 0;
    changed = strcmp(want, light_shown) != 0;
    for (int i = 0; i < leds; i++) { changed |= strcmp(wled[i], led_shown[i]) != 0; nled += wled[i][0] != 0; }
    if (!changed) return;
    if (want[0]) fprintf(stderr, "light ring: Home Assistant's light %s%s\n", want, flashing ? " (flash)" : "");
    else if (nled) fprintf(stderr, "light ring: Home Assistant's light on %d LED%s from %d%s\n", nled, nled == 1 ? "" : "s", start,
                           l->nleds > 0 && !flashing ? ", LED by LED" : "");
    else fprintf(stderr, "light ring: Home Assistant's light %s\n", !l->on && !flashing ? "off" : busy(now) ? "waits for the Echo's own" : "dark");
    /* Every new one on before an old one goes: the ring is never dark between two colours or two fills */
    if (want[0] && strcmp(want, light_shown)) run("-s", want);
    for (int i = 0; i < leds; i++) if (wled[i][0] && strcmp(wled[i], led_shown[i])) run("-s", wled[i]);
    if (light_shown[0] && strcmp(want, light_shown)) run("-u", light_shown);
    for (int i = 0; i < leds; i++) if (led_shown[i][0] && strcmp(wled[i], led_shown[i])) run("-u", led_shown[i]);
    snprintf(light_shown, sizeof light_shown, "%s", want);
    memcpy(led_shown, wled, sizeof wled);
    light_remember();
}

/* What the installed set is (not under the lock: 0.1 s on the Echo): ours if its digest is this build's of the designs
 * known installed (state/ring-installed), or of those asked for (state/ring-applied): root installed them, at a start
 * or for the page, and they are the installed ones from now on.  Then which slots play designs.  -1: no set could be
 * made to compare with */
static int installed(struct gen *g)
{
    char hex[65], file[512], *inst, *text[2]; size_t len[2] = { 0, 0 }; int rc = -1;
    path(file, sizeof file, "hassmic-leds.digest");
    inst = slurp(file);
    text[0] = rd_installed_read(&len[0]); text[1] = rd_applied_read(&len[1]);
    for (int k = 0; k < 4 && rc != 1; k++) {        /* each with the LEDs' own, then without (root had no room) */
        if (k / 2 && (!text[1] || (text[0] && len[0] == len[1] && !memcmp(text[0], text[1], len[1])))) break;   /* nothing else asked for */
        memset(g, 0, sizeof *g);
        g->applied = text[k / 2] ? text[k / 2] : ""; g->applied_n = text[k / 2] ? len[k / 2] : 0; g->noleds = k % 2;
        if (build(g)) { rc = -1; continue; }
        digest_hex(g, hex);
        if ((rc = inst && !strncmp(inst, hex, 64)) && k / 2) rd_installed_set(text[1], len[1]);
    }
    free(inst); free(text[0]); free(text[1]);
    return rc;
}

static void take(const struct gen *g, int ours)
{
    custom = ours > 0;
    have_turned = custom && g->turned;
    have_assist = custom && g->assist;
    have_leds = custom && !g->noleds;
    leds = g->leds ? g->leds : 12;
    for (int i = 0; i < ring_nslots; i++) { slot_avail[i] = custom && g->avail[i]; slot_on[i] = custom && g->on[i]; }
}

void ring_start(void (*fn)(const char *op, const char *name), void (*reloaded)(int custom_changed))
{
    struct gen g; char file[512];
    run_fn = fn; reloaded_fn = reloaded;
    int ours = installed(&g);
    take(&g, ours);
    int nd = 0; for (int i = 0; i < ring_nslots; i++) nd += slot_on[i];
    if (custom) fprintf(stderr, "ring: our animations are installed (%d files, designs in %d place%s)%s\n", g.files, nd, nd == 1 ? "" : "s",
                        have_turned ? "" : ", the listening ring not turned");
    else fprintf(stderr, "ring: %s: stock's animations only, no light in Home Assistant\n",
                 !ours ? "the installed animations are another build's or other designs" : "our animations are not installed");
    if (have_turned || slot_on[S_LISTEN]) {        /* LIPC opened now: a failure is in the log at the start, not at a wake word */
        pthread_mutex_lock(&lock); int b = beam_now(); pthread_mutex_unlock(&lock);
        if (b >= 0) fprintf(stderr, "ring: the front end's beam is %d\n", b);
    }
    const char *d = getenv("HASSMIC_STATE"); char left[48] = ""; FILE *f;   /* this light from before a restart */
    snprintf(file, sizeof file, "%s/ring-light", d ? d : "/data/local/hassmic/state");
    if ((f = fopen(file, "r"))) {
        for (int i = 0; i <= MAX_LEDS && fscanf(f, "%47s", left) == 1; i++) if (!strncmp(left, "hassmic_", 8)) run("-u", left);
        fclose(f); unlink(file);
    }
}

int ring_check(const char *applied, size_t n, char *err, size_t cap)
{
    struct gen g; memset(&g, 0, sizeof g);
    g.applied = applied; g.applied_n = n;
    int rc = build(&g);
    uint8_t d[32]; sha256_final(&g.h, d);
    if (rc) { snprintf(err, cap, "the ring's set cannot be made here (stock's files?)"); return -1; }
    if (g.why[0]) { snprintf(err, cap, "%s", g.why); return -1; }
    return 0;
}

int ring_slot_available(int i) { pthread_mutex_lock(&lock); int r = i >= 0 && i < ring_nslots && slot_avail[i]; pthread_mutex_unlock(&lock); return r; }
int ring_slot_live(int i) { pthread_mutex_lock(&lock); int r = i >= 0 && i < ring_nslots && slot_on[i]; pthread_mutex_unlock(&lock); return r; }
int ring_leds(void) { return leds; }

int ring_custom(void) { return custom; }
int ring_assistant_steps(void) { return have_assist; }
int ring_per_led(void) { return have_leds; }

/* lock held: the slot that shows PATTERN (stock's name; a volume step: its number in *step), -1 none */
static int slot_of(const char *pattern, int *step)
{
    for (int i = 0; i < ring_nslots; i++) {
        const struct ring_slot *sl = &ring_slots[i]; size_t k = strlen(sl->stock);
        if (sl->kind == RING_VOLUME || sl->kind == RING_ASSIST) {
            if (!strncmp(pattern, sl->stock, k) && isdigit((unsigned char)pattern[k])) { *step = atoi(pattern + k); return i; }
        }
        else if (!strcmp(pattern, sl->stock)) return i;
    }
    return -1;
}

/* lock held: what plays for PATTERN, turned by TURN (-1: the beam is not known): a design of its slot, else stock's
 * own turned (listening, its end), else stock's */
static void actual_for(const char *pattern, int turn, char out[40])
{
    int step = 0, i = slot_of(pattern, &step);
    const struct ring_slot *sl = i >= 0 ? &ring_slots[i] : NULL;
    if (sl && slot_on[i] && sl->kind == RING_LISTEN) snprintf(out, 40, "hassmic_s_listen_%d", turn < 0 ? 0 : turn);
    else if (sl && slot_on[i] && sl->kind == RING_END) snprintf(out, 40, "hassmic_s_end_%d", turn < 0 ? 0 : turn);
    else if (sl && slot_on[i] && sl->kind == RING_VOLUME) snprintf(out, 40, "hassmic_s_volume_%02d", step);
    else if (sl && slot_on[i] && sl->kind == RING_PLAIN) snprintf(out, 40, "hassmic_s_%s", sl->id);
    else if (sl && (sl->kind == RING_LISTEN || sl->kind == RING_END) && have_turned && turn >= 0)
        snprintf(out, 40, "hassmic_%s_%d", sl->kind == RING_LISTEN ? "listen" : "end", turn);
    else snprintf(out, 40, "%s", pattern);
}

void ring_set(const char *pattern)
{
    char actual[40]; int turn = -1;
    pthread_mutex_lock(&lock);
    int ring = !strcmp(pattern, "active-start") || !strcmp(pattern, "active-end");
    if (ring && (have_turned || slot_on[S_LISTEN] || slot_on[S_END]) && (turn = beam_now()) >= 0) turn %= leds;
    /* A design with the beam not known yet: at turn 0, and ring_tick turns it once the beam is there.  Stock's own
     * then: stock's name, which ledcontroller turns by itself while nothing else shows */
    if (ring && turn < 0 && slot_on[pattern[7] == 's' ? S_LISTEN : S_END]) turn = 0;
    actual_for(pattern, turn, actual);
    struct shown *s = find(pattern);
    if (s && strcmp(s->actual, actual)) run("-u", s->actual);
    if (!s && nshown < SHOWN_MAX) s = &shown[nshown++];
    if (s) {
        snprintf(s->logical, sizeof s->logical, "%s", pattern); snprintf(s->actual, sizeof s->actual, "%s", actual);
        s->at = now_ms(); s->ms = visible_ms(actual); s->turn = turn;
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
            snprintf(s->actual, sizeof s->actual, slot_on[S_LISTEN] ? "hassmic_s_hold_%d" : "hassmic_hold_%d", s->turn);
            run("-s", s->actual); run("-u", was);
        }
    }
    light_apply(now);
    pthread_mutex_unlock(&lock);
}

void ring_reload(void)
{
    struct gen g;
    int ours = installed(&g);               /* outside the lock: 0.1 s on the Echo */
    pthread_mutex_lock(&lock);
    int was = custom, was_leds = have_leds;
    take(&g, ours);
    ncache = 0;                             /* the same names may hold other designs now */
    long long now = now_ms();
    int nd = 0; for (int i = 0; i < ring_nslots; i++) nd += slot_on[i];
    fprintf(stderr, "ring: the set root installed is %s (designs in %d place%s)\n", custom ? "ours" : "not ours", nd, nd == 1 ? "" : "s");
    /* ledcontroller started again with nothing on: what was on, again, under the names of the new set */
    for (int i = 0; i < nshown; i++) {
        struct shown *s = &shown[i];
        if (s->ms >= 0 && now >= s->at + s->ms) continue;
        actual_for(s->logical, s->turn, s->actual);
        if (!strcmp(s->logical, "active-start") && s->turn >= 0 && strncmp(s->actual, "active", 6))  /* open already */
            snprintf(s->actual, sizeof s->actual, slot_on[S_LISTEN] ? "hassmic_s_hold_%d" : "hassmic_hold_%d", s->turn);
        s->ms = visible_ms(s->actual); s->at = now;
        run("-s", s->actual);
    }
    int any = light_shown[0];
    for (int i = 0; i < MAX_LEDS; i++) any |= led_shown[i][0];
    if (any && !custom) { light_shown[0] = 0; memset(led_shown, 0, sizeof led_shown); light_remember(); }
    else if (any) {
        if (light_shown[0]) run("-s", light_shown);
        for (int i = 0; i < MAX_LEDS; i++) if (led_shown[i][0]) run("-s", led_shown[i]);
    }
    pthread_mutex_unlock(&lock);
    if (reloaded_fn) reloaded_fn(custom != was || have_leds != was_leds);      /* the fill's entities come and go with them */
}

void ring_light_get(struct ring_light *l) { pthread_mutex_lock(&lock); *l = light; pthread_mutex_unlock(&lock); }

void ring_light_set(const struct ring_light *l, unsigned flash_ms)
{
    pthread_mutex_lock(&lock);
    if (flash_ms) { flash = *l; flash.on = 1; flash_until = now_ms() + flash_ms; }
    else { light = *l; flash_until = 0; }
    if (light.effect < 0 || light.effect >= E_COUNT) light.effect = E_NONE;
    light.fill = light.fill < 0 ? 0 : light.fill > 100 ? 100 : light.fill;
    if (light.nleds < 0 || light.nleds > MAX_LEDS) light.nleds = 0;
    flash.fill = flash.fill < 0 ? 0 : flash.fill > 100 ? 100 : flash.fill;
    long long now = now_ms();
    light_apply(now);
    if (custom && (light.on || flash_until) && !light_shown[0])
        for (int i = 0; i < nshown; i++)
            if (shown[i].ms < 0 || now < shown[i].at + shown[i].ms) {
                fprintf(stderr, "light ring: Home Assistant's light waits for the Echo's own (%s)\n", shown[i].logical); break;
            }
    pthread_mutex_unlock(&lock);
}

int ring_rotation(int set)
{
    pthread_mutex_lock(&lock);
    if (set >= 0) { rotation = set % leds; light_apply(now_ms()); }
    int r = rotation;
    pthread_mutex_unlock(&lock);
    return r;
}
