/* Bluetooth speaker: A2DP sink (SBC) on the controller ble.c drives, beside the Bluetooth LE proxy. */
#ifndef A2DP_H
#define A2DP_H

#define A2DP_PAIR_SECONDS 120

void a2dp_start(void (*changed)(void));    /* takes the radio (ble.c's thread) if nothing has yet; changed(): a2dp_pairing()
                                              changed, called on the controller thread without locks.  Call again to set it */
void a2dp_pair(int on);                    /* any thread: discoverable and pairable for A2DP_PAIR_SECONDS, or stop */
int  a2dp_pairing(void);                   /* 1 while pairable */
/* any thread */
void a2dp_volume_changed(int percent);     /* the Echo's volume moved: tell the device (AVRCP absolute volume) */
int  a2dp_button(int resume);              /* action button: 0 = pause the streaming device, 1 = resume it if the button
                                              paused it (within 30 min).  1 = consumed; needs AVRCP */
void a2dp_pause(void);                     /* another source started: pause the device, or play nothing until it
                                              starts again or a2dp_unyield() when it has no AVRCP */
void a2dp_unyield(void);                   /* the other source stopped */
#endif
