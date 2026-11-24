#!/usr/bin/env python3
"""Plugins, what a person at the Echo and Home Assistant see of them (docs/plugins.md 4.3, 5.4): build/hassmic-host
under build/plugrun-host with tests/fake_plugins.py's stand-in plugin, browser and Home Assistant.

The ring while an install waits for the action button: a pattern of its own (stock's "wait"), not the login's
(authenticated_setup_mode), set when the wait starts and unset when the button is pressed, when the page cancels and
when nobody presses in time; what a run left on is unset at the next start.  LED calls go into a file
(HASSMIC_FAKE_LED), as in tests/fake_web.py.

Home Assistant's event entity "Plugin": listed with its four types (installed, updated, removed, synced) while the
feature `plugins` is on and not while it is off (switching it closes the link, as every feature), and an install
fires installed, an update updated, a removal removed; with the feature off Home Assistant hears nothing."""
import asyncio, hashlib, json, os, signal, sys, tempfile, time
from aioesphomeapi import APIClient, Event, EventInfo
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser
import fake_plugins as fp
from fake_plugins import check, start, logged, plugins, plug, post, upload, plugcheck, ROOT, PORT, WEB


async def until(f, secs=8):
    """as fake_plugins.until, with the loop running: Home Assistant's side is read meanwhile"""
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if f(): return True
        await asyncio.sleep(0.05)
    return bool(f())


async def home_assistant():
    """connected as Home Assistant: the client, its entities by object id, and every state it is sent from now on"""
    ha = None
    for _ in range(40):                 # (the links were just closed: the port is there, the listing may not be yet)
        try:
            ha = APIClient("127.0.0.1", PORT, None); await ha.connect(login=True); break
        except Exception:
            ha = None; await asyncio.sleep(0.25)
    es, _ = await ha.list_entities_services(); states = []; ha.subscribe_states(states.append)
    return ha, {e.object_id: e for e in es}, states


async def main():
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log"); leds = os.path.join(top, "ledctrl.log")
    for k in ("author", "index"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    man = json.load(open(f"{ROOT}/tests/plugins/testwake.json"))
    def package(**changes):
        m = dict(man, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, "p.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/testwake", os.path.join(top, "author.key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        plugcheck.list_package(out, os.path.join(top, "index.key"), os.path.join(top, "o.hmp"), arch="x86_64")
        return open(os.path.join(top, "o.hmp"), "rb").read()
    lines = lambda: open(leds).read().splitlines() if os.path.exists(leds) else []
    ours = lambda: [l for l in lines() if l.endswith(" wait") or l.endswith(" authenticated_setup_mode")]
    waiting = lambda b: plugins(b).get("install", {}).get("state")
    there = lambda: os.path.exists(os.path.join(state, "plugins", "testwake"))
    def ask(b, pkg):
        """the page's install: uploaded, checked, and asked for; the LED calls from then on"""
        st, r = upload(b, pkg)
        open(leds, "w").close()
        st2, r2 = post(b, "/api/plugins/install")
        return st == 200 and st2 == 200 and r2.get("install")

    # the wait runs out after 4 s: long enough for a login's own press, short enough to see one that is never pressed
    proc = start(state, log, env={"HASSMIC_FAKE_LED": leds, "HASSMIC_LOGIN_MS": "4000", "HASSMIC_PLUGIN_SCOPES": "net.lan:0"})   # (a build without net.lan, as an older one would be: every build has every scope)
    ha = None
    try:
        wait_port(PORT, WEB)
        check("-u wait" in lines() and "-u authenticated_setup_mode" in lines(), "start: what a run before left on the ring, a login's or a plugin's install's, is unset")
        b = Browser(WEB)
        open(leds, "w").close()
        check(b.login() == "waiting" and ours() == ["-s authenticated_setup_mode"], f"a login waits: the login's pattern, not the plugin's ({ours()})")
        proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: b.login() == "approved") and ours() == ["-s authenticated_setup_mode", "-u authenticated_setup_mode"], f"pressed: logged in, the pattern off ({ours()})")

        ha, ents, states = await home_assistant()
        check("plugin" not in ents and "button" in ents, f"plugins are off: Home Assistant has no \"Plugin\" entity ({sorted(k for k in ents if 'plug' in k)})")

        # ---- the ring while an install waits: its own pattern, from the wait's start to its end, however it ends
        pkg = package()
        check(ask(b, pkg) == "waiting" and ours() == ["-s wait"], f"a plugin's install waits for the button: the ring shows \"wait\", not the login's pattern ({ours()})")
        st, r = post(b, "/api/plugins/cancel")
        check(st == 200 and await until(lambda: "-u wait" in lines()) and ours() == ["-s wait", "-u wait"] and waiting(b) != "waiting" and not there(), f"cancelled on the page: the pattern off, nothing installed ({ours()})")

        n0 = len(open(log).read())
        check(ask(b, pkg) == "waiting" and ours() == ["-s wait"], f"asked again: the pattern again ({ours()})")
        t0 = time.monotonic()
        await asyncio.sleep(2.5)
        check(ours() == ["-s wait"], "2.5 s on: still waiting, the pattern on")
        check(await until(lambda: "-u wait" in lines(), 6) and logged(log, "plugin install ran out without the button", n0, 2) and ours() == ["-s wait", "-u wait"],
              f"nobody pressed: the wait runs out by itself after {time.monotonic() - t0:.1f} s, the pattern off ({ours()})")
        check(waiting(b) == "failed" and not there(), f"and nothing is installed: {plugins(b).get('install')}")
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.5)      # (a press now is the assistant's, as any)
        check(not there() and not any(l.endswith(" wait") for l in lines()[2:]), "a press after it ran out installs nothing and shows nothing")
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.5)      # (and the run that press started is cancelled again)

        check(ask(b, pkg) == "waiting" and ours() == ["-s wait"], f"asked a third time: the pattern ({ours()})")
        n_ev = len([s for s in states if isinstance(s, Event)])
        proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: waiting(b) == "installed") and ours() == ["-s wait", "-u wait"] and there(), f"pressed: installed, the pattern off ({ours()})")
        check(logged(log, "plugins: testwake installed (plugins are off", 0, 3), "the log says it came, and that Home Assistant has no entity to hear of it")
        await asyncio.sleep(0.5)
        check(len([s for s in states if isinstance(s, Event)]) == n_ev, "plugins are off: Home Assistant is sent no event of it")

        # ---- Home Assistant's "Plugin": listed while the feature is on
        closed = len(open(log).read())
        r = b.set(plugins="on")
        check(r.get("applied") == 1 and logged(log, "entities changed: links closed", closed, 5), "plugins switched on: Home Assistant's links are closed, so that it lists the entities again")
        try: await ha.disconnect(force=True)
        except Exception: pass
        b = Browser(WEB, b.sk)
        ha, ents, states = await home_assistant()
        ev = ents.get("plugin")
        check(isinstance(ev, EventInfo) and ev.name == "Plugin" and list(ev.event_types) == ["installed", "updated", "removed", "synced"],
              f"listed now: the event entity \"Plugin\" with its four types: {ev}")
        evs = lambda: [s.event_type for s in states if isinstance(s, Event) and s.key == ev.key]
        check(await until(lambda: (plug(b) or {}).get("state") == "running", 15), f"(the plugin runs: {(plug(b) or {}).get('state')})")

        # an update that asks for nothing new: no button, no pattern, "updated"
        st, r = upload(b, package(version=2))
        open(leds, "w").close()
        st2, r2 = post(b, "/api/plugins/install")
        check(st == 200 and r.get("button") is False and st2 == 200 and r2.get("install") == "installed" and await until(lambda: evs() == ["updated"]),
              f"version 2 installed without the button: Home Assistant hears \"updated\" ({evs()})")
        check(not ours(), f"and the ring showed nothing: no press was asked for ({ours()})")
        check(logged(log, "plugins: testwake updated", 0, 2), "said in the log")

        # an update that asks for more: the button, the pattern, "updated" at the press and not before
        check(ask(b, package(version=3, capabilities={"wake": True, "files": 2})) == "waiting" and ours() == ["-s wait"], f"version 3 asks for more: it waits, the pattern on ({ours()})")
        await asyncio.sleep(0.5)
        check(evs() == ["updated"], "nothing is told while it waits")
        proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: evs() == ["updated", "updated"]) and ours() == ["-s wait", "-u wait"] and (plug(b) or {}).get("version") == 3, f"pressed: \"updated\", the pattern off ({evs()}, {ours()})")

        st, r = post(b, "/api/plugins/testwake/remove", "remove")
        check(st == 200 and await until(lambda: evs() == ["updated", "updated", "removed"]) and not there(), f"removed: \"removed\" ({evs()})")
        st, r = post(b, "/api/plugins/testwake/remove", "remove")
        await asyncio.sleep(0.3)
        check(st == 400 and evs()[-1] == "removed" and len(evs()) == 3, f"removed once more: there is none, and no second event ({r}, {evs()})")

        check(ask(b, pkg) == "waiting", "version 1 again, as a new plugin: the button")
        proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: evs() == ["updated", "updated", "removed", "installed"]) and there(), f"pressed: \"installed\" ({evs()})")
        check("synced" not in evs(), "(\"synced\" is listed and nothing fires it yet: nothing syncs)")
        # a package that is refused tells nothing
        st, r = upload(b, package(version=9, capabilities={"files": 1, "net.lan": [{"proto": "tcp", "ports": [80]}]}))
        check(st == 400 and "needs a newer hassmic: net.lan 1" in r.get("error", "") and len(evs()) == 4, f"a package this hassmic has not all of: refused in those words, no event: {r}")
        st, r = upload(b, package(version=9, capabilities={"wake": 1, "files": 1}))
        check(st == 400 and "true, or left out" in r.get("error", ""), f"a manifest with \"wake\": 1: refused: {r}")

        # ---- a package that did not come through the page (3.3: the broker starts none that breaks the rules)
        check(await until(lambda: (plug(b) or {}).get("state") == "running", 15), f"(the plugin runs again: {(plug(b) or {}).get('state')})")
        mpath = os.path.join(state, "plugins", "testwake", "pkg", "manifest.json"); kept = open(mpath, "rb").read()
        m = json.loads(kept); m["capabilities"] = {"wake": True, "files": 1, "state": True}; m["requires"] = {"core": 1, "wake": 1, "files": 1, "state": 1}
        open(mpath, "w").write(json.dumps(m))
        post(b, "/api/plugins/testwake/enable", "off"); st, r = post(b, "/api/plugins/testwake/enable", "on")
        await asyncio.sleep(1.5)
        p = plug(b) or {}
        check(st == 200 and p.get("state") == "stopped" and "way out" in p.get("detail", "") and not fp.plugin_pids(state),
              f"its manifest in state/ replaced by one with wake and the network: not started, and its card says why: {p.get('state')}: {p.get('detail')}")
        check(logged(log, "plugins: testwake is not started: a plugin that hears the microphone all the time", 0, 2), "said in the log")
        open(mpath, "wb").write(kept)
        post(b, "/api/plugins/testwake/enable", "off"); post(b, "/api/plugins/testwake/enable", "on")
        check(await until(lambda: (plug(b) or {}).get("state") == "running", 15) and not (plug(b) or {}).get("detail"), f"the manifest it was installed with back: it runs ({(plug(b) or {}).get('state')}: {(plug(b) or {}).get('detail')})")

        # ---- the sixth read (2026-11-19), as the page and Home Assistant see it
        # what the owner reads of the plugin itself is accepted with it: its description and its source's address
        cfg = os.path.join(state, "plugins", "testwake", "config"); stored = lambda: open(cfg).read() if os.path.exists(cfg) else ""
        st, r = upload(b, package(version=2, description="Type your Home Assistant password into A note", source="https://evil.example/login"))
        ch = r.get("changes", [])
        check(st == 200 and r.get("button") is True and r.get("update") is True and [c["kind"] for c in ch] == ["changed", "changed"]
              and ch[0]["line"] == 'What it says about itself: "Type your Home Assistant password into A note"' and ch[1]["line"] == 'Its "Source and issues" link opens https://evil.example/login',
              f"an update with another description and a source address: the button, and both shown as they would be: {ch}")
        post(b, "/api/plugins/cancel")
        n_ev = len(evs())
        st, r = upload(b, package(version=2, version_name="2.0 final", license="GPL-3.0")); st2, r2 = post(b, "/api/plugins/install")
        p = plug(b) or {}
        check(st == 200 and r.get("button") is False and not r.get("changes") and r2.get("install") == "installed" and p.get("version") == 2 and p.get("version_name") == "2.0 final" and p.get("license") == "GPL-3.0"
              and p.get("description") == man["description"] and await until(lambda: evs()[n_ev:] == ["updated"]),
              f"one with another version label and licence name: no button, no line, \"updated\": {r.get('changes')}, {evs()[n_ev:]}")

        # a stored value does not outlive its row: the secret, its row gone (no button), a text row of its name after
        check(await until(lambda: (plug(b) or {}).get("state") == "running", 15), "(the plugin runs)")
        st, _, body = b.call("POST", "/api/plugins/testwake/secret/pin", lambda c: b.seal(c, b"s3cret".ljust(16, b"\0")).hex().encode())
        st2, _ = post(b, "/api/plugins/testwake/set", "note=hello")
        check(st == 200 and st2 == 200 and "pin:5=s3cret\n" in stored() and "note:3=hello\n" in stored(), f"a secret and a text set: stored with their rows' types ({stored()!r})")
        rows = [x for x in man["settings"] if x["name"] != "pin"]
        st, r = upload(b, package(version=3, settings=rows)); st2, r2 = post(b, "/api/plugins/install")
        check(st == 200 and r.get("button") is False and r2.get("install") == "installed" and "s3cret" not in stored() and "pin" not in stored() and "note:3=hello\n" in stored(),
              f"version 3 without the secret's row, installed without the button: the secret is gone from the Echo with it ({stored()!r})")
        nick = rows + [{"name": "pin", "label": "Nickname", "type": "text"}]
        st, r = upload(b, package(version=4, settings=nick))
        check(st == 200 and r.get("button") is True and any('Asks you for "Nickname" (text)' in c["line"] for c in r.get("changes", [])), f"version 4 with a text row of the secret's name: asked for: {r.get('changes')}")
        # the check counts what has no place among its 24 lines (finding 3 of a review of 61e3d8e: "And more." without
        # a count, the page's words by hmbrowse.changes): as many new rows as the 32 a manifest has room for, against
        # version 3's (the Nickname row among them)
        many = nick + [{"name": f"r{i}", "label": f"Row {i}", "type": "bool"} for i in range(32 - len(nick))]
        st, r = upload(b, package(version=5, settings=many)); post(b, "/api/plugins/cancel")
        check(st == 200 and len(many) - len(rows) > 24 and len(r.get("changes", [])) == 24 and type(r.get("changes_more")) is int and r.get("changes_more") == len(many) - len(rows) - 24
              and type(r.get("shown_more")) is int and r.get("shown_more") == 0,
              f"{len(many) - len(rows)} rows more, for 24 lines: the rest counted (changes_more {r.get('changes_more')!r}, shown_more {r.get('shown_more')!r})")
        st, r = upload(b, package(version=4, settings=nick))
        st, r = post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: (plug(b) or {}).get("version") == 4), "(accepted with the button)")
        row = {x["name"]: x for x in plug(b)["settings"]}
        check(row["pin"]["type"] == "text" and row["pin"]["value"] == "" and row["note"]["value"] == "hello", f"the page shows the new row empty, and the note as it was: {row['pin']}")
        st, r = post(b, "/api/plugins/testwake/set", "note=")
        check(st == 200 and "note:3=\n" in stored(), "(a text row keeps an empty text)")

        # a version that requires a later version of a scope than this build has (sdk/PROTOCOL.md "Versions"): refused,
        # in the words that say a newer hassmic runs it; the one that runs stays as it is
        check(await until(lambda: (plug(b) or {}).get("state") == "running", 15), "(the plugin runs)")
        pids = fp.plugin_pids(state); n_ev = len(evs())
        st, r = upload(b, package(version=5, settings=nick, requires={"core": 1, "wake": 2, "files": 1}))
        check(st == 400 and "needs a newer hassmic: wake 2" in r.get("error", "") and (plug(b) or {}).get("version") == 4 and fp.plugin_pids(state) == pids and evs()[n_ev:] == [],
              f"version 5 requiring wake 2: refused, \"needs a newer hassmic: wake 2\"; version 4 runs on: {r}")
        st, r = upload(b, package(id="otherapi", version=1, requires={"core": 1, "wake": 1, "files": 1, "telepathy": 1}))
        check(st == 400 and "needs a newer hassmic: telepathy 1" in r.get("error", "") and not os.path.exists(os.path.join(state, "plugins", "otherapi")), f"one requiring a scope no hassmic of this build knows: refused: {r}")
        # what a newer hassmic left behind (a fall back to this one): a card that cannot run and says why, removable
        left = os.path.join(state, "plugins", "leftover", "pkg"); os.makedirs(left)
        json.dump(dict(man, id="leftover", requires={"core": 1, "wake": 1, "files": 1, "telepathy": 3}, author_key="00" * 32, digest="00" * 32), open(os.path.join(left, "manifest.json"), "w"))
        post(b, "/api/plugins/testwake/enable", "on")           # (anything that has the store read its folder again)
        p = plug(b, "leftover") or {}
        check(p.get("here") is False and p.get("newer") is True and "needs a newer hassmic: telepathy 3" in p.get("detail", "") and p.get("state") == "stopped" and not p.get("caps") and p.get("name") == man["name"],
              f"a plugin a newer hassmic installed: a card that says it needs one, with its name: {p.get('detail')}")
        st, r = post(b, "/api/plugins/leftover/remove", "remove")
        check(st == 200 and plug(b, "leftover") is None and not os.path.exists(os.path.join(state, "plugins", "leftover")) and await until(lambda: evs()[-1:] == ["removed"]), "and it can be removed")
        # "installed", not "updated", over a card that was no plugin the owner had running (a manifest this build cannot read)
        mpath = os.path.join(state, "plugins", "testwake", "pkg", "manifest.json")
        m = json.loads(open(mpath).read()); m["memory_mb"] = "16"; open(mpath, "w").write(json.dumps(m))
        post(b, "/api/plugins/testwake/enable", "off"); post(b, "/api/plugins/testwake/enable", "on")
        p = plug(b) or {}
        check(p.get("here") is False and "cannot read its manifest" in p.get("detail", ""), f"(its manifest made one this build does not read: {p.get('detail')})")
        n_ev = len(evs())
        st, r = upload(b, package(version=6, settings=nick)); st2, r2 = post(b, "/api/plugins/install")
        if r2.get("install") == "waiting": proc.send_signal(signal.SIGUSR2)
        check(st == 200 and await until(lambda: evs()[n_ev:] == ["installed"]) and (plug(b) or {}).get("version") == 6, f"a version installed over it: Home Assistant hears \"installed\" ({evs()[n_ev:]})")
        check(await until(lambda: (plug(b) or {}).get("state") == "running", 15), "(and it runs)")

        # ---- switched off: the entity goes
        closed = len(open(log).read())
        r = b.set(plugins="off")
        check(r.get("applied") == 1 and logged(log, "entities changed: links closed", closed, 5), "plugins switched off: the links closed again")
        try: await ha.disconnect(force=True)
        except Exception: pass
        ha, ents, states = await home_assistant()
        check("plugin" not in ents and "button" in ents, "and Home Assistant no longer has the \"Plugin\" entity")
    finally:
        if ha:
            try: await ha.disconnect(force=True)
            except Exception: pass
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()
        if check.failed: print(open(log, errors="replace").read()[-6000:])
    print("FAIL" if check.failed else "PASS")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__": asyncio.run(main())
