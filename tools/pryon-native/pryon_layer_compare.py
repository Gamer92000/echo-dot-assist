#!/usr/bin/env python3
"""Per-layer HybridConv check against a --gemm capture with x/y dumps.

usage: pryon_layer_compare.py CAPTURE AUDIO.raw
For each conv layer: measured quantized kernel on the STOCK im2col input vs the
stock output, and this emulator's own input vs the stock input.
"""
import numpy as np, json, sys
from pathlib import Path
from pryon_runtime_analysis import lfbe
from pryon_quant_model import Model, quant_matmul
cap=Path(sys.argv[1]); hits=json.loads((cap/'hits.json').read_text())
M=Model(); outs=list(M.stream(lfbe(np.fromfile(sys.argv[2],'<i2'))))
convdims=[(360,35,96),(18,1440,128),(6,1024,128),(4,768,160),(1,1280,160),(1,160,500),(1,500,100)]
cyc=-1; rep={}
for i,r in enumerate(hits):
    g=r.get('gemm')
    if not g or not (cap/f'{i:05d}-y.bin').exists(): continue
    d=(g['m'],g['k'],g['n'])
    if d==convdims[0]: cyc+=1
    if d not in convdims or cyc<5: continue
    L=convdims.index(d)
    x=np.fromfile(cap/f'{i:05d}-x.bin','<f4').reshape(g['m'],g['input_stride'])[:,:g['k']]
    y=np.fromfile(cap/f'{i:05d}-y.bin','<f4').reshape(g['m'],g['output_stride'])[:,:g['n']]
    ci=M.layers[L][0]; w,sw,zw=(M.init[k] for k in ci[1:4]); b=M.init[ci[5]]
    st={'constant_rows':0}
    yk=quant_matmul(x,w.reshape(w.shape[0],-1),sw,zw,st)+b
    mine=outs[cyc]['stats']['conv_rows'][L]
    ke=np.abs(yk-y).max(); ie=np.abs(mine-x).max()
    rep.setdefault(L,[]).append((cyc,ke,ie))
for L,v in sorted(rep.items()):
    v=np.array(v); bad_in=v[v[:,2]>1e-5]
    print(f'conv{L+1} cycles {len(v)}  kernel(stock x)->stock y max {v[:,1].max():.3g}  my input vs stock x max {v[:,2].max():.3g}  first input-diverging cycle {int(bad_in[0,0]) if len(bad_in) else None}')
