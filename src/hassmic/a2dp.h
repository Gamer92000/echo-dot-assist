/* Bluetooth speaker: A2DP sink (SBC) on the controller ble.c drives, beside the Bluetooth LE proxy; and the other way,
 * playing to a Bluetooth speaker (A2DP source). */
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

/* Playing to a Bluetooth speaker (the Echo as A2DP source; btout.c routes the mixer).  Any thread. */
void a2dp_out_search(int on);              /* look for a speaker in pairing mode for up to a minute, pair the nearest,
                                              play on it */
int  a2dp_out_searching(void);
void a2dp_out_enable(int on);              /* play on the paired speaker (connect, reconnect) or on the Echo again; on
                                              without one searches */
int  a2dp_out_enabled(void);
int  a2dp_out_stay(int set);               /* off keeps the link and only plays on the Echo again (-1 reads); a lost link
                                              is paged for once the switch is on again */
int  a2dp_out_delay(int set);              /* the speaker's latency for Sendspin in ms, -1 reads */
void a2dp_out_status(char *buf, unsigned n);   /* "None", "Searching...", "<name>: playing" and so on */
#endif
