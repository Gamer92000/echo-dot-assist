/* State files that survive a hard reset: the old content or the new one, never an empty file.  See statefile.c. */
#ifndef STATEFILE_H
#define STATEFILE_H
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

struct statefile { FILE *f; char path[320], tmp[330]; int slot; };

/* Opens PATH.tmp<n> (n 0..99, none in use by another write in flight) for writing with MODE (before the umask; 0666 =
 * as fopen), into s->f.  0, or -1 (errno set; EAGAIN: 100 writes in flight). */
int statefile_open(struct statefile *s, const char *path, mode_t mode);
/* Puts what was written to s->f on the flash, then under the real name, then the name on the flash.  Always closes
 * s->f; on failure the temp file is gone and the old file stays.  0, or -1. */
int statefile_commit(struct statefile *s);
/* Gives up: closes s->f and removes the temp file. */
void statefile_abort(struct statefile *s);
/* All of it at once: LEN bytes of DATA as PATH.  0, or -1. */
int statefile_write(const char *path, mode_t mode, const void *data, size_t len);
/* fsync of a file or a folder by name (a folder: the names in it).  0, or -1. */
int statefile_sync(const char *path);
#endif
