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
- Thermal: `mtktscpu`. Light sensor: STK3x1x on MediaTek's sensor framework (`/sys/class/misc/m_alsps_misc/`, input
  device `m_alsps_input`), no lux file.
- LEDs: none worth driving (the red/green/blue nodes are mode 0); the mic-off LED is the gating hardware's.
- Earcons: no `/system/local/share/earcon`. Wake sound inside SpeechInteractionManager (`res/-E.ogg` =
  `raw/ban_ui_wakesound`, Vorbis; `res/7E.mp3` = `ful_ui_wakesound_hybrid`; names obfuscated, mapped through
  `resources.arsc`); privacy and volume sounds in KnightSystemUI (`res/raw/kni_controls_privacy_mode_{on,off}.mp3`,
  `kni_controls_volume_adjust.mp3`); Bluetooth in KnightSettings (`kni_system_bluetooth_bt_{connected,disconnected}.mp3`),
  the comms sounds in `com.amazon.comms.multimodaltachyonarm` (`ful_comms_*.mp3`, the five Drop In uses). Every one
  stored uncompressed in its APK. No discovery (setup beacon) sound.
- DHA: no `libacehal_dha.so`; the attestation key is behind the `fireos-dha` binder service (`libdha-aidl.so`), so
  `dha.c` (Amazon downloads) needs another way in.

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
[x] done (on the PC only, unless it says otherwise), [ ] open. Nothing has run on an Echo Show yet.

**Unlock, install, boot**
- [x] Unlock: amonet-checkers v2.0.1 (public; its lk/tz are 8149's own). Root: 8149's own `boot.img` with root adb,
  `verify` out of the fstab, the policy patched (`sepolicy.rules`; the XDA `boot-root.img` is NS6570's).
- [x] Installer: `scripts/install-boot.sh` (`INSTALL=boot`), boot then system partition only; `hassmic.rc` (firewall
  `on boot`, satellite at `sys.boot_completed`). PC half checked end to end, device half untried.
- [ ] First run on a device: the boot image boots, adb is root in `su`, `otatool remount` gets `/system` writable,
  init starts both services from `/system/etc/init`.
- [x] Guided setup (`devices/checkers/setup.sh`): every step written, dry-run walks them all; the first run on a
      device is the try-out (UNTESTED=1 until then).
- [x] CI: checkers in the build matrix of `.github/workflows/build.yml` (its bundle installs now, on paper).

**Audio**
- [x] `audio_android.c` (OpenSL ES): VOICE_RECOGNITION capture, one player per stream, mixed by AudioFlinger.
- [x] Recording rights read from the firmware (`libserviceutility.so` `recordingAllowed`): root records; any other uid
  needs a package, system has "android" (`DAEMON_USER=system`).
- [ ] On the device: the recording really opens as system; only one capture at a time (Android 7 hands the input to
  the newest AudioRecord, so nothing of Alexa's may still record); the ASR pipeline's level for `micgain.c`; latency;
  whether our playback really is the echo canceller's reference (inferred from the HAL, not measured).
- [x] The front end's listening mode and wake word energies: behind `audio.h` now (`afe_listening`,
  `afe_arbitration`, `afe_stream_stopped`; LIPC on the Dots). Here transaction 3 of `audiosignalprocessor` through
  the firmware's own `/system/bin/service call` (`afe_parcel.c`: the parcel as `i32` words, the reply read back out
  of Android 7's Parcel/HexDump print, whose format strings are in this libbinder): 146, 114 + 23, 14 as above.
  `tests/unit/afe_parcel_test.c` (make unit). On the device: that the reply looks as printed here, the JSON's
  energies, and the time two `service` runs take inside the arbitration window (the Dots' LIPC pair: 150 ms).
- [x] Volume: behind `audio.h` now (`vol_read`/`vol_write`: the mixer's props on the Dots). Here a gain on each
  OpenSL ES player (`SLVolumeItf`, 0.4 dB per step, 0 silent), kept in `state/volume`; Android's own stream volumes
  (voice call, system, music) go to their maximum at every satellite start (`main.sh`: `wifictl.dex volume-max`,
  IAudioService.setStreamVolume as package "root", which 8149's AudioService notes with AppOps for uid 0; no
  `media volume` in Android 7.1's media_cmd). The keypad is grabbed (`board.grab_keys`, EVIOCGRAB) so the framework
  does not turn its volume too, and shows no volume panel. On the device: the levels, and that nothing else (the
  screen's settings) moves Android's stream volumes afterwards; the camera shutter's EV_SW on the same keypad no
  longer reaches Android either.
- [~] Equalizer: `afe_eq_get`/`afe_eq_set` (audio.h), here `command` 29 / 21 (donut's LIPC `GET/SET_USER_EQ_INFO`;
  `ADJUST` 28, `RESET` 30). This libasp has the UserEq code and its JSON (`convertUserEqJSONToVector`, BASS/MIDRANGE/
  TREBLE), reached through a vtable, so its handler was not traced: the payloads are LIPC's strings (the JSON in for
  a set) and, for the get, REQUEST_ARBITRATION_JSON's convention (an int32 reply size in). On the device: whether
  the get answers in that form.
- [ ] No micRaw (`startCapture` refused on ship builds): `scripts/mic-compare.sh` cannot work.
- [ ] Bluetooth speaker stream (`bt_open` returns -1): Android's stack would route an AudioTrack to a speaker itself.

**Alexa, firewall, network**
- [x] Alexa off: `alexa-off.sh` disables `ALEXA_PACKAGES`, `SETUP_PACKAGES`, `UPDATE_PACKAGES` with `pm`, undone by
  `alexa-on.sh` and by `boot.sh` without `hassmic.conf`; checked against a fake `pm`.
- [ ] On the device: which of those packages the screen needs (`com.amazon.bishop` is in the list; the launcher is
  `com.amazon.paladin`, left alone); what the screen shows with Alexa off.
- [x] Firewall: `lockdown.sh` as on the Dots, plus `FW_KEEP` (DHCP answers; no stock `firewall.sh`); the updater's uid
  from `packages.list` for stock-online.
- [ ] On the device, security-relevant: the invariant (Amazon's daemons only local addresses, the updater never out)
  holds with netd running, netd's own changes to OUTPUT and INPUT included; DHCP renewals and mDNS pass.
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
  the same fake framework in `tests/fake_wifi_join.py`. On a device still open: whether it really connects and gets its address, and the
  screen (`am start -a android.settings.WIFI_SETTINGS`) stays the fallback.

**Buttons, lights, sounds**
- [x] No action button: both volume keys pressed together (within 400 ms) and let go within a second are its press
  (`board.action_combo`, `buttons.c`; held 2 s they still pair for arbitration): the page's login, adb, Improv's
  authorization, answering a Drop In. Each press also moves the volume a step, one up and one down. The page words its
  texts for it (`hello.action`, `act()` in `web/app.js`); no reset by holding (the page's or Home Assistant's
  button). `tests/unit/buttons_test.c` (make unit).
- [x] Input devices by name, not event number (`board.c` `name:gpio-keys`, `name:amazon-gating`; `buttons.c` looks
  them up with EVIOCGNAME): the kernel's five DTBs give gpio-keys no label (so the platform name) and the gating
  driver its own input device, "mute" = KEY_POWER (0x74), on which `privacy_reader` reads the latch at any event.
  `amazon-gating` is the only name the kernel has for that device: confirm with `getevent -il`.
- [ ] Light sensor: STK3x1x behind MediaTek's sensor hub, no lux file (`light_sensor` empty).
- [ ] No LED ring and no `ledctrl` (`main.c` then leaves LEDs off): listening/thinking/volume/identify feedback
  would go on the screen; nothing does that yet.
- [x] Earcons: none in the firmware's file system; `sounds.c` reads them straight out of the stock apps
  (`board.c` `earcon_zip`: 12 of the 13 sounds, every one stored uncompressed, as aapt keeps audio), nothing copied;
  `/data/local/hassmic/earcon/` (`earcon_dir`) wins for own sounds. All decode from the unpacked firmware
  (`make unit`, `tests/unit/sounds_test.c`); the setup beacon (Identify) stays the built-in tone. On the device:
  the APKs readable by the daemon's user (0644, and `su` is permissive), and how they sound.

**Other features**
- [ ] Amazon downloads (`dha.c`): no `libacehal_dha.so`; the key is behind the `fireos-dha` binder service.
- [ ] Bluetooth proxy and A2DP (`-B` in `DEFAULT_ARGS` for now): only with Android's Bluetooth stack off.
- [ ] Wi-Fi motion: the gen4m driver is 32-bit here; `hassmic_rcpi4m` rewrites an arm64 `bl`, needs an ARM variant.
