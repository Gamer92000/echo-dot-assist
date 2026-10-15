#!/usr/bin/env python3
"""Assembles scripts/device/ringbar.dex, checked into the repository, from nothing but this file and
tools/dexasm.py: the light ring of the Echo Show (checkers), shown as a band along the bottom of the screen.
The Echo Show has no ring of LEDs; hassmic draws the ring "linearised" (LED by LED, left to right) and streams
the frames to this program, which only puts them on the screen: every animation is hassmic's.  It runs, as
hassmic's own child (uid system, SELinux su), as

    CLASSPATH=/data/local/hassmic/ringbar.dex app_process / Ringbar <band rows>

How it gets the band to itself:
  - the window manager's overscan (IWindowManager.setOverscan, what `wm overscan` does): every app, the home
    screen included, is laid out above the band, so nothing of theirs is under it.  Overscan is given in the
    panel's natural orientation; PhoneWindowManager.beginLayoutLw (AOSP 7) turns it by the rotation, so the side that is
    at the bottom now is: rotation 0 bottom, 1 left, 2 top, 3 right.  The Echo Show 5 has a 480x960 panel shown
    as 960x480, rotation 1: its natural left edge is the bottom.  The window manager keeps the setting in
    /data/system/display_settings.xml, so it outlives the process: `Ringbar 0` puts it back to nothing.
  - a SurfaceFlinger layer of its own (SurfaceControl, as the boot animation and the window manager make
    theirs), opaque, at layer 2000000000: the window manager puts windows at their type's layer x 10000 + 1000
    (a few hundred thousand), the boot animation sits at 2^30.  Placed at the bottom of the logical display:
    layer positions are in the rotated frame, overscan is not.

The protocol, which hassmic's C side relies on:
    argv[1]         the band's height in rows; 0 = overscan reset, no band
    stdout          one line first: "ready <width> <rows>" (then the frames are wanted), or "off" for 0
    stdin           frames of width * rows * 4 bytes: each pixel 0xAARRGGBB as a little-endian int, rows top
                    to bottom; a frame is drawn once it is whole.  End of stdin (hassmic closed it or died):
                    the program ends and its layer goes with it

Reflection only, as Wifictl does (Class.forName/getMethod/getConstructor/getField): the dex references java.*
classes alone.  Ints and objects only (floats as boxed Float, made from strings), no try/catch: whatever
throws ends the process, and hassmic sees its stdout close.  The Java source it stands for:

    public class Ringbar {
        public static void main(String[] a) {
            int h = Integer.parseInt(a[0]);
            Object wm = sinv("android.view.IWindowManager$Stub", "asInterface", "android.os.IBinder",
                             sinv("android.os.ServiceManager", "getService", "java.lang.String", "window"));
            Object di = w1(sinv0("android.hardware.display.DisplayManagerGlobal", "getInstance"),
                           "getDisplayInfo", "int", Integer.valueOf(0));
            int w = fint(di, "logicalWidth"), dh = fint(di, "logicalHeight");
            overscan(wm, fint(di, "rotation"), h);
            if (h == 0) { out("off"); return; }                 // the band switched off: overscan reset, done
            Object s = surface(w, h, dh - h);
            out(new StringBuilder().append("ready ").append(w).append(" ").append(h).toString());
            loop(s, w, h);
        }
        static void overscan(Object wm, int rot, int h) {     // (display 0, left, top, right, bottom)
            String[] ps = new String[5]; Object[] as = new Object[5];
            for (int i = 0; i < 5; i++) { ps[i] = "int"; as[i] = Integer.valueOf(0); }
            as[rot == 0 ? 4 : rot] = Integer.valueOf(h);      // the natural side at the bottom now
            inv(wm, "setOverscan", ps, as);
        }
        static Object surface(int w, int h, int y) {
            Class scc = Class.forName("android.view.SurfaceControl");
            Class ssc = Class.forName("android.view.SurfaceSession");
            Object session = ssc.newInstance();
            Class[] cs = { ssc, Class.forName("java.lang.String"),
                           Integer.TYPE, Integer.TYPE, Integer.TYPE, Integer.TYPE };
            Object[] as = { session, "hassmic-band", Integer.valueOf(w), Integer.valueOf(h),
                            Integer.valueOf(-1) /* PixelFormat.OPAQUE */, Integer.valueOf(4) /* HIDDEN */ };
            Object sc = scc.getConstructor(cs).newInstance(as);
            sinv0("android.view.SurfaceControl", "openTransaction");
            w1(sc, "setLayer", "int", Integer.valueOf(2000000000));        // above every window
            w2(sc, "setPosition", "float", "float", Float.valueOf("0"),
               Float.valueOf(String.valueOf(Integer.valueOf(y))));
            os(sc, "show");
            sinv0("android.view.SurfaceControl", "closeTransaction");
            Object s = Class.forName("android.view.Surface").newInstance();
            w1(s, "copyFrom", "android.view.SurfaceControl", sc);
            return s;
        }
        static void loop(Object s, int w, int h) {
            Method lock = meth("android.view.Surface", "lockCanvas", new String[]{ "android.graphics.Rect" });
            Method post = meth("android.view.Surface", "unlockCanvasAndPost",
                               new String[]{ "android.graphics.Canvas" });
            Method draw = meth("android.graphics.Canvas", "drawBitmap", new String[]{ "[I", "int", "int", "int",
                               "int", "int", "int", "boolean", "android.graphics.Paint" });
            InputStream in = System.in;
            int[] px = new int[w * h]; byte[] buf = new byte[w * h * 4];
            Object[] lockArgs = new Object[1];                  // { null }: the whole surface
            Object[] drawArgs = { px, Integer.valueOf(0), Integer.valueOf(w), Integer.valueOf(0), Integer.valueOf(0),
                                  Integer.valueOf(w), Integer.valueOf(h), Boolean.FALSE, null };
            while (full(in, buf)) {
                ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN).asIntBuffer().get(px);   // ARGB, row by row
                Object c = lock.invoke(s, lockArgs);
                draw.invoke(c, drawArgs);                       // offset 0, stride w, at 0,0, w x h, no alpha
                post.invoke(s, new Object[]{ c });
            }                                                    // end of stdin: the layer goes with the process
        }
        static boolean full(InputStream in, byte[] buf) {     // a whole frame, or false at the end
            for (int got = 0; got < buf.length; ) {
                int n = in.read(buf, got, buf.length - got);
                if (n < 0) return false;
                got += n;
            }
            return true;
        }
        static Method meth(String c, String n, String[] ps) {
            Class[] cs = new Class[ps.length];
            for (int i = 0; i < ps.length; i++) cs[i] = cls(ps[i]);
            return Class.forName(c).getMethod(n, cs);
        }
        static Class cls(String n) {             // Class.forName knows no primitive types ("[I" it does)
            if (n.equals("int")) return Integer.TYPE;
            if (n.equals("boolean")) return Boolean.TYPE;
            if (n.equals("float")) return Float.TYPE;
            return Class.forName(n);
        }
        static int fint(Object o, String f) { return ((Integer) o.getClass().getField(f).get(o)).intValue(); }
        static void out(String s) { System.out.println(s); System.out.flush(); }
        static Object sinv0(String c, String n) {                          // a static method, no arguments
            return Class.forName(c).getMethod(n, new Class[0]).invoke(null, new Object[0]);
        }
        // inv, sinv, w1, w2, os: as Wifictl's
    }

python3 tools/mkringbar.py            rebuild scripts/device/ringbar.dex
python3 tools/mkringbar.py --check    only re-read and check the one in the tree
    --framework=firmware/checkers/rootfs   also against the firmware's boot class path (constructors,
                                           methods, fields, PixelFormat.OPAQUE, SurfaceControl.HIDDEN)

What the code computes is tests/unit/ringbar_run.py's: it interprets the dex against fakes of the window
manager, the display and SurfaceControl/Surface/Canvas (make unit).  None of this has run on an Echo."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dexasm import (Code, Program, AGET_OBJ, APUT_OBJ, VIRT, DIRECT, STATIC, STATIC_RANGE,  # noqa: E402
                    ADD8, MUL8, SUB, MUL, ADD, IFGE, IFEQZ, IFNEZ, IFLTZ,
                    SB, STR, CLS, OBJ, MTH, INTG, ISTRM, CLSARR, STRARR, OBJARR, BYTES, INTS)
import dexasm                                                  # noqa: E402

ME = "LRingbar;"
OPAQUE, HIDDEN = -1, 4                    # PixelFormat.OPAQUE, SurfaceControl.HIDDEN (--framework checks both)
LAYER = 2000000000                        # above every window (type layer x 10000 + 1000) and the boot animation (2^30)
NAME = "hassmic-band"                     # the layer's name in dumpsys SurfaceFlinger

# the class's own methods: (name, params, return)
SELF = [
    ("main",  (STRARR,), "V"),
    ("overscan", (OBJ, "I", "I"), "V"),
    ("create", ("I", "I", "I"), OBJ),
    ("surface", (OBJ, "I"), OBJ),
    ("loop",  (OBJ, "I", "I"), "V"),
    ("full",  (ISTRM, BYTES), "Z"),
    ("meth",  (STR, STR, STRARR), MTH),
    ("cls",   (STR,), CLS),
    ("inv",   (OBJ, STR, STRARR, OBJARR), OBJ),
    ("sinv",  (STR, STR, STR, OBJ), OBJ),
    ("sinv0", (STR, STR), OBJ),
    ("w1",    (OBJ, STR, STR, OBJ), OBJ),
    ("w2",    (OBJ, STR, STR, STR, OBJ, OBJ), OBJ),
    ("os",    (OBJ, STR), OBJ),
    ("fint",  (OBJ, STR), "I"),
    ("out",   (STR,), "V"),
]
DRAW = ("[I", "int", "int", "int", "int", "int", "int", "boolean", "android.graphics.Paint")


# ---------------------------------------------------------------- the methods, as bytecode
def emit():
    m = []

    def main():
        # a=0 h=1 t=2..5 wm=6 di=7 w=8 dh=9 rot=10 s=11 n=12
        c = Code(13, 1)
        c.aget_at(2, 0, 0)
        c.invoke(STATIC, "int.parse", [2]); c.move_result(1)
        c.const_string(2, "android.os.ServiceManager"); c.const_string(3, "getService")
        c.const_string(4, "java.lang.String"); c.const_string(5, "window")
        c.invoke(STATIC, "sinv", [2, 3, 4, 5]); c.move_result(5, True)
        c.const_string(2, "android.view.IWindowManager$Stub"); c.const_string(3, "asInterface")
        c.const_string(4, "android.os.IBinder")
        c.invoke(STATIC, "sinv", [2, 3, 4, 5]); c.move_result(6, True)
        c.const_string(2, "android.hardware.display.DisplayManagerGlobal"); c.const_string(3, "getInstance")
        c.invoke(STATIC, "sinv0", [2, 3]); c.move_result(2, True)
        c.const_string(3, "getDisplayInfo"); c.const_string(4, "int")
        c.const_4(5, 0); c.invoke(STATIC, "int.value", [5]); c.move_result(5, True)     # Display.DEFAULT_DISPLAY
        c.invoke(STATIC, "w1", [2, 3, 4, 5]); c.move_result(7, True)
        for f, v in (("logicalWidth", 8), ("logicalHeight", 9), ("rotation", 10)):
            c.const_string(2, f); c.invoke(STATIC, "fint", [7, 2]); c.move_result(v)
        c.invoke(STATIC, "overscan", [6, 10, 1])
        c.ifz(IFNEZ, 1, "on")
        c.const_string(2, "off"); c.invoke(STATIC, "out", [2])
        c.ret_void()
        c.label("on")
        c.binop(SUB, 9, 9, 1)                                 # y = dh - h: the band's top row
        c.array_length(12, 0)                                 # argv's length: 2 = "builder", SurfaceControl.Builder (Android 8+)
        c.invoke(STATIC, "create", [8, 1, 12]); c.move_result(11, True)    # the SurfaceControl, which must outlive the loop:
        c.invoke(STATIC, "surface", [11, 9]); c.move_result(12, True)      # its finalizer releases the layer (Android 9: a GC
                                                                          # every ~55 s took the band away, 2026-10-14)
        c.new_instance(2, SB); c.invoke(DIRECT, "SB.new", [2])
        c.const_string(3, "ready "); c.invoke(VIRT, "SB.appendO", [2, 3])
        c.invoke(VIRT, "SB.appendI", [2, 8])
        c.const_string(3, " "); c.invoke(VIRT, "SB.appendO", [2, 3])
        c.invoke(VIRT, "SB.appendI", [2, 1])
        c.invoke(VIRT, "SB.to", [2]); c.move_result(2, True)
        c.invoke(STATIC, "out", [2])
        c.invoke(STATIC, "loop", [12, 8, 1])
        c.invoke(VIRT, "obj.getcls", [11]); c.move_result(2, True)         # a use of it after the loop: live all along
        c.ret_void()
        m.append(("main", c))

    def overscan():
        # wm=0 rot=1 h=2 ps=3 as=4 i=5 t=6 z=7 n=8
        c = Code(9, 3)
        c.const_4(8, 5); c.new_array(3, 8, STRARR); c.new_array(4, 8, OBJARR)
        c.const_string(6, "int")
        c.const_4(7, 0); c.invoke(STATIC, "int.value", [7]); c.move_result(7, True)
        c.const_4(5, 0)
        c.label("loop")
        c.if2(IFGE, 5, 8, "done")
        c.aput(6, 3, 5, APUT_OBJ); c.aput(7, 4, 5, APUT_OBJ)
        c.binop_lit8(ADD8, 5, 5, 1)
        c.goto_("loop")
        c.label("done")
        c.move(5, 1)                                          # the index of the natural side at the bottom:
        c.ifz(IFNEZ, 5, "side")                               # left 1, top 2, right 3 are the rotation itself,
        c.const_4(5, 4)                                       # bottom 4 for rotation 0
        c.label("side")
        c.invoke(STATIC, "int.value", [2]); c.move_result(7, True)
        c.aput(7, 4, 5, APUT_OBJ)
        c.const_string(6, "setOverscan")
        c.invoke(STATIC, "inv", [0, 6, 3, 4])
        c.ret_void()
        m.append(("overscan", c))

    def create():
        # w=0 h=1 n=2 scc=3 ssc=4 session=5 sc=6 t=7 cs=8 as=9 k=10
        c = Code(11, 3)
        c.const_string(7, "android.view.SurfaceControl")
        c.invoke(STATIC, "cls.forName", [7]); c.move_result(3, True)
        c.const_string(7, "android.view.SurfaceSession")
        c.invoke(STATIC, "cls.forName", [7]); c.move_result(4, True)
        c.invoke(VIRT, "cls.newi", [4]); c.move_result(5, True)
        c.const_4(7, 2)
        c.if2(IFGE, 2, 7, "builder")
        # Android 7: new SurfaceControl(session, name, w, h, format, flags)
        c.const_4(10, 6); c.new_array(8, 10, CLSARR); c.new_array(9, 10, OBJARR)
        c.const_4(10, 0); c.aput(4, 8, 10, APUT_OBJ)
        c.const_string(7, "java.lang.String"); c.invoke(STATIC, "cls.forName", [7]); c.move_result(7, True)
        c.const_4(10, 1); c.aput(7, 8, 10, APUT_OBJ)
        c.sget_obj(7, "int.TYPE")
        for k in (2, 3, 4, 5):
            c.const_4(10, k); c.aput(7, 8, 10, APUT_OBJ)
        c.const_4(10, 0); c.aput(5, 9, 10, APUT_OBJ)
        c.const_string(7, NAME); c.const_4(10, 1); c.aput(7, 9, 10, APUT_OBJ)
        for k, v in ((2, 0), (3, 1)):                         # width, height: the parameters
            c.invoke(STATIC, "int.value", [v]); c.move_result(7, True)
            c.const_4(10, k); c.aput(7, 9, 10, APUT_OBJ)
        for k, n in ((4, OPAQUE), (5, HIDDEN)):               # format, flags
            c.lit(7, n); c.invoke(STATIC, "int.value", [7]); c.move_result(7, True)
            c.const_4(10, k); c.aput(7, 9, 10, APUT_OBJ)
        c.invoke(VIRT, "cls.getc", [3, 8]); c.move_result(7, True)
        c.invoke(VIRT, "ctor.newi", [7, 9]); c.move_result(6, True)
        c.ret(6, True)
        # Android 9: the constructor is gone; new SurfaceControl.Builder(session).setName().setSize().setFormat()
        # .setFlags().build() (each setter returns the builder)
        c.label("builder")
        c.const_string(7, "android.view.SurfaceControl$Builder")
        c.invoke(STATIC, "cls.forName", [7]); c.move_result(3, True)      # scc: the Builder class from here on
        c.const_4(10, 1); c.new_array(8, 10, CLSARR); c.new_array(9, 10, OBJARR)
        c.const_4(10, 0); c.aput(4, 8, 10, APUT_OBJ); c.aput(5, 9, 10, APUT_OBJ)
        c.invoke(VIRT, "cls.getc", [3, 8]); c.move_result(7, True)
        c.invoke(VIRT, "ctor.newi", [7, 9]); c.move_result(6, True)       # sc: the builder
        c.const_string(7, "setName"); c.const_string(8, "java.lang.String"); c.const_string(9, NAME)
        c.invoke(STATIC, "w1", [6, 7, 8, 9]); c.move_result(6, True)
        c.invoke(STATIC, "int.value", [0]); c.move_result(8, True)       # w2(builder, "setSize", "int", "int", w, h):
        c.invoke(STATIC, "int.value", [1]); c.move_result(9, True)       # six consecutive registers, 4..9
        c.move(4, 6, True)
        c.const_string(5, "setSize"); c.const_string(6, "int"); c.const_string(7, "int")
        c.invoke_range(STATIC_RANGE, "w2", 4, 6); c.move_result(6, True)
        for name, v in (("setFormat", OPAQUE), ("setFlags", HIDDEN)):
            c.const_string(7, name); c.const_string(8, "int")
            c.lit(9, v); c.invoke(STATIC, "int.value", [9]); c.move_result(9, True)
            c.invoke(STATIC, "w1", [6, 7, 8, 9]); c.move_result(6, True)
        c.const_string(7, "build"); c.invoke(STATIC, "os", [6, 7]); c.move_result(6, True)
        c.ret(6, True)
        m.append(("create", c))

    def surface():
        # sc=0 y=1 t=2 cs=3 as=4 s=5; w2's arguments 6..11
        c = Code(12, 2)
        c.const_string(2, "android.view.SurfaceControl")
        c.const_string(3, "openTransaction"); c.invoke(STATIC, "sinv0", [2, 3])
        c.const_string(3, "setLayer"); c.const_string(4, "int")
        c.lit(5, LAYER); c.invoke(STATIC, "int.value", [5]); c.move_result(5, True)
        c.invoke(STATIC, "w1", [0, 3, 4, 5])
        c.move(6, 0, True); c.const_string(7, "setPosition")
        c.const_string(8, "float"); c.const_string(9, "float")
        c.const_string(10, "0"); c.invoke(STATIC, "float.value", [10]); c.move_result(10, True)
        c.invoke(STATIC, "int.value", [1]); c.move_result(11, True)
        c.invoke(STATIC, "str.valueO", [11]); c.move_result(11, True)
        c.invoke(STATIC, "float.value", [11]); c.move_result(11, True)
        c.invoke_range(STATIC_RANGE, "w2", 6, 6)
        c.const_string(3, "show"); c.invoke(STATIC, "os", [0, 3])
        c.const_string(3, "closeTransaction"); c.invoke(STATIC, "sinv0", [2, 3])
        c.const_string(3, "android.view.Surface")
        c.invoke(STATIC, "cls.forName", [3]); c.move_result(3, True)
        c.invoke(VIRT, "cls.newi", [3]); c.move_result(3, True)
        c.const_string(4, "copyFrom"); c.const_string(5, "android.view.SurfaceControl")
        c.invoke(STATIC, "w1", [3, 4, 5, 0])
        c.ret(3, True)
        m.append(("surface", c))

    def loop():
        # s=0 w=1 h=2 lock=3 post=4 draw=5 in=6 buf=7 px=8 la=9 da=10 c=11 t=12 k=13 x=14
        c = Code(15, 3)

        def meth(dst, cls, name, ps):
            c.lit(13, len(ps)); c.new_array(12, 13, STRARR)
            for i, p in enumerate(ps):
                c.const_string(14, p); c.lit(13, i); c.aput(14, 12, 13, APUT_OBJ)
            c.const_string(13, cls); c.const_string(14, name)
            c.invoke(STATIC, "meth", [13, 14, 12]); c.move_result(dst, True)
        meth(3, "android.view.Surface", "lockCanvas", ("android.graphics.Rect",))
        meth(4, "android.view.Surface", "unlockCanvasAndPost", ("android.graphics.Canvas",))
        meth(5, "android.graphics.Canvas", "drawBitmap", DRAW)
        c.sget_obj(6, "sys.in")
        c.binop(MUL, 13, 1, 2)                                # w * h pixels
        c.new_array(8, 13, INTS)
        c.binop_lit8(MUL8, 13, 13, 4)                         # 4 bytes each
        c.new_array(7, 13, BYTES)
        c.const_4(13, 1); c.new_array(9, 13, OBJARR)          # { null }: lockCanvas(null), the whole surface
        c.lit(13, len(DRAW)); c.new_array(10, 13, OBJARR)     # drawArgs; [8], the Paint, stays null
        c.const_4(13, 0); c.aput(8, 10, 13, APUT_OBJ)         # colors
        c.const_4(12, 0); c.invoke(STATIC, "int.value", [12]); c.move_result(12, True)
        for k in (1, 3, 4):                                   # offset, x, y
            c.const_4(13, k); c.aput(12, 10, 13, APUT_OBJ)
        c.invoke(STATIC, "int.value", [1]); c.move_result(12, True)
        for k in (2, 5):                                      # stride, width
            c.const_4(13, k); c.aput(12, 10, 13, APUT_OBJ)
        c.invoke(STATIC, "int.value", [2]); c.move_result(12, True)
        c.const_4(13, 6); c.aput(12, 10, 13, APUT_OBJ)        # height
        c.sget_obj(12, "bool.F"); c.const_4(13, 7); c.aput(12, 10, 13, APUT_OBJ)    # hasAlpha: the band is opaque
        c.label("frame")
        c.invoke(STATIC, "full", [6, 7]); c.move_result(12)
        c.ifz(IFNEZ, 12, "draw")
        c.ret_void()                                          # end of stdin
        c.label("draw")
        c.invoke(STATIC, "bb.wrap", [7]); c.move_result(12, True)
        c.sget_obj(13, "order.LE"); c.invoke(VIRT, "bb.order", [12, 13]); c.move_result(12, True)
        c.invoke(VIRT, "bb.asint", [12]); c.move_result(12, True)
        c.invoke(VIRT, "ib.get", [12, 8])
        c.invoke(VIRT, "m.invoke", [3, 0, 9]); c.move_result(11, True)
        c.invoke(VIRT, "m.invoke", [5, 11, 10])
        c.const_4(13, 1); c.new_array(12, 13, OBJARR); c.const_4(13, 0); c.aput(11, 12, 13, APUT_OBJ)
        c.invoke(VIRT, "m.invoke", [4, 0, 12])
        c.goto_("frame")
        m.append(("loop", c))

    def full():
        # in=0 buf=1 got=2 len=3 n=4 rest=5
        c = Code(6, 2)
        c.array_length(3, 1)
        c.const_4(2, 0)
        c.label("loop")
        c.if2(IFGE, 2, 3, "whole")
        c.binop(SUB, 5, 3, 2)
        c.invoke(VIRT, "in.read", [0, 1, 2, 5]); c.move_result(4)
        c.ifz(IFLTZ, 4, "end")                                # -1: hassmic closed the pipe
        c.binop(ADD, 2, 2, 4)
        c.goto_("loop")
        c.label("end")
        c.const_4(4, 0); c.ret(4, False)
        c.label("whole")
        c.const_4(4, 1); c.ret(4, False)
        m.append(("full", c))

    def meth():
        # c=0 n=1 ps=2 k=3 cs=4 i=5 len=6 t=7
        c = Code(8, 3)
        c.array_length(6, 2)
        c.new_array(4, 6, CLSARR)
        c.const_4(5, 0)
        c.label("loop")
        c.if2(IFGE, 5, 6, "done")
        c.aget(7, 2, 5, AGET_OBJ)
        c.invoke(STATIC, "cls", [7]); c.move_result(7, True)
        c.aput(7, 4, 5, APUT_OBJ)
        c.binop_lit8(ADD8, 5, 5, 1)
        c.goto_("loop")
        c.label("done")
        c.invoke(STATIC, "cls.forName", [0]); c.move_result(3, True)
        c.invoke(VIRT, "cls.getm", [3, 1, 4]); c.move_result(7, True)
        c.ret(7, True)
        m.append(("meth", c))

    def cls():
        # n=0 t=1
        c = Code(2, 1)
        for name, f in (("int", "int.TYPE"), ("boolean", "bool.TYPE"), ("float", "float.TYPE")):
            c.const_string(1, name); c.invoke(VIRT, "str.eq", [0, 1]); c.move_result(1)
            c.ifz(IFEQZ, 1, "not " + name)
            c.sget_obj(1, f); c.ret(1, True)
            c.label("not " + name)
        c.invoke(STATIC, "cls.forName", [0]); c.move_result(1, True)
        c.ret(1, True)
        m.append(("cls", c))

    def inv():
        # o=0 n=1 ps=2 as=3 cs=4 m=5 i=6 len=7 r=8
        c = Code(9, 4)
        c.array_length(7, 2)
        c.new_array(4, 7, CLSARR)
        c.const_4(6, 0)
        c.label("loop")
        c.if2(IFGE, 6, 7, "done")
        c.aget(8, 2, 6, AGET_OBJ)
        c.invoke(STATIC, "cls", [8]); c.move_result(8, True)
        c.aput(8, 4, 6, APUT_OBJ)
        c.binop_lit8(ADD8, 6, 6, 1)
        c.goto_("loop")
        c.label("done")
        c.invoke(VIRT, "obj.getcls", [0]); c.move_result(7, True)
        c.invoke(VIRT, "cls.getm", [7, 1, 4]); c.move_result(5, True)
        c.const_4(7, 1); c.invoke(VIRT, "m.setacc", [5, 7])   # the binder proxy class is private
        c.invoke(VIRT, "m.invoke", [5, 0, 3]); c.move_result(7, True)
        c.ret(7, True)
        m.append(("inv", c))

    def sinv():
        # c=0 n=1 p=2 a=3 k=4 cs=5 as=6 m=7 z=8
        c = Code(9, 4)
        c.invoke(STATIC, "cls.forName", [0]); c.move_result(4, True)
        c.invoke(STATIC, "cls", [2]); c.move_result(7, True)
        c.const_4(8, 1); c.new_array(5, 8, CLSARR); c.const_4(8, 0); c.aput(7, 5, 8, APUT_OBJ)
        c.const_4(8, 1); c.new_array(6, 8, OBJARR); c.const_4(8, 0); c.aput(3, 6, 8, APUT_OBJ)
        c.invoke(VIRT, "cls.getm", [4, 1, 5]); c.move_result(7, True)
        c.const_4(8, 0)                                       # null: the static call
        c.invoke(VIRT, "m.invoke", [7, 8, 6]); c.move_result(7, True)
        c.ret(7, True)
        m.append(("sinv", c))

    def sinv0():
        # c=0 n=1 k=2 cs=3 as=4 m=5 z=6
        c = Code(7, 2)
        c.invoke(STATIC, "cls.forName", [0]); c.move_result(2, True)
        c.const_4(6, 0); c.new_array(3, 6, CLSARR); c.new_array(4, 6, OBJARR)
        c.invoke(VIRT, "cls.getm", [2, 1, 3]); c.move_result(5, True)
        c.invoke(VIRT, "m.invoke", [5, 6, 4]); c.move_result(5, True)    # v6 still 0: null, the static call
        c.ret(5, True)
        m.append(("sinv0", c))

    def w1():
        # w=0 n=1 p=2 a=3 ps=4 as=5 r=6
        c = Code(7, 4)
        c.const_4(6, 1); c.new_array(4, 6, STRARR); c.const_4(6, 0); c.aput(2, 4, 6, APUT_OBJ)
        c.const_4(6, 1); c.new_array(5, 6, OBJARR); c.const_4(6, 0); c.aput(3, 5, 6, APUT_OBJ)
        c.invoke(STATIC, "inv", [0, 1, 4, 5]); c.move_result(6, True)
        c.ret(6, True)
        m.append(("w1", c))

    def w2():
        # w=0 n=1 p1=2 p2=3 a1=4 a2=5 ps=6 as=7 r=8
        c = Code(9, 6)
        c.const_4(8, 2); c.new_array(6, 8, STRARR)
        c.const_4(8, 0); c.aput(2, 6, 8, APUT_OBJ); c.const_4(8, 1); c.aput(3, 6, 8, APUT_OBJ)
        c.const_4(8, 2); c.new_array(7, 8, OBJARR)
        c.const_4(8, 0); c.aput(4, 7, 8, APUT_OBJ); c.const_4(8, 1); c.aput(5, 7, 8, APUT_OBJ)
        c.invoke(STATIC, "inv", [0, 1, 6, 7]); c.move_result(8, True)
        c.ret(8, True)
        m.append(("w2", c))

    def os_():
        # o=0 n=1 ps=2 as=3 r=4
        c = Code(5, 2)
        c.const_4(4, 0); c.new_array(2, 4, STRARR)
        c.const_4(4, 0); c.new_array(3, 4, OBJARR)
        c.invoke(STATIC, "inv", [0, 1, 2, 3]); c.move_result(4, True)
        c.ret(4, True)
        m.append(("os", c))

    def fint():
        # o=0 f=1 k=2 fl=3
        c = Code(4, 2)
        c.invoke(VIRT, "obj.getcls", [0]); c.move_result(2, True)
        c.invoke(VIRT, "cls.getf", [2, 1]); c.move_result(3, True)
        c.invoke(VIRT, "f.get", [3, 0]); c.move_result(2, True)
        c.check_cast(2, INTG)
        c.invoke(VIRT, "int.intval", [2]); c.move_result(2)
        c.ret(2, False)
        m.append(("fint", c))

    def out():
        # s=0 o=1
        c = Code(2, 1)
        c.sget_obj(1, "sys.out")
        c.invoke(VIRT, "println", [1, 0])
        c.invoke(VIRT, "flush", [1])                          # a pipe: System.out would hold the line back
        c.ret_void()
        m.append(("out", c))

    for f in (main, overscan, create, surface, loop, full, meth, cls, inv, sinv, sinv0, w1, w2, os_, fint, out):
        f()
    return m


# ---------------------------------------------------------------- against the firmware's own framework
# What the reflection asks the framework for (class, method or "<init>", parameter types, static), checked
# against the boot class path of 8149 by --framework, and the ints the code has written in.
SC, SURF, CANVAS = "Landroid/view/SurfaceControl;", "Landroid/view/Surface;", "Landroid/graphics/Canvas;"
SCB = "Landroid/view/SurfaceControl$Builder;"
DMG, DINFO = "Landroid/hardware/display/DisplayManagerGlobal;", "Landroid/view/DisplayInfo;"
REFLECTED = [
    ("Landroid/os/ServiceManager;", "getService", [STR], True),
    ("Landroid/view/IWindowManager$Stub;", "asInterface", ["Landroid/os/IBinder;"], True),
    ("Landroid/view/IWindowManager;", "setOverscan", ["I", "I", "I", "I", "I"], False),
    (DMG, "getInstance", [], True),
    (DMG, "getDisplayInfo", ["I"], False),
    ("Landroid/view/SurfaceSession;", "<init>", [], False),
    (SC, "<init>", ["Landroid/view/SurfaceSession;", STR, "I", "I", "I", "I"], False),
    (SCB, "<init>", ["Landroid/view/SurfaceSession;"], False),
    (SCB, "setName", [STR], False),
    (SCB, "setSize", ["I", "I"], False),
    (SCB, "setFormat", ["I"], False),
    (SCB, "setFlags", ["I"], False),
    (SCB, "build", [], False),
    (SC, "openTransaction", [], True),
    (SC, "closeTransaction", [], True),
    (SC, "setLayer", ["I"], False),
    (SC, "setPosition", ["F", "F"], False),
    (SC, "show", [], False),
    (SURF, "<init>", [], False),
    (SURF, "copyFrom", [SC], False),
    (SURF, "lockCanvas", ["Landroid/graphics/Rect;"], False),
    (SURF, "unlockCanvasAndPost", [CANVAS], False),
    (CANVAS, "drawBitmap", ["[I", "I", "I", "I", "I", "I", "I", "Z", "Landroid/graphics/Paint;"], False),
]
REFLECTED_FIELDS = [
    (DINFO, "logicalWidth", "I"),
    (DINFO, "logicalHeight", "I"),
    (DINFO, "rotation", "I"),
]
CONSTANTS = [
    ("Landroid/graphics/PixelFormat;", "OPAQUE", OPAQUE),
    (SC, "HIDDEN", HIDDEN),
    (SURF, "ROTATION_0", 0), (SURF, "ROTATION_90", 1), (SURF, "ROTATION_180", 2), (SURF, "ROTATION_270", 3),
]

# the constructor of Android 7, the Builder of Android 9 (athena): the code takes the one the caller says (argv[2])
ONLY_IF = {(SC, "<init>", ("Landroid/view/SurfaceSession;", STR, "I", "I", "I", "I")): "!" + SCB}
for _n, _p in (("<init>", ("Landroid/view/SurfaceSession;",)), ("setName", (STR,)), ("setSize", ("I", "I")),
               ("setFormat", ("I",)), ("setFlags", ("I",)), ("build", ())):
    ONLY_IF[(SCB, _n, _p)] = SCB

PROG = Program(ME, SELF, reflected=REFLECTED, reflected_fields=REFLECTED_FIELDS, constants=CONSTANTS, only_if=ONLY_IF)

def main():
    dexasm.cli(PROG, emit, __file__, "ringbar.dex")


if __name__ == "__main__":
    main()
