/*
 * Alarms of the Echo's own (alarms.h).  Home Assistant has timers but no alarms (PLAN.md's open item), and a wake-up
 * alarm should not hang on a server being up at 6:30: they live here, in state/alarms, and ring on the Echo's own clock
 * through the core's timer ring (main.c core_alarm_ring), snoozed or stopped as a timer is.
 *
 *   time       local time by localtime/mktime: bionic reads persist.sys.timezone (main.sh tz_set, from Home Assistant's
 *              zone, clock.c), the PC TZ.  A repeating alarm rings at its time of day on its days whatever DST does
 *              (mktime with tm_isdst -1 places 02:30 on the day the clocks go forward an hour later, as Alexa does).
 *   clock      none rings before the clock may be trusted (alarms_clock_ok): set from Home Assistant since boot
 *              (clock_synced), or a year that is no RTC's default.  A jump (the clock set) is not a minute gone by:
 *              alarms between the old and the new time do not all ring at once (RING_BACK)
 *   restarts   `fired` (kept) is the moment an alarm last rang for: an update or a crash within RING_BACK of an alarm
 *              rings it after the start, never twice
 *   state      state/alarms, a line per alarm: "id hour min days enabled once snoozed fired label"; written through
 *              statefile (issue #46)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "alarms.h"
#include "clock.h"
#include "statefile.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define RING_BACK   300                 /* s: an alarm missed by a restart this recently still rings */
#define JUMP        120                 /* s: a step of the clock bigger than this (either way) is the clock set */

static const char *const wdays[7] = { "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday" };
static const char *const Wdays[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const Wd3[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const Mon3[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ---------------------------------------------------------------- when */

/* day `d` days after the day of `base` at h:m, local time */
static time_t at_day(time_t base, int d, int h, int m)
{
    struct tm tm; localtime_r(&base, &tm);
    tm.tm_mday += d; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = 0; tm.tm_isdst = -1;
    return mktime(&tm);
}

static int wday_of(time_t t) { struct tm tm; localtime_r(&t, &tm); return tm.tm_wday; }

time_t alarms_next_at(const struct alarm *a, time_t after)
{
    if (!a->enabled) return 0;
    time_t best = a->snoozed > after ? a->snoozed : 0, t = 0;
    if (!a->days) t = a->once > after && a->once > a->fired ? a->once : 0;
    else for (int d = 0; d <= 7; d++) {
        time_t c = at_day(after, d, a->hour, a->min);
        if (c > after && c > a->fired && a->days & 1 << wday_of(c)) { t = c; break; }
    }
    if (t && (!best || t < best)) best = t;
    return best;
}

/* ---------------------------------------------------------------- words */

/* lower case; "a.m." "am", "p.m." "pm", "o'clock" gone, punctuation but ':' and '.' between digits to spaces */
static void normal(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; in[i] && n + 2 < cap; i++) {
        char c = (char)tolower((unsigned char)in[i]);
        if ((c == ':' || c == '.') && i && isdigit((unsigned char)in[i - 1]) && isdigit((unsigned char)in[i + 1])) { out[n++] = ':'; continue; }
        if (c == '\'' || c == '\xe2') { if (c == '\xe2' && in[i + 1] && in[i + 2]) i += 2; continue; }   /* o'clock, ’ */
        if (!isalnum((unsigned char)c) && !(c & 0x80)) c = ' ';
        out[n++] = c;
    }
    out[n] = 0;
    /* "a m" (from "a.m."), "p m" */
    char *p;
    while ((p = strstr(out, " a m")) && (!p[4] || p[4] == ' ')) { p[2] = 'm'; memmove(p + 3, p + 4, strlen(p + 4) + 1); }
    while ((p = strstr(out, " p m")) && (!p[4] || p[4] == ' ')) { p[2] = 'm'; memmove(p + 3, p + 4, strlen(p + 4) + 1); }
}

#define MAXTOK 48
struct toks { char t[MAXTOK][24]; int n; };

static void tokens(const char *text, struct toks *k)
{
    char buf[512]; normal(text, buf, sizeof buf);
    k->n = 0;
    for (char *s = strtok(buf, " "); s && k->n < MAXTOK; s = strtok(NULL, " ")) {
        if (!strcmp(s, "oclock") || (!strcmp(s, "clock") && k->n && !strcmp(k->t[k->n - 1], "o"))) {
            if (!strcmp(s, "clock")) k->n--;
            continue;
        }
        snprintf(k->t[k->n++], sizeof k->t[0], "%s", s);
    }
}

static int is(const struct toks *k, int i, const char *w) { return i >= 0 && i < k->n && !strcmp(k->t[i], w); }
static int has(const struct toks *k, const char *w) { for (int i = 0; i < k->n; i++) if (!strcmp(k->t[i], w)) return 1; return 0; }

static int word_num(const char *w)
{
    static const char *const ones[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                        "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen" };
    static const char *const tens[] = { "twenty", "thirty", "forty", "fifty" };
    for (int i = 0; i < 20; i++) if (!strcmp(w, ones[i])) return i;
    for (int i = 0; i < 4; i++) if (!strcmp(w, tens[i])) return 20 + 10 * i;
    if (!strcmp(w, "oh") || !strcmp(w, "o")) return 0;
    if (!strcmp(w, "a") || !strcmp(w, "an")) return 1;
    return -1;
}

/* a number at token i: digits, or words ("forty five", "twenty-one" came apart); *used tokens.  -1 none */
static int number_at(const struct toks *k, int i, int *used)
{
    *used = 0;
    if (i >= k->n) return -1;
    const char *w = k->t[i]; char *end;
    if (isdigit((unsigned char)w[0])) { long v = strtol(w, &end, 10); if (*end) return -1; *used = 1; return (int)v; }
    int v = word_num(w);
    if (v < 0) return -1;
    *used = 1;
    if (v >= 20 && v % 10 == 0 && i + 1 < k->n) { int o = word_num(k->t[i + 1]); if (o > 0 && o < 10) { v += o; *used = 2; } }
    return v;
}

static int day_name(const char *w)
{
    for (int d = 0; d < 7; d++) {
        size_t n = strlen(wdays[d]);
        if (!strcmp(w, wdays[d]) || (!strncmp(w, wdays[d], n) && !strcmp(w + n, "s"))) return d;   /* "mondays" */
    }
    return -1;
}

/* days said anywhere in the request: 0 none; *once_day the day of a once ("on monday", "tomorrow": -2 tomorrow, -3
 * today, -1 none) */
static int days_said(const struct toks *k, int *once_day, int *evening)
{
    int days = 0, every = 0;
    *once_day = -1; *evening = 0;
    for (int i = 0; i < k->n; i++) {
        const char *w = k->t[i]; int d;
        if (!strcmp(w, "every") || !strcmp(w, "each")) every = 1;
        else if (!strcmp(w, "daily") || !strcmp(w, "everyday") || (every && !strcmp(w, "day")) || (is(k, i - 1, "every") && !strcmp(w, "morning"))) days |= DAYS_DAILY;
        else if (!strcmp(w, "weekdays") || !strcmp(w, "weekday") || !strcmp(w, "workdays")) days |= DAYS_WEEKDAYS;
        else if (!strcmp(w, "weekends") || !strcmp(w, "weekend")) days |= DAYS_WEEKENDS;
        else if (!strcmp(w, "tomorrow")) *once_day = -2;
        else if (!strcmp(w, "today")) *once_day = -3;
        else if (!strcmp(w, "tonight")) { *once_day = -3; *evening = 1; }
        else if ((d = day_name(w)) >= 0) {
            size_t n = strlen(w);
            if (every || w[n - 1] == 's') days |= 1 << d;
            else if (*once_day == -1) *once_day = d;
        }
    }
    /* "every monday and friday": the days after "every" all repeat */
    if (days && *once_day >= 0) { days |= 1 << *once_day; *once_day = -1; }
    return days;
}

/* a time of day starting at token i: hour (0..23 or 1..12 with *ampm 0 none, 1 am, 2 pm), minutes; *used.  -1 none */
static int clock_at(const struct toks *k, int i, int *h, int *m, int *ampm, int *used)
{
    int u, u2, at = i;
    *ampm = 0; *m = 0;
    if (is(k, i, "noon") || is(k, i, "midday")) { *h = 12; *used = 1; return 0; }
    if (is(k, i, "midnight")) { *h = 0; *used = 1; return 0; }
    /* "half past 6", "quarter past 6", "quarter to 7", "20 past 6", "10 to 7" */
    if (is(k, i, "half") || is(k, i, "quarter") || (number_at(k, i, &u) >= 0 && (is(k, i + u, "past") || is(k, i + u, "to")
                                                                                     || (is(k, i + u, "minutes") && (is(k, i + u + 1, "past") || is(k, i + u + 1, "to")))))) {
        int mins = is(k, i, "half") ? 30 : is(k, i, "quarter") ? 15 : number_at(k, i, &u);
        int j = i + (is(k, i, "half") || is(k, i, "quarter") ? 1 : u);
        if (is(k, j, "minutes")) j++;
        int to = is(k, j, "to"), hh;
        if (!is(k, j, "past") && !to && !is(k, j, "after")) return -1;
        if ((hh = number_at(k, j + 1, &u2)) < 0 || hh > 23 || mins > 59 || (to && is(k, i, "half"))) return -1;
        *h = to ? (hh + 23) % 24 : hh; *m = to ? 60 - mins : mins;
        at = j + 1 + u2;
    } else if (i < k->n && strchr(k->t[i], ':')) {                  /* 7:30, 19:00 */
        char *e; long hh = strtol(k->t[i], &e, 10), mm;
        if (*e != ':' || !isdigit((unsigned char)e[1])) return -1;
        mm = strtol(e + 1, &e, 10);
        if (*e == ':' && isdigit((unsigned char)e[1])) { strtol(e + 1, &e, 10); }     /* "07:30:00": a time selector's seconds */
        if (*e || hh > 23 || mm > 59) return -1;
        *h = (int)hh; *m = (int)mm; at = i + 1;
        if (k->t[i][0] == '0' && hh) *ampm = 3;                     /* "07:30": the 24 h clock said it */
    } else {
        const char *w = i < k->n ? k->t[i] : "";
        size_t l = strlen(w);
        if (l >= 3 && l <= 4 && isdigit((unsigned char)w[0]) && strspn(w, "0123456789") == l) {     /* 730, 0645, 1900 */
            int v = atoi(w); *h = v / 100; *m = v % 100;
            if (*h > 23 || *m > 59) return -1;
            at = i + 1;
            if (w[0] == '0' && *h) { *used = 1; *ampm = 3; return 0; }  /* "0730": 24 h */
        } else if (l > 2 && isdigit((unsigned char)w[0]) && (!strcmp(w + l - 2, "am") || !strcmp(w + l - 2, "pm"))) {   /* "7am" */
            char d[8]; snprintf(d, sizeof d, "%.*s", (int)(l - 2), w);
            if (strspn(d, "0123456789") != strlen(d)) return -1;
            *h = atoi(d); *ampm = w[l - 2] == 'a' ? 1 : 2; at = i + 1;
            if (*h < 1 || *h > 12) return -1;
            *used = at - i; return 0;
        } else {
            int hh = number_at(k, i, &u);
            if (hh < 0 || hh > 23 || (word_num(w) == 1 && (!strcmp(w, "a") || !strcmp(w, "an")))) return -1;
            *h = hh; at = i + u;
            int mm = number_at(k, at, &u2);                         /* "seven thirty", "6 45", "seven oh five" */
            if (mm >= 0 && !is(k, at + u2, "minutes") && !is(k, at + u2, "minute") && !is(k, at + u2, "hours") && !is(k, at + u2, "hour")
                && !(word_num(k->t[at]) == 1 && (!strcmp(k->t[at], "a") || !strcmp(k->t[at], "an")))) {
                if (is(k, at, "oh") || is(k, at, "o")) { int z; mm = number_at(k, at + 1, &z); if (mm < 0 || mm > 9) return -1; u2 = 1 + z; }
                if (mm > 59) return -1;
                *m = mm; at += u2;
            }
        }
    }
    if (*ampm == 3) { *used = at - i; return 0; }
    if (is(k, at, "am") || (is(k, at, "in") && is(k, at + 1, "the") && is(k, at + 2, "morning"))) *ampm = 1;
    else if (is(k, at, "pm") || (is(k, at, "in") && is(k, at + 1, "the") && (is(k, at + 2, "evening") || is(k, at + 2, "afternoon")))
             || (is(k, at, "at") && is(k, at + 1, "night")) || is(k, at, "tonight")) *ampm = 2;
    if (*ampm && (*h < 1 || *h > 12)) { if (*h == 0 || *h > 12) { if (*h > 12 && *ampm == 2) *ampm = 0; else return -1; } }
    *used = at - i + (is(k, at, "am") || is(k, at, "pm") ? 1 : 0);
    return 0;
}

/* "in 20 minutes", "in an hour", "in 1 hour and 30 minutes", "in half an hour", "in an hour and a half": seconds, -1 */
static int relative_at(const struct toks *k, int i)
{
    if (!is(k, i, "in")) return -1;
    int j = i + 1, total = 0, any = 0, u;
    if (is(k, j, "half") && (is(k, j + 1, "an") || is(k, j + 1, "a")) && is(k, j + 2, "hour")) return 1800;
    for (;;) {
        int v = number_at(k, j, &u);
        if (v < 0) break;
        const char *unit = j + u < k->n ? k->t[j + u] : "";
        if (!strncmp(unit, "hour", 4)) total += v * 3600;
        else if (!strncmp(unit, "minute", 6) || !strcmp(unit, "min") || !strcmp(unit, "mins")) total += v * 60;
        else break;
        any = 1; j += u + 1;
        if (is(k, j, "and") && is(k, j + 1, "a") && is(k, j + 2, "half")) { total += !strncmp(unit, "hour", 4) ? 1800 : 30; j += 3; }
        if (is(k, j, "and")) j++;
    }
    return any && total > 0 && total <= 24 * 3600 ? total : -1;
}

int alarms_parse(const char *text, time_t now, struct alarm *out)
{
    struct toks k; tokens(text, &k);
    struct alarm a; memset(&a, 0, sizeof a);
    a.enabled = 1;
    for (int i = 0; i < k.n; i++) {                                  /* relative first: "in 20 minutes" */
        int s = relative_at(&k, i);
        if (s > 0) {
            time_t t = now + s; struct tm tm; localtime_r(&t, &tm);
            a.hour = tm.tm_hour; a.min = tm.tm_min; a.once = t - tm.tm_sec;
            if (a.once <= now) a.once += 60;
            *out = a; return 0;
        }
    }
    int once_day, evening, days = days_said(&k, &once_day, &evening), h = -1, m = 0, ampm = 0;
    for (int i = 0; i < k.n && h < 0; i++) {
        int hh, mm, ap, u;
        /* a bare number counts only after "at"/"for"/"to" or before am/pm: "set 2 alarms" is no time */
        if (clock_at(&k, i, &hh, &mm, &ap, &u)) continue;
        int bare = !strchr(k.t[i], ':') && !ap && isdigit((unsigned char)k.t[i][0]) ? 1 : word_num(k.t[i]) >= 0;
        if (bare && !is(&k, i - 1, "at") && !is(&k, i - 1, "for") && !is(&k, i - 1, "to") && !is(&k, i - 1, "until") && !is(&k, i - 1, "till")
            && strcmp(k.t[i], "noon") && strcmp(k.t[i], "midnight") && !is(&k, i + 1, "past") && !is(&k, i + 1, "to")) continue;
        h = hh; m = mm; ampm = ap;
    }
    if (h < 0) return -1;
    if (ampm == 3) ampm = 1;                                        /* 24 h: as said, like "am" with no 12 to turn */
    if (ampm == 2 && h < 12) h += 12;
    else if (ampm == 1 && h == 12) h = 0;
    else if (!ampm && evening && h >= 1 && h < 12) h += 12;
    a.min = m; a.days = days;
    if (days) { a.hour = h; *out = a; return 0; }
    /* once: on the day said, else the next time it comes; "7" without am/pm the next of 7:00 and 19:00 (today or
     * tomorrow), as Alexa takes it; with a day said, the morning */
    time_t t;
    if (once_day != -1) {
        int d = once_day == -2 ? 1 : once_day == -3 ? 0 : (once_day - wday_of(now) + 7) % 7;
        if (once_day >= 0 && d == 0) d = 7;                         /* "on monday" said on a Monday: next week's */
        t = at_day(now, d, h, m);
        if (once_day == -3 && !ampm && !evening && t <= now && h < 12) t = at_day(now, 0, h + 12, m);
        if (t <= now) return -1;                                    /* "today at 7" when it is past 7 pm */
    } else {
        t = at_day(now, 0, h, m);
        if (!ampm && h >= 1 && h < 12 && t <= now) { time_t pm = at_day(now, 0, h + 12, m); t = pm > now ? pm : at_day(now, 1, h, m); }
        else if (t <= now) t = at_day(now, 1, h, m);
    }
    struct tm tm; localtime_r(&t, &tm);
    a.hour = tm.tm_hour; a.once = t;
    *out = a;
    return 0;
}

int alarms_days_parse(const char *s)
{
    struct toks k; tokens(s, &k);
    int days = 0;
    for (int i = 0; i < k.n; i++) {
        const char *w = k.t[i]; int d = day_name(w);
        if (d < 0) for (int j = 0; j < 7; j++) if (strlen(w) == 3 && !strncmp(w, wdays[j], 3)) d = j;
        if (d >= 0) days |= 1 << d;
        else if (!strcmp(w, "daily") || !strcmp(w, "everyday") || (!strcmp(w, "day") && is(&k, i - 1, "every"))) days |= DAYS_DAILY;
        else if (!strcmp(w, "weekdays") || !strcmp(w, "weekday")) days |= DAYS_WEEKDAYS;
        else if (!strcmp(w, "weekends") || !strcmp(w, "weekend")) days |= DAYS_WEEKENDS;
        else if (strcmp(w, "once") && strcmp(w, "every") && strcmp(w, "and") && strcmp(w, "on")) return -1;
    }
    return days;
}

/* ---------------------------------------------------------------- saying them */

void alarms_time_text(int hour, int min, int h24, char *out, size_t cap)
{
    if (h24) { snprintf(out, cap, "%02d:%02d", hour, min); return; }
    int h = hour % 12 ? hour % 12 : 12;
    if (min) snprintf(out, cap, "%d:%02d %s", h, min, hour < 12 ? "AM" : "PM");
    else snprintf(out, cap, "%d %s", h, hour < 12 ? "AM" : "PM");
}

void alarms_days_text(int days, char *out, size_t cap)
{
    size_t n = 0; int c = 0;
    if (cap) out[0] = 0;
    if (!days) return;
    if (days == DAYS_DAILY) { snprintf(out, cap, "every day"); return; }
    if (days == DAYS_WEEKDAYS) { snprintf(out, cap, "weekdays"); return; }
    if (days == DAYS_WEEKENDS) { snprintf(out, cap, "weekends"); return; }
    for (int i = 0; i < 7; i++) {
        int d = (i + 1) % 7;                                       /* Monday first */
        if (days & 1 << d && n < cap) n += (size_t)snprintf(out + n, cap - n, "%s%s", c++ ? ", " : "", Wd3[d]);
    }
}

void alarms_format(const struct alarm *a, time_t now, int h24, char *out, size_t cap)
{
    char t[24]; alarms_time_text(a->hour, a->min, h24, t, sizeof t);
    if (a->days) {
        if (a->days == DAYS_DAILY) { snprintf(out, cap, "%s every day", t); return; }
        if (a->days == DAYS_WEEKDAYS || a->days == DAYS_WEEKENDS) { snprintf(out, cap, "%s on %s", t, a->days == DAYS_WEEKDAYS ? "weekdays" : "weekends"); return; }
        size_t n = (size_t)snprintf(out, cap, "%s on ", t); int c = 0, all = __builtin_popcount((unsigned)a->days);
        for (int i = 0; i < 7 && n < cap; i++) {
            int d = (i + 1) % 7;
            if (a->days & 1 << d) { c++; n += (size_t)snprintf(out + n, cap - n, "%s%ss", c == 1 ? "" : c == all ? " and " : ", ", Wdays[d]); }
        }
        return;
    }
    time_t when = a->once; struct tm w, n; localtime_r(&when, &w); localtime_r(&now, &n);
    time_t d0 = at_day(now, 0, 0, 0), d1 = at_day(now, 1, 0, 0), d2 = at_day(now, 2, 0, 0), d7 = at_day(now, 7, 0, 0);
    if (when >= d0 && when < d1) snprintf(out, cap, "%s today", t);
    else if (when >= d1 && when < d2) snprintf(out, cap, "%s tomorrow", t);
    else if (when >= d1 && when < d7) snprintf(out, cap, "%s on %s", t, Wdays[w.tm_wday]);
    else snprintf(out, cap, "%s on %s %d", t, Mon3[w.tm_mon], w.tm_mday);
}

/* ---------------------------------------------------------------- what was asked */

enum alarm_cmd alarms_command(const char *text, time_t now, struct alarm *a, int *has_time)
{
    struct toks k; tokens(text, &k);
    *has_time = 0;
    memset(a, 0, sizeof *a);
    if (!k.n) return ALARM_CMD_NONE;
    /* the snooze words of a few languages: Alexa answers "snooze" alone, as "stop" */
    if (has(&k, "snooze") || has(&k, "snoozen") || has(&k, "schlummern") || has(&k, "schlummer") || (k.n <= 3 && has(&k, "repeter")))
        return ALARM_CMD_SNOOZE;
    int alarm = 0, plural = 0;
    for (int i = 0; i < k.n; i++) {
        if (!strcmp(k.t[i], "alarm") || !strcmp(k.t[i], "alarms") || !strcmp(k.t[i], "alarmclock")) { alarm = 1; plural |= k.t[i][5] == 's'; }
        /* an alarm system is Home Assistant's: "disarm the alarm", "arm the alarm", "alarm panel", "security alarm" */
        if (!strcmp(k.t[i], "arm") || !strcmp(k.t[i], "disarm") || !strcmp(k.t[i], "armed") || !strcmp(k.t[i], "system")
            || !strcmp(k.t[i], "panel") || !strcmp(k.t[i], "security") || !strcmp(k.t[i], "burglar") || !strcmp(k.t[i], "intruder")
            || !strcmp(k.t[i], "smoke") || !strcmp(k.t[i], "fire") || !strcmp(k.t[i], "car") || !strcmp(k.t[i], "house")) return ALARM_CMD_NONE;
    }
    int wake = has(&k, "wake") && (has(&k, "me") || has(&k, "us"));
    if (!alarm && !wake) return ALARM_CMD_NONE;
    struct alarm p; int timed = !alarms_parse(text, now, &p);
    /* delete: cancel, delete, remove, clear; "turn off" / "disable" / "stop" only with "my" or a time ("turn off the
     * alarm" while one rings is a stop, which main.c sees as one) */
    int del = has(&k, "cancel") || has(&k, "delete") || has(&k, "remove") || has(&k, "clear") || has(&k, "erase")
              || (((has(&k, "turn") && has(&k, "off")) || has(&k, "disable") || has(&k, "unset")) && (has(&k, "my") || has(&k, "all") || timed || plural));
    if (alarm && del) {
        if (has(&k, "all") || (plural && !timed) || has(&k, "every")) return ALARM_CMD_DELETE_ALL;
        if (timed) { *a = p; *has_time = 1; }
        return ALARM_CMD_DELETE;
    }
    if (alarm && (has(&k, "what") || has(&k, "when") || has(&k, "which") || has(&k, "list") || has(&k, "show") || has(&k, "any")
                  || (has(&k, "do") && has(&k, "have")) || (has(&k, "is") && !timed) || (has(&k, "are") && !timed) || has(&k, "next")))
        return ALARM_CMD_LIST;
    /* "change my alarm to 6:30", "move the alarm to 7", "push my alarm to 7:15": the one there is, not a new one */
    if (timed && alarm && (has(&k, "change") || has(&k, "move") || has(&k, "update") || has(&k, "reschedule") || has(&k, "push")
                           || has(&k, "shift") || has(&k, "switch") || has(&k, "edit"))) { *a = p; *has_time = 1; return ALARM_CMD_CHANGE; }
    if (timed) { *a = p; *has_time = 1; return ALARM_CMD_SET; }
    /* "set an alarm" without a time we could read: said as one, answered as one ("at what time?" has no follow-up here) */
    if (wake || has(&k, "set") || has(&k, "create") || has(&k, "make") || has(&k, "add") || has(&k, "new")) return ALARM_CMD_SET;
    return ALARM_CMD_NONE;
}

/* ---------------------------------------------------------------- the Echo's alarms */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct alarm list[ALARMS_MAX];
static int nlist, started, clock_format;
static struct alarms_hooks hooks;

/* tests: HASSMIC_FAKE_CLOCK_OFFSET seconds added to the clock, or a file holding them (a path: the test moves the
 * Echo's clock as it goes), so an alarm's minute comes within seconds */
time_t alarms_now(void)
{
    const char *e = getenv("HASSMIC_FAKE_CLOCK_OFFSET"); long off = 0;
    if (e && *e == '/') { FILE *f = fopen(e, "r"); if (f) { if (fscanf(f, "%ld", &off) != 1) off = 0; fclose(f); } }
    else if (e) off = atol(e);
    return time(NULL) + off;
}
#define now_ alarms_now

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

int alarms_clock_ok(void) { return clock_synced() || now_() > 1767225600; }    /* 2026-01-01: no RTC's default */

static void save(void)                  /* lock held */
{
    char p[300]; struct statefile s;
    snprintf(p, sizeof p, "%s/alarms", state_dir());
    if (statefile_open(&s, p, 0600)) { fprintf(stderr, "alarms: cannot write %s.tmp\n", p); return; }
    fprintf(s.f, "# hassmic alarms: id hour min days enabled once snoozed fired label\n");
    for (int i = 0; i < nlist; i++) {
        const struct alarm *a = &list[i];
        fprintf(s.f, "%d %d %d %d %d %lld %lld %lld %s\n", a->id, a->hour, a->min, a->days, a->enabled,
                (long long)a->once, (long long)a->snoozed, (long long)a->fired, a->label);
    }
    if (statefile_commit(&s)) fprintf(stderr, "alarms: cannot write %s\n", p);
}

static void load(void)                  /* lock held */
{
    char p[300], line[256]; FILE *f;
    snprintf(p, sizeof p, "%s/alarms", state_dir());
    nlist = 0;
    if (!(f = fopen(p, "r"))) return;
    while (fgets(line, sizeof line, f) && nlist < ALARMS_MAX) {
        struct alarm a; long long o, s, fi; int lab = 0;
        if (line[0] == '#') continue;
        memset(&a, 0, sizeof a);
        if (sscanf(line, "%d %d %d %d %d %lld %lld %lld %n", &a.id, &a.hour, &a.min, &a.days, &a.enabled, &o, &s, &fi, &lab) < 8
            || a.id <= 0 || a.hour < 0 || a.hour > 23 || a.min < 0 || a.min > 59 || a.days < 0 || a.days > DAYS_DAILY) continue;
        a.once = (time_t)o; a.snoozed = (time_t)s; a.fired = (time_t)fi; a.enabled = a.enabled != 0;
        if (lab) { snprintf(a.label, sizeof a.label, "%s", line + lab); a.label[strcspn(a.label, "\r\n")] = 0; }
        list[nlist++] = a;
    }
    fclose(f);
}

static int by_time(const void *x, const void *y)
{
    const struct alarm *a = x, *b = y;
    return a->hour * 60 + a->min != b->hour * 60 + b->min ? a->hour * 60 + a->min - (b->hour * 60 + b->min) : a->id - b->id;
}

static void changed(void) { if (hooks.changed) hooks.changed(); }

int alarms_list(struct alarm *out, int cap)
{
    pthread_mutex_lock(&lock);
    int n = nlist < cap ? nlist : cap;
    memcpy(out, list, sizeof *out * (size_t)n);
    pthread_mutex_unlock(&lock);
    qsort(out, (size_t)n, sizeof *out, by_time);
    return n;
}

int alarms_put(const struct alarm *in)
{
    struct alarm a = *in; int id = -1;
    a.label[sizeof a.label - 1] = 0;
    for (char *c = a.label; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
    if (a.hour < 0 || a.hour > 23 || a.min < 0 || a.min > 59 || a.days < 0 || a.days > DAYS_DAILY) return -1;
    pthread_mutex_lock(&lock);
    if (!a.id) {
        if (nlist < ALARMS_MAX) {
            int top = 0; for (int i = 0; i < nlist; i++) if (list[i].id > top) top = list[i].id;
            a.id = top + 1; a.snoozed = 0; a.fired = 0;
            list[nlist++] = a; id = a.id;
        }
    } else for (int i = 0; i < nlist; i++) if (list[i].id == a.id) {
        a.fired = list[i].fired;                                    /* a ring already had stays had */
        if (a.hour != list[i].hour || a.min != list[i].min || a.days != list[i].days || a.once != list[i].once || !a.enabled) a.snoozed = 0;
        else a.snoozed = list[i].snoozed;
        list[i] = a; id = a.id;
    }
    if (id > 0) save();
    pthread_mutex_unlock(&lock);
    if (id > 0) { char t[64]; alarms_format(&a, now_(), alarms_h24(), t, sizeof t); fprintf(stderr, "alarms: %d at %s%s\n", id, t, a.enabled ? "" : " (off)"); changed(); }
    return id;
}

int alarms_delete(int id)
{
    int r = -1;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nlist; i++) if (list[i].id == id) { memmove(list + i, list + i + 1, sizeof *list * (size_t)(nlist - i - 1)); nlist--; r = 0; break; }
    if (!r) save();
    pthread_mutex_unlock(&lock);
    if (!r) { fprintf(stderr, "alarms: %d deleted\n", id); changed(); }
    return r;
}

int alarms_delete_all(void)
{
    pthread_mutex_lock(&lock);
    int n = nlist; nlist = 0;
    if (n) save();
    pthread_mutex_unlock(&lock);
    if (n) { fprintf(stderr, "alarms: all %d deleted\n", n); changed(); }
    return n;
}

void alarms_ended(int id, int minutes)
{
    int hit = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nlist; i++) if (list[i].id == id) {
        hit = 1;
        if (minutes > 0) list[i].snoozed = now_() + minutes * 60;
        else { list[i].snoozed = 0; if (!list[i].days) list[i].enabled = 0; }
    }
    if (hit) save();
    pthread_mutex_unlock(&lock);
    if (hit) { fprintf(stderr, "alarms: %d %s\n", id, minutes > 0 ? "snoozed" : "done"); changed(); }
}

time_t alarms_next(struct alarm *out)
{
    time_t now = now_(), best = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nlist; i++) {
        time_t t = alarms_next_at(&list[i], now);
        if (t && (!best || t < best)) { best = t; if (out) *out = list[i]; }
    }
    pthread_mutex_unlock(&lock);
    return best;
}

/* the zone's name: bionic's property where hassmic runs, TZ on the PC */
static void zone_name(char *out, size_t cap)
{
    out[0] = 0;
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = ""; __system_property_get("persist.sys.timezone", v); snprintf(out, cap, "%s", v);
#endif
    if (!out[0] && getenv("TZ")) snprintf(out, cap, "%s", getenv("TZ") + (getenv("TZ")[0] == ':'));
}

int alarms_clock_format(int set) { if (set >= 0 && set <= 2) clock_format = set; return clock_format; }

int alarms_h24(void)
{
    if (clock_format) return clock_format == 2;
    static const char *const h12[] = { "America/", "US/", "Canada/", "Australia/", "Pacific/Auckland", "NZ", "Asia/Kolkata", "Asia/Calcutta",
                                       "Asia/Manila", "Asia/Karachi", "Asia/Dhaka", "Africa/Cairo", "Egypt", "Pacific/Honolulu", "EST", "PST", "CST", "MST" };
    char z[64]; zone_name(z, sizeof z);
    for (size_t i = 0; i < sizeof h12 / sizeof *h12; i++) if (!strncmp(z, h12[i], strlen(h12[i]))) return 0;
    return 1;
}

void alarms_summary(char *out, size_t cap, int h24)
{
    struct alarm a[ALARMS_MAX]; int n = alarms_list(a, ALARMS_MAX); size_t k = 0;
    if (cap) out[0] = 0;
    for (int i = 0; i < n && k < cap; i++) {
        if (!a[i].enabled) continue;
        char t[16], d[48]; alarms_time_text(a[i].hour, a[i].min, h24, t, sizeof t); alarms_days_text(a[i].days, d, sizeof d);
        k += (size_t)snprintf(out + k, cap - k, "%s%s%s%s%s%s%s", k ? ", " : "", t, d[0] ? " " : "", d,
                              a[i].label[0] ? " (" : "", a[i].label, a[i].label[0] ? ")" : "");
    }
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

char *alarms_json(size_t *n)
{
    struct alarm a[ALARMS_MAX]; int k = alarms_list(a, ALARMS_MAX), h24 = alarms_h24(); time_t now = now_();
    char *t = NULL; size_t len = 0; FILE *f = open_memstream(&t, &len);
    if (!f) return NULL;
    fprintf(f, "{\"alarms\":[");
    for (int i = 0; i < k; i++) {
        char txt[96]; alarms_format(&a[i], now, h24, txt, sizeof txt);
        fprintf(f, "%s{\"id\":%d,\"hour\":%d,\"min\":%d,\"days\":%d,\"enabled\":%s,\"once\":%lld,\"snoozed\":%lld,\"next\":%lld,\"label\":",
                i ? "," : "", a[i].id, a[i].hour, a[i].min, a[i].days, a[i].enabled ? "true" : "false", (long long)a[i].once,
                (long long)(a[i].snoozed > now ? a[i].snoozed : 0), (long long)alarms_next_at(&a[i], now));
        json_str(f, a[i].label); fprintf(f, ",\"text\":"); json_str(f, txt); fputc('}', f);
    }
    fprintf(f, "],\"next\":%lld,\"now\":%lld,\"clock_ok\":%s,\"h24\":%s,\"max\":%d}", (long long)alarms_next(NULL), (long long)now,
            alarms_clock_ok() ? "true" : "false", h24 ? "true" : "false", ALARMS_MAX);
    if (fclose(f)) { free(t); return NULL; }
    *n = len;
    return t;
}

int alarms_put_text(const char *line, char *err, size_t cap)
{
    struct alarm a; int lab = 0;
    memset(&a, 0, sizeof a);
    if (sscanf(line, "%d %d %d %d %d %n", &a.id, &a.hour, &a.min, &a.days, &a.enabled, &lab) < 5 || a.id < 0
        || a.hour < 0 || a.hour > 23 || a.min < 0 || a.min > 59 || a.days < 0 || a.days > DAYS_DAILY) {
        snprintf(err, cap, "not an alarm: id hour min days enabled label"); return -1;
    }
    a.enabled = a.enabled != 0;
    if (lab) snprintf(a.label, sizeof a.label, "%s", line + lab);
    a.label[strcspn(a.label, "\r\n")] = 0;
    if (!a.days) {                                                  /* once: the next time it comes */
        time_t now = now_(), t = at_day(now, 0, a.hour, a.min);
        a.once = t > now ? t : at_day(now, 1, a.hour, a.min);
    }
    if (alarms_put(&a) < 0) { snprintf(err, cap, a.id ? "no alarm %d" : "%d alarms at most", a.id ? a.id : ALARMS_MAX); return -1; }
    return 0;
}

static void *clock_thread(void *arg)
{
    (void)arg;
    time_t last = 0; int waited = 0;
    for (;;) {
        time_t now = now_();
        if (!alarms_clock_ok()) { if (!waited++) fprintf(stderr, "alarms: waiting for the clock (Home Assistant sets it)\n"); last = 0; sleep(1); continue; }
        if (!last) last = now - RING_BACK;                          /* a start: what a restart may have missed */
        else if (now - last > JUMP || now < last) { fprintf(stderr, "alarms: the clock moved by %lld s\n", (long long)(now - last)); last = now - 1; }
        int due_id[ALARMS_MAX], n = 0; char what[ALARMS_MAX][64];
        pthread_mutex_lock(&lock);
        for (int i = 0; i < nlist; i++) {
            struct alarm *a = &list[i];
            time_t t = alarms_next_at(a, last);
            if (!t || t > now) continue;
            if (t == a->snoozed) a->snoozed = 0;
            a->fired = t;
            due_id[n] = a->id;
            if (a->label[0]) snprintf(what[n], sizeof what[n], "%s", a->label);
            else alarms_time_text(a->hour, a->min, alarms_h24(), what[n], sizeof what[n]);
            n++;
        }
        if (n) save();
        pthread_mutex_unlock(&lock);
        for (int i = 0; i < n; i++) { fprintf(stderr, "alarms: %d (%s) is due\n", due_id[i], what[i]); if (hooks.ring) hooks.ring(due_id[i], what[i]); }
        if (n) changed();
        /* the next minute's turn also changes what "next" is for a once alarm: the display reads it itself */
        last = now;
        usleep(500000);
    }
    return NULL;
}

void alarms_start(const struct alarms_hooks *h)
{
    pthread_mutex_lock(&lock);
    int go = !started; started = 1;
    if (h) hooks = *h;
    if (go) load();
    pthread_mutex_unlock(&lock);
    if (!go) return;
    fprintf(stderr, "alarms: %d kept\n", nlist);
    pthread_t t;
    if (!pthread_create(&t, NULL, clock_thread, NULL)) pthread_detach(t);
}
