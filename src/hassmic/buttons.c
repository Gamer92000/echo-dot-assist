#include "buttons.h"
#include "board.h"
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define SHORT_PRESS_MS 1000            /* acebuttond sees the holds too: 5 s its setup mode, 21 s its factory reset */
#define HOLD_WARN_MS   5000            /* action button held: the ring warns ... */
#define HOLD_RESET_MS 10000            /* ... and then hassmic's reset (core_reset), long before acebuttond's 21 s */
#define COMBO_MS       2000            /* both volume keys: stock gives that no meaning (acebuttond's are action holds) */

static struct button_handler handler;
static int muted_state;                /* latch: what was last reported; no latch: the software toggle */
static pthread_mutex_t muted_lock = PTHREAD_MUTEX_INITIALIZER;

/* 1 if the kernel tells the mute state (board.privacy_state).  Without that file the mute key can only be counted, and
 * the count is wrong as soon as the latch moved while nobody counted: radar was run that way, and a daemon restarted
 * with the mics off showed unmuted from then on, every press the wrong way round. */
static int have_latch(void)
{
    static int have = -1;
    if (have < 0) have = board.privacy_latch && board.privacy_state && access(board.privacy_state, R_OK) == 0;
    return have;
}

int buttons_muted(void)
{
    if (!have_latch()) {               /* no sysfs truth: what we last toggled to */
        pthread_mutex_lock(&muted_lock);
        int m = muted_state;
        pthread_mutex_unlock(&muted_lock);
        return m;
    }
    char c = '0'; int f = open(board.privacy_state, O_RDONLY);
    if (f < 0) return 0;
    if (read(f, &c, 1) != 1) c = '0';
    close(f);
    return c == '1';
}

/* Reports the latch when it is not what was last reported.  Input events only say when to look, and latch_poll looks
 * without one, so a lost or early event (the state read before the driver changed it) is put right a second later. */
static void latch_check(void)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;   /* reports leave in the order the state was read */
    pthread_mutex_lock(&lock);
    int now = buttons_muted();
    if (now != muted_state) {
        muted_state = now;
        if (handler.mute_changed) handler.mute_changed(now);
    }
    pthread_mutex_unlock(&lock);
}

static void *latch_poll(void *arg)
{
    (void)arg;
    for (;;) { sleep(1); latch_check(); }
    return NULL;
}

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *reader(void *arg)
{
    int rfd = (int)(long)arg;
    struct input_event ev; long long action_down = 0, up_down = 0, dn_down = 0; int combo_done = 0, held = 0;
    for (;;) {
        /* while the action button is down, wake for its hold stages: the key repeats only as long as acebuttond's
         * configuration says, not something to count on */
        if (action_down && handler.hold && held < 2) {
            long long t = now_ms() - action_down, due = (held ? HOLD_RESET_MS : HOLD_WARN_MS) - t;
            struct pollfd p = { rfd, POLLIN, 0 };
            if (due > 0 && poll(&p, 1, (int)due) == 0) continue;            /* woke for the stage: looked at below */
            if (due <= 0) { held++; handler.hold(held); continue; }
        }
        if (read(rfd, &ev, sizeof ev) != sizeof ev) break;
        if (ev.type != EV_KEY) continue;
        switch (ev.code) {
        case KEY_HELP:
            if (ev.value == 1) { action_down = now_ms(); held = 0; }
            else if (ev.value == 0) {
                if (action_down && now_ms() - action_down < SHORT_PRESS_MS && handler.action) handler.action();
                if (held == 1 && handler.hold) handler.hold(0);
                action_down = 0; held = 0;
            }
            break;
        case KEY_MUTE:
            if (ev.value == 0 && handler.mute_changed) {
                if (have_latch()) { usleep(100000); latch_check(); }
                else {                  /* a plain key: toggle and report */
                    pthread_mutex_lock(&muted_lock);
                    muted_state = !muted_state;
                    int m = muted_state;
                    pthread_mutex_unlock(&muted_lock);
                    handler.mute_changed(m);
                }
            }
            break;
        case KEY_VOLUMEUP:
        case KEY_VOLUMEDOWN: {
            long long *down = ev.code == KEY_VOLUMEUP ? &up_down : &dn_down;
            if (ev.value == 1 && handler.volume)       /* press only; key repeat (2) would run away */ handler.volume(ev.code == KEY_VOLUMEUP ? 1 : -1);
            if (ev.value == 1) *down = now_ms();
            else if (ev.value == 0) { *down = 0; combo_done = 0; }
            /* both held: the repeats of the later one (every 150 ms) tell us when 2 s have passed; the two presses
             * changed the volume by one step each way */
            long long later = up_down > dn_down ? up_down : dn_down;
            if (up_down && dn_down && !combo_done && now_ms() - later >= COMBO_MS && handler.pair) { combo_done = 1; handler.pair(); }
        } break;
        }
    }
    fprintf(stderr, "buttons: reader stopped\n");
    return NULL;
}

/* The mute button is not on the keypad: it toggles a hardware latch, and the gpio-privacy driver reports the latch through
 * its own input device.  Whatever event arrives there, the truth is the sysfs state. */
static void *privacy_reader(void *arg)
{
    struct input_event ev; int pfd = (int)(long)arg;
    while (read(pfd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type == EV_SYN) continue;
        usleep(50000);
        latch_check();
    }
    fprintf(stderr, "buttons: privacy reader stopped\n");
    return NULL;
}

int buttons_start(const char *device, const struct button_handler *h)
{
    pthread_t t; int pfd, f2;
    handler = *h;
    int f1 = open(device, O_RDONLY);
    if (f1 < 0) return -1;
    if (board.keypad2 && (f2 = open(board.keypad2, O_RDONLY)) >= 0) {   /* keys split over two nodes (biscuit) */
        if (pthread_create(&t, NULL, reader, (void *)(long)f2)) close(f2);
        else pthread_detach(t);
    } else if (board.keypad2) {
        fprintf(stderr, "buttons: %s not available, its keys go unnoticed\n", board.keypad2);
    }
    if (have_latch()) {
        muted_state = buttons_muted();                  /* the caller asks for the state at start itself */
        if (!pthread_create(&t, NULL, latch_poll, NULL)) pthread_detach(t);
    } else if (board.privacy_latch) {
        fprintf(stderr, "buttons: %s not readable, counting presses of the mute key instead\n", board.privacy_state);
    }
    if (!board.privacy_input) pfd = -1;
    else if ((pfd = open(board.privacy_input, O_RDONLY)) < 0) fprintf(stderr, "buttons: %s not available, mute button changes go unnoticed\n", board.privacy_input);
    else if (pthread_create(&t, NULL, privacy_reader, (void *)(long)pfd)) close(pfd);
    else pthread_detach(t);
    if (pthread_create(&t, NULL, reader, (void *)(long)f1)) return -1;
    pthread_detach(t);
    return 0;
}
