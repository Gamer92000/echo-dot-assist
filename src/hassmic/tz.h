/* Home Assistant's time zone as a zone name of the Echo's own time zone data.  See tz.c. */
#ifndef TZ_H
#define TZ_H
#include <stddef.h>

/* The zone of the bionic tzdata file `path` whose rule (the POSIX TZ string at the end of its TZif data) is `posix`,
 * into out: `cur` when it is one of them, else the most usual name of those (Europe/..., America/... before the
 * rest).  1 if one was found; 0 if none was (or the file is not bionic's), out untouched. */
int tz_lookup(const char *path, const char *posix, const char *cur, char *out, size_t cap);
#endif
