#!/usr/bin/env python3
"""Minimal two-pass 8051 assembler for the handful of instructions the experiments use.
Program = list of tuples: ('label',) or (mnemonic, args...).  Returns bytes."""

SFR = {'ACC': 0xe0, 'B': 0xf0, 'DPL': 0x82, 'DPH': 0x83, 'PSW': 0xd0, 'SP': 0x81, 'IE': 0xa8}

def bitaddr(s):          # 'ACC.3'
    r, b = s.split('.')
    return SFR[r] + int(b)

def enc(ins, pc, labels):
    m, *a = ins
    def rel(t):
        d = labels[t] - (pc + size(ins))
        assert -128 <= d <= 127, (ins, d)
        return d & 0xff
    def a16(t):
        v = labels[t] if isinstance(t, str) else t
        return [v >> 8, v & 0xff]
    if m == 'push': return [0xc0, SFR.get(a[0], a[0]) if isinstance(a[0], str) else a[0]]
    if m == 'pop': return [0xd0, SFR.get(a[0], a[0]) if isinstance(a[0], str) else a[0]]
    if m == 'mov_dptr': return [0x90] + a16(a[0])
    if m == 'movx_a_dptr': return [0xe0]
    if m == 'movx_dptr_a': return [0xf0]
    if m == 'inc_a': return [0x04]
    if m == 'inc_dptr': return [0xa3]
    if m == 'mov_a_imm': return [0x74, a[0]]
    if m == 'mov_b_a': return [0xf5, 0xf0]
    if m == 'mov_a_b': return [0xe5, 0xf0]
    if m == 'xch_a_b': return [0xc5, 0xf0]
    if m == 'cpl_a': return [0xf4]
    if m == 'anl_a_b': return [0x55, 0xf0]
    if m == 'jnb': return [0x30, bitaddr(a[0]), rel(a[1])]
    if m == 'jnz': return [0x70, rel(a[0])]
    if m == 'sjmp': return [0x80, rel(a[0])]
    if m == 'ljmp': return [0x02] + a16(a[0])
    if m == 'lcall': return [0x12] + a16(a[0])
    if m == 'ret': return [0x22]
    raise ValueError(m)

SIZES = {'push': 2, 'pop': 2, 'mov_dptr': 3, 'movx_a_dptr': 1, 'movx_dptr_a': 1, 'inc_a': 1, 'inc_dptr': 1,
         'mov_a_imm': 2, 'mov_b_a': 2, 'mov_a_b': 2, 'xch_a_b': 2, 'cpl_a': 1, 'anl_a_b': 2, 'jnb': 3, 'jnz': 2,
         'sjmp': 2, 'ljmp': 3, 'lcall': 3, 'ret': 1}

def size(ins):
    return SIZES[ins[0]]

def assemble(prog, base):
    labels, pc = {}, base
    for ins in prog:
        if len(ins) == 1 and ins[0] not in SIZES:
            labels[ins[0]] = pc
        else:
            pc += size(ins)
    out, pc = [], base
    for ins in prog:
        if len(ins) == 1 and ins[0] not in SIZES:
            continue
        b = enc(ins, pc, labels)
        assert len(b) == size(ins)
        out += b
        pc += len(b)
    return bytes(out), labels
