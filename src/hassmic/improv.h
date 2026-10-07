/* Setting the Echo up from a phone: Improv Wi-Fi over Bluetooth LE (https://www.improv-wifi.com/ble/), which the Home
 * Assistant app and Home Assistant's improv_ble integration speak.  See improv.c. */
#ifndef IMPROV_H
#define IMPROV_H

struct improv_hooks {
    void (*attention)(int on);          /* a phone waits for the action button (authorization) */
    void (*authorized)(void);           /* the button was pressed for it */
};

/* Advertises whenever the Echo needs it (improv.c says when); one thread.  Nothing without a Bluetooth controller */
void improv_start(const struct improv_hooks *h);
/* Action button: 1 if Improv waits for it (an Echo that is set up, without a network), the press is taken */
int  improv_authorize(void);
#endif
