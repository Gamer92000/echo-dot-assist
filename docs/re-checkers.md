# Echo Show 5 1st gen (`checkers`): what the firmware says

Worked out from the firmware only, 2026-10-07; nobody has run hassmic on an Echo Show yet. The unlock is public
(amonet-checkers, `devices/checkers/README.md`).
Firmware: `update-kindle-checkers-NS65741_user_8149_0013222532484.bin` (sha256 `f96fcfa1…73cfa3`), Fire OS 6574.1
(NS65741/8149), the same release as donut's. Model H23K37, `ro.product.model=AEOCH`, `ro.product.device=checkers`.

## Unpacking

Unlike the Dots, the update is a block OTA without `payload.bin`: `system.new.dat` + `system.transfer.list` (version 4),
`boot.img`, `images/{lk.bin,tz.img,preloader.img}`. Laid out like donut's `firmware/<codename>/`:

```sh
unzip firmware/checkers/update-kindle-*.bin -d firmware/checkers/
mkdir -p firmware/checkers/images firmware/checkers/rootfs/system && mv firmware/checkers/boot.img firmware/checkers/images/
# system.new.dat -> images/system.img: replays the "new" ranges of system.transfer.list
python3 tools/sdat2img.py firmware/checkers/system.transfer.list firmware/checkers/system.new.dat firmware/checkers/images/system.img
debugfs -R "rdump / firmware/checkers/rootfs/system" firmware/checkers/images/system.img
# boot.img (Android header v0, page 2048, MTK header on the kernel): the ramdisk (gzip cpio) into firmware/checkers/rootfs/
python3 scripts/mkbootroot.py --cpio firmware/checkers/images/boot.img firmware/checkers/ramdisk.cpio
(cd firmware/checkers/rootfs && cpio -id < ../ramdisk.cpio)
```

The kernel (`boot.img`, 4.9.77, 32-bit ARM) carries its config: `devices/checkers/kconfig`. Five device trees are
appended (proto, hvt, evt, dvt, pvt boards).

## Platform: an Android device, not a Dot

- MT8163, armv7 (`abilist64` empty), Android 7.1.2, API 25. `ro.build.type=user`, tags `amz-p,release-keys`.
- The whole Android framework runs: zygote, system_server, SurfaceFlinger, launcher (`com.amazon.bishop`), the system
  UI (`KnightSystemUI`; "Knight" is the Echo Show line). Alexa is a set of apps, not init services.
- Not A/B: one `system` (ro, dm-verity `verify`), `boot`, `recovery`, `userdata`, `persist`. The SELinux policy sits in
  the boot ramdisk (`/sepolicy`, which has `su` and `shell` domains). lk carries Amazon's one-time-unlock code.

## Audio: the front end lives in the audio HAL

No `bin/mixer`, no `libmixerAPI.so`, no `PuffinApp`, no ledcontroller. `libasp.so` (byte-different from donut's) is
linked by `vendor/lib/hw/audio.primary.mt8163.so`, `lib/libaudioflinger.so` and `lib/hw/audio.a2dp.default.so`; the
pipeline runs inside `audioserver`:

- Capture: `AudioALSACaptureDataClient` runs the ASR pipeline. Playback: `AudioALSAPlaybackHandlerNormal::doProcessAsp`
  runs the Speaker pipeline; its output is the ASR pipeline's echo reference (`WriteReferenceBuffer`, libasp
  `ReferenceMgr`). So anything played through an AudioTrack should be echo-cancelled (from the code, not measured).
- `vendor/etc/audio-algorithms/asp.cfg`: 2 mics (calibration from `/proc/idme/miccal`), 2 speakers at 48 kHz. Pipelines
  Speaker (48k/2ch), A2DP, ASR (in: 16 kHz, 4 ch, pcm24 + reference 48k/2ch; out: 16 kHz mono s16), Voice, SwwRef.
- The HAL picks the pipeline from the AudioRecord's source (`getAspPipelineType`, 0xa1984 in the HAL):

  | `audio_source` | pipeline |
  |---|---|
  | 6 VOICE_RECOGNITION, 1999 HOTWORD | ASR (0); ASR_CUSTOM (9) with a flag at stream attribute +0x10c |
  | 7 VOICE_COMMUNICATION | Voice (1) |
  | 2999 (Amazon's own) | SwwRef (6) |
  | 1 MIC | MIC (2), not in `asp.cfg`: unknown |
  | anything else | "Invalid input source" |

- Audio policy (XML): one input (Built-In Mic, 8–48k, mono/stereo), one output ("primary output", 48k stereo s16; no
  deep buffer, no offload).
- No micRaw: the binder's `startCapture` refuses on ship builds ("ASP capture is disabled for ship build").

### The `audiosignalprocessor` binder service

Registered by `amazon::asp::IpcHandlerBinder` in libasp (`service_contexts`: `audioserver_service`), descriptor
`com.amazon.asp.IAudioSignalProcessor`; client side `lib/libaspclient.so`, the AIDL in `aspclient.odex`.

| code | method |
|---|---|
| 1 / 2 | `registerListener` / `unregisterListener(IAudioEventListener)` |
| 3 | `command(int cmd, byte[] in, byte[] out)`: parcel cmd, in_len (≤ 4096), in, out_size (≤ 4096); reply exception, status, out_len, out |
| 4 / 5 | `startCapture` (4 fds) / `stopCapture` (refused on ship builds) |
| 6 / 7 | `startInjection` / `stopInjection` |
| 8 | `startIrCodeDetection(bool)` |
| 9 | `setActiveInputSource(int, byte[])` |
| 10 | `onWakeWordModelChanged(String, String)` |

The command numbers are donut's LASP ones (`com.amazon.asp.AudioSignalProcessor`):

- `SET_LISTENING_MODE` = 0x92, one int32: AFE vtable +0xc4/+0xc8 (utterance start/end) in libasp, as on donut
  (`main.c` `listening()`). Stock sends it at stream start and end (`NotifyAudioStreamStateCommand`).
- `REQUEST_ARBITRATION_JSON` = 0x17, in int32 256, out 256 bytes of JSON (voiced and ambient energy): the ESP score.
- Also `NOTIFY_PLAYBACK_STATUS` 0, `NOTIFY_TTS_STATUS` 1, `NOTIFY_MIC_MUTED` 4 (aspclient's constants).
- `NOTIFY_ASR_STREAM_STOPPED` = 14 and `SET_WAKEWORD_METADATA` = 114: not in aspclient; donut's
  `libgenericaspclient.so` pairs each LIPC name with its ASP number (`ldr` name, `movs r2, #n`; checked on 23 and
  149, which aspclient names), and checkers' libasp has the same cases: its command switch (`tbh` tables at
  0x2608e/0x2613c/0x261e4/0x26292) sends 14 to the "Stop AFE Diag and report metrics" code and 114 to the one that
  refuses an input of 0 or over 4095 bytes with "Invalid ASP_CMD_SET_WAKEWORD_METADATA data/size". `command` checks
  no caller (only capture and injection log "Caller has permission").

`IAudioEventListener.onEvent(int what, byte[])` (transaction 1): BEAM_DIRECTION 1, BEAM_INDEX 3,
ACTIVE_INPUT_SOURCE 5, SOUND_SOURCE_LOCALIZATION 12/15, PIPELINE_STATUS 19.

On donut `main.c` reaches these through `lipc-set-prop com.doppler.lasp`; checkers has no LIPC tools, so those calls fail
there (harmlessly) until a small `/dev/binder` client sends transaction 3.

## Stock's wake word path

`priv-app/SpeechInteractionManager` (package `amazon.speech.sim`, persistent) loads `lib/libwakewordserver_jni.so`
(JNI class `amazon/speech/wakewordservice/NativeWakeWordServiceCore`), which links `libpryon.so` and `libaspclient.so`:

- `AudioRecordAdapter::start` (0x16bfc), `initializeAudioRecordAndInjector` (0x1ab00): `AudioRecord(1999, 16000,
  PCM_16, MONO)` for the wake word, and `AudioRecord(2999, 16000, PCM_16, MONO)` for Pryon's "playback" decoder (no wake
  on its own TTS). Audio goes into an `amazon::AudioStream` ring for the backlog.
- libaspclient only for events (`AspEventMonitor`; reacts to what=2, pushes "ASPState" into Pryon).
- The app holds `CAPTURE_AUDIO_HOTWORD`.

Libraries in checkers' load at 0x1000–0x4000 above their file offset (libasp +0x4000, libwakewordserver_jni +0x1000):
`tools/fnstrings.py` finds nothing until that bias is applied.

## Pryon and models

- `lib/libpryon.so` is **byte-identical to donut's** (md5 `88a4e723…`), as are `libopus.so` and `libz.so`.
  `wake_pryon.c` and the stub lists carry over unchanged.
- `/system/local/models/keyword/<locale>/{ALEXA,AMAZON,COMPUTER,ECHO}`, 20 locales, in the newer layout (model "pryon":
  encoder + wake word decoders, NTT fusion, `BDPGeneratedFiles/`; plus "playback"). donut's `pryon_test` with
  `wake_pryon.c`'s calls detects ALEXA in `testdata/alexa_espeak.raw` on `en-US/ALEXA` (3.78–5.88 s) and on
  `world/ALEXA`. Under qemu the model has to be copied out of the rootfs first: qemu does not redirect `faccessat`, and
  the engine then reports "insufficient permissions" on `nttfusionconfig/ntt.cfg.json`.
- `models/AED` is the same as donut's.

## Device nodes and other board facts

- Keys (`gpio-keys`): volume up 115, volume down 114; from EVT on also `EV_SW` 9 "Camera Lens Cover". **No action
  button.** Privacy button: the `amazon-gating` driver, event "mute" with `linux,code = 0x74`. Touchscreen Goodix gt9xx.
  Event numbers: `getevent -il` on the device.
- Privacy latch: `/sys/devices/platform/amazon-gating/state` (and `enable`), chowned to system in `/init.project.rc`.
- Bluetooth: MT7668 combo, `/dev/stpbt` (bluetooth:net_bt_stack, `btmtksdio.ko`), address `/proc/idme/bt_mac_addr`.
  Owned by Android's `com.android.bluetooth`, which is no init service.
- Thermal: `mtktscpu`. Light sensor: STK3x1x (I2C 0-0044) on MediaTek's alsps framework (`/sys/class/misc/m_alsps_misc/`,
  input device `m_alsps_input`); Amazon's driver has a lux file of its own, `/sys/bus/platform/drivers/als_ps/lux`
  (below).
- LEDs: none worth driving (the red/green/blue nodes are mode 0); the mic-off LED is the gating hardware's.
- Earcons: no `/system/local/share/earcon`. Wake sound inside SpeechInteractionManager (`res/-E.ogg` =
  `raw/ban_ui_wakesound`, Vorbis; `res/7E.mp3` = `ful_ui_wakesound_hybrid`; names obfuscated, mapped through
  `resources.arsc`); privacy and volume sounds in KnightSystemUI (`res/raw/kni_controls_privacy_mode_{on,off}.mp3`,
  `kni_controls_volume_adjust.mp3`); Bluetooth in KnightSettings (`kni_system_bluetooth_bt_{connected,disconnected}.mp3`),
  the comms sounds in `com.amazon.comms.multimodaltachyonarm` (`ful_comms_*.mp3`, the five Drop In uses). Every one
  stored uncompressed in its APK. No discovery (setup beacon) sound.
- DHA: no `libacehal_dha.so`; the attestation key is behind the `fireos-dha` binder service `amazon.security.dha`
  (`libdha-aidl.so`, the HAL `amzn_dha.mt8163.so` inside it), which `dha.c` asks through `service call` (below).

## Services and network

- Alexa apps: `amazon.speech.sim`, `com.amazon.bishop` (both persistent), `com.amazon.device.echoaudioservice`,
  `com.amazon.afe.app`, `com.amazon.alexadirectivebrokerservice`, `com.amazon.alexa.awaservice`,
  `com.amazon.ambienthome`, `com.amazon.d3`. Setup: `com.amazon.amasetup.service`, `com.amazon.ds2.oobe.efd`,
  `com.amazon.kindle.otter.oobe`. Updates: `com.amazon.device.software.ota` (own sharedUserId).
- Init services that matter: `ahe` (AlexaHybrid), `dacd`, `trackerd`, `whad_cc`, `quantum`, `neo-init`,
  `neo-coordinator`, `shchipd`, `shblemeshd`, `shlocalskillsd`, `smarthomewifid`, `meshmgrservice`, `inlocservice`,
  `perfmonitord`, `avahi-daemon`, `mdnsd`. `dhcpcd_wlan0` runs `-BK`.
- Wi-Fi is Android's WifiService (system_server); `wpa_supplicant -O/data/misc/wifi/sockets -g@android:wpa_wlan0`.
- No stock `firewall.sh` (`ahe_firewall` names a file that is not there); netd owns iptables.

## What a port needs

The one list of what is done and what is open for checkers; `devices/checkers/README.md` and PLAN.md point here.
[x] done, [~] partly, [ ] open. It runs on one Echo Show (a tester's, from 2026-10-10), installed with the guided
setup; what was checked on the PC only says so.

**Unlock, install, boot**
- [x] Unlock: amonet-checkers v2.0.1 (public; its lk/tz are 8149's own). Root: 8149's own `boot.img` with root adb,
  `verify` out of the fstab, the policy patched (`sepolicy.rules`; the XDA `boot-root.img` is NS6570's).
- [x] Installer: `scripts/install-boot.sh` (`INSTALL=boot`), boot then system partition only; `hassmic.rc` (firewall
  `on boot`, satellite at `sys.boot_completed`). Installed the tester's Echo (2026-10-13).
- [x] First run on the device: `otatool remount` gets `/system` writable, init starts `hassmic` and `hassmic_fw` from
  `/system/etc/init/hassmic.rc`; pushed updates pass their self test and become the factory copy (the boot image and
  root adb: below).
- [x] Root boot image on a device (a tester's, 2026-10-10): boots, adb root in `u:r:su:s0`. Found on the way:
  - 8149's `sbin/adbd` is a user build (no `ro.secure` string, "adbd cannot run as root in production builds"):
    it drops to shell whatever `ro.secure`, `service.adb.root` or `fastboot oem flags 61` say (all three tried).
    Fixed: the XDA image's `sbin/adbd` (CyanogenMod, static, `ADBD` in device.conf) goes in (`mkbootroot.py
    --adbd`); the setup's files step fetches checkers' `boot-root.zip` for it (and takes a browser's
    `boot-root (1).zip`: donut's has the same name).
  - kaeru 2.0.0 does not clear the RTC "boot to fastboot" flag that `adb reboot bootloader` sets (`rtc_mark_fast`;
    `getvar boot-reason: RTC`, lk log `fastboot_mode=2` without reading `boot`): every `fastboot reboot` lands in
    fastboot again. Fixed: install-boot.sh boots with `fastboot continue`, and says to replug when still in fastboot.
  - The certificate page after the stock image is Amazon's kernel-signing cert with the image's sha256 in its SAN
    (`URI:sha256://4c00d790…` = stock image without that page); kaeru has `amzn_image_verify`. Whether lk refuses
    ours with it is not known (the RTC flag hid it); left out now, as the XDA image does.
  - Not byte-reproducible across zlib builds (zlib-ng packs the ramdisk ~4 KB tighter, same cpio): README gives the
    cpio's hash too.
  - The `.bin` must reach the Echo complete: an MTP copy stopped 33 MB short and TWRP said "corrupt". README and
    install-boot.sh's message: `adb push … /sdcard/update.zip`, size check, `twrp install`. Its updater-script also
    writes lk, tee1/tee2, preloader (`mmcblk0boot0`) and MBR; amonet's TWRP renames `by-name/lk` etc. to `*_real`
    (seen in its kernel log), so the by-name writes miss.
    - [ ] the preloader write (`mmcblk0boot0`, not by name) is not proven harmless.
  - setup.sh: the root step and checkers' install run in the foreground now (`live`), so install-boot.sh's hints
    show; the root step on a booted Echo with adb as shell replaces the boot image from there instead of waiting
    for TWRP.
  - Diagnostics: `fastboot oem logcat` prints preloader + lk logs; TWRP's `/sys/fs/pstore/console-ramoops*` the
    last kernel's.
- [x] Guided setup (`devices/checkers/setup.sh`): run on the tester's Echo from unlock to install (2026-10-10..13),
      fixed where it stopped (the items above); no longer marked UNTESTED.
- [x] CI: checkers in the build matrix of `.github/workflows/build.yml`.

**Audio**
- [x] `audio_android.c` (OpenSL ES): VOICE_RECOGNITION capture, one player per stream, mixed by AudioFlinger.
- [x] Recording rights read from the firmware (`libserviceutility.so` `recordingAllowed`): root records; any other uid
  needs a package, system has "android" (`DAEMON_USER=system`).
- [~] On the device (2026-10-13): the recording opens as system and runs for hours (wake word, arbitration, Home
  Assistant's pipeline), nothing of Alexa's records any more (its apps are off the partition), and it reopens itself
  after an audioserver restart ("capture delivers nothing, reopening"). The ASR pipeline's level: a talker at -41 to
  -44 dBFS, `micgain.c` adds +15 to +18 dB. Open: latency; whether our playback really is the echo canceller's
  reference (inferred from the HAL, not measured).
- [x] The front end's listening mode and wake word energies: behind `audio.h` now (`afe_listening`,
  `afe_arbitration`, `afe_stream_stopped`; LIPC on the Dots). Here transaction 3 of `audiosignalprocessor` through
  the firmware's own `/system/bin/service call` (`afe_parcel.c`: the parcel as `i32` words, the reply read back out
  of Android 7's Parcel/HexDump print, whose format strings are in this libbinder): 146, 114 + 23, 14 as above.
  `tests/unit/afe_parcel_test.c` (make unit). On the device (2026-10-13): the reply as printed here, the JSON's
  energies there (voice 9e6..4e8, ambient 8e4..2e5), both `service` runs done ~105 ms after the wake word (the Dots'
  LIPC pair: 150 ms).
- [x] Volume: behind `audio.h` (`vol_read`/`vol_write`: the mixer's props on the Dots). Here Android's own: the music
  stream's index (0..30) through IAudioService (setStreamVolume 3, getStreamMaxVolume 10, via the `service` tool as
  package "android"), kept in `state/volume`. Not a gain on each player: Amazon's audio stack put a track back to 1.0
  ~70 ms after it started, so sounds began at our level and jumped to full (the tester's Echo). Fire OS aliases
  system, voice call and TTS to the music stream (one index in `dumpsys audio`). The keypad is grabbed
  (`board.grab_keys`, EVIOCGRAB): the framework neither turns its volume too nor shows a panel, and the camera
  shutter's EV_SW on the same keypad no longer reaches Android. On the device: keys, Home Assistant and the page
  move it; a Bluetooth speaker gets its own index (AudioService keeps one per device). Separate volumes (main's #32):
  the index at the loudest kind, the others turned down in our PCM (`audio_*_gain`) by the Echo Dot 2's curve
  (`main.c` `vol_db`). Open: the Show's own curve (the index's steps plus the amplifier) is not measured, so a reply
  at 30 beside music at 60 is only about as loud as music at 30. `state/volume` through `statefile.c` (#46), written
  on the volume thread, not under `core_lock`.
- [~] Equalizer: `afe_eq_get`/`afe_eq_set` (audio.h), here `command` 29 / 21 (donut's LIPC `GET/SET_USER_EQ_INFO`;
  `ADJUST` 28, `RESET` 30). This libasp has the UserEq code and its JSON (`convertUserEqJSONToVector`, BASS/MIDRANGE/
  TREBLE), reached through a vtable, so its handler was not traced: the payloads are LIPC's strings (the JSON in for
  a set) and, for the get, REQUEST_ARBITRATION_JSON's convention (an int32 reply size in). On the device
  (2026-10-13): the get answers in that form (`{"bands":[{"name":"BASS","level":0},{"name":"TREBLE",...},
  {"name":"MIDRANGE",...}]}` in a 512-byte array, NUL-padded), the set returns status 0. Open: hearing a band move.
- [ ] No micRaw (`startCapture` refused on ship builds): `scripts/mic-compare.sh` cannot work.
- [~] Music Assistant (Sendspin, 2026-10-13): paired and plays, sync within ±10 ms over minutes, no underruns.
  Open: the first stream right after pairing stopped being heard after ~0.2 s (no error in hassmic's log; not
  seen again), and the player repeats ~170 frames per 10 s to stay in sync (0.35 %: the DAC's clock, or Android's
  buffering counted short in s_queued_us).

**Alexa, firewall, network**
- [x] Alexa off: `alexa-off.sh` disables `ALEXA_PACKAGES`, `SETUP_PACKAGES`, `UPDATE_PACKAGES` with `pm`, undone by
  `alexa-on.sh` and by `boot.sh` without `hassmic.conf`; checked against a fake `pm`.
- [x] Amazon's software off the device (the tester's Echo, 2026-10-13). `pm disable` does not control it: Fire OS
  enables System UI again at the next boot, and starts its persistent apps at boot even disabled (amazon.speech.sim,
  whad, NativeAccessorProxyServices ran with `enabled=2`), and much of it crashed without Alexa's speech service ("has
  stopped" dialogs). System UI also played stock's sounds for other apps (its NotificationPlayer: the privacy sound at
  full volume over ours) and threw without amazon.speech.sim ("SimClient was never got created"). So every Amazon app
  folder of `/system/app` and `/system/priv-app` (`pm list packages -f`: com.amazon.*, amazon.*, Ivona TTS, Arcus,
  the usage stats proxy, Sidewalk, System UI and Amazon's Settings: 120) is taken off the partition (`STOCK_APPS_OUT`,
  `sysinstall.sh`: moved into `/system/hassmic-removed` at install and with every update that becomes the factory copy,
  back at uninstall). Not the shared-library providers of `/system/etc/permissions` (amazon.jackson-19,
  com.amazon.communication, com.amazon.device.sync.sdk.internal, com.amazon.diode.eventsender,
  DeviceMessagingAndroid[Internal]SDK, DeviceSoftwareOTAContracts, MetricsApi, MinervaApi, RealtimeRegistryDeviceSDK):
  with them gone too, FusedLocation (in system_server, NoClassDefFoundError) and Settings failed, system_server
  restarted 12 times and later Package Manager was gone. Home is hassmic's own (`tools/mkhome.py`,
  `HOME_APK`): an APK without code declaring `android.app.Activity`, black, MAIN/HOME, `directBootAware` (without it
  "No home screen found" before user 0's unlock, and `sys.boot_completed` never came). Result: boot in 30 s, one Amazon
  process left (`amazonfiled`, native), ~550 MB of 1 GB free, no dialogs; `neo-service-coordinator` still panics once
  at boot (binder registration), no dialog. Before the install, after a wipe, Amazon's setup wizard is in front:
  `alexa-off.sh` at the setup's lockdown step disables it and sets `device_provisioned`/`user_setup_complete`.
  Open: uninstall puts the apps back but `alexa-on.sh` runs before that (packages missing then): disabled ones may
  stay disabled.
- [x] Firewall: `lockdown.sh` as on the Dots, plus `FW_KEEP` (DHCP answers; no stock `firewall.sh`); the updater's uid
  from `packages.list` for stock-online.
- [x] Security: stock's `init.checkers.rc` sets `service.adb.tcp.port 5555` on boot, so with our ramdisk
  (`ro.adb.secure=0`, adbd in `su`) every boot opened a root shell without authentication on the network until
  `lockdown.sh` (the firewall service, also `on boot`) closed it. Found on the tester's Echo (2026-10-13): lockdown.sh
  closing it restarted adbd, whose process group took the setup's `adb shell` along. `mkbootroot.py` now leaves that
  line out (a comment in its place), so adb over Wi-Fi opens only through hassmic (`adb_gate`); the setup still
  closes it before the lockdown for an Echo on the older boot image.
- [~] On the device, security-relevant (2026-10-13): `hassmic_out` first in OUTPUT with netd running and INPUT policy
  DROP; `lockdown.sh` re-applies once at boot (netd's own setup) and holds from there, for hours. A root shell (not
  hassmic's group) gets no connection to 1.1.1.1:80; hassmic's traffic and mDNS pass. Amazon's daemons are mostly off
  the partition (`amazonfiled` is left), the updater with them. Open: a DHCP renewal seen through.
- [x] Wi-Fi join at install and the settings page's "Switch network": through WifiService, with
  `scripts/device/wifictl.dex` (`tools/mkwifictl.py`). What stock's own setup apps do (a WifiConfiguration through
  the IWifiManager binder), from root as `CLASSPATH=…/wifictl.dex app_process / Wifictl …`; reflection only, so
  the dex is free of android.* type references. Checked on the PC, since qemu-user cannot run the firmware's ART
  (dalvikvm32 dies in its start-up): `mkwifictl.py --check` runs a type-flow check over every method, the subset of
  ART's method verifier the dex uses; `--framework=firmware/checkers/rootfs` resolves every class, method and field
  it names, and every call its reflection makes, in the dex files of 8149's `boot*.oat` (KeyMgmt.WPA_PSK's value
  too); `tests/unit/wifictl_run.py` interprets the dex against a fake WifiService, verb by verb; and
  `tests/fake_web_wifi.py` drives wifi.sh against a fake framework. dexdump alone checks structure only: the first
  dex passed it with wrong opcodes, parameters read from the wrong registers and `java/lang/List`, and would not have
  loaded. WifiService keeps one network per name and security, so an add of a saved name changes that entry in
  place: wifi.sh reads the stored keys first (`wifictl keys`, getPrivilegedConfiguredNetworks) and puts the old one
  back when the switch fails. `setWifiEnabled(null, true)` from root: 8149's `WifiServiceImpl` uses the caller's
  package name only for the permission-review consent dialog (`startConsentUi`), and review is off
  (`Build.PERMISSIONS_REVIEW_REQUIRED` unset, framework-res `config_permissionReviewRequired` false); otherwise it
  checks CHANGE_WIFI_STATE, which uid 0 has. The setup's join (`scripts/wifi-join.sh`) runs against a fake adb and
  the same fake framework in `tests/fake_wifi_join.py`. On the device: the setup's join connected and got its
  address (the network's entry says `creatorUid` 0: wifictl's). Open: the page's "Switch network" on the device. The
  screen fallback is gone with Amazon's Settings: TWRP and the firmware's own setup are the way back.

**Buttons, lights, sounds**
- [x] No action button: both volume keys pressed together (within 400 ms) and let go within a second are its press
  (`board.action_combo`, `buttons.c`; held 2 s they still pair for arbitration): the page's login, adb, Improv's
  authorization, answering a Drop In, and Home Assistant's Button event `action`. A pair moves no volume and fires no
  event of its own (main's rule since the volume steps on release: a pair is not two presses). The page words its
  texts for it (`hello.action`, `act()` in `web/app.js`); no reset by holding (the page's or Home Assistant's
  button). `tests/unit/buttons_test.c` (make unit).
- [x] Input devices by name, not event number (`board.c` `name:gpio-keys`, `name:gating`; `buttons.c` looks them up
  with EVIOCGNAME): the kernel's five DTBs give gpio-keys no label (so the platform name) and the gating driver its
  own input device, "mute" = KEY_POWER (0x74), on which `privacy_reader` reads the latch at any event. `getevent -il`
  on the device: "gating" (the platform device is `amazon-gating`), "gpio-keys", besides "mtk-kpd", "ACCDET",
  "goodix-ts", "amazon_touch", "m_alsps_input", "hwmdata"; both of ours grabbed.
- [x] Light sensor (2026-10-13, on the Echo): `board.light_sensor` = `/sys/bus/platform/drivers/als_ps/lux`, one of the
  driver attributes Amazon's stk3x1x driver puts on the platform driver `als_ps` ("94 lux"). `stk_als_lux_show` reads
  the ALS data registers over I2C at every read and scales them by the factory calibration (`/proc/idme/alscal`,
  0x4bc = 1212 ADC counts for 400 lux): code 284 gave 93-94 lux, the same as Android's light sensor in `dumpsys
  sensorservice` (which system_server's AmazonLedBrightnessControllerService keeps active, so the chip measures).
  Home Assistant gets "Illuminance"; "LED auto brightness" stays unlisted, since that is ledcontroller's engine and
  the band has none (`core_led_has_auto`). Not every attribute there is a reading, found the hard way: reading
  `cali_Light` calibrates (the light now taken as 400 lux) and writes `/data/als_cal_data.bin`, which wins over idme
  from then on; reading `ir` turns IR measurement on in the STATE register and, when it times out ("ir data is not
  ready for 300ms"), leaves ALS stopped until a reboot (the driver's own `enable` cannot restart it: its enable path
  writes STATE 0). Undone on the tester's Echo: 1212 written back to `als_cali_value` (which only sets the value),
  the file removed, rebooted. `status` prints `[ALS= 0]` even while ALS measures.
- [x] Light ring as a band along the bottom of the screen (2026-10-13, on the tester's Echo). No LED ring and no
  `ledctrl`; stock shows Alexa's attention on the screen instead ("voice chrome": SpeechInteractionManager.apk,
  BambooAttentionSystemVisualsComponent, Lottie compositions of 960x200, a 10 px line along the bottom edge drawn over
  the app in front, TYPE_SYSTEM_ERROR overlay; listening, thinking, mic off and calls only, never turned to the
  talker). Ours keeps the band for itself instead, so any app can be the screen's UI: `ring_band.c` (a second
  implementation of `ring.h`, `RING` in device.mk) draws every pattern of the core as the Dots' ring unrolled (12
  virtual LEDs blended across the width, the talker at the centre; timings and colours as donut's led-resources play
  them), and Home Assistant's "Light ring" light. `scripts/device/ringbar.dex` (`tools/mkringbar.py`, assembled by
  `tools/dexasm.py`, shared with mkwifictl.py) runs as `app_process`: it sets the window manager's overscan for the
  band (`IWindowManager.setOverscan` in the panel's natural orientation: on this 480x960 panel shown as 960x480,
  rotation 1, the natural left edge is the bottom; every app then gets 960x470) and puts a SurfaceFlinger layer of its
  own over it (SurfaceControl, layer 2000000000), drawing the frames hassmic sends on its stdin. Seen on the device:
  "ready 960 10", the layer composed on the panel's 10 px strip. Setting "Light band" (0..40 px, default 10; 0 gives
  the screen back), `wm overscan reset` on uninstall (`alexa-on.sh`, `SCREEN_BAND`). Screen power as stock's
  EnvironmentContextService: a pattern wakes the screen (IPowerManager wakeUp = transaction 9, userActivity 8,
  isInteractive 12 on 8149, through the service tool), `screen_off_timeout` 5 min, 1 min with the mic off or in Do
  not disturb (stock also 1 min at night: no night mode here). `tests/unit/ring_band_test.c`, `ringbar_run.py`.
- [x] Not done, on purpose: turning the listening animation towards the talker. The front end has the beam: libasp's
  BeamReporter sends `what=1` (BEAM_DIRECTION, the vector (sin, 0, cos) of the beam's angle) to every
  `com.amazon.asp.IAudioEventListener` registered with `audiosignalprocessor` (transaction 1, a strong binder; no
  permission check), on every change while the ASR pipeline runs; nothing polled has it (not REQUEST_ARBITRATION_JSON,
  not the HAL's parameters). A listener in ringbar.dex got them on the device (2026-10-13), but they say little: 7
  fixed beams from 2 mics (`AFE.cfg` FixedBeamFormerV2), mapped through `{4, 2, 1, 0, 9, 10, 12}` and dropped from
  N = 7 on, so only 4 ever arrive (0, 51, 103, 206 degrees); and with the user talking 15 s each from the left, the
  front and the right the beam switched several times a second wherever they stood, 0 degrees most of the time in all
  three (8, 10 and 11 s of 15). Stock's Show never turns its bar either; the band's listening cyan stays centred.
- [x] Earcons: none in the firmware's file system; `sounds.c` reads them straight out of the stock apps
  (`board.c` `earcon_zip`: 12 of the 13 sounds, every one stored uncompressed, as aapt keeps audio), nothing copied;
  `/data/local/hassmic/earcon/` (`earcon_dir`) wins for own sounds. All decode from the unpacked firmware
  (`make unit`, `tests/unit/sounds_test.c`); the setup beacon (Identify) stays the built-in tone. On the device:
  they play, read out of the APKs as the daemon's user.

**Other features**
- [~] Speaker gain (`board.c` `speaker_gain`, `spkgain.c`): the codec is a Realtek RT5616 (`CONFIG_SND_SOC_RT5616`;
  `/system/etc/audio_device.xml`, the file `audio.primary.mt8163.so` reads, says "RT5616 DAC", "LO out"); its line out
  feeds the speaker amplifier (`ext_speaker_output` switches "OUT Playback Switch" and `Ext_Speaker_Amp_Switch`).
  "OUT Playback Volume": reg 0x03, both channels, inverted, 0..39 with TLV -46.5 dB + 1.5 dB a step, read from the
  `soc_mixer_control` and TLV of checkers' own kernel (decompressed from `boot.img`): 29 = -3 dB, 31 = 0 dB, 39 = +12
  dB. The XML sets 29 among its initial settings and no path or other firmware file names it, so it should stay from
  the HAL's start on (the watcher puts ours back after an audioserver restart). Ten steps above stock, the page offers
  +12 dB (`speaker_gain_steps` 2 per `speaker_gain_db` 3: a whole dB takes the nearest step, +1 and +2 dB are both
  +1.5). `/dev/snd/controlC0` is system:audio; the daemon has group audio and runs in `su`. On the device: that 29 is
  really what the speaker plays at (`tinymix 'OUT Playback Volume'`, or hassmic's log line "speaker gain: ... now
  N"), and how loud before the line out or the amplifier clips.
- [x] Amazon downloads (`dha.c`, 2026-10-13): the attestation key is behind `fireos-dha` (init: user keystore, groups
  keystore drmrpc), registered as `amazon.security.dha` (`fireos.dha.IFireOsDha`). Transactions from `BpFireOsDha`:
  1 sign(byte[]), 2 verify, 3 getDhaPublicKey, 4 getDhaHmac, 5 getDhaCertificateChain, 6 getDhaVersion, 7/8 unlock
  code and version. Asked with `/system/bin/service call` on the tester's Echo, as system (hassmic's user; no
  permission check stops it, no extra groups): version 1 (DhaService reads the HAL device's version), an RSA-2048
  SubjectPublicKeyInfo with e 65537, a 32-byte HMAC, no certificate chain (exception -7); sign answers RSASSA-PSS,
  SHA-256, 32-byte salt over the digest it is given (verified on the PC against the key), as the Dots' HAL does.
  Amazon's MAP here is Java (`com.amazon.imp`, `kcpsdk.auth`; its APK carries an unquickened `classes.dex`, decompiled
  with androguard): `FireOsDhaSigner` builds drvV1 for version 1 and drvV3 (ES256, x5c from the chain) for 2. Its
  drvV1 is not the native MAP's text: org.json keeps the order of the puts (`jwk` = kty, n, e, mac; payload = dat,
  dev, cust; dev = typ, dt, dsn, [ds], cpuid), `n` and `mac` are padded Base64 (flags 10), `cpuid` is the `Serial`
  line of `/proc/cpuinfo` (`8b4c188d83c47b66` here; left out when missing), the date UTC, `dsn` the registration's
  serial, `ds` only in a registration by device secret (idme `mac_sec` is `0` here anyway). `dha.c` builds exactly
  that when there is no HAL but the service answers version 1 (`tests/unit/dha_jwt_test.c --service`, against a fake
  of the tool's output); on the Echo its token's signature verifies against the key in it.
  Against the real Amazon (2026-10-13, amazon.de, the settings page): register answered 401 until the code was
  entered, then the tokens ("Julian's Echo Show"); DAVS handed out echo de-DE and the sound detection model (aed
  de-DE), both installed by root. Whisper refused: `HTTP 403 Request not allowlisted for device Type A4ZP7ZC4PI6TO
  for artifact type alexa-hybrid, artifact key whisper-static` — a device type/key allowlist, not our filters (the
  AHE here asks with the Dots' filters: `ecid` 6, `modelClass` odie-litespeed of `/system/etc/ahe.config.json`, and
  has a "DAVS request denied for artifactKey - deviceType pair" path); stock never had one either
  (`/data/alexahybrid/files/AmModel` empty). The Show's whisper mode is then the cloud's (docs/re-whisper.md). The
  Dots' model can be copied over (same libpryon): untried here.
- [~] Bluetooth proxy and A2DP sink (2026-10-13): Android's stack (app/Bluetooth) is off the partition and
  `bluetooth_on=0`; the MT7668 behind `btmtksdio` speaks H4 on `/dev/stpbt` as the Dots' combo chips do (hciscan:
  reset, address, 315 LE adverts in 8 s). hassmic: "controller ready, 7C:D5:66:97:F7:7B, ACL 32 x 251", A2DP sink
  ready (SBC, AAC, aptX HD, aptX); a Pixel paired and played (aptX HD). Home Assistant's proxy subscribes and
  switches the scan between active and passive (`ble.c` scans only for a subscriber). Open: devices seen in Home
  Assistant through it.
- [x] Bluetooth speaker (A2DP source), on the tester's Echo with a PC (BlueZ/PipeWire) as the speaker (2026-10-13):
  heard clearly, back on the Echo cleanly. No remote submix needed: 8149 has AOSP N's `audio.a2dp.default.so` (not
  byte-identical to donut's, Amazon's `bt_a2dp_hw_fos` additions, same protocol) and its policy the `a2dp` module
  (`a2dp_audio_policy_configuration.xml`, 44.1 kHz stereo), so btout.c serves its abstract `.a2dp_ctrl`/`.a2dp_data`
  sockets as on the Dots (audioserver may connect to su: `sepolicy.rules`), and the route is AudioPolicy's: hassmic
  tells `media.audio_policy` an A2DP device is there, as AudioService would for Android's Bluetooth app
  (`audio_android.c` `audio_a2dp_route`). IAudioPolicyService is a native interface: transaction 1
  setDeviceConnectionState(device 0x80, state, address, name; the strings as `writeCString`, built in `afe_parcel.c`
  `svc_cstr`), 2 getDeviceConnectionState(device, address) -> state; status -38 when the HAL's CHECK_READY fails (it
  tries 3 times, 250 ms apart) or the device is already in that state. The media strategy, every player of ours, then
  plays on A2DP alone. Measured: a move takes 410–520 ms (two `service` runs; AudioFlinger opens the HAL's output
  inside it); AudioFlinger puts the output in standby (SUSPEND) 3 s after the last sound and STARTs it with the next
  (the wake sound 66 ms after the wake word, AVDTP START included), where the Dots' mixer plays on; audioserver killed
  while routed: the policy forgets the device, hassmic notices the HAL's control socket close and moves it again (1.6
  s from the kill); a route left by a hassmic that died is taken back at start (`state/a2dp-route`). AudioService
  keeps a volume index per device and sets the one the music stream plays on: the volume is set again after each move.
  A pop on the Echo's speaker when the speaker went away: "Bluetooth disconnected" and its announcement start ~30 ms
  after the link drops, on the dead A2DP output, and AudioFlinger moved them mid-sound to the speaker when the route
  went back ~200 ms later; the route moved alone, with nothing playing, made no sound. Players opened while the route
  moves now wait for it (`route_settle`, 1.5 s at most): no pop since.
- [x] Wi-Fi motion (2026-10-13, on the Echo): `hassmic_rcpi4m.c` builds for 32-bit ARM too. The driver
  (`/system/lib/modules/wlan_mt76x8_sdio.ko`, the gen4m driver as on donut, ARM, not stripped) calls nicRxFillRFB once
  at nicRxProcessDataPacket+0x64; with 4-byte pointers SW_RFB_T has ucGroupVLD at +16 and prRxStatusGroup3 at +32
  (nicRxFillRFB stores the 24-byte group 3 there, and nicRxGetRcpiValueFromRxv reads RCPI0 at its +12, as on donut).
  The `bl` is rewritten by the kernel's `patch_text` (present for KGDB, found through kallsyms; module text is
  writable, no DEBUG_SET_MODULE_RONX). Built against kernel.org 4.9.77 with the IKCONFIG and AOSP's arm-eabi-4.8:
  Amazon used Linaro GCC 6.3-2017.05, which Linaro no longer publishes; vermagic `4.9.77 SMP preempt mod_unload ARMv7
  p2v8` and `struct module` (0x200, init at 0xdc) are the kernel's. On the Echo: "nicRxProcessDataPacket+0x64 (call
  to nicRxFillRFB) hooked", RCPI 79-83 (-70 to -68 dBm), KIND 0x14005/0x14006 (VHT 80 MHz, MCS 5/6), hassmic "through
  the kernel module", Home Assistant's motion sensor went on and off.
- [x] Found on the way: `alexa-off.sh`'s `wpa_cli … -i p2p0 p2p_group_remove` once never returned (no group up, the
  socket there), and the satellite's start waited with it: no hassmic after an update. `timeout 5` now.
