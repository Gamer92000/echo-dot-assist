/*
 * The light ring of a model without one (ring.h): a band along the bottom of the screen, the Echo Show 5's.  Stock
 * shows Alexa's attention there too ("voice chrome": SpeechInteractionManager.apk, BambooAttentionSystemVisualsComponent,
 * Lottie compositions of 960x200 in assets/landscape/), as a 10 px line along the bottom edge drawn over the app in
 * front.  Here the band is reserved instead, so no app ever has it under it and any app can be the screen's UI:
 *
 *   ringbar  scripts/device/ringbar.dex (tools/mkringbar.py), run as app_process by this file: it sets the window
 *            manager's overscan for the band (every app is laid out above it; nothing of theirs draws there), puts a
 *            SurfaceFlinger layer of its own over it, says "ready <width> <rows>" and draws the frames it is sent on
 *            stdin (width x rows ARGB, little-endian), until stdin closes.  It knows nothing of patterns.
 *   patterns the core's (stock's ledcontroller names, main.c led()), drawn here: listening, thinking, mic off and calls
 *            as stock's chrome draws them (its colours read off the compositions; the listening bar widens and whitens
 *            with the talker's loudness, as its ListeningRamp is driven); what only the Dots' ring shows (replies,
 *            volume, timers, Do not disturb, setup, identify, errors) in the ring's colours, laid out in a line.  One
 *            pattern shows at a time, by priority; Home Assistant's light only while none does, as on the ring.
 *   screen   stock's power policy (EnvironmentContextService, ECStateMachine): the screen sleeps 5 min after the last
 *            activity, 1 min with the mic off or in Do not disturb; a pattern wakes it (IPowerManager.wakeUp, through the
 *            service tool: transaction 9, isInteractive 12 on 8149, checked on the first Echo Show).  An app in front
 *            keeps its own wake locks.
 */
#include "ring.h"
#include "board.h"
#include "core.h"
#include <errno.h>
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

enum pid { P_RESET, P_VOLUME, P_LISTEN, P_THINK, P_TALK, P_END, P_CALL_OUT, P_CALL_IN, P_CALL, P_CALL_END, P_ERROR,
           P_MUTE, P_UNMUTE, P_DND, P_IDENTIFY, P_LOGIN, P_PAIRING, P_TIMER, P_TIMER_SHORT, P_SETUP, P_COUNT };
/* priority order (first wins: the volume over a reply, as the ring shows it) and how long each shows: -1 until unset */
static const struct { const char *name; int ms; } pats[P_COUNT] = {
    [P_RESET] = { "factory-reset", -1 },
    [P_LISTEN] = { "active-start", -1 }, [P_THINK] = { "active-thinking", -1 }, [P_TALK] = { "active-talking", -1 },
    [P_END] = { "active-end", 600 },
    [P_CALL_OUT] = { "call_outbound_ringing", -1 }, [P_CALL_IN] = { "call_incoming", -1 },
    [P_CALL] = { "call_connected", -1 }, [P_CALL_END] = { "call_connected_end", 1480 },
    [P_VOLUME] = { "volume_step-", -1 },             /* a prefix: the step follows */
    [P_ERROR] = { "anim_start_error_short", 3300 },
    [P_MUTE] = { "mics-off_on", -1 }, [P_UNMUTE] = { "mics-off_end", 1100 },
    [P_DND] = { "do_not_disturb", 1760 },
    [P_IDENTIFY] = { "zzz_rainbow", -1 },
    [P_LOGIN] = { "authenticated_setup_mode", -1 },
    [P_PAIRING] = { "scone-setup", -1 },
    [P_TIMER] = { "ready-timer", -1 }, [P_TIMER_SHORT] = { "ready-timer-short", 2233 },
    [P_SETUP] = { "setup-mode", -1 },
};

/* Each as the Dot's file of that name plays it (timings and colours read off donut's led-resources, 2026-10-13) */
static void draw(int p, struct rgb *led, long long t, int arg)
{
    all(led, hex(0));
    switch (p) {
    case P_LISTEN:                       /* blue fills from the far side round to the talker, cyan in front, in 0.5 s;
                                            then blue with cyan at the talker, wider while they speak up (ours) */
        for (int i = 0; i < LEDS; i++) {
            float front = clamp01(t / 500.f) * (LEDS / 2), e = from_end(i);
            if (t < 500) led[i] = e < front - 1 ? hex(BLUE) : e < front ? hex(CYAN) : hex(0);
            else led[i] = mix(hex(BLUE), hex(CYAN), clamp01(1 + loud * 2.5f - from_centre(i)));
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
    case P_VOLUME: {                     /* white LEDs filled from the left, as many as the level */
        float n = arg / 100.f * LEDS;
        for (int i = 0; i < LEDS; i++) led[i] = scale(hex(WHITE), clamp01(n - i));
        break; }
    case P_IDENTIFY: for (int i = 0; i < LEDS; i++) led[i] = hue((float)i / LEDS - (float)(t % 2000) / 2000); break;
    }
}

/* ---------------------------------------------------------------- state */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static struct { long long at; int arg, on; } act[P_COUNT];
static struct ring_light light = { .on = 0, .brightness = 1, .color_brightness = 1, .r = 1, .g = 1, .b = 1, .effect = E_NONE, .fill = 100 }, flash;
static long long flash_until;
static int rows = 10, wake_req, started;

static int find(const char *pattern, int *arg)
{
    for (int i = 0; i < P_COUNT; i++) {
        size_t n = strlen(pats[i].name);
        if (i == P_VOLUME ? !strncmp(pattern, pats[i].name, n) : !strcmp(pattern, pats[i].name)) {
            if (arg) *arg = i == P_VOLUME ? atoi(pattern + n) * 100 / (board.volume_steps > 0 ? board.volume_steps : 100) : 0;
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
        if (pats[i].ms >= 0 && now >= act[i].at + pats[i].ms) { act[i].on = 0; continue; }
        return i;
    }
    return P_COUNT;
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

/* the renderer for `n` rows: its stdin in *in, the band's width in *w (0: band off).  0, or -1 when it did not come up */
static int ringbar(int n, int *in, int *w)
{
    int pin[2], pout[2]; char rs[8]; snprintf(rs, sizeof rs, "%d", n);
    const char *fake = getenv("HASSMIC_FAKE_BAND"), *dex = dex_path();
    int builder = builder_api();        /* before the fork: a property lookup takes bionic's lock, which another thread may hold */
    if (pipe(pin)) return -1;
    if (pipe(pout)) { close(pin[0]); close(pin[1]); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pin[0], 0); dup2(pout[1], 1);
        close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]);
        char cp[PATH_MAX + 16]; snprintf(cp, sizeof cp, "CLASSPATH=%s", dex); putenv(cp);
        if (fake) execl(fake, fake, rs, (char *)NULL);
        else if (builder) execl("/system/bin/app_process", "app_process", "/", "Ringbar", rs, "builder", (char *)NULL);
        else execl("/system/bin/app_process", "app_process", "/", "Ringbar", rs, (char *)NULL);
        _exit(127);
    }
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
    if (sscanf(line, "ready %d %d", &ww, &hh) != 2 || ww <= 0 || ww > MAX_W || hh != n) {
        fprintf(stderr, "band: the renderer did not come up (%s): %s\n", line[0] ? line : "no answer", dex);
        close(pin[1]); return -1;
    }
    *in = pin[1]; *w = ww;
    fprintf(stderr, "band: %d x %d at the bottom of the screen\n", ww, hh);
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
    int fd = -1, w = 0, have_rows = -1, sent = 0, timeout = 0, fails = 0;
    long long last_wake = 0, next_check = 0;
    for (;;) {
        pthread_mutex_lock(&lock);
        int want = rows;
        pthread_mutex_unlock(&lock);
        if (want != have_rows) {                    /* (re)start the renderer for the rows set */
            if (fd >= 0) close(fd);                 /* it ends at the end of its input; the layer goes with it */
            fd = -1; w = 0; sent = 0;
            if (ringbar(want, &fd, &w) < 0) {
                if (!fails++) fprintf(stderr, "band: no renderer, trying again every 30 s\n");
                sleep(30); continue;
            }
            fails = 0; have_rows = want;
            if (!want) fprintf(stderr, "band: off, the whole screen is the apps'\n");
        }
        long long now = now_ms();
        if (now >= next_check) {                    /* stock's screen timeouts by what the Echo is doing */
            int t = core_muted() || core_dnd(-1) ? QUIET_MS : TIMEOUT_MS;
            if (t != timeout) { timeout = t; screen_timeout(t); }
            next_check = now + 1000;
        }
        pthread_mutex_lock(&lock);
        int p = top(now), arg_ = p < P_COUNT ? act[p].arg : 0, blink = flash_until && now < flash_until, moving = 1;
        long long at = p < P_COUNT ? act[p].at : 0;
        struct ring_light l = blink ? flash : light;
        if (!blink) flash_until = 0;
        int wake = wake_req; wake_req = 0;
        pthread_mutex_unlock(&lock);
        if (wake && now - last_wake >= WAKE_EVERY) { last_wake = now; screen_wake(); }
        if (fd < 0) {                               /* band off: nothing to draw, wait for a change */
            pthread_mutex_lock(&lock);
            if (rows == have_rows) pthread_cond_wait(&cv, &lock);
            pthread_mutex_unlock(&lock);
            continue;
        }
        int lv = atomic_load(&level_db);            /* the talker: -50 dBFS nothing, -20 the widest */
        float target = clamp01((lv / 10.f + 50) / 30);
        loud += (target - loud) * (target > loud ? 0.35f : 0.08f);
        struct rgb led[LEDS];
        all(led, hex(0));
        if (p < P_COUNT) draw(p, led, now - at, arg_);
        else if (l.on || blink) light_draw(&l, blink, led, now);
        else moving = 0;
        unroll(led, row, w);
        float k = core_led_brightness(-1) / 100.f;
        if (!sent || memcmp(row, last, sizeof row[0] * w)) {
            if (send_frame(fd, row, w, have_rows, k) < 0) {
                fprintf(stderr, "band: the renderer is gone, starting it again\n");
                close(fd); fd = -1; have_rows = -1; sleep(1); continue;
            }
            memcpy(last, row, sizeof row[0] * w); sent = 1;
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

/* ---------------------------------------------------------------- ring.h */

int ring_generate(const char *dir) { (void)dir; fprintf(stderr, "ring: no LED ring on this model (the band is drawn at run time)\n"); return 1; }

void ring_start(void (*run)(const char *op, const char *name), void (*reloaded)(int custom_changed))
{
    (void)run; (void)reloaded;                      /* no ledctrl here, and no set root installs */
    pthread_mutex_lock(&lock);
    int go = !started; started = 1;
    pthread_mutex_unlock(&lock);
    if (!go) return;
    pthread_t t;
    if (pthread_create(&t, NULL, band_thread, NULL)) { fprintf(stderr, "band: no thread\n"); return; }
    pthread_detach(t);
}

int ring_custom(void) { return 1; }                 /* Home Assistant's light: drawn here, any colour */
/* What the Dots' ledcontroller adds (ring.c): the assistant's own volume steps, each LED's own layer (the fill, the
 * rotation, LED by LED) and designs in place of stock's animations.  None of it on the band: no slots, nothing to
 * install, the rotation stays 0 */
int ring_assistant_steps(void) { return 0; }
int ring_per_led(void) { return 0; }
const struct ring_slot ring_slots[] = { { "", "", RING_PLAIN } };
const int ring_nslots = 0;
int ring_slot_available(int i) { (void)i; return 0; }
int ring_slot_live(int i) { (void)i; return 0; }
int ring_leds(void) { return LEDS; }
int ring_check(const char *applied, size_t n, char *err, size_t cap) { (void)applied; (void)n; snprintf(err, cap, "no LED ring on this model"); return -1; }
void ring_reload(void) {}
int ring_rotation(int set) { (void)set; return 0; }

void ring_set(const char *pattern)
{
    int arg, i = find(pattern, &arg);
    if (i < 0) { fprintf(stderr, "band: no picture for \"%s\"\n", pattern); return; }
    pthread_mutex_lock(&lock);
    if (!(act[i].on && i == P_VOLUME)) act[i].at = now_ms();       /* a volume step moves the fill, not a new start */
    act[i].on = 1; act[i].arg = arg; wake_req = 1;
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
    if (set >= 0) { rows = set > MAX_ROWS ? MAX_ROWS : set; pthread_cond_signal(&cv); }
    int r = rows;
    pthread_mutex_unlock(&lock);
    return r;
}
