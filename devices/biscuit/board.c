/* Echo Dot 2nd gen 2016 (biscuit), Fire OS 6574.1.  Values checked on the running Echo (2026-09-28, the rest
 * 2026-09-30); device facts: docs/re-platform.md shows what each field is for (as found on donut). */
#include "board.h"
#include <stddef.h>

const struct board board = {
    .model = "Echo Dot 2 (biscuit)",
    .project = "hassmic.echo-dot-2",
    .product = "Echo Dot 2 (hassmic)",
    .default_name = "Echo Dot 2",
    .codename = "biscuit",

    /* event1 "mtk-kpd": action (•) = KEY_HELP, mic mute = KEY_MUTE, volume down; event2 "keys": volume ± (getevent -il) */
    .keypad = "/dev/input/event1",
    .keypad2 = "/dev/input/event2",
    /* Mute latch kept by the keypad driver, as on radar (same SoC, same line in init.mt8163_amazon.rc).  On the Echo:
     * world-readable, 0 with the mics on; not yet read with the button lit.  Read only this file of the directory:
     * power_button_state beside it takes the kernel down (NULL gpio in get_power_button_state, watchdog reboot). */
    .privacy_state = "/sys/devices/soc/10010000.keypad/amz_privacy/state",
    .privacy_input = NULL,
    .privacy_latch = 1,

    .bt_dev = "/dev/stpbt",                                         /* bluetooth:net_bt_stack on the Echo */
    .bt_service = "btmanagerd",
    .bt_mac = "/proc/idme/bt_mac_addr",                             /* 12 hex digits, read on the Echo */

    .wake_id = "alexa",
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",   /* confirmed on the Echo */
    .earcon_dir = "/system/local/share/earcon/base/",
    .thermal_type = "mtktscpu",                                     /* thermal_zone1 on the Echo */
    /* TSL2540 at 0-0039 on this Echo; the HAL (als_lidar_puffin_facade) first tries a TSL2584 at 0-0029, which is on the
     * bus but has no driver bound here: another hardware revision */
    .light_sensor = { "/sys/bus/i2c/devices/0-0029/iio:device0/calibrated_lux", "/sys/bus/i2c/devices/0-0039/als_calibrated_lux" },
    .volume_steps = 30,                                             /* /system/etc/led-resources/volume_step-01..30 */
};
