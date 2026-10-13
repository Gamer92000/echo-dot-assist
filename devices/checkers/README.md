# Echo Show 5 1st gen, 2019 (`checkers`)

**Runs on one Echo Show** (a tester's, from 2026-10-10), installed with the guided setup (`scripts/setup.sh
checkers`). The unlock is public (below); the install is `scripts/install-system.sh`, which hands over to
`scripts/install-boot.sh`. No light ring here: hassmic shows the ring as a band along the bottom of the screen
(setting "Light band"), the rest of the screen stays black for now.
Findings, and what works and what is open: [docs/re-checkers.md](../../docs/re-checkers.md). Porting guide:
[devices/README.md](../README.md).

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
| `boot-root.zip` | the XDA thread (one `boot-root.img`, see "Root" below); the install takes only its `sbin/adbd` (`ADBD` in `device.conf`, sha256 `108ba403…`) | `b2474113a1f3a4a8de6728ff72774761051945641e1a5ffa78ee25c2b19a809e` |
| `magiskpolicy32` | `patch/magiskpolicy32` of donut's `boot-root.zip` ([its README](../donut/README.md)); the guided setup takes it from `firmware/donut/boot-root.zip` (where donut's setup keeps it, and where it asks for it) | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |

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

At every start TWRP asks "Keep System Read only?" on the screen: tap **Keep Read Only**. Nothing of the install writes
`/system` from TWRP (the boot image goes in through fastboot, the files from the running Fire OS), and a system that
TWRP mounted read-write no longer passes the stock boot image's dm-verity, which is the way back while `/system` is
untouched. adb answers while the screen shows; the answer restarts TWRP's USB, so adb drops for a few seconds.

**Never write lk, preloader, tee1/tee2 (tz) or any other bootloader partition**, not even from the USB download mode:
most units have no bootrom way back, a brick there is permanent. An install for this model may touch `system` and
`boot` only. Stock updates only through TWRP: TWRP lists only `.zip`, and the `.bin` is one (`adb push
firmware/checkers/update-kindle-*.bin /sdcard/update.zip`, check the size, 1372825620 for 8149, then `adb shell twrp
install /sdcard/update.zip`; or `adb sideload` it, no rename). A copy cut short (seen over MTP) makes TWRP report the
zip corrupt. Its updater-script also writes lk, tee1/tee2, the preloader and MBR; amonet's TWRP renames `by-name/lk`
and the others to `*_real` at start, so those writes miss; flashing the amonet zip in TWRP updates
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

**Nor is 8149's own `adbd`** (first device, 2026-10-10): it is a user build (no `ro.secure` in it; `adb root` answers
"restarting adbd as root" and it comes back as shell), so `ro.secure=0`, `service.adb.root=1` and `fastboot oem flags
61` all leave adb a shell in `u:r:shell:s0`. The XDA image's `sbin/adbd` (CyanogenMod's, static) goes by `ro.secure`;
with it in 8149's ramdisk adb is root in `u:r:su:s0` (`mkbootroot.py --adbd`).

```sh
python3 scripts/mkbootroot.py --check firmware/checkers/images/boot.img  # rebuilds it exact but for the header ID
python3 scripts/mkbootroot.py firmware/checkers/images/boot.img firmware/checkers/boot-root-8149.img
```

`boot-root-8149.img`: the five properties above and `init.fosflags.sh` (byte for byte the XDA file) replaced inside
8149's ramdisk, every other ramdisk entry and the kernel untouched, Amazon's kernel-signing certificate (a page
trailing the stock image) left out: it names the stock image's sha256 (`URI:sha256://…` in its subject alt name), and
the XDA image has none either. The header ID is AOSP mkbootimg's formula, which lk does not check (the XDA image boots
with a foreign one too). sha256 `e787a08ed7d39471ae4228659879939cb7e5225e82dbdd5e5d47e4dbf2c6a9a7` (with classic zlib;
zlib-ng deflates the same ramdisk differently). **Do not flash it:** its stock policy and adbd leave adb dead or a
shell (above). What the install flashes is the same plus the patched policy, the XDA adbd and `verify` out of the fstab: `scripts/install-boot.sh --boot-only` builds and flashes just that (root adb, for
trying things with `scripts/deploy.sh` before installing).

## Install

Tried on one Echo Show. The guided setup does it (`scripts/setup.sh checkers`): amonet's `fastbrick.sh` run at its
unlock step, 8149's system partition where the Echo runs another build, the rooted boot image at the root step (`scripts/install-boot.sh --boot-only`), the build, the egress
lock and the Wi-Fi join, the install.  Wi-Fi joins through Android's WifiService — `scripts/device/wifictl.dex`
(`tools/mkwifictl.py`), called as `CLASSPATH=…/wifictl.dex app_process / Wifictl <verb>`: the same
`android.net.wifi.WifiConfiguration` stock's own setup apps put in, from root, no touchscreen and no Alexa app.  It
downloads the qemu-arm it needs into `toolchain/` when the PC has none, and takes `magiskpolicy32` out of donut's
`boot-root.zip` (`firmware/donut/`) and `adbd` out of checkers' own `boot-root.zip` on its own (the browser saves the
second as `boot-root (1).zip`; the setup takes either).  The steps written out follow, for doing them one by one
(`scripts/install-boot.sh`, from `scripts/install-system.sh`):

0. **System partition, in TWRP**, where the Echo runs another build than 8149 (`ro.build.version.incremental` of the
   system partition: mount it read-only in TWRP and read its `build.prop`; TWRP's own getprop is TWRP's build). A
   tester's Echo came out of the unlock with 0013054618500. Only `system` is written, from the image on the PC
   (`tools/sdat2img.py` of the firmware's `system.new.dat`, sha256 `SYSTEM_IMG_SHA256` in `device.conf`, exactly the
   partition's 3253731328 bytes), and read back; not with `twrp install` of the `.bin`, whose updater-script also
   writes lk, tee1/tee2, the preloader and an MBR (see the warning above):
   ```sh
   adb exec-in 'dd of=/dev/block/platform/mtk-msdc.0/11230000.MSDC0/by-name/system bs=4194304 && sync' < firmware/checkers/images/system.img
   adb shell sha256sum /dev/block/platform/mtk-msdc.0/11230000.MSDC0/by-name/system   # SYSTEM_IMG_SHA256
   ```
   The boot partition still holds the old build's image, which does not boot this system: go on to step 2 without
   a reboot.
1. **Boot image, on the PC.** `boot.img` out of the firmware `.bin` (checked against `BOOT_SHA256`), its `sepolicy`
   patched with `sepolicy.rules` by magiskpolicy under qemu-arm (the Echo has no root yet; `tools/qrun.sh` against
   `firmware/checkers/rootfs`), and `scripts/mkbootroot.py --sepolicy … --adbd firmware/checkers/adbd --no-verity`:
   root adb, `verify` out of `fstab.mt8163`, and two lines of stock's `init.checkers.rc` out: `setprop
   service.adb.tcp.port 5555` (with root adb and no authentication it opened a root shell to the network at every
   boot) and `start bootsound_anim` (Amazon's boot chime, at the full stream volume a satellite keeps). Reproducible:
   sha256 `03253eed8df8126c9fbad812664e92c87812db973ca6d05fbb940762d688c19d` with classic zlib (zlib-ng packs the same
   ramdisk tighter: then its cpio, `mkbootroot.py --cpio`, has sha256 `6467f26c4b25e0e0323fc195968b30af7343bd833126640e5f228e6ef6aa54d8`).
   The image before that change (`a1772cbe…`) booted with root adb in `su` (first device, 2026-10-10; the tester's
   2026-10-13). An Echo with the older one gets this one at the install (`install-boot.sh` compares the partition).
2. **Flash it.** The Echo's system build is checked first (`ro.build.version.incremental` 0013222532484, from TWRP or
   the booted OS), then `adb reboot bootloader` to amonet's hacked fastboot (or: Volume down while connecting power),
   `fastboot flash boot`, `fastboot continue`. Only `boot`. Skipped when the partition holds that image already.
   Not `fastboot reboot`: `adb reboot bootloader` sets the RTC's boot-to-fastboot flag and kaeru 2.0.0 does not clear
   it (`fastboot getvar boot-reason`: RTC), so every warm reboot lands in fastboot again; `continue` boots `boot`
   straight from fastboot, and pulling the power clears the flag. `fastboot oem logcat` prints the preloader's and
   lk's log.
3. **Files, from the running OS.** Root adb in the `su` domain checked; then as on donut: `/system/hassmic/`,
   `/system/etc/init/hassmic.rc` (`sysinstall.sh`; `/system` is a plain block device without verify, `otatool
   remount` clears its read-only flag if the kernel set it), `hassmic.conf`, reboot.

At boot, `hassmic.rc` starts the firewall at `on boot` and the satellite at `sys.boot_completed`; `alexa-off.sh` disables
Alexa's apps with `pm` (`ALEXA_PACKAGES`, `SETUP_PACKAGES`, `UPDATE_PACKAGES`), noting them in
`/data/local/hassmic/alexa-disabled`. Without `hassmic.conf` `boot.sh` turns exactly those back on.

```sh
# amonet done, the Echo in TWRP (Volume up while connecting power) on USB
unzip -j firmware/donut/boot-root.zip patch/magiskpolicy32 -d firmware/checkers/
unzip -p firmware/checkers/boot-root.zip boot-root.img > firmware/checkers/boot-root.img
python3 scripts/mkbootroot.py --extract firmware/checkers/boot-root.img sbin/adbd firmware/checkers/adbd
DEVICE=checkers scripts/install-system.sh   # the Echo names itself (model and MAC address); name it in Home Assistant
```

`DEVICE=checkers` is needed while it is in TWRP: there the model is not asked from the Echo. Wi-Fi:
`scripts/wifi-join.sh` joins through WifiService on this model (wifictl.dex above; the SSID goes in as hex and the
passphrase as its PSK, made on the PC, so nothing of either passes a shell); it worked on the first Echo Show
(2026-10-13).  There is no screen to fall back on: the install takes Amazon's apps off the system partition, Settings
among them, and the screen stays black (hassmic's own home, `tools/mkhome.py`).  Do not join before the lockdown:
Fire OS would reach Amazon.  The settings page's network switch
(`scripts/device/wifi.sh`) uses the same wifictl calls.

**Once `/system` is written, only a boot image without `verify` may boot it**: the stock `boot.img` would find its
hashes wrong. `scripts/install-system.sh --uninstall` turns Alexa's apps back on and takes hassmic off `/system`, but
keeps the root boot image; all the way back to stock is the firmware `.bin` flashed in TWRP (system and boot together;
amonet's TWRP leaves lk, preloader and tee alone).

## Files here

| File | State |
|---|---|
| `device.mk` | OpenSL ES backend, Pryon, the light band; Wi-Fi motion's module `hassmic_rcpi4m` built for 32-bit ARM (kernel.org 4.9.77, arm-eabi-4.8) |
| `board.c` | from the firmware, checked on the Echo: input devices by name, privacy latch, light sensor (`als_ps/lux`) |
| `device.conf` | identity and probe files checked; `INSTALL=boot` (boot and system partition, firmware build, policy tool), Alexa's packages, `FW_KEEP` (DHCP: no stock `firewall.sh`) |
| `sepolicy.rules`, `hassmic.rc` | the boot image's policy patch; init services (firewall at `on boot`, satellite at `sys.boot_completed`) |
| `kconfig` | the kernel's IKCONFIG (4.9.77, ARM) |
| `probe.md5`, `stubs/` | from the pinned firmware |

What is done and what is open, item by item: [docs/re-checkers.md, "What a port needs"](../../docs/re-checkers.md#what-a-port-needs).

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
