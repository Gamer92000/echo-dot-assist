/* sounds.c: stock sounds from earcon_dir and out of zips (board.earcon_zip).
 *   sounds_test synth DIR ZIP   a board of its own: DIR/ui_wakesound.wav overrides, ZIP (tests/unit/sounds_zip.py) holds a
 *                               stored WAV, a deflated one (refused), an entry that is not there; the timer sound of
 *                               one's own (DIR as the state folder): the page's format only, read again once replaced
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
    check(sound_present(SND_WAKE) && sound_present(SND_MICS_OFF) && !sound_present(SND_BT_ON), "present: in earcon_dir, in a zip, in neither");
    check(!sound_present(SND_MICS_ON) && !sound_present(SND_VOLUME), "not present: a deflated entry, an entry its zip does not have (the zip is there)");

    /* 0.1 s of 16-bit mono at 48 kHz, as the page makes it */
    unsigned char w[44 + 9600]; char err[200];
    memcpy(w, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\1\0\1\0\x80\xbb\0\0\0\x77\1\0\2\0\x10\0data\x80\x25\0\0", 44);
    w[4] = (36 + 9600) & 0xff; w[5] = (36 + 9600) >> 8;
    for (int i = 0; i < 4800; i++) { w[44 + 2 * i] = 777 & 0xff; w[45 + 2 * i] = 777 >> 8; }
    setenv("HASSMIC_STATE", argv[2], 1);
    check(!sound_present(SND_CUSTOM) && !sound_get(SND_CUSTOM, &pcm, &n, &rate) && !sound_custom_ms(), "own timer sound: none before an upload");
    check(!sound_custom_save(w, sizeof w, err, sizeof err) && sound_present(SND_CUSTOM) && sound_custom_ms() == 100, "own timer sound: saved, 100 ms");
    check(sound_get(SND_CUSTOM, &pcm, &n, &rate) && rate == 48000 && n == 4800 && pcm[0] == 777 && pcm[4799] == 777, "own timer sound: read back");
    w[44] = 555 & 0xff; w[45] = 555 >> 8;
    check(!sound_custom_save(w, sizeof w, err, sizeof err) && sound_get(SND_CUSTOM, &pcm, &n, &rate) && pcm[0] == 555, "own timer sound: read again once replaced");
    w[22] = 2;                                                  /* two channels */
    check(sound_custom_save(w, sizeof w, err, sizeof err) && strstr(err, "2 channels") && sound_get(SND_CUSTOM, &pcm, &n, &rate) && pcm[0] == 555,
          "own timer sound: stereo refused, the one before kept");
    w[22] = 1; w[24] = 0x44; w[25] = 0xac;                      /* 44100 Hz */
    check(sound_custom_save(w, sizeof w, err, sizeof err) && strstr(err, "44100 Hz"), "own timer sound: 44.1 kHz refused");
    check(sound_custom_save(w, 40, err, sizeof err) && !strcmp(err, "not a WAV file"), "own timer sound: a cut-off header refused");
    check(!sound_custom_delete() && !sound_present(SND_CUSTOM) && !sound_get(SND_CUSTOM, &pcm, &n, &rate) && !sound_custom_delete(),
          "own timer sound: deleted (twice: no error)");
    return fails ? 1 : 0;
}
#else
int main(void)
{
    const short *pcm; size_t n; unsigned rate; int count = 0;
    static const char *const names[] = { "ui_wakesound", "ui_wakesound_touch", "state_privacy_mode_on", "state_privacy_mode_off",
                                         "state_volume_adjust_tone", "state_bluetooth_connected", "state_bluetooth_disconnected",
                                         "state_setup_discovery_beacon", "comms_drop_in_incoming", "comms_call_connected",
                                         "comms_call_disconnected", "comms_call_incoming_ringtone", "comms_outbound_ringtone",
                                         "system_alerts_melodic_02", "system_alerts_melodic_01", "timer_sound" };
    _Static_assert(sizeof names / sizeof names[0] == SND_COUNT, "a name per sound");
    for (int s = 0; s < SND_COUNT; s++) {
        int listed = 0;
        for (const struct board_sound *b = board.earcon_zip; b && b->name; b++) listed |= !strcmp(b->name, names[s]);
        if (!listed) continue;
        char what[200];
        int ok = sound_get((enum sound)s, &pcm, &n, &rate);
        snprintf(what, sizeof what, "%s: %s, %.2f s at %u Hz, present", board.codename, names[s], ok ? (double)n / rate : 0.0, ok ? rate : 0);
        check(ok && rate >= 16000 && n > rate / 20 && sound_present((enum sound)s), what);
        count++;
    }
    check(count > 0, "the board lists sounds");
    return fails ? 1 : 0;
}
#endif
