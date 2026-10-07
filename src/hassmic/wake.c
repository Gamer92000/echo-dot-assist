/* wake.h: the engine the model names.  Engine hints (wake_property) always reach the vendor engine, which keeps them for
 * when it opens next (microWakeWord has none: its cutoff is fixed per model). */
#include "wake.h"
#include <string.h>

static const struct wake_engine *cur = &wake_vendor;
static uint64_t fed;                    /* samples through wake_feed since start, whichever engine: main.c's ring counts so */

int wake_is_mww(const char *model)
{
    size_t n = model ? strlen(model) : 0;
    return n > 5 && !strcmp(model + n - 5, ".json");
}

int  wake_open(const char *model, wake_cb cb) { cur = wake_is_mww(model) ? &wake_mww : &wake_vendor; return cur->open(model, cb); }
void wake_feed(const int16_t *samples, size_t count) { cur->feed(samples, count); fed += count; }
uint64_t wake_fed(void) { return fed; }
void wake_reset(void) { cur->reset(); }
void wake_property(const char *name, int value) { wake_vendor.property(name, value); }
void wake_close(void) { cur->close(); }
int  wake_afe_times(long *start, long *end) { return cur->afe_times(start, end); }
