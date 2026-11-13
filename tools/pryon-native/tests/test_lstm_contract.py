"""Measured HybridLSTM contract vs ONNX Runtime's LSTM; synthetic weights only.

Checks the converter's assumptions: ONNX LSTM gate order (iofc) and B=[Wb,Rb]
pass through unchanged, and Shape+Expand broadcasts one X row to 25 states.
"""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import numpy as np
import onnx
import onnxruntime as ort
from onnx import helper as h, TensorProto as T, numpy_helper as nh

from pryon_lstm_analysis import cell, qrows


class LSTMContractTests(unittest.TestCase):
    def test_expand_lstm_matches_measured_cell(self):
        r = np.random.default_rng(0)
        W, R = r.normal(0, .3, (400, 100)), r.normal(0, .3, (400, 100))
        B = r.normal(0, .3, 800)
        x = r.normal(0, 1, (1, 1, 100)).astype(np.float32)
        h0 = r.normal(0, .5, (1, 25, 100)).astype(np.float32)
        c0 = r.normal(0, .5, (1, 25, 100)).astype(np.float32)
        inits = [nh.from_array(a[None].astype(np.float32), n) for a, n in ((W, 'W'), (R, 'R'), (B, 'B'))]
        g = h.make_graph([
            h.make_node('Shape', ['h0'], ['shape']),
            h.make_node('Expand', ['x', 'shape'], ['xb']),
            h.make_node('LSTM', ['xb', 'W', 'R', 'B', '', 'h0', 'c0'], ['y', 'hn', 'cn'], hidden_size=100)],
            'lstm', [h.make_tensor_value_info(n, T.FLOAT, s) for n, s in
                     (('x', [1, 1, 100]), ('h0', [1, 25, 100]), ('c0', [1, 25, 100]))],
            [h.make_tensor_value_info(n, T.FLOAT, None) for n in ('y', 'hn', 'cn')], inits)
        m = h.make_model(g, opset_imports=[h.make_opsetid('', 14)], ir_version=8)
        y, hn, cn = ort.InferenceSession(m.SerializeToString()).run(None, {'x': x, 'h0': h0, 'c0': c0})
        self.assertEqual(y.shape, (1, 1, 25, 100))  # matches measured runtime Y
        eh, ec = cell(x[0].astype(np.float64), h0[0], c0[0], W, R, B, 'iofc', quant=False)
        np.testing.assert_allclose(hn[0], eh, atol=1e-5)
        np.testing.assert_allclose(cn[0], ec, atol=1e-5)

    def test_quantizer_saturates_zero_point_tie(self):
        # s=2, zero-point rint(63.5)=64, so 127 maps to 128 before the clip.
        np.testing.assert_allclose(qrows(np.array([[-127., 0., 127.]])), [[-128., 0., 126.]])


if __name__ == '__main__':
    unittest.main()
