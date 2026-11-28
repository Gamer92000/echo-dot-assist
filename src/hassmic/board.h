/* What differs between Echo models, as far as the daemon is concerned: identity, device nodes, stock file locations.
 * One definition per model in devices/<codename>/board.c, picked at link time by DEVICE in the Makefile (the PC builds link
 * the same one, so the protocol tests see the identity of the model they stand in for).  Everything else in src/hassmic is
 * meant to be the same on every model; when a port needs more than a different value here, add a field or a backend
 * (audio.h, wake.h) rather than an #ifdef. */
#ifndef BOARD_H
#define BOARD_H

struct board {
    int kiosk;                      /* device.conf has KIOSK_PACKAGE: the Kiosk Satellite settings exist */
    int backlight;                  /* a screen whose backlight Android's settings set (screen_brightness*): the Echo Shows;
                                       root's main.sh backlight applies the screen brightness settings */
    int adaptive_colour;            /* Fire OS's "Adaptive Color" (binder service ambientdisplayinfo, the picture quality HAL):
                                       athena; root's main.sh backlight applies the setting (device.conf ADAPTIVE_COLOUR) */
    /* identity towards Home Assistant and Music Assistant */
    const char *model;              /* ESPHome device info "model", Wyoming description */
    const char *project;            /* ESPHome "project_name" */
    const char *product;            /* Sendspin "product_name" */
    const char *default_name;       /* the model as people call it ("Echo Dot 3"); the name adds the MAC's end (main.c) */
    const char *codename;           /* devices/<codename>: names this model's bundle in a release (online updates) */

    /* inputs */
    const char *keypad;             /* input device with action and volume keys (-b overrides) */
    const char *keypad2;            /* second input device with more keys; NULL if one is enough */
    const char *privacy_state;      /* sysfs file, '1' = mics muted by the hardware latch; NULL if there is no latch */
    const char *privacy_input;      /* input device that reports changes of that latch; NULL if the keypad does */
    int action_combo;               /* 1: no action button: both volume keys pressed together and let go within a second
                                       stand in for it (buttons.c); held 2 s they still pair (arbitration) */
    int grab_keys;                  /* 1: the keypad is hassmic's alone (EVIOCGRAB): Android's framework, where there is one,
                                       would act on the volume keys too and take its own volume off on top of ours */
    int privacy_latch;              /* 1: the sysfs state is the truth (read at start, on the mute key and once a second);
                                       0: nothing to read, presses of the keypad's mute key are counted from "unmuted" */
    const char *privacy_button;     /* input device of the mute button itself, where the driver reports the latch late (athena:
                                       its gpio-privacy holds the enable line 1 s, then changes state); NULL: no early notice */
    int privacy_settle_ms;          /* with privacy_button: how long a press is believed before the latch must have confirmed it */

    /* Bluetooth: raw HCI (H4) character device, the init service that owns it in stock, where the address is stored */
    const char *bt_dev;
    const char *bt_service;
    const char *bt_mac;

    /* stock assets */
    const char *wake_id;            /* id of the firmware's own wake word model */
    const char *wake_manifest;
    const char *earcon_dir;         /* with trailing slash */
    /* where the firmware has no earcon folder: stock's sound name -> an entry of a stock app (APK) holding it, stored
     * uncompressed (sounds.c reads it out of the zip).  earcon_dir wins; {NULL}-terminated, or NULL for none */
    const struct board_sound { const char *name, *zip, *entry; } *earcon_zip;
    const char *thermal_type;       /* thermal zone reported as SoC temperature */
    const char *light_sensor[3];    /* files the stock light sensor HAL reads lux from, first that opens; {NULL}: none.  "tcs3400:<dir>": raw channels of
                                       a TCS3400 under <dir>/input*, main.c lux_tcs3400.  "tsl2540:<i2c dir>": a TSL2540's
                                       iio:device* there, calibrated from idme, main.c lux_tsl2540 */
    int volume_steps;               /* volume_step-NN animations of ledcontroller */
    /* LEDs the light really shows, where that is fewer than stock's animations draw (12): 1 = one RGB LED that shows the
     * first of each frame (the Echo Input's LP5562): nothing turned towards the talker, no LED's own layer (ring.c).
     * 0: a ring of them all */
    int ring_leds;
    /* The LED driver's frame file, where hassmic may write the ring itself (ringlive.c: the light ring live; root's main.sh
     * gives the daemon that one file, device.conf LED_FRAME).  Only on models measured (scripts/led-probe.sh): its text
     * 12 x RRGGBB, ledcontroller writing it only on a change.  NULL: no live ring */
    const char *ring_frame;
    /* The speaker's volume curve: dB under volume 100 at volumes 0..100, each `at` above the last (tests/unit/volcurve_test.c
     * checks it: the same one twice divides by 0), {-1} after the last (volcurve.c: separate volumes turn each kind down
     * by it, even volume steps put the mixer on its points).  Required, never NULL: every model has its own, measured on
     * it (tools/vol-curve.py measures an Android Echo's with a Dot beside it).  Until someone has, board_vol_dot2 stands
     * in, with a comment saying so; make unit-volcurve lists the models where it still does (devices/README.md) */
    const struct board_vol { int at; float db; } *volume_curve;
    /* audio_android.c: silence played ahead of a sound on an output that was closed (2.5 s without audio), where the
     * speaker would swallow its start while it switches on.  Measured per model (tools/cold-lead.py); 0: not measured
     * yet, 350 ms, what the Echo Show 8 needed for its 120 ms wake blip (2026-10-14); -1: measured, nothing lost, and
     * the HAL's cold open short enough for earsup.c without it (open + 85 ms under D_MAX, 400 ms; logcat) */
    int cold_lead_ms;
    /* audio_mixer.c: the mixer's micHfp carries the front end's call path ("Voice" in AFE.cfg: echo canceller, residual
     * echo suppressor, noise reduction, AGC) while a Voip stream is open, measured on this model; a call then sends it
     * (call.c), always: no setting (2026-11-26, the maintainer: no reason ever to send the assistant's instead).
     * 0: none, the assistant's micAsr.  Measured: donut (a talker, 2026-11-13), biscuit (a talker and the echo,
     * 2026-11-17), cannoli (a user's phone calls, 2026-11-26); PLAN.md "Calls with Amazon's call microphone"; the
     * Android models' as stock's (audio_android.c), radar's from the firmware */
    int call_mic;
    /* call.c's echo estimate on that path held this many dB under what plays (after a call's first 2 s): micHfp's AGC
     * puts speech at the playback's level, and an uncapped estimate ducked every quick reply.  Measured: the echo left
     * well under it (donut, biscuit: PLAN.md "Calls with Amazon's call microphone"; cannoli, a user's traces,
     * 2026-11-26).  0 (unmeasured): uncapped, as on micAsr: a quick reply's start ducked, never an echo let through */
    float call_echo_cap_db;

    /* Phone calls over Bluetooth (hfp.c, scoaudio.c; docs/re-hfp.md): where a call's audio goes once the phone opened its
     * synchronous link.  No chip here moves it over HCI: each puts it where the model's stock HAL takes it, measured per
     * model (scoprobe/ebcprobe, 2026-11-25) or read from its firmware.  SCO_NONE: no calls on this model.  SCO_PCM: the
     * chip's PCM pins on the SoC, the PCM devices of card 0 named sco_play / sco_cap in /proc/asound/pcm (as stock's
     * MediaTek HAL finds them, AudioALSADeviceParser) at 8 kHz (CVSD coded by the chip), opened with
     * sco_play_ch / sco_cap_ch channels (the first carries the call), the controls of sco_route set to 1 while it runs
     * and to 0 after.  SCO_EBC: an integrated Bluetooth core's BTCVSD (/dev/ebc): the air's bitstream, coded here
     * (cvsd.c).  bt_sco_cmd: a vendor command sent after every HCI reset, where the chip's driver does not configure its
     * PCM side itself: opcode (little endian), length, parameters; NULL: none */
    int sco;
    const char *sco_play, *sco_cap;
    int sco_play_ch, sco_cap_ch;
    const char *const *sco_route;
    const unsigned char *bt_sco_cmd;
    /* SCO_PCM: the chip codes mSBC (wideband) itself from 16 kHz linear audio on its PCM pins, the link accepted with
     * Enhanced Accept Synchronous Connection (data path 1, transport unit 16, T2's parameters); measured Echo to Echo
     * with a tone above 4 kHz both ways (scoprobe msbc -U 16, docs/re-hfp.md).  0: not measured, CVSD only */
    int sco_msbc_chip;

    /* speaker gain (spkgain.c): an ALSA control of card 0 that sets the speaker amplifier's analog gain, which the
     * settings page may raise; NULL: none.  stock: what the speaker plays at on a stock Echo (what the audio HAL's
     * speaker path sets it to, or where it stays from boot).  steps: the control's steps per db dB, 0 for 1 each (the
     * Echo Show 5's 1.5 dB steps: 2 per 3 dB; a whole dB then takes the nearest step) */
    const char *speaker_gain;
    int speaker_gain_stock;
    int speaker_gain_steps;
    int speaker_gain_db;
    int speaker_gain_min_db;        /* how far below stock the page may go, dB (negative), 0: not at all */
    /* highest control value to offer, 0 for the control's own maximum (the Show 8 2nd gen's amp distorts well below its) */
    int speaker_gain_limit;
    /* other amplifiers the same model is built with (the Echo Show 5 2nd gen: three, by board revision), each with its
     * own control: tried in order where speaker_gain is not on the card.  Fields as above; {NULL}-terminated, or NULL */
    const struct board_gain { const char *name; int stock, steps, db, min_db, limit; } *speaker_gain_alt;
};

enum { SCO_NONE, SCO_PCM, SCO_EBC, SCO_HCI };     /* board.sco; SCO_HCI: over HCI as the standard has it (the PC's tests) */

extern const struct board board;
/* volcurve.c: the Echo Dot 2's curve, measured there; the stand-in for a model whose own is not measured yet */
extern const struct board_vol board_vol_dot2[];
/* volcurve.c: board.volume_curve at volume v (clamped to 0..100): dB under volume 100 */
float board_vol_db(int v);
int   board_vol_at(float db);           /* the lowest of the curve's points at least that loud (volcurve.c) */
#endif
