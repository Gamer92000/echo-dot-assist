#!/usr/bin/env python3
"""What the speaker adaptation (wakealign.h) gets from learning only from real requests (main.c since 2026-11-17)
rather than from every wake word: measured on the speech corpus with false wakes put in.

Each German voice of testdata/corpus is a household saying its Amazon wake words (Alexa, Computer, Echo, Amazon,
Ziggy, the kinds command, joined, alone) in a fixed random order, in tools/wakealign/corpus_eval.py's room.  A
"television" is another voice whose wake words come in between as false wakes, a quarter or half of all.  Each
television's voice comes either as it is or as a television's speaker gives it (250-4000 Hz, the rest 30 dB down).
Each sequence runs through tools/wakealign/eval.c three ways: no adaptation, learning from every wake word (before), and
from real requests only (after).  Scored: the household's own requests from its 6th on (the adaptation comes in from
5), right as corpus_eval.py has it (from 40 ms before the wake word's end to 40 ms after the command's start), by what
the Echo does (the aligner where it answers, else the level's rule).  The false wakes are not scored.

Pryon's end: stock Pryon runs under qemu only with the firmware unpacked (corpus_eval.py); here it is stood in for by
the word's end as the corpus has it moved as wakealign.h measured Pryon's for the German models (Alexa 100 ms
before, Computer 25, Amazon 0, Echo 75 after, Ziggy 20 after) and a jitter of 25 ms, the same in every way compared.

  .venv/bin/python tools/wakealign/adapt_eval.py [--jobs N]          (work in build/adapt-eval/)"""
import argparse, collections, json, os, subprocess, sys, zlib
from concurrent.futures import ThreadPoolExecutor
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from corpus_eval import ROOT, CORPUS, R, PAD, BEFORE, AFTER, TOL, AMAZON, room, pcm_of

OUT = os.path.join(ROOT, "build", "adapt-eval")
PRYON_MS = {"alexa": -100, "computer": -25, "amazon": 0, "echo": 75, "ziggy": 20}
RATES = (0.0, 0.25, 0.5)
WAYS = ("none", "all", "real")


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    a = ap.parse_args()
    os.makedirs(os.path.join(OUT, "win"), exist_ok=True)
    subprocess.run(["cc", "-O2", "-I" + os.path.join(ROOT, "src", "hassmic"), os.path.join(ROOT, "tools", "wakealign", "eval.c"),
                    *(os.path.join(ROOT, "src", "hassmic", f) for f in ("wakealign.c", "wordtail.c", "statefile.c")), "-lm",
                    "-o", os.path.join(OUT, "eval")], check=True)
    rows = [json.loads(l) for l in open(os.path.join(CORPUS, "manifest.jsonl"), encoding="utf-8")]
    clips = [j for j in rows if j.get("ok") and j.get("words") and j["kind"] in ("command", "joined", "alone") and "wake_end" in j
             and j["wake"] in AMAZON and j["language"] == "de"]
    # each clip's window once (and once as a television's): the room, Pryon's end stood in for, the keyword's level
    def speaker(x):
        X = np.fft.rfft(x.astype(float)); f = np.fft.rfftfreq(len(x), 1 / R)
        X[(f < 250) | (f > 4000)] *= 10 ** (-30 / 20)
        return np.fft.irfft(X, len(x))
    tvclips = []
    for j in list(clips):
        t = dict(j, id=j["id"] + "~tv", _tv=True); tvclips.append(t)
    for k, j in enumerate(clips + tvclips):
        src = pcm_of(j)
        if j.get("_tv"): src = speaker(src)
        x = room(src, zlib.crc32(j["id"].encode()))
        jit = np.random.default_rng(zlib.crc32(j["id"].encode()) + 1).normal(0, 25)
        e = int(PAD + j["wake_end"] * R + (PRYON_MS[j["wake"]] + jit) * R / 1000); b = int(PAD + j["words"][0][0] * R)
        f = max(0, e - BEFORE); w = x[f:e + AFTER]
        j["_win"] = os.path.join(OUT, "win", j["id"].replace("/", "_") + ".raw"); w.tofile(j["_win"])
        j["_end"] = e - f; j["_from"] = f
        j["_kdb"] = 10 * np.log10(np.mean(x[b:e].astype(float) ** 2) + 1) - 20 * np.log10(32768)
        j["_lo"] = PAD + j["wake_end"] * R - TOL
        j["_hi"] = (PAD + j["command_start"] * R + TOL) if "command_start" in j else float("inf")
    by_voice = collections.defaultdict(list); by_tv = collections.defaultdict(list)
    for j in clips: by_voice[j["voice"]].append(j)
    for j in tvclips: by_tv[j["voice"]].append(j)
    homes = sorted(v for v in by_voice if v.startswith("de-"))
    wa = os.path.join(ROOT, "wakealign", "{}-de-DE.wa")
    print(f"{len(clips)} German clips, {len(homes)} households", flush=True)

    def sequence(home, tv, rate, kind):
        rng = np.random.default_rng(zlib.crc32(f"{home}|{tv}|{rate}".encode()))
        own = [by_voice[home][i] for i in np.random.default_rng(zlib.crc32(home.encode())).permutation(len(by_voice[home]))]
        n_tv = int(round(len(own) * rate / (1 - rate))) if rate else 0
        pool = by_tv[tv] if kind == "speaker" else by_voice[tv]
        fakes = [pool[i] for i in rng.permutation(len(pool))][:n_tv]
        seq = [(j, 1) for j in own]
        for f in fakes: seq.insert(int(rng.integers(0, len(seq) + 1)), (f, 0))
        return seq

    def run(job):
        home, tv, rate, kind, way = job
        seq = sequence(home, tv, rate, kind)
        lines = "".join(f"{j['_win']} {wa.format(j['wake'])} {home} {j['_end']} {j['_kdb']:.2f} {real}\n" for j, real in seq)
        r = subprocess.run([os.path.join(OUT, "eval")], input=lines, capture_output=True, text=True, check=True,
                           env=dict(os.environ, EVAL_LEARN=way))
        right = total = 0; nth = 0
        for (j, real), l in zip(seq, r.stdout.split("\n")):
            if not real: continue
            nth += 1
            if nth <= 5: continue
            rel, plain, final, rule = map(int, l.split())
            s = j["_from"] + (final if final >= 0 else rule)
            total += 1; right += j["_lo"] <= s <= j["_hi"]
        return job, right, total

    # televisions: two other German voices (as alike as it gets) and one German voice speaking English is not in the
    # corpus as Amazon words, so all are German: the hardest case for a household to be told from
    jobs = []
    for i, home in enumerate(homes):
        tvs = [homes[(i + 3) % len(homes)], homes[(i + 6) % len(homes)]]
        for tv in tvs:
            for rate in RATES:
                for kind in ("voice", "speaker"):
                    for way in WAYS: jobs.append((home, tv, rate, kind, way))
    with ThreadPoolExecutor(a.jobs) as ex: res = list(ex.map(run, jobs))
    agg = collections.defaultdict(lambda: [0, 0]); per = collections.defaultdict(lambda: [0, 0])
    for (home, tv, rate, kind, way), right, total in res:
        agg[(kind, rate, way)][0] += right; agg[(kind, rate, way)][1] += total
        per[(kind, home, rate, way)][0] += right; per[(kind, home, rate, way)][1] += total
    for kind, what in (("voice", "another German voice as it is"), ("speaker", "another German voice through a television's speaker")):
        print(f"\nRight (the household's requests from its 6th on); the television: {what}")
        print(f"  {'false wakes':<14}" + "".join(f"{w:>18}" for w in ("no adaptation", "before (all)", "after (real)")) + f"{'n':>8}")
        for rate in RATES:
            cells = [agg[(kind, rate, w)] for w in WAYS]
            print(f"  {f'{int(rate * 100)} %':<14}" + "".join(f"{100 * r / t:>17.1f}%" for r, t in cells) + f"{cells[0][1]:>8}")
        print(f"  by household at 50 %: after - before, points: " + ", ".join(
            f"{h[3:]} {100 * (per[(kind, h, 0.5, 'real')][0] - per[(kind, h, 0.5, 'all')][0]) / per[(kind, h, 0.5, 'all')][1]:+.1f}" for h in homes))
    json.dump([dict(home=h, tv=t, rate=r, kind=k, way=w, right=ri, total=to) for (h, t, r, k, w), ri, to in res], open(os.path.join(OUT, "results.json"), "w"))


if __name__ == "__main__":
    main()
