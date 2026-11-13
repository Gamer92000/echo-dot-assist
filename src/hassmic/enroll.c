/*
 * A wake word of your own, trained on the Echo: see enroll.h.  One session at a time, in a thread of its own.
 *
 *   recording   16 prompts, 3 s each by default: the word eight times, then eight other things (parts of the word, a few
 *               everyday phrases), a countdown before each.  The audio is the capture thread's own (post-AEC micAsr, what
 *               the wake word hears); the wake word is not fed meanwhile, so the word being trained cannot wake the Echo
 *               or start a request.  A recording that is silent or clipping is asked for again.  The last two of each
 *               kind are kept back: the new head never sees them until it is frozen (pryon_train).
 *   extracting  each recording, with 2 s of silence either side (the engine's warm-up and its tail), through the stock
 *               engine in pryon_extract; ~15 s each on a Dot 3, which the engine takes at the pace of the audio
 *   training    pryon_train: ~4 minutes, nice 19.  Its answer: the new head's verdict on the recordings it trained on and
 *               on the held-out ones.  Every held-out word must be heard and nothing else: else the set is not installed.
 *   packaging   enroll_pkg.c, staged and committed as any model set (artifacts.c: it must load in the Echo's own engine),
 *               then installed by root, which restarts hassmic as for any model set.
 * The recordings, the activations and the candidate model are deleted when the session ends, whatever came of it.
 *
 * Why the tools are processes: pryon_extract runs the stock engine in a process of its own (it patches its own copy of the
 * library's code in memory to read the activations; nothing of hassmic's engine is touched), and training is minutes of
 * numbers at the lowest priority.  hassmic ignores SIGCHLD (children are reaped unseen), so a tool runs under a child of
 * ours that waits for it and tells its exit code through the pipe the tool's output comes through (as artifacts.c's
 * try_load does for pryon_test).
 */
#include "enroll.h"
#include "artifacts.h"
#include "enroll_pkg.h"
#include "statefile.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RATE        16000
#define PER_CLASS   8
#define CLIPS       (2 * PER_CLASS)
#define HELD_OUT    2                   /* of each kind: the last ones */
#define PAD         (2 * RATE)          /* silence before and after each recording */
#define STEPS       1600
#define NEED_FREE   (40L << 20)         /* 16 activation files of 1.2 MB, the recordings, the candidate, the set, root's copy */

enum { ST_IDLE, ST_RECORDING, ST_EXTRACTING, ST_TRAINING, ST_PACKAGING, ST_INSTALLING, ST_DONE, ST_FAILED, ST_CANCELLED, ST_UNRESOLVED };
static const char *const state_name[] = { "idle", "recording", "extracting", "training", "packaging", "installing", "done", "failed", "cancelled", "unresolved" };

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;      /* the status below */
static struct {
    int state;
    char phrase[96], key[40], id[72], base[300], lang[16];
    int use_now, seconds, clip, phase, held_out, label;     /* phase: 0 get ready, 1 speak, 2 checked */
    char text[100], message[200], error[540];
    double phase_at;                                        /* when the phase began (monotonic s), for the countdown */
    double progress; time_t job_at;                         /* the job after the recordings, 0..1, and its start */
    int have_result, train[4], test[4];                     /* tp fn fp tn */
    int installed;
} S;
static atomic_int cancel_flag, thread_live;

static atomic_int rec_prompts;                              /* the capture thread: no wake word while the prompts run */
static pthread_mutex_t mic_lk = PTHREAD_MUTEX_INITIALIZER;
static atomic_int rec_on;
static int16_t *rec_buf; static size_t rec_want, rec_n;

static void rec_release(void)             /* the capture thread copies into rec_buf under mic_lk: detach it there, free it outside */
{
    pthread_mutex_lock(&mic_lk); int16_t *b = rec_buf; rec_buf = NULL; rec_want = rec_n = 0; pthread_mutex_unlock(&mic_lk);
    free(b);
}

static const char *env_or(const char *e, const char *d) { const char *v = getenv(e); return v ? v : d; }
static const char *state_dir(void) { return env_or("HASSMIC_STATE", "/data/local/hassmic/state"); }
static const char *models_dir(void) { return env_or("HASSMIC_MODELS", "/data/local/hassmic/models"); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
static void wdir(char *o, size_t cap) { snprintf(o, cap, "%s/enroll", state_dir()); }

/* ------------------------------------------------------------------------------------------------------ the tools */

static int tool_path(const char *env, const char *name, char *out, size_t cap)
{
    const char *t = getenv(env); char exe[300], *slash;
    if (t) { snprintf(out, cap, "%s", t); return access(out, X_OK) ? -1 : 0; }
    ssize_t l = readlink("/proc/self/exe", exe, sizeof exe - 32);
    if (l <= 0) return -1;
    exe[l] = 0; if (!(slash = strrchr(exe, '/'))) return -1;
    snprintf(slash + 1, sizeof exe - (size_t)(slash + 1 - exe), "%s", name);
    snprintf(out, cap, "%s", exe);
    return access(out, X_OK) ? -1 : 0;
}

static int base_dir(const char *manifest, char *out, size_t cap)
{
    const char *e = getenv("HASSMIC_ENROLL_BASE"); char *slash;
    snprintf(out, cap, "%s", e ? e : manifest);
    if (!e && (slash = strrchr(out, '/'))) *slash = 0;
    return 0;
}

int enroll_available(const char *base_manifest)
{
    char t[300], b[300], m[340];
    if (!base_manifest || tool_path("HASSMIC_PRYON_EXTRACT", "pryon_extract", t, sizeof t) || tool_path("HASSMIC_PRYON_TRAIN", "pryon_train", t, sizeof t)) return 0;
    base_dir(base_manifest, b, sizeof b);
    snprintf(m, sizeof m, "%s/pryon.manifest", b);
    return !access(m, R_OK);
}

/* Runs ARGV at the lowest priority, with LIMIT_MB of address space if not 0 (the engine's tool maps a lot: none),
 * (the tool's output, stderr too, line by line to LINE) under a child that waits for it.  The tool's exit code, or
 * -1; the child's group is left in *PGID so that a cancel can end all of it. */
static atomic_int tool_pgid;
static int run_tool(char *const argv[], long limit_mb, void (*line)(const char *, void *), void *ctx)
{
    int pfd[2], code = -1; pid_t pid; char buf[1024], text[1024]; size_t have = 0;
    if (pipe(pfd)) return -1;
    if ((pid = fork()) < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {                         /* only async-signal-safe calls: we are a copy of a threaded process */
        int st = 0; pid_t c; char tail[32];
        close(pfd[0]); setpgid(0, 0); signal(SIGCHLD, SIG_DFL);
        if ((c = fork()) == 0) {
            int nul = open("/dev/null", O_RDONLY);
            dup2(nul, 0); dup2(pfd[1], 1); dup2(pfd[1], 2); close(pfd[1]);
            setpriority(PRIO_PROCESS, 0, 19);
            if (limit_mb > 0) { struct rlimit rl = { (rlim_t)limit_mb << 20, (rlim_t)limit_mb << 20 }; setrlimit(RLIMIT_AS, &rl); }   /* the trainer needs 42 MB; a runaway must not take the Echo's RAM */
            execv(argv[0], argv); _exit(127);
        }
        int r = c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) ? WEXITSTATUS(st) : 255;
        int n = 0; tail[n++] = '\n'; tail[n++] = '#'; tail[n++] = 'e'; tail[n++] = 'x'; tail[n++] = 'i'; tail[n++] = 't'; tail[n++] = ' ';
        if (r >= 100) tail[n++] = (char)('0' + r / 100);
        if (r >= 10) tail[n++] = (char)('0' + r / 10 % 10);
        tail[n++] = (char)('0' + r % 10); tail[n++] = '\n';
        if (write(pfd[1], tail, (size_t)n) < 0) _exit(1);
        _exit(0);
    }
    close(pfd[1]);
    atomic_store(&tool_pgid, (int)pid);
    for (;;) {
        ssize_t r = read(pfd[0], buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n') {
                text[have] = 0;
                if (!strncmp(text, "#exit ", 6)) code = atoi(text + 6); else if (have) line(text, ctx);
                have = 0;
            } else if (have < sizeof text - 1) text[have++] = buf[i];
        }
    }
    close(pfd[0]);
    atomic_store(&tool_pgid, 0);
    return code;
}

/* ----------------------------------------------------------------------------------------------------------- state */

static void set(int state, const char *message)
{
    pthread_mutex_lock(&lk);
    S.state = state; snprintf(S.message, sizeof S.message, "%s", message ? message : "");
    pthread_mutex_unlock(&lk);
}

static void finish(int state, const char *message, const char *error)
{
    pthread_mutex_lock(&lk);
    S.state = state; snprintf(S.message, sizeof S.message, "%s", message ? message : "");
    snprintf(S.error, sizeof S.error, "%s", error ? error : "");
    if (state == ST_DONE || state == ST_FAILED) S.progress = state == ST_DONE ? 1 : S.progress;
    pthread_mutex_unlock(&lk);
}

static int cancelled(void) { return atomic_load(&cancel_flag); }

static void msleep(int ms)
{
    for (int t = 0; t < ms && !cancelled(); t += 50) usleep(50000);
}

static void remove_tree(const char *dir)
{
    DIR *d = opendir(dir); struct dirent *e; char p[600]; struct stat st;
    if (d) {
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if (!lstat(p, &st) && S_ISDIR(st.st_mode)) remove_tree(p); else unlink(p);
        }
        closedir(d);
    }
    rmdir(dir);
}

static void jesc(char *o, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(o + n, cap - n, "\\u%04x", c);
        else o[n++] = (char)c;
    }
    o[n] = 0;
}

/* ------------------------------------------------------------------------------------------------------ recording */

static const char *const generic[] = { "Hey there", "What time is it?", "Set a timer", "Turn on the lights", "Stop", "This is ordinary conversation" };

/* The words the kind of recording asks for: the word itself, or (the others) its first and last parts, then everyday phrases */
static void prompt_text(const char *phrase, int label, int i, char *out, size_t cap)
{
    char parts[2][96] = { "", "" }; const char *sp = strchr(phrase, ' '); const char *last = strrchr(phrase, ' ');
    if (label) { snprintf(out, cap, "%s", phrase); return; }
    if (sp) { snprintf(parts[0], sizeof parts[0], "%s", sp + 1); snprintf(parts[1], sizeof parts[1], "%.*s", (int)(last - phrase), phrase); }
    const char *pool[8]; int n = 0;
    if (parts[0][0] && strcasecmp(parts[0], phrase)) pool[n++] = parts[0];
    if (parts[1][0] && strcasecmp(parts[1], phrase)) pool[n++] = parts[1];
    for (size_t k = 0; k < sizeof generic / sizeof *generic; k++) pool[n++] = generic[k];
    snprintf(out, cap, "%s", pool[i % n]);
}

static int clip_ok(const int16_t *pcm, size_t n, char *why, size_t cap)
{
    int peak = 0; size_t clipped = 0;
    for (size_t i = 0; i < n; i++) { int v = abs(pcm[i]); if (v > peak) peak = v; if (v >= 32760) clipped++; }
    if (peak < 100) { snprintf(why, cap, "That was nearly silent: speak up, a little closer to the Echo."); return 0; }
    if (clipped * 100 > n) { snprintf(why, cap, "That was too loud: a little farther from the Echo."); return 0; }
    return 1;
}

static int record_clips(char *dir)
{
    char p[400], why[160];
    size_t want = (size_t)S.seconds * RATE; int16_t *clip = malloc(want * sizeof *clip);
    if (!clip) { finish(ST_FAILED, "", "no memory"); return -1; }
    pthread_mutex_lock(&mic_lk); rec_buf = clip; rec_want = rec_n = 0; pthread_mutex_unlock(&mic_lk);
    atomic_store(&rec_prompts, 1);
    for (int i = 0; i < CLIPS && !cancelled(); ) {
        int label = i < PER_CLASS, k = label ? i : i - PER_CLASS, held = k >= PER_CLASS - HELD_OUT;
        pthread_mutex_lock(&lk);
        S.clip = i; S.label = label; S.held_out = held; S.phase = 0; S.phase_at = now_s(); S.state = ST_RECORDING;
        prompt_text(S.phrase, label, k, S.text, sizeof S.text);
        snprintf(S.message, sizeof S.message, "Get ready to say it");
        pthread_mutex_unlock(&lk);
        msleep(atoi(env_or("HASSMIC_ENROLL_READY_MS", "1800")));
        if (cancelled()) break;
        pthread_mutex_lock(&mic_lk); rec_want = want; rec_n = 0; pthread_mutex_unlock(&mic_lk);
        pthread_mutex_lock(&lk); S.phase = 1; S.phase_at = now_s(); snprintf(S.message, sizeof S.message, "Say it now"); pthread_mutex_unlock(&lk);
        atomic_store(&rec_on, 1);
        double t0 = now_s();
        while (!cancelled() && now_s() - t0 < S.seconds + 6) {          /* the mic's own clock paces it; the limit is a stalled mic */
            pthread_mutex_lock(&mic_lk); int full = rec_n >= rec_want; pthread_mutex_unlock(&mic_lk);
            if (full) break;
            usleep(20000);
        }
        atomic_store(&rec_on, 0);
        pthread_mutex_lock(&mic_lk); size_t got = rec_n; rec_want = 0; pthread_mutex_unlock(&mic_lk);      /* none copies after this */
        if (cancelled()) break;
        pthread_mutex_lock(&lk); S.phase = 2; S.phase_at = now_s(); pthread_mutex_unlock(&lk);
        if (got < want) { finish(ST_FAILED, "", "The microphone stopped delivering audio."); rec_release(); atomic_store(&rec_prompts, 0); return -1; }
        if (!clip_ok(clip, got, why, sizeof why)) {                   /* the same one again */
            pthread_mutex_lock(&lk); snprintf(S.message, sizeof S.message, "%s", why); pthread_mutex_unlock(&lk);
            msleep(2200); continue;
        }
        snprintf(p, sizeof p, "%s/clip-%02d.raw", dir, i);
        FILE *f = fopen(p, "wb"); int bad = !f;
        if (f) { bad = fwrite(clip, sizeof *clip, got, f) != got; if (fclose(f)) bad = 1; }      /* closed once, whatever the write did */
        if (bad) { finish(ST_FAILED, "", "Cannot save the recording (no space?)"); rec_release(); atomic_store(&rec_prompts, 0); return -1; }
        pthread_mutex_lock(&lk); snprintf(S.message, sizeof S.message, "Got it"); pthread_mutex_unlock(&lk);
        msleep(600);
        i++;
    }
    atomic_store(&rec_prompts, 0);
    rec_release();
    return cancelled() ? -1 : 0;
}

int enroll_recording(void) { return atomic_load(&rec_prompts); }

void enroll_feed(const int16_t *pcm, size_t n)
{
    if (!atomic_load(&rec_on)) return;
    pthread_mutex_lock(&mic_lk);
    if (rec_buf && rec_n < rec_want) {            /* (rec_want is 0 once the clip is taken) */
        size_t k = rec_want - rec_n < n ? rec_want - rec_n : n;
        memcpy(rec_buf + rec_n, pcm, k * sizeof *pcm); rec_n += k;
    }
    pthread_mutex_unlock(&mic_lk);
}

/* ------------------------------------------------------------------------------------------------------ the job */

static void job_progress(double p)
{
    pthread_mutex_lock(&lk); S.progress = p > 1 ? 1 : p; pthread_mutex_unlock(&lk);
}

struct tool_out { char last[200]; int step, qat; };

static void extract_line(const char *l, void *c) { struct tool_out *o = c; snprintf(o->last, sizeof o->last, "%s", l); }

static void train_line(const char *l, void *c)
{
    struct tool_out *o = c; const char *s; int step;
    snprintf(o->last, sizeof o->last, "%s", l);
    if ((s = strstr(l, "\"step\":")) && (step = atoi(s + 7)) >= 0) {
        double p = strstr(l, "\"qat\"") ? 0.2 + 0.8 * step / STEPS : 0.2 * (step > 1000 ? 1.0 : step / 1000.0);
        job_progress(0.45 + 0.5 * p);
    }
    if ((s = strstr(l, "\"phase\":\"complete\"")) && (s = strstr(l, "\"train\":"))) {
        int a[8];
        if (sscanf(s, "\"train\":{\"tp\":%d,\"fn\":%d,\"fp\":%d,\"tn\":%d},\"test\":{\"tp\":%d,\"fn\":%d,\"fp\":%d,\"tn\":%d}",
                   a, a + 1, a + 2, a + 3, a + 4, a + 5, a + 6, a + 7) == 8) {
            pthread_mutex_lock(&lk);
            memcpy(S.train, a, sizeof S.train); memcpy(S.test, a + 4, sizeof S.test); S.have_result = 1;
            pthread_mutex_unlock(&lk);
        }
    }
}

static int pad_clip(const char *clip, const char *out)
{
    static int16_t zeros[4096]; FILE *in = fopen(clip, "rb"), *o; int16_t buf[4096]; size_t n;
    if (!in) return -1;
    if (!(o = fopen(out, "wb"))) { fclose(in); return -1; }
    for (int k = 0; k < PAD; k += 4096) fwrite(zeros, sizeof *zeros, (size_t)(PAD - k < 4096 ? PAD - k : 4096), o);
    while ((n = fread(buf, sizeof *buf, 4096, in)) > 0) fwrite(buf, sizeof *buf, n, o);
    for (int k = 0; k < PAD; k += 4096) fwrite(zeros, sizeof *zeros, (size_t)(PAD - k < 4096 ? PAD - k : 4096), o);
    fclose(in);
    return fclose(o) ? -1 : 0;
}

/* A root restart can interrupt the waiting thread. Persist intent, then consume an exact root result before
 * changing the selection. checksum.txt identifies the complete set made by enroll_make_set, not just its name. */
static int good_id(const char *id);
static int install_result(void)
{
    char path[460], id[72], phrase[96], use[8], digest[40], actual[40] = "", line[300], ok[100], bad[100];
    snprintf(path, sizeof path, "%s/enroll-pending", state_dir());
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int valid = fgets(id, sizeof id, f) && fgets(phrase, sizeof phrase, f) && fgets(use, sizeof use, f) && fgets(digest, sizeof digest, f);
    fclose(f);
    if (!valid) return 0;
    id[strcspn(id, "\n")] = 0; phrase[strcspn(phrase, "\n")] = 0; digest[strcspn(digest, "\n")] = 0;
    if (!good_id(id) || strlen(digest) != 32 || (strcmp(use, "0\n") && strcmp(use, "1\n"))) return 0;
    snprintf(ok, sizeof ok, "OK wake:%s\n", id); snprintf(bad, sizeof bad, "FAILED wake:%s:", id);
    snprintf(path, sizeof path, "%s/artifacts/result", state_dir());
    f = fopen(path, "r");
    if (!f) return 0;
    int result = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strcmp(line, ok)) { result = 1; break; }
        if (!strncmp(line, bad, strlen(bad))) { result = -1; break; }
    }
    fclose(f);
    if (!result) return 0;
    if (result > 0) {
        snprintf(path, sizeof path, "%s/%s/checksum.txt", models_dir(), id);
        f = fopen(path, "r");
        if (f) { size_t n = fread(actual, 1, sizeof actual - 1, f); actual[n] = 0; fclose(f); }
        if (strcmp(actual, digest)) { finish(ST_UNRESOLVED, "", "The installed set does not match the requested version; the previous wake word is still selected."); return 1; }
        pthread_mutex_lock(&lk); S.installed = 1; pthread_mutex_unlock(&lk);
        if (atoi(use)) {
            snprintf(path, sizeof path, "%s/wake_word", state_dir());
            snprintf(line, sizeof line, "%s\n", id);
            if (statefile_write(path, 0666, line, strlen(line))) {
                finish(ST_UNRESOLVED, "", "The set was installed, but selecting it failed."); return 1;
            }
        }
        snprintf(path, sizeof path, "%s/enroll-last", state_dir());
        snprintf(line, sizeof line, "%s\n%s\n%d\n", id, phrase, atoi(use));
        if (statefile_write(path, 0644, line, strlen(line))) { finish(ST_UNRESOLVED, "", "The set was installed, but saving the result failed."); return 1; }
        pthread_mutex_lock(&lk); S.installed = 1; snprintf(S.id, sizeof S.id, "%s", id); snprintf(S.phrase, sizeof S.phrase, "%s", phrase); S.use_now = atoi(use); pthread_mutex_unlock(&lk);
        fprintf(stderr, "enroll: %s (%s) installed%s\n", id, phrase, atoi(use) ? ", the wake word" : "");
        finish(ST_DONE, atoi(use) ? "Done. Your wake word is installed and in use." : "Done. Your wake word is installed; pick it under Your wake words.", "");
    } else {
        line[strcspn(line, "\n")] = 0;
        char why[340]; snprintf(why, sizeof why, "The installer refused it: %s", line);
        finish(ST_FAILED, "", why);
    }
    snprintf(path, sizeof path, "%s/enroll-pending", state_dir()); unlink(path);
    return 1;
}

static int job(char *dir)
{
    char ext[300], trn[300], bdir[300], p[500], pad[400], feat[400], manifest[400], model[400], cand[400], f1[300], f2[300];
    char argv_store[6][420]; char *argv[6]; struct tool_out o = { "", 0, 0 };
    if (tool_path("HASSMIC_PRYON_EXTRACT", "pryon_extract", ext, sizeof ext) || tool_path("HASSMIC_PRYON_TRAIN", "pryon_train", trn, sizeof trn)) {
        finish(ST_FAILED, "", "This Echo does not have the training tools."); return -1;
    }
    base_dir(S.base, bdir, sizeof bdir);
    snprintf(model, sizeof model, "%s/%s", bdir, ENROLL_MODEL);
    snprintf(manifest, sizeof manifest, "%s/features.json", dir);
    snprintf(pad, sizeof pad, "%s/padded.raw", dir);
    pthread_mutex_lock(&lk); S.job_at = time(NULL); S.progress = 0; S.state = ST_EXTRACTING; pthread_mutex_unlock(&lk);

    FILE *m = fopen(manifest, "w");
    if (!m) { finish(ST_FAILED, "", "Cannot write in the Echo's state folder."); return -1; }
    fputs("[", m);
    for (int i = 0; i < CLIPS; i++) {
        int label = i < PER_CLASS, k = label ? i : i - PER_CLASS;
        fprintf(m, "%s{\"path\":\"features-%02d.bin\",\"label\":%d,\"split\":\"%s\"}", i ? "," : "", i, label, k >= PER_CLASS - HELD_OUT ? "test" : "train");
    }
    fputs("]\n", m); fclose(m);

    for (int i = 0; i < CLIPS; i++) {
        if (cancelled()) return -1;
        pthread_mutex_lock(&lk);
        snprintf(S.message, sizeof S.message, "Reading recording %d of %d with the Echo's own wake word engine", i + 1, CLIPS);
        pthread_mutex_unlock(&lk);
        snprintf(p, sizeof p, "%s/clip-%02d.raw", dir, i); snprintf(feat, sizeof feat, "%s/features-%02d.bin", dir, i);
        if (pad_clip(p, pad)) { finish(ST_FAILED, "", "Cannot prepare a recording."); return -1; }
        snprintf(argv_store[0], sizeof argv_store[0], "%s", ext); snprintf(argv_store[1], sizeof argv_store[1], "%s/pryon.manifest", bdir);
        snprintf(argv_store[2], sizeof argv_store[2], "%s", pad); snprintf(argv_store[3], sizeof argv_store[3], "%s", feat);
        for (int k = 0; k < 4; k++) argv[k] = argv_store[k];
        argv[4] = NULL;
        int rc = run_tool(argv, 0, extract_line, &o);
        unlink(pad); unlink(p);                                          /* the recording is not needed again */
        if (cancelled()) return -1;
        if (rc) {
            fprintf(stderr, "enroll: pryon_extract on recording %d: exit %d: %s\n", i, rc, o.last);
            finish(ST_FAILED, "", "The wake word engine could not read a recording. Try again; if it repeats, the log says why.");
            return -1;
        }
        job_progress(0.45 * (i + 1) / CLIPS);
    }

    pthread_mutex_lock(&lk); S.state = ST_TRAINING; snprintf(S.message, sizeof S.message, "Training your wake word"); pthread_mutex_unlock(&lk);
    snprintf(cand, sizeof cand, "%s/candidate.onnx", dir);
    snprintf(argv_store[0], sizeof argv_store[0], "%s", trn); snprintf(argv_store[1], sizeof argv_store[1], "%s", model);
    snprintf(argv_store[2], sizeof argv_store[2], "%s", manifest); snprintf(argv_store[3], sizeof argv_store[3], "%s", cand);
    snprintf(argv_store[4], sizeof argv_store[4], "%d", STEPS);
    for (int k = 0; k < 5; k++) argv[k] = argv_store[k];
    argv[5] = NULL;
    memset(&o, 0, sizeof o);
    int rc = run_tool(argv, 160, train_line, &o);
    if (cancelled()) return -1;
    pthread_mutex_lock(&lk); int have = S.have_result, *tr = S.train, *te = S.test; int bad = !have || tr[1] || tr[2] || te[1] || te[2]; int t4[4], e4[4];
    memcpy(t4, tr, sizeof t4); memcpy(e4, te, sizeof e4); pthread_mutex_unlock(&lk);
    if (rc || !have) {
        fprintf(stderr, "enroll: pryon_train: exit %d: %s\n", rc, o.last);
        finish(ST_FAILED, "", "Training failed. The log says why.");
        return -1;
    }
    if (bad) {
        char why[400];
        snprintf(why, sizeof why, "It did not pass its check, so nothing was installed: it heard %d of the %d held-back recordings of your word and wrongly woke to %d of the %d held-back others. "
                 "Try again with the word said a little differently each time, and the other phrases in your normal voice.",
                 e4[0], e4[0] + e4[1], e4[2], e4[2] + e4[3]);
        fprintf(stderr, "enroll: %s not installed: train %d/%d/%d/%d test %d/%d/%d/%d\n", S.key, t4[0], t4[1], t4[2], t4[3], e4[0], e4[1], e4[2], e4[3]);
        finish(ST_FAILED, "", why);
        return -1;
    }

    /* the set, staged and checked as any model set is, then installed (root restarts hassmic) */
    pthread_mutex_lock(&lk);
    if (cancelled()) { pthread_mutex_unlock(&lk); return -1; }
    S.state = ST_PACKAGING; snprintf(S.message, sizeof S.message, "Making the wake word set"); S.progress = 0.96; pthread_mutex_unlock(&lk);
    char artid[100], err[200] = "", note[200]; snprintf(artid, sizeof artid, "wake:%s", S.id);
    if (art_room(24L << 20, err, sizeof err)) { finish(ST_FAILED, "", err); return -1; }
    art_unstage(artid);
    if (art_stage_dir(artid, f1, sizeof f1)) { finish(ST_FAILED, "", "Cannot stage the wake word set."); return -1; }
    snprintf(note, sizeof note, "phrase=%s\nkeyword=%s\nheld-out=%d/%d,%d/%d\n", S.phrase, S.key, e4[0], e4[0] + e4[1], e4[3], e4[2] + e4[3]);
    snprintf(f2, sizeof f2, "%s", f1);
    { char *slash = strrchr(f2, '/'); if (slash) { *slash = 0; mkdir(f2, 0755); } }
    if (enroll_make_set(bdir, cand, f1, S.key, note, err, sizeof err)) { finish(ST_FAILED, "", err); return -1; }
    if (art_staged(artid, err, sizeof err) || art_commit(artid, err, sizeof err)) { art_unstage(artid); finish(ST_FAILED, "", err); return -1; }
    /* From here nothing cancels it (enroll_cancel only acts while recording, extracting and training): the install is root's. */
    set(ST_INSTALLING, "Installing; the Echo restarts its wake word engine in a moment");
    char pending[300], checksum[340], digest[33] = "", record[240];
    snprintf(checksum, sizeof checksum, "%s/checksum.txt", f1);
    FILE *cf = fopen(checksum, "r");
    if (cf) { size_t n = fread(digest, 1, 32, cf); digest[n] = 0; fclose(cf); }
    snprintf(pending, sizeof pending, "%s/enroll-pending", state_dir());
    snprintf(record, sizeof record, "%s\n%s\n%d\n%s\n", S.id, S.phrase, S.use_now, digest);
    if (strlen(digest) != 32 || statefile_write(pending, 0644, record, strlen(record))) {
        finish(ST_FAILED, "", "Cannot save the installation request."); return -1;
    }
    if (art_install(err, sizeof err)) { unlink(pending); finish(ST_FAILED, "", err); return -1; }
    int waited = atoi(env_or("HASSMIC_ENROLL_INSTALL_WAIT_S", "180"));
    if (waited < 1 || waited > 3600) waited = 180;
    for (int t = 0; t < waited * 5; t++) {
        if (install_result()) return 0;
        usleep(200000);
    }
    /* Keep the request: root may still complete it. Startup reconciles it before choosing the wake word. */
    finish(ST_UNRESOLVED, "", "Installation is unconfirmed; the previous wake word is still selected. Clear the pending request to retry, or restart to check the installer's result.");

    return 0;
}

static void *session(void *arg)
{
    char dir[300], cl[340];
    (void)arg;
    wdir(dir, sizeof dir);
    remove_tree(dir);
    if (mkdir(dir, 0700) && errno != EEXIST) { finish(ST_FAILED, "", "Cannot make the session's folder."); goto end; }
    if (record_clips(dir) == 0 && !cancelled()) job(dir);
    pthread_mutex_lock(&lk); int reached = S.state >= ST_DONE; pthread_mutex_unlock(&lk);
    if (cancelled() && !reached) finish(ST_CANCELLED, "Cancelled; nothing was changed.", "");
    snprintf(cl, sizeof cl, "%s", dir);
    remove_tree(cl);                                                    /* recordings, activations, the candidate: gone */
end:
    atomic_store(&thread_live, 0);
    return NULL;
}

/* ---------------------------------------------------------------------------------------------------------- API */

int enroll_start(const char *phrase, int seconds, int use_now, const char *base_manifest, char *err, size_t errsz)
{
    char key[40], id[72], bd[300], p[400], norm[96] = ""; size_t n = 0; struct statvfs sv; pthread_t th; struct stat st;
    if (!enroll_available(base_manifest)) { snprintf(err, errsz, "this Echo cannot train a wake word (the tools or the engine are missing)"); return -1; }
    if (seconds < 2 || seconds > 6) { snprintf(err, errsz, "each recording: 2 to 6 seconds"); return -1; }
    for (const char *c = phrase; *c && n < sizeof norm - 1; c++) {                          /* the phrase as shown: single spaces, no controls */
        if ((unsigned char)*c < 0x20) continue;
        if (*c == ' ' && (!n || norm[n - 1] == ' ')) continue;
        norm[n++] = *c;
    }
    while (n && norm[n - 1] == ' ') n--;
    norm[n] = 0;
    if (enroll_name(norm, "en-US", key, sizeof key, id, sizeof id, err, errsz)) return -1;
    base_dir(base_manifest, bd, sizeof bd);
    snprintf(p, sizeof p, "%s/%s", models_dir(), id);
    if (!stat(p, &st)) {                                                                    /* one of Amazon's, or one of ours to be replaced */
        char mark[460]; snprintf(mark, sizeof mark, "%s/%s", p, ENROLL_MARK);
        if (access(mark, R_OK)) { snprintf(err, errsz, "a wake word with that name is already on this Echo (Amazon's): pick another name"); return -1; }
    }
    snprintf(p, sizeof p, "%s", state_dir());
    if (!statvfs(p, &sv) && (long long)sv.f_bavail * (long long)sv.f_frsize < NEED_FREE) { snprintf(err, errsz, "not enough free space on this Echo (40 MB needed)"); return -1; }
    pthread_mutex_lock(&lk);
    if (atomic_load(&thread_live)) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "a wake word is being trained already"); return -1; }
    snprintf(p, sizeof p, "%s/enroll-pending", state_dir());
    if (!access(p, F_OK)) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "dismiss the previous installation result first, or restart to check its pending result"); return -1; }
    memset(&S, 0, sizeof S);
    snprintf(S.phrase, sizeof S.phrase, "%s", norm); snprintf(S.key, sizeof S.key, "%s", key); snprintf(S.id, sizeof S.id, "%s", id);
    snprintf(S.base, sizeof S.base, "%s", base_manifest); snprintf(S.lang, sizeof S.lang, "en-US");
    S.use_now = use_now; S.seconds = seconds; S.state = ST_RECORDING; snprintf(S.message, sizeof S.message, "Starting");
    atomic_store(&cancel_flag, 0); atomic_store(&thread_live, 1);
    pthread_mutex_unlock(&lk);
    if (pthread_create(&th, NULL, session, NULL)) { atomic_store(&thread_live, 0); snprintf(err, errsz, "cannot start"); pthread_mutex_lock(&lk); S.state = ST_IDLE; pthread_mutex_unlock(&lk); return -1; }
    pthread_detach(th);
    fprintf(stderr, "enroll: training \"%s\" (%s)\n", norm, id);
    return 0;
}

void enroll_cancel(void)
{
    pthread_mutex_lock(&lk);
    int live = S.state == ST_RECORDING || S.state == ST_EXTRACTING || S.state == ST_TRAINING;      /* not once the set is being made or installed */
    if (live) atomic_store(&cancel_flag, 1);
    pthread_mutex_unlock(&lk);
    int g = atomic_load(&tool_pgid);
    if (live && g > 0) kill(-g, SIGKILL);
}

int enroll_dismiss(char *err, size_t errsz)
{
    pthread_mutex_lock(&lk);
    if (atomic_load(&thread_live)) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "The session is still running."); return -1; }
    if (S.state >= ST_DONE) {
        char p[300]; snprintf(p, sizeof p, "%s/enroll-pending", state_dir());
        if (!access(p, F_OK)) {
            char id[72] = "", artid[80]; FILE *f = fopen(p, "r");
            if (f) { if (!fgets(id, sizeof id, f)) id[0] = 0; fclose(f); }
            id[strcspn(id, "\n")] = 0; snprintf(artid, sizeof artid, "wake:%s", id);
            if (art_discard_install(artid, err, errsz) || (unlink(p) && errno != ENOENT)) {
                if (!*err) snprintf(err, errsz, "Cannot clear the pending request.");
                pthread_mutex_unlock(&lk); return -1;
            }
        }
        memset(&S, 0, sizeof S);
    }
    pthread_mutex_unlock(&lk);
    return 0;
}

size_t enroll_json(char *out, size_t cap)
{
    char ph[220], tx[220], msg[420], er[900], id[160], ins[300] = "", lp[300]; size_t n = 0; double now = now_s();
#define J(...) do { if (n < cap) n += (size_t)snprintf(out + n, cap - n, __VA_ARGS__); } while (0)
    pthread_mutex_lock(&lk);
    jesc(ph, sizeof ph, S.phrase); jesc(tx, sizeof tx, S.text); jesc(msg, sizeof msg, S.message); jesc(er, sizeof er, S.error); jesc(id, sizeof id, S.id);
    J("{\"state\":\"%s\",\"phrase\":\"%s\",\"id\":\"%s\",\"message\":\"%s\",\"error\":\"%s\",\"use_now\":%d,\"seconds\":%d", state_name[S.state], ph, id, msg, er, S.use_now, S.seconds);
    if (S.state == ST_RECORDING) {
        double left = S.phase == 0 ? atoi(env_or("HASSMIC_ENROLL_READY_MS", "1800")) / 1000.0 - (now - S.phase_at) : S.phase == 1 ? S.seconds - (now - S.phase_at) : 0;
        J(",\"prompt\":{\"index\":%d,\"total\":%d,\"kind\":\"%s\",\"text\":\"%s\",\"phase\":\"%s\",\"left\":%.1f,\"held_out\":%d}",
          S.clip + 1, CLIPS, S.label ? "word" : "other", tx, S.phase == 0 ? "ready" : S.phase == 1 ? "speak" : "check", left > 0 ? left : 0, S.held_out);
    }
    if (S.state >= ST_EXTRACTING && S.state <= ST_INSTALLING) {
        long el = S.job_at ? (long)(time(NULL) - S.job_at) : 0; long eta = S.progress >= 0.03 ? (long)((double)el * (1 - S.progress) / S.progress) : -1;
        J(",\"progress\":%.3f,\"elapsed\":%ld,\"eta\":%ld", S.progress, el, eta);
    }
    if (S.have_result) J(",\"result\":{\"train\":[%d,%d,%d,%d],\"test\":[%d,%d,%d,%d]}", S.train[0], S.train[1], S.train[2], S.train[3], S.test[0], S.test[1], S.test[2], S.test[3]);
    pthread_mutex_unlock(&lk);
    snprintf(lp, sizeof lp, "%s/artifacts/result", state_dir());          /* root's word on the install, after the restart too */
    FILE *f = fopen(lp, "r"); if (f) { size_t k = fread(ins, 1, sizeof ins - 1, f); ins[k] = 0; fclose(f); }
    { char e[320]; jesc(e, sizeof e, ins); J(",\"install\":\"%s\"", e); }
    snprintf(lp, sizeof lp, "%s/enroll-last", state_dir());
    if ((f = fopen(lp, "r"))) {
        char a[100] = "", b[100] = "", c[8] = ""; if (fgets(a, sizeof a, f) && fgets(b, sizeof b, f)) { if (!fgets(c, sizeof c, f)) c[0] = 0; a[strcspn(a, "\n")] = 0; b[strcspn(b, "\n")] = 0;
            char ea[220], eb[220]; jesc(ea, sizeof ea, a); jesc(eb, sizeof eb, b); J(",\"last\":{\"id\":\"%s\",\"phrase\":\"%s\",\"in_use\":%d}", ea, eb, atoi(c)); }
        fclose(f);
    }
    J("}");
    return n < cap ? n : cap - 1;
#undef J
}

static int good_id(const char *id)
{
    size_t n = strlen(id);
    if (!n || n > 63 || !isalnum((unsigned char)id[0])) return 0;
    for (; *id; id++) if (!isalnum((unsigned char)*id) && *id != '.' && *id != '_' && *id != '-') return 0;
    return 1;
}

int enroll_ours(const char *id)
{
    char p[460]; struct stat st;
    if (!good_id(id)) return 0;
    snprintf(p, sizeof p, "%s/%s/%s", models_dir(), id, ENROLL_MARK);
    return !lstat(p, &st) && S_ISREG(st.st_mode);
}

int enroll_delete(const char *id, char *err, size_t errsz)
{
    char dir[300], p[340], line[100];
    if (!enroll_ours(id)) { snprintf(err, errsz, "that is not a wake word trained on this Echo"); return -1; }
    snprintf(dir, sizeof dir, "%s/artifacts", state_dir()); mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/delete", dir); snprintf(line, sizeof line, "wake:%s\n", id);
    if (statefile_write(p, 0666, line, strlen(line))) { snprintf(err, errsz, "cannot ask for it"); return -1; }
    fprintf(stderr, "enroll: removal of %s asked for\n", id);
    return 0;
}

void enroll_init(void)
{
    char dir[300]; wdir(dir, sizeof dir);
    remove_tree(dir);
    if (!install_result()) {
        snprintf(dir, sizeof dir, "%s/enroll-pending", state_dir());
        if (!access(dir, F_OK)) finish(ST_UNRESOLVED, "", "The previous installation is unconfirmed; the previous wake word is still selected. Clear the pending request to retry, or restart to check for a later installer result.");
    }
}
