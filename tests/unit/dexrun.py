#!/usr/bin/env python3
"""Runs the hand-assembled dex files of scripts/device/ on the PC: a Dalvik interpreter for the instructions
tools/dexasm.py writes, the java.* methods of its METHODS/FIELDS tables, and the reflection (Class.forName,
getMethod, getConstructor, getField) handed to fakes of the framework classes that a test supplies.  No ART here
(qemu-user cannot run the firmware's), and dexasm's type checks say nothing about what the code computes; the
tests built on this (wifictl_run.py, ringbar_run.py) do.  Every reflected lookup must name a signature of the
program's own reflected lists, which --framework=<rootfs> checks against the firmware's classes."""
import os
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import dexasm                                                  # noqa: E402


class Exit(Exception):
    pass


class Zero(int):
    """what const 0 leaves in a register: the int 0, or null where a reference is wanted (Dalvik does not tell
    the two apart; the verifier does by use).  Arithmetic on it gives plain ints, Integer.valueOf a real 0."""


def i32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x & 0x80000000 else x


PRIMS = {"int": "I", "boolean": "Z", "float": "F"}


def desc(name):
    """a Class's getName() as a descriptor: int -> I, [I -> [I, a.b.C -> La/b/C;"""
    if name in PRIMS: return PRIMS[name]
    if name.startswith("["): return name.replace(".", "/")
    return "L" + name.replace(".", "/") + ";"


# ---------------------------------------------------------------- the Java side the dex reaches
class JClass:
    def __init__(self, name, make=None):
        self.name, self.make = name, make


class JMethod:
    def __init__(self, cls, name, params, static):
        self.cls, self.name, self.params, self.static = cls, name, params, static


class JCtor:
    def __init__(self, cls, params):
        self.cls, self.params = cls, params


class JField:
    def __init__(self, name):
        self.name = name


class ByteOrder:
    def __init__(self, n):
        self.n = n


LITTLE_ENDIAN = ByteOrder("LITTLE_ENDIAN")


class ByteBuffer:                                              # java.nio.ByteBuffer.wrap(byte[]): big-endian first
    def __init__(self, b):
        self.b, self.order = b, "BIG_ENDIAN"


class IntBuffer:                                               # its asIntBuffer(): a view, position 0
    def __init__(self, bb):
        self.bb, self.pos = bb, 0

    def get(self, dst):
        n = len(self.bb.b) // 4
        assert self.pos + len(dst) <= n, "BufferUnderflowException"
        raw = bytes(x & 0xff for x in self.bb.b[4 * self.pos:4 * (self.pos + len(dst))])
        dst[:] = struct.unpack(("<" if self.bb.order == "LITTLE_ENDIAN" else ">") + "%di" % len(dst), raw)
        self.pos += len(dst)
        return self


class Stdin:
    """System.in: the chunks given, one read() each at most (a pipe hands over what is there), then EOF"""
    def __init__(self, env, chunks):
        self.env, self.chunks, self.reads = env, [bytes(c) for c in chunks], 0

    def read(self, buf, off, n):
        assert 0 <= off and 0 < n and off + n <= len(buf), (off, n, len(buf))
        assert self.env.flushed == len(self.env.out), "reads stdin before what it printed is flushed"
        self.reads += 1
        if not self.chunks:
            return -1
        c = self.chunks[0]
        k = min(n, len(c))
        buf[off:off + k] = [x - 256 if x > 127 else x for x in c[:k]]    # Java bytes are signed
        self.chunks[0] = c[k:]
        if not self.chunks[0]:
            self.chunks.pop(0)
        return k


class JavaEnv:
    """java.* by METHODS key, and reflection onto the fakes.  A test subclasses it: CLASSES (the fake classes,
    each with JAVA = its getClass().getName(), and AS = the descriptors its methods are listed under in the
    program's reflected list, e.g. a binder proxy under its interface), NAMES (other classes Class.forName
    finds), and static_call() for the reflected static methods."""
    CLASSES = ()
    NAMES = ()

    def __init__(self, prog, stdin=()):
        self.prog = prog
        self.out, self.err, self.flushed = [], [], 0
        self.stdin = Stdin(self, stdin)
        self.classes = {c.JAVA: c for c in self.CLASSES}

    def jstr(self, o):                                         # String.valueOf / StringBuilder.append(Object)
        if o is None: return "null"
        if o is True: return "true"
        if o is False: return "false"
        if isinstance(o, (int, str)): return str(int(o)) if isinstance(o, int) else o
        if isinstance(o, float): return repr(o)
        if hasattr(o, "toString"): return o.toString()
        raise AssertionError(f"toString of {o!r}")

    def jclass(self, o):
        if isinstance(o, str): return JClass("java.lang.String")
        return JClass(type(o).JAVA, type(o))

    def for_name(self, n):
        assert n not in PRIMS, f"Class.forName({n}): ClassNotFoundException (a primitive is no class name)"
        if n in self.classes: return JClass(n, self.classes[n])
        if n in self.NAMES or n == "java.lang.String": return JClass(n)
        raise AssertionError(f"Class.forName({n}): not a class this test knows")

    def listed_as(self, cls):                                  # the descriptors a class's members are listed under
        c = self.classes.get(cls.name)
        return [desc(cls.name)] + list(getattr(c, "AS", ()))

    def get_method(self, cls, name, params):
        ps = tuple(desc(p.name) for p in params)
        for c, n, p, st in self.prog.reflected:
            if c in self.listed_as(cls) and n == name and tuple(p) == ps:
                return JMethod(cls, name, ps, st)
        raise AssertionError(f"{cls.name}.{name}{ps}: not in the program's reflected list")

    def get_ctor(self, cls, params):
        ps = tuple(desc(p.name) for p in params)
        assert (desc(cls.name), "<init>", ps) in {(c, n, tuple(p)) for c, n, p, st in self.prog.reflected}, \
            f"new {cls.name}{ps}: not in the program's reflected list"
        return JCtor(cls, ps)

    def unbox(self, what, params, args):
        """Method.invoke / Constructor.newInstance: the Object[] against the parameter types"""
        assert len(params) == len(args), f"{what}: {len(args)} arguments for {params}"
        args = [None if p[0] in "L[" and isinstance(a, Zero) else a for p, a in zip(params, args)]
        for p, a in zip(params, args):
            if p == "I": assert isinstance(a, int) and not isinstance(a, (bool, Zero)), (what, a)
            if p == "Z": assert isinstance(a, bool), (what, a)
            if p == "F": assert isinstance(a, float), (what, a)
        return args

    def invoke(self, m, obj, args):
        args = self.unbox(m.name, m.params, args)
        if m.static:
            return self.static_call(m.cls.name, m.name, args)
        assert obj is not None and not isinstance(obj, Zero), f"{m.name} on null"
        return getattr(obj, m.name)(*args)

    def static_call(self, cls, name, args):
        raise AssertionError(f"{cls}.{name}: no static method faked")

    def call(self, key, args):
        """the java.* methods, by dexasm's METHODS key"""
        a = args
        if key == "SB.new": a[0].clear(); return None
        if key == "SB.appendO": a[0].append(self.jstr(a[1])); return a[0]
        if key == "SB.appendI": a[0].append(str(int(a[1]))); return a[0]
        if key == "SB.to": return "".join(a[0])
        if key == "println": (self.out if a[0] == "out" else self.err).append(a[1]); return None
        if key == "flush":
            if a[0] == "out": self.flushed = len(self.out)
            return None
        if key == "str.eq": return a[0] == a[1]
        if key == "str.charat": return ord(a[0][a[1]])
        if key == "str.length": return len(a[0])
        if key == "str.valueO": return self.jstr(a[0])
        if key == "int.parse": return int(a[0])
        if key == "int.value": return int(a[0])
        if key == "int.intval": return a[0]
        if key == "bool.value": return bool(a[0])
        if key == "float.value": return float(a[0])
        if key == "cls.forName": return self.for_name(a[0])
        if key == "cls.newi":
            self.get_ctor(a[0], [])
            return a[0].make()
        if key == "cls.getm": return self.get_method(a[0], a[1], a[2])
        if key == "cls.getc": return self.get_ctor(a[0], a[1])
        if key == "ctor.newi": return a[0].cls.make(*self.unbox("new " + a[0].cls.name, a[0].params, list(a[1])))
        if key == "cls.getf":
            known = {n for c, n, t in self.prog.reflected_fields if c in self.listed_as(a[0])}
            assert a[1] in known, f"{a[0].name}.{a[1]}: not in the program's reflected fields"
            return JField(a[1])
        if key == "obj.getcls": return self.jclass(a[0])
        if key == "obj.tostr": return self.jstr(a[0])
        if key == "obj.eq": return a[0] is a[1] or a[0] == a[1] and type(a[0]) is type(a[1])
        if key == "m.invoke": return self.invoke(a[0], a[1], list(a[2]))
        if key == "m.setacc": return None
        if key == "f.get": return getattr(a[1], a[0].name)
        if key == "f.set":
            old = getattr(a[1], a[0].name)
            assert type(old) is type(a[2]) or old is None or (isinstance(old, bool) and isinstance(a[2], bool)), \
                f"Field.set {a[0].name}: {a[2]!r} for a {type(old).__name__}"
            setattr(a[1], a[0].name, a[2]); return None
        if key == "it.has": return a[0][1] < len(a[0][0])
        if key == "it.next": a[0][1] += 1; return a[0][0][a[0][1] - 1]
        if key == "list.iter": return [a[0], 0]
        if key == "bits.set": a[0].add(a[1]); return None
        if key == "sys.exit": raise Exit(a[0])
        if key == "in.read": return a[0].read(a[1], a[2], a[3])
        if key == "bb.wrap": return ByteBuffer(a[0])
        if key == "bb.order": assert isinstance(a[1], ByteOrder); a[0].order = a[1].n; return a[0]
        if key == "bb.asint": return IntBuffer(a[0])
        if key == "ib.get": return a[0].get(a[1])
        raise AssertionError(key)

    def sfield(self, key):
        return {"sys.out": "out", "sys.err": "err", "sys.in": self.stdin, "bool.T": True, "bool.F": False,
                "int.TYPE": JClass("int"), "bool.TYPE": JClass("boolean"), "float.TYPE": JClass("float"),
                "order.LE": LITTLE_ENDIAN}[key]


# ---------------------------------------------------------------- the interpreter
class Dex:
    def __init__(self, data, prog):
        self.prog = prog
        self.ids, _ = dexasm.read(data, prog)                  # dexasm's checks first
        strs, types, protos, fields, methods = self.ids
        u32 = lambda o: struct.unpack("<I", data[o:o + 4])[0]
        cdo = struct.unpack("<I", data[100:104])[0]

        def uleb(o):
            r = s = 0
            while True:
                b = data[o]; o += 1; r |= (b & 0x7f) << s
                if b < 0x80: return r, o
                s += 7
        o = u32(cdo + 24)
        _, o = uleb(o); _, o = uleb(o); dm, o = uleb(o); _, o = uleb(o)
        self.code, idx = {}, 0
        for _ in range(dm):
            d, o = uleb(o); _, o = uleb(o); co, o = uleb(o); idx += d
            regs, ins, _, _, _, n = struct.unpack("<HHHHII", data[co:co + 16])
            self.code[methods[idx][2]] = (regs, ins, [struct.unpack("<H", data[co + 16 + 2 * k:co + 18 + 2 * k])[0]
                                                      for k in range(n)])
        # java.* method ids -> METHODS keys, fields likewise
        self.mkeys = {}
        for k, (c, n, (ps, r)) in prog.methods.items():
            for i, (mc, (mr, mps), mn) in enumerate(methods):
                if (mc, mn, tuple(mps), mr) == (c, n, tuple(ps), r): self.mkeys[i] = k
        self.fkeys = {i: k for k, f in prog.fields.items() for i, ff in enumerate(fields)
                      if (ff[0], ff[2], ff[1]) == (f[0], f[1], f[2])}

    def run(self, env, name, args, depth=0):
        strs, types, protos, fields, methods = self.ids
        nregs, ins, units = self.code[name]
        r = [None] * nregs
        r[nregs - ins:] = args
        at, res, steps = 0, None, 0
        while True:
            steps += 1
            assert steps < 200000, f"{name}: runs away"
            op, f, o = dexasm.decode(units, at)
            nxt = at + dexasm.UNITS[f]
            if op in (0x01, 0x07): r[o[0]] = r[o[1]]
            elif op in (0x0a, 0x0c): r[o[0]] = res
            elif op == 0x0e: return None
            elif op in (0x0f, 0x11): return r[o[0]]
            elif op in (0x12, 0x13, 0x14): r[o[0]] = Zero() if o[1] == 0 else o[1]
            elif op == 0x1a: r[o[0]] = strs[o[1]]
            elif op == 0x1f: pass
            elif op == 0x21: r[o[0]] = len(r[o[1]])
            elif op == 0x22: r[o[0]] = [] if types[o[1]] == dexasm.SB else ("new", types[o[1]])
            elif op == 0x23:
                assert r[o[1]] >= 0, "NegativeArraySizeException"
                r[o[0]] = [0 if types[o[2]] in ("[B", "[I") else None] * r[o[1]]
            elif op == 0x29: nxt = at + o[0]
            elif 0x32 <= op <= 0x37:
                a, b = r[o[0]], r[o[1]]
                if [a == b, a != b, a < b, a >= b, a > b, a <= b][op - 0x32]: nxt = at + o[2]
            elif 0x38 <= op <= 0x3d:
                a = r[o[0]]
                a = 0 if a is None else (1 if not isinstance(a, int) else a)
                if [a == 0, a != 0, a < 0, a >= 0, a > 0, a <= 0][op - 0x38]: nxt = at + o[1]
            elif op == 0x46: r[o[0]] = r[o[1]][r[o[2]]]
            elif op == 0x4d:                                    # aput-object: a const 0 stored is null
                v = r[o[0]]
                r[o[1]][r[o[2]]] = None if isinstance(v, Zero) else v
            elif op == 0x4f: r[o[1]][r[o[2]]] = r[o[0]]
            elif op == 0x62: r[o[0]] = env.sfield(self.fkeys[o[1]])
            elif f in ("35c", "3rc"):
                mi, rs = o
                av = [r[x] for x in rs]
                mc, _, mn = methods[mi]
                if mc == self.prog.me:                             # one of our own: interpret it
                    res = self.run(env, mn, av, depth + 1)
                elif self.mkeys[mi] == "str.new_b":                # new String(byte[]): the platform's UTF-8
                    s = bytes(b & 0xff for b in av[1]).decode("utf-8", "replace")
                    for k, x in enumerate(r):
                        if x is av[0]: r[k] = s
                    res = None
                else:
                    res = env.call(self.mkeys[mi], av)
            elif op in (0x90, 0x91, 0x92, 0x95, 0x96, 0x98):
                a, b = r[o[1]], r[o[2]]
                r[o[0]] = i32({0x90: a + b, 0x91: a - b, 0x92: a * b, 0x95: a & b, 0x96: a | b,
                               0x98: a << (b & 31)}[op])
            elif op in (0xd8, 0xda, 0xde, 0xe0, 0xe2):
                a, n = r[o[1]], o[2]
                r[o[0]] = i32({0xd8: a + n, 0xda: a * n, 0xde: a | n, 0xe0: a << (n & 31),
                               0xe2: (a & 0xffffffff) >> (n & 31)}[op])
            elif op == 0x8d: r[o[0]] = ((r[o[1]] & 0xff) ^ 0x80) - 0x80
            else:
                raise AssertionError(f"{name}: {op:#x} not run here")
            at = nxt


class Checks:
    """ok/FAIL lines, a count at the end"""
    def __init__(self):
        self.fails = []

    def __call__(self, cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond: self.fails.append(what)

    def done(self, summary):
        if self.fails:
            print(f"{len(self.fails)} failed")
            sys.exit(1)
        print(summary)
