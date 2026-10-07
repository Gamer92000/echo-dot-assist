/* Echo Show 5 1st gen 2019 (checkers), Fire OS 6574.1.  Device facts from the firmware only (no Echo Show tried yet):
 * docs/re-checkers.md.  Values marked "verify" need the running device. */
#include "board.h"
#include <stddef.h>

const struct board board = {
    .model = "Echo Show 5 (checkers)",
    .project = "hassmic.echo-show-5",
    .product = "Echo Show 5 (hassmic)",
    .default_name = "Echo Show",
    .codename = "checkers",

    /* gpio-keys: volume up/down only, and from the EVT boards on the camera shutter (EV_SW 9).  No action button.
     * verify: event numbers with getevent -il */
    .keypad = "/dev/input/event2",
    .privacy_state = "/sys/devices/platform/amazon-gating/state",   /* chowned to system by /init.project.rc */
    .privacy_input = NULL,                                          /* verify: the amazon-gating driver's input device */
    .privacy_latch = 1,

    .bt_dev = "/dev/stpbt",                                         /* MT7668 combo, btmtksdio.ko */
    .bt_service = NULL,                                             /* Android's com.android.bluetooth, not an init service */
    .bt_mac = "/proc/idme/bt_mac_addr",

    .wake_id = "alexa",
    /* the newer model layout (encoder/decoder, NTT); loads in pryon_test under qemu and detects ALEXA */
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",
    /* no earcon folder in the firmware: the sounds are inside SpeechInteractionManager and KnightSystemUI, to be
     * extracted into this one at install */
    .earcon_dir = "/data/local/hassmic/earcon/",
    .thermal_type = "mtktscpu",
    .light_sensor = { NULL },                                       /* STK3x1x on MediaTek's sensor hub, no lux file; verify */
    .volume_steps = 0,                                              /* no LED ring */
};
