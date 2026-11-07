/* buttons.c with board.grab_keys (the Echo Shows, issue #140): never a grab with a key down, or Android, which saw the
 * key go down, repeats it for as long as hassmic runs.  The grab waits for every key to be up, however long, and nothing
 * read meanwhile is acted on (Android has it); a key that went down between the look and the grab lets the grab go
 * again.  The same for the privacy device (the gating driver's KEY_POWER).  A device that cannot say which keys are
 * down (EVIOCGKEY fails) is grabbed at once, as before.  Keypad and privacy device are FIFOs; EVIOCGKEY and EVIOCGRAB
 * are faked per FIFO (-Wl,--wrap=ioctl), the key state set by the test.
 *   buttons_grab_test FIFO [nokey]   (the privacy device: FIFO.privacy; nokey: EVIOCGKEY fails on the keypad) */
#include "board.h"
#include "buttons.h"
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define PRIVACY "build/buttons_grab.fifo.privacy"           /* board.privacy_input: a constant, so a fixed path */
const struct board board = { .grab_keys = 1, .privacy_input = PRIVACY };
static atomic_int ups, downs, events;
static void on_volume(int d) { if (d > 0) ups++; else downs++; }
static void on_mute(int m) { (void)m; }
static void on_event(int button, int hold) { (void)button; (void)hold; events++; }

/* one input device of the fake: which key it has down, its grab, how often it was grabbed */
struct fake {
    const char *path;
    atomic_int held;                   /* the code of the key down, 0: none */
    atomic_int grabbed, grabs;
    atomic_int press_on_grab;          /* the next grab finds this key down (pressed between the look and the grab) */
    int no_keys;                       /* EVIOCGKEY fails: not an input device */
};
static struct fake keypad, privacy;

static struct fake *fake_of(int fd)
{
    char link[32], path[PATH_MAX], want[PATH_MAX]; ssize_t n;      /* PATH_MAX: realpath's, fortified, aborts on less */
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    if ((n = readlink(link, path, sizeof path - 1)) < 0) return NULL;
    path[n] = 0;
    struct fake *f[2] = { &keypad, &privacy };
    for (int i = 0; i < 2; i++) if (f[i]->path && realpath(f[i]->path, want) && !strcmp(path, want)) return f[i];
    return NULL;
}

int __real_ioctl(int fd, unsigned long req, ...);
int __wrap_ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req);
    struct fake *f = fake_of(fd);
    if (f && req == EVIOCGRAB) {
        int on = va_arg(ap, int);
        va_end(ap);
        f->grabbed = on;
        if (on) { f->grabs++; if (f->press_on_grab) { f->held = f->press_on_grab; f->press_on_grab = 0; } }
        return 0;
    }
    if (f && _IOC_TYPE(req) == 'E' && _IOC_NR(req) == 0x18 && _IOC_DIR(req) == _IOC_READ) {   /* EVIOCGKEY(len) */
        unsigned char *keys = va_arg(ap, unsigned char *);
        va_end(ap);
        if (f->no_keys) return -1;
        memset(keys, 0, _IOC_SIZE(req));
        int k = f->held;
        if (k && k / 8 < (int)_IOC_SIZE(req)) keys[k / 8] |= 1 << (k % 8);
        return 0;
    }
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return __real_ioctl(fd, req, arg);
}

static int fails;
static void key(int fd, int code, int value)
{
    struct input_event ev; memset(&ev, 0, sizeof ev);
    ev.type = EV_KEY; ev.code = code; ev.value = value;
    if (write(fd, &ev, sizeof ev) != sizeof ev) perror("write");
}
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
/* a condition that comes true: looked at until it is, 3 s at most (tests/run.py runs many tests at once) */
#define EVENTUALLY(cond, what) do { for (int i_ = 0; i_ < 150 && !(cond); i_++) usleep(20000); check(cond, what); } while (0)
/* one that must stay false: given time to go wrong first */
#define NEVER(cond, what) do { usleep(300000); check(!(cond), what); } while (0)

static int fifo(const char *path)
{
    unlink(path);
    if (mkfifo(path, 0600)) { perror("mkfifo"); return -1; }
    return open(path, O_RDWR);                              /* the reader's open does not block, nor see EOF */
}

int main(int argc, char **argv)
{
    struct button_handler h = { .volume = on_volume, .mute_changed = on_mute, .event = on_event };
    if (argc < 2) return 2;
    keypad.path = argv[1]; privacy.path = PRIVACY;
    int kfd = fifo(keypad.path), pfd = fifo(privacy.path);
    if (kfd < 0 || pfd < 0) return 2;

    if (argc > 2 && !strcmp(argv[2], "nokey")) {
        keypad.no_keys = 1;
        if (buttons_start(argv[1], &h)) { fprintf(stderr, "buttons_start failed\n"); return 2; }
        EVENTUALLY(keypad.grabbed && keypad.grabs == 1, "EVIOCGKEY fails: grabbed at once");
        key(kfd, KEY_VOLUMEUP, 1); usleep(100000); key(kfd, KEY_VOLUMEUP, 0);
        EVENTUALLY(ups == 1 && events == 1, "and its keys are ours");
        unlink(keypad.path); unlink(privacy.path);
        return fails ? 1 : 0;
    }

    keypad.held = KEY_VOLUMEDOWN; key(kfd, KEY_VOLUMEDOWN, 1);      /* held as hassmic starts: Android saw them go down */
    privacy.held = KEY_POWER; key(pfd, KEY_POWER, 1);
    if (buttons_start(argv[1], &h)) { fprintf(stderr, "buttons_start failed\n"); return 2; }
    for (int i = 0; i < 5; i++) { key(kfd, KEY_VOLUMEDOWN, 2); usleep(50000); }
    NEVER(keypad.grabs || privacy.grabs, "keys down at start: neither device grabbed");

    privacy.held = 0; key(pfd, KEY_POWER, 0);
    EVENTUALLY(privacy.grabbed, "the privacy device's key let go: grabbed");
    check(!keypad.grabs, "the keypad's key still down: not grabbed");

    keypad.press_on_grab = KEY_VOLUMEUP; keypad.held = 0; key(kfd, KEY_VOLUMEDOWN, 0);
    EVENTUALLY(keypad.grabs == 1 && !keypad.grabbed, "let go, but another key down by the grab: let go again");

    keypad.held = 0; key(kfd, KEY_VOLUMEUP, 0);
    EVENTUALLY(keypad.grabbed && keypad.grabs == 2, "every key up: grabbed");
    NEVER(ups || downs || events, "what Android had (presses, repeats, key-ups) was not acted on");

    key(kfd, KEY_VOLUMEUP, 1); usleep(100000); key(kfd, KEY_VOLUMEUP, 0);
    EVENTUALLY(ups == 1 && events == 1, "a press after the grab: ours");
    unlink(keypad.path); unlink(privacy.path);
    return fails ? 1 : 0;
}
