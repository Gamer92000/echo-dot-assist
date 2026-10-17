/* Echo Show 8 1st gen 2019 (crown), Fire OS 6.5.7.0 (NS6570).  The same platform as checkers (MT8163, RT5616, gating latch,
 * MT7668 Bluetooth), probed over adb on a running Echo Show 8 on 2026-10-13.  Values marked "verify" are not checked
 * against a running hassmic yet. */
#include "board.h"
#include <stddef.h>

/* No earcon folder in this firmware: stock's sounds are resources of its apps, all stored uncompressed (sounds.c reads
 * them out of the APKs).  No wake sound listed: SpeechInteractionManager here has no ful_ui_wakesound_hybrid, and its
 * ban_ui_wakesound is res/eb.wav, an empty file, and so is ban_ui_endpointing (res/ie.wav): no APK or library of this
 * firmware has the sound (aapt dump resources over every app, 2026-10-14), the built-in tone is the one it plays.  No setup beacon in
 * any app either: the built-in tone stays for Identify. */
#define KSUI "/system/priv-app/KnightSystemUI/KnightSystemUI.apk"
#define KSET "/system/priv-app/KnightSettings/KnightSettings.apk"
#define COMM "/system/priv-app/com.amazon.comms.multimodaltachyonarm/com.amazon.comms.multimodaltachyonarm.apk"
static const struct board_sound sounds[] = {
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
    .model = "Echo Show 8 (crown)",
    .project = "hassmic.echo-show-8",
    .product = "Echo Show 8 (hassmic)",
    .default_name = "Echo Show 8",                                 /* + MAC: "Echo Show 8 5695c4"; slug = the project's model */
    .codename = "crown",

    /* gpio-keys: volume up/down only, and from the EVT boards on the camera shutter (EV_SW 9).  No action button.  By
     * name: the event numbers depend on the probe order (the kernel's DTBs: gpio-keys has no label, so the input device
     * takes the platform device's name; MediaTek's matrix keypad is "mtk-kpd", nothing on it here) */
    .keypad = "name:gpio-keys",
    .action_combo = 1,                                              /* the settings page's login, adb, Improv */
    .grab_keys = 1,                                                 /* else PhoneWindowManager turns its stream volume too */
    .privacy_state = "/sys/devices/platform/amazon-gating/state",   /* chowned to system by /init.project.rc */
    /* the gating driver's own input device: "mute", KEY_POWER (0x74, DTB); any event there means a latch read
     * (buttons.c privacy_reader), the once-a-second read of the state catches it otherwise.  getevent -il on crown
     * (2026-10-13): "gating", /dev/input/event0, KEY_POWER */
    .privacy_input = "name:gating",
    .privacy_latch = 1,

    .bt_dev = "/dev/stpbt",                                         /* MT7668 combo, btmtksdio.ko */
    .bt_service = NULL,                                             /* none: Android's stack (app/Bluetooth) is off the partition */
    .bt_mac = "/proc/idme/bt_mac_addr",

    .wake_id = "alexa",
    /* the newer model layout (encoder/decoder, NTT); loads in pryon_test under qemu and detects ALEXA */
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",
    .earcon_dir = "/data/local/hassmic/earcon/",                    /* none of stock's: own sounds here win over the apps' */
    .earcon_zip = sounds,
    .thermal_type = "mtktscpu",
    /* STK3x1x on MediaTek's alsps framework; Amazon's driver has a lux attribute: an I2C read of the ALS data at each
     * read, code * 400 / the factory calibration (/proc/idme/alscal), the same value Android's light sensor reports
     * (2026-10-13).  Its other attributes are not all readings: cali_Light calibrates against 400 lux and writes
     * /data/als_cal_data.bin, ir switches ALS off until a reboot (docs/re-checkers.md; the same driver on crown, docs/re-crown.md). */
    .light_sensor = { "/sys/bus/platform/drivers/als_ps/lux" },
    .volume_steps = 100,                                            /* the light band (ring_band.c) shows the level in % */
    /* Realtek RT5616: its line out feeds the speaker amplifier.  0..39 = -46.5..+12 dB, 1.5 dB a step (the kernel's
     * tables); the HAL's initial settings in /system/etc/audio_device.xml set 30, -1.5 dB (the Echo Show 8 reads 30 30
     * from tinymix, 2026-10-13); verify that nothing writes it later */
    .speaker_gain = "OUT Playback Volume",
    .speaker_gain_stock = 30,
    .speaker_gain_steps = 2,
    .speaker_gain_db = 3,
    .speaker_gain_min_db = -6,                                      /* 30 is loud at full volume on this Show: 2026-10-15 */
};
