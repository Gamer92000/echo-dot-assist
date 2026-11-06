/* alarms.c without its clock thread: what requests are read as, when alarms ring (DST included), how they are said,
 * and what state/alarms keeps.  The time zone is New York's: 12 h, and DST ends on the Sunday the tests start on. */
#include "alarms.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int clock_synced(void) { return 1; }

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static time_t at(int y, int mo, int d, int h, int mi)
{
    struct tm tm = { 0 }; tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_isdst = -1;
    return mktime(&tm);
}

static void parse(const char *text, time_t now, int ok, int h, int m, int days, time_t once)
{
    struct alarm a; int r = alarms_parse(text, now, &a);
    if (!ok) { CHECK(r < 0, "\"%s\": not a time", text); return; }
    CHECK(!r && a.hour == h && a.min == m && a.days == days && (days || a.once == once),
          "\"%s\" -> %02d:%02d days %#x once %+lld min (got %d %02d:%02d %#x %+lld)", text, h, m, days,
          (long long)(once - now) / 60, r, a.hour, a.min, a.days, (long long)(a.once - now) / 60);
}

static void command(const char *text, time_t now, enum alarm_cmd want, int timed)
{
    struct alarm a; int ht; enum alarm_cmd c = alarms_command(text, now, &a, &ht);
    CHECK(c == want && ht == timed, "\"%s\" -> command %d%s (got %d, time %d)", text, want, timed ? " with a time" : "", c, ht);
}

int main(void)
{
    setenv("TZ", "America/New_York", 1); tzset();
    char dir[] = "/tmp/alarms_testXXXXXX";
    if (!mkdtemp(dir)) return 1;
    setenv("HASSMIC_STATE", dir, 1);

    time_t now = at(2026, 11, 1, 14, 0);                        /* Sunday 2 pm, the day DST ended */
    parse("set an alarm for 7 am", now, 1, 7, 0, 0, at(2026, 11, 2, 7, 0));
    parse("Set an alarm for 7 a.m.", now, 1, 7, 0, 0, at(2026, 11, 2, 7, 0));
    parse("wake me up at 6:30", now, 1, 18, 30, 0, at(2026, 11, 1, 18, 30));       /* the next 6:30: tonight */
    parse("set an alarm for 7:30 p.m.", now, 1, 19, 30, 0, at(2026, 11, 1, 19, 30));
    parse("alarm at 7 tomorrow", now, 1, 7, 0, 0, at(2026, 11, 2, 7, 0));
    parse("set an alarm for 10", now, 1, 22, 0, 0, at(2026, 11, 1, 22, 0));
    parse("set an alarm for noon", now, 1, 12, 0, 0, at(2026, 11, 2, 12, 0));
    parse("set an alarm for 0730", now, 1, 7, 30, 0, at(2026, 11, 2, 7, 30));
    parse("set an alarm for 7am", now, 1, 7, 0, 0, at(2026, 11, 2, 7, 0));
    parse("wake me up at seven thirty am", now, 1, 7, 30, 0, at(2026, 11, 2, 7, 30));
    parse("wake me at half past seven tomorrow", now, 1, 7, 30, 0, at(2026, 11, 2, 7, 30));
    parse("set an alarm for quarter to 8 in the morning", now, 1, 7, 45, 0, at(2026, 11, 2, 7, 45));
    parse("set an alarm for 19:15", now, 1, 19, 15, 0, at(2026, 11, 1, 19, 15));
    parse("set an alarm for 6.45 am", now, 1, 6, 45, 0, at(2026, 11, 2, 6, 45));
    parse("set an alarm tonight at 9", now, 1, 21, 0, 0, at(2026, 11, 1, 21, 0));
    parse("set an alarm for seven oh five a.m.", now, 1, 7, 5, 0, at(2026, 11, 2, 7, 5));
    parse("set an alarm for monday at 6", now, 1, 6, 0, 0, at(2026, 11, 2, 6, 0));
    parse("set an alarm for sunday at 6 am", now, 1, 6, 0, 0, at(2026, 11, 8, 6, 0));   /* said on a Sunday: next week's */
    parse("set an alarm for 6:45 on weekdays", now, 1, 6, 45, DAYS_WEEKDAYS, 0);
    parse("wake me up every day at 7", now, 1, 7, 0, DAYS_DAILY, 0);
    parse("set an alarm every monday and friday at 5:30 am", now, 1, 5, 30, 1 << 1 | 1 << 5, 0);
    parse("set an alarm for 9 am on weekends", now, 1, 9, 0, DAYS_WEEKENDS, 0);
    parse("set an alarm in 20 minutes", now, 1, 14, 20, 0, now + 20 * 60);
    parse("set an alarm in an hour and a half", now, 1, 15, 30, 0, now + 90 * 60);
    parse("wake me up in half an hour", now, 1, 14, 30, 0, now + 30 * 60);
    parse("set an alarm in 1 hour and 15 minutes", now, 1, 15, 15, 0, now + 75 * 60);
    parse("set 2 alarms", now, 0, 0, 0, 0, 0);
    parse("set an alarm", now, 0, 0, 0, 0, 0);
    parse("set an alarm for 25:00", now, 0, 0, 0, 0, 0);

    parse("07:30:00", now, 1, 7, 30, 0, at(2026, 11, 2, 7, 30));    /* Home Assistant's time selector */
    CHECK(alarms_days_parse("") == 0 && alarms_days_parse("once") == 0, "days: none is once");
    CHECK(alarms_days_parse("weekdays") == DAYS_WEEKDAYS && alarms_days_parse("every day") == DAYS_DAILY && alarms_days_parse("daily") == DAYS_DAILY, "days: words");
    CHECK(alarms_days_parse("mon, wed, Fri") == (1 << 1 | 1 << 3 | 1 << 5) && alarms_days_parse("Saturday and Sunday") == DAYS_WEEKENDS, "days: names");
    CHECK(alarms_days_parse("someday") < 0, "days: not days");
    command("snooze", now, ALARM_CMD_SNOOZE, 0);
    command("Snooze.", now, ALARM_CMD_SNOOZE, 0);
    command("set an alarm for 7 am", now, ALARM_CMD_SET, 1);
    command("wake me up at 6:30 tomorrow", now, ALARM_CMD_SET, 1);
    command("set an alarm", now, ALARM_CMD_SET, 0);
    command("Change alarm to twelve.", now, ALARM_CMD_CHANGE, 1);
    command("move my alarm to 6:30", now, ALARM_CMD_CHANGE, 1);
    command("change the alarm", now, ALARM_CMD_NONE, 0);
    command("cancel my alarm", now, ALARM_CMD_DELETE, 0);
    command("delete the 7 am alarm", now, ALARM_CMD_DELETE, 1);
    command("cancel all alarms", now, ALARM_CMD_DELETE_ALL, 0);
    command("turn off my alarms", now, ALARM_CMD_DELETE_ALL, 0);
    command("what alarms do I have", now, ALARM_CMD_LIST, 0);
    command("when is my alarm?", now, ALARM_CMD_LIST, 0);
    command("do I have any alarms", now, ALARM_CMD_LIST, 0);
    command("disarm the alarm", now, ALARM_CMD_NONE, 0);
    command("arm the alarm system", now, ALARM_CMD_NONE, 0);
    command("turn off the smoke alarm", now, ALARM_CMD_NONE, 0);
    command("turn on the kitchen lights", now, ALARM_CMD_NONE, 0);
    command("what time is it", now, ALARM_CMD_NONE, 0);
    command("set a timer for 7 minutes", now, ALARM_CMD_NONE, 0);

    /* when they ring */
    struct alarm a = { .id = 1, .hour = 6, .min = 45, .days = DAYS_WEEKDAYS, .enabled = 1 };
    CHECK(alarms_next_at(&a, now) == at(2026, 11, 2, 6, 45), "weekdays 6:45 from Sunday afternoon: Monday 6:45");
    a.fired = at(2026, 11, 2, 6, 45);
    CHECK(alarms_next_at(&a, at(2026, 11, 2, 6, 40)) == at(2026, 11, 3, 6, 45), "rung on Monday: never twice, Tuesday next");
    a.snoozed = at(2026, 11, 2, 6, 54);
    CHECK(alarms_next_at(&a, at(2026, 11, 2, 6, 46)) == a.snoozed, "snoozed: again at 6:54");
    a.enabled = 0;
    CHECK(alarms_next_at(&a, now) == 0, "off: never, snoozed or not");
    struct alarm o = { .id = 2, .hour = 7, .min = 0, .once = at(2026, 11, 2, 7, 0), .enabled = 1 };
    CHECK(alarms_next_at(&o, now) == o.once && alarms_next_at(&o, o.once) == 0, "once: at its moment, then never");
    struct alarm d = { .id = 3, .hour = 7, .min = 0, .days = DAYS_DAILY, .enabled = 1 };
    CHECK(alarms_next_at(&d, at(2026, 10, 31, 8, 0)) == at(2026, 11, 1, 7, 0), "daily 7:00 over the end of DST: 7:00 local");
    CHECK(at(2026, 11, 1, 7, 0) - at(2026, 10, 31, 7, 0) == 25 * 3600, "(that day has 25 hours)");
    struct alarm s = { .id = 4, .hour = 2, .min = 30, .days = DAYS_DAILY, .enabled = 1 };
    time_t t = alarms_next_at(&s, at(2026, 3, 8, 0, 0));
    struct tm tm; localtime_r(&t, &tm);
    CHECK(t > at(2026, 3, 8, 0, 0) && tm.tm_mday == 8 && tm.tm_hour == 3 && tm.tm_min == 30, "2:30 on the day 2:00 is skipped: 3:30 that day");
    CHECK(alarms_next_at(&s, t) == at(2026, 3, 9, 2, 30), "then 2:30 the next day");

    /* said */
    char buf[96];
    alarms_format(&o, now, 0, buf, sizeof buf); CHECK(!strcmp(buf, "7 AM tomorrow"), "\"%s\"", buf);
    alarms_format(&a, now, 0, buf, sizeof buf); CHECK(!strcmp(buf, "6:45 AM on weekdays"), "\"%s\"", buf);
    struct alarm mf = { .hour = 17, .min = 30, .days = 1 << 1 | 1 << 5, .enabled = 1 };
    alarms_format(&mf, now, 0, buf, sizeof buf); CHECK(!strcmp(buf, "5:30 PM on Mondays and Fridays"), "\"%s\"", buf);
    alarms_format(&mf, now, 1, buf, sizeof buf); CHECK(!strcmp(buf, "17:30 on Mondays and Fridays"), "\"%s\"", buf);
    struct alarm wk = { .hour = 9, .min = 0, .once = at(2026, 11, 5, 9, 0), .enabled = 1 };
    alarms_format(&wk, now, 0, buf, sizeof buf); CHECK(!strcmp(buf, "9 AM on Thursday"), "\"%s\"", buf);
    alarms_days_text(1 << 1 | 1 << 3 | 1 << 0, buf, sizeof buf); CHECK(!strcmp(buf, "Mon, Wed, Sun"), "days \"%s\"", buf);

    /* kept */
    struct alarm n1 = { .hour = 7, .min = 0, .days = DAYS_WEEKDAYS, .enabled = 1 };
    snprintf(n1.label, sizeof n1.label, "Work");
    struct alarm o2 = o; o2.id = 0;
    int id1 = alarms_put(&n1), id2 = alarms_put(&o2);
    CHECK(id1 == 1 && id2 == 2, "ids from 1 (%d, %d)", id1, id2);
    char err[96];
    CHECK(!alarms_put_text("0 6 15 65 1 Gym time", err, sizeof err), "the page's line adds one");
    struct alarm l[ALARMS_MAX]; int n = alarms_list(l, ALARMS_MAX);
    CHECK(n == 3 && l[0].hour == 6 && !strcmp(l[0].label, "Gym time") && l[1].hour == 7, "listed by time of day, labels kept");
    alarms_ended(id2, 9);
    alarms_ended(id1, 0);
    char p[300]; snprintf(p, sizeof p, "%s/alarms", dir);
    FILE *f = fopen(p, "r"); int lines = 0; char line[256];
    while (f && fgets(line, sizeof line, f)) lines += line[0] != '#';
    if (f) fclose(f);
    CHECK(lines == 3, "state/alarms: a line each");
    n = alarms_list(l, ALARMS_MAX);
    int snoozed = 0, work_on = 0;
    for (int i = 0; i < n; i++) { if (l[i].id == id2) snoozed = l[i].snoozed > time(NULL); if (l[i].id == id1) work_on = l[i].enabled; }
    CHECK(snoozed && work_on, "snoozed for 9 minutes; a repeating alarm stays on when stopped");
    alarms_ended(id2, 0);
    n = alarms_list(l, ALARMS_MAX);
    for (int i = 0; i < n; i++) if (l[i].id == id2) CHECK(!l[i].enabled && !l[i].snoozed, "a once alarm stopped is off");
    CHECK(!alarms_delete(id1) && alarms_delete(id1) < 0, "deleted once");
    CHECK(alarms_delete_all() == 2 && alarms_list(l, ALARMS_MAX) == 0, "all deleted");
    CHECK(alarms_put_text("1 7 0", err, sizeof err) < 0, "a short line refused: %s", err);

    remove(p); rmdir(dir);
    printf(fails ? "alarms: %d FAILED\n" : "alarms: all ok\n", fails);
    return fails != 0;
}
