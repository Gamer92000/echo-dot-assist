#!/usr/bin/env python3
"""Table-driven 8051 disassembler with recursive traversal.
usage: d51.py file base [entry ...]"""
import sys
SFR = {0x80:'P0',0x81:'SP',0x82:'DPL',0x83:'DPH',0x87:'PCON',0x88:'TCON',0x89:'TMOD',0x8a:'TL0',0x8b:'TL1',
 0x8c:'TH0',0x8d:'TH1',0x90:'P1',0x98:'SCON',0x99:'SBUF',0xa0:'P2',0xa8:'IE',0xb0:'P3',0xb8:'IP',
 0xd0:'PSW',0xe0:'ACC',0xf0:'B'}
def d(a): return SFR.get(a, '%02Xh'%a)
def bit(b):
    if b < 0x80: return '%02Xh.%d'%(0x20+(b>>3), b&7)
    return '%s.%d'%(d(b&0xf8), b&7)
# operand kinds: A,R0..,@R0,@R1, #imm, dir, bit, /bit, rel, addr16, addr11, #imm16, C, DPTR, @A+DPTR, @A+PC, AB, @DPTR
T = {}
def op(code, mn, *ops): T[code] = (mn, ops)
op(0x00,'NOP'); op(0x02,'LJMP','a16'); op(0x03,'RR','A'); op(0x04,'INC','A'); op(0x05,'INC','dir')
op(0x06,'INC','@R0'); op(0x07,'INC','@R1')
for r in range(8): op(0x08+r,'INC','R%d'%r); op(0x18+r,'DEC','R%d'%r)
op(0x10,'JBC','bit','rel'); op(0x12,'LCALL','a16'); op(0x13,'RRC','A'); op(0x14,'DEC','A'); op(0x15,'DEC','dir')
op(0x16,'DEC','@R0'); op(0x17,'DEC','@R1'); op(0x20,'JB','bit','rel'); op(0x22,'RET'); op(0x23,'RL','A')
op(0x24,'ADD','A','#'); op(0x25,'ADD','A','dir'); op(0x26,'ADD','A','@R0'); op(0x27,'ADD','A','@R1')
for r in range(8):
    op(0x28+r,'ADD','A','R%d'%r); op(0x38+r,'ADDC','A','R%d'%r); op(0x48+r,'ORL','A','R%d'%r)
    op(0x58+r,'ANL','A','R%d'%r); op(0x68+r,'XRL','A','R%d'%r); op(0x78+r,'MOV','R%d'%r,'#')
    op(0x88+r,'MOV','dir','R%d'%r); op(0x98+r,'SUBB','A','R%d'%r); op(0xa8+r,'MOV','R%d'%r,'dir')
    op(0xb8+r,'CJNE','R%d'%r,'#','rel'); op(0xc8+r,'XCH','A','R%d'%r); op(0xd8+r,'DJNZ','R%d'%r,'rel')
    op(0xe8+r,'MOV','A','R%d'%r); op(0xf8+r,'MOV','R%d'%r,'A')
op(0x30,'JNB','bit','rel'); op(0x32,'RETI'); op(0x33,'RLC','A'); op(0x34,'ADDC','A','#'); op(0x35,'ADDC','A','dir')
op(0x36,'ADDC','A','@R0'); op(0x37,'ADDC','A','@R1'); op(0x40,'JC','rel'); op(0x42,'ORL','dir','A')
op(0x43,'ORL','dir','#'); op(0x44,'ORL','A','#'); op(0x45,'ORL','A','dir'); op(0x46,'ORL','A','@R0'); op(0x47,'ORL','A','@R1')
op(0x50,'JNC','rel'); op(0x52,'ANL','dir','A'); op(0x53,'ANL','dir','#'); op(0x54,'ANL','A','#'); op(0x55,'ANL','A','dir')
op(0x56,'ANL','A','@R0'); op(0x57,'ANL','A','@R1'); op(0x60,'JZ','rel'); op(0x62,'XRL','dir','A'); op(0x63,'XRL','dir','#')
op(0x64,'XRL','A','#'); op(0x65,'XRL','A','dir'); op(0x66,'XRL','A','@R0'); op(0x67,'XRL','A','@R1'); op(0x70,'JNZ','rel')
op(0x72,'ORL','C','bit'); op(0x73,'JMP','@A+DPTR'); op(0x74,'MOV','A','#'); op(0x75,'MOV','dir','#'); op(0x76,'MOV','@R0','#')
op(0x77,'MOV','@R1','#'); op(0x80,'SJMP','rel'); op(0x82,'ANL','C','bit'); op(0x83,'MOVC','A','@A+PC'); op(0x84,'DIV','AB')
op(0x85,'MOV','dir1','dir2'); op(0x86,'MOV','dir','@R0'); op(0x87,'MOV','dir','@R1'); op(0x90,'MOV','DPTR','#16')
op(0x92,'MOV','bit','C'); op(0x93,'MOVC','A','@A+DPTR'); op(0x94,'SUBB','A','#'); op(0x95,'SUBB','A','dir')
op(0x96,'SUBB','A','@R0'); op(0x97,'SUBB','A','@R1'); op(0xa0,'ORL','C','/bit'); op(0xa2,'MOV','C','bit'); op(0xa3,'INC','DPTR')
op(0xa4,'MUL','AB'); op(0xa5,'???'); op(0xa6,'MOV','@R0','dir'); op(0xa7,'MOV','@R1','dir'); op(0xb0,'ANL','C','/bit')
op(0xb2,'CPL','bit'); op(0xb3,'CPL','C'); op(0xb4,'CJNE','A','#','rel'); op(0xb5,'CJNE','A','dir','rel')
op(0xb6,'CJNE','@R0','#','rel'); op(0xb7,'CJNE','@R1','#','rel'); op(0xc0,'PUSH','dir'); op(0xc2,'CLR','bit'); op(0xc3,'CLR','C')
op(0xc4,'SWAP','A'); op(0xc5,'XCH','A','dir'); op(0xc6,'XCH','A','@R0'); op(0xc7,'XCH','A','@R1'); op(0xd0,'POP','dir')
op(0xd2,'SETB','bit'); op(0xd3,'SETB','C'); op(0xd4,'DA','A'); op(0xd5,'DJNZ','dir','rel'); op(0xd6,'XCHD','A','@R0')
op(0xd7,'XCHD','A','@R1'); op(0xe0,'MOVX','A','@DPTR'); op(0xe2,'MOVX','A','@R0'); op(0xe3,'MOVX','A','@R1'); op(0xe4,'CLR','A')
op(0xe5,'MOV','A','dir'); op(0xe6,'MOV','A','@R0'); op(0xe7,'MOV','A','@R1'); op(0xf0,'MOVX','@DPTR','A'); op(0xf2,'MOVX','@R0','A')
op(0xf3,'MOVX','@R1','A'); op(0xf4,'CPL','A'); op(0xf5,'MOV','dir','A'); op(0xf6,'MOV','@R0','A'); op(0xf7,'MOV','@R1','A')
for p in range(8):
    op(0x01+p*0x20,'AJMP','a11'); op(0x11+p*0x20,'ACALL','a11')
def length(c):
    mn, ops = T[c]; n = 1
    for o in ops:
        if o in ('a16','#16'): n += 2
        elif o in ('dir','#','bit','/bit','rel','a11','dir1','dir2'): n += 1
    return n
def dis(buf, base, pc):
    c = buf[pc-base]; mn, ops = T[c]; n = length(c); b = buf[pc-base:pc-base+n]
    i = 1; out = []; tgt = []
    dirs = []
    for o in ops:
        if o == 'a16': v = b[i]<<8|b[i+1]; i += 2; out.append('%04Xh'%v); tgt.append(v)
        elif o == '#16': v = b[i]<<8|b[i+1]; i += 2; out.append('#%04Xh'%v)
        elif o == '#': out.append('#%02Xh'%b[i]); i += 1
        elif o in ('dir',): out.append(d(b[i])); i += 1
        elif o in ('dir1','dir2'): i += 1
        elif o == 'bit': out.append(bit(b[i])); i += 1
        elif o == '/bit': out.append('/'+bit(b[i])); i += 1
        elif o == 'rel': r = b[i]; r = r-256 if r>127 else r; i += 1; v = pc+n+r; out.append('%04Xh'%v); tgt.append(v)
        elif o == 'a11': v = ((pc+n)&0xf800)|((c>>5)<<8)|b[i]; i += 1; out.append('%04Xh'%v); tgt.append(v)
        else: out.append(o)
    if c == 0x85: out = [d(b[2]), d(b[1])]
    return n, b, mn, out, tgt
def main():
    f, base = sys.argv[1], int(sys.argv[2], 0)
    buf = open(f,'rb').read(); end = base+len(buf)
    entries = [int(x,0) for x in sys.argv[3:]] or [base]
    seen = {}; work = list(entries); labels = set(entries); calls = set(); ext = {}
    while work:
        pc = work.pop()
        while base <= pc < end and pc not in seen:
            n, b, mn, out, tgt = dis(buf, base, pc); seen[pc] = n
            for t in tgt:
                labels.add(t)
                if base <= t < end: work.append(t)
                else: ext.setdefault(t, []).append(pc)
                if mn in ('LCALL','ACALL'): calls.add(t)
            if mn in ('LJMP','AJMP','SJMP','RET','RETI','JMP'): break
            pc += n
    pc = base
    while pc < end:
        if pc in seen:
            n, b, mn, out, tgt = dis(buf, base, pc)
            lab = ('\n%s_%04X:'%('sub' if pc in calls else 'L', pc)) if pc in labels else ''
            if lab: print(lab)
            print('  %04X: %-9s %-6s %s'%(pc, ' '.join('%02X'%x for x in b), mn, ', '.join(out)))
            pc += n
        else:
            s = pc
            while pc < end and pc not in seen: pc += 1
            data = buf[s-base:pc-base]
            for k in range(0, len(data), 16):
                print('  %04X: db %s'%(s+k, ' '.join('%02X'%x for x in data[k:k+16])))
    print('\n; targets outside the blob:')
    for t in sorted(ext): print(';  %04X from %s'%(t, ' '.join('%04X'%x for x in ext[t])))
main()
