#!/usr/bin/env python3
"""Annotate llvm-objdump Thumb disassembly with strings referenced via
`ldr rN, [pc, #x]` + `add rN, pc` pairs.  Usage: annotate_thumb.py <elf> <asm> [func]"""
import re, struct, sys
elf = open(sys.argv[1], 'rb').read()
# program headers -> vaddr to file offset
phoff, = struct.unpack_from('<I', elf, 0x1c); phentsize, phnum = struct.unpack_from('<HH', elf, 0x2a)
segs = []
for i in range(phnum):
    t, off, va, _, fsz = struct.unpack_from('<IIIII', elf, phoff + i * phentsize)
    if t == 1: segs.append((va, off, fsz))
def rd(va, n):
    for v, o, s in segs:
        if v <= va < v + s: return elf[o + va - v:o + va - v + n]
    return b''
def cstr(va):
    b = rd(va, 200); e = b.find(b'\0')
    if e <= 3: return None
    s = b[:e]
    return s.decode() if all(32 <= c < 127 or c in (9, 10) for c in s) else None
func = sys.argv[3] if len(sys.argv) > 3 else None
pend = {}; on = func is None
for line in open(sys.argv[2]):
    line = line.rstrip('\n')
    if func:
        if re.match(r'^[0-9a-f]+ <' + re.escape(func) + r'>:', line): on = True
        elif on and re.match(r'^[0-9a-f]+ <', line): break
    if not on: continue
    m = re.match(r'\s*([0-9a-f]+):\s+ldr(?:\.w)?\s+(r\d+|r1[0-2]|lr), \[pc, #[^\]]+\]\s+@ 0x([0-9a-f]+)', line)
    if m: pend[m.group(2)] = int(m.group(3), 16)
    m = re.match(r'\s*([0-9a-f]+):\s+add\s+(r\d+|lr), pc$', line)
    if m and m.group(2) in pend:
        w = rd(pend.pop(m.group(2)), 4)
        if len(w) == 4:
            tgt = (struct.unpack('<I', w)[0] + int(m.group(1), 16) + 4) & 0xffffffff
            s = cstr(tgt)
            line += f'    ; -> 0x{tgt:x}' + (f' "{s}"' if s else '')
    print(line)
