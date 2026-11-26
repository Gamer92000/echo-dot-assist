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
 * anyone on the network could otherwise move the clock, and with it the validity of the certificates tls.c checks.
 *
 * Time zone: Home Assistant's comes with its time, as the rule of its zone (tz.c turns it into a zone name).  It goes to
 * root the same way (state/timezone, one line), and main.sh sets persist.sys.timezone, plus Android's alarm service
 * where there is one (it tells the Java side).  The logs stay in UTC whatever the zone.
 * Which of the zones with that rule: the one of the country Home Assistant's home is in (its zone.home's location,
 * geo.c), near a border or without a location the Wi-Fi's country (apcountry.c), tz.c's choice among them; what came of
 * it, and how, stays in state/zone, hassmic's own ("<zone> home|wifi|kept|guess|rule <its country> <the home's> <the
 * Wi-Fi's>", "-" for none; root takes
 * state/timezone away at once, issue #176's review: the language never saw it on an Echo).  The language spoken here (main.c speech_locale, the Echo Show's
 * date) takes the zone only when it is known: a country's, or one set before and kept, never a guess (CET without a
 * country is Europe/Amsterdam: a Dutch date in Germany), nor hassmic's own guess kept by root since.  Worked out again
 * with each answer of Home Assistant's (every link, every 6 h; a new network's new address closes the links) and when
 * its home location changes.
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
#include "logread.h"
#include "clock.h"
#include "tz.h"
#include "apcountry.h"
#include "geo.h"
#include "screen.h"
#include "statefile.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
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

/* how the zone was found, as state/zone says it: the country of Home Assistant's home location, the Wi-Fi's country,
 * the zone set kept, a guess among those of the rule, the rule itself (no zone has it) */
enum { ZONE_HOME, ZONE_WIFI, ZONE_KEPT, ZONE_GUESS, ZONE_RULE };
static const char *const zone_hows[] = { "home", "wifi", "kept", "guess", "rule" };
static int zone_known(int how) { return how == ZONE_HOME || how == ZONE_WIFI || how == ZONE_KEPT; }

/* state/zone: "<zone> <how> <its country> <the home's> <the Wi-Fi's>" (a country "-" none), 1 if read; the countries
 * into cc[0..2] when given */
static void cc_take(char *to, const char *v) { if (strlen(v) == 2 && isupper((unsigned char)v[0]) && isupper((unsigned char)v[1])) memcpy(to, v, 3); else to[0] = 0; }
static int zone_read(char *zone, size_t cap, int *how, char cc[][3])
{
    char p[300], z[100], h[16], c[3][8] = { "-", "-", "-" }; FILE *f; int ok = 0;
    snprintf(p, sizeof p, "%s/zone", state_dir());
    if ((f = fopen(p, "r"))) { ok = fscanf(f, "%99s %15s %7s %7s %7s", z, h, c[0], c[1], c[2]) >= 2; fclose(f); }
    if (ok && !strcmp(h, "country")) snprintf(h, sizeof h, "wifi");    /* bfcbfac's word, never released */
    for (int i = 0; ok && i < (int)(sizeof zone_hows / sizeof *zone_hows); i++)
        if (!strcmp(h, zone_hows[i])) {
            snprintf(zone, cap, "%s", z); *how = i;
            for (int k = 0; cc && k < 3; k++) cc_take(cc[k], c[k]);
            return 1;
        }
    return 0;
}

int clock_zone(char *out, size_t cap)
{
    int how;
    if (!zone_read(out, cap, &how, NULL) || !zone_known(how)) { if (cap) out[0] = 0; return 0; }
    return 1;
}

int clock_zone_info(char *zone, size_t cap, const char **how, char cc[3][3])
{
    int h;
    if (!zone_read(zone, cap, &h, cc)) { if (cap) zone[0] = 0; cc[0][0] = cc[1][0] = cc[2][0] = 0; *how = ""; return 0; }
    *how = zone_hows[h];
    return 1;
}

/* What came of Home Assistant's rule, kept in state/zone (only when it changed: /data is flash).  A zone kept from what
 * was set is hassmic's own guess still when that is what it set before (persist.sys.timezone survives where the Echo
 * keeps its properties); a zone known before stays known when the country is missing once (a link before Home
 * Assistant's location came, an access point without the element) and the lookup calls it the guess's own */
static void zone_note(const char *zone, int how, const char *cc, const char *home, const char *wifi, const char *why)
{
    char old[100] = "", occ[3][3] = { "", "", "" }, p[300], line[160]; int was = -1;
    zone_read(old, sizeof old, &was, occ);
    int same = !strcmp(old, zone);
    if (how == ZONE_KEPT && was == ZONE_GUESS && same) how = ZONE_GUESS;
    if (how == ZONE_GUESS && zone_known(was) && same) { how = was; cc = occ[0]; }
    if (how == ZONE_KEPT && zone_known(was) && same && occ[0][0]) cc = occ[0];  /* the country it was found by, for the page */
    if (was == how && same && !strcmp(occ[0], cc) && !strcmp(occ[1], home) && !strcmp(occ[2], wifi)) return;
    snprintf(p, sizeof p, "%s/zone", state_dir());
    snprintf(line, sizeof line, "%s %s %s %s %s\n", zone, zone_hows[how], cc[0] ? cc : "-", home[0] ? home : "-", wifi[0] ? wifi : "-");
    if (was == how && same && !strcmp(occ[0], cc)) {    /* only what the countries say changed: no line in the log */
        if (statefile_write(p, 0644, line, strlen(line))) fprintf(stderr, "time zone: cannot write %s\n", p);
        return;
    }
    if (statefile_write(p, 0644, line, strlen(line))) { fprintf(stderr, "time zone: cannot write %s\n", p); return; }
    /* private: the country of the home's location, and the reason, which names the countries the home and the Wi-Fi
     * are in (a plugin is told the zone, never where the home is) */
    if (how == ZONE_HOME) log_private("time zone: %s, of the country of Home Assistant's home location, %s", zone, cc);
    else if (how == ZONE_WIFI) log_private("time zone: %s, of the Wi-Fi's country %s (%s)", zone, cc, why);
    else if (how == ZONE_KEPT) log_private("time zone: %s, the one set or known before (%s)", zone, why);
    else if (how == ZONE_GUESS) log_private("time zone: %s, a guess among those of that rule (%s): not taken for the language", zone, why);
    if (zone_known(how) != zone_known(was) || (zone_known(how) && !same)) screen_language_changed();
}

/* Home Assistant's home location (zone.home's latitude and longitude, proto_esphome.c), under zone_lock.  Never
 * written anywhere nor logged: only the country found from it goes into state/zone */
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;
static double home_lat, home_lon;
static int home_set, home_heard;                        /* home_heard: Home Assistant answered for zone.home in this run */
static long long home_wait_until, home_settle_until;    /* monotonic ms: see zone_thread */
static char zone_rule[128];                             /* the last rule Home Assistant gave, for a location after it */

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* zone_thread: the rule worked through (the countries, the lookup, the files) */
static void zone_from_ha(const char *posix)
{
    static char told[100];
    char cur[100] = "", zone[100] = "", p[300], line[104], wifi[3] = "", home[3] = "", near[GEO_NEAR_MAX][3], why[96];
    int how = ZONE_RULE, tzhow = TZ_GUESS, nnear = 0, have_home; double lat, lon;
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
    if (!cur[0] && zone_read(known, sizeof known, &kh, NULL) && zone_known(kh)) set = known;
    pthread_mutex_lock(&zone_lock); have_home = home_set; lat = home_lat; lon = home_lon; pthread_mutex_unlock(&zone_lock);
    /* coordinates only in here: never into a file or the log */
    if (have_home) geo_country(lat, lon, home, near, &nnear);
    apcountry_get(wifi);                                /* ~1 ms: a netlink dump (Dot 2) */
    /* the home's country; within GEO_BORDER_KM of another, the Wi-Fi's where it is that one (the borders are drawn to
     * ~500 m, Home Assistant's location may be the town's); without a location, the Wi-Fi's.  The other one second,
     * for a rule the first has no zone of */
    const char *cand[2] = { NULL, NULL }; int src[2] = { ZONE_HOME, ZONE_WIFI };
    int wifi_near = 0;
    for (int i = 0; i < nnear; i++) if (!strcmp(near[i], wifi)) wifi_near = 1;
    if (home[0] && wifi[0] && strcmp(home, wifi) && wifi_near) { cand[0] = wifi; src[0] = ZONE_WIFI; cand[1] = home; src[1] = ZONE_HOME; }
    else if (home[0]) { cand[0] = home; cand[1] = wifi[0] && strcmp(home, wifi) ? wifi : NULL; }
    else if (wifi[0]) { cand[0] = wifi; src[0] = ZONE_WIFI; }
    int found = 0;
    for (int c = 0; c < 3 && how == ZONE_RULE; c++) {   /* each candidate, then none */
        const char *cc = c < 2 ? cand[c] : "";
        if (c < 2 && !cc) continue;
        int f = t ? tz_lookup(t, posix, set, cc, zone, sizeof zone, &tzhow) : 0;
        for (size_t i = 0; !t && !f && i < sizeof TZDATA / sizeof *TZDATA; i++) f = tz_lookup(TZDATA[i], posix, set, cc, zone, sizeof zone, &tzhow);
        if (!f) break;                                  /* no zone has the rule: no country changes that */
        found = 1;
        if (tzhow == TZ_COUNTRY && c < 2) how = src[c];
        else if (c == 2) how = tzhow == TZ_KEPT ? ZONE_KEPT : ZONE_GUESS;
    }
    const char *cc = how == ZONE_HOME ? home : how == ZONE_WIFI ? wifi : "";
    if (how == ZONE_WIFI && home[0] && wifi_near) snprintf(why, sizeof why, "the home is within %g km of %s, Home Assistant's home's country %s", GEO_BORDER_KM, wifi, home);
    else if (how == ZONE_WIFI && home[0]) snprintf(why, sizeof why, "no zone of that rule in %s, Home Assistant's home's country", home);
    else if (how == ZONE_WIFI) snprintf(why, sizeof why, "no home location from Home Assistant");
    else if (home[0] || wifi[0]) snprintf(why, sizeof why, "no zone of that rule in %s%s%s", home[0] ? home : "", home[0] && wifi[0] && strcmp(home, wifi) ? " or " : "", wifi[0] && strcmp(home, wifi) ? wifi : "");
    else snprintf(why, sizeof why, "no home location from Home Assistant, no country from the Wi-Fi");
    if (!found) { snprintf(zone, sizeof zone, "%s", posix); how = ZONE_RULE; }
    zone_note(zone, how, cc, home, wifi, why);
    if (!strcmp(zone, cur)) return;
    snprintf(p, sizeof p, "%s/timezone", state_dir()); snprintf(line, sizeof line, "%s\n", zone);
    if (statefile_write(p, 0666, line, strlen(line))) { fprintf(stderr, "time zone: cannot write %s\n", p); return; }
    if (strcmp(told, zone) && found) fprintf(stderr, "time zone: Home Assistant's, %s (%s), handed to root (was %s)\n", zone, posix, cur[0] ? cur : "none");
    else if (strcmp(told, zone)) fprintf(stderr, "time zone: Home Assistant's rule %s handed to root as it is: no zone of this Echo's data has it (was %s)\n", posix, cur[0] ? cur : "none");
    snprintf(told, sizeof told, "%s", zone);
}

/* Home Assistant's answer comes in with core_lock held (proto_esphome.c), and the work is not for under it: the
 * netlink dump waits on the kernel's rtnl lock, which a driver may hold a while, the lookup reads every one of the
 * ~600 zones' rules, and the borders are inflated.  So a thread of its own takes the newest rule given (older ones
 * still waiting are dropped: the same rule, or one gone stale), one at a time, in order */
static char zone_pending[128];
static int zone_busy;

static void *zone_thread(void *arg)
{
    char posix[128];
    int inline_ = arg != NULL;                          /* run by the caller (no thread): never wait under its lock */
    pthread_mutex_lock(&zone_lock);
    while (zone_pending[0]) {
        /* at a link's start Home Assistant's time comes before its answer for zone.home: up to 2 s for that (else the
         * zone is worked out without it, written, and again with it: two writes, and root setting the zone twice); a
         * home that moved comes as latitude and longitude one after the other: 300 ms for both, before every run */
        for (long long now = mono_ms(); !inline_ && ((!home_heard && now < home_wait_until) || now < home_settle_until); now = mono_ms()) {
            pthread_mutex_unlock(&zone_lock); usleep(50000); pthread_mutex_lock(&zone_lock);
        }
        snprintf(posix, sizeof posix, "%s", zone_pending); zone_pending[0] = 0;
        pthread_mutex_unlock(&zone_lock);
        zone_from_ha(posix);
        pthread_mutex_lock(&zone_lock);
    }
    zone_busy = 0;
    pthread_mutex_unlock(&zone_lock);
    return NULL;
}

static void zone_queue(const char *posix)               /* zone_lock held: posix (shorter than zone_pending) to the thread */
{
    pthread_t t; pthread_attr_t a;
    snprintf(zone_pending, sizeof zone_pending, "%s", posix);
    if (zone_busy) return;
    zone_busy = 1;
    pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, zone_thread, NULL)) {    /* no thread: here then, as before */
        pthread_mutex_unlock(&zone_lock); zone_thread(&zone_busy); pthread_mutex_lock(&zone_lock);
    }
    pthread_attr_destroy(&a);
}

void clock_zone_from_ha(const char *posix)
{
    if (!posix[0]) return;                              /* an older Home Assistant (aioesphomeapi) says none */
    pthread_mutex_lock(&zone_lock);
    snprintf(zone_rule, sizeof zone_rule, "%s", strlen(posix) < sizeof zone_rule ? posix : "?");   /* too long: refused there */
    zone_queue(zone_rule);
    pthread_mutex_unlock(&zone_lock);
}

void clock_home_expect(void)
{
    pthread_mutex_lock(&zone_lock);
    if (!home_heard) home_wait_until = mono_ms() + 2000;
    pthread_mutex_unlock(&zone_lock);
}

void clock_home(double lat, double lon)
{
    /* 0, 0 is Home Assistant's location when none was set (core_config: latitude = 0): no location */
    int set = isfinite(lat) && isfinite(lon) && (lat != 0 || lon != 0) && fabs(lat) <= 90 && fabs(lon) <= 180;
    pthread_mutex_lock(&zone_lock);
    /* moved by more than ~100 m, or set or unset: the zone again, if Home Assistant gave its rule */
    int changed = set != home_set || (set && (fabs(lat - home_lat) > 0.001 || fabs(lon - home_lon) > 0.001));
    home_set = set; if (set) { home_lat = lat; home_lon = lon; }
    if (changed && home_heard) home_settle_until = mono_ms() + 300;
    home_heard = 1;
    if (changed && zone_rule[0]) zone_queue(zone_rule);
    pthread_mutex_unlock(&zone_lock);
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

/* A line longer than LINE is cut into pieces, each stamped as a line of its own.  A piece after a cut is the same
 * line's, so it carries the mark the line has (logread.h: a marked line's text after the cut went to the plugins
 * unmarked; a review, 2026-11-21).  log_private's lines are kept short of the cut; this is for anything else that
 * comes past it with a mark.  A line break inside a value cannot be told here from the next line (what is one write
 * the pipe does not say): log_private takes those out before the line is written */
/* how many bytes at the end of T (N bytes) are the start of a mark and not all of it: a cut there split the mark */
static size_t mark_begun(const char *t, size_t n)
{
    static const char *const marks[] = { LOG_PRIVATE_MARK, LOG_SEALED_MARK };
    size_t best = 0;
    for (size_t k = 0; k < 2; k++)
        for (size_t l = strlen(marks[k]) - 1; l > best; l--) if (l <= n && !memcmp(t + n - l, marks[k], l)) { best = l; break; }
    return best;
}
static void forward(int in, int out)
{
    char buf[4096], line[2048 + 40], held[16]; size_t n = 0, pre = 0, nheld = 0; int noise = 0; const char *carry = NULL;
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        for (ssize_t i = 0; i < r; i++) {
            if (!n) {
                pre = n = stamp(line, 40);
                if (carry) { memcpy(line + n, carry, strlen(carry)); n += strlen(carry); carry = NULL; }
                if (nheld) { memcpy(line + n, held, nheld); n += nheld; nheld = 0; }
            }
            int end = buf[i] == '\n';
            if (!end) line[n++] = buf[i];
            if (!end && n < sizeof line - 1) continue;
            line[n] = 0;
            /* cut: the rest is this line's too.  A mark cut in two ("(priv" here, "ate) " in the next piece) is a mark
             * in neither: its first bytes go over into the next piece, which then has it whole (Q3 of a review,
             * 2026-11-21) */
            if (!end && (nheld = mark_begun(line + pre, n - pre))) { n -= nheld; memcpy(held, line + n, nheld); line[n] = 0; }
            if (!end) carry = logread_mark(line + pre, n - pre);
            if (!strcmp(line + pre, idme_noise[noise])) noise = (noise + 1) % 3;
            else if (!noise && !strcmp(line + pre, idme_noise[1])) noise = 2;
            else if (dha_noise(line + pre)) noise = 0;
            else { noise = 0; line[n++] = '\n'; put_all(out, line, n); }
            n = 0;
        }
    }
    if (n || nheld) { if (!n) n = stamp(line, 40); memcpy(line + n, held, nheld); n += nheld; line[n++] = '\n'; put_all(out, line, n); }      /* a last line without its end */
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
        /* the plugins' launcher (plugrun.h) ends when hassmic's end of its socket closes: not held open by this
         * process, which outlives hassmic on purpose */
        { const char *e = getenv("HASSMIC_PLUGIN_FD"); if (e && atoi(e) > 2) close(atoi(e)); }
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
