/*
 * The Echo's volume curve (board.h board_vol_db): what separate volumes turn each kind down by (main.c mix_gain).  A
 * file of its own so tests/unit/volcurve_test.c checks it against each model's board.c.
 */
#include "board.h"

/* the Echo Dot 2's, dB under volume 100 (2026-10-17: a -30 dBFS tone through the Music, Earcon and TTS streams in
 * micRaw, the three within 0.5 dB of each other; libasp's boost above 80 included, the mixer alone is nearly flat
 * there); every model without a measured one of its own takes it */
static const struct board_vol dot2_curve[] = {
    { 0, -60 }, { 5, -47.3f }, { 10, -40.6f }, { 20, -35.7f }, { 30, -30.5f }, { 40, -26.5f }, { 50, -22.4f },
    { 60, -19.5f }, { 70, -18.4f }, { 80, -14.0f }, { 90, -10.0f }, { 100, 0 }, { -1, 0 } };

float board_vol_db(int v)
{
    const struct board_vol *c = board.volume_curve ? board.volume_curve : dot2_curve;
    int i = 1;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    while (c[i].at < v && c[i + 1].at >= 0) i++;                /* {-1} after the last point */
    return c[i - 1].db + (c[i].db - c[i - 1].db) * (v - c[i - 1].at) / (c[i].at - c[i - 1].at);
}
