# Echo Show 8 2nd gen, 2021 (`athena`)

**Installed and running on one Echo Show 8 2nd gen** (2026-10-14 to 2026-10-17; "What works and what is open" below,
[docs/re-athena.md](../../docs/re-athena.md)). It is the first model on Android 9 (Fire OS 7): not the platform of
`checkers` and `crown`, so the install is its own: no boot image of ours, nothing flashed from this PC (TWRP only
flashes the unlock's root zip). Porting guide: [devices/README.md](../README.md).

Model A8H3N2 (the label; `ro.product.model` AEOAT), MediaTek MT8183 (8 cores, 2 GB), 8" touchscreen (1280 x 800 landscape), microphones
behind Amazon's front end (`libasp`, in the audio HAL), camera with a shutter, volume keys, mic mute (a privacy latch,
as the Dots'). No action button, no LED ring: hassmic draws the light ring as a band along the bottom of the screen
(setting "Light band"). Kernel 4.4.146, 32-bit userland, Treble (`/vendor` is a partition), system as root, not A/B.

## Firmware

Pinned: `update-kindle-athena-PS75741_user_7080_0036274153604.bin`, Fire OS 7574.1 (PS75741.7080N, build 7080), from
[FTVDB](https://ftvdb.com/echo/firmware/com.amazon.athena.android.os/84ffb126664e1a3569404ba61d580e05-36274153604-fire-os-7574-1-ps75741-7080-2026-09-29/),
sha256 `7a76fe6f…c422a1a` (full value in `device.conf`). `libpryon`, `libasp`, the audio HAL, `libaspclient`,
`libaudioflinger` and the wake word service are byte-identical in build 7077 (checked), but the build is pinned to the
one the first Echo ran: `scripts/install-system.sh` stops on any other `ro.build.display.id` than PS75741.7080N. Put the
`.bin` into `firmware/athena/` (not in git).

## Unlock and root (public: saiyaku-athena)

saiyaku-athena, a serial-port exploit for the MT8183 and not this project's work, unlocks it; [`unlock.md`](unlock.md)
has its steps, and the guided setup (`scripts/setup.sh athena`, below) runs them for you. Files into `firmware/athena/`
(not in git):

| File | Source | sha256 |
|---|---|---|
| `saiyaku-athena-v1.0.0.zip` | [the XDA thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-8-2nd-gen-2021-athena.4804958/) | `6d48b4b253fa6cbecb9166c87c0177be09c8a02e5f2efd2bc0d0aec939626218` |
| `boot-root.zip` | the XDA thread; unzipped into `firmware/athena/boot-root/`, its `patch/magiskpolicy32` is the tool the install patches the policy with, on the Echo | `741d904f7e2616c70be2c10f3466efb515769632779546a869f5591a6a9c827a` |

`boot-root.zip`, flashed in TWRP, makes adb root *by patching the running system in place*: `ro.secure=0`, a patched
`adbd`, `su` and `adbd` permissive in the SELinux policy it rewrites in `/vendor/etc/selinux/precompiled_sepolicy`, OTA
blocked (`otacerts.zip` emptied, `softwareupdates.amazon.com` pointed at localhost). dm-verity is off
(`ro.boot.veritymode=disabled`).

## Install

The guided setup: `scripts/setup.sh athena` (`--dry-run` walks the steps without running any). By hand, with the Echo
rooted and on adb:

1. **Unpack the firmware** (below), build: `make DEVICE=athena`.
2. **Lock down, switch Alexa off**: `scripts/deploy.sh`, then on the Echo `sh /data/local/hassmic/lockdown.sh` and
   `sh /data/local/hassmic/alexa-off.sh`.
3. **Install**: `scripts/install-system.sh` (`INSTALL=running`). It writes `/system/hassmic/`,
   `/system/etc/init/hassmic.rc`, takes the Amazon apps of `STOCK_APPS_OUT` off the partition (kept in
   `/system/hassmic-removed`), adds hassmic's home screen, and extends the policy the unlock left in
   `/vendor/etc/selinux/precompiled_sepolicy` with `sepolicy.rules` (the old copy stays beside it as
   `.pre-hassmic`). The Echo reboots into the satellite: hassmic is up ~22 s after the kernel starts.

Updating later: `scripts/ota-push.sh <echo-ip>` (`DEVICE=athena`). `scripts/install-system.sh --uninstall` puts the apps
and the policy back and takes hassmic off `/system`; all the way back is the firmware `.bin` in TWRP. The Echo is on a
network from the unlock; the guided setup joins one only when it has none (`scripts/wifi-join.sh`). The settings page's
"Switch network" runs the same `wifi.sh` (below).

## What works and what is open

Seen on the Echo (2026-10-14 to 2026-10-17): boot into hassmic, the firewall under the patched policy, the satellite's
self test (OpenSL ES capture, Pryon's "Alexa"), Home Assistant (adoption, the wake sound, questions answered with
spoken replies), volume keys and the mute latch, Bluetooth (proxy, phone audio) once Android's HIDL Bluetooth HAL is
stopped, the light band (SurfaceControl.Builder), Amazon's UI gone with the apps, hassmic's home screen, Music Assistant
over Sendspin, Wi-Fi (the setup's join and the settings page's "Switch network"), "Illuminance", "Speaker gain",
"Download from Amazon" (the attestation through `fireosdhaservice`, version 2 as crown), Wi-Fi motion, push updates.
**Open**: the lux under a bright lamp held close; not confirmed on this Echo: Improv, the factory reset, `--uninstall`
and the update check. Details and measurements: [docs/re-athena.md](../../docs/re-athena.md).

**Mute button.** Amazon's `gpio-privacy` driver changes its state 1.2-1.3 s after a press that mutes (0.3 s unmuting;
`enable-gpio-toggle-duration` 1000 ms in the device tree), while the red light under the button follows the press.
hassmic reads the button's own input device (`gpio-privacy-button`, `board.privacy_button`) and shows the press at
once; the latch confirms it within 2 s, or the press is taken back.

**Speaker.** The amplifier's volume is numid 84 of card 0 (83 in tinymix's list), "Master TAS5805m Playback Volume",
0..158, 100 at boot, no dB table in the driver. Phone and Sendspin music at full volume is quiet at 100. Listened to with
an Icecast stream through Sendspin (2026-10-15): 115, 120 and 122 clean, 130 distorted. The setting `speaker_gain` (page
only) therefore stops at 118 (`board.speaker_gain_limit`; a bass-heavy rap track clipped at 122): 2 steps per dB (the
chip's 0.5 dB, assumed) = +9 dB.

**Wi-Fi.** Android's framework owns the networks (as on `checkers`: no `wpa_supplicant` to drive, DHCP by the framework),
so the settings page's "Switch network" and the setup's join go through `scripts/device/wifictl.dex`, the framework's own
`IWifiManager`. Android 9's takes the caller's package name in nearly every call, hides the SSID and the scan from any
uid that is not a system app's, and hands lists out as `ParceledListSlice`; the dex notices `ro.build.version.sdk` 28 and
asks the Android 9 way, and `device.conf` `WIFICTL_USER=system` has `wifi.sh` run it as uid 1000 (package `"android"`).
Measured 2026-10-14: status, list, keys, scan and scan results, a join of the network it was on (new lease, saved), and a
join of a network that does not exist (times out, removes the entry, back on the first one).

**Light sensor.** The TCS3400's driver leaves its own `als_lux` at 0 (no calibration table), so hassmic computes the lux
from the raw channels the way stock's virtual sensor does, within ~10 % of its value in steady light; Home Assistant gets an
"Illuminance" sensor. Under a lamp held close it jumped between ~100 and 66000 lx: open.

**Sockets.** Android 9's libc has netd mark every socket in `connect()` and `accept()`, and the unlock's policy did not
let netd at sockets of the `su` domain, which hassmic runs in: both failed with EBADF. The install's `sepolicy.rules`
allows it. See `docs/re-athena.md`.

## Files here

| File | State |
|---|---|
| `device.mk` | OpenSL ES, Pryon, light band, `hassmic_rcpi4m` for 32-bit ARM (kernel 4.4.146) |
| `board.c` | gpio-keys, `gpio-privacy` latch and its button, TAS5805m (speaker gain up to 118 = +9 dB), TCS3400 lux, sounds out of the apps' APKs incl. the wake sound |
| `device.conf` | build 7080 pins, `INSTALL=running`, `SEPOLICY_FILE` in `/vendor`, `WIFICTL_USER`, Alexa's packages, `STOCK_APPS_OUT` |
| `sepolicy.rules`, `hassmic.rc` | init into `su`, netd may mark su's sockets; the two init services |
| `kconfig` | the Echo's own `/proc/config.gz` (4.4.146, ARM), stack protector off, LOCALVERSION `+` |
| `Module.symvers` | the kernel's CRCs of what the module uses (`CONFIG_MODVERSIONS`), read out of boot.img's Image |
| `probe.md5`, `stubs/` | from the pinned firmware |
| `setup.sh` | the guided steps, unlock (saiyaku's `meta-step.sh`) and root (boot-root in TWRP) included |
| `unlock.md` | the unlock and root by hand (saiyaku-athena) |

## Unpack the firmware

```sh
mkdir -p firmware/athena/ota firmware/athena/images firmware/athena/rootfs
unzip firmware/athena/update-kindle-athena-PS75741_user_7080_*.bin system.new.dat.br system.transfer.list \
      vendor.new.dat.br vendor.transfer.list boot.img -d firmware/athena/ota
for p in system vendor; do
  brotli -d firmware/athena/ota/$p.new.dat.br -o firmware/athena/ota/$p.new.dat
  python3 tools/sdat2img.py firmware/athena/ota/$p.transfer.list firmware/athena/ota/$p.new.dat firmware/athena/images/$p.img
done
debugfs -R "rdump /system firmware/athena/rootfs" firmware/athena/images/system.img
rm firmware/athena/rootfs/system/vendor; mkdir firmware/athena/rootfs/system/vendor     # a link to /vendor
debugfs -R "rdump / firmware/athena/rootfs/system/vendor" firmware/athena/images/vendor.img
```
