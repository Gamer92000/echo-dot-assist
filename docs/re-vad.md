# Voice activity in the stock front end

Firmware NS65741: donut (Echo Dot 3) `libasp.so` md5 `33183bec8dffe8cb81d89b0afb74e23a`, biscuit (Echo Dot 2)
`8ee7dcdeaa8be49ab1e4a446eca4f2ae`. Static analysis of donut's `libasp.so` (`firmware/donut/re/libasp.ann.asm`) and of
`libAFEMetadataDecoder.so` (md5 `70b9ec4e933db6990255adc33522b182`, the same file on donut, biscuit and checkers),
the DAVS wake word package stock Alexa had cached on the Dot 3 (`device-logs/davs/`), plus every micAsr capture in
`device-logs/` decoded frame by frame, and a live dump from an Echo Dot 2 (2026-10-28, hassmic running, quiet room).
Addresses: `libasp.so` vaddrs (= file offset + 0x3000), `libAFEMetadataDecoder.so` vaddrs (= file offset). For the
issue behind it see GitHub #66. Anything not shown by code, config or a capture is marked **UNVERIFIED**.

## Summary

- **There is a per-frame voice signal, and hassmic already receives it.** The front end runs a DNN voice activity
  detector (`vad_lite.tflite`) on every 8 ms frame and writes its probability, quantised to 2 bits, into the lowest
  bit of the micAsr samples, in every 128-sample frame, next to the 16-bit clock Pryon reads. No LIPC, no polling:
  it arrives with the audio hassmic already reads.
- **The value is `floor(4 * p)` capped at 3**: 0 = p < 0.25, 3 = p ≥ 0.75. With it come two 5-bit posteriors of the
  front end's "UED Fusion" (user engagement and its "ES" node), the frame RMS in dB, the playback flag, ERLE, volume
  and more (layout in section 2).
- **It follows the talker.** Echo Dot 2 captures: normal voice 99–100 % of speech frames at ≥ 2, quiet voice 89–100 %,
  whispers 25–80 %; non-speech sounds as loud as the voice 0–7 %; the Echo's own replies (residual up to -35 dBFS)
  0 %. It rises within 1–4 frames of a word and falls below 2 within ~0–300 ms of the last loud frame.
- **It misses quiet, distant speech.** A quiet sentence on the Dot 3 at ~7 dB over the floor (`mic-compare-143359`)
  never left 0. So it can say "speech has ended", not "there was no speech".
- **Background voices: not tested.** It is a speech detector, not a talker detector; nothing shows it would tell the
  user from a conversation or a TV in the room (**UNVERIFIED** either way; no capture has one).
- **The "VAD" stage of `AFE.cfg` is something else**: the arbitration energies (voiceEnergy/ambientEnergy), read once
  per wake word through `LASP_CMD_REQUEST_ARBITRATION_JSON`. `vadThreshold`/`vadHangover` belong to the VoIP downlink
  noise reducer.
- **Stock never used it to end a request.** PuffinApp stops capture on the cloud's `StopCapture` directive. The
  on-device end-of-speech detectors are elsewhere: Pryon's natural turn taking (NTT, which does read the UED and DNN
  VAD fields from the LSBs) and AHE's local-ASR endpointer, both behind DAVS models; on the Dot 3 the NTT's own DVAD
  scorer was commented out of the cached package.

## 1. The stages

`/system/vendor/etc/audio-algorithms/AFE.cfg`, ASR path. Donut and biscuit have the same entries and the same
`vad_lite.tflite` (md5 `9727555d2d494f9e6a16968d8b160b03`, 50704 bytes; also on athena, checkers, radar).

| entry | what it is | output |
|---|---|---|
| `"DNN VAD": "DNN VAD"` | TFLite model through EdgeFlow: input `input_1` [1,64,1,6] float, two recurrent states [1,42], output [1,1,1]; ops CONV_2D ×9, FULLY_CONNECTED ×6, LOGISTIC ×5, TANH ×2. One probability per 128-sample frame. Config: `Use STFT input` false, `Use AEC output` false, `Number of Mics` 1, `ASR Processing Equivalent Delay` 160 samples ("0 when using AEC output"), `ASROut LSB bit offset` 16 | `UED Fusion`'s VAD evidence, and the LSB metadata (section 3) |
| `"VAD": "Voice Activity Detector"` | arbitration energies (ESP). `getArbitrationData() VAD for Best to Listen not instantiated` (0x13672c); its config is the gains of the arbitration JSON (`PGA Gain`, `AFE Out Gain`, `referenceAudioLevelOutputInDb`) | `LASP_CMD_REQUEST_ARBITRATION_JSON` only |
| `"UED Fusion": "UED Fusion"` | Bayes net: UE (user engaged) → ES → observations VSS, DTD, VAD, with PBA (playback active) as second parent; HMM post-processing (`Post Proc Transition Matrix UED/ES`). Priors `*_cpd_priors.cfg`; ultrasound proximity, SSL and face detection inputs only on devices that have them (UED mode 1/2, `e4f0e`; the Dots run the defaults of `e4fb4`: SSL 0, SSL prob 0.5, FD prob 0.5) | LSB metadata; a log line on request |
| `vadThreshold`, `vadHangover` | `VOIP Rx Fullband NR` (call downlink) | – |

The input of the DNN VAD is two complex spectra: one from a Q31 mono buffer converted to float (`e6348`), one from a
second buffer (`e63fc`), turned into magnitudes per bin (`e648e`). The config reads as "one raw mic, delayed 160
samples to line up with the ASR output". The replies of the Echo itself never set it (section 4), so the model
presumably also sees the playback reference. Which buffers the caller passes is **UNVERIFIED** (the UED is reached
only through the AFE vtable, slot +0x2dc, and no caller of that slot was found in `libasp.so` or `mixer`). How
`ASROut LSB bit offset` (config +0x28, printed at 0x6f756) is used was not traced; the value reaches the LSBs
through the UED re-encode below, not directly.

## 2. The LSB metadata, version 3.3

Encoder `libasp.so` 0xbcb90 (version switch `tbh` at 0xbcc3c, 11 layouts; index 10 = 0xbd35c = version byte 0x63).
Stock decoder `libAFEMetadataDecoder.so` (`AudioMetadataDecoder::processAudio` 0x79f0: 128-bit sliding window, sync
0xa5 at 0x7b1a; `decodeMetadataVersion3_0` 0x89a8, `3_3` 0x8cf8). Bits are MSB first, one per sample, in the lowest
bit of the s16 sample; a frame is 128 samples and starts at a stream sample index ≡ 0 (mod 128) (every hassmic dump
starts on one: the "capture sample" numbers in `boot.log` are multiples of 256).

| samples | bits | field | source in the encoder (AFE = `GenericAFE` object) | seen |
|---|---|---|---|---|
| 0–7 | 8 | sync 0xa5 | `bcbd2` | |
| 8–15 | 8 | version: major 3 bits, minor 5 bits | 0x63 = 3.3 | every frame of every capture, both Dots |
| 16–22 | 7 | magic 0x68 | encoder refuses other values (`bd35c`) | |
| 23–28 | 6 | frame counter | AFE +0x11a8 (`70800`) | +1 per frame, wraps at 64: drop detection |
| 29–44 | 16 | clock, ms | AFE +0x15c8, a 64-bit time / 10^6 (`716a4`) | jitters (frames are processed in bursts), 8 ms on average |
| 45–52 | 8 | ERLE, dB, 0..255 | AFE +0x166c, from the AEC (`72276`, `722fc`: 10·log10 ratio) | 0 without playback |
| 53 | 1 | flag set with the ERLE field | always 1 (`71162`) | 1 |
| 54–61 | 8 | frame RMS, dBFS + 256 | `bcb20`: 10·log10(mean (s & ~1)²) − 90 | matches the audio within 1 dB |
| 62–63 | 2 | ? | AFE +0x994 object, byte +0x80/+0x81 | |
| 64 | 1 | ? | AFE +0x888 object, byte +0x1d4 | 0 |
| 65–66 | 2 | ? | two state bytes (`711c2`, `711e2`) | 0 |
| 67 | 1 | playback active (**UNVERIFIED** name) | AFE +0x1198 | 1 exactly while the Echo plays (earcon, reply) |
| 68–74 | 7 | volume | AFE +0x11e0 | = `CurrentVolume` of `LASP_CMD_GET_ASP_DEBUG_INFO` (60 live) |
| 75 | 1 | mic muted | AFE +0x11b4 | 1 in the muted capture (`mic-compare-143409`, -90 dBFS) |
| 76–83 | 8 | ARA value × 255 (Pryon's `ARA_VSS`) | ARA_V2 getter 0xce4fc | ~1.0 in silence, ~0 in speech and playback |
| 84–88 | 5 | AEC value × 31, probably double talk (Pryon's `DTD`) | AEC_V2 float +0x394 (`70764`) | 0 most of the time |
| 89–93 | 5 | **UED** posterior × 32, capped 31 | UED +0x40 | 5 at rest, up to 29 in speech |
| 94–98 | 5 | **ES** posterior × 32, capped 31 | UED +0x44 | 0 at rest, up to 30 in speech |
| 99–102 | 4 | ? (ultrasound proximity on devices with it, **UNVERIFIED**) | – | 0 |
| 103–105 | 3 | SSL angle / 45° | UED +0x60 | 0 on the Dots |
| 106–107 | 2 | SSL probability × 4 | UED +0x58 | 2 (= the 0.5 default) |
| 108–110 | 3 | face angle / 45° | UED +0x70 | 0 |
| 111–112 | 2 | face probability × 4 | UED +0x68 | 2 (default) |
| 113–114 | 2 | **DNN VAD probability × 4**, capped 3 | UED +0x78 | see section 4 |
| 115–119 | 5 | ? | record +0x10 | 0 |
| 120–127 | 8 | checksum = number of 1 bits in samples 0–119, mod 256 | decoder: popcount per field (0x83a0) | 100 % of frames in all captures |

Names in bold or with Pryon names are confirmed by the UED log format (below) or by Pryon's signal list
(`ntt.cfg.json` `afe_metadata_collection`: `ERLE_RAW`, `ARA_VSS`, `DTD`, `UED`, `DNN_VAD_PROB`, `VOLUME`, `FD_ANGLE`,
`FD_PROB`, `SSL_ANGLE`, `SSL_PROB`, `PLAYBACK_ACTIVE`, `ULTRA_PROX_DISTANCE`, `ULTRA_PROX_CONF`). The stock decoder
scales the two 5-bit posteriors by 1/32, the AEC value by 1/31, the 8-bit ARA value by 1/256.

## 3. How the VAD value gets into the frame

Two passes over each output frame, both in `libasp.so`:

1. **ASR path.** The AFE process function (0x713c4) builds the record on its stack (`70800`, `71162`–`7133e`: magic,
   counter, clock, ERLE, RMS, flags, volume, mute, ARA, AEC; the UED bits left 0) and encodes it into the frame
   (`7134a`: `bl 0xbcb90`, encoder object AFE +0xf1c).
2. **UED re-encode** (`encodeWithUEDMetaData`, 0x7a794, AFE vtable +0x2e0 at 0x1f93b4). Decodes the frame again
   (0x81e44 into AFE +0x16b0), and only if the version byte is 0x63 and the UED object exists (AFE +0x77c): copies
   the UED's results (`e4ece`: 0x5c bytes from UED +0x24 to AFE +0x780) and packs them into the record word at AFE
   +0x16bc: UED +0x40 ×32 → bits 5–9, +0x44 ×32 → 10–14, SSL angle /45 → 19–21, SSL prob ×4 → 22–23, face angle /45
   → 24–26, face prob ×4 → 27–28, **VAD prob ×4 → 29–30** (`7a93e`), then re-encodes. Every 0x927c (37500) frames it
   logs a heartbeat ("Last 16-bit timestamp decoded at the same time, ASR path timestamp: %d, UED path timestamp: %d"):
   the UED runs on a path of its own and its latest result is stamped into the ASR frame being encoded.

The UED process (0xe4ee4, AFE vtable +0x2dc through 0x7a784): its DNN VAD object is UED +0x14; `bl 0xe6340` → DNN VAD
(0xe6448: features, EdgeFlow run, `"DNNVAD ERROR! : execute fail"` 0x1402e3) writes the output tensor's first float
(tensor index = `Output index` 0, `e69aa`) to UED +0x18, copied to UED +0x78 (`e4fec`). No smoothing between the
network and the metadata; the network itself is recurrent. The fusion (0xe511c) feeds "VAD" = UED +0x78 as evidence
(string at 0x134266) beside PBA, VOL, ERLE, VSS, DTD, SER.

Other outlets, none per frame for a client:
- **Log line.** "UED: Frame: %d, PBA: %d, Vol: %d, erle: %f, ser: %d, vss: %f, dtd: %f, es: %f, upDec: %d, …,
  vad: %d, vadConf: %f, ua: %f, ued: %f, ued_od: %f" (0xe5440), every N frames once `LASP_CMD_SET_UED` gets
  `{"uedCmdId":2,"uedCmdValue":N}` (0xe554c; id 1 is "unsupported ued command"). Full precision, but logcat. Not
  tried: it changes the mixer's logging.
- **LIPC.** `com.doppler.lasp` events are `beam_direction`, `beam_index`, `notify_ultrasound_event`,
  `notify_lpmsd_event` (`libgenericaspclient.so`); no getter among the 65 `LASP_CMD_*` names returns the VAD.
  `LASP_CMD_GET_ASP_DEBUG_INFO` on the Echo 2: pipelines `asr` (9 ch in, 1 ch out) and `speaker`, mode flags, no VAD.
- **Mixer's `AlgoMetadataDecoder`.** Only counts corrupted LSB frames for metrics (`mixer` strings).
- `LASP_CMD_SET_WWF_MODE` / EFUM only log in this AFE class (`7ac10`, `7ac20`).

## 4. Against the captures

All decoded with the layout above (`device-logs/vad/afemeta.py`, git-ignored like the captures; every frame of
every capture passes sync, magic and checksum). "Speech span" = the cut of `device-logs/whisper/segs.txt`. ≥2 = p ≥
0.5. The two Echo 2 dumps of 2026-10-28 are `device-logs/vad/echo2-dropin-20261008.raw` (the dump hassmic still had
from 2026-10-08) and `echo2-quiet-20261028.raw`.

| capture | device | what | frames ≥1 | frames ≥2 |
|---|---|---|---|---|
| `whisper/A1–A3` | Echo 2 | normal voice, -39..-46 dBFS peak, speech span | 100 % | 98.8 / 100 / 100 % (89–100 % at 3) |
| `whisper/C1–C3` | Echo 2 | quiet voice, -48..-49 dBFS | 100 % | 89 / 100 / 100 % |
| `whisper/B1–B3` | Echo 2 | whispered, -51..-52 dBFS | 60 / 100 / 76 % | 25 / 80 / 34 % |
| `whisper/x1`, `x2` | Echo 2 | sounds without words, -43 / -51 dBFS | 1.6 / 8.9 % | 0 / 7 % |
| `whisper/rec-de` 50–58 s | Echo 2 | the sounds after the last command (no words), up to -43 dBFS | 7.4 % | 3.5 % |
| `whisper/rec-de` 0–4 s | Echo 2 | quiet room | 0 % | 0 % |
| `whisper/live3-de`, 4 replies | Echo 2 | the Echo's own reply (playback flag 1), 4 × 2.5 s, up to -35 dBFS | 0.1 % | 0 % |
| `whisper/live-de` 20.3–24.8 s | Echo 2 | own reply, up to -37 dBFS | 1.1 % | 0 % |
| `vad/echo2-dropin-20261008` | Echo 2 | Drop In call, far end from the PC test peer (content not noted) | 1.4 % | 0.3 % |
| `vad/echo2-quiet-20261028` | Echo 2 | quiet room, 12 s, -66..-77 dBFS | 0 % | 0 % |
| `cap-first` | Dot 3 | 131 s of a steady low sound (156–375 Hz, ±2 dB), content not noted | 1.1 % | 0.3 % |
| `micAsr-170444` | Dot 3 | talking, whole file | | 48 % |
| `mic-compare-143359` 4.3–7.2 s | Dot 3 | quiet sentence ~7 dB over the floor, listening mode 1 | 0 % | **0 %** |

Timing (Echo 2, `rec-de` 14.30 s and 18.72 s; frames of 8 ms):
- **Onset:** level up at 14.296 → value 1 at 14.312, 2 at 14.328, 3 at 14.360.
- **End:** level at the floor from 18.736 → 3 until 18.80, 2 at 18.816, 1 at 18.880, 0 at 18.928. ES 27 → 11 at 18.88,
  1 by 19.06. Over 43 utterance ends in four captures: below 2 within 0–300 ms of the last frame 10 dB over the floor,
  0 within ~50–450 ms (level-based reference, rough).
- **Pauses inside a sentence:** normal voice (`rec-de` 14–22 s, three sentences, 40 ms means) never below 2;
  commands through Home Assistant (`live*-de`) dip below 2 for up to 0.44 s (between wake word and command, between
  words); quiet voice sits at 1 for up to ~0.6 s at the soft end of a sentence; whispers (`rec-de` 30–38 s) go
  between 0 and 2 inside a sentence.
- **Lag:** none measurable against the audio of the same frames (level/value cross-correlation flat over ±40 ms).

Listening mode: the value works without it (`rec-de`, `live-de` outside requests: hassmic sets it only while a
pipeline streams) and with it (`live3-de` commands, after the wake word). No code on the UED path reads the
utterance flag (AFE +0x11d7); whether the DNN's input is upstream of the cancellers the flag freezes is **UNVERIFIED**.

## 5. What stock did

- **PuffinApp**: the cloud ends the request (`StopCapture` directive, `SetEndOfSpeechOffset`; metrics
  `ServerProcessing.UtteranceEndToStopCapture`), plus a local `LISTENING_TIMEOUT_EXPIRES`. It links the metadata
  decoder but imports only `afeDecoderCreate/Destroy/ProcessAudio/SetLoggingCallback`, no field getter.
  `libacsdkEndpointDetector.so` is the smart home "Alexa.EndpointDetector" capability, not speech.
- **Pryon NTT** (natural turn taking, wake-word-free follow-ups): `libWakeWordManager.so` sets
  `PryonApi_SetNttEndOfSpeechCallback`, `PryonDecoder_SetNttConfiguration`, `PryonDecoder_PushNttEvent`
  (`PryonDecoder::processNttEndOfSpeech`), PuffinApp has `nttEndOfSpeechCallback` and `NaturalTurnTakingHandler`
  (`enableDVAD`/`disableDVAD`, `LASP_CMD_SET_WWF_MODE`). The cached DAVS wake word package of the Dot 3
  (`device-logs/davs/…/wakeword_alexa_c2d2431c…/unpacked/nttfusionconfig/`) shows the inputs: `ntt_conv` =
  `DVAD_PB AND UED AND NO_LOCKOUT`, where UED is the LSB field (`afe_ued.cfg.json`: signal `UED`, `ABOVE` 0.5) and
  DVAD is a 0.8 MB device-directed-speech ONNX model inside Pryon (`dvaddecoder/`). In that package
  `dvaddecoder/pryon.config` has its scorer line "Commented out by BDP Fragment Assembler", so NTT could not run on
  the Dot 3 (**UNVERIFIED** at run time; `ntt-listening.animation` exists in `led-resources`). The same config has
  Pryon collect `DNN_VAD_PROB` and the other fields around each NTT detection (`afe_metadata_collection`).
- **AHE** (local ASR, "Litespeed"): `EndOfUtterance`, `EstimatedEndOfUtterance`, and in `libpryon.so`
  `PryonNodeEOSEstimation` (`vad_assisted_backporch.cc`, `eos_vad_voting_queue.cc`, `uttdet_vad_feature_extractor.cc`):
  an endpointer of the local recogniser, needing Litespeed models from DAVS that the Dot 3's AHE policy did not
  select (re-whisper.md: "caching" pipelines).
- **PryonLite** has an energy VAD for low-power mode (`PryonLiteDecoder::processVadEvent`), not for endpointing.

## 6. Reading it in hassmic

- **Where:** in the capture thread, on the raw micAsr block from `cap_read()`, before the gain, RNNoise or any other
  processing (as `wake_feed` and the dump get it). Frames start at stream sample index ≡ 0 (mod 128); check sync,
  version, magic and checksum per frame and search sample by sample after a mismatch (a stream reopen), as the stock
  decoder does.
- **Cost:** ~128 bit operations per 8 ms frame; no syscalls, no LIPC, nothing polled.
- **Latency:** none on top of the audio: the value rides in the frame it describes, so it lines up with what hassmic
  streams to Home Assistant. The network's own tail is ~0.1–0.3 s.
- **Resolution:** 2 bits. The ES (bits 94–98) and UED (89–93) posteriors have 5 bits, fall a little slower
  (ES 0 at rest, ≤ 4 during replies, up to 24 on single loud non-speech sounds) and could serve as a second opinion.
- **Use for #66, from these numbers:** only after the value was ≥ 2 for a while in this request (the talker was
  seen); then "0 for ~0.8–1 s" ends about 1 s after the last word without cutting the pauses seen (≤ 0.44 s below 2
  in voiced commands; value 1 holds quiet ends and most whispering; non-speech is 0 in 91–100 % of frames). Never end
  because it stayed 0: the Dot 3's quiet sentence did. Whether a second talker or a TV holds it up is the open point
  that decides whether it beats Home Assistant's own VAD for #66.
- **Muted:** digital silence, mute bit 1, value 0.

What hassmic does with it (2026-10-28): `src/hassmic/afemeta.c` decodes a frame (sync, version, magic, checksum;
VAD, RMS, playback, mute; `tests/unit/afemeta_test.c`; the same counts as above on `cap-first`, `thump/E_micAsr`,
`vad/echo2-dropin-20261008` and `whisper/A1`, the last at phase 64: the segments are cut). `main.c` `ring_speech`
reads it from the wake word ring (raw micAsr by capture index), phase 0 until a frame fails, then looks for it in
the 128 samples before. `micend.c` (setting `end_of_request`, issue #66) uses it as a veto, not as the end itself:
a frame counts as the talker only when it is near the keyword's level *and* the VAD is >= 1 (or the stream has no
records), so non-speech as loud as the voice does not hold a request open, and speech the VAD misses never ends one
(only Home Assistant's VAD does then). The log line `end of request: heard here (...; Amazon's VAD heard no speech
in <n> of <m> loud frames)` shows what it vetoed per request. `tests/fake_ha_end.py` writes records into the PC's
capture (talker 3, dishes as loud 0) and moves their phase every loop. Veto only at 0, not under 2: inside
commands it dips under 2 for up to 0.44 s (mostly to 1), and aggressive ends after 0.45 s.

## Open questions

- **Background talkers, TV, music**: no capture has them. Test: a radio talk show at 2–3 m, the user speaking a
  command, both with listening mode 1; compare value 3 frames during radio only vs. user + radio, and the ES / UED
  fields.
- The DNN VAD's two input buffers (caller of AFE vtable slot +0x2dc not found).
- Samples 62–66, 99–102, 115–119: sources found for some, meaning not.
- The Android Echos (checkers, athena): their `AFE.cfg` has the same DNN VAD and UED entries and checkers has
  the same decoder library; whether the HAL's AudioRecord output carries the LSB metadata is **UNVERIFIED**.
- Full-precision values through `LASP_CMD_SET_UED` logging (not tried: changes the mixer's behaviour).
