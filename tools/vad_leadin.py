#!/usr/bin/env python3
"""Does Home Assistant's voice activity detection hear a request that opens on the command (issue #80)?  Its own
classes (assist_pipeline's VoiceCommandSegmenter on pymicro-vad, a new one per run, as HA makes them) on what the Echo
sends after a wake word: the command at once, then the room, with and without main.c's 800 ms of silence ahead.  The
commands are the speech corpus's commands alone (testdata/corpus, kind "bare": 30 voices, 4 languages), as they would
follow a cut at the right place; in a room (a 50 ms reverb, speech at -26 dBFS as the mic gain leaves it, the room at
-55 dBFS before and after, 3 s of it after the command).

  build/ha-venv/bin/python tools/vad_leadin.py [--publish docs/wake-word-end]   (build/ha-venv: tests/ha_real_dropin.py --setup)

Heard: the segmenter went into the command; ended: it also found the command's end within those 3 s (else HA waits
for its timeout, 15 s, before it acts on the request)."""
import collections, json, os, sys, wave
import numpy as np
from homeassistant.components.assist_pipeline.audio_enhancer import MicroVadSpeexEnhancer
from homeassistant.components.assist_pipeline.vad import VoiceCommandSegmenter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "testdata", "corpus"); R = 16000
RIR = np.exp(-np.arange(int(0.25 * R)) / (0.05 * R)) * np.random.default_rng(7).standard_normal(int(0.25 * R)); RIR[0] = 6; RIR /= np.abs(RIR).sum() / 4


def request(x, seed, lead_ms):
    """what HA gets: [silence], the command in the room, 3 s of the room"""
    rng = np.random.default_rng(seed)
    y = np.convolve(x.astype(float), RIR)[:len(x)]; act = np.abs(y) > np.abs(y).max() * 0.05
    y *= 10 ** (-26 / 20) * 32768 / np.sqrt(np.mean(y[act] ** 2) + 1e-9)
    y = np.concatenate([y, np.zeros(3 * R)]) + rng.standard_normal(len(y) + 3 * R) * 32768 * 10 ** (-55 / 20) * 1.0
    return np.concatenate([np.zeros(lead_ms * 16), np.clip(y, -32768, 32767)]).astype("<i2").tobytes()


def segment(pcm):
    """(heard, ended) by HA's classes, 10 ms at a time"""
    # as default_pipeline.py makes it for a satellite (no gain, no suppression: the ESPHome audio settings are not
    # used); the VAD there runs on the audio before speex in any case
    v = MicroVadSpeexEnhancer(0, 0, True); seg = VoiceCommandSegmenter(); heard = False
    for i in range(0, len(pcm) - 319, 320):
        if not seg.process(0.01, v.enhance_chunk(pcm[i:i + 320], i // 32).speech_probability): return heard or seg.in_command, True
        heard |= seg.in_command
    return heard, False


def main():
    import argparse, subprocess, datetime
    ap = argparse.ArgumentParser(); ap.add_argument("--publish", help="the tables as Markdown, each request as JSONL, into this folder")
    a = ap.parse_args()
    rows = [json.loads(l) for l in open(os.path.join(CORPUS, "manifest.jsonl"), encoding="utf-8")]
    bare = [j for j in rows if j["kind"] == "bare" and j.get("ok") and j.get("words")]
    res = []
    for k, j in enumerate(bare):
        with wave.open(os.path.join(CORPUS, j["file"])) as w: x = np.frombuffer(w.readframes(w.getnframes()), "<i2")
        s, e = int(j["words"][0][0] * R), int(j["words"][-1][1] * R)
        x = x[s:e]                                         # opening on the command, as the replay from a right cut does
        r = dict(id=j["id"], voice=j["voice"], language=j["language"], command=j["command"], seconds=round((e - s) / R, 2), kind="alone")
        for lead in (0, 800): r[lead] = segment(request(x, k, lead))
        res.append(r)
    # setting wake_word_in_request: the replay from 100 ms before the wake word, so HA's VAD has the word too: the
    # corpus's requests said in one breath, the commands under 0.6 s
    whole = [j for j in rows if j["kind"] == "command" and not j["comma"] and j.get("ok") and j.get("words") and j.get("command_start")
             and j["words"][-1][1] - j["command_start"] < 0.6]
    for k, j in enumerate(whole):
        with wave.open(os.path.join(CORPUS, j["file"])) as w: x = np.frombuffer(w.readframes(w.getnframes()), "<i2")
        x = x[max(0, int((j["words"][0][0] - 0.1) * R)):int(j["words"][-1][1] * R)]
        r = dict(id=j["id"], voice=j["voice"], language=j["language"], command=j["command"],
                 seconds=round(j["words"][-1][1] - j["command_start"], 2), kind="with the wake word")
        for lead in (0, 800): r[lead] = segment(request(x, k, lead))
        res.append(r)
    tables = []
    def table(title, sel, key):
        g = collections.defaultdict(list)
        for r in res:
            if sel(r): g[key(r)].append(r)
        rows_ = []
        for k in sorted(g, key=str):
            rs = g[k]; n = len(rs); f = lambda lead, i: sum(r[lead][i] for r in rs)
            rows_.append([k, n, f"{100 * f(0, 0) / n:.0f} %", f"{100 * f(0, 1) / n:.0f} %", f"{100 * f(800, 0) / n:.0f} %", f"{100 * f(800, 1) / n:.0f} %"])
        tables.append((title, ["", "n", "heard, no silence", "ended, no silence", "heard, 800 ms", "ended, 800 ms"], rows_))
    length = lambda r: "under 0.4 s" if r["seconds"] < 0.4 else "0.4-0.8 s" if r["seconds"] < 0.8 else "0.8 s and more"
    table("The command alone opening the request, by its length", lambda r: r["kind"] == "alone", length)
    table("The command alone opening the request, by language", lambda r: r["kind"] == "alone", lambda r: r["language"])
    table("The command alone opening the request, all", lambda r: r["kind"] == "alone", lambda r: "all")
    table("With the wake word in the request (wake_word_in_request), commands under 0.6 s", lambda r: r["kind"] != "alone", lambda r: "all")
    for title, head, rows_ in tables:
        print(f"\n{title}"); print("  " + "".join(f"{h:>20}" for h in head))
        for r in rows_: print("  " + "".join(f"{str(c):>20}" for c in r))
    left = sorted({(r["voice"], r["command"]) for r in res if r["kind"] == "alone" and not r[800][1]})
    print("\nnot ended even with the silence:", left[:30])
    if a.publish:
        os.makedirs(a.publish, exist_ok=True)
        commit = subprocess.run(["git", "-C", ROOT, "describe", "--always"], capture_output=True, text=True).stdout.strip()
        # what decides the numbers changed since that commit: say so (the results themselves do not count)
        if subprocess.run(["git", "-C", ROOT, "diff", "--quiet", "HEAD", "--", "src/hassmic", "tools/wakealign", "tools/vad_leadin.py",
                           "wakealign", "testdata/corpus/manifest.jsonl"]).returncode: commit += " with changes"
        md = ["# Home Assistant's VAD on requests that open on the command: the speech corpus", "",
              f"Made by `build/ha-venv/bin/python tools/vad_leadin.py` at {commit}, {datetime.date.today()} (Home Assistant "
              f"{__import__('homeassistant.const', fromlist=['__version__']).__version__}). What it is: "
              "[`../wake-word-end.md`](../wake-word-end.md). Heard: HA's VoiceCommandSegmenter went into the command; ended: it "
              "also found the command's end within the 3 s of room after it (else HA waits for its 15 s timeout).", ""]
        for title, head, rows_ in tables:
            md += [f"## {title}", "", "| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
            md += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows_] + [""]
        md += ["## Not ended even with the silence", "", ", ".join(f"{v}: \"{c}\"" for v, c in left), ""]
        open(os.path.join(a.publish, "vad.md"), "w").write("\n".join(md))
        with open(os.path.join(a.publish, "vad.jsonl"), "w") as f:
            for r in sorted(res, key=lambda r: (r["kind"], r["id"])):
                f.write(json.dumps({**{k: r[k] for k in ("id", "kind", "voice", "language", "command", "seconds")},
                                    "no_silence": r[0], "lead_800": r[800]}, ensure_ascii=False, separators=(",", ":")) + "\n")
        print(f"\nwritten: {a.publish}/vad.md, vad.jsonl")

if __name__ == "__main__":
    main()
