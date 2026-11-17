/* tz_lookup on one tzdata file: tz_test <tzdata> <posix> [<zone set> [<country>]] prints the zone found and how
 * (country, kept, guess), or "-" for none.  Driven by tz_ref.py, which builds the file from Python's tzdata as bionic's
 * ZoneCompactor does. */
#include "tz.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    static const char *const hows[] = { "country", "kept", "guess" };
    char z[64]; int how = -1;
    if (argc < 3) { fprintf(stderr, "usage: tz_test <tzdata> <posix> [<zone set> [<country>]]\n"); return 2; }
    if (tz_lookup(argv[1], argv[2], argc > 3 && argv[3][0] ? argv[3] : NULL, argc > 4 ? argv[4] : NULL, z, sizeof z, &how))
        printf("%s %s\n", z, how >= 0 && how < 3 ? hows[how] : "?");
    else puts("-");
    return 0;
}
