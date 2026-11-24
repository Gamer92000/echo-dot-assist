# Unpacks an A/B update payload into one image per partition:
#   payload_dump.py <payload.bin | Amazon's OTA .bin> <output folder>
# Amazon's OTA download (update-kindle-<device>-...bin) is a zip with payload.bin inside, stored
# uncompressed (issue #221: the zip itself fails the CrAU check), so it is read in place.
import struct, sys, lzma, bz2, os, zipfile
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
if len(sys.argv) != 3: sys.exit('usage: payload_dump.py <payload.bin | OTA .bin> <output folder>')
f = open(sys.argv[1], 'rb'); out = sys.argv[2]
start = 0
if f.read(4) == b'PK\x03\x04':
    z = zipfile.ZipFile(f)
    if 'payload.bin' not in z.namelist(): sys.exit(sys.argv[1] + ': a zip without payload.bin')
    i = z.getinfo('payload.bin')
    if i.compress_type != zipfile.ZIP_STORED: sys.exit(sys.argv[1] + ': payload.bin is compressed; unzip it first')
    f.seek(i.header_offset); h = f.read(30)
    start = i.header_offset + 30 + sum(struct.unpack('<HH', h[26:30]))
f.seek(start)
if f.read(4) != b'CrAU': sys.exit(sys.argv[1] + ': not an update payload (no CrAU), nor an OTA zip with one')
os.makedirs(out, exist_ok=True)
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
