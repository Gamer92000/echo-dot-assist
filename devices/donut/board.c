/* Echo Dot 3rd gen 2018 (donut), Fire OS 6574.1.  Device facts: docs/re-platform.md. */
#include "board.h"

const struct board board = {
    .model = "Echo Dot 3 (donut)",
    .project = "hassmic.echo-dot-3",
    .product = "Echo Dot 3 (hassmic)",
    .default_name = "Echo Dot 3",
    .codename = "donut",

    .keypad = "/dev/input/event3",                                  /* gpio-keys: action, volume */
    .privacy_state = "/sys/devices/platform/gpio-privacy/state",
    .privacy_input = "/dev/input/event1",                           /* gpio-privacy driver, not the keypad */
    .privacy_latch = 1,

    .bt_dev = "/dev/stpbt",                                         /* MediaTek WMT combo chip */
    .bt_service = "btmanagerd",
    .bt_mac = "/proc/idme/bt_mac_addr",                             /* 12 hex digits, as btmac.sh reads it */

    .wake_id = "alexa",
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",
    .earcon_dir = "/system/local/share/earcon/base/",
    .thermal_type = "mtktscpu",
    .light_sensor = { "/sys/bus/i2c/devices/0-0039/iio:device0/calibrated_lux" },  /* TSL2572 (IIO), als_donut_puffin_facade */
    .volume_steps = 30,
};
