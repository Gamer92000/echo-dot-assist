/* A card's JSON (show_card's argument, on stdin) as cards.c hands it to the screen UI: the "cb/cr/cw/cf/cm/ce" lines
 * (scene.h), for build/uishot's UISHOT_LINES (a card of the Echo's format, v2: docs/cards.md).  cards.c with the stand-ins of tests/unit/cards_test.c.
 *   build/cardlines < card.json > card.lines && UISHOT_LINES=card.lines build/uishot dir fonts 1280 774 10 */
#include "../src/hassmic/cards.c"

pthread_mutex_t core_lock = PTHREAD_MUTEX_INITIALIZER;
int screen_present(void) { return 1; }
void screen_card(const char *l) { fputs(l, stdout); }
void screen_card_hold(int h) { (void)h; }
void ring_screen_wake(void) {}
int esphome_event(const char *s, const char *const *k, int n) { (void)s; (void)k; (void)n; return 0; }
void screen_bin(const char *l, const void *d, size_t n) { (void)l; (void)d; (void)n; }
uint8_t *esphome_fetch(const char *url, size_t max, size_t *n) { (void)url; (void)max; (void)n; return NULL; }
int esphome_action(const char *s, const char *const *k, int n) { (void)s; (void)k; (void)n; return 0; }

int main(void)
{
    static char js[CARD_JSON_MAX + 1]; size_t n = fread(js, 1, CARD_JSON_MAX, stdin); js[n] = 0;
    char err[160];
    if (strstr(js, "\"request\"")) cards_run(1);    /* a request's card: as while a request runs (a notice shows only
                                                       when none does) */
    { const char *c = strstr(js, "\"conversation_id\""); if (c) { char id[96] = ""; sscanf(c, "\"conversation_id\" : \"%95[^\"]", id);
      if (!id[0]) sscanf(c, "\"conversation_id\":\"%95[^\"]", id);
      if (id[0]) cards_conversation(id); } }
    if (cards_show(js, err, sizeof err)) { fprintf(stderr, "cardlines: refused: %s\n", err); return 1; }
    return 0;
}
