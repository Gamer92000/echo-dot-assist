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

## Running bionic binaries on the PC

`qemu-arm -L firmware/rootfs` hangs in a futex unless the PID is below 65536: bionic mutexes store a 16-bit owner tid.
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
