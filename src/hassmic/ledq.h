/* The ring's ledctrl calls (issue #40): one at a time, in the order asked, each waited for, in a thread of its own so
 * the caller never waits for ledcontroller.  ring.c's patterns go through it (main.c led_run). */
#ifndef LEDQ_H
#define LEDQ_H

#define LEDQ_MAX 64                     /* calls queued at most */
#define LEDQ_TIMEOUT_MS 2000            /* a ledctrl still running after this is killed, and the next one runs */

/* ledctrl: the binary (NULL: none); fake: a file each call is appended to instead ("<op> <pattern>\n", the tests on the
 * PC; wins over ledctrl); child: run in the child before exec (NULL: nothing).  Again to change them; 0, or -1 when the
 * thread cannot start (the calls are dropped then) */
int  ledq_start(const char *ledctrl, const char *fake, void (*child)(void), int timeout_ms);
void ledq_run(const char *op, const char *pattern);
int  ledq_wait(int ms);                 /* the tests: 0 once all queued calls ran, -1 after ms */
#endif
