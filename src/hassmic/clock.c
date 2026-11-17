/*
 * The Echo's clock, and when each log line was written.
 *
 * Clock: nothing sets it any more.  Stock synced with Amazon's time servers, which the egress lock keeps out, and the
 * Echo runs on from its RTC: the Dot 2 was 23 h behind on 2026-10-06.  Home Assistant knows the time and says it when a
 * device asks (ESPHome GetTimeRequest, answered by aioesphomeapi itself), so proto_esphome.c asks on every link and
 * every 6 h on it (a link without the device key only for "Next timer"'s end).  hassmic may not set the clock (no
 * CAP_SYS_TIME as the daemon's user), so the answer goes to root the way update and adb requests do: state/clock holds
 * "<epoch> <CLOCK_BOOTTIME when it was right>", the latter as /proc/uptime words it (seconds.centiseconds: the same clock, and main.sh's mksh only counts to 2^31, which
 * milliseconds pass after 25 days).  main.sh's netwatch (every 10 s) adds what has passed since, sets the time and the RTC,
 * and sets the property hassmic.clock.synced, which a reboot clears.  Only Home Assistant over the keyed link counts:
 * anyone on the network could otherwise move the clock, and with it the validity of the certificates curl checks.
 *
 * Time zone: Home Assistant's comes with its time, as the rule of its zone (tz.c turns it into a zone name).  It goes to
 * root the same way (state/timezone, one line), and main.sh sets persist.sys.timezone, plus Android's alarm service
 * where there is one (it tells the Java side).  The logs stay in UTC whatever the zone.
 * Which of the zones with that rule is the Wi-Fi's country's (apcountry.c, tz.c); what came of it, and how, stays in
 * state/zone, hassmic's own ("<zone> country|kept|guess|rule"; root takes state/timezone away at once, issue #176's
 * review: the language never saw it on an Echo).  The language spoken here (main.c speech_locale, the Echo Show's
 * date) takes the zone only when it is known: the country's, or one set before and kept, never a guess (CET without a
 * country is Europe/Amsterdam: a Dutch date in Germany), nor hassmic's own guess kept by root since.  The country is
 * read again with each answer of Home Assistant's (every link, every 6 h; a new network's new address closes the links).
 *
 * Log lines: hassmic's stderr is boot.log (main.sh), so every fprintf(stderr) there, and what the programs it runs print
 * (lipc tools, Amazon's idme tool), gets a time in front: "2026-10-06 15:18:02.417Z" (UTC; the settings page shows it
 * in the browser's zone) once root set the clock, "boot+29935.512" (seconds since boot) before, as a wrong date is
 * worse than none.  stderr becomes a pipe, and a process of its own (forked before any thread: "hassmic-log") reads it
 * and writes the stamped lines to the log.  A process, not a thread: when hassmic dies, its last lines - the ones that
 * say why - are still in the pipe, and the reader writes them out before it sees the end.  And not hassmic's child but
 * its grandchild (init's, once the middle one has gone): hassmic ignores SIGCHLD, and then a plain wait() blocks until
 * every child has exited.  Amazon's attestation module waits for its idme tool that way: with the reader as a child it
 * waited for ever, and hassmic never opened its ports (Dot 2, 2026-10-06).
 * stdout goes the same way: hassmic itself prints nothing there in this mode, the programs it runs do.
 * The pipe's write end does not block: a reader that stalls costs lines, never hassmic itself.
 */
#include "clock.h"
#include "tz.h"
#include "apcountry.h"
#include "screen.h"
#include "statefile.h"
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define EPOCH_MIN 1767225600u           /* 2026-01-01: an earlier answer is a wrong clock in Home Assistant */

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

int clock_synced(void)
{
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";
    __system_property_get("hassmic.clock.synced", v);
    return !strcmp(v, "1");
#else
    const char *e = getenv("HASSMIC_CLOCK_SYNCED");     /* tests: 0 plays an Echo whose clock nobody set */
    return !e || strcmp(e, "0");                        /* a PC keeps its own time */
#endif
}

void clock_from_ha(uint32_t epoch)
{
    char p[300], t[64]; struct timespec bt;
    long long off = (long long)epoch - (long long)time(NULL);
    if (epoch < EPOCH_MIN) { fprintf(stderr, "clock: Home Assistant says %u, earlier than this build: ignored\n", epoch); return; }
    if (clock_synced() && off >= -2 && off <= 2) return;
    clock_gettime(CLOCK_BOOTTIME, &bt);
    snprintf(p, sizeof p, "%s/clock", state_dir());
    snprintf(t, sizeof t, "%u %lld.%02ld\n", epoch, (long long)bt.tv_sec, bt.tv_nsec / 10000000);
    if (statefile_write(p, 0666, t, strlen(t))) { fprintf(stderr, "clock: cannot write %s\n", p); return; }
    fprintf(stderr, "clock: Home Assistant's time handed to root, this Echo's clock is %lld s %s\n", off < 0 ? -off : off, off < 0 ? "ahead" : "behind");
}

/* bionic's time zone data: an update's (N and later, then M), else the system's */
static const char *const TZDATA[] = { "/data/misc/zoneinfo/current/tzdata", "/data/misc/zoneinfo/tzdata",
                                      "/system/usr/share/zoneinfo/tzdata" };

static const char *const zone_hows[] = { "country", "kept", "guess", "rule" };   /* TZ_*, then a rule itself */
#define ZONE_RULE 3

/* state/zone: the zone and how it was found (zone_hows), 1 if read */
static int zone_read(char *zone, size_t cap, int *how)
{
    char p[300], z[100], h[16]; FILE *f; int ok = 0;
    snprintf(p, sizeof p, "%s/zone", state_dir());
    if ((f = fopen(p, "r"))) { ok = fscanf(f, "%99s %15s", z, h) == 2; fclose(f); }
    for (int i = 0; ok && i < (int)(sizeof zone_hows / sizeof *zone_hows); i++)
        if (!strcmp(h, zone_hows[i])) { snprintf(zone, cap, "%s", z); *how = i; return 1; }
    return 0;
}

int clock_zone(char *out, size_t cap)
{
    int how;
    if (!zone_read(out, cap, &how) || (how != TZ_COUNTRY && how != TZ_KEPT)) { if (cap) out[0] = 0; return 0; }
    return 1;
}

/* What came of Home Assistant's rule, kept in state/zone (only when it changed: /data is flash).  A zone kept from what
 * was set is hassmic's own guess still when that is what it set before (persist.sys.timezone survives where the Echo
 * keeps its properties) */
static void zone_note(const char *zone, int how, const char *cc)
{
    char old[100] = "", p[300], line[128]; int was = -1, known_was, known;
    zone_read(old, sizeof old, &was);
    if (how == TZ_KEPT && was == TZ_GUESS && !strcmp(old, zone)) how = TZ_GUESS;
    /* and the other way: a zone known before (the country's, say Europe/Amsterdam for NL) stays known when the country
     * is missing once (a mesh node without the element, a read cut short) and the lookup calls it the guess's own */
    if (how == TZ_GUESS && (was == TZ_COUNTRY || was == TZ_KEPT) && !strcmp(old, zone)) how = was;
    if (was == how && !strcmp(old, zone)) return;
    snprintf(p, sizeof p, "%s/zone", state_dir()); snprintf(line, sizeof line, "%s %s\n", zone, zone_hows[how]);
    if (statefile_write(p, 0644, line, strlen(line))) { fprintf(stderr, "time zone: cannot write %s\n", p); return; }
    known_was = was == TZ_COUNTRY || was == TZ_KEPT; known = how == TZ_COUNTRY || how == TZ_KEPT;
    char why[64];
    if (cc[0]) snprintf(why, sizeof why, "the Wi-Fi's country %s has no zone of that rule", cc);
    else snprintf(why, sizeof why, "no country from the Wi-Fi");
    if (how == TZ_COUNTRY) fprintf(stderr, "time zone: %s, of the Wi-Fi's country %s\n", zone, cc);
    else if (how == TZ_KEPT) fprintf(stderr, "time zone: %s, the one set or known before (%s)\n", zone, why);
    else if (how == TZ_GUESS) fprintf(stderr, "time zone: %s, a guess among those of that rule (%s): not taken for the language\n", zone, why);
    if (known != known_was || (known && strcmp(old, zone))) screen_language_changed();
}

/* zone_thread: the rule worked through (the Wi-Fi's country, the lookup, the files) */
static void zone_from_ha(const char *posix)
{
    static char told[100];
    char cur[100] = "", zone[100] = "", p[300], line[104], cc[3] = "";
    int how = ZONE_RULE;
    size_t n = strlen(posix);
    if (!n) return;                                     /* an older Home Assistant (aioesphomeapi) says none */
    /* a property value (92 bytes with its end), nothing root's shell would read as more than a word, and a rule, not a
     * path: it starts with the zone's abbreviation (a letter, or "<" for one like <+03>), never ":" or "/" */
    if (n > 91 || strspn(posix, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789<>+-,./:") != n ||
        !(posix[0] == '<' || (posix[0] >= 'A' && posix[0] <= 'Z') || (posix[0] >= 'a' && posix[0] <= 'z')) || strstr(posix, "..") ||
        strcspn(posix, "/") < strcspn(posix, ",")) {     /* a "/" only in the rules after the first ",": as main.sh checks */
        if (strcmp(told, "?")) fprintf(stderr, "time zone: Home Assistant's \"%.100s\" not understood, ignored\n", posix);
        snprintf(told, sizeof told, "?");
        return;
    }
#ifdef __ANDROID__
    __system_property_get("persist.sys.timezone", cur);
#else
    const char *e = getenv("HASSMIC_TIMEZONE"); snprintf(cur, sizeof cur, "%s", e ? e : "");   /* tests: the zone set */
#endif
    const char *t = getenv("HASSMIC_TZDATA");
    /* no zone set (the Dot 2 starts every boot without persist.sys.timezone): the one known here before stands for it,
     * so a country missing once keeps it rather than a guess */
    char known[100] = "", *set = cur; int kh;
    if (!cur[0] && zone_read(known, sizeof known, &kh) && (kh == TZ_COUNTRY || kh == TZ_KEPT)) set = known;
    apcountry_get(cc);                                  /* ~1 ms: a netlink dump (Dot 2) */
    int found = t ? tz_lookup(t, posix, set, cc, zone, sizeof zone, &how) : 0;
    for (size_t i = 0; !t && !found && i < sizeof TZDATA / sizeof *TZDATA; i++) found = tz_lookup(TZDATA[i], posix, set, cc, zone, sizeof zone, &how);
    if (!found) { snprintf(zone, sizeof zone, "%s", posix); how = ZONE_RULE; }
    zone_note(zone, how, cc);
    if (!strcmp(zone, cur)) return;
    snprintf(p, sizeof p, "%s/timezone", state_dir()); snprintf(line, sizeof line, "%s\n", zone);
    if (statefile_write(p, 0666, line, strlen(line))) { fprintf(stderr, "time zone: cannot write %s\n", p); return; }
    if (strcmp(told, zone) && found) fprintf(stderr, "time zone: Home Assistant's, %s (%s), handed to root (was %s)\n", zone, posix, cur[0] ? cur : "none");
    else if (strcmp(told, zone)) fprintf(stderr, "time zone: Home Assistant's rule %s handed to root as it is: no zone of this Echo's data has it (was %s)\n", posix, cur[0] ? cur : "none");
    snprintf(told, sizeof told, "%s", zone);
}

/* Home Assistant's answer comes in with core_lock held (proto_esphome.c), and the work is not for under it: the
 * netlink dump waits on the kernel's rtnl lock, which a driver may hold a while, and the lookup reads every one of the
 * ~600 zones' rules.  So a thread of its own takes the newest rule given (older ones still waiting are dropped: the
 * same rule, or one gone stale), one at a time, in order */
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;
static char zone_pending[128];
static int zone_busy;

static void *zone_thread(void *arg)
{
    char posix[128];
    (void)arg;
    pthread_mutex_lock(&zone_lock);
    while (zone_pending[0]) {
        snprintf(posix, sizeof posix, "%s", zone_pending); zone_pending[0] = 0;
        pthread_mutex_unlock(&zone_lock);
        zone_from_ha(posix);
        pthread_mutex_lock(&zone_lock);
    }
    zone_busy = 0;
    pthread_mutex_unlock(&zone_lock);
    return NULL;
}

void clock_zone_from_ha(const char *posix)
{
    pthread_t t; pthread_attr_t a; int start;
    if (!posix[0]) return;                              /* an older Home Assistant (aioesphomeapi) says none */
    pthread_mutex_lock(&zone_lock);
    snprintf(zone_pending, sizeof zone_pending, "%.*s", (int)sizeof zone_pending - 1, posix);
    if (strlen(posix) >= sizeof zone_pending) snprintf(zone_pending, sizeof zone_pending, "?");   /* too long: refused there */
    start = !zone_busy; zone_busy = 1;
    pthread_mutex_unlock(&zone_lock);
    if (!start) return;
    pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, zone_thread, NULL)) zone_thread(NULL);     /* no thread: here then, as before */
    pthread_attr_destroy(&a);
}

/* ---------------------------------------------------------------- the log */

static size_t stamp(char *o, size_t cap)
{
    struct timespec ts; struct tm tm;
    if (clock_synced()) {
        clock_gettime(CLOCK_REALTIME, &ts); gmtime_r(&ts.tv_sec, &tm);
        return (size_t)snprintf(o, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03ldZ ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    }
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (size_t)snprintf(o, cap, "boot+%lld.%03ld ", (long long)ts.tv_sec, ts.tv_nsec / 1000000);
}

static void put_all(int fd, const char *p, size_t n)
{
    while (n) { ssize_t w = write(fd, p, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) return; p += w; n -= (size_t)w; }
}

/* Amazon's attestation module runs /system/bin/idme to read the raw identity store first; as the daemon's user that
 * fails with these three lines, four times per signing (davs.c's probe at start, every Amazon login), and it signs from
 * /proc/idme anyway (docs/re-davs-login.md).  Dropped when they come as that group, nowhere else.  The module itself
 * then warns that it cannot drop its groups (it does not run as root) and carries on: that line goes too.  Cannoli (NAND,
 * no mmcblk0boot1) prints the last two only. */
static const char *const idme_noise[3] = { "Could not open /dev/block/mmcblk0boot1!", "Can't read the idme.", "system error occured while processing command" };
static int dha_noise(const char *l)
{
    static const char at[] = "Error in hardware/amazon_hal/security/amzn_dha/";
    return !strncmp(l, at, sizeof at - 1) && strstr(l, "[Warning: setgroups failed") && strstr(l, "continuing]");
}

static void forward(int in, int out)
{
    char buf[4096], line[2048 + 40]; size_t n = 0, pre = 0; int noise = 0;
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        for (ssize_t i = 0; i < r; i++) {
            if (!n) pre = n = stamp(line, 40);
            int end = buf[i] == '\n';
            if (!end) line[n++] = buf[i];
            if (!end && n < sizeof line - 1) continue;
            line[n] = 0;
            if (!strcmp(line + pre, idme_noise[noise])) noise = (noise + 1) % 3;
            else if (!noise && !strcmp(line + pre, idme_noise[1])) noise = 2;
            else if (dha_noise(line + pre)) noise = 0;
            else { noise = 0; line[n++] = '\n'; put_all(out, line, n); }
            n = 0;
        }
    }
    if (n) { line[n++] = '\n'; put_all(out, line, n); }      /* a last line without its end */
}

void clock_log_start(void)
{
    int p[2], out = dup(2);
    if (out < 0 || pipe(p)) { if (out >= 0) close(out); return; }
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); close(out); return; }
    if (!pid && fork()) _exit(0);                       /* the middle one: gone at once, the reader is init's */
    if (!pid) {
        /* "pidof hassmic" lists it too (its command line is hassmic's), so "kill $(pidof hassmic)" (scripts/lib/setup.sh)
         * and "kill -TTIN $(pidof hassmic)" reach it.  Stopped or killed by a signal meant for hassmic it would take the
         * log with it, the lines saying how hassmic went among them.  It ends when the last writer has closed the pipe */
        static const int ignore[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1, SIGUSR2, SIGPIPE, SIGTTIN, SIGTTOU, SIGTSTP, SIGWINCH };
        for (size_t i = 0; i < sizeof ignore / sizeof ignore[0]; i++) signal(ignore[i], SIG_IGN);
        prctl(PR_SET_NAME, "hassmic-log", 0, 0, 0);
        close(p[1]);
        forward(p[0], out);
        _exit(0);
    }
    close(p[0]); close(out);
    waitpid(pid, NULL, 0);                              /* the middle one (SIGCHLD is not ignored yet) */
#ifdef F_SETPIPE_SZ
    fcntl(p[1], F_SETPIPE_SZ, 256 * 1024);       /* room for a burst (the start writes ~100 lines at once) */
#endif
    dup2(p[1], 2); dup2(p[1], 1); close(p[1]);
    fcntl(2, F_SETFL, fcntl(2, F_GETFL) | O_NONBLOCK);         /* one open file: stdout's too */
    setvbuf(stderr, NULL, _IONBF, 0); setvbuf(stdout, NULL, _IOLBF, 0);
}
