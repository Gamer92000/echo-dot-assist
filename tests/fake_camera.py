#!/usr/bin/env python3
"""The PC's stand-in for src/cam/cam.c (HASSMIC_FAKE_CAMERA): its records on stdout, from an H.264 test picture
(testdata/camera_testsrc.h264: Baseline like the Echo's encoder, a key frame a second; looped), paced at 15 fps with the Echo's kind of
pts (a boot clock in us, far from 0), until stdin ends.  The codec config first in a record of its own, as
MediaCodec gives it.  FAKE_CAMERA_LOG: a file it appends "start"/"stop" to; FAKE_CAMERA_FAIL=1: exits at once (a
camera another app has)."""
import os, select, struct, sys, time

FPS = 15
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 2 s of ffmpeg's testsrc, 320x240, Baseline, a key frame a second (CI has no ffmpeg):
#   ffmpeg -f lavfi -i testsrc=size=320x240:rate=15 -t 2 -pix_fmt yuv420p -c:v libx264 -profile:v baseline -g 15 -bf 0
#          -x264-params slices=1 -crf 30 -f h264 testdata/camera_testsrc.h264
SRC = os.path.join(ROOT, "testdata", "camera_testsrc.h264")


def nals(b):
    i, out = b.find(b"\x00\x00\x01"), []
    while i >= 0:
        j = b.find(b"\x00\x00\x01", i + 3)
        k = j if j >= 0 else len(b)
        out.append(b[i + 3:k].rstrip(b"\x00") if j >= 0 else b[i + 3:])
        i = j
    return out


def units(b):
    """(config, [(key, access unit)]) in Annex B with 4 byte start codes, as MediaCodec writes them"""
    config, aus, cur = b"", [], b""
    for n in nals(b):
        t = n[0] & 0x1F
        if t in (7, 8):
            if not aus and not cur: config += b"\x00\x00\x00\x01" + n
            continue
        if t in (1, 5):
            aus.append((t == 5, cur + b"\x00\x00\x00\x01" + n)); cur = b""
        elif t != 9:
            cur += b"\x00\x00\x00\x01" + n          # SEI ahead of its picture
    return config, aus


def rec(t, pts, data):
    sys.stdout.buffer.write(b"HMCV" + bytes([t, 0, 0, 0]) + struct.pack("<IQ", len(data), pts) + data)
    sys.stdout.buffer.flush()


def main():
    log = os.environ.get("FAKE_CAMERA_LOG")
    note = lambda w: log and open(log, "a").write(w + "\n")
    note("start")
    if os.environ.get("FAKE_CAMERA_FAIL"):
        print("camera: the camera did not open (-16): another app has it, or it is refused", file=sys.stderr)
        sys.exit(3)
    config, aus = units(open(SRC, "rb").read())
    pts, t0, i = 52_495_150_266, time.monotonic(), 0
    rec(1, 0, config)
    try:
        while True:
            r, _, _ = select.select([0], [], [], 0)
            if r and not os.read(0, 64):
                break
            key, au = aus[i % len(aus)]
            rec(2 if key else 3, pts, au)
            i += 1; pts += 1_000_000 // FPS
            time.sleep(max(0, t0 + i / FPS - time.monotonic()))
    except BrokenPipeError:
        pass
    note("stop")


if __name__ == "__main__":
    main()
