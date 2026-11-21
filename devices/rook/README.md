# Echo Spot 1st gen, 2017 (`rook`)

**Installed and running on one Echo Spot** (2026-11-20; "What works and what is open" below): voice in Home Assistant,
wake word, replies, volume, mic mute. Tracked in [#69](https://github.com/Gamer92000/echo-dot-assist/issues/69). The
Spot has the SoC of the Echo Show 5 and 8 (`checkers`, `crown`) but an older Android: Fire OS 5.5.6.9 is Android 5.1.1
(API 22) on a 3.18.19 kernel with a 32-bit userland, and the install is the Shows' (`INSTALL=boot`, through
`scripts/install-system.sh`, which hands over to `scripts/install-boot.sh`). Porting guide:
[devices/README.md](../README.md); what [crown's page](../crown/README.md) says about the install holds here too.

Model VN94DQ, MediaTek MT8163, round 480 x 480 touchscreen, 2 microphones, camera with a shutter, volume keys, mic mute
(a key on `mtk-kpd` and a latch). No light ring: hassmic draws it as a band along the bottom of the screen. No action
button. Broadcom Wi-Fi and Bluetooth.

## Firmware

Pinned: `update-kindle-rook-304.6.9.0_user_690918120.bin`, Fire OS 5.5.6.9 (`ro.build.display.id` is `LVY48F` on every
5.5.x build; the build is told apart by `ro.build.version.incremental`, `304.6.9.0_user_690918120`), sha256 `616db16d…5aa`
(full value in `device.conf`). The system partition must be exactly this build's: the boot image is made from its
`boot.img` and hassmic links against its libraries.

## Unlock (public: amonet-rook)

By hand, as the [XDA thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-spot-2017-rook.4754878/)
(R0rt1z2) says: [`unlock.md`](unlock.md). The files of this project's steps go into `firmware/rook/` (not in git):

| File | Source | sha256 |
|---|---|---|
| `update-kindle-rook-304.6.9.0_user_690918120.bin` | Amazon | `616db16d…5aa` |
| `magiskpolicy32` | `lib/armeabi-v7a/libmagiskpolicy.so` of [Magisk v30.7](https://github.com/topjohnwu/Magisk/releases/tag/v30.7)'s APK; the guided setup fetches it | `e5fafc1f…f3a` |
| `firmware/crown/rootfs/` (or checkers', donut's) | another model's unpacked firmware, only for its linker and libc: the policy tool does not start on Fire OS 5.5's (`SEPOLICY_ROOTFS` in `device.conf`; unpack as in [crown's page](../crown/README.md)) | |
| `adbd` | `sbin/adbd` of Google's Android 7.1.1 emulator image (see [crown's page](../crown/README.md)); the guided setup fetches it | see `device.conf` |

**Stop once TWRP shows on the Echo's screen**, and tap **Keep Read Only** if it asks. Nothing of the install writes
`/system` from TWRP except the system image's `dd` below.

**Never write lk, preloader, tee1/tee2 (tz) or any other bootloader partition, and never flash the `.bin` in TWRP**: its
updater-script writes them. An install for this model touches `system` and `boot` only.

## Install

The guided setup does all of it: `scripts/setup.sh rook` (`--dry-run` walks the steps without running any). By hand:

1. **Unpack the firmware**, build: `make DEVICE=rook` (see below).
2. **System partition**: must be 304.6.9.0's (`adb shell getprop ro.build.version.incremental`). Else write
   `firmware/rook/images/system.img` (sha256 `f14d0493…ce8da67`, 1744723968 bytes) to
   `/dev/block/platform/mtk-msdc.0/by-name/system` from TWRP with `dd`, and read it back (`setup.sh` does it in 64 MB
   pieces with a check of each). Wipe cache and data first.
3. **Root boot image**: `scripts/install-boot.sh --boot-only`: the firmware's `boot.img` with root adb, `verify` out of
   the fstab, the policy patched by `sepolicy.rules`, and `hassmic.rc` appended to `init.rook.rc` (Android 5.1's init reads
   rc files from the boot ramdisk only: `RAMDISK_RC` in `device.conf`). Flashed in amonet's hacked fastboot.
4. **Lock down, join Wi-Fi, install**: `scripts/device/lockdown.sh`, then `scripts/wifi-join.sh` (do not join before the
   lockdown: Fire OS would reach Amazon), then `DEVICE=rook scripts/install-system.sh`.

`hassmic.rc` lives in the boot ramdisk here, so an update (push or online) cannot change it: a release that changes
`hassmic.rc` needs `scripts/install-boot.sh --boot-only` again (a bundle's copy in `/system/etc/init` is not read).

Updating later: `scripts/ota-push.sh <echo-ip>` (`DEVICE=rook`). `scripts/install-system.sh --uninstall` turns Alexa's apps
back on and takes hassmic off `/system`.

## What works and what is open

Works on the Echo Spot (2026-11-20): boot into hassmic (40-50 s), Wi-Fi, Home Assistant (ESPHome, encrypted), wake word
"alexa", replies, volume keys, mic mute, the light band. The wake sound is the built-in blip (Amazon's is not a plain
resource in this firmware). Android 5.1 is old: its toolbox lacks `sed`, `tr`, `cut`, `head`, `tail`, `wc`, `pkill`,
`pidof`, `timeout`, `awk`, so the build ships a toybox (`tools/toybox.sh`, `TOYBOX := 1` in `device.mk`) that `main.sh`
puts on the PATH.

The light band is an arc along the lower edge of the round glass (`board.round_px`), 40 rows by default, up to 120
("Light band" on the settings page). Keep it at 40 or less if you want an Android dialog's buttons reachable.

**Known limits**: Amazon's newer Alexa wake word model (the one with a `DETECTION_AUXILIARY` spotter) does not load
here, the page's copy refuses it ("does not load here"): this firmware's `libpryon` predates it. Its own stock "Alexa" and
the Echo model work, as do sound detection and whisper copied from other Echos.

**Open**: the volume curve (the Dot 2's stands in), light sensor, speaker gain, Wi-Fi motion (Broadcom: no module),
Bluetooth (no `/dev/stpbt`; `ttyMT1`), Wi-Fi setup from a phone (Improv needs Bluetooth), "Download from Amazon" (no
device attestation token yet), hassmic's own screen and Kiosk Satellite (Android 5.1).

## Files here

| File | State |
|---|---|
| `device.mk` | OpenSL ES, Pryon, light band, API 22 (`armv7a-linux-androideabi22`), toybox |
| `board.c` | as the Shows', probed on the Echo; speaker gain `HP Driver Gain Volume` stock 9 |
| `device.conf` | 5.5.6.9 pins, `INSTALL=boot`, `RAMDISK_RC`, `SEPOLICY_ROOTFS`, Alexa's packages; `STOCK_APPS_OUT`: System UI and Amazon Communication Services (`tcomm`), the two persistent apps that crash without Alexa's client; the rest of Amazon's apps are only disabled |
| `sepolicy.rules`, `hassmic.rc` | the Shows', appended to `init.rook.rc` |
| `probe.md5`, `stubs/` | from the pinned firmware |
| `setup.sh` | the guided steps, as crown's |

## Unpack the firmware

```sh
unzip firmware/rook/update-kindle-*.bin -d firmware/rook/
mkdir -p firmware/rook/images firmware/rook/rootfs/system
mv firmware/rook/boot.img firmware/rook/images/
python3 tools/sdat2img.py firmware/rook/system.transfer.list firmware/rook/system.new.dat firmware/rook/images/system.img
debugfs -R "rdump / firmware/rook/rootfs/system" firmware/rook/images/system.img   # ownership errors are harmless
```
