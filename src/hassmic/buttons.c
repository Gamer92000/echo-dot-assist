#include "buttons.h"
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define PRIVACY_STATE "/sys/devices/platform/gpio-privacy/state"
#define SHORT_PRESS_MS 1000            /* longer holds belong to acebuttond: 5 s setup mode, 21 s factory reset */

static struct button_handler handler;
static int fd = -1;

int buttons_muted(void)
{
    char c = '0'; int f = open(PRIVACY_STATE, O_RDONLY);
    if (f < 0) return 0;
    if (read(f, &c, 1) != 1) c = '0';
    close(f);
    return c == '1';
}

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *reader(void *arg)
{
    struct input_event ev; long long action_down = 0;
    (void)arg;
    while (read(fd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type != EV_KEY) continue;
        switch (ev.code) {
        case KEY_HELP:
            if (ev.value == 1) action_down = now_ms();
            else if (ev.value == 0 && action_down && now_ms() - action_down < SHORT_PRESS_MS && handler.action) handler.action();
            break;
        case KEY_MUTE:
            if (ev.value == 0 && handler.mute_changed) { usleep(100000); handler.mute_changed(buttons_muted()); }
            break;
        case KEY_VOLUMEUP:
        case KEY_VOLUMEDOWN:
            if (ev.value == 1 && handler.volume)       /* press only; key repeat (2) would run away */ handler.volume(ev.code == KEY_VOLUMEUP ? 1 : -1);
            break;
        }
    }
    fprintf(stderr, "buttons: reader stopped\n");
    return NULL;
}

int buttons_start(const char *device, const struct button_handler *h)
{
    pthread_t t;
    fd = open(device, O_RDONLY);
    if (fd < 0) return -1;
    handler = *h;
    return pthread_create(&t, NULL, reader, NULL) ? -1 : 0;
}
