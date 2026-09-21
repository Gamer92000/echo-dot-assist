/* Top buttons of the Echo Dot 3: GPIO keypad on /dev/input/event3 (see docs/re-platform.md). */
#ifndef BUTTONS_H
#define BUTTONS_H

struct button_handler {
    void (*action)(void);               /* short press of the action button */
    void (*mute_changed)(int muted);    /* hardware privacy latch changed */
    void (*volume)(int direction);      /* +1 / -1, also on key repeat */
};

/* Starts a reader thread.  Returns -1 if the input device cannot be opened (PC build: always). */
int  buttons_start(const char *device, const struct button_handler *h);
int  buttons_muted(void);               /* current privacy state, 0 if unknown */
#endif
