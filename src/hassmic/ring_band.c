/*
 * The light ring of a model without one (ring.h): a band along the bottom of the screen, the Echo Show 5's.  Stock
 * shows Alexa's attention there too ("voice chrome": SpeechInteractionManager.apk, BambooAttentionSystemVisualsComponent,
 * Lottie compositions of 960x200 in assets/landscape/), as a 10 px line along the bottom edge drawn over the app in
 * front.  Here the band is reserved instead, so no app ever has it under it and any app can be the screen's UI:
 *
 *   renderer the screen UI's process (screen.h, src/ui/android.c), where it runs: one more layer in a process that is
 *            there anyway (ringbar's VM was 16 MB of PSS of its own, 2026-10-14), drawn natively; this file sends it one
 *            row per frame (the band is the same in every row).  Else, and for the tests (HASSMIC_FAKE_BAND):
 *   ringbar  scripts/device/ringbar.dex (tools/mkringbar.py), run as app_process by this file: it sets the window
 *            manager's overscan for the band (every app is laid out above it; nothing of theirs draws there), puts a
 *            SurfaceFlinger layer of its own over it, says "ready <width> <rows>" and draws the frames it is sent on
 *            stdin (width x rows ARGB, little-endian), until stdin closes.  It knows nothing of patterns.
 *   patterns the core's (stock's ledcontroller names, main.c led()), drawn here: listening, thinking, mic off and calls
 *            as stock's chrome draws them (its colours read off the compositions; the listening bar widens and whitens
 *            with the talker's loudness, as its ListeningRamp is driven); what only the Dots' ring shows (replies,
 *            volume, timers, Do not disturb, setup, identify, errors) in the ring's colours, laid out in a line.  One
 *            pattern shows at a time, by priority; Home Assistant's light only while none does, as on the ring.
 *   designs  the settings page's (ringdesign.h), drawn here in place of a pattern as they are, frame by frame: no set
 *            for root to install, Apply shows them at once.  Flattened, never turned (no beam to follow): cut open
 *            opposite where stock's listening ring points at turn 0, so the side facing the talker is in the middle, as
 *            in the patterns.  The page gets the patterns as files (ring_stock) to start designs from.
 *   screen  stock's power policy (EnvironmentContextService, ECStateMachine): the screen sleeps 5 min after the last
 *            activity, 1 min with the mic off or in Do not disturb; a pattern wakes it (IPowerManager.wakeUp, through the
 *            service tool: transaction 9, isInteractive 12 on 8149, checked on the first Echo Show).  An app in front
 *            keeps its own wake locks.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                     /* pipe2 on glibc */
#endif
#include "ring.h"
#include "board.h"
#include "core.h"
#include "ringdesign.h"
#include "screen.h"
#include "textdraw.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

const char *const ring_effects[] = { "None", "Pulse", "Spin", "Blink", "Rainbow" };
const int ring_neffects = 5;
enum { E_NONE, E_PULSE, E_SPIN, E_BLINK, E_RAINBOW };

#define FRAME_MS     33                 /* 30 frames a second while something moves */
#define MAX_W        2048
#define MAX_ROWS     40
#define MAX_TEXT_ROWS 1200              /* the text layer's: the tallest Show's screen and then some */
#define TIMEOUT_MS   300000             /* stock's screen_off_timeout_standard (Arcus default) */
#define QUIET_MS     60000              /* stock's _dnd and _privacy */
#define WAKE_EVERY   2000               /* the power calls at most this often (a process each) */

struct rgb { float r, g, b; };

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static struct rgb hex(unsigned v) { return (struct rgb){ (v >> 16 & 255) / 255.f, (v >> 8 & 255) / 255.f, (v & 255) / 255.f }; }
static struct rgb mix(struct rgb a, struct rgb b, float k) { return (struct rgb){ a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k }; }
static struct rgb scale(struct rgb a, float k) { return (struct rgb){ a.r * k, a.g * k, a.b * k }; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float ease(float t) { t = clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }      /* out-cubic, as Lottie's */
static float wave(long long t, int period) { return 0.5f - 0.5f * cosf(2 * (float)M_PI * (float)(t % period) / period); }

static struct rgb hue(float h)                   /* h in turns: full colour */
{
    h = (h - floorf(h)) * 6; float f = h - floorf(h);
    switch ((int)h) {
    case 0: return (struct rgb){ 1, f, 0 };
    case 1: return (struct rgb){ 1 - f, 1, 0 };
    case 2: return (struct rgb){ 0, 1, f };
    case 3: return (struct rgb){ 0, 1 - f, 1 };
    case 4: return (struct rgb){ f, 0, 1 };
    default: return (struct rgb){ 1, 0, 1 - f };
    }
}

/* ---------------------------------------------------------------- drawing: the ring, unrolled */

/* The Dots' ring has 12 LEDs; laid out along the band, each takes a twelfth of the width and blends into its neighbours
 * over the middle 40 % between them: the ring's steps stay visible, without hard edges.  Unrolled with the talker at the
 * centre: what stock's ring turns towards the talker (the cyan of the listening ring) is in the middle, the side away
 * from it at both ends. */
#define LEDS 12
static void unroll(const struct rgb led[LEDS], struct rgb *px, int w)
{
    for (int x = 0; x < w; x++) {
        float f = (x + 0.5f) * LEDS / w - 0.5f; int i = (int)floorf(f); float u = f - i;
        int a = i < 0 ? 0 : i, b = i + 1 > LEDS - 1 ? LEDS - 1 : i + 1;
        px[x] = mix(led[a], led[b], clamp01((u - 0.5f) * 2.5f + 0.5f));
    }
}

static void all(struct rgb *led, struct rgb c) { for (int i = 0; i < LEDS; i++) led[i] = c; }
static float from_centre(int i) { return fabsf(i - (LEDS - 1) / 2.0f) - 0.5f; }  /* 0 for the middle two, 5 at the ends */
static float from_end(int i) { return i < LEDS / 2 ? i : LEDS - 1 - i; }            /* 0 at the ends, 5 in the middle */

/* one LED running round the ring (wrapping at the ends) at a fractional position, so it glides; tail: how many LEDs
 * behind it fade out */
static void runner(struct rgb *led, long long t, int lap, struct rgb c, float tail)
{
    float p = (float)(t % lap) / lap * LEDS;
    for (int i = 0; i < LEDS; i++) {
        float d = fmodf(p - i + LEDS + LEDS / 2.0f, LEDS) - LEDS / 2.0f;     /* > 0: behind the head */
        led[i] = scale(c, clamp01(d < 0 ? 1 + d : 1 - d / (tail + 1)));
    }
}

/* ---------------------------------------------------------------- the patterns: the Dots' ring's, in a line */

#define CYAN     0x00FFFF
#define BLUE     0x0000FF
#define GREEN    0x00FF18                /* call_outbound_ringing's */
#define VIOLET   0x6000C0                /* do_not_disturb at its peak (its file is dim: 0c0019 at the brightest LED) */
#define ORANGE   0xFF5500                /* setup-mode's */
#define RED      0xFF0000
#define WHITE    0xFFFFFF

static atomic_int level_db = -1000;      /* the talker, dBFS x 10, from the pipeline's audio (ring_level) */
static float loud;                       /* smoothed 0..1; the band thread's */

enum pid { P_RESET, P_VOLUME, P_ASSIST, P_LISTEN, P_THINK, P_TALK, P_END, P_CALL_OUT, P_CALL_IN, P_CALL, P_CALL_END, P_ERROR,
           P_MUTE, P_UNMUTE, P_DND, P_IDENTIFY, P_LOGIN, P_PAIRING, P_TIMER, P_TIMER_SHORT, P_SETUP, P_COUNT };
/* priority order (first wins: the volume over a reply, as the ring shows it) and how long each shows: -1 until unset */
static const struct { const char *name; int ms; } pats[P_COUNT] = {
    [P_RESET] = { "factory-reset", -1 },
    [P_LISTEN] = { "active-start", -1 }, [P_THINK] = { "active-thinking", -1 }, [P_TALK] = { "active-talking", -1 },
    [P_END] = { "active-end", 600 },
    [P_CALL_OUT] = { "call_outbound_ringing", -1 }, [P_CALL_IN] = { "call_incoming", -1 },
    [P_CALL] = { "call_connected", -1 }, [P_CALL_END] = { "call_connected_end", 1480 },
    [P_VOLUME] = { "volume_step-", -1 },             /* a prefix: the step follows */
    [P_ASSIST] = { "hassmic_assist_", -1 },          /* the replies' own volume (ring_assistant_steps), the same */
    [P_ERROR] = { "anim_start_error_short", 3300 },
    [P_MUTE] = { "mics-off_on", -1 }, [P_UNMUTE] = { "mics-off_end", 1100 },
    [P_DND] = { "do_not_disturb", 1760 },
    [P_IDENTIFY] = { "zzz_rainbow", -1 },
    [P_LOGIN] = { "authenticated_setup_mode", -1 },
    [P_PAIRING] = { "scone-setup", -1 },
    [P_TIMER] = { "ready-timer", -1 }, [P_TIMER_SHORT] = { "ready-timer-short", 2233 },
    [P_SETUP] = { "setup-mode", -1 },
};

/* Each as the Dot's file of that name plays it (timings and colours read off donut's led-resources, 2026-10-13);
 * loudness: the talker's, 0..1 */
static void draw(int p, struct rgb *led, long long t, int arg, float loudness)
{
    all(led, hex(0));
    switch (p) {
    case P_LISTEN:                       /* blue fills from the far side round to the talker, cyan in front, in 0.5 s;
                                            then blue with cyan at the talker, wider while they speak up (ours) */
        for (int i = 0; i < LEDS; i++) {
            float front = clamp01(t / 500.f) * (LEDS / 2), e = from_end(i);
            if (t < 500) led[i] = e < front - 1 ? hex(BLUE) : e < front ? hex(CYAN) : hex(0);
            else led[i] = mix(hex(BLUE), hex(CYAN), clamp01(1 + loudness * 2.5f - from_centre(i)));
        }
        break;
    case P_THINK:                        /* cyan and blue LEDs taking turns, every 100 ms */
        for (int i = 0; i < LEDS; i++) led[i] = (i + t / 100) % 2 ? hex(CYAN) : hex(BLUE);
        break;
    case P_TALK: all(led, mix(hex(BLUE), hex(CYAN), 1 - wave(t, 1690))); break;     /* the ring breathing cyan to blue */
    case P_END:                          /* dark from the talker outwards, the last LEDs cyan, 0.6 s */
        for (int i = 0; i < LEDS; i++) {
            float gone = clamp01(t / 600.f) * (LEDS / 2 + 1), c = from_centre(i);
            led[i] = c >= gone ? hex(BLUE) : c >= gone - 1 ? hex(CYAN) : hex(0);
        }
        break;
    case P_MUTE: all(led, scale(hex(RED), 1 - 0.4f * ease(t / 700.f))); break;     /* bright red settling to dark red */
    case P_UNMUTE: all(led, scale(hex(RED), 0.6f * (1 - clamp01(t / 1100.f)))); break;
    case P_DND: all(led, scale(hex(VIOLET), sinf((float)M_PI * clamp01(t / 1760.f)))); break;
    case P_ERROR: all(led, scale(hex(RED), 0.9f * wave(t, 1100))); break;            /* three red pulses */
    case P_LOGIN: all(led, scale(hex(BLUE), wave(t, 1800))); break;                   /* blue breathing */
    case P_PAIRING: runner(led, t, 1060, hex(BLUE), 0); break;                         /* one blue LED round the ring */
    case P_SETUP: runner(led, t, 1380, hex(ORANGE), 0); break;                         /* one orange LED round the ring */
    case P_RESET: all(led, scale(hex(ORANGE), 0.1f + 0.9f * wave(t, 830))); break;
    case P_TIMER: case P_TIMER_SHORT:    /* groups of three cyan LEDs, blinking against the others, blue at their edges */
        for (int i = 0; i < LEDS; i++) {
            int on = (i / 3 + t / 375) % 2;
            led[i] = on ? hex(CYAN) : i % 3 == 1 ? scale(hex(CYAN), 0.35f) : scale(hex(BLUE), 0.6f);
        }
        break;
    case P_CALL_OUT: runner(led, t, 900, hex(GREEN), 5); break;                        /* a green arc going round */
    case P_CALL_IN:                      /* green pairs pulsing, a step on at every pulse */
        for (int i = 0; i < LEDS; i++) led[i] = (i + t / 1300) % 4 < 2 ? scale(hex(GREEN), 0.25f + 0.75f * wave(t, 1300)) : hex(0);
        break;
    case P_CALL: runner(led, t, 2400, scale(hex(GREEN), 0.85f), 7); break;           /* green waves going round */
    case P_CALL_END:                     /* green running from the middle to both ends */
        for (int i = 0; i < LEDS; i++) {
            float d = clamp01(t / 1480.f) * (LEDS / 2 + 2) - from_centre(i);
            led[i] = scale(hex(GREEN), d > 0 && d < 2 ? 1 - fabsf(d - 1) : 0);
        }
        break;
    case P_VOLUME: case P_ASSIST: {      /* white LEDs filled from the left, as many as the level (the replies' own
                                            volume in cyan, as ring.c makes its steps) */
        float n = arg / 100.f * LEDS;
        for (int i = 0; i < LEDS; i++) led[i] = scale(hex(p == P_ASSIST ? CYAN : WHITE), clamp01(n - i));
        break; }
    case P_IDENTIFY: for (int i = 0; i < LEDS; i++) led[i] = hue((float)i / LEDS - (float)(t % 2000) / 2000); break;
    }
}

/* ---------------------------------------------------------------- state */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t tcv = PTHREAD_COND_INITIALIZER;   /* the text layer's thread: text, setting, band height */
static struct { long long at; int arg, on, ms; } act[P_COUNT];     /* ms: how long it shows (pats', or its design's) */
static struct ring_light light = { .on = 0, .brightness = 1, .color_brightness = 1, .r = 1, .g = 1, .b = 1, .effect = E_NONE, .fill = 100 }, flash;
static long long flash_until;
static int rows = 10, wake_req, started;
static atomic_int screen_hold;          /* timers show on the text layer: the screen stays on (band thread) */
#define HOLD_MS      86400000           /* the screen's timeout meanwhile: a day */

static int find(const char *pattern, int *arg)
{
    for (int i = 0; i < P_COUNT; i++) {
        size_t n = strlen(pats[i].name); int step = i == P_VOLUME || i == P_ASSIST;
        if (step ? !strncmp(pattern, pats[i].name, n) : !strcmp(pattern, pats[i].name)) {
            if (arg) *arg = step ? atoi(pattern + n) * 100 / (board.volume_steps > 0 ? board.volume_steps : 100) : 0;
            return i;
        }
    }
    return -1;
}

/* lock held: what shows now (P_COUNT: Home Assistant's light or nothing); finished ones go */
static int top(long long now)
{
    for (int i = 0; i < P_COUNT; i++) {
        if (!act[i].on) continue;
        if (act[i].ms >= 0 && now >= act[i].at + act[i].ms) { act[i].on = 0; continue; }
        return i;
    }
    return P_COUNT;
}

/* ---------------------------------------------------------------- designs (ringdesign.h)
 *
 * ring.c's slots, their ids and order (the page's and state/ring-applied's: the same on every model, so designs and
 * packs go from Echo to Echo), each one of the patterns above.  A design is drawn flattened: band position p (0 at the
 * left) shows its LED p + SHIFT, the ring cut open opposite LEDs 10 and 11, where stock's listening ring points at turn
 * 0 (web/ring.js RING_TALKER): what faces the talker is in the middle, as in the patterns (unroll).  Never turned. */
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
#define SHIFT 5                          /* 10.5 - 5.5: the talker's pair to the middle of the band */
static struct rd_design des[RING_SLOTS_MAX];    /* under lock: state/ring-installed's, by slot */

static int slot_of(int p) { for (int i = 0; i < ring_nslots; i++) if (!strcmp(ring_slots[i].stock, pats[p].name)) return i; return -1; }
static int pid_of(int s) { for (int p = 0; p < P_COUNT; p++) if (!strcmp(ring_slots[s].stock, pats[p].name)) return p; return -1; }
static int stepped(int p) { return p == P_VOLUME || p == P_ASSIST; }

/* the frame a design shows t ms after it started, as ledcontroller plays one: its frames up to the loop once, then the
 * loop again and again; played once, then dark (NULL) */
static const struct rd_frame *design_frame(const struct rd_design *d, long long t)
{
    int lp = d->loop < 0 ? d->nframes : d->loop;
    long long intro = 0, rest = 0;
    for (int i = 0; i < d->nframes; i++) *(i < lp ? &intro : &rest) += d->f[i].ms;
    if (t < intro) for (int i = 0; i < lp; i++) { if (t < d->f[i].ms) return &d->f[i]; t -= d->f[i].ms; }
    if (d->loop < 0 || !rest) return NULL;
    t = (t - intro) % rest;
    for (int i = lp; i < d->nframes; i++) { if (t < d->f[i].ms) return &d->f[i]; t -= d->f[i].ms; }
    return NULL;
}

/* How long a design shows, as ring.c's visible_ms reads a file: until unset while its loop has light, else up to its
 * last lit frame */
static int design_ms(const struct rd_design *d)
{
    int at = 0, lit_end = 0, loop_lit = 0;
    for (int i = 0; i < d->nframes; i++) {
        int lit = 0;
        for (int k = 0; k < d->leds; k++) lit |= d->f[i].px[k][0] | d->f[i].px[k][1] | d->f[i].px[k][2];
        at += d->f[i].ms;
        if (lit && d->loop >= 0 && i >= d->loop) loop_lit = 1;
        if (lit && (d->loop < 0 || i < d->loop)) lit_end = at;
    }
    return loop_lit ? -1 : lit_end;
}

/* lock held: how long pattern p shows now (a volume: until unset, its design only colours it) */
static int shows_ms(int p)
{
    int s = slot_of(p);
    return s >= 0 && des[s].nframes && !stepped(p) ? design_ms(&des[s]) : pats[p].ms;
}

static struct rgb seen(const unsigned char c[3]) { return (struct rgb){ c[0] / 255.f, c[1] / 255.f, c[2] / 255.f }; }

/* lock held: led[] from slot s's design, t ms after pattern p started; a volume's fill (arg) in its first frame's
 * colours, each LED as bright as the fill has it there (ring.c's tinted steps) */
static void design_draw(int s, int p, struct rgb *led, long long t, int arg)
{
    const struct rd_design *d = &des[s];
    all(led, hex(0));
    if (stepped(p)) {
        float n = arg / 100.f * LEDS;
        for (int i = 0; i < LEDS; i++) led[i] = scale(seen(d->f[0].px[(i + SHIFT) % LEDS]), clamp01(n - i));
        return;
    }
    const struct rd_frame *f = design_frame(d, t);
    if (f) for (int i = 0; i < LEDS; i++) led[i] = seen(f->px[(i + SHIFT) % LEDS]);
}

/* ---------------------------------------------------------------- the patterns as files, for the settings page
 *
 * The page starts designs from stock's animations and shows them beside the designs: here the patterns, drawn frame by
 * frame as .animation files have them (LED by LED in the design's order, the driver's duty: gamma 2.2, as Amazon's) */
#define STOCK_MS 40                      /* a frame of a gradual change */
#define STEP_MS  5                       /* looked at this often: a jump (thinking's turns, 100 ms) cut where it is */
/* how each runs: intro ms played once, then loop ms again and again (0: once, then dark) */
static const struct { int intro, loop; } runs[P_COUNT] = {
    [P_LISTEN] = { 500, STOCK_MS }, [P_THINK] = { 0, 200 }, [P_TALK] = { 0, 1690 }, [P_END] = { 600, 0 },
    [P_MUTE] = { 700, STOCK_MS }, [P_UNMUTE] = { 1100, 0 }, [P_DND] = { 1760, 0 }, [P_ERROR] = { 3300, 0 },
    [P_LOGIN] = { 0, 1800 }, [P_PAIRING] = { 0, 1060 }, [P_SETUP] = { 0, 1380 }, [P_RESET] = { 0, 830 },
    [P_TIMER] = { 0, 750 }, [P_TIMER_SHORT] = { 2233, 0 }, [P_CALL_OUT] = { 0, 900 }, [P_CALL_IN] = { 0, 5200 },
    [P_CALL] = { 0, 2400 }, [P_CALL_END] = { 1480, 0 }, [P_IDENTIFY] = { 0, 2000 },
};

static int duty(float v) { return (int)lroundf(powf(clamp01(v), 2.2f) * 255); }

static void put_frame(FILE *f, int ms, const struct rgb *led)
{
    fprintf(f, "%d:", ms);
    for (int j = 0; j < LEDS; j++) {
        struct rgb c = led[(j - SHIFT + LEDS) % LEDS];         /* the design's LED j is band position j - SHIFT */
        fprintf(f, "%s%02X%02X%02X", j ? "," : "", duty(c.r), duty(c.g), duty(c.b));
    }
    fputc('\n', f);
}

/* how far a picture moved between two looks: the largest change of any LED's channel */
static float moved(const struct rgb *a, const struct rgb *b)
{
    float d = 0;
    for (int i = 0; i < LEDS; i++) d = fmaxf(d, fmaxf(fabsf(a[i].r - b[i].r), fmaxf(fabsf(a[i].g - b[i].g), fabsf(a[i].b - b[i].b))));
    return d;
}

/* pattern p from ms `from` to `to` as frames: a new one where the picture jumps (at that very ms), or every STOCK_MS
 * while it changes gradually; what stays the same is one frame */
static void put_run(FILE *f, int p, int from, int to, int arg)
{
    struct rgb led[LEDS], last[LEDS], cur[LEDS]; int start = from;
    if (to <= from) return;
    draw(p, cur, from, arg, 0); memcpy(last, cur, sizeof cur);
    for (int t = from + STEP_MS; t < to; t += STEP_MS) {
        draw(p, led, t, arg, 0);
        if (moved(led, cur) > 0 && (moved(led, last) > 0.15f || t - start >= STOCK_MS)) {
            put_frame(f, t - start, cur); memcpy(cur, led, sizeof led); start = t;
        }
        memcpy(last, led, sizeof led);
    }
    put_frame(f, to - start, cur);
}

char *ring_stock(int s, size_t *n)
{
    int p = s >= 0 && s < ring_nslots ? pid_of(s) : -1;
    char *t = NULL; size_t len = 0;
    FILE *f = p >= 0 ? open_memstream(&t, &len) : NULL;
    if (!f) return NULL;
    if (stepped(p))                      /* a step per LED, "step NN" before each, as rd_stock gives the Dots' files */
        for (int k = 1; k <= LEDS; k++) {
            struct rgb led[LEDS];
            for (int i = 0; i < LEDS; i++) led[i] = i < k ? hex(p == P_ASSIST ? CYAN : WHITE) : hex(0);  /* as draw()'s fill */
            fprintf(f, "step %02d\n", k); put_frame(f, 1000, led);
        }
    else {
        put_run(f, p, 0, runs[p].intro, 0);
        fputs("loop\n", f);
        if (runs[p].loop) put_run(f, p, runs[p].intro, runs[p].intro + runs[p].loop, 0);
        else { struct rgb dark[LEDS]; all(dark, hex(0)); put_frame(f, 1000, dark); }    /* a dark loop: once */
    }
    if (fclose(f)) { free(t); return NULL; }
    *n = len;
    return t;
}

/* Home Assistant's light as ring.c gives it the ring: pulse 2.4 s from 35 %, two comets over the colour at 25 % once a
 * second, blink 0.5 s, the colour wheel turning in 2 s */
static void light_draw(const struct ring_light *l, int blink, struct rgb *led, long long t)
{
    struct rgb c = scale((struct rgb){ l->r, l->g, l->b }, l->brightness * l->color_brightness);
    switch (blink ? E_BLINK : l->effect) {
    case E_PULSE: all(led, scale(c, 0.35f + 0.65f * wave(t, 2400))); break;
    case E_SPIN:
        for (int i = 0; i < LEDS; i++) {
            float p = (float)(t % 1000) / 1000 * LEDS, d1 = fmodf(p - i + 2 * LEDS, LEDS), d2 = fmodf(p + LEDS / 2 - i + 2 * LEDS, LEDS), d = fminf(d1, d2);
            led[i] = scale(c, d < 1 ? 1 : d < 2 ? 0.65f : d < 3 ? 0.42f : 0.25f);
        }
        break;
    case E_BLINK: all(led, t % 1000 < 500 ? c : hex(0)); break;
    case E_RAINBOW: for (int i = 0; i < LEDS; i++) led[i] = scale(hue((float)i / LEDS - (float)(t % 2000) / 2000), l->brightness); break;
    default: all(led, c);
    }
}

/* ---------------------------------------------------------------- the screen: power, and the renderer process */

static void run_quiet(char *const argv[]) { core_spawn(argv); }

static void screen_wake(void)
{
    char ms[24]; snprintf(ms, sizeof ms, "%lld", now_ms());           /* uptimeMillis is CLOCK_MONOTONIC */
    char *wake[] = { "/system/bin/service", "call", "power", "9", "i64", ms, "s16", "hassmic", "s16", "android", NULL };
    char *act_[] = { "/system/bin/service", "call", "power", "8", "i64", ms, "i32", "0", "i32", "0", NULL };   /* userActivity */
    run_quiet(wake); run_quiet(act_);
}

static void screen_timeout(int ms)
{
    char v[16]; snprintf(v, sizeof v, "%d", ms);
    char *argv[] = { "/system/bin/settings", "put", "system", "screen_off_timeout", v, NULL };
    run_quiet(argv);
    fprintf(stderr, "band: the screen sleeps after %d s\n", ms / 1000);
}

static const char *dex_path(void)
{
    static char p[PATH_MAX];
    const char *e = getenv("HASSMIC_RINGBAR");
    if (e) return e;
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 32);
    if (n <= 0) return "/data/local/hassmic/ringbar.dex";
    p[n] = 0;
    char *s = strrchr(p, '/'); strcpy(s ? s + 1 : p, "ringbar.dex");   /* next to the binary, as bundles put it */
    return p;
}

/* Android 8 and later (athena's 9) have no SurfaceControl constructor: the renderer is told to use SurfaceControl.Builder
 * (tools/mkringbar.py, argv[2]).  The PC has no properties and the old way, as the test's stand-in expects. */
static int builder_api(void)
{
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";
    return __system_property_get("ro.build.version.sdk", v) > 0 && atoi(v) >= 26;
#else
    return 0;
#endif
}

#define VIA_SCREEN -2                   /* "fd" of the screen UI's band (screen_band) */

/* the renderer for `n` rows: its stdin in *in (VIA_SCREEN: the screen UI), the band's width in *w (0: band off).  0, 1
 * off with the screen UI's overscan, or -1 when it did not come up, -2 the screen UI not up yet (asked again a second
 * later: the thread goes on meanwhile).  n = -p: the text layer, p % of the screen's height, its rows in *h: always a
 * ringbar.dex of its own (the screen UI draws the band only), its layer under the band's and the UI's */
static int ringbar(int n, int *in, int *w, int *h)
{
    if (n >= 0 && !getenv("HASSMIC_FAKE_BAND") && screen_ui()) {   /* the screen UI draws it: its width once it is up */
        if (!n) { *in = -1; *w = 0; return 1; }               /* (it resets the overscan itself, started anew) */
        for (int i = 0; i < 10 && !(*w = screen_width()); i++) usleep(100000);
        if (!*w) return -2;
        if (*w < 0 || *w > MAX_W) {                           /* the UI's word, as the renderer's below */
            fprintf(stderr, "band: the screen UI's width is out of range: %d\n", *w); *w = 0; return -1;
        }
        *in = VIA_SCREEN;
        fprintf(stderr, "band: %d x %d at the bottom of the screen, drawn by the screen UI\n", *w, n);
        return 0;
    }
    int pin[2], pout[2]; char rs[8]; snprintf(rs, sizeof rs, "%d", n);
    const char *fake = getenv("HASSMIC_FAKE_BAND"), *dex = dex_path();
    int builder = builder_api();        /* before the fork: a property lookup takes bionic's lock, which another thread may hold */
    /* the child's environment made here too, CLASSPATH in place of any: putenv after the fork allocates, and malloc's
     * lock may be another thread's at that moment */
    extern char **environ;
    size_t ne = 0; while (environ[ne]) ne++;
    char cp[PATH_MAX + 16], **envp = malloc((ne + 2) * sizeof *envp);
    if (!envp) return -1;
    snprintf(cp, sizeof cp, "CLASSPATH=%s", dex);
    size_t m = 0; envp[m++] = cp;
    for (size_t i = 0; i < ne; i++) if (strncmp(environ[i], "CLASSPATH=", 10)) envp[m++] = environ[i];
    envp[m] = NULL;
    /* The renderer gets these as its stdin and stdout and nothing else of hassmic's: its listening sockets and the log's
     * pipe it would hold until it exits (ports bound, no EOF for the log's reader).  The pipes close-on-exec, as ledq.c's
     * (dup2 clears the flag on 0 and 1); every other descriptor the child closes before exec, from a bound found here */
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
    /* what to run, made before the fork too: after it only async-signal-safe calls (execve, not execle) */
    char *argv_[7] = { NULL }; const char *prog;
    if (fake) { prog = fake; argv_[0] = (char *)fake; argv_[1] = rs; }
    else {
        prog = "/system/bin/app_process";
        argv_[0] = "app_process"; argv_[1] = "/"; argv_[2] = "Ringbar"; argv_[3] = rs; argv_[4] = builder ? "builder" : NULL;
    }
    if (pipe2(pin, O_CLOEXEC)) { free(envp); return -1; }
    if (pipe2(pout, O_CLOEXEC)) { close(pin[0]); close(pin[1]); free(envp); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        /* each end first to a descriptor of 3 or more: a daemon with 0 or 1 closed got them from pipe2, and a dup2 onto
         * itself would keep close-on-exec; F_DUPFD's copy has none, nor have dup2's */
        int in = fcntl(pin[0], F_DUPFD, 3), out = fcntl(pout[1], F_DUPFD, 3);
        if (in < 0 || out < 0 || dup2(in, 0) < 0 || dup2(out, 1) < 0) _exit(127);
        for (long d = 3; d < maxfd; d++) close((int)d);      /* stderr stays: its lines are in the log */
        execve(prog, argv_, envp);
        _exit(127);
    }
    free(envp);
    close(pin[0]); close(pout[1]);
    if (pid < 0) { close(pin[1]); close(pout[0]); return -1; }
    char line[64]; size_t k = 0; long long until = now_ms() + 15000;      /* ART's start: ~1 s */
    while (k + 1 < sizeof line && now_ms() < until) {
        struct pollfd p = { pout[0], POLLIN, 0 };
        if (poll(&p, 1, 500) <= 0) continue;
        ssize_t r = read(pout[0], line + k, 1);
        if (r <= 0) break;
        if (line[k] == '\n') break;
        k++;
    }
    line[k] = 0; close(pout[0]);
    int ww = 0, hh = 0;
    if (!strcmp(line, "off") && !n) { close(pin[1]); *w = 0; *in = -1; return 0; }
    if (sscanf(line, "ready %d %d", &ww, &hh) != 2 || ww <= 0 || ww > MAX_W || (n >= 0 ? hh != n : hh <= 0 || hh > MAX_TEXT_ROWS)) {
        fprintf(stderr, "band: the renderer did not come up (%s): %s\n", line[0] ? line : "no answer", dex);
        close(pin[1]); return -1;
    }
    *in = pin[1]; *w = ww; if (h) *h = hh;
    fprintf(stderr, n >= 0 ? "band: %d x %d at the bottom of the screen\n" : "text: %d x %d over the screen\n", ww, hh);
    return 0;
}

static int send_frame(int fd, const struct rgb *row, int w, int n, float k)
{
    static uint8_t buf[MAX_W * MAX_ROWS * 4];
    for (int x = 0; x < w; x++) {
        uint8_t *p = buf + 4 * x;                   /* 0xAARRGGBB, little-endian */
        p[0] = (uint8_t)lroundf(clamp01(row[x].b * k) * 255); p[1] = (uint8_t)lroundf(clamp01(row[x].g * k) * 255);
        p[2] = (uint8_t)lroundf(clamp01(row[x].r * k) * 255); p[3] = 255;
    }
    if (fd == VIA_SCREEN) return screen_band(buf, w);          /* one row: the UI repeats it; -1: the UI was given up */
    for (int y = 1; y < n; y++) memcpy(buf + (size_t)y * w * 4, buf, (size_t)w * 4);
    size_t len = (size_t)w * n * 4, off = 0;
    while (off < len) {
        ssize_t r = write(fd, buf + off, len - off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

static void *band_thread(void *arg)
{
    (void)arg;
    static struct rgb row[MAX_W], last[MAX_W];
    int fd = -1, w = 0, have_rows = -1, sent = 0, timeout = 0, fails = 0, waited = 0, off_ui = 0;
    float last_k = -1;                              /* the brightness of the frame sent last */
    long long last_wake = 0, next_check = 0;
    for (;;) {
        pthread_mutex_lock(&lock);
        int want = rows, wake = wake_req; wake_req = 0;
        pthread_mutex_unlock(&lock);
        long long now = now_ms();
        if (now >= next_check) {                    /* stock's screen timeouts by what the Echo is doing (first: also
                                                       while a renderer is awaited) */
            int t = atomic_load(&screen_hold) ? HOLD_MS : core_muted() || core_dnd(-1) ? QUIET_MS : TIMEOUT_MS;
            if (t != timeout) { timeout = t; screen_timeout(t); }
            next_check = now + 1000;
        }
        if (wake && now - last_wake >= WAKE_EVERY) { last_wake = now; screen_wake(); }
        if (want != have_rows) {                    /* (re)start the renderer for the rows set */
            if (fd >= 0) close(fd);                 /* it ends at the end of its input; the layer goes with it */
            fd = -1; w = 0; sent = 0;
            int rc = ringbar(want, &fd, &w, NULL);
            if (rc == -2) {                         /* the screen UI on its way (screen.c gives it 30 s): soon again */
                if (!waited++) fprintf(stderr, "band: waiting for the screen UI\n");
                pthread_mutex_lock(&lock);
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
                if (rows == want) pthread_cond_timedwait(&cv, &lock, &ts);
                pthread_mutex_unlock(&lock);
                continue;
            }
            waited = 0; off_ui = rc == 1;
            if (rc < 0) {
                if (!fails++) fprintf(stderr, "band: no renderer, trying again every 30 s\n");
                pthread_mutex_lock(&lock);          /* a new height ("Light band") tries again at once */
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 30;
                if (rows == want) pthread_cond_timedwait(&cv, &lock, &ts);
                pthread_mutex_unlock(&lock);
                continue;
            }
            fails = 0; have_rows = want;
            if (!want) fprintf(stderr, "band: off, the whole screen is the apps'\n");
        }
        struct rgb led[LEDS];
        pthread_mutex_lock(&lock);
        now = now_ms();                             /* under the lock: a ring_set just before would be in its future */
        int p = top(now), arg_ = p < P_COUNT ? act[p].arg : 0, blink = flash_until && now < flash_until, moving = 1;
        long long at = p < P_COUNT ? act[p].at : 0;
        struct ring_light l = blink ? flash : light;
        if (!blink) flash_until = 0;
        wake = wake_req; wake_req = 0;
        int s = p < P_COUNT ? slot_of(p) : -1, designed = s >= 0 && des[s].nframes;
        if (designed) design_draw(s, p, led, now - at, arg_);      /* here: ring_reload frees the designs it replaces */
        pthread_mutex_unlock(&lock);
        if (wake && now - last_wake >= WAKE_EVERY) { last_wake = now; screen_wake(); }
        if (fd == VIA_SCREEN && !screen_ui()) {     /* the screen UI given up (screen.c): ringbar.dex draws the band now */
            fprintf(stderr, "band: the screen UI was given up, the band's own renderer takes over\n");
            fd = -1; have_rows = -1; continue;
        }
        if (fd == -1) {                             /* band off: nothing to draw, wait for a change (a second: the */
            if (off_ui && !screen_ui()) {           /* timeouts above, and a screen UI given up, whose top rows of */
                off_ui = 0; have_rows = -1; continue;   /* overscan (its grip) ringbar.dex's "off" gives back) */
            }
            pthread_mutex_lock(&lock);
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
            if (rows == have_rows && !wake_req) pthread_cond_timedwait(&cv, &lock, &ts);
            pthread_mutex_unlock(&lock);
            continue;
        }
        int lv = atomic_load(&level_db);            /* the talker: -50 dBFS nothing, -20 the widest */
        float target = clamp01((lv / 10.f + 50) / 30);
        loud += (target - loud) * (target > loud ? 0.35f : 0.08f);
        if (!designed) {
            if (p < P_COUNT) draw(p, led, now - at, arg_, loud);
            else if (l.on || blink) { light_draw(&l, blink, led, now); moving = blink || l.effect != E_NONE; }  /* a still
                                                     * light: no frames 30 times a second, the 1 s wait below takes a new level */
            else { all(led, hex(0)); moving = 0; }
        }
        unroll(led, row, w);
        float k = core_led_brightness(-1) / 100.f;
        if (!sent || k != last_k || memcmp(row, last, sizeof row[0] * w)) {    /* the level too: a still light dims */
            if (send_frame(fd, row, w, have_rows, k) < 0) {
                fprintf(stderr, "band: the renderer is gone, starting it again\n");
                if (fd >= 0) close(fd);
                fd = -1; have_rows = -1; sleep(1); continue;
            }
            memcpy(last, row, sizeof row[0] * w); last_k = k; sent = 1;
        }
        pthread_mutex_lock(&lock);
        if (moving) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += FRAME_MS * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&cv, &lock, &ts);
        } else {                                    /* dark: until something changes, or the timeout check is due */
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
            if (!wake_req) pthread_cond_timedwait(&cv, &lock, &ts);
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

/* ---------------------------------------------------------------- the text layer
 * What was said and the reply (ring.h ring_text), and the timers (ring_timer), drawn by textdraw.c and shown by a second
 * ringbar.dex in its text mode ("-100"): see-through, the whole screen's height, layered just under the band, whose rows
 * it leaves clear.  A process of its own, so the band's 30 frames a second never queue behind a frame of a megabyte or
 * two; started with either setting and kept while one is on (ART takes ~1 s to start, about what speech to text takes).
 * One picture at a time: the words while they show, else what rings, else the running timers.  Drawn on a change only (a
 * running timer: once a second); fading scales its alpha.  Words come at once and go 8 s ("Speech stays for") after the
 * satellite is idle again, a new request takes the last one's words away at once; a ringing timer pulses.  While timers show, the screen
 * stays on (the band thread's timeout): a timer across the room is what the screen is for then. */
#define TEXT_PCT      100              /* the whole screen above the band: 55 % was small (cronos, 2026-11-03) */
#define FADE_IN_MS    200
#define FADE_OUT_MS   600
#define TEXT_FRAME_MS 50                /* 20 frames a second while it fades */
#define PULSE_MS      600               /* a ringing timer: bright and dim by turns */
#define PULSE_LOW     0.45f
#define TIMERS_MAX    6

static int text_on, timers_on, text_gen, text_idle, text_hold_s = 8;    /* lock; text_hold_s: "Speech stays for", 0 till the next */
static long long text_idle_at;
static char text_heard[600], text_reply[1100], timer_ringing[288];
struct shown_timer { char id[40], name[64]; int total, left, active; long long at; };
static struct shown_timer timers[TIMERS_MAX];
static int ntimers;

void ring_text(int what, const char *text)
{
    pthread_mutex_lock(&lock);
    if (what == RING_TEXT_RUN) { text_heard[0] = text_reply[0] = 0; text_idle = 0; text_gen++; }
    else if (what == RING_TEXT_HEARD) {             /* a follow-up's words: the last reply goes with the last words */
        snprintf(text_heard, sizeof text_heard, "%s", text ? text : ""); text_reply[0] = 0; text_idle = 0; text_gen++;
    } else if (what == RING_TEXT_REPLY) { snprintf(text_reply, sizeof text_reply, "%s", text ? text : ""); text_idle = 0; text_gen++; }
    else if (what == RING_TEXT_IDLE && !text_idle) { text_idle = 1; text_idle_at = now_ms(); }
    if (text_on && what != RING_TEXT_IDLE && text && *text) { wake_req = 1; pthread_cond_broadcast(&cv); }  /* the screen on */
    pthread_cond_broadcast(&tcv);
    pthread_mutex_unlock(&lock);
}

int ring_text_on(int set)
{
    pthread_mutex_lock(&lock);
    if (set >= 0 && !!set != text_on) { text_on = !!set; text_gen++; pthread_cond_broadcast(&tcv); }
    int r = text_on;
    pthread_mutex_unlock(&lock);
    return r;
}

int ring_text_hold(int set)
{
    pthread_mutex_lock(&lock);
    if (set >= 0 && set != text_hold_s) { text_hold_s = set; text_gen++; pthread_cond_broadcast(&tcv); }
    int r = text_hold_s;
    pthread_mutex_unlock(&lock);
    return r;
}

int ring_timers_on(int set)
{
    pthread_mutex_lock(&lock);
    if (set >= 0 && !!set != timers_on) { timers_on = !!set; text_gen++; pthread_cond_broadcast(&tcv); }
    int r = timers_on;
    pthread_mutex_unlock(&lock);
    return r;
}

/* lock held: a change the screen shows, woken for it */
static void timers_changed(int wake)
{
    text_gen++;
    if (timers_on && wake) { wake_req = 1; pthread_cond_broadcast(&cv); }
    pthread_cond_broadcast(&tcv);
}

void ring_timer(const char *id, const char *name, int total, int left, int active)
{
    pthread_mutex_lock(&lock);
    int i = 0;
    while (i < ntimers && strcmp(timers[i].id, id)) i++;
    if (i == TIMERS_MAX) { memmove(timers, timers + 1, sizeof timers[0] * --i); ntimers = i; }     /* the oldest goes */
    int added = i == ntimers;
    if (added) ntimers++;
    struct shown_timer *t = &timers[i];
    snprintf(t->id, sizeof t->id, "%s", id);
    /* "a timer for 5 minutes" has no name: its length stands in, as core_timer_finished words it */
    unsigned h = (unsigned)total / 3600, m = (unsigned)total % 3600 / 60;
    if (name && name[0]) snprintf(t->name, sizeof t->name, "%s", name);
    else if (total % 60 || !total) snprintf(t->name, sizeof t->name, "%d s", total);
    else if (h && m) snprintf(t->name, sizeof t->name, "%u h %u min", h, m);
    else if (h) snprintf(t->name, sizeof t->name, "%u h", h);
    else snprintf(t->name, sizeof t->name, "%u min", m);
    t->total = total; t->left = left; t->active = active; t->at = now_ms();
    timers_changed(added);
    pthread_mutex_unlock(&lock);
}

void ring_timer_gone(const char *id)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < ntimers; i++) if (!strcmp(timers[i].id, id)) {
        memmove(timers + i, timers + i + 1, sizeof timers[0] * (size_t)(ntimers - i - 1));
        ntimers--;
        timers_changed(0);
        break;
    }
    pthread_mutex_unlock(&lock);
}

void ring_timers_ringing(const char *names)
{
    pthread_mutex_lock(&lock);
    if (strcmp(timer_ringing, names ? names : "")) {
        snprintf(timer_ringing, sizeof timer_ringing, "%s", names ? names : "");
        timers_changed(timer_ringing[0] != 0);
    }
    pthread_mutex_unlock(&lock);
}

/* the picture at alpha a (0..1) to the renderer: each pixel's own alpha scaled.  0, or -1 when it is gone */
static int send_text(int fd, const uint32_t *px, uint8_t *buf, int w, int h, float a)
{
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = px[i]; uint8_t *b = buf + 4 * i;          /* 0xAARRGGBB, little-endian */
        b[0] = (uint8_t)(p & 255); b[1] = (uint8_t)(p >> 8 & 255); b[2] = (uint8_t)(p >> 16 & 255);
        b[3] = (uint8_t)lroundf((p >> 24) * a);
    }
    for (size_t off = 0, len = n * 4; off < len; ) {
        ssize_t r = write(fd, buf + off, len - off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

static int by_left(const void *a, const void *b)
{
    const struct textdraw_timer *x = a, *y = b;
    return x->left - y->left;
}

enum { SHOW_NONE, SHOW_WORDS, SHOW_RING, SHOW_TIMERS };

static void *text_thread(void *arg)
{
    (void)arg;
    int fd = -1, w = 0, h = 0, gen = -1, kept = -1, shown = SHOW_NONE, shown_sec = -1, fails = 0, held = 0;
    uint32_t *px = NULL; uint8_t *buf = NULL;
    float alpha = 0, sent = -1;
    long long last = now_ms();
    static char heard[sizeof text_heard], reply[sizeof text_reply], ring[sizeof timer_ringing];
    static struct shown_timer tm[TIMERS_MAX];
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!text_on && !timers_on && fd < 0) pthread_cond_wait(&tcv, &lock);
        int words_on = text_on, tim_on = timers_on, g = text_gen, keep = rows, idle = text_idle, n = ntimers;
        long long hold = text_hold_s ? text_hold_s * 1000LL : LLONG_MAX / 2;    /* 0: until the next request */
        long long idle_at = text_idle_at;
        memcpy(heard, text_heard, sizeof heard); memcpy(reply, text_reply, sizeof reply);
        memcpy(ring, timer_ringing, sizeof ring); memcpy(tm, timers, sizeof tm);
        pthread_mutex_unlock(&lock);
        if (!words_on && !tim_on) {                 /* both off: the renderer ends at the end of its input */
            close(fd); fd = -1; free(px); free(buf); px = NULL; buf = NULL;
            gen = kept = shown_sec = -1; shown = SHOW_NONE; alpha = 0; sent = -1;
            if (held) { atomic_store(&screen_hold, 0); held = 0; }
            fprintf(stderr, "text: off\n");
            continue;
        }
        if (fd < 0) {
            if (textdraw_init() < 0 || ringbar(-TEXT_PCT, &fd, &w, &h) < 0) {
                if (!fails++) fprintf(stderr, "text: no renderer, trying again every 30 s\n");
                pthread_mutex_lock(&lock);
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 30;
                if (text_on || timers_on) pthread_cond_timedwait(&tcv, &lock, &ts);
                pthread_mutex_unlock(&lock);
                continue;
            }
            fails = 0;
            px = malloc((size_t)w * h * 4); buf = malloc((size_t)w * h * 4);
            if (!px || !buf) { fprintf(stderr, "text: no memory for %d x %d\n", w, h); close(fd); fd = -1; sleep(30); continue; }
            gen = kept = shown_sec = -1; shown = SHOW_NONE; sent = -1;
        }
        long long now = now_ms();
        int words = words_on && (heard[0] || reply[0]) && (!idle || now - idle_at < hold);
        int show = words ? SHOW_WORDS : tim_on && ring[0] ? SHOW_RING : tim_on && n ? SHOW_TIMERS : SHOW_NONE;
        int sec = show == SHOW_TIMERS ? (int)(now / 1000) : -1;    /* a running timer: a new picture each second */
        if (show != SHOW_NONE && (show != shown || g != gen || keep != kept || sec != shown_sec)) {
            if (show == SHOW_WORDS) textdraw_render(px, w, h, keep, heard, reply);
            else if (show == SHOW_RING) {
                struct textdraw_timer t = { ring, 0, 0, 1 };
                textdraw_timers(px, w, h, keep, &t, 1, 1);
            } else {
                struct textdraw_timer t[TIMERS_MAX];
                for (int i = 0; i < n; i++) {           /* counted down here from what Home Assistant last said */
                    int left = tm[i].active ? tm[i].left - (int)((now - tm[i].at) / 1000) : tm[i].left;
                    t[i] = (struct textdraw_timer){ tm[i].name, left < 0 ? 0 : left, tm[i].total, tm[i].active };
                }
                qsort(t, (size_t)n, sizeof t[0], by_left);     /* the one that ends first on top */
                textdraw_timers(px, w, h, keep, t, n, 0);
            }
            gen = g; kept = keep; shown_sec = sec; sent = -1;
        }
        if (show == SHOW_NONE && shown == SHOW_WORDS && !heard[0] && !reply[0]) alpha = 0;     /* a new request: at once */
        shown = show;
        float target = show == SHOW_NONE ? 0 : show == SHOW_RING && now / PULSE_MS % 2 ? PULSE_LOW : 1;
        float dt = (float)(now - last); last = now;
        if (dt > 2 * TEXT_FRAME_MS) dt = TEXT_FRAME_MS;  /* a fade starts from where it was, not from the last wait */
        if (alpha < target) alpha = fminf(target, alpha + dt / FADE_IN_MS);
        else if (alpha > target) alpha = fmaxf(target, alpha - dt / FADE_OUT_MS);
        if (alpha != sent) {
            if (send_text(fd, px, buf, w, h, alpha) < 0) {
                fprintf(stderr, "text: the renderer is gone, starting it again\n");
                close(fd); fd = -1; free(px); free(buf); px = NULL; buf = NULL; sleep(1); continue;
            }
            sent = alpha;
        }
        int awake = show == SHOW_RING || show == SHOW_TIMERS;
        if (awake != held) { atomic_store(&screen_hold, awake); held = awake; }
        pthread_mutex_lock(&lock);
        if (text_gen == g && rows == keep && text_idle == idle) {
            long long ms = alpha != target ? TEXT_FRAME_MS                       /* fading */
                         : show == SHOW_RING ? PULSE_MS - now % PULSE_MS        /* the next pulse */
                         : show == SHOW_TIMERS ? 1000 - now % 1000              /* the next second */
                         : show == SHOW_WORDS && idle && text_hold_s ? idle_at + hold - now    /* until the words go */
                         : -1;                                                  /* until something changes */
            if (ms >= 0) {
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += ms / 1000; ts.tv_nsec += (ms % 1000) * 1000000L;
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&tcv, &lock, &ts);
            } else pthread_cond_wait(&tcv, &lock);
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

/* ---------------------------------------------------------------- ring.h */

int ring_generate(const char *dir) { (void)dir; fprintf(stderr, "ring: no LED ring on this model (the band is drawn at run time)\n"); return 1; }

void ring_start(void (*run)(const char *op, const char *name), void (*reloaded)(int custom_changed))
{
    (void)run; (void)reloaded;                      /* no ledctrl here, and no set root installs */
    pthread_mutex_lock(&lock);
    int go = !started; started = 1;
    pthread_mutex_unlock(&lock);
    if (!go) return;
    ring_reload();                                  /* the designs applied before */
    pthread_t t;
    if (pthread_create(&t, NULL, band_thread, NULL)) {
        fprintf(stderr, "band: no thread\n");
        pthread_mutex_lock(&lock); started = 0; pthread_mutex_unlock(&lock);   /* a later ring_start tries again */
        return;
    }
    pthread_detach(t);
    if (pthread_create(&t, NULL, text_thread, NULL)) fprintf(stderr, "text: no thread\n");
    else pthread_detach(t);
}

int ring_custom(void) { return 1; }                 /* Home Assistant's light: drawn here, any colour */
int ring_assistant_steps(void) { return 1; }        /* the replies' own volume in cyan (P_ASSIST) */
/* Each LED's own layer of the Dots' ledcontroller (ring.c: the fill, the rotation, LED by LED): not on the band, the
 * rotation stays 0 */
int ring_per_led(void) { return 0; }
int ring_effect_listed(int i) { return i >= 0 && i < ring_neffects; }
int ring_slot_available(int i) { return i >= 0 && i < ring_nslots; }      /* every slot is one of the patterns */
int ring_slot_live(int i) { if (i < 0 || i >= ring_nslots) return 0; pthread_mutex_lock(&lock); int r = des[i].nframes > 0; pthread_mutex_unlock(&lock); return r; }
int ring_leds(void) { return LEDS; }
int ring_rotation(int set) { (void)set; return 0; }
int ring_designs_live(void) { return 1; }

int ring_check(const char *applied, size_t n, char *err, size_t cap)
{
    struct rd_design ds[RING_SLOTS_MAX]; int ids[RING_SLOTS_MAX], rc = 0;
    memset(ds, 0, sizeof ds);
    if (rd_applied_parse(applied, n, ds, ids)) { snprintf(err, cap, "not a set of designs"); rc = -1; }
    for (int i = 0; i < ring_nslots; i++) {
        if (!rc && ds[i].nframes && ds[i].leds != LEDS) {
            snprintf(err, cap, "%s: drawn for %d LEDs, the band shows the ring's %d", ring_slots[i].id, ds[i].leds, LEDS);
            rc = -1;
        }
        rd_free(&ds[i]);
    }
    return rc;
}

static int same_design(const struct rd_design *a, const struct rd_design *b)
{
    return a->nframes == b->nframes && a->loop == b->loop && a->leds == b->leds
        && (!a->nframes || !memcmp(a->f, b->f, sizeof *a->f * (size_t)a->nframes));
}

/* state/ring-installed's designs from now on (an Apply: ringdesign.c wrote it); what shows with a changed design starts
 * again with it, the rest goes on */
void ring_reload(void)
{
    struct rd_design ds[RING_SLOTS_MAX]; int ids[RING_SLOTS_MAX], nd = 0; size_t n = 0;
    char *t = rd_installed_read(&n);
    memset(ds, 0, sizeof ds);
    if (t && rd_applied_parse(t, n, ds, ids) != 0)                  /* not ours: what it parsed of it goes */
        for (int i = 0; i < ring_nslots; i++) rd_free(&ds[i]);
    free(t);
    for (int i = 0; i < ring_nslots; i++) {
        if (ds[i].nframes && ds[i].leds != LEDS) { fprintf(stderr, "band: the design for %s is for %d LEDs, left out\n", ring_slots[i].id, ds[i].leds); rd_free(&ds[i]); }
        nd += ds[i].nframes > 0;
    }
    pthread_mutex_lock(&lock);
    int changed[RING_SLOTS_MAX];
    for (int i = 0; i < ring_nslots; i++) { changed[i] = !same_design(&des[i], &ds[i]); rd_free(&des[i]); des[i] = ds[i]; }
    long long now = now_ms();
    for (int i = 0; i < P_COUNT; i++) {                 /* what shows: from its start again only where its design changed */
        int s = slot_of(i);
        if (!act[i].on || (act[i].ms >= 0 && now >= act[i].at + act[i].ms) || s < 0 || !changed[s]) continue;
        act[i].at = now; act[i].ms = shows_ms(i);
    }
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "band: designs in %d place%s\n", nd, nd == 1 ? "" : "s");
}

void ring_set(const char *pattern)
{
    int arg, i = find(pattern, &arg);
    if (i < 0) { fprintf(stderr, "band: no picture for \"%s\"\n", pattern); return; }
    pthread_mutex_lock(&lock);
    if (!(act[i].on && stepped(i))) act[i].at = now_ms();        /* a volume step moves the fill, not a new start */
    act[i].on = 1; act[i].arg = arg; act[i].ms = shows_ms(i); wake_req = 1;
    if (i == P_LISTEN) atomic_store(&level_db, -1000);              /* the last command's loudness is not this one's */
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lock);
}

void ring_unset(const char *pattern)
{
    int i = find(pattern, NULL);
    if (i < 0) return;
    pthread_mutex_lock(&lock);
    act[i].on = 0;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lock);
}

void ring_tick(void) {}

void ring_light_get(struct ring_light *l) { pthread_mutex_lock(&lock); *l = light; pthread_mutex_unlock(&lock); }

void ring_light_set(const struct ring_light *l, unsigned flash_ms)
{
    pthread_mutex_lock(&lock);
    if (flash_ms) { flash = *l; flash.on = 1; flash_until = now_ms() + flash_ms; }
    else light = *l;
    if (light.effect < 0 || light.effect >= ring_neffects) light.effect = E_NONE;
    if (light.on || flash_ms) wake_req = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lock);
}

void ring_level(int dbfs10) { atomic_store(&level_db, dbfs10); }

int ring_band_rows(int set)
{
    pthread_mutex_lock(&lock);
    if (set >= 0) { rows = set > MAX_ROWS ? MAX_ROWS : set; pthread_cond_signal(&cv); pthread_cond_broadcast(&tcv); }
    int r = rows;
    pthread_mutex_unlock(&lock);
    return r;
}
