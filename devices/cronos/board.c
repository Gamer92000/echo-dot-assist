/* Echo Show 5 2nd gen 2021 (cronos), Fire OS 6574.1 (NS65741/8149).  checkers' platform and build, with its own mute
 * button driver and speaker amplifiers.  Worked out from the firmware (kernel config, device trees, init files) and
 * checked on the first cronos, 2026-10-26 (docs/re-cronos.md).  Values marked "verify" are not checked yet. */
#include "board.h"
#include <stddef.h>

/* No earcon folder, as on checkers: stock's sounds are resources of its apps, all stored uncompressed (sounds.c reads
 * them out of the APKs).  The same entries as checkers', each found stored in this firmware's APKs; SpeechInteractionManager's
 * resources.arsc names ful_ui_wakesound_hybrid, res/7E.mp3 as on checkers.  No setup beacon in any app: the built-in
 * tone stays for Identify. */
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

/* The speaker amplifier depends on the board revision (the kernel carries a device tree per revision, picked by lk; in
 * the order of CONFIG_BUILD_ARM_APPENDED_DTB_IMAGE_NAMES): RT5616E line out (proto to DVT, PVT), RT5616 line out (DVT
 * 3.2, PVT 4.2; both the control checkers has), TI TAS5805M (DVT 3.1, 3.3, PVT 4.1) or Maxim MAX98396 (PVT 4.3).  Each has its own audio_device_<amp>.xml in /system/etc; the names below
 * are the controls as this kernel spells them; which one a unit has: tinymix, or the I2C device names (the first
 * cronos tried, 2026-10-26: max98396 at 2-003d). */
static const struct board_gain speaker_gains[] = {
    /* TAS5805M: the digital volume, as on the Echo Show 8 2nd gen (athena: 0..158, higher is louder, 0.5 dB a step
     * assumed: the chip's own; 118 was its speaker's limit there).  The HAL sets 110 here; +4 dB at most until heard */
    { "Master TAS5805m Playback Volume A", 110, 2, 0, 0, 118 },
    /* MAX98396: its speaker gain, 0..17 = +4..+21 dB, 1 dB a step (the kernel's TLV: DB_RANGE 0..0x11, DB_SCALE 400/100);
     * the HAL sets 8, +12 dB (audio_device_max98396.xml), read so on the first cronos (2026-10-26).  "Digital Volume A"
     * stands at 127, its top: no headroom there.  +6 dB at most until someone has listened at full volume */
    { "Speaker Volume A", 8, 1, 1, 0, 14 },
    { NULL, 0, 0, 0, 0, 0 },
};

const struct board board = {
    .model = "Echo Show 5 2nd gen (cronos)",
    .project = "hassmic.echo-show-5-2nd-gen",
    .product = "Echo Show 5 2nd gen (hassmic)",
    .default_name = "Echo Show 5 2nd gen",                         /* + MAC: "Echo Show 5 2nd gen 5695c4"; slug = the project's model */
    .codename = "cronos",

    /* gpio-keys: volume up/down and the camera shutter (EV_SW 9), as on checkers (device trees).  No action button.  By
     * name: the event numbers depend on the probe order */
    .keypad = "name:gpio-keys",
    .action_combo = 1,                                              /* the settings page's login, adb, Improv */
    .grab_keys = 1,                                                 /* else PhoneWindowManager turns its stream volume too */
    /* Not checkers' amazon-gating: the gpio-privacy driver (CONFIG_KEYBOARD_GPIO_PRIVACY, no CONFIG_GATING), as on athena.
     * state is chowned to system by /init.project.rc; its input devices are the latch ("gpio-privacy-state", SW_MUTE_DEVICE)
     * and the button ("gpio-privacy-button", KEY_POWER): getevent -il on the first cronos (event0, event1), presses seen.  The
     * device tree holds the enable line for enable-gpio-toggle-duration = 1000 ms before the state changes, as athena's
     * (verify the timing) */
    .privacy_state = "/sys/devices/platform/gpio-privacy/state",
    .privacy_input = "name:gpio-privacy-state",
    .privacy_latch = 1,
    .privacy_button = "name:gpio-privacy-button",
    .privacy_settle_ms = 2000,

    .bt_dev = "/dev/stpbt",                                         /* MT7668 combo, btmtksdio.ko (ueventd: bluetooth:net_bt_stack) */
    .bt_service = NULL,                                             /* none: Android's stack (app/Bluetooth) is off the partition */
    .bt_mac = "/proc/idme/bt_mac_addr",

    .wake_id = "alexa",
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",
    .earcon_dir = "/data/local/hassmic/earcon/",                    /* none of stock's: own sounds here win over the apps' */
    .earcon_zip = sounds,
    .thermal_type = "mtktscpu",
    /* MediaTek's alsps framework as on checkers; the device trees name a JSA1214 (checkers: STK3x1x) and the kernel has
     * CONFIG_ALS_FORMULA_CHANGE.  checkers' lux attribute is there: 250 lux in a lit room on the first cronos (2026-10-26) */
    .light_sensor = { "/sys/bus/platform/drivers/als_ps/lux" },
    .volume_steps = 100,                                            /* the light band (ring_band.c) shows the level in % */
    /* RT5616 / RT5616E boards: checkers' control and stock (audio_device.xml and audio_device_rt5616.xml both set 29,
     * -3 dB, 1.5 dB a step); other amplifiers in speaker_gains above */
    .speaker_gain = "OUT Playback Volume",
    .speaker_gain_stock = 29,
    .speaker_gain_steps = 2,
    .speaker_gain_db = 3,
    .speaker_gain_alt = speaker_gains,
};
