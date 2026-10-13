#!/usr/bin/env python3
"""Runs scripts/device/ringbar.dex on the PC with tests/unit/dexrun.py's Dalvik interpreter, against fakes of
what its reflection reaches: the window manager's binder (overscan), DisplayManagerGlobal's DisplayInfo, and
SurfaceSession/SurfaceControl/Surface/Canvas.  What it checks is what hassmic relies on: argv[1] = rows, the
overscan on the natural side that is at the bottom, a layer of width x rows at the bottom above every window,
"ready <w> <rows>" (flushed) before stdin is read, every whole frame of stdin drawn as it came (little-endian
0xAARRGGBB, rows top to bottom), however the pipe splits it, and the end at EOF; "0" resets the overscan and
says "off".  The reflected names are tools/mkringbar.py's lists, which --framework checks against 8149."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dexrun                                                  # noqa: E402
from dexrun import ROOT, i32                                   # noqa: E402
import mkringbar as mk                                         # noqa: E402  (dexrun put tools/ on the path)

DEX = os.path.join(ROOT, "scripts", "device", "ringbar.dex")


class Fake:
    env = None                                                 # the JavaEnv of the run, set by it

    def log(self, *what):
        self.env.log.append(what)


class WindowManager(Fake):                                     # IWindowManager's binder proxy
    JAVA = "android.view.IWindowManager$Stub$Proxy"
    AS = ("Landroid/view/IWindowManager;",)

    def setOverscan(self, display, left, top, right, bottom):
        self.log("overscan", display, left, top, right, bottom)


class DisplayInfo(Fake):
    JAVA = "android.view.DisplayInfo"

    def __init__(self, w, h, rot):
        self.logicalWidth, self.logicalHeight, self.rotation = w, h, rot


class DisplayManagerGlobal(Fake):
    JAVA = "android.hardware.display.DisplayManagerGlobal"

    def getDisplayInfo(self, display):
        assert display == 0, display
        return DisplayInfo(*self.env.display)


class SurfaceSession(Fake):
    JAVA = "android.view.SurfaceSession"

    def __init__(self):
        self.log("session")


class SurfaceControl(Fake):
    JAVA = "android.view.SurfaceControl"

    def __init__(self, session, name, w, h, fmt, flags):
        assert isinstance(session, SurfaceSession)
        self.env.sc = self
        self.log("surfacecontrol", name, w, h, fmt, flags)

    def in_tx(self, *what):
        assert self.env.tx, f"{what[0]} outside a transaction"
        self.log(*what)

    def setLayer(self, z): self.in_tx("layer", z)
    def setPosition(self, x, y): self.in_tx("position", x, y)
    def show(self): self.in_tx("show")


class Canvas(Fake):                                            # what lockCanvas returns: Surface's own subclass
    JAVA = "android.view.Surface$CompatibleCanvas"
    AS = ("Landroid/graphics/Canvas;",)

    def drawBitmap(self, colors, offset, stride, x, y, w, h, alpha, paint):
        assert self.env.locked is self, "draws on a canvas not locked"
        self.log("draw", list(colors), offset, stride, x, y, w, h, alpha, paint)


class Surface(Fake):
    JAVA = "android.view.Surface"

    def __init__(self):
        self.of = None

    def copyFrom(self, sc):
        assert isinstance(sc, SurfaceControl) and not self.env.tx
        self.of = sc
        self.log("copyFrom")

    def lockCanvas(self, rect):
        assert self.of is self.env.sc and self.env.locked is None, "lockCanvas: not set up, or locked twice"
        self.env.locked = Canvas()
        self.log("lock", rect)
        return self.env.locked

    def unlockCanvasAndPost(self, c):
        assert c is self.env.locked, "posts a canvas it did not lock"
        self.env.locked = None
        self.log("post")


class JavaEnv(dexrun.JavaEnv):
    CLASSES = (WindowManager, DisplayInfo, DisplayManagerGlobal, SurfaceSession, SurfaceControl, Surface, Canvas)
    NAMES = ("android.os.ServiceManager", "android.view.IWindowManager$Stub", "android.os.IBinder",
             "android.graphics.Rect", "android.graphics.Canvas", "android.graphics.Paint", "[I")

    def __init__(self, display, stdin=()):
        super().__init__(mk.PROG, stdin)
        Fake.env = self
        self.display, self.log, self.tx, self.sc, self.locked = display, [], False, None, None
        self.wm, self.dmg = WindowManager(), DisplayManagerGlobal()

    def static_call(self, cls, name, args):
        if (cls, name) == ("android.os.ServiceManager", "getService"):
            assert args == ["window"], args; return "binder:window"
        if (cls, name) == ("android.view.IWindowManager$Stub", "asInterface"):
            assert args == ["binder:window"], args; return self.wm
        if (cls, name) == ("android.hardware.display.DisplayManagerGlobal", "getInstance"):
            return self.dmg
        if cls == "android.view.SurfaceControl" and name in ("openTransaction", "closeTransaction"):
            assert self.tx == (name == "closeTransaction"), f"{name} twice"
            self.tx = name == "openTransaction"
            self.log.append((name,)); return None
        return super().static_call(cls, name, args)



def frame(w, h, seed):
    """a frame as hassmic writes it, and the ints Canvas must get: 0xAARRGGBB, signed as Java has them"""
    px = [(0xff000000 | (i * 2654435761 + seed * 40503) & 0xffffff) if i % 7 else (i * seed) & 0xffffffff
          for i in range(w * h)]                               # some with alpha below 0xff, and 0 too
    return struct.pack("<%dI" % len(px), *px), [i32(p) for p in px]


def main():
    dex = dexrun.Dex(open(DEX, "rb").read(), mk.PROG)
    check = dexrun.Checks()

    def run(argv, display, stdin=()):
        env = JavaEnv(display, stdin)
        dex.run(env, "main", [list(argv)])
        return env

    # the Echo Show 5: a 480x960 panel shown as 960x480, rotation 1
    f1, px1 = frame(960, 10, 1)
    f2, px2 = frame(960, 10, 2)
    env = run(["10"], (960, 480, 1), [f1[:1000], f1[1000:], f2])
    log = env.log
    check(log[0] == ("overscan", 0, 10, 0, 0, 0), "rotation 1: setOverscan(0, 10, 0, 0, 0), the natural left edge")
    check(("surfacecontrol", "hassmic-band", 960, 10, -1, 4) in log,
          "a 960x10 layer, PixelFormat.OPAQUE, created HIDDEN")
    setup = [e for e in log if e[0] in ("openTransaction", "layer", "position", "show", "closeTransaction", "copyFrom")]
    check(setup == [("openTransaction",), ("layer", 2000000000), ("position", 0.0, 470.0), ("show",),
                    ("closeTransaction",), ("copyFrom",)],
          "in one transaction: layer 2000000000, at (0, 470), shown; then the Surface made from it")
    check(env.out == ["ready 960 10"] and env.flushed == 1, "prints \"ready 960 10\", flushed before stdin is read")
    draws = [e for e in log if e[0] == "draw"]
    want = lambda px: ("draw", px, 0, 960, 0, 0, 960, 10, False, None)
    check(len(draws) == 2 and draws[0] == want(px1), "frame 1 (two short reads): exactly its pixels, stride 960, "
          "960x10 at 0,0, no alpha, no Paint")
    check(len(draws) == 2 and draws[1] == want(px2), "frame 2: exactly its pixels")
    frames = [e[0] for e in log if e[0] in ("lock", "draw", "post")]
    check(frames == ["lock", "draw", "post"] * 2 and all(e[1] is None for e in log if e[0] == "lock"),
          "each frame: lockCanvas(null), drawBitmap on that canvas, unlockCanvasAndPost of it")
    check(env.stdin.reads == 4 and env.locked is None, "returns at the end of stdin, nothing left locked")

    env = run(["10"], (960, 480, 1), [f1, f2[:5000]])
    check(len([e for e in env.log if e[0] == "draw"]) == 1, "a frame cut short by the end of stdin is not drawn")

    env = run(["0"], (960, 480, 1), [f1])
    check(env.log == [("overscan", 0, 0, 0, 0, 0)] and env.out == ["off"],
          "\"0\": overscan reset to nothing, prints \"off\" (no band)")
    check(env.stdin.reads == 0 and env.sc is None, "\"0\": no session, no layer, stdin left alone")

    env = run(["10"], (480, 960, 0), [])
    check(env.log[0] == ("overscan", 0, 0, 0, 0, 10), "rotation 0: the band in the bottom overscan")
    check(("surfacecontrol", "hassmic-band", 480, 10, -1, 4) in env.log and ("position", 0.0, 950.0) in env.log
          and env.out == ["ready 480 10"], "rotation 0: 480x10 at (0, 950)")
    for rot, side in ((2, (0, 24, 0, 0)), (3, (0, 0, 24, 0))):     # left, top, right, bottom
        env = run(["24"], (960, 480, rot), [])
        check(env.log[0] == ("overscan", 0) + side, f"rotation {rot}: overscan {side}")

    check.done("ringbar.dex: overscan, layer and frames as expected")


if __name__ == "__main__":
    main()
