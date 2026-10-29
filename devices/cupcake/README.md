# Echo Input, 2018 (`cupcake`)

**Installed and running on one Echo Input** (2026-10-29): worked out from the firmware (2026-10-28), then tried on the
device, everything as expected. It builds, CI publishes its bundle, and the guided setup (`scripts/setup.sh cupcake`)
walks the steps below. Findings: [docs/re-cupcake.md](../../docs/re-cupcake.md). Porting guide: [devices/README.md](../README.md).

Model C1125P (`ro.product.model` AEOCK), `ro.product.device` = `cupcake`. MediaTek MT8516, four microphones, one RGB
LED, action and mute buttons, **no speaker**: sound comes from the 3.5 mm jack or a Bluetooth speaker.

- **Platform**: the Echo Dot 3's hardware ([`donut`](../donut/README.md): A/B, the same kernel, kamakiri and TWRP) with
  the Echo Shows' software (Android's framework runs: Alexa is apps, Wi-Fi is Android's, audio goes through AudioFlinger
  with Amazon's front end inside the audio HAL; `src/hassmic/audio_android.c`).
- **Install**: `INSTALL=twrp-ab`, as donut: the policy patched under the running OS, written with the files into the
  active system slot. Amazon's apps go off the partition (`STOCK_APPS_OUT`, kept in `/system/hassmic-removed`, back with
  `--uninstall`).
- **Buttons**: action (`gpio-keys`, KEY_HELP), mute (`gpio-privacy`, a latch software can set but not clear). No volume
  keys: the volume is Home Assistant's, the settings page's, or the phone's over Bluetooth.
- **Light**: one LED (LP5562) behind Amazon's `ledcontroller`. Stock's patterns and Home Assistant's "Light ring"
  show on it; nothing turns towards the talker, and there is no fill LED by LED (`board.c` `ring_leds`). Designs from
  the settings page are drawn for 12 LEDs: only the first shows.
- **Wake word**: `libpryon.so` with the stock `en-US/ALEXA` model, plus models from Amazon ("Download from Amazon").

## Install

The guided way: `scripts/setup.sh cupcake` runs these same steps and checks each one.

Read the [XDA thread][xda] in full first: the unlock is not this project's work.

> **Never let the Echo go online with Amazon on the way.** It would update itself, and an update can close the hole the
> unlock uses. Do not set it up with the Alexa app.

### Requirements

Besides the general [requirements](../../README.md#requirements): an Echo Input from 2018 (C1125P), a micro-USB data
cable, Linux running on the PC itself (not a VM: the bootrom's USB timing is tight), and these files in
`firmware/cupcake/` (git-ignored):

| File | Source | sha256 |
|---|---|---|
| `update-kindle-cupcake-NS6575_user_8338_0013323244164.bin` | stock Fire OS 6.5.7.5 for `cupcake`, [FTVDB](https://ftvdb.com/echo/firmware/com.amazon.cupcake.android.os/675ff746519090e1cbd2366a92132d55-13323244164-fire-os-6-5-7-5-ns6575-8338-2026-10-14/) | `6f076e3193bd0431818147fe0f41172d55ed2fa8361f88b90368bdbed6da310b` |
| `kamakiri-cupcake-v1.0.0.zip` | bootloader unlock + TWRP, [XDA thread][xda] (its `lk` and `tee` are this firmware's, byte for byte) | `bb76e4a86c90015ac80b5bd2aacb5e75b4caa3919f900f202ef480708cf6616f` |
| `boot-root.zip` | **the Echo Dot 3's** root zip, [donut's XDA thread][xda-donut] (its system-as-root path fits this firmware) | `de49cc88b27a8e77cf97cf0156bee50e4ddc0e116c41aaede06b494e38397be0` |
| `toolchain/android-ndk-r21e/` | unpack <https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip> into `toolchain/` | – |

Use exactly this firmware version: everything was worked out against its binaries, and `scripts/probe.sh` checks the
Echo for it. Flashing it in step 1 replaces whatever the Echo ran before.

### 1. Unlock, flash stock firmware, root

The Echo Input runs from its micro-USB socket, and the bootrom answers on it: no opening, no soldering. Until the install
the PC's port powers it (USB 2 gives 0.5 A, less than its own adapter; what it draws is not measured): if it restarts
or drops off adb during the later steps, put a powered USB hub between them.

```sh
sudo cp scripts/51-echo-unlock.rules /etc/udev/rules.d/ && sudo udevadm control --reload   # USB without sudo; keeps ModemManager off the bootrom
unzip firmware/cupcake/kamakiri-cupcake-v1.0.0.zip -d firmware/cupcake/kamakiri
K=$(dirname "$(find firmware/cupcake/kamakiri -name bootrom-step.sh)")
(cd "$K" && ./bootrom-step.sh)    # start it, then: hold the action button (•), plug the micro-USB cable from the PC into
                                  # the Echo (its power adapter out), keep holding until it asks for Enter
(cd "$K" && ./fastboot-step.sh)   # when the light shows a rotating rainbow (kaeru); ends in TWRP
```

Flash the firmware into both A/B slots, then root, as on donut (TWRP makes the slot it flashed the active one; do not
switch it back by hand):

```sh
F=firmware/cupcake/update-kindle-cupcake-NS6575_user_8338_0013323244164.bin
adb shell twrp wipe cache; adb shell twrp wipe data
adb push $F /sdcard/update.zip
adb shell twrp install /sdcard/update.zip      # one slot; it becomes the active one
adb reboot recovery
adb shell twrp install /sdcard/update.zip      # must name the OTHER slot this time
adb reboot recovery
adb push firmware/cupcake/boot-root.zip /sdcard/ && adb shell twrp install /sdcard/boot-root.zip   # patches both slots
adb reboot
```

TWRP refuses to write lk, preloader and tee: leave them alone in any case. Result: stock Fire OS, unregistered, no
Wi-Fi, and `adb shell` is root.

### 2. Unpack the firmware and build

With `PREBUILT=1` (a commit CI built) only `boot-root` needs unpacking.

```sh
unzip firmware/cupcake/update-kindle-*.bin payload.bin -d firmware/cupcake/
python3 tools/payload_dump.py firmware/cupcake/payload.bin firmware/cupcake/images
mkdir -p firmware/cupcake/rootfs
debugfs -R "rdump / firmware/cupcake/rootfs" firmware/cupcake/images/system.img
unzip firmware/cupcake/boot-root.zip -d firmware/cupcake/boot-root    # the installer uses its patch/magiskpolicy32
make kernel-tools DEVICE=cupcake          # optional: Wi-Fi motion's module (donut's kernel and compiler)
make DEVICE=cupcake
scripts/probe.sh                          # must not list any DIFFERENT library
```

### 3. Lock down, then join Wi-Fi

```sh
scripts/deploy.sh                                        # binaries and scripts to /data/local/hassmic
adb shell sh /data/local/hassmic/lockdown.sh             # firewall: local addresses only
adb shell sh /data/local/hassmic/alexa-off.sh            # Alexa's apps and Amazon's setup off (pm disable)
mkdir -p secrets && printf '%s\n%s\n' 'My SSID' 'my passphrase' > secrets/wifi.conf
scripts/wifi-join.sh                                     # through Android's WifiService (wifictl.dex)
```

Do not reboot before step 4: the Wi-Fi profile survives a reboot, the lock does not.

### 4. Install it, adopt it in Home Assistant

```sh
scripts/install-system.sh                        # the Echo names itself ("Echo Input 5695c4"); name it in Home Assistant
```

Then as on [donut](../donut/README.md#5-install-it-adopt-it-in-home-assistant): Home Assistant discovers it (ESPHome,
port 26053), pick the Assist pipeline, say "Alexa". Back up `secrets/update.key`. Music Assistant and Bluetooth: as on
donut (steps 6 and 7 there); with no speaker, a Bluetooth speaker or the jack is where it plays.

[xda]: https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-input-2018-cupcake.4797603/
[xda-donut]: https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/
