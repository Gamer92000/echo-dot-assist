/* What tests/unit/plugrun_test.py has the launcher start (a static program, as a plugin is): it says on its stderr
 * what the sandbox let it do, one word a check, then does what its control socket tells it.
 *   "exit <n>": ends with that status;  "abort": abort() (tgkill to itself);  "spin": never ends, ignoring SIGTERM;
 *   "burn": nothing but CPU from then on, in a second thread (what the launcher counts is the whole process's). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static void *thread(void *a) { *(int *)a = 1; return NULL; }
static void *burn(void *a) { volatile unsigned long n = 0; (void)a; for (;;) n++; return NULL; }
static const char *err(long r) { return r >= 0 ? "ok" : errno == EPERM ? "EPERM" : errno == ENOSYS ? "ENOSYS" : strerror(errno); }

int main(int argc, char **argv, char **envp)
{
    char line[64]; int ran = 0; pthread_t t; struct timespec ts; struct rlimit rl; ssize_t n;
    fprintf(stderr, "argv0=%s argc=%d env=%s\n", argv[0], argc, envp[0] ? envp[0] : "-");
    fprintf(stderr, "open=%s\n", err(syscall(SYS_openat, AT_FDCWD, "/etc/passwd", O_RDONLY)));
    fprintf(stderr, "socket=%s\n", err(syscall(SYS_socket, AF_INET, SOCK_DGRAM, 0)));
    fprintf(stderr, "fork=%s\n", err(syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0)));
    fprintf(stderr, "execve=%s\n", err(syscall(SYS_execve, "/bin/true", argv, envp)));
    fprintf(stderr, "kill=%s\n", err(syscall(SYS_kill, 1, 0)));
    fprintf(stderr, "setfl=%s\n", err(fcntl(3, F_SETFL, O_NONBLOCK)));
    fprintf(stderr, "getfl=%s\n", err(fcntl(3, F_GETFL)));
    fprintf(stderr, "mmap_exec=%s\n", err((long)syscall(SYS_mmap, 0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == -1 ? -1 : 0));
    fprintf(stderr, "mmap_fd=%s\n", err((long)syscall(SYS_mmap, 0, 4096, PROT_READ, MAP_PRIVATE, 2, 0) == -1 ? -1 : 0));
    fprintf(stderr, "mmap=%s\n", err((long)syscall(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == -1 ? -1 : 0));
    fprintf(stderr, "clock=%s\n", err(clock_gettime(CLOCK_MONOTONIC, &ts)));
    fprintf(stderr, "thread=%s\n", !pthread_create(&t, NULL, thread, &ran) && !pthread_join(t, NULL) && ran ? "ok" : "failed");
    fprintf(stderr, "fd0=%s fd4=%s fd5=%s\n", err(fcntl(0, F_GETFD)), fcntl(4, F_GETFD) < 0 ? "closed" : "open", fcntl(5, F_GETFD) < 0 ? "closed" : "open");
    getrlimit(RLIMIT_NOFILE, &rl);
    fprintf(stderr, "nofile=%ld\n", (long)rl.rlim_cur);
    getrlimit(RLIMIT_AS, &rl);
    fprintf(stderr, "as_mb=%ld\n", (long)(rl.rlim_cur >> 20));
    fprintf(stderr, "ready\n");
    while ((n = recv(3, line, sizeof line - 1, 0)) > 0) {
        line[n] = 0;
        if (!strncmp(line, "exit ", 5)) return atoi(line + 5);
        if (!strcmp(line, "abort")) abort();
        if (!strcmp(line, "burn")) { fprintf(stderr, pthread_create(&t, NULL, burn, NULL) ? "no thread\n" : "burning\n"); continue; }
        if (!strcmp(line, "spin")) { signal(SIGTERM, SIG_IGN); fprintf(stderr, "spinning\n"); for (;;) poll(NULL, 0, 1000); }
    }
    return 0;
}
