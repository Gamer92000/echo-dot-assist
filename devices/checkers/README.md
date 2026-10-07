# Echo Show 5 1st gen, 2019 (`checkers`)

**Not installable: no unlock or root method is known.** What is here builds (`make DEVICE=checkers`, also with
`STUBS=1`) and is worked out from the firmware only; nothing has run on an Echo Show. Findings and what a port still
needs: [docs/re-checkers.md](../../docs/re-checkers.md). Porting guide: [devices/README.md](../README.md).

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

## Files here

| File | State |
|---|---|
| `device.mk` | OpenSL ES backend, Pryon, no Wi-Fi motion module (the gen4m driver is 32-bit here; `hassmic_rcpi4m` is arm64) |
| `board.c` | from the firmware; event numbers, privacy input device and light sensor to check on the device |
| `device.conf` | identity and probe files checked; `INSTALL=none`; package lists for the on-device scripts to come |
| `kconfig` | the kernel's IKCONFIG (4.9.77, ARM) |
| `probe.md5`, `stubs/` | from the pinned firmware |

Missing until there is a way in: `hassmic.rc`, `sepolicy.rules`, `setup.sh`, an install method, a place in CI's build
matrix.

## Unpack the firmware

```sh
unzip firmware/checkers/update-kindle-*.bin -d firmware/checkers/
```

Then `system.new.dat` to `images/system.img` (sdat2img), `debugfs -R "rdump / firmware/checkers/rootfs/system"` on it,
and the `boot.img` ramdisk into `firmware/checkers/rootfs/`: [docs/re-checkers.md](../../docs/re-checkers.md#unpacking).
