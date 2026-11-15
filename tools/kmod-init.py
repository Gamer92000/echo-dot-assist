#!/usr/bin/env python3
"""Moves a 32-bit ARM module's init pointer in its struct module (.gnu.linkonce.this_module) to where the Echo's kernel
reads it, for a kernel whose struct module is not kernel.org's (Makefile, device.mk KMOD_INIT).

    tools/kmod-init.py <module.ko> <offset>

Cannoli's kernel is Android's common kernel: every stock module there has init_module at 0xfc and cleanup_module at
0x208 in a 0x240-byte struct, kernel.org's 5.4.86 with the same config puts init at 0xd8 and exit at 0x208, size
0x240: the 36 bytes Android adds come before init and are padding before the cacheline-aligned core_layout in ours.
Everything else the kernel takes from the module's copy (name at 12) is where it reads it; the rest is zeros, which
it fills itself.  The section's one relocation for init_module (R_ARM_ABS32) gets the new offset; the word there must
be zero and no other relocation may point at it.  Idempotent: a module already moved is left as it is.
"""
import struct, sys

def main():
    if len(sys.argv) != 3: sys.exit(__doc__)
    path, to = sys.argv[1], int(sys.argv[2], 0)
    d = bytearray(open(path, 'rb').read())
    if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1: sys.exit(f'{path}: not a 32-bit little-endian ELF')
    shoff, = struct.unpack_from('<I', d, 32); shnum, shstrndx = struct.unpack_from('<HH', d, 48)
    sh = [struct.unpack_from('<10I', d, shoff + 40 * i) for i in range(shnum)]
    def name(off, strsec): o = sh[strsec][4] + off; return bytes(d[o:d.index(b'\0', o)]).decode()
    secs = {name(s[0], shstrndx): (i, s) for i, s in enumerate(sh)}
    _, mod = secs['.gnu.linkonce.this_module']
    _, rel = secs['.rel.gnu.linkonce.this_module']
    if rel[1] != 9: sys.exit(f'{path}: .rel.gnu.linkonce.this_module is not SHT_REL')
    symtab = sh[rel[6]]                                 # sh_link: the relocations' symbol table
    if not to % 4 == 0 or to + 4 > mod[5]: sys.exit(f'{path}: {to:#x} outside struct module ({mod[5]:#x} bytes)')
    entries = [(rel[4] + 8 * i, *struct.unpack_from('<II', d, rel[4] + 8 * i)) for i in range(rel[5] // 8)]
    init = None
    for at, off, info in entries:
        st_name, = struct.unpack_from('<I', d, symtab[4] + 16 * (info >> 8))
        if name(st_name, symtab[6]) == 'init_module' and info & 0xff == 2: init = (at, off, info)
    if not init: sys.exit(f'{path}: no R_ARM_ABS32 to init_module in struct module')
    at, off, info = init
    if off == to: return
    if any(o == to for _, o, _ in entries): sys.exit(f'{path}: another relocation at {to:#x}')
    if d[mod[4] + to:mod[4] + to + 4] != b'\0' * 4: sys.exit(f'{path}: {to:#x} in struct module is not zero')
    # REL: the addend is the word at the place, zero for a symbol's own address; carried along
    d[mod[4] + to:mod[4] + to + 4] = d[mod[4] + off:mod[4] + off + 4]
    d[mod[4] + off:mod[4] + off + 4] = b'\0' * 4
    struct.pack_into('<I', d, at, to)
    open(path, 'wb').write(d)
    print(f'{path}: init_module {off:#x} -> {to:#x}')

if __name__ == '__main__':
    main()
