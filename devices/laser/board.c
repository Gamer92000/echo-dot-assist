/* Echo 4th gen 2020 (laser), Fire OS NS6575 / 8349.  Firmware and device-tree evidence, read on a rooted Echo
 * (2026-11-26); the volume curve measured on the unit (2026-11-28, board_vol_laser), the rest of the audio not yet. */
#include "board.h"
#include <stddef.h>

const struct board board = {
    .model = "Echo 4 (laser)",
    .project = "hassmic.echo-4",
    .product = "Echo 4 (hassmic)",
    .default_name = "Echo 4",
    .codename = "laser",

    /* gpio-keys: KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_HELP (the action button), as cannoli's.
     * Resolve names, not event numbers: runtime probe order is unknown. */
    .keypad = "name:gpio-keys",
    /* the gpio-privacy driver as cannoli's: its input devices are the latch ("gpio-privacy-state", SW_MUTE_DEVICE) and
     * the button ("gpio-privacy-button", KEY_MUTE); none is called "gpio-privacy".  The device tree's
     * enable-gpio-toggle-duration is 2 ms here, so the latch confirms a press at once: no early notice from the button
     * is needed (athena's and cronos's drivers hold the line a second) */
    .privacy_input = "name:gpio-privacy-state",
    .privacy_state = "/sys/devices/platform/gpio-privacy/state",
    .privacy_latch = 1,
    .bt_dev = "/dev/stpbt",                             /* 240,0, bluetooth:net_bt_stack 0660 */
    .bt_service = "btmanagerd",
    .bt_mac = "/proc/idme/bt_mac_addr",                 /* 12 hex digits, as btmac.sh reads it */
    .wake_id = "alexa",
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",
    .earcon_dir = "/system/local/share/earcon/base/",   /* 70 files */
    /* 4.9's own SoC zone; the others are therm0..3_s, the tmp108/tmp112 sensors and stock's virtual ones */
    .thermal_type = "mtktscpu",
    /* TSL2572 on i2c 2-0039 (IIO iio:device1, beside the SoC's auxadc at iio:device0), its calibrated lux from the
     * unit's idme points (alscal, alsparams): the same sensor and node as donut's */
    .light_sensor = { "/sys/bus/i2c/devices/2-0039/iio:device1/calibrated_lux" },
    .volume_steps = 30,                                 /* volume_step-NN.animation of /system/etc/led-resources */
    /* 24 LEDs (two LP5036, lp5036:ledmod_0..23; stock's animations 24 pixels a frame).  com.amazon.ledd BeamDirection is
     * in LEDs of the 24 here as on the Dots' 12: ledcontroller turns by it unchanged (LedDevice, 0x1eb40: LED i from
     * pixel (i + n - turn) mod n), and the Echo reported 18 at a start (2026-11-27).  AFE.cfg's "BeamInfos" (12 beams,
     * indexes 0..11) is the simulator's, as its comment says */
    /* The ring live (ringlive.c): no frame file; libacehal_led.so writes "%d %d %d" into LED i's color file, through
     * descriptors it keeps, then "1"/"0" into both chips' ledhven as the ring goes lit or dark (read out of it,
     * 2026-11-27; init.ledcontroller.rc chowns exactly these to ledcontroller).  The level is global_current's, which
     * stays ledcontroller's.  device.conf LED_FRAME lists the same files.  Tested on the Echo (2026-11-28) */
    .ring_led_color = "/sys/class/leds/lp5036:ledmod_%d/color",
    .ring_led_power = (const char *const[]){ "/sys/class/i2c-dev/i2c-0/device/0-0030/ledhven",
                                             "/sys/class/i2c-dev/i2c-0/device/0-0031/ledhven", NULL },
    /* measured on the unit 2026-11-28 (volcurve.c board_vol_laser): a multitone on hassmic's own output, recorded on
     * an Echo Dot 2 ~10 cm away; the output plateaus from ~80 up */
    .volume_curve = board_vol_laser,
    /* The amplifiers are a TI TAS5805M's two channels ("Master TAS5805m Playback Volume A" and "B", 110 each from
     * boot on, vendor mixer_paths.xml): a digital volume of the amp, not an analog gain, and one control per channel
     * where the page offers one.  Left alone until someone has listened at a raised one */
    /* micHfp, from the firmware only (2026-11-27): stock's own calls record it (libpjmedia-audiodev.so, byte for byte
     * cannoli's), and AFE.cfg has the "Voice" path (VOIP AEC, Frequency Masking Lite RES, NR, CNG, FB AGC -23 dBFS,
     * +20 constant, +12 at most, VOIP Tx limiter).  The Dot 5's, from the same build, worked once measured; not
     * measured here, so call.c's echo cap (call_echo_cap_db) stays off */
    .call_mic = 1,
    /* phone calls (docs/re-hfp.md): cannoli's path, from the firmware, not run here.  The same MT7663
     * (/system/vendor/firmware/mt7663_patch_e2_hdr.bin) behind the same btmtksdio v0.0.1.13_2023062701, which sets the
     * chip's PCM side itself; vendor mixer_paths.xml's "normal-playback bt-sco" sets O00 I00 / O01 I01 and
     * "normal-capture bt-sco" O02 I02 / O03 I03 (cannoli's leaves both empty: its routes came from its kernel's DAPM
     * table, mt8519-afe-pcm.c, DL6 -> O00/O01 -> PCM1 -> O02/O03 -> UL5).  Card 0 has DL6 and UL5.  That MT8512's
     * AFE feeds I00/I01 from DL6 is cannoli's MT8519 taken for it: laser's kernel is packed in a form not read yet.
     * Channels as cannoli's (UL5 two whatever is asked) until measured */
    .sco = SCO_PCM, .sco_play = "DL6 Playback", .sco_cap = "UL5 Capture", .sco_play_ch = 1, .sco_cap_ch = 2,
    .sco_msbc_chip = 1,             /* not run here: cannoli's MT7663 and links, measured there (2026-11-25) */
    .sco_route = (const char *const[]){ "O00 I00 Switch", "O01 I01 Switch", "O02 I02 Switch", "O03 I03 Switch", NULL },
    /* Thread and Zigbee (docs/re-thread.md): a Qorvo GP712 on /dev/gp (gp712_*.ko); stock runs Thread through its
     * otbr-agent with this RCP on a pty, Zigbee through ace_zigbee_service on Qorvo's host stack.  The RCP answers on
     * a plain socket too: Spinel 4.3, RCP API 11 (min host 4), radio caps ack timeout, energy scan, retries, CSMA,
     * sleep to transmit (measured 2026-11-27) */
    .radio_rcp = "/system/bin/ot-rcp",
};
