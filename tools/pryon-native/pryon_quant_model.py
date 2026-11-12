#!/usr/bin/env python3
"""NumPy emulation of the stock Pryon graph with its measured quantized arithmetic.

Every HybridConv/HybridGemm/HybridLSTM matrix product quantizes each input row
(conv: each im2col patch) with the measured rule: s=(max-min)/127, inv=1/s
(float32), q=min(rint(x*inv)+rint(-min*inv), 127),
y=float32(sum((q-zq)(qw-zw))) * float32(s*sw).
HybridConv adds its bias inside the kernel; HybridGemm does not. ReLU, MaxPool,
BatchNorm, normalization and softmax run in float. Streaming state follows
tools/pryon-native/LEDGER.md: 6 LFBE frames per cycle, conv history = trailing
(kernel_h - stride_h) rows, LSTM ring reset of row t % 25.

Constant (zero-range) rows have no measured rule; they use the exact float
product and are counted in `constant_rows`.
"""
import numpy as np
import onnx
from onnx import numpy_helper as nh

from pryon_paths import DEFAULT_SOURCE

P = 'StatefulPartitionedCall/functional_5/'


def quantize_activation_rows(x):
    """Measured activation quantizer; shared by inference and QAT preparation."""
    x = np.asarray(x, np.float32)
    lo, hi = x.min(1), x.max(1)
    s = (hi-lo)/np.float32(127)
    const = s == 0
    s = np.where(const, np.float32(1), s)
    inv = (np.float32(1)/s).astype(np.float32)
    z = np.rint(-lo*inv)
    q = np.minimum(np.rint(x*inv[:, None])+z[:, None], 127)-z[:, None]
    return q, s, const


def quant_matmul(x, w, sw, zw, stats):
    """x [M,K] float32 rows; w int8 [N,K]; returns float32 [M,N].

    Epilogue float32(acc) * float32(s*sw): measured bit-exact on conv2..7 (conv1
    rows lying wholly below zero still differ by 1-3 accumulator units). Also
    applied to dense and LSTM products, where it is unmeasured but within
    the measured tolerances."""
    x = np.asarray(x, np.float32)
    q, s, const = quantize_activation_rows(x)
    stats['constant_rows'] += int(const.sum())
    wd = w.astype(np.float64)-zw[:, None]
    acc = q.astype(np.float64)@wd.T  # exact integer accumulator
    y = acc.astype(np.float32)*(s[:, None]*sw[None, :].astype(np.float32))
    if const.any():
        y[const] = x[const].astype(np.float64)@(wd*sw[:, None]).T
    return y


class Model:
    def __init__(self, source=DEFAULT_SOURCE):
        m = onnx.load(source)
        self.init = {i.name: nh.to_array(i) for i in m.graph.initializer
                     if i.raw_data or i.float_data or i.int32_data or i.int64_data}
        nodes = list(m.graph.node)
        attr = lambda n: {a.name: onnx.helper.get_attribute_value(a) for a in n.attribute}
        self.mean = self.init[nodes[4].input[1]].ravel()
        self.inv_std = self.init[nodes[5].input[1]].ravel()
        self.layers = []  # (conv inputs, attrs, pool attrs|None, bn inputs)
        for k, n in enumerate(nodes):
            if n.op_type != 'HybridConv':
                continue
            a = attr(n)
            pool = next((attr(x) for x in nodes[k+1:k+3] if x.op_type == 'MaxPool'), None)
            bn = next(x for x in nodes[k+1:k+4] if x.op_type == 'BatchNormalization')
            self.layers.append((list(n.input), a, pool, list(bn.input), attr(bn)['epsilon']))
        self.lstm = {n.name: list(n.input) for n in nodes if n.op_type == 'HybridLSTM'}
        self.gemm = {n.name.split('/')[2]: list(n.input) for n in nodes if n.op_type == 'HybridGemm'}

    def conv(self, x, inputs, a, stats):
        """x [C,T,F]; valid conv via im2col and quantized patches."""
        w, sw, zw = (self.init[i] for i in inputs[1:4])
        b = self.init[inputs[5]].astype(np.float32)
        kh, kw = a['kernel_shape'][-2:]
        sh, sf = a.get('strides', [1, 1])[-2:]
        C, T, F = x.shape
        to, fo = (T-kh)//sh+1, (F-kw)//sf+1
        patches = np.lib.stride_tricks.sliding_window_view(x, (kh, kw), axis=(1, 2))[:, ::sh, ::sf]
        rows = patches[:, :to, :fo].transpose(1, 2, 0, 3, 4).reshape(to*fo, C*kh*kw)
        stats.setdefault('conv_rows', []).append(rows)
        y = quant_matmul(rows, w.reshape(w.shape[0], -1), sw, zw, stats)+b
        return y.reshape(to, fo, -1).transpose(2, 0, 1).astype(np.float32)

    def bn(self, x, inputs, eps):
        g, b, mu, var = (self.init[i] for i in inputs[1:5])
        # Stock precomputes float32 affine coefficients, then rounds x*scale+shift
        # once. A float64 intermediate reproduces the captured fused epilogue.
        scale = g/np.sqrt(var+eps)
        shift = b-mu*scale
        return (x.astype(np.float64)*scale[:, None, None]+shift[:, None, None]).astype(np.float32)

    def lstm_cell(self, name, x, h, c, stats):
        i = self.lstm[name]
        W, R = (self.init[i[k]][0] for k in (1, 5))
        gx = quant_matmul(x, W, self.init[i[2]][0], self.init[i[3]][0], stats)
        gh = quant_matmul(h, R, self.init[i[6]][0], self.init[i[7]][0], stats)
        B = self.init[i[9]][0].astype(np.float64)
        g = gx+gh+B[:400]+B[400:]
        it, ot, ft, ct = np.split(g, 4, axis=1)
        sig = lambda v: 1/(1+np.exp(-v))
        c2 = sig(ft)*c+sig(it)*np.tanh(ct)
        return (sig(ot)*np.tanh(c2)).astype(np.float32), c2.astype(np.float32)

    def dense(self, key, x, stats, bias=True):
        i = self.gemm[key]
        y = quant_matmul(x, *(self.init[k] for k in i[1:4]), stats)
        if bias:
            y = y+self.init[P+key+'/BiasAdd/ReadVariableOp:0']
        return y.astype(np.float32)

    def stream(self, frames, history=0.0):
        """Yield per-cycle dict: lstm_x [100], classifier_input [25,100],
        softmax_ww [25,3], se [25,2], stats."""
        hist = [None]*len(self.layers)
        state = {n: [np.zeros((25, 100), np.float32)]*2 for n in self.lstm}
        for t in range(len(frames)//6):
            stats = {'constant_rows': 0}
            x = ((np.asarray(frames[6*t:6*t+6], np.float32)-self.mean)*self.inv_std).astype(np.float32)[None]
            for k, (ci, a, pool, bi, eps) in enumerate(self.layers):
                kh, sh = a['kernel_shape'][-2], a.get('strides', [1, 1])[-2]
                keep = kh-sh
                if keep:
                    if hist[k] is None:
                        hist[k] = np.full((x.shape[0], keep, x.shape[2]), history, np.float32)
                    x = np.concatenate([hist[k], x], axis=1)
                    hist[k] = x[:, -keep:]
                x = np.maximum(self.conv(x, ci, a, stats), 0)
                if pool:
                    ph, pw = pool['kernel_shape']; s1, s2 = pool['strides']
                    v = np.lib.stride_tricks.sliding_window_view(x, (ph, pw), axis=(1, 2))[:, ::s1, ::s2]
                    x = v.max(axis=(3, 4))
                x = self.bn(x, bi, eps)
            lx = x.reshape(1, -1)
            out = {'lstm_x': lx[0], 'stats': stats}
            for name in self.lstm:
                h, c = (s.copy() for s in state[name])
                h[t % 25 if t else slice(None)] = 0
                c[t % 25 if t else slice(None)] = 0
                state[name] = list(self.lstm_cell(name, lx, h, c, stats))
            ww, se = state['LSTM__67'][0], state['LSTM__53'][0]
            a_ww = np.maximum(self.dense('fc_ww_1', ww, stats), 0)
            out['classifier_input'] = a_ww
            logits = self.dense('fc_output_ww', a_ww, stats)
            e = np.exp(logits-logits.max(1, keepdims=True))
            out['softmax_ww'] = e/e.sum(1, keepdims=True)
            out['se'] = self.dense('fc_output_se', np.maximum(self.dense('fc_se_1', se, stats), 0), stats)
            out['h_ww'], out['h_se'] = ww, se
            yield out
