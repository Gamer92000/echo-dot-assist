#!/usr/bin/env python3
"""The move from the built-in microWakeWord engine (deprecated) to the microWakeWord plugin (docs/plugins.md 5.9, as
built; web.c mww_migrate, the Wake word page's "Move to the plugin"), end to end on the PC: build/hassmic-host under
build/plugrun-host, the real plugin (hassmic-plugin-microwakeword beside this tree, or HASSMIC_MWW_PLUGIN: its PC
build, made here with its own `make host`) from a stand-in plugin index (tests/plugsite.py), pymicro-wakeword's
models, testdata/alexa_espeak.raw as the microphone, this script as the settings page and as Home Assistant.

The built-in engine with two models, both tuned on its page (one below the plugin's lowest threshold), both
listening (Home Assistant's two selects), and Home Assistant's problem sensor that says it is deprecated.  A plugin
that calls itself microwakeword and is not the official one (a file, another author's key): the move refuses to
copy the models to it unless told to.  The page's steps as it takes them: plugins switched on, the plugin fetched
from the index and its install waiting for the action button (cancelled: nothing installed, the engine as it was;
then pressed); the move refused while the plugin does not run (the engine as it was); then the models COPIED into
the plugin's own list through the store's item path (the plugin's own check), byte for byte, their threshold and
window as tuned (the one out of the plugin's range brought into it, and said), state/mww as it was with nothing of
the plugin's beside it, the same two wake words in the plugin's slots, the engine the plugin, the problem sensor
gone, and a real "Alexa" in the audio heard by the plugin.  The two stores apart: a model deleted or uploaded anew
on the plugin's card, one added anew on the built-in engine's page, each leaves the other's alone.  Then the switch
back: the engine setting alone, the built-in engine listening for the same two words.  The copy beside another
engine (Amazon's): the models only, the engine as it was, and the switch as its own step.  Last, an Echo whose
plugin read state/mww under a build before (its marks there, no list of its own): its own copies at the next start.

Without the plugin's tree it says so and passes (CI checks out this repository alone); its own `make test` runs it in
this tree's PC build."""
import asyncio, hashlib, json, os, shutil, signal, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser
import plugsite
from plugsite import plugcheck
import fake_plugins as fp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TREE = os.environ.get("HASSMIC_MWW_PLUGIN", os.path.join(os.path.dirname(ROOT), "hassmic-plugin-microwakeword"))
SITE = 16993
check = fp.check


def art_copy(b, aid, files):
    """the page's Copy models from another Echo: its file list and digest, the pieces, the commit (artifacts.c)"""
    names = sorted(files); h = hashlib.blake2b(digest_size=32)
    for n in names: h.update(n.encode() + b"\0"); h.update(str(len(files[n])).encode() + b"\0"); h.update(files[n])
    st, r = fp.post(b, "/api/artifact/begin", f"{aid} {h.hexdigest()}\n" + "".join(f"{n} {len(files[n])}\n" for n in names))
    for n in names:
        if st == 200 and files[n]: st, r = fp.post(b, f"/api/artifact/chunk/{aid}/{n}/0", files[n])
    return fp.post(b, f"/api/artifact/commit/{aid}") if st == 200 else (st, r)


def marks(state, m, title, fields):
    """what the plugin of a build with the alias left beside a model it had checked in state/mww"""
    for name, data in (("checked", ""), ("title", title + "\n"), ("fields", fields)): open(os.path.join(state, "mww", m, name), "w").write(data)


def unmarks(state, m):
    for name in ("checked", "title", "fields"):
        if os.path.exists(os.path.join(state, "mww", m, name)): os.unlink(os.path.join(state, "mww", m, name))


def fetch(b):
    """the page's fetch from the index: asked, then polled until the Echo checked the package or says why not"""
    st, r = fp.post(b, "/api/plugins/fetch", "microwakeword")
    if st != 200: return {"state": "refused", "error": r.get("error", "")}
    return fp.until(lambda: (lambda s: s if s.get("state") != "fetching" else None)(json.loads(b.call("GET", "/api/plugins/fetch")[2] or b"{}")), 30) or {}


async def main():
    if not os.path.isfile(os.path.join(TREE, "manifest.json")):
        print(f"skip: no hassmic-plugin-microwakeword tree at {TREE} (HASSMIC_MWW_PLUGIN names another place)"); return 0
    r = subprocess.run(["make", "-C", TREE, "host"], capture_output=True, text=True)
    if r.returncode: print(r.stdout + r.stderr); print("FAIL the plugin's PC build (make host in its tree)"); return 1
    import pymicro_wakeword
    models = os.path.join(os.path.dirname(pymicro_wakeword.__file__), "models")
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log")
    # (community plugins on, as the Plugins page's switch leaves them: this test installs plugins from a file; the switch itself: fake_plugins_community.py)
    open(os.path.join(state, "config"), "a").write("plugins_community=on\n")
    plugcheck.otatool.keygen(os.path.join(top, "author.key"), os.path.join(top, "author.pub"))
    site = plugsite.Site(top, SITE)           # (its key is top/index.key: the one fake_plugins.start has the Echo trust)
    pkg = os.path.join(top, "microwakeword.hmp")
    plugcheck.pack(os.path.join(TREE, "manifest.json"), os.path.join(TREE, "build", "microwakeword-host"), os.path.join(top, "author.key"), pkg,
                   license_=os.path.join(TREE, "LICENSE"), arch="x86_64")
    site.build({"microwakeword": [open(pkg, "rb").read()]}, serial=int(time.time()) - 60)
    env = {"HASSMIC_PLUGIN_INDEX": site.base, "HASSMIC_PLUGINS_BUILT": str(int(time.time())), "HASSMIC_PLUGIN_DAILY": "3600000:86400000:3600000"}
    unmark = lambda: os.path.exists(os.path.join(state, "plugins", ".mww-own")) and os.unlink(os.path.join(state, "plugins", ".mww-own"))
    cfg = lambda: dict(l.strip().split("=", 1) for l in open(os.path.join(state, "config")) if "=" in l and not l.startswith("#"))
    read = lambda *p: open(os.path.join(state, *p), "rb").read() if os.path.exists(os.path.join(state, *p)) else b""
    proc = fp.start(state, log, env=env)
    ha = None
    try:
        wait_port(fp.PORT, fp.WEB)
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "logged in with the button")

        # ---- the built-in engine as an owner has it: two models, one tuned, both listening
        for mid, man in (("alexa", open(os.path.join(models, "alexa.json"), "rb").read()), ("hey_jarvis", b"")):
            st, r = fp.post(b, f"/api/mww/add/{mid}", b"%d\n" % len(man) + man + open(os.path.join(models, mid + ".tflite"), "rb").read())
            check(st == 200, f"built-in engine: {mid} added on its page: {r}")
        # (hey_jarvis's manifest at 0.30, as a copy from another Echo or a build before leaves one: the built-in engine
        # reads any threshold between 0 and 1 from a manifest, its page's own slider and the plugin 0.5 to 0.99)
        jm = json.load(open(os.path.join(state, "mww", "hey_jarvis", "manifest.json"))); jm["micro"]["probability_cutoff"] = 0.3
        open(os.path.join(state, "mww", "hey_jarvis", "manifest.json"), "w").write(json.dumps(jm))
        st, r = fp.post(b, "/api/mww/edit/alexa", "cutoff=0.8\nwindow=7\n")
        r3 = b.set(wake_engine="microwakeword")
        st2, _, body = b.call("GET", "/api/mww")
        check(st == 200 and r3.get("applied") == 1 and cfg().get("wake_engine") == "microwakeword" and abs(next(m for m in json.loads(body)["models"] if m["id"] == "hey_jarvis")["cutoff"] - 0.3) < 1e-6,
              f"tuned (0.80 over 7; the other at 0.30 by its manifest, which the plugin does not take), the engine switched to it: {r3}")
        ha, avail, active = await fp.ha_config()
        await ha.set_voice_assistant_configuration(["alexa", "hey_jarvis"])
        check(fp.until(lambda: read("mww_word") == b"alexa\nhey_jarvis\n"), f"both listening, picked in Home Assistant's two selects: {read('mww_word')!r}")
        ents, _ = await ha.list_entities_services()
        dep = [e for e in ents if e.object_id == "wake_word_engine_deprecated"]
        states = []; ha.subscribe_states(states.append)
        check(dep and dep[0].device_class == "problem" and "deprecated" in dep[0].name and "plugin" in dep[0].name, f"Home Assistant: a problem sensor that says the built-in engine is deprecated: {dep and dep[0].name}")
        for _ in range(50):                 # (the loop's own sleep: the states arrive on it)
            if dep and any(getattr(s, "key", None) == dep[0].key and s.state for s in states): break
            await asyncio.sleep(0.1)
        check(dep and any(getattr(s, "key", None) == dep[0].key and s.state for s in states), "... and it is on")
        await ha.disconnect(); ha = None
        before = {m: (read("mww", m, "manifest.json"), read("mww", m, "model.tflite")) for m in ("alexa", "hey_jarvis")}
        # (a review of 913ec8d: an Echo with state/mww and no plugin ever installed logged "cannot write
        # .../.mww-own" at every start, and the pass was marked done before any plugin could take it)
        proc.terminate(); proc.wait(5)
        marks(state, "alexa", "Alexa", "cutoff=0.80\nwindow=7\n")
        n0 = len(open(log).read())
        proc = fp.start(state, log, env=env); wait_port(fp.PORT, fp.WEB)
        check(fp.logged(log, "plugins: none can run", n0) and not fp.logged(log, "mww-own", n0, 2) and not os.path.exists(os.path.join(state, "plugins", ".mww-own"))
              and sorted(os.listdir(os.path.join(state, "mww", "alexa"))) == ["checked", "fields", "manifest.json", "model.tflite", "title"],
              "a start with the alias era's marks and no plugin installed: nothing written, nothing logged, the marks left for a start that has the plugin")
        unmarks(state, "alexa")      # (the move below as the built-in engine has it)

        # ---- the page's steps: plugins on, the plugin fetched, its install waits for the button
        st, r = fp.post(b, "/api/mww/migrate", "go")
        check(st == 200 and "not installed" in r.get("error", "") and cfg().get("wake_engine") == "microwakeword", f"the move before the plugin is here: refused, the engine as it was: {r}")
        r = b.set(plugins="on")
        check(r.get("applied") == 1, "plugins switched on (the page's first step where they are off)")

        # ---- a plugin that calls itself microwakeword, not the official one: a file, another author's key
        plugcheck.otatool.keygen(os.path.join(top, "other.key"), os.path.join(top, "other.pub"))
        side = os.path.join(top, "side.hmp")
        plugcheck.pack(os.path.join(TREE, "manifest.json"), os.path.join(TREE, "build", "microwakeword-host"), os.path.join(top, "other.key"), side,
                       license_=os.path.join(TREE, "LICENSE"), arch="x86_64")
        st, r = fp.upload(b, open(side, "rb").read())
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(st == 200 and r.get("tier") == "sideload" and fp.until(lambda: (fp.plug(b, "microwakeword") or {}).get("state") == "running", 20), f"a sideload named microwakeword installed from a file, with the button: {r.get('error')}")
        check(r.get("key") and fp.plug(b, "microwakeword").get("key") == r["key"] and fp.plug(b, "microwakeword").get("author") == "hassmic",
              f"its card has its author key's fingerprint beside the author it names itself (\"hassmic\": the move's dialog shows both): {r.get('key')} {fp.plug(b, 'microwakeword').get('key')}")
        st, r = fp.post(b, "/api/mww/migrate", "go")
        check(st == 200 and "not the official microWakeWord" in r.get("error", "") and "a file, not listed" in r.get("error", "") and r.get("copied") == 0 and r.get("tier") == "sideload"
              and cfg().get("wake_engine") == "microwakeword" and not os.path.exists(os.path.join(state, "plugins", "microwakeword", "files"))
              and not any(os.path.exists(os.path.join(state, "mww", m, "checked")) for m in ("alexa", "hey_jarvis")),
              f"the move to it: refused, naming its author and tier; nothing copied, nothing of the built-in engine's given to it, the engine as it was: {r}")
        # (a review of 61028a8: the copy at start of what the alias era left is for the official plugin alone; an id is
        # no proof of whose a plugin is, and this one was never given the folder by this build)
        proc.terminate(); proc.wait(5)
        for m in ("alexa", "hey_jarvis"): marks(state, m, "Alexa", "cutoff=0.80\nwindow=7\n")
        n0 = len(open(log).read())
        proc = fp.start(state, log, env=env); wait_port(fp.PORT, fp.WEB)
        check(fp.until(lambda: (fp.plug(b, "microwakeword") or {}).get("state") == "running", 20) and not fp.logged(log, "copied into its own list", n0, 2)
              and not os.path.exists(os.path.join(state, "plugins", "microwakeword", "files")) and not os.path.exists(os.path.join(state, "plugins", ".mww-own"))
              and all(sorted(os.listdir(os.path.join(state, "mww", m))) == ["checked", "fields", "manifest.json", "model.tflite", "title"] for m in ("alexa", "hey_jarvis")),
              "the alias era's marks at a start with the sideload installed: nothing of state/mww given to it, nothing marked done, the marks left (a review of 913ec8d)")
        for m in ("alexa", "hey_jarvis"): unmarks(state, m)     # (the move below as the built-in engine has it)
        st, r = fp.post(b, "/api/plugins/microwakeword/remove", "remove")
        check(st == 200 and fp.until(lambda: not fp.plug(b, "microwakeword")), f"(removed: {r})")
        f = fetch(b)
        check(f.get("state") == "checked" and f.get("check", {}).get("tier") == "official" and f["check"].get("button") and len(f["check"].get("caps", [])) == 2,
              f"the plugin fetched from the plugin index: official, two capabilities, the button to accept them: {f.get('state')} {f.get('error')}")
        st, r = fp.post(b, "/api/plugins/install")
        check(st == 200 and r.get("install") == "waiting", f"its install waits for the action button: {r}")
        fp.post(b, "/api/plugins/cancel")
        time.sleep(1)
        check(not fp.plug(b, "microwakeword") and not os.path.exists(os.path.join(state, "plugins", "microwakeword")) and cfg().get("wake_engine") == "microwakeword",
              "no press, cancelled: nothing installed, the built-in engine listens as before")
        f = fetch(b)
        st, r = fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(f.get("state") == "checked" and fp.until(lambda: (fp.plug(b, "microwakeword") or {}).get("state") == "running", 20), "fetched again, the button pressed: installed, running")
        check(cfg().get("wake_engine") == "microwakeword", "(installing it switched nothing)")

        # ---- a failure on the way leaves the engine as it was
        fp.post(b, "/api/plugins/microwakeword/enable", "off")
        fp.until(lambda: (fp.plug(b, "microwakeword") or {}).get("state") != "running")
        st, r = fp.post(b, "/api/mww/migrate", "go")
        check(st == 200 and "does not run" in r.get("error", "") and cfg().get("wake_engine") == "microwakeword" and not os.path.exists(os.path.join(state, "wake_word-microwakeword")),
              f"the plugin switched off: the move refused, the engine and its words as they were: {r}")
        fp.post(b, "/api/plugins/microwakeword/enable", "on")
        check(fp.until(lambda: (fp.plug(b, "microwakeword") or {}).get("state") == "running", 20), "(on again)")

        # ---- the move
        n0 = len(open(log).read())
        st, r = fp.post(b, "/api/mww/migrate", "go")
        check(st == 200 and not r.get("error") and r.get("models") == 2 and r.get("copied") == 2 and r.get("words") == ["alexa", "hey_jarvis"] and r.get("engine") == "plugin:microwakeword"
              and r.get("switched") is True and r.get("refused") == [], f"moved: 2 of 2 models, the same two wake words, the engine the plugin: {r}")
        adj = {x["id"]: x["why"] for x in r.get("adjusted", [])}
        check(list(adj) == ["hey_jarvis"] and "0.30" in adj["hey_jarvis"] and "(0.5 to 0.99): copied with 0.50" in adj["hey_jarvis"],
              f"the threshold the plugin does not take: brought into its range and said, the model copied all the same: {r.get('adjusted')}")
        items = {x["id"]: x for x in {y["name"]: y for y in fp.plug(b, "microwakeword")["settings"]}["models"]["items"]}
        check(items.get("alexa", {}).get("checked") and items["alexa"].get("fields") == {"cutoff": "0.80", "window": "7"} and items["alexa"].get("title") == "Alexa",
              f"each checked by the plugin, the threshold and window as tuned on the built-in engine: {items.get('alexa')}")
        check(items.get("hey_jarvis", {}).get("checked") and items["hey_jarvis"].get("fields") == {"cutoff": "0.50", "window": "5"}, f"... the other at the plugin's lowest threshold: {items.get('hey_jarvis')}")
        own = lambda m, f: read("plugins", "microwakeword", "files", "models", m, f)
        check(all(before[m] == (own(m, "manifest.json"), own(m, "model.tflite")) for m in before),
              "copied: the plugin's own list has the built-in engine's files, byte for byte")
        check(all(before[m] == (read("mww", m, "manifest.json"), read("mww", m, "model.tflite")) and sorted(os.listdir(os.path.join(state, "mww", m))) == ["manifest.json", "model.tflite"] for m in before),
              f"state/mww as it was, nothing of the plugin's beside the files: {[sorted(os.listdir(os.path.join(state, 'mww', m))) for m in before]}")
        check(read("wake_word-microwakeword") == b"alexa\nhey_jarvis\n" and read("mww_word") == b"alexa\nhey_jarvis\n" and cfg().get("wake_engine") == "plugin:microwakeword",
              "the plugin's pick the built-in engine's, which is kept; the setting the plugin")
        check(fp.logged(log, "\"alexa\" listens", n0) and fp.logged(log, "\"hey_jarvis\" listens (second wake word)", n0), "the plugin listens for both, in their slots")
        ha, avail, active = await fp.ha_config()
        ents, _ = await ha.list_entities_services()
        check(active == ["alexa", "hey_jarvis"] and ("alexa", "Alexa") in avail and not [e for e in ents if e.object_id == "wake_word_engine_deprecated"],
              f"Home Assistant: the same two active, the deprecated sensor gone: {active} {avail}")
        started = asyncio.Event(); phrases = []
        async def handle_start(conv_id, flags, settings, phrase): phrases.append(phrase); started.set(); return 0
        async def nothing(*a, **k): pass
        ha.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        try: await asyncio.wait_for(started.wait(), 25)
        except asyncio.TimeoutError: pass
        check(started.is_set() and phrases[-1] == "Alexa" and fp.logged(log, "wake: alexa (plugin microwakeword", n0), f"\"Alexa\" in the audio, heard by the plugin: a run for Home Assistant: {phrases}")
        await ha.disconnect(); ha = None

        # ---- the two stores apart: what changes on one leaves the other alone
        st, r = fp.post(b, "/api/plugins/microwakeword/item/models/hey_jarvis/delete")
        check(st == 200 and not own("hey_jarvis", "model.tflite") and before["hey_jarvis"] == (read("mww", "hey_jarvis", "manifest.json"), read("mww", "hey_jarvis", "model.tflite")),
              f"the plugin's copy deleted on its card: the built-in engine's model is still there, as it was: {r}")
        st, r = fp.item(b, "hey_jarvis", {".tflite": open(os.path.join(models, "hey_jarvis.tflite"), "rb").read()}, "microwakeword")
        check(st == 200 and not own("hey_jarvis", "manifest.json") and read("mww", "hey_jarvis", "manifest.json") == before["hey_jarvis"][0],
              f"uploaded anew on the plugin's card with its .tflite alone: the built-in engine keeps its manifest: {r}")
        man = open(os.path.join(models, "alexa.json"), "rb").read()
        st, r = fp.post(b, "/api/mww/add/alexa", b"%d\n" % len(man) + man + open(os.path.join(models, "alexa.tflite"), "rb").read())
        items = {x["id"]: x for x in {y["name"]: y for y in fp.plug(b, "microwakeword")["settings"]}["models"]["items"]}
        check(st == 200 and items.get("alexa", {}).get("checked") and items["alexa"].get("title") == "Alexa" and items["alexa"].get("fields") == {"cutoff": "0.80", "window": "7"},
              f"a new version of alexa on the built-in engine's page: the plugin's copy keeps its check, title and fields: {items.get('alexa')}")
        fp.post(b, "/api/mww/edit/alexa", "cutoff=0.8\nwindow=7\n")

        # ---- copied again while the plugin listens (a review of 61028a8): a model the plugin has gets the built-in
        # engine's files and keeps the threshold and window set on the plugin's card; a new one comes with the built-in
        # engine's, 0.99 (the top of both ranges) taken as it is; "switch" to the plugin that listens: nothing changes
        st, r = fp.post(b, "/api/plugins/microwakeword/item/models/alexa/field", "cutoff=0.85")
        check(st == 200, f"(the plugin's alexa tuned on its card: 0.85) {r}")
        man = open(os.path.join(models, "okay_nabu.json"), "rb").read()
        st, r = fp.post(b, "/api/mww/add/okay_nabu", b"%d\n" % len(man) + man + open(os.path.join(models, "okay_nabu.tflite"), "rb").read())
        st2, r2 = fp.post(b, "/api/mww/edit/okay_nabu", "cutoff=0.99\n")
        check(st == 200 and st2 == 200, f"(okay_nabu added on the built-in engine's page, at 0.99) {r} {r2}")
        pick0 = read("wake_word-microwakeword")
        st, r = fp.post(b, "/api/mww/migrate", "go")
        items = {x["id"]: x for x in {y["name"]: y for y in fp.plug(b, "microwakeword")["settings"]}["models"]["items"]}
        check(st == 200 and not r.get("error") and r.get("copied") == 3 and r.get("switched") is False and r.get("engine") == "plugin:microwakeword"
              and "alexa" in r.get("kept", []) and items.get("alexa", {}).get("fields") == {"cutoff": "0.85", "window": "7"} and own("alexa", "model.tflite") == before["alexa"][1],
              f"copied again: the plugin's alexa has the built-in engine's files and keeps its own 0.85 (said as kept): {r} {items.get('alexa')}")
        check("okay_nabu" not in [x["id"] for x in r.get("adjusted", [])] and items.get("okay_nabu", {}).get("checked") and items["okay_nabu"].get("fields", {}).get("cutoff") == "0.99",
              f"the new one at 0.99: copied as it is, nothing said to be outside the plugin's range: {r.get('adjusted')} {items.get('okay_nabu')}")
        st, r = fp.post(b, "/api/mww/migrate", "switch")
        check(st == 200 and "is the wake word engine already" in r.get("error", "") and r.get("switched") is False and read("wake_word-microwakeword") == pick0
              and cfg().get("wake_engine") == "plugin:microwakeword", f"\"switch\" while the plugin listens: refused, its pick as it was: {r}")
        # (a review of 913ec8d: 0.995 was rounded to 1.00 before the clamp, and the note named 1.00)
        fp.post(b, "/api/plugins/microwakeword/item/models/okay_nabu/delete")
        jm = json.load(open(os.path.join(state, "mww", "okay_nabu", "manifest.json"))); jm["micro"]["probability_cutoff"] = 0.995
        open(os.path.join(state, "mww", "okay_nabu", "manifest.json"), "w").write(json.dumps(jm))
        st, r = fp.post(b, "/api/mww/migrate", "go")
        adj = {x["id"]: x["why"] for x in r.get("adjusted", [])}
        items = fp.until(lambda: (lambda it: it if it.get("okay_nabu", {}).get("checked") else None)(
            {x["id"]: x for x in {y["name"]: y for y in fp.plug(b, "microwakeword")["settings"]}["models"]["items"]}), 15) or {}
        check(st == 200 and "its threshold 0.995 is outside what the plugin takes (0.5 to 0.99): copied with 0.99" in adj.get("okay_nabu", "")
              and items.get("okay_nabu", {}).get("fields", {}).get("cutoff") == "0.99", f"one at 0.995: copied with 0.99, and the note says so of 0.995: {adj} {items.get('okay_nabu')}")
        fp.post(b, "/api/mww/delete/okay_nabu"); fp.post(b, "/api/plugins/microwakeword/item/models/okay_nabu/delete")
        fp.post(b, "/api/plugins/microwakeword/item/models/alexa/field", "cutoff=0.80")

        # ---- and back: the engine setting alone
        n0 = len(open(log).read())
        r = b.set(wake_engine="microwakeword")
        check(r.get("applied") == 1 and fp.logged(log, "wake word: engine now microWakeWord", n0) and cfg().get("wake_engine") == "microwakeword", f"switched back with one setting: {r}")
        ha, avail, active = await fp.ha_config()
        ents, _ = await ha.list_entities_services()
        st, _, body = b.call("GET", "/api/mww"); mw = json.loads(body)
        check(active == ["alexa", "hey_jarvis"] and [e for e in ents if e.object_id == "wake_word_engine_deprecated"] and mw.get("active") == "alexa" and mw.get("second") == "hey_jarvis"
              and next(m for m in mw["models"] if m["id"] == "alexa")["cutoff"] == 0.8, f"the built-in engine listens for the same two again, tuned as it was; the sensor back: {active} {mw.get('active')} {mw.get('second')}")
        await ha.disconnect(); ha = None

        # ---- the copy while another engine listens: the models only; the switch its own step
        r = b.set(wake_engine="amazon")
        check(r.get("applied") == 1 and cfg().get("wake_engine") == "amazon", "(Amazon's engine listens)")
        st, r = fp.post(b, "/api/mww/migrate", "go")
        check(st == 200 and not r.get("error") and r.get("copied") == 2 and r.get("switched") is False and r.get("engine") == "amazon" and r.get("words") == []
              and r.get("offer") == ["alexa", "hey_jarvis"] and cfg().get("wake_engine") == "amazon" and own("hey_jarvis", "manifest.json") == before["hey_jarvis"][0],
              f"copied beside Amazon's engine: the models (again, the plugin's copies get their files), the engine stays Amazon's, the switch offered with its words: {r}")
        st, r = fp.post(b, "/api/mww/migrate", "switch")
        check(st == 200 and not r.get("error") and r.get("switched") is True and r.get("copied") == 0 and r.get("words") == ["alexa", "hey_jarvis"] and cfg().get("wake_engine") == "plugin:microwakeword",
              f"the separate step: the engine the plugin, listening for those words, nothing copied: {r}")

        # ---- an Echo whose plugin read state/mww under a build before: its own list at the next start, once
        proc.terminate(); proc.wait(5)
        shutil.rmtree(os.path.join(state, "plugins", "microwakeword", "files"))
        check(not os.path.exists(os.path.join(state, "plugins", ".mww-own")), "(no start since the copy at start was last undone: this is such an Echo's first)")
        marks(state, "alexa", "Not <b>its</b> title", "cutoff=0.80\nwindow=7\n"); marks(state, "hey_jarvis", "Hey Jarvis", "cutoff=0.97\nwindow=5\nbogus=1\n")
        os.makedirs(os.path.join(state, "mww", "unchecked")); open(os.path.join(state, "mww", "unchecked", "model.tflite"), "wb").write(before["alexa"][1])
        open(os.path.join(state, "mww", "unchecked", "manifest.json"), "wb").write(before["alexa"][0])
        # (a review of 913ec8d: a start that did not read the plugin, here without its package for this build's API,
        # marked the pass done and stripped the marks: the fields tuned on its card in the alias era lost for good)
        pkgs = os.path.join(state, "plugins", "microwakeword", "pkg"); api = os.listdir(pkgs)[0]
        os.rename(os.path.join(pkgs, api), os.path.join(top, "mww-pkg"))
        n0 = len(open(log).read())
        proc = fp.start(state, log, env=env); wait_port(fp.PORT, fp.WEB)
        check(fp.until(lambda: fp.plug(b, "microwakeword") is not None) and not fp.logged(log, "copied into its own list", n0, 2)
              and not os.path.exists(os.path.join(state, "plugins", ".mww-own")) and read("mww", "hey_jarvis", "fields") == b"cutoff=0.97\nwindow=5\nbogus=1\n"
              and all(sorted(os.listdir(os.path.join(state, "mww", m))) == ["checked", "fields", "manifest.json", "model.tflite", "title"] for m in ("alexa", "hey_jarvis")),
              "a start where the plugin has no package for this build: nothing copied, nothing marked done, the marks and tuned fields left for the next start")
        check("key" not in (fp.plug(b, "microwakeword") or {"key": 1}), f"... its card without a key: its manifest was not read (a fingerprint of zeros named nobody's): {fp.plug(b, 'microwakeword')}")
        proc.terminate(); proc.wait(5)
        os.rename(os.path.join(top, "mww-pkg"), os.path.join(pkgs, api))
        n0 = len(open(log).read())
        proc = fp.start(state, log, env=env)
        wait_port(fp.PORT, fp.WEB)
        check(fp.logged(log, "2 models it listed from the built-in engine's folder (state/mww, a build before this one) copied into its own list, for it to check", n0, 15)
              and fp.until(lambda: (fp.plug(b, "microwakeword") or {}).get("state") == "running", 20), "the next start: the models it had checked there copied into its own list")
        items = fp.until(lambda: (lambda it: it if all(x.get("checked") for x in it.values()) and len(it) == 2 else None)(
            {x["id"]: x for x in {y["name"]: y for y in fp.plug(b, "microwakeword")["settings"]}["models"]["items"]}), 20) or {}
        check(sorted(items) == ["alexa", "hey_jarvis"] and items["alexa"].get("fields") == {"cutoff": "0.80", "window": "7"} and items["hey_jarvis"].get("fields") == {"cutoff": "0.97", "window": "5"}
              and all(before[m] == (own(m, "manifest.json"), own(m, "model.tflite")) for m in before),
              f"... the files byte for byte, the threshold and window tuned; a model it never checked there not given to it: {items}")
        check(items.get("alexa", {}).get("title") == "Alexa" and fp.logged(log, "plugin microwakeword", n0),
              f"... each checked and named by the plugin itself, not by the title left beside it (a review of 61028a8: copied raw): {items.get('alexa')}")
        check(all(sorted(os.listdir(os.path.join(state, "mww", m))) == ["manifest.json", "model.tflite"] for m in before) and before["alexa"] == (read("mww", "alexa", "manifest.json"), read("mww", "alexa", "model.tflite"))
              and os.path.exists(os.path.join(state, "plugins", ".mww-own")), "state/mww the built-in engine's alone again: its files as they were, the plugin's marks gone; the copy noted as made")
        check(fp.logged(log, "\"alexa\" listens", n0, 15), "the plugin listens for its word from its own list")
        # (a review of 913ec8d: it checks the copies one at a time; the word it listed later stayed out of the second
        # slot until a restart, while Home Assistant showed both)
        check(fp.logged(log, "\"hey_jarvis\" listens (second wake word)", n0, 15), "... and for the second of the pick, listed later, without a restart")
        ha, avail, active = await fp.ha_config(); await ha.disconnect(); ha = None
        check(active == ["alexa", "hey_jarvis"], f"Home Assistant: both active: {active}")

        # ---- once (a review of 61028a8): what reaches state/mww later with a `checked` beside it is never the plugin's
        st, r = fp.post(b, "/api/plugins/microwakeword/item/models/alexa/field", "cutoff=0.85")
        st2, r2 = art_copy(b, "mww:alexa", {"manifest.json": before["alexa"][0], "model.tflite": before["alexa"][1], "checked": b"", "title": b"Not Alexa\n", "fields": b"cutoff=0.55\nwindow=3\n"})
        arts = {a["id"]: a for a in json.loads(b.call("GET", "/api/artifacts")[2])["artifacts"]}
        check(st == 200 and st2 == 200 and sorted(os.listdir(os.path.join(state, "mww", "alexa"))) == ["manifest.json", "model.tflite"]
              and sorted(f["name"] for f in arts.get("mww:alexa", {}).get("files", [])) == ["manifest.json", "model.tflite"],
              f"a copy of mww:alexa as an Echo with the alias lists it (its marks too) from another Echo: only the model's two files kept, and listed: {r2} {arts.get('mww:alexa')}")
        marks(state, "alexa", "Not Alexa", "cutoff=0.55\nwindow=3\n")   # (a fallback to a build with the alias leaves them so)
        shutil.copytree(os.path.join(state, "mww", "alexa"), os.path.join(state, "mww", "later"))
        proc.terminate(); proc.wait(5)
        n0 = len(open(log).read())
        proc = fp.start(state, log, env=env); wait_port(fp.PORT, fp.WEB)
        fp.until(lambda: (fp.plug(b, "microwakeword") or {}).get("state") == "running", 20)
        items = {x["id"]: x for x in {y["name"]: y for y in fp.plug(b, "microwakeword")["settings"]}["models"]["items"]}
        check(not fp.logged(log, "copied into its own list", n0, 3) and sorted(items) == ["alexa", "hey_jarvis"] and items["alexa"].get("fields") == {"cutoff": "0.85", "window": "7"}
              and items["alexa"].get("title") == "Alexa" and sorted(os.listdir(os.path.join(state, "mww", "alexa"))) == ["manifest.json", "model.tflite"],
              f"the next start: nothing copied again, the plugin's alexa as it was (0.85, its own title), nothing new added; the marks gone: {items.get('alexa')}")

        # ---- the official plugin without `wake` (a review of 61028a8): never given what a wake word plugin had checked
        st, r = fp.post(b, "/api/plugins/microwakeword/remove", "remove")
        mw = json.load(open(os.path.join(TREE, "manifest.json"))); mw["capabilities"] = {"files": 4}; mw["requires"] = plugcheck.requires_for(mw["capabilities"]); mw["version"] = mw["version"] + 1
        open(os.path.join(top, "nowake.json"), "w").write(json.dumps(mw))
        plugcheck.pack(os.path.join(top, "nowake.json"), os.path.join(TREE, "build", "microwakeword-host"), os.path.join(top, "author.key"), os.path.join(top, "nowake.hmp"),
                       license_=os.path.join(TREE, "LICENSE"), arch="x86_64")
        site.build({"microwakeword": [open(pkg, "rb").read(), open(os.path.join(top, "nowake.hmp"), "rb").read()]}, serial=int(time.time()) - 30)
        f = fetch(b); st, r = fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(f.get("check", {}).get("tier") == "official" and fp.until(lambda: (fp.plug(b, "microwakeword") or {}).get("accepted") and not fp.plug(b, "microwakeword").get("wake"), 20),
              f"(the official microwakeword in a version without wake, installed with the button) {f.get('error')} {r}")
        proc.terminate(); proc.wait(5)
        marks(state, "alexa", "Alexa", "cutoff=0.80\nwindow=7\n")
        unmark()
        n0 = len(open(log).read())
        proc = fp.start(state, log, env=env); wait_port(fp.PORT, fp.WEB)
        fp.until(lambda: (fp.plug(b, "microwakeword") or {}).get("state") == "running", 20)
        check(not fp.logged(log, "copied into its own list", n0, 3) and not os.path.exists(os.path.join(state, "plugins", "microwakeword", "files", "models", "alexa"))
              and not os.path.exists(os.path.join(state, "plugins", ".mww-own"))
              and sorted(os.listdir(os.path.join(state, "mww", "alexa"))) == ["checked", "fields", "manifest.json", "model.tflite", "title"],
              "a start with a microwakeword without wake: nothing of state/mww given to it, nothing marked done, the marks left")
    finally:
        if ha:
            try: await ha.disconnect()
            except Exception: pass
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()
    if check.failed:
        print("---- log"); print(open(log, errors="replace").read()[-6000:])
    print("mww migrate: " + ("FAILED" if check.failed else "all passed"))
    return 1 if check.failed else 0

sys.exit(asyncio.run(main()))
