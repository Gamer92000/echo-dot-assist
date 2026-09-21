/* Sendspin player: synchronised multiroom audio from Music Assistant.  Runs beside the voice protocol. */
#ifndef SENDSPIN_H
#define SENDSPIN_H
#include <stddef.h>
int  sendspin_start(int port);                          /* listener + player threads; the server dials us */
void sendspin_mdns(int port, char *out, size_t outsz);  /* <service> element for the avahi service file */
#endif
