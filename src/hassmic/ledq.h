/* hassmic's ledctrl calls (issue #40): one at a time, in the order asked, each waited for, in a thread of its own so
 * the caller never waits for ledcontroller.  ring.c's patterns (main.c led_run) and the brightness go through it.
 *
 * The wait is a pipe ledctrl holds until it exits (ledq.c).  A fork that does not exec, in another thread, between its
 * pipe2 and close would keep that pipe open too: every call then waits out the timeout.  hassmic's only such fork
 * (clock.c) runs before ledq_start; keep it that way. */
#ifndef LEDQ_H
#define LEDQ_H

#define LEDQ_MAX 64                     /* calls queued at most */
#define LEDQ_ARGS 4                     /* arguments per call at most (-a off -b N) */
#define LEDQ_TIMEOUT_MS 2000            /* a ledctrl still running after this is killed, and the next one runs */
/* Patterns set and unset queued one behind the other go in one ledctrl, this many at most: it takes repeated -s and -u
 * and sends each in order (getopt loop), and a fill of 11 LEDs off the ring for a wake word costs one ~35 ms call
 * instead of eleven (issue #57) */
#define LEDQ_BATCH 32

/* ledctrl: the binary (NULL: none); fake: a file each call is appended to instead, at once ("<args>\n", the tests on
 * the PC; wins over ledctrl); child: run in the child before exec (NULL: nothing).  Again to change them; 0, or -1 when
 * the thread cannot start (the calls are dropped then) */
int  ledq_start(const char *ledctrl, const char *fake, void (*child)(void), int timeout_ms);
void ledq_run(const char *op, const char *pattern);         /* ledctrl <op> <pattern> */
void ledq_args(int n, const char *const args[]);            /* ledctrl <args...>, n <= LEDQ_ARGS */
int  ledq_wait(int ms);                 /* the tests: 0 once all queued calls ran, -1 after ms */
#endif
