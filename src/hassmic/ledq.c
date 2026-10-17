/* The ring's ledctrl calls in order: see ledq.h.
 *
 * Why: run_argv forks ledctrl and does not wait, so two calls started together reach ledcontroller in either order,
 * and an unset that overtakes its set leaves the pattern on until the next unset of it.  Measured on a Dot 2
 * (2026-10-16, the ring's frame read back): ledctrl -s and -u of active-talking started at once left the ring lit 2
 * times in 5, the -u after the -s returned never (0 in 15).  Seen after replies of 19 and 41 ms (talking set and unset
 * in that time): the talking ring on an idle Echo until the next wake word.
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

static struct { char op[4], pattern[48]; } q[LEDQ_MAX];
static int head, len, running;
static char path[128], fake[256];
static void (*child_fn)(void);
static int timeout = LEDQ_TIMEOUT_MS, started;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t more = PTHREAD_COND_INITIALIZER, done = PTHREAD_COND_INITIALIZER;

static void exec_one(const char *bin, void (*fn)(void), int ms, const char *op, const char *pattern)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC)) { fprintf(stderr, "light ring: no pipe for ledctrl (%s), %s %s dropped\n", strerror(errno), op, pattern); return; }
    pid_t pid = fork();
    if (pid == 0) {                     /* only async-signal-safe calls from here: we are a copy of a threaded process */
        if (fn) fn();
        int nul = open("/dev/null", O_WRONLY);          /* its chat (two log lines per call) not in the log; its */
        if (nul >= 0) dup2(nul, 1);                     /* errors (stderr) are */
        fcntl(p[1], F_SETFD, 0);                        /* kept across exec: closed when ledctrl exits */
        execl(bin, bin, op, pattern, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) { fprintf(stderr, "light ring: no process for ledctrl (%s), %s %s dropped\n", strerror(errno), op, pattern); close(p[0]); return; }
    struct pollfd fd = { .fd = p[0], .events = POLLIN };
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    int r, left = ms;
    while ((r = poll(&fd, 1, left)) < 0 && errno == EINTR) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        left = ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left < 0) left = 0;
    }
    if (r == 0) {                       /* the pipe still open: ledctrl has not exited, so pid is still ours to kill */
        kill(pid, SIGKILL);
        fprintf(stderr, "light ring: ledctrl %s %s still running after %d ms, killed\n", op, pattern, ms);
    }
    close(p[0]);
}

static void *thread(void *arg)
{
    (void)arg;
    for (;;) {
        char op[4], pattern[48], bin[sizeof path], file[sizeof fake];
        void (*fn)(void); int ms;
        pthread_mutex_lock(&lock);
        while (!len) pthread_cond_wait(&more, &lock);
        memcpy(op, q[head].op, sizeof op); memcpy(pattern, q[head].pattern, sizeof pattern);
        head = (head + 1) % LEDQ_MAX; len--; running = 1;
        memcpy(bin, path, sizeof bin); memcpy(file, fake, sizeof file); fn = child_fn; ms = timeout;
        pthread_mutex_unlock(&lock);
        if (file[0]) { FILE *f = fopen(file, "a"); if (f) { fprintf(f, "%s %s\n", op, pattern); fclose(f); } }
        else if (bin[0]) exec_one(bin, fn, ms, op, pattern);
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
    if (!started && (path[0] || fake[0])) {
        pthread_t t;
        if (pthread_create(&t, NULL, thread, NULL)) { fprintf(stderr, "light ring: no thread for ledctrl, the ring stays as it is\n"); r = -1; }
        else { pthread_detach(t); started = 1; }
    }
    pthread_mutex_unlock(&lock);
    return r;
}

/* Full (never seen: ledctrl takes a few ms): a queued set gives way, the oldest.  An unset lost leaves a pattern on
 * for good (the bug this is for), a set lost only misses one animation. */
void ledq_run(const char *op, const char *pattern)
{
    pthread_mutex_lock(&lock);
    if (!started) { pthread_mutex_unlock(&lock); return; }
    if (len == LEDQ_MAX) {
        int i = 0;
        while (i < len && strcmp(q[(head + i) % LEDQ_MAX].op, "-s")) i++;
        if (i == len) {
            fprintf(stderr, "light ring: ledctrl falls behind, %s %s dropped\n", op, pattern);
            pthread_mutex_unlock(&lock);
            return;
        }
        fprintf(stderr, "light ring: ledctrl falls behind, %s %s dropped\n", q[(head + i) % LEDQ_MAX].op, q[(head + i) % LEDQ_MAX].pattern);
        for (; i + 1 < len; i++) q[(head + i) % LEDQ_MAX] = q[(head + i + 1) % LEDQ_MAX];
        len--;
    }
    int i = (head + len++) % LEDQ_MAX;
    snprintf(q[i].op, sizeof q[i].op, "%s", op); snprintf(q[i].pattern, sizeof q[i].pattern, "%s", pattern);
    pthread_cond_signal(&more);
    pthread_mutex_unlock(&lock);
}

int ledq_wait(int ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000; until.tv_nsec += (ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    int r = 0;
    pthread_mutex_lock(&lock);
    while ((len || running) && !r) r = pthread_cond_timedwait(&done, &lock, &until);
    pthread_mutex_unlock(&lock);
    return len || running ? -1 : 0;
}
