# Pryon / NemoRT reverse engineering — live investigation

Updated 2026-11-11. **In progress.** This report distinguishes measured runtime
behavior from hypotheses. No original library, firmware or model has been edited.
No device, microphone, playback, flashing or remote push is involved.

## Confirmed results

### Actual classifier activations captured and compared

The stock runtime reaches the wake classifier with **25 × 100 float32 values**,
then produces **25 × 3 values** before the graph's separate bias addition and
softmax. The input is:

`StatefulPartitionedCall/functional_5/fc_output_ww/Tensordot/Reshape:0`

This is a post-ReLU dense representation, not simply the LSTM hidden state.
Captured weight scales, zero-points and sums match the original ONNX exactly.
The captured signed-int8 weights match after accounting for runtime packing:
interleave the first two output rows element-by-element, then the remaining row.

The stock Alexa sample still produces both its Accept and NearMiss detections in
the **classifier-only** capture, with no audio backlog reset. Against the **same
captured finite input rows**, the actual translated ONNX Gemm and a reconstruction
of the runtime's quantized math give:

| Calculation | Compared output values | Maximum absolute error | RMSE |
|---|---:|---:|---:|
| Translated ONNX classifier Gemm | 9,225 | 1.20172119 | 0.08869606 |
| Reconstructed quantized classifier | 9,225 | 0.0000240641 | 0.00000148294 |

Evidence: `local-data/rea-pryon/alexa-classifier/comparison.json`,
`classifier-activations.npz`, per-call `*-x.bin` / `*-y.bin`, `hits.json`,
and `runtime.txt`. These are local, Git-ignored artifacts. The comparison executes
the classifier node and its translated weight initializer in ONNX Runtime; it
does **not** establish whole-encoder parity.

### HybridGemm quantization

Live execution uses the CPUNK signed-char HybridGemm implementation at library
offset **`0x0123897c`** (Thumb function pointer `0x0123897d`). The alternative
signed-short implementation at `0x012394b8` did not fire in the bounded test.
The filename's `int16` suffix does not establish the executed kernel's datatype.

For measured finite, nonconstant activation rows:

```
sx = float32((max(x) - min(x)) / 127)
zx = round_to_nearest(-min(x) / sx)
qx = round_to_nearest(x / sx) + zx
y = sx * sw * sum((qx - zx) * (qw - zw))
```

The observed rounding agrees with NumPy `rint` on captured values; tie behavior
has not yet been separately forced. The subtraction need not be materialized:
weight sums and activation sums support the equivalent integer correction
`dot(qx,qw) - zx*sum(qw) - zw*sum(qx) + K*zx*zw`.
Constant rows, saturation edge cases and accumulator ceilings remain unverified.
For one captured first-convolution block, this rule matched all **12,600**
quantized activation bytes, and its scales and zero-points, exactly. Quantization
does not force the real interval to include zero: a wholly negative row had a
large positive integer zero-point (e.g. 8254).

Quantization intermediates can be read immediately after the call at
`0x01238a9a`, by breaking at **`0x01238a9e`**. At that point the quantized
activation pointer is `r9`, scale pointer is `[sp+0x88]`, zero-point pointer is
`[sp+0x84]`, and activation-sum pointer is `[sp+0xb4]`.

These are instruction-level and controlled-runtime findings, not yet REA
decompiler conclusions. Early scratch `alexa-quantized` files had the `qx` and
`xz` pointers mislabeled; **do not use them**. The corrected capture implementation
and later `alexa-full` / `alexa-aligned` runs use the offsets above.

### HybridConv executes through the same quantized matrix kernel

One inference cycle contains these seven matrix dimensions `(M,K,N)`:

```
(360,35,96), (18,1440,128), (6,1024,128), (4,768,160),
(1,1280,160), (1,160,500), (1,500,100)
```

The first matrix reshapes to `[6,60,7,5]`: six output times, sixty output
frequencies, seven time taps, five frequency taps. Adjacent time and frequency
windows overlap exactly in measured buffers. Thus the normalized acoustic input
can be recovered from these windows without modifying the model. The last time
tap reconstructs six new 64-band feature frames per inference cycle.

The live Conv-to-Gemm wrapper caller is `0x00c509cc`; the initially investigated
KaldiV2 Conv implementation at `0x00b5e758` did not execute in the bounded probe.
Do not confuse available implementations with the active path.

### HybridLSTM: full contract measured (2026-11-11, `lstm-values`)

The active inner LSTM routine is **`0x00c72344`**, reached through return address
`0x00c72235`. Its input projection reaches `0x00c657dc`, then the matrix wrapper
`0x00c4d71c`, then CPUNK HybridGemm.

| Operand | Actual shape |
|---|---|
| X (`r2` descriptor `[1,1,100,ptr]`) | `[1,1,100]` |
| Input W / recurrent R | `[400,100]` each |
| Initial hidden / cell (stack args 9 / 10, `[ptr,rows,cols,stride]`) | `[25,100]` |
| Y (stack arg 12, `[1,1,25,100,ptr]`) | `[1,1,25,100]` |
| Returned hidden / cell (stack args 13 / 14) | `[25,100]` |

`pryon_debug_capture.py --hook 0xc72344 --lstm --lstm-values` saved X, both states
and Y for all **276** calls (138 cycles × 2 branches) of a full slow run (both stock
detections, no backlog reset, zero skipped backlog). `pryon_lstm_analysis.py` shows:

- **Branches:** even calls are `LSTM__67` (ww), odd calls `LSTM__53` (se). Both
  read the same X.
- **Broadcast:** the single X row drives all 25 state rows (one `[1,100]x[100,400]`
  projection per branch).
- **Gate order:** ONNX `iofc`, `B = [Wb, Rb]`. Standard sigmoid/tanh/tanh.
- **Ring reset (host-side, not in the graph):** before cycle 0 all 25 rows are zero.
  Before cycle *t*, row `t mod 25` of h and c is zeroed and the other 24 carry over
  bit-exactly from the previous outputs. Both branches use the same slot. It wraps
  24 → 0 and holds for all 138 cycles, across both detections (no extra reset).
  Row `j` is thus an LSTM restarted every 25 cycles: a sliding window of ≤ 25 steps
  (≤ 1.5 s at 60 ms per cycle).
- **Y == returned hidden** in every call.
- **Quantization:** x and every h row use the same per-row asymmetric 127-step
  rule as CPUNK HybridGemm. With that, h max error is **6.2e-7** and c max error is
  **2.1e-6** over all finite rows (float32-exact). Float math instead gives an h max
  error of 0.76. Two rows (calls 10/11, row 3) hit a zero-point tie where `qx` reached
  **128**. The recurrent kernel **saturates to 127** (error 1e-7 versus 0.9 with wrap).
- **Recurrent kernel unidentified:** no `(25,100,400)` call reaches the hooked CPUNK
  Gemm, so `h·R` runs elsewhere. The saturation finding belongs to that kernel.
  `quantize_rows` (CPUNK path) still rejects out-of-range values as unmeasured.
- X is non-finite for cycles 0–2 and finite garbage (up to 4.9e31) at cycles 3–4,
  from uninitialized conv history. Affected rows flush out slot by slot through the ring.

### Audio frontend configuration and path

The active model configuration specifies:

- 16 kHz PCM; 400-sample (25 ms) frames, 160-sample (10 ms) hop.
- Dither 0, DC removal, pre-emphasis 0.96875, Povey window, FFT rounded to 512.
- LFBE, 64 mel bands, lower edge 80 Hz, upper edge **7200 Hz**.
- No frontend mean/variance normalization, stacking, or affine transform.
- The ONNX graph itself subtracts a learned 64-value mean and multiplies by a
  learned reciprocal standard deviation before convolution.

**Frontend parity confirmed (2026-11-11, `alexa-aligned`).** `pryon_runtime_analysis.lfbe`
implements the configuration above: int16 amplitude units, frame 0 at sample 0 with
no padding, per-frame DC removal, pre-emphasis with first sample scaled by
`1-0.96875`, symmetric Povey window, 512-point power spectrum, 64 HTK-mel triangles
80–7200 Hz, and `log(max(power, 1))`. Against all **828** LFBE frames recovered from
the stock runtime's first convolution windows (52,992 values), it gives max absolute
error **1.45e-4** and RMSE **3.6e-6** at frame offset 0. All 33,572 floor positions
match. The residual (max relative 2.6e-5) is mostly the FFT algorithm (stock uses Ne10
float32), not plain float32-vs-float64 rounding; see "Frontend: stage-by-stage bit parity".
Reproduce with `pryon_runtime_analysis.py CAPTURE --audio RAW`.

The older `probe_pryon_inference.wav_logmel` is **not** this frontend (different
window/STFT, normalized PCM, no DC removal or pre-emphasis, 7600 Hz upper edge,
different floor) and must not be used for parity work.

Observed current path:
`PryonDecoder_PushAudioEventSamples -> configured LFBE frontend -> input_feats ->
graph global normalization -> streaming Conv windows -> CPUNK HybridGemm ->
LSTM branches -> dense/ReLU -> 100-D classifier input -> Gemm -> bias -> softmax`.

## Artifact identities

| Artifact | SHA-256 |
|---|---|
| libpryon.so | `65df801e37ad0bbdfaf8922174bceb084fae6ba71c5076b1e23a000523f97027` |
| Original ONNX | `572ca159779c403a02298a016d0548ef618d68732f41709ba80b2235269868a9` |
| Upstream pryon_test | `107e8079ca73ac0c2d30d4cc1bb2cd62d78ffe8202bf64fa97cc643cdea23f84` |
| Alexa raw sample | `8df28d48563e2794c1ec913784b54849e7cd5d485fd8243a0ed224b0e251fefc` |

Original paths are under `firmware/donut/rootfs/`, `build/donut/` and `testdata/` of this repository. All new work is in this repo. Do not commit
firmware, models, weights, recordings or raw tensor data.

## Reproduction and capture quality

`pryon_debug_capture.py` speaks the QEMU GDB remote protocol over a local Unix
socket. It reads registers/memory and sets emulator breakpoints; it never writes
guest memory or source files. The library and harness hashes gate hardcoded
addresses. It terminates only its owned QEMU child.

Run with the installed bubblewrap to supply low PIDs (required by this Android
bionic build), a read-only filesystem, and one writable evidence directory:

```sh
bwrap --ro-bind / / \
  --bind "$PWD/local-data" "$PWD/local-data" \
  --dev /dev --proc /proc --unshare-pid --die-with-parent \
  python3 pryon_debug_capture.py --hook 0x123897c --gemm \
  --classifier-only --quantized --frontend --hits 10000 \
  --out local-data/rea-pryon/NEW-CAPTURE

.venv/bin/python pryon_runtime_analysis.py local-data/rea-pryon/NEW-CAPTURE
```

The debugger socket needs an environment that allows a local Unix socket.
No sysctl or persistent host setting is changed. Default input pacing is 0.025×;
debugging runs slowly to prevent Pryon's 7-second audio-backlog reset. Check the
capture's runtime output and comparison's `backlog_reset` flag before claiming
alignment. Reaching `--hits` ends the capture early; a full run ends with the
guest exit packet and `pushed ... samples` output.

**Excluded capture:** `alexa-full` overflowed the queue, reset the backlog and
lost speech. It is not frontend/end-to-end parity evidence. `alexa-aligned` is its
slower replacement. It completed with the full 134,407 samples pushed, both stock
detections (NearMiss 1.39–2.04 s, Accept 3.86–5.62 s), no backlog reset and
`skipped_audio_backlog_since_last_ww_msec: 0`. It reproduces the classifier result
above exactly. `alexa-classifier` is the
completed detection-preserving classifier comparison above.

REA 6.4.0 opened and hashed the binary with Ghidra 12.1.4 selected. MCP searches
and decompilation timed out at 300 seconds; the CLI then hit its 330-second
startup limit. The documented per-process `REA_GHIDRA_STARTUP_TIMEOUT_MS=1200000`
is now being used for a CLI decompilation and saved analysis snapshot under
`local-data/rea-pryon/`. No REA pseudocode has returned at this checkpoint.
Do not label ordinary disassembly/runtime observations as REA Evidence IDs.

### Native float encoder versus stock (2026-11-11)

`convert_pryon_native.py` now inserts `Expand(X, Shape(initial_h))` in front of each
native LSTM. `pryon_native_stream.py` streams LFBE through the native copy with
the measured contract: 6 frames per cycle, conv buffers fed back, LSTM ring reset.
`pryon_native_stream.py CAPTURE RAW` compares the classifier input over 108
post-warmup cycles of `alexa-aligned`:

| Comparison (cycles ≥ 30) | Max abs | RMSE |
|---|---:|---:|
| Native float encoder, classifier input (stock RMS 1.15) | 1.19 | 0.039 |
| Wake softmax: native encoder + exact quantized head | 0.218 | 0.0147 |
| Wake softmax: stock encoder + float head | 0.0125 | 0.00068 |

The gap is in the **encoder**, not the head. Splitting it:

- **Conv stack:** native LSTM input X versus stock `lx` (cycles ≥ 30) has RMSE
  0.018, 1.7% of the stock RMS 1.07, and max 0.106. The error is smooth with no
  structural offset.
- **Row age:** classifier-input error grows smoothly with the LSTM window age
  `(t-row) mod 25 + 1`, from RMSE 0.006 at age 1 to 0.044 at age 25. That is
  recurrent drift, not a step-like wiring fault.
- **LSTM alone:** driving a self-run 25-row ring from the stock X sequence, the
  quantized emulation reproduces stock ww hidden states to **6.5e-7** over 108
  cycles (se: 2.5e-3, warmup handling crude). Float math drifts to 0.126 / 0.501.
  The LSTM share of the gap is therefore quantization. The conv share is consistent
  with it, given the first-conv byte match above, but is not yet layer-verified.
- **Stream wiring checked:** history buffers are used (changing them changes
  every output), and `buffer_output_7` equals the normalized LFBE frames. Native
  NaN history does not reach the outputs only because ORT `MaxPool` drops NaN
  against a finite neighbour. Stock conv history starts as **uninitialized memory**:
  X at cycles 3–4 holds finite garbage up to 4.9e31. Exclude those warmup cycles.

Bit-parity needs per-row 127-step emulation before every matrix product. For convs
the rows are im2col patches, so it cannot be one tensor-wide fake-quant. Note that
`DynamicQuantizeLinear` is the wrong rule: it uses 255 steps and forces zero into
the range.

**Training caveat:** `train_pryon_head.py` and `probe_pryon_inference.py` still run
one-shot with zero states, which makes every row age 1. The stock runtime scores
rows of age 1–25. Training must go through `pryon_native_stream`, so don't rerun
training until it does.

The decoder's reported `classifier_score` (e.g. NearMiss 0.913608) matches **no**
single per-row softmax value. It is a post-graph aggregate, still open.

### Quantizer refinement and quantized whole-model emulation (2026-11-11)

**Reciprocal multiply.** Re-checking conv1 against every saved `alexa-aligned`
(360,35,96) call showed 5 of 49,612 rows off, all containing exact .5 ties. The
kernel quantizes with `inv = float32(1/s)`, `q = rint(x*inv) + rint(-min*inv)`
(round-half-even), not `x/s`. With that, **all 49,612 rows** match (max 4e-5,
float accumulation). Round-half-away variants fail. The head and LSTM fits are
unchanged under the new rule. Both shared quantizers now use it.

**Conv Gemm includes the bias.** The stock (360,35,96) output equals the quantized
product plus the HybridConv bias. HybridGemm (dense/heads) outputs exclude the bias,
which the graph adds separately.

**`pryon_quant_model.py`** emulates the whole graph in NumPy:
- im2col convs through the measured quantized product, quantized per patch;
- float ReLU, MaxPool, BatchNorm;
- conv history of the trailing `kernel_h - stride_h` rows;
- the quantized LSTM with the ring;
- quantized dense layers and heads.

`pryon_quant_compare.py` compares it with the stock captures (cycles ≥ 30):

| Tensor | Float native RMSE | Quant emulation RMSE (max) |
|---|---:|---:|
| LSTM input X | 0.018 | 0.00055 (0.008) |
| Classifier input | 0.039 | 0.014 (0.33) |
| Wake softmax | 0.0147 | 0.0043 (0.097) |

X is exact (≤ 1e-6) on 91 of 133 cycles, including all silence. It deviates by
about 2e-3 in bursts during speech, so one conv-stack detail is still unresolved.
A per-layer capture (`layers`, every Gemm x/y for the first ~36 cycles) is in
progress to locate it.

## Quantized emulation: exact given stock LFBE (2026-11-11)

`pryon_quant_model.py` fed the stock-recovered LFBE frames
(`local-data/rea-pryon/alexa-aligned/lfbe-recovered.npy`) reproduces the stock
LSTM input X on **all 133 post-warmup cycles**, max error 1.19e-6.

With my own `lfbe()` output, only 91/133 cycles are exact. The bursts of
~2e-3 during speech are caused by `lfbe()`'s up-to-1.45e-4 float deviation
flipping quantization rounding in conv1. So the conv stack, BN, pooling and
streaming emulation are correct. The remaining parity item is the **frontend's
float32 arithmetic** (the order of FFT/power/mel/log ops in float32), not the
model.

Fused-BN variants (`a*x+b`, `(x-mu)*a+b`) make no difference.

### Tooling changes

`pryon_debug_capture.py` socket timeout 30 s -> 600 s (slow `--speed` runs);
`pryon_quant_compare.py` takes an optional LFBE .npy; `pryon_quant_model.py`
exposes per-layer im2col rows in `stats['conv_rows']`.

### Per-layer capture `layers` (every Gemm x/y, first ~36 cycles, speed 0.01, no backlog reset)

The measured quantized product applied to the **stock** im2col inputs reproduces
the **stock** outputs for all 7 HybridConv layers over 32 cycles. Max abs errors:
conv1 1.8e-5, conv2 1.1e-3 (one float32 ulp at |y|~9300, see "Conv epilogue"), conv3 8.4e-5,
conv4 2.8e-5, conv5 6.2e-5, conv6 4.9e-5, conv7 6.5e-5.

My own emulator inputs first differ from stock at cycles 24–29, the onset of
speech. Together with the stock-LFBE result above, that pins the remaining
mismatch on the frontend's float arithmetic. (The conv7 "first diverging cycle 5"
is the 1e-5 threshold against float BN noise, not a real divergence.)
Analysis script: `pryon_layer_compare.py CAPTURE AUDIO.raw`.

### Conv epilogue: float32, bit-exact on conv2..conv7 (2026-11-11)

The "conv2 1.1e-3 outlier" above was one float32 ulp on an output of -9312.98,
not a kernel difference. The measured epilogue is

    y = float32(acc) * float32(s * sw) + bias     (acc = exact integer sum)

With it, the quantized product on stock im2col inputs reproduces every stock
output value of conv2..conv7 (0 ulps, 32 cycles, `layers` capture). Other
orders score lower: `(acc*s)*sw` 80%, `(acc*sw)*s` 81%, float64 then round
84%. `pryon_quant_model.quant_matmul` now uses this epilogue.

Conv1 residue (3% of values): rows lying wholly below zero (silence frames)
get zero points around 8000 and differ by 1-3 accumulator units in scattered
columns. The quantized values are the same; integer-exact expansions
(`sum q*w - zw*sum q - z*sum w + K*z*zw`, with the model's `_sums`) and
float accumulation orders (sequential, 2/4/8 lanes) do not reproduce it.
Still open. The effect is about 3e-5 on an activation.

## Frontend: stage-by-stage bit parity (2026-11-11)

The LFBE path runs in the feature extractor at +0x105f7c0. Measured stages:

| Stage    | Stock implementation                                            | Status      |
|----------|-----------------------------------------------------------------|-------------|
| pre-FFT  | Kaldi ProcessWindow in float (DC sum/400, preemph, povey)       | bit-exact   |
| FFT      | Ne10 `ne10_fft_r2c_1d_float32_neon`, NE10_UNROLL_LEVEL 1        | bit-exact   |
| power    | Kaldi ComputePowerSpectrum, float                               | bit-exact   |
| mel      | Kaldi MelBanks, float weights, OpenBLAS sdot_vfp order, floor 1 | bit-exact   |
| log      | VectorBase::ApplyLog (+0xa33fc4) -> bionic msun `logf`          | bit-exact   |

Evidence:

- **FFT dispatch.** The extractor calls the srfft slot +0xa361ba when its
  srfft_ member is set, which is always the case at 512. That wrapper runs
  +0x126b5f8 and repacks to Kaldi layout (`[re0, reN/2, re1, im1, ...]`).
  +0x126b5f8 switches on nfft 2/4/8, then runs radix-8/radix-4x4 NEON
  butterflies. Its state layout (nfft at +4, `r_twiddles_neon` at +0x14,
  `r_factors_neon` at +0x1c) is Ne10's level-1 `ne10_fft_r2c_state_float32_t`.
  The 256-point "SplitRadixComplexFft" ctor objects are built but never
  computed. Kaldi `RealFft` (+0xa35020) only serves an idle 320-point pipeline
  whose input is all zeros. Kaldi's own srfft is not used.
- **Captures.** `pryon_fft_capture.py` saved FFT in/out (`fft-io`, 840 calls)
  and ApplyLog in/out (`log-io`). Each frame is FFT'd twice, by two identical
  consumers.
  - My pre-FFT frames match the stock FFT inputs bit for bit (358/358).
  - Replaying the stock FFT outputs through `pryon_lfbe.c` (power, mel, log,
    then graph normalization) matches all 183/183 frames bit-exactly at the
    conv1 input.
  - msun `logf` matches 26,560/26,560 log values. glibc `logf` gets 99.7%.
  - The mel dot needs OpenBLAS's ARMv7 `sdot_vfp` (+0x51d9e0) order: two
    accumulators over blocks of 4, remainder into the first, then `s0+s1`.
    That gives 100%. Sequential order gets 65%, float64 67%, pairwise 85%.
  - Mel weights must be float32 with MelScale using the same msun `logf`.
    Pryon does not apply Kaldi's HTK bin-0 quirk (bin 0 is off by up to 2.2 if
    applied).
- **Why the FFT dominated.** A float32 FFT's error in a quiet bin scales with
  the frame's strongest bin, which caused the valley-bin outliers. Kaldi's
  float split-radix with its float twiddle recurrence is 10x worse than stock.
  kissfft `kiss_fftr` (double twiddles) gets within 2.3e-7 relative but only
  17% bit-exact. Ne10's butterfly order is required.
- **End to end.** With the kissfft stand-in, LFBE is 88% bit-exact (max 2.9e-5
  normalized), and LSTM X is exact within 1e-5 on 96/133 cycles (was 91 with
  float64 numpy).

`pryon_lfbe.c` is the frontend (`cc -O2 -ffp-contract=off`). Its
`PRYON_FFT_FROM` env replays captured FFT outputs.

**Ne10 FFT reimplemented; frontend bit-exact end to end.**

- `pryon_lfbe.c` now carries its own implementation of the Ne10 level-1 r2c
  path for nfft 512, in Ne10's operation order. The radix-8 first stage
  handles 4 columns per float32x4. Then come two radix-4x4 twiddled stages
  (fstride 4/mstride 8, then 1/32), and the radix-4 last stage with super
  twiddles, followed by the wrapper's Kaldi repack.
- Twiddle tables are checked first against the stock cfg tables, dumped
  read-only with `pryon_fft_capture.py --stage ne10cfg`:
  - stock `r_factors_neon` = 3,16 | 4,32 | 4,8 | 8,1
  - 120 `r_twiddles_neon` and 408 `r_super_twiddles_neon` values are
    identical, bit for bit
  - `NE10_PI` is `(float)pi`, even where it is assigned to a double
  - `phase` is a float, then double `cos`/`sin`
- Results:
  - FFT: 358/358 captured frames bit-exact.
  - LFBE from raw PCM: **828/828 frames bit-exact** at the conv1 input.
  - Quantized graph fed with this frontend: 133/133 cycles with LSTM X within
    1e-5 (max 1.07e-6), identical to the run fed stock-recovered LFBE. From
    cycle 30 the classifier input is within 2.4e-6 and the wake softmax within
    1.8e-7.
- `pryon_runtime_analysis.lfbe()` now calls `pryon_lfbe.c`, building it on
  demand, so every parity check and future training run uses the exact
  frontend. `tests/test_lfbe_frontend.py` guards the build against an
  independent float64 reference.
- Remaining sub-1e-5 graph residue: the conv1 negative-row zero-point
  difference (above) plus my float order for BatchNorm/pool. These give conv2,
  conv5 and conv7 input differences of at most 1.5e-5.

## Decoder: score and accept/near-miss rule (2026-11-11)

Source of the rule:
- The model package's `kw.cfg.json` (`firmware/.../models/keyword/en-US/ALEXA/`)
  configures a `moving-average-max-pool` classifier with window-size 3 and
  trim-tail 8, accept 0.923, notify 0.69, keyword-feature-window-length 100
  and lock-period 100.
- `softmax_output_ww` classes are [background, ALEXA, STOP]. STOP accepts at
  0.55 from the awake state (`op.cfg.json`).
- `fc_output_se` gives keyword start/end "time-alignment-scores".
- `search.decoder_type = kaldi-key-phrase`. The HMM score is unused
  (`kwHmmScore` = -FLT_MAX).

Measured rule, implemented in `pryon_decoder.py`:
- **Row age.** Row r at cycle t has age `(t - r) % 25 + 1`, i.e. cycles since
  its ring reset.
- **Score(t).** Drop the 8 youngest ages and keep ages 9..25. Take the
  width-3 moving average across adjacent ages and use its max.
- **Accept (RESULT type=2).** The first cycle with score >= 0.923 that is
  outside the lock. The detection sample is `960*(t+1)`, the end of that
  cycle's frames. The lock then lasts 100 frames, after which the same
  utterance can re-accept at the first cycle boundary past it (seen as
  pairs, e.g. 864 -> 966).
- **Near-miss (type=0).** The peak score of a run of cycles >= 0.69 that
  never accepts. It is emitted 100 frames after the peak cycle's end frame.

Evidence:
- alexa-aligned: accept 0.936928 = cycle 33, ages 23..25, sample 32640.
  Near-miss 0.913608 = cycle 76, ages 14..16, detection 462+100 frames =
  sample 89920. Both exact.
- `local-data/rea-pryon/decoder/stream1.raw` (14 TTS corpus "alexa" clips
  with 1.5 s gaps, stock run at -x 1, no backlog skips): 10 of 12 stock accepts
  match the emulator's score to 6 printed digits at the same detection
  sample. The isolated-clip and two-clip runs match exactly.

Open:
- **Event 3/4 of stream1.** Stock 0.942877/0.999121 vs mine
  0.945528/0.999103. It reproduces in a 4 s cut from sample 96000
  (`cut100.raw`) but not with other cycle phases, so it is an emulator
  divergence on that audio, not a decoder rule. Stock classifier activations
  for the cut were captured in `cut100-cls` and still need comparing.
- ~~Clips after sample 483840~~ Explained. After 12 accepts in a short
  period, stock logs "Defensively restarting Pryon decoder due to too many
  detection events" and the harness stops at 486400 samples (30.4 s). Test
  streams must stay below 12 accepts. The configured limit is not printed.
- **cut100 divergence localized.** Emulator classifier input matches stock to
  1e-6 for cycles 29..41, then jumps to 6.7e-2 at cycle 42 and grows: a single
  quantization flip at the clip onset. The per-layer capture
  (`cut100-layers`) is running to locate the layer.
- **kwSampleStartIndex / kwSampleEndIndex.**
  - First accepts always report start = E - 49 frames (E = detection frame).
  - Re-accepts report E - 119..124. alexa-aligned reports E - 65.
  - The end fits roughly E - 100*mean(se1 of winning rows) + 101 (1.7-frame
    residual).
  - Not derived. It is metadata only, so the decision does not depend on it.
    The config-string offsets for the code search are noted in the session log.

Training target: the per-cycle `cycle_score` above, with its age trimming, is
what an arbitrary-utterance head must drive past 0.923 for the positive and
keep below 0.69 for negatives.

## Training reproducibility: the Echo's libm (2026-11-12)

The native trainer (`native/pryon_qat.c`) uses `exp`, `log` and `log1p` inside
a double-precision BFGS plus Adam optimizer and an int8 weight quantizer. The
host build used glibc and the Echo build used the device's bionic libm; they
produced different models from identical inputs.

Measured with `local-data/libm-probe/probe.c` on 240,006 fixed inputs:
- The real Echo (Android 7.1.2, API 25, ARMv7) and the firmware's
  `/system/lib/libm.so` under QEMU agree on every bit.
- glibc differs from the Echo on 7.5% of `exp`, 1.5% of `log` and 0.03% of
  `log1p` results, always by 1 ulp. glibc 2.4x uses newer table-driven code.
- The Echo's functions are the classic Sun/FreeBSD msun `e_exp.c`, `e_log.c`
  and `s_log1p.c` (same constants, same order, plain VFP `vmul`/`vadd`, no
  fused multiply-add). The disassembly of `exp` at +0x9208 matches msun.

The trainer now carries a faithful port of those three functions
(`pq_exp`, `pq_log`, `pq_log1p`). It is bit-identical to the Echo on all
240,006 inputs. A unit test pins the 64-bit FNV-1a hash `96594667583f1fd3` of those outputs (not SHA-256). The decisive end
to end check: the host trainer now reproduces, byte for byte (sha256
`0dc0df2315fed134...`), the model the Echo itself trained earlier with its
real libm. Principle kept: reuse the vendor's exact machinery rather than
substituting a different implementation. The same source compiles to
WebAssembly, so a browser build can reproduce the Echo's results too.

An intermediate attempt replaced these with libm-free approximations. That made
host and Echo agree with each other but not with the Echo's own libm, and it
was reverted. Its apparent "held-out false accept" result is not meaningful.

## Contribution review follow-up

The mathematical-output hash above is 64-bit FNV-1a `96594667583f1fd3`, not SHA-256. SHA-256 remains the identity
check for binaries and model artifacts. References to the private research document in shipped source comments
now point to this committed ledger.

The pinned NDK r21e / API 24 harness build was repeated during review into `build/review-harness`: it reproduces
`107e8079ca73ac0c2d30d4cc1bb2cd62d78ffe8202bf64fa97cc643cdea23f84` and `main = 0x1098`. Both debugger scripts
now enforce that harness identity in addition to the unchanged library guard. README documents the compiler,
archive digest, flags and commands. No identity bypass was added.

Public synthetic frontend vectors are committed in `tests/golden_lfbe.json`; their source is the reconstructed
implementation, not the private stock captures. Five unittest checks (including the vector and synthetic LSTM
contracts) and four standalone capture/analysis checks pass without firmware. These checks do not replace the
historical stock-library measurements or establish acoustic reliability. Complete Ne10 and Kaldi legal texts and
the Sun notice are retained through `THIRD_PARTY_NOTICES.md` and `licenses/`. Amazon asset redistribution permission
is not established; generated model sets remain outside published source.
