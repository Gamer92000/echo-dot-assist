# Echo Show 5 1st gen (`checkers`): what the firmware says

Worked out from the firmware only, 2026-10-07; nobody has run hassmic on an Echo Show yet, and no unlock is known.
Firmware: `update-kindle-checkers-NS65741_user_8149_0013222532484.bin` (sha256 `f96fcfa1…73cfa3`), Fire OS 6574.1
(NS65741/8149), the same release as donut's. Model H23K37, `ro.product.model=AEOCH`, `ro.product.device=checkers`.

## Unpacking

Unlike the Dots, the update is a block OTA without `payload.bin`: `system.new.dat` + `system.transfer.list` (version 4),
`boot.img`, `images/{lk.bin,tz.img,preloader.img}`. Laid out like donut's `firmware/<codename>/`:

```sh
unzip firmware/checkers/update-kindle-*.bin -d firmware/checkers/
# system.new.dat -> images/system.img: replay the "new" ranges of system.transfer.list (sdat2img)
mkdir -p firmware/checkers/rootfs/system
debugfs -R "rdump / firmware/checkers/rootfs/system" firmware/checkers/images/system.img
# boot.img (Android header v0, page 2048): the ramdisk (gzip cpio) unpacks into firmware/checkers/rootfs/
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

1. **Unlock and root.** Nothing known. Everything else waits for it.
2. **Audio:** `src/hassmic/audio_android.c` (OpenSL ES, written, untried): capture VOICE_RECOGNITION 16 kHz mono,
   one player per stream. Open: whether system uid gets the recording, the ASR pipeline's level, latency. Then a binder
   client for `SET_LISTENING_MODE` / `REQUEST_ARBITRATION_JSON` in place of `lipc-set-prop`, and volume (the Dots set
   it in the mixer through LIPC; here Android's stream volumes).
3. **Stopping Alexa:** `pm disable` of the packages above; `hassmic.rc` on `sys.boot_completed=1`.
4. **Firewall:** `lockdown.sh` against netd's chains, the update app's uid cut off. Security-relevant: the invariant
   (Amazon's daemons only reach local addresses, updates never get out) has to be proven again here.
5. **Wi-Fi switch:** through Android's WifiService (`wifi.sh` assumes a free wpa_supplicant).
6. **Settings page login and adb:** no action button; a volume key combination or the touchscreen instead.
7. **Earcons** extracted at install; **screen**: left to stock at first.
8. Optional: Bluetooth proxy and A2DP once Android's stack is off; Wi-Fi motion needs an ARM32 `hassmic_rcpi4m`.
