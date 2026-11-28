#!/usr/bin/env python3
"""Small test programs for the GP712's MAC filter (the sequencer), on mkseq.py's ISA, for tools/gp712's tests; nothing
here goes into the product's patch (src/kmod/hassmic_gpfilter.h is mkseq.py's alone)."""
from mkseq import ST, STr, LD, MOV, ADD, JEQ, JNE, JMP, MARK   # noqa: F401  (mkseq.py writes its files on import)

FCF0 = 0x6d1d


def ack_counter(at=0x3540):
    """the ack frames that reach the filter program, counted where phase 1 has copied the FCF (0x306e, was MARK 0x0a;
    MOV Plo,#0x96): 16-bit counters at 0x3fdc (every ack) and 0x3fde (acks with the security bit).  Returns the code's
    place, its bytes, the hook's place and the bytes the hook replaces (for the undo)."""
    hook, orig = 0x306e, "170a1296"
    def build(labels):
        code = []
        def here(): return at + len(code)
        def lab(n): labels[n] = here()
        g = lambda n: labels.get(n, at)
        code += LD(1, FCF0, 0x07) + JNE(1, 0x02, g('out'))                     # an ack?
        code += LD(4, 0x3fdc, 0xff) + ADD(4, 1) + STr(4, 0x3fdc) + JNE(4, 0, g('sec'))
        code += LD(4, 0x3fdd, 0xff) + ADD(4, 1) + STr(4, 0x3fdd)
        lab('sec')
        code += LD(1, FCF0, 0x08) + JEQ(1, 0, g('out'))                         # with the security bit?
        code += LD(4, 0x3fde, 0xff) + ADD(4, 1) + STr(4, 0x3fde) + JNE(4, 0, g('out'))
        code += LD(4, 0x3fdf, 0xff) + ADD(4, 1) + STr(4, 0x3fdf)
        lab('out')
        code += MARK(0x0a) + MOV(2, 0x96) + JMP(0x3072)                        # the replaced instructions, back
        return code
    labels = {}
    build(labels)
    code = bytes(build(labels))
    assert at + len(code) <= 0x3600     # the gap after the patch's ack part (0x34ac..0x3525), before the 8051's program
    return at, code, hook, orig


def ack_counters():
    """ack_counter's count at two more places of the filter program (both do not fit the gap beside it), for the acks
    and the acks with the security bit: phase 2's start (0x312a, was LD A,[0x6d22]:
    0x3fd0 all, 0x3fd2 secured) and the ack's match (0x3402, was ST [0x434],#1: 0x3fd4 all, 0x3fd6 secured); 16-bit
    counters.  Returns [(code's place, bytes, hook's place, the bytes it replaces), ...]."""
    def counter(at, cnt, tail):
        def build(labels):
            code = []
            def lab(n): labels[n] = at + len(code)
            g = lambda n: labels.get(n, at)
            code += LD(1, FCF0, 0x07) + JNE(1, 0x02, g('out'))
            code += LD(4, cnt, 0xff) + ADD(4, 1) + STr(4, cnt) + JNE(4, 0, g('sec'))
            code += LD(4, cnt + 1, 0xff) + ADD(4, 1) + STr(4, cnt + 1)
            lab('sec')
            code += LD(1, FCF0, 0x08) + JEQ(1, 0, g('out'))
            code += LD(4, cnt + 2, 0xff) + ADD(4, 1) + STr(4, cnt + 2) + JNE(4, 0, g('out'))
            code += LD(4, cnt + 3, 0xff) + ADD(4, 1) + STr(4, cnt + 3)
            lab('out')
            code += tail
            return code
        labels = {}
        build(labels)
        return bytes(build(labels))
    out, at = [], 0x3530
    for hook, orig, cnt, tail in ((0x312a, "21ff226d", 0x3fd0, LD(1, 0x6d22, 0xff) + JMP(0x312e)),
                                  (0x3402, "30013404", 0x3fd4, ST(0x0434, 0x01) + JMP(0x3406))):
        code = counter(at, cnt, tail)
        out.append((at, code, hook, orig))
        at += len(code)
    assert at <= 0x3600
    return out
