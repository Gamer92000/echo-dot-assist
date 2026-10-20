/* Lux from a TCS3400's raw channels (main.c lux_tcs3400 reads them; athena).  ams' DN40 for the part: IR = (R+G+B-C)/2
 * taken off each colour, lux = (0.136 R + G - 0.444 B) / CPL, CPL = itime * gain / (GA * DGF) with DGF 310.  GA, the
 * glass in front of the sensor, is the one number the part's note leaves to the product: the glass passes 0.103 of the
 * light (GA = 1/0.103), which fits stock's own value (virtual_als, read from `dumpsys sensorservice` beside the raw
 * channels, 2026-10-14): 2.7 lx at clear 182 / gain 64, 3003 lx at clear 36335 / gain 16, 2100 lx at clear 27202 /
 * gain 16: within 10 %, the readings being taken a second or two apart while the light moved. */
#ifndef LUX_H
#define LUX_H

static inline float lux_tcs3400_calc(double r, double g, double b, double c, double itime_ms, double gain)
{
    double ir = (r + g + b - c) / 2, cpl = itime_ms * gain * 0.103 / 310, l;
    if (itime_ms <= 0 || gain <= 0) return -1;
    if (ir < 0) ir = 0;
    l = (0.136 * (r - ir) + (g - ir) - 0.444 * (b - ir)) / cpl;
    return (float)(l < 0 ? 0 : l);
}
#endif
