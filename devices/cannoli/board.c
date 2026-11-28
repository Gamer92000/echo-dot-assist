/* Cannoli NS6575 / 8349. Firmware + DTB evidence; no hardware measurements. */
#include "board.h"
#include <stddef.h>
const struct board board = {
    .model = "Echo Dot 5 (cannoli)",
    .project = "hassmic.echo-dot-5",
    .product = "Echo Dot 5 (hassmic)",
    .default_name = "Echo Dot 5",
    .codename = "cannoli",
    /* DTB gpio-keys: KEY_HELP=138, VOLUMEUP=115, VOLUMEDOWN=114.
     * Resolve names, not event numbers: runtime probe order is unknown. */
    .keypad = "name:gpio-keys",
    /* the gpio-privacy driver as athena's: its input devices are the latch ("gpio-privacy-state", SW_MUTE_DEVICE) and the
     * button ("gpio-privacy-button", KEY_MUTE); none is called "gpio-privacy" (2026-11-15: "not available, mute button
     * changes go unnoticed", the state file's once-a-second look the only one).  The device tree's
     * enable-gpio-toggle-duration is 50 ms here, not athena's 1000: no early notice from the button needed */
    .privacy_input = "name:gpio-privacy-state",
    .privacy_state = "/sys/devices/platform/gpio-privacy/state",
    .privacy_latch = 1,
    .bt_dev = "/dev/stpbt",
    .bt_service = "btmanagerd",
    .bt_mac = "/proc/idme/bt_mac_addr",
    .wake_id = "alexa",
    .wake_manifest = "/system/local/models/keyword/en-US/ALEXA/pryon.manifest",
    .earcon_dir = "/system/local/share/earcon/base/",
    /* ams TSL2540 on i2c 4-0039 (IIO: visible and IR counts), lux as stock's libacehal_ambientLightSensor.so makes it,
     * with the unit's factory points from idme (lux.h; checked against stock's alsctrl on the Echo) */
    .light_sensor = { "tsl2540:/sys/bus/i2c/devices/4-0039" },
    .thermal_type = "soc_max",                      /* no mtktscpu on the 5.4 kernel: soc_max, cpu_little0, ap_ntc0..4, ... */
    .volume_steps = 30,
    /* Placeholder curve, as on other unmeasured upstream models. */
    .volume_curve = board_vol_dot2,
    /* The speaker amplifier is a Maxim MAX98396 (i2c 3-0038), as on some Echo Show 5 2nd gen boards (cronos): its speaker
     * gain "Speaker Volume A", 0..17 = +4..+21 dB, 1 dB a step (the kernel's TLV read on the Echo, 2026-11-15: DB_RANGE
     * 0..17, DB_SCALE 400/100); vendor mixer_paths.xml's normal-playback speaker-maxim path sets 10, +14 dB.  "Digital
     * Volume A" stands at 126 = 0 dB of -63..+0.5: no headroom there.  /dev/snd/controlC0 is system:audio 0660, hassmic
     * has audio.  +6 dB at most until someone has listened at full volume, as on cronos */
    .speaker_gain = "Speaker Volume A",
    .speaker_gain_stock = 10,
    .speaker_gain_limit = 16,
    /* micHfp in call mode (2026-11-26, a user's Dot 5 with a phone over HFP, issue #233): speech -10..-30 dBFS (micAsr
     * -50..-58), the room between words -88; a count from 30 cm, 1.5 m and 3 m at -27, -28, -29 dBFS median, every
     * frame past the gate after the first second; WhatsApp's far end "a huge difference".  Stock's own calls record it
     * too (libpjmedia-audiodev.so), and AFE.cfg's "Voice" path is biscuit's (AEC, Frequency Masking RES, FB AGC -23) */
    .call_mic = 1,
    .call_echo_cap_db = -30,                        /* (board.h: the echo measured well under it) */
    /* phone calls (docs/re-hfp.md): the MT7663's PCM on the SoC's PCM1, from the kernel's DAPM routes (mt8519-afe-pcm,
     * mendocino "HFP Out"/"HFP In"): DL6 -> O00/O01 -> PCM1, PCM1 -> O02/O03 -> UL5 (two channels whatever is asked,
     * and only while DL6 plays).  The chip's PCM side the driver sets itself after every reset (0xfc72 49 00 80 00) */
    .sco = SCO_PCM, .sco_play = "DL6 Playback", .sco_cap = "UL5 Capture", .sco_play_ch = 1, .sco_cap_ch = 2,
    .sco_msbc_chip = 1,             /* the MT7663 codes mSBC from DL6/UL5 at 16 kHz, set up and accepted (2026-11-25) */
    .sco_route = (const char *const[]){ "O00 I00 Switch", "O01 I01 Switch", "O02 I02 Switch", "O03 I03 Switch", NULL },
};
