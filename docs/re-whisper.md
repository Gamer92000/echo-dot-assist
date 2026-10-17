# Whisper detection in the stock firmware

Firmware NS65741 (donut). Static analysis of `AlexaHybridExecutionControllerLitespeed` (AHE,
"AlexaHybridLitespeed-1.0.18051.0"), `libpryon.so` and `PuffinApp`, plus `device-logs/oobe-updating-logcat.txt`.
The same `libpryon.so` ships on biscuit (md5 `88a4e723f4a1d89a5e0370b411a332b2`). On an Echo Dot 2 (biscuit),
2026-10-04, these ran: `WhisperApi_getLibraryAttributes`, `setLogEventHandler`, `setEventHandler`,
`loadWhisperModelset` and `createWhisperDetector`. The DAVS model was then fetched and run on that Echo
(`src/tools/whisper_test.c`, section 5). Addresses are
vaddrs (= file offsets for `.text` and `.rodata`) in the named binary, AHE unless named. Anything not shown by code,
config or a run is marked **UNVERIFIED**.

## Summary

- **The detector is on the device.** It is part of `libpryon.so`, behind an exported C API (`WhisperApi_*`), beside
  the wake word and sound detection (`docs/re-pryon.md`, `docs/re-aed.md`).
- **Only AHE calls it.** AHE is Amazon's on-device "hybrid" engine: local speech recognition and understanding,
  local TTS, local skills. AHE loads the API through dlopen (`PryonWhisperWrapperDlopen`).
- **PuffinApp has no whisper code.** It has no string "whisper", "isWhisper" or "whispered", and it sends no whisper
  flag to the cloud. Its `audioFeatures` / `DeviceAudioFeaturesReport` is the front end's spectral energy report,
  not whisper. (Other hits in `/system` are unrelated: `libamazonwha.so` "Whisperplay" is Fire TV casting,
  `libffs_*` `ProtobufWhisperJoinBlePacket` is setup.)
- **The model is not in `/system`.** It is the DAVS artifact `whisper-static`. AHE subscribes to it only when
  Amazon's server-side `ahap-policy` selects "static" Litespeed pipelines.
- **This Echo never ran it.** The policy it got on 2026-09-21 selected "caching" pipelines, so AHE unsubscribed
  `whisper-static` and tore down the whisper pipeline (logcat line 3263). The model was never fetched.
- **The local result stays local.** It feeds AHE's local result (`isWhisper`), the local execution policy
  (`allowWhisper`) and local TTS (whispered SSML). The path that would send device-side results upstream is a stub in
  this build (section 3).
- **So for cloud requests the cloud decides** from the uploaded audio, and the device plays the whispered TTS it
  gets back (inferred: nothing on the device tells the cloud).

## 1. The API (libpryon)

**Shape:**
- **Separate state:** a singleton of its own, apart from `PryonApi_*` / `PryonDecoder_*`. Log lines are tagged
  `{whisper}`. Source: `pryon/cpp/api/whisper.cpp`, `pryon/cpp/pryon_whisper/*`.
- **Return codes:** every export is a C wrapper around C++ with a try/catch. 0 = ok, 1 = exception or "unknown model
  set". The message goes to the log handler at level 2: `detectorId=<id>: <what> in WhisperApi_<fn>`.
- **Names:** model sets and detectors are named by strings the caller chooses.
- **One event handler per process** (`setEventHandler`), not per detector. `createWhisperDetector` throws
  "WhisperEventHandler cannot be NULL" without one.
- **Text callbacks:** `getLibraryAttributes`, `getModelSetInfo` and the log handler all call
  `cb(int, const char *g, const char *text)`. `g` is a global string, empty in every run. Log levels seen: 1 stack
  trace, 2 error, 4 info, 5 debug.

Header: [`src/include/pryon_api.h`](../src/include/pryon_api.h).

| export | offset | arguments | notes |
|---|---|---|---|
| `WhisperApi_getLibraryAttributes` | 0x5ff571 | `(cb)` | Synchronous. On the Echo: `{"engineCompatibilityIds":[1],"engineUuid":"da5620af-cace-58d0-ae0d-39724cc0b2a8"}`. |
| `WhisperApi_getModelSetInfo` | 0x5ff5e5 | `(modelSetId, cb)` | SIGSEGV on a set built from the AED manifest; presumably needs a whisper model (**UNVERIFIED**). |
| `WhisperApi_loadWhisperModelset` | 0x5ff345 | `(modelSetId, manifestPath)` | Same loader as `PryonModelSet_New`; loaded the AED manifest (returns 0). |
| `WhisperApi_deleteWhisperModelset` | 0x5ff755 | `(modelSetId)` | |
| `WhisperApi_setEventHandler` | 0x600691 | `(fn)` | Process-wide. |
| `WhisperApi_setLogEventHandler` | 0x600855 | `(fn(level, g, msg))` | |
| `WhisperApi_createWhisperDetector` | 0x5feadd | `(detectorId, modelSetId, modelName, WhisperAudioFormat fmt)` | `fmt` is 16 bytes by value. AHE passes `"pryon"`, `"pryon"`, `{0, 16000, 16, 1}`. |
| `WhisperApi_pushAudioEvent` | 0x5ff8f9 | `(detectorId, const int16_t *samples, uint32_t n, uint64_t sampleIndex)` | Copies `n*2` bytes. The u64 is on the stack, 8-aligned; r3 is unused. |
| `WhisperApi_pushEndOfUtterance` | 0x6001f1 | `(detectorId, utteranceId, uint64_t start, uint64_t end)` | Asserts non-empty `utteranceId` and `end >= start`. |
| `WhisperApi_pushSessionEnd` | 0x5ffed5 | `(detectorId)` | |
| `WhisperApi_backlogWait` | 0x600511 | `(detectorId, int32_t timeout)` | AHE passes -1; presumably ms, -1 = forever (**UNVERIFIED**). |
| `WhisperApi_deleteWhisperDetector` | 0x600481 | `(detectorId)` | |

**Arguments, as AHE uses them:**
- **`createWhisperDetector` model name:** "nope" was accepted too, and the run then failed for another reason (see
  Model set). Whether it names a model inside the set is **UNVERIFIED**; use "pryon" as AHE does.
- **`pushAudioEvent` index:** the count of samples pushed to this detector so far. AHE starts at 0 per detector and
  adds `n` after each push (`[fp+24]`, 0x5c9fb4). It skips the call when n = 0.
- **`pushEndOfUtterance` units:** 10 ms frames. AHE divides its sample indices by 160 (0x5c9f02, 0x5c9f0c):
  - `start` = the start-of-speech sample from BeginSession, / 160;
  - `end` = the end-of-utterance sample, / 160;
  - AHE skips the call when start ≥ end;
  - `utteranceId` = `"Utterance-<n>"`.

**Event handler** (proxy vtable 0x1282500, methods 0x60077d, 0x6007c1, 0x600811):

```c
fn(const char *detectorId, const char *utteranceId, const char *type, const char *json, 0, 0)
```

- **`type`:** `"result"`, `"metrics"` or `"metadata"`.
- **`utteranceId`:** a real string for `"result"` (presumably the id given to `pushEndOfUtterance`); the empty
  global for the other two.
- **Last two words:** always 0. A pointer plus a length, or one u64 (**UNVERIFIED**).
- **Thread:** presumably the detector's worker thread (**UNVERIFIED**).

**Result.** AHE's `WhisperFinalProcessor` (0x5f06f4…0x5f076a) parses the `"result"` JSON, reads
`whisper_results` → `confidence` as an integer, and sets **whispered = confidence > 500**. Missing fields give
"Whisper results don't exist" / "Missing confidence field". So the JSON is presumably
`{"whisper_results":{"confidence":<int>}}`; a run confirmed the object (section 5). libpryon's
`WhisperDeciderProcessor` has config keys `score_index`, `score_min`, `score_max`, `confidence_threshold`,
`confidence_min`, `confidence_max`. That suggests the network score is mapped to 0..1000 with the model's threshold
at 500 (**UNVERIFIED**).

**Timing.** The result comes only after `pushEndOfUtterance`. The evidence is these metric names:
`whisper_eou_enqueued_for_processing`, `whisper_eou_delay_msec`, `whisper_eou_end_frame_index`,
`whisper_eou_short_process_immediately` ("Processing EOU immediately due to short utterance"),
`whisper_eou_audio_ready_process_immediately`, `whisper_eou_result_callback_latency_msec`. An end of utterance
whose end frame has not been pushed yet is presumably queued until that audio arrives, plus
`WHISPER_EOU_DELAY_MSEC` ("EOU event: reset … to zero due to" cancels the delay) (**UNVERIFIED** at run time).

**Audio:** 16 kHz, 16-bit, mono, encoding 0. Which mic stream AHE feeds was not traced. It is presumably the request
audio PuffinApp hands to AHE (micAsr, as for the speech recognizer, **UNVERIFIED**).

### Model set

- **Manifest:** `pryon_whisper.manifest`, presumably the usual Pryon `ModelSetManifest` JSON (`default_model_name`
  "pryon" → `pryon.config`).
- **Whisper section required:** `createWhisperDetector` on the AED set failed with
  `assertion "mModel->getConfig()->getWhisperConfigProvider().getFeConfigProvider()" failed
  (pryon_whisper_detector.cpp:136)`. So `pryon.config` must carry a whisper section. libpryon knows these keys:
  - `whisper.enabled` ("When set to true, Whisper detection will be performed…");
  - `whisper.runtime_config` ("Path to the Whisper Detection configuration.");
  - `whisper_acoustic_scoring_config`;
  - a whisper front-end config. Without one, libpryon logs "Cannot find whisper frontend config, use ASR frontend
    config"; the assertion above apparently fires before that fallback.
- **AHE's files (loader 0x5c8740–0x5c8e40):**
  - `whisper_runtime_config.json` (0x5c8dc8), key `confidence_config_filepath` (0x5c8dfe);
  - that file: `whisper_confidence_threshold` (one value) or `whisper_confidence_thresholds` (several, 0x5c884a). libpryon
    asserts its object type is `"whisper_confidence_config"`. Where AHE applies these thresholds was not traced;
  - multichannel id from the manifest, default 1 ("Defaulting mcid for Whisper Stream to 1").

## 2. Getting the model (DAVS)

**AHE's subscription** (`WhisperStaticArtifactUpdater`, 0x5a0798–0x5a0812, "Changing to static litespeed
pipelines, subscribing to whisper" 0x5a0902):
- key `"whisper-static"` (key list at 0x30d598);
- metadata `{"ecid": "6"}`. The "6" is a literal in AHE (0x17195d), not read from libpryon. libpryon's
  `WhisperApi_getLibraryAttributes` reports `engineCompatibilityIds [1]`, and its general attributes have no
  `whisper_ecids`;
- `Decompress=true`, `DavsEndpointTargeting=global`. "global" is presumably the NA host `api.amazonalexa.com`:
  `ahap-policy` went there, while `spectrum-nlu-personalized` went to `api.eu.amazonalexa.com`.

**Wire request** (`Davs2UrlBuilder` 0x665040–0x665334), a rapidjson document in this key order:
- `artifactType` = the constant `"alexa-hybrid"` (0x66507e);
- `artifactKey`;
- `currentArtifactMD5` and `patch` {`diffAlgorithms` ["ahebsdiffv1"], …} only when a copy exists already;
- `filters`: every metadata entry as `key: [value]` (arrays, helper 0x66a19c), then `modelClass: [<config
  ArtifactManager.ModelClass, default "sonar">]`, which is `odie-litespeed` in `/system/etc/ahe.config.json`, then
  `locale: [<locale>]` only for per-locale subscriptions (**UNVERIFIED** for whisper).

The JSON is base64-encoded (0x62fb28) and appended to `/v2/deviceArtifacts/?artifactFilter=` (0x17e6d3), as for
wake words and AED. The logcat shows a shorter journal form (`DavsQueryBuilder` 0x676774):
`query={"artifactType":"<key>","filters":"{\"compatibilityId\":\"1\"}"}`. That form is not what goes on the wire.

Which filters DAVS wants, tried 2026-10-05 with an Echo Dot 2's and an Echo Dot 3's token, every one for each of
the 13 locales of `scripts/artifacts.sh` (de-DE en-US en-GB fr-FR it-IT es-ES ja-JP pt-BR en-CA fr-CA en-AU en-IN es-MX):
1. `{"ecid":["6"],"modelClass":["odie-litespeed"]}`
2. the same with `"locale"`
3. `{"ecid":["6"]}`
4. `{"ecid":["1"],"modelClass":["odie-litespeed"],"locale":[…]}`
5. `{"compatibilityId":["6"],"modelClass":["odie-litespeed"]}`

Only request 2 with `"locale":["en-US"]` answers; all 64 others get HTTP 404 "No suitable artifact found for
request.". So there is no model per language, and `tools/davs-fetch.py <map.db> whisper` (and the "Other artifacts"
list of `scripts/artifacts.sh`) sends that one request, into `device-logs/models/whisper-en-US/`. Both Echos got the
same answers and the same model. Artifact `df014d05b47afff9a3f3e5c34ab3a538`, 497928 bytes, files dated 2020-11-19. Contents in section 5.

**Other AHE artifacts seen on 2026-09-21:**

| key | metadata | endpoint |
|---|---|---|
| `ahap-policy` | `compatibilityId` "1" | NA |
| `policy-engine-config` | `compatibilityId` "11" | NA |
| `spectrum-nlu-personalized` | `compatibilityId` "7" | EU |

## 3. How AHE uses it

**`ahap-policy` decides whether it runs.** It is a DAVS artifact read by `AhapPolicyFactory` (0x5c6ad2–0x5c6e48).
Keys:
- `version` (required);
- `wwPrefixConfig`;
- `supportsLitespeedCaching`;
- `tcnCacheExpirationCheckIntervalSeconds`;
- `tcnCacheDomainInvalidationDenylist` [{`domain`}];
- `arePreviewResultsEnabled`.

`supportsLitespeedCaching` presumably picks the pipeline mode (inferred, branch not traced):
- caching: unsubscribe `whisper-static` and `s2i-tiny-static`, subscribe `s2i-tiny-caching`;
- static: subscribe `whisper-static`, and request `WHISPER_DETECTION_RESULT` ("PipelineType is TCN, add
  WHISPER_DETECTION_RESULT as requested result type" 0x2ffaf2).

This Echo got artifact `db1db8bb8890b65ebdecee0ea1c5ead3` (md5 `9e536797…`), which selected caching. Its content
is only on the device (`/data/alexahybrid/files/AmModel/ahap-policy.9e536797….de-DE.1.1`) and has not been read.

**Call sequence** (`PryonWhisperWrapperDlopen`, vtable 0x8219f0: +8 create, +0xc load, +0x10 delete model set,
+0x14 push audio, +0x18 session end, +0x1c end of utterance, +0x20 delete detector, +0x24 backlogWait,
+0x28 setEventHandler):
1. **Artifact loaded** (0x5c8b84…0x5c8bf8): `loadWhisperModelset("pryon", "<dir>/pryon_whisper.manifest")`, then
   `setEventHandler(0x5c918d)`, then "Whisper Detection model is available".
2. **BeginSession:** `createWhisperDetector(id, "pryon", "pryon", {0,16000,16,1})` (0x5c9b02). The id presumably
   starts with `whisper-detector-` (0x13fcb3, **UNVERIFIED**).
3. **Each audio chunk:** `pushAudioEvent(id, samples, n, samplesSoFar)` (0x5c9ce6). The audio comes from PuffinApp
   over AHE's sockets (PuffinApp `aheUdsPath`, `AdkHybridRouter`; AHE `SharedMemoryAudioDelivery`,
   `OpusDecodingAttachmentReader`).
4. **End of utterance**, after the local speech recognizer's endpointer: `pushEndOfUtterance(id, "Utterance-<k>",
   speechStart/160, eou/160)` (0x5c9f72).
5. **EndSession:** `backlogWait(id, -1)`, `pushSessionEnd(id)`, `deleteWhisperDetector(id)` (0x5c9dc2, 0x5c9e16,
   0x5c9e6a).

**What the result is used for (all local):**
1. **Execution plan:** `SpeechExecutionPlan` (0x3dd7ec) carries `isWhisper` beside `isSilence`, `isFalseWakeUp`,
   `isWakeWord` and `recognitionConfidence`.
2. **Local-versus-cloud policy** (0x4096c0–0x409bbe). Rules of `policy-engine-config` have `allowWhisper`, beside
   `executionType`, `abortOnType`, `arbitrationIndicator`, `allowFollowUp`, `allowUnsupportedVoice` and
   `timeoutsInMillis`.
   - "Whisper detected but not allowed, creating plan with infinite timeout": the local result is not executed on a
     timer, so the request waits for the cloud.
   - "Arbitrated TZero not allowed for whisper, downgrading to NORMAL plan": a whispered utterance cannot take the
     "execute the local result at once" plan.
3. **Policy context:** `audioFeatures.isUtteranceWhispered` (0x40d570), beside `systemBriefMode` and
   `speechRecognizerFollowUpMode`. Presumably input to the CLEAR policy JS (DAVS `clear-policy`;
   `/system/etc/ahe.clear.policy.js`, named in `ahe.config.json`, does not exist in the image) (**UNVERIFIED**).
4. **Local TTS** (0x4305e2, 0x4312da):
   - `isWhisper` is passed with `overrideVoiceId`, `locale` and `assistantsVoice`;
   - prompts are rewritten from `<speak>(.+?)</speak>` to
     `<speak><amazon:effect name="whispered">…</amazon:effect></speak>`;
   - the TTS cache (0x3c216c, 0x3c9172, 0x5177a2) keeps `isWhispered` per entry and strips the wrapper when matching.

**Upstream path (stub).** Results would be written to an "Intermediate Stream" by the ISG (Intermediate Stream
Generator: `IsgAprnSubscriber`, `IsgWhisperResultWritten`, metric
`EstimatedEndOfUtteranceToWhisperResultLeavesDeviceTime`). In this build the only classes are
`IntermediateStreamGeneratorStub` / `IntermediateStreamStub`, and the log tag is `AHE-AHAP-ISGWrapperCrosstownStub`
(0x2ff666). So on donut a local whisper result most likely never reaches the cloud (**UNVERIFIED** at runtime).
Whisper metrics go to ILM / PMET ("Whisper metrics are reported to ILM").

## 4. In hassmic

`src/hassmic/whisper_pryon.c` (`whisper.h`) runs it as AHE does, and `proto_esphome.c` lists the binary sensor "Last
request whispered":
- **Model:** DAVS only (`scripts/artifacts.sh`, ~0.5 MB), so every user fetches it with their own Amazon
  registration, as for extra wake words. It goes into `/data/local/hassmic/whisper`, not `models/`: its stray
  `pryon.manifest` would be taken for a wake word. Without it there is no sensor.
- **Span:** a fresh detector per request, fed the raw mic audio from 0.5 s after the start of streaming to Home
  Assistant's VAD end (`core_mic_off`). A run that ends any other way (cancel, timeout) is not scored.
- **Why 0.5 s:** streaming starts when the wake word is recognised, before its last sound has died away. Said in a
  normal voice, that tail took whispered requests down to 895–979 (live, below). The start given with
  `pushEndOfUtterance` changes nothing: the detector scores all audio it was fed. So the tail is not fed at all.
- **Cut-off:** the threshold the result names (922: no locale is set). The scores are far apart (section 5), so the
  per-locale ones would not change an answer.
- **Timing:** the result comes within milliseconds of the end of utterance, while speech to text still runs, so the
  state is in Home Assistant before the conversation agent renders its prompt.
- **Use:** Home Assistant's pipeline has no whisper flag, and Piper cannot whisper. The sensor is for the prompt
  template: the agent marks its answer for a TTS engine that can whisper.

## 5. The model and first runs

**Files** (`whisper-static`, en-US):
- `pryon_whisper.manifest` → model `whisper` → `pryon_whisper.config` → `INCLUDE "pryon_whisper.inc"`. That file says
  "everything is dummy value except the two lines": `whisper.enabled = 1` and
  `whisper.runtime_config = "whisper_runtime_config.json"`. The rest (`HCLG.fst`, `final.trans`, `words.txt` with
  Kindle words, `pdf.counts`) is required filler for a recognizer that is not built.
- `whisper_runtime_config.json` names four files:
  - `whisper_fe_config.json`: LFBE, 64 mel bins 80–7200 Hz, 25/10 ms, global mean/variance from
    `whisper_components/model.v8.0.mvn`, then running mean normalisation;
  - `whisper_acoustic_scoring_config.json`: the classifier `whisper_components/model.v8.0.mlp` (265 KB, `dnnrt`);
  - `whisper_vad_config.json`: a DNN speech detector `whisper_components/dnn_vad.mlp` (207 KB) on ±8 stacked frames;
  - `whisper_confidence_config.json`: `confidence_frame_window_size` 25, `number_of_frames_dropped` 50,
    `use_end_of_speech` false, and `whisper_confidence_thresholds` per locale: default 922, de-DE 862, en-GB 737,
    en-CA 764, fr-FR 831, it-IT 841, es-ES 921, ja-JP 821, … (en-US falls to the default).
- A stray `pryon.manifest` for speaker-ID enrollment, whose config is not in the artifact.

So the detector is a small DNN on filterbank features, gated by its own speech detector. The locale only picks the
threshold, so one model serves all languages, and DAVS has no other (section 2: en-US only).

**Events seen** (`whisper_test`, Echo Dot 2, model pushed to `/data/local/tmp`):
- `"result"`: `{"whisper_results":{"confidence":8,"detector_id":"whisper-detector-1","device_type":"","end_frame_index":1000,"locale":"","start_frame_index":0,"threshold":922,"utterance_id":"Utterance-1"}}`.
  `threshold` is the "default" threshold, since nothing set a locale. How to set one (`locale`, `device_type`) is
  **UNVERIFIED**. So which cut-off decides is open: AHE's `> 500` (section 1) or the locale threshold.
- `"metrics"`: latency counters, `whisper_processed_audio_on_eou`, `whisper_left_over_frames`, and at session end
  `whisper_max_rss_kb` (~10.9 MB) and `whisper_detector_creation_time_msec` (10–13 ms).
- `"metadata"`: `frame_confidences`, one value per 10 ms frame (0..1000), plus `frame_indices` and the threshold.
- The result came at once after `pushEndOfUtterance` (`whisper_eou_audio_ready_process_immediately` 1), and the
  callback's last two words were 0 every time.
- The log says once "keyword am_fe_config_filepath does not exist" (level 3), harmless.

**Normal speech** (earlier `micAsr` recordings, whole file as one utterance):

| clip | content | confidence |
|---|---|---|
| micAsr-170444 (10 s) | talking | 8 |
| the same +24 dB | | 8: input level does not matter |
| micAsr-164947 (5 s) | quiet, -61..-65 dBFS | 80 |
| micAsr-180305 (15 s) | mostly quiet, a few loud seconds | 442 |
| micAsr-180350 (15 s) | mostly quiet, a few loud seconds | 671 |

The per-frame values are high (≈ 900) over long quiet stretches of the last two clips. So a whole file is not a fair
test: stock scores only the command, from the endpointer's start to its end.

**Whispered commands** (2026-10-04, Echo Dot 2, German, 1–2 m, processed mic stream from hassmic's capture dump,
`device-logs/whisper/rec-de.raw`). Three commands ("Schalte das Licht in der Küche ein.", "Wie wird das Wetter
morgen?", "Stell einen Timer auf fünf Minuten."), each spoken three ways. The sentences were cut out by level. Each
clip ran in a fresh detector, as stock makes one per request: 0.5 s of audio before the speech, and the utterance
marked from speech start to speech end (`-u`), with `-l de-DE` (threshold 862):

| how | peak level | confidence (3 commands) |
|---|---|---|
| normal voice | -38..-45 dBFS | 1, 0, 1 |
| whispered | -50..-51 dBFS | **996, 995, 998** |
| quiet voice, not whispered | -47..-48 dBFS | 18, 1, 2 |
| two sounds after the last command (no words) | -43, -50 dBFS | 859, 211 |

- **Commands:** whispered and voiced commands are far apart, so the cut-off hardly matters: 500 (AHE) and 862
  (de-DE) give the same answer on every command.
- **Quiet is not whisper:** speaking softly is not taken for whispering.
- **The catch:** sounds without speech (breath, rustling) score high, like the quiet stretches of the old clips.
  Stock only scores what its endpointer took for a command, and so would hassmic: only the span Home Assistant's VAD
  calls speech.
- **Not tried:** other speakers, larger distances, music or TV in the room, English.

**Live, in hassmic** (2026-10-04, Echo Dot 2, Home Assistant with the wake word "Echo" said normally and the command
whispered; `whisper:` lines of `boot.log`, the capture dump replayed with `whisper_test`):

| build | whispered | normal |
|---|---|---|
| fed from the start of streaming | 940, 885, 641; then 895, 979, 973 | 0, 435 (a first try, unclear how it was spoken) |
| the first 0.5 s not fed | 999, 984 | 1, 0 |

- **Replay matches live:** replaying the exact spans (`whisper: request from capture sample N`, the dump's own start
  sample in its "capture dump: on" line) gave the same 895/979/973.
- **The tail is the cause:** the per-0.25 s curve (`whisper: per 0.25 s:`) started with ~1 s near 0, the voiced end of
  "Echo" ("Oh, wie spät ist es?" in the transcript).
- **Skipping fixes it:** without the first 0.5 s the replays scored 996–998, and without 1 s 997–999.
- **Not the cause:** real-time pacing (998/995 at `-x 1`), and up to 3 s of silence after the speech (≥ 987; 5 s:
  905–972).
- **Harmless warning:** libpryon logs "Backwards timestamps of duration … detected" now and then.

