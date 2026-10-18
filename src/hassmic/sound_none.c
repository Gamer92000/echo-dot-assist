/* No sound detector: PC test build.  HASSMIC_FAKE_SOUND=<Amazon type> (e.g. dogBark) makes every ~10 s window of the
 * audio fed while it is on report that type, as the stock detector would (tests/fake_ha_esphome_features.py).
 * HASSMIC_FAKE_SOUND_WINDOW=<ms> makes the windows that long instead (the model's 9.98 s cost that test 30 s).
 * HASSMIC_FAKE_SOUND_MODEL=<file>: what it holds is the model there is, read at each call: "newer", "firmware" (also
 * without the file), "none", or "broken" (the firmware's, which then does not load) (tests/fake_web.py). */
#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sound_cb callback;
static const char *fake;
static uint64_t fed;
static int opened;
static uint64_t window = 159680;         /* samples: 9.98 s windows, as the model's */

static void fake_model(char *out, size_t cap)
{
    const char *p = getenv("HASSMIC_FAKE_SOUND_MODEL"); FILE *f = p ? fopen(p, "r") : NULL;
    snprintf(out, cap, "firmware");
    if (f) { if (fgets(out, (int)cap, f)) out[strcspn(out, "\r\n")] = 0; fclose(f); }
}

enum sound_model sound_model(void)
{
    char m[16]; fake_model(m, sizeof m);
    return !strcmp(m, "newer") ? SOUND_NEWER : !strcmp(m, "none") ? SOUND_NONE : SOUND_FIRMWARE;
}

int sound_open(const char *const *types, int n, sound_cb cb)
{
    const char *e = getenv("HASSMIC_FAKE_SOUND"); char m[16];
    fake_model(m, sizeof m);
    if (!strcmp(m, "broken") || !strcmp(m, "none")) return -1;
    callback = cb; fake = NULL; fed = 0; opened = 1;
    const char *w = getenv("HASSMIC_FAKE_SOUND_WINDOW");
    window = w && atoi(w) > 0 ? (uint64_t)atoi(w) * 16 : 159680;
    for (int i = 0; e && i < n; i++) if (!strcmp(types[i], e)) fake = types[i];
    return 0;
}

void sound_feed(const int16_t *samples, size_t count)
{
    (void)samples;
    if (!opened || !fake) return;
    if ((fed + count) / window > fed / window) callback(&fake, 1);
    fed += count;
}

int sound_window_ms(void)       /* the model's and the decoder's lag, as sound_pryon.c's; else the fake window, no lag */
{
    const char *e = getenv("HASSMIC_FAKE_SOUND_WINDOW");
    return e && atoi(e) > 0 ? atoi(e) : 11000;
}

void sound_close(void) { opened = 0; }
