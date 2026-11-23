#include "board.h"
/* The microphone latch is active high; event codes were measured on this unit. */
const struct board board = {
    .model = "Mi Smart Speaker L09G",
    .project = "hassmic.l09g",
    .product = "Mi Smart Speaker L09G",
    .default_name = "Mi Smart Speaker",
    .codename = "l09g",
    .keypad = "name:keys-mil09g",
    .privacy_state = "/sys/devices/platform/soc/ff800000.aobus/ff805000.i2c/i2c-1/1-000a/mute_status",
    .privacy_latch = 1,
    .volume_steps = 24,
    .thermal_type = "soc_thermal",
    .wake_id = "none",
    .volume_curve = board_vol_dot2,
};
