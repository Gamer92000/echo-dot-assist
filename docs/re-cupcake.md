# Echo Input 2018 (`cupcake`): what the firmware says

Worked out from the firmware, 2026-10-28; installed and running on one Echo Input since 2026-10-29, everything as
expected. The unlock is
kamakiri-cupcake v1.0.0 ([XDA](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-input-2018-cupcake.4797603/): bootrom exploit over the micro-USB socket, TWRP in `swdl`; its
`bin/lk-cupcake.bin` and `bin/tee-cupcake.bin` are byte-identical to this firmware's `lk` and `tee1`/`tee2`, its
preloader is another build and not flashed by its scripts; `devices/cupcake/README.md`).
Firmware: `update-kindle-cupcake-NS6575_user_8338_0013323244164.bin` (sha256 `6f076e31…6da310b`), Fire OS 6.5.7.5
(NS6575/8338, built 2026-10-14). Model C1125P, `ro.product.model=AEOCK`, `ro.product.device=cupcake`.

## Unpacking

An A/B update with `payload.bin`, as donut's, and unpacked the same way:

```sh
unzip -o firmware/cupcake/update-kindle-*.bin payload.bin -d firmware/cupcake/
python3 tools/payload_dump.py firmware/cupcake/payload.bin firmware/cupcake/images   # boot lk tee1 tee2 preloader system
mkdir -p firmware/cupcake/rootfs
debugfs -R "rdump / firmware/cupcake/rootfs" firmware/cupcake/images/system.img      # system_root_image: / with /system
```

`boot.img`: MTK header on a gzip'd arm64 kernel, 4.4.22+ (Amazon's GCC 4.9 build, as donut's), four device trees
appended (Proto, HVT, EVT, DVT/PVT). They differ in the mute latch's GPIO only (`state-gpios` 0x49 on Proto/HVT, 0x15
on EVT and later).

## Platform: donut's hardware, checkers' software

- MT8516 (`fstab.mt8167`, `init.mt8167.rc`), armv7 user space (`abilist64` empty), Android 7.1.2, API 25,
  `ro.build.characteristics=headless`. A/B, `system_root_image=true`: the policy is `/sepolicy` on the system partition,
  `default.prop` and `fstab.mt8167` (`verify`) too, as on donut.
- But the whole Android framework runs, as on the Echo Shows: zygote, system_server, SurfaceFlinger (no screen), Alexa
  as apps (`amazon.speech.sim` and friends), Wi-Fi through Android's WifiService (no `wifisvc`; `wpa_supplicant
  -O/data/misc/wifi/sockets -g@android:wpa_wlan0`), Bluetooth through Android's app. No `mixer`, `libmixerAPI.so`,
  `PuffinApp`, `btmanagerd`, `acebuttond`, `uxeventd`, `libacehal_*`.
- Kept from the Dots: Amazon's `ledcontroller` (here a binder service, `ILedController`, and `ledctrl`), the Dots'
  `led-resources`, `/system/local/share/earcon/base/`, stock's `firewall.sh` (started at `sys.boot_completed` by
  `init.oobe.rc`), `init.mt8167.usb.rc` byte-identical to donut's (adb over Wi-Fi at `on boot`: `lockdown.sh adb_gate`).
- So in `device.conf`: `INSTALL=twrp-ab` (donut's installer) and `ANDROID=1` (the scripts' Android world: wifictl.dex,
  `pm`, the apps off the partition). Until this model those two went together; `ANDROID` is a variable of its own now.

## Audio: the front end in the audio HAL

`libasp.so` (its own build) is linked by `vendor/lib/hw/audio.primary.mt8167.so`, `libaudioflinger.so` and
`audio.a2dp.default.so`, as on checkers; this HAL uses libasp's C API (`asp_create_pipeline`, `asp_process`,
`add_echo_reference`). `AudioALSACaptureDataClient`'s constructor (0x90a20) picks the pipeline from the record's source
exactly as checkers' `getAspPipelineType`: 6 VOICE_RECOGNITION and 1999 HOTWORD → 0 (ASR), 7 → 1 (Voice), 1 → 2 (MIC),
2999 → 6 (SwwRef), anything else -1. So `audio_android.c` (OpenSL ES, VOICE_RECOGNITION) carries over unchanged.
`vendor/etc/audio-algorithms/AFE.cfg`: 4 mics at 16 kHz (two TLV320AIC3101 ADCs at i2c 0-0018 and 0-001a, as donut),
2 speaker channels at 48 kHz as reference, `Num Internal Speakers` 0.

- **No speaker.** The output is the MT8167's own codec on the 3.5 mm jack (and Bluetooth); no amplifier in the DTB, so no
  `speaker_gain`. The volume is Android's music stream as on the Shows; its curve on the line out was not measured
  (`volume_curve` NULL: the Dot 2's).
- `audiosignalprocessor` (`com.amazon.asp.IAudioSignalProcessor`, `audioserver_service`) and `amazon.security.dha`
  (`fireos.dha.IFireOsDha`, `fireos-dha` as keystore with drmrpc; HAL `amzn_dha.mt8167.so`, no `libacehal_dha.so`) are
  registered under checkers' names: `afe_parcel.c` and `dha.c`'s service path apply. Which DHA version it answers
  (1: drvV1, 2: drvV3) does not matter: `dha.c` takes either, and it works on the device.
- The framework's binder codes, read out of `boot-framework.oat` (`tools/dexasm.py` `boot_classes`): IAudioService
  setStreamVolume 3, getStreamMaxVolume 10; IPowerManager wakeUp 9, userActivity 8, isInteractive 12: checkers'.
  `/system/bin/service` is byte-identical to checkers'.
- `scripts/device/wifictl.dex`: `tools/mkwifictl.py --check --framework=firmware/cupcake/rootfs` resolves every
  reference against 19521 boot classes.

## Pryon and models

- `lib/libpryon.so` is a build of its own (md5 `1d3315ea…`, Pryon `-2.41-g-M`), with every symbol of
  `devices/donut/stubs/libpryon.syms`. donut's `pryon_test` under qemu against this rootfs, on a copy of
  `en-US/ALEXA` (the newer layout, with `alexa-wakeworddecoder/` and `siddecoder/`), finds ALEXA in
  `testdata/alexa_espeak.raw` at 3.78–5.88 s, score 0.289, NearMiss: the same as checkers' library and model on the
  same file. `libopus.so`, `libz.so` are donut's.
- 20 locales under `/system/local/models/keyword/`, `AED`, `NTT`, `PMA` beside it.

## Board facts (DTB, init, keylayouts)

- Buttons: `gpio-keys` with the action button only (KEY_HELP 0x8a); no volume keys. The mute button is the
  `gpio-privacy` driver's (KEY_MUTE 0x71), the state in `/sys/devices/platform/gpio-privacy/state` (chowned to system by
  `/init.project.rc`). Input device names from the keylayouts `gpio-keys.kl` and `gpio-privacy.kl`, which hand them to
  Android as BUTTON_MODE and MUTE: grabbed (`board.grab_keys`).
- Light: one RGB LED, a TI LP5562 at i2c 0-0030 (`libled_hal.so` `Lp5562LedDevice`, `/sys/bus/i2c/devices/0-0030/leds/
  {R,G,B}`). The animations still draw 12 LEDs, and the layer config has 5 layers (donut's 6; listening's are in layer
  3, volume and factory reset in 4). Stock's `active-start` lights only the first LED, so that is the one that shows:
  `board.ring_leds` 1 makes `ring.c` turn nothing and make no LED's own layers (622 files instead of 2422), and ours go
  into the layer that lists `active-start` (a lookup now, 4 on the Dots: their set is byte-identical to before).
  `ledcontroller` links no liblipc here, so there is no `com.amazon.ledd BeamDirection` anyway.
  Home Assistant's light offers no "Spin" here (`ring_effect_listed`): on one LED it is only a pulse twice a second.
- Light sensor: TSL2572 at 0-0039 as donut's; `libled_hal.so`'s ambient light engine reads `iio:device0/calibrated_lux`
  or `in_illuminance0_input` there.
- Bluetooth: `/dev/stpbt` (bluetooth:net_bt_stack 0660), `btmtksdio.ko`, address `/proc/idme/bt_mac_addr` (`btmac.sh`).
  Android's `com.android.bluetooth` owns it in stock; `bluetoothd` (BlueZ) is there but disabled.
- Thermal: `mtktscpu`. Earcons: the Dots' folder with the wake sound, `state_setup_discovery_beacon` and the timer tones.
- Wi-Fi: `wlan_mt76x8_sdio.ko`, a build of its own of donut's gen4m driver with donut's layout: `nicRxFillRFB` called at
  `nicRxProcessDataPacket+0x50`, the same stores into `SW_RFB_T`. Every stock module has vermagic `4.4.22+ SMP preempt
  mod_unload aarch64` and a 0x300-byte `struct module` with init at 0x158 and exit at 0x2d8, as donut's: `hassmic_rcpi4m`
  builds with donut's config fragment.
- System partition: 1 GB image, 111 MB free.

## Root: donut's boot-root.zip

Its `update-binary` takes the system-as-root path when the zip has `patch/init.fosflags.sh` (donut's does), for both
slots: `buildvariant=userdebug` on the boot image's command line (needs `veritykeyid`, which cupcake's has), dm-verity
off at the end of the system image, `ro.secure=0`, `ro.adb.secure=0`, `ro.debuggable=1` in `default.prop`, an
`init.fosflags.sh` that turns on adb without authentication (cupcake's stock one is byte-identical to donut's), the
autoroot site of `sbin/adbd` (`10b50220` followed by `044624b1` 8 bytes on: at 0x19bf4 in cupcake's adbd as in
donut's, though the files differ), the policy through its magiskpolicy with the system's libraries (`permissive su`,
adbd into su), `verify` out of the fstab, the build number raised. Nothing in it is donut-only, and it works
here (2026-10-29).

## What a port needs

[x] done, [~] partly, [ ] open.

- [x] `devices/cupcake/`: board.c, device.conf, device.mk, hassmic.rc, sepolicy.rules (donut's), probe.md5, stubs
  (byte-identical builds), setup.sh (`--dry-run` walks it), README.md. CI builds it.
- [x] Unlock with kamakiri-cupcake, both slots flashed, boot-root: on a device.
- [x] Install (`install-system.sh` from the running OS), the apps off the partition (`STOCK_APPS_OUT`, 64 folders; the
  shared libraries of `/system/etc/permissions` and SimpleLauncher, a code-less home, stay), boot.
- [x] Audio: recording as system, the ASR pipeline's level, our playback as the echo canceller's reference.
- [x] Buttons and the mute latch by name; KEY_HELP held 10 s (factory reset).
- [x] The LED: the LP5562 shows the first of the 12, HA's light, designs.
- [x] DHA, light sensor file, Wi-Fi motion module, Bluetooth on `/dev/stpbt` with Android's stack gone, the
  wake lock (`/sys/power/autosleep`).
- No volume keys: arbitration's pairing by holding Volume up + Volume down cannot be done on this model; Home
  Assistant's way (the tags, or the key action) can.
