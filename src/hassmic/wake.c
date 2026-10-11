/* wake.h: the engine the model names, per slot.  Engine hints (wake_property) always reach the vendor engine, which
 * keeps them for when it opens next (microWakeWord has none: its cutoff is fixed per model). */
#include "wake.h"
#include <stdatomic.h>
#include <string.h>

static const struct wake_engine *cur[WAKE_SLOTS];  /* NULL: the slot is closed */
static uint64_t fed;                    /* samples through wake_feed since start, whichever engine: main.c's ring counts so */
static wake_cb user_cb;
static atomic_int last_slot;            /* the slot that accepted a keyword last: wake_afe_times asks its engine */

int wake_is_mww(const char *model)
{
    size_t n = model ? strlen(model) : 0;
    return n > 5 && !strcmp(model + n - 5, ".json");
}

static void heard(int slot, const char *keyword, uint64_t begin, uint64_t end)
{
    atomic_store(&last_slot, slot);
    user_cb(slot, keyword, begin, end);
}

int wake_open(int slot, const char *model, wake_cb cb)
{
    if (slot < 0 || slot >= WAKE_SLOTS) return -1;
    wake_close(slot);
    user_cb = cb;
    const struct wake_engine *e = wake_is_mww(model) ? &wake_mww : &wake_vendor;
    if (e->open(slot, model, heard)) return -1;
    cur[slot] = e;
    return 0;
}

void wake_feed(const int16_t *samples, size_t count)
{
    for (int i = 0; i < WAKE_SLOTS; i++) if (cur[i]) cur[i]->feed(i, samples, count);
    fed += count;
}

uint64_t wake_fed(void) { return fed; }
void wake_reset(void) { for (int i = 0; i < WAKE_SLOTS; i++) if (cur[i]) cur[i]->reset(i); }
void wake_property(const char *name, int value) { wake_vendor.property(name, value); }

void wake_close(int slot)
{
    for (int i = 0; i < WAKE_SLOTS; i++)
        if ((slot < 0 || i == slot) && cur[i]) { cur[i]->close(i); cur[i] = NULL; }
}

int wake_afe_times(long *start, long *end)
{
    int s = atomic_load(&last_slot);
    return cur[s] ? cur[s]->afe_times(s, start, end) : 0;
}
