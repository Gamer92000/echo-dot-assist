#!/usr/bin/env python3
"""Where each word of make.py's corpus starts and ends: the Montreal Forced Aligner (3.4.3 in toolchain/mfa/, its
pretrained models in toolchain/mfa/mfa_root) with the whole text known, the ground truth wakealign.c is measured
against (wakealign.h).  Into the manifest: "words" [[start, end, word], ...] in seconds, "wake_end" (the end of the
wake word's last word), "command_start" (the next word's start) and "gap" (between them: the pause OmniVoice made, or
none: a comma seldom gives one).  Only the clips verify.py passed; per language, each voice a speaker (MFA adapts to
it as it would to a person).  Then the joins, made anew each run: for each command clip with a comma, the voice's wake
word alone, cut 40 ms after its end, a pause, and the command alone from 40 ms before its start (kind "joined",
"pause" the silence put in, 0.15-0.6 s; words, wake_end, command_start and gap from the two alignments): pauses of
known length, which OmniVoice does not make by itself.

  toolchain/omnivoice/venv/bin/python tools/speechcorpus/align.py OUT [--all]      (only the unaligned ones, else all)"""
import argparse, json, os, re, shutil, subprocess, sys, tempfile, wave, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MFA = os.path.join(ROOT, "toolchain", "mfa")
# language: acoustic model, dictionary, G2P model for the words not in it (tools/wakealign/build.sh's)
MODELS = {"de": ("german_mfa", "german_mfa", "german_mfa"), "en": ("english_mfa", "english_us_mfa", "english_us_mfa"),
          "fr": ("french_mfa", "french_mfa", "french_mfa"), "es": ("spanish_mfa", "spanish_spain_mfa", "spanish_spain_mfa")}


def words(path):
    """a TextGrid's words tier: [(start, end, word)], silences left out"""
    t = open(path, encoding="utf-8").read(); out = []; cur = None
    for m in re.finditer(r'name = "(\w+)"|xmin = ([\d.]+)\s+xmax = ([\d.]+)\s+text = "([^"]*)"', t):
        if m[1]: cur = m[1]
        elif cur == "words" and m[4]: out.append((float(m[2]), float(m[3]), m[4]))
    return out


def lock(out):
    """one of make.py, verify.py, align.py on a corpus at a time: each writes the whole manifest (two at once lost rows)"""
    import fcntl
    os.makedirs(out, exist_ok=True); f = open(os.path.join(out, ".lock"), "w")
    try: fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError: sys.exit(f"{out}: another make.py, verify.py or align.py works on it")
    lock.held = f


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("out"); ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    lock(a.out)
    man = os.path.join(a.out, "manifest.jsonl")
    rows = [json.loads(l) for l in open(man, encoding="utf-8")]
    env = dict(os.environ, MFA_ROOT_DIR=os.path.join(MFA, "mfa_root"))
    pm = os.path.join(MFA, "mfa_root", "pretrained_models")
    for lang, (am, dic, g2p) in MODELS.items():
        todo = [j for j in rows if j["language"] == lang and j.get("ok") and (a.all or "words" not in j)]
        if not todo: continue
        tmp = tempfile.mkdtemp(prefix=f"corpus-mfa-{lang}-")
        try:
            for j in todo:                              # the corpus as MFA reads it: <speaker>/<utterance>.wav + .lab
                d = os.path.join(tmp, "in", j["voice"]); os.makedirs(d, exist_ok=True)
                base = os.path.join(d, os.path.basename(j["id"]))
                os.symlink(os.path.abspath(os.path.join(a.out, j["file"])), base + ".wav")
                open(base + ".lab", "w", encoding="utf-8").write(re.sub(r"[^\w' -]+", " ", j["text"]).strip() + "\n")
            print(f"{lang}: aligning {len(todo)} clips", flush=True)
            r = subprocess.run([os.path.join(MFA, "bin", "micromamba"), "run", "-p", os.path.join(MFA, "env"), "mfa", "align",
                                os.path.join(tmp, "in"), os.path.join(pm, "dictionary", dic + ".dict"), os.path.join(pm, "acoustic", am + ".zip"),
                                os.path.join(tmp, "out"), "--g2p_model_path", os.path.join(pm, "g2p", g2p + ".zip"), "--clean", "-j", "16", "-q"],
                               env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True)
            if r.returncode: print(r.stdout[-2000:] + r.stderr[-2000:], file=sys.stderr); sys.exit(f"{lang}: mfa align failed")
            for j in todo:
                tg = os.path.join(tmp, "out", j["voice"], os.path.basename(j["id"]) + ".TextGrid")
                if not os.path.exists(tg): j["words"] = None; continue      # MFA gave up on it (beam): no truth
                wd = words(tg); nw = len(j["wake_text"].split())
                j["words"] = [[round(s, 3), round(e, 3), w] for s, e, w in wd]
                if nw and len(wd) >= nw:
                    j["wake_end"] = round(wd[nw - 1][1], 3)
                    if len(wd) > nw: j["command_start"] = round(wd[nw][0], 3); j["gap"] = round(wd[nw][0] - wd[nw - 1][1], 3)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        with open(man + ".tmp", "w", encoding="utf-8") as f:
            for j in rows: f.write(json.dumps(j, ensure_ascii=False) + "\n")
        os.replace(man + ".tmp", man)
        got = [j for j in todo if j.get("words")]
        gaps = sorted(j["gap"] for j in got if "gap" in j)
        print(f"{lang}: {len(got)} of {len(todo)} aligned; pause after the wake word: median {gaps[len(gaps) // 2] if gaps else '-'} s, "
              f"none (< 30 ms) in {sum(g < 0.03 for g in gaps)} of {len(gaps)}", flush=True)
    rows = join(a.out, rows)
    with open(man + ".tmp", "w", encoding="utf-8") as f:
        for j in sorted(rows, key=lambda j: j["id"]): f.write(json.dumps(j, ensure_ascii=False) + "\n")
    os.replace(man + ".tmp", man)


def pcm_of(out, j):
    with wave.open(os.path.join(out, j["file"])) as w: return np.frombuffer(w.readframes(w.getnframes()), "<i2")


def join(out, rows):
    """the joined clips, anew: returns rows without the old ones and with the new"""
    sys.path.insert(0, HERE); import make
    rows = [j for j in rows if j["kind"] != "joined"]
    key = {(j["voice"], j["language"], j["kind"], j["wake"] if j["kind"] == "alone" else j["command"]): j for j in rows
           if j.get("ok") and j.get("words") and j["kind"] in ("alone", "bare")}
    new = []
    for j in rows:
        if j["kind"] != "command" or not j["comma"]: continue
        al, ba = key.get((j["voice"], j["language"], "alone", j["wake"])), key.get((j["voice"], j["language"], "bare", j["command"]))
        if not al or not ba or "wake_end" not in al: continue
        pause = round(0.15 + 0.45 * (zlib.crc32(j["id"].encode()) % 1000) / 999, 3)
        a, b = pcm_of(out, al), pcm_of(out, ba)
        cut = min(len(a), int((al["wake_end"] + 0.04) * 16000)); start = max(0, int((ba["words"][0][0] - 0.04) * 16000))
        pcm = np.concatenate([a[:cut], np.zeros(int(pause * 16000), "<i2"), b[start:]])
        shift = (cut + int(pause * 16000) - start) / 16000
        rel = os.path.join(os.path.dirname(j["file"]), "joined-" + os.path.basename(j["file"]))
        make.write_wav(os.path.join(out, rel), pcm)
        words = al["words"] + [[round(s + shift, 3), round(e + shift, 3), w] for s, e, w in ba["words"]]
        rms, peak = make.levels(pcm)
        new.append(dict({k: j[k] for k in ("voice", "voice_description", "voice_language", "language", "wake", "wake_text", "command")},
                        id=os.path.splitext(rel)[0], file=rel, kind="joined", comma=False, text=j["text"], parts=[al["id"], ba["id"]],
                        pause=pause, seconds=round(len(pcm) / 16000, 3), rms_dbfs=rms, peak=peak, ok=True, words=words,
                        wake_end=al["wake_end"], command_start=words[len(al["words"])][0],
                        gap=round(words[len(al["words"])][0] - al["wake_end"], 3)))
    print(f"joined: {len(new)} clips, pauses {min((x['gap'] for x in new), default=0):.2f}-{max((x['gap'] for x in new), default=0):.2f} s", flush=True)
    return rows + new


if __name__ == "__main__":
    main()
