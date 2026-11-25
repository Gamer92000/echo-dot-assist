/* Echo Input 2018 (cupcake), Fire OS 6.5.7.5 (NS6575/8338).  Device facts from the firmware (docs/re-cupcake.md), as
 * found on a running Echo Input (2026-10-29). */
#include "board.h"
#include <stddef.h>

const struct board board = {
    .model = "Echo Input (cupcake)",
    .project = "hassmic.echo-input",
    .product = "Echo Input (hassmic)",
    .default_name = "Echo Input",                                  /* + MAC: "Echo Input 5695c4"; slug = the project's model */
    .codename = "cupcake",

    /* gpio-keys: the action button only (KEY_HELP, DTB); no volume keys on this model.  By name, as the keylayouts of
     * /system/usr/keylayout are (gpio-keys.kl, gpio-privacy.kl): the event numbers depend on the probe order */
    .keypad = "name:gpio-keys",
    /* Android's framework runs: gpio-keys.kl and gpio-privacy.kl hand both keys on as MUTE and BUTTON_MODE, so they are
     * hassmic's alone (EVIOCGRAB), as on the Echo Shows */
    .grab_keys = 1,
    /* the gpio-privacy driver, donut's (the same 4.4.22 kernel): state chowned to system by /init.project.rc, its own
     * input device reports the mute button (KEY_MUTE) */
    .privacy_state = "/sys/devices/platform/gpio-privacy/state",
    .privacy_input = "name:gpio-privacy",
    .privacy_latch = 1,

    .bt_dev = "/dev/stpbt",                                         /* MediaTek combo chip, btmtksdio.ko, as donut's */
    .bt_service = NULL,                                             /* none: Android's stack (app/Bluetooth) is off the partition */
    .bt_mac = "/proc/idme/bt_mac_addr",                             /* 12 hex digits, as btmac.sh reads it */

    .wake_id = "alexa",
    /* the newer model layout (encoder/decoder, NTT); loads in donut's pryon_test under qemu against this firmware's
     * libpryon and finds ALEXA in testdata/alexa_espeak.raw as checkers' does (3.78-5.88 s) */
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",
    .earcon_dir = "/system/local/share/earcon/base/",               /* the Dots' folder, wake sound and timer tones included */
    .thermal_type = "mtktscpu",
    /* TSL2572 (IIO) at 0-0039 as on donut: the files libled_hal.so's ambient light engine reads, in its order */
    .light_sensor = { "/sys/bus/i2c/devices/0-0039/iio:device0/calibrated_lux",
                      "/sys/bus/i2c/devices/0-0039/iio:device0/in_illuminance0_input" },
    .volume_steps = 30,                                             /* volume_step-01..30, as the Dots' */
    /* One RGB LED (TI LP5562 at 0-0030, libled_hal.so's Lp5562LedDevice).  Stock's animations still draw 12 LEDs; its
     * listening ring lights only the first (active-start), so that one is what shows */
    .ring_leds = 1,
    /* No speaker: a 3.5 mm line out (the MT8167's own codec) and Bluetooth.  No amplifier to raise; the volume is
     * Android's music stream (audio_android.c), whose curve is not measured on this model yet: the Echo Dot 2's
     * stands in (devices/README.md: how to measure) */
    .volume_curve = board_vol_dot2,
    /* phone calls (docs/re-hfp.md): donut's path, from the firmware, not run here.  The same MT8516 and MT7668, donut's
     * "bt-sco" paths in its mixer_paths.xml, its own kernel's link "2ND EXT Codec" (2ND I2S, linux_bt_sco_codec), and
     * its bootloader's GPIO table (LK's mt_gpio_set_default) muxing the 2nd I2S's pins as donut's does: GPIO55-57 mode
     * 4, GPIO25 mode 6 (its device tree does not; donut's does both).  The old btmtksdio: the chip's PCM setup ours.
     * The call plays where the Echo Input plays: its jack or a Bluetooth speaker */
    .sco = SCO_PCM, .sco_play = "MultiMedia2_PLayback", .sco_cap = "VOIP_Call_BT_Capture", .sco_play_ch = 1, .sco_cap_ch = 1,
    .sco_route = (const char *const[]){ "O00 I07 Switch", "O01 I08 Switch", "O13 I00 Switch", "O14 I01 Switch", "O11 I10 Switch", NULL },
    .bt_sco_cmd = (const unsigned char[]){ 0x72, 0xfc, 4, 0x03, 0x10, 0x00, 0x4a },
    .sco_msbc_chip = 1,             /* not run here: donut's MT7668 and paths, measured there (2026-11-25) */
};
