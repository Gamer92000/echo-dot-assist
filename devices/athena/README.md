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
`libaudioflinger` and the wake word service are byte-identical in builds 7077 and 7082 (PS75741.7082N, build
0036274154116; checked on an Echo running it). Put the `.bin` into `firmware/athena/` (not in git).

Other builds: `scripts/probe.sh` compares those six files (`PROBE_FILES`, `probe.md5`) with 7080's. All the same (7077,
7082N): installed as 7080, with no question. Any different: the setup and `scripts/install-system.sh` print a large
warning naming them and ask before they go on (`HASSMIC_FIRMWARE_OK=1` answers yes for `install-system.sh` without a
terminal), since no step here puts 7080 back. Amazon moved athena to a new Fire OS line with
`update-kindle-athena-PS75751_user_7806_0036811210372.bin` (PS75751.7806N, build 7806, Android 9 still): two of the
six differ (`libpryon.so`, `libaudioflinger.so`; different build IDs, but the same exported symbols — the embedded
`codebuild` source paths say these were rebuilt, not that the API moved), so it asks. Installed and self-tested on an
Echo on 7806 (2026-11-01): boot, wake word, voice assistant start, and Kiosk Satellite (install, Wi-Fi install, the
`kiosk_mic` restart-on-revoke behavior, and a full uninstall) all worked.

## Unlock and root (public: saiyaku-athena)

saiyaku-athena, a serial-port exploit for the MT8183 and not this project's work, unlocks it, by hand, as its [XDA
thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-8-2nd-gen-2021-athena.4804958/) says:
[`unlock.md`](unlock.md). The guided setup (`scripts/setup.sh athena`, below) does not run it: it names the thread,
waits for Enter, then checks for the Echo on adb. The same for the root that follows, boot-root installed from TWRP as
the thread says: the setup waits for Enter and checks for root adb. The unlock's and the root's own files
(`boot-root.zip`) are the thread's: it says which, kept current with its procedure, and nothing here downloads or reads
them. The file of this project's steps goes into `firmware/athena/` (not in git):

| File | Source | sha256 |
|---|---|---|
| `magiskpolicy32` | `lib/armeabi-v7a/libmagiskpolicy.so` of `Magisk-v30.7.apk`, [Magisk v30.7](https://github.com/topjohnwu/Magisk/releases/tag/v30.7) (APK `e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5`): the tool the install patches the policy with, on the Echo; the guided setup fetches it. Until 2026-11-09 the one in the thread's `boot-root.zip` (not compared byte for byte with this one; Magisk's is not tried on an athena yet) | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |

`boot-root.zip`, installed in TWRP, makes adb root *by patching the running system in place*: `ro.secure=0`, a patched
`adbd`, `su` and `adbd` permissive in the SELinux policy it rewrites in `/vendor/etc/selinux/precompiled_sepolicy`, OTA
blocked (`otacerts.zip` emptied, `softwareupdates.amazon.com` pointed at localhost). dm-verity is off
(`ro.boot.veritymode=disabled`).

## Install

The guided setup: `scripts/setup.sh athena` (`--dry-run` walks the steps without running any). By hand, with the Echo
rooted and on adb:

1. **Unpack the firmware** (below), build: `make DEVICE=athena`; magiskpolicy into `firmware/athena/magiskpolicy32`
   (the table above; the commands as in [checkers' README](../checkers/README.md#2-unpack-the-firmware-on-the-pc)).
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
hassmic's own screen (the face, the drawer, the home app, Music Assistant: [the guide](../../docs/GUIDE.md#the-screen-echo-show))
is built for it, with Android 9's `SurfaceControl.Builder` and `getTasks`, and checked against this firmware's
framework.
**Open**: the lux under a bright lamp held close; not confirmed on this Echo: Improv, the factory reset, `--uninstall`,
the update check and the screen UI. Details and measurements: [docs/re-athena.md](../../docs/re-athena.md).

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
| `setup.sh` | the guided steps: the unlock and root (boot-root) by the XDA thread, each checked afterwards |
| `unlock.md` | the unlock and root by the XDA thread |

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

**Tested on a real Echo (2026-11-01, PS75751.7806N):** `kiosk-install.sh` over both USB and Wi-Fi; `kiosk_tick`
opening the app, granting permissions and starting the update helper on first boot; camera and microphone off by
default. `kiosk_camera` and `kiosk_mic` only restart the app's process when turned *off* (Android kills an app whose
already-granted dangerous permission is revoked; granting one does not need a restart — `main.sh` already says so at
the `kiosk_perm` definition). `kiosk_internet` restarts nothing either way: it is a firewall rule, not a permission.
Kiosk Satellite wins the screen-timeout race against hassmic's `screen_off_timeout`: it holds its own
`SCREEN_BRIGHT_WAKE_LOCK` (`FLAG_KEEP_SCREEN_ON`) while its activity is foregrounded, so hassmic's 300 s setting never
gets a chance to fire.

Uninstalling it is not a plain `adb uninstall`: Kiosk Satellite is an Android device admin, which Android refuses to
uninstall. `scripts/kiosk-install.sh --uninstall` takes the admin off first;
[docs/kiosk-satellite.md](../../docs/kiosk-satellite.md#uninstalling) has how, and the way by hand used on this Echo
before the script existed. Once the app was gone, ports 2324 and 6053 closed within about 3 seconds.
