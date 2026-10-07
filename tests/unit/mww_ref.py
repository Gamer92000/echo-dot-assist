"""microWakeWord's front end and interpreter (src/hassmic/mww_*.c) against the reference libraries.

mww_test (tests/unit/mww_test.c) runs ours on the same audio:
- features: bit for bit what pymicro-features (TFLite Micro's audio front end, which microWakeWord trains with) gives;
- quantized model input: what pymicro-wakeword makes of those features;
- every inference of every model pymicro-wakeword ships (and any .tflite named on the command line): what TFLite gives
  with its reference kernels for the same input, the model's variables carried from one inference to the next as on the
  Echo.  LOGISTIC may differ by one step (ours is TFLite's float table, TFLite's reference kernel is fixed point).
Audio: a synthetic second of tones, chirps and noise at several levels, plus, where they are, spoken phrases from
espeak-ng and the Echo's own captures in device-logs/.

    .venv/bin/python tests/unit/mww_ref.py build/mww_test [more.tflite ...]
"""

import io
import math
import shutil
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np
from ai_edge_litert.interpreter import Interpreter, OpResolverType
from pymicro_features import MicroFrontend
import pymicro_wakeword

ROOT = Path(__file__).resolve().parents[2]
BIN = sys.argv[1]
MODELS = sorted((Path(pymicro_wakeword.__file__).parent / "models").glob("*.tflite")) + [Path(p) for p in sys.argv[2:]]


def synthetic() -> bytes:
    rng = np.random.default_rng(7)
    t = np.arange(16000 * 4) / 16000
    parts = [
        0.3 * np.sin(2 * math.pi * 440 * t[:16000]),
        0.05 * np.sin(2 * math.pi * (200 + 3000 * t[:16000]) * t[:16000]),
        0.001 * rng.standard_normal(16000),
        0.9 * rng.standard_normal(8000).clip(-1, 1),
        np.zeros(8000),
        0.02 * np.sin(2 * math.pi * 1000 * t[:16000]) * (1 + np.sin(2 * math.pi * 3 * t[:16000])),
    ]
    return (np.concatenate(parts) * 32767).astype("<i2").tobytes()


def wav16k(data: bytes) -> bytes:
    w = wave.open(io.BytesIO(data))
    pcm = np.frombuffer(w.readframes(w.getnframes()), "<i2")
    if w.getframerate() != 16000:
        x = np.arange(0, len(pcm), w.getframerate() / 16000)
        pcm = np.interp(x, np.arange(len(pcm)), pcm).astype("<i2")
    return pcm.tobytes()


def audio():
    yield "synthetic", synthetic()
    if shutil.which("espeak-ng"):
        for text in ["okay nabu, turn on the kitchen lights", "hey jarvis what time is it", "alexa", "hey mycroft"]:
            yield f"espeak '{text}'", wav16k(subprocess.run(["espeak-ng", "--stdout", text], capture_output=True, check=True).stdout)
    for p in sorted((ROOT / "device-logs").glob("micAsr-*.wav"))[:3]:
        yield p.name, wav16k(p.read_bytes())


def ref_features(raw: bytes):
    fe, out, i = MicroFrontend(), [], 0
    while i + 320 <= len(raw):
        r = fe.process_samples(raw[i : i + 320])
        i += r.samples_read * 2
        if r.features:
            out.append(r.features)
    return np.array(out)


fails = 0
for name, raw in audio():
    raw = raw[: len(raw) // 320 * 320]
    ref = ref_features(raw)
    mine = np.loadtxt(io.StringIO(subprocess.run([BIN, "features"], input=raw, capture_output=True, check=True).stdout.decode()), ndmin=2)
    same = ref.shape == mine.shape and np.array_equal(ref, mine * 0.0390625)
    print(f"features {name}: {len(mine)} windows {'identical' if same else 'DIFFER'}")
    fails += not same
    for model in MODELS:
        it = Interpreter(str(model), experimental_op_resolver_type=OpResolverType.BUILTIN_REF)
        it.allocate_tensors()
        inp, out = it.get_input_details()[0], it.get_output_details()[0]
        scale, zp = inp["quantization"]
        lines = subprocess.run([BIN, "model", str(model)], input=raw, capture_output=True, check=True).stdout.decode().splitlines()
        stride = inp["shape"][1]
        qbad = pbad = pmax = 0
        for k, line in enumerate(lines):
            q, p = line.split("=")
            q = np.array(q.split(), dtype=np.int8).reshape(inp["shape"])
            want = np.round(ref[k * stride : (k + 1) * stride] / scale + zp).clip(-128, 127).astype(np.int8).reshape(inp["shape"])
            qbad += not np.array_equal(q, want)
            it.set_tensor(inp["index"], q)
            it.invoke()
            r = int(it.get_tensor(out["index"]).reshape(-1)[0])
            d = abs(r - int(p))
            pbad += d > 1
            pmax = max(pmax, r)
        ok = not qbad and not pbad and len(lines) == len(ref) // stride
        print(f"  {model.stem}: {len(lines)} inferences, peak {pmax}/255, input {'identical' if not qbad else f'{qbad} differ'}, "
              f"output {'within 1/256' if not pbad else f'{pbad} off by more than 1/256'}")
        fails += not ok
sys.exit(1 if fails else 0)
