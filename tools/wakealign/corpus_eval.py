#!/usr/bin/env python3
"""Where the replay after the wake word starts, measured on the speech corpus (testdata/corpus, tools/speechcorpus)
against its ground truth (MFA's word times with the whole text): Pryon's end as it is, the release's rule
(wordtail.c), the aligner alone, with the household's speaker adaptation, and what the Echo does (the aligner where it
answers, else the rule).  Right: from 40 ms before the wake word's end to 40 ms after the command's start (in a pause,
anywhere in it; the word alone: anywhere after its end); else the word's tail is left in, or the command's start is
cut.

  .venv/bin/python tools/wakealign/corpus_eval.py [--jobs N] [--publish docs/wake-word-end]   (work in build/corpus-eval/)

Each clip in a room as tools of issue #79 had it (a 50 ms reverb, speech at -30 dBFS, noise at -58 dBFS, 1 s before
and after), the wake word found by the engine a household would use: stock Pryon under qemu (tools/qrun.sh,
build/donut/pryon_test; de-DE models from device-logs/models, en-US: the firmware's Alexa, computer-/echo-en-US; French
and Spanish have no Pryon model here, so their Amazon words go through the de-DE ones, marked "proxy"), microWakeWord's
words through pymicro-wakeword.  The aligner of the household's language (the voice's own: a German voice's English
too, as setting speech_language would have it; en-AU as en-GB), each voice one household, its wake words in a fixed
random order (the adaptation learns as it goes).  tools/wakealign/eval.c (built here) does the three starts."""
import argparse, collections, json, os, re, subprocess, sys, wave, zlib
from concurrent.futures import ThreadPoolExecutor
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORPUS = os.path.join(ROOT, "testdata", "corpus"); OUT = os.path.join(ROOT, "build", "corpus-eval")
R = 16000; PAD = R; BEFORE, AFTER = 24000, 8000; TOL = 640
AMAZON = ("alexa", "amazon", "computer", "echo", "ziggy"); MWW = ("okay_nabu", "hey_jarvis", "hey_mycroft")
RIR = np.exp(-np.arange(int(0.25 * R)) / (0.05 * R)) * np.random.default_rng(7).standard_normal(int(0.25 * R)); RIR[0] = 6; RIR /= np.abs(RIR).sum() / 4


def household_locale(voice):
    if voice.startswith("de-"): return "de-DE"
    if voice.startswith(("en-gb", "en-au")): return "en-GB"
    if voice.startswith("en-in"): return "en-IN"
    if voice.startswith("fr-"): return "fr-FR"
    if voice.startswith("es-"): return "es-ES"
    return "en-US"


def pryon_model(wake, lang):
    """(manifest or None for the firmware's Alexa, proxy?) or None when there is none"""
    dl = lambda i: os.path.join(ROOT, "device-logs", "models", i, "unpacked", "pryon.manifest")
    if lang == "en":
        if wake == "alexa": return (None, False)
        return (dl(f"{wake}-en-US"), False) if os.path.exists(dl(f"{wake}-en-US")) else None
    return (dl(f"{wake}-de-DE"), lang != "de") if os.path.exists(dl(f"{wake}-de-DE")) else None


def room(x, seed):
    rng = np.random.default_rng(seed)
    x = np.concatenate([np.zeros(PAD), x.astype(float), np.zeros(PAD)])
    n = 1 << (len(x) + len(RIR)).bit_length(); y = np.fft.irfft(np.fft.rfft(x, n) * np.fft.rfft(RIR, n), n)[:len(x)]   # np.convolve's, faster
    y *= 10 ** (-30 / 20) * 32768 / np.sqrt(np.mean(y[PAD:-PAD] ** 2) + 1e-9)
    return np.clip(y + rng.standard_normal(len(y)) * 32768 * 10 ** (-58 / 20), -32768, 32767).astype("<i2")


def pcm_of(j):
    with wave.open(os.path.join(CORPUS, j["file"])) as w: return np.frombuffer(w.readframes(w.getnframes()), "<i2")


SPEED = 2                   # pryon_test -x: twice real time (qemu does ~4 alone); a run that skipped audio is redone at 1


def skipped(out):
    """audio Pryon left out (its result metadata), ms"""
    return sum(int(m) for m in re.findall(r'"skipped_audio_backlog_since_last_ww_msec":(\d+)', out))


def detect_pryon(groups, jobs):
    """pryon_test over each clip's room audio on its own (as the measurements of issue #79 ran it: a clip is 3-6 s, so
    the decoder's backlog never nears the ~7 s past which it skips audio, and a slip cannot carry into the next clip);
    {clip id: (begin, end)}, the first wake word in the clip"""
    os.makedirs(os.path.join(OUT, "pryon"), exist_ok=True); runs = []
    for man, clips in groups.items():
        model = os.path.basename(os.path.dirname(os.path.dirname(man))) if man else "alexa-en-US-firmware"
        for j in clips:
            # named by what is in it (the clip, the room, the speed): a changed corpus never meets old results
            key = zlib.crc32((j["id"] + f" {RIR.sum():.6f} -30 -58 x{SPEED} {len(j['_room'])}").encode())
            base = os.path.join(OUT, "pryon", f"{model}-{j['id'].replace('/', '_')}-{key:08x}")
            runs.append((man, j, base + ".raw", base + ".txt"))
    def run(r):
        man, j, raw, res = r
        if not os.path.exists(res):
            j["_room"].tofile(raw)
            for speed in (SPEED, 1):
                cmd = [os.path.join(ROOT, "tools", "qrun.sh"), "-t", "300", os.path.join(ROOT, "build", "donut", "pryon_test")] + (["-m", man] if man else []) + ["-x", str(speed), raw]
                p = subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL)
                if not skipped(p.stdout): break
            if skipped(p.stdout): sys.exit(f"{raw}: Pryon skipped audio even in real time")
            open(res + ".tmp", "w").write(p.stdout); os.replace(res + ".tmp", res); os.remove(raw)
        m = re.search(r"keyword=(?!STOP)\S+ type=2 begin=(\d+) end=(\d+)", open(res).read())
        return (j["id"], (int(m[1]), int(m[2]))) if m else None
    out = {}
    with ThreadPoolExecutor(jobs) as ex:
        for h in ex.map(run, runs):
            if h: out[h[0]] = h[1]
    return out


def detect_mww(clips):
    from pymicro_wakeword import MicroWakeWord, MicroWakeWordFeatures, Model
    models = {"okay_nabu": Model.OKAY_NABU, "hey_jarvis": Model.HEY_JARVIS, "hey_mycroft": Model.HEY_MYCROFT}
    out = {}
    for j in clips:
        mww = MicroWakeWord.from_builtin(models[j["wake"]]); fe = MicroWakeWordFeatures(); n = 0
        x = j["_room"]
        for i in range(0, len(x), 160):
            for f in fe.process_streaming(x[i:i + 160].tobytes()):
                if mww.process_streaming(f) and j["id"] not in out: out[j["id"]] = (max(0, i + 160 - R), i + 160)
            if j["id"] in out: break
    return out


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--jobs", type=int, default=24)
    ap.add_argument("--cflags", default="", help="for eval.c, e.g. wakealign.c's pause constants: -DPAUSE_LOOK=20 -DFLOOR_DB=6.0 ...")
    ap.add_argument("--publish", help="the tables as Markdown and each request as JSONL into this folder (docs/wake-word-end)")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    subprocess.run(["cc", "-O2", *a.cflags.split(), "-I" + os.path.join(ROOT, "src", "hassmic"), os.path.join(ROOT, "tools", "wakealign", "eval.c"),
                    *(os.path.join(ROOT, "src", "hassmic", f) for f in ("wakealign.c", "wordtail.c", "statefile.c")), "-lm",
                    "-o", os.path.join(OUT, "eval")], check=True)
    rows = [json.loads(l) for l in open(os.path.join(CORPUS, "manifest.jsonl"), encoding="utf-8")]
    clips = [j for j in rows if j.get("ok") and j.get("words") and j["kind"] in ("command", "joined", "stop", "alone") and "wake_end" in j]
    for k, j in enumerate(clips): j["_room"] = room(pcm_of(j), k)
    print(f"{len(clips)} clips with a wake word", flush=True)
    groups = collections.defaultdict(list); skipped = 0
    for j in clips:
        if j["wake"] in MWW: continue
        pm = pryon_model(j["wake"], j["language"])
        if pm is None: skipped += 1; continue
        j["_proxy"] = pm[1]; groups[pm[0]].append(j)
    print(f"Pryon: {sum(map(len, groups.values()))} clips over {len(groups)} models ({skipped} without one: Amazon, Ziggy in English)", flush=True)
    hits = detect_pryon(groups, a.jobs)
    hits.update(detect_mww([j for j in clips if j["wake"] in MWW]))
    found = [j for j in clips if j["id"] in hits]
    print(f"detected: {len(found)} of {len(clips)}", flush=True)
    # the windows, each household in its fixed random order, one eval.c per household
    os.makedirs(os.path.join(OUT, "win"), exist_ok=True)
    by_home = collections.defaultdict(list)
    for j in found: by_home[j["voice"]].append(j)
    def household(item):
        voice, js = item
        js = [js[i] for i in np.random.default_rng(zlib.crc32(voice.encode())).permutation(len(js))]
        lines = []
        for j in js:
            b, e = hits[j["id"]]; x = j["_room"]; f = max(0, e - BEFORE); w = x[f:e + AFTER]
            p = os.path.join(OUT, "win", j["id"].replace("/", "_") + ".raw"); w.tofile(p)
            kdb = 10 * np.log10((np.mean(x[b:e].astype(float) ** 2) if e > b else 0) + 1) - 20 * np.log10(32768)
            # main.c align_resolve: the language spoken, else the word's own (microWakeWord's and the firmware's: en-US)
            wa = os.path.join(ROOT, "wakealign", f"{j['wake']}-{household_locale(voice)}.wa")
            if not os.path.exists(wa):
                own = "en-US" if j["wake"] in MWW or j["language"] == "en" else "de-DE"
                wa = os.path.join(ROOT, "wakealign", f"{j['wake']}-{own}.wa")
            lines.append(f"{p} {wa} {voice} {e - f} {kdb:.2f}\n"); j["_from"] = f
        r = subprocess.run([os.path.join(OUT, "eval")], input="".join(lines), capture_output=True, text=True, check=True)
        for j, l in zip(js, r.stdout.split("\n")):
            rel, plain, final = map(int, l.split()); f = j["_from"]
            j["_starts"] = dict(pryon=hits[j["id"]][1], release=f + rel, aligner=f + plain if plain >= 0 else -1,
                                adapted=f + final if final >= 0 else -1, echo=f + final if final >= 0 else f + rel)
        return js
    with ThreadPoolExecutor(a.jobs) as ex: done = [j for js in ex.map(household, by_home.items()) for j in js]
    # scoring
    def verdict(j, s):
        if s < 0: return "none"
        end = PAD + j["wake_end"] * R                   # the word alone: nothing follows, any start after its end is right
        nxt = PAD + j["command_start"] * R if "command_start" in j else float("inf")
        return "right" if end - TOL <= s <= nxt + TOL else ("tail" if s < end - TOL else "cut")
    res = []
    for j in done:
        res.append(dict(id=j["id"], voice=j["voice"], language=j["language"], wake=j["wake"], kind=j["kind"],
                        proxy=j.get("_proxy", False), gap=j.get("gap"), **{k: verdict(j, s) for k, s in j["_starts"].items()},
                        pryon_ms=round((hits[j["id"]][1] - PAD - j["wake_end"] * R) / 16),
                        # each start against the wake word's end and the command's start, in ms (None: no start)
                        ms={k: None if v < 0 else round((v - PAD - j["wake_end"] * R) / 16) for k, v in j["_starts"].items()},
                        next_ms=round(((j["command_start"] if "command_start" in j else j["seconds"]) - j["wake_end"]) * 1000),
                        window=j["id"].replace("/", "_") + ".raw", window_from=int(j["_from"]), pryon_end=int(hits[j["id"]][1]),
                        pryon_begin=int(hits[j["id"]][0]), truth_end=int(PAD + j["wake_end"] * R),
                        truth_next=int(PAD + (j["command_start"] if "command_start" in j else j["seconds"]) * R),
                        starts={k: int(v) for k, v in j["_starts"].items()}))
    json.dump(res, open(os.path.join(OUT, "results.json"), "w"), indent=0)
    tables = report(res)
    for title, head, rows in tables:                     # as text
        print(f"\n{title}"); w0 = max(len(str(r[0])) for r in rows) + 2
        print("  " + " " * w0 + "".join(f"{h:>14}" for h in head[1:]))
        for r in rows: print("  " + f"{str(r[0]):{w0}}" + "".join(f"{str(c):>14}" for c in r[1:]))
    if a.publish:                                        # into the repo: the tables as Markdown, each request as a line
        os.makedirs(a.publish, exist_ok=True)
        commit = subprocess.run(["git", "-C", ROOT, "describe", "--always"], capture_output=True, text=True).stdout.strip()
        # what decides the numbers changed since that commit: say so (the results themselves do not count)
        if subprocess.run(["git", "-C", ROOT, "diff", "--quiet", "HEAD", "--", "src/hassmic", "tools/wakealign", "tools/vad_leadin.py",
                           "wakealign", "testdata/corpus/manifest.jsonl"]).returncode: commit += " with changes"
        md = [f"# Where the request starts after the wake word: the speech corpus", "",
              f"Made by `tools/wakealign/corpus_eval.py{' --cflags ' + repr(a.cflags) if a.cflags else ''}` at {commit}, "
              f"{__import__('datetime').date.today()}; {len(res)} wake words of {len(clips)} clips. What it is and how to read it: "
              "[`../wake-word-end.md`](../wake-word-end.md). Right: from 40 ms before the wake word's end to 40 ms after the "
              "command's start (in a pause anywhere in it; the word alone: anywhere after its end). Columns: Pryon's end as it is, "
              "the release's rule (wordtail.c), the aligner alone (else the rule), with the household's adaptation, and the "
              "Echo (the aligner where it answers, else the rule).", ""]
        for title, head, rows in tables:
            md += [f"## {title}", "", "| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
            md += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows] + [""]
        open(os.path.join(a.publish, "corpus.md"), "w").write("\n".join(md))
        keep = ("id", "voice", "language", "wake", "kind", "proxy", "gap", "pryon_begin", "pryon_end", "truth_end", "truth_next", "starts")
        with open(os.path.join(a.publish, "corpus.jsonl"), "w") as f:
            for r in sorted(res, key=lambda r: r["id"]): f.write(json.dumps({k: r[k] for k in keep}, separators=(",", ":")) + "\n")
        print(f"\nwritten: {a.publish}/corpus.md, corpus.jsonl")


WAYS = ("pryon", "release", "aligner", "adapted", "echo")
BANDS = [("cut >300", 300, None), ("cut 200-300", 200, 300), ("cut 100-200", 100, 200), ("cut 40-100", 40, 100),
         ("right", None, None), ("tail 40-100", 40, 100), ("tail 100-200", 100, 200), ("tail >200", 200, None)]


def report(res):
    """[(title, header, rows)]: right by group; how far off (the wake word's tail left in, the command cut, ms bands);
    where the starts lie (percentiles)"""
    out = []
    def right(title, key):
        g = collections.defaultdict(list)
        for r in res: g[key(r)].append(r)
        rows = []
        for k in sorted(g, key=str):
            rs = g[k]; n = len(rs)
            rows.append([k if isinstance(k, str) else " ".join(x for x in k if x), n] + [f"{100 * sum(r[w] == 'right' for r in rs) / n:.0f} %" for w in WAYS]
                        + [sum(r["echo"] == "tail" for r in rs), sum(r["echo"] == "cut" for r in rs), f"{int(np.median([r['pryon_ms'] for r in rs])):+d}"])
        out.append((f"Right {title}", ["", "n"] + list(WAYS) + ["Echo: tail", "Echo: cut", "Pryon's end - word's, ms (median)"], rows))
    right("in all", lambda r: "all")
    right("by kind (joined: a pause of 0.23-0.68 s put in)", lambda r: r["kind"])
    right("by language (proxy: Amazon's words in fr/es through de-DE Pryon)", lambda r: (r["language"], "proxy" if r["proxy"] else ""))
    right("by wake word", lambda r: r["wake"])
    right("by voice", lambda r: r["voice"])
    for kind in (None, "command", "joined", "alone", "stop"):
        js = [r for r in res if kind is None or r["kind"] == kind]; rows = []
        for w in WAYS:
            sz = []
            for r in js:
                st = r["starts"][w] if r["starts"][w] >= 0 else r["starts"]["release"]      # no answer: by the level
                sz.append(((r["truth_end"] - st) / 16, (st - r["truth_next"]) / 16 if r["kind"] != "alone" else -1e9))
            row = [w]
            for b, lo, hi in BANDS:
                if lo is None: k = sum(t <= 40 and c <= 40 for t, c in sz)
                else: k = sum((c if b.startswith("cut") else t) > lo and (hi is None or (c if b.startswith("cut") else t) <= hi) for t, c in sz)
                row.append(f"{100 * k / max(1, len(js)):.1f} %")
            rows.append(row)
        out.append((f"How far off, {kind or 'all'} ({len(js)}): the command cut or the wake word's tail left in, ms",
                    [""] + [b for b, _, _ in BANDS], rows))
    rows = []
    for kind in ("command", "joined", "alone", "stop"):
        for w in WAYS:
            v = [r["ms"][w] for r in res if r["kind"] == kind and r["ms"][w] is not None]
            if v: rows.append([f"{kind} {w}", len(v)] + [f"{int(np.percentile(v, q)):+d}" for q in (10, 25, 50, 75, 90)])
    out.append(("Where the starts lie against the wake word's end, ms (percentiles)", ["", "n", "10th", "25th", "50th", "75th", "90th"], rows))
    return out

if __name__ == "__main__":
    main()
