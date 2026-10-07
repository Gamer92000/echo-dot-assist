# Echo Show 5 1st gen, 2019 (`checkers`)

**Not installable yet.** The unlock is public (below), but the installation for this model is still to be written.
What is here builds (`make DEVICE=checkers`, also with `STUBS=1`) and is worked out from the firmware only; nothing has
run on an Echo Show. Findings and what a port still needs: [docs/re-checkers.md](../../docs/re-checkers.md). Porting
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
| `boot-root.zip` | the XDA thread (one `boot-root.img`, see "Root" below) | `b2474113a1f3a4a8de6728ff72774761051945641e1a5ffa78ee25c2b19a809e` |

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

The better way, built: the same changes applied to 8149's own `boot.img`, so kernel, ramdisk and `/system` stay one
build.

```sh
python3 scripts/mkbootroot.py --check firmware/checkers/images/boot.img  # rebuilds it exact but for the header ID
python3 scripts/mkbootroot.py firmware/checkers/images/boot.img firmware/checkers/boot-root-8149.img
```

`boot-root-8149.img`: the five properties above and `init.fosflags.sh` (byte for byte the XDA file) replaced inside
8149's ramdisk, every other ramdisk entry and the kernel untouched, Amazon's kernel-signing certificate (a page
trailing the stock image) carried over; the header ID is AOSP mkbootimg's formula, which lk does not check (the XDA
image boots with a foreign one too). sha256 `afca0a33e1bede3e9e9d749820bea4f67f11ac8db77e1636075478e86f8c7c89`,
8140800 bytes. Flashed the same way (`fastboot flash boot`); not tried on a device. Still open with the install: the
policy in `boot.img`'s ramdisk patched for our services (from 8149's `/sepolicy`, as on donut).

## Files here

| File | State |
|---|---|
| `device.mk` | OpenSL ES backend, Pryon, no Wi-Fi motion module (the gen4m driver is 32-bit here; `hassmic_rcpi4m` is arm64) |
| `board.c` | from the firmware; event numbers, privacy input device and light sensor to check on the device |
| `device.conf` | identity and probe files checked; `INSTALL=none`; package lists for the on-device scripts to come |
| `kconfig` | the kernel's IKCONFIG (4.9.77, ARM) |
| `probe.md5`, `stubs/` | from the pinned firmware |

Missing: an install method (TWRP or hacked fastboot, no A/B slots, dm-verity on `system`, the policy in `boot.img`'s
ramdisk patched for our services), and with it `hassmic.rc`, `sepolicy.rules`, `setup.sh` and a place in CI's build
matrix.

## Unpack the firmware

```sh
unzip firmware/checkers/update-kindle-*.bin -d firmware/checkers/
```

Then `system.new.dat` to `images/system.img` (sdat2img), `debugfs -R "rdump / firmware/checkers/rootfs/system"` on it,
and the `boot.img` ramdisk into `firmware/checkers/rootfs/`: [docs/re-checkers.md](../../docs/re-checkers.md#unpacking).
