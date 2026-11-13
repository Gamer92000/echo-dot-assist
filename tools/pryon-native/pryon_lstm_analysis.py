#!/usr/bin/env python3
"""Test HybridLSTM hypotheses against stock-runtime --lstm-values captures.

Checks, per captured call: state ring (which row resets, carry-over of the rest),
Y == returned hidden, and the cell math for each candidate gate order, in float
and with the measured per-row activation quantizer. Reads only local captures.
"""
from pathlib import Path
import argparse
import json
import numpy as np
import onnx
from onnx import numpy_helper as nh

from pryon_paths import DEFAULT_SOURCE
from pryon_runtime_analysis import metrics

NODES = ['LSTM__53', 'LSTM__67']  # hit parity order is fitted, not assumed
ORDERS = ['iofc', 'ifco', 'ifoc', 'icfo']


def sigmoid(v): return 1/(1+np.exp(-np.clip(v, -500, 500)))


def deq(q, s, z): return (q.astype(np.float64)-z[..., None])*s[..., None]


def qrows(x):
    """Measured per-row 127-step quantizer, dequantized. A zero-point tie can
    reach 128; the recurrent kernel saturates it to 127 (measured, calls 10/11)."""
    x = x.astype(np.float32)
    lo, hi = x.min(1, keepdims=True), x.max(1, keepdims=True)
    s = (hi-lo)/np.float32(127)
    s = np.where(s == 0, 1, s)
    inv = (np.float32(1)/s).astype(np.float32)  # reciprocal multiply, as measured on conv1 ties
    z = np.rint(-lo*inv)
    return ((np.minimum(np.rint(x*inv)+z, 127)-z)*s).astype(np.float64)


def cell(x, h, c, W, R, B, order, quant):
    if quant:
        x, h = qrows(x), qrows(h)
    g = x@W.T + h@R.T + B[:400] + B[400:]
    parts = dict(zip(order, np.split(g, 4, axis=1)))
    i, f, o, cc = sigmoid(parts['i']), sigmoid(parts['f']), sigmoid(parts['o']), np.tanh(parts['c'])
    c2 = f*c + i*cc
    return o*np.tanh(c2), c2


def load_nodes(source):
    m = onnx.load(source)
    init = {i.name: nh.to_array(i) for i in m.graph.initializer if i.raw_data or i.float_data or i.int32_data}
    out = {}
    for n in m.graph.node:
        if n.name in NODES:
            x = n.input
            out[n.name] = (deq(init[x[1]][0], init[x[2]][0], init[x[3]][0]),
                           deq(init[x[5]][0], init[x[6]][0], init[x[7]][0]),
                           init[x[9]][0].astype(np.float64))
    return out


def analyze(capture, source):
    nodes = load_nodes(source)
    hits = json.loads((capture/'hits.json').read_text())
    read = lambda i, t: np.fromfile(capture/f'{i:05d}-{t}.bin', '<f4').reshape(-1, 100).astype(np.float64)
    report = {'calls': len(hits), 'ring': [], 'fits': {}}
    prev = {}
    for i in range(len(hits)):
        if not (capture/f'{i:05d}-ho.bin').exists():
            break
        x, h, c, y, ho, co = [read(i, t) for t in ('lx', 'h', 'c', 'ly', 'ho', 'co')]
        branch = i % 2
        zero = [int(j) for j in np.where((h == 0).all(1) & (c == 0).all(1))[0]]
        ring = {'call': i, 'branch': branch, 'zero_rows': zero,
                'y_equals_ho': bool(np.array_equal(y, ho, equal_nan=True)),
                'x_finite': bool(np.isfinite(x).all())}
        if branch in prev:
            pho, pco = prev[branch]
            keep = [j for j in range(25) if j not in zero]
            ring['carried_rows_equal'] = bool(np.array_equal(h[keep], pho[keep], equal_nan=True)
                                              and np.array_equal(c[keep], pco[keep], equal_nan=True))
        prev[branch] = (ho, co)
        report['ring'].append(ring)
        good = np.isfinite(x).all() & np.isfinite(h).all(1) & np.isfinite(c).all(1) & np.isfinite(ho).all(1)
        if not good.any():
            continue
        for name, (W, R, B) in nodes.items():
            for order in ORDERS:
                for quant in (False, True):
                    hh, cc = cell(x, h[good], c[good], W, R, B, order, quant)
                    key = f'{name}/{order}/{"quant" if quant else "float"}/branch{branch}'
                    report['fits'].setdefault(key, []).append((hh-ho[good], cc-co[good]))
    summary = {}
    for key, parts in report['fits'].items():
        dh = np.concatenate([p[0] for p in parts]); dc = np.concatenate([p[1] for p in parts])
        summary[key] = {'h': metrics(dh, 0*dh), 'c': metrics(dc, 0*dc)}
    report['fits'] = dict(sorted(summary.items(), key=lambda kv: kv[1]['h']['rmse']))
    (capture/'lstm-comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--source', type=Path, default=DEFAULT_SOURCE)
    a = p.parse_args(); r = analyze(a.capture, a.source)
    rings = r['ring']
    print('calls', len(rings), 'y==ho all', all(x['y_equals_ho'] for x in rings),
          'carry-over all', all(x.get('carried_rows_equal', True) for x in rings))
    print('zero rows (branch 0):', [x['zero_rows'] if len(x['zero_rows']) < 25 else 'all' for x in rings if x['branch'] == 0][:40])
    for k, v in list(r['fits'].items())[:8]:
        print(k, 'h rmse %.3g max %.3g | c rmse %.3g max %.3g' % (v['h']['rmse'], v['h']['max_abs'], v['c']['rmse'], v['c']['max_abs']))
