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

`amonet-checkers-v2.0.1.zip`, from the XDA thread for the Echo Show 5 1st gen (2019, H23K37 only: the Echo Show 8
`crown` and the Echo Show 5 2nd gen `cronos` have threads of their own). It ends with an unlocked bootloader and TWRP.
The zip goes into `firmware/checkers/` (not in git); its checksum is to be pinned here once it is.

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
the unlock. Which firmware versions the exploit takes is not said in the post: check the thread before pinning.

## Files here

| File | State |
|---|---|
| `device.mk` | OpenSL ES backend, Pryon, no Wi-Fi motion module (the gen4m driver is 32-bit here; `hassmic_rcpi4m` is arm64) |
| `board.c` | from the firmware; event numbers, privacy input device and light sensor to check on the device |
| `device.conf` | identity and probe files checked; `INSTALL=none`; package lists for the on-device scripts to come |
| `kconfig` | the kernel's IKCONFIG (4.9.77, ARM) |
| `probe.md5`, `stubs/` | from the pinned firmware |

Missing: an install method (TWRP, no A/B slots, dm-verity on `system`, the policy in `boot.img`'s ramdisk), and with it
`hassmic.rc`, `sepolicy.rules`, `setup.sh` and a place in CI's build matrix.

## Unpack the firmware

```sh
unzip firmware/checkers/update-kindle-*.bin -d firmware/checkers/
```

Then `system.new.dat` to `images/system.img` (sdat2img), `debugfs -R "rdump / firmware/checkers/rootfs/system"` on it,
and the `boot.img` ramdisk into `firmware/checkers/rootfs/`: [docs/re-checkers.md](../../docs/re-checkers.md#unpacking).
