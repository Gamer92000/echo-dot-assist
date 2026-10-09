#!/usr/bin/env python3
"""Fixtures for tests/unit/sounds_test.c: DIR/ui_wakesound.wav (16 kHz mono, 160 samples of 1000) and ZIP, an APK as
far as sounds.c cares: res/raw/stored.wav (48 kHz stereo, 480 frames of 300/-100) stored, the same deflated."""
import os, struct, sys, zipfile


def wav(rate, ch, frames):
    data = b"".join(struct.pack("<" + "h" * len(f), *f) for f in frames)
    fmt = struct.pack("<HHIIHH", 1, ch, rate, rate * ch * 2, ch * 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"LIST" + struct.pack("<I", 3) + b"abc\0" + \
        b"data" + struct.pack("<I", len(data)) + data                 # an odd chunk before the data: padded, skipped
    return b"RIFF" + struct.pack("<I", len(body)) + body


d, z = sys.argv[1], sys.argv[2]
os.makedirs(d, exist_ok=True)
open(os.path.join(d, "ui_wakesound.wav"), "wb").write(wav(16000, 1, [(1000,)] * 160))
stereo = wav(48000, 2, [(300, -100)] * 480)
with zipfile.ZipFile(z, "w") as f:
    f.writestr(zipfile.ZipInfo("AndroidManifest.xml"), b"\0" * 64)
    f.writestr("res/raw/stored.wav", stereo, compress_type=zipfile.ZIP_STORED)
    f.writestr("res/raw/deflated.wav", stereo, compress_type=zipfile.ZIP_DEFLATED)
