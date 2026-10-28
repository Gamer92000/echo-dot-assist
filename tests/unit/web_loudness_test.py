#!/usr/bin/env python3
"""web/loudness.js (the settings page's timer sound level) through node, against BS.1770-4 written out here (which
ffmpeg's ebur128 agreed with on Amazon's timer and alarm tones, 2026-10-28): the loudness of a sine, of a sound with
silence the gates leave out, of silence; the ring's beep (main.c BEEP_PEAK) at Alexa's level (app.js TS_LUFS);
matching: a quiet clip turned up, a loud one down, one too peaky held under the ceiling, the limiter never over it; a
clip shorter than a block measured over and over, as the ring plays it."""
import base64, json, math, os, random, re, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
R = 48000
JS = """
const L = require(process.argv[1]), fs = require('fs');
const out = JSON.parse(fs.readFileSync(0, 'utf8')).map(([what, target, b64]) => {
  const b = Buffer.from(b64, 'base64'), x = new Float32Array(b.length / 4);
  for (let i = 0; i < x.length; i++) x[i] = b.readFloatLE(4 * i);
  if (what === 'lufs') return L.lufs(x);
  if (what === 'ring') return L.ringLufs(x);
  const m = L.match(x, target, { loop: what === 'match_ring' });
  return m && { lufs: m.lufs, peak: L.peak(m.out), gain_db: m.gain_db, limited: m.limited, short_db: m.short_db,
                out: Buffer.from(m.out.buffer, m.out.byteOffset, m.out.byteLength).toString('base64') };
});
console.log(JSON.stringify(out));
"""
bad = n = 0


def check(cond, what):
    global bad, n
    n += 1
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: bad += 1


def lufs(x):
    """BS.1770-4 integrated loudness of mono floats at 48 kHz: K-weighting, 400 ms blocks every 100 ms, gates"""
    def biquad(x, b, a):
        y = []; x1 = x2 = y1 = y2 = 0.0
        for v in x:
            o = b[0] * v + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2
            x2, x1, y2, y1 = x1, v, y1, o; y.append(o)
        return y
    y = biquad(x, (1.53512485958697, -2.69169618940638, 1.19839281085285), (1, -1.69065929318241, 0.73248077421585))
    y = biquad(y, (1.0, -2.0, 1.0), (1, -1.99004745483398, 0.99007225036621))
    c = [0.0]
    for v in y: c.append(c[-1] + v * v)
    blk, step = R * 4 // 10, R // 10
    z = [(c[i + blk] - c[i]) / blk for i in range(0, len(y) - blk + 1, step)]
    L = lambda m: -0.691 + 10 * math.log10(m) if m > 0 else -math.inf
    g = [v for v in z if L(v) > -70]
    if not g: return -math.inf
    rel = L(sum(g) / len(g)) - 10
    g = [v for v in g if L(v) > rel]
    return L(sum(g) / len(g))


def node(items):
    """[(what, target, samples)] -> loudness.js's answers; what match() made is measured here ("ref"), not by the code
    under test"""
    payload = json.dumps([[w, t, base64.b64encode(struct.pack(f"<{len(x)}f", *x)).decode()] for w, t, x in items])
    r = subprocess.run(["node", "-e", JS, os.path.join(ROOT, "web/loudness.js")], input=payload, capture_output=True, text=True)
    if r.returncode: print(r.stderr); sys.exit(1)
    res = json.loads(r.stdout)
    for v in res:
        if isinstance(v, dict):
            b = base64.b64decode(v.pop("out")); v["ref"] = lufs(list(struct.unpack(f"<{len(b) // 4}f", b)))
    return res


def sine(f, db, secs): return [10 ** (db / 20) * math.sin(2 * math.pi * f * i / R) for i in range(int(R * secs))]


# main.c's ring blip and app.js's target, as the code has them
main_c = open(os.path.join(ROOT, "src/hassmic/main.c")).read()
app_js = open(os.path.join(ROOT, "web/app.js")).read()
peak_ = int(re.search(r"#define BEEP_PEAK (\d+)", main_c).group(1))
target = float(re.search(r"TS_LUFS = (-?[\d.]+)", app_js).group(1))
N = R * 12 // 100
blip = [int(peak_ * min(1.0, min(i, N - i) / (R * 0.01)) * math.sin(2 * math.pi * (880.0 if i < N // 2 else 1320.0) * i / R)) / 32768 for i in range(N)]
ring = (blip * 3 + [0.0] * int(R * 0.8)) * 6

tone = sine(997, -20, 3)
gated = [0.0] * R * 2 + tone + [0.0] * R * 2              # silence on both sides: the gates leave it out
random.seed(63)
noise = [random.uniform(-0.05, 0.05) for _ in range(R * 3)]
quiet = sine(440, -30, 3)
loud = [max(-0.95, min(0.95, 4 * v)) for v in sine(300, -1, 3)]
peaky = [0.01 * math.sin(2 * math.pi * 440 * i / R) + (0.7 if i % R < 24 else 0) for i in range(R * 4)]    # starts on a click
bursts = [0.025 * math.sin(2 * math.pi * 440 * i / R) + (noise[i] * 10 if i % (R // 2) < 480 else 0) for i in range(R * 3)]   # 10 ms of noise to -6 dBFS twice a second over a quiet tone

short = sine(1000, -20, 0.25)                             # 250 cycles: seamless when looped
js = node([("lufs", 0, tone), ("lufs", 0, gated), ("lufs", 0, [0.0] * R * 2), ("lufs", 0, ring), ("lufs", 0, noise),
           ("match", target, quiet), ("match", target, loud), ("match", target, peaky), ("match", target, bursts), ("match", target, [0.0] * R),
           ("lufs", 0, short), ("ring", 0, short), ("ring", 0, tone), ("match_ring", target, short)])
lt, lg, ls, lr, ln, mq, ml, mp, mb, m0, lsh, rsh, rt, msh = js
ref = lufs(tone)
check(abs(ref - (-23.0)) < 0.05 and abs(lt - ref) < 0.001, f"a 997 Hz sine at -20 dBFS: {lt:.3f} LUFS (reference {ref:.3f}; the standard's 0 dBFS sine is -3.01)")
check(abs(lg - lufs(gated)) < 0.001 and abs(lg - ref) < 0.5, f"silence on both sides gated out (but for the blocks across its edges): {lg:.3f}, {lufs(gated):.3f}")
check(ls is None, f"silence: no loudness ({ls})")
check(abs(ln - lufs(noise)) < 0.001, f"noise: as the reference ({ln:.3f}, {lufs(noise):.3f})")
check(abs(lr - target) < 0.1 and abs(lr - lufs(ring)) < 0.001, f"main.c's ring beep (peak {peak_}) at Alexa's level, {target}: {lr:.2f} LUFS")
check(mq and not mq["limited"] and abs(mq["lufs"] - target) < 0.1 and abs(mq["ref"] - target) < 0.1 and abs(mq["gain_db"] - (target - lufs(quiet))) < 0.01,
      f"a quiet clip turned up to it by the difference, nothing to hold back: {mq and round(mq['gain_db'], 2)} dB, {mq and round(mq['ref'], 2)} LUFS")
check(ml and not ml["limited"] and abs(ml["ref"] - target) < 0.1 and ml["gain_db"] < -10, f"a loud one turned down: {ml and round(ml['gain_db'], 2)} dB")
check(mp and mp["limited"] and mp["peak"] <= 0.8911 and mp["gain_db"] == 24 and abs(mp["ref"] + mp["short_db"] - target) < 0.1,
      f"a quiet one with clicks, the first at its start: the clicks held at -1 dBFS ({mp and 20 * math.log10(mp['peak']):.2f}), turned up 24 dB at most, {mp and round(mp['short_db'], 1)} dB short said")
check(mb and mb["limited"] and mb["peak"] <= 0.8911 and abs(mb["ref"] - target) < 0.15,
      f"bursts of noise: held under the ceiling and still at the level ({mb and (mb['limited'], round(20 * math.log10(mb['peak']), 2), round(mb['ref'], 2))})")
check(m0 is None, "silence: nothing to match")
check(lsh is None and abs(rsh - lufs(sine(1000, -20, 3))) < 0.01, f"0.25 s: no block of its own; over and over as it rings, as 3 s of it ({rsh:.3f})")
check(abs(rt - lt) < 0.001, f"3 s or more: measured as it is ({rt:.3f})")
check(msh and abs(msh["lufs"] - target) < 0.1, f"a 0.25 s clip matched as it rings: {msh and round(msh['lufs'], 2)}")
print(f"{n - bad} of {n} passed")
sys.exit(1 if bad else 0)
