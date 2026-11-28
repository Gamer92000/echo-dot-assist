/*
 * The Echo's volume curve (board.h board_vol_db, board.volume_curve): what separate volumes turn each kind down by and
 * where even volume steps put the mixer (main.c mix_gain, mix_target).  A file of its own so tests/unit/volcurve_test.c
 * checks it against each model's board.c.
 */
#include "board.h"

/* the Echo Dot 2's (biscuit), dB under volume 100 (2026-10-17: a -30 dBFS tone through the Music, Earcon and TTS
 * streams in micRaw, the three within 0.5 dB of each other; libasp's boost above 80 included, the mixer alone is nearly
 * flat there).  Here, not in biscuit's board.c, because it also stands in for every model not measured yet (board.h) */
const struct board_vol board_vol_dot2[] = {
    { 0, -60 }, { 5, -47.3f }, { 10, -40.6f }, { 20, -35.7f }, { 30, -30.5f }, { 40, -26.5f }, { 50, -22.4f },
    { 60, -19.5f }, { 70, -18.4f }, { 80, -14.0f }, { 90, -10.0f }, { 100, 0 }, { -1, 0 } };

/* the Echo 4's (laser), dB under volume 100 (2026-11-28, on the one unit): a 4-tone multitone (470/1070/2230/3770 Hz,
 * -48 dBFS each) played on the TTS/Music stream through a dev-build tone hook, recorded on an Echo Dot 2 ~10 cm away
 * (its micRaw, fixed gain, no echo canceller; laser's own micRaw is not free to mixcap and a bare mixplay never drives
 * laser's speaker, so the tone has to come from hassmic's own output), volume stepped with the keypad and read back
 * from MainVolume.  The low tone (470 Hz) sits under the room's 120 Hz mains hum, so the low-volume points come from
 * the three higher tones.  The output plateaus from ~80 up (measured within ~3 dB there, peaking near 90 with libasp's
 * top-end boost, then flat/slightly down at 100); flattened to a monotonic approach to 0 here, as the curve requires.
 * Volume 0 is extrapolated (effectively off).  The three higher tones agreed within ~1 dB per step. */
const struct board_vol board_vol_laser[] = {
    { 0, -50 }, { 10, -34.5f }, { 20, -28.3f }, { 30, -21.2f }, { 40, -14.3f }, { 50, -10.3f }, { 60, -4.4f },
    { 70, -2.5f }, { 80, -1.1f }, { 90, -0.3f }, { 100, 0 }, { -1, 0 } };

float board_vol_db(int v)
{
    const struct board_vol *c = board.volume_curve;
    int i = 1;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    while (c[i].at < v && c[i + 1].at >= 0) i++;                /* {-1} after the last point */
    return c[i - 1].db + (c[i].db - c[i - 1].db) * (v - c[i - 1].at) / (c[i].at - c[i - 1].at);
}

/* the lowest volume the Echo plays at least `db` at (Even volume steps, main.c): one of the curve's own points, where
 * it was measured (on the Show each point is the very index played; between two the interpolation is only within 1 dB
 * of it), so what is left over to take off in our PCM is exact */
int board_vol_at(float db)
{
    const struct board_vol *c = board.volume_curve;
    for (; c[1].at >= 0; c++) if (c->db >= db) return c->at;
    return c->at;                                               /* the last: 100, 0 dB */
}
