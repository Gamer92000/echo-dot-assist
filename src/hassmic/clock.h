/* The Echo's clock and the time on every log line.  See clock.c. */
#ifndef CLOCK_H
#define CLOCK_H
#include <stdint.h>

/* From here on every line hassmic (and what it runs) writes to stderr starts with when it was written.  Call once, early,
 * before any thread starts.  The log keeps its lines when hassmic dies: a process of its own writes them out. */
void clock_log_start(void);
/* Home Assistant's time (GetTimeResponse, over the connection with the device key): handed to root to set the clock,
 * when it is off or not set since boot.  Any lock may be held. */
void clock_from_ha(uint32_t epoch);
int  clock_synced(void);                        /* root set the clock from Home Assistant since boot */
#endif
