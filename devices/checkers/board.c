/* Echo Show 5 1st gen 2019 (checkers), Fire OS 6574.1.  Device facts from the firmware only (no Echo Show tried yet):
 * docs/re-checkers.md.  Values marked "verify" need the running device. */
#include "board.h"
#include <stddef.h>

/* No earcon folder in this firmware: stock's sounds are resources of its apps, all stored uncompressed (sounds.c reads
 * them out of the APKs).  SpeechInteractionManager's names are shrunk (res/7E.mp3); its resources.arsc maps them back:
 * ful_ui_wakesound_hybrid = res/7E.mp3 (ban_ui_wakesound, res/-E.ogg, is the other product's, and Ogg).  No setup
 * beacon in any app: the built-in tone stays for Identify. */
#define SIM  "/system/priv-app/SpeechInteractionManager/SpeechInteractionManager.apk"
#define KSUI "/system/priv-app/KnightSystemUI/KnightSystemUI.apk"
#define KSET "/system/priv-app/KnightSettings/KnightSettings.apk"
#define COMM "/system/priv-app/com.amazon.comms.multimodaltachyonarm/com.amazon.comms.multimodaltachyonarm.apk"
static const struct board_sound sounds[] = {
    { "ui_wakesound", SIM, "res/7E.mp3" },
    { "ui_wakesound_touch", SIM, "res/7E.mp3" },
    { "state_privacy_mode_on", KSUI, "res/raw/kni_controls_privacy_mode_on.mp3" },
    { "state_privacy_mode_off", KSUI, "res/raw/kni_controls_privacy_mode_off.mp3" },
    { "state_volume_adjust_tone", KSUI, "res/raw/kni_controls_volume_adjust.mp3" },
    { "state_bluetooth_connected", KSET, "res/raw/kni_system_bluetooth_bt_connected.mp3" },
    { "state_bluetooth_disconnected", KSET, "res/raw/kni_system_bluetooth_bt_disconnected.mp3" },
    { "comms_drop_in_incoming", COMM, "res/raw/ful_comms_drop_in_incoming.mp3" },
    { "comms_call_connected", COMM, "res/raw/ful_comms_call_connected.mp3" },
    { "comms_call_disconnected", COMM, "res/raw/ful_comms_call_disconnected.mp3" },
    { "comms_call_incoming_ringtone", COMM, "res/raw/ful_comms_call_incoming_ringtone.mp3" },
    { "comms_outbound_ringtone", COMM, "res/raw/ful_comms_outbound_ringtone.mp3" },
    { NULL, NULL, NULL },
};

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
    .earcon_dir = "/data/local/hassmic/earcon/",                    /* none of stock's: own sounds here win over the apps' */
    .earcon_zip = sounds,
    .thermal_type = "mtktscpu",
    .light_sensor = { NULL },                                       /* STK3x1x on MediaTek's sensor hub, no lux file; verify */
    .volume_steps = 0,                                              /* no LED ring */
};
