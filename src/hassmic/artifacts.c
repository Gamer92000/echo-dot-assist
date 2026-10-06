/*
 * Artifacts from Amazon's DAVS (scripts/artifacts.sh puts them on one Echo from the PC) copied between Echos by the settings
 * page, through the browser: it reads them in pieces from one Echo and writes them to another, both over signed requests
 * whose answers are signed as well (web.c), so nobody on the network can change them on the way.
 *
 * What there is, by id:
 *   wake:<name>  a wake word model set, models/<name>/ (Home Assistant offers each in the wake word select)
 *   sound        Amazon's newer sound detection model, aed/ (sound_pryon.c takes it over the firmware's)
 *   whisper      the whisper detection model, whisper/ (whisper_pryon.c)
 * Each is a flat folder of files.  Its digest: BLAKE2b-256 over, file by file in name order, "name\0size\0" and the
 * content; the same folder on two Echos has the same digest.  Kept per folder until a file's size or time changes:
 * hashing a 24 MB set takes seconds on these CPUs.
 *
 * Writing: begin (the files, their sizes, the digest; enough free space for the copy and root's copy of it), pieces into
 * state/artifacts/<dir>/ (dir: wake.<name>, sound, whisper), commit (every file whole, no other file, the digest right;
 * a wake word set must also load in this Echo's own engine, pryon_test, as scripts/artifacts.sh checks: the Echo 2's
 * older engine cannot load every set), then install: state/artifacts/request names what is ready, and root's watcher
 * (main.sh) runs artifact-install.sh, which reads the files as the daemon's user (so nothing staged here can point root
 * at a file of its own), copies them into a fresh root-owned folder, swaps it in and restarts hassmic: the wake word list
 * and the whisper model are read at start.  The models folders are root's (adb wrote them), hence the detour.
 */
#include "artifacts.h"
#include "../third_party/monocypher.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_FILES 64
#define MAX_TOTAL (96L << 20)           /* the biggest set so far is 24 MB (ziggy-de-DE) */
#define MARGIN    (8L << 20)            /* left free on /data after root's copy */
#define NCACHE    32

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;  /* begin/commit/install, the digest cache */
#define J(...) do { if (n < cap) n += (size_t)snprintf(o + n, cap - n, __VA_ARGS__); } while (0)     /* JSON into o[cap] at n */

static const char *env_or(const char *e, const char *d) { const char *v = getenv(e); return v ? v : d; }
static const char *state_dir(void) { return env_or("HASSMIC_STATE", "/data/local/hassmic/state"); }
static const char *models_dir(void) { return env_or("HASSMIC_MODELS", "/data/local/hassmic/models"); }

/* names as Amazon's sets have them ("int16_streaming.onnx", "echo-de-DE"): no path, nothing hidden */
static int good_name(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 63 || !isalnum((unsigned char)s[0])) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '.' && *s != '_' && *s != '-') return 0;
    return 1;
}

/* id -> the folder it lives in, the folder it is staged in (state/artifacts/<stage>), and its kind; -1 if not an id */
static int resolve(const char *id, char *dir, size_t dcap, char *stage, size_t scap, const char **kind)
{
    if (!strncmp(id, "wake:", 5) && good_name(id + 5)) {
        snprintf(dir, dcap, "%s/%s", models_dir(), id + 5); snprintf(stage, scap, "%s/artifacts/wake.%s", state_dir(), id + 5); *kind = "wake";
    } else if (!strcmp(id, "sound")) {
        snprintf(dir, dcap, "%s", env_or("HASSMIC_AED", "/data/local/hassmic/aed")); snprintf(stage, scap, "%s/artifacts/sound", state_dir()); *kind = "sound";
    } else if (!strcmp(id, "whisper")) {
        snprintf(dir, dcap, "%s", env_or("HASSMIC_WHISPER", "/data/local/hassmic/whisper")); snprintf(stage, scap, "%s/artifacts/whisper", state_dir()); *kind = "whisper";
    } else return -1;
    return 0;
}

struct file { char name[64]; long size; long long mtime; };
static int cmp_file(const void *a, const void *b) { return strcmp(((const struct file *)a)->name, ((const struct file *)b)->name); }

/* the regular files of a folder, sorted; -1 if it is not there, or holds something else (a folder, a link, a bad name) */
static int list_dir(const char *dir, struct file *f, int max, int strict)
{
    DIR *d = opendir(dir); struct dirent *e; int n = 0; char p[512]; struct stat st;
    if (!d) return -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') { if (strict && strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { n = -1; break; } continue; }
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (lstat(p, &st) || !S_ISREG(st.st_mode) || !good_name(e->d_name) || n == max) { if (strict) { n = -1; break; } continue; }
        snprintf(f[n].name, sizeof f[n].name, "%.63s", e->d_name); f[n].size = (long)st.st_size; f[n].mtime = (long long)st.st_mtime; n++;
    }
    closedir(d);
    if (n > 0) qsort(f, (size_t)n, sizeof *f, cmp_file);
    return n;
}

static int digest(const char *dir, const struct file *f, int n, uint8_t out[32])
{
    crypto_blake2b_ctx ctx; static uint8_t buf[65536]; char p[512], sz[24];
    crypto_blake2b_init(&ctx, 32);
    for (int i = 0; i < n; i++) {
        int fd; ssize_t k; long left = f[i].size;
        snprintf(sz, sizeof sz, "%ld", f[i].size);
        crypto_blake2b_update(&ctx, (const uint8_t *)f[i].name, strlen(f[i].name) + 1);
        crypto_blake2b_update(&ctx, (const uint8_t *)sz, strlen(sz) + 1);
        snprintf(p, sizeof p, "%s/%s", dir, f[i].name);
        if ((fd = open(p, O_RDONLY | O_NOFOLLOW)) < 0) return -1;
        while (left > 0 && (k = read(fd, buf, sizeof buf)) > 0) { crypto_blake2b_update(&ctx, buf, (size_t)k); left -= k; }
        close(fd);
        if (left) return -1;
    }
    crypto_blake2b_final(&ctx, out);
    return 0;
}

static struct { char dir[320]; long long key; uint8_t d[32]; } cache[NCACHE];
static unsigned cache_next;

/* lk held */
static int cached_digest(const char *dir, const struct file *f, int n, uint8_t out[32])
{
    long long key = n;
    for (int i = 0; i < n; i++) key = key * 1000003 + f[i].size * 31 + f[i].mtime;
    for (int i = 0; i < NCACHE; i++) if (cache[i].key == key && !strcmp(cache[i].dir, dir)) { memcpy(out, cache[i].d, 32); return 0; }
    if (digest(dir, f, n, out)) return -1;
    unsigned c = cache_next++ % NCACHE;
    snprintf(cache[c].dir, sizeof cache[c].dir, "%.300s", dir); cache[c].key = key; memcpy(cache[c].d, out, 32);
    return 0;
}

static void hex(char *o, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", b[i]); }

/* lk held: one artifact's JSON, if its folder holds what it should (a wake word set its manifest) */
static size_t one_json(char *o, size_t cap, const char *id, int first)
{
    char dir[300], stage[300], dh[65]; const char *kind; struct file f[MAX_FILES]; uint8_t d[32]; size_t n = 0; long total = 0;
    int k, manifest = 0;
    if (resolve(id, dir, sizeof dir, stage, sizeof stage, &kind) || (k = list_dir(dir, f, MAX_FILES, 0)) <= 0) return 0;
    for (int i = 0; i < k; i++) { total += f[i].size; manifest |= !strcmp(f[i].name, "pryon.manifest") || !strcmp(f[i].name, "pryon_whisper.manifest"); }
    if (!manifest || cached_digest(dir, f, k, d)) return 0;
    hex(dh, d, 32);
    J("%s{\"id\":\"%s\",\"kind\":\"%s\",\"name\":\"%s\",\"size\":%ld,\"digest\":\"%s\",\"files\":[", first ? "" : ",", id, kind,
      strchr(id, ':') ? strchr(id, ':') + 1 : id, total, dh);
    for (int i = 0; i < k; i++) J("%s{\"name\":\"%s\",\"size\":%ld}", i ? "," : "", f[i].name, f[i].size);
    J("]}");
    return n < cap ? n : cap;
}

static long free_bytes(void)
{
    struct statvfs s;
    return statvfs(state_dir(), &s) ? -1 : (long)((unsigned long long)s.f_bavail * s.f_frsize > (unsigned long long)LONG_MAX
                                                  ? LONG_MAX : (unsigned long long)s.f_bavail * s.f_frsize);
}

size_t art_list_json(char *o, size_t cap)
{
    size_t n = 0; DIR *d; struct dirent *e; int first = 1; char id[80], p[300];
    pthread_mutex_lock(&lk);
    J("{\"artifacts\":[");
    if ((d = opendir(models_dir()))) {
        while ((e = readdir(d))) {
            if (!good_name(e->d_name)) continue;                   /* .try-*: scripts/artifacts.sh testing one */
            snprintf(id, sizeof id, "wake:%.63s", e->d_name);
            size_t k = one_json(o + n, cap - n, id, first); n += k; if (k) first = 0;
        }
        closedir(d);
    }
    for (int i = 0; i < 2; i++) { size_t k = one_json(o + n, cap - n, i ? "whisper" : "sound", first); n += k; if (k) first = 0; }
    J("],\"free\":%ld,\"staged\":[", free_bytes());
    snprintf(p, sizeof p, "%s/artifacts", state_dir()); first = 1;
    if ((d = opendir(p))) {
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 6 && !strcmp(e->d_name + l - 6, ".ready")) { J("%s\"%.*s\"", first ? "" : ",", (int)(l - 6), e->d_name); first = 0; }
        }
        closedir(d);
    }
    J("],\"result\":\"");
    snprintf(p, sizeof p, "%s/artifacts/result", state_dir());
    FILE *f = fopen(p, "r"); int c;
    if (f) { while ((c = fgetc(f)) != EOF && n + 8 < cap) { if (c == '"' || c == '\\') o[n++] = '\\'; o[n++] = c == '\n' ? ' ' : (char)c; } fclose(f); }
    J("\"}");
    pthread_mutex_unlock(&lk);
    return n < cap ? n : cap - 1;
}

int art_read(const char *id, const char *file, long off, long len, void **data, size_t *got, char *err, size_t errsz)
{
    char dir[300], stage[300], p[400]; const char *kind; int fd; struct stat st;
    *data = NULL; *got = 0;
    if (resolve(id, dir, sizeof dir, stage, sizeof stage, &kind) || !good_name(file)) { snprintf(err, errsz, "no such artifact"); return -1; }
    if (off < 0 || len <= 0 || len > ART_CHUNK_MAX) { snprintf(err, errsz, "bad range"); return -1; }
    snprintf(p, sizeof p, "%s/%s", dir, file);
    if ((fd = open(p, O_RDONLY | O_NOFOLLOW)) < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        snprintf(err, errsz, "no such file"); return -1;
    }
    if (off > st.st_size) off = (long)st.st_size;
    if (len > st.st_size - off) len = (long)(st.st_size - off);
    if (!(*data = malloc((size_t)len + 1))) { close(fd); snprintf(err, errsz, "no memory"); return -1; }
    ssize_t k = len ? pread(fd, *data, (size_t)len, off) : 0;
    close(fd);
    if (k < 0) { free(*data); *data = NULL; snprintf(err, errsz, "read failed"); return -1; }
    *got = (size_t)k;
    return 0;
}

/* ---------------------------------------------------------------- writing */

/* state/artifacts/<stage>.expect: "<digest hex>\n<file> <size>\n..." */
static int read_expect(const char *stage, char dh[65], struct file *f, int max)
{
    char p[320], line[128]; FILE *x; int n = 0;
    snprintf(p, sizeof p, "%s.expect", stage);
    if (!(x = fopen(p, "r"))) return -1;
    if (!fgets(line, sizeof line, x) || sscanf(line, "%64s", dh) != 1) { fclose(x); return -1; }
    while (n < max && fgets(line, sizeof line, x)) if (sscanf(line, "%63s %ld", f[n].name, &f[n].size) == 2) n++;
    fclose(x);
    return n;
}

static void rm_tree(const char *dir)         /* a staged folder: flat */
{
    DIR *d = opendir(dir); struct dirent *e; char p[400];
    if (d) {
        while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { snprintf(p, sizeof p, "%s/%s", dir, e->d_name); unlink(p); }
        closedir(d);
    }
    rmdir(dir);
}

static void unstage(const char *stage)        /* lk held */
{
    char p[320];
    rm_tree(stage);
    snprintf(p, sizeof p, "%s.expect", stage); unlink(p);
    snprintf(p, sizeof p, "%s.ready", stage); unlink(p);
}

int art_begin(const char *spec, char *err, size_t errsz)
{
    char id[80], dh[80], dir[300], stage[300], p[320], line[160]; const char *kind; struct file f[MAX_FILES]; int n = 0, k;
    long total = 0; const char *s = spec; FILE *x;
    if (sscanf(s, "%79s %79s%n", id, dh, &k) != 2 || strlen(dh) != 64 || resolve(id, dir, sizeof dir, stage, sizeof stage, &kind)) {
        snprintf(err, errsz, "not an artifact"); return -1;
    }
    for (s += k; *s; ) {
        size_t l = strcspn(s, "\n"); char name[80]; long size;
        snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), s);
        s += l + (s[l] == '\n');
        if (!line[0]) continue;
        if (sscanf(line, "%79s %ld", name, &size) != 2 || !good_name(name) || size < 0 || n == MAX_FILES) { snprintf(err, errsz, "bad file list"); return -1; }
        for (int i = 0; i < n; i++) if (!strcmp(f[i].name, name)) { snprintf(err, errsz, "a file twice"); return -1; }
        snprintf(f[n].name, sizeof f[n].name, "%.63s", name); f[n].size = size; total += size; n++;
    }
    if (!n || total > MAX_TOTAL) { snprintf(err, errsz, n ? "too big" : "no files"); return -1; }
    pthread_mutex_lock(&lk);
    unstage(stage);
    long fr = free_bytes();
    /* the staged copy and root's copy of it exist at the same time, before the staged one goes */
    if (fr >= 0 && fr < 2 * total + MARGIN) {
        pthread_mutex_unlock(&lk);
        snprintf(err, errsz, "not enough space on this Echo: %ld MB free, %ld MB needed", fr >> 20, (2 * total + MARGIN) >> 20); return -1;
    }
    snprintf(p, sizeof p, "%s/artifacts", state_dir()); mkdir(p, 0700);
    if (mkdir(stage, 0755) && errno != EEXIST) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "cannot write %s", stage); return -1; }
    snprintf(p, sizeof p, "%s.expect", stage);
    if (!(x = fopen(p, "w"))) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "cannot write %s", p); return -1; }
    fprintf(x, "%s\n", dh);
    for (int i = 0; i < n; i++) fprintf(x, "%s %ld\n", f[i].name, f[i].size);
    fclose(x);
    for (int i = 0; i < n; i++) {               /* every file there from the start: an empty one gets no piece */
        char fp[400]; snprintf(fp, sizeof fp, "%s/%.63s", stage, f[i].name);
        int fd = open(fp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
        if (fd >= 0) close(fd);
    }
    pthread_mutex_unlock(&lk);
    fprintf(stderr, "artifacts: receiving %s, %d files, %ld KB\n", id, n, total >> 10);
    return 0;
}

int art_chunk(const char *id, const char *file, long off, const void *data, size_t len, char *err, size_t errsz)
{
    char dir[300], stage[300], p[400], dh[65]; const char *kind; struct file f[MAX_FILES]; int n, fd, i;
    if (resolve(id, dir, sizeof dir, stage, sizeof stage, &kind) || !good_name(file)) { snprintf(err, errsz, "not an artifact"); return -1; }
    pthread_mutex_lock(&lk);
    n = read_expect(stage, dh, f, MAX_FILES);
    pthread_mutex_unlock(&lk);
    for (i = 0; i < n && strcmp(f[i].name, file); i++) ;
    if (n < 0 || i == n) { snprintf(err, errsz, "not being received"); return -1; }
    if (off < 0 || off + (long)len > f[i].size) { snprintf(err, errsz, "past the end of %s", file); return -1; }
    snprintf(p, sizeof p, "%s/%s", stage, file);
    if ((fd = open(p, O_WRONLY | O_NOFOLLOW)) < 0) { snprintf(err, errsz, "not being received"); return -1; }
    ssize_t w = pwrite(fd, data, len, off);
    close(fd);
    if (w != (ssize_t)len) { snprintf(err, errsz, "write failed (space?)"); return -1; }
    return 0;
}

/* pryon_test next to our binary (or HASSMIC_PRYON_TEST): 1 = the set does not load, 3 = loaded (no audio to hear).
 * hassmic ignores SIGCHLD, so its children are reaped unseen: a child of ours waits for pryon_test itself and passes
 * the exit code on through a pipe.  -1: no pryon_test here. */
static int try_load(const char *manifest)
{
    char exe[300] = "", *slash; const char *t = getenv("HASSMIC_PRYON_TEST"); int pfd[2], code = -1; pid_t pid;
    if (!t) {
        ssize_t l = readlink("/proc/self/exe", exe, sizeof exe - 16);
        if (l <= 0) return -1;
        exe[l] = 0; if ((slash = strrchr(exe, '/'))) strcpy(slash + 1, "pryon_test");
        t = exe;
    }
    if (access(t, X_OK) || pipe(pfd)) return -1;
    if ((pid = fork()) == 0) {                  /* only async-signal-safe calls from here: we are a copy of a threaded process */
        int st = 0; pid_t c;
        close(pfd[0]); signal(SIGCHLD, SIG_DFL);
        if ((c = fork()) == 0) {
            int nul = open("/dev/null", O_RDWR); dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
            execl(t, t, "-m", manifest, "/dev/null", (char *)NULL); _exit(127);
        }
        unsigned char r = c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) ? (unsigned char)WEXITSTATUS(st) : 255;
        if (write(pfd[1], &r, 1) != 1) _exit(1);
        _exit(0);
    }
    close(pfd[1]);
    if (pid > 0) { unsigned char r; if (read(pfd[0], &r, 1) == 1) code = r; }
    close(pfd[0]);
    return code;
}

int art_commit(const char *id, char *err, size_t errsz)
{
    char dir[300], stage[300], dh[65], gh[65], p[400]; const char *kind; struct file want[MAX_FILES], have[MAX_FILES]; uint8_t d[32]; int n, m;
    if (resolve(id, dir, sizeof dir, stage, sizeof stage, &kind)) { snprintf(err, errsz, "not an artifact"); return -1; }
    pthread_mutex_lock(&lk);
    n = read_expect(stage, dh, want, MAX_FILES);
    m = list_dir(stage, have, MAX_FILES, 1);
    int ok = n > 0 && m == n;
    for (int i = 0; ok && i < n; i++) {                     /* both sorted? want is in the sender's order: look each up */
        int j; for (j = 0; j < m && strcmp(have[j].name, want[i].name); j++) ;
        ok = j < m && have[j].size == want[i].size;
    }
    if (!ok) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "incomplete: not every file arrived whole"); return -1; }
    if (digest(stage, have, m, d)) { unstage(stage); pthread_mutex_unlock(&lk); snprintf(err, errsz, "cannot read what arrived"); return -1; }
    hex(gh, d, 32);
    if (strcmp(gh, dh)) {
        unstage(stage); pthread_mutex_unlock(&lk);
        snprintf(err, errsz, "digest does not match: damaged on the way"); fprintf(stderr, "artifacts: %s arrived damaged, dropped\n", id); return -1;
    }
    if (!strcmp(kind, "wake")) {
        snprintf(p, sizeof p, "%s/pryon.manifest", stage);
        int r = access(p, R_OK) ? 1 : try_load(p);
        if (r == 127) r = -1;                               /* pryon_test itself did not run: not the set's fault */
        if (r == 1 || r == 255) {                           /* 255: it died loading the set */
            unstage(stage); pthread_mutex_unlock(&lk);
            snprintf(err, errsz, "this Echo's wake word engine cannot load it"); fprintf(stderr, "artifacts: %s does not load here, dropped\n", id); return -1;
        }
        if (r < 0) fprintf(stderr, "artifacts: no pryon_test here, %s installed untried\n", id);
    }
    snprintf(p, sizeof p, "%s.ready", stage);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd >= 0) close(fd);
    pthread_mutex_unlock(&lk);
    fprintf(stderr, "artifacts: %s arrived whole, waits to be installed\n", id);
    return 0;
}

int art_install(char *err, size_t errsz)
{
    char p[300], tmp[320], line[320]; DIR *d; struct dirent *e; FILE *f; int n = 0;
    pthread_mutex_lock(&lk);
    snprintf(p, sizeof p, "%s/artifacts", state_dir());
    snprintf(tmp, sizeof tmp, "%s/request.tmp", p);
    if (!(d = opendir(p)) || !(f = fopen(tmp, "w"))) { if (d) closedir(d); pthread_mutex_unlock(&lk); snprintf(err, errsz, "nothing to install"); return -1; }
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l <= 6 || strcmp(e->d_name + l - 6, ".ready")) continue;
        snprintf(line, sizeof line, "%.*s", (int)(l - 6), e->d_name);
        if (!strncmp(line, "wake.", 5)) line[4] = ':';            /* the stage folder's name back to the id */
        fprintf(f, "%s\n", line); n++;
    }
    closedir(d);
    fclose(f);
    snprintf(line, sizeof line, "%s/result", p); unlink(line);
    snprintf(line, sizeof line, "%s/request", p);
    if (!n || rename(tmp, line)) { unlink(tmp); pthread_mutex_unlock(&lk); snprintf(err, errsz, "nothing to install"); return -1; }
    pthread_mutex_unlock(&lk);
    fprintf(stderr, "artifacts: %d handed to root to install; hassmic restarts\n", n);
    return 0;
}
