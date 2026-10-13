/* Playing to a Bluetooth speaker, the mixer's side: hassmic stands in for btmanagerd towards the mixer's own A2DP route
 * (docs/re-a2dp-source.md).  a2dp.c does the radio side and drives this from the controller thread. */
#ifndef BTOUT_H
#define BTOUT_H
#include <stddef.h>
#include <stdint.h>

void btout_start(void);                 /* once: HAL sockets, AIPC service, route thread */

/* controller thread (a2dp.c) */
void   btout_ready(uint64_t addr, const char *name, int bitpool, unsigned mtu);  /* a stream to it is open: route the
                                                                                   mixer there, SBC at that bitpool */
void   btout_gone(void);                /* no stream any more: the mixer back to the Echo's speaker */
void   btout_route(int on);             /* play on the speaker (default) or, with the link kept, on the Echo: only the
                                           mixer's route moves */
void   btout_streaming(int on);         /* AVDTP started (also: the answer to a START btout_want asked for) or suspended */
void   btout_start_failed(void);        /* the speaker refused to start */
void   btout_absvol(int on, int pct);   /* the speaker takes absolute volume (AVRCP), at pct now: it sets the volume, the
                                           mixer plays at full scale (core_speaker) */
int    btout_want(void);                /* the mixer has started its output and not suspended it, or the keep-awake tone
                                           plays while the speaker stands by */
size_t btout_packet(unsigned char *buf, size_t max);    /* next media packet (RTP header and SBC frames), 0 = none */

/* any thread */
int  btout_routed(void);                /* the mixer plays to the speaker */
int  btout_moving(void);                /* a change of the route is asked and not made yet */
int  btout_idle(void);                  /* only the keep-awake tone goes to the speaker (standing by) */
int  btout_speaker_volume(void);        /* the Echo's volume is the speaker's own (AVRCP absolute volume, routed) */
long long btout_latency_us(void);       /* what the speaker adds to the Echo's own output latency (Sendspin), 0 when not routed */
int  btout_keep_awake(int set);         /* an inaudible tone under everything sent (-1 reads): for speakers that mute
                                           themselves in silence and cut off what comes after it (issue #24) */
#endif
