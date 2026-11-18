/* geo_country for geo_ref.py: "lat lon" per line on stdin, "<cc> <near,...|->" or "-" per line on stdout; with -u
 * <scale>: "deg" per line in, geo_unit's units out */
#include "geo.h"
#include <stdio.h>

#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    double lat, lon; char cc[3], near[GEO_NEAR_MAX][3]; int n;
    if (argc == 3 && !strcmp(argv[1], "-u")) {
        uint32_t scale = (uint32_t)strtoul(argv[2], NULL, 10);
        while (scanf("%lf", &lat) == 1) printf("%d\n", (int)geo_unit(lat, scale));
        return 0;
    }
    while (scanf("%lf %lf", &lat, &lon) == 2) {
        if (!geo_country(lat, lon, cc, near, &n)) { puts("-"); continue; }
        printf("%s ", cc);
        if (!n) printf("-");
        for (int i = 0; i < n; i++) printf("%s%s", i ? "," : "", near[i]);
        putchar('\n');
    }
    return 0;
}
