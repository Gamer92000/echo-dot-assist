// runas [-r GROUP] [-Z CONTEXT] [-c CAP[,CAP...]] USER GROUP[,GROUP...] CMD [ARGS...]
// Drop from the root adb shell to a service user.  The stock image has no su, and AIPC (used by
// libmixerAPI) refuses uid 0: "Root user is not allowed to use AIPC".
// -r: real group id.  The first GROUP stays the effective group (the mixer only records for a client
// whose effective group is aipc); the process may switch its filesystem group to the real one with
// setfsgid(), which is what the firewall's owner match sees on sockets it creates then.  The real
// group because exec resets the saved one to the effective one.
// -Z: the SELinux domain CMD runs in (hassmic's own, u:r:hassmic:s0: scripts/system/sepolicy-hassmic.rules), this
// process's own once the user is dropped, just before the exec: not the exec's (attr/exec), since updates run from
// /data, which is nosuid, and there the kernel refuses an exec into a domain that su does not bound (Dot 2: "op=
// security_bounded_transition seresult=denied"); a single-threaded process may change its own.  Everything here runs in su, which the unlock makes permissive: there any
// process may set any property (service.adb.tcp.port, ctl.start ...: init asks SELinux only, Android 7 and later)
// and switch to any domain.  A policy without that domain (installed before it, the system partition is written
// only by install-system.sh) leaves CMD in this one, said on stderr.
// -c: capabilities CMD keeps as the user (NET_ADMIN, NET_RAW, NET_BIND_SERVICE), raised as ambient ones so they
// survive the exec of a program without file capabilities (Linux 4.3 and later; every Echo with a radio runs 4.9):
// our Thread border router (main.sh thread_run) needs its tun, routes and ICMPv6 sockets, as stock's otservice had
// them from init (init.openthread.rc: user openthread, capabilities NET_RAW NET_ADMIN), and nothing else of root's.
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/capability.h>

#ifndef PR_CAP_AMBIENT                          /* the NDK's API 24 headers predate them */
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_RAISE 2
#endif

static int cap_of(const char *s)
{
    static const struct { const char *name; int cap; } caps[] = {
        { "NET_BIND_SERVICE", CAP_NET_BIND_SERVICE }, { "NET_ADMIN", CAP_NET_ADMIN }, { "NET_RAW", CAP_NET_RAW },
    };
    for (size_t i = 0; i < sizeof caps / sizeof *caps; i++) if (!strcmp(s, caps[i].name)) return caps[i].cap;
    return -1;
}

static int gid_of(const char *s, gid_t *out)
{
    struct group *g = getgrnam(s);
    char *end;
    if (g) { *out = g->gr_gid; return 0; }
    *out = (gid_t)strtoul(s, &end, 10);
    return *end ? -1 : 0;
}

int main(int argc, char **argv)
{
    gid_t gids[32], real = (gid_t)-1;
    int n = 0;
    struct passwd *pw;
    char *tok;
    const char *ctx = NULL;
    uint32_t keep = 0;                          /* -c: bits of capabilities below 32 */

    while (argc > 2 && (!strcmp(argv[1], "-r") || !strcmp(argv[1], "-Z") || !strcmp(argv[1], "-c"))) {
        if (argv[1][1] == 'Z') ctx = argv[2];
        else if (argv[1][1] == 'c') {
            for (tok = strtok(argv[2], ","); tok; tok = strtok(NULL, ",")) {
                int c = cap_of(tok);
                if (c < 0) { fprintf(stderr, "runas: unknown capability %s\n", tok); return 1; }
                keep |= 1u << c;
            }
        }
        else if (gid_of(argv[2], &real)) { fprintf(stderr, "runas: unknown group %s\n", argv[2]); return 1; }
        argv += 2; argc -= 2;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: runas [-r GROUP] [-Z CONTEXT] [-c CAP[,CAP...]] USER GROUP[,GROUP...] CMD [ARGS...]\n");
        return 2;
    }
    pw = getpwnam(argv[1]);
    if (!pw) { fprintf(stderr, "runas: unknown user %s\n", argv[1]); return 1; }

    for (tok = strtok(argv[2], ","); tok && n < 32; tok = strtok(NULL, ",")) {
        if (gid_of(tok, &gids[n])) { fprintf(stderr, "runas: unknown group %s\n", tok); return 1; }
        n++;
    }
    if (keep && prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0)) { perror("runas: keepcaps"); return 1; }
    if (setgroups(n, gids) || setresgid(real == (gid_t)-1 ? gids[0] : real, gids[0], gids[0]) || setuid(pw->pw_uid)) {
        perror("runas"); return 1;
    }
    if (keep) {
        /* setuid kept the permitted set (KEEPCAPS) and emptied the effective one: these alone, then each raised as
         * ambient (it needs the bit permitted and inheritable).  Any failure ends here: a program that was meant to
         * have them must not start without them and fail in odd ways later */
        struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
        struct __user_cap_data_struct d[2] = { { keep, keep, keep }, { 0, 0, 0 } };
        if (syscall(SYS_capset, &h, d)) { perror("runas: capset"); return 1; }
        for (int c = 0; c < 32; c++)
            if (keep & (1u << c) && prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, c, 0, 0)) { perror("runas: ambient"); return 1; }
    }
    if (ctx) {                                  /* the process's own file: any user may write its own */
        int f = open("/proc/self/attr/current", O_WRONLY | O_CLOEXEC);
        size_t l = strlen(ctx);
        if (f < 0 || write(f, ctx, l) != (ssize_t)l)
            fprintf(stderr, "runas: no SELinux domain %s (%s): runs in the caller's\n", ctx, strerror(errno));
        if (f >= 0) close(f);
    }

    execvp(argv[3], argv + 3);
    perror(argv[3]);
    return 127;
}
