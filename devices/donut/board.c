/* Echo Dot 3rd gen 2018 (donut), Fire OS 6574.1.  Device facts: docs/re-platform.md. */
#include "board.h"
#include <stddef.h>

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
    /* ISSI IS31FL3236 at I2C 0-003f: 12 x RRGGBB text, scaled by the unit's calibration in the driver (PLAN.md "Light
     * ring live", 2026-11-12) */
    .ring_frame = "/sys/bus/i2c/devices/0-003f/frame",
    /* volume curve not measured on this model yet: the Echo Dot 2's stands in (devices/README.md: how to measure) */
    .volume_curve = board_vol_dot2,
    /* TI TAS2770 amplifier at 2-0044: AMP_LEVEL (reg 0x03), 0..20 = 11..21 dBV full-scale output in 0.5 dB steps.  16 from
     * boot on, never written by the firmware (2026-10-11, spkgain.c): +2 dB is all it has */
    .speaker_gain = "DAC Playback Volume",
    /* micHfp in call mode: speech -20 dBFS against -43 in its pauses, where micAsr had -48/-65 (2026-11-13) */
    .call_mic = 1,
    .call_echo_cap_db = -30,                        /* (board.h: the echo measured well under it) */
    .speaker_gain_stock = 16,
    .speaker_gain_steps = 2,
    /* phone calls (docs/re-hfp.md): the MT7668 on the SoC's 2nd I2S, stock's mixer_paths.xml "bt-sco" paths: DL2
     * (MultiMedia2_PLayback) to it, its capture through HW GAIN1 into the DAI memif (VOIP_Call_BT_Capture).  This
     * btmtksdio (v0.0.1.00) leaves the chip's PCM side unset: the newer one's 0xfc72 for the MT7668 (cannoli's driver,
     * btmtk_sdio_set_audio_slave), without which the phone's audio never reaches the SoC (2026-11-25) */
    .sco = SCO_PCM, .sco_play = "MultiMedia2_PLayback", .sco_cap = "VOIP_Call_BT_Capture", .sco_play_ch = 1, .sco_cap_ch = 1,
    .sco_route = (const char *const[]){ "O00 I07 Switch", "O01 I08 Switch", "O13 I00 Switch", "O14 I01 Switch", "O11 I10 Switch", NULL },
    .bt_sco_cmd = (const unsigned char[]){ 0x72, 0xfc, 4, 0x03, 0x10, 0x00, 0x4a },
    .sco_msbc_chip = 1,             /* the MT7668 codes mSBC from DL2 / the 2nd I2S at 16 kHz, set up and accepted (2026-11-25) */
};
