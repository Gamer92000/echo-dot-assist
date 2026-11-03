#!/usr/bin/env python3
"""Assembles scripts/device/screen.dex, checked into the repository, from nothing but this file and tools/dexasm.py:
the loader of the Echo Show's screen UI (src/ui/, libhmui.so).  Everything the UI does is native code; Java is only
the way into a process that may talk to the framework (SurfaceFlinger, the activity manager, Skia's text and
images), so this program does two things:

    public class Screen {
        public static void main(String[] a) {
            System.load(a[0]);                  // libhmui.so: its JNI_OnLoad starts the UI's thread
            Thread.currentThread().join();      // and the process lives as long as that thread: it calls exit()
        }
    }

run, as hassmic's own child (uid system, SELinux su), as

    CLASSPATH=/system/hassmic/screen.dex app_process / Screen /system/hassmic/libhmui.so

The protocol on stdin/stdout is the library's (src/ui/ui.h).  The framework classes the library reaches through
JNI are listed in src/ui/android.c's JNI table; --framework checks them against the firmware's boot class path
as it checks this dex's own references (JNI sees private members too, so they need not be public).

python3 tools/mkscreen.py            rebuild scripts/device/screen.dex
python3 tools/mkscreen.py --check    only re-read and check the one in the tree
    --framework=firmware/checkers/rootfs   also against the firmware's boot class path, the JNI table included
                                           (any number: every model with the UI, make unit-screen)"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dexasm import Code, Program, AGET_OBJ, VIRT, STATIC, STRARR, STR  # noqa: E402,F401
import dexasm                                                  # noqa: E402

ME = "LScreen;"
THR = "Ljava/lang/Thread;"
METHODS = dict(dexasm.METHODS)
METHODS.update({
    "sys.load": (dexasm.SYS, "load", ((STR,), "V")),
    "thr.cur":  (THR, "currentThread", ((), THR)),
    "thr.join": (THR, "join", ((), "V")),
})
SELF = [("main", (STRARR,), "V")]
JNI_TABLE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src", "ui", "android.c")


def emit():
    c = Code(2, 1)                                            # a=0 t=1
    c.aget_at(1, 0, 0)
    c.invoke(STATIC, "sys.load", [1])
    c.invoke(STATIC, "thr.cur", []); c.move_result(1, True)
    c.invoke(VIRT, "thr.join", [1])
    c.ret_void()
    return [("main", c)]


def jni_table(path):
    """the JNI table of src/ui/android.c: lines  J(var, "class/Name", "method", "(params)ret", static)  and
    F(var, "class/Name", "field", "type", static) -> reflected methods and fields as check_framework wants them"""
    src = open(path).read()
    meths, flds = [], []

    def types(sig):
        out, i = [], 0
        while i < len(sig):
            j = i
            while sig[j] == "[":
                j += 1
            j = sig.index(";", j) + 1 if sig[j] == "L" else j + 1
            out.append(sig[i:j]); i = j
        return out
    for m in re.finditer(r'\bJ\(\s*\w+\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"\(([^)]*)\)([^"]+)"\s*,\s*(\d)\s*\)', src):
        cls, name, params, ret, static = m.groups()
        meths.append(("L" + cls + ";", name, types(params), static == "1", ret))
    for m in re.finditer(r'\bF\(\s*\w+\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(\d)\s*\)', src):
        cls, name, ty, static = m.groups()
        flds.append(("L" + cls + ";", name, ty, static == "1"))
    assert meths, f"no JNI table in {path}"
    return meths, flds


def check_jni(rootfs):
    """every method and field of the JNI table exists on the firmware's boot class path, static where the table says so,
    with the return type it says (GetMethodID matches the whole signature)"""
    meths, flds = jni_table(JNI_TABLE)
    bc = dexasm.boot_classes(rootfs)

    def supers(c):
        seen, todo = [], [c]
        while todo:
            x = todo.pop(0)
            if x is None or x in seen or x not in bc:
                continue
            seen.append(x)
            todo += [bc[x][1]] + bc[x][2]
        return seen
    bad = []
    for c, n, ps, static, ret in meths:
        if c not in bc:
            bad.append(f"{c}: no such class"); continue
        hit = next(((x, bc[x][3][(n, tuple(ps))]) for x in ([c] if n == "<init>" else supers(c))
                    if (n, tuple(ps)) in bc[x][3]), None)
        if hit is None:
            bad.append(f"{c}.{n}({''.join(ps)}): not found")
        elif bool(hit[1] & 0x0008) != static:
            bad.append(f"{c}.{n}({''.join(ps)}): {'static' if hit[1] & 0x0008 else 'not static'} on the device")
        elif bc[hit[0]][5][(n, tuple(ps))] != ret:
            bad.append(f"{c}.{n}({''.join(ps)}): returns {bc[hit[0]][5][(n, tuple(ps))]} on the device, the table says {ret}")
    classes = re.findall(r'\bC\(\w+, "([^"]+)"\)', open(JNI_TABLE).read())   # FindClass's table
    assert classes, f"no class table in {JNI_TABLE}"
    bad += [f"{c}: no such class (FindClass)" for c in classes if "L" + c + ";" not in bc]
    for c, n, ty, static in flds:
        got = next((bc[x][4][n] for x in supers(c) if n in bc[x][4]), None)
        if got is None or got[0] != ty:
            bad.append(f"{c}.{n}: {got and got[0]}, the table says {ty}")
        elif bool(got[1] & 0x0008) != static:
            bad.append(f"{c}.{n}: {'static' if got[1] & 0x0008 else 'not static'} on the device")
    assert not bad, "\n".join(bad)
    print(f"jni {rootfs}: {len(classes)} classes, {len(meths)} methods and {len(flds)} fields of {os.path.relpath(JNI_TABLE)} found on the boot class path")


PROG = Program(ME, SELF, methods=METHODS)


def main():
    dexasm.cli(PROG, emit, __file__, "screen.dex")
    fw = [a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--framework=")]
    for f in fw:
        check_jni(f)


if __name__ == "__main__":
    main()
