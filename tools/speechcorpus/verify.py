#!/usr/bin/env python3
"""Checks every clip of make.py's corpus is what its text says: speech to text by canary-1b-v2 (wyoming-onnx-asr in the
container "canary" on the host "main", reached through the "omnivoice" container there, which has the wyoming
package), compared word by word.  Writes "heard", "ok" and, when not, "why" into the manifest.

  toolchain/omnivoice/venv/bin/python tools/speechcorpus/verify.py OUT [--all]      (only the unchecked ones, else all)

The whole run: make.py OUT; verify.py OUT; twice make.py OUT --redo and verify.py OUT; align.py OUT; verify.py OUT
(the joins).  tools/speechcorpus/run.sh OUT does that.

What canary writes otherwise and is let through: numbers as digits, German imperatives as the third person ("mach" ->
"macht"), the wake words as it spells them (WAKE_HEARD; "Ziggy" as "Sigi", "Nabu" as "Navu") and their case and
spacing, words run together ("Computer Pause" as "Computerpause"), French endings not said ("éteins" as "éteint").
Nothing else: one word heard wrong ("Licht" as "nicht") and the clip is out.  Duration and level too: a clip
over 8 s, or under -45 dBFS, is something else than a short request."""
import argparse, base64, difflib, json, os, re, subprocess, sys, unicodedata
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
STT = r'''
import asyncio, base64, json, sys
from wyoming.asr import Transcribe, Transcript
from wyoming.audio import AudioChunk, AudioStart, AudioStop
from wyoming.client import AsyncTcpClient
async def stt(pcm, lang):
    async with AsyncTcpClient("10.11.0.3", 10300) as c:
        await c.write_event(Transcribe(language=lang).event())
        await c.write_event(AudioStart(rate=16000, width=2, channels=1).event())
        for i in range(0, len(pcm), 8192): await c.write_event(AudioChunk(rate=16000, width=2, channels=1, audio=pcm[i:i + 8192]).event())
        await c.write_event(AudioStop().event())
        while True:
            e = await c.read_event()
            if Transcript.is_type(e.type): return Transcript.from_event(e).text
async def main():
    for line in sys.stdin:
        j = json.loads(line)
        print(json.dumps({"id": j["id"], "heard": await stt(base64.b64decode(j["pcm"]), j["lang"])}, ensure_ascii=False), flush=True)
asyncio.run(main())
'''
# how canary spells the wake words (seen in the tests of issue #79, 2026-10-29), as word lists after norm()
WAKE_HEARD = {"alexa": [["alexa"]], "computer": [["computer"], ["komputer"]], "echo": [["echo"], ["eko"], ["ecco"]],
              "amazon": [["amazon"], ["amazone"]],
              "ziggy": [["ziggy"], ["siggy"], ["sigi"], ["segi"], ["ziggi"], ["zigi"], ["siggi"], ["sigui"], ["zigui"], ["sigue"]],
              "okay_nabu": [["okay", "nabu"], ["ok", "nabu"], ["okay", "navu"], ["okay", "nabo"], ["ok", "navu"], ["okaynabu"]],
              "hey_jarvis": [["hey", "jarvis"], ["hey", "jervis"], ["hi", "jarvis"], ["heyjarvis"]],
              "hey_mycroft": [["hey", "mycroft"], ["hey", "microsoft"], ["hey", "my", "craft"], ["hey", "mycraft"], ["hey", "mikroft"]]}


def norm(s, lang):
    """lower case words without accents' variants of punctuation; digits as words (num2words in the language)"""
    from num2words import num2words
    s = unicodedata.normalize("NFC", s.lower()).replace("’", "'").replace("-", " ")
    # a long digit string is no number said (canary made one up once): left as it is, it matches nothing
    s = re.sub(r"\d+", lambda m: " " + num2words(int(m[0]), lang=lang).replace("-", " ") + " " if len(m[0]) < 7 else m[0], s)
    s = re.sub(r"[^\w' ]+", " ", s).replace("ß", "ss")
    return s.split()


def same_word(a, b, lang):
    if a == b or (lang == "de" and {a, b} == {"stopp", "stop"}): return True
    if lang == "de" and any(a == b + x or b == a + x for x in ("t", "e", "te")): return True    # mach -> macht, schalte -> schaltete
    # French spellings that sound the same: endings not said ("éteins" / "éteint", "mets" / "met")
    if lang == "fr" and re.sub(r"(ent|[stx])+$", "", a) == re.sub(r"(ent|[stx])+$", "", b): return True
    return False


def check(j, heard):
    """None if the clip says its text, else why not"""
    lang = j["language"]; h = norm(heard, lang)
    if not h: return "nothing heard"
    if "".join(h) == "".join(norm(j["text"], lang)): return None      # the same letters ("Computerpause", "Jarvison")
    if j["kind"] == "bare":
        want = norm(j["command"], lang)
        ok = len(h) == len(want) and all(same_word(a, b, lang) for a, b in zip(h, want))
        return None if ok else f"heard as {' '.join(h)!r}"
    wake = next((v for v in WAKE_HEARD[j["wake"]] if h[:len(v)] == v), None)
    if wake is None:                                    # canary's own spelling, close enough: 2 of 3 letters
        n = len(norm(j["wake_text"], lang)); got = "".join(h[:n]); want = "".join(norm(j["wake_text"], lang))
        if difflib.SequenceMatcher(None, got, want).ratio() < 0.6: return f"wake word heard as {' '.join(h[:n])!r}"
        wake = h[:n]
    rest = h[len(wake):]; want = norm(j["command"], lang)
    if len(rest) != len(want) or not all(same_word(a, b, lang) for a, b in zip(rest, want)):
        # "volume down" as "volume, down", "est-ce" as "est ce": the same letters in other words
        if "".join(rest) != "".join(want): return f"command heard as {' '.join(rest)!r}"
    return None


def judge(jj):
    why = check(jj, jj["heard"])
    if not why and jj["seconds"] > 8: why = f"{jj['seconds']} s long"
    if not why and jj["rms_dbfs"] < -45: why = f"at {jj['rms_dbfs']} dBFS"
    jj["ok"] = why is None; jj.pop("why", None)
    if why: jj["why"] = why


def lock(out):
    """one of make.py, verify.py, align.py on a corpus at a time: each writes the whole manifest (two at once lost rows)"""
    import fcntl
    os.makedirs(out, exist_ok=True); f = open(os.path.join(out, ".lock"), "w")
    try: fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError: sys.exit(f"{out}: another make.py, verify.py or align.py works on it")
    lock.held = f


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("out"); ap.add_argument("--all", action="store_true")
    ap.add_argument("--recheck", action="store_true", help="what was heard judged again (these rules changed), no STT")
    a = ap.parse_args()
    lock(a.out)
    man = os.path.join(a.out, "manifest.jsonl")
    rows = [json.loads(l) for l in open(man, encoding="utf-8")]
    def save():
        with open(man + ".tmp", "w", encoding="utf-8") as f:
            for j in rows: f.write(json.dumps(j, ensure_ascii=False) + "\n")
        os.replace(man + ".tmp", man)
    if a.recheck:
        had = [j for j in rows if j.get("heard") and "file" in j]; was = sum(j["ok"] for j in had)
        for j in had: judge(j)
        save(); print(f"judged again: {sum(j['ok'] for j in had)} of {len(had)} say their text (were {was})"); return
    todo = [j for j in rows if a.all or "heard" not in j]          # align.py's joins too: made "ok", never heard
    import wave
    p = subprocess.Popen(["ssh", "main", "docker exec -i omnivoice python -c " + repr(f"import base64; exec(base64.b64decode('{base64.b64encode(STT.encode()).decode()}'))")],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")
    def heard(pcm, lang, id_):
        p.stdin.write(json.dumps({"id": id_, "lang": lang, "pcm": base64.b64encode(pcm).decode()}) + "\n"); p.stdin.flush()
        return json.loads(p.stdout.readline())["heard"]
    # the voices' references first (make.py voices/voices.json): one canary cannot make out (seen: fr-m-teen's, every
    # clip of it "nothing heard" or "oui") is turned down, and make.py --redo makes the voice anew
    vj_path = os.path.join(a.out, "voices", "voices.json")
    vj = json.load(open(vj_path, encoding="utf-8")) if os.path.exists(vj_path) else {}
    for v, info in vj.items():
        if "heard" in info: continue
        with wave.open(os.path.join(a.out, "voices", v + ".wav")) as w: pcm = w.readframes(w.getnframes())
        info["heard"] = heard(pcm, info["language"], v); h, want = norm(info["heard"], info["language"]), norm(info["text"], info["language"])
        info["ok"] = difflib.SequenceMatcher(None, h, want).ratio() >= 0.85
        print(f"voice {v}: {'ok' if info['ok'] else 'turned down: heard ' + repr(info['heard'])}", flush=True)
    if vj:
        with open(vj_path + ".tmp", "w", encoding="utf-8") as f: json.dump(vj, f, ensure_ascii=False, indent=1)
        os.replace(vj_path + ".tmp", vj_path)
    print(f"{len(todo)} clips to check", flush=True)
    by = {j["id"]: j for j in rows}; n = 0
    for j in todo:                                       # one at a time: canary answers each in ~0.2 s
        with wave.open(os.path.join(a.out, j["file"])) as w: pcm = w.readframes(w.getnframes())
        jj = by[j["id"]]; jj["heard"] = heard(pcm, j["language"], j["id"]); judge(jj)
        n += 1
        if n % 200 == 0: save(); print(f"{n} of {len(todo)}: {sum(1 for x in todo[:n] if x['ok'])} ok", flush=True)
    p.stdin.close(); p.wait(); save()
    bad = [j for j in todo if not j["ok"]]
    print(f"{len(todo) - len(bad)} of {len(todo)} say their text; turned down:")
    for j in bad[:40]: print(f"  {j['id']}: {j['text']!r} heard {j['heard']!r} ({j['why']})")


if __name__ == "__main__":
    main()
