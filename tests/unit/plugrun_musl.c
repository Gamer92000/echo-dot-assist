/* What tests/unit/plugrun_test.py has the launcher start as a musl program, which plugins are (docs/plugins.md 5.8:
 * zig cc or a musl cross gcc, never bionic or glibc): musl's raise(), pthread_kill(), pthread_cancel() and abort()
 * send their signals with tkill, where glibc's (plugrun_child.c) use tgkill, and only tgkill was allowed: raise()
 * answered EPERM and its handler never ran, abort() fell through to its last resort and ended as SIGSEGV, and with
 * the development flag the first raise() ended the plugin as SIGSYS (a review of 6631974).  One word a check on its
 * stderr, then "ready"; then, on the control socket's "abort", abort().  Built by the Makefile's unit-plugrun with
 * zig cc -target x86_64-linux-musl (skipped, and said, where there is no zig) */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t got;
static void on(int s) { (void)s; got++; }
/* (poll, a cancellation point: pause() is a call of its own on x86_64, which the table has not; ARM has none, and
 * musl's pause() is ppoll there) */
static void *wait_forever(void *a) { (void)a; for (;;) poll(NULL, 0, -1); return NULL; }

int main(void)
{
    char line[16]; pthread_t t; int r; ssize_t n;
    signal(SIGUSR1, on);
    r = raise(SIGUSR1);
    fprintf(stderr, "raise=%s handled=%d\n", r ? strerror(errno) : "ok", (int)got);
    if (pthread_create(&t, NULL, wait_forever, NULL)) { fprintf(stderr, "thread=failed\nready\n"); return 1; }
    r = pthread_kill(t, SIGUSR1);
    for (int i = 0; i < 100 && got < 2; i++) usleep(10000);         /* (the thread's handler runs on its own time) */
    fprintf(stderr, "pthread_kill=%s handled=%d\n", r ? strerror(r) : "ok", (int)got);
    r = pthread_cancel(t);
    if (!r) r = pthread_join(t, NULL);
    fprintf(stderr, "pthread_cancel=%s\n", r ? strerror(r) : "ok");
    fprintf(stderr, "ready\n");
    while ((n = recv(3, line, sizeof line - 1, 0)) > 0) {
        line[n] = 0;
        if (!strcmp(line, "abort")) abort();
        if (!strcmp(line, "exit")) return 0;
    }
    return 0;
}
