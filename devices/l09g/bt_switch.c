/* Desired BlueZ pairing state, persisted for the local Bluetooth service. */
#include "statefile.h"
#include <stdio.h>
#include <stdlib.h>

int l09g_bt_power(int set)
{
    char path[320];
    const char *state = getenv("HASSMIC_STATE");
    snprintf(path, sizeof path, "%s/bluetooth.enabled", state ? state : "/data/l09g-assist/state");
    if (set >= 0) {
        const char *value = set ? "1\n" : "0\n";
        if (statefile_write(path, 0600, value, 2)) fprintf(stderr, "bluetooth: cannot save switch state\n");
    }
    FILE *f = fopen(path, "r");
    if (!f) return 0; /* New pairings require an explicit request. */
    int c = fgetc(f); fclose(f);
    return c == '1';
}
