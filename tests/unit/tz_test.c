/* tz_lookup on one tzdata file: tz_test <tzdata> <posix> [<zone set>] prints the zone found, or "-" for none.
 * Driven by tz_ref.py, which builds the file from Python's tzdata as bionic's ZoneCompactor does. */
#include "tz.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    char z[64];
    if (argc < 3) { fprintf(stderr, "usage: tz_test <tzdata> <posix> [<zone set>]\n"); return 2; }
    puts(tz_lookup(argv[1], argv[2], argc > 3 ? argv[3] : NULL, z, sizeof z) ? z : "-");
    return 0;
}
