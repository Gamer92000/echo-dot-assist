# Pryon's wake word head and what training one needs

What was learnt about Amazon's engine (`libpryon.so`, firmware 6574.1, sha256 `65df801e…f97027`) to train a wake word on
the Echo (`src/hassmic/enroll.c`).  Measured on the real library under qemu and on an Echo Dot 3; nothing here is Amazon's
text.  Offsets are for that build.

## The model and its last layer

A wake word set (`models/<word>-<locale>/`) holds one ONNX model, `runtime_stateless_standard.streaming.fixed.int16.onnx`
(109 nodes: 7 `HybridConv`, 2 `HybridLSTM`, 4 `HybridGemm`, Pryon's own int8 operators), and small configs: `pryon.config`,
`kw.cfg.json` (the keyword spotters and thresholds), `op.cfg.json` (what a keyword does in each state),
`watermark.cfg.json`, `words.shrunk.txt` (the symbols `<eps> 0`, `ALEXA 1`, `STOP 2`), `checksum.txt`.

The set the feature starts from is the firmware's own "Alexa" set.  The wake word's output is one `HybridGemm`,
`fc_output_ww`: an int8 weight matrix [3,100] (rows: background, the keyword, STOP), per-row scale and zero point, per-row
weight sums (`int32_data`, not raw data) and a float bias added by a separate node.  Training a word means fitting row 1
(and its scale, zero point, sum and bias) on the 100-dimensional input of that layer; the encoder stays as it is.  The
set made here is Amazon's with that one row replaced, the word in `ALEXA`'s place in the configs and words file, and the
`STOP` spotter and its rules cut out (symbol 2 stays: the network still has three outputs).  The engine does not appear to check
`checksum.txt` against the files (a set with a checksum of our own, 32 hex characters, loads), and an extra file in the folder
(`enrolled.txt`) does not stop it loading.

## The head's input, per cycle

The engine runs the network every 6 frames of 10 ms.  The head sees a [25,100] matrix each cycle: one row per
LSTM "lane" (the encoder's 100-value output goes through two LSTMs whose state is a ring of 25 lanes; before cycle t
the state of lane t mod 25 is zeroed, all lanes at cycle 0).  A lane's age in cycles is (t − lane) mod 25 + 1.  The first
cycles after a start carry uninitialized convolution history (NaN or garbage for about five) and the lanes are still filling:
training ignores the first 30.

`pryon_extract` reads that matrix from the engine itself: the head's matrix product is a function in the library
(+0x123897c, the quantized `HybridGemm`/`HybridConv` kernel; called with the activation matrix, the packed weights, scales,
zero points and sums), and the tool, in its own process, redirects its first instructions (8 bytes, checked against the
expected ones, and the library by its SHA-256) to a stub that copies the matrix and jumps on.  Only that process's private
copy of the pages changes.  The activations match a PC-side reimplementation of the whole network to 2e-6.

## The head's arithmetic (what training must reproduce)

Each row of the activations (and of every convolution's input patch) is quantized by its own range:
`s = (max − min)/127`, `inv = float32(1/s)`, `z = rint(−min·inv)`, `q = min(rint(x·inv) + z, 127)` (round half to even, a
reciprocal multiply rather than a division: the difference shows on exact halves), then
`y = float32(Σ (q − z)(w − zw)) · float32(s · sw) (+ bias)`.  A row whose values are all equal has no measured rule.
Convolutions add their bias inside the kernel; `HybridGemm` does not.  Training therefore fits the weights on the
dequantized, exactly quantized forward pass (a straight-through estimator), which is what `src/tools/pryon_qat.c` does.

## What the engine does with the head's output

`kw.cfg.json`: classifier `moving-average-max-pool`, window 3, `trim-tail` 8; accept 0.923, notify (near miss) 0.69;
keyword feature window 100 and lock 100 frames.  Measured on the engine's own results (a replay of 14 of its events: 12 matched to six
digits, two within 0.003): the wake probability is the head's softmax for class 1 per lane; the score of a cycle is the best moving
average over three adjacent ages among ages 9 to 23 (the eight youngest dropped); it accepts at the first unlocked cycle at
or over 0.923 (the detection sample is 960·(t+1)), then locks 100 frames; a run over 0.69 that never accepts is the near
miss, reported 100 frames after its peak.  `pryon_train` optimizes exactly this score and checks it on recordings it held back.

## Training is only reproducible with the Echo's own libm

The optimizer (float BFGS, then Adam over the quantized forward pass) amplifies one-ulp differences into different models,
and glibc's `exp`/`log`/`log1p` differ from bionic's in the last bit (7.5 % of `exp` results over 240,006 inputs,
1.5 % of `log`, 0.03 % of `log1p`).  The Echo's functions are FreeBSD msun's `e_exp.c`, `e_log.c`, `s_log1p.c` (the
disassembly of the firmware's `exp` has msun's constants and operation order, plain VFP multiplies, no fused
multiply-add).  `src/tools/pryon_qat.c` carries them, bit-identical on all 240,006 inputs taken from the Echo and from the
firmware's `libm.so` under qemu; `tests/unit/pryon_qat_test.c` pins a hash of them.  The trainer then produces the same
model on a PC, under qemu and on the Echo.  Build it with `-ffp-contract=off`.

## If the audio front end is to be reproduced too (not needed for training here)

The engine's feature extractor is Kaldi's float32 LFBE with four substitutions found by comparing every stage with the
engine's own buffers: Ne10's `ne10_fft_r2c_1d_float32_neon` for the 512-point FFT (level-1 butterfly order, twiddles
identical to the engine's tables), the mel sums through OpenBLAS's ARMv7 `sdot` (two accumulators over blocks of four),
bionic's msun `logf`, and no HTK first-bin quirk.  With them the 64 features per frame matched on every frame of a test
file.

## Not known

The library address of the LSTMs' recurrent kernel; how the engine scores rows of age under 9 (they are not used);
whether other languages' sets share this head's layout (the code refuses a base set that is not Alexa and Stop).


## Enrollment review fixes and validation

The installation API queues work; it does not prove installation succeeded. Enrollment now persists an
`enroll-pending` record before queueing, waits for an exact `OK wake:<id>` result from root, and checks the installed
set's content identifier (`checksum.txt`, generated from SHA-256 over the package files) against the requested
version before writing `wake_word` or the successful `enroll-last` record. Startup reconciles that pending record
before scanning/loading wake words, because root may restart the daemon before its enrollment thread sees the result.
A refusal preserves the previous selection. A timeout reports an unconfirmed outcome and retains the pending record;
restart checks the eventual result. The page offers Clear pending request: this retracts abandoned work under the
root installer lock and permits another training session. It does not claim that nothing was installed.

Recording buffer detachment and capture use the same mutex. A stream is closed exactly once even when its close
fails. Cancellation and the transition from training to packaging share the status mutex: an accepted cancellation
cannot subsequently queue installation, and a delayed cancel during packaging/installation cannot overwrite the
outcome. The root deletion path uses the upstream hardened request/result handling and reads daemon-owned inputs
as the daemon, just as installation does.

Reproduce the software regression checks from the repository root:

```sh
make build/hassmic-host unit-pryon_qat unit-enroll_pkg
python3 tests/fake_web_enroll.py
python3 tests/fake_web_wakewords.py
```

The web tests use synthetic model files and stand-in tools. They exercise API, capture, cancellation, installer
refusal, confirmation, and restart recovery, but do not measure acoustic accuracy. Prior Dot 3 demonstrations remain
experimental observations, not a false-acceptance benchmark. Generated packages contain Amazon-derived assets;
keep them and all personal recordings out of published source. Permission to redistribute those packages has not
been established.

Review validation: sanitizer-backed `unit-enroll_state` and `unit-enroll_pkg`, `unit-pryon_qat`, the complete
`fake_web_enroll.py` flow, `fake_web_wakewords.py`, and upstream `fake_web_artifacts.py` pass. The state regression
covers concurrent capture/buffer teardown, exact ID matching, mismatched installed versions, missing confirmation,
root refusal, restart recovery and non-cancellable installation states. The web flow includes a delayed cancel after
installation begins and a failed install preserving the prior successful result. Both the host daemon and the Dot 3
daemon/extractor/trainer cross-build. JavaScript and shell syntax checks pass. The host build retains upstream's
`setregid` ignored-result warning; the enrollment translation unit compiles with `-Wall -Wextra` without warnings.
No Echo configuration, service, firmware, vendor model, or personal recording was changed during this review.
Hardware validation of the revised installer/selection flow remains a follow-up before an upstream release.


## Second review: unresolved installation recovery

Timeouts, installed-version mismatches, and failed selection/result writes now have an explicit `unresolved` state.
The failure card shows the specific error, without a blanket claim that nothing was installed. Clear pending request
uses the same signed dismiss API; a successful clear returns to idle and permits training again. It never deletes an
installed model or changes the current wake-word selection.

Root holds `/data/local/hassmic/.artifact-lock` from before claiming its request through publication of its result.
This inode lives in the root-owned data directory, is created during the normal startup migration, and stays in place.
The daemon opens it read-only and attempts a nonblocking exclusive `flock` before clearing unresolved work. While
holding that lock and its own artifact mutex, it removes only the target ID from an unclaimed request (preserving
other artifact IDs), removes the target stage/ready marker, then clears `enroll-pending`. If root is still copying,
clearing is refused and the pending operation stays visible. Closing the descriptor, including process exit, releases
the lock; there is no stale PID/marker to clean up. Old root installers without the lock must be updated before using
this recovery action; it deliberately refuses to guess that an uncoordinated installer is idle.

Android mksh closes nonstandard descriptors when executing a child. On the Dot 3, `exec 9>>...; flock -x 9` fails with
Bad file descriptor. The installer therefore uses descriptor 0, inherited by `flock` and copying children. It does not
consume commands from stdin. This difference was found and verified on the physical Echo, not inferred from the host.

Checks: `make unit-enroll_state unit-enroll_ui unit-enroll_pkg unit-pryon_qat`, `tests/fake_web_enroll.py`,
`tests/fake_web_artifacts.py`, `tests/fake_web_wakewords.py`, shell/JavaScript syntax, and host/Dot 3 cross-builds.
The state and signed web API tests cover permanent installer silence, wrong installed checksums, failed selection
writes, clearing refused while locked, clearing after release, preservation of other queued IDs and installed
models, and a new training session after recovery. The renderer test executes the actual failure-card function.

Physical validation used a separate root-owned experiment directory and state/model tree. It preserved private copies
of the active binary, configuration, selection and trained package first; originals were untouched. The revised root
installer installed a copied existing trained package, and the actual compiled startup code confirmed the exact
result/checksum before selecting it in the test state. The stock device Pryon engine then initialized that installed
package and processed an existing test recording. Installer silence and checksum mismatch reached unresolved and
cleared to idle without deleting the installed package. A deliberately delayed real root copy blocked clearing;
confirmation succeeded after it completed. The ARM state regression also ran as the daemon user on the Dot.
Private reports and the engine trace are in ignored `build/review2-echo/`; no proprietary model or recording is added
to Git. The active wake-word selection was compared with its preserved copy, and no production service was restarted.
This validates the installer/state transitions on hardware; it is not a fresh microphone-enrollment or accuracy trial.
