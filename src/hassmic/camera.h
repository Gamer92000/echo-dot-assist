/* The Echo Show's camera (docs/camera.md): H.264 from src/cam/cam.c (libhmcam.so in an app_process of its own, run
 * only while a stream runs), for one viewer at a time: the Echo Local integration's relay, which hands it to Home
 * Assistant (web.c GET /api/camera/stream, encrypted with the integration's key).  Off by default (setting "camera");
 * the lens shutter stops it.  On every build: present only where the board has a camera and libhmcam.so + screen.dex
 * are next to the binary, or on the PC with HASSMIC_FAKE_CAMERA (a program writing cam.c's records). */
#ifndef CAMERA_H
#define CAMERA_H
#include <stddef.h>
#include <stdint.h>

enum { CAM_CONFIG = 1, CAM_KEY = 2, CAM_FRAME = 3 };            /* cam.c's record types */

int  camera_present(void);
int  camera_enable(int on);                     /* the setting; -1 reads.  Off ends a stream */
void camera_lens(int covered);                  /* buttons.c: SW_CAMERA_LENS_COVER; covered ends a stream */
int  camera_covered(void);                      /* 1 covered, 0 open, -1 no shutter switch seen */
int  camera_streaming(void);
size_t camera_json(char *o, size_t cap);        /* {"present":..,"on":..,"covered":..,"streaming":..} for /api/state */

/* web.c: one stream.  camera_begin: a stream id (> 0), or -1 and why ("off", "covered", "busy", "absent", "failed").
 * camera_next: the next record into *data (malloc'd, the caller frees), its type and pts; 1 one, 0 nothing within
 * wait_ms, -1 the stream ended (the child stopped, the setting went off, the lens was covered).  camera_end: stops it. */
int  camera_begin(const char **why);
int  camera_next(int id, int *type, long long *pts_us, uint8_t **data, size_t *len, int wait_ms);
void camera_end(int id);
#endif
