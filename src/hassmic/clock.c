/*
 * The Echo's clock, and when each log line was written.
 *
 * Clock: nothing sets it any more.  Stock synced with Amazon's time servers, which the egress lock keeps out, and the
 * Echo runs on from its RTC: the Dot 2 was 23 h behind on 2026-10-06.  Home Assistant knows the time and says it when a
 * device asks (ESPHome GetTimeRequest, answered by aioesphomeapi itself), so proto_esphome.c asks on every link with the
 * device key and every 6 h on it.  hassmic may not set the clock (no CAP_SYS_TIME as the daemon's user), so the answer
 * goes to root the way update and adb requests do: state/clock holds "<epoch> <CLOCK_BOOTTIME when it was right>", the
 * latter as /proc/uptime words it (seconds.centiseconds: the same clock, and main.sh's mksh only counts to 2^31, which
 * milliseconds pass after 25 days).  main.sh's netwatch (every 10 s) adds what has passed since, sets the time and the RTC,
 * and sets the property hassmic.clock.synced, which a reboot clears.  Only Home Assistant over the keyed link counts:
 * anyone on the network could otherwise move the clock, and with it the validity of the certificates curl checks.
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
#include <errno.h>
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
    char p[300], tmp[310]; FILE *f; struct timespec bt;
    long long off = (long long)epoch - (long long)time(NULL);
    if (epoch < EPOCH_MIN) { fprintf(stderr, "clock: Home Assistant says %u, earlier than this build: ignored\n", epoch); return; }
    if (clock_synced() && off >= -2 && off <= 2) return;
    clock_gettime(CLOCK_BOOTTIME, &bt);
    snprintf(p, sizeof p, "%s/clock", state_dir()); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    if (!(f = fopen(tmp, "w"))) { fprintf(stderr, "clock: cannot write %s\n", tmp); return; }
    fprintf(f, "%u %lld.%02ld\n", epoch, (long long)bt.tv_sec, bt.tv_nsec / 10000000);
    if (fclose(f) || rename(tmp, p)) { unlink(tmp); fprintf(stderr, "clock: cannot write %s\n", p); return; }
    fprintf(stderr, "clock: Home Assistant's time handed to root, this Echo's clock is %lld s %s\n", off < 0 ? -off : off, off < 0 ? "ahead" : "behind");
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
 * then warns that it cannot drop its groups (it does not run as root) and carries on: that line goes too. */
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
