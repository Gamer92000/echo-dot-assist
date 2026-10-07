# Echo Show 5 1st gen, 2019 (`checkers`)

**Untested: nothing here has run on an Echo Show yet.** The unlock is public (below); the install is written
(`scripts/install-system.sh`, which hands over to `scripts/install-boot.sh`) and its PC half checked, the device half
not. What is here builds (`make DEVICE=checkers`, also with `STUBS=1`) and is worked out from the firmware only. Findings and what a port still needs: [docs/re-checkers.md](../../docs/re-checkers.md). Porting
guide: [devices/README.md](../README.md).

Model H23K37 (`ro.product.model` AEOCH). MediaTek MT8163, 5.5" touchscreen, 2 microphones, camera with a shutter.

## What the firmware says (2026-10-07)

Pinned firmware: `update-kindle-checkers-NS65741_user_8149_0013222532484.bin`, Fire OS 6574.1 (NS65741/8149), the same
release as donut's; sha256 `f96fcfa1240f809711fcf855ce7742762437e4f37546153153635c096773cfa3`. Against `donut`:

- `lib/libpryon.so`, `libopus.so`, `libz.so`: **byte-identical.** The `en-US/ALEXA` model (a newer layout) detects
  ALEXA in `pryon_test` under qemu.
- **No `mixer`, no `libmixerAPI.so`.** Amazon's front end runs inside the audio HAL; hassmic uses Android's audio
  through OpenSL ES (`src/hassmic/audio_android.c`).
- A full Android device: Alexa is apps (stopped with `pm disable`), Wi-Fi is Android's, netd owns iptables, Bluetooth
  is Android's stack. `scripts/system/` and `scripts/device/` do not handle that yet.
- No action button, no LED ring; a screen.

## Unlock (public: amonet-checkers)

XDA thread with the full instructions (R0rt1z2): [UNLOCK][ROOT][TWRP][UNBRICK] Amazon Echo Show 5 1st gen (2019)
(checkers) <https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-5-1st-gen-2019-checkers.4762900/#post-90318137>.
H23K37 only: the Echo Show 8 `crown` and the Echo Show 5 2nd gen `cronos` have threads of their own. It ends with an
unlocked bootloader and TWRP (written to `recovery` and `swdl`). These files go into `firmware/checkers/` (not in git):

| File | Source | sha256 |
|---|---|---|
| `amonet-checkers-v2.0.1.zip` | the XDA thread | `770324a8ed5ab922c0383f8ba072d70fc0190cc2c879f12f67b8d6cfa3ad30ee` |
| `update-kindle-checkers-NS65741_user_8149_0013222532484.bin` | Amazon | `f96fcfa1240f809711fcf855ce7742762437e4f37546153153635c096773cfa3` |
| `boot-root.zip` | the XDA thread (one `boot-root.img`, see "Root" below; not used by the install) | `b2474113a1f3a4a8de6728ff72774761051945641e1a5ffa78ee25c2b19a809e` |
| `magiskpolicy32` | `patch/magiskpolicy32` of donut's `boot-root.zip` ([its README](../donut/README.md)) | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |

The post names no firmware version. The pinned one fits: the `lk.bin` and `tz.img` inside amonet-checkers v2.0.1 are
byte-identical to those of 8149 (only its `preloader.img` differs).

- **Option 1, a working Echo:** no opening. `./fastbrick.sh` (Linux) or `fastbrick.bat`; with the Echo on its power
  supply, hold all three buttons until "=> FASTBOOT mode..." shows, then connect micro-USB. Type `YES`, follow the
  screen. **After the 10 s grace period nothing may interrupt it** (up to 5 min): an interruption bricks it for good.
- **Option 2, a bricked Echo made in 2019:** open it (power and USB flex stay connected), `sudo ./bootrom-step.sh`, plug
  in micro-USB while shorting test point TP30 to ground, until the script says otherwise; then `sudo
  ./fastboot-step.sh`. Newer revisions have the bootrom's USB download disabled, so this most likely fails on them.
  ModemManager must be stopped.

Afterwards, with the volume keys held while power is connected: Volume down = hacked fastboot, Volume up = TWRP,
Mute + Volume down (USB connected) = preloader USB download for MTKClient (shows as "MT8163 Preloader", maker
"PWNED", connecting and disconnecting). `boot-recovery.sh` / `boot-fastboot.sh` from the zip do the same over USB.

**Never write lk, preloader, tee1/tee2 (tz) or any other bootloader partition**, not even from the USB download mode:
most units have no bootrom way back, a brick there is permanent. An install for this model may touch `system` and
`boot` only. Stock updates only through TWRP (the `.bin` renamed to `.zip`); flashing the amonet zip in TWRP updates
the unlock.

## Root

The thread's way: in hacked fastboot (Volume down, or Mute, while booting; or from TWRP) `fastboot oem flags 61` (FOS
flags: adb on, adb authentication off), `fastboot flash boot boot-root.img`, `fastboot reboot`. adb is then a root
shell, and `adb remount` makes `/system` writable.

**`boot-root.img` is not built from the pinned firmware.** Against the 8149 `boot.img` (the kernel with MediaTek's
512-byte header in both; 8149's ramdisk a bare gzip stream, the XDA image's with a `ROOTFS` header too):

- its kernel and ramdisk come from **NS6570 / 6086** (`ro.bootimage.build.fingerprint`, `selinux_version`; kernel
  `4.9.77-g1f91447-dirty`, 2025-09-23, where 8149's is `4.9.77-ga3ad36a2995d-dirty`, 2026-09-28);
- `default.prop`: `ro.secure=0`, `ro.adb.secure=0`, `ro.debuggable=1`, `persist.sys.usb.config=mtp,adb`;
- `init.fosflags.sh` cut down to setting adb on, without authentication;
- `sepolicy`, `file_contexts.bin`, `seapp_contexts`, `service_contexts`, `init` and `sbin/*` differ (6570's or
  patched: not separated yet); `init.whad_cc.rc` lacks 8149's `net_raw` group; `fstab.mt8163` is the same (`/system`
  `wait,verify`).

So it runs 6570's kernel and policy under 8149's `/system`. The modules on `/system/lib/modules` say only
`vermagic=4.9.77` and the kernel has no MODVERSIONS, so they load; whether 8149's Wi-Fi and Bluetooth drivers work
against 6570's kernel, and whether 6570's policy covers 8149's services, is not known. A bad boot is recoverable:
hacked fastboot stays, and 8149's `boot.img` flashes back.

The better way: the same changes applied to 8149's own `boot.img`, so kernel, ramdisk and `/system` stay one
build. **The changes alone are not enough there:** with `ro.secure=0` adbd switches itself to its `--root_seclabel
u:r:su:s0` (`init.usb.rc`) and, in AOSP 7, stops with a fatal error when that is refused. 8149's policy has `su` as a
permissive domain but no rule that lets adbd into it; the XDA image gets past that with 6570's policy, which makes adbd
(and `untrusted_app`: any installed app) permissive. So 8149's image also needs its policy patched
(`sepolicy.rules`: donut boot-root's rules for adb, our services' rules; not `untrusted_app`).

```sh
python3 scripts/mkbootroot.py --check firmware/checkers/images/boot.img  # rebuilds it exact but for the header ID
python3 scripts/mkbootroot.py firmware/checkers/images/boot.img firmware/checkers/boot-root-8149.img
```

`boot-root-8149.img`: the five properties above and `init.fosflags.sh` (byte for byte the XDA file) replaced inside
8149's ramdisk, every other ramdisk entry and the kernel untouched, Amazon's kernel-signing certificate (a page
trailing the stock image) carried over; the header ID is AOSP mkbootimg's formula, which lk does not check (the XDA
image boots with a foreign one too). sha256 `afca0a33e1bede3e9e9d749820bea4f67f11ac8db77e1636075478e86f8c7c89`.
**Do not flash it:** its stock policy leaves adb dead (above). What the install flashes is the same plus the patched
policy and `verify` out of the fstab: `scripts/install-boot.sh --boot-only` builds and flashes just that (root adb, for
trying things with `scripts/deploy.sh` before installing).

## Install

Untested on a device. What it does (`scripts/install-boot.sh`, from `scripts/install-system.sh`):

1. **Boot image, on the PC.** `boot.img` out of the firmware `.bin` (checked against `BOOT_SHA256`), its `sepolicy`
   patched with `sepolicy.rules` by magiskpolicy under qemu-arm (the Echo has no root yet; `tools/qrun.sh` against
   `firmware/checkers/rootfs`), and `scripts/mkbootroot.py --sepolicy … --no-verity`: root adb, `verify` out of
   `fstab.mt8163`. Reproducible: sha256 `551811776d92aa0e851b677077b5d86b1826f6161bb32c76acd97798af9fa7e2`.
2. **Flash it.** The Echo's system build is checked first (`ro.build.version.incremental` 0013222532484, from TWRP or
   the booted OS), then `adb reboot bootloader` to amonet's hacked fastboot (or: Volume down while connecting power),
   `fastboot flash boot`, `fastboot reboot`. Only `boot`. Skipped when the partition holds that image already.
3. **Files, from the running OS.** Root adb in the `su` domain checked; then as on donut: `/system/hassmic/`,
   `/system/etc/init/hassmic.rc` (`sysinstall.sh`; `/system` is a plain block device without verify, `otatool
   remount` clears its read-only flag if the kernel set it), `hassmic.conf`, reboot.

At boot, `hassmic.rc` starts the firewall at `on boot` and the satellite at `sys.boot_completed`; `alexa-off.sh` disables
Alexa's apps with `pm` (`ALEXA_PACKAGES`, `SETUP_PACKAGES`, `UPDATE_PACKAGES`), noting them in
`/data/local/hassmic/alexa-disabled`. Without `hassmic.conf` `boot.sh` turns exactly those back on.

```sh
# amonet done, the Echo in TWRP (Volume up while connecting power) on USB
unzip -j firmware/donut/boot-root.zip patch/magiskpolicy32 -d firmware/checkers/
DEVICE=checkers scripts/install-system.sh "Kitchen"
```

`DEVICE=checkers` is needed while it is in TWRP: there the model is not asked from the Echo. Wi-Fi: not through
`scripts/wifi-join.sh` (that drives wpa_supplicant directly; here Android's WifiService owns it). Join on the screen
once installed (the egress lock is up from `on boot`): `adb shell am start -a android.settings.WIFI_SETTINGS`, if
Amazon's settings app takes that intent; not tried. Do not join before the install: Fire OS would reach Amazon.

**Once `/system` is written, only a boot image without `verify` may boot it**: the stock `boot.img` would find its
hashes wrong. `scripts/install-system.sh --uninstall` turns Alexa's apps back on and takes hassmic off `/system`, but
keeps the root boot image; all the way back to stock is the firmware `.bin` flashed in TWRP (system and boot together;
amonet's TWRP leaves lk, preloader and tee alone).

## Files here

| File | State |
|---|---|
| `device.mk` | OpenSL ES backend, Pryon, no Wi-Fi motion module (the gen4m driver is 32-bit here; `hassmic_rcpi4m` is arm64) |
| `board.c` | from the firmware; event numbers, privacy input device and light sensor to check on the device |
| `device.conf` | identity and probe files checked; `INSTALL=boot` (boot and system partition, firmware build, policy tool), Alexa's packages, `FW_KEEP` (DHCP: no stock `firewall.sh`) |
| `sepolicy.rules`, `hassmic.rc` | the boot image's policy patch; init services (firewall at `on boot`, satellite at `sys.boot_completed`) |
| `kconfig` | the kernel's IKCONFIG (4.9.77, ARM) |
| `probe.md5`, `stubs/` | from the pinned firmware |

Missing: a run on a device; `setup.sh` (the guided setup needs a Wi-Fi join through Android); a place in CI's build
matrix; the settings page's Wi-Fi switch (`wifi.sh` drives wpa_supplicant; here WifiService owns it).

## Unpack the firmware

```sh
unzip firmware/checkers/update-kindle-*.bin -d firmware/checkers/
```

```sh
mkdir -p firmware/checkers/images firmware/checkers/rootfs/system
mv firmware/checkers/boot.img firmware/checkers/images/
python3 tools/sdat2img.py firmware/checkers/system.transfer.list firmware/checkers/system.new.dat firmware/checkers/images/system.img
debugfs -R "rdump / firmware/checkers/rootfs/system" firmware/checkers/images/system.img
python3 scripts/mkbootroot.py --cpio firmware/checkers/images/boot.img firmware/checkers/ramdisk.cpio
(cd firmware/checkers/rootfs && cpio -id < ../ramdisk.cpio)
```

More in [docs/re-checkers.md](../../docs/re-checkers.md#unpacking).
