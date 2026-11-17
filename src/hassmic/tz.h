/* Home Assistant's time zone as a zone name of the Echo's own time zone data.  See tz.c. */
#ifndef TZ_H
#define TZ_H
#include <stddef.h>

enum { TZ_COUNTRY, TZ_KEPT, TZ_GUESS };     /* how tz_lookup found it */

/* The zone of the bionic tzdata file `path` whose rule (the POSIX TZ string at the end of its TZif data) is `posix`,
 * into out, *how saying how it was picked: one zone.tab lists for `country` (two letters, "DE"; NULL or "" none),
 * `cur` when it is in that country (or no zone of that country has the rule), else the country's first by area, then
 * zone.tab's order (TZ_COUNTRY); else `cur` when it has the rule (TZ_KEPT, TZ_GUESS when it is the one a guess gives); else
 * the most usual name of those (Europe/..., America/... before the rest: TZ_GUESS).  1 if one was found; 0 if none was (or the file is not bionic's), out
 * untouched. */
int tz_lookup(const char *path, const char *posix, const char *cur, const char *country, char *out, size_t cap, int *how);
#endif
