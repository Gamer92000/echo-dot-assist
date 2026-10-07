/* No local wake word: PC test build.  SIGUSR1 to the daemon simulates a detection (see main.c). */
#include "wake.h"
#include <stdlib.h>
static int  none_open(const char *manifest, wake_cb cb) { (void)manifest; (void)cb; return 0; }
static void none_feed(const int16_t *samples, size_t count) { (void)samples; (void)count; }
static void none_reset(void) {}
static void none_property(const char *name, int value) { (void)name; (void)value; }
static void none_close(void) {}
static int  none_afe_times(long *start, long *end) { (void)start; (void)end; return 0; }
const char *wake_attributes(void) { return getenv("HASSMIC_FAKE_WAKE_ATTRS"); }
const struct wake_engine wake_vendor = { none_open, none_feed, none_reset, none_property, none_close, none_afe_times };
