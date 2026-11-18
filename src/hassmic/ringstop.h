/* "Stop" said near one Echo ends a timer ringing on another (issue #161).  See ringstop.c. */
#ifndef RINGSTOP_H
#define RINGSTOP_H
#include <stddef.h>

int  ringstop_start(void);                      /* the sender's thread; after arb_start.  -1: not started */
int  ringstop_from_others(int set);             /* the setting timer_stop_from_others: 0/1, -1 only reads.  Any lock */
void ringstop_ringing(int on);                  /* a timer rings here (main.c alarm_set); core_lock held, never blocks */
/* A "stop" here that ended nothing of this Echo's: the members that say they ring are told to stop.  The count of
 * them (0: nobody rings, or nothing can be sent: no thread, Kiosk Satellite's mode, no members); who: the first one's
 * name, for the log.  core_lock held; takes arb's lock briefly, never waits on the network */
int  ringstop_forward(char *who, size_t cap);
/* A member's message to every member (arb.c hooks->message, its thread, `all`, no lock held).  Takes arb's lock
 * (arb_mode) and ringstop's, never core_lock: a stop is carried out by ringstop's thread (core_alarm_stop_elsewhere) */
void ringstop_message(const unsigned char from[8], const char *node, const unsigned char *p, size_t n);
#endif
