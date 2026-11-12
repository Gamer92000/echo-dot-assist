// runas [-r GROUP] [-Z CONTEXT] USER GROUP[,GROUP...] CMD [ARGS...]
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
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    while (argc > 2 && (!strcmp(argv[1], "-r") || !strcmp(argv[1], "-Z"))) {
        if (argv[1][1] == 'Z') ctx = argv[2];
        else if (gid_of(argv[2], &real)) { fprintf(stderr, "runas: unknown group %s\n", argv[2]); return 1; }
        argv += 2; argc -= 2;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: runas [-r GROUP] [-Z CONTEXT] USER GROUP[,GROUP...] CMD [ARGS...]\n");
        return 2;
    }
    pw = getpwnam(argv[1]);
    if (!pw) { fprintf(stderr, "runas: unknown user %s\n", argv[1]); return 1; }

    for (tok = strtok(argv[2], ","); tok && n < 32; tok = strtok(NULL, ",")) {
        if (gid_of(tok, &gids[n])) { fprintf(stderr, "runas: unknown group %s\n", tok); return 1; }
        n++;
    }
    if (setgroups(n, gids) || setresgid(real == (gid_t)-1 ? gids[0] : real, gids[0], gids[0]) || setuid(pw->pw_uid)) {
        perror("runas"); return 1;
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
