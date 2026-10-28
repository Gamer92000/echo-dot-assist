# Echo Show 8 1st gen (`crown`): what differs from checkers

Probed over adb on one rooted Echo Show 8, 2026-10-13 (read-only commands, then a trial run of hassmic from
`/data/local/hassmic`, nothing installed). Everything not listed here is as on the Echo Show 5: read
[re-checkers.md](re-checkers.md) first. Firmware: `update-kindle-crown-NS6570_user_6074_0012383140484.bin`, Fire OS 6.5.7.0
(NS6570, build 6074), sha256 `4b6fb088…ce421`; `ro.product.device=crown`, `ro.product.model=AEOCW`.

## Same as checkers

MT8163 (armv7, zygote32), Android 7.1.2 (SDK 25), kernel 4.9.77 (`devices/crown/kconfig` is its own IKCONFIG), no mixer
(the front end, libasp, is inside the audio HAL), Alexa as apps, no A/B (`INSTALL=boot`), RT5616 speaker codec, mute
latch (`gating` input) beside `gpio-keys` (volume keys, camera shutter), MT7668 Bluetooth on `/dev/stpbt`, Wi-Fi
`wlan_mt76x8_sdio.ko`, `/proc/idme/bt_mac_addr`, thermal `mtktscpu`, light sensor `als_ps/lux`, no LED ring and no
action button. `adbd` and `magiskpolicy32` are byte-identical to checkers'.

## Different

| | crown | note |
|---|---|---|
| Screen | 800x1280 panel, landscape (1280x800), density 213 | the light band is 1280 x 10 at the bottom |
| `libpryon.so` | 2.41 (checkers' is older) | `pryon_test` loads the firmware's ALEXA model; the stubs list is regenerated (26/17/8 symbols for pryon/opus/z) |
| `libasp.so`, audio HAL | differ from checkers' | `probe.md5` pins them |
| Speaker gain | `OUT Playback Volume` 0..39, stock **30** (checkers: 29), 1.5 dB steps | `board.c` |
| Wake sound | the stock one is not in SIM's APK (`res/eb.wav` is empty) | `ui_wakesound*` entries left out of `board.c`: hassmic's built-in blip plays (no app has the sound, below) |
| Stock apps | ten of checkers' `STOCK_APPS_OUT` do not exist; crown has more of its own (`settings.windfall`, `ahe.damca`, `kindle.cms-service`, `FireOSDownloadProvider`, `TabletCamera2`, `knight.ecs`, `knight.hds`) | to classify by what crashes after an install |
| Attestation | the binder service `amazon.security.dha` answers `getDhaVersion` = **2** | `dha.c` builds drvV1 (version 1) only; see below |

## Unlock and root

amonet-crown v2.0.1 (XDA, R0rt1z2; `devices/crown/unlock.md`), then hacked fastboot (kaeru 2.0.0). Its `boot-root.img`
is **NS6571's**, not NS6570's; the install builds its own from the firmware's `boot.img` (`scripts/install-boot.sh`), so
the unlock's image is only a way to get root adb. In hacked fastboot `fastboot boot` does not boot an image;
`fastboot continue` boots the `boot` partition, and `adb reboot bootloader` leaves the RTC flag that sends every warm
reboot back to fastboot until the power is pulled (as on checkers).

## Trial run (2026-10-13)

`scripts/deploy.sh` and `run.sh` from the PC, on the Echo as it was: OpenSL ES capture (VOICE_RECOGNITION, 16 kHz mono),
wake word "alexa" detected (type 0 and 2 at 20 to 50 s into a 90 s recording), volume up/down and the mic mute latch
read through the grabbed input devices, band 1280 x 10, speaker gain 0..39 read at stock. A second `run.sh` at the same
time gave "Address already in use" and "EVIOCGRAB busy": one instance only.
Not seen: Bluetooth (Android's stack still owns the controller before an install: "controller does not answer"),
Wi-Fi (the Echo had no network), the install.

## Boot and the mute latch (2026-10-14)

Kernel start to hassmic: firewall 11 s, Android's `boot_completed` and the satellite start 22 s, hassmic 64 s (40 s with
`alexa-off.sh` asking `pm` once instead of once per package: tried, not part of this port). The mute latch (`amazon-gating/state`) reads 0, 2, 1 while the
driver comes up (12, 16, 17 s) and then **1 (muted) until something lets it go by itself 20-80 s after power on**: no file
of the firmware's userspace names `amazon-gating` apart from `init.project.rc`'s chown, and nobody pressed the button.
System server plays its privacy sound at 14 s (and "off" 17 s later, both missing: SystemUI's resources are not there).
A hassmic that starts before the release finds the latch on; it now shows it without the "mics off" sound
(`mute_latch(1, 0)`), the release is announced as any button release.

## Sounds (2026-10-14)

The output stays closed until a sound comes: opening AudioALSA's playback takes ~170 ms (`openPcmDriver`, logcat) and
the amplifier switches on after that. The built-in wake blip is 120 ms: written to a cold output it was never heard, the
0.5 s mute sounds were (clipped). `audio_android.c` starts an earcon 350 ms of silence late when no player of ours had
audio for 2.5 s. hassmic plays no separate "reply" sound on any model: the answer is Home Assistant's speech.

## Wi-Fi motion (2026-10-14)

`wlan_mt76x8_sdio.ko` is checkers' driver layout: `nicRxProcessDataPacket` calls `nicRxFillRFB` at +0x64 (the one
`bl` in its first instructions), `ucGroupVLD` is stored at SW_RFB_T +16, the group 3 pointer at +32, and
`nicRxGetRcpiValueFromRxv` reads RCPI from byte 12 of it. So `hassmic_rcpi4m` needs no change and no parameters:
`make kernel-tools DEVICE=crown` (kernel.org 4.9.77, `arm-eabi-4.8`), `make DEVICE=crown` builds
`build/crown/hassmic_rcpi4m.ko` from `devices/crown/kconfig`. `insmod` on the Echo (nothing else changed): "hooked",
`/proc/hassmic_rcpi4m` counts frames (KIND 0x14009: the access point's VHT unicast frames, as on the Show 5).

Seen in HA's sensor too (2026-10-14): after switching it on it stayed "clear" for the first minute, then followed
movement; `wm_kind_norm` only sets each frame kind's level from its first 5 readings. No threshold change was needed.

## DHA version 2 (2026-10-14)

`amazon.security.dha` answers version 2 on crown. Transactions as checkers: 1 sign(digest), 3 getDhaPublicKey (a P-256
SPKI), 4 getDhaHmac (exception -7), 5 getDhaCertificateChain (5.4 kB of PEM, the leaf first, LF line ends), 6 version.
Java MAP's drvV3 (`com.amazon.imp`, `kcpsdk.auth.q` a/m/j/h, `v.i`, `l0.d/o`, decompiled with androguard):
header `{"typ":"drvV3","alg":"ES256","x5c":["<first certificate>"]}` (DER re-encoded, Base64 flags 2: standard, padded;
org.json writes `/` as `\/`), payload as drvV1's (`dat`, `dev` {typ, dt, dsn, cpuid}, `cust`), signature = the
service's DER ECDSA over SHA-256 of the two parts, as R || S (32 bytes each). `dha.c` `jwt_java_v3` builds it
(`tests/unit/dha_jwt_test.c --service2` byte for byte). Against the real Amazon: login by code pair, the models
downloaded and installed from the settings page on a Show 8.

## Open
- The guided setup on a real Echo Show 8 (the install by `scripts/install-system.sh` works, 2026-10-14).
- Separate volumes take the Echo Dot 2's curve (`board.volume_curve` unset) until `tools/vol-curve.py` has measured
  this one; the light ring designs on the band (`ring_band.c`, as on checkers, where they are) not tried here.
- The wake blip by ear (lead-in of 350 ms, `audio_android.c`).

## Bluetooth, wake sound, extra apps (2026-10-14)

- Bluetooth: with `app/Bluetooth` and `KnightBluetoothAVRCPClient` off the partition, hassmic owns `/dev/stpbt`:
  "controller ready, ACL 32 x 251", passive and active scans, a GATT connection with service discovery, "a2dp: ready",
  codecs SBC/AAC/aptX/aptX HD. Nothing to change in `board.c`.
- Wake sound: `ban_ui_wakesound` (`res/eb.wav`) and `ban_ui_endpointing` (`res/ie.wav`) are both empty files in SIM's APK,
  and no other app has a resource of that name: this firmware has no wake sound file, so hassmic's built-in blip plays.
- `MODEL_NUMBER` C7H6N3 is the label's (`/proc/idme` has no model number: product_name/productid read 0).
- Crown's extra Amazon apps, on the installed Echo (none runs, none crashes, load comes from audioserver and hassmic):
  `settings.windfall` is in `STOCK_APPS_OUT` already; `kindle.cms-service` does not even parse ("Required FireOS Feature
  package_recency:3 not on device"); `knight.ecs` / `knight.hds` are not on this firmware (only a permission named after
  ecs in `smarthomemapviewapp`); `FireOSDownloadProvider(Ui)` is the system download provider that
  `com.android.providers.downloads` loads its library from and `TabletCamera2` is Android's camera app: both stay.
  `ahe.damca` (a service and a receiver, idle, no Alexa client needed) and `smarthomemapviewapp` stay too: removing an app
  only pays when it crashes or burns CPU.

## Kiosk Satellite as a display (2026-10-14, trial)

`me.jxl.kiosk_satellite` 2026.10.23 (armeabi-v7a, minSdk 24, Flutter + WebView) installs with `adb install` and starts, but
needs a WebView provider: with `app/AmazonWebView` (Chromium 118, `com.amazon.webview.chromium`) off the partition the
Echo had none ("No WebView installed"). It is no longer in crown's `STOCK_APPS_OUT`; after the reboot the provider loads
and the setup wizard shows, above the light band (the band's overscan is respected). Open: its voice assistant stays off
(deny RECORD_AUDIO/CAMERA), HOME choice, screen timeout, ports above 16384 for its own server, updates (its APK cannot
reach GitHub through the egress lock).

### Kiosk Satellite: helper, keyboard, screen (2026-10-14)

- Update helper: the app's content provider `content://me.jxl.kiosk_satellite.update-helper/start` returns a script that
  starts `app_process … UpdateHelper serve <uid> <token>` (the app's own code, a local TCP socket, the app installs its
  updates through it). Over adb that is root's; `main.sh` `kiosk_helper` (device.conf `KIOSK_PACKAGE`) runs it as `shell`
  with groups `log,inet` (`inet`: without it `ServerSocket` fails with "Permission denied"), after boot and again every 5 min
  (an update of the app ends it; the script is idempotent). The app still has to fetch its APK: the egress lock drops its
  traffic to the internet, so touchless updates need either hassmic fetching the APK or letting the app out.
- Keyboard: the Echo has no input method (Amazon's `bluestone-kb` needs Amazon's account libraries). Simple Keyboard 6.7
  (`rkr.simplekeyboard.inputmethod`, only VIBRATE, minSdk 24) installed by hand and set with `ime set`.
- Screen: `ANDROID_SETTINGS` now sets manual brightness 120 (auto mode had the backlight at 1/255), and
  `accelerometer_rotation=0`, `user_rotation=1`: the Echo has no accelerometer; KS had put it in portrait, where the band's
  overscan became a strip down the left edge.

## Kiosk Satellite: start, internet, keyboard (2026-10-14)

- Nothing started the app after a boot (no HOME category: hassmic-home is the home screen). `main.sh` `kiosk_helper` now
  opens it once with `monkey` (as shell) when it is not running, 10 s after boot_completed, then keeps the update helper up.
- Egress: setting `kiosk_internet` (page, System; screen models only, not exported, default on since the remote admin
  entry below). `lockdown.sh` `kiosk_state` reads `state/config`: anything but `kiosk_internet=off` opens it and adds `-m owner --uid-owner <uid of KIOSK_PACKAGE from
  packages.list> -j RETURN` before the gid rule. Checked on the Echo: uid 10000 reaches 1.1.1.1:80, shell user times out;
  Amazon's daemons have other uids.
- `install_keyboard` in `scripts/lib/setup.sh` (crown `step_install`, asked, default no): Simple Keyboard release 148
  (6.7), sha256 pinned, `adb install`, `ime enable` + `ime set`.
- Not in setup: Kiosk Satellite itself (the user picks the app: VACA and others exist).

### Remote admin port and internet default (2026-10-14)

TCP 2324 (KS's web page/API; 6053, its ESPHome API, added 2026-10-15 after Home Assistant could not reach it) is outside the admitted 16384-32767: `lockdown.sh` `keep()` now admits it from private
addresses where device.conf has `KIOSK_PACKAGE` (nothing listens without the app). LAN egress from KS's uid already
worked (the chain returns local destinations); a hosted or external HA URL needs the internet, so `kiosk_internet` now
defaults to on (an Echo that saved "off" before keeps it).

### Do not make Kiosk Satellite the home app (2026-10-14)

Leave `org.hassmic.home` as HOME. KS declares no HOME category, so Android will not offer it (`cmd package
set-home-activity` refused it), and it need not be: the home activity is what keeps the screen's overscan and the light
band's layer in place, and `main.sh` opens KS over it once per boot. Skip any KS step that asks for the launcher role or
a lockdown/kiosk mode built on it (its device admin only has force-lock here). Cost: if the app is closed or crashes,
the screen shows the plain home screen until the next boot or a manual start (remote admin on 2324 or
`monkey -p me.jxl.kiosk_satellite -c android.intent.category.LAUNCHER 1`).
