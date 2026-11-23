#!/usr/bin/env python3
"""The self test with a plugin as the wake word engine, and hassmic updates beside plugins (docs/plugins.md 2.6, 5.9;
main.c selftest_thread, plugin.c plugin_selftest_ok, ota.c, update.c): build/hassmic-host under build/plugrun-host
with the stand-in wake word plugin (tests/plugins/testwake.c) as its wake word engine; this script as the settings
page, as Home Assistant (the update entity's install), as GitHub (releases signed with a throwaway release key), as
the plugin index's site (tests/plugsite.py) and as root's installer.

A hassmic update never breaks a plugin (sdk/PROTOCOL.md "Versions": a scope only adds), so nothing of the plugins'
holds an update or is fetched for it: an online update and a push are handed to root as they come, the index asked
for nothing, the wake word plugin's mark of having listened kept for the next hassmic's self test.  Then the self test
at restarts with that mark: not waited for with plugins off or the plugin switched off, nor where this build needs
a newer hassmic's scope version for it (a fall back to an older build: HASSMIC_PLUGIN_SCOPES lowers this one), its
card saying so and Amazon's wake word standing in; waited for, and the update held, with the launcher silent or the
plugin's program not the one accepted.  With community plugins off: not waited for a file's plugin, waited for one
whose listing this build does not read, unless its mark says it ran as a file's ("listed 0"), which a community
listing's mark says too.  Held so, the self test goes on asking: the Echo's own look at the index takes the listing,
the plugin listens, and the self test passes late, the build the fallback, the other plugins started.  With a bundle
at root's installer the self test asks nothing; root's "no" lets it ask again; root busy past the handoff's time: its
late "OK" keeps the self test quiet, its late "no" lets it pass."""
import asyncio, base64, json, os, resource, signal, socket, subprocess, sys, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from aioesphomeapi import APIClient, ZERO_NOISE_PSK
from aioesphomeapi.model import UpdateInfo, UpdateState, UpdateCommand
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser
import plugsite
from plugsite import plugcheck
import fake_plugins as fp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GH, SITE, OTA = 16994, 16995, 16996
PCTOOL = [sys.executable, f"{ROOT}/scripts/otatool.py"]
check = fp.check


class GitHub(BaseHTTPRequestHandler):
    releases, files, hits = [], {}, []

    def log_message(self, *a): pass

    def send(self, code, body, ctype="application/json"):
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)

    def do_GET(self):
        GitHub.hits.append(self.path)
        if self.path.startswith("/repos/o/r/releases?per_page="): return self.send(200, json.dumps(GitHub.releases[:10]).encode())
        if self.path == "/repos/o/r/releases/latest": return self.send(200, json.dumps(GitHub.releases[0]).encode())
        parts = self.path.split("/")
        if len(parts) == 4 and parts[1] == "dl" and (parts[2], parts[3]) in GitHub.files: return self.send(200, GitHub.files[(parts[2], parts[3])], "application/octet-stream")
        self.send(404, b"not found")


def installer(state, keys, stop, got, busy):
    """root's ota_watch: take the request and its nonce, delete an old answer, verify against the release key or the
    update key, unpack, answer with the nonce under it; nothing while BUSY is set (root's loop at other work: a light
    ring's set, a reset's stop)"""
    ota = os.path.join(state, "ota")
    while not stop.is_set():
        if not busy.is_set() and os.path.exists(os.path.join(ota, "request")):
            nonce = (open(os.path.join(ota, "request")).read().split() + ["-", "-"])[1]
            os.unlink(os.path.join(ota, "request"))
            if os.path.exists(os.path.join(ota, "result")): os.unlink(os.path.join(ota, "result"))
            dest = os.path.join(state, f"installed{len(got) + 1}"); res = "FAILED no key"
            for k in keys:
                r = subprocess.run([f"{ROOT}/build/otatool-host", "install", k, f"{ota}/bundle", f"{ota}/bundle.sig", dest], capture_output=True, text=True)
                if r.returncode == 0: res = f"OK {r.stdout.strip()}"; break
            got.append(res)
            open(os.path.join(ota, "result.tmp"), "w").write(f"{res}\nnonce {nonce}\n"); os.rename(os.path.join(ota, "result.tmp"), os.path.join(ota, "result"))
        stop.wait(0.2)


async def main():
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log")
    for k in ("author", "release", "update"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    site = plugsite.Site(top, SITE)
    man = json.load(open(f"{ROOT}/tests/plugins/testwake.json"))
    def package(listed=False, **changes):
        m = dict(man, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, f"p{m['id']}-{m['version']}.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/testwake", os.path.join(top, "author.key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        if listed: plugcheck.list_package(out, site.key, out + ".l", arch="x86_64"); out += ".l"
        return open(out, "rb").read()
    v1 = package()
    version = subprocess.run(["make", "-s", "-C", ROOT, "version"], capture_output=True, text=True, check=True).stdout.strip()    # (what build/hassmic-host reports)
    serial = int(time.time()) - 3000
    site.build({"testwake": [v1]}, serial=serial)
    payload = os.path.join(top, "main.sh"); open(payload, "w").write("#!/bin/sh\necho main\n")
    def release(version, key="release"):
        tag = f"v{version}-beta"; d = os.path.join(top, version); os.mkdir(d)
        out = os.path.join(d, "b.bundle")
        subprocess.run(PCTOOL + ["pack", os.path.join(top, key + ".key"), version, out, payload], check=True, capture_output=True)
        GitHub.files[(tag, "hassmic-donut.bundle")] = open(out, "rb").read(); GitHub.files[(tag, "hassmic-donut.bundle.sig")] = open(out + ".sig", "rb").read()
        GitHub.releases.insert(0, {"tag_name": tag, "html_url": f"https://github.com/o/r/releases/tag/{tag}", "prerelease": True, "body": f"Release {version}."})
        return out
    gh = ThreadingHTTPServer(("127.0.0.1", GH), GitHub); threading.Thread(target=gh.serve_forever, daemon=True).start()
    stop, got, busy = threading.Event(), [], threading.Event()
    keys = [os.path.join(top, "release.pub"), os.path.join(top, "update.pub")]      # (emptied: root refuses every bundle)
    threading.Thread(target=installer, args=(state, keys, stop, got, busy), daemon=True).start()
    env = {"HASSMIC_PLUGIN_INDEX": site.base, "HASSMIC_PLUGINS_BUILT": str(int(time.time())), "HASSMIC_PLUGIN_DAILY": "3600000:86400000:3600000", "HASSMIC_PLUGIN_RELIST": "3600000:3600000",
           "HASSMIC_CLOCK_SYNCED": "1", "HASSMIC_PLUGIN_FRESH": "3600:0", "HASSMIC_RELEASE_PUB": os.path.join(top, "release.pub"),
           "HASSMIC_UPDATE_PUB": os.path.join(top, "update.pub"), "HASSMIC_UPDATE_API": f"http://127.0.0.1:{GH}/repos/o/r",
           "HASSMIC_UPDATE_DOWNLOAD": f"http://127.0.0.1:{GH}/dl", "HASSMIC_ADB_OPEN": os.path.join(state, "adb-open.root"),
           "HASSMIC_SELFTEST": "6000:2000"}       # (the self test's window 6 s, asked again every 2 s: 30 s and 10 s on an Echo)
    open(os.path.join(state, "config"), "w").write("plugins=on\n")
    proc = fp.start(state, log, extra=("-o", str(OTA)), env=env)
    pkgdir = os.path.join(state, "plugins", "testwake", "pkg")
    ha = None
    try:
        wait_port(fp.PORT, fp.WEB, OTA)
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "logged in with the button")
        # ---- the plugin installed and accepted, the wake word engine, listening; a package kept beside it for 0.1
        st, r = fp.upload(b, open(os.path.join(site.dir, "packages", "testwake-1.hmp"), "rb").read())       # (as the index lists it)
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(fp.until(lambda: (fp.plug(b) or {}).get("state") == "running", 20), f"the stand-in wake word plugin installed (official, with the button), running: {r.get('error')}")
        st, r = fp.item(b, "burst", {".model": b"MODEL Burst\n"})
        r = b.set(wake_engine="plugin:testwake")
        check(r.get("applied") == 1 and fp.until(lambda: os.path.exists(os.path.join(state, "plugins", "testwake", "ready")), 10)
              and open(os.path.join(state, "plugins", "testwake", "ready")).read().endswith("\nlisted 1\n"),
              f"it is the wake word engine and listens (its mark for the self test, which says it ran as listed): {r}")
        # (the self test passed before the first update is handed over: once root has one, a hassmic about to be replaced
        # passes none, U1 of a review of 1d704e9, and the "week" below counts from that pass)
        check(fp.logged(log, "self test: passed", 0, 30), "(the self test passed)")

        # ---- Home Assistant with a key: only it installs
        key = base64.b64encode(os.urandom(32))
        prov = APIClient("127.0.0.1", fp.PORT, None, noise_psk=ZERO_NOISE_PSK); await prov.connect()
        await prov.noise_encryption_set_key(key); await prov.disconnect()
        ha = APIClient("127.0.0.1", fp.PORT, None, noise_psk=key.decode()); await ha.connect(login=True)
        ents, _ = await ha.list_entities_services(); upd = next(e for e in ents if isinstance(e, UpdateInfo))
        states = []; ha.subscribe_states(states.append)
        last = lambda: ([s for s in states if isinstance(s, UpdateState) and s.key == upd.key] or [UpdateState()])[-1]
        async def reconnect():
            nonlocal ha
            for _ in range(50):
                try:
                    ha = APIClient("127.0.0.1", fp.PORT, None, noise_psk=key.decode()); await ha.connect(login=True); break
                except Exception: await asyncio.sleep(0.2)
            await ha.list_entities_services(); states.clear(); ha.subscribe_states(states.append)
        async def wait_for(pred, secs=20):
            for _ in range(int(secs * 10)):
                if pred(): return True
                await asyncio.sleep(0.1)
            return pred()

        # ---- an online update: nothing of the plugins' holds it or is fetched for it, the mark stays for its self test
        ready = os.path.join(state, "plugins", "testwake", "ready")
        hits0 = len(site.hits)
        release("2099.01.01.000000")
        b.set(online_updates="beta")
        check(await wait_for(lambda: last().latest_version == "2099.01.01.000000"), f"the update offered: {last().latest_version}")
        n0 = len(open(log).read())
        ha.update_command(upd.key, UpdateCommand.INSTALL)
        check(await wait_for(lambda: "installed, restarting" in last().release_summary, 40) and got == ["OK 2099.01.01.000000"],
              f"installed as it came, nothing held: {last().release_summary!r} {got}")
        check(len(site.hits) == hits0 and not fp.logged(log, "plugins: downloading", n0, 1) and os.path.exists(ready) and (fp.plug(b) or {}).get("state") == "running",
              f"the plugin index asked for nothing for it, the wake word plugin's mark kept for the next hassmic, the plugin running: {site.hits[hits0:]}")
        u = json.loads(b.call("GET", "/api/state")[2])
        check("update" not in u, f"nothing of an update held on the settings page: {u.get('update')}")
        # ---- a push: the same
        out = release("2099.01.02.000000", key="update")
        n0 = len(open(log).read())
        r = await asyncio.to_thread(subprocess.run, PCTOOL + ["push", "127.0.0.1", str(OTA), out, out + ".sig"], capture_output=True, text=True)
        check(r.returncode == 0 and r.stdout.startswith("OK 2099.01.02.000000") and os.path.exists(ready) and not fp.logged(log, "will not wait for it", n0, 1),
              f"a pushed bundle: installed, the mark kept: {r.stdout.strip()} {r.stderr.strip()}")
        try: await ha.disconnect()
        except Exception: pass
        ha = None

        # ---- the self test where the wake word plugin does not run (a review of 61028a8): an update installed with
        # plugins off, or with the plugin switched off, left its mark of having listened, and the next hassmic waited
        # for it in vain: never the fallback
        def conf(on):
            text = "".join(l for l in open(os.path.join(state, "config")) if not l.startswith("plugins="))
            open(os.path.join(state, "config"), "w").write(text + f"plugins={on}\n")
        for case in ("plugins off", "the plugin switched off"):
            proc.terminate(); proc.wait(5)
            open(ready, "w").write("ready under the build before\n")
            if case == "plugins off": conf("off")
            else: conf("on"); os.unlink(os.path.join(state, "plugins", "testwake", "enabled"))
            n0 = len(open(log).read())
            proc = fp.start(state, log, extra=("-o", str(OTA)), env=env)
            wait_port(fp.PORT, fp.WEB, OTA)
            check("wake_engine=plugin:testwake" in open(os.path.join(state, "config")).read() and fp.logged(log, "self test: passed", n0, 40),
                  f"{case}, its mark there: the self test does not wait for a wake word plugin that does not run, and passes")

        # ---- a fall back to an older hassmic (HASSMIC_PLUGIN_SCOPES: this build without wake): the plugin requires
        # what it has not, its card says so, Amazon's wake word stands in, and the self test does not wait for it
        proc.terminate(); proc.wait(5)
        conf("on"); open(os.path.join(state, "plugins", "testwake", "enabled"), "w").close()
        open(ready, "w").write("ready under the build before\nlisted 1\n")
        n0 = len(open(log).read())
        proc = fp.start(state, log, extra=("-o", str(OTA)), env=dict(env, HASSMIC_PLUGIN_SCOPES="wake:0"))
        wait_port(fp.PORT, fp.WEB, OTA)
        p = fp.plug(b) or {}
        check(fp.logged(log, "self test: passed", n0, 40) and p.get("newer") is True and p.get("here") is False and "needs a newer hassmic: wake 1" in p.get("detail", "")
              and p.get("version") == 1 and p.get("name") == "Test wake word", f"a build without wake: the card says it needs a newer hassmic, the self test does not wait for it: {p}")
        check(fp.logged(log, "testwake cannot run: it needs a newer hassmic: wake 1", n0, 1) and os.path.exists(os.path.join(pkgdir, "manifest.json")),
              "(said in the log, the package kept)")
        st, r = fp.upload(b, v1)
        check(st == 400 and "needs a newer hassmic: wake 1" in r.get("error", ""), f"its package again in that build: refused at install with the same words: {st} {r.get('error')}")
        proc.terminate(); proc.wait(5)
        n0 = len(open(log).read())
        proc = fp.start(state, log, extra=("-o", str(OTA)), env=env)
        wait_port(fp.PORT, fp.WEB, OTA)
        check(fp.until(lambda: (fp.plug(b) or {}).get("state") == "running", 20) and not (fp.plug(b) or {}).get("newer") and fp.logged(log, "self test: passed", n0, 40),
              "the build that has wake again: it runs, listens, and the self test passes")

        # ---- ... and where it does not run by the new build's doing, the update is held as before 913ec8d (a review
        # of 913ec8d: a build whose launcher never answered passed in 1 s while Amazon's wake word listened, and
        # became the fallback): the launcher silent, the plugin's program not the one accepted
        open(os.path.join(state, "plugins", "testwake", "enabled"), "w").close()
        prog = os.path.join(pkgdir, "plugin"); good = open(prog, "rb").read()
        held = "self test: the wake word plugin testwake, which listened under the version before, does not under this one"
        for case in ("the launcher silent", "the plugin's program changed"):
            proc.terminate(); proc.wait(5)
            conf("on")
            open(prog, "wb").write(good[:-1] + bytes([good[-1] ^ 1]) if case == "the plugin's program changed" else good)
            open(ready, "w").write("ready under the build before\n")
            n0 = len(open(log).read())
            quiet = None
            if case == "the launcher silent":
                quiet, fd = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                proc = fp.start(state, log, extra=("-o", str(OTA)), env=env, launcher_fd=fd.fileno()); fd.close()
            else: proc = fp.start(state, log, extra=("-o", str(OTA)), env=env)
            wait_port(fp.PORT, fp.WEB, OTA)
            check(fp.logged(log, held, n0, 40) and not fp.logged(log, "self test: passed", n0, 1),
                  f"{case}, its mark there: the self test waits for the wake word plugin, which does not listen, and the update does not become the fallback")
            if quiet: quiet.close()
        open(prog, "wb").write(good)

        # ---- community plugins off: the owner's choice for a plugin that is not official, which the self test does not
        # wait for; but not where the listing is one this build does not read (5.2's build of the new index key alone,
        # the plugin still listed by the old key): that update stopped the official wake word engine as "from a file"
        # and was promoted with no wake word at all (a review of 3c5d7ff, R2): held as the new build's doing
        lsig = os.path.join(pkgdir, "listing.sig"); had = open(lsig, "rb").read() if os.path.exists(lsig) else None
        cfg = os.path.join(state, "config"); kept = "".join(l for l in open(cfg) if l.startswith("plugins_community="))
        def community(on):
            text = "".join(l for l in open(cfg) if not l.startswith("plugins_community="))
            open(cfg, "w").write(text + (f"plugins_community={on}\n" if on else kept))
        # (S8 of a review of 3fcd319: the mark says whether the plugin ran as listed; one that ran as a file's before, its
        # listing no more read by that build than by this one, with community plugins on then and off now: the owner's
        # choice, as for a file's; a mark without the line, a build's before, counts as listed)
        # (U2 of a review of 1d704e9: a community listing this build does not read is the owner's switch whatever the mark
        # says: one without the line, a build's before, or 9cd7a77's "listed 1", which it wrote for any listing that held,
        # held every update, and with community plugins off the plugin never runs to write its mark anew)
        for case in ("no listing (a file's), community plugins off", "a listing this build does not read, community plugins off",
                     "a listing this build does not read, community plugins off, the mark of one that ran as a file's",
                     "a community listing this build does not read, community plugins off, a mark without the line",
                     "a community listing this build does not read, community plugins off, a mark that says listed"):
            proc.terminate(); proc.wait(5)
            conf("on"); community("off")
            if case.startswith("no listing"):
                if os.path.exists(lsig): os.unlink(lsig)
            else: open(lsig, "wb").write(b"00" * 64 + b"\nhassmic plugin listing 2\nid testwake\nversion 1\ntier "
                                         + (b"community" if case.startswith("a community") else b"official") + b"\n")
            open(ready, "w").write("ready under the build before\n" + ("listed 0\n" if case.endswith("file's") else "listed 1\n" if case.endswith("says listed") else ""))
            n0 = len(open(log).read())
            proc = fp.start(state, log, extra=("-o", str(OTA)), env=env)
            wait_port(fp.PORT, fp.WEB, OTA)
            if case.startswith("no listing") or case.endswith("file's") or case.startswith("a community"):
                check(fp.logged(log, "self test: passed", n0, 40), f"{case}: the owner's choice, the self test does not wait for the plugin, and passes")
            else:
                check(fp.logged(log, held, n0, 40) and not fp.logged(log, "self test: passed", n0, 1) and fp.logged(log, "by a plugin index key this hassmic does not read", n0, 1),
                      f"{case}: not the owner's choice; the self test waits for the wake word plugin, and the update does not become the fallback")
        proc.terminate(); proc.wait(5)
        community(None)
        if had is None: os.unlink(lsig)
        else: open(lsig, "wb").write(had)

        # ---- T5 of a review of 9cd7a77: a wake word plugin that ran by a community listing ran by the owner's switch for
        # community plugins, as a file's does: its mark says "listed 0" (with community plugins off it runs under no
        # build; "listed 1" held every update once the owner had switched them off, its listing unread there)
        mj = open(os.path.join(pkgdir, "manifest.json"), "rb").read()
        community("on"); open(lsig, "wb").write(plugcheck.listing(json.loads(mj), mj, site.key, "community", serial))
        if os.path.exists(ready): os.unlink(ready)
        n0 = len(open(log).read())
        proc = fp.start(state, log, extra=("-o", str(OTA)), env=env)
        wait_port(fp.PORT, fp.WEB, OTA)
        check(fp.until(lambda: os.path.exists(ready), 20) and open(ready).read().endswith("\nlisted 0\n") and (fp.plug(b) or {}).get("tier") == "community",
              f"a community listing, community plugins on: it listens, and its mark says it did not run as listed: {open(ready).read() if os.path.exists(ready) else None!r} {(fp.plug(b) or {}).get('tier')}")
        proc.terminate(); proc.wait(5)
        community(None)
        if had is None: os.unlink(lsig)
        else: open(lsig, "wb").write(had)

        # ---- T1 of a review of 9cd7a77: held by the wake word plugin alone, the self test goes on asking past its 30 s.
        # 5.2's step (3): the plugin's listing one this build does not read, community plugins off; the Echo's own look
        # at the index (relist_tick, here 12 s after the start: after the window) takes the index's listing, the plugin
        # listens, and the self test passes then: the build becomes the fallback (state/ota/healthy, which root takes
        # whenever it comes), and the other plugins, which wait for the self test, start.  It ended at 30 s: every
        # other plugin stopped for the whole run, the build the fallback only at a restart
        site.build({"testwake": [v1], "testother": [package(id="testother", name="Test other")]})
        proc = fp.start(state, log, extra=("-o", str(OTA)), env=env)
        wait_port(fp.PORT, fp.WEB, OTA)
        st, r = fp.upload(b, open(os.path.join(site.dir, "packages", "testother-1.hmp"), "rb").read())
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(fp.until(lambda: (fp.plug(b, "testother") or {}).get("state") == "running", 20), f"(a second plugin, official, installed with the button and running: {r.get('error')})")
        proc.terminate(); proc.wait(5)
        community("off"); open(lsig, "wb").write(b"00" * 64 + b"\nhassmic plugin listing 2\nid testwake\nversion 1\ntier official\n")
        open(ready, "w").write("ready under the build before\n")
        healthy = os.path.join(state, "ota", "healthy"); online = os.path.join(top, "online"); open(online, "w").close()
        if os.path.exists(healthy): os.unlink(healthy)
        n0 = len(open(log).read())
        proc = fp.start(state, log, extra=("-o", str(OTA)), env=dict(env, HASSMIC_PLUGIN_RELIST="12000:3600000", HASSMIC_FAKE_ONLINE=online))
        wait_port(fp.PORT, fp.WEB, OTA)
        check(fp.logged(log, held, n0, 40) and not os.path.exists(healthy) and (fp.plug(b, "testother") or {}).get("state") != "running",
              f"the listing unread, community off: the self test held at 30 s, not the fallback, the other plugin waits: {(fp.plug(b, 'testother') or {}).get('state')}")
        check(fp.logged(log, "testwake version 1: the plugin index's listing taken", n0, 30) and fp.logged(log, "self test: passed, ", n0, 25)
              and os.path.exists(healthy) and fp.until(lambda: (fp.plug(b, "testother") or {}).get("state") == "running", 10),
              f"the Echo's own look takes the listing, the plugin listens: the self test passes then, the build the fallback, the other plugin started: {(fp.plug(b, 'testother') or {}).get('state')}")
        check(open(healthy).read() == version + "\n", f"`healthy` names the build that passed, for root to promote only that one: {open(healthy).read()!r} {version!r}")
        proc.terminate(); proc.wait(5)

        # ---- U1 of a review of 1d704e9, the same hold: once a bundle is with root's installer (a push, root said OK),
        # the self test asks nothing, even with the plugin listening again: a late pass left `healthy` in the seconds
        # before root installed the next, and root promoted that untested one.  Root's "no" lets it ask again, and it
        # passes
        open(lsig, "wb").write(b"00" * 64 + b"\nhassmic plugin listing 2\nid testwake\nversion 1\ntier official\n")
        open(ready, "w").write("ready under the build before\n")
        if os.path.exists(healthy): os.unlink(healthy)
        plain = os.path.join(top, "plain.bundle")
        subprocess.run(PCTOOL + ["pack", os.path.join(top, "update.key"), "2099.02.01.000000", plain, payload], check=True, capture_output=True)
        n0 = len(open(log).read())
        proc = fp.start(state, log, extra=("-o", str(OTA)), env=dict(env, HASSMIC_PLUGIN_RELIST="16000:3600000", HASSMIC_FAKE_ONLINE=online))
        wait_port(fp.PORT, fp.WEB, OTA)
        check(fp.logged(log, held, n0, 40) and not os.path.exists(healthy), "(held by the wake word plugin at the window's end)")
        n_got = len(got)
        r = await asyncio.to_thread(subprocess.run, PCTOOL + ["push", "127.0.0.1", str(OTA), plain, plain + ".sig"], capture_output=True, text=True)
        check(r.stdout.startswith("OK 2099.02.01.000000") and len(got) == n_got + 1, f"(a bundle pushed, root installed it: {r.stdout.strip()} {r.stderr.strip()})")
        check(fp.logged(log, "testwake version 1: the plugin index's listing taken", n0, 30) and fp.logged(log, 'testwake: "burst" listens', n0, 20), "(the Echo's own look at 16 s takes the listing, the plugin listens)")
        time.sleep(3)                                       # (an ask after it)
        check(not fp.logged(log, "self test: passed", n0, 1) and not os.path.exists(healthy),
              "U1: with a bundle at root's installer the self test asks nothing, the plugin listening: no pass, no `healthy` for root to take after the install")
        saved = keys[:]; keys.clear()
        r = await asyncio.to_thread(subprocess.run, PCTOOL + ["push", "127.0.0.1", str(OTA), plain, plain + ".sig"], capture_output=True, text=True)
        keys.extend(saved)
        check(r.stdout.startswith("FAILED") and fp.logged(log, "self test: passed, ", n0, 15) and os.path.exists(healthy) and open(healthy).read() == version + "\n",
              f"root refused the next push: the self test asks again and passes, `healthy` names this build: {r.stdout.strip()}")
        proc.terminate(); proc.wait(5)

        # ---- V2 of a review of 0119caf: root busy past the handoff's minute (2 s here, HASSMIC_OTA_ANSWER_S) answers
        # later.  The bundle stayed "with the installer" for the run: a later "no" never read, the self test never
        # passed, the other plugins stopped until a restart.  Its answer is read once root took the request: "OK" keeps
        # the self test quiet (the install is under way), a "no" lets it ask again
        open(ready, "w").write("ready under the build before\n")
        if os.path.exists(healthy): os.unlink(healthy)
        n0 = len(open(log).read())
        proc = fp.start(state, log, extra=("-o", str(OTA)), env=dict(env, HASSMIC_PLUGIN_RELIST="10000:3600000", HASSMIC_FAKE_ONLINE=online, HASSMIC_OTA_ANSWER_S="2"))
        wait_port(fp.PORT, fp.WEB, OTA)
        busy.set(); n_got = len(got)
        r = await asyncio.to_thread(subprocess.run, PCTOOL + ["push", "127.0.0.1", str(OTA), plain, plain + ".sig"], capture_output=True, text=True)
        check("did not answer" in r.stdout and len(got) == n_got, f"(root busy: no answer within the handoff's time: {r.stdout.strip()})")
        check(fp.logged(log, 'testwake: "burst" listens', n0, 30), "(the Echo's own look at 10 s takes the listing, the plugin listens)")
        time.sleep(3)
        check(not fp.logged(log, "self test: passed", n0, 1) and not os.path.exists(healthy), "V2: root has the bundle and said nothing yet: no pass, the plugin listening")
        busy.clear()
        check(fp.logged(log, "update: the installer answered late: OK 2099.02.01.000000", n0, 10) and len(got) == n_got + 1, f"(root installed it late: {got[n_got:]})")
        time.sleep(3)
        check(not fp.logged(log, "self test: passed", n0, 1) and not os.path.exists(healthy), "V2: root's late \"OK\" read: no pass, the install under way")
        busy.set(); n_got = len(got)
        r = await asyncio.to_thread(subprocess.run, PCTOOL + ["push", "127.0.0.1", str(OTA), plain, plain + ".sig"], capture_output=True, text=True)
        check("did not answer" in r.stdout, f"(the next push, root busy again: {r.stdout.strip()})")
        saved = keys[:]; keys.clear(); busy.clear()
        check(fp.logged(log, "update: the installer answered late: FAILED", n0, 10) and fp.logged(log, "self test: passed, ", n0, 10)
              and os.path.exists(healthy) and open(healthy).read() == version + "\n",
              f"V2: root's late \"no\" read once it took the request: the self test asks again and passes, `healthy` names this build: {got[n_got:]}")
        keys.extend(saved)
        proc.terminate(); proc.wait(5)
        community(None)
        if had is None: os.unlink(lsig)
        else: open(lsig, "wb").write(had)
    finally:
        if ha:
            try: await ha.disconnect()
            except Exception: pass
        stop.set(); proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()
        gh.shutdown()
    if check.failed:
        print("---- log"); print(open(log, errors="replace").read()[-8000:])
    print("plugins selftest: " + ("FAILED" if check.failed else "all passed"))
    return 1 if check.failed else 0

sys.exit(asyncio.run(main()))
