#!/usr/bin/env python3
"""Resolve pc-relative string refs in a Thumb function.
usage: fnstrings.py lib start_hex stop_hex   (addresses = vaddr == file offset for .text/.rodata)"""
import sys,struct,subprocess,re
lib,start,stop=sys.argv[1],int(sys.argv[2],16),int(sys.argv[3],16)
d=open(lib,'rb').read()
asm=subprocess.run(['llvm-objdump','-d','--no-show-raw-insn','--triple=thumbv7-linux-gnueabi',
  f'--start-address={start:#x}',f'--stop-address={stop:#x}',lib],capture_output=True,text=True).stdout
pend={}
for line in asm.splitlines():
    m=re.match(r'\s*([0-9a-f]+):\s+(\S+)\s+(.*)',line)
    if not m: continue
    a=int(m.group(1),16); op=m.group(2); args=m.group(3)
    m2=re.match(r'(r\d+), \[pc, #0x[0-9a-f]+\]\s+@ 0x([0-9a-f]+)',args)
    if op.startswith('ldr') and m2:
        pend[m2.group(1)]=int(m2.group(2),16); continue
    m3=re.match(r'(r\d+), pc',args)
    if op=='add' and m3 and m3.group(1) in pend:
        lit=pend.pop(m3.group(1)); off=struct.unpack('<I',d[lit:lit+4])[0]
        t=(off+a+4)&0xffffffff
        if t<len(d):
            e=d.find(b'\0',t); s=d[t:e]
            if 2<len(s)<200 and all(32<=c<127 for c in s): print(f'{a:#x}: {s.decode()}')
    
