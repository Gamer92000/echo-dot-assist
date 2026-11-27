#!/usr/bin/env python3
"""Compare captured stock Pryon tensors with the translated ONNX head.

Raw recordings, weights and captured activations remain in ignored local-data.
This does not edit the firmware, original model, or translated model.
"""
from pathlib import Path
import argparse
import hashlib
import json
import numpy as np

from pryon_paths import DEFAULT_SOURCE


def quantize_rows(x):
    """Measured CPUNK nonconstant-row rule; reject unmeasured constant rows."""
    x = np.asarray(x, dtype=np.float32)
    if x.ndim != 2 or not np.isfinite(x).all():
        raise ValueError('Expected finite matrix')
    scale = (x.max(axis=1)-x.min(axis=1))/np.float32(127)
    if np.any(scale == 0):
        raise ValueError('Constant-row quantization needs separate runtime evidence')
    inv = (np.float32(1)/scale).astype(np.float32)  # measured: reciprocal multiply (conv1 ties)
    zero = np.rint(-x.min(axis=1)*inv).astype(np.int32)
    quant = (np.rint(x*inv[:,None]).astype(np.int32)+zero[:,None])
    if np.any((quant < -128)|(quant > 127)):
        raise ValueError('Quantized value outside measured int8 range')
    return quant.astype(np.int8), scale, zero


def pack_weight_pairs(w):
    return np.concatenate([w[i:i+2].T.ravel() for i in range(0,len(w),2)])


import os
HERE = Path(__file__).resolve().parent
LFBE_BIN = Path(os.environ.get('PRYON_LFBE_BIN', HERE.parents[1]/'build/pryon_lfbe'))     # built on first use from pryon_lfbe.c


def lfbe(pcm):
    """Stock Pryon LFBE frontend, bit-exact (see tools/pryon-native/LEDGER.md, "Frontend").

int16 PCM -> [frames, 64] float32 via pryon_lfbe.c, rebuilt when the source is
newer than the binary. 400-sample frames / 160 hop from sample 0, no padding.
"""
    import subprocess, tempfile
    src = HERE/'pryon_lfbe.c'
    if not LFBE_BIN.exists() or LFBE_BIN.stat().st_mtime < src.stat().st_mtime:
        LFBE_BIN.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['cc', '-O2', '-ffp-contract=off', '-o', str(LFBE_BIN), str(src), '-lm'], check=True)
    with tempfile.TemporaryDirectory() as d:
        np.asarray(pcm, '<i2').tofile(f'{d}/in.raw')
        subprocess.run([str(LFBE_BIN), f'{d}/in.raw', f'{d}/out.f32'], check=True)
        return np.fromfile(f'{d}/out.f32', '<f4').reshape(-1, 64)


def metrics(a,b):
    d=np.asarray(a,dtype=np.float64)-np.asarray(b,dtype=np.float64)
    return {'values':int(d.size),'max_abs':float(np.max(abs(d))),
            'rmse':float(np.sqrt(np.mean(d*d)))}


def analyze(capture, source, native, audio=None):
    import onnx
    import onnxruntime as ort
    from onnx import numpy_helper as nh
    original=onnx.load(source)
    init={i.name:nh.to_array(i) for i in original.graph.initializer
          if i.raw_data or i.float_data or i.int32_data or i.int64_data}
    head=next(n for n in original.graph.node if n.op_type=='HybridGemm' and 'fc_output_ww/' in n.name)
    w,sw,zw=[init[x] for x in head.input[1:4]]
    translated=onnx.load(native)
    node=next(n for n in translated.graph.node if n.name==head.name+'__native')
    weight=next(i for i in translated.graph.initializer if i.name==node.input[1])
    graph=onnx.helper.make_graph([node],'captured-classifier',
          [onnx.helper.make_tensor_value_info(node.input[0],onnx.TensorProto.FLOAT,[None,100])],
          [onnx.helper.make_tensor_value_info(node.output[0],onnx.TensorProto.FLOAT,[None,3])],[weight])
    model=onnx.helper.make_model(graph,opset_imports=list(translated.opset_import))
    model.ir_version=translated.ir_version
    session=ort.InferenceSession(model.SerializeToString(),providers=['CPUExecutionProvider'])
    rows=[];features=[];all_ref=[];all_float=[];all_quant=[];all_x=[]
    mean=init[original.graph.node[4].input[1]].ravel()
    inverse_std=init[original.graph.node[5].input[1]].ravel()
    for index,record in enumerate(json.loads((capture/'hits.json').read_text())):
        if 'gemm' not in record:
            continue
        g=record['gemm'];m,k,n=[g[key] for key in ('m','k','n')]
        prefix=capture/f'{index:05d}'
        def read(name,dtype): return np.fromfile(str(prefix)+'-'+name+'.bin',dtype)
        if not Path(str(prefix)+'-x.bin').exists():
            continue
        x=read('x',np.float32).reshape(m,g['input_stride'])[:,:k]
        if (m,k,n)==(360,35,96):
            windows=x.reshape(6,60,7,5)
            if not np.array_equal(windows[:,:-1,:,1:],windows[:,1:,:,:-1],equal_nan=True):
                raise ValueError('First-convolution frequency overlap mismatch')
            current=np.concatenate([windows[:,0,6,:4],windows[:,:,6,4]],axis=1)
            features.append(current/inverse_std+mean)
        if (k,n)!=(100,3):
            continue
        if not np.array_equal(pack_weight_pairs(w),read('w',np.int8)):
            raise ValueError('Captured classifier weights differ from source')
        for name,expected,dtype in [('scale',sw,np.float32),('zero',zw,np.int32),
                                    ('sum',init[head.input[4]],np.int32)]:
            if not np.array_equal(read(name,dtype),expected):
                raise ValueError(f'Classifier {name} mismatch')
        y=read('y',np.float32).reshape(m,g['output_stride'])[:,:n]
        good=np.isfinite(x).all(axis=1)&np.isfinite(y).all(axis=1)
        row={'call':index,'rows':m,'finite_rows':int(good.sum())}
        if good.any():
            a=x[good];b=y[good]
            float_y=session.run(None,{node.input[0]:a})[0]
            q,s,z=quantize_rows(a)
            quant_y=((q.astype(np.int64)-z[:,None])@(w.astype(np.int64)-zw[:,None]).T)*s[:,None]*sw[None,:]
            row['translated_onnx_head']=metrics(float_y,b)
            row['reconstructed_quantized_head']=metrics(quant_y,b)
            if Path(str(prefix)+'-qx.bin').exists():
                actual_q=read('qx',np.int8).reshape(m,k)[good]
                row['quantized_activation_mismatches']=int(np.count_nonzero(q!=actual_q))
                row['activation_scale_max_abs']=float(np.max(abs(s-read('xs',np.float32)[good])))
                row['activation_zero_mismatches']=int(np.count_nonzero(z!=read('xz',np.int32)[good]))
            all_x.append(a);all_ref.append(b);all_float.append(float_y);all_quant.append(quant_y)
        rows.append(row)
    runtime=(capture/'runtime.txt').read_text()
    report={'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
            'native_sha256':hashlib.sha256(native.read_bytes()).hexdigest(),
            'classifier_input':head.input[0],'captures':rows,
            'backlog_reset':'backlog size reset' in runtime,
            'detections':[s for s in runtime.splitlines() if s.startswith('RESULT ')],
            'scope':'Real captured classifier activations through isolated translated ONNX Gemm; not whole-encoder parity'}
    if all_ref:
        ref=np.concatenate(all_ref)
        report['translated_onnx_head']=metrics(np.concatenate(all_float),ref)
        report['reconstructed_quantized_head']=metrics(np.concatenate(all_quant),ref)
        np.savez(capture/'classifier-activations.npz',activation=np.concatenate(all_x),
                 reference=ref,translated=np.concatenate(all_float),quantized=np.concatenate(all_quant))
    if features:
        np.save(capture/'lfbe-recovered.npy',np.concatenate(features))
        report['recovered_lfbe_frames']=sum(len(f) for f in features)
        if audio:
            recovered=np.concatenate(features);candidate=lfbe(np.fromfile(audio,'<i2'))[:len(recovered)]
            report['frontend_lfbe']=metrics(candidate,recovered)
            report['frontend_lfbe']['floor_positions_match']=bool(np.array_equal(candidate==0,recovered==0))
    (capture/'comparison.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    parser.add_argument('--source',type=Path,default=DEFAULT_SOURCE)
    parser.add_argument('--native',type=Path,default=Path('local-data/pryon-training/pryon-native-float.onnx'))
    parser.add_argument('--audio',type=Path,help='raw s16le PCM pushed by the capture; enables LFBE parity check')
    a=parser.parse_args();report=analyze(a.capture,a.source,a.native,a.audio)
    print(json.dumps({k:v for k,v in report.items() if k!='captures'},indent=2))
