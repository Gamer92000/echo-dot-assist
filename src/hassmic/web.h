/* The settings page: HTTP on one port (default 28931, inside the firewall's 16384-32767), served by hassmic itself.
 * See web.c for the login (approved with the action button) and how requests are signed. */
#ifndef WEB_H
#define WEB_H

struct web_hooks {
    void (*attention)(int what);        /* something waits for the action button: show it on the ring.  1 a login or
                                         * adb, 2 a plugin's install or a copied plugin's note (a pattern of its own);
                                         * 0 nothing any more */
    void (*approved)(int ok);           /* the press (a note: the hold) approved one (1), or a login was refused or ran out (0) */
};

int  web_start(int port, const struct web_hooks *h);     /* 0 = listening */
/* The action button: 1 if a login was waiting and is approved now (the press is used up), 0 otherwise.  Any lock. */
int  web_approve(void);
/* The action button held for a second, down since DOWN_MS (monotonic ms): 1 if a copied plugin's note waited for
 * that, armed before the button went down, and is dismissed now (the hold is used up: not Home Assistant's "Button"
 * event), 0 otherwise.  A press never serves that wait.  Any lock. */
int  web_hold(long long down_ms);
/* A wait for the action button that is not this page's (a phone's setup, improv.c) is over, by the press or without
 * one: no login of a browser that is not approved is armed for the quiet time after it, as after a wait of the
 * page's, and one armed before it ends.  Only leaves a mark (web.c applies it before it next looks at its waits):
 * takes no lock and waits for nothing, any thread, any lock held (improv.c's is). */
void web_quiet(void);
/* Such a wait began: what a restart of hassmic is to keep of it goes onto the flash now (web.c hold_save).  As
 * web_quiet: no lock, nothing waited for. */
void web_phone(void);
/* A plugin's package from the push port (ota.c, web.c "packages the Echo brings itself"), installed only where
 * nothing is to be accepted; blocks until it is installed or refused, RES the port's answer */
#include <stddef.h>
#include <stdint.h>
int  web_plugin_push(const uint8_t *data, size_t len, char *res, size_t cap);
#endif
