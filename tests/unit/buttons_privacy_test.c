/* buttons.c with board.privacy_button (athena): the gpio-privacy driver changes its state 1.2-1.3 s after the press, so a
 * press is reported at once as the opposite of the latch; the latch confirms it with no second report, and a press the
 * latch never follows is taken back after privacy_settle_ms.  Presses while one is still due do not count.
 *   buttons_privacy_test FIFO (the keypad; the button FIFO and the state file are in build/) */
#include "board.h"
#include "buttons.h"
#include <fcntl.h>
#include <linux/input.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE  "build/buttons_privacy.state"
#define BUTTON "build/buttons_privacy.button"
const struct board board = { .privacy_state = STATE, .privacy_latch = 1, .privacy_button = BUTTON, .privacy_settle_ms = 1500 };
static atomic_int reports, last = -1;
static void on_mute(int m) { reports++; last = m; }

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static void latch(int on) { FILE *f = fopen(STATE, "w"); fputc(on ? '1' : '0', f); fclose(f); }
static void press(int fd)
{
    struct input_event ev; memset(&ev, 0, sizeof ev);
    ev.type = EV_KEY; ev.code = KEY_POWER; ev.value = 1;
    if (write(fd, &ev, sizeof ev) != sizeof ev) perror("write");
    ev.value = 0;
    if (write(fd, &ev, sizeof ev) != sizeof ev) perror("write");
}

int main(int argc, char **argv)
{
    struct button_handler h = { .mute_changed = on_mute };
    if (argc != 2) return 2;
    unlink(argv[1]); unlink(BUTTON);
    if (mkfifo(argv[1], 0600) || mkfifo(BUTTON, 0600)) { perror("mkfifo"); return 2; }
    int keep = open(argv[1], O_RDWR), bfd = open(BUTTON, O_RDWR);
    latch(0);
    if (buttons_start(argv[1], &h)) { fprintf(stderr, "buttons_start failed\n"); return 2; }

    press(bfd); usleep(100000);
    check(reports == 1 && last == 1 && buttons_muted() == 1, "press: muted reported at once, before the latch");
    press(bfd); usleep(100000);
    check(reports == 1 && buttons_muted() == 1, "a second press while the first is due: ignored");
    usleep(1100000); latch(1); usleep(1200000);           /* the driver, 1.2 s after the press */
    check(reports == 1 && last == 1 && buttons_muted() == 1, "the latch confirms: no second report");

    press(bfd); usleep(100000);
    check(reports == 2 && last == 0 && buttons_muted() == 0, "press again: unmuted at once");
    usleep(2500000);                                       /* the latch never follows */
    check(reports == 3 && last == 1 && buttons_muted() == 1, "not confirmed within privacy_settle_ms: taken back");

    latch(0); usleep(1200000);
    check(reports == 4 && last == 0, "a latch change without a press: reported by the poll");
    close(keep); close(bfd); unlink(argv[1]); unlink(BUTTON); unlink(STATE);
    return fails ? 1 : 0;
}
