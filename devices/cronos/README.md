# Echo Show 5 2nd gen, 2021 (`cronos`)

**Installed and running on one Echo Show 5 2nd gen** (2026-10-26, with the guided setup; "What works and what is open" below, [docs/re-cronos.md](../../docs/re-cronos.md)). It is the Echo Show 5 1st gen (`checkers`) with another mute button
driver and other speaker amplifiers: the same platform, the same Fire OS build (NS65741/8149), the same install
(`INSTALL=boot` through `scripts/install-system.sh`, which hands over to `scripts/install-boot.sh`). No light ring:
hassmic shows it as a band along the bottom of the screen (setting "Light band"). Porting guide:
[devices/README.md](../README.md); what checkers' page says about the install holds here too, and is not repeated:
[devices/checkers/README.md](../checkers/README.md).

Model C76N8S (`ro.product.model` AEOCN), MediaTek MT8163, 5.5" touchscreen (1024 x 600 panel), 2 microphones, camera
with a shutter, volume keys, mic mute. No action button.

## Firmware

Pinned: `update-kindle-cronos-NS65741_user_8149_0013222532484.bin`, Fire OS 6574.1 (NS65741/8149), sha256
`fa6203b55d8764201cd531507750e66e1ab2215969433b52d715a4b6089f1b09`, 1388134745 bytes. Amazon's link, as
[ftvdb.com](https://ftvdb.com/echo/firmware/com.amazon.cronos.android.os/) lists it:
<https://d1s31zyz7dcc2d.cloudfront.net/2026/9/28/d8beae39-5193-4cbe-8efd-b1ea742e772e/update-kindle-cronos-NS65741_user_8149_0013222532484.bin>.
This is cronos' own image, not checkers' (same build number, other files). The system partition must be exactly this
build's: the boot image is made from its `boot.img` and hassmic links against its libraries. Newer builds (8150, 8151)
are not supported until pinned here.

## Unlock (public: amonet-cronos)

[`unlock.md`](unlock.md) is the guide from the XDA thread (R0rt1z2). Files into `firmware/cronos/` (not in git):

| File | Source | sha256 |
|---|---|---|
| `amonet-cronos-v2.0.1.zip` | the XDA thread; its `lk.bin` and `tz.img` are 8149's | `07a49c8fb2b244330297810b261333ba23976eca5ab3e0d9032f45f4c9847621` |
| `update-kindle-cronos-NS65741_user_8149_0013222532484.bin` | Amazon (above) | `fa6203b5…f1b09` |
| `boot-root.zip` | the XDA thread's second post (installed in TWRP there; its `boot.img` is NS6571's); the install takes only `sbin/adbd` out of it (`ADBD` in `device.conf`: checkers' file, `108ba403…`) | `49326e27cc42159b2659604a586855ca0a267f3e064ed42cfa64cd78275e40b3` |
| `magiskpolicy32` | `patch/magiskpolicy32` of donut's `boot-root.zip` ([its README](../donut/README.md)); the guided setup takes it from there | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |

amonet's `fastbrick.sh` runs its own `bin/fastboot`, an x86_64 Linux program (glibc only), on every 64-bit PC. On an
aarch64 PC such as a Raspberry Pi it runs only under qemu-user with x86_64 glibc (`qemu-user-binfmt`, `QEMU_LD_PREFIX`):
try it first with the Echo in fastboot mode, `bin/fastboot getvar product` (reads only), before the unlock. The guided
setup itself expects an x86_64 PC (the qemu-arm it downloads, the NDK); a Raspberry Pi worked with `qemu-user` from its
package manager (docs/re-cronos.md, "First install").

**Never write lk, preloader, tee1/tee2 (tz) or any other bootloader partition.** This model has no bootrom USB download:
a brick there is permanent. An install for this model touches `system` and `boot` only.

## Install

The guided setup does all of it: `scripts/setup.sh cronos` (`--dry-run` walks the steps without running any). On Linux
(Ubuntu: `adb`, `fastboot`, `e2fsprogs` for `debugfs`, `python3`, `unzip`, `curl`, `make`, `cc`, `sqlite3`, `bc`, `xz`). By hand,
as checkers':

1. **Unpack the firmware** (below), build: `make DEVICE=cronos` (or `make DEVICE=cronos STUBS=1` without the firmware).
2. **Unlock** ([`unlock.md`](unlock.md)); the Echo ends in TWRP: tap **Keep Read Only**.
3. **System partition**: must be 8149's (`ro.build.version.incremental` = `0013222532484` in its `build.prop`). Else write
   `firmware/cronos/images/system.img` (sha256 `78c4c2fa…091669`, 3253731328 bytes) to
   `/dev/block/platform/mtk-msdc.0/11230000.MSDC0/by-name/system` from TWRP and read it back; `setup.sh` does this in
   64 MB pieces with a check of each.
4. **Root boot image**: `scripts/install-boot.sh --boot-only` (the firmware's `boot.img`, sha256 `ecdd1452…87db49`, with
   root adb, `verify` out of the fstab, adb over Wi-Fi and the boot chime out of `init.cronos.rc`, the policy patched by
   `sepolicy.rules`; flashed in hacked fastboot, then `fastboot continue`).
5. **Lock down, join Wi-Fi, install**: `scripts/wifi-join.sh` after the lockdown (`scripts/device/lockdown.sh`; do not
   join before it: Fire OS would reach Amazon), then `DEVICE=cronos scripts/install-system.sh`.

Updating later: `DEVICE=cronos scripts/ota-push.sh <echo-ip>`. `scripts/install-system.sh --uninstall` turns Alexa's
apps back on and takes hassmic off `/system`; all the way back is the firmware `.bin` as a `.zip` in TWRP after a factory
reset there (amonet v2.0.0 and later), as the thread describes.

## What works and what is open

Seen on the first cronos (2026-10-26; [docs/re-cronos.md](../../docs/re-cronos.md)): the whole guided setup (amonet,
system partition, root boot image, build, lockdown, Wi-Fi, install), hassmic up and in Home Assistant, the wake word
engine, the volume keys, the mute button through `gpio-privacy` (athena's driver, not checkers' `amazon-gating`), the
light band (960 x 10), the light sensor (`als_ps/lux`), a push update over Wi-Fi that passed its self test.

The speaker amplifier is one of three by board revision; the first cronos has the Maxim MAX98396. Speaker gain:
RT5616/RT5616E as checkers', TAS5805M up to +4 dB, MAX98396 up to +6 dB, the caps until someone has listened at full
volume (the TAS5805M's steps are assumed, it is in no cronos seen yet).

**Open:** speaker gain by ear, Wi-Fi motion (module built, its hook site as checkers'), the DHA version for "Download
from Amazon", Bluetooth.

The screen stays black but for the band: the install takes Amazon's apps off the system partition, Settings and System
UI among them, as on checkers (`STOCK_APPS_OUT`; why: [devices/checkers/README.md](../checkers/README.md)). The Echo's
settings are on its settings page and in Home Assistant.

## Files here

| File | State |
|---|---|
| `device.mk` | checkers': OpenSL ES, Pryon, light band, `hassmic_rcpi4m` for 32-bit ARM (kernel 4.9.77), own kconfig |
| `board.c` | checkers' but the mute button (gpio-privacy, as athena) and the amplifiers (`speaker_gain_alt`); checked on the Echo |
| `device.conf` | 8149 pins of this firmware, `INSTALL=boot`, checkers' packages and services; `STOCK_APPS_OUT` without the two apps cronos lacks |
| `sepolicy.rules`, `hassmic.rc` | checkers' (the policy patch checked with magiskpolicy on the PC) |
| `kconfig` | the firmware's own IKCONFIG (4.9.77, ARM) |
| `probe.md5` | from the pinned firmware |
| `stubs/` | checkers' (the same symbols, in the same order, in this firmware's libraries) |
| `setup.sh` | checkers' steps with cronos' XDA files, the bootloader check before amonet, the udev step |

## Unpack the firmware

On Linux:

```sh
unzip firmware/cronos/update-kindle-*.bin -d firmware/cronos/
mkdir -p firmware/cronos/images firmware/cronos/rootfs/system
mv firmware/cronos/boot.img firmware/cronos/images/
python3 tools/sdat2img.py firmware/cronos/system.transfer.list firmware/cronos/system.new.dat firmware/cronos/images/system.img
debugfs -R "rdump / firmware/cronos/rootfs/system" firmware/cronos/images/system.img   # ownership errors are harmless
python3 scripts/mkbootroot.py --cpio firmware/cronos/images/boot.img firmware/cronos/ramdisk.cpio
(cd firmware/cronos/rootfs && cpio -id < ../ramdisk.cpio)
```
