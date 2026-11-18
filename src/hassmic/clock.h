/* The Echo's clock and the time on every log line.  See clock.c. */
#ifndef CLOCK_H
#define CLOCK_H
#include <stddef.h>
#include <stdint.h>

/* From here on every line hassmic (and what it runs) writes to stderr starts with when it was written.  Call once, early,
 * before any thread starts.  The log keeps its lines when hassmic dies: a process of its own writes them out. */
void clock_log_start(void);
/* Home Assistant's time (GetTimeResponse, over the connection with the device key): handed to root to set the clock,
 * when it is off or not set since boot.  Any lock may be held. */
void clock_from_ha(uint32_t epoch);
/* Home Assistant's time zone (GetTimeResponse field 2: a POSIX TZ string; same link): handed to root as a zone name of
 * the Echo's time zone data (tz.c), or the string itself where none has that rule, when it is not the zone set.  Any
 * lock may be held: the work is done in a thread of its own, after this returns. */
void clock_zone_from_ha(const char *posix);
int  clock_zone(char *out, size_t cap);         /* the zone known here (the country's of Home Assistant's home or of
                                                   the Wi-Fi, or one set and kept; never a guess), "" and 0 none.  Any
                                                   thread: reads state/zone */
int  clock_zone_info(char *zone, size_t cap, const char **how, char cc[3][3]);    /* the zone, how ("home", "wifi", "kept",
                                                   "guess", "rule"), and the countries: the zone's, the home's, the
                                                   Wi-Fi's ("" none); 0 none yet.  For the page */
/* Home Assistant's home location (zone.home's latitude and longitude, keyed link): its country names the zone (geo.c).
 * 0, 0 or out of range: none.  Any lock may be held. */
void clock_home(double lat, double lon);
void clock_home_expect(void);                   /* zone.home asked for: the zone waits up to 2 s for it (once a run) */
int  clock_synced(void);                        /* root set the clock from Home Assistant since boot */
#endif
