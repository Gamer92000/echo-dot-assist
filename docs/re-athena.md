# Echo Show 8 2nd gen (`athena`): what is different

Probed over adb on one rooted Echo Show 8 2nd gen, 2026-10-14, then installed on it. It is **not** the platform of
`checkers`/`crown` (read [re-crown.md](re-crown.md) and [re-checkers.md](re-checkers.md) for what they share with it:
no mixer, OpenSL ES, a light band on the screen). Firmware: `update-kindle-athena-PS75741_user_7080_0036274153604.bin`,
Fire OS 7574.1 (build 7080), sha256 `7a76fe6f84847c183e617fe6bb0c311faa9ad5ab56803b3a6115fb763c422a1a`;
`ro.product.device=athena`, `ro.product.model=AEOAT`.

## Platform

| | athena | crown |
|---|---|---|
| SoC | MT8183, 8 cores, 2 GB | MT8163 |
| Android | 9 (SDK 28), Treble (`/vendor` partition, HIDL HALs), system as root | 7.1.2 |
| Kernel | 4.4.146, 32-bit (armv7l), `/proc/config.gz` | 4.9.77 |
| Boot | not A/B; `boot` p20, `system` p21, `vendor` p12; verity off | not A/B |
| SELinux | **enforcing**; the policy is `/vendor/etc/selinux/precompiled_sepolicy` | permissive su in boot.img's policy |
| Speaker amp | TI TAS5805m: `Master TAS5805m Playback Volume` 0..158, 100 at boot | RT5616 line out |
| Light sensor | TCS3400 RGBC, an input device with `als_*` attributes | STK3x1x |
| Wi-Fi | `wlan_mt76x3_sdio.ko` (named gen2, its RX path gen4m's) | `wlan_mt76x8_sdio.ko` (gen4m) |
| Bluetooth | `/dev/stpbt`, `btmtksdio.ko`, owned by HIDL `bluetooth-1-0` | `/dev/stpbt` |
| Inputs | `gpio-keys` (volume, camera shutter), `gpio-privacy-state` (`SW_MUTE_DEVICE`), `gpio-privacy-button` (`KEY_POWER`), `tcs3400_als`, touch | `gating` |
| Privacy latch | `/sys/devices/platform/gpio-privacy/state` (the Dots' driver) | `amazon-gating/state` |
| `libpryon.so` | 2.41 (md5 `3d687c25…`), differs from crown's; `libopus`, `libz` differ too | |
| Wake word | `/system/local/models/keyword/en-US/ALEXA/pryon.manifest` (21 locales) | same path |

`libasp.so` is in `/vendor/lib`, loaded by the vendor audio HAL (`fireos.hardware.audio@2.0-service`); `PROBE_FILES` names
it as `vendor/lib/libasp.so`. The unpacked firmware keeps `/vendor` as `rootfs/system/vendor`.

## Root as the unlock leaves it

saiyaku's `boot-root.zip` does not touch the boot image: it edits system and vendor in place (see the README): the policy
has `su` and `adbd` permissive and `allow adbd su process { transition dyntransition }`, **no** `allow init su process`.
So hassmic's init services (`seclabel u:r:su:s0`) need `sepolicy.rules`, applied by `scripts/install-system.sh` on top of
that file with the unlock's own `magiskpolicy32`, run on the Echo; `sysinstall.sh` remounts `/vendor` read-write for the
write (`otatool remount rw /vendor` works as for `/system`: BLKROSET, verity off) and keeps `precompiled_sepolicy.pre-hassmic`
for the uninstall. Init loads the patched file at boot (the plat-and-mapping sha256 pair is unchanged).
`device.conf`: `INSTALL=running`, `SEPOLICY_FILE`.

## Found on the way (all fixed unless marked open)

- **`accept()` and `connect()` failed with `EBADF`** under the unlock's policy: every listener of hassmic ended at its
  first connection and no reply could be fetched. The cause, netd denied on sockets of the `su` domain, and the rule
  that fixes it: "The libc's sockets" below.
- **The band's renderer** (`ringbar.dex`) needed `SurfaceControl.Builder`: Android 9 has no
  `SurfaceControl(session, name, w, h, format, flags)`. hassmic passes `builder` as a second argument when
  `ro.build.version.sdk` is 26 or more; the same dex serves the older models. And the Java `SurfaceControl` has to stay
  reachable: with nothing using it after the setup its finalizer released the layer at a GC, every ~55 s (`nativeLockCanvas`
  threw `IllegalArgumentException`, hassmic restarted the renderer each time); the program now uses it after its loop.
  `--framework` of `tools/mkringbar.py` cannot read Android 9's boot classes (compact dex inside vdex): `make unit` checks
  checkers'. Checked once by hand against athena's boot classes (2026-10-20): every class and method both dex programs
  reflect on is there, the Android 9 ones included.
  `screen_wake()` (`service call power 9 i64 .. s16 .. s16 ..`) is unchanged on Android 9 (checked: `mWakefulness` Asleep -> Awake).
- **Push updates** work since the `accept()` fix (the two tried before it stalled or were reset: the listener was dead):
  pushed, self test passed, promoted to the factory copy. ART leaves an `oat/` directory beside a dex it loads
  (`ringbar.dex`); `sysinstall.sh factory` tried to copy it as a file ("cannot write /system/hassmic/oat.new") and now skips directories.
- **Bluetooth**: `/dev/stpbt` opens only for one process (`btmtk_fops_open: mode is 3` in dmesg, `ENOENT`): the HIDL HAL
  `bluetooth-1-0` has it. `BT_SERVICE=bluetooth-1-0` (alexa-off.sh stops it), `app/Bluetooth` is in `STOCK_APPS_OUT`,
  `global/bluetooth_on=0`. After that: "controller ready", A2DP ready.
- **Wake sound**: unlike crown's, athena's SpeechInteractionManager has `ful_ui_wakesound_hybrid` = `res/7E.mp3` (as
  checkers) and `ful_ui_endpointing` = `res/Eh.mp3`; `board.c` lists the wake sound and the same others as crown's. All decode
  (`build/sounds_athena`, in `make unit`). No setup beacon in any app.
- **Firewall**: netd's chains, all policies ACCEPT before the lockdown, nothing of Amazon's stock `firewall.sh`: crown's
  `FW_KEEP` (DHCP answers) is enough.
- **Stock apps**: crown's `STOCK_APPS_OUT` fits 101 of 113 entries; athena's own Amazon apps were added by the same rule (not
  a shared library of `/etc/permissions`) and `Launcher3QuickStep` (Android's launcher, which would compete with hassmic's
  home screen). With them gone: boot_completed at ~17 s, one system_server, hassmic's home screen focused, 88 packages of 212,
  one crash seen (`pq@2.2-service`, MediaTek's picture-quality HAL, once per boot, before hassmic's start). Before they
  went: with Alexa's apps only disabled, Amazon's ambient home crashed ("AmbientHomeAndroid keeps stopping") and covered the
  screen, as on crown.
- **Volume**: Android 9's `IAudioService` has four more methods before the volume calls (the player tracking ones):
  setStreamVolume is transaction 7 and getStreamMaxVolume 14 (3 and 10 on Android 7; 3 is a one-way call here, and 10
  answers 0), picked by `ro.build.version.sdk` in `audio_android.c`. Before, the volume keys and Music Assistant's volume
  did nothing (2026-10-15).
- **Mute button**: the `gpio-privacy` driver holds the enable line for `enable-gpio-toggle-duration` (1000 ms in the device
  tree) before it changes `state` and reports the latch: button event to latch 1.23 and 1.29 s muting, 0.28 s unmuting
  (2026-10-15), while the red light under the button follows the press. `board.privacy_button` (`gpio-privacy-button`,
  KEY_POWER, not grabbed) and `privacy_settle_ms` 2000: `buttons.c` reports the press at once as the opposite of the
  latch, and the latch confirms it or the press is taken back (`tests/unit/buttons_privacy_test.c`).
- **Home Assistant**: adopted, a voice round trip (wake, transcript, spoken reply), 2026-10-14.
- **"Download from Amazon"** works (2026-10-14). There is no `libacehal_dha.so`: the key is behind the HIDL HAL
  `fireos.hardware.fireosdha@2.0` (service `fireos.hardware.fireosdha@2.0-service`, user keystore, group drmrpc;
  `amzn_dha.mt8183.so`, certificate in `/persist/dha_certificate.pem`), and the Java binder service `fireosdhaservice` in
  front of it answers exactly as crown's `amazon.security.dha` does (asked as uid system 2026-10-14: 3 = a P-256 public
  key, 5 = the certificate chain, 6 = version **2**, 1 = sign, which wants a 32-byte digest: "Invalid digest length" in
  logcat otherwise). So `dha.c` builds Java MAP's drvV3 from it (`svc_names`: it tries `amazon.security.dha`, then
  `fireosdhaservice`); nothing else new. Daemon user system is enough. Unit-tested with the fake service under athena's
  name. The Echo's network must let it reach api.amazon.<site>, api.amazonalexa.com and CloudFront. The Echo's own Pryon
  models come with the firmware.
- **Wi-Fi motion** works (2026-10-16): the driver is named gen2 (`wlan_mt76x3_sdio.ko`, a module, 4.4.146 kernel) but its
  RX path is gen4m's: `nicRxProcessDataPacket` calls `nicRxFillRFB`, whose RFB has the group-valid byte at +16 and the RX
  vector pointer at +32, and `nicRxGetRcpiValueFromRxv` reads word 3 byte 0, so `hassmic_rcpi4m.c` runs with its 32-bit
  defaults (hook at +0x68). Unicast frames RCPI 119-120 (VHT 80 MHz MCS 9, `KIND` 0x14009), broadcasts 129 (`0x100b`),
  `iw` says -46 dBm; the motion sensor switched on when someone walked past. Three things differ from checkers: the kernel
  has no `patch_text` and its module text is read-only, so the module makes the page writable (`set_memory_rw` through
  kallsyms) and writes under `stop_machine`; `CONFIG_MODVERSIONS` is on, so the module carries the kernel's CRCs
  (`devices/athena/Module.symvers`, read out of boot.img's Image: the words behind `__kcrctab_<name>`); kernel.org's
  4.4.146 with the device's config (stack protector off: arm-eabi-4.8 lacks `-strong`; LOCALVERSION `+` for the vermagic).
- **The band's renderer** died at a GC (fixed above) and once at start (`lockCanvas` while the home activity came up,
  ~74 s): hassmic restarts it by itself.
- **Sounds**: the first OpenSL ES player after idle takes about 575 ms before the HAL pulls its first buffer (the TAS5805m
  unmutes: `tas5805m_mute(): triggered unmute mode` in the kernel log), then `LEAD_MS` of silence (re-crown.md "Sounds").
  With the experimental wake sound removal on, Home Assistant often ended the command before it was spoken (2026-10-15):
  leave it off here.
- **Timing**: hassmic up 22 s after the kernel starts (satellite start 17.9 s, self test 22.7 s). Steady: the audio HAL
  ~74 % of one core (Amazon's front end), hassmic ~39 % (Pryon), SoC 36 °C, load average ~13 (threads in D state).

## Open

- The lux under a lamp held close ("Light sensor").
- Not confirmed on this Echo: Improv, the factory reset, `--uninstall` and the online update
  check.
- The speaker's volume curve (TAS5805m digital volume, no dB table: 0.5 dB a step assumed).

## Wi-Fi (Android 9's WifiService)

Android's framework owns the networks: `WifiConfigStore.xml`, wpa_supplicant behind HIDL, DHCP by the framework (no
`dhcpcd`); `wpa_cli` on `/data/vendor/wifi/wpa/sockets/wlan0` connects but never gets an answer (the supplicant's domain
may not write to a root-owned client socket; not pursued). So the Show 5's way: `wifictl.dex` against the binder service
`wifi`. What 2026-10-14 showed of `IWifiManager` on this build:

- Methods: `getConnectionInfo(String)`, `getScanResults(String)`, `startScan(String)`, `addOrUpdateNetwork(WifiConfiguration,
  String)`, `enableNetwork(int, boolean, String)`, `removeNetwork(int, String)`, `reconnect(String)`, `reassociate(String)`,
  `disconnect(String)`, `setWifiEnabled(String, boolean)`; `getConfiguredNetworks()` and `getPrivilegedConfiguredNetworks()`
  take nothing and return a `ParceledListSlice` (`getList()`); the privileged one carries the stored `preSharedKey`.
- As uid 0 with any package name (`root`, `android`, `com.android.shell`, ...) the calls work but `getConnectionInfo`'s SSID
  is empty and `getScanResults` is empty, with no log line and with or without `secure/location_mode`. As uid 1000
  (`runas system system`) with `"android"` both are complete (31 networks after `startScan`; 6 s was enough). Neither needs
  location mode.
- `wifictl.dex` (`tools/mkwifictl.py`): `pkg()` reads `ro.build.version.sdk` and is `"android"` on 28; then `inv` appends
  it to the calls above, `startScan`/`getScanResults`/`setWifiEnabled` pass it, and `lst()` unwraps the slice. On any other
  sdk nothing changed (checkers' behaviour is the one before). ART's verifier needs a `check-cast` to String before
  `equals` (the PC's checks did not catch it; the first run on the Echo died with a VerifyError).
- `devices/athena/device.conf` `WIFICTL_USER=system`: `wifi.sh` and `wifi-join.sh` run the dex through `runas` as that user.
- `ping` failed here with "connect: Bad file descriptor" under the unlock's policy ("The libc's sockets"). The
  default route is not in the main table (Android's per-network tables), so `agateway()` finds no router and `ajoin` goes by
  the address on `wlan0` alone.
- Tried: `wifi.sh take` with `scan`; `join` of the network the Echo is on (replaced in place, new lease, `ok ... 1`); `join`
  of a made-up name with `WIFI_JOIN_SECS=15` (`notfound`, the entry removed, `back on WuTang-LAN`).

## Light sensor

`tcs3400_als` (`/sys/devices/virtual/input/input3`): `als_clear`, `als_red`, `als_green`, `als_blue` follow the room
(clear ~180 in a dark room, 25000-40000 under a lamp), `als_itime` 700 ms, `als_gain` auto (64 dark, 16 and 4 bright).
`als_lux` stays 0 and `als_cct` 0: the calibration behind them, `als_lux_table` (`1,0,0,0,0,0`), is unset (writing it took a
value but changed nothing within 2 s; restored). Stock does not use the driver's lux: `sensor_config.json` has a
`virtual_als` of type light on top of the `rgbw` sensor, and `dumpsys sensorservice` shows its value, which is what the
conversion was fitted to: ams' DN40 formula for the part with the glass in front of the sensor passing 0.103 of the light
(`src/hassmic/lux.h`, 2.7 lx dark, 3003 lx bright, within ~10 %). `board.light_sensor` is `tcs3400:<dir>`
(`main.c lux_tcs3400` finds the input by its name, whose number is not fixed). The "Illuminance" sensor in Home Assistant
comes from it. **Needs more work** (2026-10-14): under a lamp held close to the sensor the value jumped between
~100, 66000, 7000 and 2000 lx. Suspects: the sensor's auto gain (64/16/4) and 700 ms integration changing between two
reads of the channels (hassmic reads six attributes one after the other, so gain, time and counts can come from different
cycles), and saturation of the 16-bit channels at bright light. Not yet looked into; the fit was only checked in steady
light. The LED ring's auto brightness does not exist here (the band, `core_led_has_auto`).

## Speaker amplifier (2026-10-15)

`/dev/snd/controlC0` has `Master TAS5805m Playback Volume` (numid 84, integer 0..158, 100 at boot, no TLV: the dB step is
unknown) and `TAS5805m LPM mode` (bool, 1). Listened to with Sendspin music at full volume (2026-10-15): 115, 120, 122
clean; 130 distorted. `board.c` limits it to 118 (a rap track's bass clipped at 122) (`speaker_gain_limit`, steps 2 per
dB: the dB step is assumed). The audio HAL did not rewrite it after a voice reply or a change of the setting
(2026-10-15); the watcher in `spkgain.c` (ours written back when it sees stock) stays as on the Dot 2. The Bluetooth audio path is Android's
AudioTrack volume times this, so raising the amp raises phone audio too.

## The libc's sockets

Under the unlock's policy the libc's `accept()`, `accept4()` and `connect()` failed with EBADF, while the same system calls
made directly worked, and `socket`, `bind`, `sendto`, `recvfrom`, `getsockname` were fine (tested with throwaway programs
on the Echo, 2026-10-14). toybox's `ping` and `nc` failed with it too. For hassmic: every listener ended at its first
connection (Home Assistant could not connect; `main()` returned 1 and the service restarted every time it tried, the OTA
and Sendspin threads stopped), hassmic could not fetch a spoken reply (`net: cannot connect to <HA>:8123` within a
millisecond, the light band ending without its thinking animation), and libcurl (the firmware's 7.58: online
updates, "Download from Amazon") said "Couldn't connect".

**Cause (2026-10-15):** Android 9's libc (libnetd_client) asks netd's fwmarkd to mark the socket in `connect()`,
`accept4()` and `sendto()`, and netd then reads the caller's socket. Stock policy lets it do that for the app domains only;
for sockets of the `su` domain, which every hassmic service, root's scripts, runas and adb run in, it was denied (`avc:
denied ... scontext=u:r:netd:s0 tcontext=u:r:su:s0 tclass=tcp_socket permissive=0`), and the libc answered EBADF.
`devices/athena/sepolicy.rules` allows it (`allow netd su tcp_socket|udp_socket|rawip_socket`), after which `toybox nc`
works. hassmic runs as the system user through `runas`, which changes the uid only, so it is in `su` too.
