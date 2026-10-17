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
`4b6fb088…ce421` (full value in `device.conf`). The system partition must be exactly this build's: the boot image is
made from its `boot.img` and hassmic links against its libraries. amonet does not depend on the Fire OS version.

## Unlock (public: amonet-crown)

[`unlock.md`](unlock.md) is the guide from the XDA thread (R0rt1z2). Files into `firmware/crown/` (not in git):

| File | Source | sha256 |
|---|---|---|
| `amonet-crown-v2.0.1.zip` | the XDA thread | `5db974c321ae2f4586f7c51c95b7bdffb61ef85bde05ce7e62f78196a984c24f` |
| `update-kindle-crown-NS6570_user_6074_0012383140484.bin` | Amazon | `4b6fb088…ce421` |
| `boot-root.zip` | the XDA thread (one `boot-root.img`, NS6571's); the install takes only its `sbin/adbd` (`ADBD` in `device.conf`) | `d18f8256869820a87527ec08e75ec495362103ed4ad669e946d052f0f705012d` |
| `magiskpolicy32` | `patch/magiskpolicy32` of donut's `boot-root.zip` ([its README](../donut/README.md)); the guided setup takes it from there | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |

1. With the Echo on its power supply, hold all three buttons until "=> FASTBOOT mode..." shows, connect micro-USB,
   `./fastbrick.sh` (from the unzipped `amonet/`), type `YES`. **After the 10 s grace period nothing may interrupt it**
   (up to 5 min): that bricks it for good. It ends in TWRP.
2. TWRP asks "Keep System Read only?": tap **Keep Read Only**. Nothing of the install writes `/system` from TWRP.
3. Root: hacked fastboot (the Mute key while it boots, or from TWRP), `fastboot oem flags 61`, `fastboot flash boot
   boot-root.img`, then `fastboot continue` (not `reboot`: see docs/re-crown.md). adb is root. The guided setup does
   its own root step from the firmware's `boot.img` instead, so this is only for an Echo that should be rooted by hand.

**Never write lk, preloader, tee1/tee2 (tz) or any other bootloader partition.** An install for this model touches
`system` and `boot` only.

## Install

The guided setup does all of it: `scripts/setup.sh crown` (`--dry-run` walks the steps without running any). By hand:

1. **Unpack the firmware** (see below), build: `make DEVICE=crown`.
2. **System partition**: must be NS6570's (`adb shell getprop ro.build.version.incremental` = `0012383140484`). Else write
   `firmware/crown/images/system.img` (sha256 `0877b48d…bf41db`, 3253731328 bytes) to `/dev/block/platform/mtk-msdc.0/11230000.MSDC0/by-name/system`
   from TWRP, and read it back; `setup.sh` does this in 64 MB pieces with a check of each.
3. **Root boot image**: `scripts/install-boot.sh --boot-only` (the firmware's `boot.img` with root adb, `verify` out of
   the fstab, the policy patched by `sepolicy.rules`; flashed in hacked fastboot).
4. **Lock down, join Wi-Fi, install**: `scripts/wifi-join.sh` after the lockdown (`scripts/device/lockdown.sh`; do not
   join before it: Fire OS would reach Amazon), then `DEVICE=crown scripts/install-system.sh`.

Updating later: `scripts/ota-push.sh <echo-ip>` (`DEVICE=crown`). `scripts/install-system.sh --uninstall` turns Alexa's apps
back on and takes hassmic off `/system`; all the way back is the firmware `.bin` as a `.zip` in TWRP, or amonet's
`restore-stock-crown`.

## What works and what is open

Installed and running on a real Echo Show 8 (2026-10-14): boot into hassmic, Wi-Fi, Home Assistant (ESPHome entities,
buttons, voice questions answered), volume keys, mic mute, the light band (waiting for Alexa, listening, thinking,
reply). Bluetooth (proxy, A2DP sink, pairing) works once Android's stack is off the partition. The wake sound is the built-in
blip (stock's is not in this firmware); crown's extra Amazon apps are classified. **Open**: whether the blip is heard
after the cold-output lead-in (by ear). Details: [docs/re-crown.md](../../docs/re-crown.md).

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
