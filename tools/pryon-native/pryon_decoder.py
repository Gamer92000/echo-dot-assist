#!/usr/bin/env python3
"""Stock Pryon keyword decision rule, measured against stock RESULT metadata.

Config (ALEXA model kw.cfg.json): classifier "moving-average-max-pool",
window-size 3, trim-tail 8; accept 0.923, notify 0.69; keyword-feature-window
100 frames; lock-period 100 frames. softmax_output_ww classes are
[background, ALEXA, STOP].

Per network cycle t (6 frames), row r of the 25-row LSTM ring has age
(t - r) % 25 + 1 cycles. Score(t) = max over ages 9..23 of the mean of the
posteriors at ages a, a+1, a+2 (the 8 youngest ages are trimmed). Accept on
the first unlocked cycle with score >= accept; the detection sample is
960 * (t + 1); lock for 100 frames, then re-accept is possible. Near-miss:
the peak score of a >= notify run that never accepts, emitted 100 frames after
the peak cycle's end frame.

Not yet derived: kwSampleStartIndex / kwSampleEndIndex (from fc_output_se).
usage: pryon_decoder.py RAW_S16LE    (prints events from the quantized emulator)
"""
import sys
import numpy as np

ACCEPT, NOTIFY, TRIM, WIN, LOCK_FRAMES, FEATURE_WINDOW = 0.923, 0.69, 8, 3, 100, 100


def cycle_score(p, t, k=1):
    """p: [T, 25, 3] softmax; returns (score, window start age)."""
    a = np.array([p[t, (t-age+1) % 25, k] for age in range(TRIM+1, 26)], np.float64)
    ma = sum(a[i:len(a)-WIN+1+i] for i in range(WIN))/WIN
    i = int(np.argmax(ma))
    return float(ma[i]), i+TRIM+1


def decode(p, k=1, start=5):
    """Yield events (type, score, detection_sample, cycle). type 2 accept, 0 near-miss."""
    lock_until, run = -1, None  # run: (peak score, peak cycle)
    for t in range(start, len(p)):
        s, _ = cycle_score(p, t, k)
        end_frame = 6*(t+1)
        if end_frame > lock_until and s >= ACCEPT:
            yield 2, s, 960*(t+1), t
            lock_until, run = end_frame+LOCK_FRAMES, None
            continue
        if s >= NOTIFY and end_frame > lock_until:
            run = (s, t) if run is None or s > run[0] else run
        elif run is not None and 6*(run[1]+1)+FEATURE_WINDOW <= end_frame:
            yield 0, run[0], 160*(6*(run[1]+1)+FEATURE_WINDOW), run[1]
            run = None


if __name__ == '__main__':
    from pryon_runtime_analysis import lfbe
    from pryon_quant_model import Model
    p = np.array([o['softmax_ww'] for o in Model().stream(lfbe(np.fromfile(sys.argv[1], '<i2')))])
    for e in decode(p):
        print('type %d score %.6f detection %d (cycle %d)' % e)
