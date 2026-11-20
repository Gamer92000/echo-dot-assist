# pryon-native: Amazon's wake word engine, reconstructed from its behaviour

A PC-side reimplementation of what `libpryon.so` (firmware 6574.1) does for a wake word, and the evidence that it matches
the real library.  It was reverse engineered independently from the shipped binary: by running it under qemu, reading
what it computes at chosen points, and disassembling functions where behaviour alone did not decide.  No Amazon source
code was used.  (It is not a "clean room": the binary was disassembled and executed.)  Nothing of Amazon's is in this folder: the
tools read the model and the library from the firmware you unpacked (`devices/donut/README.md`).

[`LEDGER.md`](LEDGER.md) is the log of every measurement, in order, with what each one ruled out.  The Echo-side use of it,
training a wake word, is in the separate [training contribution](https://github.com/BenStein1/echo-dot-assist/tree/pr-train-wake-word),
`docs/re-pryon-training.md` and `src/hassmic/enroll.c`; those files are not part of this research branch.

## What is reconstructed, and what it was checked against

| Piece | Where | Matches the real engine |
|---|---|---|
| Audio front end (LFBE, 64 mel bins) | `pryon_lfbe.c`, `pryon_runtime_analysis.lfbe()` | bit for bit, all 828 frames of the test file; each stage (windowing, Ne10's FFT, power, mel sums in OpenBLAS's order, bionic's `logf`) captured and compared separately |
| The network (7 convolutions, 2 LSTMs, the heads), with the engine's own int8 arithmetic | `pryon_quant_model.py` | conv2 to conv7 bit-exact on the engine's inputs; the LSTM input within 1.1e-6 on every cycle; the classifier within 2.4e-6 |
| The 25-lane LSTM state ring, per-row quantization, gate order | `pryon_lstm_analysis.py`, `LEDGER.md` | the engine's own LSTM buffers, 6e-7 |
| The decision rule (when it accepts, the score, the lock) | `pryon_decoder.py` | 12 of 14 of the engine's events to six digits, two within 0.003 |

## Verifying it yourself

Needs: the unpacked firmware (`firmware/donut/rootfs`), `qemu-arm` and `bwrap` (bionic needs a PID namespace), `make
DEVICE=donut` for `build/donut/pryon_test`, and `pip install -r requirements.txt`.  The captures are made with the engine
in a debugger: `pryon_debug_capture.py` runs `pryon_test` under qemu's GDB stub, sets breakpoints at the matrix-product
kernel (+0x123897c) and the stages named in `LEDGER.md`, and only reads registers and memory (it never writes the guest's
memory or any file of the firmware).  Run the commands in `LEDGER.md`, "Reproduction", from the repository root with
`PYTHONPATH=tools/pryon-native`, for example:

```sh
# 1. the engine's activations on a test file, every matrix product (hours at 0.01x pace, minutes for one clip)
bwrap --ro-bind / / --bind "$PWD/local-data" "$PWD/local-data" --dev /dev --proc /proc --unshare-pid --die-with-parent \
  python3 tools/pryon-native/pryon_debug_capture.py --hook 0x123897c --gemm --hits 560 --speed 0.01 \
  --out local-data/rea-pryon/layers
# 2. the emulator against it, layer by layer
PYTHONPATH=tools/pryon-native python3 tools/pryon-native/pryon_layer_compare.py local-data/rea-pryon/layers testdata/alexa_espeak.raw
# 3. the decision rule against the engine's own events
python3 tools/pryon-native/pryon_decoder.py testdata/alexa_espeak.raw      # then compare with pryon_test's RESULT lines
```

`python3 tools/pryon-native/tests/test_pryon_runtime_capture.py` runs the standalone debugger/quantization checks.
`python3 -m unittest discover -s tools/pryon-native/tests` runs the checks that need no firmware: the LSTM contract against
ONNX Runtime, the quantizer, the debugger's wire protocol, and the front end against an independent float64 version.
`make unit-pryon_qat` (with the training tools) checks the trainer's `exp`/`log`/`log1p` against a hash taken from the Echo's own libm.

## What is not here

The float translation of the model to standard ONNX operators that the early comparisons used (`LEDGER.md`, "Native float
encoder versus stock"), and the experiments that trained heads on recordings, are left out: they are the way here, not the
result.  The Echo-side training is in `src/hassmic/enroll.c` and `src/tools/`.

## Third-party work this follows

Kaldi's feature pipeline (Apache-2.0) as the shape of the front end; Ne10's NEON real FFT (BSD-3-Clause), reimplemented in
its operation order in `pryon_lfbe.c`; FreeBSD's msun `logf`/`exp`/`log`/`log1p` (Sun's permissive notice, kept in the
sources); OpenBLAS's ARMv7 `sdot` as an order of operations observed in the library.  Each is credited where it is used.


## Pinned harness build and public regression vectors

The harness guard is reproducible; it is not the digest of an unavailable release executable. On Linux x86_64 use
Android NDK r21e (`Pkg.Revision = 21.4.7075529`, Clang 9.0.9, Android build 7019983). The archive used by this repository's
CI is `https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip`, SHA-256
`ad7ce5467e18d40050dc51b8e7affc3e635c85bd8c59be62de32352328ed467e`.
Unpack it under `toolchain/android-ndk-r21e`, and unpack your own Dot 3 firmware as described in `devices/donut/README.md`.
No firmware or model is supplied by this contribution.

```sh
make DEVICE=donut OUT=build/review-harness build/review-harness/pryon_test
sha256sum build/review-harness/pryon_test
readelf -Ws build/review-harness/pryon_test | grep ' main$'
```

Verified again during review: SHA-256 is
`107e8079ca73ac0c2d30d4cc1bb2cd62d78ffe8202bf64fa97cc643cdea23f84`; ARM `main` has ELF value `0x1098`.
The target is `armv7a-linux-androideabi24-clang`, with `-O2 -Wall -Wextra -fPIE -Isrc/include`, linked with
`-pie -fuse-ld=lld -Wl,--allow-shlib-undefined -Wl,--unresolved-symbols=ignore-in-shared-libs` against the firmware's
`system/lib/libpryon.so` and `libz.so`. Version/build defines in the Makefile are unused by this harness.
Use the normal `build/donut/pryon_test` target for captures; the alternate output above preserves any existing binary.
A different toolchain may give a different digest. Use the pinned recipe rather than removing the check: both capture
scripts depend on this executable's breakpoint address. Library SHA-256 remains independently checked.

`tests/golden_lfbe.json` contains a deterministic synthetic PCM generation rule and frontend output vectors. There
is no recording, weight or private capture in it. Its provenance is the reconstructed implementation: it is a public
regression oracle, **not an independently captured Amazon-runtime oracle**. The silence, float64 reference, quantizer,
LSTM-contract and debugger protocol checks also require no firmware. Run them with:

```sh
python3 -m pip install -r tools/pryon-native/requirements.txt
python3 -m unittest discover -s tools/pryon-native/tests
python3 tools/pryon-native/tests/test_pryon_runtime_capture.py
```

The detailed stock-library measurements in `LEDGER.md` remain historical measurements whose private captures are not
published. Reproducing those measurements means collecting new captures from your own firmware with the guarded
scripts; the public synthetic tests alone do not substantiate stock-runtime parity or acoustic accuracy.
Full license conditions and disclaimers are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `licenses/`.
Generated wake-word packages still contain Amazon-derived model data; keep them out of source releases. Redistribution
permission for those assets has not been established.
