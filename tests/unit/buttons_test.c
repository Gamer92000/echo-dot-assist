/* buttons.c on a board without an action button (board.action_combo, checkers): both volume keys pressed together and
 * let go quickly are the action button's press; held 2 s they pair; one after the other they are volume steps (on
 * release, with an event each for Home Assistant), pressed far apart or held they are neither.  A pair is never a volume
 * step or an event of its own.  Events through a FIFO, as the keypad would deliver them.
 *   buttons_test FIFO */
#include "board.h"
#include "buttons.h"
#include <fcntl.h>
#include <linux/input.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const struct board board = { .action_combo = 1 };
static atomic_int actions, pairs, ups, downs, events, multis, last_multi;
static void on_action(void) { actions++; }
static void on_pair(void) { pairs++; }
static void on_multi(int n) { multis++; last_multi = n; }
static void on_volume(int d) { if (d > 0) ups++; else downs++; }
static void on_mute(int m) { (void)m; }
static void on_event(int button, int hold) { (void)button; (void)hold; events++; }
#include <time.h>
static atomic_int helds, take; static atomic_llong held_down;
static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static int on_held(long long down) { helds++; held_down = down; return take; }

static int fd, fails;
static void key(int code, int value)
{
    struct input_event ev; memset(&ev, 0, sizeof ev);
    ev.type = EV_KEY; ev.code = code; ev.value = value;
    if (write(fd, &ev, sizeof ev) != sizeof ev) perror("write");
}
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static void settle(void) { usleep(100000); }
static void tap(void) { key(KEY_HELP, 1); usleep(40000); key(KEY_HELP, 0); }

int main(int argc, char **argv)
{
    struct button_handler h = { .action = on_action, .pair = on_pair, .volume = on_volume, .mute_changed = on_mute,
                               .event = on_event, .multi = on_multi, .held = on_held };
    if (argc != 2) return 2;
    unlink(argv[1]);
    if (mkfifo(argv[1], 0600)) { perror("mkfifo"); return 2; }
    int keep = open(argv[1], O_RDWR);                       /* the reader's open does not block, nor see EOF */
    if (buttons_start(argv[1], &h)) { fprintf(stderr, "buttons_start failed\n"); return 2; }
    fd = keep;

    key(KEY_VOLUMEUP, 1); usleep(50000); key(KEY_VOLUMEDOWN, 1); usleep(200000);
    key(KEY_VOLUMEUP, 0); usleep(30000); key(KEY_VOLUMEDOWN, 0); settle();
    check(actions == 1 && pairs == 0 && ups == 0 && downs == 0 && events == 0, "both together, let go: one action press, no step, no event");

    key(KEY_VOLUMEDOWN, 1); usleep(30000); key(KEY_VOLUMEUP, 1); usleep(100000);
    key(KEY_VOLUMEDOWN, 0); key(KEY_VOLUMEUP, 0); settle();
    check(actions == 2, "the other order too, counted once");

    key(KEY_VOLUMEUP, 1); usleep(100000); key(KEY_VOLUMEUP, 0); usleep(100000);
    key(KEY_VOLUMEDOWN, 1); usleep(100000); key(KEY_VOLUMEDOWN, 0); settle();
    check(actions == 2 && ups == 1 && downs == 1 && events == 2, "one after the other: volume only, an event each");

    key(KEY_VOLUMEUP, 1); usleep(600000); key(KEY_VOLUMEDOWN, 1); usleep(100000);
    key(KEY_VOLUMEDOWN, 0); key(KEY_VOLUMEUP, 0); settle();
    check(actions == 2, "the second press 600 ms after the first: not a chord");

    key(KEY_VOLUMEUP, 1); usleep(20000); key(KEY_VOLUMEDOWN, 1);
    for (int i = 0; i < 16; i++) { usleep(150000); key(KEY_VOLUMEDOWN, 2); }   /* auto repeat while held */
    key(KEY_VOLUMEDOWN, 0); key(KEY_VOLUMEUP, 0); settle();
    check(pairs == 1 && actions == 2, "held 2 s: pairing, no action press");

    key(KEY_VOLUMEUP, 1); key(KEY_VOLUMEDOWN, 1); usleep(1300000);
    key(KEY_VOLUMEUP, 0); key(KEY_VOLUMEDOWN, 0); settle();
    check(actions == 2 && pairs == 1, "held 1.3 s without repeats: neither");
    /* ... but the 1 s hold, for what waits for one (a copied plugin's note), told when the later key went down */
    check(helds == 1 && mono_ms() - held_down > 1300 && mono_ms() - held_down < 1700, "...it is the 1 s hold, with the time the pair went down");
    key(KEY_VOLUMEUP, 1); key(KEY_VOLUMEDOWN, 1); usleep(2300000);
    key(KEY_VOLUMEUP, 0); key(KEY_VOLUMEDOWN, 0); settle();
    check(helds == 1 && actions == 2, "held 2.3 s without repeats: not the 1 s hold (the pairing hold's time, by the clock)");
    { int h0 = helds, p0 = pairs;
      key(KEY_VOLUMEUP, 1); usleep(20000); key(KEY_VOLUMEDOWN, 1);
      for (int i = 0; i < 16; i++) { usleep(150000); key(KEY_VOLUMEDOWN, 2); }
      key(KEY_VOLUMEDOWN, 0); key(KEY_VOLUMEUP, 0); settle();
      check(pairs == p0 + 1 && helds == h0, "held 2 s with repeats: pairing, not the 1 s hold"); }

    /* the action button's own key: a press alone is the action at once; with multi press on, 450 ms late, and two or
     * three presses in a row are the multi handler's instead */
    int a0 = actions;
    tap(); settle();
    check(actions == a0 + 1 && multis == 0, "multi press off: a single press acts at once");
    buttons_multi_press(1);
    a0 = actions;
    tap(); settle();
    check(actions == a0 && multis == 0, "multi press on: a single press waits");
    usleep(500000);
    check(actions == a0 + 1 && multis == 0, "... and acts when no second one came");
    tap(); usleep(150000); tap(); usleep(700000);
    check(actions == a0 + 1 && multis == 1 && last_multi == 2, "two presses: multi 2, no action");
    tap(); usleep(120000); tap(); usleep(120000); tap(); settle();
    check(actions == a0 + 1 && multis == 2 && last_multi == 3, "three presses: multi 3 at once");
    tap(); usleep(700000); tap(); usleep(700000);
    check(actions == a0 + 3 && multis == 2, "two presses 700 ms apart: two single presses");
    key(KEY_HELP, 1); usleep(1200000); key(KEY_HELP, 0); settle();
    check(actions == a0 + 3 && multis == 2, "a hold is no press");
    /* the hold is offered to what waits for one before it is Home Assistant's event, with when the button went down */
    { int e0 = events, h0 = helds; long long t0 = mono_ms();
      key(KEY_HELP, 1); usleep(1200000);
      check(helds == h0 + 1 && events == e0 + 1 && held_down >= t0 && held_down - t0 < 100, "the action button held 1 s: offered with its down time, not taken: the event");
      key(KEY_HELP, 0); settle();
      take = 1; e0 = events;
      key(KEY_HELP, 1); usleep(1200000); key(KEY_HELP, 0); settle();
      check(helds == h0 + 2 && events == e0, "...taken (a note's wait): no event for Home Assistant");
      take = 0; }
    buttons_multi_press(0);
    unlink(argv[1]);
    return fails ? 1 : 0;
}
