/* L09G's 24 RGB LEDs through the same AW20072 frame ioctl as stock. */
#include "led_l09g.h"
#include "statefile.h"
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct frame { uint32_t start, length; uint8_t *pixels; };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int muted, activity, last_activity, call_state, volume = -1, started, pending;
static long long wake_at, end_at;
static uint8_t last[72];
static struct l09g_light ambient = { 0, 0.5f, 1, 1, 1, 1 };

static float clamp(float v) { return !isfinite(v) || v < 0 ? 0 : v > 1 ? 1 : v; }
static void light_path(char *path, size_t size)
{
    const char *state = getenv("HASSMIC_STATE");
    snprintf(path, size, "%s/light-ring", state ? state : "/data/l09g-assist/state");
}
static void light_load(void)
{
    char path[320]; struct l09g_light l;
    light_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    int n = fscanf(f, "%d %f %f %f %f %f", &l.on, &l.brightness, &l.color_brightness, &l.r, &l.g, &l.b);
    fclose(f);
    if (n == 6) {
        l.on = !!l.on; l.brightness = clamp(l.brightness); l.color_brightness = clamp(l.color_brightness);
        l.r = clamp(l.r); l.g = clamp(l.g); l.b = clamp(l.b); ambient = l;
    }
}
void l09g_light_get(struct l09g_light *light)
{
    pthread_mutex_lock(&lock); *light = ambient; pthread_mutex_unlock(&lock);
}
void l09g_light_set(const struct l09g_light *light)
{
    char path[320], value[160];
    pthread_mutex_lock(&lock);
    ambient = *light; ambient.on = !!ambient.on;
    ambient.brightness = clamp(ambient.brightness); ambient.color_brightness = clamp(ambient.color_brightness);
    ambient.r = clamp(ambient.r); ambient.g = clamp(ambient.g); ambient.b = clamp(ambient.b);
    light_path(path, sizeof path);
    int n = snprintf(value, sizeof value, "%d %.6f %.6f %.6f %.6f %.6f\n", ambient.on, ambient.brightness,
                     ambient.color_brightness, ambient.r, ambient.g, ambient.b);
    if (n > 0 && (size_t)n < sizeof value && statefile_write(path, 0600, value, (size_t)n))
        fprintf(stderr, "l09g leds: cannot save light state\n");
    pending = 1; pthread_cond_signal(&changed); pthread_mutex_unlock(&lock);
}

#define WAKE_MS 480
#define END_MS 467
#define FRAME_MS 40
static long long now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
struct visual {
    int muted, activity, call_state, volume;
    long long wake_at, end_at;
    struct l09g_light light;
};
static int animating(const struct visual *s, long long now)
{
    return s->call_state == 1 || s->call_state == 2 ||
           (s->activity && s->wake_at && now - s->wake_at < WAKE_MS) ||
           (!s->activity && s->end_at && now - s->end_at < END_MS);
}
static void render(uint8_t data[72], const struct visual *s, long long now)
{
    long long wake_age = now - s->wake_at, end_age = now - s->end_at;
    int waking = s->activity && s->wake_at && wake_age >= 0 && wake_age < WAKE_MS;
    int ending = !s->activity && s->end_at && end_age >= 0 && end_age < END_MS;
    float pulse = 0.04f + 0.96f * (0.5f + 0.5f * sinf((float)(now % 1200) * 6.2831853f / 1200));
    for (int i = 0; i < 24; i++) {
        uint8_t *p = data + i * 3;
        if (s->muted) p[0] = 100;
        else if (s->call_state == 2) { p[1] = (uint8_t)(220 * pulse); p[2] = (uint8_t)(24 * pulse); }
        else if (waking) {
            int lit = 1 + (int)(wake_age * 24 / WAKE_MS);
            if (i < lit) { p[1] = 80; p[2] = 140; }
            if (i == lit - 1) { p[0] = 35; p[1] = 155; p[2] = 220; }
        }
        else if (s->activity == 1) { p[1] = 70; p[2] = 120; }
        else if (s->activity == 2) { p[0] = 60; p[2] = 100; }
        else if (s->activity == 3) p[2] = 110;
        else if (ending) { float fade = 1.0f - (float)end_age / END_MS; p[1] = (uint8_t)(35 * fade); p[2] = (uint8_t)(150 * fade); }
        else if (s->volume >= 0) { if (i < s->volume) p[0] = p[1] = p[2] = 70; }
        else if (s->call_state == 1) { p[1] = (uint8_t)(120 * pulse); p[2] = (uint8_t)(20 * pulse); }
        else if (s->call_state == 3) { p[1] = 110; p[2] = 20; }
        else if (s->light.on) {
            float level = 255 * s->light.brightness * s->light.color_brightness;
            p[0] = (uint8_t)(level * s->light.r + 0.5f); p[1] = (uint8_t)(level * s->light.g + 0.5f); p[2] = (uint8_t)(level * s->light.b + 0.5f);
        }
    }
}
/* Called with lock held. */
static struct visual snapshot(void)
{
    return (struct visual){ muted, activity, call_state, volume, wake_at, end_at, ambient };
}

static void *worker(void *arg)
{
    (void)arg;
    const char *device = getenv("L09G_LED_DEVICE");
    int fd = open(device ? device : "/dev/aw20072", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("l09g leds: open"); return NULL; }
    memset(last, 0xff, sizeof last);
    for (;;) {
        uint8_t data[72] = {0};
        pthread_mutex_lock(&lock);
        while (!pending) {
            struct visual s = snapshot();
            if (animating(&s, now_ms())) {
                struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
                until.tv_nsec += FRAME_MS * 1000000L;
                if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&changed, &lock, &until);
                break;
            }
            pthread_cond_wait(&changed, &lock);
        }
        pending = 0;
        struct visual s = snapshot();
        pthread_mutex_unlock(&lock);
        render(data, &s, now_ms());
        if (memcmp(data, last, sizeof data)) {
            struct frame f = { 0, sizeof data, data };
            if (ioctl(fd, 0xc0107a01, &f)) perror("l09g leds: frame");
            else memcpy(last, data, sizeof data);
        }
    }
    return NULL;
}
int l09g_led_start(void)
{
    pthread_t thread;
    if (started) return 0;
    light_load(); started = 1; pending = 1;
    if (pthread_create(&thread, NULL, worker, NULL)) { started = 0; return -1; }
    pthread_detach(thread);
    return 0;
}
void l09g_led_event(const char *op, const char *pattern)
{
    int set = !strcmp(op, "-s");
    pthread_mutex_lock(&lock);
    if (!strcmp(pattern, "mics-off_on")) muted = set;
    else if (!strcmp(pattern, "active-start")) { if (set) { activity = 1; wake_at = now_ms(); end_at = 0; } else if (activity == 1) { last_activity = 1; activity = 0; } }
    else if (!strcmp(pattern, "active-thinking")) { if (set) { activity = 2; end_at = 0; } else if (activity == 2) { last_activity = 2; activity = 0; } }
    else if (!strcmp(pattern, "active-talking")) { if (set) { activity = 3; end_at = 0; } else if (activity == 3) { last_activity = 3; activity = 0; } }
    else if (!strcmp(pattern, "active-end") && set) { activity = 0; end_at = last_activity == 3 ? now_ms() : 0; wake_at = 0; }
    else if (!strcmp(pattern, "call_outbound_ringing")) { if (set) call_state = 1; else if (call_state == 1) call_state = 0; }
    else if (!strcmp(pattern, "call_incoming")) { if (set) call_state = 2; else if (call_state == 2) call_state = 0; }
    else if (!strcmp(pattern, "call_connected")) { if (set) call_state = 3; else if (call_state == 3) call_state = 0; }
    else if (!strncmp(pattern, "volume_step-", 12)) {
        int n = atoi(pattern + 12); if (n < 0) n = 0; if (n > 24) n = 24;
        if (set) volume = n; else if (volume == n) volume = -1;
    }
    pending = 1; pthread_cond_signal(&changed);
    pthread_mutex_unlock(&lock);
}
