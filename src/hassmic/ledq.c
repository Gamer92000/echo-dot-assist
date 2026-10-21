/* hassmic's ledctrl calls in order: see ledq.h.
 *
 * Why: run_argv forks ledctrl and does not wait, so two calls started together reach ledcontroller in either order,
 * and an unset that overtakes its set leaves the pattern on until the next unset of it.  Measured on a Dot 2
 * (2026-10-16, the ring's frame read back): ledctrl -s and -u of active-talking started at once left the ring lit 2
 * times in 5, the -u after the -s returned never (0 in 15).  Seen after replies of 19 and 41 ms (talking set and unset
 * in that time): the talking ring on an idle Echo until the next wake word.  The same for brightness: a slider dragged
 * in Home Assistant sends levels ms apart, and the last one to arrive stayed, not the last one sent.
 *
 * Waited for by a pipe the child keeps open across exec and the kernel closes when ledctrl exits: SIGCHLD is ignored
 * in hassmic, so there is no exit status to wait for, and stdout is no measure (ledctrl chats on it, any amount).
 * The pipe also gives the timeout: a ledctrl that hangs (ledcontroller restarting) is killed, the ring goes on. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                     /* pipe2 on glibc */
#endif
#include "ledq.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct call { int n; char arg[LEDQ_ARGS][48]; };
static struct call q[LEDQ_MAX];
static int head, len, running;
static char path[128], fake[256];
static void (*child_fn)(void);
static int timeout = LEDQ_TIMEOUT_MS, started;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t more = PTHREAD_COND_INITIALIZER, done = PTHREAD_COND_INITIALIZER;

/* the arguments of NC calls, space-separated, as far as they fit */
static const char *text(const struct call *c, int nc, char *buf, size_t n)
{
    size_t l = 0; buf[0] = 0;
    for (int k = 0; k < nc; k++)
        for (int i = 0; i < c[k].n && l < n; i++) l += (size_t)snprintf(buf + l, n - l, "%s%s", l ? " " : "", c[k].arg[i]);
    return buf;
}

static int pattern(const struct call *c) { return c->n == 2 && (!strcmp(c->arg[0], "-s") || !strcmp(c->arg[0], "-u")); }

/* 1: still running after MS, killed */
static int exec_one(const char *bin, void (*fn)(void), int ms, const struct call *c, int nc)
{
    int p[2]; char what[200];
    if (pipe2(p, O_CLOEXEC)) { fprintf(stderr, "light ring: no pipe for ledctrl (%s), %s dropped\n", strerror(errno), text(c, nc, what, sizeof what)); return 0; }
    pid_t pid = fork();
    if (pid == 0) {                     /* only async-signal-safe calls from here: we are a copy of a threaded process */
        char *argv[LEDQ_BATCH * LEDQ_ARGS + 2] = { (char *)bin }; int a = 1;
        for (int k = 0; k < nc; k++) for (int i = 0; i < c[k].n; i++) argv[a++] = (char *)c[k].arg[i];
        setpgid(0, 0);                  /* a group of its own: the kill below reaches what it started, nothing else */
        if (fn) fn();
        int nul = open("/dev/null", O_WRONLY | O_CLOEXEC);  /* its chat (two log lines per call) not in the log; */
        if (nul >= 0) dup2(nul, 1);                         /* its errors (stderr) are */
        fcntl(p[1], F_SETFD, 0);                            /* kept across exec: closed when ledctrl exits */
        execv(bin, argv);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) { fprintf(stderr, "light ring: no process for ledctrl (%s), %s dropped\n", strerror(errno), text(c, nc, what, sizeof what)); close(p[0]); return 0; }
    struct pollfd fd = { .fd = p[0], .events = POLLIN };
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    int r, left = ms;
    while ((r = poll(&fd, 1, left)) < 0 && errno == EINTR) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        left = ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left < 0) left = 0;
    }
    /* Still open: ledctrl, or a child of it, still runs.  The process group outlives ledctrl while any of them does, and
     * its id cannot be handed out again before the group is gone: kill(-pid) never reaches a stranger, even with
     * ledctrl itself already reaped (SIGCHLD ignored) */
    if (r == 0) {
        kill(-pid, SIGKILL);
        fprintf(stderr, "light ring: ledctrl %s still running after %d ms, killed\n", text(c, nc, what, sizeof what), ms);
    }
    close(p[0]);
    return r == 0;
}

static void *thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct call c[LEDQ_BATCH]; char bin[sizeof path]; void (*fn)(void); int ms, nc = 0;
        pthread_mutex_lock(&lock);
        while (!len) pthread_cond_wait(&more, &lock);
        do { c[nc++] = q[head]; head = (head + 1) % LEDQ_MAX; len--; }
        while (len && nc < LEDQ_BATCH && pattern(&c[0]) && pattern(&q[head]));
        running = 1;
        memcpy(bin, path, sizeof bin); fn = child_fn; ms = timeout;
        pthread_mutex_unlock(&lock);
        /* A batch killed took the calls after the one that hung with it: its unsets again, each alone (an unset lost
         * leaves a pattern on for good; a set lost only misses one animation) */
        if (bin[0] && exec_one(bin, fn, ms, c, nc) && nc > 1)
            for (int k = 0; k < nc; k++) if (!strcmp(c[k].arg[0], "-u")) exec_one(bin, fn, ms, &c[k], 1);
        pthread_mutex_lock(&lock);
        running = 0;
        if (!len) pthread_cond_broadcast(&done);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int ledq_start(const char *ledctrl, const char *fake_file, void (*child)(void), int timeout_ms)
{
    int r = 0;
    pthread_mutex_lock(&lock);
    snprintf(path, sizeof path, "%s", ledctrl ? ledctrl : "");
    snprintf(fake, sizeof fake, "%s", fake_file ? fake_file : "");
    child_fn = child; timeout = timeout_ms > 0 ? timeout_ms : LEDQ_TIMEOUT_MS;
    if (!started && path[0]) {
        pthread_t t;
        if (pthread_create(&t, NULL, thread, NULL)) { fprintf(stderr, "light ring: no thread for ledctrl, the ring stays as it is\n"); r = -1; }
        else { pthread_detach(t); started = 1; }
    }
    pthread_mutex_unlock(&lock);
    return r;
}

/* Full (never seen: ledctrl takes a few ms): a queued set gives way, the oldest; an unset lost leaves a pattern on for
 * good (the bug this is for), a set lost only misses one animation.  Without a set in the queue the new call is
 * dropped: 64 unsets and brightness calls waiting, while ring.c has at most 24 patterns on to unset. */
void ledq_args(int n, const char *const args[])
{
    struct call c = { .n = n < LEDQ_ARGS ? n : LEDQ_ARGS };
    char what[200];
    for (int i = 0; i < c.n; i++) snprintf(c.arg[i], sizeof c.arg[i], "%s", args[i]);
    pthread_mutex_lock(&lock);
    if (fake[0]) {                      /* at once, in the caller: the PC tests read the file right after an action */
        FILE *f = fopen(fake, "a");
        if (f) { fprintf(f, "%s\n", text(&c, 1, what, sizeof what)); fclose(f); }
        pthread_mutex_unlock(&lock);
        return;
    }
    if (!started) { pthread_mutex_unlock(&lock); return; }
    if (len == LEDQ_MAX) {
        int i = 0;
        while (i < len && strcmp(q[(head + i) % LEDQ_MAX].arg[0], "-s")) i++;
        if (i == len) {
            fprintf(stderr, "light ring: ledctrl falls behind, %s dropped\n", text(&c, 1, what, sizeof what));
            pthread_mutex_unlock(&lock);
            return;
        }
        fprintf(stderr, "light ring: ledctrl falls behind, %s dropped\n", text(&q[(head + i) % LEDQ_MAX], 1, what, sizeof what));
        for (; i + 1 < len; i++) q[(head + i) % LEDQ_MAX] = q[(head + i + 1) % LEDQ_MAX];
        len--;
    }
    q[(head + len++) % LEDQ_MAX] = c;
    pthread_cond_signal(&more);
    pthread_mutex_unlock(&lock);
}

void ledq_run(const char *op, const char *pattern)
{
    const char *args[] = { op, pattern };
    ledq_args(2, args);
}

int ledq_wait(int ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000; until.tv_nsec += (ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    int r = 0, busy;
    pthread_mutex_lock(&lock);
    while ((len || running) && !r) r = pthread_cond_timedwait(&done, &lock, &until);
    busy = len || running;
    pthread_mutex_unlock(&lock);
    return busy ? -1 : 0;
}
