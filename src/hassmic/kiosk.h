/* The home app's side of the Echo Show's screen (docs/screen-apps.md): what an app made for hassmic gets and claims.
 * screen.c's UI decides which app has the screen (the home app the Screen page or Echo Local picked) and starts it; this
 * is the rest of the contract, served over HTTP on 127.0.0.1 to that app alone (web.c):
 *
 *   - its config: Home Assistant's address, a login made for this Echo by Echo Local, its screens and settings
 *     (state/kiosk, from POST /api/kiosk, signed, the token sealed);
 *   - its claims: what it draws itself while it is in front (the request's words, a ring), said in the path it polls
 *     (/kiosk/config/front/draws/<words,ring>); the UI leaves a ring to an app that claims it, else brings its face
 *     with Stop over the app until the ring ends (screen_claim);
 *   - what to draw: /kiosk/state (a request's words, what rings, the next alarm), a long poll. */
#ifndef KIOSK_H
#define KIOSK_H
#include <stddef.h>

#define KIOSK_DRAWS_WORDS 1
#define KIOSK_DRAWS_RING  2

void  kiosk_load(void);                                  /* state/kiosk at start */
int   kiosk_store(const char *text, char *err, size_t cap);   /* POST /api/kiosk's lines (token unsealed): 0, or -1 + err */
char *kiosk_json(int with_token, size_t *len);           /* malloc'd; with_token: the app's own answer (loopback) */
int   kiosk_peer_ok(int fd);                             /* the loopback connection on fd is the home app's (its uid) */
int   kiosk_draws_parse(const char *t);                  /* "/draws[/words,ring]" after /front: KIOSK_DRAWS_* */
void  kiosk_polled(int front, int draws);                /* the app polled /front (resumed, claiming draws) or /back */
int   kiosk_screen_names(char *out, size_t cap);         /* its screens' names, tab-separated (not hassmic: ones) */

/* what the screen shows, for an app that draws it itself (GET /kiosk/state[/<v>]) */
void  kiosk_run(int active);                             /* a request runs (any state but IDLE) */
void  kiosk_heard(const char *text);
void  kiosk_reply(const char *text);
void  kiosk_ringing(const char *what);                   /* what rings now (a timer's or an alarm's name), NULL nothing */
void  kiosk_changed(void);                               /* the alarms or the clock changed */
size_t kiosk_state_json(char *out, size_t cap);
void  kiosk_state_wait(unsigned v, int ms);              /* until the state's version is not v, at most ms */
#endif
