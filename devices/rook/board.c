/* Echo Spot 1st gen 2017 (rook), Fire OS 5.5.6.9 (304.6.9.0).  MT8163 with Android 5.1.1 (the Knight family: Echo Show 1st
 * gen), a round 480x480 screen, no LED ring.  Probed over adb on a running Echo Spot on 2026-11-20.  Values marked
 * "verify" are not checked against a running hassmic yet. */
#include "board.h"
#include <stddef.h>

/* Sounds: stock's, where the apps keep them, are looked for once hassmic runs here (verify: none listed, the built-in
 * tones play meanwhile). */
const struct board board = {
    .model = "Echo Spot (rook)",
    .project = "hassmic.echo-spot",
    .product = "Echo Spot (hassmic)",
    .round_px = 480,                                               /* the Spot's glass is a circle in a square panel */
    .cold_slack_ms = 2500,                                         /* slplay, 2026-11-20: 2.5 s to play the first 0.47 s after boot */
    .default_name = "Echo Spot",                                   /* + MAC: "Echo Spot 5695c4"; slug = the project's model */
    .codename = "rook",

    /* "keys" (gpio-keys): volume up and down.  "mtk-kpd": the mute button as KEY_POWER (getevent -l, 2026-11-20; its
     * capabilities also list KEY_HELP and KEY_VOLUMEDOWN, which no button sends).  No action button: both volume keys
     * together stand in for it.  By name: the event numbers depend on the probe order. */
    .keypad = "name:keys",
    .action_combo = 1,                                              /* the settings page's login, adb, Improv */
    .grab_keys = 1,                                                 /* else PhoneWindowManager turns its stream volume too */
    /* The mute latch of the keypad driver (the Dots' neighbour: 10010000.keypad), 1 = muted.  Pressing mute switched it
     * 0 -> 1 and back (2026-11-20).  Read only this file of the directory (biscuit: power_button_state beside it takes
     * the kernel down; not tried here). */
    .privacy_state = "/sys/devices/soc/10010000.keypad/privacy_state",
    /* any event of mtk-kpd means a latch read (buttons.c privacy_reader); grabbed, or Android takes KEY_POWER for the
     * power button and turns the screen off */
    .privacy_input = "name:mtk-kpd",
    .privacy_latch = 1,

    /* Broadcom BCM43569 on /dev/ttyMT1 (bt_vendor.conf: UartPort, firmware patch /system/vendor/firmware/BCM43569A2_*.hcd):
     * no /dev/stpbt, so ble.c and a2dp.c have nothing to open yet.  Not part of the first port. */
    .bt_dev = NULL,
    .bt_service = NULL,
    .bt_mac = "/proc/idme/bt_mac_addr",

    .wake_id = "alexa",
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",   /* verify: loads in pryon_test */
    .earcon_dir = "/data/local/hassmic/earcon/",
    .earcon_zip = NULL,
    .thermal_type = "mtktscpu",
    /* IIO light sensor on MediaTek's alsps (soc:als_ps@0): in_illuminance_input, 2.07 in a lit room (2026-11-20; lux, or
     * raw? verify against a lamp) */
    .light_sensor = { "/sys/bus/iio/devices/iio:device0/in_illuminance_input" },
    .volume_steps = 100,                                            /* the light band (ring_band.c) shows the level in % */
    /* volume curve not measured on this model yet: the Echo Dot 2's stands in (devices/README.md: how to measure) */
    .volume_curve = board_vol_dot2,
    /* TI TLV320AIC32x4's headphone driver feeds the speaker amplifier, as the Echo Dot 2's: HP Driver Gain Volume is 9
     * here (audio_device.xml's ext_speaker_output sets 9, ext_speaker_output off 11; tinymix 62 reads 9 9) */
    .speaker_gain = "HP Driver Gain Volume",
    .speaker_gain_stock = 9,
};
