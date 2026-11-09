#!/usr/bin/env python3
"""wakealign.c against tools/wakealign/walign.py (numpy): synthetic models in the shape extract.py writes (MFA's
topologies: 3 states for a phone, 5 for sil and spn; random GMMs, LDA, transition probabilities), audio of noise and
tones; both must find the same end, to the sample.  Then every cut short or altered word or shared file is refused,
words of one acoustic model share its GMMs (added to, not moved), and a shared file made anew for others is refused.
  wakealign_ref.py build/wakealign_test"""
import json, os, shutil, subprocess, sys, tempfile
import numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "wakealign"))
import walign

BIN = sys.argv[1]
FEAT = {"frame_shift": 10, "frame_length": 25, "preemphasis_coefficient": 0.97, "num_mel_bins": 23, "low_frequency": 20,
        "high_frequency": 7800, "num_coefficients": 13, "cepstral_lifter": 22}


def topo(n, rng):
    """[(pdf class, [(dest, log prob)])]: MFA's for a phone (0 -> 1|2|end, 1 -> 1|2, 2 -> end) or for sil/spn"""
    def norm(v): v = np.array(v, float); return np.log(v / v.sum())
    if n == 3:
        return [[(1, 2, -1), norm(rng.uniform(0.2, 1, 3))], [(1, 2), norm(rng.uniform(0.2, 1, 2))], [(-1,), [0.0]]]
    st = [[(0, 1, 2, 3), norm(rng.uniform(0.2, 1, 4))]] + [[(1, 2, 3, 4), norm(rng.uniform(0.2, 1, 4))] for _ in range(3)]
    return st + [[(4, -1), norm(rng.uniform(0.2, 1, 2))]]


def model(rng, nphones, nvar=1, lda=None):
    """extract.py's npz for a made-up word: nvar pronunciations of nphones phones each (lda: another word's, the same
    acoustic model's)"""
    names = ["sil_start"]
    for k in range(nvar):
        names += [f"{k}:w0_sil", f"{k}:w0_start"] + [f"{k}:w{i}" for i in range(1, nphones - 1)] + [f"{k}:wl_sil", f"{k}:wl_spn"]
    names += ["sil_mid", "spn", "sil_end"]
    sil = topo(5, rng); spn = topo(5, rng); hmms = []; npdf = 0; shared = {}
    for n in names:
        kind = "sil" if n.startswith("sil") else "spn" if n == "spn" else n                # sil and spn: one model wherever
        t = sil if kind == "sil" else spn if kind == "spn" else topo(3, rng)
        h = []
        for s, (dests, lps) in enumerate(t):
            key = (kind, s) if kind in ("sil", "spn") else None
            if key and key in shared: pdf = shared[key]
            else:
                pdf = npdf; npdf += 1
                if key: shared[key] = pdf
            h.append([pdf, [[int(d), float(lp)] for d, lp in zip(dests, lps)]])
        hmms.append(h)
    ng = rng.integers(2, 5, npdf); G = int(ng.sum())
    iv = rng.uniform(0.5, 2.0, (G, 40)); means = rng.normal(0, 1.5, (G, 40))
    variants = [dict(phones=["a"] * nphones, probs=[float(rng.uniform(0.3, 1)), float(rng.uniform(0.05, 0.5)), float(rng.uniform(0.5, 2)), float(rng.uniform(0.5, 2))])
                for _ in range(nvar)]
    path = tempfile.mktemp(suffix=".npz")
    np.savez(path, word="test", variants=json.dumps(variants), chain=np.array(names), hmms=json.dumps(hmms),
             pdfs=np.arange(npdf), ngauss=ng, gconsts=rng.normal(-40, 5, G), means_invvars=means * iv, inv_vars=iv,
             lda=rng.normal(0, 0.08, (40, 91)) if lda is None else lda, features=json.dumps(FEAT),
             bracketed_probs=np.array([0.99, rng.uniform(0.2, 0.9), rng.uniform(0.5, 2), rng.uniform(0.2, 1)]))
    return walign.Model(path)


def audio(rng):
    """noise, then a few tones and noise bursts of random lengths, then noise: 1-2.5 s"""
    parts = [rng.normal(0, 30, int(rng.uniform(0.2, 0.6) * 16000))]
    for _ in range(rng.integers(2, 6)):
        n = int(rng.uniform(0.05, 0.3) * 16000); t = np.arange(n) / 16000
        parts.append(rng.uniform(500, 6000) * np.sin(2 * np.pi * rng.uniform(150, 3000) * t) + rng.normal(0, rng.uniform(30, 2000), n))
    parts.append(rng.normal(0, 30, int(rng.uniform(0.2, 0.6) * 16000)))
    return np.clip(np.concatenate(parts), -32768, 32767).astype("<i2")


def run(wa, x):
    raw = tempfile.mktemp(suffix=".raw"); x.tofile(raw)
    r = subprocess.run([BIN, wa, raw], capture_output=True, text=True); os.unlink(raw)
    return r.returncode, r.stdout.strip()


def refused(wa, path, data):
    """wa (or its shared file) with data instead: refused (exit 2)?  The file put back after"""
    keep = open(path, "rb").read(); open(path, "wb").write(data)
    try: return run(wa, audio(rng))[0] == 2
    finally: open(path, "wb").write(keep)


bad = 0; same = 0; total = 0; ends = 0
rng = np.random.default_rng(5); tmp = tempfile.mkdtemp()
for k in range(12):
    m = walign.compact(model(rng, int(rng.integers(2, 7)), int(rng.integers(1, 4))), int(rng.integers(2, 5)))
    wa, was = os.path.join(tmp, f"w{k}.wa"), os.path.join(tmp, f"s{k}.was"); walign.write_wa(m, wa, was)
    g = walign.build(m)
    for _ in range(10):
        x = audio(rng); py = walign.word_end(m, x, g); code, c = run(wa, x)
        total += 1; ends += py >= 0
        if code == 0 and int(c) == py: same += 1
        else:
            bad = 1; print(f"FAIL model {k}: C {code}/{c}, Python {py}")
    if k == 0:                                          # either file cut short anywhere, or a field altered, is refused
        n = 0; r = 0
        for path in (wa, was):
            data = open(path, "rb").read(); cuts = sorted(set(range(0, 200, 7)) | set(range(200, len(data), max(1, len(data) // 40))))
            for cut in cuts: r += refused(wa, path, data[:cut]); n += 1
        w = open(wa, "rb").read(); nl = w[8]; refs = 12 + nl + 4          # the word's first GMM: place, check
        s = open(was, "rb").read(); rows, cols = 40, 91; index = 48 + 8 + rows * cols * 4 + 8
        for path, data, pos, val in ((wa, w, 4, 2),             # version 2 (one file a word) is not read
                                     (wa, w, 8, 99),            # the shared file's name longer than there is
                                     (wa, w, 12, ord("/")),     # ... or a path
                                     (wa, w, refs + 3, 0x7f),   # a GMM it does not have
                                     (wa, w, refs + 4, w[refs + 4] ^ 1),   # its check
                                     (was, s, 4, 2), (was, s, 8, 99),
                                     (was, s, index - 5, 0x7f),            # more GMMs than the file has
                                     (was, s, index + 7, 0x7f),            # the first one's place past the end
                                     (was, s, len(s) - 1, s[-1] ^ 1)):     # a byte of the last one's
            d = bytearray(data); d[pos] = val; r += refused(wa, path, bytes(d)); n += 1
        ok = r == n
        print(("ok   " if ok else "FAIL ") + f"cut short or altered word and shared files refused: {r} of {n}")
        bad |= not ok
ok = same == total and ends > total // 2
print(("ok   " if ok else "FAIL ") + f"wakealign.c and walign.py find the same end: {same} of {total} (an end found in {ends})")
bad |= not ok

# words of one acoustic model in one shared file: a second (and a third) word's GMMs added, the first word's left where
# they were, a GMM both use kept once (sil, spn: the same model's); another acoustic model's word refused there; the
# shared file made anew for the third word alone: the first word's file refused, not read wrong
m1 = walign.compact(model(rng, 4, 2), 3); lda = m1.lda
m2 = walign.compact(model(rng, 5, 1, lda), 3); m2.gconsts[:m1.off[2]] = m1.gconsts[:m1.off[2]]
m2.mi[:m1.off[2]] = m1.mi[:m1.off[2]]; m2.iv[:m1.off[2]] = m1.iv[:m1.off[2]]; m2.ngauss[:2] = m1.ngauss[:2]
m2.off = np.concatenate([[0], np.cumsum(m2.ngauss)]).astype(int)             # its first two GMMs: m1's
w1, w2, w3, sh = (os.path.join(tmp, n) for n in ("one.wa", "two.wa", "three.wa", "lang.was"))
walign.write_wa(m1, w1, sh); n1 = len(walign.read_shared(sh)[3]); walign.write_wa(m1, w1, sh); again = len(walign.read_shared(sh)[3])
walign.write_wa(m2, w2, sh); n2 = len(walign.read_shared(sh)[3])
used = lambda m: len({p for p, _ in walign.build(m)[0]})
xs = [audio(rng) for _ in range(6)]
ends1 = [run(w1, x) for x in xs]; ends2 = [run(w2, x) for x in xs]
ok = (again == n1 and n2 < n1 + used(m2) and all(c == 0 and int(v) == walign.word_end(m1, x) for (c, v), x in zip(ends1, xs))
      and all(c == 0 and int(v) == walign.word_end(m2, x) for (c, v), x in zip(ends2, xs)))
print(("ok   " if ok else "FAIL ") + f"two words in one shared file: {n1} GMMs, {n1} again for the same word, {n2} with the second "
      f"(it uses {used(m2)}); both align as walign.py")
bad |= not ok
other = walign.compact(model(rng, 3), 3)
try: walign.write_wa(other, w3, sh); ok = False
except ValueError: ok = True
print(("ok   " if ok else "FAIL ") + "another acoustic model's word is not put into that shared file")
bad |= not ok
m3 = walign.compact(model(rng, 4, 1, lda), 3); fresh = os.path.join(tmp, "fresh"); os.mkdir(fresh)
walign.write_wa(m3, os.path.join(fresh, "three.wa"), os.path.join(fresh, "lang.was")); shutil.copy(os.path.join(fresh, "lang.was"), sh)
ok = run(w1, xs[0])[0] == 2 and run(os.path.join(fresh, "three.wa"), xs[0])[0] == 0
print(("ok   " if ok else "FAIL ") + "a shared file made anew for another word: the old word's file refused")
bad |= not ok

# speaker adaptation: 12 windows in turn, each aligned with what was learnt so far and then added, as main.c does;
# the same ends, and the same transform at the end (to float32's rounding of near-equal sums), saved and read back
for k in range(3):
    m = walign.compact(model(rng, int(rng.integers(3, 6)), int(rng.integers(1, 3))), 4)
    wa, was = os.path.join(tmp, f"a{k}.wa"), os.path.join(tmp, f"a{k}.was"); walign.write_wa(m, wa, was); g = walign.build(m)
    xs = [audio(rng) for _ in range(12)]; raws = []
    for x in xs: raws.append(tempfile.mktemp(suffix=".raw")); x.tofile(raws[-1])
    st = tempfile.mktemp(suffix=".adapt")
    r = subprocess.run([BIN, "adapt", wa, st] + raws, capture_output=True, text=True)
    ad = walign.Adapt(); py = []
    for x in xs: py.append(walign.word_end(m, x, g, ad.xform, adapt=ad)); ad.estimate()
    c = [int(v) for v in r.stdout.split()]
    D = 40; raw = open(st, "rb").read(); off = 8 + 8 + 4 + 4 + 8 + D * (D + 1) * 8 + D * (D + 1) * (D + 1) * 8
    cx = np.frombuffer(raw[off:off + D * (D + 1) * 4], "<f4").reshape(D, D + 1)
    err = np.abs(cx - ad.xform).max() / np.abs(ad.xform).max() if ad.xform is not None else 0.0
    ok = r.returncode == 0 and c == py and ad.xform is not None and err < 1e-3
    print(("ok   " if ok else "FAIL ") + f"adaptation {k}: C and Python ends {'the same' if c == py else (c, py)}, "
          f"transform after {ad.requests} requests apart by {err:.1e} of its largest, saved and read back (exit {r.returncode})")
    bad |= not ok
    for f in raws + [st]: os.path.exists(f) and os.unlink(f)
shutil.rmtree(tmp)
sys.exit(1 if bad else 0)
