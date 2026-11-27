#!/usr/bin/env python3
"""Capture the stock LFBE frontend's FFT (libpryon +0xa361ba) in/out buffers.

The LFBE feature extractor (+0x105f99a) calls the srfft slot +0xa361ba(this,
float *data, bool forward) when its srfft_ member is set (always, for 512).
The wrapper dispatches to +0x126b5f8, identified from measured state layout
and execution as Ne10 ne10_fft_r2c_1d_float32_neon (NE10_UNROLL_LEVEL 1),
then repacks the result into Kaldi's FFT layout. It is NOT KissFFT.
The Kaldi RealFft fallback (+0xa35020) serves an idle 320-point pipeline.
Saves the 512 floats at entry and after return.
--stage ne10cfg stops once at the Ne10 r2c entry (+0x126b5f8, r2 = cfg) and
saves the level-1 cfg struct, r_factors_neon, r_twiddles_neon and
r_super_twiddles_neon tables (nfft 512).
--stage log instead hooks VectorBase<float>::ApplyLog (+0xa33fc4, r0 = {data,
dim}) to save the mel energies before and after the log.
Read-only debugger use, same harness and identity gates as
pryon_debug_capture.py. Run under bwrap (see tools/pryon-native/LEDGER.md).
usage: pryon_fft_capture.py OUT_DIR [--calls N] [--audio RAW] [--speed X]
"""
import argparse, hashlib, json, struct, subprocess, time
from pathlib import Path
from pryon_debug_capture import Remote, harness_main

STAGES = {'fft': 0xa361ba, 'log': 0xa33fc4, 'ne10cfg': 0x126b5f8}
N = 512


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('out', type=Path)
    p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
    p.add_argument('--audio', type=Path)
    p.add_argument('--calls', type=int, default=200)
    p.add_argument('--speed', type=float, default=0.025)
    p.add_argument('--stage', choices=STAGES, default='fft')
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    sock = a.out.resolve()/'debug.sock'
    audio = a.audio or a.source/'testdata/alexa_espeak.raw'
    lib = a.source/'firmware/donut/rootfs/system/lib/libpryon.so'
    exe = a.source/'build/donut/pryon_test'
    if hashlib.sha256(lib.read_bytes()).hexdigest() != '65df801e37ad0bbdfaf8922174bceb084fae6ba71c5076b1e23a000523f97027':
        raise RuntimeError('Library identity differs')
    main_offset = harness_main(exe)
    (a.out/'identity.json').write_text(json.dumps({str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (lib, exe, audio)}, indent=2)+'\n')
    cmd = ['qemu-arm', '-g', str(sock), '-L', str(a.source/'firmware/donut/rootfs'),
           '-E', 'LD_LIBRARY_PATH=/system/lib:/system/vendor/lib', str(exe), '-x', str(a.speed), str(audio)]
    log = (a.out/'runtime.txt').open('w')
    proc = subprocess.Popen(cmd, stdout=log, stderr=log)
    try:
        for _ in range(100):
            if sock.exists():
                break
            time.sleep(.02)
        r = Remote(sock)
        r.command('?')
        maps = Path(f'/proc/{proc.pid}/maps').read_text()
        base = int(next(x for x in maps.splitlines() if 'pryon_test' in x and x.split()[2] == '00000000').split('-')[0], 16)
        r.breakpoint(base+main_offset, thumb=False); r.command('c'); r.breakpoint(base+main_offset, thumb=False, remove=True)
        maps = Path(f'/proc/{proc.pid}/maps').read_text()
        lb = int(next(x for x in maps.splitlines() if 'libpryon.so' in x and x.split()[2] == '00000000').split('-')[0], 16)
        HOOK = STAGES[a.stage]
        r.breakpoint(lb+HOOK)
        meta = []
        i = 0
        while i < a.calls:
            if not r.command('c').startswith('T05'):
                break
            regs = r.regs()
            if a.stage == 'ne10cfg':
                cfg = struct.unpack('<9I', r.memory(regs[2], 36))
                nfft = cfg[1]
                tables = {'cfg': (regs[2], 36), 'r_twiddles_neon': (cfg[5], nfft//4*8),
                          'r_factors_neon': (cfg[7], 64*4), 'r_super_twiddles_neon': (cfg[8], (12+nfft//32*12)*8)}
                for name, (ptr, size) in tables.items():
                    (a.out/f'ne10-{name}.bin').write_bytes(r.memory(ptr, size))
                meta.append({'nfft': nfft, 'cfg_words': list(cfg)})
                break
            data, dim = (regs[1], N) if a.stage == 'fft' else struct.unpack('<2I', r.memory(regs[0], 8))
            (a.out/f'{i:05d}-in.bin').write_bytes(r.memory(data, dim*4))
            ret = regs[14] & ~1
            r.breakpoint(lb+HOOK, remove=True); r.breakpoint(ret)
            if not r.command('c').startswith('T05') or r.regs()[15] != ret:
                raise RuntimeError('unexpected return stop')
            (a.out/f'{i:05d}-out.bin').write_bytes(r.memory(data, dim*4))
            r.breakpoint(ret, remove=True); r.breakpoint(lb+HOOK)
            meta.append({'dim': dim, 'forward': regs[2], 'return_offset': ret-lb})
            i += 1
        (a.out/'calls.json').write_text(json.dumps(meta, indent=1)+'\n')
    finally:
        if proc.poll() is None:
            proc.terminate(); proc.wait(timeout=5)
        sock.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
