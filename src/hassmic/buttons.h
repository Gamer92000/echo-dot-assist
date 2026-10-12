/* Top buttons: GPIO keypad (board.keypad) and the hardware mute latch (board.privacy_*); donut: docs/re-platform.md. */
#ifndef BUTTONS_H
#define BUTTONS_H

enum { BUTTON_ACTION, BUTTON_VOLUME_UP, BUTTON_VOLUME_DOWN };

struct button_handler {
    void (*action)(void);               /* short press of the action button */
    void (*mute_changed)(int muted);    /* hardware privacy latch changed */
    void (*volume)(int direction);      /* +1 / -1: a volume key let go within 1 s (a hold is event()'s alone) */
    void (*pair)(void);                 /* may be NULL: Volume up and Volume down held together for 2 s */
    void (*hold)(int stage);            /* may be NULL: action button held: 1 at 5 s, 2 at 10 s; 0 let go in between */
    /* may be NULL: for Home Assistant (core_button_events).  A key (BUTTON_*) held 1 s (hold 1), or a volume key let go
     * before that (hold 0); never while both volume keys are down (pair).  The short press of the action button is
     * action() above, which tells it on itself: a press that approves a login is not a press for Home Assistant */
    void (*event)(int key, int hold);
};

/* Starts a reader thread.  Returns -1 if the input device cannot be opened (PC build: always). */
int  buttons_start(const char *device, const struct button_handler *h);
int  buttons_muted(void);               /* current privacy state, 0 if unknown */
#endif
