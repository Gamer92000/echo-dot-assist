/* Wake-word backend.  The callback runs on a detector thread (Pryon's), or in wake_feed (microWakeWord's).
 * Two engines behind it, picked by the model wake_open() is given (wake.c): a microWakeWord manifest (.json, wake_mww.c,
 * on every build) or a model set of the model's own engine (wake_pryon.c on the Echos, wake_none.c on the PC).
 * Up to WAKE_SLOTS models listen at once (Home Assistant's "Wake word" and "Wake word 2"), each in a slot of its own
 * that is fed the same audio: a Pryon decoder each, or a microWakeWord interpreter each.
 * wake_feed and wake_open/close: one thread (the capture thread, main() before it runs); the others: any thread. */
#ifndef WAKE_H
#define WAKE_H
#include <stddef.h>
#include <stdint.h>

#define WAKE_SLOTS 2

/* slot: the one that heard it.  begin, end: where the keyword lies, counted in samples passed to wake_feed() since the
 * start (whichever engine) */
typedef void (*wake_cb)(int slot, const char *keyword, uint64_t begin, uint64_t end);

int  wake_open(int slot, const char *manifest, wake_cb cb);
/* Near misses (Pryon's detection type 0: close to the keyword, not accepted): slot, keyword.  Set once before the
 * first wake_open; the callback runs where wake_cb does.  microWakeWord reports none */
typedef void (*wake_miss_cb)(int slot, const char *keyword);
void wake_on_miss(wake_miss_cb cb);
void wake_near_miss(int slot, const char *keyword);     /* the engines: hand one on */
void wake_feed(const int16_t *samples, size_t count);   /* every open slot */
void wake_reset(void);                 /* audio discontinuity */
void wake_property(const char *name, int value);   /* engine hint, e.g. "AlarmState" 1: the model lowers its threshold */
void wake_close(int slot);             /* -1: every slot */
/* Where the last accepted keyword (whichever slot) lies on the clock of Amazon's front end (ms, 16 bit, wraps), which
 * the engine reads from the stream itself.  0: not known (no such marks in the stream, PC build). */
int  wake_afe_times(long *start, long *end);
/* The engine's attributes line (JSON: wakeword_ecids, aed_ecids, ...), NULL without an engine.  DAVS asks for model
 * sets with the ids of the engine that will load them; an older engine (radar's) cannot load a set it did not name. */
const char *wake_attributes(void);

/* the engines (wake.c picks, per slot); attributes stay the vendor engine's; property: every slot of the engine */
struct wake_engine {
    int  (*open)(int slot, const char *model, wake_cb cb);
    void (*feed)(int slot, const int16_t *samples, size_t count);
    void (*reset)(int slot);
    void (*property)(const char *name, int value);
    void (*close)(int slot);
    int  (*afe_times)(int slot, long *start, long *end);
};
extern const struct wake_engine wake_vendor, wake_mww;
int wake_is_mww(const char *model);    /* a microWakeWord manifest rather than one of the vendor engine's */
uint64_t wake_fed(void);                /* samples fed so far: an engine opened later counts on from there */
#endif
