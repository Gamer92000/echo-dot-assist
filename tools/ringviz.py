#!/usr/bin/env python3
"""What musiclight.c heard in a track and what the music effects drew, against the track's ground truth
(tools/ringmusic.py):
    tools/ringviz.py out-dir [truth.json] [--check]
prints the drums' precision and recall (an onset within 50 ms of one written down), the tempo found, how many beats
fell on a kick, and writes out-dir/effects.png: per effect three windows (intro, groove, drop) of 4 s, a row a
frame (30 a second, top to bottom), the ring cut open at LED 1 (left) round to LED 12, as seen; beside each window
the truth (kick red, snare blue, hi-hat grey).  --check (make unit-ringviz): fails below what was measured when
musiclight.c was written (kick F 0.85, snare 0.55, hi-hat 0.85, the tempo within 2 bpm in the groove and the drop,
nine in ten beats on a kick where the kick plays, the accent every 4 beats on a bar or half bar)."""
import csv, json, os, sys
import numpy as np
from PIL import Image, ImageDraw

args = [a for a in sys.argv[1:] if a != "--check"]; checking = "--check" in sys.argv
d = args[0]; truth = json.load(open(args[1])) if len(args) > 1 else None
fails = []
rows = list(csv.DictReader(open(os.path.join(d, "features.csv"))))
col = lambda k: np.array([float(r[k]) for r in rows])
t = col("t")


def onsets(k):
    v = col(k); s = sorted(set(x for x in v if x >= 0)); return np.array(s)


def score(found, want, tol=0.05):
    want = np.array(want); used = set(); tp = 0
    for f in found:
        i = np.argmin(np.abs(want - f)) if len(want) else -1
        if i >= 0 and abs(want[i] - f) <= tol and i not in used: used.add(i); tp += 1
    p = tp / len(found) if len(found) else 0; r = tp / len(want) if len(want) else 0
    return p, r, (2 * p * r / (p + r) if p + r else 0)


if truth:
    for k in ("kick", "snare", "hat"):
        p, r, f = score(onsets(k), truth[k])
        print(f"{k:6s} found {len(onsets(k)):4d} of {len(truth[k]):4d}: precision {p:.2f} recall {r:.2f} F {f:.2f}")
        if f < {"kick": 0.85, "snare": 0.55, "hat": 0.85}[k]: fails.append(f"{k} F {f:.2f}")
    bpm, conf = col("bpm"), col("confidence")
    for name, (a, b) in truth["sections"].items():
        m = (t >= a + 1) & (t < b)
        print(f"{name:10s} bpm {np.median(bpm[m]):6.1f} (truth {truth['bpm']})  confidence {np.median(conf[m]):.2f}")
        if name in ("groove", "drop") and abs(np.median(bpm[m]) - truth["bpm"]) > 2: fails.append(f"{name} bpm {np.median(bpm[m]):.1f}")
    beats = onsets("beat_at"); kicks = np.array(truth["kick"])
    on = [b for b in beats if len(kicks) and np.min(np.abs(kicks - b)) <= 0.06]
    a, b = truth["sections"]["groove"][0] + 3, truth["sections"]["drop"][1]
    inb = [x for x in beats if a <= x < b]; onb = [x for x in on if a <= x < b]
    print(f"beats while the kick plays: {len(onb)} of {len(inb)} on a kick (within 60 ms)")
    with_kick = [x for x in inb if not truth["sections"]["build"][0] <= x < truth["sections"]["build"][1]]
    if len(onb) < 0.9 * len(with_kick): fails.append(f"beats on a kick {len(onb)} of {len(with_kick)}")
    bn = col("beat_no"); bar = 4 * 60 / truth["bpm"]
    downs = sorted(set(round(float(r["beat_at"]), 3) for r in rows if float(r["beat_at"]) >= a and int(r["beat_no"]) % 4 == 0 and float(r["beat_at"]) < b))
    dok = sum(1 for x in downs if min(abs((x % bar)), abs(bar - x % bar)) < 0.06)
    half = sum(1 for x in downs if min(abs((x % (bar / 2))), abs(bar / 2 - x % (bar / 2))) < 0.06)
    print(f"the bar's first beats: {dok} of {len(downs)} on a bar line, {half} on a bar or a half bar (the accent every 4 beats in time)")
    if half < 0.9 * len(downs): fails.append(f"accents {half} of {len(downs)} on a bar or half bar")

effects = sorted(f[:-4] for f in os.listdir(d) if f.endswith(".f32"))
wins = []
if truth:
    s = truth["sections"]; wins = [("intro", s["intro"][0] + 2), ("groove", s["groove"][0] + 6), ("drop", s["drop"][0] + 6)]
else:
    wins = [("start", 2), ("middle", 20), ("later", 40)]
CELL, ROWH, SECS, FPS = 14, 3, 4, 30
W = 12 * CELL + 10; H = SECS * FPS * ROWH
img = Image.new("RGB", (len(wins) * (W + 20) + 160, len(effects) * (H + 40) + 30), (14, 16, 20))
dr = ImageDraw.Draw(img)
for wi, (wn, w0) in enumerate(wins): dr.text((160 + wi * (W + 20), 8), f"{wn} {w0:.1f}-{w0 + SECS:.1f} s", fill=(200, 200, 200))
for ei, e in enumerate(effects):
    fr = np.fromfile(os.path.join(d, e + ".f32"), "<f4").reshape(-1, 12, 3)
    y0 = 30 + ei * (H + 40)
    dr.text((8, y0 + H // 2), e.replace("_", " "), fill=(230, 230, 230))
    for wi, (wn, w0) in enumerate(wins):
        x0 = 160 + wi * (W + 20)
        a = int(w0 * FPS * 1000 / 33 / FPS)                     # frames are 33 ms apart
        seg = fr[a:a + SECS * FPS]
        px = (np.clip(seg, 0, 1) * 255).astype(np.uint8)       # as seen
        big = np.repeat(np.repeat(px, ROWH, axis=0), CELL, axis=1)
        img.paste(Image.fromarray(big, "RGB"), (x0, y0))
        if truth:
            for k, c in (("kick", (255, 60, 60)), ("snare", (80, 140, 255)), ("hat", (130, 130, 130))):
                for x in truth[k]:
                    if w0 <= x < w0 + SECS:
                        yy = y0 + int((x - w0) * 1000 / 33) * ROWH
                        xx = x0 + 12 * CELL + 2 + {"kick": 0, "snare": 3, "hat": 6}[k]
                        dr.line((xx, yy, xx + 2, yy), fill=c, width=2)
img.save(os.path.join(d, "effects.png"))
print("pictures:", os.path.join(d, "effects.png"))
if checking:
    print("ringviz: " + ("FAILED: " + ", ".join(fails) if fails else "all ok"))
    sys.exit(1 if fails else 0)
