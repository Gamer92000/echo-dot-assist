/* Top buttons: GPIO keypad (board.keypad) and the hardware mute latch (board.privacy_*); donut: docs/re-platform.md. */
#ifndef BUTTONS_H
#define BUTTONS_H

struct button_handler {
    void (*action)(void);               /* short press of the action button */
    void (*mute_changed)(int muted);    /* hardware privacy latch changed */
    void (*volume)(int direction);      /* +1 / -1, also on key repeat */
    void (*pair)(void);                 /* may be NULL: Volume up and Volume down held together for 2 s */
    void (*hold)(int stage);            /* may be NULL: action button held: 1 at 5 s, 2 at 10 s; 0 let go in between */
};

/* Starts a reader thread.  Returns -1 if the input device cannot be opened (PC build: always). */
int  buttons_start(const char *device, const struct button_handler *h);
int  buttons_muted(void);               /* current privacy state, 0 if unknown */
#endif
