/* Music Assistant's own API (its websocket, port 8095), for the screen's music pages (src/ui/music.c): the library,
 * search, the queue, the players and their groups.  Sendspin (sendspin.c) plays and says what plays; this browses and
 * groups.  See mass.c. */
#ifndef MASS_H
#define MASS_H
#include <stddef.h>

void   mass_start(void);
void   mass_peer(const char *ip);              /* sendspin.c: the address a Music Assistant server played here from */
/* the settings page: the server's URL and a long-lived token ("" keeps the one there is, for the same URL only; "-"
 * forgets it).  A token only ever goes to the address it was saved with: with a token, a URL is needed (the page gives
 * the one it showed as found).  0 saved, -2 a token and no URL, -3 a kept token and another URL, -4 https, -1 not
 * written */
int    mass_set(const char *url, const char *token);
size_t mass_status_json(char *o, size_t cap);  /* {"state","text","url","found","token"}: the page's row */
void   mass_ui(const char *line);              /* a request of the screen's UI (scene.h: ml, mplay, mq, mg, mv, mgv, mhere, img) */
void   mass_replay(void);                      /* the UI started anew: what it was told, again */
void   mass_stop(void);                        /* the UI given up: no session until hassmic starts again */
#endif
