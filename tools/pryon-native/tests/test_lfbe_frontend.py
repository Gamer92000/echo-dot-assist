"""pryon_lfbe.c builds, runs and agrees with an independent float64 LFBE.

Bit parity with the stock runtime needs local captures (tools/pryon-native/LEDGER.md);
this only guards the build and gross regressions on synthetic audio.
"""
import sys
import json
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import numpy as np

from pryon_runtime_analysis import lfbe


def float64_lfbe(pcm):
    x = np.asarray(pcm, np.float64)
    fr = np.lib.stride_tricks.sliding_window_view(x, 400)[::160].copy()
    fr -= fr.mean(1, keepdims=True)
    fr[:, 1:] -= 0.96875*fr[:, :-1].copy()
    fr[:, 0] *= 1-0.96875
    fr *= (0.5-0.5*np.cos(2*np.pi*np.arange(400)/399))**0.85
    power = np.abs(np.fft.rfft(fr, n=512))**2
    mel = lambda f: 1127*np.log1p(f/700)
    grid, edges = mel(np.arange(257)*16000/512), np.linspace(mel(80), mel(7200), 66)
    bank = np.maximum(0, np.minimum((grid[:, None]-edges[:-2])/(edges[1:-1]-edges[:-2]),
                                    (edges[2:]-grid[:, None])/(edges[2:]-edges[1:-1])))
    return np.log(np.maximum(power@bank, 1))


class LfbeFrontendTests(unittest.TestCase):
    def test_public_synthetic_vector(self):
        vector = json.loads(Path(__file__).with_name('golden_lfbe.json').read_text())
        rule = vector['input']
        pcm = ((np.arange(rule['samples'], dtype=np.int64)*rule['multiplier']) % rule['modulus']-rule['subtract']).astype(np.int16)
        np.testing.assert_allclose(lfbe(pcm), np.array(vector['output'], np.float32), rtol=0, atol=2e-6)

    def test_silence_is_floor(self):
        np.testing.assert_array_equal(lfbe(np.zeros(560, np.int16)), np.zeros((2, 64)))

    def test_matches_float64_reference(self):
        r = np.random.default_rng(0)
        t = np.arange(16000)/16000
        pcm = (3000*np.sin(2*np.pi*440*t)+800*r.standard_normal(t.size)).astype(np.int16)
        ours, ref = lfbe(pcm), float64_lfbe(pcm)
        self.assertEqual(ours.shape, ref.shape)
        np.testing.assert_allclose(ours, ref, atol=2e-4)


if __name__ == '__main__':
    unittest.main()
