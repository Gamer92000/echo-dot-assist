# Sound detection (Alexa Guard) in the stock firmware

Firmware NS65741 (donut). The model was run with the stock `libpryon.so` under qemu-arm (`src/tools/aed_test.c`,
2026-10-01). The rest is static analysis of `PuffinApp`, `SmartHomed`, `libSmartHomeEchoAdapter.so`,
`libCapabilityLibrary.so`, `libpryon.so`, `libasp.so` and the configs. Addresses are file offsets (= vaddr) in
the named binary. For `libCapabilityLibrary.so` they are vaddr, which is the file offset + 0x11000. Nothing was
watched on a running stock Echo. Anything not shown by code, config or a run is marked **UNVERIFIED**.

`/system/vendor/lib/libaed.so` has nothing to do with this. It is MediaTek's crash reporter (AEE: `aee_aed_raise_exception`).

## Summary

The Echo detects sounds on the device. Amazon's cloud confirms them and decides what happens next.

1. **Detection (PuffinApp):** PuffinApp runs a second Pryon decoder on its microphone ring, beside the wake word.
   The decoder uses the AED model: 12 sound types, one score per type per ~10 s window.
2. **Gate and pause:** detection stays off until the cloud turns it on through `SmartHomed`. PuffinApp also pauses it
   when the front end's low-power sound detector changes state.
3. **Hand-off (SmartHomed):** a hit becomes a LIPC event. `SmartHomed` cuts the audio clip out of PuffinApp's shared
   memory and sends it to Amazon, either for verification or as a plain report.
4. **Ring and tone:** when a clip goes out for verification, the ring flashes cyan and, if the user asked for it,
   a tone plays.
5. **After the cloud answers:** only a verified event (with the recording) reaches the `Alexa.AcousticEventSensor`
   capability. Notifications, routines and Guard's alarm-company calls all happen in the cloud.
6. **Near misses:** scores between the near-miss and detection thresholds have their audio uploaded to Amazon's ML
   ingestion service. This looks like training data.

## 1. The model and what Pryon does with it

`/system/local/models/AED/`: `pryon.manifest` → model `pryon` → `pryon.config`. The config sets
`search.decoder_type = "acoustic-event-detector"`, `recognizer.client_type = "aed"`, LFBE features with 25/10 ms
frames, an MLP (`model_gcmvn.cpu.v1.q.mlp`, 830 KB) and `AED.json`. `AED.json` lists 12 types, each with a detection
threshold, a near-miss threshold and an energy threshold (108 for humanPresence, 82 for the rest):

| type | detect | near miss | | type | detect | near miss |
|---|---|---|---|---|---|---|
| humanPresence | 0.79 | 0.60 | | cough | 0.982 | 0.80 |
| smokeAlarm | 0.825 | 0.40 | | waterSounds | 0.901 | 0.85 |
| glassBreak | 0.80 | 0.40 | | beepingAppliance | 0.953 | 0.75 |
| dogBark | 0.991 | 0.88 | | smokeSiren | 0.825 | 0.40 |
| babyCry | 0.955 | 0.60 | | carbonMonoxideSiren | 0.825 | 0.40 |
| snore | 0.964 | 0.80 | | runningWater | 0.982 | 0.80 |

**Windows.** `scorer.batch_scorer_reset_interval_msec = 9980` and `result_emission.regular_event_period_msec = 9980`.
The network is reset every 998 frames ("Resetting the network at frame [ 998]"). The decoder reports once per window:
the first at sample 159920 (9.99 s), then every 9.98 s, counted from the start of the stream rather than from the
start of a sound. A report comes even when nothing was heard. A sound can therefore take up to ~10 s to be reported.
A window still open when the stream ends is never reported.

**Same API as the wake word (`docs/re-pryon.md`), plus four things:**

- **Callback:** `PryonApi_SetAcousticEventDetectionResultCallback(cb)` stores `cb` in a global (`libpryon` 0x601ac8).
  It is called as `cb(decoderId, result)` on the decoder's `audio_to_result` thread. PuffinApp registers its
  callback at 0x444d08. The callback is presumably 0x44555c: it logs "resultsCallback", checks the decoder id and
  reads `+0x10` and `+0x1c`.
- **Decoder:** PuffinApp creates it with `PryonDecoder_NewMultichannelAudioDecoder` (`libPryonDetector`
  `createPryonDecoder` 0x35cc). The arguments are the same three ids as the spotter, then NULL, five zero words and
  the format by value. `PryonDecoder_NewSpotterAudioDecoder` gave identical scores on the same clip.
- **Enabling detection:** nothing is detected until two kinds of client property are set to 1
  (`PryonDecoder_PushClientEvents`):
  - `AcousticEventDetectionEnabled`;
  - one `aed_<type>_enabled` per type (PuffinApp `PryonAcousticEventManager` 0x44066c builds the name from `"aed_"`
    + type + `"_enabled"`).

  Without them the network runs but no callback comes. A type that is not enabled is missing from the result.
- **Result:** `PryonAcousticEventResult` in `src/include/pryon_api.h` has these fields:
  - `+0x08` u64: sample index at the end of the window;
  - `+0x10`: 0 = nothing, 1 = near miss only, 2 = something detected (matches the JSON in all 28 windows run);
  - `+0x14`: JSON text.

  The JSON is `{"amzn1.activity.device.detection.audio.aed":{...}}`. Per type it holds `score` (= `confidence`),
  `detected`, `nearMiss`, `decodingThreshold`, `nearMissThreshold`, `energyThreshold`, `maxEnergyLevel` and
  `numSamplesAboveEnergyThreshold`. It also carries `aedModelChecksum` and CMS statistics
  (`adaptiveCmsForFirstFrame`). smokeSiren also carries
  `"detectionDescriptorProfile":"{\"detector_variant\":\"SMOKE_SIREN\",...}"`, which is a JSON string inside the
  JSON.

### Test runs (qemu, stock model, `aed_test`)

**Inputs:**
- Real recordings: ESC-50 clips (github.com/karolpiczak/ESC-50, 5 s each, 44.1 kHz resampled to 16 kHz). Per class,
  3 clips were joined with 2 s of silence between them.
- A synthetic smoke alarm: T3 pattern, 3.15 kHz, 0.5 s on/off.
- A synthetic CO alarm: T4 pattern, 0.1 s beeps.
- 10 s of real `micAsr` speech from the Echo.

**Caveat:** the ESC-50 clips are close-mic recordings, not what the front end delivers. Treat them as a sanity check
of the model, not as a measurement of its accuracy.

| input | window 1 (0–10 s) | window 2 (10–20 s) |
|---|---|---|
| smoke T3 (synthetic) | **smokeAlarm, smokeSiren, carbonMonoxideSiren 0.990**, beepingAppliance 0.983, humanPresence 0.989 | – |
| CO T4 (synthetic) | nothing (beepingAppliance 0.66) | nothing (0.71) |
| speech (micAsr) | **humanPresence 0.996** | – |
| dog | **glassBreak 0.901** (dogBark 0.45) | **dogBark 0.998** |
| glass breaking | near miss beepingAppliance | **glassBreak 1.000** |
| crying baby | near miss babyCry 0.910 | **humanPresence 0.981**, near miss babyCry 0.915 |
| snoring | nothing | **snore 0.993**, humanPresence 0.878 |
| coughing | **beepingAppliance 0.974, glassBreak 0.803** | near miss cough 0.950 |
| pouring water | **waterSounds 0.989, glassBreak 0.980, humanPresence 0.985** | **waterSounds 0.987**, humanPresence 0.985 |
| toilet flush | **waterSounds 0.952** | **waterSounds 0.981, glassBreak 0.944**, humanPresence 0.826 |
| door knock | **humanPresence 0.948** | nothing |
| clock alarm | nothing | **humanPresence 0.992**, near miss beepingAppliance 0.903 |
| footsteps, water drops, siren | nothing (near misses only) | nothing |

**What the runs show:**
- **The real sounds are found.** Smoke alarm, glass, dog, snoring, speech and water were all detected in at least one
  window.
- **There are many false hits.** glassBreak fires on a barking dog, pouring water and a toilet flush. humanPresence
  fires on knocks, an alarm clock and water. beepingAppliance fires on a cough. Stock reduces these through
  verification in the cloud (section 3), which only a stock Echo with an Amazon account can use.
- **Some types share one score.** smokeAlarm, smokeSiren and carbonMonoxideSiren had identical scores in all 28
  windows, and so did cough and runningWater. `score_output_index` differs between them in `AED.json`, so the network
  presumably has fewer distinct outputs than there are types (**UNVERIFIED** why).
- **The CO pattern was not recognised.** The synthetic T4 beeps were not detected, although the T3 pattern at the same
  pitch was.

## 2. PuffinApp: the detector

**Audio.** PuffinApp creates its microphone ring at 0x12c632:
- an AVS SDS of 16-bit words with up to 12 readers, over shared memory named `puffin-micStream` ("Created shared
  memory for AudioInputStream");
- published as LIPC `com.amazon.puffin` `micStreamShm` / `micStreamShmSize` (0x13ce8c);
- fed from `micAsr` (`docs/FINDINGS.md`).

"Start AED initialization" (0x139822) passes exactly this stream to `PryonAcousticEventManager` (0x43e21c, argument
`[sp+0xfc/0x100]` set at 0x12c63a/0x12c64e). So AED hears the same processed `micAsr` audio as the wake word. hassmic
already reads that audio.

**Model.** `AEDAssetsManager` downloads newer models from Amazon (DAVS, `aed_ecids` per locale, `class-10`) and
records them in `/data/avs/aed_cache.json`. `/system/local/models/AED/pryon.manifest` is the fallback ("No cached
model, using fallback"). A "personalized AED" artifact (presumably the custom sounds a user teaches) is loaded through
`PryonModelSet_SetAedCustomSoundConfig`. The request (`AEDInventory::createRequest` 0x43d898): filters `filterVersion` "2",
`engineCompatibilityIdList` = the engine's `aed_ecids` ([1,2,3,5,6,7] on donut and biscuit), `modelClass` "class-10",
`location` NA, EU or FE. The artifact type and key come from two global strings "AED" (0xf4274, 0xf42c0); the
spelling on the wire is not confirmed. `scripts/artifacts.sh` fetches it along with wake words ("Other artifacts"
in its menu), into `device-logs/models/aed-<region>/`, and installs it into `/data/local/hassmic/aed`.

**Newer model (fetched 2026-10-01 with the Echo Dot 2):**
- **Request:** the name is `AED`/`AED`, as in PuffinApp; region EU.
- **Download:** artifact `3691a6e38fd015ba28505293bf0d3196`, files dated 2022-10-05.
- **Changes against /system:**
  - the weights file is the same size, with 74280 of its 833732 bytes changed, spread over the whole file;
  - smokeAlarm, smokeSiren and carbonMonoxideSiren thresholds go from 0.825 to 0.845;
  - the siren variant is `SMOKE_SIREN_GB`.
- **Results:** on all clips of section 1 the two models agree in every window, with scores within ±0.02, except for
  the shared smoke/CO score. The T4 CO beeps now reach 0.47 (near miss, still not detected), and a door knock window
  0.41. The false hits stay the same. So the newer model is not worth having so far. `scripts/artifacts.sh` can
  still install it (`/data/local/hassmic/aed`, not ticked by default); `sound_pryon.c` then takes it in place of
  the firmware's, and the firmware's again if it does not load.

**On/off.**
- LIPC properties `AEDOverallState` (Enabled/Disabled) and `AEDTypeState_<type>`, written by `SmartHomed`, become
  the client properties above (`applyDecoderStateLocked`, "FailedToSetOverallMode", "FailedToSetEventType").
  `AEDSupportedTypes` lists the types the model has.
- **Pause:** `onSoundStateChange(newState)` (0x4416b4) resumes the detection loop for state 0 (metric
  `resumeDuration`) and pauses it for state 1 (0x44223c, metric `pauseDuration`). The state comes from
  `LPMSoundStateEventHandler` (0x22480c…), which receives the mixer's LIPC event `notify_lpmsd_event`. That event
  comes from the front end's Low Power Mode Sound Detector (`libasp` 0x18d72; it logs "LPMSD: In High Power / In Low
  Power"). LPM only runs while PuffinApp sets `LPM_ELIGIBLE` and `persist.pwrsvc.LPM_ENABLED=1` (the default in
  `build.prop`). So stock pauses AED while the front end is in its low-power state. Which LPMSD state means "quiet"
  is **UNVERIFIED**; `libasp` sends 2 or 3 (0x21740), and PuffinApp's mapping of those to 0/1 was not traced.

**Output.**
- LIPC event `AcousticEventDetected` (0xf4518) carries the detection time, start/end sample index and the result JSON.
- **Near misses** are handled separately ("Handling near-miss acoustic event detection." 0x445cb0). Their audio is
  uploaded to the ML ingestion service (`uploadAEDNearMiss`, `AED_near_miss_audio_size`). Builds labelled
  `AED_CS_NM_DEV/QA/STAGING/PHASE2-4` suggest it was rolled out in stages.

## 3. SmartHomed: from a hit to the cloud

`SmartHomed` loads `libSmartHomeEchoAdapter.so` (Echo-specific) and `libCapabilityLibrary.so` (generic smart-home
capabilities, with C++ symbols).

**Turning detection on.** `EchoAcousticEventSensorCapability` takes `setMode` from the cloud: `overallMode`
ENABLED/DISABLED and per-type `detectionModes`, including `cloudVerificationMode`. It writes the PuffinApp
properties and restores them after a reboot (`restoreDetectionModes`). Typical triggers are Guard Away mode and
routines "when Alexa hears …" (product knowledge, not from the code).

**Handling a hit (`AEDEventProvider::handleEvent`, adapter 0x29ba8):**
- drops the event if overall detection or that type is off;
- checks the model checksum, compatibility version and personalized artifact;
- asks `CallStateListener::isCallDetected` (a call in progress or ended `inLast10Secs`) and logs
  `AcousticEventDetected, type, verifiable, callDetected`;
- cuts `audioSamples` from `micStreamShm` between start and end sample index (`AEDAudioStreamHelper`);
- queues either `kMsgSendCloudVerifyRequest` or `kMsgSendCloudReportEvent`.

**Sending.** Both go out through the device's own `AEDCloud` capability (`EchoAEDCloudCapability` in the adapter):
- **Report:** action `ReportDetection` posts the event to Amazon's AED cloud service ("SuccessfullyPostedAEDEvent").
- **Verify:** action `VerifyDetection` posts it with an `eventRequestId` and a timeout.
  - The answer comes back as action `VerifyDetectionResult`: `verificationResults` {`detection` {`eventType`,
    `detectionStatus` DETECTED | NOT_DETECTED}, `timeOfSample`, `uncertaintyInMs`}, plus `recording`.
  - Without an answer in time, `kMsgReportVerificationTimedout` follows.
  - `AcousticEventVerified detected=1` with its `media` goes to `EchoAcousticEventSensorCapability::onEvent`, which
    feeds the `Alexa.AcousticEventSensor` capability. A verified event without media is dropped
    ("VerifiedEventMissingMedia"). The recording is what the Alexa app plays back.

**Ring and tone.**
- **Flash:** `EchoAEDCloudCapability::handleVerifyMessage` (adapter 0x3f924) first calls
  `mCapCallback->notifyDetected(label)`, before anything is sent. That is vtable slot +0xc, which is
  `AEDCloudCapabilityInstance::notifyDetected` (the vtable entry at 0x293f58 was resolved from the packed
  relocations). `handleDetected` then emits the capability event `eventDetected` {`label`}. `SmartHomed`'s
  `SmartHomeUx::handleAEDEvent` (0xa70f4) turns it into `AcousticEventDetectedUx` (0xa8228). That reads
  `com.amazon.puffin` `AcousticSetting`, the cloud setting `Alexa.EventDetectionSensor.CloudVerification`
  `acousticConfirmation`:
  - `TONE` → UX event `aed-detected`: a cyan flash plus `state_sent_to_cloud.mp3`;
  - otherwise → `aed-detected-noearcon`: the flash only.

  `handleReportMessage` makes no such call, so a type that is only reported does not flash.
- **Guard on:** `aed-enabled` (a faint white layer) is played or cleared by `ModeIndicatorUx` (0xa84a0) from the
  `Alexa.ModeIndicator` property `securityMode`, which the cloud sets. The mode strings sit in a lookup table that was
  not resolved.

## 4. In hassmic (2026-10-01)

`src/hassmic/sound_pryon.c` (backend, `sound.h`) and the "sound detection" section of `main.c`; README "Sound detection".

- **Off by default.** The ESPHome switch "Sound detection" (kept in the settings file) makes the capture thread open a
  second decoder (`NewSpotterAudioDecoder` on the `/system` model, the same results as PuffinApp's constructor) and feed
  it the same `micAsr` blocks as the wake word. Off, there is no decoder at all.
- **Types:** the client properties enable the 10 types that map to Home Assistant events. smokeAlarm, smokeSiren and
  carbonMonoxideSiren become `smoke_or_co_alarm` (one score). cough stays `cough`, waterSounds becomes `water`, and
  runningWater (the same score as cough) is not enabled. humanPresence is not enabled: it fires on any talk.
- **Output:** one ESPHome event entity, "Sound", with the event type naming the sound, per window that detected it.
  Nothing is sent while the Echo is muted.
- **Own playback:** a window is dropped when the Echo itself played something during it, or within 11 s before the
  result: TTS, a ringing timer, music (Sendspin, Bluetooth), earcons. This stands in for part of what stock's cloud
  check does.
- **CPU:** `aed_test` on the Echo Dot 2 (biscuit, 4 cores at 1.3 GHz), real-time input: 7.9 s of CPU per minute, i.e.
  13 % of one core. hassmic alone (wake word included) used 36 % of one core in the same minute.
- **Tests:**
  - `tests/fake_ha_esphome.py` (PC build, `sound_none.c` fakes a dog per window): entities, switch off by default and
    kept, event, and an announcement's window dropped.
  - `build/donut/hassmic-qemu` with the stock model on ESC-50 clips: glass breaking gave `glass_break` in each window
    containing it; footsteps gave nothing.
- **On a real Echo:** Echo Dot 2, room audio through the front end, about 45 minutes with detection on. A real cough
  gave `cough` in two windows in a row, twice. Nothing else fired.
- **Still open:**
  - played test sounds in the room (smoke alarm, glass, dog), and talking or TV;
  - whether to require a type in two windows in a row.
- **No low-power pause:** stock's LPM pause does not apply, since hassmic never sets `LPM_ELIGIBLE`.
