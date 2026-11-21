/* The Echo Wall Clock: its time and the Echo's timers on its LED ring, as stock Alexa shows them, over the Alexa Gadgets
 * protocol on an RFCOMM channel (a2dp.c keeps the link, rfcomm.c the channel).  See wallclock.c. */
#ifndef WALLCLOCK_H
#define WALLCLOCK_H
#include <stddef.h>
#include <time.h>

enum { WC_RUNNING, WC_PAUSED, WC_RINGING };
struct wc_timer { char id[40]; int state; long long end_ms; };  /* end_ms: CLOCK_MONOTONIC, ms (running ones) */

/* any thread, any lock */
void wallclock_timers(const struct wc_timer *t, int n);        /* every timer there is now (the core: running, paused,
                                                                   ringing), replacing what was told before */
void wallclock_tz(const char *posix);                          /* Home Assistant's time zone, a POSIX TZ string */
void wallclock_poke(void (*poke)(void));                       /* called when there is something to send (a2dp.c) */

/* the controller thread: the session on an open RFCOMM channel */
void wallclock_up(int (*send)(void *ctx, const void *p, size_t n), void *ctx, long long now_ms);
void wallclock_down(void);
void wallclock_rx(const unsigned char *p, size_t n, long long now_ms);
int  wallclock_tick(long long now_ms);                          /* -1: no answer to Discover in time (drop the link) */
int  wallclock_ready(void);                                     /* it answered Discover: a clock is on the line */
void wallclock_name(char *buf, size_t n);                       /* what it called itself ("" none yet) */

/* for the tests: the local time of `utc` by a POSIX TZ rule (offset east of UTC in seconds, -1 if not understood: 0
 * returned then), and the ISO 8601 text the clock takes ("2026-10-14T10:44:19-04:00") */
int  wallclock_offset(const char *posix, time_t utc, int *east);
void wallclock_iso(char *out, size_t cap, time_t utc, const char *posix);
void wallclock_pace(int ms);                                    /* at least this long between two messages (0) */
void wallclock_fake_time(long long real_ms);                    /* the wall clock's time from here on (0: the real one) */
size_t wallclock_frame(unsigned char *out, size_t cap, int seq, const unsigned char *payload, size_t n);
#endif
