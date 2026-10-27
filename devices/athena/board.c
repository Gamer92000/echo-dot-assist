/* Echo Show 8 2nd gen 2021 (athena), Fire OS 7574.1 (PS75741, build 7080).  MediaTek MT8183, Android 9, 8" touchscreen,
 * TAS5805m speaker amplifier, TCS3400 light sensor, a privacy gating driver as on the Dots.  Probed over adb on a running
 * Echo Show 8 on 2026-10-14, then installed on it (docs/re-athena.md). */
#include "board.h"
#include <stddef.h>

/* No earcon folder, as on checkers and crown: stock's sounds are resources of its apps, stored uncompressed (sounds.c reads
 * them out of the APKs, and out of /system/hassmic-removed once the install took the apps off the partition).  crown's entries
 * and the wake sound, checked in this firmware's APKs (aapt dump resources, 2026-10-14).  Unlike crown's, athena's
 * SpeechInteractionManager has ful_ui_wakesound_hybrid (res/7E.mp3, as on checkers; ban_ui_wakesound is res/-E.ogg, the
 * other product's) and ful_ui_endpointing (res/Eh.mp3, not used).  No setup beacon in any app: the built-in tone stays for Identify. */
#define SIM "/system/priv-app/SpeechInteractionManager/SpeechInteractionManager.apk"
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
    .model = "Echo Show 8 2nd gen (athena)",
    .project = "hassmic.echo-show-8-2nd-gen",
    .product = "Echo Show 8 2nd gen (hassmic)",
    .default_name = "Echo Show 8 2nd gen",                          /* + MAC; slug = the project's model */
    .codename = "athena",

    /* gpio-keys: volume up/down and the camera shutter (EV_SW 9).  No action button.  By name: the event numbers depend
     * on the probe order (getevent -il: gpio-keys is event4 on the Echo Show 8, 2026-10-14) */
    .keypad = "name:gpio-keys",
    .action_combo = 1,                                              /* the settings page's login, adb, Improv */
    .grab_keys = 1,                                                 /* else PhoneWindowManager turns its stream volume too */
    /* the gpio-privacy driver (as donut's): state is 1 with the mics muted, its two input devices are the latch ("gpio-privacy-state",
     * SW_MUTE_DEVICE) and the button ("gpio-privacy-button", KEY_POWER).  state is system's, 0444 */
    .privacy_state = "/sys/devices/platform/gpio-privacy/state",
    .privacy_input = "name:gpio-privacy-state",
    .privacy_latch = 1,
    /* the driver holds the enable line for enable-gpio-toggle-duration (1000 ms, device tree) before it changes state and
     * reports it: measured 2026-10-15, button event to latch 1.23 and 1.29 s muting, 0.28 s unmuting, while the red light
     * under the button follows the press at once.  The press itself is early enough to show it */
    .privacy_button = "name:gpio-privacy-button",
    .privacy_settle_ms = 2000,

    .bt_dev = "/dev/stpbt",                                         /* btmtksdio.ko */
    /* no init service of Amazon's: the HIDL HAL that holds the node (bluetooth-1-0) is stopped by alexa-off.sh (device.conf
     * BT_SERVICE), Android's Bluetooth app is off the partition (STOCK_APPS_OUT) */
    .bt_service = NULL,
    .bt_mac = "/proc/idme/bt_mac_addr",

    .wake_id = "alexa",
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",
    .earcon_dir = "/data/local/hassmic/earcon/",
    .earcon_zip = sounds,
    .thermal_type = "mtktscpu",
    /* TCS3400 (RGBC), an input device of its own whose attributes carry the raw channels; its als_lux stays 0 (no
     * calibration table), so main.c computes the lux as stock's virtual_als does.  The input's number is not fixed, but
     * this driver's name is. */
    .light_sensor = { "tcs3400:/sys/devices/virtual/input" },
    .volume_steps = 100,                                            /* the light band (ring_band.c) shows the level in % */
    /* TI TAS5805m amplifier: its "Master TAS5805m Playback Volume" is a digital volume, 0..158, 100 at boot, no dB table
     * (0.5 dB steps assumed: the chip's own).  Listened to with music at full volume, 2026-10-15: 115 and 120 clean, 122
     * clean, 130 distorted ("gross"); a rap track's bass then clipped at 122, so 118 is the limit: +9 dB on that assumption.  The
     * audio HAL did not rewrite it after a voice reply or a change of the setting (2026-10-15); the watcher stays, as on the Dot 2. */
    .speaker_gain = "Master TAS5805m Playback Volume",
    .speaker_gain_stock = 100,
    .speaker_gain_steps = 2,
    .speaker_gain_limit = 118,
};
