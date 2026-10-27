/* Fire TV Cube 3, gazelle, Fire OS PS7717.5741N. Already unlocked only. */
#include "board.h"
const struct board board = {
    .model="Fire TV Cube 3 (gazelle)", .project="cube-assist.fire-tv-cube-3",
    .product="Fire TV Cube 3 (Cube Assist)", .default_name="Fire TV Cube 3",
    .codename="gazelle", .fire_tv=1, .keypad="/dev/input/event6",
    .privacy_state="/sys/devices/platform/gpio-privacy/state",
    .privacy_input="/dev/input/event4", .privacy_latch=1,
    /* The Fire TV Bluetooth stack keeps ownership of the remote/controller. */
    .bt_dev="/dev/cube-assist-no-raw-bluetooth", .bt_service="", .bt_mac="",
    .wake_id="alexa", .wake_manifest="/system/local/models/keyword/en-GB/ALEXA/pryon.manifest",
    .earcon_dir="/system/local/share/earcon/base/", .thermal_type="soc_thermal",
    .volume_steps=30,
};
