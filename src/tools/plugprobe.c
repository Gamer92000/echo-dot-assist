/* The sandbox, asked from inside (docs/plugins.md 2.4, 6): a static program the launcher starts as it starts a plugin,
 * which makes every call the filter has a rule about, both ways, with raw system calls, and says for each whether the
 * filter did what sdk/filter.def says.  One line a check on its stderr, "wrong N" at the end, its exit status the
 * count (255 at most).
 *
 *   on an Echo:  scripts/plugin-probe.sh       (built with musl: a static bionic program does not start in there)
 *   on the PC:   make unit-plugrun             (built for x86_64, against the filter's x86_64 translation)
 *
 * "refused" means the filter's own answer: EPERM, or ENOSYS for numbers above 386 on ARM.  "passed" means the call
 * reached the kernel, whatever the kernel then said (a TCP option on a socket that is not TCP gets ENOPROTOOPT: it
 * passed).  Raw calls, because a libc may turn the answer into another one: musl reports the filter's EPERM on an
 * anonymous mmap as ENOMEM. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#ifndef MADV_FREE
#define MADV_FREE 8
#endif
#ifdef SYS_mmap2
#define MMAP SYS_mmap2
#else
#define MMAP SYS_mmap
#endif
#ifdef SYS_fcntl64
#define FCNTL SYS_fcntl64
#else
#define FCNTL SYS_fcntl
#endif
#ifndef SYS_getrandom
#define SYS_getrandom (-1)
#endif

static int wrong, checks;

static void say(const char *what, int ok, const char *got)
{
    char line[200]; int n = snprintf(line, sizeof line, "%s %s: %s\n", ok ? "ok   " : "WRONG", what, got);
    checks++; wrong += !ok;
    if (write(2, line, (size_t)n) < 0) { }
}

/* the call's answer was the filter's refusal */
static void refused(const char *what, long r)
{
    int e = errno;
    say(what, r == -1 && e == EPERM, r != -1 ? "it went through" : e == EPERM ? "refused" : strerror(e));
}
__attribute__((unused)) static void nosys(const char *what, long r)      /* ARM only */
{
    int e = errno;
    say(what, r == -1 && e == ENOSYS, r != -1 ? "it went through" : e == ENOSYS ? "no such call" : strerror(e));
}
/* the call reached the kernel */
static void passed(const char *what, long r)
{
    int e = errno;
    say(what, r != -1 || e != EPERM, r != -1 ? "passed" : e == EPERM ? "refused" : strerror(e));
}

static void *thread(void *a) { *(int *)a = 1; return NULL; }

int main(int argc, char **argv, char **envp)
{
    static int word; int ran = 0, sv[2] = { -1, -1 }, v = 1; socklen_t l = sizeof v; pthread_t t; struct timespec ts; struct rlimit rl; char name[16] = "probe";
    long page = 4096; void *p;
    (void)argc;

    /* ---- no file system, no sockets of its own making, no other process or program */
    refused("open", syscall(SYS_openat, AT_FDCWD, "/proc/self/status", O_RDONLY));
#ifdef SYS_open
    refused("open (the old call)", syscall(SYS_open, "/dev/null", O_RDONLY));
#endif
    refused("socket", syscall(SYS_socket, AF_INET, SOCK_DGRAM, 0));
    refused("socketpair", syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sv));
    refused("connect", syscall(SYS_connect, 3, &v, sizeof v));
    refused("bind", syscall(SYS_bind, 3, &v, sizeof v));
    refused("accept4", syscall(SYS_accept4, 3, 0, 0, 0));
    refused("sendmsg", syscall(SYS_sendmsg, 3, &v, 0));
#ifdef SYS_fork
    refused("fork", syscall(SYS_fork));
#endif
    refused("clone as a process", syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0));
    refused("clone with bionic's thread flags", syscall(SYS_clone, 0x3d0f00 & ~0x100000, 0, 0, 0, 0));
    refused("execve again", syscall(SYS_execve, "/system/bin/sh", argv, envp));
    refused("kill", syscall(SYS_kill, 1, 0));
    refused("tgkill to another process", syscall(SYS_tgkill, 1, 1, 0));
    passed("tgkill to itself", syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), 0));
    passed("tkill to its own thread (musl's raise, abort, pthread_kill)", syscall(SYS_tkill, syscall(SYS_gettid), 0));
    refused("ptrace", syscall(SYS_ptrace, 0, 0, 0, 0));
    refused("personality", syscall(SYS_personality, 0xffffffff));
    refused("setpriority", syscall(SYS_setpriority, 0, 0, 0));
    refused("memfd_create", syscall(SYS_memfd_create, "x", 0));
#ifdef __arm__
    nosys("a call above 386 (clock_gettime64)", syscall(403, 0, &ts));
    nosys("bpf's neighbour (387)", syscall(387, 0, 0, 0));
#endif

    /* ---- descriptors it holds */
    passed("fcntl F_GETFD", syscall(FCNTL, 3, F_GETFD));
    passed("fcntl F_GETFL", syscall(FCNTL, 3, F_GETFL));
    passed("fcntl F_DUPFD_CLOEXEC", syscall(FCNTL, 3, F_DUPFD_CLOEXEC, 10));
    refused("fcntl F_SETFL", syscall(FCNTL, 3, F_SETFL, O_NONBLOCK));
    refused("fcntl F_SETOWN", syscall(FCNTL, 3, F_SETOWN, 1));
    refused("fcntl F_DUPFD", syscall(FCNTL, 3, F_DUPFD, 10));
#ifdef SYS_fcntl64
    passed("the old fcntl F_GETFD (Go's runtime asks so)", syscall(SYS_fcntl, 0, F_GETFD));
    refused("the old fcntl F_SETFL", syscall(SYS_fcntl, 3, F_SETFL, O_NONBLOCK));
#endif
    passed("ioctl FIONREAD", syscall(SYS_ioctl, 3, FIONREAD, &v));
    refused("ioctl FIONBIO", syscall(SYS_ioctl, 3, FIONBIO, &v));
    refused("ioctl TIOCSTI", syscall(SYS_ioctl, 0, TIOCSTI, "x"));

    /* ---- the sockets it was handed (the control socket stands for them) */
    passed("sendto without an address, MSG_DONTWAIT", syscall(SYS_sendto, 3, "", 0, MSG_DONTWAIT | MSG_NOSIGNAL, 0, 0));
    refused("sendto with an address", syscall(SYS_sendto, 3, "", 0, 0, &v, sizeof v));
    refused("sendto with MSG_OOB", syscall(SYS_sendto, 3, "", 0, MSG_OOB, 0, 0));
#ifdef SYS_send
    refused("send with MSG_OOB", syscall(SYS_send, 3, "", 0, MSG_OOB));
    passed("send with MSG_NOSIGNAL", syscall(SYS_send, 3, "", 0, MSG_NOSIGNAL | MSG_DONTWAIT));
#endif
    passed("getsockopt SO_ERROR", syscall(SYS_getsockopt, 3, SOL_SOCKET, SO_ERROR, &v, &l));
    passed("getsockopt SO_TYPE", syscall(SYS_getsockopt, 3, SOL_SOCKET, SO_TYPE, &v, &l));
    refused("getsockopt SO_PEERCRED", syscall(SYS_getsockopt, 3, SOL_SOCKET, SO_PEERCRED, &v, &l));
    passed("setsockopt SO_RCVBUF", syscall(SYS_setsockopt, 3, SOL_SOCKET, SO_RCVBUF, &v, sizeof v));
    passed("setsockopt TCP_NODELAY", syscall(SYS_setsockopt, 3, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v));
    passed("setsockopt IP_TOS", syscall(SYS_setsockopt, 3, IPPROTO_IP, IP_TOS, &v, sizeof v));
    refused("setsockopt SO_BINDTODEVICE", syscall(SYS_setsockopt, 3, SOL_SOCKET, SO_BINDTODEVICE, "lo", 3));
    refused("setsockopt SO_LINGER", syscall(SYS_setsockopt, 3, SOL_SOCKET, SO_LINGER, &v, sizeof v));
    refused("setsockopt SO_RCVTIMEO", syscall(SYS_setsockopt, 3, SOL_SOCKET, SO_RCVTIMEO, &ts, sizeof ts));
    refused("setsockopt IP_MULTICAST_IF", syscall(SYS_setsockopt, 3, IPPROTO_IP, IP_MULTICAST_IF, &v, sizeof v));

    /* ---- memory: anonymous and private, never executable, never a descriptor */
    p = (void *)syscall(MMAP, 0, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    passed("mmap anonymous, read-write", (long)p == -1 ? -1 : 0);
    if ((long)p != -1) {
        passed("mmap MAP_FIXED over its own page (Go's arena)", syscall(MMAP, p, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == -1 ? -1 : 0);
        refused("mprotect PROT_EXEC", syscall(SYS_mprotect, p, page, PROT_READ | PROT_EXEC));
        passed("mprotect read-only", syscall(SYS_mprotect, p, page, PROT_READ));
        passed("madvise MADV_DONTNEED", syscall(SYS_madvise, p, page, MADV_DONTNEED));
        refused("madvise MADV_HUGEPAGE", syscall(SYS_madvise, p, page, 14));
        passed("mremap MAYMOVE", syscall(SYS_mremap, p, page, 2 * page, MREMAP_MAYMOVE) == -1 ? -1 : 0);
        refused("mremap FIXED", syscall(SYS_mremap, p, page, page, MREMAP_MAYMOVE | MREMAP_FIXED, 0x10000000));
    }
    refused("mmap PROT_EXEC", syscall(MMAP, 0, page, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == -1 ? -1 : 0);
    refused("mmap MAP_SHARED", syscall(MMAP, 0, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0) == -1 ? -1 : 0);
    refused("mmap of a descriptor", syscall(MMAP, 0, page, PROT_READ, MAP_PRIVATE, 2, 0) == -1 ? -1 : 0);
    refused("mlock", syscall(SYS_mlock, &word, sizeof word));

    /* ---- threads */
    say("a thread (the runtime's own clone)", !pthread_create(&t, NULL, thread, &ran) && !pthread_join(t, NULL) && ran, ran ? "ran" : "did not start");
    passed("futex FUTEX_WAKE_PRIVATE", syscall(SYS_futex, &word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0));
    refused("futex FUTEX_LOCK_PI", syscall(SYS_futex, &word, FUTEX_LOCK_PI, 0, 0, 0, 0));
    refused("futex FUTEX_WAKE_OP", syscall(SYS_futex, &word, FUTEX_WAKE_OP, 1, 0, &word, 0));
    passed("prctl PR_SET_NAME", syscall(SYS_prctl, PR_SET_NAME, name, 0, 0, 0));
    refused("prctl PR_SET_DUMPABLE", syscall(SYS_prctl, PR_SET_DUMPABLE, 1, 0, 0, 0));
    refused("prctl PR_SET_PDEATHSIG", syscall(SYS_prctl, PR_SET_PDEATHSIG, 0, 0, 0, 0));
    refused("prctl PR_SET_SECCOMP", syscall(SYS_prctl, PR_SET_SECCOMP, 0, 0, 0, 0));
    passed("sched_getaffinity (Go's runtime)", syscall(SYS_sched_getaffinity, 0, sizeof name, name));
    refused("sched_setaffinity", syscall(SYS_sched_setaffinity, 0, sizeof name, name));

    /* ---- time, randomness, who it is */
    passed("clock_gettime", clock_gettime(CLOCK_MONOTONIC, &ts));
    passed("getrandom", syscall(SYS_getrandom, name, 8, 0));
    passed("nanosleep", syscall(SYS_nanosleep, &(struct timespec){ 0, 1000000 }, 0));
    say("no_new_privs", prctl(39 /* PR_GET_NO_NEW_PRIVS */, 0, 0, 0, 0) == 1 || errno == EPERM, "set (or not to be asked)");

    /* ---- what the launcher set, said for the table: not right or wrong by themselves */
    { char line[200]; int n; unsigned long as = 0, nf = 0, np = 0;
      if (!getrlimit(RLIMIT_AS, &rl)) as = (unsigned long)rl.rlim_cur;
      if (!getrlimit(RLIMIT_NOFILE, &rl)) nf = (unsigned long)rl.rlim_cur;
      if (!getrlimit(RLIMIT_NPROC, &rl)) np = (unsigned long)rl.rlim_cur;
      say("its own limits read (getrlimit, as the runtime asks)", as && nf && np, "32 MB, 64 descriptors, 8 threads below");
      refused("prlimit64 to set a limit", syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &(struct rlimit){ 4096, 4096 }, 0));
      n = snprintf(line, sizeof line, "     uid %d gid %d, memory limit %lu MB, %lu descriptors, %lu threads, argv0 %s, env %s\n",
                   (int)getuid(), (int)getgid(), as >> 20, nf, np, argv[0], envp[0] ? envp[0] : "-");
      if (write(2, line, (size_t)n) < 0) { } }
    { char line[80]; int n = snprintf(line, sizeof line, "probe: %d checks, wrong %d\n", checks, wrong); if (write(2, line, (size_t)n) < 0) { } }
    return wrong > 255 ? 255 : wrong;
}
