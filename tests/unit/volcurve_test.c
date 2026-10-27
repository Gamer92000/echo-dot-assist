/* volcurve.c against a model's board.c (built once per model, make unit-volcurve): the curve 0 dB at 100, rising,
 * clamped outside 0..100, straight between its points, its {-1} end found; the Echo Show 5's (checkers) points at the
 * index each volume plays at, as tools/vol-curve.py measured them; a model without one takes the Echo Dot 2's. */
#include "board.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what) { printf("%s %s: %s\n", ok ? "ok  " : "FAIL", board.codename, what); if (!ok) fails++; }
static int near(float a, float b) { return fabsf(a - b) < 0.01f; }

int main(void)
{
    int rising = 1;
    for (int v = 1; v <= 100; v++) if (board_vol_db(v) < board_vol_db(v - 1)) rising = 0;
    check(near(board_vol_db(100), 0), "0 dB at volume 100");
    check(rising, "never quieter as the volume goes up");
    check(near(board_vol_db(-5), board_vol_db(0)) && near(board_vol_db(150), 0), "outside 0..100: its ends");
    if (board.volume_curve) {
        int n = 0, at_points = 1, ordered = 1;
        for (const struct board_vol *c = board.volume_curve; c->at >= 0; c++, n++) {
            if (!near(board_vol_db(c->at), c->db)) at_points = 0;
            if (n && (c->at <= c[-1].at || c->db < c[-1].db)) ordered = 0;     /* the same `at` twice: a division by 0 */
        }
        check(ordered, "its points rising, each further than the last, never quieter");
        check(n >= 2 && board.volume_curve[0].at == 0 && board.volume_curve[n - 1].at == 100, "its points from 0 to 100, the end marked");
        check(at_points, "each point where it says");
    }
    if (!strcmp(board.codename, "checkers")) {
        check(near(board_vol_db(7), -60 + (-51.2f + 60) * 2 / 5), "volume 7: straight between 5 and 10");
        check(near(board_vol_db(30), -31.8f) && near(board_vol_db(90), -4.0f), "the Show's own: 30 at index 9, 90 at index 27");
    }
    if (!board.volume_curve)
        check(near(board_vol_db(30), -30.5f) && near(board_vol_db(5), -47.3f) && near(board_vol_db(25), -33.1f), "the Echo Dot 2's");
    return fails ? 1 : 0;
}
