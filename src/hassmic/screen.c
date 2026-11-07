/*
 * The Echo Show's screen UI (screen.h): src/ui/ built as libhmui.so, run by scripts/device/screen.dex in an
 * app_process of its own, hassmic's child:
 *
 *   CLASSPATH=<dir>/screen.dex HMUI_BAND=<band rows> app_process / Screen <dir>/libhmui.so      (<dir>: this binary's)
 *
 * A process of its own because the UI needs the framework (SurfaceFlinger, the activity manager, Skia for text and
 * images) and a Java VM to reach it, and because whatever it parses (artwork, app labels and icons) must not take the
 * satellite down with it.  Its protocol is src/ui/scene.h's: lines on its stdin (an image after "art <n>" or
 * "img <key> <n>"), lines back on its stdout.  It runs as hassmic's own user and groups: the line protocol keeps a UI
 * gone wrong from steering hassmic (every line checked, nothing it says trusted), but it is no privilege boundary: the
 * process can read what hassmic can (state/, the Music Assistant token too).
 *
 * Callers never wait for it: they change the state kept here (patterns by generation, so an "on" that came and went
 * between two writes is still sent, as on and off), and one thread writes what changed since the last write, the level at most 30
 * times a second.  The satellite's own state (mute, Do not disturb, setup, ringing timers, a call's peer, the light
 * sensor, the volume) is read once a second.  Music Assistant's API (mass.c) queues its lines here as they come
 * (screen_line); the UI's requests for it go straight to mass_ui.  A UI that ends is started again (at once after a
 * minute up, else 1 s, doubling to a minute), and gets everything anew; so does a new band height (the light_band
 * setting), which changes its layer.
 *
 * While a UI is meant to run (from the start, through its restarts, until it is given up) hassmic's own pid names
 * state/screen-ui.<pid>: root's kiosk_tick (main.sh) then leaves opening Kiosk Satellite to the UI (its home app,
 * scene.h), so that the two never fight over what is in front; nor does root open it over the face the user chose while
 * a UI starts again (a new band height, one that hung).  hassmic gone, its pid is too, and root takes over.
 *
 * HASSMIC_SCREEN=<program>: run that instead (tests), with HMUI_BAND set the same way.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                     /* pipe2 on glibc */
#endif
#include "screen.h"
#include "board.h"
#include "core.h"
#include "dropin.h"
#include "json.h"
#include "mass.h"
#include "ring.h"
#include "sendspin.h"
#include "settings.h"
#include "statefile.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NPAT     40
#define ART_MAX  (4 << 20)
#define LEVEL_MS 33
#define FIFO_MAX (8 << 20)               /* Music Assistant's lines and images not yet read by the UI */
/* Kiosk Satellite's package (device.conf KIOSK_PACKAGE, the same on every model that has it) and root's mark that it
 * is installed (main.sh kiosk_tick: the daemon's user cannot read packages.list) */
#define KIOSK_PKG "me.jxl.kiosk_satellite"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
/* told: the UI has it on; came: it came on since the last write; at: when it came on (mono_ms) */
static struct { char name[48]; int on, told, came; unsigned gen, sent; long long at; } pats[NPAT];
static int npats;
static atomic_int level_db = -1000;
static char tz[160]; static unsigned tz_gen, tz_sent;
static char title[256], artist[256], album[256]; static unsigned meta_gen, meta_sent;
static int mstate, mspeed = 1000; static long long mpos, mpos_at, mdur; static unsigned prog_gen, prog_sent;
static int cvol, cmuted, cshuffle = -1; static char cmds[256], crepeat[8]; static unsigned ctl_gen, ctl_sent;
static uint8_t *art; static size_t artn; static unsigned art_gen, art_sent;
static int music; static unsigned gone_gen, gone_sent;
static char home[128]; static int home_set, home_told; static unsigned home_gen, home_sent;   /* told: this UI has it */
static int started;
static atomic_uint ui_gen, ui_dead;
static atomic_int ui_width;
/* a UI that never stays up (8 starts in a row, each gone within 10 s of its "ready" or never up in READY_MS: some 3
 * to 6 minutes with the waits), or one that comes up and hangs, 3 times within 10 minutes of each other: given up until
 * hassmic starts again, so that the band's own renderer (ring_band.c) and root's kiosk_tick take over */
#define GIVE_UP 8
#define READY_MS 30000                  /* the UI's "ready" (a VM, the dex, libhmui, its layers: some 3 s on the Show);
                                           HASSMIC_SCREEN_READY_MS for the tests */
#define STAYED_MS 60000                 /* up this long, one that ends starts again at once (HASSMIC_SCREEN_STAYED_MS) */
#define WAIT_MS 1000                    /* the first wait before a start again, doubling to 60 of them (_WAIT_MS) */
static atomic_int gave_up;
static atomic_uint pong_n;                                      /* the last "pong <n>" of this UI (its reader) */
static uint8_t band_row[2048 * 4]; static int band_w; static unsigned band_gen, band_sent;     /* the UI instance; a reader that saw its end stores its number */
static struct { char *b; size_t n, cap; } fifo;                 /* Music Assistant's, in order */
/* the launchable apps as the UI listed them at its start: the settings page's choice of home app */
struct app { char pkg[96], label[64]; int sys; };
static struct app apps[64], apps_new[64]; static int napps, napps_new;

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

static void state_path(char *out, size_t n, const char *f)
{
    const char *d = getenv("HASSMIC_STATE");
    snprintf(out, n, "%s/%s", d ? d : "/data/local/hassmic/state", f);
}

static int load(const char *f, char *out, size_t n)              /* 1 if the file is there */
{
    char p[300]; state_path(p, sizeof p, f); out[0] = 0;
    FILE *fp = fopen(p, "r");
    if (!fp) return 0;
    if (fgets(out, (int)n, fp)) out[strcspn(out, "\r\n")] = 0;
    fclose(fp);
    return 1;
}

static void save(const char *f, const char *v)
{
    char p[300], b[400]; state_path(p, sizeof p, f);
    int n = snprintf(b, sizeof b, "%s\n", v);
    statefile_write(p, 0644, b, (size_t)n);
}

/* what the UI shows may not cross a line: tabs and newlines out of free text */
static void clean(char *out, size_t n, const char *in)          /* control characters (a line's end, a tab) as spaces */
{
    size_t k = 0;
    for (; in && *in && k + 1 < n; in++) out[k++] = (unsigned char)*in < 0x20 || *in == 0x7F ? ' ' : *in;
    out[k] = 0;
}

static int dirty;                                               /* (lock) something changed since the writer looked */
static void kick(void) { dirty = 1; pthread_cond_signal(&cv); }   /* (lock held) */

/* ---------------------------------------------------------------- screen.h */

/* a UI can run here (its files there, not given up): else no metadata and covers asked of Sendspin, no Music Assistant
 * session, no Screen page for nothing */
int screen_present(void) { return screen_ui(); }

void screen_led(const char *op, const char *pattern)
{
    if (!pattern) return;
    int on = !strcmp(op, "-s");
    pthread_mutex_lock(&lock);
    if (strncmp(pattern, "volume_step-", 12) && strncmp(pattern, "hassmic_assist_", 15)) {   /* the volume's steps (the keys', the
                                                                   Assistant's own): the band shows them, not the UI */
        int i = 0;
        while (i < npats && strcmp(pats[i].name, pattern)) i++;
        if (i == npats && npats < NPAT) snprintf(pats[npats++].name, sizeof pats[0].name, "%s", pattern);
        else if (i == npats) { static int told; if (!told++) fprintf(stderr, "screen: %d patterns kept, %s not for the UI\n", NPAT, pattern); }
        if (i < npats && (on || pats[i].on)) { pats[i].on = on; pats[i].gen++; if (on) { pats[i].came = 1; pats[i].at = mono_ms(); } }
    }
    kick();
    pthread_mutex_unlock(&lock);
}

void screen_level(int dbfs10) { atomic_store(&level_db, dbfs10); }

void screen_tz(const char *posix_tz)
{
    char t[sizeof tz]; clean(t, sizeof t, posix_tz);
    pthread_mutex_lock(&lock);
    if (t[0] && strcmp(t, tz)) { snprintf(tz, sizeof tz, "%s", t); tz_gen++; save("tz", tz); kick(); }
    pthread_mutex_unlock(&lock);
}

void screen_music_meta(const char *t, const char *a, const char *al)
{
    pthread_mutex_lock(&lock);
    clean(title, sizeof title, t); clean(artist, sizeof artist, a); clean(album, sizeof album, al);
    music = 1; meta_gen++; kick();
    pthread_mutex_unlock(&lock);
}

void screen_music_progress(int state, long long pos_ms, long long dur_ms, int speed)
{
    pthread_mutex_lock(&lock);
    mstate = state; mpos = pos_ms < 0 ? 0 : pos_ms; mpos_at = mono_ms(); mdur = dur_ms < 0 ? 0 : dur_ms; mspeed = speed;
    music = 1; prog_gen++; kick();
    pthread_mutex_unlock(&lock);
}

void screen_music_controller(int volume, int muted, const char *commands, const char *repeat, int shuffle)
{
    pthread_mutex_lock(&lock);
    cvol = volume; cmuted = muted; clean(cmds, sizeof cmds, commands);
    if (repeat) clean(crepeat, sizeof crepeat, repeat);
    if (shuffle >= 0) cshuffle = shuffle;
    music = 1; ctl_gen++; kick();
    pthread_mutex_unlock(&lock);
}

void screen_music_art(const void *image, size_t n)
{
    if (n > ART_MAX) n = 0;
    uint8_t *copy = n ? malloc(n) : NULL;
    if (n && !copy) return;
    if (n) memcpy(copy, image, n);
    pthread_mutex_lock(&lock);
    free(art); art = copy; artn = n; art_gen++; kick();
    pthread_mutex_unlock(&lock);
}

void screen_music_gone(void)
{
    pthread_mutex_lock(&lock);
    if (music || art) {                                         /* all of it: a UI started later tells the next one's */
        music = 0; title[0] = artist[0] = album[0] = cmds[0] = crepeat[0] = 0; free(art); art = NULL; artn = 0;   /* own */
        mstate = 0; mpos = mdur = 0; mspeed = 1000; cvol = cmuted = 0; cshuffle = -1;   /* (each sends what it has) */
        gone_gen++; art_gen++; kick();
    }
    pthread_mutex_unlock(&lock);
}

int screen_band(const void *argb, int w)
{
    if (atomic_load(&gave_up)) return -1;                        /* the band's own renderer, then */
    if (w <= 0 || w > 2048) return 0;
    pthread_mutex_lock(&lock);
    memcpy(band_row, argb, (size_t)w * 4); band_w = w; band_gen++; kick();
    pthread_mutex_unlock(&lock);
    return 0;
}

int screen_width(void) { return atomic_load(&ui_width); }

static int kiosk_installed(void)
{
    const char *k = getenv("HASSMIC_KIOSK_APP");
    return board.kiosk && !access(k ? k : "/data/local/hassmic/kiosk-app", F_OK);
}

/* the settings page's Screen page: the apps the UI found, the home app chosen (null: none, Kiosk Satellite or the face) */
size_t screen_status_json(char *o, size_t cap)
{
    size_t n = 0; char a[6 * 96 + 3], b[6 * 64 + 3];               /* (escaped: 6 a byte at most) */
    #define ADD(...) do { int k_ = snprintf(o + n, n < cap ? cap - n : 0, __VA_ARGS__); if (k_ > 0) n += (size_t)k_; } while (0)
    pthread_mutex_lock(&lock);
    ADD("{\"apps\":[");
    for (int i = 0, k = 0; i < napps; i++) {
        json_esc(a, sizeof a, apps[i].pkg); json_esc(b, sizeof b, apps[i].label);
        if (n + strlen(a) + strlen(b) + 60 + sizeof a > cap) break;   /* the rest left out, the answer whole */
        ADD("%s{\"pkg\":%s,\"label\":%s,\"system\":%s}", k++ ? "," : "", a, b, apps[i].sys ? "true" : "false");
    }
    json_esc(a, sizeof a, home);
    ADD("],\"home\":%s,\"kiosk\":\"%s\"}", home_set ? a : "null", kiosk_installed() ? KIOSK_PKG : "");
    pthread_mutex_unlock(&lock);
    #undef ADD
    return n < cap ? n : cap ? cap - 1 : 0;
}

/* an Android package name's characters (the page's and the UI's alike: neither is trusted) */
static int pkg_ok(const char *pkg)
{
    for (const char *c = pkg; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_')) return 0;
    return strlen(pkg) < sizeof home;
}

/* the home app from the page: a package, "" the face, NULL none chosen (Kiosk Satellite where installed); the UI
 * applies it at once */
int screen_set_home(const char *pkg)
{
    if (pkg && !pkg_ok(pkg)) return -1;
    pthread_mutex_lock(&lock);
    home_set = pkg != NULL; snprintf(home, sizeof home, "%s", pkg ? pkg : ""); home_gen++; kick();
    pthread_mutex_unlock(&lock);
    if (pkg) save("screen_home", pkg);
    else { char p[300]; state_path(p, sizeof p, "screen_home"); unlink(p); }
    fprintf(stderr, "screen: home is %s (the settings page)\n", !pkg ? "not chosen" : pkg[0] ? pkg : "the face");
    return 0;
}

static int fifo_put(const void *p, size_t n)                    /* lock held */
{
    if (fifo.n + n > FIFO_MAX) return -1;
    if (fifo.n + n > fifo.cap) {
        size_t c = (fifo.n + n) * 2; char *b = realloc(fifo.b, c);
        if (!b) return -1;
        fifo.b = b; fifo.cap = c;
    }
    memcpy(fifo.b + fifo.n, p, n); fifo.n += n;
    return 0;
}

static int ui_up;                                              /* a UI to read the FIFO (lock held): none, nothing queued */

void screen_line(const char *line)
{
    pthread_mutex_lock(&lock);
    size_t at = fifo.n;
    if (ui_up && (fifo_put(line, strlen(line)) || fifo_put("\n", 1))) { fifo.n = at; fprintf(stderr, "screen: the UI reads too slowly, a line dropped\n"); }
    kick();
    pthread_mutex_unlock(&lock);
}

void screen_bin(const char *line, const void *data, size_t n)
{
    pthread_mutex_lock(&lock);
    size_t at = fifo.n;
    if (ui_up && (fifo_put(line, strlen(line)) || fifo_put("\n", 1) || (n && fifo_put(data, n)))) { fifo.n = at; fprintf(stderr, "screen: the UI reads too slowly, an image dropped\n"); }
    kick();
    pthread_mutex_unlock(&lock);
}

/* ---------------------------------------------------------------- the UI process */

static const char *next_to_me(const char *file, char *buf, size_t n)
{
    ssize_t k = readlink("/proc/self/exe", buf, n - 32);
    if (k <= 0) { snprintf(buf, n, "/system/hassmic/%s", file); return buf; }
    buf[k] = 0;
    char *s = strrchr(buf, '/');
    snprintf(s ? s + 1 : buf, n - (size_t)(s ? s + 1 - buf : 0), "%s", file);   /* next to the binary, as bundles put it */
    return buf;
}

int screen_ui(void)
{
    static atomic_int have = -1;                                  /* looked once: an update restarts hassmic */
    if (atomic_load(&gave_up)) return 0;
    if (atomic_load(&have) < 0) {
        char dex[PATH_MAX], lib[PATH_MAX];
        atomic_store(&have, getenv("HASSMIC_SCREEN") ? 1 : !access(next_to_me("screen.dex", dex, sizeof dex), R_OK) &&
                                                     !access(next_to_me("libhmui.so", lib, sizeof lib), R_OK));
    }
    return atomic_load(&have);
}

/* The UI's pid for root's kiosk_tick (main.sh): in the name of an empty file, state/screen-ui.<pid>, which root reads
 * by listing the folder; it never opens a file of ours there (one swapped for a link or a fifo between its look and
 * its open could point root anywhere).  pid 0: none, every one there removed (an old one: not a UI of this start) */
static void marker(pid_t pid)
{
    char d[300]; state_path(d, sizeof d, ""); DIR *dir = opendir(d); struct dirent *e;
    while (dir && (e = readdir(dir))) if (!strncmp(e->d_name, "screen-ui", 9)) { char p[600]; snprintf(p, sizeof p, "%s%s", d, e->d_name); unlink(p); }
    if (dir) closedir(dir);
    if (pid > 0) { char f[40]; snprintf(f, sizeof f, "screen-ui.%d", (int)pid); char p[340]; state_path(p, sizeof p, f); statefile_write(p, 0644, "", 0); }
}

/* A direct child, long-lived (as ringbar.dex is): with SIGCHLD ignored a plain wait() anywhere in hassmic would block
 * on it.  None waits on the Show (no DHA HAL there, clock.c says why it matters); a model with a screen whose libraries
 * wait for their children would need it double-forked.
 *
 * As ring_band.c starts its renderer: everything the child needs made before the fork (a putenv after it allocates, and
 * malloc's lock may be another thread's at that moment), pipes close-on-exec from the start (a ledctrl forked meanwhile
 * must not hold their ends), every other descriptor closed in the child, execve */
static pid_t ui_start(int band, int *in, int *out)
{
    int pin[2], pout[2]; char dex[PATH_MAX], lib[PATH_MAX], cp[PATH_MAX + 16], bs[24];
    const char *fake = getenv("HASSMIC_SCREEN");
    next_to_me("screen.dex", dex, sizeof dex); next_to_me("libhmui.so", lib, sizeof lib);
    static int told;
    if (!fake && (access(dex, R_OK) || access(lib, R_OK))) { if (!told++) fprintf(stderr, "screen: %s or %s missing, no screen UI\n", dex, lib); return -1; }
    snprintf(cp, sizeof cp, "CLASSPATH=%s", dex); snprintf(bs, sizeof bs, "HMUI_BAND=%d", band);
    /* libhmui.so's folder on the library path: Android 7's class loader namespace (libnativeloader) lets System.load
     * open only /data, /mnt/expand and java.library.path, which app_process takes from LD_LIBRARY_PATH.  Installed in
     * /system/hassmic, without it: "dlopen failed: library ... is not accessible for the namespace" (cronos, 2026-11-04) */
    char lp[PATH_MAX + 20]; snprintf(lp, sizeof lp, "LD_LIBRARY_PATH=%s", lib);
    char *slash = strrchr(lp, '/'); if (slash && slash > lp + 16) *slash = '\0';
    /* ...and the child's real group made its effective one first, as main.c's child_ids: runas -r leaves hassmic real
     * group 3990 and effective system, the kernel makes an exec with the two apart AT_SECURE (cap_bprm_secureexec), and
     * bionic's linker then drops LD_LIBRARY_PATH: java.library.path empty, /system/hassmic refused all the same
     * (checkers, 2026-11-04: AT_SECURE 1 in the UI's auxv, ld_library_paths="").  Our copies in /data never needed it */
    gid_t eg = getegid();
    extern char **environ;
    size_t ne = 0; while (environ[ne]) ne++;
    char **envp = malloc((ne + 4) * sizeof *envp);
    if (!envp) return -1;
    size_t m = 0; envp[m++] = cp; envp[m++] = bs; envp[m++] = lp;
    for (size_t i = 0; i < ne; i++) if (strncmp(environ[i], "CLASSPATH=", 10) && strncmp(environ[i], "HMUI_BAND=", 10) &&
                                        strncmp(environ[i], "LD_LIBRARY_PATH=", 16)) envp[m++] = environ[i];
    envp[m] = NULL;
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
    char *argv_[6] = { NULL }; const char *prog;
    if (fake) { prog = fake; argv_[0] = (char *)fake; }
    else { prog = "/system/bin/app_process"; argv_[0] = "app_process"; argv_[1] = "/"; argv_[2] = "Screen"; argv_[3] = lib; }
    if (pipe2(pin, O_CLOEXEC)) { free(envp); return -1; }
    if (pipe2(pout, O_CLOEXEC)) { close(pin[0]); close(pin[1]); free(envp); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        int i = fcntl(pin[0], F_DUPFD, 3), o = fcntl(pout[1], F_DUPFD, 3);   /* as ring_band.c: never a dup2 onto itself */
        if (i < 0 || o < 0 || dup2(i, 0) < 0 || dup2(o, 1) < 0) _exit(127);
        for (long d = 3; d < maxfd; d++) close((int)d);          /* none of hassmic's sockets in the VM; stderr: the log */
        setregid(eg, eg);                                         /* (a failure: the UI from /data still loads) */
        execve(prog, argv_, envp);
        _exit(127);
    }
    free(envp);
    close(pin[0]); close(pout[1]);
    if (pid < 0) { close(pin[1]); close(pout[0]); return -1; }
    *in = pin[1]; *out = pout[0];
    fcntl(*in, F_SETFL, fcntl(*in, F_GETFL) | O_NONBLOCK);        /* (write_all's deadline) */
    return pid;
}

/* the UI's commands, done by threads of their own: those to a server wait on its socket (a Sendspin server's, Music
 * Assistant's: 5 s a send), and the reader must go on reading the UI's pongs meanwhile, or a UI that is fine would be
 * taken for hung; the Echo's own (a timer's Stop, Drop In, the volume) in a queue of their own, never behind those */
#define CMDQ 64
static struct cmdq { char l[CMDQ][1024]; int head, n; pthread_cond_t cv; } cq[2] = {   /* (lock) 0: the Echo's, 1: servers' */
    { .cv = PTHREAD_COND_INITIALIZER }, { .cv = PTHREAD_COND_INITIALIZER } };

static void command(char *l)
{
    if (!strncmp(l, "cmd ", 4)) {
        char cmd[32] = ""; long long arg = -1;
        if (sscanf(l + 4, "%31s %lld", cmd, &arg) >= 1 && sendspin_control(cmd, arg)) fprintf(stderr, "screen: %s refused (no Music Assistant controller)\n", cmd);
    } else if (!strncmp(l, "vol ", 4) || !strncmp(l, "dnd ", 4)) {      /* as the page sets them: Home Assistant told too */
        char v[16], e[160]; snprintf(v, sizeof v, "%d", atoi(l + 4));
        pthread_mutex_lock(&core_lock);
        if (settings_set(l[0] == 'v' ? "volume" : "do_not_disturb", v, e, sizeof e)) fprintf(stderr, "screen: %s\n", e);
        pthread_mutex_unlock(&core_lock);
    } else if (!strcmp(l, "dropin answer")) {
        dropin_answer();                                          /* a ringing call only: never ends one answered meanwhile */
    } else if (!strcmp(l, "dropin hangup")) dropin_hangup("the screen");
    else if (!strcmp(l, "timer stop")) { pthread_mutex_lock(&core_lock); core_alarm_stop("the screen"); pthread_mutex_unlock(&core_lock); }
    else if (!strncmp(l, "ml ", 3) || !strncmp(l, "mplay ", 6) || !strncmp(l, "mq ", 3) || !strncmp(l, "mg ", 3) || !strncmp(l, "mv ", 3) ||
             !strncmp(l, "mgv ", 4) || !strncmp(l, "mhere ", 6) || !strncmp(l, "img ", 4)) mass_ui(l);
}

static void *commands(void *arg)
{
    struct cmdq *q = arg; char l[sizeof q->l[0]];
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!q->n) pthread_cond_wait(&q->cv, &lock);
        memcpy(l, q->l[q->head], sizeof l); q->head = (q->head + 1) % CMDQ; q->n--;
        pthread_mutex_unlock(&lock);
        command(l);
    }
    return NULL;
}

/* what the UI says: one line each (scene.h) */
static void on_line(char *l)
{
    if (!strncmp(l, "log ", 4)) fprintf(stderr, "screen: %s\n", l + 4);
    else if (!strncmp(l, "app ", 4)) {                          /* <pkg>\t<label>\t<system> */
        char *t1 = strchr(l + 4, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t2 && napps_new < (int)(sizeof apps_new / sizeof apps_new[0])) {
            struct app *a = &apps_new[napps_new]; char lb[sizeof a->label];
            snprintf(a->pkg, sizeof a->pkg, "%.*s", (int)(t1 - l - 4), l + 4);
            snprintf(lb, sizeof lb, "%.*s", (int)(t2 - t1 - 1), t1 + 1); clean(a->label, sizeof a->label, lb);
            a->sys = atoi(t2 + 1);
            if (pkg_ok(a->pkg) && a->pkg[0]) napps_new++;
        }
    } else if (!strcmp(l, "apps.")) {
        pthread_mutex_lock(&lock); memcpy(apps, apps_new, sizeof apps); napps = napps_new; pthread_mutex_unlock(&lock);
        napps_new = 0;
    } else {
        struct cmdq *q = &cq[l[0] == 'm' || l[0] == 'i' || !strncmp(l, "cmd ", 4)];   /* (ml, mplay, mq, mg, mv, mgv, mhere,
                                                                   img: Music Assistant's; cmd: Sendspin's) */
        pthread_mutex_lock(&lock);
        int full = q->n == CMDQ;
        if (!full) { snprintf(q->l[(q->head + q->n) % CMDQ], sizeof q->l[0], "%s", l); q->n++; pthread_cond_signal(&q->cv); }
        pthread_mutex_unlock(&lock);
        if (full) fprintf(stderr, "screen: %.40s dropped (%d commands waiting)\n", l, CMDQ);
    }
}

struct rd { int fd; unsigned gen; };                            /* a reader: its UI's stdout and generation */

static void *reader(void *arg)
{
    struct rd me = *(struct rd *)arg; free(arg);
    int fd = me.fd; unsigned gen = me.gen; char buf[1024]; size_t have = 0; ssize_t r; int skip = 0;
    while ((r = read(fd, buf + have, sizeof buf - 1 - have)) > 0) {
        have += (size_t)r; buf[have] = 0;
        char *s = buf, *nl;
        /* a UI let go of (gone, hung, a new band height: the next one may run already) says nothing more */
        while ((nl = strchr(s, '\n'))) {
            *nl = 0;
            if (!skip && (!strncmp(s, "ready ", 6) || !strncmp(s, "pong ", 5))) {   /* (looked at and kept under the lock */
                pthread_mutex_lock(&lock);                         /* ui_gone lets go of it with: never a stopped one's */
                int mine = gen == atomic_load(&ui_gen);            /* for the next) */
                if (mine && s[0] == 'r') atomic_store(&ui_width, atoi(s + 6));
                if (mine && s[0] == 'p') atomic_store(&pong_n, (unsigned)strtoul(s + 5, NULL, 10));
                pthread_mutex_unlock(&lock);
                if (mine && s[0] == 'r') fprintf(stderr, "screen: up, %s\n", s + 6);
            } else if (!skip && gen == atomic_load(&ui_gen)) on_line(s);
            skip = 0; s = nl + 1;
        }
        have = strlen(s); memmove(buf, s, have);
        if (have == sizeof buf - 1) { have = 0; skip = 1; }      /* a line too long: dropped, to its end */
    }
    close(fd);
    if (gen == atomic_load(&ui_gen)) atomic_store(&ui_dead, gen);   /* its end: the writer starts it again now (one let
                                                                   go of says nothing: its end late, the next one's lost) */
    pthread_mutex_lock(&lock); kick(); pthread_mutex_unlock(&lock);
    return NULL;
}

/* ---------------------------------------------------------------- the writer */

struct out { char *b; size_t n, cap; };

/* 0, or -1 nothing appended (no memory: a line and the bytes it announces go together or not at all, the callers') */
static int put(struct out *o, const char *fmt, ...)
{
    va_list ap; char line[1024];
    va_start(ap, fmt); int n = vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof line) n = sizeof line - 1;
    if (o->n + (size_t)n + 1 > o->cap) {
        size_t c = (o->n + (size_t)n + 1) * 2; char *b = realloc(o->b, c);
        if (!b) return -1;
        o->b = b; o->cap = c;
    }
    memcpy(o->b + o->n, line, (size_t)n); o->n += (size_t)n; o->b[o->n++] = '\n';
    return 0;
}

static int put_bin(struct out *o, const void *p, size_t n)     /* bytes right after the line that announces them */
{
    if (o->n + n > o->cap) {
        size_t c = (o->n + n) * 2; char *b = realloc(o->b, c);
        if (!b) return -1;
        o->b = b; o->cap = c;
    }
    memcpy(o->b + o->n, p, n); o->n += n;
    return 0;
}

/* -1 the UI is gone, -2 it took nothing for wait_ms (alive and hung: it is killed, else the screen and the band would
 * stay as they are for good); the pipe is non-blocking */
#define HUNG_MS 10000
static int write_all(int fd, const void *p, size_t n, long long wait_ms)
{
    const uint8_t *b = p; long long since = mono_ms();
    while (n) {
        ssize_t r = write(fd, b, n);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == EAGAIN) {
            long long left = since + wait_ms - mono_ms();
            if (left <= 0) return -2;
            struct pollfd q = { fd, POLLOUT, 0 };
            if (poll(&q, 1, (int)left) < 0 && errno != EINTR) return -1;
            continue;
        }
        if (r <= 0) return -1;
        b += r; n -= (size_t)r; since = mono_ms();
    }
    return 0;
}

/* the settings page's address: the Wi-Fi's IPv4 one */
static void web_url(char *out, size_t n)
{
    struct ifaddrs *ifa, *i; out[0] = 0;
    if (!core_web_port || getifaddrs(&ifa)) return;
    for (i = ifa; i; i = i->ifa_next) {
        char a[INET_ADDRSTRLEN];
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        if (!inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof a)) continue;
        snprintf(out, n, "http://%s:%d", a, core_web_port);
        break;
    }
    freeifaddrs(ifa);
}

static struct { int mute, dnd, oobe, evol, kiosk; char timers[300], peer[64], web[96]; } now_s, sent_s;

static void read_core(int slow)
{
    char names[300] = "", peer[64] = "";
    pthread_mutex_lock(&core_lock);
    now_s.mute = core_muted(); now_s.dnd = core_dnd(-1) > 0; now_s.evol = core_volume();
    if (core_timers_ringing(names, sizeof names) <= 0) names[0] = 0;
    pthread_mutex_unlock(&core_lock);
    now_s.oobe = core_oobe();
    clean(now_s.timers, sizeof now_s.timers, names);
    if (!dropin_running() || dropin_status(peer, sizeof peer) == DROPIN_IDLE) peer[0] = 0;   /* a call over: no peer */
    clean(now_s.peer, sizeof now_s.peer, peer);
    if (slow) {                                                   /* every 10 s: what changes rarely */
        now_s.kiosk = kiosk_installed();
        web_url(now_s.web, sizeof now_s.web);
    }
}

/* the UI gone, or let go of (a new band height, hung): nothing more queued for it, its pid no more root's sign; one
 * still running gets 2 s to go by itself (the end of its input), then is killed */
static void ui_gone(int *in, pid_t pid)
{
    if (*in >= 0) close(*in);
    *in = -1;
    pthread_mutex_lock(&lock);
    atomic_fetch_add(&ui_gen, 1); atomic_store(&ui_width, 0);    /* its reader's lines no more: a "ready" from a UI */
    ui_up = 0; fifo.n = 0;                                       /* stopped as it came up would be the next one's; */
    cq[1].n = 0;                                                 /* nor its requests (its lists' numbers: the next one's) */
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < 20 && pid > 0 && !kill(pid, 0); i++) usleep(100000);   /* (SIGCHLD ignored: gone is gone) */
    if (pid > 0 && !kill(pid, 0)) kill(pid, SIGKILL);
}

static void *writer(void *arg)
{
    (void)arg;
    int in = -1, out = -1, band = -1, fails = 0, lv_sent = -1, replay = 0; long long up_at = 0, next_core = 0, next_slow = 0, lv_at = 0;
    int again = 0;                                              /* the wait before a start: doubles until one ran a minute */
    pid_t pid = -1; long long ready_at = 0;                     /* when this UI said "ready" (0: not yet) */
    unsigned ping_n = 0; long long ping_at = 0;                 /* the last "ping" and when (answered: pong_n == ping_n) */
    int hungs = 0;                                              /* UIs that came up and hung later, since one ran 10 min */
    const char *rm = getenv("HASSMIC_SCREEN_READY_MS"), *hm = getenv("HASSMIC_SCREEN_HUNG_MS"), *sm = getenv("HASSMIC_SCREEN_STAYED_MS"),
               *wm = getenv("HASSMIC_SCREEN_WAIT_MS");      /* (the tests') */
    long long ready_ms = rm ? atoll(rm) : READY_MS, hung_ms = hm ? atoll(hm) : HUNG_MS, stayed_ms = sm ? atoll(sm) : STAYED_MS,
              wait0 = wm ? atoll(wm) : WAIT_MS;
    struct out o = { 0 };
    for (;;) {
        int want = ring_band_rows(-1); if (want < 0) want = 0;
        if (in >= 0 && want != band) {
            fprintf(stderr, "screen: the band is %d rows now, the UI starts again\n", want);
            ui_gone(&in, pid);
            if (fails) fails--;                                   /* (let go of, not failed: its start counts for nothing) */
            if (again) again--;
        }
        if (in < 0) {
            if (ready_at && mono_ms() - ready_at > stayed_ms) again = 0;
            ready_at = 0;                                         /* (a start that fails next: no UI up meanwhile) */
            if (fails >= GIVE_UP || hungs >= 3) {
                fprintf(stderr, "screen: the UI never stays up: given up until hassmic starts again (the band draws itself)\n");
                atomic_store(&gave_up, 1);
                ui_gone(&in, -1);
                marker(0);                                        /* (root's kiosk_tick opens the app again) */
                mass_stop();                                      /* (no UI for its lines; the page shows none either) */
                for (;;) pause();
            }
            if (again) {
                long long d = wait0 * (again > 6 ? 60 : 1 << (again - 1)); struct timespec ts = { d / 1000, d % 1000 * 1000000 }; nanosleep(&ts, NULL);
                want = ring_band_rows(-1); if (want < 0) want = 0;   /* (a new height meanwhile: that one's UI) */
            }
            if ((pid = ui_start(want, &in, &out)) < 0) { fails++; again++; continue; }
            ready_at = 0; napps_new = 0;                         /* (a list a UI that died left half done: not this one's) */
            ping_n = 0; ping_at = 0; atomic_store(&pong_n, 0);
            pthread_t t; struct rd *rd = malloc(sizeof *rd);
            unsigned g = atomic_fetch_add(&ui_gen, 1) + 1;
            if (rd) { rd->fd = out; rd->gen = g; }
            if (!rd || pthread_create(&t, NULL, reader, rd)) { close(out); free(rd); } else pthread_detach(t);
            band = want; up_at = mono_ms(); fails++; again++;
            pthread_mutex_lock(&lock);                          /* all of it again */
            for (int i = 0; i < npats; i++) { pats[i].told = pats[i].came = 0; pats[i].sent = pats[i].on ? pats[i].gen - 1 : pats[i].gen; }
            tz_sent = meta_sent = prog_sent = ctl_sent = art_sent = gone_sent = band_sent = 0; home_told = 0;
            if (!music) meta_sent = meta_gen, prog_sent = prog_gen, ctl_sent = ctl_gen;
            if (!music && !artn) art_sent = art_gen;              /* (a cover before any metadata: told too) */
            fifo.n = 0; ui_up = 1;                                /* answers to the last UI's requests: not this one's */
            pthread_mutex_unlock(&lock);
            memset(&sent_s, 0xFF, sizeof sent_s); sent_s.timers[0] = sent_s.peer[0] = sent_s.web[0] = 1;
            lv_sent = -1; next_slow = 0;
            o.n = 0;
            put(&o, "name %s", core_name);
            put(&o, "act %s", board.action_combo ? "both volume keys" : "the action button");
            replay = 1;
        }
        if (atomic_load(&ui_dead) == atomic_load(&ui_gen)) {
            fprintf(stderr, "screen: the UI ended, starting it again\n"); atomic_store(&ui_dead, 0);
            ui_gone(&in, pid);                                    /* (its output closed, the process may live on) */
            continue;
        }
        long long now = mono_ms();
        if (now >= next_core) { read_core(now >= next_slow); if (now >= next_slow) next_slow = now + 10000; next_core = now + 1000; }
        /* what the UI asks of hassmic's state: before the home app's choice, which goes by Kiosk Satellite */
        if (now_s.kiosk != sent_s.kiosk) put(&o, "kiosk %s", now_s.kiosk ? KIOSK_PKG : "");
        if (strcmp(now_s.web, sent_s.web)) put(&o, "web %s", now_s.web);
        uint8_t *img = NULL; size_t imgn = 0; int send_img = 0;
        pthread_mutex_lock(&lock);
        dirty = 0;                                               /* (what changes from here on: looked at again at once) */
        if (home_sent != home_gen || !home_told) { put(&o, "home %s", home_set ? home : "-"); home_sent = home_gen; home_told = 1; }
        if (tz_sent != tz_gen) { if (tz[0]) put(&o, "tz %s", tz); tz_sent = tz_gen; }
        /* with how long ago it came on: a new UI ends the timed ones (an error's flash, which nothing unsets) by
         * themselves; one that came and went since the last write: on, then off */
        for (int i = 0; i < npats; i++) {
            if (pats[i].sent == pats[i].gen) continue;
            if (pats[i].on || (pats[i].came && !pats[i].told)) put(&o, "p+ %s %lld", pats[i].name, now - pats[i].at);
            if (!pats[i].on && (pats[i].told || pats[i].came)) put(&o, "p- %s", pats[i].name);
            pats[i].told = pats[i].on; pats[i].came = 0; pats[i].sent = pats[i].gen;
        }
        if (gone_sent != gone_gen) { if (!music) put(&o, "mx"); gone_sent = gone_gen; }
        if (music && meta_sent != meta_gen) { put(&o, "mt %s\t%s\t%s", title, artist, album); meta_sent = meta_gen; }
        if (music && ctl_sent != ctl_gen) {
            put(&o, "mc %d %d %s %s %d", cvol, cmuted, cmds[0] ? cmds : "-", crepeat[0] ? crepeat : "off", cshuffle > 0);
            ctl_sent = ctl_gen;
        }
        if (music && prog_sent != prog_gen) {
            long long p = mpos + (mstate == 1 ? (now - mpos_at) * mspeed / 1000 : 0);
            put(&o, "mp %d %lld %lld %d", mstate, p, mdur, mspeed); prog_sent = prog_gen;
        }
        if (art_sent != art_gen) {
            if (artn && (img = malloc(artn))) { memcpy(img, art, artn); imgn = artn; }
            send_img = 1; art_sent = art_gen;
        }
        if (band_sent != band_gen && band_w) {
            size_t mk = o.n; if (put(&o, "band %d", band_w) || put_bin(&o, band_row, (size_t)band_w * 4)) o.n = mk; else band_sent = band_gen;
        }
        pthread_mutex_unlock(&lock);
        if (now_s.mute != sent_s.mute) put(&o, "mute %d", now_s.mute);
        if (now_s.dnd != sent_s.dnd) put(&o, "dnd %d", now_s.dnd);
        if (now_s.oobe != sent_s.oobe) put(&o, "oobe %d", now_s.oobe);
        if (now_s.evol != sent_s.evol) put(&o, "ev %d", now_s.evol);
        if (strcmp(now_s.timers, sent_s.timers)) put(&o, "timers %s", now_s.timers);
        if (strcmp(now_s.peer, sent_s.peer)) put(&o, "peer %s", now_s.peer);
        sent_s = now_s;
        /* the level: in percent of -50..-20 dBFS (the band's scale), while it changes, 30 times a second at most */
        int db = atomic_load(&level_db), lv = (int)lroundf(fmaxf(0, fminf(100, (db / 10.0f + 50) / 30 * 100)));
        if (lv != lv_sent && now - lv_at >= LEVEL_MS) { put(&o, "lv %d", lv); lv_sent = lv; lv_at = now; }
        if (send_img) { size_t mk = o.n; if (put(&o, "art %zu", imgn) || (imgn && put_bin(&o, img, imgn))) o.n = mk; }   /* (both or none) */
        /* alive: a ping every 3 s once up, answered by its main loop (an idle Echo writes nothing else, and a UI stuck in
         * a call would never fill the pipe) */
        if (ready_at && atomic_load(&pong_n) == ping_n && now - ping_at >= 3000) { if (!put(&o, "ping %u", ping_n + 1)) { ping_n++; ping_at = now; } }
        /* Music Assistant: the state again for a new UI (not under the lock: mass.c queues it through screen_line) */
        if (replay) { replay = 0; mass_replay(); }
        pthread_mutex_lock(&lock);
        if (fifo.n) { put_bin(&o, fifo.b, fifo.n); fifo.n = 0; }
        if (fifo.cap > (1u << 20)) { free(fifo.b); fifo.b = NULL; fifo.cap = 0; }
        pthread_mutex_unlock(&lock);
        /* a UI still starting reads nothing before its "ready" (android.c): the cover can fill the pipe meanwhile, which
         * is no hang until its time to come up is over */
        long long give = ready_at ? hung_ms : up_at + ready_ms - now > hung_ms ? up_at + ready_ms - now : hung_ms;
        int bad = o.n ? write_all(in, o.b, o.n, give) : 0;
        free(img); o.n = 0;
        if (o.cap > (1u << 20)) { free(o.b); o.b = NULL; o.cap = 0; }   /* a burst's (a cover, a long list): not kept */
        if (!bad && ready_at && atomic_load(&pong_n) != ping_n && now - ping_at > hung_ms) bad = -2;   /* no answer: hung all the same */
        if (bad == -2 && pid > 0) {
            fprintf(stderr, "screen: the UI takes nothing (hung): stopped\n"); kill(pid, SIGKILL);
            if (ready_at) hungs++;                                /* (one never up counts in fails) */
        }
        if (bad) {
            fprintf(stderr, "screen: the UI is gone, starting it again\n");
            ui_gone(&in, pid);
            continue;
        }
        if (!ready_at && atomic_load(&ui_width)) ready_at = now;
        if (!ready_at && now - up_at > ready_ms) {                  /* never came up: as one that died */
            fprintf(stderr, "screen: the UI did not come up in %lld ms: stopped\n", ready_ms);
            ui_gone(&in, pid);
            continue;
        }
        if (fails && ready_at && now - ready_at > 10000) fails = 0;   /* it came up and stayed up */
        if (hungs && ready_at && now - ready_at > 600000) hungs = 0;
        pthread_mutex_lock(&lock);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        long wait_ms = lv != lv_sent || now - lv_at < 1000 ? LEVEL_MS : 250;   /* (a level that moves: looked at 30 times a
                                                                   second, the band's frames or not; the core's state is read every second anyway) */
        ts.tv_nsec += wait_ms * 1000000L; ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L;
        if (!fifo.n && !dirty) pthread_cond_timedwait(&cv, &lock, &ts);   /* (a change while it wrote: no wait for it) */
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

void screen_start(void)
{
    pthread_t t;
    pthread_mutex_lock(&lock);
    int go = !started; started = 1;
    home_set = load("screen_home", home, sizeof home); load("tz", tz, sizeof tz);
    if (tz[0]) tz_gen++;
    pthread_mutex_unlock(&lock);
    if (!go) return;
    marker(screen_ui() ? getpid() : 0);                         /* (one of the last run's goes: its pid is no more) */
    if (screen_ui()) mass_start();
    for (int i = 0; i < 2; i++) if (!pthread_create(&t, NULL, commands, &cq[i])) pthread_detach(t);
    if (!pthread_create(&t, NULL, writer, NULL)) pthread_detach(t);
}
