#!/usr/bin/env python3
"""A speech corpus for tests and measurements: wake words and commands said by OmniVoice's designed voices (k2-fsa
OmniVoice's voice design: gender, age, pitch, whisper, English accents, from a description; nobody's voice cloned).

  toolchain/omnivoice/venv/bin/python tools/speechcorpus/make.py OUT [--redo] [--voices a,b]    (all of it: run.sh)

OmniVoice in toolchain/omnivoice/ (DEVELOPMENT.md "Speech corpus"), on the GPU.  Each voice of VOICES first says a
sentence of its language made from its description (voices/<id>.wav, the seed fixed); every clip of it is cloned from
that, so a voice stays one speaker across its clips (what the speaker adaptation learns from: wakealign.h).  Per voice:
each wake word with commands of its language, with a comma after the wake word (OmniVoice's pause there is rare and
short: align.py measures it) or without, the wake word alone, "<wake word>, stop", and each command alone (a request
from the button; align.py joins them to the wake word alone with pauses of known length); the voices of
OTHER_LANG also in another language (a German speaker's English: wakealign.h's language finding).  16 kHz mono s16 WAV,
the Echo's rate, in OUT/<voice>/, one manifest.jsonl line each.  --redo: the clips verify.py turned down, again with
the next seed (up to 3 tries), and their verify.py and align.py results dropped; a voice whose reference verify.py
could not make out, anew (up to 6 tries: for some seeds the voice design gives nothing intelligible), and its clips
once verify.py passed that.  Seeded throughout: a run from scratch makes the same clips (tools/speechcorpus/run.sh)."""
import argparse, json, os, random, sys, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# id: (description, language); the description in OmniVoice's words (models/omnivoice.py _resolve_instruct).  Accents
# act on English only
VOICES = {
    "de-f-young": ("female, young adult, moderate pitch", "de"), "de-m-middle": ("male, middle-aged, low pitch", "de"),
    "de-f-elderly": ("female, elderly, moderate pitch", "de"), "de-m-young": ("male, young adult, moderate pitch", "de"),
    "de-f-middle-high": ("female, middle-aged, high pitch", "de"), "de-m-elderly": ("male, elderly, very low pitch", "de"),
    "de-f-teen": ("female, teenager, high pitch", "de"), "de-m-child": ("male, child, high pitch", "de"),
    "de-f-whisper": ("female, young adult, whisper", "de"), "de-m-whisper": ("male, middle-aged, whisper", "de"),
    "en-us-f-young": ("female, young adult, american accent", "en"), "en-us-m-middle": ("male, middle-aged, low pitch, american accent", "en"),
    "en-us-m-elderly": ("male, elderly, low pitch, american accent", "en"), "en-gb-f-elderly": ("female, elderly, british accent", "en"),
    "en-gb-m-young": ("male, young adult, british accent", "en"), "en-au-f-middle": ("female, middle-aged, australian accent", "en"),
    "en-in-m-young": ("male, young adult, indian accent", "en"), "en-ca-f-teen": ("female, teenager, canadian accent", "en"),
    "en-ru-m-middle": ("male, middle-aged, russian accent", "en"), "en-zh-f-young": ("female, young adult, chinese accent", "en"),
    "en-ja-m-young": ("male, young adult, japanese accent", "en"), "en-us-f-whisper": ("female, middle-aged, whisper, american accent", "en"),
    "fr-f-young": ("female, young adult, moderate pitch", "fr"), "fr-m-middle": ("male, middle-aged, low pitch", "fr"),
    "fr-f-elderly": ("female, elderly, moderate pitch", "fr"), "fr-m-teen": ("male, teenager, moderate pitch", "fr"),
    "es-f-young": ("female, young adult, moderate pitch", "es"), "es-m-middle": ("male, middle-aged, moderate pitch", "es"),
    "es-f-middle-high": ("female, middle-aged, high pitch", "es"), "es-m-elderly": ("male, elderly, low pitch", "es"),
}
OTHER_LANG = {"de-f-young": "en", "de-m-middle": "en", "de-f-elderly": "en", "de-m-young": "en", "en-us-f-young": "de", "fr-m-middle": "en"}

# what each voice says first, in its language: the reference every clip of it is cloned from
REFERENCE = {"de": "Heute Abend kommen ein paar Freunde vorbei, wir kochen zusammen und spielen danach noch eine Runde Karten.",
             "en": "A few friends are coming over tonight, we will cook together and play a round of cards afterwards.",
             "fr": "Quelques amis viennent ce soir, nous allons cuisiner ensemble et jouer aux cartes ensuite.",
             "es": "Esta noche vienen unos amigos, vamos a cocinar juntos y después jugaremos a las cartas."}

# wake words as written in the text (Amazon's and microWakeWord's: tools/wakealign/build.sh)
WAKE = {"alexa": "Alexa", "computer": "Computer", "echo": "Echo", "amazon": "Amazon", "ziggy": "Ziggy",
        "okay_nabu": "Okay Nabu", "hey_jarvis": "Hey Jarvis", "hey_mycroft": "Hey Mycroft"}
STOP = {"de": "stopp", "en": "stop", "fr": "stop", "es": "para"}

# commands: short and long, starting on vowels, plosives, fricatives, nasals, liquids (where a cut shows: issue #79),
# numbers in words (verify.py reads canary's digits back)
COMMANDS = {
    "de": ["mach das Licht an", "Licht aus", "an", "aus", "Pause", "lauter", "leiser", "wie spät ist es",
           "wie wird das Wetter morgen", "stell einen Timer auf fünf Minuten", "erinnere mich morgen an den Müll",
           "spiel Musik", "nächstes Lied", "schalte die Heizung auf zweiundzwanzig Grad", "öffne die Rollläden",
           "ist die Haustür abgeschlossen", "mach den Fernseher aus", "gute Nacht", "Licht im Wohnzimmer an",
           "setz Milch auf die Einkaufsliste", "wie viel Uhr ist es", "Rollläden runter", "ein bisschen heller",
           "noch zehn Minuten", "abbrechen"],
    "en": ["turn on the lights", "lights off", "on", "off", "pause", "louder", "volume down", "what time is it",
           "what's the weather tomorrow", "set a timer for five minutes", "remind me to take out the trash tomorrow",
           "play some music", "next track", "set the thermostat to seventy two", "open the blinds",
           "is the front door locked", "turn off the TV", "good night", "turn on the living room lights",
           "add milk to the shopping list", "how long is left on the timer", "lower the blinds", "a bit brighter",
           "ten more minutes", "cancel"],
    "fr": ["allume la lumière", "éteins la lumière", "pause", "plus fort", "moins fort", "quelle heure est-il",
           "quel temps fera-t-il demain", "mets un minuteur de cinq minutes", "rappelle-moi de sortir les poubelles",
           "joue de la musique", "chanson suivante", "règle le chauffage sur vingt-deux degrés", "ouvre les volets",
           "est-ce que la porte est fermée", "éteins la télé", "bonne nuit", "allume le salon",
           "ajoute du lait à la liste de courses", "baisse les volets", "un peu plus clair", "annule"],
    "es": ["enciende la luz", "apaga la luz", "pausa", "más alto", "más bajo", "qué hora es", "qué tiempo hará mañana",
           "pon un temporizador de cinco minutos", "recuérdame sacar la basura mañana", "pon música",
           "siguiente canción", "pon la calefacción a veintidós grados", "abre las persianas",
           "está cerrada la puerta", "apaga la tele", "buenas noches", "enciende la luz del salón",
           "añade leche a la lista de la compra", "baja las persianas", "un poco más claro", "cancela"],
}
PER_WAKE = 8                # commands per wake word and voice, each with a pause and in one breath
PER_WAKE_OTHER = 3          # in the other language


def items():
    """every clip of the corpus: (id, voice, language, wake, command or "", kind, pause, text)"""
    out = []
    for v, (_, home) in VOICES.items():
        rnd = random.Random(f"corpus {v}"); k = 0                  # ids by voice: another voice renumbers nothing
        def add(*it):
            nonlocal k
            out.append((f"{v}/{it[0]}-{it[1]}-{k:03d}", v) + it); k += 1
        for lang, n in [(home, PER_WAKE)] + ([(OTHER_LANG[v], PER_WAKE_OTHER)] if v in OTHER_LANG else []):
            said = []
            for w, ww in WAKE.items():
                for c in rnd.sample(COMMANDS[lang], n):
                    for pause in (True, False): add(lang, w, c, "command", pause, f"{ww}{',' if pause else ''} {c}.")
                    said.append(c)
                add(lang, w, "", "alone", False, f"{ww}.")
                if lang == home: add(lang, w, STOP[lang], "stop", True, f"{ww}, {STOP[lang]}.")
            for c in sorted(set(said)):                 # each command alone (a request from the button; align.py's joins)
                add(lang, "bare", c, "bare", False, f"{c[0].upper()}{c[1:]}.")
    return out


def to16k(a, rate):
    import torch, torchaudio
    x = torchaudio.functional.resample(torch.as_tensor(a, dtype=torch.float32), rate, 16000).numpy()
    return np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")


def write_wav(path, pcm):
    import wave
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path + ".tmp", "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(pcm.tobytes())
    os.replace(path + ".tmp", path)


def levels(pcm):
    x = pcm.astype(float); rms = np.sqrt((x * x).mean() + 1e-9)
    return round(20 * np.log10(rms / 32768), 1), int(np.abs(x).max())


def lock(out):
    """one of make.py, verify.py, align.py on a corpus at a time: each writes the whole manifest (two at once lost rows)"""
    import fcntl
    os.makedirs(out, exist_ok=True); f = open(os.path.join(out, ".lock"), "w")
    try: fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError: sys.exit(f"{out}: another make.py, verify.py or align.py works on it")
    lock.held = f


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("out"); ap.add_argument("--redo", action="store_true")
    ap.add_argument("--voices", help="only these (comma separated), to try one")
    a = ap.parse_args()
    only = set(a.voices.split(",")) if a.voices else set(VOICES)
    lock(a.out)
    import torch
    from omnivoice.models.omnivoice import OmniVoice
    os.environ.setdefault("HF_HOME", os.path.join(ROOT, "toolchain", "omnivoice", "hf"))
    m = OmniVoice.from_pretrained("k2-fsa/OmniVoice", device_map="cuda", dtype=torch.float16)
    man = os.path.join(a.out, "manifest.jsonl")
    have = {}
    if os.path.exists(man):
        for l in open(man, encoding="utf-8"): j = json.loads(l); have[j["id"]] = j
    # the voices: each one's reference from its description, made once, or again (--redo) when verify.py could not
    # make it out (voices/voices.json), and then every clip of that voice too
    vj_path = os.path.join(a.out, "voices", "voices.json")
    vj = json.load(open(vj_path, encoding="utf-8")) if os.path.exists(vj_path) else {}
    refs = {}
    for v, (ins, lang) in VOICES.items():
        if v not in only: continue
        p = os.path.join(a.out, "voices", v + ".wav")
        if os.path.exists(p) and v not in vj: vj[v] = dict(description=ins, language=lang, text=REFERENCE[lang], tries=1)
        again = a.redo and vj.get(v, {}).get("ok") is False and vj[v]["tries"] < 6        # fr-m-teen: 4 seeds of 6
        if not os.path.exists(p) or again:
            n = vj[v]["tries"] + 1 if again else 1
            torch.manual_seed(zlib.crc32(v.encode() if n == 1 else f"{v} {n}".encode()))
            au = m.generate(text=REFERENCE[lang], language=lang, instruct=ins)
            write_wav(p, to16k(au[0], m.sampling_rate)); print(f"voice {v}: {ins}{f' (try {n})' if n > 1 else ''}", flush=True)
            vj[v] = dict(description=ins, language=lang, text=REFERENCE[lang], tries=n)
            if again:
                for k in [k for k, j in have.items() if j["voice"] == v]: del have[k]
        refs[v] = m.create_voice_clone_prompt(ref_audio=p, ref_text=REFERENCE[lang])
    os.makedirs(os.path.dirname(vj_path), exist_ok=True)
    with open(vj_path + ".tmp", "w", encoding="utf-8") as f: json.dump(vj, f, ensure_ascii=False, indent=1)
    os.replace(vj_path + ".tmp", vj_path)
    todo = []
    for it in items():
        j = have.get(it[0])
        # a voice made again waits for verify.py to pass its reference before its clips are made from it
        if vj.get(it[1], {}).get("tries", 1) > 1 and vj[it[1]].get("ok") is not True: continue
        if it[1] in only and (j is None or (a.redo and j.get("ok") is False and j.get("tries", 1) < 3)): todo.append(it)
    print(f"{len(todo)} clips to make ({len(have)} in the manifest)", flush=True)
    for v in [v for v in VOICES if v in only]:          # by voice: one clone prompt a batch
        mine = [it for it in todo if it[1] == v]
        for b in range(0, len(mine), 16):
            batch = mine[b:b + 16]
            tries = [have[it[0]].get("tries", 1) + 1 if it[0] in have else 1 for it in batch]
            torch.manual_seed(zlib.crc32(f"{v} {b} {max(tries)}".encode()))
            try: aus = m.generate(text=[it[7] for it in batch], language=[it[2] for it in batch], voice_clone_prompt=[refs[v]] * len(batch))
            except RuntimeError:                         # one of them breaks it
                aus = []
                for it in batch:
                    try: aus.append(m.generate(text=it[7], language=it[2], voice_clone_prompt=refs[v])[0])
                    except RuntimeError as e: aus.append(e); print(f"{it[0]}: not made ({e})", flush=True)
            for it, au, t in zip(batch, aus, tries):
                row = dict(id=it[0], voice=v, voice_description=VOICES[v][0], voice_language=VOICES[v][1],
                           language=it[2], wake=it[3] if it[3] in WAKE else "", wake_text=WAKE.get(it[3], ""), command=it[4],
                           kind=it[5], comma=it[6], text=it[7], tries=t)
                if isinstance(au, Exception) or len(au) == 0:   # turned down, as verify.py would: --redo tries again
                    have[it[0]] = dict(row, heard="", ok=False, why=f"not made: {au if isinstance(au, Exception) else 'no audio'}"); continue
                pcm = to16k(au, m.sampling_rate); rel = it[0] + ".wav"; write_wav(os.path.join(a.out, rel), pcm)
                rms, peak = levels(pcm)
                have[it[0]] = dict(row, file=rel, seconds=round(len(pcm) / 16000, 3), rms_dbfs=rms, peak=peak)
            print(f"{v}: {min(b + 16, len(mine))} of {len(mine)}", flush=True)
            with open(man + ".tmp", "w", encoding="utf-8") as f:
                for j in sorted(have.values(), key=lambda j: j["id"]): f.write(json.dumps(j, ensure_ascii=False) + "\n")
            os.replace(man + ".tmp", man)


if __name__ == "__main__":
    main()
