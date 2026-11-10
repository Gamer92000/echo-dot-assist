/* Music ducked on the other Echos that heard the wake word (issue #165).  See duck.c. */
#ifndef DUCK_H
#define DUCK_H
#include <stddef.h>

int  duck_start(void);                          /* the sender's thread; after arb_start.  -1: not started */
int  duck_for_others(int set);                  /* the setting duck_for_others: 0/1, -1 only reads.  Any lock */
/* Our own run started (1) or ended (0): core_set_state, core_lock held.  Never blocks */
void duck_busy(int busy);
/* Capture thread: a round of ours runs and is decided at due (monotonic ms); the music goes down from now on.
 * duck_won: we answer, the round's duck is over (our own state ducks from here) */
void duck_round(long long due);
void duck_won(void);
/* A member's message (arb.c hooks->message, its thread): 1 if it was ours, 0 for the next module (Drop In) */
int  duck_message(const unsigned char from[8], const char *node, const unsigned char *p, size_t n);
/* The music goes down now: our own run, our round, or another Echo answering what we heard too.  Any thread, no lock */
int  duck_active(void);
#endif
