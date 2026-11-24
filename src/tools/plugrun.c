// plugrun -L -- CMD [ARGS...]
// plugrun -T [-D] PROGRAM     the launcher by itself, for trying the sandbox (scripts/plugin-probe.sh): PROGRAM
//                             started as a plugin is, its stderr and exit status passed on; -D with the development flag
//                             (a refused system call raises SIGSYS instead of answering EPERM)
// The plugins' launcher (docs/plugins.md 2.1 to 2.4; the protocol: src/include/plugrun.h).  Root's, and without any
// policy: asked to start a program, it starts it as a plugin user, under the system call filter and the limits,
// always.  What may run, and with which capabilities, is hassmic's business (signatures, what the owner accepted);
// a hassmic that was taken over gets, through this, a process that can do less than hassmic itself.
//
// Root is needed for the plugin's user, the SELinux domain, the CPU group and the OOM score: none of it can be done
// by hassmic, whose domain may enter no other.  And plugins cannot be hassmic's children: hassmic ignores SIGCHLD,
// and Amazon's attestation module waits for its idme tool with a plain wait(), which then blocks until every child
// has gone (Dot 2, 2026-10-06, clock.c).  So main.sh runs `plugrun -L -- runas ... hassmic ...`: this process makes
// a socketpair, forks twice (the middle one exits: the launcher is init's child) and becomes CMD, with the pair's
// other end as a descriptor named in HASSMIC_PLUGIN_FD.  hassmic's pid, exit code and main.sh's five-exits rule
// stay what they are.  When anything fails before that, CMD runs without it and hassmic has no plugins.
//
// Root opens no path of hassmic's here (issue #130 was about exactly those): programs and sockets arrive as
// descriptors.  The launcher ends, after killing its children, when the socket closes or the process it started as
// hassmic is gone (looked at every 2 s, so a forgotten copy of the descriptor cannot keep plugins alive): every
// hassmic start has a fresh launcher and no plugin outlives hassmic.
//
// The PC build (-DPLUGRUN_HOST, build/plugrun-host: hassmic's own tests) changes no user, domain or CPU group and
// uses the filter's x86_64 translation; HASSMIC_PLUGIN_SANDBOX=0 there leaves the filter out, =broken plays a kernel
// without one.  The device build has no such switch: never unsandboxed.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/seccomp.h>
#include "plugrun.h"
#include "plugfilter.h"

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif
#ifndef PR_CAPBSET_DROP
#define PR_CAPBSET_DROP 24
#endif

#define GROUP_ORDINARY "/dev/cpuctl/hassmic-plugins"   /* all plugins together: a quarter of one ordinary thread when busy */
#define GROUP_WAKE     "/dev/cpuctl/hassmic-wake"      /* a wake word engine that starves is an Echo that does not answer */
#define KILL_MS 2000                                    /* SIGTERM, then SIGKILL */

static struct { pid_t pid; long long kill_at; } slots[PLUGRUN_SLOTS];
static int sock = -1, chld[2] = { -1, -1 };
static int sandbox_off, sandbox_ok;
#ifndef PLUGRUN_HOST
static int domain = PLUGRUN_DOMAIN_NONE, groups_ok;
#endif
static pid_t self;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static int put(const char *path, const char *text)
{
    int f = open(path, O_WRONLY | O_CLOEXEC), ok;
    if (f < 0) return -1;
    ok = write(f, text, strlen(text)) == (ssize_t)strlen(text);
    close(f);
    return ok ? 0 : -1;
}

/* The filter of sdk/filter.def for this process, with what only it knows: its pid (tgkill to itself only) and the
 * three pointers of its one execve.  no_new_privs first: without it an unprivileged process may not install one, and
 * with it nothing the plugin runs can gain anything by a set-id bit. */
static int filter(const void *path, const void *argv, const void *envp, int develop)
{
#ifdef PLUGFILTER_NONE
    (void)path; (void)argv; (void)envp; (void)develop;
    errno = ENOSYS; return -1;
#else
    static struct sock_filter prog[sizeof plugfilter_prog / sizeof plugfilter_prog[0]];
    struct sock_fprog fp = { sizeof prog / sizeof prog[0], prog };
    unsigned long long v[4] = { (unsigned long long)getpid(), (unsigned long long)(uintptr_t)path, (unsigned long long)(uintptr_t)argv, (unsigned long long)(uintptr_t)envp };
    memcpy(prog, plugfilter_prog, sizeof prog);
    for (size_t i = 0; i < sizeof plugfilter_patches / sizeof plugfilter_patches[0]; i++)
        prog[plugfilter_patches[i].at].k = (uint32_t)(v[plugfilter_patches[i].what] >> (plugfilter_patches[i].half ? 32 : 0));
    /* the development flag: every refusal of the table's (EPERM) a trap instead; ENOSYS above 386 stays what it is */
    if (develop)
        for (size_t i = 0; i < sizeof prog / sizeof prog[0]; i++)
            if (prog[i].code == (BPF_RET | BPF_K) && prog[i].k == (SECCOMP_RET_ERRNO | EPERM)) prog[i].k = SECCOMP_RET_TRAP;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;
    if (sandbox_off) return 0;
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fp);
#endif
}

/* At the launcher's start: a child under the filter must be refused open and socket.  A kernel without
 * CONFIG_SECCOMP_FILTER, or one that lies: READY says so and hassmic starts no plugin. */
static int self_test(void)
{
    int st; pid_t p = fork();
    if (p < 0) return 0;
    if (!p) {
        if (filter(NULL, NULL, NULL, 0)) _exit(2);
        if (sandbox_off) _exit(0);
        long a = syscall(SYS_openat, AT_FDCWD, "/dev/null", O_RDONLY); int ea = errno;
        long b = syscall(SYS_socket, AF_UNIX, SOCK_STREAM, 0); int eb = errno;
        _exit(a == -1 && ea == EPERM && b == -1 && eb == EPERM ? 0 : 3);
    }
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) ;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

#ifndef PLUGRUN_HOST
/* The SELinux domain a plugin gets, by writing /proc/self/attr/current as runas -Z does (the exec's context is
 * refused from /data, which is nosuid): the plugin's own where the policy has it, else hassmic's, else the
 * launcher's su (docs/plugins.md 2.7: three kinds of Echo in the field).  Tried in a child, once. */
static const char *domain_name(int d) { return d == PLUGRUN_DOMAIN_PLUGIN ? "u:r:hassmic_plugin:s0" : "u:r:hassmic:s0"; }
static int domain_find(void)
{
    int st; pid_t p = fork();
    if (p < 0) return PLUGRUN_DOMAIN_NONE;
    if (!p) {
        if (!put("/proc/self/attr/current", domain_name(PLUGRUN_DOMAIN_PLUGIN))) _exit(PLUGRUN_DOMAIN_PLUGIN);
        if (!put("/proc/self/attr/current", domain_name(PLUGRUN_DOMAIN_HASSMIC))) _exit(PLUGRUN_DOMAIN_HASSMIC);
        _exit(PLUGRUN_DOMAIN_NONE);
    }
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) ;
    return WIFEXITED(st) ? WEXITSTATUS(st) : PLUGRUN_DOMAIN_NONE;
}

/* CONFIG_CGROUP_SCHED is set in every model's kernel; a hard cap (CFS bandwidth) and memory groups only on biscuit
 * and radar, so neither is relied on: shares only */
static int groups_make(void)
{
    mkdir(GROUP_ORDINARY, 0755); mkdir(GROUP_WAKE, 0755);
    put(GROUP_ORDINARY "/cpu.rt_runtime_us", "0"); put(GROUP_WAKE "/cpu.rt_runtime_us", "0");
    return !put(GROUP_ORDINARY "/cpu.shares", "256") && !put(GROUP_WAKE "/cpu.shares", "1024")
           && !access(GROUP_ORDINARY "/tasks", W_OK) && !access(GROUP_WAKE "/tasks", W_OK);
}
#endif

static void say(uint32_t type, uint32_t slot, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    struct plugrun_msg m = { type, slot, a, b, c, d };
    if (send(sock, &m, sizeof m, MSG_DONTWAIT | MSG_NOSIGNAL) != sizeof m && type != PLUGRUN_READY)
        fprintf(stderr, "plugrun: hassmic does not take a message (%u for slot %u)\n", type, slot);
}

/* The forked child that becomes a plugin: docs/plugins.md 2.3, step by step; a step that fails is told through rep
 * (close-on-exec: its end of file is the exec) and the child ends.  fd: the program, the control socket, stderr. */
static void fail(int rep, int step) { int e[2] = { step, errno }; if (write(rep, e, sizeof e) < 0) { } _exit(126); }

static void become_plugin(uint32_t slot, uint32_t mem_mb, uint32_t threads, uint32_t cls, int develop, const int *fd, int rep)
{
    static char *const argv[] = { "plugin", NULL }, *const envp[] = { "HASSMIC_PLUGIN=1", NULL };
    static const char path[] = "/proc/self/fd/4";
    char num[24];
    int keep[4], top = 3, null;
    struct rlimit nofile;

    /* 1: only the three descriptors (and rep).  Moved out of the way first: any of them may sit at 0..5 */
    for (int i = 0; i < 4; i++) { if ((keep[i] = fcntl(i < 3 ? fd[i] : rep, F_DUPFD_CLOEXEC, 10)) < 0) fail(rep, 1); if (keep[i] > top) top = keep[i]; }
    rep = keep[3];
    if (getrlimit(RLIMIT_NOFILE, &nofile) || nofile.rlim_cur > 65536) nofile.rlim_cur = 65536;
    for (int i = 0; i < (int)nofile.rlim_cur; i++)
        if (i != keep[0] && i != keep[1] && i != keep[2] && i != keep[3]) close(i);
    if ((null = open("/dev/null", O_RDWR)) != 0 || dup2(0, 1) != 1) fail(rep, 1);
    if (dup2(keep[2], 2) != 2 || dup2(keep[1], PLUGIN_FD_CONTROL) != PLUGIN_FD_CONTROL || dup3(keep[0], 4, O_CLOEXEC) != 4 || dup3(keep[3], 5, O_CLOEXEC) != 5) fail(rep, 1);
    close(keep[0]); close(keep[1]); close(keep[2]); close(keep[3]); rep = 5;
    setsid(); umask(077);
    if (chdir("/")) fail(rep, 1);

    /* 2: the kernel takes a plugin before the mixer or hassmic; the CPU group */
    put("/proc/self/oom_score_adj", "900");
    snprintf(num, sizeof num, "%d", (int)getpid());
#ifndef PLUGRUN_HOST
    if (groups_ok && put(cls == PLUGRUN_CPU_WAKE ? GROUP_WAKE "/tasks" : GROUP_ORDINARY "/tasks", num)) fail(rep, 2);
#endif
    if (cls != PLUGRUN_CPU_WAKE) setpriority(PRIO_PROCESS, 0, 5);

    /* 3: limits.  The address space is the only memory limit there is without memory groups; no RLIMIT_CPU, plugins
     * run for months (hassmic's watchdog looks at CPU time instead).  RLIMIT_NPROC counts per user, and each slot is
     * a user: on the PC, where it is the tester's own, it is left alone */
    { struct { int what; rlim_t v; } l[] = {
          { RLIMIT_AS, (rlim_t)mem_mb << 20 }, { RLIMIT_NOFILE, 64 }, { RLIMIT_STACK, 1 << 20 }, { RLIMIT_CORE, 0 },
          { RLIMIT_FSIZE, 0 }, { RLIMIT_MEMLOCK, 0 }, { RLIMIT_MSGQUEUE, 0 }, { RLIMIT_NICE, 0 }, { RLIMIT_RTPRIO, 0 },
#ifndef PLUGRUN_HOST
          { RLIMIT_NPROC, threads },
#endif
      };
      for (size_t i = 0; i < sizeof l / sizeof l[0]; i++) { struct rlimit r = { l[i].v, l[i].v }; if (setrlimit(l[i].what, &r)) fail(rep, 3); } }
    (void)threads;

#ifndef PLUGRUN_HOST
    /* 4: a user of its own per slot, no supplementary group (on every kernel but cannoli's, paranoid networking then
     * refuses an inet socket: a second wall behind the filter), no capability to come back to */
    for (int c = 0; c < 64; c++) if (prctl(PR_CAPBSET_DROP, c, 0, 0, 0) && errno != EINVAL) fail(rep, 4);
    { uid_t id = PLUGRUN_UID + slot;
      if (setgroups(0, NULL) || setresgid(id, id, id) || setresuid(id, id, id) || getuid() != id || geteuid() != id) fail(rep, 4); }
#else
    (void)slot;
#endif

    /* 5: gone with the launcher (after the user change, which clears it) */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) || getppid() != self) fail(rep, 5);

#ifndef PLUGRUN_HOST
    /* 6: the domain found at start */
    if (domain != PLUGRUN_DOMAIN_NONE && put("/proc/self/attr/current", domain_name(domain))) fail(rep, 6);
#endif

    /* 7 */
    { sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL); }
    /* SIGPIPE stays ignored across the exec; the launcher's other ignores must not: a plugin without a handler of
     * its own has to end on KILL's SIGTERM, not 2 s later on SIGKILL */
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);

    /* 8, 9: the filter lets this one execve through, by its three pointers.  By descriptor: 3.18 has no execveat,
     * and a process may always open its own /proc/self/fd */
    if (filter(path, argv, envp, develop)) fail(rep, 8);
    execve(path, argv, envp);
    fail(rep, 9);
}

static void spawn(const struct plugrun_msg *m, const int *fd)
{
    int rep[2], e[2] = { 0, 0 }; pid_t p; ssize_t n; struct pollfd pf;
    uint32_t mem = m->a < 8 ? 8 : m->a > 96 ? 96 : m->a, threads = m->b < 1 ? 1 : m->b > 16 ? 16 : m->b;
    /* never unsandboxed: on a kernel whose filter did not hold at the start nothing is started, whoever asks */
    if (!sandbox_ok) { say(PLUGRUN_FAILED, m->slot, ENOSYS, 0, 0, 0); return; }
    if (slots[m->slot].pid) { say(PLUGRUN_FAILED, m->slot, EBUSY, 0, 0, 0); return; }
    if (pipe2(rep, O_CLOEXEC)) { say(PLUGRUN_FAILED, m->slot, errno, 0, 0, 0); return; }
    if ((p = fork()) < 0) { say(PLUGRUN_FAILED, m->slot, errno, 0, 0, 0); close(rep[0]); close(rep[1]); return; }
    if (!p) { close(rep[0]); become_plugin(m->slot, mem, threads, m->c == PLUGRUN_CPU_WAKE ? PLUGRUN_CPU_WAKE : PLUGRUN_CPU_ORDINARY, !!(m->d & PLUGRUN_SPAWN_DEVELOP), fd, rep[1]); }
    close(rep[1]);
    /* the child's own steps take milliseconds; one that hangs before its exec is not waited for */
    pf.fd = rep[0]; pf.events = POLLIN;
    while ((n = poll(&pf, 1, 5000)) < 0 && errno == EINTR) ;
    if (n <= 0) { kill(p, SIGKILL); e[0] = 0; e[1] = ETIMEDOUT; n = sizeof e; }
    else while ((n = read(rep[0], e, sizeof e)) < 0 && errno == EINTR) ;
    close(rep[0]);
    slots[m->slot].pid = p; slots[m->slot].kill_at = 0;       /* reaped as any: a failed one's EXITED follows its FAILED */
    if (n == 0) say(PLUGRUN_SPAWNED, m->slot, (uint32_t)p, 0, 0, 0);
    else say(PLUGRUN_FAILED, m->slot, (uint32_t)e[1], (uint32_t)e[0], 0, 0);
}

static void reap(void)
{
    int st; struct rusage ru; pid_t p;
    while ((p = wait4(-1, &st, WNOHANG, &ru)) > 0)
        for (int i = 0; i < PLUGRUN_SLOTS; i++) if (slots[i].pid == p) {
            slots[i].pid = 0;
            say(PLUGRUN_EXITED, (uint32_t)i, (uint32_t)st, (uint32_t)(ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000),
                (uint32_t)(ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000), (uint32_t)ru.ru_maxrss);
        }
}

/* A child's CPU time so far in ms: utime and stime of /proc/<pid>/stat, which are the whole thread group's, threads
 * that ended included (a plugin that spins in threads it keeps replacing is counted).  -1: not readable.  Only ever
 * a slot's own pid, a child not reaped yet: the number cannot be another process's */
static long long cpu_ms(pid_t pid)
{
    char path[40], b[512], *c; int fd; ssize_t n; unsigned long long ut, st; static long tck;
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) return -1;
    n = read(fd, b, sizeof b - 1); close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    /* after the name, which may hold anything: the state and ten numbers, then the two */
    if (!(c = strrchr(b, ')')) || sscanf(c + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return -1;
    if (!tck && (tck = sysconf(_SC_CLK_TCK)) <= 0) tck = 100;
    return (long long)((ut + st) * 1000 / (unsigned long long)tck);
}

static void on_chld(int s) { (void)s; int e = errno; if (write(chld[1], "", 1) < 0) { } errno = e; }

/* One message of hassmic's.  0: go on; -1: end (the socket closed, or anything but the protocol: root takes nothing
 * else from hassmic) */
static int take(void)
{
    struct plugrun_msg m; int fd[8], nfd = 0, r = -1;
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof fd)]; } ctl;
    struct iovec iov = { &m, sizeof m };
    struct msghdr h = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = &ctl, .msg_controllen = sizeof ctl };
    ssize_t n = recvmsg(sock, &h, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
    if (n == 0) return -1;                      /* hassmic's end is closed: the ordinary end, nothing to say */
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < k && nfd < 8; i++) memcpy(&fd[nfd++], CMSG_DATA(c) + i * sizeof(int), sizeof(int));
        }
    if (n != sizeof m || (h.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || m.slot >= PLUGRUN_SLOTS) goto out;
    if (m.type == PLUGRUN_SPAWN && nfd == 3) { spawn(&m, fd); r = 0; }
    else if (m.type == PLUGRUN_KILL && !nfd) {
        if (slots[m.slot].pid && !slots[m.slot].kill_at) { kill(slots[m.slot].pid, SIGTERM); slots[m.slot].kill_at = now_ms() + KILL_MS; }
        r = 0;
    }
    else if (m.type == PLUGRUN_USAGE && !nfd) {
        /* reported, not judged (plugrun.h): the time and this clock in one go, so that the two belong together
         * however late hassmic reads them */
        pid_t p = slots[m.slot].pid; long long ms = p ? cpu_ms(p) : -1;
        say(PLUGRUN_USED, m.slot, (uint32_t)p, ms < 0 ? 0 : (uint32_t)ms, (uint32_t)now_ms(), ms >= 0);
        r = 0;
    }
out:
    for (int i = 0; i < nfd; i++) close(fd[i]);
    if (r) fprintf(stderr, "plugrun: not the protocol (%zd bytes, type %u, slot %u, %d descriptors): ending\n", n, n >= (ssize_t)sizeof m ? m.type : 0, n >= (ssize_t)sizeof m ? m.slot : 0, nfd);
    return r;
}

static void launcher(int s, pid_t hassmic)
{
    long long looked = now_ms();
    sock = s; self = getpid();
    setsid();                           /* init's stop kills hassmic's process group: this one ends by itself, below */
    signal(SIGPIPE, SIG_IGN); signal(SIGHUP, SIG_IGN); signal(SIGTERM, SIG_IGN);
    { int n = open("/dev/null", O_RDWR); if (n >= 0) { dup2(n, 0); dup2(n, 1); if (n > 2) close(n); } }
#ifdef PLUGRUN_HOST
    { const char *e = getenv("HASSMIC_PLUGIN_SANDBOX"); sandbox_off = e && !strcmp(e, "0");
      sandbox_ok = e && !strcmp(e, "broken") ? 0 : self_test();
      /* the PC has no SELinux: HASSMIC_PLUGIN_DOMAIN=plugin|hassmic says what an Echo's launcher would have found, for
       * the page's state line and the community switch's warning (tests; the plugin runs as it always does here) */
      const char *dm = getenv("HASSMIC_PLUGIN_DOMAIN");
      say(PLUGRUN_READY, 0, PLUGRUN_VERSION, (uint32_t)sandbox_ok, dm && !strcmp(dm, "plugin") ? PLUGRUN_DOMAIN_PLUGIN : dm && !strcmp(dm, "hassmic") ? PLUGRUN_DOMAIN_HASSMIC : PLUGRUN_DOMAIN_NONE, 0); }
#else
    domain = domain_find(); groups_ok = groups_make();
    sandbox_ok = self_test();
    if (!sandbox_ok) fprintf(stderr, "plugrun: the system call filter does not hold on this kernel: no plugins\n");
    say(PLUGRUN_READY, 0, PLUGRUN_VERSION, (uint32_t)sandbox_ok, (uint32_t)domain, (uint32_t)groups_ok);
#endif
    if (pipe2(chld, O_CLOEXEC | O_NONBLOCK)) _exit(1);
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_chld; sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; sigaction(SIGCHLD, &sa, NULL); }
    for (;;) {
        struct pollfd p[2] = { { sock, POLLIN, 0 }, { chld[0], POLLIN, 0 } };
        char drain[64]; long long now;
        if (poll(p, 2, 500) < 0 && errno != EINTR) break;
        while (read(chld[0], drain, sizeof drain) > 0) ;
        reap();
        now = now_ms();
        for (int i = 0; i < PLUGRUN_SLOTS; i++)
            if (slots[i].pid && slots[i].kill_at && now >= slots[i].kill_at) { kill(slots[i].pid, SIGKILL); slots[i].kill_at = now + KILL_MS; }
        if (now - looked >= 2000) { looked = now; if (kill(hassmic, 0) && errno == ESRCH) break; }
        if (p[0].revents & POLLIN) { if (take()) break; }
        else if (p[0].revents & (POLLHUP | POLLERR | POLLNVAL)) break;
    }
    for (int i = 0; i < PLUGRUN_SLOTS; i++) if (slots[i].pid) kill(slots[i].pid, SIGKILL);
    _exit(0);
}

/* -T: this process in hassmic's place towards a launcher of its own: one SPAWN, the program's stderr copied to ours,
 * its exit status ours (254: it did not start).  What the launcher found is said first */
static int trial(const char *program, int develop)
{
    static const char *const domains[] = { "none (the launcher's own)", "u:r:hassmic:s0", "u:r:hassmic_plugin:s0" };
    int sv[2], ctl[2], pp[2], prog, fds[3], status = 254, open_ = 1, failed = 0; pid_t l; struct plugrun_msg m; char buf[4096]; ssize_t n;
    union { struct cmsghdr h; char b[CMSG_SPACE(3 * sizeof(int))]; } c;
    struct iovec iov = { &m, sizeof m }; struct msghdr h = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = &c, .msg_controllen = sizeof c.b };
    if ((prog = open(program, O_RDONLY | O_CLOEXEC)) < 0) { perror(program); return 254; }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ctl) || pipe2(pp, O_CLOEXEC) || (l = fork()) < 0) { perror("plugrun"); return 254; }
    if (!l) { close(sv[1]); close(prog); close(ctl[0]); close(ctl[1]); close(pp[0]); close(pp[1]); launcher(sv[0], getppid()); }
    close(sv[0]);
    fcntl(pp[0], F_SETFL, O_NONBLOCK);              /* read to its end after EXITED, never waited on */
    if (recv(sv[1], &m, sizeof m, 0) != sizeof m || m.type != PLUGRUN_READY) { fprintf(stderr, "plugrun: the launcher did not start\n"); return 254; }
    fprintf(stderr, "launcher: the kernel's filter %s, SELinux domain %s, CPU groups %s\n", m.b ? "holds" : "DOES NOT HOLD", domains[m.c < 3 ? m.c : 0], m.d ? "made" : "not there");
    m = (struct plugrun_msg){ PLUGRUN_SPAWN, 0, 32, 8, PLUGRUN_CPU_ORDINARY, develop ? PLUGRUN_SPAWN_DEVELOP : 0 };
    fds[0] = prog; fds[1] = ctl[1]; fds[2] = pp[1];
    memset(&c, 0, sizeof c); c.h.cmsg_level = SOL_SOCKET; c.h.cmsg_type = SCM_RIGHTS; c.h.cmsg_len = CMSG_LEN(sizeof fds); memcpy(CMSG_DATA(&c.h), fds, sizeof fds);
    if (sendmsg(sv[1], &h, 0) != sizeof m) { perror("plugrun"); return 254; }
    close(prog); close(ctl[1]); close(pp[1]);
    long long until = now_ms() + 30000;
    for (;;) {
        struct pollfd p[2] = { { sv[1], POLLIN, 0 }, { open_ ? pp[0] : -1, POLLIN, 0 } };
        if (poll(p, 2, 1000) < 0 && errno != EINTR) break;
        /* a program that waits for hassmic (any real plugin does, on descriptor 3) would wait for ever: 30 s */
        if (until && now_ms() > until) { fprintf(stderr, "plugrun: still running after 30 s: ended\n"); m = (struct plugrun_msg){ PLUGRUN_KILL, 0, 0, 0, 0, 0 }; if (send(sv[1], &m, sizeof m, 0) < 0) break; until = 0; }
        if (p[1].revents & (POLLIN | POLLHUP)) { if ((n = read(pp[0], buf, sizeof buf)) > 0) { if (write(2, buf, (size_t)n) < 0) { } } else if (n == 0) open_ = 0; }
        if (!(p[0].revents & (POLLIN | POLLHUP))) continue;
        if (recv(sv[1], &m, sizeof m, MSG_DONTWAIT) != sizeof m) break;
        if (m.type == PLUGRUN_FAILED) { failed = 1; fprintf(stderr, "plugrun: not started: step %u of the launch (docs/plugins.md 2.3): %s\n", m.b, strerror((int)m.a)); if (!m.b) break; }
        if (m.type == PLUGRUN_EXITED) {
            while (open_ && (n = read(pp[0], buf, sizeof buf)) > 0) if (write(2, buf, (size_t)n) < 0) { }
            if (failed) break;                              /* the launcher's own child, which never became the program */
            if (WIFEXITED((int)m.a)) status = WEXITSTATUS((int)m.a);
            else if (WTERMSIG((int)m.a) == SIGSYS && develop) { status = 128 + SIGSYS; fprintf(stderr, "plugrun: the program made a system call the sandbox refuses (SIGSYS: the development flag)\n"); }
            else { status = 128 + WTERMSIG((int)m.a); fprintf(stderr, "plugrun: the program was killed by signal %d\n", WTERMSIG((int)m.a)); }
            fprintf(stderr, "launcher: %u ms of CPU, %u kB at most\n", m.b + m.c, m.d);
            break;
        }
    }
    close(sv[1]);
    while (waitpid(l, NULL, 0) < 0 && errno == EINTR) ;
    return status;
}

int main(int argc, char **argv)
{
    int sv[2], fd; pid_t me = getpid(), mid; char num[16];
    if (argc == 3 && !strcmp(argv[1], "-T")) return trial(argv[2], 0);
    if (argc == 4 && !strcmp(argv[1], "-T") && !strcmp(argv[2], "-D")) return trial(argv[3], 1);
    if (argc < 4 || strcmp(argv[1], "-L") || strcmp(argv[2], "--")) { fprintf(stderr, "usage: plugrun -L -- CMD [ARGS...]\n       plugrun -T [-D] PROGRAM\n"); return 2; }
#ifndef PLUGRUN_HOST
    if (geteuid()) { fprintf(stderr, "plugrun: not root: no plugins\n"); goto run; }
#endif
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { fprintf(stderr, "plugrun: socketpair: %s: no plugins\n", strerror(errno)); goto run; }
    if ((mid = fork()) < 0) { fprintf(stderr, "plugrun: fork: %s: no plugins\n", strerror(errno)); close(sv[0]); close(sv[1]); goto run; }
    if (!mid) {
        pid_t l = fork();
        if (!l) { close(sv[1]); launcher(sv[0], me); }
        _exit(l < 0);
    }
    close(sv[0]);
    { int st;
      while (waitpid(mid, &st, 0) < 0 && errno == EINTR) ;
      if (!WIFEXITED(st) || WEXITSTATUS(st)) { fprintf(stderr, "plugrun: no launcher: no plugins\n"); close(sv[1]); goto run; } }
    /* descriptor 3 where it is free (what the caller left open there is the caller's), else whichever it got */
    fd = sv[1];
    if (fd != 3 && fcntl(3, F_GETFD) < 0 && errno == EBADF && dup2(fd, 3) == 3) { close(fd); fd = 3; }
    else if (fcntl(fd, F_SETFD, 0)) { close(fd); goto run; }
    snprintf(num, sizeof num, "%d", fd);
    setenv(PLUGRUN_FD_ENV, num, 1);
run:
    execvp(argv[3], argv + 3);
    perror(argv[3]);
    return 127;
}
