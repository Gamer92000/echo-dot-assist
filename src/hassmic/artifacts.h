/* Amazon's artifacts this Echo has (wake word model sets, the newer sound detection model, the whisper model), for the
 * settings page to copy them from one Echo to another: listed with a digest, read in pieces, written in pieces into
 * state/artifacts/, checked, then installed by root (scripts/device/artifact-install.sh, run by main.sh's watcher),
 * which restarts hassmic.  See artifacts.c.  Any thread; no lock needed from callers. */
#ifndef ARTIFACTS_H
#define ARTIFACTS_H
#include <stddef.h>

#define ART_CHUNK_MAX 262144            /* the most one piece may carry, read or written */

/* {"artifacts":[{"id","kind","name","size","digest","files":[{"name","size"}]}],"free":bytes,"staged":[ids],"result":"..."} */
size_t art_list_json(char *out, size_t cap);
/* len bytes of one file from off: malloc'd into *data (the caller frees), *n read.  0 ok, else err says why */
int art_read(const char *id, const char *file, long off, long len, void **data, size_t *n, char *err, size_t errsz);
/* "<id> <digest hex>\n<file> <size>\n...": a new copy of that artifact starts in state/artifacts (an older one there goes) */
int art_begin(const char *spec, char *err, size_t errsz);
int art_chunk(const char *id, const char *file, long off, const void *data, size_t n, char *err, size_t errsz);
/* every file whole and the digest right; wake word sets also load in pryon_test: then it waits for art_install */
int art_commit(const char *id, char *err, size_t errsz);
/* hand what is staged to root: it installs, then restarts hassmic */
int art_install(char *err, size_t errsz);
#endif
