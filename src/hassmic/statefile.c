/*
 * Writing a state file so that a power cut or hard reset leaves the old content or the new one (issue #46).
 *
 * A temp file plus rename() alone does not: /data is ext4 (mmcblk0p16 on the Dot 3), which keeps file data in memory
 * for up to about 30 s before it writes it to the flash, while the rename can reach the flash sooner.  ext4's
 * safeguard for exactly this pattern (auto_da_alloc: flush on a rename over a file) is off where Android mounts /data
 * with noauto_da_alloc.  Then a reset in that window leaves an empty file under the real name: state/arbitration came
 * back empty on 2 of 8 cold boots of one Dot 2, 2 s after a wake word round had written it (#36).  So the data is
 * fsync'd before the rename, and the folder after it (the rename itself is an entry of the folder).  These files
 * change rarely (a setting, a pairing, a wake word round), so the fsyncs' cost is paid rarely; it is not measured on
 * the Echos yet, and settings_save pays it under core_lock (the capture thread waits that long once).
 */
#include "statefile.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static atomic_uint seq;

int statefile_sync(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC), r;
    if (fd < 0) return -1;
    r = fsync(fd);
    close(fd);
    return r;
}

static int sync_parent(const char *path)
{
    char d[320]; const char *s = strrchr(path, '/');
    if (!s) return statefile_sync(".");
    snprintf(d, sizeof d, "%.*s", (int)(s - path) ? (int)(s - path) : 1, path);
    return statefile_sync(d);
}

int statefile_open(struct statefile *s, const char *path, mode_t mode)
{
    int fd;
    s->f = NULL;
    if (snprintf(s->path, sizeof s->path, "%s", path) >= (int)sizeof s->path) { errno = ENAMETOOLONG; return -1; }
    /* a temp name per write: two threads writing one file (adbwifi_ask from the page, the push port and Home Assistant)
     * each rename their own complete file; with one name, one would unlink the other's and rename it before it holds
     * anything.  One of a reset stays behind and is reused when the number comes round again. */
    snprintf(s->tmp, sizeof s->tmp, "%s.tmp%u", path, atomic_fetch_add(&seq, 1) % 100);
    unlink(s->tmp);                                 /* a fresh file, never one a link points elsewhere */
    if ((fd = open(s->tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode)) < 0) return -1;
    if (!(s->f = fdopen(fd, "w"))) { close(fd); unlink(s->tmp); return -1; }
    return 0;
}

int statefile_commit(struct statefile *s)
{
    int bad = ferror(s->f) || fflush(s->f) || fsync(fileno(s->f));
    if (fclose(s->f)) bad = 1;
    s->f = NULL;
    if (bad || rename(s->tmp, s->path)) { unlink(s->tmp); return -1; }
    sync_parent(s->path);                           /* the new name; a failure here leaves the old file or the new one */
    return 0;
}

void statefile_abort(struct statefile *s)
{
    if (s->f) fclose(s->f);
    s->f = NULL;
    unlink(s->tmp);
}

int statefile_write(const char *path, mode_t mode, const void *data, size_t len)
{
    struct statefile s;
    if (statefile_open(&s, path, mode)) return -1;
    if (len && fwrite(data, 1, len, s.f) != len) { statefile_abort(&s); return -1; }
    return statefile_commit(&s);
}
