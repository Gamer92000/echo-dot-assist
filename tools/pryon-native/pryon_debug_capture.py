#!/usr/bin/env python3
"""Read-only ARM guest observation through QEMU's local GDB socket.

Original ELF/model bytes are never written. QEMU implements Z0 breakpoints
in its translation machinery. Run in a low-PID namespace for Android bionic.
"""
from pathlib import Path
import argparse
import hashlib
import json
import socket
import struct
import subprocess
import time


class Remote:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(600)  # slow --speed runs reach the first hook late
        self.sock.connect(str(path))

    def read_exact(self,size):
        data=bytearray()
        while len(data)<size:
            part=self.sock.recv(size-len(data))
            if not part: raise EOFError('Incomplete debugger packet')
            data.extend(part)
        return bytes(data)

    def command(self, text):
        payload = text.encode()
        self.sock.sendall(b'$' + payload + b'#' + f'{sum(payload) & 255:02x}'.encode())
        while True:
            c = self.sock.recv(1)
            if not c:
                raise EOFError('QEMU debugger closed')
            if c == b'$':
                break
            if c == b'-':
                raise RuntimeError('QEMU rejected debugger checksum')
        data = bytearray()
        while True:
            c = self.sock.recv(1)
            if not c:
                raise EOFError('Incomplete debugger packet')
            if c == b'#':
                break
            data.extend(c)
        checksum = self.read_exact(2)
        if len(checksum) != 2 or int(checksum, 16) != sum(data) & 255:
            raise RuntimeError('Invalid debugger checksum')
        self.sock.sendall(b'+')
        decoded = bytearray()
        i = 0
        while i < len(data):
            if data[i] == 125:
                i += 1
                decoded.append(data[i] ^ 32)
            elif data[i] == 42:
                i += 1
                decoded.extend([decoded[-1]] * (data[i] - 29))
            else:
                decoded.append(data[i])
            i += 1
        return decoded.decode()

    def memory(self, address, size):
        result = bytearray()
        for offset in range(0, size, 1024):
            response = self.command(f'm{address+offset:x},{min(1024,size-offset):x}')
            if response.startswith('E'):
                raise RuntimeError(f'Memory {address+offset:#x}: {response}')
            result.extend(bytes.fromhex(response))
        if len(result) != size:
            raise RuntimeError('Short memory read')
        return bytes(result)

    def regs(self):
        return struct.unpack('<16I', bytes.fromhex(self.command('g'))[:64])

    def breakpoint(self, address, thumb=True, remove=False):
        reply = self.command(f'{"z" if remove else "Z"}0,{address & ~1:x},{2 if thumb else 4}')
        if reply != 'OK':
            raise RuntimeError(f'Breakpoint refused: {reply}')


def harness_main(executable):
    """The main breakpoint belongs to this exact pinned build; never guess it for another binary."""
    if hashlib.sha256(executable.read_bytes()).hexdigest() != '107e8079ca73ac0c2d30d4cc1bb2cd62d78ffe8202bf64fa97cc643cdea23f84':
        raise RuntimeError('Harness identity differs; rebuild with the pinned NDK recipe in README.md')
    return 0x1098


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path, default=Path('local-data/rea-pryon/bootstrap'))
    parser.add_argument('--audio', type=Path)
    parser.add_argument('--hook', type=lambda x:int(x,0), action='append')
    parser.add_argument('--hits', type=int, default=1)
    parser.add_argument('--gemm', action='store_true', help='Capture verified CPUNK int8 GEMM input/output buffers')
    parser.add_argument('--classifier-only', action='store_true', help='Save full buffers only for the 100-to-3 wake classifier')
    parser.add_argument('--quantized', action='store_true', help='Also read GEMM activation quantization intermediates')
    parser.add_argument('--frontend', action='store_true', help='Also capture first-convolution normalized acoustic windows')
    parser.add_argument('--speed',type=float,default=0.025,help='Harness audio pacing multiplier; slow down to avoid debugger-induced backlog drops')
    parser.add_argument('--lstm',action='store_true',help='Read the active LSTM tensor descriptors before and after execution')
    parser.add_argument('--lstm-values',action='store_true',help='With --lstm, also save X, state and Y float buffers')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    path = args.out.resolve()/'debug.sock'
    if path.exists():
        raise RuntimeError(f'Refusing existing socket: {path}')
    audio = args.audio or args.source/'testdata/alexa_espeak.raw'
    library=args.source/'firmware/donut/rootfs/system/lib/libpryon.so'
    executable=args.source/'build/donut/pryon_test'
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (library,executable,audio)}
    if hashes[str(library)]!='65df801e37ad0bbdfaf8922174bceb084fae6ba71c5076b1e23a000523f97027':
        raise RuntimeError('Library identity differs; offsets must be re-established')
    main_offset = harness_main(executable)
    (args.out/'identity.json').write_text(json.dumps(hashes,indent=2)+'\n')
    command = ['qemu-arm', '-g', str(path), '-L', str(args.source/'firmware/donut/rootfs'),
               '-E', 'LD_LIBRARY_PATH=/system/lib:/system/vendor/lib',
               str(args.source/'build/donut/pryon_test'), '-x', str(args.speed), str(audio)]
    with (args.out/'runtime.txt').open('w') as output:
        process = subprocess.Popen(command, stdout=output, stderr=output)
        try:
            for _ in range(100):
                if path.exists() or process.poll() is not None:
                    break
                time.sleep(.02)
            remote = Remote(path)
            print('initial stop', remote.command('?'), flush=True)
            initial_maps = Path(f'/proc/{process.pid}/maps').read_text()
            print('\n'.join(x for x in initial_maps.splitlines() if 'pryon_test' in x), flush=True)
            exe_map = next(x for x in initial_maps.splitlines() if 'pryon_test' in x and x.split()[2] == '00000000')
            base = int(exe_map.split('-')[0],16)
            remote.breakpoint(base+main_offset, thumb=False)
            print('main stop', remote.command('c'), flush=True)
            regs = remote.regs()
            maps = Path(f'/proc/{process.pid}/maps').read_text()
            (args.out/'maps.txt').write_text(maps)
            print('registers', [hex(x) for x in regs], flush=True)
            print('\n'.join(x for x in maps.splitlines() if 'libpryon' in x), flush=True)
            lib_map = next(x for x in maps.splitlines() if 'libpryon.so' in x and x.split()[2] == '00000000')
            lib_base = int(lib_map.split('-')[0],16)
            remote.breakpoint(base+main_offset, thumb=False, remove=True)
            if args.hook is not None:
                for offset in args.hook:
                    remote.breakpoint(lib_base+offset)
                records = []
                for index in range(args.hits):
                    stop = remote.command('c')
                    if not stop.startswith('T05'):
                        print('terminal',stop,flush=True)
                        break
                    regs = remote.regs()
                    hook = regs[15]
                    record = {'stop':stop,'registers':list(regs),'library_base':lib_base,'memory':{}}
                    for reg in [0,1,2,3,13]:
                        try:
                            data = remote.memory(regs[reg],128)
                            record['memory'][str(reg)] = data.hex()
                        except RuntimeError:
                            pass
                    if args.lstm:
                        stack=struct.unpack('<16I',remote.memory(regs[13],64))
                        descriptors={name:ptr for name,ptr in [('X',regs[2]),('W',regs[3])]+[(f'stack{j}',p) for j,p in enumerate(stack[:15])]}
                        record['descriptors']={}
                        for name,ptr in descriptors.items():
                            try:
                                record['descriptors'][name]=list(struct.unpack('<16I',remote.memory(ptr,64)))
                            except RuntimeError:pass
                        d=record['descriptors']
                        # Measured layouts: X=[1,1,100,ptr], Y=[1,1,25,100,ptr], matrices=[ptr,rows,cols,stride]
                        def save(tag,ptr,count):
                            (args.out/f'{index:05d}-{tag}.bin').write_bytes(remote.memory(ptr,count*4))
                        if args.lstm_values:
                            save('lx',d['X'][3],d['X'][2])
                            for tag,slot in [('h',9),('c',10)]:
                                desc=d[f'stack{slot}'];save(tag,desc[0],desc[1]*desc[3])
                        remote.breakpoint(hook,remove=True)
                        ret=regs[14]&~1
                        remote.breakpoint(ret)
                        result=remote.command('c')
                        if not result.startswith('T05') or remote.regs()[15]!=ret:
                            raise RuntimeError('Unexpected LSTM return')
                        remote.breakpoint(ret,remove=True)
                        record['returned_descriptors']={}
                        for name,ptr in descriptors.items():
                            try:record['returned_descriptors'][name]=list(struct.unpack('<16I',remote.memory(ptr,64)))
                            except RuntimeError:pass
                        if args.lstm_values:
                            rd=record['returned_descriptors'];y=rd['stack12'];save('ly',y[4],y[2]*y[3])
                            for tag,slot in [('ho',13),('co',14)]:
                                desc=rd[f'stack{slot}'];save(tag,desc[0],desc[1]*desc[3])
                        remote.breakpoint(hook)
                    if args.gemm and hook-lib_base == 0x123897c:
                        stack = struct.unpack('<32I', remote.memory(regs[13],128))
                        m,k,n = regs[2],regs[3],stack[11]
                        if not (0 < m*k <= 2000000 and 0 < n*k <= 2000000 and 0 < m*n <= 2000000):
                            raise RuntimeError(f'Unexpected GEMM dimensions: {m,k,n}')
                        record['gemm'] = {'m':m,'k':k,'n':n,'input_stride':stack[0],
                                          'output_stride':stack[12],'return_offset':regs[14]-lib_base,
                                          'weights_bytes':stack[2]}
                        selected = not args.classifier_only or (k==100 and n==3) or (args.frontend and k==35 and n==96)
                        buffers = {'x':(regs[1],m*stack[0]*4), 'w':(stack[1],stack[2]),
                                   'scale':(stack[3],stack[4]*4),'zero':(stack[5],stack[6]*4),
                                   'sum':(stack[7],stack[8]*4)}
                        if selected:
                            for name,(ptr,size) in buffers.items():
                                (args.out/f'{index:05d}-{name}.bin').write_bytes(remote.memory(ptr,size))
                        remote.breakpoint(hook,remove=True)
                        if selected and args.quantized and k==100 and n==3:
                            quant_stop = lib_base+0x1238a9e
                            remote.breakpoint(quant_stop)
                            response=remote.command('c')
                            qregs=remote.regs()
                            if not response.startswith('T05') or qregs[15]!=quant_stop:
                                raise RuntimeError('Unexpected quantization stop')
                            remote.breakpoint(quant_stop,remove=True)
                            local=struct.unpack('<46I',remote.memory(qregs[13],184))
                            for name,ptr,size in [('qx',qregs[9],m*k),('xs',local[34],m*4),
                                                  ('xz',local[33],m*4),('xsum',local[45],m*4)]:
                                (args.out/f'{index:05d}-{name}.bin').write_bytes(remote.memory(ptr,size))
                        ret = regs[14]&~1
                        remote.breakpoint(ret)
                        returned = remote.command('c')
                        if not returned.startswith('T05') or remote.regs()[15] != ret:
                            raise RuntimeError(f'Unexpected GEMM return stop {returned}')
                        remote.breakpoint(ret,remove=True)
                        if selected:
                            (args.out/f'{index:05d}-y.bin').write_bytes(remote.memory(stack[9],m*stack[12]*4))
                        remote.breakpoint(hook)
                    records.append(record)
                    if not args.classifier_only or (args.gemm and selected):
                        print('hit',index,'pc',hex(regs[15]-lib_base),'args',[hex(x) for x in regs[:4]],flush=True)
                    if index % 13 == 12 or index == args.hits-1:
                        (args.out/'hits.json').write_text(json.dumps(records,indent=2)+'\n')
                    if not args.gemm and not args.lstm:
                        remote.breakpoint(hook,remove=True)
                        remote.command('s')
                        remote.breakpoint(hook)
                (args.out/'hits.json').write_text(json.dumps(records,indent=2)+'\n')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            path.unlink(missing_ok=True)
            for p in (library,executable,audio):
                if hashlib.sha256(p.read_bytes()).hexdigest()!=hashes[str(p)]:
                    raise RuntimeError(f'Source changed during capture: {p}')


if __name__ == '__main__':
    main()
