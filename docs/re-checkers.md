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
- Also `NOTIFY_PLAYBACK_STATUS` 0, `NOTIFY_TTS_STATUS` 1, `NOTIFY_MIC_MUTED` 4, `SET_WAKEWORD_METADATA`.

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
  `kni_controls_volume_adjust.mp3`). No Bluetooth or discovery sounds.
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
- [ ] The front end's listening mode and wake word energies: `main.c` calls `lipc-set-prop` / `lipc-get-prop`
  (`LASP_CMD_SET_LISTENING_MODE`, `LASP_CMD_REQUEST_ARBITRATION_JSON`, `SET_WAKEWORD_METADATA`), which this firmware
  does not have. Needs a small binder client for `audiosignalprocessor`, transaction 3 (above: 0x92, 0x17).
  Without it the cancellers adapt to the talker after ~1.5 s (as on donut before `listening()`), and arbitration
  falls back to the own SNR score.
- [ ] Volume: `set_prop_volume` runs `audio_manager_set_prop` (the Dots' mixer); here Android's stream volumes
  (`AudioManager`/`media volume`, or per-player gain in `audio_android.c`).
- [ ] Equalizer: `LASP_CMD_GET/SET_USER_EQ_INFO` through LIPC, absent here (the binder `command` may take it).
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
  `scripts/device/wifictl.dex` (`tools/mkwifictl.py`, checked by `tests/fake_web_wifi.py` against a fake
  framework). What stock's own setup apps do (a WifiConfiguration through the IWifiManager binder), from root as
  `CLASSPATH=…/wifictl.dex app_process / Wifictl …`; reflection only, so the dex is free of android.* type
  references. On a device still open: whether it really connects and gets its address (the fake framework says so),
  and the screen (`am start -a android.settings.WIFI_SETTINGS`) stays the fallback.

**Buttons, lights, sounds**
- [ ] No action button: the settings page's login (`web_approve()` in `on_action`) and opening adb over Wi-Fi from the
  page both wait for one. A volume key combination or the touchscreen instead.
- [ ] `board.c` values from the running device: keypad event number (`getevent -il`), the privacy driver's input
  device (its "mute" key is KEY_POWER, 0x74, not KEY_MUTE: `buttons.c` would miss it, the once-a-second read of
  `amazon-gating/state` still catches it), light sensor path.
- [ ] No LED ring and no `ledctrl` (`main.c` then leaves LEDs off): listening/thinking/volume/identify feedback
  would go on the screen; nothing does that yet.
- [ ] Earcons: none in the firmware's file system; the sounds are in SpeechInteractionManager and KnightSystemUI
  (`res/raw/...`): extract and convert into `/data/local/hassmic/earcon/` (`board.c` `earcon_dir`) at install.

**Other features**
- [ ] Amazon downloads (`dha.c`): no `libacehal_dha.so`; the key is behind the `fireos-dha` binder service.
- [ ] Bluetooth proxy and A2DP (`-B` in `DEFAULT_ARGS` for now): only with Android's Bluetooth stack off.
- [ ] Wi-Fi motion: the gen4m driver is 32-bit here; `hassmic_rcpi4m` rewrites an arm64 `bl`, needs an ARM variant.
