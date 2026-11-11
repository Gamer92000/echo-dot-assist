/* Music ducked on the other Echos that heard the wake word (issue #165).  See duck.c. */
#ifndef DUCK_H
#define DUCK_H
#include <stddef.h>

int  duck_start(void);                          /* the sender's thread; after arb_start.  -1: not started */
int  duck_for_others(int set);                  /* the setting duck_for_others: 0/1, -1 only reads.  Any lock */
/* Our own run: busy while the state is not IDLE, Home Assistant's media aside (main.c).  core_lock held.  Never blocks */
void duck_busy(int busy);
/* A new run follows this one at once (a follow-up question, a wake word that cut the reply): the others stay down
 * across the idle moment between the two, DUCK_BRIDGE_MS at most.  Before the IDLE that ends this one; core_lock held */
void duck_follow(void);
/* Capture thread: a round of ours runs and is decided at due (monotonic ms); the music goes down from now on.
 * duck_won: we answer (round: after a round we won, whose losers wait for our "on"; 0: without one), the round's
 * own duck is over (our state ducks from here) */
void duck_round(long long due);
void duck_won(int round);
/* A member's message to every member (arb.c hooks->message, its thread, `all`) */
void duck_message(const unsigned char from[8], const char *node, const unsigned char *p, size_t n);
/* The music goes down now: our own run, our round, or another Echo answering what we heard too.  Any thread, no lock */
int  duck_active(void);
#endif
