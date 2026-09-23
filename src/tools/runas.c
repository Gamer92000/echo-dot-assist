// runas [-r GROUP] USER GROUP[,GROUP...] CMD [ARGS...]
// Drop from the root adb shell to a service user.  The stock image has no su, and AIPC (used by
// libmixerAPI) refuses uid 0: "Root user is not allowed to use AIPC".
// -r: real group id.  The first GROUP stays the effective group (the mixer only records for a client
// whose effective group is aipc); the process may switch its filesystem group to the real one with
// setfsgid(), which is what the firewall's owner match sees on sockets it creates then.  The real
// group because exec resets the saved one to the effective one.
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

    if (argc > 2 && !strcmp(argv[1], "-r")) {
        if (gid_of(argv[2], &real)) { fprintf(stderr, "runas: unknown group %s\n", argv[2]); return 1; }
        argv += 2; argc -= 2;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: runas [-r GROUP] USER GROUP[,GROUP...] CMD [ARGS...]\n");
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

    execvp(argv[3], argv + 3);
    perror(argv[3]);
    return 127;
}
