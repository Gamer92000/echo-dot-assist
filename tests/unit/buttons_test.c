/* buttons.c on a board without an action button (board.action_combo, checkers): both volume keys pressed together and
 * let go quickly are the action button's press; held 2 s they pair; one after the other, or pressed far apart, they
 * are volume steps only.  Events through a FIFO, as the keypad would deliver them.
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
static atomic_int actions, pairs, ups, downs;
static void on_action(void) { actions++; }
static void on_pair(void) { pairs++; }
static void on_volume(int d) { if (d > 0) ups++; else downs++; }
static void on_mute(int m) { (void)m; }

static int fd, fails;
static void key(int code, int value)
{
    struct input_event ev; memset(&ev, 0, sizeof ev);
    ev.type = EV_KEY; ev.code = code; ev.value = value;
    if (write(fd, &ev, sizeof ev) != sizeof ev) perror("write");
}
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static void settle(void) { usleep(100000); }

int main(int argc, char **argv)
{
    struct button_handler h = { .action = on_action, .pair = on_pair, .volume = on_volume, .mute_changed = on_mute };
    if (argc != 2) return 2;
    unlink(argv[1]);
    if (mkfifo(argv[1], 0600)) { perror("mkfifo"); return 2; }
    int keep = open(argv[1], O_RDWR);                       /* the reader's open does not block, nor see EOF */
    if (buttons_start(argv[1], &h)) { fprintf(stderr, "buttons_start failed\n"); return 2; }
    fd = keep;

    key(KEY_VOLUMEUP, 1); usleep(50000); key(KEY_VOLUMEDOWN, 1); usleep(200000);
    key(KEY_VOLUMEUP, 0); usleep(30000); key(KEY_VOLUMEDOWN, 0); settle();
    check(actions == 1 && pairs == 0 && ups == 1 && downs == 1, "both together, let go: one action press (and a step each way)");

    key(KEY_VOLUMEDOWN, 1); usleep(30000); key(KEY_VOLUMEUP, 1); usleep(100000);
    key(KEY_VOLUMEDOWN, 0); key(KEY_VOLUMEUP, 0); settle();
    check(actions == 2, "the other order too, counted once");

    key(KEY_VOLUMEUP, 1); usleep(100000); key(KEY_VOLUMEUP, 0); usleep(100000);
    key(KEY_VOLUMEDOWN, 1); usleep(100000); key(KEY_VOLUMEDOWN, 0); settle();
    check(actions == 2, "one after the other: volume only");

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
    unlink(argv[1]);
    return fails ? 1 : 0;
}
