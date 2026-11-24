/* Push updates: receive a signed bundle over TCP and hand it to the root-side installer (scripts/system/main.sh). */
#ifndef OTA_H
#define OTA_H
int ota_start(int port);        /* listener thread; does nothing useful without /system/hassmic/update.pub.  Also
                                   opens adb over Wi-Fi for whoever holds the update key (ota.c) */
/* The self test passed: root makes the update that runs the factory copy (main.sh healthy), if it is this build.
 * 0 told; -1 could not write; 1 told nothing: a bundle is with the installer (ota_handed), hassmic is about to be
 * replaced, and this build is not the one root would promote */
int ota_healthy(void);
int ota_handed(void);           /* 1 while a bundle handed to root may still be installed (no "FAILED" from it, in
                                   time or, once root took the request, after the minute; a thread of ota.c's reads
                                   that late answer too) */
#include <stddef.h>
#include <stdint.h>
/* A bundle hassmic downloaded itself (update.c), signed with the release key or OWN (NULL: none; the key the settings
 * page's Sources had root trust beside it, sources.c): checked against those, handed to root's installer like a push.  RES gets the installer's answer ("OK <version>": hassmic is restarted on it; "FAILED <why>").
 * 0 on OK; -1 refused or not handed over; 1 root did not answer in time (it may install it still: ota_handed reads
 * its answer later).  Blocks for up to a minute. */
int ota_handoff(const uint8_t *bundle, size_t len, const uint8_t sig[64], const uint8_t *own, char *res, size_t cap);
#endif
