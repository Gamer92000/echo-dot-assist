#!/usr/bin/env python3
"""A made-up track for the live ring's music effects (musiclight.c, ringlive.c), and the ground truth beside it:
124 bpm, 32 bars in stereo at 48 kHz.  Bars 1-4 an intro (pads and hi-hats only, quiet), 5-12 kick and bass come in,
13-16 a build (snare roll speeding up, a noise riser, pads only), 17-28 the drop (kick, snare on 2 and 4, 16th hats
panned right, an arpeggio panned left, chord stabs, bass), 29-32 a breakdown back to pads.  Mastered loud, as pop is
(a soft clipper over the mix).
    tools/ringmusic.py out.raw [truth.json]      s16le stereo 48 kHz; truth: the kick, snare and hat onsets in seconds
tools/ringviz (make build/ringviz) renders what the effects make of it."""
import json, sys
import numpy as np

RATE, BPM, BARS = 48000, 124, 32
BEAT = 60 / BPM
rng = np.random.default_rng(7)


def env(n, attack, decay):
    t = np.arange(n) / RATE
    return np.minimum(1, t / max(attack, 1e-4)) * np.exp(-t / decay)


def kick():
    n = int(0.35 * RATE); t = np.arange(n) / RATE
    f = 45 + 110 * np.exp(-t / 0.03)                       # the pitch drop of an 808-ish kick
    return np.sin(2 * np.pi * np.cumsum(f) / RATE) * env(n, 0.002, 0.12) * 0.9


def snare():
    n = int(0.25 * RATE); t = np.arange(n) / RATE
    body = np.sin(2 * np.pi * 190 * t) * env(n, 0.001, 0.05)
    noise = np.convolve(rng.standard_normal(n), [1, -0.6], "same") * env(n, 0.001, 0.09)
    return (0.5 * body + 0.45 * noise) * 0.8


def hat():
    n = int(0.06 * RATE)
    x = np.diff(rng.standard_normal(n + 1)) * env(n, 0.0005, 0.018)     # high-passed noise
    return x * 0.35


def tone(freq, secs, shape="saw", vol=0.2, attack=0.01, decay=0.5):
    n = int(secs * RATE); t = np.arange(n) / RATE
    ph = (freq * t) % 1
    w = 2 * ph - 1 if shape == "saw" else np.sin(2 * np.pi * freq * t)
    return w * env(n, attack, decay) * vol


def add(buf, at, x, pan=0.0):
    i = int(at * RATE)
    if i >= buf.shape[1]: return
    x = x[:buf.shape[1] - i]
    a = (pan + 1) * np.pi / 4                                   # equal power, the middle at 1 both sides
    buf[0, i:i + len(x)] += x * np.cos(a) * 2 ** 0.5
    buf[1, i:i + len(x)] += x * np.sin(a) * 2 ** 0.5


def main():
    out = sys.argv[1]; truth_f = sys.argv[2] if len(sys.argv) > 2 else None
    total = BARS * 4 * BEAT + 1
    buf = np.zeros((2, int(total * RATE)))
    truth = {"bpm": BPM, "kick": [], "snare": [], "hat": [], "sections": {"intro": [0, 4 * 4 * BEAT], "groove": [16 * BEAT, 48 * BEAT],
             "build": [48 * BEAT, 64 * BEAT], "drop": [64 * BEAT, 112 * BEAT], "breakdown": [112 * BEAT, 128 * BEAT]}}
    chords = [[57, 60, 64], [53, 57, 60], [55, 59, 62], [52, 55, 59]]          # Am F G Em
    midi = lambda m: 440 * 2 ** ((m - 69) / 12)
    for bar in range(BARS):
        b0 = bar * 4 * BEAT; ch = chords[bar % 4]
        drop = 16 <= bar < 28; groove = 4 <= bar < 12; build = 12 <= bar < 16
        pad_vol = 0.05 if drop else 0.07
        for m in ch:                                            # pads: a bar each, soft attack
            add(buf, b0, tone(midi(m), 4 * BEAT, "sin", pad_vol, 0.3, 3.0), pan=-0.2)
        for k in range(8 if not build else 0):                  # 8th hats (16ths in the drop), panned right
            for s in ([0, 0.5] if drop else [0]):
                at = b0 + (k + s) * BEAT / 2
                if bar < 28 or bar >= 28 and k % 2 == 0:
                    add(buf, at, hat() * (0.7 if s else 1.0) * (0.6 if bar < 4 else 1), pan=0.7)
                    truth["hat"].append(round(at, 4))
        if groove or drop:
            for k in range(4):
                add(buf, b0 + k * BEAT, kick()); truth["kick"].append(round(b0 + k * BEAT, 4))
                root = midi(ch[0] - 24)                         # bass on the off-beats
                add(buf, b0 + k * BEAT + BEAT / 2, tone(root, BEAT / 2, "saw", 0.18, 0.005, 0.15))
        if drop:
            for k in (1, 3):
                add(buf, b0 + k * BEAT, snare()); truth["snare"].append(round(b0 + k * BEAT, 4))
            for k in range(16):                                 # the arpeggio, left
                m = ch[k % 3] + 12 * (1 + (k // 3) % 2)
                add(buf, b0 + k * BEAT / 4, tone(midi(m), BEAT / 4, "saw", 0.06, 0.002, 0.08), pan=-0.7)
            for k in (0, 2.5):                                  # stabs
                for m in ch: add(buf, b0 + k * BEAT, tone(midi(m + 12), BEAT / 2, "saw", 0.07, 0.002, 0.12))
        if build:                                               # a snare roll speeding up, a noise riser
            per = [1, 1, 2, 4][bar - 12]
            for k in range(4 * per):
                at = b0 + k * BEAT / per
                add(buf, at, snare() * (0.4 + 0.15 * (bar - 12))); truth["snare"].append(round(at, 4))
            n = int(4 * BEAT * RATE); riser = rng.standard_normal(n) * np.linspace(0, 0.25, n) * (bar - 11) / 4
            add(buf, b0, np.convolve(riser, [1, -0.9], "same"))
    buf = np.tanh(buf * 1.6) / np.tanh(1.6)                      # loud, as it is mastered
    pcm = (buf.T * 0.89 * 32767).astype("<i2")
    pcm.tofile(out)
    if truth_f: json.dump(truth, open(truth_f, "w"))


if __name__ == "__main__":
    main()
