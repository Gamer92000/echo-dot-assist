/* statefile.c (issue #46): the data is fsync'd before the rename and the folder after it (fsync and rename wrapped
 * with -Wl,--wrap to log their order), the mode is the one asked for, a failed or abandoned write leaves the old file
 * and no temp file, a temp file left by a reset is replaced, never followed when it is a link, and threads writing one
 * file at once each put a whole one in place (the last rename wins). */
#include "statefile.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[64] = "/tmp/statefile_test.XXXXXX", path[96], other[96], order[64];
static int fails, fail_rename, wrapped = 1;

int __real_fsync(int fd);
int __real_rename(const char *a, const char *b);
int __wrap_fsync(int fd)
{
    struct stat st; fstat(fd, &st);
    if (wrapped) strcat(order, S_ISDIR(st.st_mode) ? "D" : "F");
    return __real_fsync(fd);
}
int __wrap_rename(const char *a, const char *b)
{
    if (wrapped) strcat(order, "R");
    if (fail_rename) { errno = EIO; return -1; }
    return __real_rename(a, b);
}

static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static char *slurp(const char *f, char *buf, size_t n)
{
    FILE *fp = fopen(f, "r"); size_t l = 0;
    if (fp) { l = fread(buf, 1, n - 1, fp); fclose(fp); }
    buf[l] = 0; return buf;
}

static int temps(void)                              /* temp files in the folder */
{
    DIR *d = opendir(dir); struct dirent *e; int n = 0;
    while ((e = readdir(d))) n += strstr(e->d_name, ".tmp") != NULL;
    closedir(d);
    return n;
}

static void *writer(void *arg)                      /* 200 writes of one letter's line, 3000 bytes */
{
    static char line[2][3001]; int k = (int)(long)arg, bad = 0;
    memset(line[k], 'a' + k, 3000);
    for (int i = 0; i < 200; i++) bad |= statefile_write(path, 0600, line[k], 3000);
    return (void *)(long)bad;
}

int main(void)
{
    char buf[256]; struct statefile s; struct stat st;
    umask(022);
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(path, sizeof path, "%s/config", dir);
    snprintf(other, sizeof other, "%s/other", dir);

    order[0] = 0;
    check(!statefile_write(path, 0600, "a=1\n", 4), "write");
    check(!strcmp(slurp(path, buf, sizeof buf), "a=1\n"), "content");
    check(!strcmp(order, "FRD"), "file synced, renamed, folder synced");
    check(!stat(path, &st) && (st.st_mode & 0777) == 0600, "mode 0600");
    check(!temps(), "no temp file left");

    check(!statefile_open(&s, path, 0666), "open");
    fputs("a=2\n", s.f);
    check(!strcmp(slurp(path, buf, sizeof buf), "a=1\n"), "old content until the commit");
    check(!statefile_commit(&s), "commit");
    check(!strcmp(slurp(path, buf, sizeof buf), "a=2\n"), "new content");
    check(!stat(path, &st) && (st.st_mode & 0777) == 0644, "mode 0666 under the umask, as fopen");

    check(!statefile_open(&s, path, 0600), "open again");
    fputs("a=3\n", s.f);
    statefile_abort(&s);
    check(!strcmp(slurp(path, buf, sizeof buf), "a=2\n"), "abort keeps the old file");
    check(!temps(), "abort removes the temp file");

    char t[120]; FILE *f;                           /* every temp name taken: by a reset's leftovers, then by links */
    for (int i = 0; i < 100; i++) { snprintf(t, sizeof t, "%s.tmp%d", path, i); f = fopen(t, "w"); fputs("stale", f); fclose(f); }
    check(!statefile_write(path, 0600, "a=4\n", 4) && !strcmp(slurp(path, buf, sizeof buf), "a=4\n"), "a stale temp file is replaced");
    for (int i = 0; i < 100; i++) { snprintf(t, sizeof t, "%s.tmp%d", path, i); unlink(t); }

    f = fopen(other, "w"); fputs("victim\n", f); fclose(f);
    int linked = 1;
    for (int i = 0; i < 100; i++) { snprintf(t, sizeof t, "%s.tmp%d", path, i); linked &= !symlink(other, t); }
    check(linked, "temp files as links");
    check(!statefile_write(path, 0600, "a=5\n", 4), "write past the link");
    check(!strcmp(slurp(other, buf, sizeof buf), "victim\n"), "the link's target untouched");
    check(!lstat(path, &st) && S_ISREG(st.st_mode) && !strcmp(slurp(path, buf, sizeof buf), "a=5\n"), "a plain file under the name");
    for (int i = 0; i < 100; i++) { snprintf(t, sizeof t, "%s.tmp%d", path, i); unlink(t); }

    fail_rename = 1;
    check(statefile_write(path, 0600, "a=6\n", 4) == -1, "a failed rename: an error");
    fail_rename = 0;
    check(!strcmp(slurp(path, buf, sizeof buf), "a=5\n") && !temps(), "a failed rename: the old file, no temp file");

    char missing[120]; snprintf(missing, sizeof missing, "%s/nofolder/x", dir);
    check(statefile_write(missing, 0600, "x", 1) == -1, "no folder: an error");

    order[0] = 0;
    check(!statefile_write(path, 0600, "", 0) && !strcmp(slurp(path, buf, sizeof buf), "") && !strcmp(order, "FRD"), "empty content");

    order[0] = 0;
    check(!statefile_sync(dir) && !strcmp(order, "D"), "sync of a folder");

    wrapped = 0;                                    /* order[] is not for threads */
    pthread_t th[2]; void *r[2]; static char big[4000];
    for (long k = 0; k < 2; k++) pthread_create(&th[k], NULL, writer, (void *)k);
    for (int k = 0; k < 2; k++) pthread_join(th[k], &r[k]);
    size_t n = strlen(slurp(path, big, sizeof big)); int same = n == 3000;
    for (size_t i = 1; i < n; i++) same &= big[i] == big[0];
    check(!r[0] && !r[1], "two threads at once: every write succeeded");
    check(same && (big[0] == 'a' || big[0] == 'b'), "two threads at once: one whole file in place");
    check(!temps(), "two threads at once: no temp file left");

    unlink(path); unlink(other); rmdir(dir);
    printf("%s\n", fails ? "statefile: FAILED" : "statefile: all ok");
    return fails != 0;
}
