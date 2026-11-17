/* lyrion.h: Lyrion's volume from what audg carries, against Lyrion's own sums for every volume.  The reference is
 * Slim::Player::SqueezePlay getVolumeParameters (-74 dB, step point 25, step fraction .5), Squeezebox2 getVolume,
 * dBToFixed (8 bits of accuracy between -30 and 0 dB) and the old-style volume_map, word for word in C. */
#include "lyrion.h"
#include <stdio.h>

static uint32_t lms_gain(int volume)
{
    const double total = -74, step_point = 25, step_db = total * .5, max_db = 0;
    double slope_high = (max_db - step_db) / (100 - step_point), slope_low = (step_db - total) / (step_point - 0), db;
    if (volume > step_point) db = slope_high * (volume - 100) + max_db;
    else db = slope_low * (volume - 0) + total;
    double mult = pow(10, db / 20);
    if (db >= -30 && db <= 0) return (uint32_t)((long)(mult * (1 << 8) + 0.5) * (1 << 8));
    return (uint32_t)(long)(mult * (1 << 16) + 0.5);
}

static const int volume_map[101] = { 0, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5, 5, 6, 6, 7, 8, 9, 9, 10, 11, 12, 13, 14, 15, 16, 16, 17,
    18, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 32, 33, 34, 35, 37, 38, 39, 40, 42, 43, 44, 46, 47, 48, 50, 51, 53, 54,
    56, 57, 59, 60, 61, 63, 65, 66, 68, 69, 71, 72, 74, 75, 77, 79, 80, 82, 84, 85, 87, 89, 90, 92, 94, 96, 97, 99, 101,
    103, 104, 106, 108, 110, 112, 113, 115, 117, 119, 121, 123, 125, 127, 128 };

int main(void)
{
    int bad = 0;
    for (int v = 1; v <= 100; v++) {
        uint32_t g = lms_gain(v);
        int back = lyrion_volume_of_gain(g, (uint32_t)volume_map[v]);
        if (back != v) { printf("FAIL volume %d: gain %u read back as %d\n", v, g, back); bad = 1; }
        if (g != lyrion_gain16(v)) { printf("FAIL volume %d: gain %u, ours %u\n", v, g, lyrion_gain16(v)); bad = 1; }
        /* without the old-style gain (another Lyrion's sums): the formula, a step off at most */
        int near = lyrion_volume_of_gain(g + 1, 999);
        if (near < v - 1 || near > v + 1) { printf("FAIL volume %d: gain %u + 1 read as %d\n", v, g, near); bad = 1; }
    }
    if (lyrion_volume_of_gain(0, 0) != 0) { printf("FAIL gain 0 is not volume 0\n"); bad = 1; }
    if (lyrion_volume_of_gain(65536, 128) != 100 || lyrion_volume_of_gain(1, 0) != 1) { printf("FAIL the ends\n"); bad = 1; }
    if (!bad) printf("ok   every volume 1..100 back from Lyrion's gain as Lyrion had it; a gain off its sums a step off at most\n");
    return bad;
}
