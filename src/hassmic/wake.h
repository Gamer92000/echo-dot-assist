/* Wake-word backend.  The callback runs on a detector thread. */
#ifndef WAKE_H
#define WAKE_H
#include <stddef.h>
#include <stdint.h>

typedef void (*wake_cb)(const char *keyword);

int  wake_open(const char *manifest, wake_cb cb);
void wake_feed(const int16_t *samples, size_t count);
void wake_reset(void);                 /* audio discontinuity */
void wake_close(void);
#endif
