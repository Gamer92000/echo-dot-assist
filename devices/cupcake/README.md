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
cable, and these files in `firmware/cupcake/` (git-ignored):

| File | Source | sha256 |
|---|---|---|
| `update-kindle-cupcake-NS6575_user_8338_0013323244164.bin` | stock Fire OS 6.5.7.5 for `cupcake`, [FTVDB](https://ftvdb.com/echo/firmware/com.amazon.cupcake.android.os/675ff746519090e1cbd2366a92132d55-13323244164-fire-os-6-5-7-5-ns6575-8338-2026-10-14/) | `6f076e3193bd0431818147fe0f41172d55ed2fa8361f88b90368bdbed6da310b` |
| `magiskpolicy32` | `lib/armeabi-v7a/libmagiskpolicy.so` of `Magisk-v30.7.apk`, [Magisk v30.7](https://github.com/topjohnwu/Magisk/releases/tag/v30.7) (APK `e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5`); `scripts/setup.sh` fetches it itself | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |
| `toolchain/android-ndk-r21e/` | unpack <https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip> into `toolchain/` | – |

The unlock's own files are the [XDA thread][xda]'s, the root's (`boot-root.zip`, the Echo Dot 3's: its system-as-root
path fits this firmware) [donut's XDA thread][xda-donut]'s: they say which, kept current with their procedure. Nothing
here downloads or reads them.

Use exactly this firmware version: everything was worked out against its binaries, and `scripts/probe.sh` checks the
Echo for it. Flashing it in step 1 replaces whatever the Echo ran before.

### 1. Unlock, flash stock firmware, root

The Echo Input runs from its micro-USB socket, which also carries its USB: no opening, no soldering. Until the install
the PC's port powers it (USB 2 gives 0.5 A, less than its own adapter; what it draws is not measured): if it restarts
or drops off adb during the later steps, put a powered USB hub between them.

Once, first: USB access without sudo, and ModemManager kept off the Echo's bootrom.

```sh
sudo cp scripts/51-echo-unlock.rules /etc/udev/rules.d/ && sudo udevadm control --reload
```

Unlock the Echo **by hand, as the [XDA thread][xda] says**, from its first post: which Echos it is for, its files, how
to get the Echo ready and every command are there, kept up to date. R0rt1z2 asks (since kamakiri-donut v1.1.0) that the
unlock is only ever run that way, never from an installer or a script, so this page does not repeat its steps and
`scripts/setup.sh cupcake` does not run them either: it names the thread, waits for Enter and then checks for TWRP on
adb. **Stop once TWRP is running**: the firmware below is this project's step, and boot-root comes after it. Stuck, or
the Echo does something the thread does not say? Ask in the thread, with what the Echo did, in your own words, and the
tool's output or log as it is.

Flash the firmware into both A/B slots, as on donut (TWRP makes the slot it flashed the active one; do not switch it
back by hand):

```sh
F=firmware/cupcake/update-kindle-cupcake-NS6575_user_8338_0013323244164.bin
adb shell twrp wipe cache; adb shell twrp wipe data
adb push $F /sdcard/update.zip
adb shell twrp install /sdcard/update.zip      # one slot; it becomes the active one
adb reboot recovery
adb shell twrp install /sdcard/update.zip      # must name the OTHER slot this time
adb reboot recovery
```

Then root, by hand: the Echo Dot 3's `boot-root` (its system-as-root path fits this firmware), installed from TWRP as
[donut's XDA thread][xda-donut] says, with the file the thread offers now, until Fire OS has started with root adb
(`adb shell id -u` says 0). `scripts/setup.sh cupcake` does not install it either: it waits for Enter and checks.

TWRP refuses to write lk, preloader and tee: leave them alone in any case. Result: stock Fire OS, unregistered, no
Wi-Fi, and `adb shell` is root.

### 2. Unpack the firmware and build

With `PREBUILT=1` (a commit CI built) only magiskpolicy needs fetching.

```sh
unzip firmware/cupcake/update-kindle-*.bin payload.bin -d firmware/cupcake/
python3 tools/payload_dump.py firmware/cupcake/payload.bin firmware/cupcake/images
mkdir -p firmware/cupcake/rootfs
debugfs -R "rdump / firmware/cupcake/rootfs" firmware/cupcake/images/system.img
curl -fsSL -o toolchain/Magisk-v30.7.apk https://github.com/topjohnwu/Magisk/releases/download/v30.7/Magisk-v30.7.apk   # the installer's magiskpolicy
sha256sum toolchain/Magisk-v30.7.apk   # e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5
unzip -p toolchain/Magisk-v30.7.apk lib/armeabi-v7a/libmagiskpolicy.so > firmware/cupcake/magiskpolicy32
chmod 755 firmware/cupcake/magiskpolicy32
sha256sum firmware/cupcake/magiskpolicy32   # e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a
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
