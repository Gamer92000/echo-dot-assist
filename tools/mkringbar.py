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
            Object sc = create(w, h, a.length);                 // a[1] = "builder": SurfaceControl.Builder (Android 8+)
            Object s = surface(sc, dh - h);
            out(new StringBuilder().append("ready ").append(w).append(" ").append(h).toString());
            loop(s, w, h);
            sc.getClass();          // the SurfaceControl must stay reachable as long as the layer is wanted: its
                                    // finalizer releases the layer, and the Surface copied from it does not hold it
                                    // (Android 9: a GC every ~55 s took the band away, lockCanvas threw, 2026-10-14).
                                    // A use after the loop keeps it live all along, as a static field would.
        }
        static void overscan(Object wm, int rot, int h) {     // (display 0, left, top, right, bottom)
            String[] ps = new String[5]; Object[] as = new Object[5];
            for (int i = 0; i < 5; i++) { ps[i] = "int"; as[i] = Integer.valueOf(0); }
            as[rot == 0 ? 4 : rot] = Integer.valueOf(h);      // the natural side at the bottom now
            inv(wm, "setOverscan", ps, as);
        }
        static Object create(int w, int h, int n) {
            Class ssc = Class.forName("android.view.SurfaceSession");
            Object session = ssc.newInstance();     // a local is enough: its finalizer only drops a reference to the
                                                    // native SurfaceComposerClient, which the native SurfaceControl
                                                    // holds too (AOSP 7 and 9); the layer goes with the control alone
            if (n < 2) {                            // Android 7: new SurfaceControl(session, name, w, h, format, flags)
                Class[] cs = { ssc, Class.forName("java.lang.String"),
                               Integer.TYPE, Integer.TYPE, Integer.TYPE, Integer.TYPE };
                Object[] as = { session, "hassmic-band", Integer.valueOf(w), Integer.valueOf(h),
                                Integer.valueOf(-1) /* PixelFormat.OPAQUE */, Integer.valueOf(4) /* HIDDEN */ };
                return Class.forName("android.view.SurfaceControl").getConstructor(cs).newInstance(as);
            }
            // Android 9: the constructor is gone; each setter returns the builder
            Object b = Class.forName("android.view.SurfaceControl$Builder")
                           .getConstructor(new Class[]{ ssc }).newInstance(new Object[]{ session });
            b = w1(b, "setName", "java.lang.String", "hassmic-band");
            b = w2(b, "setSize", "int", "int", Integer.valueOf(w), Integer.valueOf(h));
            b = w1(b, "setFormat", "int", Integer.valueOf(-1));
            b = w1(b, "setFlags", "int", Integer.valueOf(4));
            return os(b, "build");
        }
        static Object surface(Object sc, int y) {
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
                    ADD8, MUL8, SUB, MUL, ADD, IFGE, IFEQZ, IFNEZ, IFLTZ, DIV8,
                    SB, STR, CLS, OBJ, MTH, INTG, ISTRM, CLSARR, STRARR, OBJARR, BYTES, INTS)
import dexasm                                                  # noqa: E402

ME = "LRingbar;"
OPAQUE, TRANSLUCENT, HIDDEN = -1, -3, 4     # PixelFormat.OPAQUE/TRANSLUCENT, SurfaceControl.HIDDEN (--framework checks them)
LAYER = 2000000000                        # above every window (type layer x 10000 + 1000) and the boot animation (2^30)
NAME = "hassmic-band"                     # the layer's name in dumpsys SurfaceFlinger (the text layer's too)

# the class's own methods: (name, params, return)
SELF = [
    ("main",  (STRARR,), "V"),
    ("overscan", (OBJ, "I", "I"), "V"),
    ("create", ("I", "I", "I", "I"), OBJ),
    ("surface", (OBJ, "I", "I"), OBJ),
    ("loop",  (OBJ, "I", "I", "I"), "V"),
    ("srcpaint", (), OBJ),
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
        # a=0 h=1 t=2..5 wm=6 di=7 w=8 dh=9 rot=10 s=11 n=12 fmt=13 layer=14 alpha=15
        c = Code(16, 1)
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
        c.ifz(IFLTZ, 1, "text")                               # -p: the text layer, p % of the screen's height
        c.invoke(STATIC, "overscan", [6, 10, 1])
        c.ifz(IFNEZ, 1, "on")
        c.label("off")
        c.const_string(2, "off"); c.invoke(STATIC, "out", [2])
        c.ret_void()
        c.label("on")
        c.binop(SUB, 9, 9, 1)                                 # y = dh - h: the band's top row
        c.lit(13, OPAQUE); c.lit(14, LAYER); c.const_4(15, 0)
        c.goto_("go")
        # the text layer: no overscan (the apps keep the screen), see-through, just under the band, standing on the
        # screen's bottom as the band does (the band covers its last rows)
        c.label("text")
        c.const_4(2, 0); c.binop(SUB, 1, 2, 1)                # p = -h
        c.binop(MUL, 1, 9, 1); c.binop_lit8(DIV8, 1, 1, 100)  # its rows: dh * p / 100
        c.ifz(IFEQZ, 1, "off")
        c.binop(SUB, 9, 9, 1)
        c.lit(13, TRANSLUCENT); c.lit(14, LAYER - 1); c.const_4(15, 1)
        c.label("go")
        c.array_length(12, 0)                                 # argv's length: 2 = "builder", SurfaceControl.Builder (Android 8+)
        c.invoke(STATIC, "create", [8, 1, 12, 13]); c.move_result(11, True)  # the SurfaceControl, which must outlive the loop:
        c.invoke(STATIC, "surface", [11, 9, 14]); c.move_result(12, True)    # its finalizer releases the layer (Android 9: a GC
                                                                            # every ~55 s took the band away, 2026-10-14)
        c.new_instance(2, SB); c.invoke(DIRECT, "SB.new", [2])
        c.const_string(3, "ready "); c.invoke(VIRT, "SB.appendO", [2, 3])
        c.invoke(VIRT, "SB.appendI", [2, 8])
        c.const_string(3, " "); c.invoke(VIRT, "SB.appendO", [2, 3])
        c.invoke(VIRT, "SB.appendI", [2, 1])
        c.invoke(VIRT, "SB.to", [2]); c.move_result(2, True)
        c.invoke(STATIC, "out", [2])
        c.invoke(STATIC, "loop", [12, 8, 1, 15])
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
        # w=0 h=1 n=2 fmt=3 scc=4 ssc=5 session=6 sc=7 t=8 cs=9 as=10 k=11
        c = Code(12, 4)
        c.const_string(8, "android.view.SurfaceControl")
        c.invoke(STATIC, "cls.forName", [8]); c.move_result(4, True)
        c.const_string(8, "android.view.SurfaceSession")
        c.invoke(STATIC, "cls.forName", [8]); c.move_result(5, True)
        c.invoke(VIRT, "cls.newi", [5]); c.move_result(6, True)
        c.const_4(8, 2)
        c.if2(IFGE, 2, 8, "builder")
        # Android 7: new SurfaceControl(session, name, w, h, format, flags)
        c.const_4(11, 6); c.new_array(9, 11, CLSARR); c.new_array(10, 11, OBJARR)
        c.const_4(11, 0); c.aput(5, 9, 11, APUT_OBJ)
        c.const_string(8, "java.lang.String"); c.invoke(STATIC, "cls.forName", [8]); c.move_result(8, True)
        c.const_4(11, 1); c.aput(8, 9, 11, APUT_OBJ)
        c.sget_obj(8, "int.TYPE")
        for k in (2, 3, 4, 5):
            c.const_4(11, k); c.aput(8, 9, 11, APUT_OBJ)
        c.const_4(11, 0); c.aput(6, 10, 11, APUT_OBJ)
        c.const_string(8, NAME); c.const_4(11, 1); c.aput(8, 10, 11, APUT_OBJ)
        for k, v in ((2, 0), (3, 1), (4, 3)):                 # width, height, format: the parameters
            c.invoke(STATIC, "int.value", [v]); c.move_result(8, True)
            c.const_4(11, k); c.aput(8, 10, 11, APUT_OBJ)
        c.lit(8, HIDDEN); c.invoke(STATIC, "int.value", [8]); c.move_result(8, True)    # flags
        c.const_4(11, 5); c.aput(8, 10, 11, APUT_OBJ)
        c.invoke(VIRT, "cls.getc", [4, 9]); c.move_result(8, True)
        c.invoke(VIRT, "ctor.newi", [8, 10]); c.move_result(7, True)
        c.ret(7, True)
        # Android 9: the constructor is gone; new SurfaceControl.Builder(session).setName().setSize().setFormat()
        # .setFlags().build() (each setter returns the builder)
        c.label("builder")
        c.const_string(8, "android.view.SurfaceControl$Builder")
        c.invoke(STATIC, "cls.forName", [8]); c.move_result(4, True)      # scc: the Builder class from here on
        c.const_4(11, 1); c.new_array(9, 11, CLSARR); c.new_array(10, 11, OBJARR)
        c.const_4(11, 0); c.aput(5, 9, 11, APUT_OBJ); c.aput(6, 10, 11, APUT_OBJ)
        c.invoke(VIRT, "cls.getc", [4, 9]); c.move_result(8, True)
        c.invoke(VIRT, "ctor.newi", [8, 10]); c.move_result(7, True)       # sc: the builder
        c.const_string(8, "setName"); c.const_string(9, "java.lang.String"); c.const_string(10, NAME)
        c.invoke(STATIC, "w1", [7, 8, 9, 10]); c.move_result(7, True)
        c.invoke(STATIC, "int.value", [0]); c.move_result(9, True)       # w2(builder, "setSize", "int", "int", w, h):
        c.invoke(STATIC, "int.value", [1]); c.move_result(10, True)      # six consecutive registers, 5..10
        c.move(5, 7, True)
        c.const_string(6, "setSize"); c.const_string(7, "int"); c.const_string(8, "int")
        c.invoke_range(STATIC_RANGE, "w2", 5, 6); c.move_result(7, True)
        c.const_string(8, "setFormat"); c.const_string(9, "int")
        c.invoke(STATIC, "int.value", [3]); c.move_result(10, True)
        c.invoke(STATIC, "w1", [7, 8, 9, 10]); c.move_result(7, True)
        c.const_string(8, "setFlags"); c.const_string(9, "int")
        c.lit(10, HIDDEN); c.invoke(STATIC, "int.value", [10]); c.move_result(10, True)
        c.invoke(STATIC, "w1", [7, 8, 9, 10]); c.move_result(7, True)
        c.const_string(8, "build"); c.invoke(STATIC, "os", [7, 8]); c.move_result(7, True)
        c.ret(7, True)
        m.append(("create", c))

    def surface():
        # sc=0 y=1 layer=2 t=3 cs=4 as=5 s=6; w2's arguments 7..12
        c = Code(13, 3)
        c.const_string(3, "android.view.SurfaceControl")
        c.const_string(4, "openTransaction"); c.invoke(STATIC, "sinv0", [3, 4])
        c.const_string(4, "setLayer"); c.const_string(5, "int")
        c.invoke(STATIC, "int.value", [2]); c.move_result(6, True)
        c.invoke(STATIC, "w1", [0, 4, 5, 6])
        c.move(7, 0, True); c.const_string(8, "setPosition")
        c.const_string(9, "float"); c.const_string(10, "float")
        c.const_string(11, "0"); c.invoke(STATIC, "float.value", [11]); c.move_result(11, True)
        c.invoke(STATIC, "int.value", [1]); c.move_result(12, True)
        c.invoke(STATIC, "str.valueO", [12]); c.move_result(12, True)
        c.invoke(STATIC, "float.value", [12]); c.move_result(12, True)
        c.invoke_range(STATIC_RANGE, "w2", 7, 6)
        c.const_string(4, "show"); c.invoke(STATIC, "os", [0, 4])
        c.const_string(4, "closeTransaction"); c.invoke(STATIC, "sinv0", [3, 4])
        c.const_string(4, "android.view.Surface")
        c.invoke(STATIC, "cls.forName", [4]); c.move_result(4, True)
        c.invoke(VIRT, "cls.newi", [4]); c.move_result(4, True)
        c.const_string(5, "copyFrom"); c.const_string(6, "android.view.SurfaceControl")
        c.invoke(STATIC, "w1", [4, 5, 6, 0])
        c.ret(4, True)
        m.append(("surface", c))

    def loop():
        # s=0 w=1 h=2 alpha=3 lock=4 post=5 draw=6 in=7 buf=8 px=9 la=10 da=11 c=12 t=13 k=14 x=15
        c = Code(16, 4)

        def meth(dst, cls, name, ps):
            c.lit(14, len(ps)); c.new_array(13, 14, STRARR)
            for i, p in enumerate(ps):
                c.const_string(15, p); c.lit(14, i); c.aput(15, 13, 14, APUT_OBJ)
            c.const_string(14, cls); c.const_string(15, name)
            c.invoke(STATIC, "meth", [14, 15, 13]); c.move_result(dst, True)
        meth(4, "android.view.Surface", "lockCanvas", ("android.graphics.Rect",))
        meth(5, "android.view.Surface", "unlockCanvasAndPost", ("android.graphics.Canvas",))
        meth(6, "android.graphics.Canvas", "drawBitmap", DRAW)
        c.sget_obj(7, "sys.in")
        c.binop(MUL, 14, 1, 2)                                # w * h pixels
        c.new_array(9, 14, INTS)
        c.binop_lit8(MUL8, 14, 14, 4)                         # 4 bytes each
        c.new_array(8, 14, BYTES)
        c.const_4(14, 1); c.new_array(10, 14, OBJARR)         # { null }: lockCanvas(null), the whole surface
        c.lit(14, len(DRAW)); c.new_array(11, 14, OBJARR)     # drawArgs; [8], the Paint, null for the band
        c.const_4(14, 0); c.aput(9, 11, 14, APUT_OBJ)         # colors
        c.const_4(13, 0); c.invoke(STATIC, "int.value", [13]); c.move_result(13, True)
        for k in (1, 3, 4):                                   # offset, x, y
            c.const_4(14, k); c.aput(13, 11, 14, APUT_OBJ)
        c.invoke(STATIC, "int.value", [1]); c.move_result(13, True)
        for k in (2, 5):                                      # stride, width
            c.const_4(14, k); c.aput(13, 11, 14, APUT_OBJ)
        c.invoke(STATIC, "int.value", [2]); c.move_result(13, True)
        c.const_4(14, 6); c.aput(13, 11, 14, APUT_OBJ)        # height
        c.sget_obj(13, "bool.F"); c.const_4(14, 7); c.aput(13, 11, 14, APUT_OBJ)    # hasAlpha: the band is opaque
        c.ifz(IFEQZ, 3, "frame")
        # the text layer: its pixels have alpha, and each frame replaces the last one whole (Paint in SRC mode), the
        # see-through parts too; over (the default) they would keep what the buffer showed before
        c.sget_obj(13, "bool.T"); c.const_4(14, 7); c.aput(13, 11, 14, APUT_OBJ)
        c.invoke(STATIC, "srcpaint", []); c.move_result(13, True)
        c.lit(14, 8); c.aput(13, 11, 14, APUT_OBJ)
        c.label("frame")
        c.invoke(STATIC, "full", [7, 8]); c.move_result(13)
        c.ifz(IFNEZ, 13, "draw")
        c.ret_void()                                          # end of stdin
        c.label("draw")
        c.invoke(STATIC, "bb.wrap", [8]); c.move_result(13, True)
        c.sget_obj(14, "order.LE"); c.invoke(VIRT, "bb.order", [13, 14]); c.move_result(13, True)
        c.invoke(VIRT, "bb.asint", [13]); c.move_result(13, True)
        c.invoke(VIRT, "ib.get", [13, 9])
        c.invoke(VIRT, "m.invoke", [4, 0, 10]); c.move_result(12, True)
        c.invoke(VIRT, "m.invoke", [6, 12, 11])
        c.const_4(14, 1); c.new_array(13, 14, OBJARR); c.const_4(14, 0); c.aput(12, 13, 14, APUT_OBJ)
        c.invoke(VIRT, "m.invoke", [5, 0, 13])
        c.goto_("frame")
        m.append(("loop", c))

    def srcpaint():
        # new Paint() with setXfermode(new PorterDuffXfermode(PorterDuff.Mode.SRC)).  p=0 k=1 t=2 cs=3 as=4 mode=5 mc=6
        c = Code(7, 0)
        c.const_string(2, "android.graphics.Paint"); c.invoke(STATIC, "cls.forName", [2]); c.move_result(1, True)
        c.invoke(VIRT, "cls.newi", [1]); c.move_result(0, True)
        c.const_string(2, "android.graphics.PorterDuff$Mode"); c.invoke(STATIC, "cls.forName", [2]); c.move_result(6, True)
        c.const_string(2, "SRC"); c.invoke(VIRT, "cls.getf", [6, 2]); c.move_result(2, True)
        c.const_4(5, 0); c.invoke(VIRT, "f.get", [2, 5]); c.move_result(5, True)      # a static field: get(null)
        c.const_string(2, "android.graphics.PorterDuffXfermode"); c.invoke(STATIC, "cls.forName", [2]); c.move_result(1, True)
        c.const_4(2, 1); c.new_array(3, 2, CLSARR); c.new_array(4, 2, OBJARR)
        c.const_4(2, 0); c.aput(6, 3, 2, APUT_OBJ); c.aput(5, 4, 2, APUT_OBJ)
        c.invoke(VIRT, "cls.getc", [1, 3]); c.move_result(2, True)
        c.invoke(VIRT, "ctor.newi", [2, 4]); c.move_result(2, True)
        c.const_string(1, "setXfermode"); c.const_string(3, "android.graphics.Xfermode")
        c.invoke(STATIC, "w1", [0, 1, 3, 2])
        c.ret(0, True)
        m.append(("srcpaint", c))

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

    for f in (main, overscan, create, surface, loop, srcpaint, full, meth, cls, inv, sinv, sinv0, w1, w2, os_, fint, out):
        f()
    return m


# ---------------------------------------------------------------- against the firmware's own framework
# What the reflection asks the framework for (class, method or "<init>", parameter types, static), checked
# against the boot class path of each firmware --framework names (make unit: every model with the band), and the ints
# the code has written in.
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
    ("Landroid/graphics/Paint;", "<init>", [], False),
    ("Landroid/graphics/Paint;", "setXfermode", ["Landroid/graphics/Xfermode;"], False),
    ("Landroid/graphics/PorterDuffXfermode;", "<init>", ["Landroid/graphics/PorterDuff$Mode;"], False),
]
REFLECTED_FIELDS = [
    (DINFO, "logicalWidth", "I"),
    (DINFO, "logicalHeight", "I"),
    (DINFO, "rotation", "I"),
    ("Landroid/graphics/PorterDuff$Mode;", "SRC", "Landroid/graphics/PorterDuff$Mode;"),
]
CONSTANTS = [
    ("Landroid/graphics/PixelFormat;", "OPAQUE", OPAQUE),
    ("Landroid/graphics/PixelFormat;", "TRANSLUCENT", TRANSLUCENT),
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
