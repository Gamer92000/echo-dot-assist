/* lux.h against stock's own values: readings taken together with `dumpsys sensorservice`'s virtual_als (2026-10-14). */
#include "lux.h"
#include <math.h>
#include <stdio.h>

static int fails;
static void near(double got, double want, const char *what)
{
    int ok = fabs(got - want) <= want * 0.12;       /* the pairs were read a second or two apart, in moving light */
    printf("%s %s: %.2f (stock %.2f)\n", ok ? "ok  " : "FAIL", what, got, want);
    if (!ok) fails++;
}

int main(void)
{
    near(lux_tcs3400_calc(97, 50, 48, 182, 700, 64), 2.7, "dark room, gain 64");
    near(lux_tcs3400_calc(16240, 14350, 9044, 36335, 700, 16), 3003, "bright, gain 16");
    near(lux_tcs3400_calc(12199, 10776, 6714, 27202, 700, 16), 2100, "bright, gain 16, another");
    printf("%s no integration time: no answer\n", lux_tcs3400_calc(1, 1, 1, 1, 0, 64) < 0 ? "ok  " : "FAIL");
    printf("%s no light: 0\n", lux_tcs3400_calc(0, 0, 0, 0, 700, 64) == 0 ? "ok  " : "FAIL");
    return fails;
}
