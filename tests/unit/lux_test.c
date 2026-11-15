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

static void exact(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

int main(void)
{
    near(lux_tcs3400_calc(97, 50, 48, 182, 700, 64), 2.7, "dark room, gain 64");
    near(lux_tcs3400_calc(16240, 14350, 9044, 36335, 700, 16), 3003, "bright, gain 16");
    near(lux_tcs3400_calc(12199, 10776, 6714, 27202, 700, 16), 2100, "bright, gain 16, another");
    printf("%s no integration time: no answer\n", lux_tcs3400_calc(1, 1, 1, 1, 0, 64) < 0 ? "ok  " : "FAIL");
    printf("%s no light: 0\n", lux_tcs3400_calc(0, 0, 0, 0, 700, 64) == 0 ? "ok  " : "FAIL");

    /* TSL2540 (cannoli), against stock's alsctrl on the Echo, 2026-11-15: it prints whole numbers (truncated), read
     * together with the raw channels.  With idme's data (-P alsparams -C alscal): uncalibrated, calibrated */
    struct tsl2540_cal k;
    tsl2540_parse(&k, "alstime=700 alsgain=5 capcolor=4 ", "ams_0_0=0,0,0 ams_20_0=2E2,4,7C ams_400_0=3AF5,52,9F3 ");
    exact(k.have && k.row == 4 && k.gain == 5 && k.time_ms == 700 && k.vis20 == 738 && k.ir20 == 124 && k.vis400 == 15093
          && k.ir400 == 2547, "idme parsed");
    exact((int)lux_tsl2540_raw(1866, 242, tsl2540_real_gain(256), 700, 4) == 7, "1866/242 uncalibrated: 7");
    exact((int)lux_tsl2540_calc(1866, 242, 256, 0.700, &k) == 50, "1866/242: 50 lx");
    exact((int)lux_tsl2540_calc(1939, 249, 256, 0.700, &k) == 52, "1939/249: 52 lx");
    /* without -P/-C alsctrl takes row 0 and no calibration: its uncalibrated 278, 279, 274 */
    exact((int)lux_tsl2540_raw(1962, 252, 128, 700, 0) == 278 && (int)lux_tsl2540_raw(1971, 253, 128, 700, 0) == 279 &&
          (int)lux_tsl2540_raw(1936, 250, 128, 700, 0) == 274, "row 0, uncalibrated: 278, 279, 274");
    struct tsl2540_cal none; tsl2540_parse(&none, "alstime=700 alsgain=5 capcolor=4", "ams_0_0=0,0,0");
    exact(lux_tsl2540_calc(1866, 242, 256, 0.700, &none) < 0, "no calibration: no reading");
    exact(lux_tsl2540_calc(1866, 242, 100, 0.700, &k) < 0 && lux_tsl2540_calc(1866, 242, 256, 0, &k) < 0,
          "a calibscale stock never sets, no integration time: no reading (not the line's 0.45 lx)");
    /* the line through the 20 and 400 lx points meets 0 counts a little above 0 lx: 20 - u20 * 380 / (u400 - u20) */
    float dark = lux_tsl2540_calc(0, 0, 256, 0.700, &k);
    exact(dark > 0.4 && dark < 0.5, "dark: 0.45 lx, as stock's line has it");
    tsl2540_parse(&none, "capcolor=9", NULL);
    exact(none.row == 0 && none.gain == 5 && none.time_ms == 721, "capcolor 9 is row 0; stock's defaults");
    return fails;
}
