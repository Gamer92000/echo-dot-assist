#include "diag.h"
#include "board.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

float diag_soc_temp(void)
{
    char path[80], type[32]; float t = NAN;
    for (int z = 0; z < 16; z++) {
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", z);
        FILE *f = fopen(path, "r"); if (!f) break;
        int ok = fscanf(f, "%31s", type) == 1; fclose(f);
        if (!ok || strcmp(type, board.thermal_type)) continue;
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        if ((f = fopen(path, "r"))) { int mc; if (fscanf(f, "%d", &mc) == 1) t = mc / 1000.0f; fclose(f); }
        break;
    }
    return t;
}

static int stat_read(unsigned long long *busy, unsigned long long *total)
{
    unsigned long long v[8] = { 0 }; FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4) return -1;
    *total = 0; for (int i = 0; i < 8; i++) *total += v[i];
    *busy = *total - v[3] - v[4];                               /* idle, iowait */
    return 0;
}

float diag_cpu(void)
{
    unsigned long long b0, t0, b1, t1;
    if (stat_read(&b0, &t0)) return NAN;
    usleep(250000);
    if (stat_read(&b1, &t1) || t1 <= t0) return NAN;
    return 100.0f * (float)(b1 - b0) / (float)(t1 - t0);
}
