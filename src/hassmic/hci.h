/* The controller thread (ble.c) shared with the Bluetooth speaker (a2dp.c).  Everything here runs on that thread. */
#ifndef HCI_H
#define HCI_H
#include <stddef.h>
#include <stdint.h>

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

/* hfp.c, called by a2dp.c: the hands-free unit on a2dp.c's links (LINK: its index of the link) */
#define HFP_CHANNEL 1                                       /* our RFCOMM server channel, in the SDP record */
int  hfp_listed(void);                                      /* the feature is on: the SDP record, RFCOMM's PSM, the
                                                               class of device's telephony bit */
void hfp_rfcomm_up(int link, uint64_t addr);                /* an RFCOMM L2CAP channel opened on LINK */
void hfp_rfcomm_rx(int link, const unsigned char *p, size_t n);
void hfp_rfcomm_down(int link);
int  hfp_sco_request(uint64_t addr, int esco);              /* a synchronous link asked for: 0 refuse, else the codec
                                                               to accept it for (scoaudio.h SCO_CVSD, SCO_MSBC) */
void hfp_sco_up(int status, unsigned handle, uint64_t addr, int air);   /* air: the event's air mode (2 CVSD, 3 transparent) */
unsigned hfp_sdp_features(void);
void hfp_sco_unanswered(void);                              /* the accept for the link just asked for was not sent */                            /* the Hands-Free record's SupportedFeatures */
int  hfp_sco_down(unsigned handle);                         /* 1: it was the call's */
int  hfp_upkeep(void);                                      /* -1 = controller gone */
int  hfp_busy(void);
void hfp_lost(void);
/* a2dp.c, for hfp.c */
void a2dp_rfcomm_send(int link, const void *frame, size_t n);    /* on LINK's RFCOMM channel */
void a2dp_later(unsigned op, const void *par, unsigned n);       /* an HCI command, sent in upkeep */
unsigned a2dp_link_handle(int link);
const char *a2dp_link_name(int link);                       /* the device's name, "" until known */
#endif
