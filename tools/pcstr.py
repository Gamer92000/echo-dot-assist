#!/usr/bin/env python3
# usage: pcstr.py lib literal_addr add_pc_addr   -> resolves thumb "ldr rX,[pc,#lit]; add rX,pc" string
import sys,struct
d=open(sys.argv[1],'rb').read()
for i in range(2,len(sys.argv),2):
    lit=int(sys.argv[i],16); pc=int(sys.argv[i+1],16)
    off=struct.unpack('<I',d[lit:lit+4])[0]; a=(off+pc+4)&0xffffffff
    fo=a
    e=d.find(b'\0',fo); print(hex(a), d[fo:e][:120])
