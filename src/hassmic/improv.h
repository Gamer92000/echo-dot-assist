/* Setting the Echo up from a phone: Improv Wi-Fi over Bluetooth LE (https://www.improv-wifi.com/ble/), which the Home
 * Assistant app and Home Assistant's improv_ble integration speak.  See improv.c. */
#ifndef IMPROV_H
#define IMPROV_H

struct improv_hooks {
    void (*attention)(int on);          /* a phone waits for the action button (authorization) */
    void (*authorized)(void);           /* the button was pressed for it (no lock of improv.c's held) */
};

/* Advertises whenever the Echo needs it (improv.c says when); one thread.  Nothing without a Bluetooth controller */
void improv_start(const struct improv_hooks *h);
/* Action button: 1 if a connected phone waits for it (an Echo that is set up, without a network: the ring shows
 * it), the press is taken; never with no phone there */
int  improv_authorize(void);
/* 1 while a connected phone waits for that press (the ring shows it): any thread, no lock */
int  improv_waits(void);
/* 1 while the button is a phone's setup's business: on an Echo that is set up and offers the setup for its lost
 * network (improv.c's M_LOST, never else), a phone connected, or the press's time, a switch or "provisioned"
 * running.  web.c arms no stranger's login meanwhile, and keeps that over a restart.  Any thread, no lock */
int  improv_holds(void);
#endif
