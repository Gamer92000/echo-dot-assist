# Echo Show 8 1st gen, 2019 (`crown`)

**Installed and running on one Echo Show 8** (2026-10-14; "What works and what is open" below,
[docs/re-crown.md](../../docs/re-crown.md)); the guided setup was tried on it too. It is the platform of the Echo Show 5 (`checkers`): the install is the
same, `INSTALL=boot` through `scripts/install-system.sh`, which hands over to `scripts/install-boot.sh`. No light
ring: hassmic shows it as a band along the bottom of the screen (setting "Light band"). Porting guide:
[devices/README.md](../README.md); what checkers' page says about the install holds here too, and is not repeated:
[devices/checkers/README.md](../checkers/README.md).

Model AEOCW (`ro.product.model`), MediaTek MT8163, 8" touchscreen (1280 x 800 landscape), 2 microphones, camera with a
shutter, volume keys, mic mute. No action button.

## Firmware

Pinned: `update-kindle-crown-NS6570_user_6074_0012383140484.bin`, Fire OS 6.5.7.0 (NS6570), sha256
`4b6fb088…ce421` (full value in `device.conf`). Download it from its
[FTVDB record](https://ftvdb.com/echo/firmware/com.amazon.crown.android.os/90ad1be88b4e7ed9ddb0ef58c8d6200a-12383140484-fire-os-6-5-7-0-ns6570-6074-2025-08-23/).
The system partition must be exactly this build's: the boot image is made from its `boot.img` and hassmic links
against its libraries.

## Unlock (public: amonet-crown)

By hand, as the [XDA
thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-8-1st-gen-2019-crown.4766687/) (R0rt1z2) says:
[`unlock.md`](unlock.md). The unlock's own files are the thread's: it says which, kept current with its procedure;
nothing here downloads or reads them, its `boot-root.zip` included. The files of this project's steps go into
`firmware/crown/` (not in git):

| File | Source | sha256 |
|---|---|---|
| `update-kindle-crown-NS6570_user_6074_0012383140484.bin` | Amazon, link above | `4b6fb088…ce421` |
| `magiskpolicy32` | `lib/armeabi-v7a/libmagiskpolicy.so` of `Magisk-v30.7.apk`, [Magisk v30.7](https://github.com/topjohnwu/Magisk/releases/tag/v30.7) (APK `e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5`); the guided setup fetches it, by hand as in [checkers' README](../checkers/README.md#2-unpack-the-firmware-on-the-pc) | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |
| `adbd` | `sbin/adbd` of Google's Android 7.1.1 emulator image (`system-images;android-25;google_apis;armeabi-v7a` r18; only its ramdisk is read, `scripts/zipmember.py`): a userdebug adbd that goes by `ro.secure`, for root adb in our boot image (`ADBD` in `device.conf`). The guided setup fetches it. Not tried on a Show yet; until 2026-11-09 the XDA boot-root image's (CyanogenMod's) was the tested one | `cc0178051cbf3fa0c7df5c880abd1becf55ce9f35b60ac851aae2edd106a3c18` |

**Stop once TWRP shows on the Echo's screen.** TWRP asks "Keep System Read only?": tap **Keep Read Only**. Nothing of
the install writes `/system` from TWRP. The guided setup does its own root step from the firmware's `boot.img`; an Echo
that should only be rooted, without the install: as the XDA thread says.

**Never write lk, preloader, tee1/tee2 (tz) or any other bootloader partition.** An install for this model touches
`system` and `boot` only.

## Install

The guided setup does all of it: `scripts/setup.sh crown` (`--dry-run` walks the steps without running any). By hand:

1. **Unpack the firmware** (see below), build: `make DEVICE=crown`.
2. **System partition**: must be NS6570's (`adb shell getprop ro.build.version.incremental` = `0012383140484`). Else write
   `firmware/crown/images/system.img` (sha256 `0877b48d…bf41db`, 3253731328 bytes) to `/dev/block/platform/mtk-msdc.0/11230000.MSDC0/by-name/system`
   from TWRP, and read it back; `setup.sh` does this in 64 MB pieces with a check of each. Wipe cache and data first,
   from TWRP's main menu (past "Keep System Read only?": answering it restarts USB), with
   `adb shell 'twrp wipe cache && twrp wipe data'`: the old build's app updates in `/data/app` would run over this
   older system. `adb shell 'mount /data 2>/dev/null; grep " /data " /proc/mounts && ls /data/app'` must then print the mount and no app.
3. **Root boot image**: `scripts/install-boot.sh --boot-only` (the firmware's `boot.img` with root adb, `verify` out of
   the fstab, the policy patched by `sepolicy.rules`; flashed in hacked fastboot).
4. **Lock down, join Wi-Fi, install**: `scripts/wifi-join.sh` after the lockdown (`scripts/device/lockdown.sh`; do not
   join before it: Fire OS would reach Amazon), then `DEVICE=crown scripts/install-system.sh`.

Updating later: `scripts/ota-push.sh <echo-ip>` (`DEVICE=crown`). `scripts/install-system.sh --uninstall` turns Alexa's
apps back on, takes hassmic off `/system` and wipes cache and data in TWRP (as on [cronos](../cronos/README.md)); all
the way back to stock is the XDA thread's.

## What works and what is open

Installed and running on a real Echo Show 8 (2026-10-14): boot into hassmic, Wi-Fi, Home Assistant (ESPHome entities,
buttons, voice questions answered), volume keys, mic mute, the light band (waiting for Alexa, listening, thinking,
reply). Bluetooth (proxy, A2DP sink, pairing) works once Android's stack is off the partition. The wake sound is the built-in
blip (stock's is not in this firmware); crown's extra Amazon apps are classified. **Open**: whether the blip is heard
after the cold-output lead-in (by ear), hassmic's own screen (the face, the drawer, the home app, Music Assistant:
[the guide](../../docs/GUIDE.md#the-screen-echo-show)) on the Echo: built and checked against this firmware's
framework only. Details: [docs/re-crown.md](../../docs/re-crown.md).

## Files here

| File | State |
|---|---|
| `device.mk` | as checkers: OpenSL ES, Pryon, light band, `hassmic_rcpi4m` for 32-bit ARM (kernel 4.9.77) |
| `board.c` | from checkers, checked on the Echo; speaker gain stock 30; no `ui_wakesound` |
| `device.conf` | NS6570 pins, `INSTALL=boot`, Alexa's packages and `STOCK_APPS_OUT` cut to what crown has |
| `sepolicy.rules`, `hassmic.rc` | checkers' |
| `kconfig` | the Echo's own IKCONFIG (4.9.77, ARM) |
| `probe.md5`, `stubs/` | from the pinned firmware |
| `setup.sh` | the guided steps, as checkers' |

## Unpack the firmware

```sh
unzip firmware/crown/update-kindle-*.bin -d firmware/crown/
mkdir -p firmware/crown/images firmware/crown/rootfs/system
mv firmware/crown/boot.img firmware/crown/images/
python3 tools/sdat2img.py firmware/crown/system.transfer.list firmware/crown/system.new.dat firmware/crown/images/system.img
debugfs -R "rdump / firmware/crown/rootfs/system" firmware/crown/images/system.img   # ownership errors are harmless
python3 scripts/mkbootroot.py --cpio firmware/crown/images/boot.img firmware/crown/ramdisk.cpio
(cd firmware/crown/rootfs && cpio -id < ../ramdisk.cpio)
```

## Kiosk Satellite (optional)

Shows a Home Assistant dashboard on the screen; hassmic stays the voice satellite. The guided setup offers it. On an Echo
that is already installed:

```sh
scripts/kiosk-install.sh             # over USB
scripts/kiosk-install.sh <echo-ip>   # over Wi-Fi
scripts/kiosk-install.sh --uninstall [<echo-ip>]   # takes it off again
```

That fetches the newest release and Simple Keyboard, and the Echo opens the app by itself. Do its setup from your PC at
`http://<echo-ip>:2324`, where you can paste the Home Assistant token: scanning a QR code on the Echo needs "Kiosk Satellite
camera" on the settings page (System). Set its remote admin password: that page is open to your network. Leave the app's voice assistant off, and do not make it the home app.

By hand: `kiosk-satellite-<version>.armeabi-v7a.apk` from its [releases](https://github.com/jxlarrea/kiosk-satellite/releases),
`adb install`. What the app is allowed, and what it costs: [docs/GUIDE.md](../../docs/GUIDE.md#kiosk-satellite-on-the-screen).
