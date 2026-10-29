/* No local wake word: PC test build.  SIGUSR1 to the daemon simulates a detection (see main.c). */
#include "wake.h"
#include <stdlib.h>
static int  none_open(int slot, const char *manifest, wake_cb cb) { (void)slot; (void)manifest; (void)cb; return 0; }
static void none_feed(int slot, const int16_t *samples, size_t count) { (void)slot; (void)samples; (void)count; }
static void none_reset(int slot) { (void)slot; }
static void none_property(const char *name, int value) { (void)name; (void)value; }
static void none_close(int slot) { (void)slot; }
/* HASSMIC_FAKE_AFE (audio_file.c): a place on the front end's clock, so that SIGUSR1's wake word asks it */
static int  none_afe_times(int slot, long *start, long *end) { (void)slot; *start = *end = 0; return getenv("HASSMIC_FAKE_AFE") != NULL; }
const char *wake_attributes(void) { return getenv("HASSMIC_FAKE_WAKE_ATTRS"); }
const struct wake_engine wake_vendor = { none_open, none_feed, none_reset, none_property, none_close, none_afe_times };
