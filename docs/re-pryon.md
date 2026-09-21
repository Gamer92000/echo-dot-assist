# libpryon.so — wake word API notes

Firmware NS65741, `firmware/rootfs/system/lib/libpryon.so` (19.8 MB, Thumb-2). Static analysis only; nothing here has been executed.
Header: [`src/include/pryon_api.h`](../src/include/pryon_api.h).

Tools used: `tools/fn.sh` (dump one function from an `.asm`), `tools/fnstrings.py` (resolve pc-relative strings in an address range), `tools/pcstr.py`.
Disassembly: `re/libpryon_api.asm` (0x600c00–0x605200, the exported C wrappers), `re/libWakeWordManager.asm`, `re/libAmazonKWD.asm`, `re/libPryonDetector.asm`.

## Shape of the API

- Process-wide singleton. No init call. Model sets and decoders are named by caller-chosen strings.
- Every exported `Pryon*` function is a thin wrapper: it calls an inner C++ method with the singleton as `this`, catches exceptions, returns `1` on exception and the inner status (0 = ok) otherwise. See `PryonModelSet_New` at `0x602228`.
- Inner methods validate arguments and name them in error strings, which is where the parameter names below come from.
- `libWakeWordManager.so` keeps C++ symbols (`wwm::Pryon`, `wwm::PryonDecoder`), so the stock call sequence is readable.

## Minimal call sequence (what PuffinApp's `wwm` does)

1. `PryonApi_SetLoggingCallback(cb)` — `wwm::Pryon::init` `0x52fae`. Callback is `wwm::Pryon::pryonLoggingCallback(int, char const*, char const*)` `0x53570`.
2. `PryonApi_SetEnumeratedResultCallback(cb)` — `0x5301a`. Callback is `wwm::Pryon::resultsCallback(char const*, _PryonEnumeratedResult*)` `0x536b0`; first arg is the decoder id.
3. `PryonModelSet_New("WakeWordManager_ModelSet", "<dir>/pryon.manifest", "")` — `wwm::Pryon::newModelSet` `0x54c2e`. Inner function `0x69e810` rejects with `null or empty PryonModelSetId`, `null or empty PryonManifestPath`, `null PryonSituationList`, and logs `Creating model set <id> from <path> using Pryon engine build <uuid>`.
4. `PryonDecoder_NewPryonMultichannelAudioFormat_Default(&fmt)` — `0x602a30`. Writes `{0, 16000, 16, 1}` then 32 ints `{0, 5, 5, …}`; total 0x90 bytes.
5. `PryonDecoder_NewSpotterAudioDecoder(decoderId, modelSetId, "pryon", fmt /* by value */, "{}")` — `wwm::Pryon::createDecoder` `0x5658c`. The wrapper at `0x6033d4` copies 0x90 bytes starting at the `r3` slot and reads the fifth argument at `[r7+0x98]`, which confirms by-value passing and the struct size. Stock code renames the calling thread to `PryonT` around this call, so the decoder thread inherits that name.
6. Loop: `PryonDecoder_PushAudioEventSamples(decoderId, sampleIndex, samples, sampleCount)` — `wwm::PryonDecoder::pushAudio(unsigned long long, short const*, unsigned int)` `0x50034`. Inner `0x6bb0c4` computes `bytes = sampleCount << 1` (`0x6bb110`), so the count is in samples.
7. On a gap in audio: `PryonDecoder_SessionEnd(decoderId)` — `wwm::PryonDecoder::endContiguousAudio` `0x50132`.
8. Teardown: `PryonDecoder_Delete(decoderId)` (`0x4ff88`), `PryonModelSet_Delete(modelSetId)`.

`libAmazonKWD.so` (the older AVS-SDK-style detector) uses `PryonDecoder_PushAudioEvent(decoderId, PryonAudioEvent*)` instead. That event is a typed/JSON object (`samples`, `samples_with_annotation`, `wakeword_indices`, `cms_json`, `lookahead_metadata_blob`, `utterance_boundary` — strings at `0x6bc5ba…`). Not needed; `PushAudioEventSamples` is simpler. It also calls `PryonDecoder_BacklogWait(decoderId, -1)` (`0xafaa`).

## Result struct

Fields read by `wwm::PryonDecoder::setKeywordDetectionInfo` (`0x50c10`) and `kwd::PryonKeyWordDetector::resultsCallback` (`0x9c20`):

| Offset | Type | Use |
|---|---|---|
| `+0x08` | u64 | sample index, copied to `KeywordDetected[0]` |
| `+0x1c` | `const char*` | keyword name |
| `+0x24` | i32, 0..3 | detection type. wwm remaps `{0,1,2,3} → {1,2,0,3}`. AmazonKWD proceeds only for 2 and 3, and sets a flag for 3 |
| `+0x28` | u32 | metadata blob size |
| `+0x2c` | `void*` | metadata blob (wwm logs `result contains no metadata!` when empty) |
| `+0x40` | u64 | sample index, copied to `KeywordDetected[8]` |

Which of `+0x08` / `+0x40` is begin and which is end is a guess. The model config has `keyword_spotter.emit_nearmiss = 1`, so expect callbacks for near misses; types 0 and 1 are probably those.

## Model on the system image

`/system/local/models/keyword/en-US/ALEXA/`: `pryon.manifest` → model `pryon` → `pryon.config` (LFBE front end, 64 mel bins, 25 ms / 10 ms) → `nemort_scorer_config.json` → ONNX int16 model. Keywords `ALEXA`, `STOP` (`words.shrunk.txt`); accept threshold 0.923, lower when client properties such as `AudioPlayerState` are set (`kw.cfg.json`). All plaintext. The ONNX runtime ("nemort") is inside libpryon; the `libtvm_modelops_ww_*.so` files are only for TVM-format models, which this one is not.

PuffinApp normally prefers newer models downloaded to `/data/.../speech/wakeword_models/davs/resources/`; the `/system` copy is the fallback.

## Secure mode

`PryonApi_TurnOffSecureMode` (`0x600e38`) calls a virtual method on the object at `api+0x184`, which is `srgs::SmcApi` (`disableSecureMode`, source `grxmlparser/smc_api.cpp`). SMC is the supplemental-model compiler for ASR grammars. No stock caller of the keyword path (`libWakeWordManager`, `libAmazonKWD`, `libPryonDetector`, `PuffinApp` imports) calls it, and the keyword model is unencrypted, so it does not gate wake-word loading.

## Runtime needs

- `DT_NEEDED`: `liblog libfstfar libm libfst libdl libc++_shared libc`. All in `/system/lib`.
- Imports `dlopen`, `getenv`, `__system_property_get`, `socket`/`connect`, `sched_setaffinity`. Names seen for `dlopen`: `libtvm_modelops*.so`, `libffi3.so` — not needed for the ONNX model. Which env vars and properties are read was not traced.
- libpryon uses `libc++_shared` (NDK `std::__ndk1`) while the rest of the system uses platform `libc++`. A plain C caller avoids any conflict.

## Open questions

1. `detectionType` values: which is accept, which is near miss. Test tool should print every callback.
2. Begin/end order of the two sample indices.
3. Preferred chunk size. `wwm::PryonDecoder::frameSize()` exists but was not read; 10 ms (160 samples) matches the front-end shift and is a safe start.
4. Whether `PryonModelSet_New` verifies `checksum.txt` against the files (strings mention `wwModelChecksum`, `gRootChecksum`). Relevant only if the model directory is modified.
5. Whether anything in libpryon misbehaves without Amazon system properties or under `qemu-arm`.
6. `PryonDecoder_BacklogWait` timeout unit.
