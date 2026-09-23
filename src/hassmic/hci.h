/* The controller thread (ble.c) shared with the Bluetooth speaker (a2dp.c).  Everything here runs on that thread. */
#ifndef HCI_H
#define HCI_H
#include <stddef.h>

/* ble.c */
int  hci_cmd(unsigned op, const void *par, unsigned n);    /* waits for Command Complete / Status: HCI status, -1 = gone.
                                                               Never from inside event handling (it pumps events itself) */
const unsigned char *hci_ret(void);                         /* the last Command Complete's parameters after the status */
int  hci_write(const void *h4, size_t n);                   /* one H4 packet as it is */
void hci_poke(void);                                        /* any thread: wake the controller thread for upkeep */

/* a2dp.c, called by ble.c */
int  a2dp_setup(void);                                      /* after the reset; -1 = controller gone */
int  a2dp_event(const unsigned char *p, size_t n);          /* event code, length, parameters.  1 = a BR/EDR one, handled */
int  a2dp_acl(const unsigned char *p, size_t n);            /* ACL packet after the H4 byte.  1 = on a BR/EDR link */
int  a2dp_completed(unsigned handle, unsigned n);           /* Number Of Completed Packets.  1 = a BR/EDR link's */
int  a2dp_upkeep(void);                                     /* HCI commands the events asked for, timeouts.  -1 = gone */
int  a2dp_busy(void);                                       /* timers running: upkeep wanted even without events */
void a2dp_lost(void);                                       /* controller gone: every link with it */
int  a2dp_streaming(void);                                  /* audio flows: LE scanning pauses, the radio is busy enough */
#endif
