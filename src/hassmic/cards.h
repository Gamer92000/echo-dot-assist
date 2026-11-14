/* Cards on the Echo Show's screen (cards.c): what Home Assistant pushes with the ESPHome action show_card, drawn by the
 * screen UI over whatever shows (src/ui/card.c, on the words layer).  Four kinds: entities (a row each, its words as
 * Home Assistant made them, and a control where the card asks for one: a switch, a light's brightness, a cover's
 * open/stop/close, a thermostat's setpoint, a player's play/pause, Run), the weather (now and a few days), a message
 * (with a picture: a camera's snapshot) and a list (rows of text: a calendar, a shopping list); buttons on any of them
 * (an event for Home Assistant when pressed).  docs/cards.md has the format.
 *
 * A notice (an automation's card) shows its timeout from when it came (0: until tapped away or cleared); a voice
 * request hides it while it runs and while the request's own card shows.  A request's card (scope "request") shows
 * while that request is this Echo's, from the answer until timeout seconds after the Echo is idle again.  A card with
 * an id is updated in place ("update": the same id again) and cleared by its id.
 *
 * Live: Home Assistant (hassmic_cards) sends the card again when one of its entities changed; a tap on a control runs
 * a Home Assistant action (needs "Allow the device to perform Home Assistant actions") and shows its result at once. */
#ifndef CARDS_H
#define CARDS_H
#include <stddef.h>
#include <stdint.h>

#define CARD_JSON_MAX 8192              /* a show_card's card argument, at most (screen.c's card_lines holds what it becomes) */
#define CARDS_VERSION 2                 /* the card format ("v") this Echo takes; docs/cards.md */

int  cards_show(const char *json, char *err, size_t errn);  /* show_card: 0 taken (shown, or kept for this request) */
void cards_clear(const char *id, const char *why);          /* clear_card: that id's card (NULL or "": what shows) */
void cards_run(int start);                                  /* the satellite: 1 a request starts, 0 idle again */
void cards_conversation(const char *id);                    /* Home Assistant's intent end: this request's conversation */
void cards_ui(const char *line);                            /* the UI's "card ..." lines (scene.h) */
void cards_image(const char *key, int size);                /* the UI's "img card<seq>.<n> <size>": a message card's picture */
int  cards_on(int set);                                     /* setting screen_cards; -1 reads */
void cards_start(void);                                     /* its thread (expiry, refreshes); a model with a screen */
void cards_resend_state(void);                              /* Home Assistant subscribed: what shows, told again */

/* proto_esphome.c, for cards.c (core_lock held): one of Home Assistant's actions run, and an event fired (esphome.*:
 * needs no permission), data n key/value pairs; -1 when no Home Assistant client is there to tell */
int  esphome_action(const char *service, const char *const *kv, int n);
int  esphome_event(const char *event, const char *const *kv, int n);
uint8_t *esphome_fetch(const char *url, size_t max, size_t *n);    /* any thread, no lock: an http(s) body, malloc'd */
#endif
