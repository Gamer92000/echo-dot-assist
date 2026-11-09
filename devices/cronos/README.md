# Echo Show 5 2nd gen, 2021 (`cronos`)

**Installed and running on one Echo Show 5 2nd gen** (2026-10-26, with the guided setup; "What works and what is open" below, [docs/re-cronos.md](../../docs/re-cronos.md)). It is the Echo Show 5 1st gen (`checkers`) with another mute button
driver and other speaker amplifiers: the same platform, the same Fire OS build (NS65741/8149), the same install
(`INSTALL=boot` through `scripts/install-system.sh`, which hands over to `scripts/install-boot.sh`). No light ring:
hassmic shows it as a band along the bottom of the screen (setting "Light band"). Porting guide:
[devices/README.md](../README.md); what checkers' page says about the install holds here too, and is not repeated:
[devices/checkers/README.md](../checkers/README.md).

Model C76N8S (`ro.product.model` AEOCN), MediaTek MT8163, 5.5" touchscreen (960 x 480, as checkers'), 2 microphones, camera
with a shutter, volume keys, mic mute. No action button.

## Firmware

Pinned: `update-kindle-cronos-NS65741_user_8149_0013222532484.bin`, Fire OS 6574.1 (NS65741/8149), sha256
`fa6203b55d8764201cd531507750e66e1ab2215969433b52d715a4b6089f1b09`, 1388134745 bytes. Download it from its
[FTVDB record](https://ftvdb.com/echo/firmware/com.amazon.cronos.android.os/c204364d8a65c6da5d60024c6334fe1e-13222532484-fire-os-6574-1-ns65741-8149-2026-09-28/).
This is cronos' own image, not checkers' (same build number, other files). The system partition must be exactly this
build's: the boot image is made from its `boot.img` and hassmic links against its libraries. Newer builds (8150, 8151)
are not supported until pinned here.

## Unlock (public: amonet-cronos)

By hand, as the [XDA
thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-5-2nd-gen-2021-cronos.4772596/) (R0rt1z2)
says: [`unlock.md`](unlock.md). The unlock's own files are the thread's: it says which, kept current with its procedure;
nothing here downloads or reads them, its `boot-root.zip` included. The files of this project's steps go into
`firmware/cronos/` (not in git):

| File | Source | sha256 |
|---|---|---|
| `update-kindle-cronos-NS65741_user_8149_0013222532484.bin` | Amazon, link above | `fa6203b5…f1b09` |
| `magiskpolicy32` | `lib/armeabi-v7a/libmagiskpolicy.so` of `Magisk-v30.7.apk`, [Magisk v30.7](https://github.com/topjohnwu/Magisk/releases/tag/v30.7) (APK `e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5`); the guided setup fetches it, by hand as in [checkers' README](../checkers/README.md#2-unpack-the-firmware-on-the-pc) | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |
| `adbd` | `sbin/adbd` of Google's Android 7.1.1 emulator image (`system-images;android-25;google_apis;armeabi-v7a` r18; only its ramdisk is read, `scripts/zipmember.py`): a userdebug adbd that goes by `ro.secure`, for root adb in our boot image (`ADBD` in `device.conf`). The guided setup fetches it. Not tried on a Show yet; until 2026-11-09 the XDA boot-root image's (CyanogenMod's) was the tested one | `cc0178051cbf3fa0c7df5c880abd1becf55ce9f35b60ac851aae2edd106a3c18` |

On an aarch64 PC such as a Raspberry Pi the guided setup installs `qemu-user` from the package manager for the policy
tool (its qemu-arm download is x86_64) and,
on Ubuntu 24.04 and later, asks to allow user namespaces until the next boot (`tools/qrun.sh` needs one). The NDK is
x86_64 too: take the release's binaries when the setup offers them, else the build runs under `qemu-user-binfmt`
(about 15 min on a Raspberry Pi 4; docs/re-cronos.md, "First install"). Over ssh the udev step gives the USB devices
to a group the user is in (`plugdev`, `adbusers`, `sudo` or `wheel`): `uaccess` serves only whoever sits at the PC.

**Never write lk, preloader, tee1/tee2 (tz) or any other bootloader partition.** This model has no bootrom USB download:
a brick there is permanent. An install for this model touches `system` and `boot` only.

## Install

The guided setup does all of it: `scripts/setup.sh cronos` (`--dry-run` walks the steps without running any). On Linux
(Ubuntu: `adb`, `fastboot`, `e2fsprogs` for `debugfs`, `python3`, `unzip`, `curl`, `make`, `cc`, `sqlite3`, `bc`, `xz`). By hand,
as checkers':

1. **Unpack the firmware** (below), build: `make DEVICE=cronos` (or `make DEVICE=cronos STUBS=1` without the firmware).
2. **Unlock** by hand, as the XDA thread says ([`unlock.md`](unlock.md)); stop once TWRP shows on the Echo's screen:
   tap **Keep Read Only**.
3. **System partition**: must be 8149's (`ro.build.version.incremental` = `0013222532484` in its `build.prop`). Else write
   `firmware/cronos/images/system.img` (sha256 `78c4c2fa…091669`, 3253731328 bytes) to
   `/dev/block/platform/mtk-msdc.0/11230000.MSDC0/by-name/system` from TWRP and read it back; `setup.sh` does this in
   64 MB pieces with a check of each. Wipe cache and data first, from TWRP's main menu (past "Keep System Read only?":
   answering it restarts USB), with `adb shell 'twrp wipe cache && twrp wipe data'`: the old build's app updates in
   `/data/app` would run over this older system. `adb shell 'mount /data 2>/dev/null; grep " /data " /proc/mounts && ls /data/app'` must then print the mount and no app.
4. **Root boot image**: `scripts/install-boot.sh --boot-only` (the firmware's `boot.img`, sha256 `ecdd1452…87db49`, with
   root adb, `verify` out of the fstab, adb over Wi-Fi and the boot chime out of `init.cronos.rc`, the policy patched by
   `sepolicy.rules`; flashed in hacked fastboot, then `fastboot continue`).
5. **Lock down, join Wi-Fi, install**: `scripts/wifi-join.sh` after the lockdown (`scripts/device/lockdown.sh`; do not
   join before it: Fire OS would reach Amazon), then `DEVICE=cronos scripts/install-system.sh`.

Updating later: `DEVICE=cronos scripts/ota-push.sh <echo-ip>`. `scripts/install-system.sh --uninstall` turns Alexa's
apps back on, takes hassmic off `/system` and wipes cache and data in TWRP (tap **Keep Read Only** when it asks):
Amazon's apps lost their data and permissions when the install took them off, and without the wipe their setup
wizard crashes in a loop ("OOBE has stopped"). The Echo then starts at Amazon's setup. All the way back to stock is the
XDA thread's.

## What works and what is open

Seen on the first cronos (2026-10-26; [docs/re-cronos.md](../../docs/re-cronos.md)): the whole guided setup (amonet,
system partition, root boot image, build, lockdown, Wi-Fi, install), hassmic up and in Home Assistant, the wake word
engine, the volume keys, the mute button through `gpio-privacy` (athena's driver, not checkers' `amazon-gating`), the
light band (960 x 10), the light sensor (`als_ps/lux`), a push update over Wi-Fi that passed its self test.

The speaker amplifier is one of three by board revision; the first cronos has the Maxim MAX98396. Speaker gain:
RT5616/RT5616E as checkers', TAS5805M up to +4 dB, MAX98396 up to +6 dB, the caps until someone has listened at full
volume (the TAS5805M's steps are assumed, it is in no cronos seen yet).

hassmic's own screen (the face, the drawer, the home app, Music Assistant: [the guide](../../docs/GUIDE.md#the-screen-echo-show))
is built for it as for checkers; its JNI table resolves against this firmware (`make unit-screen`).

**Open:** speaker gain by ear, Wi-Fi motion (module built, its hook site as checkers'), the DHA version for "Download
from Amazon", Bluetooth, the screen UI on the Echo.

Amazon's screens are gone: the install takes Amazon's apps off the system partition, Settings and System UI among them,
as on checkers (`STOCK_APPS_OUT`; why: [devices/checkers/README.md](../checkers/README.md)). hassmic's own screen
takes their place ([docs/GUIDE.md](../../docs/GUIDE.md#the-screen-echo-show)); on this model only its band has been
seen so far (open, above). The Echo's settings are on its settings page and in Home Assistant.

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
| `setup.sh` | checkers' steps with cronos' XDA files, the udev step |

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
