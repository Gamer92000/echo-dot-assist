/* Sound detection backend: the acoustic event detector of the stock firmware (Alexa Guard's, docs/re-aed.md), off unless
 * Home Assistant turns it on.  The callback runs on a detector thread. */
#ifndef SOUND_H
#define SOUND_H
#include <stddef.h>
#include <stdint.h>

/* One call per scoring window (~10 s of audio) in which at least one type passed its threshold; types are Amazon's names
 * from AED.json ("dogBark", "smokeAlarm", ...), only the ones sound_open() enabled. */
typedef void (*sound_cb)(const char *const *types, int n);

int  sound_open(const char *const *types, int n, sound_cb cb);    /* the types to detect; 0: running */
/* The model sound_open() takes, or took: Amazon's newer one (installed by scripts/artifacts.sh), the firmware's, or
 * none (neither readable: nothing to switch on).  For the settings page; any thread. */
enum sound_model { SOUND_NONE, SOUND_FIRMWARE, SOUND_NEWER };
enum sound_model sound_model(void);
void sound_feed(const int16_t *samples, size_t count);
void sound_close(void);
#endif
