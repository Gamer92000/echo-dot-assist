/* The Echo Show's camera for one viewer (camera.h, docs/camera.md).
 *
 * The child: src/cam/cam.c as libhmcam.so in an app_process of its own, started for a stream and gone with it (no
 * camera process while nobody looks; some 3 s from the start to the first frame on checkers, most of it the VM, which
 * Home Assistant's go2rtc and stream both wait out).  Started as screen.c starts the UI: everything made before the
 * fork, pipes close-on-exec, every other descriptor closed in the child, execve.  hassmic ignores SIGCHLD, so the
 * kernel reaps it; stopping it = its stdin closed (cam.c lets the camera go and exits), SIGKILL 2 s on if it hangs.
 *
 * One viewer at a time: the integration relays one stream to every viewer in Home Assistant, so a second request is
 * a second integration (or a stale one) and gets "busy" rather than taking the first one's camera.
 *
 * Its own lock, never held across a read of the child's pipe: the reader is web.c's connection thread. */
#define _GNU_SOURCE
#include "camera.h"
#include "board.h"
#include "core.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REC_MAX (4 << 20)               /* a record's bytes at most: a 1280x720 key frame is 30-60 kB at 1.5 Mbit/s */

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static int on, covered = -1, id_next = 1;
static struct { int id; pid_t pid; int in, out, ended; } cur = { 0, 0, -1, -1, 0 };

static long long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

static const char *next_to_me(const char *file, char *buf, size_t n)       /* as screen.c's */
{
    ssize_t k = readlink("/proc/self/exe", buf, n - 32);
    if (k <= 0) { snprintf(buf, n, "/system/hassmic/%s", file); return buf; }
    buf[k] = 0;
    char *s = strrchr(buf, '/');
    snprintf(s ? s + 1 : buf, n - (size_t)(s ? s + 1 - buf : 0), "%s", file);
    return buf;
}

int camera_present(void)
{
    static int have = -1;                                       /* looked once: an update restarts hassmic */
    if (have < 0) {
        char dex[PATH_MAX], lib[PATH_MAX];
        have = getenv("HASSMIC_FAKE_CAMERA") ? 1 : board.camera && !access(next_to_me("screen.dex", dex, sizeof dex), R_OK) &&
                                                   !access(next_to_me("libhmcam.so", lib, sizeof lib), R_OK);
    }
    return have;
}

/* the PC's shutter: HASSMIC_FAKE_LENS names a file, "1" in it covered (looked at whenever asked) */
static int lens_now(void)
{
    const char *f = getenv("HASSMIC_FAKE_LENS"); char c = 0;
    if (f) { FILE *p = fopen(f, "r"); if (p) { c = (char)fgetc(p); fclose(p); } return c == '1'; }
    return covered;
}

static void stop_locked(const char *why)
{
    if (!cur.id) return;
    if (cur.in >= 0) { close(cur.in); cur.in = -1; }            /* cam.c: stdin's end, the camera let go */
    if (cur.pid > 0) {
        pid_t p = cur.pid;
        for (int i = 0; i < 20 && !kill(p, 0); i++) usleep(100000);
        if (!kill(p, 0)) { kill(p, SIGKILL); fprintf(stderr, "camera: the camera process did not stop, killed\n"); }
    }
    cur.pid = 0; cur.ended = 1;
    fprintf(stderr, "camera: stream %d ended: %s\n", cur.id, why);
}

int camera_enable(int v)
{
    pthread_mutex_lock(&lk);
    if (v >= 0) { on = !!v; if (!on) stop_locked("the camera switched off"); }
    int r = on;
    pthread_mutex_unlock(&lk);
    return r;
}

void camera_lens(int c)
{
    pthread_mutex_lock(&lk);
    if (covered != c) fprintf(stderr, "camera: lens %s\n", c ? "covered" : "open");
    covered = c;
    if (c) stop_locked("the lens was covered");
    pthread_mutex_unlock(&lk);
}

int camera_covered(void) { pthread_mutex_lock(&lk); int c = lens_now(); pthread_mutex_unlock(&lk); return c; }
int camera_streaming(void) { pthread_mutex_lock(&lk); int s = cur.id && !cur.ended; pthread_mutex_unlock(&lk); return s; }

size_t camera_json(char *o, size_t cap)
{
    pthread_mutex_lock(&lk);
    int n = snprintf(o, cap, "{\"present\":%d,\"on\":%d,\"covered\":%d,\"streaming\":%d}", camera_present(), on, lens_now(), cur.id && !cur.ended);
    pthread_mutex_unlock(&lk);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

/* lk held */
static pid_t spawn(int *in, int *out)
{
    int pin[2], pout[2]; char dex[PATH_MAX], lib[PATH_MAX], cp[PATH_MAX + 16];
    const char *fake = getenv("HASSMIC_FAKE_CAMERA");
    next_to_me("screen.dex", dex, sizeof dex); next_to_me("libhmcam.so", lib, sizeof lib);
    snprintf(cp, sizeof cp, "CLASSPATH=%s", dex);
    /* HMCAM_DOT=1: pictures leave the Echo, the red dot shows (cam.c); never 0, and none inherited */
    static char dot[] = "HMCAM_DOT=1";
    extern char **environ;
    size_t ne = 0; while (environ[ne]) ne++;
    char **envp = malloc((ne + 3) * sizeof *envp);
    if (!envp) return -1;
    size_t m = 0; envp[m++] = cp; envp[m++] = dot;
    for (size_t i = 0; i < ne; i++) if (strncmp(environ[i], "CLASSPATH=", 10) && strncmp(environ[i], "HMCAM_DOT=", 10)) envp[m++] = environ[i];
    envp[m] = NULL;
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
    char *argv_[6] = { NULL }; const char *prog;
    if (fake) { prog = fake; argv_[0] = (char *)fake; }
    else { prog = "/system/bin/app_process"; argv_[0] = "app_process"; argv_[1] = "/"; argv_[2] = "Screen"; argv_[3] = lib; }
    if (pipe2(pin, O_CLOEXEC)) { free(envp); return -1; }
    if (pipe2(pout, O_CLOEXEC)) { close(pin[0]); close(pin[1]); free(envp); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        int i = fcntl(pin[0], F_DUPFD, 3), o = fcntl(pout[1], F_DUPFD, 3);   /* never a dup2 onto itself */
        if (i < 0 || o < 0 || dup2(i, 0) < 0 || dup2(o, 1) < 0) _exit(127);
        for (long d = 3; d < maxfd; d++) close((int)d);          /* none of hassmic's sockets in the VM; stderr: the log */
        execve(prog, argv_, envp);
        _exit(127);
    }
    free(envp);
    close(pin[0]); close(pout[1]);
    if (pid < 0) { close(pin[1]); close(pout[0]); return -1; }
    *in = pin[1]; *out = pout[0];
    return pid;
}

int camera_begin(const char **why)
{
    int r = -1;
    pthread_mutex_lock(&lk);
    if (!camera_present()) *why = "absent";
    else if (!on) *why = "off";
    else if (lens_now() == 1) *why = "covered";
    /* busy until the last stream's reader let go of its pipe (camera_end): closed here, the number could come back
     * for the new pipe under a reader still polling it */
    else if ((cur.id && !cur.ended) || cur.out >= 0) *why = "busy";
    else {
        int in, out; pid_t pid = spawn(&in, &out);
        if (pid < 0) *why = "failed";
        else {
            cur.id = id_next++; cur.pid = pid; cur.in = in; cur.out = out; cur.ended = 0; r = cur.id;
            fprintf(stderr, "camera: stream %d starting\n", r);
        }
    }
    pthread_mutex_unlock(&lk);
    return r;
}

static int read_full(int fd, void *p, size_t n, long long deadline)
{
    uint8_t *c = p;
    while (n) {
        struct pollfd q = { fd, POLLIN, 0 };
        int w = (int)(deadline - ms()); if (w < 0) return -1;
        int k = poll(&q, 1, w);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        ssize_t got = read(fd, c, n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        c += got; n -= (size_t)got;
    }
    return 0;
}

int camera_next(int id, int *type, long long *pts_us, uint8_t **data, size_t *len, int wait_ms)
{
    pthread_mutex_lock(&lk);
    if (cur.id == id && !cur.ended && lens_now() == 1) stop_locked("the lens was covered");
    int fd = cur.id == id && !cur.ended ? cur.out : -1;
    pthread_mutex_unlock(&lk);
    if (fd < 0) return -1;
    struct pollfd q = { fd, POLLIN, 0 };
    int k = poll(&q, 1, wait_ms);
    if (k == 0 || (k < 0 && errno == EINTR)) return 0;
    uint8_t h[20];
    /* a record once begun comes whole: 2 s for the rest of it (cam.c writes header and bytes back to back) */
    if (k < 0 || read_full(fd, h, sizeof h, ms() + 2000) || memcmp(h, "HMCV", 4)) goto gone;
    uint32_t n = (uint32_t)h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24;
    unsigned long long p = 0;
    for (int i = 7; i >= 0; i--) p = p << 8 | h[12 + i];
    if (n > REC_MAX || !(*data = malloc(n ? n : 1))) goto gone;
    if (read_full(fd, *data, n, ms() + 2000)) { free(*data); *data = NULL; goto gone; }
    *type = h[4]; *pts_us = (long long)p; *len = n;
    return 1;
gone:
    pthread_mutex_lock(&lk);
    if (cur.id == id && !cur.ended) stop_locked("the camera process stopped (its log above says why)");
    pthread_mutex_unlock(&lk);
    return -1;
}

void camera_end(int id)
{
    pthread_mutex_lock(&lk);
    if (cur.id == id) {
        if (!cur.ended) stop_locked("the viewer went");
        if (cur.out >= 0) { close(cur.out); cur.out = -1; }
    }
    pthread_mutex_unlock(&lk);
}
