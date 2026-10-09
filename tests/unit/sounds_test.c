/* sounds.c: stock sounds from earcon_dir and out of zips (board.earcon_zip).
 *   sounds_test synth DIR ZIP   a board of its own: DIR/ui_wakesound.wav overrides, ZIP (tests/unit/sounds_zip.py) holds a
 *                               stored WAV, a deflated one (refused), an entry that is not there
 *   sounds_test board           linked with a model's board.c, HASSMIC_EARCON_ROOT at its unpacked firmware: every
 *                               entry of its table decodes, as the Echo would read it */
#define MINIMP3_IMPLEMENTATION
#include "../../src/third_party/minimp3.h"
#include "board.h"
#include "sounds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

#ifdef SYNTH
static char dir[200];
static const struct board_sound zipped[] = {
    { "ui_wakesound", "", "res/override.wav" },                    /* earcon_dir has it: never read */
    { "state_privacy_mode_on", NULL, "res/raw/stored.wav" },
    { "state_privacy_mode_off", NULL, "res/raw/deflated.wav" },
    { "state_volume_adjust_tone", NULL, "res/raw/missing.mp3" },
    { NULL, NULL, NULL },
};
static struct board_sound table[5];
const struct board board = { .earcon_dir = dir, .earcon_zip = table };

int main(int argc, char **argv)
{
    const short *pcm; size_t n; unsigned rate;
    if (argc != 4 || strcmp(argv[1], "synth")) { fprintf(stderr, "usage: sounds_test synth DIR ZIP\n"); return 2; }
    snprintf(dir, sizeof dir, "%s/", argv[2]);
    memcpy(table, zipped, sizeof zipped);
    for (int i = 0; table[i].name; i++) if (!table[i].zip || !*table[i].zip) table[i].zip = argv[3];
    check(sound_get(SND_WAKE, &pcm, &n, &rate) && rate == 16000 && n == 160 && pcm[0] == 1000, "earcon_dir first: its WAV, mono");
    check(sound_get(SND_MICS_OFF, &pcm, &n, &rate) && rate == 48000 && n == 480 && pcm[0] == (300 + -100) / 2 && pcm[479] == (300 + -100) / 2,
          "a stored WAV out of the zip, stereo mixed down");
    check(!sound_get(SND_MICS_ON, &pcm, &n, &rate), "a deflated entry: refused (stock's audio is stored)");
    check(!sound_get(SND_VOLUME, &pcm, &n, &rate), "an entry the zip does not have: none");
    check(!sound_get(SND_BT_ON, &pcm, &n, &rate), "a sound in neither: none (the built-in tone then)");
    return fails ? 1 : 0;
}
#else
int main(void)
{
    const short *pcm; size_t n; unsigned rate; int count = 0;
    static const char *const names[] = { "ui_wakesound", "ui_wakesound_touch", "state_privacy_mode_on", "state_privacy_mode_off",
                                         "state_volume_adjust_tone", "state_bluetooth_connected", "state_bluetooth_disconnected",
                                         "state_setup_discovery_beacon", "comms_drop_in_incoming", "comms_call_connected",
                                         "comms_call_disconnected", "comms_call_incoming_ringtone", "comms_outbound_ringtone" };
    for (int s = 0; s < SND_COUNT; s++) {
        int listed = 0;
        for (const struct board_sound *b = board.earcon_zip; b && b->name; b++) listed |= !strcmp(b->name, names[s]);
        if (!listed) continue;
        char what[200];
        int ok = sound_get((enum sound)s, &pcm, &n, &rate);
        snprintf(what, sizeof what, "%s: %s, %.2f s at %u Hz", board.codename, names[s], ok ? (double)n / rate : 0.0, ok ? rate : 0);
        check(ok && rate >= 16000 && n > rate / 20, what);
        count++;
    }
    check(count > 0, "the board lists sounds");
    return fails ? 1 : 0;
}
#endif
