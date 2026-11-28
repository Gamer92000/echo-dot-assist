#!/usr/bin/env python3
"""Builds the bytes of the on-device experiments (static design, nothing run):
  rxhook.bin   8051 code at 0x3F00, entered from coex's main loop head (0x3842 patched to LJMP 3F00h)
  seq_ext.bin  sequencer extension at 0x34AC (the free gap after the MAC filter) + the one-instruction hooks
Run: python3 -I mkexp.py  (writes the files beside itself, prints a summary)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from a51 import assemble

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- 8051 hook (experiment i) ----------
# Mailbox (XDATA, inside the free 0x3F00-0x3FDF; the host reads it with single-byte readRegExternal):
#  3FC0 0xA5 marker | 3FC1/2 loops (LE16) | 3FC3/4 rising edges of rib 0x43A bit0 (data indication pending)
#  3FC5/6 rising edges of rib 0x400 bit3 (RX PBM in use) | 3FC7 prev 0x43A | 3FC8 prev 0x400
#  3FC9 0x400 at the last bit3 edge | 3FCA..3FCD es time 0x586..0x589 at that edge
#  3FD8/9 written by the patched sequencer (phase-1 count), 3FDA its 2015-ack count, 3FDB last FCF1
P = []
def a(*x): P.append(x)
a('hook')
for r in ('ACC', 'DPL', 'DPH', 'PSW', 'B'): a('push', r)
a('mov_dptr', 0x3fc0); a('mov_a_imm', 0xa5); a('movx_dptr_a')
a('inc_dptr'); a('lcall', 'inc16')                                  # 3FC1/2 loops
# rib 0x43A bit 0
a('mov_dptr', 0x043a); a('movx_a_dptr'); a('mov_b_a')              # B = now
a('mov_dptr', 0x3fc7); a('movx_a_dptr'); a('xch_a_b')              # A = now, B = prev
a('movx_dptr_a'); a('xch_a_b'); a('cpl_a'); a('anl_a_b')           # prev := now; A = now & ~prev ... (B=now)
a('jnb', 'ACC.0', 'n1'); a('mov_dptr', 0x3fc3); a('lcall', 'inc16')
a('n1')
# rib 0x400 bit 3
a('mov_dptr', 0x0400); a('movx_a_dptr'); a('mov_b_a')
a('mov_dptr', 0x3fc8); a('movx_a_dptr'); a('xch_a_b')
a('movx_dptr_a'); a('xch_a_b'); a('cpl_a'); a('anl_a_b')
a('jnb', 'ACC.3', 'n2')
a('mov_dptr', 0x3fc5); a('lcall', 'inc16')
a('mov_a_b'); a('mov_dptr', 0x3fc9); a('movx_dptr_a')               # 0x400 at the edge
for i in range(4):                                                  # time, 0x586 first (latching read)
    a('mov_dptr', 0x0586 + i); a('movx_a_dptr'); a('mov_dptr', 0x3fca + i); a('movx_dptr_a')
a('n2')
for r in ('B', 'PSW', 'DPH', 'DPL', 'ACC'): a('pop', r)
a('mov_dptr', 0x0205)                                               # the instruction the patch replaced
a('ljmp', 0x3845)
a('inc16')                                                          # LE16 counter at DPTR
a('movx_a_dptr'); a('inc_a'); a('movx_dptr_a'); a('jnz', 'r'); a('inc_dptr'); a('movx_a_dptr'); a('inc_a'); a('movx_dptr_a')
a('r'); a('ret')

hook, lab = assemble(P, 0x3f00)
assert 0x3f00 + len(hook) <= 0x3fc0, hex(len(hook))
open(os.path.join(HERE, 'rxhook.bin'), 'wb').write(hook)

# ---------------------------------------------------------------- sequencer extension ---------------
# ISA as reconstructed in macfilt.lst.  Registers: 0 zero, 1 A, 2/3 P (pointer lo/hi), 4 I, 5 J, 6 C, 7 marker.
def ST(addr, v): return [0x30, v, addr & 0xff, addr >> 8]
def STr(r, addr, orv=0): return [0x30 | r, orv, addr & 0xff, addr >> 8]
def LD(r, addr, mask): return [0x20 | r, mask, addr & 0xff, addr >> 8]
def MOV(r, v): return [0x10 | r, v]
def ADD(r, v): return [0x60 | r, v]
def JEQ(r, v, t): return [0x40 | r, v, t & 0xff, t >> 8]
def JNE(r, v, t): return [0x50 | r, v, t & 0xff, t >> 8]
def JMP(t): return JEQ(0, 0, t)
def COPY(n): return [0x88, n - 1]
def MARK(v): return [0x17, v]

# 802.15.4-2015's PAN ID compression (table 7-2), for frame version 2 only: which PANs a header carries, by the
# addressing modes (FCF1 & 0x0c dst, FCF1 & 0xc0 src) and the compression bit (FCF0 & 0x40).  The tree leaves C (r6) =
# bit 0 dst PAN present | bit 1 src PAN present.  2006's rule (the filter's own) differs for both extended
# (compressed: no PAN at all; not: the dst PAN only) and for one address alone with the bit set (no PAN).
# Only opcodes Amazon's own program uses (the review, 2026-11-27: r2/r3 loads and compares are inferred, never seen):
# A is loaded again from the scratch copy of the FCF (0x6d1d/0x6d1e) for every test, C holds the answer.
FCF0, FCF1 = 0x6d1d, 0x6d1e
def dmode(): return LD(1, FCF1, 0x0c)
def smode(): return LD(1, FCF1, 0xc0)
def comp(): return LD(1, FCF0, 0x40)
def pan_tree(code, lab, g, p):
    code += MOV(6, 0)
    code += dmode() + JNE(1, 0, g(p + 'd'))                         # a destination address?
    code += smode() + JNE(1, 0, g(p + 'ns'))
    code += comp() + JEQ(1, 0, g(p + 'e')) + MOV(6, 1) + JMP(g(p + 'e'))   # none/none: the dst PAN only when compressed
    lab(p + 'ns')
    code += comp() + JNE(1, 0, g(p + 'e')) + MOV(6, 2) + JMP(g(p + 'e'))   # none/src: the src PAN unless compressed
    lab(p + 'd')
    code += smode() + JNE(1, 0, g(p + 'ds'))
    code += comp() + JNE(1, 0, g(p + 'e')) + MOV(6, 1) + JMP(g(p + 'e'))   # dst/none: the dst PAN unless compressed
    lab(p + 'ds')
    code += dmode() + JNE(1, 0x0c, g(p + 'r')) + smode() + JNE(1, 0xc0, g(p + 'r'))
    code += comp() + JNE(1, 0, g(p + 'e')) + MOV(6, 1) + JMP(g(p + 'e'))   # ext/ext: the dst PAN unless compressed
    lab(p + 'r')
    code += MOV(6, 1) + comp() + JNE(1, 0, g(p + 'e')) + MOV(6, 3)          # the rest: dst PAN; src PAN unless compressed
    lab(p + 'e')


def build(base, enh=True, v2015=False):
    # two passes for labels
    for _ in range(2):
        code, L = [], getattr(build, 'L', {})
        def here(): return base + len(code)
        def lab(n): L[n] = here()
        g = lambda n: L.get(n, base)
        # ---- E1 counter, entered from phase 1 (0x3020 hook), then the replaced instruction, back
        lab('p1')
        code += LD(4, 0x3fd8, 0xff) + ADD(4, 1) + STr(4, 0x3fd8)    # 3FD8 ++ (8 bit; I is free in phase 1 here)
        code += ST(0x6901, 0x00)                                     # Imm-Ack FCF1 back to 0 for every frame
        code += ST(0x60e9, 0x03)                                     # replaced instruction (ack length 3)
        code += JMP(0x3024)
        # ---- the ack decision, entered from 0x3420 (was: ST [0x434],#4)
        lab('ack')
        code += MARK(0x50)
        code += LD(1, 0x6d02, 0x04) + JEQ(1, 0, g('imm'))           # not 2015: Imm-Ack as before
        code += LD(4, 0x3fda, 0xff) + ADD(4, 1) + STr(4, 0x3fda)     # 3FDA ++ : a 2015 frame wanting an ack
        code += LD(1, 0x6d1e, 0xc0)                                  # its source addressing mode
        code += STr(1, 0x3fdb)                                       # (for the host: last src mode)
        if not enh:
            code += JMP(g('noack'))                                  # variant ii-a: count, no ack
        code += JEQ(1, 0x80, g('short')) + JNE(1, 0xc0, g('noack'))  # none/reserved: no ack at all
        code += ST(0x0885, 0x69)                                     # dst page = PBM 7 data (src page is 0x6d)
        code += MOV(4, 0x12) + MOV(5, 0x03) + COPY(2)                # ack dst PAN  = rx src PAN (S.12)
        code += MOV(4, 0x14) + MOV(5, 0x05) + COPY(8)                # ack dst addr = rx src ext (S.14..1b)
        code += ST(0x6901, 0x2c) + ST(0x60e9, 13)                    # FCF1: ver 2, dst ext; length w/o FCS
        code += JMP(g('done'))
        lab('short')
        code += ST(0x0885, 0x69)
        code += MOV(4, 0x12) + MOV(5, 0x03) + COPY(2)
        code += MOV(4, 0x14) + MOV(5, 0x05) + COPY(2)
        code += ST(0x6901, 0x28) + ST(0x60e9, 7)                     # FCF1: ver 2, dst short
        lab('done')
        code += ST(0x0885, 0x6a)                                     # dst page back: 0x3424 on uses 0x6a
        lab('imm')
        code += ST(0x0434, 0x04)                                     # the replaced instruction: send the ack
        lab('noack')
        code += JMP(0x3424)
        if v2015:          # at its own base (build_2015): the gap after the filter has room for the ack only
            # ---- phase 1, entered from 0x30be (was: MARK 0x14; MOV I,#3): a 2015 header's length by its own rules
            lab('h1')
            code += MARK(0x14) + MOV(4, 3)
            code += LD(1, 0x6d02, 0x04) + JEQ(1, 0, 0x30c2)             # 2003/2006: the filter's own code
            code += MOV(6, 9) + dmode() + JEQ(1, 0x04, 0x348e)          # reserved modes: rejected as before
            code += MOV(6, 8) + smode() + JEQ(1, 0x40, 0x348e)
            # sequence number suppressed (FCF1 bit 0): the header one byte shorter, which neither parser follows;
            # Thread never sends it: dropped (as a frame version the filter does not take, reason 2)
            code += MOV(6, 2) + LD(1, FCF1, 0x01) + JNE(1, 0, 0x348e)
            pan_tree(code, lab, g, 'a')
            code += JEQ(6, 0, g('a_nd')) + JEQ(6, 2, g('a_nd')) + ADD(4, 2)
            lab('a_nd')
            code += JEQ(6, 0, g('a_ns')) + JEQ(6, 1, g('a_ns')) + ADD(4, 2)
            lab('a_ns')
            code += dmode() + JEQ(1, 0, g('a_da')) + ADD(4, 2) + JEQ(1, 0x08, g('a_da')) + ADD(4, 6)
            lab('a_da')
            code += smode() + JEQ(1, 0, g('a_sa')) + ADD(4, 2) + JEQ(1, 0x80, g('a_sa')) + ADD(4, 6)
            lab('a_sa')
            code += JMP(0x30fc)                                         # command id, security byte: as before
            # ---- phase 2, entered from 0x3132 (was: MARK 0x1e; LD I,[0x402]): a 2015 header's fields
            lab('h2')
            code += MARK(0x1e) + LD(4, 0x0402, 0xff)
            code += LD(1, 0x6d02, 0x04) + JEQ(1, 0, 0x3138)
            pan_tree(code, lab, g, 'b')
            code += ADD(4, 2) + MOV(5, 0x03) + COPY(1) + ADD(4, 1)       # seq -> S.03 (pages: frame -> 0x6d, phase 1's)
            code += LD(1, 0x6d03, 0xff) + STr(1, 0x6902)                 # the ACK's seq
            code += ST(0x6d06, 0xff) + ST(0x6d07, 0xff)                  # no dst PAN: as the broadcast PAN
            code += JEQ(6, 0, g('b_nd')) + JEQ(6, 2, g('b_nd')) + MOV(5, 0x06) + COPY(2) + ADD(4, 2)
            lab('b_nd')
            code += dmode() + JEQ(1, 0, g('b_da')) + MOV(5, 0x08) + JEQ(1, 0x08, g('b_dsh'))
            code += COPY(8) + ADD(4, 8) + JMP(g('b_da'))
            lab('b_dsh')
            code += COPY(2) + ADD(4, 2)
            lab('b_da')
            # the src PAN (the Enh-Ack's dst PAN, the data-pending match): its own, else the dst PAN, else stack 0's
            code += JEQ(6, 2, g('b_sp')) + JEQ(6, 3, g('b_sp'))
            code += JEQ(6, 1, g('b_spd'))
            code += LD(1, 0x6a1a, 0xff) + STr(1, 0x6d12) + LD(1, 0x6a1b, 0xff) + STr(1, 0x6d13) + JMP(g('b_spe'))
            lab('b_spd')
            code += LD(1, 0x6d06, 0xff) + STr(1, 0x6d12) + LD(1, 0x6d07, 0xff) + STr(1, 0x6d13) + JMP(g('b_spe'))
            lab('b_sp')
            code += MOV(5, 0x12) + COPY(2) + ADD(4, 2)
            lab('b_spe')
            code += smode() + JEQ(1, 0, g('b_sa')) + MOV(5, 0x14) + JEQ(1, 0x80, g('b_ssh'))
            code += COPY(8) + ADD(4, 8) + JMP(g('b_sa'))
            lab('b_ssh')
            code += COPY(2) + ADD(4, 2)
            lab('b_sa')
            code += ST(0x6d11, 0x01) + ST(0x6d05, 0x01)                  # as 0x3164/0x3168
            code += JMP(0x319a)                                          # addresses copied: on as before
            # ---- the frame pending bit, entered from 0x343e (was: LD A,[0x433] & 0x08): the filter looks the
            # sender up in the data-pending tables for a data request command only; Thread 1.2's enhanced frame
            # pending (a sleepy device's 2015 data frame, answered with an Enh-Ack) asks the same of data frames
            lab('h3')
            code += LD(1, 0x0433, 0x08) + JNE(1, 0, 0x3444)            # a data request: the filter's own way
            code += LD(1, 0x6d02, 0x04) + JEQ(1, 0, g('h3_no'))          # not 2015: as before (P = 0x3482, no lookup)
            code += LD(1, 0x6d00, 0x02) + JNE(1, 0, 0x3444)              # a 2015 data frame: looked up as a request
            lab('h3_no')
            code += MOV(1, 0) + JMP(0x3442)                              # A = 0: 0x3442's "J P if A == 0" skips it
        build.L = L
    return bytes(code), L

build.L = {}
ext0, L0 = build(0x34ac, enh=False, v2015=False)
open(os.path.join(HERE, 'seq_noack.bin'), 'wb').write(ext0)
build.L = {}
ext, L = build(0x34ac)
assert 0x34ac + len(ext) <= 0x3600, hex(len(ext))
open(os.path.join(HERE, 'seq_enhack.bin'), 'wb').write(ext)
# the 2015 parser at 0x2c00 (chip RAM nothing uses; the sequencer runs code from it: 2026-11-27, a counter there
# counted every frame), built by the same function with the ack's half left out
build.L = {}
_, L15 = build(0x2c00, enh=True, v2015=True)
skip = L15['h1'] - 0x2c00                       # the ack's half comes first: built so that h1 lands at 0x2c00
build.L = {}
code15, L15 = build(0x2c00 - skip, enh=True, v2015=True)
assert L15['h1'] == 0x2c00
code15 = code15[skip:]                          # never entered from here: dropped
assert 0x2c00 + len(code15) <= 0x2f00, hex(len(code15))
open(os.path.join(HERE, 'seq_2015.bin'), 'wb').write(code15)
hooks = {0x3020: bytes(JMP(L['p1'])), 0x3420: bytes(JMP(L['ack'])), 0x30be: bytes(JMP(L15['h1'])), 0x3132: bytes(JMP(L15['h2'])),
         0x343e: bytes(JMP(L15['h3']))}
HOOK_ORDER = (0x3020, 0x30be, 0x3132, 0x343e, 0x3420)
# the bytes each hook replaces (macfilt.bin, gp712_mac.ko .rodata+0x41c), for the undo
ORIGINAL = {0x3020: '3003e960', 0x3420: '30043404', 0x30be: '17141403', 0x3132: '171e24ff', 0x343e: '21083304'}
with open(os.path.join(HERE, 'seq_hooks.txt'), 'w') as f:
    for k in HOOK_ORDER:                                         # the ack's last: its frames parsed right first
        f.write('%x %s %s\n' % (k, hooks[k].hex(), ORIGINAL[k]))

print('rxhook.bin: %d bytes at 0x3f00..%#x; coex patch at 0x3842: 02 3f 00 (was 90 02 05)' % (len(hook), 0x3f00 + len(hook) - 1))
print('seq_noack.bin: %d bytes (hooks: same addresses, ack label %#x)' % (len(ext0), L0['ack']))
print('seq_enhack.bin: %d bytes at 0x34ac..%#x' % (len(ext), 0x34ac + len(ext) - 1))
for k, v in hooks.items():
    print('  hook %#06x: %s' % (k, v.hex(' ')))
print('  labels:', {k: hex(v) for k, v in L.items()})

# ---------------------------------------------------------------- the module's copy ----------------
# src/kmod/hassmic_gpfilter.h for hassmic_gptime (which applies it after every gpHal_Init): the extensions, the hooks
# with the bytes they replace, and the FNV-1a of the filter program as analysed (gp712_mac.ko .rodata+0x41c; the
# module patches nothing where the chip's program differs)
def fnv(b):
    h = 0x811c9dc5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xffffffff
    return h
FILTER_LEN, FILTER_FNV = 0x4ac, 0x1a6981cf   # of gp712_mac.ko's .rodata+0x41c (laser's firmware; never in git)
mf = os.path.join(HERE, 'macfilt.bin')       # where a copy is at hand: held to it
if os.path.exists(mf):
    assert fnv(open(mf, 'rb').read()) == FILTER_FNV
def carr(name, b):
    return 'static const u8 %s[%d] = {\n%s\n};\n' % (name, len(b), '\n'.join(
        '    ' + ' '.join('0x%02x,' % x for x in b[i:i + 16]) for i in range(0, len(b), 16)))
hdr = ['/* made by tools/gp712/mkseq.py: do not edit (docs/re-gp712.md "The MAC filter program", PLAN.md 2.1) */',
       '#define GPF_FILTER_AT 0x3000', '#define GPF_FILTER_LEN 0x%x' % FILTER_LEN, '#define GPF_FILTER_FNV 0x%08xu' % FILTER_FNV,
       '#define GPF_ACK_AT 0x34ac', carr('gpf_ack', ext), '#define GPF_P2015_AT 0x2c00', carr('gpf_p2015', code15),
       'static const struct { u16 at; u8 hook[4], orig[4]; } gpf_hooks[%d] = {' % len(HOOK_ORDER)]
for k in HOOK_ORDER:
    hdr.append('    { 0x%04x, { %s }, { %s } },' % (k, ', '.join('0x%02x' % x for x in hooks[k]),
                                                     ', '.join('0x%02x' % x for x in bytes.fromhex(ORIGINAL[k]))))
hdr.append('};')
open(os.path.join(HERE, '..', '..', 'src', 'kmod', 'hassmic_gpfilter.h'), 'w').write('\n'.join(hdr) + '\n')
