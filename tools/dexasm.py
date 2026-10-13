"""The machinery behind the hand-assembled dex files of scripts/device/ (tools/mkwifictl.py -> wifictl.dex,
tools/mkringbar.py -> ringbar.dex): there is no javac or d8 on the PC that builds this repository, and a one-class
program that reaches the framework through reflection is small enough to write as Dalvik bytecode.

    Code            one static method's instructions (the subset both programs use)
    Program         what a program is made of: its class, its own methods, the java.* methods and fields it
                    calls directly (METHODS/FIELDS below, shared), the framework names its reflection asks for
    build()         the dex file: id tables sorted as the format wants, one public class of static methods
    read()          parses a dex back, checks checksum, signature, map and order, decodes every instruction and
                    runs verify() over every method (what ART's method verifier would refuse of this subset)
    check_framework the classes, methods and fields the dex names, and the ones its reflection asks for, looked
                    up in the firmware's own boot*.oat (--framework=<rootfs>)
    cli()           the tools' common command line: rebuild, --check, --framework=

Every program references java.* classes alone, so ART has nothing to resolve but the platform, and the dex loads
on any Android 7.  tests/unit/dexrun.py interprets what read() returns, against fakes of the framework."""
import hashlib
import os
import struct
import sys
import zlib

# ------------------------------------------------------------------ Dalvik assembler
class Code:
    """One static method's instructions: 16-bit units, labels patched at the end.  Only what the programs need:
    no *-wide, no try/catch, no registers above 15 where a format asks for nibbles.  assemble() leaves the
    instruction list alone, so it can run twice: once to collect the ids, once to resolve them.
    Registers are written as the methods below name them, parameters first (v0 = the first parameter); Dalvik
    puts the ins in the last registers of the frame, so every operand goes through _r(): logical v -> physical
    (v - ins) mod nregs."""
    def __init__(self, nregs, ins):
        self.nregs, self.ins = nregs, ins
        self.units, self.labels, self.branches, self.outs = [], {}, [], 0

    def _r(self, v, bits=8):
        assert 0 <= v < self.nregs, f"v{v} outside a frame of {self.nregs}"
        r = (v - self.ins) % self.nregs
        assert r < (1 << bits), f"v{v} (physical v{r}) does not fit {bits} bits"
        return r

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = len(self.units)

    def _branch(self, units, name):                         # the offset goes into the last unit
        self.units += units
        self.branches.append((len(self.units) - 1, name))

    def const_string(self, v, s):                           # 21c
        self.units += [(self._r(v) << 8) | 0x1a, ("str", s)]

    def const_4(self, v, n):                                # 11n, signed 4-bit literal
        assert -8 <= n <= 7
        self.units += [((n & 0xf) << 12) | (self._r(v, 4) << 8) | 0x12]

    def const_16(self, v, n):                               # 21s
        self.units += [(self._r(v) << 8) | 0x13, n & 0xffff]

    def const(self, v, n):                                  # 31i: any 32-bit literal
        assert -(1 << 31) <= n < (1 << 31)
        self.units += [(self._r(v) << 8) | 0x14, n & 0xffff, (n >> 16) & 0xffff]

    def lit(self, v, n):                                    # the shortest const for n
        if -8 <= n <= 7: self.const_4(v, n)
        elif -32768 <= n <= 32767: self.const_16(v, n)
        else: self.const(v, n)

    def move(self, a, b, obj=False):                        # 12x
        self.units += [(self._r(b, 4) << 12) | (self._r(a, 4) << 8) | (0x07 if obj else 0x01)]

    def move_result(self, v, obj=False):                    # 11x
        self.units += [(self._r(v) << 8) | (0x0c if obj else 0x0a)]

    def check_cast(self, v, t):                             # 21c
        self.units += [(self._r(v) << 8) | 0x1f, ("type", t)]

    def new_instance(self, v, t):                           # 21c
        self.units += [(self._r(v) << 8) | 0x22, ("type", t)]

    def new_array(self, a, b, t):                           # 22c: a = new t[b]
        self.units += [(self._r(b, 4) << 12) | (self._r(a, 4) << 8) | 0x23, ("type", t)]

    def array_length(self, a, b):                           # 12x
        self.units += [(self._r(b, 4) << 12) | (self._r(a, 4) << 8) | 0x21]

    def aget(self, a, b, c, op):                            # 23x: a = b[c], c a register
        self.units += [(self._r(a) << 8) | op, (self._r(c) << 8) | self._r(b)]

    def aget_at(self, a, b, k):                             # a = b[k], k a literal (a holds it first)
        self.const_4(a, k)
        self.aget(a, b, a, AGET_OBJ)

    def aput(self, a, b, c, op):                            # 23x: b[c] = a
        self.units += [(self._r(a) << 8) | op, (self._r(c) << 8) | self._r(b)]

    def goto_(self, name):                                  # 20t: goto/16
        self._branch([0x0029, 0], name)

    def if2(self, op, a, b, name):                          # 22t: if-eq .. if-le
        assert 0x32 <= op <= 0x37
        self._branch([(self._r(b, 4) << 12) | (self._r(a, 4) << 8) | op, 0], name)

    def ifz(self, op, a, name):                             # 21t: if-eqz .. if-lez
        assert 0x38 <= op <= 0x3d
        self._branch([(self._r(a) << 8) | op, 0], name)

    def invoke(self, op, m, regs):                          # 35c: 0-4 argument registers
        assert 0 <= len(regs) <= 4
        self.outs = max(self.outs, len(regs))
        c, d, e, f = ([self._r(r, 4) for r in regs] + [0] * 4)[:4]
        self.units += [(len(regs) << 12) | op, ("method", m), c | (d << 4) | (e << 8) | (f << 12)]

    def invoke_range(self, op, m, first, n):                # 3rc: n consecutive registers from first
        self.outs = max(self.outs, n)
        r = [self._r(first + i, 16) for i in range(n)]
        assert r == list(range(r[0], r[0] + n)), "a range must not wrap past the parameters"
        self.units += [(n << 8) | op, ("method", m), r[0]]

    def sget_obj(self, v, f):                               # 21c: sget-object
        self.units += [(self._r(v) << 8) | 0x62, ("field", f)]

    def binop(self, op, a, b, c):                           # 23x: a = b <op> c
        self.units += [(self._r(a) << 8) | op, (self._r(c) << 8) | self._r(b)]

    def binop_lit8(self, op, a, b, n):                      # 22b: a = b <op> n; AA|op, CC|BB (literal high)
        assert -128 <= n <= 127
        self.units += [(self._r(a) << 8) | op, ((n & 0xff) << 8) | self._r(b)]

    def int_to_byte(self, a, b):                            # 12x
        self.units += [(self._r(b, 4) << 12) | (self._r(a, 4) << 8) | 0x8d]

    def ret_void(self):
        self.units += [0x0e]

    def ret(self, v, obj):                                  # 11x
        self.units += [(self._r(v) << 8) | (0x11 if obj else 0x0f)]

    def assemble(self, tables):
        units = list(self.units)
        for at, name in self.branches:
            off = self.labels[name] - (at - 1)              # relative to the branch instruction itself
            assert -32768 <= off <= 32767, f"branch to {name} out of range"
            units[at] = off & 0xffff
        return struct.pack("<HHHHII", self.nregs, self.ins, self.outs, 0, 0, len(units)) + \
            b"".join(struct.pack("<H", u if isinstance(u, int) else tables.index(u)) for u in units)


AGET_OBJ, APUT_OBJ, APUT_BYTE = 0x46, 0x4d, 0x4f
VIRT, DIRECT, STATIC, IFACE, STATIC_RANGE = 0x6e, 0x70, 0x71, 0x72, 0x77
ADD, SUB, MUL, AND, OR, SHL = 0x90, 0x91, 0x92, 0x95, 0x96, 0x98
ADD8, MUL8, OR8, SHL8, USHR8 = 0xd8, 0xda, 0xde, 0xe0, 0xe2
IFNE, IFGE, IFLT, IFEQZ, IFNEZ, IFLTZ, IFGTZ = 0x33, 0x35, 0x34, 0x38, 0x39, 0x3a, 0x3c

J = "Ljava/lang/"
SB, STR = J + "StringBuilder;", J + "String;"
CLS, OBJ, MTH, FLD = J + "Class;", J + "Object;", J + "reflect/Method;", J + "reflect/Field;"
CTOR = J + "reflect/Constructor;"
ITR, LST, BST = "Ljava/util/Iterator;", "Ljava/util/List;", "Ljava/util/BitSet;"
INTG, BOOL, FLT, SYS = J + "Integer;", J + "Boolean;", J + "Float;", J + "System;"
PSTRM, ISTRM = "Ljava/io/PrintStream;", "Ljava/io/InputStream;"
BBUF, IBUF, BORD = "Ljava/nio/ByteBuffer;", "Ljava/nio/IntBuffer;", "Ljava/nio/ByteOrder;"
CLSARR, STRARR, OBJARR = "[Ljava/lang/Class;", "[Ljava/lang/String;", "[Ljava/lang/Object;"
BYTES, INTS = "[B", "[I"

# every java.* method the programs call directly: key -> (class, method, (params, return)).  Only what a
# program's code names goes into its dex, so one table serves all of them (and tests/unit/dexrun.py runs them by key).
METHODS = {
    "SB.new":     (SB, "<init>", ((), "V")),
    "SB.appendO": (SB, "append", ((OBJ,), SB)),
    "SB.appendI": (SB, "append", (("I",), SB)),
    "SB.to":      (SB, "toString", ((), STR)),
    "println":    (PSTRM, "println", ((STR,), "V")),
    "str.eq":     (STR, "equals", ((OBJ,), "Z")),
    "str.new_b":  (STR, "<init>", ((BYTES,), "V")),
    "str.charat": (STR, "charAt", (("I",), "C")),
    "str.length": (STR, "length", ((), "I")),
    "str.valueO": (STR, "valueOf", ((OBJ,), STR)),
    "int.parse":  (INTG, "parseInt", ((STR,), "I")),
    "int.value":  (INTG, "valueOf", (("I",), INTG)),
    "int.intval": (INTG, "intValue", ((), "I")),
    "bool.value": (BOOL, "valueOf", (("Z",), BOOL)),
    "cls.forName": (CLS, "forName", ((STR,), CLS)),
    "cls.newi":   (CLS, "newInstance", ((), OBJ)),
    "cls.getm":   (CLS, "getMethod", ((STR, CLSARR), MTH)),
    "cls.getf":   (CLS, "getField", ((STR,), FLD)),
    "obj.getcls": (OBJ, "getClass", ((), CLS)),
    "obj.tostr":  (OBJ, "toString", ((), STR)),
    "obj.eq":     (OBJ, "equals", ((OBJ,), "Z")),
    "m.invoke":   (MTH, "invoke", ((OBJ, OBJARR), OBJ)),
    "m.setacc":   (MTH, "setAccessible", (("Z",), "V")),
    "f.get":      (FLD, "get", ((OBJ,), OBJ)),
    "f.set":      (FLD, "set", ((OBJ, OBJ), "V")),
    "it.has":     (ITR, "hasNext", ((), "Z")),
    "it.next":    (ITR, "next", ((), OBJ)),
    "list.iter":  (LST, "iterator", ((), ITR)),
    "bits.set":   (BST, "set", (("I",), "V")),
    "sys.exit":   (SYS, "exit", (("I",), "V")),
    "flush":      (PSTRM, "flush", ((), "V")),
    "in.read":    (ISTRM, "read", ((BYTES, "I", "I"), "I")),
    "float.value": (FLT, "valueOf", ((STR,), FLT)),
    "cls.getc":   (CLS, "getConstructor", ((CLSARR,), CTOR)),
    "ctor.newi":  (CTOR, "newInstance", ((OBJARR,), OBJ)),
    "bb.wrap":    (BBUF, "wrap", ((BYTES,), BBUF)),
    "bb.order":   (BBUF, "order", ((BORD,), BBUF)),
    "bb.asint":   (BBUF, "asIntBuffer", ((), IBUF)),
    "ib.get":     (IBUF, "get", ((INTS,), IBUF)),
}
FIELDS = {
    "sys.out": (SYS, "out", PSTRM),
    "sys.err": (SYS, "err", PSTRM),
    "sys.in":  (SYS, "in", ISTRM),
    "bool.T":  (BOOL, "TRUE", BOOL),
    "bool.F":  (BOOL, "FALSE", BOOL),
    "int.TYPE": (INTG, "TYPE", CLS),
    "bool.TYPE": (BOOL, "TYPE", CLS),
    "float.TYPE": (FLT, "TYPE", CLS),
    "order.LE": (BORD, "LITTLE_ENDIAN", BORD),
}


class Program:
    """One program: its class (me), its own static methods as (name, params, return), the java.* methods and
    fields its code calls by key (METHODS/FIELDS unless given), and what its reflection asks the framework for,
    checked by --framework: reflected (class, method, params, static) with "<init>" for a constructor (found
    in that class alone, as getConstructor and Class.newInstance do), reflected_fields (class, name, type), and
    constants (class, static int field, the value the code has written in)."""
    def __init__(self, me, own, methods=None, fields=None, reflected=(), reflected_fields=(), constants=()):
        self.me, self.own = me, list(own)
        self.methods, self.fields = methods or METHODS, fields or FIELDS
        self.reflected, self.reflected_fields, self.constants = list(reflected), list(reflected_fields), list(constants)
        self.own_by_name = {n: (tuple(ps), r) for n, ps, r in self.own}
        assert not set(self.own_by_name) & set(self.methods), "a method of ours named like a java.* key"


def shorty(p):
    def one(t):
        return t[0] if t[0] in "VZBSCIF" else "L"
    return one(p[1]) + "".join(one(t) for t in p[0])


def method_id(prog, key):
    """(class, name, params, return) of a java.* method by its key, or of one of the program's own by name"""
    if key in prog.methods:
        c, n, (ps, r) = prog.methods[key]
        return (c, n, tuple(ps), r)
    ps, r = prog.own_by_name[key]
    return (prog.me, key, ps, r)


class Collector:
    """first pass: note every string/type/proto/field/method mentioned, order doesn't matter"""
    def __init__(self, prog):
        self.prog = prog
        self.s, self.t, self.p, self.f, self.m = [], [], [], [], []

    def index(self, u):
        kind, v = u
        if kind == "str":
            if v not in self.s: self.s.append(v)
            return 0
        if kind == "type":
            self.index(("str", v))
            if v not in self.t: self.t.append(v)
            return 0
        if kind == "field":
            fd = self.prog.fields[v]
            for x in fd[0::2]:
                self.index(("type", x))
            self.index(("str", fd[1]))
            if fd not in self.f: self.f.append(fd)
            return 0
        if kind in ("method", "self"):                        # a java.* method, or one of ours
            self._method(method_id(self.prog, v))
            return 0
        raise ValueError(kind)

    def _method(self, m):
        self.index(("type", m[0]))
        self.index(("str", m[1]))
        self._proto((tuple(m[2]), m[3]))
        if m not in self.m: self.m.append(m)

    def _proto(self, p):
        for q in p[0]:
            self.index(("type", q))
        self.index(("type", p[1]))
        if p not in self.p: self.p.append(p)


class Tables:
    """second pass: sorted, with indices"""
    def __init__(self, coll):
        self.prog = coll.prog
        self.s = sorted(set(coll.s) | {shorty(p) for p in coll.p})
        s2i = {s: i for i, s in enumerate(self.s)}
        self.t = sorted(set(coll.t), key=s2i.get)
        t2i = {t: i for i, t in enumerate(self.t)}
        self.p = sorted(set(coll.p), key=lambda p: (t2i[p[1]], [t2i[x] for x in p[0]]))
        p2i = {p: i for i, p in enumerate(self.p)}
        self.f = sorted(set(coll.f), key=lambda f: (t2i[f[0]], s2i[f[1]], t2i[f[2]]))
        self.m = sorted(set(coll.m), key=lambda m: (t2i[m[0]], s2i[m[1]], p2i[(tuple(m[2]), m[3])]))
        self.s2i, self.t2i, self.p2i = s2i, t2i, p2i
        self.f2i = {f: i for i, f in enumerate(self.f)}
        self.m2i = {m: i for i, m in enumerate(self.m)}
        assert len(self.s) < 0x10000 and len(self.m) < 0x10000
        for st in self.s:                                     # every string data item fits one uleb byte
            assert len(st) < 128, st

    def index(self, u):
        kind, v = u
        if kind == "str":
            return self.s2i[v]
        if kind == "type":
            return self.t2i[v]
        if kind == "field":
            return self.f2i[self.prog.fields[v]]
        if kind in ("method", "self"):
            return self.m2i[method_id(self.prog, v)]
        raise ValueError(kind)


def uleb(n):
    b = bytearray()
    while True:
        x = n & 0x7f
        n >>= 7
        b.append(x | (0x80 if n else 0))
        if not n:
            return bytes(b)


def build(prog, codes):
    """the dex of prog, its methods' code as [(name, Code)] in the order the code items go into the file"""
    assert sorted(n for n, _ in codes) == sorted(prog.own_by_name), \
        ([n for n in prog.own_by_name if n not in {x for x, _ in codes}] or
         [n for n, _ in codes if n not in prog.own_by_name])
    me = prog.me
    coll = Collector(prog)
    for n in prog.own_by_name:                                # defined, whether or not referenced
        coll._method(method_id(prog, n))
    coll.index(("type", J + "Object;"))                       # the superclass
    for _, c in codes:
        c.assemble(coll)                                      # pass 1: collect
    tabs = Tables(coll)
    blobs = [c.assemble(tabs) for _, c in codes]              # pass 2: resolve
    ns, nt, np, nf, nm, ncd = len(tabs.s), len(tabs.t), len(tabs.p), len(tabs.f), len(tabs.m), 1

    # ---- offsets
    o = 112
    so, o = o, o + 4 * ns
    to, o = o, o + 4 * nt
    po, o = o, o + 12 * np
    fo, o = o, o + 8 * nf
    mo, o = o, o + 8 * nm
    cdo, o = o, o + 32 * ncd
    data_off = o
    tl_off = {}
    for p in tabs.p:
        if p[0]:
            tl_off[p] = o
            o = (o + 4 + 2 * len(p[0]) + 3) & ~3            # zero-padded to 4, like libdex wants
    o = (o + 3) & ~3
    code_offs = []
    for b in blobs:
        code_offs.append(o)
        o += (len(b) + 3) & ~3
    sd = {}
    for s in tabs.s:
        sd[s] = o
        o += 2 + len(s)
    o = (o + 3) & ~3

    class_data_off = o

    # ---- class data: every method direct and static, sorted by method id
    enc = sorted((tabs.m2i[method_id(prog, n)],
                  0x0009 if n == "main" else 0x0008, co) for (n, _), co in zip(codes, code_offs))
    cd = uleb(0) + uleb(0) + uleb(len(enc)) + uleb(0)
    prev = 0
    for idx, acc, co in enc:
        cd += uleb(idx - prev) + uleb(acc) + uleb(co)
        prev = idx
    o = class_data_off + len(cd)
    o = (o + 3) & ~3
    map_off = o

    # ---- serialize
    out = bytearray(112)
    for s in tabs.s:
        out += struct.pack("<I", sd[s])
    for t in tabs.t:
        out += struct.pack("<I", tabs.s2i[t])
    for p in tabs.p:
        out += struct.pack("<III", tabs.s2i[shorty(p)], tabs.t2i[p[1]], tl_off.get(p, 0))
    for f in tabs.f:                                         # class_idx, type_idx, name_idx
        out += struct.pack("<HHI", tabs.t2i[f[0]], tabs.t2i[f[2]], tabs.s2i[f[1]])
    for mc, mn, mp, mr in tabs.m:
        out += struct.pack("<HHI", tabs.t2i[mc], tabs.p2i[(tuple(mp), mr)], tabs.s2i[mn])
    out += struct.pack("<IIIIIIII", tabs.t2i[me], 0x0001, tabs.t2i[J + "Object;"], 0,
                       0xFFFFFFFF, 0, class_data_off, 0)
    for p in tabs.p:
        if p[0]:
            blob = struct.pack("<I", len(p[0])) + b"".join(struct.pack("<H", tabs.t2i[t]) for t in p[0])
            out += blob + b"\0" * (-len(blob) % 4)          # padded to 4, as laid out above
    for b in blobs:
        out += b + b"\0" * (((len(b) + 3) & ~3) - len(b))
    for s in tabs.s:
        out += uleb(len(s)) + s.encode() + b"\0"
    while len(out) % 4:
        out += b"\0"
    out += cd
    while len(out) % 4:
        out += b"\0"
    entries = [(0x0000, 1, 0), (0x0001, ns, so), (0x0002, nt, to), (0x0003, np, po),
               (0x0004, nf, fo), (0x0005, nm, mo), (0x0006, ncd, cdo)]
    if tl_off:
        entries.append((0x1001, len(tl_off), min(tl_off.values())))
    entries.append((0x2001, len(blobs), code_offs[0]))
    entries.append((0x2002, ns, sd[tabs.s[0]]))
    entries.append((0x2000, 1, class_data_off))
    entries.append((0x1000, 1, map_off))
    entries.sort(key=lambda e: e[2])
    out += struct.pack("<I", len(entries)) + b"".join(struct.pack("<HHII", t, 0, n, f) for t, n, f in entries)

    # ---- header
    file_size = len(out)
    struct.pack_into("<20I", out, 32, file_size, 112, 0x12345678, 0, 0, map_off,
                     ns, so, nt, to, np, po, nf, fo, nm, mo, ncd, cdo, file_size - data_off, data_off)
    out[12:32] = hashlib.sha1(bytes(out[32:])).digest()
    struct.pack_into("<I", out, 8, zlib.adler32(bytes(out[12:])) & 0xffffffff)
    out[:8] = b"dex\n035\0"
    return bytes(out)


# ---------------------------------------------------------------- checking the result
# Every opcode the writer emits: format, and what the verifier below makes of it.  Anything else is refused.
OPS = {0x01: "12x", 0x07: "12x", 0x0a: "11x", 0x0c: "11x", 0x0e: "10x", 0x0f: "11x", 0x11: "11x",
       0x12: "11n", 0x13: "21s", 0x14: "31i", 0x1a: "21c", 0x1f: "21c", 0x21: "12x", 0x22: "21c", 0x23: "22c",
       0x29: "20t", 0x46: "23x", 0x4d: "23x", 0x4f: "23x", 0x62: "21c", 0x77: "3rc", 0x8d: "12x"}
OPS.update({op: "22t" for op in range(0x32, 0x38)})
OPS.update({op: "21t" for op in range(0x38, 0x3e)})
OPS.update({op: "35c" for op in (0x6e, 0x70, 0x71, 0x72)})
OPS.update({op: "23x" for op in range(0x90, 0x9b)})        # add-int .. ushr-int
OPS.update({op: "22b" for op in range(0xd8, 0xe3)})        # add-int/lit8 .. ushr-int/lit8
UNITS = {"10x": 1, "11n": 1, "11x": 1, "12x": 1, "20t": 2, "21c": 2, "21s": 2, "21t": 2, "22b": 2, "22c": 2,
         "22t": 2, "23x": 2, "31i": 3, "35c": 3, "3rc": 3}
PRIM = "ZBSCI"


def decode(units, at):
    """one instruction at unit index at: (op, fmt, operands); operands as the format has them"""
    w = units[at]
    op, hi = w & 0xff, w >> 8
    assert op in OPS, f"opcode {op:#x} at {at}"
    f = OPS[op]
    assert at + UNITS[f] <= len(units), f"{op:#x} at {at} runs past the end"
    x = units[at + 1] if UNITS[f] > 1 else 0
    if f == "10x": return op, f, ()
    if f == "11n": return op, f, (hi & 0xf, ((hi >> 4) ^ 8) - 8)
    if f == "11x": return op, f, (hi,)
    if f == "12x": return op, f, (hi & 0xf, hi >> 4)
    if f == "20t": return op, f, ((x ^ 0x8000) - 0x8000,)
    if f in ("21c", "21s"): return op, f, (hi, x if f == "21c" else (x ^ 0x8000) - 0x8000)
    if f == "21t": return op, f, (hi, (x ^ 0x8000) - 0x8000)
    if f == "22b": return op, f, (hi, x & 0xff, ((x >> 8) ^ 0x80) - 0x80)
    if f == "22c": return op, f, (hi & 0xf, hi >> 4, x)
    if f == "22t": return op, f, (hi & 0xf, hi >> 4, (x ^ 0x8000) - 0x8000)
    if f == "23x": return op, f, (hi, x & 0xff, x >> 8)
    y = units[at + 2]
    if f == "31i": return op, f, (hi, ((x | (y << 16)) ^ 0x80000000) - 0x80000000)
    if f == "35c":
        n = hi >> 4
        assert n <= 4, "no fifth register is written"
        return op, f, (x, [(y >> (4 * i)) & 0xf for i in range(n)])
    return op, f, (x, list(range(y, y + hi)))                 # 3rc


def verify(name, units, nregs, ins, outs, params, ret, ids):
    """Type flow over one method, as ART's verifier does it for the subset written here: every register read
    holds a value of the right kind on every path (int, a reference of a known type, null, or a new-instance
    not yet constructed), parameters arrive in the last ins registers, invokes match their prototypes,
    move-result follows an invoke that returns something, arrays are arrays of the right kind."""
    strs, types, protos, fields, methods = ids
    where = f"{name}:"
    assert ins == len(params), f"{where} ins {ins} for {len(params)} parameters"
    starts, at = set(), 0
    while at < len(units):
        starts.add(at)
        at += UNITS[decode(units, at)[1]]
    obj = "Ljava/lang/Object;"

    def merge(a, b):
        if a == b: return a
        if "U" in (a, b): return "U"
        if {a, b} == {"0", "I"}: return "I"
        if a == "0" and b[0] == "R": return b
        if b == "0" and a[0] == "R": return a
        if a[0] == "R" and b[0] == "R": return ("R", obj)
        return "X"                                           # conflict: unusable

    def is_int(t): return t in ("I", "0")
    def is_ref(t): return t == "0" or t[0] == "R"
    def kind(desc): return "I" if desc in PRIM else ("R", desc)

    entry = ["U"] * nregs
    for i, d in enumerate(params):
        assert d != "J" and d != "D", "no wide values"
        entry[nregs - ins + i] = kind(d)
    state = {0: (entry, None)}
    work = [0]
    while work:
        at = work.pop()
        regs, pending = state[at]
        regs = list(regs)
        op, f, o = decode(units, at)
        here = f"{where}{at:04x} op {op:#04x}"

        def get(r):
            assert r < nregs, f"{here}: v{r} outside the frame of {nregs}"
            t = regs[r]
            assert t not in ("U", "X"), f"{here}: v{r} read before it holds a value on every path"
            return t

        def need_int(r): assert is_int(get(r)), f"{here}: v{r} is {regs[r]}, not an int"
        def need_ref(r):
            t = get(r)
            assert is_ref(t), f"{here}: v{r} is {t}, not a reference"
            return t

        def put(r, t):
            assert r < nregs, f"{here}: v{r} outside the frame"
            regs[r] = t

        nxt, branch, result = [at + UNITS[f]], None, None
        if op in (0x01, 0x07):
            (need_ref if op == 0x07 else need_int)(o[1])
            assert get(o[1])[0] != "N", f"{here}: an unconstructed object copied"
            put(o[0], regs[o[1]])
        elif op in (0x0a, 0x0c):
            assert pending is not None and pending != "V", f"{here}: move-result without an invoke that returns"
            assert (pending in PRIM) == (op == 0x0a), f"{here}: move-result kind for a {pending}"
            put(o[0], kind(pending))
        elif op == 0x0e:
            assert ret == "V", f"{here}: return-void from a method returning {ret}"
            nxt = []
        elif op in (0x0f, 0x11):
            assert ret != "V" and (ret in PRIM) == (op == 0x0f), f"{here}: return kind for {ret}"
            (need_int if op == 0x0f else need_ref)(o[0])
            nxt = []
        elif op in (0x12, 0x13, 0x14):
            put(o[0], "0" if o[1] == 0 else "I")
        elif op == 0x1a:
            assert o[1] < len(strs)
            put(o[0], ("R", "Ljava/lang/String;"))
        elif op == 0x1f:
            need_ref(o[0])
            put(o[0], ("R", types[o[1]]))
        elif op == 0x21:
            t = need_ref(o[1])
            assert t != "0" and t[1][0] == "[", f"{here}: array-length of {t}"
            put(o[0], "I")
        elif op == 0x22:
            t = types[o[1]]
            assert t[0] == "L", f"{here}: new-instance of {t}"
            put(o[0], ("N", t, at))
        elif op == 0x23:
            need_int(o[1])
            assert types[o[2]][0] == "[", f"{here}: new-array of {types[o[2]]}"
            put(o[0], ("R", types[o[2]]))
        elif op == 0x29:
            nxt, branch = [], o[0]
        elif 0x32 <= op <= 0x37:
            a, b_ = get(o[0]), get(o[1])
            ok = (is_int(a) and is_int(b_)) or (op in (0x32, 0x33) and is_ref(a) and is_ref(b_))
            assert ok, f"{here}: compares {a} with {b_}"
            branch = o[2]
        elif 0x38 <= op <= 0x3d:
            t = get(o[0])
            assert is_int(t) or (op in (0x38, 0x39) and is_ref(t)), f"{here}: tests {t}"
            branch = o[1]
        elif op in (0x46, 0x4d, 0x4f):
            v, arr, idx = o
            need_int(idx)
            t = need_ref(arr)
            assert t != "0", f"{here}: array access on null"
            comp = t[1][1:]
            assert t[1][0] == "[", f"{here}: v{arr} is {t[1]}, not an array"
            if op == 0x46:
                assert comp[0] in "L[", f"{here}: aget-object from {t[1]}"
                put(v, ("R", comp))
            elif op == 0x4d:
                assert comp[0] in "L[", f"{here}: aput-object into {t[1]}"
                need_ref(v)
            else:
                assert comp in "BZ", f"{here}: aput-byte into {t[1]}"
                need_int(v)
        elif op == 0x62:
            fc, ft, fn = fields[o[1]]
            assert ft[0] in "L[", f"{here}: sget-object of a {ft} field"
            put(o[0], ("R", ft))
        elif f in ("35c", "3rc"):
            mi, rs = o
            mc, (mret, mps), mn = methods[mi]
            static = op in (0x71, 0x77)
            assert len(rs) == len(mps) + (0 if static else 1), \
                f"{here}: {mn} takes {len(mps)} arguments{'' if static else ' and this'}, given {len(rs)}"
            assert len(rs) <= outs, f"{here}: outs {outs} < {len(rs)}"
            if mn == "<init>":
                # ART: "rejecting non-direct call to constructor" (the first Echo Show refused the class so)
                assert op == 0x70, f"{here}: <init> not by invoke-direct"
            args = rs
            if not static:
                t = get(rs[0])
                if mn == "<init>":
                    assert t[0] == "N" and t[1] == mc, f"{here}: <init> of {mc} on {t}"
                    made = t
                else:
                    assert t[0] == "R", f"{here}: {mn} called on {t}"
                args = rs[1:]
            for r, d in zip(args, mps):
                t = get(r)
                if d in PRIM:
                    assert is_int(t), f"{here}: {mn} wants {d}, v{r} is {t}"
                else:
                    assert is_ref(t), f"{here}: {mn} wants {d}, v{r} is {t}"
                    if t != "0" and d != obj:
                        assert (t[1][0] == "[") == (d[0] == "["), f"{here}: {mn} wants {d}, v{r} is {t[1]}"
                        if d[0] == "[": assert t[1] == d, f"{here}: {mn} wants {d}, v{r} is {t[1]}"
            if mn == "<init>":
                regs = [("R", mc) if x == made else x for x in regs]
            result = mret if mret != "V" else None            # void leaves nothing for a move-result to read
        elif f == "23x":
            need_int(o[1]); need_int(o[2]); put(o[0], "I")
        elif f == "22b" or op == 0x8d:
            need_int(o[1]); put(o[0], "I")
        else:
            raise AssertionError(f"{here}: not handled")
        if branch is not None:
            assert branch != 0, f"{here}: branch to itself"
            nxt.append(at + branch)
        for n in nxt:
            assert n in starts, f"{here}: falls or jumps to {n:#x}, not an instruction"
            if n in state:
                old = state[n][0]
                merged = [merge(x, y) for x, y in zip(old, regs)]
                assert state[n][1] is None and result is None or n == at + UNITS[f], f"{here}: result across a jump"
                if merged != old or state[n][1] != result:
                    state[n] = (merged, result); work.append(n)
            else:
                state[n] = (regs, result); work.append(n)
    return len(state)


def read(data, prog):
    """parse the dex of prog back and walk it; raises on anything ART would refuse.  Returns the ids (strings,
    types, protos, fields, methods) and, per method id, the invoke opcodes used on it, for check_framework()."""
    assert data[:8] == b"dex\n035\0", data[:8]
    (file_size, hs, endian, _, _, map_off, ns, so, nt, to, np, po, nf, fo, nm, mo, ncd, cdo,
     data_size, data_off) = struct.unpack("<20I", data[32:112])
    assert file_size == len(data) and hs == 112 and endian == 0x12345678
    assert zlib.adler32(data[12:]) & 0xffffffff == struct.unpack("<I", data[8:12])[0]
    assert hashlib.sha1(data[32:]).digest() == data[12:32]
    u16 = lambda o: struct.unpack("<H", data[o:o + 2])[0]
    u32 = lambda o: struct.unpack("<I", data[o:o + 4])[0]
    # the map: every entry where it says, offsets ascending
    n = u32(map_off)
    seen = [(u16(map_off + 4 + 12 * i), u32(map_off + 4 + 12 * i + 4), u32(map_off + 4 + 12 * i + 8))
            for i in range(n)]                                 # (type, size, offset)
    assert [e[2] for e in seen] == sorted(e[2] for e in seen)
    where = {t: (sz, f) for t, sz, f in seen}
    assert where[0x0001][0] == ns and where[0x0002][0] == nt and where[0x0003][0] == np
    assert where[0x0005][0] == nm and where[0x0006][0] == ncd and where[0x1000][0] == 1
    # ids sorted, strings readable, every offset inside the file
    strs = []
    for i in range(ns):
        o = u32(so + 4 * i)
        l = data[o]; o += 1
        assert data[o + l] == 0
        strs.append(data[o:o + l].decode())
    assert strs == sorted(strs)
    tids = [u32(to + 4 * i) for i in range(nt)]
    assert tids == sorted(tids)
    types = [strs[t] for t in tids]
    protos = []
    for i in range(np):
        _, rt, tl = struct.unpack("<III", data[po + 12 * i:po + 12 * i + 12])
        ps = [types[u16(tl + 4 + 2 * k)] for k in range(u32(tl))] if tl else []
        protos.append((types[rt], ps))
    fields = [(types[u16(fo + 8 * i)], types[u16(fo + 8 * i + 2)], strs[u32(fo + 8 * i + 4)]) for i in range(nf)]
    methods = [(types[u16(mo + 8 * i)], protos[u16(mo + 8 * i + 2)], strs[u32(mo + 8 * i + 4)]) for i in range(nm)]
    ids = (strs, types, protos, fields, methods)
    # class data: the method list, code offsets
    def uleb_at(o):
        r = s = 0
        while True:
            b = data[o]; o += 1
            r |= (b & 0x7f) << s
            if b < 0x80:
                return r, o
            s += 7
    o = u32(cdo + 24)                                        # the class_def's class_data_off
    sf, o = uleb_at(o); iff, o = uleb_at(o); dm, o = uleb_at(o); vm, o = uleb_at(o)
    assert ncd == 1 and (sf, iff, vm) == (0, 0, 0) and dm == len(prog.own)
    me = types[u32(cdo)]
    assert me == prog.me, f"class {me}, not {prog.me}"
    uses = {}
    idx = 0
    for _ in range(dm):
        d, o = uleb_at(o); acc, o = uleb_at(o); co, o = uleb_at(o)
        idx += d
        mc, (mret, mps), mn = methods[idx]
        assert mc == me and acc & 0x0008, f"{mn}: not a static method of {me}"
        assert co >= where[0x2001][1] and co < len(data)
        regs, insz, outsz, tries, dbg, isz = struct.unpack("<HHHHII", data[co:co + 16])
        assert tries == 0 and dbg == 0 and insz <= regs and regs <= 16 and outsz <= 6
        units = [u16(co + 16 + 2 * k) for k in range(isz)]
        verify(mn, units, regs, insz, outsz, mps, mret, ids)
        at = 0
        while at < len(units):
            op, f, ops_ = decode(units, at)
            if f in ("35c", "3rc"):
                uses.setdefault(ops_[0], set()).add(op)
            at += UNITS[f]
    for mi, ops_ in uses.items():                             # one class of ours: static calls only to it
        if methods[mi][0] == me:
            assert ops_ <= {0x71, 0x77}, f"{methods[mi][2]}: called with {ops_}"
    return ids, uses


# ---------------------------------------------------------------- against the firmware's own framework
def boot_classes(rootfs):
    """every class of the boot class path: the dex files inside /system/framework/<arch>/boot*.oat (Android 7
    keeps them there whole).  descriptor -> (access, super, interfaces, {(name, params): access},
    {name: (type, access, initial int value or None)})"""
    import glob
    import re
    out = {}
    oats = sorted(glob.glob(f"{rootfs}/system/framework/*/boot*.oat"))
    assert oats, f"no boot*.oat under {rootfs}/system/framework"
    for path in oats:
        blob = open(path, "rb").read()
        for m in re.finditer(rb"dex\n03[5-9]\0", blob):
            base = m.start()
            size = struct.unpack("<I", blob[base + 32:base + 36])[0]
            d = blob[base:base + size]
            if len(d) != size or struct.unpack("<I", d[36:40])[0] != 112:
                continue
            u16 = lambda o: struct.unpack("<H", d[o:o + 2])[0]
            u32 = lambda o: struct.unpack("<I", d[o:o + 4])[0]
            (ns, so, nt, to, np, po, nf, fo, nm, mo, ncd, cdo) = struct.unpack("<12I", d[56:104])

            def uleb_at(o):
                r = s = 0
                while True:
                    b = d[o]; o += 1
                    r |= (b & 0x7f) << s
                    if b < 0x80:
                        return r, o
                    s += 7

            def string(i):
                o = u32(so + 4 * i)
                _, o = uleb_at(o)
                return d[o:d.index(b"\0", o)].decode("utf-8", "replace")
            ty = lambda i: string(u32(to + 4 * i))

            def params(p):
                tl = u32(po + 12 * p + 8)
                return tuple(ty(u16(tl + 4 + 2 * k)) for k in range(u32(tl))) if tl else ()
            for c in range(ncd):
                cls, acc, sup, ifo, _, _, cdata, sv = struct.unpack("<8I", d[cdo + 32 * c:cdo + 32 * c + 32])
                desc = ty(cls)
                ifs = [ty(u16(ifo + 4 + 2 * k)) for k in range(u32(ifo))] if ifo else []
                meths, flds = {}, {}
                if cdata:
                    o = cdata
                    sf, o = uleb_at(o); iff, o = uleb_at(o); dm, o = uleb_at(o); vm, o = uleb_at(o)
                    vals = []                                # the static fields' initial values, ints only
                    if sv:
                        nv, v = uleb_at(sv)
                        for _ in range(nv):
                            t = d[v]; v += 1
                            n = 0 if (t & 0x1f) in (0x1e, 0x1f) else (t >> 5) + 1
                            vals.append(int.from_bytes(d[v:v + n], "little", signed=True) if (t & 0x1f) == 4 else None)
                            v += n
                    idx = 0
                    for k in range(sf + iff):
                        if k == sf: idx = 0
                        dd, o = uleb_at(o); fa, o = uleb_at(o); idx += dd
                        flds[string(u32(fo + 8 * idx + 4))] = (ty(u16(fo + 8 * idx + 2)), fa,
                                                                 vals[k] if k < min(sf, len(vals)) else None)
                    idx = 0
                    for k in range(dm + vm):
                        if k == dm: idx = 0
                        dd, o = uleb_at(o); ma, o = uleb_at(o); _, o = uleb_at(o); idx += dd
                        meths[(string(u32(mo + 8 * idx + 4)), params(u16(mo + 8 * idx + 2)))] = ma
                out.setdefault(desc, (acc, ty(sup) if sup != 0xFFFFFFFF else None, ifs, meths, flds))
    return out


def check_framework(prog, ids, uses, rootfs):
    """every class, method and field the dex names, and every one its reflection asks for (prog.reflected,
    .reflected_fields, .constants), exists on the firmware's boot class path, with the invoke kind that fits
    it (static, interface, virtual, direct).  What reflection asks for must be public too: getMethod,
    getField and getConstructor see nothing else, and Class.newInstance wants a class it can instantiate."""
    strs, types, protos, fields, methods = ids
    bc = boot_classes(rootfs)
    me = prog.me

    def supers(c):                                           # the class, its superclasses, all interfaces
        seen, todo = [], [c]
        while todo:
            x = todo.pop(0)
            if x is None or x in seen or x not in bc:
                continue
            seen.append(x)
            todo += [bc[x][1]] + bc[x][2]
        return seen

    def method(c, n, ps):
        for x in supers(c):
            if (n, tuple(ps)) in bc[x][3]:
                return x, bc[x][3][(n, tuple(ps))]
        return None, None

    def field(c, n):
        for x in supers(c):
            if n in bc[x][4]:
                return bc[x][4][n]
        return None

    bad = []
    for t in types:
        e = t.lstrip("[")
        if e[0] == "L" and e != me and e not in bc:
            bad.append(f"type {t}: not on the boot class path")
    for mi, (mc, (mret, mps), mn) in enumerate(methods):
        if mc == me:
            continue
        x, acc = method(mc, mn, mps)
        if x is None:
            bad.append(f"{mc}.{mn}({''.join(mps)}): no such method"); continue
        for op in uses.get(mi, ()):
            static = bool(acc & 0x0008)
            iface = bool(bc[mc][0] & 0x0200)
            want = {0x71: static, 0x77: static, 0x72: not static and iface, 0x6e: not static and not iface,
                    0x70: mn == "<init>" or bool(acc & 0x0002)}[op]
            if not want:
                bad.append(f"{mc}.{mn}: invoke {op:#x} does not fit (static {static}, interface {iface})")
    for fc, ft, fn in fields:
        got = field(fc, fn)
        if got is None or got[0] != ft or not got[1] & 0x0008:
            bad.append(f"{fc}.{fn}: no static field of type {ft}")
    for c, n, ps, static in prog.reflected:
        sig = f"{c}.{n}({''.join(ps)})"
        if n == "<init>":                                     # constructors are not inherited
            acc = bc[c][3].get((n, tuple(ps))) if c in bc else None
            if acc is None or static:
                bad.append(f"reflection: constructor {sig} not found"); continue
            if bc[c][0] & 0x0600:
                bad.append(f"reflection: {c} is abstract or an interface, no instance of it")
        else:
            x, acc = method(c, n, ps)
            if x is None or bool(acc & 0x0008) != static:
                bad.append(f"reflection: {sig} {'static ' if static else ''}not found"); continue
        if not acc & 0x0001:
            bad.append(f"reflection: {sig} is not public (access {acc:#x})")
    for c, n, t in prog.reflected_fields:
        got = field(c, n)
        if got is None or got[0] != t:
            bad.append(f"reflection: field {c}.{n} of type {t} not found")
        elif not got[1] & 0x0001:
            bad.append(f"reflection: field {c}.{n} is not public")
    for c, n, v in prog.constants:
        got = field(c, n)
        if got is None or not got[1] & 0x0008 or got[2] != v:
            bad.append(f"{c}.{n} is {got and got[2]}, the dex has {v} written in")
    assert not bad, "\n".join(bad)
    return len(bc)


def cli(prog, emit, tool, dex):
    """the command line of tools/mk<name>.py: rebuild scripts/device/<dex> (or the path given), --check it
    against what this builds, --framework=<rootfs> also against the firmware.  Nothing is written before the
    dex parses back."""
    blob = build(prog, emit())
    ids, uses = read(blob, prog)
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    fw = [a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--framework=")]
    if fw:
        print(f"framework: {check_framework(prog, ids, uses, fw[0])} boot classes, every reference found")
    out = args[0] if args else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(tool))),
                                            "scripts", "device", dex)
    if "--check" in sys.argv:
        have = open(out, "rb").read()
        assert have == blob, f"{out} is not what tools/{os.path.basename(tool)} builds"
        print(f"ok {out} ({len(blob)} bytes, as assembled)")
        return
    open(out, "wb").write(blob)
    print(f"{out}: {len(blob)} bytes")
