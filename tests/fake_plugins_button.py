#!/usr/bin/env python3
"""On the PC: `button` and rows of a plugin's own on its card, with a stand-in (tests/plugins/testbutton.c)
under build/plugrun-host and build/hassmic-host, this script as the settings page and as the person at the Echo
(SIGUSR2: the action button pressed, SIGURG: held a second, as buttons.c tells main.c).

The install: `button`'s line in the owner's words.  The presses: a stand-in Home Assistant (aioesphomeapi, as
tests/fake_plugins_ha.py) is connected and subscribed to states (connecting again whenever hassmic closed its link,
as Home Assistant does), and each press and hold reaches the plugin exactly as Home Assistant's "Button" event entity
receives it over that link (the events it got and the plugin's BUTTONs are the same list), and a press that served a
wait for the button reaches neither: a login of the settings page, a plugin's install (Home Assistant's side of that
on updates, whose install leaves its link up).  The rows: the entities marked "page" on the card in /api/plugins, with the values the plugin said, changing as it says new ones; the one that is not
marked stays an entity, with its value.  Updates: "page" added or taken away needs no button and moves an entity in
or out of the rows; `button` dropped needs none, added back it asks.  A build without the scope (an older one, as
HASSMIC_PLUGIN_SCOPES=button:0 makes it): the installed plugin is not started and its card says it needs a newer
hassmic, and a package asking for it is refused in the same words; an entity's "page" is ha.entities 1's, nothing
to require for it."""
import asyncio, json, os, shutil, signal, subprocess, sys, tempfile, time
from aioesphomeapi import APIClient, Event, EventInfo
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser
import fake_plugins as fp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk"))
import plugcheck
check = fp.check
# the plugin's BUTTON as Home Assistant's "Button" names it (sdk/plugmsg.h: key 1 action, 2 volume up, 3 volume down;
# how 1 pressed, 2 held; main.c core_button_events)
AS_HA = {f"button {k} {h}": n + ("_hold" if h == 2 else "") for k, n in ((1, "action"), (2, "volume_up"), (3, "volume_down")) for h in (1, 2)}


async def until(f, secs=8):
    """as fake_plugins.until, with the loop running: Home Assistant's side is read meanwhile"""
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if f(): return True
        await asyncio.sleep(0.05)
    return bool(f())


async def home_assistant():
    """connected as Home Assistant, subscribed to states: the client, the "Button" event entity, every state from now"""
    ha = None
    for _ in range(40):                 # (just started: the port may not be there yet)
        try:
            ha = APIClient("127.0.0.1", fp.PORT, None); await ha.connect(login=True); break
        except Exception:
            ha = None; await asyncio.sleep(0.25)
    es, _ = await ha.list_entities_services(); states = []; ha.subscribe_states(states.append)
    return ha, next((e for e in es if e.object_id == "button"), None), states


async def main():
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log")
    for k in ("author", "index"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    man = json.load(open(f"{ROOT}/tests/plugins/testbutton.json"))

    def package(**changes):
        """official: the index's listing (the test's index key)"""
        m = dict(man, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, "p.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/testbutton", os.path.join(top, "author.key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        plugcheck.list_package(out, os.path.join(top, "index.key"), os.path.join(top, "o.hmp"), arch="x86_64")
        return open(os.path.join(top, "o.hmp"), "rb").read()

    def entities(*marks):
        """the manifest's entities, "page" as MARKS says for presses, last, held (None: the key left out)"""
        es = [dict(e) for e in man["capabilities"]["ha.entities"]]
        for e, mark in zip(es, marks):
            e.pop("page", None)
            if mark is not None: e["page"] = mark
        return es

    text = lambda: open(log, errors="replace").read()
    heard = lambda n0=0: [l.split("plugin testbutton: ", 1)[1] for l in text()[n0:].splitlines() if "plugin testbutton: button " in l]
    rows = lambda b: {r["name"]: r.get("value") for r in (fp.plug(b, "testbutton") or {}).get("rows", [])}
    ha = {}                             # Home Assistant: its client, the "Button" entity, the states it was sent

    async def connect():
        if ha.get("c"):
            try: await ha["c"].disconnect(force=True)
            except Exception: pass
        ha["c"], ha["button"], ha["states"] = await home_assistant()
        await asyncio.sleep(0.2); ha["at"] = len(text())
        bev = ha["button"]
        if not (isinstance(bev, EventInfo) and bev.name == "Button"): check(False, f"(Home Assistant connected, the \"Button\" event entity listed: {bev})")

    async def linked():
        """Home Assistant's link up: hassmic closes every link when the entities it lists change (plugins switched on,
        a plugin's entities listed or changed, proto->entities_changed), and Home Assistant connects again; so does
        this stand-in, before each press, as Home Assistant would have by then"""
        if "client disconnected" in text()[ha["at"]:]: await connect()

    # what Home Assistant's "Button" received, over its link (not hassmic's own "button: ..." line, which is written
    # before hassmic looks whether Home Assistant is there at all)
    told = lambda: [x.event_type for x in ha["states"] if isinstance(x, Event) and x.key == ha["button"].key]

    async def press(proc, sig=signal.SIGUSR2, want=None):
        """the button, then the plugin's line for it (WANT) and Home Assistant's event, or 1.5 s of nothing; what each
        heard.  Every press also checks that the two are the same list: the plugin hears what Home Assistant receives"""
        await linked()
        n0 = len(text()); e0 = len(told()); proc.send_signal(sig)
        if want: await until(lambda: want in heard(n0) and len(told()) > e0, 5)
        await asyncio.sleep(1.5 if not want else 0.3)    # (and nothing more comes: a second event would be wrong)
        got, evs = heard(n0), told()[e0:]
        # (Home Assistant's link up all the while: an event it did not receive because hassmic had just closed its
        # link would pass for a press that served a wait)
        up = "client disconnected" not in text()[max(n0, ha["at"]):]
        check(up and ([AS_HA.get(g, g) for g in got] == evs or ("button" not in caps_now and got == [])),
              f"(the plugin heard what Home Assistant's \"Button\" received, its link up throughout: plugin {got}, Home Assistant {evs}, link up {up})")
        return got, evs
    caps_now = {"button"}

    # (community plugins switched on as the page's switch leaves them: not needed for the official stand-in, set all the same
    # so that nothing here waits on that switch)
    open(os.path.join(state, "config"), "a").write("plugins_community=on\n")
    proc = fp.start(state, log)
    try:
        wait_port(fp.PORT, fp.WEB)
        await connect()
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "logged in with the button")
        await asyncio.sleep(1.0)
        check(told() == [], f"the press that logged the browser in: Home Assistant received no \"Button\" event: {told()}")
        b.set(plugins="on")

        # ---- the install: its line; the press that installs it is the install's, nobody else's
        st, r = fp.upload(b, package())
        lines = [c["line"] for c in r.get("caps", [])]
        check(st == 200 and r.get("button") is True and "Hears presses of this Echo's buttons, as Home Assistant's \"Button\" does" in lines,
              f"a plugin with button: its line at install, the button asked for: {st} {lines} {r.get('error')}")
        await linked(); n0 = len(text()); e0 = len(told()); fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: (fp.plug(b, "testbutton") or {}).get("accepted")) and await until(lambda: (fp.plug(b, "testbutton") or {}).get("state") == "running", 20),
              "the press installs it, and it runs")
        await asyncio.sleep(0.5)
        # (Home Assistant's side of this press says nothing: the install, inside web_approve, lists the plugin's
        # entities, which closes Home Assistant's links before on_action could tell it anything.  Proven by a build
        # that tells Home Assistant of every press web_approve served: this line passed, the login's above and the
        # updates' below, whose links stay up, failed.  So a press that served an install is held to Home Assistant
        # there: versions 2 and 5)
        check(told()[e0:] == [] and heard(n0) == [], f"the press that installed it: nothing heard, no \"Button\" event: {told()[e0:]} {heard(n0)}")

        # ---- presses and holds, as Home Assistant's "Button" hears them
        got, evs = await press(proc, want="button 1 1")
        check(got == ["button 1 1"] and evs == ["action"], f"the action button pressed: the plugin hears it (key 1, pressed), as Home Assistant's \"Button\" receives it: {got} {evs}")
        got, evs = await press(proc, signal.SIGURG, want="button 1 2")
        check(got == ["button 1 2"] and evs == ["action_hold"], f"held a second: key 1, held, as Home Assistant receives action_hold: {got} {evs}")

        # ---- the rows: the three marked "page", the plugin's values, as they change
        check(fp.until(lambda: rows(b) == {"presses": "2", "last": "action held", "held": "on"}) and fp.until(lambda: "value" in next(e for e in fp.plug(b, "testbutton")["entities"] if e["name"] == "quiet")),
              f"its rows on the card: the three marked page, with what it said: {rows(b)}")
        p = fp.plug(b, "testbutton")
        r0 = p.get("rows", [])
        check([(x["name"], x["label"], x["kind"], x.get("unit")) for x in r0] == [("presses", "Presses heard", "sensor", "presses"), ("last", "Last press", "text_sensor", ""), ("held", "Held last", "binary_sensor", "")],
              f"each row by its label, kind and unit, in the manifest's order: {r0}")
        ents = {e["name"]: e for e in p.get("entities", [])}
        check(ents["quiet"].get("value") == "7.5" and not ents["quiet"].get("page") and ents["presses"].get("page") and "value" not in ents["presses"],
              f"the entity not marked: no row, its value among the entities; a row's value not twice: {ents}")
        await press(proc, want="button 1 1")
        check(fp.until(lambda: rows(b) == {"presses": "3", "last": "action pressed", "held": "off"}), f"the rows as the values change (the card's poll): {rows(b)}")

        # ---- a press that served a wait for the button: a login of another browser
        b2 = Browser(fp.WEB); n0 = len(text())
        # (asked again while "busy": web.c keeps other browsers back for a moment after every press, 0.3 s here)
        check(fp.until(lambda: b2.ask().get("login") == "waiting", 5), "another browser asks to log in: the Echo waits for the button")
        got, evs = await press(proc)
        check(b2.ask().get("login") == "approved" and got == [] and evs == [], f"the press approves it, and neither the plugin nor Home Assistant hears of it: {got} {evs}")
        got, evs = await press(proc, want="button 1 1")
        check(got == ["button 1 1"] and evs == ["action"], f"the next press is a press again: {got} {evs}")

        # ---- ... and a plugin's install: an update that asks for the button (a capability more)
        st, r = fp.upload(b, package(version=2, capabilities=dict(man["capabilities"], state=True)))
        check(st == 200 and r.get("button") is True, f"(version 2 with state: asks for the button: {r.get('error')})")
        fp.post(b, "/api/plugins/install")
        got, evs = await press(proc)
        check(fp.until(lambda: (fp.plug(b, "testbutton") or {}).get("version") == 2) and got == [] and evs == [],
              f"the press that accepts an install: neither the plugin nor Home Assistant hears of it: {got} {evs}")
        check(fp.until(lambda: (fp.plug(b, "testbutton") or {}).get("state") == "running", 20), "(version 2 runs)")

        # ---- updates: "page" is no power, button is one
        st, r = fp.upload(b, package(version=3, capabilities=dict(man["capabilities"], state=True, **{"ha.entities": entities(True, False, None, True)})))
        check(st == 200 and r.get("button") is False, f"\"page\" taken off two entities and put on another: no button ({r.get('changes')})")
        fp.post(b, "/api/plugins/install")
        check(fp.until(lambda: (fp.plug(b, "testbutton") or {}).get("version") == 3) and fp.until(lambda: (fp.plug(b, "testbutton") or {}).get("state") == "running", 20)
              and fp.until(lambda: set(rows(b)) == {"presses", "quiet"}), f"installed without a press; the rows as the manifest marks them now: {rows(b)}")
        st, r = fp.upload(b, package(version=4, capabilities={"ha.entities": man["capabilities"]["ha.entities"]}))
        check(st == 200 and r.get("button") is False, f"button and state dropped: less, no button ({r.get('changes')})")
        fp.post(b, "/api/plugins/install")
        check(fp.until(lambda: (fp.plug(b, "testbutton") or {}).get("version") == 4) and fp.until(lambda: (fp.plug(b, "testbutton") or {}).get("state") == "running", 20), "(version 4 runs)")
        caps_now.discard("button")
        got, evs = await press(proc)
        check(got == [] and evs == ["action"], f"without button: Home Assistant receives the press, the plugin does not: {got} {evs}")
        caps_now.add("button")
        st, r = fp.upload(b, package(version=5))
        check(st == 200 and r.get("button") is True and any("Hears presses" in c.get("line", "") for c in r.get("changes", [])),
              f"button added back: wider, the button asked for, the line named: {r.get('changes')}")
        fp.post(b, "/api/plugins/install"); await linked(); e0 = len(told()); await press(proc)
        check(told()[e0:] == [], f"(the press that accepted version 5: no \"Button\" event: {told()[e0:]})")
        check(fp.until(lambda: (fp.plug(b, "testbutton") or {}).get("version") == 5) and fp.until(lambda: (fp.plug(b, "testbutton") or {}).get("state") == "running", 20), "(version 5 installed with the press)")

        # ---- an entity's "page" with ha.entities 1: there is no later version to require for it
        st, r = fp.upload(b, package(id="oldpage", version=1, capabilities={"ha.entities": entities(True)[:1]}))
        check(st == 200 and r.get("requires", {}).get("ha.entities") == 1, f"\"page\" on an entity, ha.entities 1: taken: {r.get('error')} {r.get('requires')}")
        fp.post(b, "/api/plugins/cancel")

        # ---- a build without button (HASSMIC_PLUGIN_SCOPES=button:0, as an older hassmic): not started, refused at install
        proc.terminate(); proc.wait(10)
        proc = fp.start(state, log, env={"HASSMIC_PLUGIN_SCOPES": "button:0"})
        wait_port(fp.PORT, fp.WEB)
        await connect()
        p = fp.until(lambda: (lambda q: q if q and "needs a newer hassmic: button 1" in (q.get("detail") or "") else None)(fp.plug(b, "testbutton")), 10) or fp.plug(b, "testbutton") or {}
        check("needs a newer hassmic: button 1" in (p.get("detail") or "") and p.get("newer") is True and p.get("state") != "running" and p.get("rows") == [],
              f"a build without button: the plugin is not started, its card says it needs a newer hassmic, no rows: {p.get('state')} {p.get('detail')}")
        st, r = fp.upload(b, package(id="otherbutton", version=1))
        check(st == 400 and "needs a newer hassmic: button 1" in r.get("error", ""), f"a package asking for it: refused at install, in the same words: {r.get('error')}")
        fp.post(b, "/api/plugins/cancel")
        caps_now.discard("button")
        got, evs = await press(proc)
        check(got == [] and evs == ["action"], f"a press: Home Assistant receives it, no plugin hears it: {got} {evs}")
    finally:
        if ha.get("c"):
            try: await ha["c"].disconnect(force=True)
            except Exception: pass
        proc.terminate()
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
    if check.failed:
        print("---- log"); print(open(log, errors="replace").read()[-8000:])
    shutil.rmtree(top, ignore_errors=True)              # (its own folder, by its name: nothing else in /tmp)
    print("plugins-button: " + ("FAILED" if check.failed else "all passed"))
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    asyncio.run(main())
