# Findings: Echo Dot 3 (`donut_puffin`, NS65741 / Fire OS 6574.1)

Sources: static analysis of the OTA, plus experiments on the PC under `qemu-arm` where stated.
Nothing here has run on a device yet. Detailed notes: [re-pryon.md](re-pryon.md), [re-platform.md](re-platform.md).

## Verdict

Amazon's audio front end can be kept as is. The Alexa client (`PuffinApp`) has to be **replaced**, not tapped:
the processed mic stream has one consumer, and the wake word runs inside `PuffinApp`.

## Audio stack

| Piece | Role |
|---|---|
| `/system/bin/mixer` (user `audio`) | Owns the audio HAL. Runs the whole front end in-process via `libasp.so` on the application processor. Starts on `service.nvram_init=ready`, independent of Alexa. |
| `/system/vendor/etc/audio-algorithms/{asp.cfg,AFE.cfg}` | Front-end config, plain JSON. ASR path: mic calibration (`/proc/idme/miccal.*`) → 80 Hz HPF → AEC_V2 → FixedBeamFormerV2 (8 beams) → beam merger → VAD → gain. In: 6 ch 16 kHz pcm32. Out: **16 kHz mono s16**. AEC reference: mixer's own 48 kHz stereo post-mix playback. |
| `libmixerAPI.so` | Plain C client API. Header: [`src/include/mixer_api.h`](../src/include/mixer_api.h). |
| `shmd` | Shared-memory broker used underneath the API. |
| `/system/bin/PuffinApp` (user `puffin`) | AVS Device SDK client. Holds `micAsr`, runs wake word via `libpryon.so`. |
| `ledcontroller` | LED ring. Also the thing that starts Alexa: sets `com.amazon.puffin.PUFFIN_START=1` when boot animation `anim_start_phase2` ends (0x1c026). That property starts `puffin`, `puffinmrmd`, `dacd`, `smarthomed`. |

## Mixer client API (recovered by disassembly)

- Reference client: `BTSinkPlayer` (30 KB). Calls `MixerOpenRec("btA2dp")` and `MixerOpenPlay(rate, ch, bits, "Music")`.
- `MixerOpenRec(type)` = `MixerOpenRecCh(type, 1)`. Accepted types (strcmp table 0xdadc–0xdb60): `btA2dp btSco micHfp micPstn lineIn ultraSound tapDetect micMultiChAsr fhtSsl loopback`, with `micAsr` as the default path. Block size 1600 bytes per channel (50 ms).
- Opening creates a `mixer::DataTrans` ring, then writes a stream descriptor into `/data/mixer_streams/` (mode 0777) that the daemon picks up.
- `MixerGetBufRec(h, int *status, unsigned *bytes)` → block pointer or NULL, waits up to 1500 ms. `MixerReleaseBufRec(h)`.
- `MixerOpenPlay(rate, ch, bits, type)`; rate 4000–96000. Types: `Music Alarm Silent TTS Earcon Voip`.
- `MixerGetBufPlay(h, int *status, unsigned *capacity)`, `MixerReleaseBufPlay(h, bytesWritten)`, `MixerDrain`, `MixerFlush`, `MixerPause`, `MixerResume`, `MixerClose`.
- **Single consumer:** `mixer` logs `Mixer_Record:AddNewOutput:previous ASR instance exists:force clean up`.
- Mic mute: `mixer` reads LIPC `com.doppler.buttond` property `muteState` (`libaudioCtrl.so` 0x1a58c). No Alexa involved.
- LIPC property `AllowMic` on `com.doppler.audiod` gates recording. Default without PuffinApp unknown — first thing to check if capture is silent.

## Listening mode (libasp.so, disassembly + device, 2026-09-30)

The front end has to be told when a command is being spoken. Without it a sentence is cancelled as interference after
~1.5 s (captures: micAsr 0-3 dB over its floor for the rest of a quiet sentence, micRaw 8-10 dB; with the flag micAsr
within 1 dB of micRaw over 4 s).

- The flag: one byte in the AFE object (`GenericAFE` +0x11d7), set by the method that logs `Utterance start detected`
  (0x77ed0, vtable +0xc4), cleared by `Utterance end detected` (0x77eec, vtable +0xc8). Nothing else writes it: no
  timeout.
- Three ways to set / clear it, all LIPC properties of `com.doppler.lasp`:
  - `LASP_CMD_SET_LISTENING_MODE` (Int, 1 / 0; ASP command 146). What hassmic uses. Also shows as `InListeningMode` in
    `LASP_CMD_GET_ASP_DEBUG_INFO`.
  - Reading `LASP_CMD_REQUEST_ARBITRATION_JSON` sets it (handler at 0x15c5c: utterance start, fill the reply, "Start
    AFE Diag"). **This is stock's path**: PuffinApp reads it after every wake word for the cloud's device arbitration.
    Reply: `{"sequenceID":0,"voiceEnergy":8717,"ambientEnergy":62048,"voiceEnergy_BToA":..,"ambientEnergy_BToA":..,
    "PGA_Gain":12.00,"ADC_Gain":0.00,"AFE_Input_Gain":12.00,"AFE_Output_Gain":4.20,"referenceAudioLevelOutputInDb":-52.00}`.
  - `LASP_CMD_NOTIFY_ASR_STREAM_STOPPED` (Int) clears it ("Stop AFE Diag and report metrics", 0x270ac). Stock's end.
  - Neither PuffinApp nor `libgenericaspclient.so` contains the name `LASP_CMD_SET_LISTENING_MODE`; stock never
    sends it.
- What the flag does, per 8 ms frame in the AFE's process function (0x713c4):
  - `ARA_V2` (interference canceller, AFE +0x8d4): state setter 0xce438 stores `(utterance || voice messaging) && !TTS`
    at +0x194; 0xce1b8 computes `update = energy_ok && !that` (+0x1c8) and skips the filter update loop when 0 (with
    "ARA_v2 Enable Leaky Update" the leaky update runs instead). The training and periodic-reset counters (0xcdf38)
    stand still too.
  - `AEC_V2` (echo canceller, AFE +0x8d0): setter 0xcb4e8 stores `utterance && !TTS` at +0x380; 0xcb220 sets
    `adapt = ref_ok && mic_ok && !that` (+0x354), no filter update when 0.
  - `GroupBeamMergerV2` (AFE +0x904): setter 0x868d8 stores `utterance || voice messaging` at +0xda; 0x86b1c skips the
    search for a better beam group while it is set: the beam stays on the talker.
  - So while a reply (TTS stream) plays, both cancellers keep adapting whatever the flag says; with music they freeze
    for the length of the command.
- Other AFE state bytes met on the way: +0x11d8 in TTS, +0x11d5 in alarm, +0x11d6 in music alarm, +0x11c0 in playback,
  +0x120c voice messaging, +0x11d9 AFE diagnostics running (ORed with the utterance flag in two places).
- `LASP_CMD_SET_WAKEWORD_METADATA` (`{"timestamp_before_ww_start":ms,"timestamp_before_ww_end":ms}`) only feeds the
  arbitration energies and metrics ("1-mic ESP"); it does not touch the flag. Without valid times the front end logs
  "Falling back to default behavior of using the 1100 ms oldest sample buffer - 500 ms noise 600 ms wakeword".
- Where the times come from: the front end writes its clock (ms, 16 bit) into the lowest bit of the micAsr samples:
  in every 128-sample frame the first 23 samples carry a fixed pattern, data follows. Pryon reads it back and reports
  in the result's metadata (24-byte header starting `JSON_GZ_AND_FP`, gzip JSON, fingerprint):
  `"audioMetadataDuringDetection":{.."afe_frame_before_ww_start":50,"afe_frame_before_ww_end":14,
  "playback_volume_before_ww_start":66,"timestamp_before_ww_start":8823,"timestamp_before_ww_end":9559},
  "deviceMetrics":{.."has_audio_lsb_metadata":1,..}`. Seen under qemu with the low bits of a device capture put onto
  `testdata/alexa_espeak.raw` (`pryon_test` prints the JSON as `META`); a file without them gives
  `has_audio_lsb_metadata:0` and no times. The clock is not the pipeline's sample count: (InSampleCnt / 16 + ~2600)
  mod 65536 on one mixer run.
- hassmic's arbitration score (2026-09-30) is stock's sequence: metadata times to the front end, read the JSON,
  1000 * log10(voiceEnergy / ambientEnergy); `LASP_CMD_NOTIFY_ASR_STREAM_STOPPED` afterwards. Both lipc tools in one
  `sh -c`: 150 ms on the device.
- `LASP_CMD_SET_NDVC_BYPASS` (Int): noise dependent volume control off; no effect on micAsr (one capture).
- ARA step size in `AFE.cfg` ("ARA_V2 Steady state mu <mode>", 0.75): 0.01 gives nearly the same result as the flag
  (mode here was "lineout": something in the 3.5 mm jack switches the tuning). Not needed with the flag.
- `micRaw` gives one channel whatever is asked for; `micMultiChAsr` gives nothing (mixer sets `multi_ch_asr_disable=1`
  in the HAL at start). `/proc/idme/miccal.0-3` are floats (this Echo: 1.51, 1.72, 0.91, 8.05).

## Wake word

- Engine `libpryon.so` (20 MB), deps only `libfst`, `libfstfar`, `libc++_shared`, `liblog`. Header: [`src/include/pryon_api.h`](../src/include/pryon_api.h).
- Model is plaintext: `/system/local/models/keyword/en-US/ALEXA/` (ONNX int16 + text configs). Keywords `ALEXA`, `STOP`.
- Sequence: `PryonModelSet_New(id, manifestFile, "")` → `PryonDecoder_NewSpotterAudioDecoder(id, msId, "pryon", format, "{}")` → `PryonApi_SetEnumeratedResultCallback` → `PryonDecoder_PushAudioEventSamples(id, index, samples, count)`.
- **Verified on the PC** (`tools/qrun.sh build/pryon_test testdata/alexa_espeak.raw`): stock library loads the stock model outside PuffinApp and detects a synthetic "Alexa". Result `detectionType` 2 = Accept, 0 = NearMiss, confirmed against the library's own log (`classifier_score=0.937, active_threshold=0.923, detection_type=Accept`).
- Must be fed at real-time rate: backlog over ~7 s is dropped. `PryonDecoder_BacklogWait` returns early.
- No secure-mode or licence check on this path.

## Platform (details in re-platform.md)

- LED: `ledctrl -s <pattern>` / `-u` / `-c`, patterns = file names in `/system/etc/led-resources/`. No sender identity needed.
- Volume: `audio_manager_set_prop MainVolume|TTSVolume 0-100`, `Mute 0/1` (LIPC `com.doppler.audiod`, served by `mixer`).
- Buttons: `/dev/input/event3`, keys 115/114 (volume), 138 (action), 113 (mic mute). Hardware `gpio-privacy` driver.
- Firewall drops inbound except TCP 16384–32767 and a few ports. Hence daemon default port 16700. Hook for extra rules: `/system/bin/debug_firewall.sh`.
- Nothing reboots or restarts when PuffinApp is missing. No autosleep without it.
- OTA: `otad`, `ace_otad` autostart; `update_engine` on demand.
- SELinux: `ro.boot.selinux=disable` in build.prop.
- Clock: nothing sets it behind the egress lock (stock synced with Amazon): the Dot 2 ran 83082 s (23 h) behind on
  2026-10-06. `date -u @<epoch>` (toybox) sets it as root, `hwclock -w -u` the RTC (`/dev/rtc0`). hassmic now takes
  Home Assistant's time (`clock.c`).
- `/system/bin/sh` is mksh R52 with 32-bit numbers: `$((2147483647 + 1))` is -2147483648, and `[ x -ge 4102444800 ]`
  compares a wrapped number. No awk; toybox grep takes no `\|` (use `-e` per pattern).
- Amazon's attestation module (`lib/hw/amzn_dha.<soc>.so`) runs `/system/bin/idme` and waits for it with a plain
  `wait()`; in a process that ignores SIGCHLD that blocks until every child has exited (POSIX), so a long-lived child
  of hassmic hung it (`clock.c` double-forks its log reader).

## Running bionic binaries on the PC

`qemu-arm -L firmware/donut/rootfs` hangs in a futex unless the PID is below 65536: bionic mutexes store a 16-bit owner tid.
`tools/qrun.sh` wraps qemu in a fresh PID namespace. `libmixerAPI` clients abort under qemu (no LIPC/D-Bus), `libpryon` works.

## Protocol choice

- Now: Wyoming satellite (`src/hassmic`). Raw PCM both ways, no decoder on device. Checked against the reference `wyoming` 1.10.2 library (`tests/fake_ha.py`).
- Later: ESPHome native API subset for timers, announcements, media player. Needs WAV over HTTP for TTS.
- mDNS: `avahi-daemon` is on the device; service file not added yet. Add the satellite in HA by IP and port.

## Open risks, in order

1. `micAsr` may need state that only PuffinApp sets (`AllowMic`, low-power mode, multichannel setup).
2. `TTS` stream ducking/volume behaviour and whether it needs audio focus from PuffinApp.
3. Device firmware may differ from NS65741: `scripts/probe.sh` compares library hashes.
4. Pryon accuracy on real `micAsr` audio (in-band AFE metadata bits are expected there; the engine logs `NoLSBMetadataObserved` on clean audio).
