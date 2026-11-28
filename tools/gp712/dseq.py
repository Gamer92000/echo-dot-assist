#!/usr/bin/env python3
"""Disassembler for the GP712 (k7c) sequence processor ("seqproc") bytecode: the MAC filter program
macfilt.bin (gpHal_MacFilter_RamData, loaded at 0x3000).  ISA reconstructed from the program itself
(see macfilt.lst's head for the evidence per opcode).  Usage: dseq.py macfilt.bin [base]"""
import sys

NAMES = {
    0x0012: 'plme.0x12 (stacks on ch?)', 0x000d: 'plme.0x0d',
    0x0402: 'rib.rx_frame_ptr_lo', 0x0403: 'rib.rx_frame_ptr_hi', 0x0404: 'rib.0x404',
    0x0430: 'rib.0x430', 0x0431: 'rib.0x431', 0x0432: 'rib.0x432', 0x0433: 'rib.0x433(ftype bits)',
    0x0434: 'rib.0x434 (filter result/cmd)', 0x0435: 'rib.0x435 (reject reason)',
    0x0436: 'rib.0x436', 0x0437: 'rib.0x437', 0x0438: 'rib.0x438 (event)', 0x0439: 'rib.0x439',
    0x043b: 'rib.0x43b',
    0x0883: 'seqdma.src_page', 0x0885: 'seqdma.dst_page', 0x0889: 'seqdma.0x889 (MTCH latch?)',
    0x6900: 'pbm7[0] ackFCF0', 0x6901: 'pbm7[1] ackFCF1', 0x6902: 'pbm7[2] ackSeq',
    0x60e9: 'pbm7opt+9', 0x60eb: 'pbm7opt+0xb', 0x60ec: 'pbm7opt+0xc', 0x60f2: 'pbm7opt+0x12',
    0x6d00: 'S.ftype1hot', 0x6d02: 'S.fver1hot', 0x6d03: 'S.seq', 0x6d04: 'S.dst.stk',
    0x6d05: 'S.dst.?', 0x6d06: 'S.dstPAN', 0x6d07: 'S.dstPAN+1', 0x6d08: 'S.dstAddr',
    0x6d10: 'S.src.stk', 0x6d11: 'S.src.?', 0x6d12: 'S.srcPAN', 0x6d14: 'S.srcAddr',
    0x6d1c: 'S.cmdId/byte', 0x6d1d: 'S.FCF0', 0x6d1e: 'S.FCF1', 0x6d1f: 'S.0x1f', 0x6d20: 'S.0x20',
    0x6d22: 'S.0x22', 0x6d23: 'S.0x23',
    0x6a2a: 'mf.0x2a', 0x6a2b: 'mf.dp_short_n', 0x6a2c: 'mf.dp_long_n', 0x6a2d: 'mf.0x2d(b0 fpdflt,b1 promisc)',
    0x6a2e: 'mf.0x2e', 0x6a2f: 'mf.ftype_ok', 0x6a30: 'mf.fver_ok', 0x6a31: 'mf.0x31', 0x6a32: 'mf.0x32 (addr recog)',
    0x6a33: 'mf.0x33', 0x6a34: 'mf.0x34', 0x6a35: 'mf.0x35', 0x6a36: 'mf.bcast(ffff)',
}

def nm(a):
    n = NAMES.get(a)
    return '[%#06x %s]' % (a, n) if n else '[%#06x]' % a

R = {0: 'z', 1: 'A', 2: 'Plo', 3: 'Phi', 4: 'I', 5: 'J', 6: 'C', 7: 'M'}

def dis(b, base):
    pc = 0
    out = []
    while pc < len(b):
        op = b[pc]
        hi, lo = op >> 4, op & 0xf
        def a16(o):
            return b[pc + o] | (b[pc + o + 1] << 8)
        r = R.get(lo & 7, 'r%d' % (lo & 7))
        if hi == 0:
            n, txt = 2, 'END                       ; %02x: back to idle / wait for next event' % b[pc + 1]
        elif hi == 1:
            n = 2
            if lo == 7:
                txt = 'MARK %#04x' % b[pc + 1]
            else:
                txt = 'MOV  %s, #%#04x' % (r, b[pc + 1])
        elif hi == 2:
            n = 4
            txt = 'LD   %s, %s & %#04x' % (r, nm(a16(2)), b[pc + 1])
        elif hi == 3:
            n = 4
            v = b[pc + 1]
            if lo == 0:
                txt = 'ST   %s, #%#04x' % (nm(a16(2)), v)
            else:
                txt = 'ST   %s, %s%s' % (nm(a16(2)), r, (' | %#04x' % v) if v else '')
        elif hi in (4, 5):
            cc = '==' if hi == 4 else '!='
            if lo & 8:
                n = 2
                tgt = 'P'
            else:
                n = 4
                tgt = '%#06x' % a16(2)
            if (lo & 7) == 0 and hi == 4:
                txt = 'JMP  %s' % tgt
            else:
                txt = 'J    %s if %s %s %#04x' % (tgt, r, cc, b[pc + 1])
        elif hi == 6:
            n = 2
            txt = 'ADD  %s, #%#04x' % (r, b[pc + 1])
        elif hi == 7:
            n = 4
            txt = 'CMP  %s = (src[%#04x..+%d] == dst[%#04x..])' % (r, b[pc + 2], b[pc + 1] + 1, b[pc + 3])
        elif hi == 8:
            n = 2
            txt = 'COPY dst[J] <- src[I], %d bytes' % (b[pc + 1] + 1)
        elif hi == 0xc:
            n = 4
            txt = 'TSTM %s = src[%#04x] & dst[%#04x]  (%#04x)' % (r, b[pc + 2], b[pc + 3], b[pc + 1])
        elif hi == 0xd:
            n = 4
            txt = 'MTCH %s = match(src[%#04x], dst[%#04x], mode %#04x)' % (r, b[pc + 2], b[pc + 3], b[pc + 1])
        else:
            n, txt = 1, '??? %02x' % op
        raw = ' '.join('%02x' % x for x in b[pc:pc + n])
        out.append((base + pc, raw, txt))
        pc += n
    return out

if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x3000
    for a, raw, txt in dis(data, base):
        print('%04x: %-12s %s' % (a, raw, txt))
