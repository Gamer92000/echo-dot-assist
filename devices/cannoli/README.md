# Echo Dot 5th gen, 2022 (`cannoli`)

Model C2N6L4, MediaTek MT8519BAAV (the AZ2), Fire OS 6.5.7.5, build `NS6575/8349` (2026-10-27), `ro.product.device` = `cannoli`.
The port below is hardware-tested, on the snapshot author's unit and on ours (unlocked, brought from its launch firmware
to 8349, set up from a phone and adopted by Home Assistant, 2026-11-14/15; PLAN.md). This README also holds the
boot-chain findings and the from-stock guide.

- **Storage**: NAND (Micron NW942), UBI volumes `boot_a`/`boot_b`, `uboot_a`/`uboot_b`, `system_a`/`system_b`,
  `tee1`/`tee2`, `scp`, `mcupm`, `brhgptlk_0..3`. No GPT, no eMMC; `scripts/otatool.py`'s remount path does not apply.
- **Radio**: MT7653BSN Wi-Fi/BT (eero mesh), two PCB antennas.
- **Audio**: 3 mics through a TI TLV320ADC5140 on the switch PCB, Maxim MAX98396 mono amp (no jack this gen),
  12 RGB LEDs (W4970 driver), temperature sensor, light sensor.
- **USB**: no connector. It comes out on the 3×3 matrix of gold test pads in a recess behind the base's rubber foot
  — see [Wiring](#wiring). **GND is the centre pad of the nine, D− the pad
  straight below GND, D+ the corner pad left of D−**; the [diagram](#wiring) colours the three as the USB wires
  they take. Power stays on the normal 15 V adapter; never bridge 5 V onto these pads.

## Boot chain

| Stage | Where | What |
| --- | --- | --- |
| preloader | `brhgptlk`-adjacent, not in the OTA | exposes a USB serial (0E8D:2000) that accepts a D7 upload / D5 jump of an image to 0x200000 — no exploit needed |
| BL2/LK | `brhgptlk_0..3` | verifies the `uboot` partition FIT (RSA-PSS, `key-fit`, the key sits in an FDT inside BL2 at 0x353E8) |
| U-Boot | `uboot_a`/`uboot_b` | **2019.04-g71f44a240c** (built 2026-04-07), a FIT itself; AArch64, loads 0x44E00000; `mtk_boot` = `ubi read … ${boot_partition}; bootm …#conf-1`; Volume-Up hook is patched in by the builder |
| kernel | `boot_a`/`boot_b` | signed FIT, `sha256,rsa2048`, key `fireos-production`, one config per board rev (`cannoli-hvt`, `cannoli-evt-aluminum`, …, plus `cannoli-*-recovery` with a ramdisk) |

CVE-2026-46728 lives here: U-Boot's `fit_config_check_sig()` reads the `hashed-nodes` hint from the FIT being verified
(attacker-controlled, itself unsigned) — confirmed in the disassembly at 0x44E2FB90 (`fdt_getprop(fit, noffset,
"hashed-nodes", …)`), strings `Can't get hashed-nodes property` / `hashed-nodes property must be null-terminated`.
The BL2→uboot check falls to the same flaw. `cannoli-os-builder`'s `shadow()` weaponizes it: the stock `kernel-1`
node moves into an `archive/` subnode, the signature's `hashed-nodes` is repointed at the archive copy, and the
patched payload replaces the real node — the signed byte stream is unchanged, so **Amazon's stock signature verifies
over a fully patched U-Boot** (proved offline against the real key, see [state](#what-is-prepared-here)).

## The port

Merged here (source identity
`cannoli8349-037875a37199e1c67784b51b`). It keeps the stock kernel (5.4.86), Amazon's mixer and Pryon
wake word, and the DSP firmware; `INSTALL=cannoli-ubi` — the offline builder installs files and patches the policy
into the SquashFS image, and the ext4 installer refuses the device. `/system` gets a writable OverlayFS on `/data`
(`prepare.sh`, started from `hassmic.rc` at post-fs-data; the overlay's upper root is re-labelled so stock non-root
services still execute). The satellite starts from storage/mixer readiness instead of Alexa's launch flag, which
lockdown clears.

Verified on the first unit: normal and full power-cycle boots, root USB ADB, enforcing microphone capture,
Wi-Fi persistence (with a DHCP `INFORM6` fix), encrypted Home Assistant adoption, Music Assistant pairing,
announcement playback; Volume-Up recovery confirmed there.  On ours: the stock ring again after 8349's BL2 (Arrival
day 1b), setup from a phone (Improv), push updates and adb over Wi-Fi from a cold boot, the mute button, the light
sensor against stock's own lux, speaker gain's control (+4..+21 dB, stock +14), the Wi-Fi kept out of power save,
the action and volume buttons, the wake word and echo cancellation in a room, Wi-Fi motion (its kernel module).
The ring's own animations (Home Assistant's light, the turned listening ring, designs from the settings page) in its
binary format, the light drawn through `libhmled.so` in ledcontroller (PLAN.md).  Still open (PLAN.md): Bluetooth
audio, the volume curve (the Dot 2's stands in), and guided
setup (`scripts/setup.sh` support: no `devices/cannoli/setup.sh` yet — the from-stock guide below is the manual
flow).  Rooted stock boots keep upstream automatic updates disabled (the
builder disables OTA autostart).

## Unlock and root

The route is tested end-to-end (rooted slot A, TWRP slot B, Volume-Up recovery), on our unit too (2026-11-14).
It lives in three repositories next to this one:

- **[firebreak](https://github.com/Gamer92000/firebreak)**, the cannoli route: the installer — `docs/cannoli.md`
  there is the step-by-step guide. RAM-boots patched U-Boot over the preloader serial, flashes slot B through that
  fastboot, boots TWRP, backs up A, writes rooted A through UBI with readback verification, and checks the booted
  system (root adb, key auth, SELinux enforcing, USB-only adb, OTA autostart off).
- **[cannoli-os-builder](https://github.com/Gamer92000/cannoli-os-builder)**: the offline bundle builder (stock
  repack + patched loaders; `--port` bakes this port's update bundle in). The OTA it pins is exactly our
  `firmware/cannoli/update-kindle-cannoli-NS6575_user_8349_0013323246980.bin`
  (sha256 `a55e7223…f5996d9`), and the OTA's `tee`/`scp`/`mcupm` images match the installer's on-device gates.
- **[twrp_device_amazon_cannoli](https://github.com/Gamer92000/twrp_device_amazon_cannoli)**: the headless TWRP
  device tree, build patches and recovery packer.

Hold **Volume Up** (the `+` button) at power-on to enter slot-B TWRP once installed. The preloader route stays
available as recovery if A and B are both broken.

## What is prepared here

| Piece | State |
| --- | --- |
| OTA, unpacked partitions, disassembly | `firmware/cannoli/` (git-ignored); `unpack/parts/` from `tools/payload_dump.py` |
| firebreak cannoli route | [firebreak] checkout, `uv sync` done, tests: **1372 passed** (1 pre-existing macOS-only failure, also on main) |
| builder inputs | [cannoli-os-builder] `work/inputs`: images verified against pinned hashes, NDK r21e, `magiskpolicy32`, `adbd`; `adbkey.pub` present |
| TWRP | [twrp_device_amazon_cannoli]: tests pass, **full pinned build done** (`work/twrp`, 26 GB — sync + build ≈ 8 min on 32 cores); packaging rehearsed with placeholder serial `REHEARSAL0001` → `work/recovery-rehearsal/boot_b.img` (13.25 MB ≤ 0xD0B000), signature streams asserted unchanged |
| patched loaders | all five built into `cannoli-os-builder`'s `work/dry` with the rehearsal `boot_b` size (`access`, `select-a/b`, `uboot_a`, `uboot_b`): each carries a **valid stock Amazon signature** over patched code |
| host tools | `fastboot`, `adb`, `repo`, `rsync` 3.4.1 (`~/.local`) present; **`squashfs-tools` missing** (`sudo pacman -S squashfs-tools`), udev rules not installed, bundle build needs sudo (password) |

## What is still missing

1. **Port follow-ups** (PLAN.md): guided setup (the installer exists in [firebreak]; the setup glue here does
   not), the volume curve (the Dot 2's stands
   in), and the open hardware items above.  Done: `STUBS=1` stubs (all seven binaries byte-identical between
   firmware and stub link), the CI build-matrix entry, the version from the commit as on every model (`version.py`
   gives only the build's source identity; a fixed date only where git gives none).
2. **Firmware ZIPs** only when the entry points change (our unit has this version's, flashed 2026-11-15).  `prepare.sh`: init runs the copy flashed into the SquashFS before the
   overlay is mounted; that copy runs the one the last update left in the overlay's upper folder
   (`/data/hassmic-system/upper/hassmic/prepare.sh`, with `steps`), so its steps, the mount included, come with
   updates.  It falls back to its own steps when there is none, it is not trusted (folder not root's 0700, a link,
   the file not root's alone, no `# hassmic-prepare-api 1` line, does not parse), fails, hangs, or leaves `/system`
   without the overlay; the
   property that starts the satellite is set either way.  Limits: the fallback's `lockdown.sh` is the update's (the
   SquashFS copy lies under the overlay), and an update's steps first run at the boot after its promotion.
   Rollback: a promotion overwrites the copy in the overlay, so the copy it replaces is kept
   (`/data/hassmic-system/previous`, only one that was reached from the network).  After two boots in a row that
   nobody on the network reached the Echo on (Home Assistant, the settings page, Music Assistant, a push update; a boot
   before it is set up or without a network address does not count, with a version that was reached before), the
   flashed `prepare.sh` steps back, one step
   per two such boots: an update not promoted yet is dropped, else the factory copy goes back to `previous`, else to
   the copy flashed into the SquashFS.  Each version taken out is never installed again (`ota/refused`).  A broken
   update means pulling the plug a few times, not opening the Echo.  Home Assistant, the page and Music Assistant all
   away for two boots step back too.
3. **Units flashed before the builder's `--port`**: their SquashFS copy lacks `VERSION`, `libhmled.so`,
   `hassmic_rcpi4m.ko` and `latency`, so the last rollback step runs there without Home Assistant's light ring and
   Wi-Fi motion, and without a version to show.  [cannoli-os-builder]'s `--port` now bakes every file of the
   port's bundle in, `VERSION` and `update.pub` included: a reflash with it closes this.

## From stock to a Home Assistant satellite

Everything between a stock Dot 5 and an adopted satellite, in order — the port is baked into the image, so the
Echo comes up as a satellite instead of rooted stock. [firebreak's cannoli guide][firebreak-guide] tells the same
story at full length; this is the working path with this repo in the loop. Work on an x86-64 Linux host with
~50 GB free (the pinned TWRP tree alone is 26 GB), and **never let the Echo onto Wi-Fi before the route is
done** — an Amazon update moves it off build 8349, which everything here is pinned against.

### Prerequisites

- A T8 Torx screwdriver, a fine-tip soldering iron, flux, thin wire (26–30 AWG), and a USB cable to sacrifice
  or a USB breakout board — only its data and ground wires are used
- Host: `adb`/`fastboot` (platform-tools), `uv`, `squashfs-tools`, Python 3.10+, and `qemu-user` (or a static
  `qemu-arm` to hand the builder)
- Four checkouts side by side: this one, [firebreak], [cannoli-os-builder], [twrp_device_amazon_cannoli]
- Once, from the firebreak checkout: `uv sync`, then
  `sudo install -m 644 contrib/73-cannoli.rules /etc/udev/rules.d/` with
  `sudo udevadm control --reload-rules && sudo udevadm trigger`, and
  `sudo usermod -aG plugdev,dialout "$USER"` — log out and back in

### Wiring

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../../docs/img/cannoli-pads-dark.svg">
  <img src="../../docs/img/cannoli-pads-light.svg" alt="Wiring diagram, the board as printed with FLEX SIDE at the top: the 3×3 test-pad matrix in a silkscreen outline between the two large gold screw holes, a round pad above it. GND on the centre pad (cross-hatched, as the pad right of it); D− straight below GND, D+ the corner pad left of D−. The three pads are coloured as the USB wires they take; the +5 V wire stays unconnected.">
</picture>

Peel the rubber foot off the base with a thin knife or spudger: the pad matrix sits in a recess right behind it.
Undo the four T8 Torx screws underneath for good access to the board. Hold the board as the
diagram draws it — `FLEX SIDE` printed at the top, a round pad just above the matrix, the matrix between the two
large gold screw holes, its centre pad and the one right of it cross-hatched — and solder three wires:

| Pad | Where | USB wire |
| --- | --- | --- |
| GND | centre pad of the nine | black (or shield) |
| D− | straight below GND | white |
| D+ | corner pad left of D− | green |

Leave the cable's +5 V (red) wire unconnected and insulated: the Dot keeps running on its own 15 V adapter, and
5 V on these pads can damage the board. Insulate the joints, route the wires out through the recess, put the screws
back in and stick the foot loosely back on.

### The USB serial

Hold the **action button** (the circle) while powering on → fastboot; `fastboot devices` shows the **USB
serial**. Write it down — it names the recovery packaging, the OS bundle and the install.

### Build this port's update bundle

In this checkout:

```sh
DEVICE=cannoli make all
mkdir -p secrets
[ -f secrets/update.key ] || python3 scripts/otatool.py keygen secrets/update.key secrets/update.pub
DEVICE=cannoli scripts/bundle.sh secrets/update.key "$(DEVICE=cannoli make -s version)"
```

Back `secrets/update.key` up somewhere safe: it signs every future update and opens adb over Wi-Fi. Build this
bundle right before the firmware bundle — a stale `build/cannoli/hassmic.bundle` bakes a stale port, and the
builder cannot tell.

### Build recovery and the OS bundle

In the TWRP checkout — inputs and pinned build per its README — package the recovery for your serial:

```sh
python3 -m cannoli_twrp.pack --images ../cannoli-os-builder/work/inputs/images \
  --ramdisk work/twrp/out/target/product/cannoli/ramdisk-recovery.img \
  --graphics work/twrp/out/target/product/cannoli/system/lib/libminuitwrp.so \
  --serial <serial> --output work/recovery
```

In the OS-builder checkout, with its inputs prepared, assemble the bundle with the port baked in:

```sh
adb start-server   # ~/.android/adbkey.pub must exist; the bundle embeds it
sudo python3 -m cannoli_os.build \
  --images work/inputs/images --recovery ../twrp_device_amazon_cannoli/work/recovery \
  --adbd work/inputs/adbd --policy work/inputs/magiskpolicy32 \
  --ndk work/inputs/android-ndk-r21e --adb-key "$HOME/.android/adbkey.pub" \
  --serial <serial> --port /absolute/path/to/echo-dot-assist \
  --output work/bundle
```

`--port` is what makes the image boot as a satellite instead of plain rooted stock. Add
`--qemu /path/to/qemu-arm` when the host's qemu-user is not visible to sudo.

### Unlock, root, install

```sh
uv run firebreak unlock --device cannoli \
  --bundle ../cannoli-os-builder/work/bundle --serial <serial> \
  --backup backups/cannoli-original
```

Follow the power prompts; the listener opens before each power-on to catch the BL2 USB window (~45 s). `--check`
first if you want a no-write pass over an already-installed recovery. A failed slot-A write resumes by rerunning
with the **same bundle and backup directory** — never delete the only completed backup to work around an error.
The backup does **not** keep the original slot-B images or boot selection: keep the bundle and the wires
soldered — the preloader route is the unbrick path. Slot A alone goes back with
`uv run firebreak restore backups/cannoli-original --device cannoli --bundle … --serial …`.

### Factory-fresh units (2022 launch firmware)

- **Factory-fresh units ship the 2022 launch firmware**, and firebreak's prerequisite gate fails on them
   (`Unsupported firmware prerequisite: mcupm_a`; every protected volume bigger than the 8349 set, FIT
   timestamps Oct/Nov 2022). Fix, from TWRP: write the 8349 stock `tee1`, `tee2`, `scp_a`/`scp_b`,
   `mcupm_a`/`mcupm_b`, `uboot_a`, `boot_a`, `system_a` volumes with `Recovery.write` (upgrade direction,
   anti-rollback-safe; LK untouched; the preloader route stays available). First boot with the 8349 TEE raises
   the anti-rollback eFuse, as any Amazon OTA would. Then preflight passes and the unlock proceeds.
- **Those units also keep their 2022 launch BL2, which starves the 8349 SCP**: the ring's LED manager lives
   in the SCP firmware, and the launch `brhgptlk` never brings it up (no `SCP is ready`, ledcontroller's HAL
   fails, the kernel's `boot-anim` LED trigger swirls for good — PLAN.md, 2026-11-14/15). Fix, from TWRP
   (Volume Up at power-on): the four `brhgptlk_*` are **raw MTD partitions**, not UBI — `dump_image
   brhgptlk_N` first (launch copies kept in `firmware/cannoli/lk-launch-backup/`), then `flash_image
   brhgptlk_N` with the OTA's `brhgptlk_0.img`, readback (`dump_image` + `sha256sum`) before the next copy.
   The shadowed U-Boot passes 8349 BL2's FIT checks (as on the first unit); the SCP then answers and the stock
   animations are ledcontroller's again. Skip this only if a permanently dark ring is acceptable — main.sh
   `leds()` keeps the kernel swirl off either way.

### After the install

The Echo boots straight into the port. Set it up from your phone (Improv) and adopt it in Home Assistant, as
the other models do. The guided setup does not cover cannoli yet — `scripts/setup.sh` stops with a pointer to
this README; updates go the pushed-bundle way: build a fresh bundle (above) and `scripts/ota-push.sh` it over
Wi-Fi, or pick a channel on the settings page. Volume Up at power-on enters slot-B TWRP; do not use TWRP's
generic wipe/format/ZIP interface on cannoli's NAND — only this route's own commands.

[firebreak]: https://github.com/Gamer92000/firebreak
[firebreak-guide]: https://github.com/Gamer92000/firebreak/blob/main/docs/cannoli.md
[cannoli-os-builder]: https://github.com/Gamer92000/cannoli-os-builder
[twrp_device_amazon_cannoli]: https://github.com/Gamer92000/twrp_device_amazon_cannoli
