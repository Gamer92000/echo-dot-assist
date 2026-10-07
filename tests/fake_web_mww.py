#!/usr/bin/env python3
"""The wake word engines of the settings page on build/hassmic-host.  microWakeWord: models managed from the settings page (tests/webclient.py as the browser),
Home Assistant (aioesphomeapi) watching the wake word list, and real detections: the PC build has no Amazon engine
(SIGUSR1 stands in for it), but microWakeWord runs there as on the Echo, on testdata/alexa_espeak.raw looped.
Upload with and without a manifest, refusals (not a model, a bad id, a manifest for other features), the engine switch
(refused without a model), rename, cutoff and window, a second model and the pick, downloads, a copy as an artifact
(the Echos section's way), deletes down to none (back to Amazon's engine, saved), and a start with microWakeWord on.
Home Assistant's wake word (the stream, as -w remote): the switch asks root for a restart, the restarted satellite
streams to Home Assistant, which gets no wake words of ours, and its "wake word heard" event is answered as a wake word
of our own; back again the same way, and nothing to switch while hassmic.conf says -w remote."""
from aioesphomeapi import VoiceAssistantEventType
import asyncio, hashlib, json, os, signal, subprocess, sys, tempfile, time
from aioesphomeapi import APIClient
import pymicro_wakeword
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import Browser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODELS = os.path.join(os.path.dirname(pymicro_wakeword.__file__), "models")
PORT, WEB = 16981, 16982


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def upload(b, mid, model, manifest=None):
    data = open(os.path.join(MODELS, model + ".tflite"), "rb").read() if isinstance(model, str) else model
    j = (open(os.path.join(MODELS, manifest + ".json"), "rb").read() if manifest and not manifest.startswith("{") else (manifest or "").encode())
    st, _, body = b.call("POST", f"/api/mww/add/{mid}", f"{len(j)}\n".encode() + j + data)
    return st, json.loads(body)


def mww(b):
    st, _, body = b.call("GET", "/api/mww")
    return json.loads(body) if st == 200 else {}


async def ha_config():
    ha = APIClient("127.0.0.1", PORT, None); await ha.connect(login=True)
    await ha.list_entities_services()
    cfg = await ha.get_voice_assistant_configuration(5)
    return ha, sorted((w.id, w.wake_word) for w in cfg.available_wake_words), list(cfg.active_wake_words)


def start(state, log, extra=()):
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(state, "models"),
               HASSMIC_FAKE_WHISPER="1", HASSMIC_LOG=log, HASSMIC_CLOCK_SYNCED="0")
    return subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo MWW", "-L", "-z", "0", "-o", "0",
                             "-a", "0", "-W", str(WEB), *extra], env=env, stderr=open(log, "a"))


def restart(proc, state, log, extra=()):
    """what root's watcher does on state/restart (main.sh): the satellite stopped and started again"""
    os.remove(os.path.join(state, "restart"))
    proc.send_signal(signal.SIGTERM); proc.wait(5)
    return start(state, log, extra)


async def main():
    state = tempfile.mkdtemp(); log = os.path.join(state, "log")
    cfg = lambda: dict(l.strip().split("=", 1) for l in open(os.path.join(state, "config")) if "=" in l and not l.startswith("#"))
    proc = start(state, log)
    try:
        await asyncio.sleep(0.6)
        b = Browser(WEB)
        check(b.login_with_button(proc), "logged in with the button")
        m = mww(b)
        check(m.get("engine") == "amazon" and m.get("models") == [] and m.get("active") is None, f"Amazon's engine by default, no models: {m}")
        r = b.set(wake_engine="microwakeword")
        check(r.get("applied") == 0 and "no microWakeWord model" in r.get("errors", ""), f"microWakeWord without a model: refused: {r}")

        st, r = upload(b, "broken", os.urandom(4000))
        check(st == 400 and "TensorFlow Lite" in r.get("error", ""), f"not a model: refused: {r}")
        st, r = upload(b, "Bad/Id", "alexa", "alexa")
        check(st == 400 and "id" in r.get("error", ""), f"a bad id: refused: {r}")
        st, r = upload(b, "alexa", "alexa", '{"type":"micro","wake_word":"Alexa","micro":{"probability_cutoff":0.9,"feature_step_size":20}}')
        check(st == 400 and "feature_step_size" in r.get("error", ""), f"a manifest for 20 ms features: refused: {r}")
        st, r = upload(b, "alexa", "alexa", "alexa")
        m = mww(b)
        check(st == 200 and [(x["id"], x["name"], x["cutoff"], x["window"], x["langs"]) for x in m["models"]] == [("alexa", "Alexa", 0.9, 5, "en")],
              f"uploaded with its manifest: {m['models']}")
        man = json.load(open(os.path.join(state, "mww", "alexa", "manifest.json")))
        check(man["model"] == "model.tflite" and man["micro"]["probability_cutoff"] == 0.9 and man["author"] == "Kevin Ahrendt",
              "the manifest written anew in microWakeWord's format")
        st, r = upload(b, "hey_jarvis", "hey_jarvis")
        hj = [x for x in mww(b)["models"] if x["id"] == "hey_jarvis"]
        check(st == 200 and hj and hj[0]["name"] == "Hey Jarvis" and hj[0]["cutoff"] == 0.97, f"uploaded without a manifest: named after the id, defaults: {hj}")

        # Home Assistant before the switch: Amazon's list (the PC's stand-in "Alexa")
        ha, avail, active = await ha_config()
        check(all(not i.startswith("hey_jarvis") for i, _ in avail), f"Amazon's engine: Home Assistant lists its wake words: {avail}")
        started = asyncio.Event(); phrases = []
        async def handle_start(conv_id, flags, settings, phrase): phrases.append(phrase); started.set(); return 0
        async def nothing(*a, **k): pass
        ha.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        await asyncio.sleep(0.5)

        n0 = len(open(log).read())
        r = b.set(wake_engine="microwakeword"); await asyncio.sleep(1.0)
        check(r.get("applied") == 1 and cfg().get("wake_engine") == "microwakeword" and "engine now microWakeWord" in open(log).read()[n0:],
              f"the engine switched, saved: {r}")
        check("links closed" in open(log).read()[n0:], "Home Assistant sent away to read the new list")
        try: await ha.disconnect()
        except Exception: pass
        ha, avail, active = await ha_config()
        check(avail == [("alexa", "Alexa"), ("hey_jarvis", "Hey Jarvis")] and active == ["alexa"], f"Home Assistant lists microWakeWord's models: {avail}, {active}")
        ha.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        try: await asyncio.wait_for(started.wait(), 25)
        except asyncio.TimeoutError: pass
        check(started.is_set() and phrases[-1] == "Alexa" and "(microwakeword, mean" in open(log).read(),
              f"\"Alexa\" in the audio heard by microWakeWord: a pipeline for Home Assistant ({phrases})")

        st, _, body = b.call("GET", "/api/mww/file/alexa/model.tflite")
        check(st == 200 and body == open(os.path.join(MODELS, "alexa.tflite"), "rb").read(), "the model downloads as uploaded")
        st, _, body = b.call("GET", "/api/mww/file/alexa/../../config")
        check(st in (400, 404), "no other file through the download")

        st, _, body = b.call("POST", "/api/mww/edit/alexa", b"name=Computer Alexa\ncutoff=0.8\nwindow=7\n")
        a = [x for x in mww(b)["models"] if x["id"] == "alexa"][0]
        check(st == 200 and (a["name"], a["cutoff"], a["window"]) == ("Computer Alexa", 0.8, 7), f"renamed, cutoff and window changed: {a}")
        st, _, body = b.call("POST", "/api/mww/edit/alexa", b"cutoff=1.5\n")
        check(st == 400 and "cutoff" in json.loads(body)["error"], "a cutoff out of range: refused")
        st, _, body = b.call("POST", "/api/mww/use/hey_jarvis"); await asyncio.sleep(1.0)
        check(st == 200 and mww(b)["active"] == "hey_jarvis" and open(os.path.join(state, "mww_word")).read().strip() == "hey_jarvis"
              and "microwakeword: \"Hey Jarvis\" loaded" in open(log).read(), "picked on the page: active, kept, loaded")
        try: await ha.disconnect()
        except Exception: pass
        ha, avail, active = await ha_config()
        check(("alexa", "Computer Alexa") in avail and active == ["hey_jarvis"], f"Home Assistant: the new name, the page's pick: {avail}, {active}")
        await ha.set_voice_assistant_configuration(["alexa"]); await asyncio.sleep(0.6)
        check(mww(b)["active"] == "alexa", "picked in Home Assistant: active on the page")

        # copied as an artifact, as the Echos section does from another Echo: begin, pieces, commit; no root, no restart
        st, _, body = b.call("GET", "/api/artifacts")
        arts = {x["id"]: x for x in json.loads(body)["artifacts"]}
        check("mww:alexa" in arts and sorted(f["name"] for f in arts["mww:alexa"]["files"]) == ["manifest.json", "model.tflite"],
              f"listed among the artifacts: {sorted(arts)}")
        files = {f["name"]: b.call("GET", f"/api/artifact/read/mww:alexa/{f['name']}/0/{f['size']}")[2] for f in arts["mww:alexa"]["files"]}
        h = hashlib.blake2b(digest_size=32)
        for name in sorted(files): h.update(name.encode() + b"\0" + str(len(files[name])).encode() + b"\0" + files[name])
        check(h.hexdigest() == arts["mww:alexa"]["digest"], "its digest as artifacts.c makes it")
        spec = f"mww:alexa2 {h.hexdigest()}\n" + "".join(f"{n} {len(d)}\n" for n, d in files.items())
        ok = b.call("POST", "/api/artifact/begin", spec.encode())[0] == 200
        for n, d in files.items(): ok &= b.call("POST", f"/api/artifact/chunk/mww:alexa2/{n}/0", d)[0] == 200
        st, _, body = b.call("POST", "/api/artifact/commit/mww:alexa2"); await asyncio.sleep(0.3)
        check(ok and st == 200 and any(x["id"] == "alexa2" and x["name"] == "Computer Alexa" for x in mww(b)["models"])
              and not os.path.exists(os.path.join(state, "artifacts", "mww.alexa2")), f"a copy committed: in place at once, nothing staged: {body}")
        bad = {"manifest.json": files["manifest.json"], "model.tflite": files["model.tflite"][:-100]}     # whole, but not a model
        h = hashlib.blake2b(digest_size=32)
        for name in sorted(bad): h.update(name.encode() + b"\0" + str(len(bad[name])).encode() + b"\0" + bad[name])
        b.call("POST", "/api/artifact/begin", (f"mww:bad {h.hexdigest()}\n" + "".join(f"{n} {len(d)}\n" for n, d in bad.items())).encode())
        for n, d in bad.items(): b.call("POST", f"/api/artifact/chunk/mww:bad/{n}/0", d)
        st, _, body = b.call("POST", "/api/artifact/commit/mww:bad")
        check(st == 400 and not any(x["id"] == "bad" for x in mww(b)["models"]) and not os.path.exists(os.path.join(state, "artifacts", "mww.bad")),
              f"a copy that arrived whole but does not load: refused, nothing left: {json.loads(body)}")

        # deleted down to none: the active one goes first, then Amazon's engine is back
        for mid in ("alexa", "alexa2"):
            check(b.call("POST", f"/api/mww/delete/{mid}")[0] == 200, f"deleted {mid}")
        await asyncio.sleep(0.6)
        check(mww(b)["active"] == "hey_jarvis" and not os.path.exists(os.path.join(state, "mww", "alexa")), "the active one deleted: the next one active")
        check(b.call("POST", "/api/mww/delete/nope")[0] == 400, "deleting one that is not there: refused")

        # a start with microWakeWord on: loaded at once, Amazon's model not tried first
        proc.send_signal(signal.SIGTERM); proc.wait(5)
        n0 = len(open(log).read())
        proc = start(state, log); await asyncio.sleep(1.0)
        tail = open(log).read()[n0:]
        check("microwakeword: \"Hey Jarvis\" loaded" in tail and "(en, microWakeWord), active" in tail and "wake word: now" not in tail,
              "restarted: microWakeWord's pick loaded from the start, no Amazon model before it")
        b = Browser(WEB, b.sk)
        check(b.call("POST", "/api/mww/delete/hey_jarvis")[0] == 200, "deleted the last one")
        await asyncio.sleep(0.8)
        m = mww(b)
        check(m["engine"] == "amazon" and m["models"] == [] and cfg().get("wake_engine") == "amazon" and "back to Amazon's engine" in open(log).read(),
              f"no model left: Amazon's engine again, saved: {m}")
        try: await ha.disconnect()
        except Exception: pass

        # Home Assistant listens: a restart, then the stream
        r = b.set(wake_engine="homeassistant")
        check(r.get("applied") == 1 and cfg().get("wake_engine") == "homeassistant" and open(os.path.join(state, "restart")).read() == "wake word engine\n"
              and mww(b)["engine"] == "homeassistant", f"Home Assistant's wake word: saved, the restart asked of root: {r}")
        n0 = len(open(log).read())
        proc = restart(proc, state, log); await asyncio.sleep(1.0)
        check(", wake=remote" in open(log).read()[n0:], "restarted: the wake word is Home Assistant's (as -w remote)")
        b = Browser(WEB, b.sk)
        ha, avail, active = await ha_config()
        check(avail == [] and active == [], f"Home Assistant is offered no wake words of the Echo's: {avail}")
        starts = []; first = asyncio.Event()
        async def remote_start(conv_id, flags, settings, phrase): starts.append((flags, phrase)); first.set(); return 0
        ha.subscribe_voice_assistant(handle_start=remote_start, handle_stop=nothing, handle_audio=nothing)
        try: await asyncio.wait_for(first.wait(), 5)
        except asyncio.TimeoutError: pass
        check(starts and starts[0][0] & 2, f"the Echo streams at once, Home Assistant to find the wake word (flags {starts[:1]})")
        n0 = len(open(log).read())
        ha.send_voice_assistant_event(VoiceAssistantEventType.VOICE_ASSISTANT_WAKE_WORD_END, None); await asyncio.sleep(0.5)
        check("state: idle -> listening" in open(log).read()[n0:], "Home Assistant heard the wake word: the Echo listens, as on its own wake word")
        r = b.set(wake_engine="microwakeword")
        check(r.get("applied") == 0 and "no microWakeWord model" in r.get("errors", "") and cfg().get("wake_engine") == "homeassistant",
              f"to microWakeWord without a model: refused here too: {r}")
        r = b.set(wake_engine="amazon")
        check(r.get("applied") == 1 and cfg().get("wake_engine") == "amazon" and os.path.exists(os.path.join(state, "restart")), f"back to Amazon's: a restart again: {r}")
        try: await ha.disconnect()
        except Exception: pass
        n0 = len(open(log).read())
        proc = restart(proc, state, log); await asyncio.sleep(1.0)
        check(", wake=local" in open(log).read()[n0:], "restarted: the wake word is the Echo's own again")

        # hassmic.conf's -w remote decides: the page cannot switch it
        proc.send_signal(signal.SIGTERM); proc.wait(5)
        proc = start(state, log, ("-w", "remote")); await asyncio.sleep(1.0)
        b = Browser(WEB, b.sk)
        r = b.set(wake_engine="microwakeword")
        check(mww(b)["engine"] == "homeassistant" and r.get("applied") == 0 and "-w remote" in r.get("errors", "") and not os.path.exists(os.path.join(state, "restart")),
              f"-w remote in hassmic.conf: the page shows Home Assistant's and switches nothing: {r}")
    finally:
        proc.send_signal(signal.SIGTERM)
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
    if check.failed:
        print(open(log).read()[-4000:])
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
