/* Sendspin player: synchronised multiroom audio from Music Assistant.  Runs beside the voice protocol. */
#ifndef SENDSPIN_H
#define SENDSPIN_H
#include <stddef.h>
int  sendspin_start(int port);                          /* listener + player threads; the server dials us */
void sendspin_init(void);                               /* keys only: for printing the pairing token */
void sendspin_pairing_token(char *out, size_t outsz);   /* what the operator pastes into the server to pair */
int  sendspin_button(void);                             /* action button: 1 = used for play / pause */
int  sendspin_skip(int dir);                           /* action button pressed twice (+1: next) or three times (-1: previous): 1 = sent, only
                                                           while the group plays and the server allows it */
int  sendspin_stop(void);                               /* "<wake word>, stop": pause the group if it plays, the button
                                                           resumes it.  1 = paused */
void sendspin_pause(void);                              /* another source started: pause the group if it plays */
void sendspin_volume_changed(int percent);              /* volume changed locally: tell the server */
void sendspin_mdns(int port, char *out, size_t outsz);  /* <service> element for the avahi service file */
int  sendspin_unpaired(int set);                        /* unpaired servers may play: -1 asks, 0/1 sets (kept in the state) */
int  sendspin_linked(void);                             /* a server holds the player now */
int  sendspin_expected(void);                           /* a server held the player role here: it ought to come back */
int  sendspin_control(const char *cmd, long long arg);  /* a controller command (play, pause, next, previous, stop,
                                                           volume arg, mute arg, seek arg ms, repeat_*, shuffle,
                                                           unshuffle) if the server allows it: 0 sent */
const char *sendspin_client_id(void);                  /* this Echo's id towards Music Assistant (its player id there) */
#endif
