/* Alarms of the Echo's own (alarms.c): set by voice ("wake me up at 6:30"), from Home Assistant (the actions
 * esphome.<node>_set_alarm / _delete_alarm) or on the settings page; kept in state/alarms, rung by the core's timer ring
 * (main.c core_alarm_ring) on the Echo's local time, without Home Assistant.  Home Assistant has timers only.
 *
 * The parts without a clock or a lock (alarms_parse, alarms_command, alarms_next_at, alarms_format) are pure, for
 * tests/unit/alarms_test.c. */
#ifndef ALARMS_H
#define ALARMS_H
#include <stddef.h>
#include <time.h>

#define ALARMS_MAX   16
#define ALARM_LABEL  48

/* days: bit d for tm_wday d (0 Sunday .. 6 Saturday); 0 = once, on `once` */
struct alarm {
    int id;                             /* 1.., stable: the page and Home Assistant name it by this */
    int hour, min;
    int days;
    int enabled;
    time_t once;                        /* days == 0: the one moment it rings (local time when set) */
    time_t snoozed;                     /* rings again then (0: not snoozed) */
    time_t fired;                       /* the last moment it rang for: never twice for one, across restarts */
    char label[ALARM_LABEL];
};

enum { DAYS_DAILY = 0x7f, DAYS_WEEKDAYS = 0x3e, DAYS_WEEKENDS = 0x41 };

/* ---------------------------------------------------------------- pure */

/* When `a` rings next after `after` (local time, by mktime, so DST is the zone's), 0: never (disabled, a once past) */
time_t alarms_next_at(const struct alarm *a, time_t after);

/* "7", "7:30", "7.30 pm", "19:00", "noon", "seven thirty", "half past six", "quarter to 8"; with "tomorrow", "today",
 * "tonight", "on monday", "every day", "weekdays", "weekends", "every monday and friday"; or "in 20 minutes", "in an
 * hour and a half".  English words; digits whatever the language.  Fills hour, min, days and (for a once) once,
 * relative to `now`.  0, -1: not a time */
int alarms_parse(const char *text, time_t now, struct alarm *out);

/* Days as people say them: "every day", "weekdays", "weekends", "Mon, Wed, Fri", "" for once */
void alarms_days_text(int days, char *out, size_t cap);
/* Days as Home Assistant's action gives them: "", "once" 0; "daily" / "every day", "weekdays", "weekends", or day names
 * and their first three letters ("mon, wed, fri"); -1 none of these */
int  alarms_days_parse(const char *s);
/* "7:30 AM" or "07:30" (h24) */
void alarms_time_text(int hour, int min, int h24, char *out, size_t cap);
/* One alarm for a sentence: "7:30 AM tomorrow", "7 AM on weekdays", "6:45 AM on Monday" */
void alarms_format(const struct alarm *a, time_t now, int h24, char *out, size_t cap);

/* A request spoken to the Echo, what speech to text made of it.  ALARM_CMD_NONE: not about alarms (Home Assistant
 * answers it).  *a holds the time for SET, and for DELETE the one named (has_time) */
/* CHANGE: "change my alarm to 6:30", "move the alarm to 7": the next alarm to that time (its days kept) */
enum alarm_cmd { ALARM_CMD_NONE, ALARM_CMD_SET, ALARM_CMD_DELETE, ALARM_CMD_DELETE_ALL, ALARM_CMD_LIST, ALARM_CMD_SNOOZE, ALARM_CMD_CHANGE };
enum alarm_cmd alarms_command(const char *text, time_t now, struct alarm *a, int *has_time);

/* ---------------------------------------------------------------- the Echo's (alarms.c, own lock: any thread, core_lock
 * held or not; the callbacks into the core are made without alarms' lock) */

struct alarms_hooks {
    void (*ring)(int id, const char *what);     /* an alarm is due: what = its label or its time ("7:30 AM") */
    void (*changed)(void);                      /* the list or the next alarm changed (Home Assistant's sensors, the display) */
};
void alarms_start(const struct alarms_hooks *h);    /* loads state/alarms, starts the clock thread */
int  alarms_list(struct alarm *out, int cap);       /* a copy, by time of day */
/* Add one (id 0) or replace the one of that id: its id, -1 full or no such id */
int  alarms_put(const struct alarm *a);
int  alarms_delete(int id);                         /* 0, -1: none */
int  alarms_delete_all(void);                       /* how many went */
/* The ring of alarm `id` ended: snoozed for `minutes`, or (0) dismissed: a once alarm is switched off */
void alarms_ended(int id, int minutes);
/* The next to ring: its moment and the alarm (out may be NULL); 0 none */
time_t alarms_next(struct alarm *out);
/* The clock may be trusted: Home Assistant set it, or the RTC kept it.  No alarm rings without */
int  alarms_clock_ok(void);
time_t alarms_now(void);                            /* the clock alarms ring by (tests: HASSMIC_FAKE_CLOCK_OFFSET) */
/* "Alarms" for Home Assistant: "07:30 weekdays, 09:00 Sat (Gym)"; "" none */
void alarms_summary(char *out, size_t cap, int h24);
/* The clock's format, the settings' "clock_format" (0 auto, 1 12 h, 2 24 h): auto is 12 h in the Americas, Australia
 * and India (the zone's name), 24 h elsewhere */
int  alarms_h24(void);
int  alarms_clock_format(int set);                  /* -1 reads */

/* JSON for the settings page: {"alarms":[{"id":1,"hour":7,"min":30,"days":62,"enabled":true,"label":"",
 * "next":<epoch|0>,"text":"7:30 AM on weekdays"}],"next":<epoch|0>,"clock_ok":true,"h24":false} */
char *alarms_json(size_t *n);
/* The page's edit: "id hour min days enabled label..." (id 0 adds; days 0 = the next time it comes); 0 or -1 with err */
int  alarms_put_text(const char *line, char *err, size_t cap);
#endif
