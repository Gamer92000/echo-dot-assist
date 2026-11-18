/* The country a place is in, from borders built into hassmic (geo/borders.bin, Natural Earth's).  See geo.c. */
#ifndef GEO_H
#define GEO_H

#define GEO_BORDER_KM 3.0               /* other countries this close are named too (tools/geo/geo.py BORDER_KM) */
#define GEO_NEAR_MAX_KM 50.0            /* in none: the nearest within this (a coast drawn coarser than the house) */
#define GEO_NEAR_MAX 8                  /* at most this many named near (the first by name) */
#define GEO_SEEN_MAX 32

/* 1 and cc ("DE") for the country at lat/lon (degrees), and in near[0..*nnear) the others within GEO_BORDER_KM;
 * 0 none (the sea, Antarctica, a place out of range).  ~10 ms on a Dot 2: it inflates the borders each time.  Any
 * thread. */
int geo_country(double lat, double lon, char cc[3], char near[][3], int *nnear);
#include <stdint.h>
int32_t geo_unit(double deg, uint32_t scale);   /* degrees to the file's units: geo.py's round(); for geo_ref.py */
#endif
