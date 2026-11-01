/* The library of sound packs.  WAVs are 96 kB/s, so a pack is 9.6 MB at most (sounds.c's lengths) and about 1.7 MB at
 * stock's: small next to /data, but the state files live there too, and a full partition fails their writes.  So the
 * library has a cap, and nothing is written with less than FREE_MIN left on the partition, whatever the cap says. */
#include "soundpack.h"
#include "sounds.h"
#include "statefile.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define FREE_MIN (32ull << 20)
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
static void base_path(char *out, size_t cap) { snprintf(out, cap, "%s/sound-packs", state_dir()); }
static void pack_path(int id, char *out, size_t cap) { snprintf(out, cap, "%s/sound-packs/%d", state_dir(), id); }
static void name_path(int id, char *out, size_t cap) { snprintf(out, cap, "%s/sound-packs/%d/name", state_dir(), id); }
static void wav_path(int id, int s, char *out, size_t cap) { snprintf(out, cap, "%s/sound-packs/%d/%s.wav", state_dir(), id, sound_id(s)); }
static int good_id(int id) { return id >= 1 && id <= SP_PACKS_MAX; }
static int good_sound(int s) { return s >= 0 && s < SND_COUNT && sound_id(s); }
static int exists(int id) { char p[320]; struct stat st; name_path(id, p, sizeof p); return good_id(id) && !stat(p, &st); }
static size_t size_of(const char *p) { struct stat st; return stat(p, &st) ? 0 : (size_t)st.st_size; }

static size_t bytes_of(int id, unsigned *sounds)
{
    char p[320]; size_t n = 0;
    if (sounds) *sounds = 0;
    for (int s = 0; s < SND_COUNT; s++) {
        if (!good_sound(s)) continue;
        wav_path(id, s, p, sizeof p);
        size_t k = size_of(p);
        n += k;
        if (k && sounds) *sounds |= 1u << s;
    }
    return n;
}
static size_t total(void) { size_t n = 0; for (int id = 1; id <= SP_PACKS_MAX; id++) if (exists(id)) n += bytes_of(id, NULL); return n; }

static size_t lib_max(void) { const char *e = getenv("HASSMIC_SOUNDPACK_MAX"); return e ? (size_t)strtoul(e, NULL, 10) : SP_LIBRARY_MAX; }
static int disk_ok(size_t need, char *err, size_t errsz)
{
    const char *e = getenv("HASSMIC_SOUNDPACK_FREE_MIN");
    unsigned long long floor = e ? strtoull(e, NULL, 10) : FREE_MIN;
    struct statvfs v;
    if (statvfs(state_dir(), &v)) { snprintf(err, errsz, "cannot tell the space left: %s", strerror(errno)); return 0; }
    if ((unsigned long long)v.f_bavail * v.f_frsize < need + floor) {
        snprintf(err, errsz, "not enough room left on this Echo: delete some of its models or packs first");
        return 0;
    }
    return 1;
}
/* need more bytes in the library, replacing `gone` */
static int room(size_t need, size_t gone, char *err, size_t errsz)
{
    if (total() - gone + need > lib_max()) { snprintf(err, errsz, "the library is full: %u MB at most, delete a pack first", (unsigned)(lib_max() >> 20)); return 0; }
    return disk_ok(need, err, errsz);
}

static void remove_pack(int id)
{
    char p[320];
    name_path(id, p, sizeof p); unlink(p);          /* first the name: without it there is no pack, whatever a reset leaves */
    pack_path(id, p, sizeof p); statefile_sync(p);  /* gone on the flash before its sounds are */
    for (int s = 0; s < SND_COUNT; s++) if (good_sound(s)) { wav_path(id, s, p, sizeof p); unlink(p); }
    pack_path(id, p, sizeof p); rmdir(p);
    base_path(p, sizeof p); statefile_sync(p);
}

int soundpack_new(const char *name, size_t len, int current, char *err, size_t errsz)
{
    char p[320]; int id = 0;
    if (!len || len > SP_NAME_MAX) { snprintf(err, errsz, "a name of 1 to %d bytes", SP_NAME_MAX); return -1; }
    for (size_t i = 0; i < len; i++) if ((unsigned char)name[i] < 32 || name[i] == 127) { snprintf(err, errsz, "no control characters in a name"); return -1; }
    pthread_mutex_lock(&mu);
    for (int i = 1; i <= SP_PACKS_MAX && !id; i++) if (!exists(i)) id = i;
    if (!id) { snprintf(err, errsz, "%d packs at most: delete one first", SP_PACKS_MAX); goto no; }
    if (!disk_ok(len, err, errsz)) goto no;
    remove_pack(id);                                /* what a reset left of one half made */
    base_path(p, sizeof p);
    if (!mkdir(p, 0755)) statefile_sync(state_dir());
    else if (errno != EEXIST) goto cannot;
    pack_path(id, p, sizeof p);
    if (mkdir(p, 0755)) goto cannot;
    base_path(p, sizeof p); statefile_sync(p);
    size_t copied = 0;                              /* total() does not see a pack without its name yet */
    if (current) for (int s = 0; s < SND_COUNT; s++) {
        size_t n; unsigned char *w;
        if (!good_sound(s) || !(w = sound_own_read(s, &n))) continue;
        int ok = room(copied + n, 0, err, errsz);
        copied += n;
        if (ok) { wav_path(id, s, p, sizeof p); if (statefile_write(p, 0644, w, n)) { ok = 0; snprintf(err, errsz, "cannot write %s", p); } }
        free(w);
        if (!ok) { remove_pack(id); goto no; }
    }
    name_path(id, p, sizeof p);                     /* last: a pack is there once it has its name */
    if (statefile_write(p, 0644, name, len)) goto cannot;
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "sound pack %d: made\n", id);
    return id;
cannot:
    snprintf(err, errsz, "cannot write %s", p);
    if (id) remove_pack(id);
no:
    pthread_mutex_unlock(&mu);
    return -1;
}

int soundpack_put(int id, int s, const void *wav, size_t len, char *err, size_t errsz)
{
    char p[320]; int r = -1;
    if (!good_sound(s)) { snprintf(err, errsz, "no such sound"); return -1; }
    if (sound_own_check(s, wav, len, err, errsz)) return -1;
    pthread_mutex_lock(&mu);
    if (!exists(id)) snprintf(err, errsz, "no such pack");
    else {
        wav_path(id, s, p, sizeof p);
        if (room(len, size_of(p), err, errsz)) {
            if ((r = statefile_write(p, 0644, wav, len))) snprintf(err, errsz, "cannot write %s", p);
        }
    }
    pthread_mutex_unlock(&mu);
    return r;
}

static unsigned char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); struct stat st; unsigned char *b;
    if (!f) return NULL;
    if (fstat(fileno(f), &st) || st.st_size <= 0 || (size_t)st.st_size > SOUND_CUSTOM_MAX || !(b = malloc((size_t)st.st_size))) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)st.st_size, f) != (size_t)st.st_size) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)st.st_size;
    return b;
}

unsigned char *soundpack_get(int id, int s, size_t *len)
{
    char p[320]; unsigned char *b = NULL;
    if (!good_sound(s)) return NULL;
    pthread_mutex_lock(&mu);
    if (exists(id)) { wav_path(id, s, p, sizeof p); b = read_all(p, len); }
    pthread_mutex_unlock(&mu);
    return b;
}

/* Every file checked before the first is touched: a bad one leaves the sounds in use as they were */
int soundpack_apply(int id, char *err, size_t errsz)
{
    char p[320]; int r = -1; size_t need = 0;
    pthread_mutex_lock(&mu);
    if (!exists(id)) { snprintf(err, errsz, "no such pack"); goto out; }
    for (int s = 0; s < SND_COUNT; s++) {
        size_t n; unsigned char *w;
        if (!good_sound(s)) continue;
        wav_path(id, s, p, sizeof p);
        if (!size_of(p)) continue;
        int bad = !(w = read_all(p, &n)) || sound_own_check(s, w, n, err, errsz);
        if (!w) snprintf(err, errsz, "cannot read %s", p);
        free(w);
        if (bad) goto out;
        need += n;
    }
    if (!disk_ok(need, err, errsz)) goto out;
    r = 0;
    for (int s = 0; s < SND_COUNT && !r; s++) {
        size_t n; unsigned char *w;
        if (!good_sound(s)) continue;
        wav_path(id, s, p, sizeof p);
        if (!size_of(p)) { if ((r = sound_own_delete(s))) snprintf(err, errsz, "cannot delete a sound in use"); continue; }
        if (!(w = read_all(p, &n))) { snprintf(err, errsz, "cannot read %s", p); r = -1; continue; }
        r = sound_own_save(s, w, n, err, errsz);
        free(w);
    }
    if (!r) fprintf(stderr, "sound pack %d: applied\n", id);
out:
    pthread_mutex_unlock(&mu);
    return r;
}

int soundpack_delete(int id)
{
    int r = -1;
    pthread_mutex_lock(&mu);
    if (exists(id)) { remove_pack(id); r = 0; }
    pthread_mutex_unlock(&mu);
    return r;
}

int soundpack_info(int id, char *name, size_t *bytes, unsigned *sounds)
{
    char p[320]; FILE *f; int r = -1;
    pthread_mutex_lock(&mu);
    if (exists(id)) {
        name_path(id, p, sizeof p);
        if ((f = fopen(p, "r"))) {
            size_t n = fread(name, 1, SP_NAME_MAX, f); name[n] = 0; fclose(f);
            *bytes = bytes_of(id, sounds); r = 0;
        }
    }
    pthread_mutex_unlock(&mu);
    return r;
}

size_t soundpack_bytes(void) { pthread_mutex_lock(&mu); size_t n = total(); pthread_mutex_unlock(&mu); return n; }
size_t soundpack_max(void) { return lib_max(); }
