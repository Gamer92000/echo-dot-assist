/* No local wake word: PC test build.  SIGUSR1 to the daemon simulates a detection (see main.c). */
#include "wake.h"
#include <stdlib.h>
int  wake_open(const char *manifest, wake_cb cb) { (void)manifest; (void)cb; return 0; }
void wake_feed(const int16_t *samples, size_t count) { (void)samples; (void)count; }
void wake_reset(void) {}
void wake_property(const char *name, int value) { (void)name; (void)value; }
void wake_close(void) {}
int  wake_afe_times(long *start, long *end) { (void)start; (void)end; return 0; }
const char *wake_attributes(void) { return getenv("HASSMIC_FAKE_WAKE_ATTRS"); }
