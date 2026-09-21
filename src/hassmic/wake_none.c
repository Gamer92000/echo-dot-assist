/* No local wake word: PC test build.  SIGUSR1 to the daemon simulates a detection (see main.c). */
#include "wake.h"
int  wake_open(const char *manifest, wake_cb cb) { (void)manifest; (void)cb; return 0; }
void wake_feed(const int16_t *samples, size_t count) { (void)samples; (void)count; }
void wake_reset(void) {}
void wake_close(void) {}
