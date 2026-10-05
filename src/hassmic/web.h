/* The settings page: HTTP on one port (default 28931, inside the firewall's 16384-32767), served by hassmic itself.
 * See web.c for the login (approved with the action button) and how requests are signed. */
#ifndef WEB_H
#define WEB_H

struct web_hooks {
    void (*attention)(int on);          /* a login waits for the action button: show it on the ring (and stop) */
    void (*approved)(int ok);           /* the press approved one (1), or a login was refused or ran out (0) */
};

int  web_start(int port, const struct web_hooks *h);     /* 0 = listening */
/* The action button: 1 if a login was waiting and is approved now (the press is used up), 0 otherwise.  Any lock. */
int  web_approve(void);
#endif
