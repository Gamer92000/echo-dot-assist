/* microWakeWord models on this Echo (mww.h): one folder each under <state>/mww, hassmic's own (unlike Amazon's model
 * sets, which root installs): manifest.json and model.tflite.  A model is only ever put in place whole and after it
 * loaded in our interpreter: written into a hidden folder beside the others, then renamed over the old one.  After every
 * change the core lists the wake words anew (core_wake_models_changed). */
#include "mww.h"
#include "core.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MANIFEST_MAX 8192

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;  /* changes to the folder */

const char *mww_dir(void)
{
    static char d[300]; const char *e = getenv("HASSMIC_MWW"), *s = getenv("HASSMIC_STATE");
    if (e) return e;
    snprintf(d, sizeof d, "%s/mww", s ? s : "/data/local/hassmic/state");
    return d;
}

int mww_good_id(const char *id)
{
    size_t n = strlen(id);
    if (!n || n > 40 || !isalnum((unsigned char)id[0])) return 0;
    for (; *id; id++) if (!(islower((unsigned char)*id) || isdigit((unsigned char)*id) || *id == '_' || *id == '-')) return 0;
    return 1;
}

void mww_paths(const char *id, char *manifest, char *model, size_t cap)
{
    if (manifest) snprintf(manifest, cap, "%s/%s/manifest.json", mww_dir(), id);
    if (model) snprintf(model, cap, "%s/%s/model.tflite", mww_dir(), id);
}

/* ---------------------------------------------------------------- the manifest
 * Not a general JSON parser: the few keys of microWakeWord's manifest, by name wherever they sit (they are unique in it),
 * each value a string, a number or an array of strings.  Anything else is ignored. */

static const char *skip_ws(const char *p, const char *e) { while (p < e && isspace((unsigned char)*p)) p++; return p; }

/* a JSON string at p (after its quote) into out, \u escapes outside ASCII dropped; past the closing quote, NULL if none */
static const char *jstr(const char *p, const char *e, char *out, size_t cap)
{
    size_t n = 0;
    for (; p < e && *p != '"'; p++) {
        char c = *p;
        if (c == '\\' && p + 1 < e) {
            c = *++p;
            if (c == 'u') {
                unsigned v = 0;
                if (e - p < 5) return NULL;
                for (int k = 1; k <= 4; k++) { if (!isxdigit((unsigned char)p[k])) return NULL; v = v * 16 + (unsigned)(isdigit((unsigned char)p[k]) ? p[k] - '0' : (tolower((unsigned char)p[k]) - 'a' + 10)); }
                p += 4; c = v >= 0x20 && v < 0x7f ? (char)v : 0;
            } else if (c == 'n' || c == 't' || c == 'r' || c == 'b' || c == 'f') c = ' ';
        }
        if (c && (unsigned char)c >= 0x20 && n + 1 < cap) out[n++] = c;
    }
    if (cap) out[n] = 0;
    return p < e ? p + 1 : NULL;
}

/* where KEY's value starts, NULL if it is not there */
static const char *jkey(const char *j, const char *e, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = j; p + kl + 2 < e; p++) {
        if (*p != '"' || strncmp(p + 1, key, kl) || p[kl + 1] != '"') continue;
        const char *v = skip_ws(p + kl + 2, e);
        if (v < e && *v == ':') return skip_ws(v + 1, e);
    }
    return NULL;
}

static int jget_str(const char *j, const char *e, const char *key, char *out, size_t cap)
{
    const char *v = jkey(j, e, key);
    if (cap) out[0] = 0;
    return v && v < e && *v == '"' && jstr(v + 1, e, out, cap) ? 0 : -1;
}

static int jget_num(const char *j, const char *e, const char *key, double *out)
{
    const char *v = jkey(j, e, key); char t[32]; size_t n = 0;
    if (!v) return -1;
    while (v + n < e && n < sizeof t - 1 && strchr("0123456789.-+eE", v[n])) n++;
    memcpy(t, v, n); t[n] = 0;
    char *end; *out = strtod(t, &end);
    return n && !*end ? 0 : -1;
}

int mww_manifest_parse(const char *json, size_t len, struct mww_info *i, char *err, size_t errsz)
{
    const char *e = json + len, *v; char t[64]; double d;
    memset(i, 0, sizeof *i);
    if (len > MANIFEST_MAX) { snprintf(err, errsz, "manifest too long"); return -1; }
    if (!jget_str(json, e, "type", t, sizeof t) && strcmp(t, "micro")) { snprintf(err, errsz, "a \"%s\" model, not microWakeWord's", t); return -1; }
    if (jget_str(json, e, "wake_word", i->name, sizeof i->name) || !i->name[0]) { snprintf(err, errsz, "manifest without a wake_word"); return -1; }
    if (jget_num(json, e, "probability_cutoff", &d) || !(d > 0 && d < 1)) { snprintf(err, errsz, "manifest without a probability_cutoff"); return -1; }
    i->cutoff = (float)d;
    i->window = !jget_num(json, e, "sliding_window_size", &d) ? (int)d : 5;
    if (i->window < MWW_WINDOW_MIN || i->window > MWW_WINDOW_MAX) { snprintf(err, errsz, "sliding_window_size %d", i->window); return -1; }
    if (!jget_num(json, e, "feature_step_size", &d) && (int)d != 10) { snprintf(err, errsz, "feature_step_size %d: this Echo makes features every 10 ms", (int)d); return -1; }
    jget_str(json, e, "author", i->author, sizeof i->author);
    jget_str(json, e, "website", i->website, sizeof i->website);
    if ((v = jkey(json, e, "trained_languages")) && *v == '[') {
        size_t n = 0;
        for (v = skip_ws(v + 1, e); v && v < e && *v == '"'; ) {
            if (!(v = jstr(v + 1, e, t, sizeof t))) break;
            if (t[0] && n + strlen(t) + 2 < sizeof i->langs) n += (size_t)snprintf(i->langs + n, sizeof i->langs - n, "%s%s", n ? "," : "", t);
            v = skip_ws(v, e);
            if (v < e && *v == ',') v = skip_ws(v + 1, e);
        }
    }
    return 0;
}

static size_t jesc(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    if (cap) out[n] = 0;
    return n;
}

static int write_manifest(const char *path, const struct mww_info *i)
{
    char tmp[340], name[200], author[200], web[400], langs[300] = ""; FILE *f; size_t n = 0;
    jesc(name, sizeof name, i->name); jesc(author, sizeof author, i->author); jesc(web, sizeof web, i->website);
    for (const char *s = i->langs; *s; ) {
        size_t l = strcspn(s, ","); char one[64], esc[140];
        snprintf(one, sizeof one, "%.*s", (int)(l < sizeof one - 1 ? l : sizeof one - 1), s); jesc(esc, sizeof esc, one);
        if (n + strlen(esc) + 4 < sizeof langs) n += (size_t)snprintf(langs + n, sizeof langs - n, "%s\"%s\"", n ? ", " : "", esc);
        s += l + (s[l] == ',');
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(f = fopen(tmp, "w"))) return -1;
    fprintf(f, "{\n  \"type\": \"micro\",\n  \"wake_word\": \"%s\",\n  \"author\": \"%s\",\n  \"website\": \"%s\",\n  \"model\": \"model.tflite\",\n"
               "  \"trained_languages\": [%s],\n  \"version\": 2,\n  \"micro\": {\n    \"probability_cutoff\": %.2f,\n"
               "    \"sliding_window_size\": %d,\n    \"feature_step_size\": 10\n  }\n}\n", name, author, web, langs, i->cutoff, i->window);
    if (fclose(f) || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

/* ---------------------------------------------------------------- reading */

static char *slurp(const char *path, size_t max, size_t *n)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW); struct stat st; char *b;
    *n = 0;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > (off_t)max || !(b = malloc((size_t)st.st_size + 1))) { close(fd); return NULL; }
    ssize_t k = read(fd, b, (size_t)st.st_size);
    close(fd);
    if (k != st.st_size) { free(b); return NULL; }
    b[k] = 0; *n = (size_t)k;
    return b;
}

static int info_in(const char *dir, struct mww_info *i, char *err, size_t errsz)
{
    char p[400]; size_t n; struct stat st; char *j;
    snprintf(p, sizeof p, "%s/manifest.json", dir);
    if (!(j = slurp(p, MANIFEST_MAX, &n))) { snprintf(err, errsz, "no manifest.json"); return -1; }
    int r = mww_manifest_parse(j, n, i, err, errsz);
    free(j);
    if (r) return -1;
    snprintf(p, sizeof p, "%s/model.tflite", dir);
    if (lstat(p, &st) || !S_ISREG(st.st_mode) || st.st_size > MWW_MODEL_MAX) { snprintf(err, errsz, "no model.tflite"); return -1; }
    i->size = (long)st.st_size;
    return 0;
}

int mww_info(const char *id, struct mww_info *i)
{
    char d[300], e[120];
    if (!mww_good_id(id)) return -1;
    snprintf(d, sizeof d, "%.250s/%.40s", mww_dir(), id);
    if (info_in(d, i, e, sizeof e)) return -1;
    snprintf(i->id, sizeof i->id, "%s", id);
    return 0;
}

static int cmp_info(const void *a, const void *b) { return strcmp(((const struct mww_info *)a)->id, ((const struct mww_info *)b)->id); }

int mww_list(struct mww_info *out, int max)
{
    DIR *d = opendir(mww_dir()); struct dirent *e; int n = 0;
    if (!d) return 0;
    while (n < max && (e = readdir(d))) if (mww_good_id(e->d_name) && !mww_info(e->d_name, &out[n])) n++;
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, cmp_info);
    return n;
}

int mww_check_dir(const char *dir, char *err, size_t errsz)
{
    struct mww_info i; char p[400]; size_t n; char *m; struct mww_model *mod;
    if (info_in(dir, &i, err, errsz)) return -1;
    snprintf(p, sizeof p, "%s/model.tflite", dir);
    if (!(m = slurp(p, MWW_MODEL_MAX, &n))) { snprintf(err, errsz, "cannot read model.tflite"); return -1; }
    mod = mww_model_load(m, n, err, errsz);
    free(m);
    if (!mod) return -1;
    mww_model_free(mod);
    return 0;
}

size_t mww_list_json(char *o, size_t cap)
{
    static struct mww_info l[MWW_MODELS_MAX]; size_t n = 0; char a[5][340];
    pthread_mutex_lock(&lk);
    int k = mww_list(l, MWW_MODELS_MAX);
    n += (size_t)snprintf(o + n, cap - n, "[");
    for (int i = 0; i < k && n < cap; i++) {
        jesc(a[0], sizeof a[0], l[i].name); jesc(a[1], sizeof a[1], l[i].langs); jesc(a[2], sizeof a[2], l[i].author); jesc(a[3], sizeof a[3], l[i].website);
        n += (size_t)snprintf(o + n, cap - n, "%s{\"id\":\"%s\",\"name\":\"%s\",\"langs\":\"%s\",\"author\":\"%s\",\"website\":\"%s\",\"cutoff\":%.2f,\"window\":%d,\"size\":%ld}",
                              i ? "," : "", l[i].id, a[0], a[1], a[2], a[3], l[i].cutoff, l[i].window, l[i].size);
    }
    if (n < cap) n += (size_t)snprintf(o + n, cap - n, "]");
    pthread_mutex_unlock(&lk);
    return n < cap ? n : cap - 1;
}

/* ---------------------------------------------------------------- changing (lk held) */

static void rm_dir(const char *dir)             /* a model folder: files only */
{
    DIR *d = opendir(dir); struct dirent *e; char p[400];
    if (d) {
        while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { snprintf(p, sizeof p, "%s/%s", dir, e->d_name); unlink(p); }
        closedir(d);
    }
    rmdir(dir);
}

/* the folder NEW becomes model ID, the old one (if any) goes */
static int swap_in(const char *fresh, const char *id, char *err, size_t errsz)
{
    char dst[300], old[320];
    snprintf(dst, sizeof dst, "%.250s/%.40s", mww_dir(), id); snprintf(old, sizeof old, "%.250s/.old-%.40s", mww_dir(), id);
    rm_dir(old);
    if (rename(dst, old) && errno != ENOENT) { snprintf(err, errsz, "cannot replace %s", id); return -1; }
    if (rename(fresh, dst)) { rename(old, dst); snprintf(err, errsz, "cannot write %s", dst); return -1; }
    rm_dir(old);
    return 0;
}

static int write_file(const char *path, const void *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    ssize_t w = write(fd, data, n);
    return close(fd) || w != (ssize_t)n ? -1 : 0;
}

static int count_others(const char *id)
{
    static struct mww_info l[MWW_MODELS_MAX];
    int k = mww_list(l, MWW_MODELS_MAX), n = 0;
    for (int i = 0; i < k; i++) n += strcmp(l[i].id, id) != 0;
    return n;
}

int mww_add(const char *id, const char *json, size_t jn, const void *model, size_t mn, char *err, size_t errsz)
{
    struct mww_info i; struct mww_model *m; char fresh[320], p[400];
    if (!mww_good_id(id)) { snprintf(err, errsz, "a model's id: lower case letters, digits, _ and -, at most 40"); return -1; }
    if (!mn || mn > MWW_MODEL_MAX) { snprintf(err, errsz, "the model must be 1 byte to %d KB", MWW_MODEL_MAX >> 10); return -1; }
    if (jn) { if (mww_manifest_parse(json, jn, &i, err, errsz)) return -1; }
    else {                                      /* no manifest: microWakeWord's defaults, named after the id */
        memset(&i, 0, sizeof i); i.cutoff = 0.97f; i.window = 5;
        snprintf(i.name, sizeof i.name, "%s", id);
        for (char *c = i.name; *c; c++) { if (*c == '_' || *c == '-') *c = ' '; *c = (char)(c == i.name || c[-1] == ' ' ? toupper((unsigned char)*c) : *c); }
    }
    if (i.cutoff < MWW_CUTOFF_MIN) i.cutoff = MWW_CUTOFF_MIN;
    if (i.cutoff > MWW_CUTOFF_MAX) i.cutoff = MWW_CUTOFF_MAX;
    if (!(m = mww_model_load(model, mn, err, errsz))) return -1;
    mww_model_free(m);
    pthread_mutex_lock(&lk);
    if (count_others(id) >= MWW_MODELS_MAX) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "at most %d models: delete one first", MWW_MODELS_MAX); return -1; }
    mkdir(mww_dir(), 0755);
    snprintf(fresh, sizeof fresh, "%.250s/.new-%.40s", mww_dir(), id);
    rm_dir(fresh);
    int r = -1;
    if (mkdir(fresh, 0755)) snprintf(err, errsz, "cannot write %s", fresh);
    else {
        snprintf(p, sizeof p, "%s/model.tflite", fresh);
        if (write_file(p, model, mn)) snprintf(err, errsz, "cannot write the model (space?)");
        else {
            snprintf(p, sizeof p, "%s/manifest.json", fresh);
            if (write_manifest(p, &i)) snprintf(err, errsz, "cannot write the manifest");
            else r = swap_in(fresh, id, err, errsz);
        }
        if (r) rm_dir(fresh);
    }
    pthread_mutex_unlock(&lk);
    if (r) return -1;
    fprintf(stderr, "microwakeword: model %s (\"%s\", cutoff %.2f, window %d, %zu bytes) added\n", id, i.name, i.cutoff, i.window, mn);
    core_wake_models_changed();
    return 0;
}

int mww_install_dir(const char *dir, const char *id, char *err, size_t errsz)
{
    if (!mww_good_id(id)) { snprintf(err, errsz, "not a model id"); return -1; }
    if (mww_check_dir(dir, err, errsz)) return -1;
    pthread_mutex_lock(&lk);
    mkdir(mww_dir(), 0755);
    int r = count_others(id) >= MWW_MODELS_MAX ? (snprintf(err, errsz, "at most %d models: delete one first", MWW_MODELS_MAX), -1) : swap_in(dir, id, err, errsz);
    pthread_mutex_unlock(&lk);
    if (r) return -1;
    fprintf(stderr, "microwakeword: model %s arrived from another Echo\n", id);
    core_wake_models_changed();
    return 0;
}

int mww_edit(const char *id, const char *text, char *err, size_t errsz)
{
    struct mww_info i; char path[340];
    pthread_mutex_lock(&lk);
    if (mww_info(id, &i)) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "no such model"); return -1; }
    for (const char *s = text; *s; ) {
        size_t l = strcspn(s, "\n"); char line[200], *v;
        snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), s);
        s += l + (s[l] == '\n');
        line[strcspn(line, "\r")] = 0;
        if (!(v = strchr(line, '='))) continue;
        *v++ = 0;
        if (!strcmp(line, "name")) {
            size_t n = strlen(v);
            while (n && isspace((unsigned char)v[n - 1])) v[--n] = 0;
            while (isspace((unsigned char)*v)) v++;
            if (!*v || strlen(v) >= sizeof i.name) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "a name of 1 to %zu bytes", sizeof i.name - 1); return -1; }
            snprintf(i.name, sizeof i.name, "%s", v);
        } else if (!strcmp(line, "cutoff")) {
            char *end; double c = strtod(v, &end);
            if (*end || !(c >= MWW_CUTOFF_MIN - 1e-6 && c <= MWW_CUTOFF_MAX + 1e-6)) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "cutoff %.2f to %.2f", MWW_CUTOFF_MIN, MWW_CUTOFF_MAX); return -1; }
            i.cutoff = (float)c;
        } else if (!strcmp(line, "window")) {
            char *end; long w = strtol(v, &end, 10);
            if (*end || w < MWW_WINDOW_MIN || w > MWW_WINDOW_MAX) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "window %d to %d", MWW_WINDOW_MIN, MWW_WINDOW_MAX); return -1; }
            i.window = (int)w;
        } else { pthread_mutex_unlock(&lk); snprintf(err, errsz, "%s: nothing to change", line); return -1; }
    }
    mww_paths(id, path, NULL, sizeof path);
    int r = write_manifest(path, &i);
    pthread_mutex_unlock(&lk);
    if (r) { snprintf(err, errsz, "cannot write the manifest"); return -1; }
    fprintf(stderr, "microwakeword: model %s now \"%s\", cutoff %.2f, window %d\n", id, i.name, i.cutoff, i.window);
    core_wake_models_changed();
    return 0;
}

int mww_delete(const char *id, char *err, size_t errsz)
{
    char d[300]; struct stat st;
    if (!mww_good_id(id)) { snprintf(err, errsz, "no such model"); return -1; }
    snprintf(d, sizeof d, "%.250s/%.40s", mww_dir(), id);
    pthread_mutex_lock(&lk);
    int r = lstat(d, &st) || !S_ISDIR(st.st_mode) ? -1 : 0;
    if (!r) rm_dir(d);
    pthread_mutex_unlock(&lk);
    if (r) { snprintf(err, errsz, "no such model"); return -1; }
    fprintf(stderr, "microwakeword: model %s deleted\n", id);
    core_wake_models_changed();
    return 0;
}
