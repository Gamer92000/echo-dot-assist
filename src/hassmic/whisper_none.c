/* No whisper detector: PC test build.  HASSMIC_FAKE_WHISPER=1 calls every scored request whispered, 0 not whispered;
 * unset, there is no model and so no detection (tests/fake_ha_esphome.py). */
#include "whisper.h"
#include <stdlib.h>

static whisper_cb callback;
static int active;

int whisper_open(whisper_cb cb)
{
    if (!getenv("HASSMIC_FAKE_WHISPER")) return -1;
    callback = cb;
    return 0;
}

void whisper_begin(void) { active = callback != NULL; }
void whisper_feed(const int16_t *samples, size_t count) { (void)samples; (void)count; }

void whisper_end(int score)
{
    if (!active) return;
    active = 0;
    if (score) { int w = atoi(getenv("HASSMIC_FAKE_WHISPER")) != 0; callback(w, w ? 999 : 1, 922); }
}
