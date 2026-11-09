# Echo Show 5 1st gen, 2019 (`checkers`)

Turns an Echo Show 5 (1st gen, model H23K37) into a Home Assistant voice satellite.

**Status:** runs on one Echo Show (a tester's, since 2026-10-10), installed with the guided setup: wake word, Home
Assistant's voice pipeline, Music Assistant, a Bluetooth speaker and the light sensor work. There is no action button:
both volume keys pressed together stand in for it. There is no light ring on this model, so hassmic draws the ring as
a band along the bottom of the screen (setting "Light band"). The screen shows hassmic's face, or an app you pick
(Kiosk Satellite, a dashboard): [docs/GUIDE.md](../../docs/GUIDE.md#the-screen-echo-show). What works and what is
still open, item by item: [docs/re-checkers.md](../../docs/re-checkers.md#what-a-port-needs).

Hardware: MediaTek MT8163, 5.5" touchscreen, 2 microphones, camera with a shutter, volume keys, mic mute.
`ro.product.model` is AEOCH. Not to be confused with the Echo Show 5 2nd gen ([`cronos`](../cronos/README.md)) or the
Echo Show 8 ([`crown`](../crown/README.md)): they have unlocks of their own.

## Read this first

The unlock and install write to the Echo's flash. These rules keep it recoverable:

- **Never write lk, preloader, tee1/tee2 (tz) or any other bootloader partition**, not even from USB download mode.
  Most units have no way back from a broken bootloader: the brick is permanent. Of the partitions, this install writes
  only `boot` and `system` (step 3 also wipes cache and data). Going all the way back to stock is the one exception:
  [below](#going-back-to-stock).
- **The unlock is the [XDA thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-5-1st-gen-2019-checkers.4762900/)'s.** Do it by hand, as the thread says: what must not be
  interrupted, and when it is over, is there.
- **In TWRP, always tap "Keep Read Only"** when it asks "Keep System Read only?". A system TWRP has mounted read-write
  no longer passes the stock boot image's checks, and the stock image is your way back while `/system` is untouched.
- **Do not join Wi-Fi before the lockdown step.** Fire OS would reach Amazon and could update itself, which can close
  the unlock.
- **Once hassmic is on `/system`, only a boot image without `verify` boots it.** The stock `boot.img` would find the
  system's hashes wrong. To go back, see [Going back to stock](#going-back-to-stock).

## What you need

- The Echo Show 5 1st gen, its power supply, and a micro-USB cable.
- A Linux PC (x86_64 is easiest: the setup downloads the qemu-arm it needs; elsewhere install `qemu-user`).
- One file to download yourself: save it into `~/Downloads`, the guided setup picks it up from there (or from
  `firmware/checkers/`) and checks its sha256. The two programs below it the setup fetches itself.

| File | Where to get it | sha256 |
|---|---|---|
| `update-kindle-checkers-NS65741_user_8149_0013222532484.bin` | Amazon's firmware, Fire OS 6574.1 (NS65741/8149), 1372825620 bytes: download it from its [FTVDB record](https://ftvdb.com/echo/firmware/com.amazon.checkers.android.os/2a4c5a8f3af7da963955229bd1431411-13222532484-fire-os-6574-1-ns65741-8149-2026-09-28/) (Amazon's own help pages give only version numbers now). Downloaded and checked 2026-11-01 | `f96fcfa1240f809711fcf855ce7742762437e4f37546153153635c096773cfa3` |
| `magiskpolicy32` | `lib/armeabi-v7a/libmagiskpolicy.so` of `Magisk-v30.7.apk`, [Magisk v30.7](https://github.com/topjohnwu/Magisk/releases/tag/v30.7) (APK `e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5`): the policy patch, run on the PC under qemu-arm | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |
| `adbd` | `sbin/adbd` of Google's Android 7.1.1 emulator image (`system-images;android-25;google_apis;armeabi-v7a` r18; only its ramdisk is read, `scripts/zipmember.py`): a userdebug adbd that goes by `ro.secure`, for root adb in our boot image (`ADBD` in `device.conf`). Not tried on a Show yet; until 2026-11-09 the XDA boot-root image's (CyanogenMod's) was the tested one | `cc0178051cbf3fa0c7df5c880abd1becf55ce9f35b60ac851aae2edd106a3c18` |

The unlock's own files are the [XDA thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-5-1st-gen-2019-checkers.4762900/)'s: it says which, kept current with its procedure. Nothing
here downloads or reads them, its `boot-root.zip` included.

Pin the firmware: the Echo must run exactly build 8149 (`ro.build.version.incremental` 0013222532484). The boot
image is made from this build's `boot.img`, and hassmic links against its libraries. The XDA post names no version,
but 8149 fits: the `lk.bin` and `tz.img` inside amonet-checkers v2.0.1 are byte-identical to 8149's.

## Install

`scripts/setup.sh checkers` does all of it: the top-level [README](../../README.md#install) says how it runs (stopping
and resuming, `--dry-run`, an Echo rooted already). Below is the same work by hand, for when something needs doing
differently. The order differs a little: the setup locks the Echo down and joins Wi-Fi before it installs.
`scripts/install-system.sh` hands over to `scripts/install-boot.sh` for this model; the guided setup calls the same
scripts.

### 1. Unlock

Unlock the Echo **by hand, as the [XDA
thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-5-1st-gen-2019-checkers.4762900/) says**, from
its first post: which Echos it is for, its files, how to get the Echo ready and every command are there, kept up to
date. R0rt1z2 asks (since kamakiri-donut v1.1.0) that the unlock is only ever run that way, never from an installer or a
script, so this page does not repeat its steps and `scripts/setup.sh checkers` does not run them either: it names the
thread, waits for Enter and then checks for TWRP on adb. **Stop once TWRP shows on the Echo's screen**: the system, root
and install below are this project's. Stuck, or the Echo does something the thread does not say? Ask in the thread, with
what the Echo did, in your own words, and the tool's output or log as it is.

Once unlocked, a key held while connecting power picks what it boots:

| Hold | Boots into |
|---|---|
| Volume down | the unlock's hacked fastboot |
| Volume up | TWRP |

In TWRP, adb answers while the screen shows; answering "Keep Read Only" restarts TWRP's USB, so adb drops for a few
seconds.

### 2. Unpack the firmware on the PC

```sh
mkdir -p firmware/checkers && mv ~/Downloads/update-kindle-checkers-*.bin firmware/checkers/
unzip firmware/checkers/update-kindle-*.bin -d firmware/checkers/
mkdir -p firmware/checkers/images firmware/checkers/rootfs/system
mv firmware/checkers/boot.img firmware/checkers/images/
python3 tools/sdat2img.py firmware/checkers/system.transfer.list firmware/checkers/system.new.dat firmware/checkers/images/system.img
debugfs -R "rdump / firmware/checkers/rootfs/system" firmware/checkers/images/system.img
python3 scripts/mkbootroot.py --cpio firmware/checkers/images/boot.img firmware/checkers/ramdisk.cpio
(cd firmware/checkers/rootfs && cpio -id < ../ramdisk.cpio)
```

Then the two programs, from where they are made (the guided setup does the same):

```sh
curl -fsSL -o toolchain/Magisk-v30.7.apk https://github.com/topjohnwu/Magisk/releases/download/v30.7/Magisk-v30.7.apk
sha256sum toolchain/Magisk-v30.7.apk   # e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5
unzip -p toolchain/Magisk-v30.7.apk lib/armeabi-v7a/libmagiskpolicy.so > firmware/checkers/magiskpolicy32
python3 scripts/zipmember.py https://dl.google.com/android/repository/sys-img/google_apis/armeabi-v7a-25_r18.zip \
    armeabi-v7a/ramdisk.img firmware/checkers/adbd --cpio sbin/adbd
chmod 755 firmware/checkers/magiskpolicy32 firmware/checkers/adbd
sha256sum firmware/checkers/magiskpolicy32 firmware/checkers/adbd   # SEPOLICY_TOOL_SHA256 and ADBD_SHA256 in device.conf
```

More on the firmware's layout: [docs/re-checkers.md](../../docs/re-checkers.md#unpacking).

### 3. Put build 8149 on the system partition (only if needed)

Skip this if the Echo already runs 8149. To find out, boot TWRP, mount the system partition read-only and read its
`build.prop` (TWRP's own `getprop` reports TWRP's build, not the system's). A tester's Echo came out of the unlock
with 0013054618500.

Write only `system`, from the image you unpacked, and read it back. Do **not** use `twrp install` with the `.bin`: its
updater script also writes lk, tee1/tee2, the preloader and an MBR.

```sh
# in TWRP's main menu, past "Keep System Read only?" (Keep Read Only)
adb shell 'twrp wipe cache && twrp wipe data'   # the old build's app updates in /data/app would run over this system
adb shell 'mount /data 2>/dev/null; grep " /data " /proc/mounts && ls /data/app'   # must print the mount and no app
adb exec-in 'dd of=/dev/block/platform/mtk-msdc.0/11230000.MSDC0/by-name/system bs=4194304 && sync' < firmware/checkers/images/system.img
adb shell sha256sum /dev/block/platform/mtk-msdc.0/11230000.MSDC0/by-name/system   # must equal SYSTEM_IMG_SHA256 in device.conf
```

The image is exactly the partition's 3253731328 bytes. The boot partition still holds the old build's image, which
does not boot this system: go straight on to step 4 without rebooting.

### 4. Root boot image and the install

With the Echo in TWRP on USB:

```sh
DEVICE=checkers scripts/install-system.sh
```

`DEVICE=checkers` is needed because in TWRP the model can't be read from the Echo. The script does three things:

1. **Builds the boot image on the PC** from 8149's own `boot.img` (checked against `BOOT_SHA256`): root adb, `verify`
   taken out of `fstab.mt8163`, the SELinux policy patched with `sepolicy.rules` (by magiskpolicy under qemu-arm, since
   the Echo has no root yet), and two lines of stock's `init.checkers.rc` taken out: `setprop service.adb.tcp.port
   5555` (with root adb and no authentication it opened a root shell to the network at every boot) and `start
   bootsound_anim` (Amazon's boot chime, at full volume). Why it can't just use the XDA thread's `boot-root.img`:
   [docs/re-checkers.md](../../docs/re-checkers.md#the-xda-boot-rootimg).
2. **Flashes it.** Checks the system build first, then `adb reboot bootloader` into hacked fastboot, `fastboot flash
   boot`, `fastboot continue`. Only `boot` is written, and only when it differs.
3. **Installs the files from the running Fire OS**: checks that adb is root in the `su` domain, then writes
   `/system/hassmic/`, `/system/etc/init/hassmic.rc` and `hassmic.conf`, and reboots.

To get root adb without installing (for trying things with `scripts/deploy.sh`):
`scripts/install-boot.sh --boot-only`.

### 5. Wi-Fi

Only after the lockdown (the firewall is part of the install). `scripts/wifi-join.sh` joins through Android's
WifiService: the SSID goes in as hex and the passphrase as its PSK, computed on the PC, so neither passes through a
shell. It worked on the first Echo Show (2026-10-13). There is no on-screen fallback: the install takes Amazon's apps
off the system partition, Settings among them. The settings page's "Switch network…" uses the same calls.

## Kiosk Satellite (optional)

Shows a Home Assistant dashboard on the screen; hassmic stays the voice satellite. The guided setup offers it. On an
Echo that is already installed:

```sh
scripts/kiosk-install.sh             # over USB
scripts/kiosk-install.sh <echo-ip>   # over Wi-Fi
scripts/kiosk-install.sh --uninstall [<echo-ip>]   # takes it off again
```

That fetches the newest release and Simple Keyboard, and the Echo opens the app by itself. Do its setup from your PC
at `http://<echo-ip>:2324`, where you can paste the Home Assistant token: scanning a QR code on the Echo needs "Kiosk
Satellite camera" on the settings page (System). Set its remote admin password: that page is open to your network.
Leave the app's voice assistant off, and do not make it the home app.

By hand: `kiosk-satellite-<version>.armeabi-v7a.apk` from its [releases](https://github.com/jxlarrea/kiosk-satellite/releases),
`adb install`. What the app is allowed, and what it costs: [docs/GUIDE.md](../../docs/GUIDE.md#kiosk-satellite-on-the-screen).

## How it runs

At boot, `hassmic.rc` starts the firewall at `on boot` and the satellite once `sys.boot_completed` is set.
`alexa-off.sh` disables Alexa's apps with `pm` (`ALEXA_PACKAGES`, `SETUP_PACKAGES`, `UPDATE_PACKAGES` in
`device.conf`) and notes which in `/data/local/hassmic/alexa-disabled`. Without `hassmic.conf`, `boot.sh` turns exactly
those back on. The home screen is hassmic's own (`tools/mkhome.py`, an APK without code), and hassmic's screen UI
draws the face over it and the light band along the bottom; the settings page's Screen page can make another app the
home app.

Wi-Fi goes through `scripts/device/wifictl.dex` (`tools/mkwifictl.py`), run as `CLASSPATH=…/wifictl.dex app_process /
Wifictl <verb>`. It puts in the same `android.net.wifi.WifiConfiguration` stock's setup apps do, from root, with no
touchscreen and no Alexa app.

## Going back to stock

- `scripts/install-system.sh --uninstall` turns Alexa's apps back on, takes hassmic off `/system` and wipes cache and
  data in TWRP (tap **Keep Read Only** when it asks): Amazon's apps come back without their data and default
  permissions otherwise, and their setup wizard crashes in a loop ("OOBE has stopped"). It keeps the root boot image.
- All the way back to stock (the firmware with its bootloaders) and unbricking are the
  [XDA thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-5-1st-gen-2019-checkers.4762900/)'s:
  as it says. It is the one step that goes against the first rule above: the firmware `.bin`'s updater script also
  writes lk, tee1/tee2, the preloader and an MBR ([docs/re-checkers.md](../../docs/re-checkers.md#what-a-port-needs)).

## Troubleshooting

- **Every reboot lands in fastboot.** `adb reboot bootloader` sets the RTC's boot-to-fastboot flag and kaeru 2.0.0
  does not clear it (`fastboot getvar boot-reason` says RTC). Use `fastboot continue` to boot from fastboot, and pull
  the power once to clear the flag.
- **A boot fails after flashing.** Hacked fastboot stays: hold Volume down while connecting power, then `fastboot
  flash boot build/checkers/boot.stock.img` (8149's own) while nothing of hassmic is on `/system` yet, or
  `build/checkers/boot-hassmic.img` (the install's) once it is: the stock image does not accept that system. Then
  `fastboot continue`.
- **Logs from the bootloader:** `fastboot oem logcat` prints the preloader's and lk's log.

## Reference

### The firmware, compared with the Echo Dot 3 (2026-10-07)

8149 is the same Fire OS release as donut's.

- `lib/libpryon.so`, `libopus.so`, `libz.so` are **byte-identical**. The `en-US/ALEXA` model (a newer layout) detects
  ALEXA in `pryon_test` under qemu.
- **No `mixer`, no `libmixerAPI.so`.** Amazon's audio front end runs inside the audio HAL; hassmic uses Android's audio
  through OpenSL ES (`src/hassmic/audio_android.c`).
- A full Android device: Alexa is apps (stopped with `pm disable`), Wi-Fi is Android's, netd owns iptables, Bluetooth
  is Android's stack.
- No action button, no LED ring; a screen.

### Files in this folder

| File | What it holds |
|---|---|
| `device.mk` | OpenSL ES backend, Pryon, the light band; Wi-Fi motion's module `hassmic_rcpi4m` built for 32-bit ARM (kernel.org 4.9.77, arm-eabi-4.8) |
| `board.c` | from the firmware, checked on the Echo: input devices by name, privacy latch, light sensor (`als_ps/lux`) |
| `device.conf` | identity and probe files; `INSTALL=boot` (boot and system partition, firmware build, policy tool), Alexa's packages, `FW_KEEP` (DHCP: no stock `firewall.sh`) |
| `sepolicy.rules`, `hassmic.rc` | the boot image's policy patch; init services (firewall at `on boot`, satellite at `sys.boot_completed`) |
| `kconfig` | the kernel's IKCONFIG (4.9.77, ARM) |
| `probe.md5`, `stubs/` | from the pinned firmware |
| `setup.sh` | the guided setup's steps for this model |

Porting another model: [devices/README.md](../README.md).
