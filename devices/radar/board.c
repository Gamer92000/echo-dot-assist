/* Echo 2nd gen 2017 (radar), Fire OS 6572.  Values checked on the running Echo (2026-09-28): getevent -il, thermal
 * zones, /proc/idme, led-resources.  Input is laid out like biscuit's, not donut's. */
#include "board.h"
#include <stddef.h>

const struct board board = {
    .model = "Echo 2 (radar)",
    .project = "hassmic.echo-2",
    .product = "Echo 2 (hassmic)",
    .default_name = "Echo 2",
    .codename = "radar",

    /* mtk-kpd: action (•) = KEY_HELP, mic mute = KEY_MUTE (and a volume key); "keys": volume ± (donut's event3 does
     * not exist here, so no button worked) */
    .keypad = "/dev/input/event1",
    .keypad2 = "/dev/input/event2",
    /* No gpio-privacy device as on donut, but the keypad driver keeps the mute latch: amz_privacy/state, the file init
     * hands to ace_button_mgr (init.mt8163_amazon.rc).  Read 1 with the button lit and the mics cut (2026-09-30).
     * Changes arrive as KEY_MUTE on the keypad. */
    .privacy_state = "/sys/devices/soc/10010000.keypad/amz_privacy/state",
    .privacy_input = NULL,
    .privacy_latch = 1,

    .bt_dev = "/dev/stpbt",                                         /* MediaTek combo, bluetooth:net_bt_stack */
    .bt_service = "btmanagerd",
    .bt_mac = "/proc/idme/bt_mac_addr",                             /* 12 hex digits */

    .wake_id = "alexa",
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",   /* shipped in this build */
    .earcon_dir = "/system/local/share/earcon/base/",               /* same layout as donut */
    .thermal_type = "mtktscpu",                                     /* thermal_zone1 */
    /* TSL2540 at 0-0039 on this Echo; the HAL (als_radar_puffin_facade) first tries a TSL2584 at 0-0029, which is on the
     * bus but has no driver bound here: another hardware revision */
    .light_sensor = { "/sys/bus/i2c/devices/0-0029/iio:device0/calibrated_lux", "/sys/bus/i2c/devices/0-0039/als_calibrated_lux" },
    .volume_steps = 30,                                             /* volume_step-01..30 in led-resources */
};
