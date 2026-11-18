/* Lux from a TCS3400's raw channels (main.c lux_tcs3400 reads them; athena).  ams' DN40 for the part: IR = (R+G+B-C)/2
 * taken off each colour, lux = (0.136 R + G - 0.444 B) / CPL, CPL = itime * gain / (GA * DGF) with DGF 310.  GA, the
 * glass in front of the sensor, is the one number the part's note leaves to the product: the glass passes 0.103 of the
 * light (GA = 1/0.103), which fits stock's own value (virtual_als, read from `dumpsys sensorservice` beside the raw
 * channels, 2026-10-14): 2.7 lx at clear 182 / gain 64, 3003 lx at clear 36335 / gain 16, 2100 lx at clear 27202 /
 * gain 16: within 10 %, the readings being taken a second or two apart while the light moved. */
#ifndef LUX_H
#define LUX_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline float lux_tcs3400_calc(double r, double g, double b, double c, double itime_ms, double gain)
{
    double ir = (r + g + b - c) / 2, cpl = itime_ms * gain * 0.103 / 310, l;
    if (itime_ms <= 0 || gain <= 0) return -1;
    if (ir < 0) ir = 0;
    l = (0.136 * (r - ir) + (g - ir) - 0.444 * (b - ir)) / cpl;
    return (float)(l < 0 ? 0 : l);
}

/* Lux from a TSL2540's two channels (cannoli: iio:device0 of i2c 4-0039, main.c lux_tsl2540 reads them), as stock's
 * libacehal_ambientLightSensor.so computes it (calc 0x4318, getLux 0x4430; read from its disassembly, 2026-11-15).
 * Uncalibrated: r = ir/vis blends two linear forms of row capcolor (idme alsparams; the cover's colour, by its name) by
 * smootherstep between c[6] and c[7], divided by the real gain and the integration time in ms.  Calibrated: a straight
 * line through the unit's two factory points (idme alscal: counts at 20 and 400 lx, taken at alsparams' gain and
 * time).  Checked against stock's own alsctrl on the Echo with the same idme data: 1866/242 -> 50, 1939/249 -> 52
 * (lux_test.c).  Without the calibration stock answers 0 for good; here there is no reading then. */
static const double tsl2540_rows[8][8] = {
    { 212.3177, 53.07943, 53.07943, 243.4285, 60.85712, -9.29433, 0.24941, 1 },         /* c[2] = c[1] as shipped */
    { 116.1907, 1.120936, -0.18, 24.30953, 6.077383, -1.01792, 0.329448, 1 },
    { 226.9058, 56.72645, -11.7879, 239.7658, 59.94146, -9.90576, 0.282603, 1 },
    { 560.9854676, 17.40776771, -2.591608011, 293.0102725, 73.25256822, -11.39621282, 0.251987727, 1 },
    { 37.65063, 9.412658, -2.15521, 38.92021, 9.730053, -1.48395, 0.282607, 1 },
    { 565.8752297, 11.63762545, -3.504493061, 246.9294843, 61.7323711, -9.858968887, 0.251987727, 1 },
    { 185.5159, 46.37898, 9.090086, 207.656, 51.91401, -9.66167, 0.268493, 1 },
    { 320.6533, 12.99457, -0.06072, 141.0411, 35.26027, -6.3573, 0.278486, 1 },
};
/* alsgain's index and the sysfs calibscale it writes -> the gain the formula takes */
static const double tsl2540_gain[6] = { 0.5, 1, 4, 16, 64, 128 };
static const int tsl2540_calibscale[6] = { 1, 2, 8, 32, 128, 256 };

struct tsl2540_cal { int row, gain; double time_ms; unsigned vis20, ir20, vis400, ir400; int have; };

static inline double tsl2540_real_gain(int calibscale)
{
    for (int i = 0; i < 6; i++) if (tsl2540_calibscale[i] == calibscale) return tsl2540_gain[i];
    return 0;
}

static inline double lux_tsl2540_raw(double vis, double ir, double gain, double t_ms, int row)
{
    const double *c = tsl2540_rows[row & 7];
    if (vis <= 0 || gain <= 0 || t_ms <= 0) return 0;
    double x = (ir / vis - c[6]) / (c[7] - c[6]), s = x <= 0 ? 0 : x >= 1 ? 1 : x * x * x * (10 + x * (-15 + 6 * x));
    return ((1 - s) * c[0] * (c[1] * vis + c[2] * ir) + s * c[3] * (c[4] * vis + c[5] * ir)) / (gain * t_ms);
}

/* idme's two strings, as stock parses them: alsparams by key (alsgain an index 0..5, default 5; alstime in ms, default
 * 721; capcolor a row, 8 and up taken as 0), alscal all nine hex fields or nothing (field 2 of each point and the 0 lx
 * point are read and not used) */
static inline void tsl2540_parse(struct tsl2540_cal *k, const char *params, const char *cal)
{
    const char *p; unsigned u[9];
    k->row = 0; k->gain = 5; k->time_ms = 721; k->have = 0;
    if (params && (p = strstr(params, "alsgain="))) k->gain = atoi(p + 8);
    if (params && (p = strstr(params, "alstime="))) k->time_ms = atof(p + 8);
    if (params && (p = strstr(params, "capcolor="))) k->row = atoi(p + 9);
    if (k->row < 0 || k->row > 7) k->row = 0;
    if (k->gain < 0 || k->gain > 5) k->gain = 5;
    if (cal && sscanf(cal, "ams_0_0=%x,%x,%x ams_20_0=%x,%x,%x ams_400_0=%x,%x,%x",
                      &u[0], &u[1], &u[2], &u[3], &u[4], &u[5], &u[6], &u[7], &u[8]) == 9) {
        k->vis20 = u[3]; k->ir20 = u[5]; k->vis400 = u[6]; k->ir400 = u[8]; k->have = 1;
    }
}

/* -1: no calibration (or a degenerate one), a calibscale outside stock's six, or no integration time */
static inline float lux_tsl2540_calc(double vis, double ir, int calibscale, double itime_s, const struct tsl2540_cal *k)
{
    if (!k->have || tsl2540_real_gain(calibscale) <= 0 || itime_s <= 0) return -1;     /* not a gain stock sets */
    double g = tsl2540_gain[k->gain],
           u = lux_tsl2540_raw(vis, ir, tsl2540_real_gain(calibscale), itime_s * 1000, k->row),
           u20 = lux_tsl2540_raw(k->vis20, k->ir20, g, k->time_ms, k->row),
           u400 = lux_tsl2540_raw(k->vis400, k->ir400, g, k->time_ms, k->row);
    if (u400 - u20 <= 0) return -1;
    double l = 20 + (u - u20) * (400 - 20) / (u400 - u20);
    return (float)(l > 0 ? l : 0);
}
#endif
