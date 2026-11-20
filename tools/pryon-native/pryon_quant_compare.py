#!/usr/bin/env python3
"""Compare pryon_quant_model against stock captures (lstm-values, alexa-aligned)."""
import numpy as np, json, sys, time
sys.path.insert(0,'.')
from pathlib import Path
from pryon_runtime_analysis import lfbe
from pryon_quant_model import Model
from pryon_paths import DEFAULT_AUDIO
pcm=np.fromfile(DEFAULT_AUDIO,'<i2')
feats=np.load(sys.argv[1]) if len(sys.argv)>1 else lfbe(pcm)
t0=time.time(); outs=list(Model().stream(feats)); print('cycles',len(outs),'time %.1fs'%(time.time()-t0),'const rows total',sum(o['stats']['constant_rows'] for o in outs))
cap=Path('local-data/rea-pryon/lstm-values')
lx=np.array([np.fromfile(cap/f'{2*t:05d}-lx.bin','<f4') for t in range(138)])
ho=np.array([np.fromfile(cap/f'{2*t:05d}-ho.bin','<f4').reshape(25,100) for t in range(138)])
hs=np.array([np.fromfile(cap/f'{2*t+1:05d}-ho.bin','<f4').reshape(25,100) for t in range(138)])
ca=Path('local-data/rea-pryon/alexa-aligned'); hits=json.loads((ca/'hits.json').read_text())
A=np.array([np.fromfile(ca/f'{i:05d}-x.bin','<f4').reshape(25,100) for i,r in enumerate(hits) if r.get('gemm') and (r['gemm']['k'],r['gemm']['n'])==(100,3)])
import onnx
from onnx import numpy_helper as nh
from pryon_paths import DEFAULT_SOURCE
bias=next(nh.to_array(i) for i in onnx.load(DEFAULT_SOURCE).graph.initializer if i.name.endswith('fc_output_ww/BiasAdd/ReadVariableOp:0'))
Y=np.array([np.fromfile(ca/f'{i:05d}-y.bin','<f4').reshape(25,r['gemm']['output_stride'])[:,:3] for i,r in enumerate(hits) if r.get('gemm') and (r['gemm']['k'],r['gemm']['n'])==(100,3)])+bias
S=np.exp(Y-Y.max(-1,keepdims=True)); S/=S.sum(-1,keepdims=True)
def cmp(name,ours,stock,start=30):
    ours=np.asarray(ours)[:len(stock)]; ok=np.isfinite(stock)&(np.abs(stock)<1e6); ok[:start]=False
    d=np.abs(ours[ok]-stock[ok]); print(f'{name:22s} values {ok.sum():7d} max {d.max():.3g} rmse {np.sqrt((d*d).mean()):.3g}  p99.9 {np.percentile(d,99.9):.3g}')
for st in (5,30):
    print('--- from cycle',st)
    cmp('LSTM X',[o['lstm_x'] for o in outs],lx,st)
    cmp('ww hidden',[o['h_ww'] for o in outs],ho,st)
    cmp('se hidden',[o['h_se'] for o in outs],hs,st)
    cmp('classifier input',[o['classifier_input'] for o in outs],A,st)
    cmp('wake softmax',[o['softmax_ww'] for o in outs],S,st)
print('--- per-cycle LSTM X max err / const rows')
e=[np.abs(outs[t]['lstm_x']-lx[t]).max() for t in range(138)]
c=[outs[t]['stats']['constant_rows'] for t in range(138)]
for t in range(5,138):
    if e[t]>1e-4 or c[t]: print(t,'err %.2g'%e[t],'const',c[t], end=' | ')
print(); print('cycles with err<=1e-5:',sum(1 for t in range(5,138) if e[t]<=1e-5),'of',133)
