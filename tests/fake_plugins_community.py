#!/usr/bin/env python3
"""Community plugins (docs/plugins.md 2.7, 3.3) on the PC: build/hassmic-host under build/plugrun-host with the stand-in
wake word plugin (tests/plugins/testwake.c), the settings page as a stand-in browser (tests/webclient.py).

The switch (docs/plugins.md 12, the eighth round): a setting of the page as any other.  Off by default; on and off
from an approved browser (POST /api/set) without a press of the action button (nothing waits for it, the ring shows
nothing), refused from a browser that is not let in; exported, and taken from an import and a preset.  Off: a package
the plugin index does not list as official (a file, a community listing) refused at the check and at the install
(switched off between the two), such a plugin installed already stopped and kept, its card saying why, an official one
untouched; such a wake word engine down with words that say which switch, and not chosen as the engine; on again, it
runs without being installed again; one request (an export's lines) that switches them on and chooses such an engine
takes both, one that switches them off refuses the engine, in either order; the plugins switch exported as well (the
owner's decision, 2026-11-24), an export listing plugins, community plugins and the engine in that order, and one
request with all three taken on an Echo with plugins off, whatever order its lines come in.  Over restarts: the switch as it was left;
no line for it in state/config: off, whatever is installed (no first start's rule); a preset that says otherwise than
state/config settles the start once (never turned and back); on, with a community wake word engine: started in main(),
never refused at the start.  The sandbox's state line and what the page reads for the warning (in /api/plugins and in
a package's check, for the install dialog): the launcher's SELinux domain (HASSMIC_PLUGIN_DOMAIN on the PC: plugin,
hassmic, none) as "complete", "hassmic", "su", and the model's install method (device.conf INSTALL, HASSMIC_INSTALL
here)."""
import asyncio, json, os, re, signal, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser, req
import fake_plugins as fp
from fake_plugins import check, plugins, plug, post, upload, until, ROOT
sys.path.insert(0, os.path.join(ROOT, "sdk"))
import plugcheck


def main():
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log"); leds = os.path.join(top, "ledctrl.log")
    for k in ("author", "index"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    man = json.load(open(f"{ROOT}/tests/plugins/testwake.json"))
    cfg = lambda: dict(l.strip().split("=", 1) for l in open(os.path.join(state, "config")) if "=" in l and not l.startswith("#"))

    def package(tier="official", **changes):
        """testwake as the two repositories make it: the author's package; for "official" or "community" with the
        index key's listing of that tier, "file" without one"""
        m = dict(man, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, "p.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/testwake", os.path.join(top, "author.key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        if tier != "file":
            plugcheck.list_package(out, os.path.join(top, "index.key"), os.path.join(top, "o.hmp"), tier=tier, arch="x86_64")
            out = os.path.join(top, "o.hmp")
        return open(out, "rb").read()

    def run(env=None):
        e = {"HASSMIC_FAKE_LED": leds, "HASSMIC_LOGIN_MS": "4000:300"}; e.update(env or {})
        p = fp.start(state, log, env=e); wait_port(fp.PORT, fp.WEB); return p

    def community(b, word):
        """the page's switch: POST /api/set, as web/plugins.js plgCommunitySet does; the answer whole"""
        return b.set(plugins_community=word)

    def holds(b): return ((b.state() or {}).get("button") or {}).get("holds")
    lines = lambda: open(leds).read().splitlines() if os.path.exists(leds) else []

    def install(b, proc, pkg, pid):
        st, r = upload(b, pkg)
        if st != 200: return st, r
        st, r = post(b, "/api/plugins/install")
        if r.get("install") == "waiting": proc.send_signal(signal.SIGUSR2)
        return (200, r) if until(lambda: (plug(b, pid) or {}).get("accepted")) else (409, r)

    proc = run()
    try:
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "logged in with the button")
        b2 = Browser(fp.WEB)
        check(b2.login_with_button(proc), "a second browser, approved too")
        b.set(plugins="on")
        p = plugins(b)
        check(p.get("community") is False and p.get("sandbox") == "su" and p.get("install_method") == "twrp-ab",
              f"community plugins off by default; the launcher in no SELinux domain (the PC) and the model's install method in the answer: {[p.get(k) for k in ('community', 'sandbox', 'install_method')]}")

        # ---- off: only official packages, whichever way they come
        st, r = upload(b, package("file"))
        check(st == 400 and "community plugins are switched off on this Echo" in r.get("error", "") and "a file" in r.get("error", ""), f"a package from a file: refused at the check ({r.get('error')})")
        st, r = upload(b, package("community"))
        check(st == 400 and "a community plugin (the plugin index lists it, not as official)" in r.get("error", ""), f"the index's community listing: refused too ({r.get('error')})")
        post(b, "/api/plugins/cancel")
        st, r = install(b, proc, package("official"), "testwake")
        check(st == 200 and (plug(b) or {}).get("tier") == "official" and until(lambda: (plug(b) or {}).get("state") == "running"),
              f"an official one: installed with its button, and it runs: {st} {r}")

        # ---- on and off: a setting, from an approved browser only, no press
        st, _, body = Browser(fp.WEB).call("POST", "/api/set", b"plugins_community=on\n")
        check(st == 401 and plugins(b).get("community") is False, f"from a browser that is not let in: refused, still off ({st} {body[:60]})")
        st, _, body = req(fp.WEB, "POST", "/api/set", b"plugins_community=on\n")
        check(st == 401 and plugins(b).get("community") is False, f"unsigned: refused, still off ({st})")
        st, r = post(b, "/api/plugins/community", "on")
        check(st != 200 and plugins(b).get("community") is False, f"the old wait for the button is gone: {st} {r}")
        open(leds, "w").close(); n0 = len(open(log).read())
        r = community(b, "on")
        check(r.get("applied") == 1 and not r.get("errors") and plugins(b).get("community") is True and cfg().get("plugins_community") == "on"
              and fp.logged(log, "community plugins switched on", n0),
              f"on from an approved browser at once, kept in state/config, said: {r}")
        check(holds(b) == "none" and "-s wait" not in lines(), f"no press asked for: nothing waits for the button, the ring shows nothing: {holds(b)} {lines()}")

        # ---- a community plugin, then off: stopped and kept
        st, r = install(b, proc, package("file", id="sideplug", name="Side plugin"), "sideplug")
        check(st == 200 and (plug(b, "sideplug") or {}).get("tier") == "sideload" and until(lambda: (plug(b, "sideplug") or {}).get("state") == "running"),
              f"with them on a plugin from a file installs (its own button) and runs: {st} {r}")
        n0 = len(open(log).read())
        check(community(b2, "off").get("applied") == 1, "off from any approved browser, no button")
        sp = until(lambda: (lambda q: q if q and q.get("state") == "stopped" else None)(plug(b, "sideplug")))
        check(plugins(b).get("community") is False and sp and sp.get("community_off") is True and "community plugins are switched off" in sp.get("detail", sp.get("why", "")) + json.dumps(sp),
              f"off: the community plugin is stopped, its card says why: {sp and {k: sp.get(k) for k in ('state', 'community_off')}}")
        check(os.path.isdir(os.path.join(state, "plugins", "sideplug")) and (plug(b) or {}).get("state") == "running", "and kept; the official one runs on")
        check(not [x for x in fp.plugin_pids(state) if "sideplug" in os.readlink(f"/proc/{x}/exe")], "its process is gone")
        check(cfg().get("plugins_community") == "off" and fp.logged(log, "community plugins switched off", n0), "said, and kept so in state/config")
        check(community(b2, "on").get("applied") == 1 and until(lambda: (plug(b, "sideplug") or {}).get("state") == "running", 15),
              "on again from the second browser: it runs again, nothing installed anew (the others than the wake engine's 10 s after the start)")
        community(b2, "off")
        check(until(lambda: (plug(b, "sideplug") or {}).get("state") == "stopped"), "and off again")

        # ---- switched off between the check and the press: nothing written
        st, r = install(b, proc, package("official", version=2), "testwake")       # (an update of the official one: no button, nothing to do with it)
        check(community(b, "on").get("applied") == 1 and plugins(b).get("community") is True, "on again")
        st, r = upload(b, package("file", id="lateplug", name="Late plugin"))
        check(st == 200 and r.get("tier") == "sideload" and r.get("sandbox") == "su" and r.get("install_method") == "twrp-ab",
              f"a file checked while they are on; the check names this Echo's sandbox and install method, for the dialog's warning: {r.get('error')} {r.get('sandbox')} {r.get('install_method')}")
        community(b, "off")
        st, r = post(b, "/api/plugins/install")
        if r.get("install") == "waiting": proc.send_signal(signal.SIGUSR2)
        time.sleep(1.5)
        check(not os.path.exists(os.path.join(state, "plugins", "lateplug")) and not plug(b, "lateplug"),
              f"switched off before its press: the install writes nothing ({r}, {plugins(b).get('install')})")
        post(b, "/api/plugins/cancel")

        # ---- exported, and taken from an import ("make like this Echo" and Compare & copy post the same)
        community(b, "on")
        st, _, body = b.call("GET", "/api/export")
        check(st == 200 and re.search(rb"(?m)^plugins_community=on$", body), "in the export, as it is")
        st, _, body = b.call("POST", "/api/set", b"# hassmic settings, exported from Another Echo (Echo Dot 3 x)\nplugins_community=off\nmic_level=-20\n")
        r = json.loads(body)
        check(r.get("applied") == 2 and not r.get("errors") and plugins(b).get("community") is False and until(lambda: (plug(b, "sideplug") or {}).get("state") == "stopped"),
              f"an import saying off switches it off: {r}")
        st, _, body = b.call("POST", "/api/set", b"# hassmic settings, exported from Another Echo (Echo Dot 3 x)\nplugins_community=on\nmic_level=-21\n")
        r = json.loads(body)
        check(r.get("applied") == 2 and plugins(b).get("community") is True and until(lambda: (plug(b, "sideplug") or {}).get("state") == "running"),
              f"one saying on switches it on, and the plugin installed here runs: {r}")
        st, _, body = b.call("GET", "/api/export")
        check(re.search(rb"(?m)^plugins_community=on$", body) and re.search(rb"(?m)^plugins=on$", body),
              "the plugins switch itself is exported too (the owner's decision, 2026-11-24: each install still takes the Echo's button)")

        # ---- the wake word engine a community plugin
        st, r = install(b, proc, package("file", id="sidewake", name="Side wake word"), "sidewake")
        r = b.set(wake_engine="plugin:sidewake")
        check(r.get("applied") == 1, f"a community wake word plugin chosen as the engine while they are on: {r}")
        community(b, "off")
        check(until(lambda: plugins(b).get("wake_problem") == 2 and "community plugins are switched off" in plugins(b).get("wake_why", "")),
              f"off: the engine is down, and the words say which switch: {plugins(b).get('wake_why')}")
        w = plugins(b).get("wake_why", "")
        check(w.startswith("The wake word engine is the plugin sidewake, which is from a file") and "switch community plugins on there, or choose another wake word engine" in w
              and "action button" not in w.split("The action button works")[0], f"(main.c's words for this case, and no press asked for in them: {w})")
        r = b.set(wake_engine="amazon")
        r = b.set(wake_engine="plugin:sidewake")
        check(r.get("applied") == 0 and "community plugins are switched off" in r.get("errors", ""), f"and while they are off it is not chosen again: {r}")

        # ---- one request that switches them on and chooses such an engine ("make like this Echo", an import,
        # Compare & copy, a preset: an export's lines, the engine's before the switch's): both taken, whatever the
        # order; and one that switches them off with such an engine: the engine refused, whatever the order (a review
        # of 00d492a, 1: the engine was judged by the scan's mark from before the switch, and refused)
        check(until(lambda: (plug(b, "sidewake") or {}).get("community_off") is True), "(the plugin marked by the scan while they are off)")
        st, _, exp = b.call("GET", "/api/export")
        exp = exp.decode(); keys = [l.split("=", 1)[0] for l in exp.splitlines()]
        check("wake_engine" in keys and "plugins_community" in keys and "plugins" in keys and keys.index("plugins") < keys.index("plugins_community") < keys.index("wake_engine"),
              f"an export lists plugins, then community plugins, then the engine, the order they are applied in: {[k for k in keys if k in ('plugins', 'plugins_community', 'wake_engine')]}")
        # (the order on the wire is no help: the lines below are put back in the table's, the engine first, as an export
        # of before and a file edited by hand may have them)
        exp = "\n".join([l for l in exp.splitlines() if l.startswith("wake_engine=")] + [l for l in exp.splitlines() if not l.startswith("wake_engine=")]) + "\n"
        on = re.sub(r"(?m)^wake_engine=.*$", "wake_engine=plugin:sidewake", re.sub(r"(?m)^plugins_community=.*$", "plugins_community=on", exp))
        st, _, body = b.call("POST", "/api/set", on.encode()); r = json.loads(body)
        check(st == 200 and not r.get("errors") and cfg().get("wake_engine") == "plugin:sidewake" and cfg().get("plugins_community") == "on" and plugins(b).get("community") is True,
              f"an export that says on and the community plugin as the engine: both taken in one request: {r.get('errors')} {cfg().get('wake_engine')} {cfg().get('plugins_community')}")
        check(until(lambda: (plug(b, "sidewake") or {}).get("state") == "running", 15) and "community plugins are switched off" not in plugins(b).get("wake_why", ""),
              f"and the engine's plugin runs: {(plug(b, 'sidewake') or {}).get('state')} {plugins(b).get('wake_why')}")
        for body in (b"wake_engine=plugin:sidewake\nplugins_community=off\n", b"plugins_community=off\nwake_engine=plugin:sidewake\n"):
            b.set(wake_engine="amazon"); community(b, "on")
            st, _, rb = b.call("POST", "/api/set", body); r = json.loads(rb)
            check(r.get("applied") == 1 and "community plugins are switched off" in r.get("errors", "") and cfg().get("wake_engine") == "amazon" and cfg().get("plugins_community") == "off",
                  f"off and the community plugin as the engine in one request ({body.splitlines()[0].decode()} first): off taken, the engine refused: {r}")
        # ---- the same onto an Echo with plugins off: plugins, community plugins and the engine in one request, the
        # engine's line first and plugins' last: all three taken, and the engine's plugin runs
        b.set(wake_engine="amazon"); community(b, "off")
        check(b.set(plugins="off").get("applied") == 1 and until(lambda: not fp.plugin_pids(state)), "plugins off: none runs")
        three = re.sub(r"(?m)^plugins=.*\n", "", re.sub(r"(?m)^wake_engine=.*\n", "", on)); three = "wake_engine=plugin:sidewake\n" + three + "plugins=on\n"
        check(re.search(r"(?m)^plugins_community=on$", three) and not re.search(r"(?m)^plugins=off$", three), "(the request says on for both switches)")
        st, _, body = b.call("POST", "/api/set", three.encode()); r = json.loads(body)
        check(st == 200 and not r.get("errors") and cfg().get("plugins") == "on" and cfg().get("plugins_community") == "on" and cfg().get("wake_engine") == "plugin:sidewake"
              and plugins(b).get("community") is True,
              f"an export saying plugins on, community plugins on and the community plugin as the engine, applied where plugins are off: all three taken in one request: {r.get('errors')} {cfg().get('plugins')} {cfg().get('plugins_community')} {cfg().get('wake_engine')}")
        check(until(lambda: (plug(b, "sidewake") or {}).get("state") == "running", 15) and not re.search("switched off", plugins(b).get("wake_why", "")),
              f"and the engine's plugin runs: {(plug(b, 'sidewake') or {}).get('state')} {plugins(b).get('wake_why')}")
        b.set(wake_engine="amazon"); community(b, "off")
        st, r = post(b, "/api/plugins/sidewake/remove", "remove")
        check(st == 200 and not plug(b, "sidewake"), f"removed while off, without the button (two plugins run at once on the PC's build: room for the rest): {st} {r}")
    finally:
        proc.terminate(); proc.wait()

    # ---- over a restart: as it was left (off), the community plugins kept and not started; on, they start
    proc = run()
    try:
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "after a restart: logged in")
        p = plugins(b)
        check(p.get("community") is False and (plug(b, "sideplug") or {}).get("community_off") is True and (plug(b, "sideplug") or {}).get("state") == "stopped"
              and until(lambda: (plug(b) or {}).get("state") == "running", 15), "off over the restart: the community plugin kept and not started, the official one runs")
        check(community(b, "on").get("applied") == 1 and until(lambda: (plug(b, "sideplug") or {}).get("state") == "running"), "on: it runs again, nothing installed anew")
    finally:
        proc.terminate(); proc.wait()
    proc = run()
    try:
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "after another restart: logged in")
        check(plugins(b).get("community") is True and until(lambda: (plug(b, "sideplug") or {}).get("state") == "running", 15), "on over the restart: it starts with hassmic (with the others, 10 s after the start: no self test here)")
    finally:
        proc.terminate(); proc.wait()

    # ---- no line for it in state/config: off, whatever is installed (no first start's rule: nothing was released
    # with plugins), and nothing written for it
    def drop_line():
        c = os.path.join(state, "config"); t = [l for l in open(c).read().splitlines(True) if not l.startswith("plugins_community=")]; open(c, "w").write("".join(t))
    drop_line()
    proc = run()
    try:
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "no line for the switch: logged in")
        sp = until(lambda: (lambda q: q if q and q.get("community_off") else None)(plug(b, "sideplug")), 5)
        check(plugins(b).get("community") is False and sp and sp.get("state") == "stopped" and not [x for x in fp.plugin_pids(state) if "sideplug" in os.readlink(f"/proc/{x}/exe")],
              f"off, with a plugin from a file installed and switched on: kept and never started: {sp and {k: sp.get(k) for k in ('state', 'community_off')}}")
        check(cfg().get("plugins_community") in (None, "off"), f"and state/config does not say on: {cfg().get('plugins_community')}")
    finally:
        proc.terminate(); proc.wait()

    # ---- a preset (scripts/setup.sh --preset) as an import: on for this run and kept, the plugin from a file started;
    # then one saying off
    n0 = len(open(log).read())
    open(os.path.join(state, "preset"), "w").write("plugins_community=on\nmic_level=-20\n")
    proc = run()
    try:
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "a preset saying on: logged in")
        check(plugins(b).get("community") is True and os.path.exists(os.path.join(state, "preset.applied")) and cfg().get("plugins_community") == "on"
              and cfg().get("mic_level") == "-20" and until(lambda: (plug(b, "sideplug") or {}).get("state") == "running", 15),
              f"on, the preset taken whole and kept, the plugin from a file runs: {plugins(b).get('community')} {cfg().get('plugins_community')}")
    finally:
        proc.terminate(); proc.wait()
    open(os.path.join(state, "preset"), "w").write("plugins_community=off\n")
    n0 = len(open(log).read())
    proc = run()
    try:
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "a preset saying off: logged in")
        # (state/config says on: the start settles on the preset's off once, before the first scan, and is never
        # turned to the config's on and back in between, where the broker could start the plugin; a review of
        # 00d492a, 4)
        t = open(log).read()[n0:]
        check("community plugins switched" not in t and "plugins: sideplug: running" not in t,
              f"the preset's off over state/config's on: the switch never turned on and back at the start, the plugin never started: {[l for l in t.splitlines() if 'community plugins switched' in l or 'sideplug: running' in l]}")
        sp = until(lambda: (lambda q: q if q and q.get("community_off") else None)(plug(b, "sideplug")), 5)
        check(plugins(b).get("community") is False and cfg().get("plugins_community") == "off" and sp and sp.get("state") == "stopped",
              f"off, kept so, the plugin from a file stopped: {sp and {k: sp.get(k) for k in ('state', 'community_off')}}")
        st, r = upload(b, package("file", id="presetplug", name="Preset plugin"))
        check(st == 400 and "community plugins are switched off on this Echo" in r.get("error", ""), f"a package from a file refused: {st} {r.get('error')}")
        post(b, "/api/plugins/cancel")
    finally:
        proc.terminate(); proc.wait()
    # (a preset settings_preset cannot apply, over 64 kB, that says on, beside state/config's off: main.c's look ahead
    # read it, settings_preset ignores it; the start ends with state/config's off, told to the store at its end)
    open(os.path.join(state, "preset"), "w").write("#" + "x" * 70000 + "\nplugins_community=on\n")
    n0 = len(open(log).read())
    proc = run()
    try:
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "a preset too long to apply: logged in")
        time.sleep(11)                          # (past the others' start, 10 s after hassmic's here)
        t = open(log).read()[n0:]; sp = plug(b, "sideplug") or {}
        check(plugins(b).get("community") is False and cfg().get("plugins_community") == "off" and sp.get("community_off") is True and "plugins: sideplug: running" not in t
              and "preset" in t and "unreadable" in t,
              f"state/config's off stands, in the store too, and the plugin is never started: {plugins(b).get('community')} {sp.get('state')} {[l for l in t.splitlines() if 'community' in l or 'preset' in l]}")
    finally:
        proc.terminate(); proc.wait()
    os.remove(os.path.join(state, "preset"))

    # ---- on, and a community plugin the wake word engine, over a restart: the store's switch is set before the
    # first scan (main.c, from settings_peek), so the engine's plugin starts in main() and is never refused at the
    # start (a review of 00d492a, 3)
    proc = run()
    try:
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "community engine: logged in")
        community(b, "on")
        st, r = install(b, proc, package("file", id="sidewake", name="Side wake word"), "sidewake")
        r = b.set(wake_engine="plugin:sidewake")
        check(st == 200 and r.get("applied") == 1 and until(lambda: (plug(b, "sidewake") or {}).get("state") == "running", 15),
              f"a community plugin from a file is the engine, community plugins on: {st} {r}")
    finally:
        proc.terminate(); proc.wait()
    n0 = len(open(log).read())
    proc = run()
    try:
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "community engine, after a restart: logged in")
        check(until(lambda: (plug(b, "sidewake") or {}).get("state") == "running", 15), "the community engine's plugin runs after the restart")
        t = open(log).read()[n0:]
        check("sidewake is not started" not in t and "community plugins switched" not in t and "plugins: sidewake: running" in t,
              f"never refused at the start, nor the switch turned: {[l for l in t.splitlines() if 'sidewake' in l or 'community' in l][:8]}")
    finally:
        proc.terminate(); proc.wait()

    # ---- the sandbox's state line: what the launcher found, and the model's install method as given
    for dom, word in (("plugin", "complete"), ("hassmic", "hassmic"), ("none", "su")):
        proc = run({"HASSMIC_PLUGIN_DOMAIN": dom, "HASSMIC_INSTALL": "boot"})
        try:
            b = Browser(fp.WEB)
            ok = b.login_with_button(proc)
            p = plugins(b)
            check(ok and p.get("sandbox") == word and p.get("install_method") == "boot" and fp.logged(log, {"complete": "(sandbox complete)", "hassmic": "(sandbox without its SELinux wall)", "su": "(sandbox without SELinux)"}[word]),
                  f"the launcher's domain {dom}: \"{word}\" on the page and in the log, the install method as the model's: {p.get('sandbox')} {p.get('install_method')}")
            # (and in the check of a package, which the install dialog of one that is not official reads: the warning
            # in full there where the sandbox misses its wall, web/plugins.js plgWallFor)
            st, r = upload(b, package("file", id="dialogplug", name="Dialog plugin"))
            check(st == 200 and r.get("sandbox") == word and r.get("install_method") == "boot", f"  the same in a package's check: {st} {r.get('sandbox')} {r.get('install_method')} {r.get('error')}")
            post(b, "/api/plugins/cancel")
        finally:
            proc.terminate(); proc.wait()

    print("fake_plugins_community: " + ("FAILED" if check.failed else "all ok"))
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
