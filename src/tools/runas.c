// runas USER GROUP[,GROUP...] CMD [ARGS...]
// Drop from the root adb shell to a service user.  The stock image has no su, and AIPC (used by
// libmixerAPI) refuses uid 0: "Root user is not allowed to use AIPC".
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
    gid_t gids[32];
    int n = 0;
    struct passwd *pw;
    char *tok;

    if (argc < 4) {
        fprintf(stderr, "usage: %s USER GROUP[,GROUP...] CMD [ARGS...]\n", argv[0]);
        return 2;
    }
    pw = getpwnam(argv[1]);
    if (!pw) { fprintf(stderr, "runas: unknown user %s\n", argv[1]); return 1; }

    for (tok = strtok(argv[2], ","); tok && n < 32; tok = strtok(NULL, ",")) {
        if (gid_of(tok, &gids[n])) { fprintf(stderr, "runas: unknown group %s\n", tok); return 1; }
        n++;
    }
    if (setgroups(n, gids) || setgid(gids[0]) || setuid(pw->pw_uid)) { perror("runas"); return 1; }

    execvp(argv[3], argv + 3);
    perror(argv[3]);
    return 127;
}
