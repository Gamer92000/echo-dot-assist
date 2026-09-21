import struct, sys, lzma, bz2, os
def varint(b, i):
    r = 0; s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7f) << s; s += 7
        if not c & 0x80: return r, i
def fields(b):
    i = 0
    while i < len(b):
        k, i = varint(b, i); f, w = k >> 3, k & 7
        if w == 0: v, i = varint(b, i)
        elif w == 2:
            l, i = varint(b, i); v = b[i:i+l]; i += l
        elif w == 1: v = b[i:i+8]; i += 8
        elif w == 5: v = b[i:i+4]; i += 4
        else: raise Exception(w)
        yield f, v
f = open(sys.argv[1], 'rb'); out = sys.argv[2]
assert f.read(4) == b'CrAU'
ver, msz = struct.unpack('>QQ', f.read(16)); ssz = struct.unpack('>I', f.read(4))[0]
man = f.read(msz); f.read(ssz); base = f.tell()
bs = 4096
parts = []
for k, v in fields(man):
    if k == 3: bs = v
    if k == 13: parts.append(v)
for p in parts:
    name = None; ops = []
    for k, v in fields(p):
        if k == 1: name = v.decode()
        if k == 8: ops.append(v)
    print(name, len(ops)); 
    o = open(os.path.join(out, name + '.img'), 'wb')
    for op in ops:
        t = 0; off = 0; ln = 0; dst = []
        for k, v in fields(op):
            if k == 1: t = v
            elif k == 2: off = v
            elif k == 3: ln = v
            elif k == 6:
                d = dict(fields(v)); dst.append((d.get(1, 0), d.get(2, 0)))
        f.seek(base + off); data = f.read(ln)
        if t == 0: pass
        elif t == 1: data = bz2.decompress(data)
        elif t == 8: data = lzma.decompress(data)
        elif t == 6: data = b'\0' * (sum(n for _, n in dst) * bs)
        else: raise Exception('op type %d' % t)
        p0 = 0
        for s, n in dst:
            o.seek(s * bs); o.write(data[p0:p0 + n * bs]); p0 += n * bs
    o.close()
