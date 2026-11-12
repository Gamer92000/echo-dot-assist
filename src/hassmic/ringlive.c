/*
 * The light ring live (ringlive.h).  What the driver does, measured on a Dot 3 (donut, 2026-11-12, scripts/led-probe.sh;
 * PLAN.md "Light ring live"):
 *   - ISSI's IS31FL3236 at I2C 0-003f.  Its frame file is text, 12 x "RRGGBB" and a newline, LED 0 first, in the order of
 *     the animation files; libacehal_led.so writes it "%02X%02X%02X" per LED.  The driver scales each channel by the
 *     unit's calibration as it takes a frame (ledpwmscaling, root's: 0000FD written reads back 0000A9), so what reads
 *     back is not what was written; a frame is compared with what read back after our own write.
 *   - ledcontroller writes the frame only when its picture changes: idle, and over a static pattern, never (0 writes in
 *     10 s).  A frame written by someone else stays until ledcontroller's next change; over a moving pattern its next
 *     frame replaced ours after 40 ms.  On every set and unset it writes again (the last unset: dark).
 *   - What ledcontroller writes is the file's colour x its brightness table's pwm for the ring's level
 *     (led_mute_ring_config.json rgbTable, by the cap colour, level = index 0..100: solid_blue at 100, 50, 25, 10, 1 read
 *     back A9, 76, 31, 1D, 15, each the table's pwm x 255 through the calibration), truncated (scaleDutyCycle).  It
 *     sets the chip's current (led_current) with the level too; that file stays ledcontroller's.
 *   - A write takes ~15 ms, nearly all of it the I2C bus (1.3 ms of CPU): 65 a second at most.  RINGLIVE_FPS keeps us at 30.
 * So hassmic writes colours as seen through the same gamma ring.c maps designs with and the same pwm ledcontroller uses:
 * a design played live looks as installed, and nothing is brighter than ledcontroller would make it at that level (at
 * level 100 FD, its own most).  Whatever comes from Home Assistant goes through that, and a pixel never above 255 x pwm.
 */
#include "ringlive.h"
#include "ring.h"
#include "board.h"
#include "json.h"
#include "statefile.h"
#include <ctype.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define MAX_LEDS   RING_LEDS_MAX
#define GAMMA      2.2f                 /* as ring.c's designs and light */
#define TEXT_MAX   (MAX_LEDS * 6 + 8)
#define CHECK_MS   200                  /* how often the frame is read back while ours shows */
#define RESUME_MS  150                  /* after the Echo's own pattern: ledcontroller's unset writes first (ledq) */
#define FIGHT_N    3                    /* read back changed this often within FIGHT_MS: something else animates */
#define FIGHT_MS   3000
#define BACKOFF_MS 5000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int avail, leds = 12, dirty;
static _Atomic int writes;
static char frame_path[256];
static float pwm[101];
static int (*hold_fn)(void), (*level_fn)(void);
static void (*changed_fn)(void);

static struct live_led led[MAX_LEDS];
static long long off_at[MAX_LEDS];      /* a frame's timeout: that LED dark then; 0 none */
static struct { struct rd_design d; int on, drawn, repeat; long long t0; float bri; char name[RD_NAME]; } anim;

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static float clamp01(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; }

/* ---------------------------------------------------------------- pure parts */

void ringlive_duty(const float seen[3], float p, unsigned char out[3])
{
    p = clamp01(p);
    int cap = (int)(255 * p);
    for (int c = 0; c < 3; c++) {
        int v = (int)lroundf(powf(clamp01(seen[c]), GAMMA) * 255);      /* ring.c colour_rgb's mapping */
        int o = (int)(v * p);                                           /* the HAL's: multiplied, truncated */
        out[c] = (unsigned char)(o < 0 ? 0 : o > cap ? cap : o);
    }
}

void ringlive_text(const unsigned char (*px)[3], int n, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    char *o = out;
    for (int i = 0; i < n; i++) for (int c = 0; c < 3; c++) { *o++ = hex[px[i][c] >> 4]; *o++ = hex[px[i][c] & 15]; }
    *o++ = '\n'; *o = 0;
}

int ringlive_table(const char *s, size_t n, int capcolor, float out[101])
{
    struct json j = { 0 }; char want[16] = "";
    if (json_parse(&j, s, n)) { json_free(&j); return -1; }
    int rgb = json_get(&j, 0, "rgbTable"), caps = json_get(&j, 0, "cap_color"), rc = -1, ntab = 0;
    if (capcolor >= 0 && json_type(&j, caps) == J_ARR)
        JSON_EACH(&j, caps, k) if (json_int(&j, json_get(&j, k, "cap_color_number"), -1) == capcolor) json_str(&j, json_get(&j, k, "color"), want, sizeof want);
    if (json_type(&j, rgb) == J_OBJ) {
        for (int i = 0; i <= 100; i++) out[i] = 2;
        for (int t = json_first(&j, rgb); t >= 0; t = json_next(&j, rgb, t)) {
            int tab = json_next(&j, rgb, t);    /* an object's key, then its value */
            char key[16] = ""; json_str(&j, t, key, sizeof key);
            if (json_type(&j, tab) != J_ARR || (want[0] && strcmp(key, want))) { t = tab; continue; }
            if (json_len(&j, tab) < 101) { ntab = -1; break; }
            int i = 0;
            JSON_EACH(&j, tab, e) {
                if (i > 100) break;
                double p = json_num(&j, json_get(&j, e, "pwm"), -1);
                if (!(p > 0 && p <= 1)) { ntab = -1; break; }
                if (p < out[i]) out[i] = (float)p;      /* the cap colour unknown: the dimmer of the tables */
                i++;
            }
            if (ntab < 0) break;
            ntab++;
            t = tab;
        }
        if (ntab > 0) rc = 0;
    }
    json_free(&j);
    return rc;
}

/* ---------------------------------------------------------------- the frame file */

static int frame_write(const char *t)
{
    int fd = open(frame_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = strlen(t);
    int rc = write(fd, t, n) == (ssize_t)n ? 0 : -1;    /* sysfs takes a frame in one write */
    writes++;
    int e = errno;
    close(fd);
    errno = e;
    return rc;
}

static int frame_read(char *out, size_t cap)
{
    int fd = open(frame_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, out, cap - 1);
    close(fd);
    if (k < 0) return -1;
    out[k] = 0;
    return 0;
}

static void marker(int on)              /* what this run left on the ring: dark again at the next start */
{
    char p[300]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/ring-live", d ? d : "/data/local/hassmic/state");
    if (on) statefile_write(p, 0600, "1\n", 2); else unlink(p);
}

/* ---------------------------------------------------------------- the picture */

static int hw_of(int j, int rot) { return (leds - 1 + rot + j) % leds; }      /* ring.c's fill: volume_step-01's LED first */

/* lock held: what the ring shows now, as seen, per LED of the driver; *next: ms until it changes by itself.  1 if the
 * LEDs' states or what plays changed by itself (a timeout, an animation's end) */
static int picture(long long now, int rot, float seen[][3], int *next)
{
    int ch = 0;
    *next = CHECK_MS;
    memset(seen, 0, sizeof(float) * 3 * MAX_LEDS);
    for (int j = 0; j < leds; j++)
        if (off_at[j] && now >= off_at[j]) { off_at[j] = 0; led[j].on = 0; ch = 1; }
        else if (off_at[j] && off_at[j] - now < *next) *next = (int)(off_at[j] - now);
    if (anim.on) {
        const struct rd_design *d = &anim.d; long long total = 0, loop_at = 0, e = now - anim.t0;
        for (int i = 0; i < d->nframes; i++) { if (anim.drawn && i == d->loop) loop_at = total; total += d->f[i].ms; }
        if (total <= 0) total = 1;
        int done = 0;
        if (anim.repeat > 0) { if (e >= total * anim.repeat) done = 1; else e %= total; }
        else if (!anim.drawn) e %= total;                                   /* Home Assistant's: until stopped */
        else if (d->loop < 0) { if (e >= total) done = 1; }                  /* a design "once" */
        else if (e >= total) e = loop_at + (e - total) % (total - loop_at > 0 ? total - loop_at : 1);
        if (done) { rd_free(&anim.d); memset(&anim, 0, sizeof anim); ch = 1; }
        else {
            int i = 0; long long at = 0;
            while (i < d->nframes - 1 && at + d->f[i].ms <= e) at += d->f[i++].ms;
            long long left = at + d->f[i].ms - e;
            if (left < *next) *next = (int)(left > 0 ? left : 1);
            for (int k = 0; k < leds && k < d->leds; k++) {
                int h = anim.drawn ? k : hw_of(k, rot);
                for (int c = 0; c < 3; c++) seen[h][c] = d->f[i].px[k][c] / 255.0f * anim.bri;
            }
            return ch;
        }
    }
    for (int j = 0; j < leds; j++) if (led[j].on) {
        int h = hw_of(j, rot);
        seen[h][0] = led[j].r * led[j].brightness; seen[h][1] = led[j].g * led[j].brightness; seen[h][2] = led[j].b * led[j].brightness;
    }
    return ch;
}

static int active_locked(void)
{
    if (anim.on) return 1;
    for (int j = 0; j < leds; j++) if (led[j].on && (led[j].r > 0 || led[j].g > 0 || led[j].b > 0) && led[j].brightness > 0) return 1;
    return 0;
}

/* ---------------------------------------------------------------- the writer */

static void *writer(void *arg)
{
    (void)arg;
    char text[TEXT_MAX] = "", seen_back[TEXT_MAX] = "", now_back[TEXT_MAX];
    long long last_write = 0, last_check = 0, resume_at = 0, backoff_until = 0, fight_t0 = 0;
    int owned = 0, held = 0, fights = 0, failed = 0;
    for (;;) {
        int rot = ring_rotation(-1), lvl = level_fn ? level_fn() : 100, next;
        float seen[MAX_LEDS][3];
        if (lvl < 0) lvl = 0;
        if (lvl > 100) lvl = 100;
        pthread_mutex_lock(&lock);
        long long now = now_ms();
        int ch = picture(now, rot, seen, &next), content = active_locked();
        pthread_mutex_unlock(&lock);
        if (ch && changed_fn) changed_fn();

        if (content || owned) {
            if (hold_fn && hold_fn()) {         /* the Echo's own pattern, or muted: hands off, ledcontroller writes */
                if (!held) fprintf(stderr, "light ring live: the Echo's own pattern shows, live waits\n");
                held = 1; next = next < CHECK_MS ? next : CHECK_MS;
            }
            else if (held) {                    /* ledcontroller wrote last: ours again, not counted as a fight */
                held = 0; resume_at = now + RESUME_MS; next = RESUME_MS; text[0] = 0; fights = 0;
            }
            else if (now < resume_at) next = (int)(resume_at - now);
            else if (now < backoff_until) next = (int)(backoff_until - now);
            else {
                unsigned char px[MAX_LEDS][3]; char want[TEXT_MAX];
                for (int h = 0; h < leds; h++) {
                    if (content) ringlive_duty(seen[h], pwm[lvl], px[h]); else memset(px[h], 0, 3);
                }
                ringlive_text((const unsigned char (*)[3])px, leds, want);
                int need = strcmp(want, text) || !owned;
                if (!need && now - last_check >= CHECK_MS) {        /* ledcontroller wrote over it? */
                    last_check = now;
                    if (!frame_read(now_back, sizeof now_back) && strcmp(now_back, seen_back)) {
                        need = 1;
                        if (now - fight_t0 > FIGHT_MS) { fight_t0 = now; fights = 0; }
                        if (++fights >= FIGHT_N) {
                            fprintf(stderr, "light ring live: something else keeps writing the ring (a pattern hassmic did not set?), live waits %d s\n", BACKOFF_MS / 1000);
                            backoff_until = now + BACKOFF_MS; fights = 0; need = 0; next = BACKOFF_MS;
                        }
                    }
                }
                if (need && now - last_write < 1000 / RINGLIVE_FPS) { next = (int)(1000 / RINGLIVE_FPS - (now - last_write)); need = 0; }
                if (need) {
                    last_write = last_check = now;
                    if (frame_write(want)) {
                        if (!failed) fprintf(stderr, "light ring live: cannot write %s (%s), tries again in 10 s\n", frame_path, strerror(errno));
                        failed = 1; backoff_until = now + 10000;
                    }
                    else {
                        if (failed) fprintf(stderr, "light ring live: %s written again\n", frame_path);
                        failed = 0;
                        snprintf(text, sizeof text, "%s", want);
                        if (frame_read(seen_back, sizeof seen_back)) seen_back[0] = 0;
                        if (content && !owned) { fprintf(stderr, "light ring live: on the ring (level %d)\n", lvl); marker(1); }
                        if (!content) { fprintf(stderr, "light ring live: dark, the ring is ledcontroller's again\n"); marker(0); text[0] = 0; }
                        owned = content;
                    }
                }
            }
        }
        if (next < 5) next = 5;
        if (next > CHECK_MS) next = CHECK_MS;
        if (!content && !owned) next = 1000;    /* nothing of ours: a change pokes the thread awake anyway */
        pthread_mutex_lock(&lock);
        if (!dirty) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (long)next * 1000000L; ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L;
            pthread_cond_timedwait(&cv, &lock, &ts);
        }
        dirty = 0;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static void poke(void) { dirty = 1; pthread_cond_signal(&cv); }       /* lock held */

/* ---------------------------------------------------------------- start */

static char *slurp(const char *file, size_t max, size_t *n)
{
    FILE *f = fopen(file, "r"); char *t; size_t k = 0;
    if (!f) return NULL;
    if (!(t = malloc(max + 1))) { fclose(f); return NULL; }
    k = fread(t, 1, max, f); fclose(f);
    t[k] = 0; if (n) *n = k;
    return t;
}

static int capcolor(void)               /* idme alsparams "... capcolor=0": the table's colour; -1 unknown */
{
    const char *f = getenv("HASSMIC_FAKE_ALSPARAMS");
    char *t = slurp(f ? f : "/proc/idme/alsparams", 512, NULL), *c = t ? strstr(t, "capcolor=") : NULL;
    int v = c && isdigit((unsigned char)c[9]) ? atoi(c + 9) : -1;
    free(t);
    return v;
}

int ringlive_start(int (*hold)(void), int (*level)(void), void (*changed)(void))
{
    const char *fake = getenv("HASSMIC_FAKE_FRAME"), *p = fake ? fake : board.ring_frame, *why = NULL;
    char t[TEXT_MAX + 64], table[300]; size_t n;
    hold_fn = hold; level_fn = level; changed_fn = changed;
    leds = ring_leds();
    snprintf(table, sizeof table, "%s/led_mute_ring_config.json", getenv("HASSMIC_LED_ROOT") ? getenv("HASSMIC_LED_ROOT") : "/system/etc");
    char *js = NULL;
    if (!p || !*p) return -1;                                       /* not on this model (yet): nothing to say */
    snprintf(frame_path, sizeof frame_path, "%s", p);
    if (leds < 2 || leds > MAX_LEDS) why = "not a ring of LEDs";
    else if (access(frame_path, W_OK)) why = "hassmic may not write the ring's frame (root's main.sh gives it at the satellite's start; an older one does not)";
    else if (frame_read(t, sizeof t) || strlen(t) != (size_t)leds * 6 + 1 || strspn(t, "0123456789abcdefABCDEF") != (size_t)leds * 6)
        why = "the ring's frame is not 12 x RRGGBB as measured";
    else if (!(js = slurp(table, 1 << 16, &n)) || ringlive_table(js, n, capcolor(), pwm)) why = "no brightness table (led_mute_ring_config.json)";
    free(js);
    if (why) { fprintf(stderr, "light ring live: not here: %s\n", why); return -1; }
    int cc = capcolor();
    fprintf(stderr, "light ring live: %s, %d LEDs, at most %02X at level 100 (cap colour %d)\n", frame_path, leds, (int)(255 * pwm[100]), cc);
    {   /* a frame the last run left (it ended while live showed): dark, ledcontroller's idle picture */
        char m[300]; const char *d = getenv("HASSMIC_STATE");
        snprintf(m, sizeof m, "%s/ring-live", d ? d : "/data/local/hassmic/state");
        if (!access(m, F_OK)) {
            unsigned char z[MAX_LEDS][3]; memset(z, 0, sizeof z); ringlive_text((const unsigned char (*)[3])z, leds, t);
            if (!frame_write(t)) fprintf(stderr, "light ring live: what the last run left on the ring is dark again\n");
            unlink(m);
        }
    }
    pthread_t th;
    if (pthread_create(&th, NULL, writer, NULL)) { fprintf(stderr, "light ring live: no thread\n"); return -1; }
    pthread_detach(th);
    avail = 1;
    return 0;
}

int ringlive_available(void) { return avail; }
int ringlive_writes(void) { return writes; }
int ringlive_leds(void) { return leds; }

/* ---------------------------------------------------------------- what Home Assistant asks */

void ringlive_led_get(int i, struct live_led *l)
{
    pthread_mutex_lock(&lock);
    if (i >= 0 && i < leds) *l = led[i]; else memset(l, 0, sizeof *l);
    pthread_mutex_unlock(&lock);
}

void ringlive_led_set(int i, const struct live_led *l)
{
    if (i < 0 || i >= leds) return;
    pthread_mutex_lock(&lock);
    led[i] = *l;
    led[i].brightness = clamp01(l->brightness); led[i].r = clamp01(l->r); led[i].g = clamp01(l->g); led[i].b = clamp01(l->b);
    off_at[i] = 0;
    poke();
    pthread_mutex_unlock(&lock);
}

/* "#rrggbb" or "rrggbb" into 0..1 a channel, the largest 1, and its level (the largest / 255) */
static int colour(const char *s, struct live_led *l)
{
    s += *s == '#';
    if (!*s || !strcasecmp(s, "off") || !strcasecmp(s, "black")) { l->on = 0; return 0; }
    if (strlen(s) != 6 || strspn(s, "0123456789abcdefABCDEF") != 6) return -1;
    unsigned long v = strtoul(s, NULL, 16);
    float r = (v >> 16 & 255) / 255.0f, g = (v >> 8 & 255) / 255.0f, b = (v & 255) / 255.0f, mx = fmaxf(r, fmaxf(g, b));
    l->on = mx > 0;
    l->brightness = mx;
    l->r = mx > 0 ? r / mx : 1; l->g = mx > 0 ? g / mx : 1; l->b = mx > 0 ? b / mx : 1;
    return 0;
}

int ringlive_frame(char colors[][16], int n, float brightness, int timeout_s, char *err, size_t cap)
{
    struct live_led want[MAX_LEDS]; memset(want, 0, sizeof want);
    if (n > leds) { snprintf(err, cap, "%d colours for %d LEDs", n, leds); return -1; }
    for (int i = 0; i < n; i++)
        if (colour(colors[i], &want[i])) { snprintf(err, cap, "\"%s\" is no colour (#rrggbb)", colors[i]); return -1; }
    brightness = clamp01(brightness);
    pthread_mutex_lock(&lock);
    long long off = timeout_s > 0 ? now_ms() + (long long)timeout_s * 1000 : 0;
    for (int i = 0; i < leds; i++) {
        if (i < n) {
            if (want[i].on) want[i].brightness *= brightness;
            led[i] = want[i];
        }
        else memset(&led[i], 0, sizeof led[i]);
        if (!led[i].brightness) led[i].brightness = 1;          /* an LED off keeps a level for when it comes on */
        if (!led[i].r && !led[i].g && !led[i].b) led[i].r = led[i].g = led[i].b = 1;
        off_at[i] = led[i].on ? off : 0;
    }
    poke();
    pthread_mutex_unlock(&lock);
    return 0;
}

static int play(struct rd_design *d, int drawn, int repeat, float brightness, const char *name, char *err, size_t cap)
{
    if (d->leds != leds) { snprintf(err, cap, "made for %d LEDs, the ring has %d", d->leds, leds); rd_free(d); return -1; }
    pthread_mutex_lock(&lock);
    rd_free(&anim.d);
    anim.d = *d; anim.on = 1; anim.drawn = drawn; anim.repeat = repeat < 0 ? 0 : repeat; anim.t0 = now_ms();
    anim.bri = clamp01(brightness);
    snprintf(anim.name, sizeof anim.name, "%s", name);
    poke();
    pthread_mutex_unlock(&lock);
    return 0;
}

int ringlive_animate(const char *lines, size_t n, int repeat, float brightness, char *err, size_t cap)
{
    struct rd_design d; char head[32];
    int k = snprintf(head, sizeof head, "leds %d\n", leds);
    char *t = malloc((size_t)k + n + 1);
    if (!t) { snprintf(err, cap, "no memory"); return -1; }
    memcpy(t, head, (size_t)k); memcpy(t + k, lines, n); t[k + n] = 0;
    int rc = rd_parse(t, (size_t)k + n, &d, err, cap);
    free(t);
    if (rc) return -1;
    return play(&d, 0, repeat, brightness, "animation", err, cap);
}

int ringlive_design(const char *name, int repeat, float brightness, char *err, size_t cap)
{
    struct rd_design d;
    if (rd_find(name, &d)) { snprintf(err, cap, "no design \"%s\" in the library", name); return -1; }
    char nm[RD_NAME]; snprintf(nm, sizeof nm, "%s", d.name);
    return play(&d, 1, repeat, brightness, nm, err, cap);
}

void ringlive_stop(void)
{
    pthread_mutex_lock(&lock);
    rd_free(&anim.d); memset(&anim, 0, sizeof anim);
    poke();
    pthread_mutex_unlock(&lock);
}

void ringlive_clear(void)
{
    pthread_mutex_lock(&lock);
    rd_free(&anim.d); memset(&anim, 0, sizeof anim);
    for (int i = 0; i < leds; i++) { led[i].on = 0; off_at[i] = 0; }
    poke();
    pthread_mutex_unlock(&lock);
}

int ringlive_active(void) { pthread_mutex_lock(&lock); int a = active_locked(); pthread_mutex_unlock(&lock); return a; }

void ringlive_playing(char *out, size_t cap)
{
    pthread_mutex_lock(&lock);
    snprintf(out, cap, "%s", anim.on ? anim.name : "");
    pthread_mutex_unlock(&lock);
}
