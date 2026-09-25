/* Wake-word backend.  The callback runs on a detector thread. */
#ifndef WAKE_H
#define WAKE_H
#include <stddef.h>
#include <stdint.h>

/* begin, end: where the keyword lies, counted in samples passed to wake_feed() since wake_open() */
typedef void (*wake_cb)(const char *keyword, uint64_t begin, uint64_t end);

int  wake_open(const char *manifest, wake_cb cb);
void wake_feed(const int16_t *samples, size_t count);
void wake_reset(void);                 /* audio discontinuity */
void wake_property(const char *name, int value);   /* engine hint, e.g. "AlarmState" 1: the model lowers its threshold */
void wake_close(void);
#endif
