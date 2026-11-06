/* The other side's picture in a Drop In, on this Echo Show's screen (docs/video-dropin.md): cam.c's play mode as a child,
 * fed the call's H.264 records (camera.h's types) as they come, from another Echo Show (dropin.c, over the call's UDP)
 * or a browser (web.c's call channel, through Echo Local).  Present where the camera's library is (board.camera,
 * libhmcam.so and screen.dex next to the binary) and a screen runs, or on the PC with HASSMIC_FAKE_VIDEO (a program
 * reading the records on its stdin). */
#ifndef VIDEO_H
#define VIDEO_H
#include <stddef.h>
#include <stdint.h>

int  video_present(void);
/* A record of call gen's picture: copied, queued, never waited on (the caller is a network thread).  Behind: frames
 * dropped up to the next key frame */
void video_show(int gen, int type, long long pts_us, const uint8_t *data, size_t len);
/* This Show's own picture while it sends one (the same records as leave): small in a corner, mirrored */
void video_self(int gen, int type, long long pts_us, const uint8_t *data, size_t len);
/* Call gen's other side frosted for its first ms (the caller's screen, the Echo called did not ring); before its records */
void video_blur(int gen, int ms);
/* Call gen's picture is over (the call ended, its picture stopped): both players go, and the pictures with them */
void video_stop(int gen);
void video_self_stop(int gen);                  /* only this Show's own (its camera stopped mid-call) */
int  video_shown(void);                         /* records handed to a player since the start: tests, the log */
int  video_self_shown(void);
#endif
