#!/usr/bin/env python3
"""Sources (src/hassmic/sources.c, issue #175): where online updates and the plugin index come from, set on the settings
page, against build/hassmic-host.  This script is the settings page (tests/webclient.py), Home Assistant (to install),
a Forgejo with releases (over plain http: its API is GitHub's for what update.c reads, at /api/v1/repos/<o>/<r>), root
(main.sh's ota_watch and page_pub, the same otatool commands: the update key of the page taken from state/update-key,
every bundle checked against the release key and that one) and the owner's own plugin index (tests/plugsite.py).

What it holds: a server alone is applied at once and changes nothing of what is trusted (a bundle under another key
still never reaches root); a key of the owner's or no check waits for the action button, only for a server of the
owner's, and is the press's only for what was asked; root is asked for the key and the page says when it holds it;
no check means no .sig fetched and hassmic's own key signs; back to the project's at once, root's key taken away;
the plugin index of the owner's: restart asked, the old index's floors forgotten, its key beside the project's, no
check takes lists without signatures; the page's level (0 none, 1 a server, 2 a key or no check)."""
import asyncio, base64, json, os, shutil, signal, subprocess, sys, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from aioesphomeapi import APIClient, ZERO_NOISE_PSK
from aioesphomeapi.model import UpdateInfo, UpdateState, UpdateCommand

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import wait_port, Browser
import plugsite
PORT, FJ_PORT, WEB, IDX = 16966, 16967, 16968, 16969
OTATOOL = f"{ROOT}/build/otatool-host"
PCTOOL = [sys.executable, f"{ROOT}/scripts/otatool.py"]
fails = 0


def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    fails += not ok


class Forgejo(BaseHTTPRequestHandler):
    """/api/v1/repos/o/r/releases (per_page ignored, as Forgejo does), .../releases/latest, /o/r/releases/download/<tag>/<asset>"""
    releases, files, hits = [], {}, []

    def log_message(self, *a): pass

    def send(self, code, body, ctype="application/json"):
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)

    def do_GET(self):
        Forgejo.hits.append(self.path)
        path = self.path.split("?")[0]
        if path == "/api/v1/repos/o/r/releases": return self.send(200, json.dumps(Forgejo.releases).encode())
        if path == "/api/v1/repos/o/r/releases/latest":
            rel = [r for r in Forgejo.releases if not r["prerelease"]]
            return self.send(200, json.dumps(rel[0]).encode()) if rel else self.send(404, b'{"message":"not found"}')
        p = path.split("/")
        if len(p) == 7 and p[1:5] == ["o", "r", "releases", "download"] and (p[5], p[6]) in Forgejo.files:
            return self.send(200, Forgejo.files[(p[5], p[6])], "application/octet-stream")
        self.send(404, b"not found")


def release(tag, pre=True):
    return {"tag_name": tag, "name": tag, "prerelease": pre, "html_url": f"http://forgejo/o/r/releases/tag/{tag}", "body": f"{tag} notes"}


def root(state, tmp, release_pub, stop, log):
    """main.sh's ota_watch as far as Sources go: page_pub (state/update-key: 32 bytes kept, empty taken away) before the
    request, then the bundle checked against the release key and the page's key (otatool install, as root does)"""
    ota, page = os.path.join(state, "ota"), os.path.join(tmp, "page-update.pub"); n = 0
    while not stop.is_set():
        k = os.path.join(state, "update-key")
        if os.path.exists(k):
            data = open(k, "rb").read(); os.unlink(k)
            if len(data) == 32: open(page + ".new", "wb").write(data); os.rename(page + ".new", page); log.append("key")
            elif not data and os.path.exists(page): os.unlink(page); log.append("forgot")
        if os.path.exists(os.path.join(ota, "request")):
            os.unlink(os.path.join(ota, "request")); n += 1
            dest = os.path.join(state, f"installed{n}"); res = "FAILED the signature verifies against no key"
            for key in [release_pub] + ([page] if os.path.exists(page) else []):
                r = subprocess.run([OTATOOL, "install", key, f"{ota}/bundle", f"{ota}/bundle.sig", dest], capture_output=True, text=True)
                if r.returncode == 0: res = f"OK {r.stdout.strip()}"; log.append(f"installed {r.stdout.strip()} {os.path.basename(key)}"); break
                shutil.rmtree(dest, ignore_errors=True)
            open(os.path.join(ota, "result.tmp"), "w").write(res + "\n"); os.rename(os.path.join(ota, "result.tmp"), os.path.join(ota, "result"))
        stop.wait(0.1)


async def wait_for(pred, secs=10):
    for _ in range(int(secs * 10)):
        if pred(): return True
        await asyncio.sleep(0.1)
    return pred()


def post(b, body):
    st, _, data = b.call("POST", "/api/sources", body.encode())
    return json.loads(data) if st == 200 else {"status": st}


async def main():
    tmp = tempfile.mkdtemp(); state = os.path.join(tmp, "state"); os.makedirs(state)
    rel_pub, rel_sec = os.path.join(tmp, "release.pub"), os.path.join(tmp, "release.key")
    own_pub, own_sec = os.path.join(tmp, "mine.pub"), os.path.join(tmp, "mine.key")
    for sec, pub in ((rel_sec, rel_pub), (own_sec, own_pub), (own_sec + ".other", own_pub + ".other")):
        subprocess.run(PCTOOL + ["keygen", sec, pub], check=True, capture_output=True)
    main_sh = os.path.join(tmp, "main.sh"); open(main_sh, "w").write("#!/bin/sh\necho main\n")

    def publish(tag, key, sig=True):
        out = os.path.join(tmp, f"{tag}.bundle")
        subprocess.run(PCTOOL + ["pack", key, tag[1:].removesuffix("-beta"), out, main_sh], check=True, capture_output=True)
        Forgejo.files[(tag, "hassmic-donut.bundle")] = open(out, "rb").read()
        if sig: Forgejo.files[(tag, "hassmic-donut.bundle.sig")] = open(out + ".sig", "rb").read()
        Forgejo.releases.insert(0, release(tag))

    fj = ThreadingHTTPServer(("127.0.0.1", FJ_PORT), Forgejo); threading.Thread(target=fj.serve_forever, daemon=True).start()
    site = plugsite.Site(tmp, IDX, key="own-index"); site.build({})
    os.makedirs(os.path.join(tmp, "proj")); project_index = plugsite.Site(os.path.join(tmp, "proj"), IDX + 1)     # the project's index: another key
    stop = threading.Event(); rootlog = []
    threading.Thread(target=root, args=(state, tmp, rel_pub, stop, rootlog), daemon=True).start()
    log = os.path.join(tmp, "log")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_SETTINGS=os.path.join(state, "settings"),
               HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(tmp, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(tmp, "hassmic.service"), HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_MODELS=os.path.join(state, "models"), HASSMIC_ADB_OPEN=os.path.join(state, "adb-open.root"),
               HASSMIC_RELEASE_PUB=rel_pub, HASSMIC_PAGE_UPDATE_PUB=os.path.join(tmp, "page-update.pub"),
               # the project's own: never asked once Sources name the owner's
               HASSMIC_UPDATE_API="http://127.0.0.1:9/repos/p/p", HASSMIC_UPDATE_DOWNLOAD="http://127.0.0.1:9/dl",
               HASSMIC_PLUGINS_PUB=project_index.pub, HASSMIC_PLUGIN_INDEX=project_index.base, HASSMIC_CLOCK_SYNCED="0",
               HASSMIC_LOGIN_MS="20000:300")
    start = lambda: subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-L", "-W", str(WEB)],
                                     env=env, stderr=open(log, "a"))
    proc = start(); wait_port(PORT, WEB)
    try:
        page = Browser(WEB)
        check(page.login_with_button(proc), "settings page: logged in with the action button")
        other = Browser(WEB)                            # (before any wait: one withdrawn keeps logins back for its time)
        check(other.login_with_button(proc, busy_secs=20), "a second browser logged in")
        src = lambda: page.state()["sources"]
        s = src()
        check(s["level"] == 0 and s["updates"]["url"] == "" and s["updates"]["default"].startswith("https://github.com/") and s["plugins"]["default"] == project_index.base,
              f"the project's both by default, level 0: {s}")

        # nobody but an approved browser
        st, _, _ = Browser(WEB).call("POST", "/api/sources", f"updates_url=http://127.0.0.1:{FJ_PORT}/o/r\n".encode())
        check(st == 401 and src()["updates"]["url"] == "", "a browser not approved changes nothing (401)")
        # a key or no check only for a server of the owner's; addresses checked
        mine = open(own_pub, "rb").read().hex()
        r = post(page, f"updates_key={mine}\n")
        check(r.get("sources") == "refused" and "server of your own" in r.get("why", ""), f"a key for the project's server refused: {r}")
        r = post(page, "updates_unchecked=1\n")
        check(r.get("sources") == "refused", f"no check for the project's server refused: {r}")
        for bad in ("ftp://x/o/r", "http://x/onlyone", "http://x/o/r?x=1", 'http://x/o/"r', "http://x/o/r y", "https://user:secret@x/o/r"):
            r = post(page, f"updates_url={bad}\n")
            check(r.get("sources") == "refused", f"{bad!r} refused: {r.get('why')}")
        r = post(page, f"updates_url=http://127.0.0.1:{FJ_PORT}/o/r\nupdates_key=00\n")
        check(r.get("sources") == "refused" and "64 hex" in r.get("why", ""), f"a key that is no key refused: {r}")
        r = post(page, f"updates_url=http://127.0.0.1:{FJ_PORT}/o/r\nupdates_key={'00' * 32}\n")
        check(r.get("sources") == "refused" and "any signature" in r.get("why", ""), f"a key of small order refused: {r}")
        check(src()["level"] == 0 and not os.path.exists(os.path.join(state, "sources")), "nothing of the refused saved")

        # a server alone: at once, level 1; online updates look there and take the release key's builds
        r = post(page, f"updates_url=http://127.0.0.1:{FJ_PORT}/o/r/\n")
        s = src()
        check(r.get("sources") == "applied" and s["updates"]["url"] == f"http://127.0.0.1:{FJ_PORT}/o/r" and s["level"] == 1,
              f"a server of the owner's applied at once (the / at its end dropped), level 1: {r} {s['updates']}")
        check("updates_url" in open(os.path.join(state, "sources")).read() and not os.path.exists(os.path.join(state, "update-key")),
              "state/sources saved; root asked nothing")
        cfg = page.set(online_updates="beta")
        check(not cfg.get("github_unreachable") and any(h.startswith("/api/v1/repos/o/r/releases") for h in Forgejo.hits),
              f"switching the channel on asked the owner's server, not GitHub: {cfg}")
        exp = page.call("GET", "/api/export")[2].decode()
        check("sources" not in exp and "127.0.0.1" not in exp, "not in the export")

        cli = APIClient("127.0.0.1", PORT, None); await cli.connect(login=True)
        ents, _ = await cli.list_entities_services(); upd = next(e for e in ents if isinstance(e, UpdateInfo))
        key = base64.b64encode(os.urandom(32))
        prov = APIClient("127.0.0.1", PORT, None, noise_psk=ZERO_NOISE_PSK); await prov.connect()
        await prov.noise_encryption_set_key(key); await prov.disconnect()
        enc = APIClient("127.0.0.1", PORT, None, noise_psk=key.decode()); await enc.connect(login=True)
        states = []; enc.subscribe_states(states.append)
        last = lambda: ([x for x in states if isinstance(x, UpdateState) and x.key == upd.key] or [UpdateState()])[-1]

        async def install(version, secs=15):
            enc.update_command(upd.key, UpdateCommand.CHECK)
            await wait_for(lambda: last().latest_version == version)
            enc.update_command(upd.key, UpdateCommand.INSTALL)
            await wait_for(lambda: not last().in_progress and ("installed" in last().release_summary or "not installed" in last().release_summary
                                                                or "failed" in last().release_summary), secs)
            return last().release_summary

        publish("v2099.01.01.000000-beta", rel_sec)
        r = await install("2099.01.01.000000")
        check("installed" in r and any("2099.01.01.000000 release.pub" in x for x in rootlog),
              f"from the owner's Forgejo, under the release key: installed ({r!r})")
        check(any(h == "/o/r/releases/download/v2099.01.01.000000-beta/hassmic-donut.bundle" for h in Forgejo.hits), "downloaded at Forgejo's address")
        publish("v2099.01.02.000000-beta", own_sec)
        r = await install("2099.01.02.000000")
        check("release key" in r and not any("2099.01.02" in x for x in rootlog), f"a server alone trusts nothing more: the owner's key's bundle never reaches root ({r!r})")

        # a key of the owner's: waits for the button, ring on; another key asked meanwhile is another wait; the press
        # puts in force what was asked
        r = post(page, f"updates_key={mine}\n")
        b = page.state()["button"]
        check(r.get("sources") == "waiting" and r.get("holds") == "sources" and b["holds"] == "sources" and b["mine"] and src()["level"] == 1,
              f"a key of the owner's waits for the action button, nothing in force yet: {r} {b}")
        r = post(page, "cancel\n")
        check(r.get("sources") == "cancelled" and page.state()["button"]["holds"] == "none", "cancelled: nothing waits")
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.5)
        check(src()["level"] == 1 and not src()["updates"]["key"], "a press after the cancel puts nothing in force")
        r = post(page, f"updates_key={mine}\n")
        r2 = post(other, f"updates_key={open(own_pub + '.other', 'rb').read().hex()}\n")
        check(r.get("sources") == "waiting" and r2.get("sources") == "busy" and r2.get("holds") == "sources", f"another browser's key while one waits: busy {r2}")
        proc.send_signal(signal.SIGUSR2)
        check(await wait_for(lambda: src()["updates"]["key"] == mine), "pressed: the owner's key in force")
        s = src()
        check(s["level"] == 2, f"level 2 (red): {s['level']}")
        check(await wait_for(lambda: "key" in rootlog and src()["updates"]["root"] == "held") and open(os.path.join(tmp, "page-update.pub"), "rb").read() == open(own_pub, "rb").read(),
              f"root asked for it (state/update-key) and holds it; the page says so: {src()['updates'].get('root')}")
        publish("v2099.01.03.000000-beta", own_sec)
        r = await install("2099.01.03.000000")
        check("installed" in r and any("2099.01.03.000000 page-update.pub" in x for x in rootlog), f"the owner's key's bundle installed ({r!r})")
        publish("v2099.01.04.000000-beta", own_sec + ".other")
        r = await install("2099.01.04.000000")
        check("not installed" in r and not any("2099.01.04" in x for x in rootlog), f"another key's still refused ({r!r})")

        # the server changed while a key stands: the button again (the key would then vouch for another server's)
        r = post(page, f"updates_url=http://127.0.0.1:{FJ_PORT}/x/y\n")
        check(r.get("sources") == "waiting", f"another server for the key: waits for the button: {r}")
        post(page, "cancel\n")

        # a change between the arming and the press: the press is not for it
        r = post(page, "updates_unchecked=1\n")
        r2 = post(other, f"updates_url=http://127.0.0.1:{FJ_PORT}/o/r\nupdates_key=\n")      # trusts less: at once
        check(r.get("sources") == "waiting" and r2.get("sources") == "applied", f"no check waits; the key taken away by another browser at once: {r} {r2}")
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.5)
        s = src()
        check(not s["updates"]["unchecked"] and not s["updates"]["key"] and s["level"] == 1, f"the press after that change put nothing in force: {s['updates']}")
        check(await wait_for(lambda: "forgot" in rootlog), "root told to forget the key")

        # no check: the button; no .sig fetched, hassmic's own key signs, root holds that one
        r = post(page, "updates_unchecked=1\n")
        proc.send_signal(signal.SIGUSR2)
        check(r.get("sources") == "waiting" and await wait_for(lambda: src()["updates"]["unchecked"]), "no check: in force with the button")
        ownk = os.path.join(state, "update-own.key")
        check(os.path.exists(ownk) and os.stat(ownk).st_mode & 0o777 == 0o600, "hassmic's own key made, 0600")
        check(await wait_for(lambda: src()["updates"]["root"] == "held"), "root holds hassmic's own key")
        publish("v2099.01.05.000000-beta", own_sec + ".other", sig=False)
        n = len(Forgejo.hits)
        r = await install("2099.01.05.000000")
        check("installed" in r and any("2099.01.05.000000 page-update.pub" in x for x in rootlog), f"a bundle under no key we know, no .sig on the server: installed ({r!r})")
        check(not any(h.endswith(".sig") for h in Forgejo.hits[n:]), "no signature asked for")

        # the key taken away before root took the request for it: root ends up with none (the last request wins)
        stop.set(); await asyncio.sleep(0.3)        # root's look paused
        r = post(page, "updates_key=" + mine + "\nupdates_unchecked=0\n"); proc.send_signal(signal.SIGUSR2)
        await wait_for(lambda: src()["updates"]["key"] == mine)
        r2 = post(page, "updates_key=\n")
        stop = threading.Event(); threading.Thread(target=root, args=(state, tmp, rel_pub, stop, rootlog), daemon=True).start()
        check(r.get("sources") == "waiting" and r2.get("sources") == "applied" and await wait_for(lambda: not os.path.exists(os.path.join(tmp, "page-update.pub")) and src()["updates"]["root"] == "held"),
              f"a key taken away within root's look: root holds none ({r2})")
        r = post(page, "updates_unchecked=1\n"); proc.send_signal(signal.SIGUSR2)
        check(await wait_for(lambda: src()["updates"]["unchecked"] and src()["updates"]["root"] == "held"), "no check again, root holds hassmic's own key")

        # back to the project's: at once, root's key taken away, the own key gone, level 0
        r = post(page, "updates_url=\nupdates_unchecked=0\n")
        check(r.get("sources") == "applied" and src()["level"] == 0 and not os.path.exists(ownk), f"back to the project's at once, own key gone: {r}")
        check(await wait_for(lambda: rootlog.count("forgot") >= 2 and not os.path.exists(os.path.join(tmp, "page-update.pub"))), "root's key taken away")

        # the plugin index: a site of the owner's restarts hassmic and forgets the old one's floors
        page.set(plugins="on")
        os.makedirs(os.path.join(state, "plugins"), exist_ok=True)
        for f in (".serial", ".revoked", ".listed", ".installed"):
            open(os.path.join(state, "plugins", f), "w").write("x\n")
        r = post(page, f"plugins_url={site.base}\n")
        check(r.get("sources") == "applied" and src()["level"] == 1 and open(os.path.join(state, "restart")).read() == "plugin index\n",
              f"the owner's plugin index at once, restart asked: {r}")
        check(all(os.path.exists(os.path.join(state, "plugins", f)) for f in (".serial", ".revoked", ".listed", ".installed")),
              "a server alone (a mirror, the same keys): the old index's floors kept (an older list of the project's stays refused)")
        for f in (".serial", ".revoked", ".listed"): os.unlink(os.path.join(state, "plugins", f))
        os.unlink(os.path.join(state, "restart"))

        async def restart_and_look():
            nonlocal proc
            proc.terminate(); proc.wait(); proc = start(); wait_port(PORT, WEB)
            page.call("POST", "/api/plugins/index", b"again")
            for _ in range(200):
                o = json.loads(page.call("GET", "/api/plugins/index")[2])
                if o.get("state") not in ("looking", None): return o
                await asyncio.sleep(0.1)
            return o

        o = await restart_and_look()
        check(o.get("state") != "ok" and "signature" in json.dumps(o) and any(h == "/revoked.json" for h in site.hits),
              f"its lists under its own key, not the project's: not taken ({o.get('state')}, {o.get('error', o.get('err'))})")
        r = post(page, f"plugins_key={open(site.pub, 'rb').read().hex()}\n"); proc.send_signal(signal.SIGUSR2)
        await wait_for(lambda: src()["plugins"]["key"])
        r2 = post(page, "plugins_key=\n")
        check(r2.get("sources") == "applied" and not os.path.exists(os.path.join(state, "sources-forget")),
              "a key set and taken away before the restart: no mark, the floors stay (what this start trusts did not change)")
        await asyncio.sleep(1.5)                        # (the quiet time after that press)
        for f in (".serial", ".listed"): open(os.path.join(state, "plugins", f), "w").write("x\n")
        r = post(page, f"plugins_key={open(site.pub, 'rb').read().hex()}\n")
        proc.send_signal(signal.SIGUSR2)
        check(r.get("sources") == "waiting" and await wait_for(lambda: src()["plugins"]["key"]) and src()["level"] == 2, "its key with the button: level 2")
        check(os.path.exists(os.path.join(state, "sources-forget")) and os.path.exists(os.path.join(state, "plugins", ".serial")),
              "what is trusted changed: marked to be forgotten at the next start (not under the running one)")
        o = await restart_and_look()
        check(not os.path.exists(os.path.join(state, "sources-forget")) and "forgotten" in open(log).read(), "forgotten at the start, the mark gone")
        check(o.get("state") == "ok", f"its lists under the owner's key taken: {o.get('state')} {o.get('error', o.get('err'))}")
        for f in ("index.json.sig", "revoked.json.sig"): os.unlink(os.path.join(site.dir, f))
        r = post(page, "plugins_unchecked=1\n")
        proc.send_signal(signal.SIGUSR2)
        check(r.get("sources") == "waiting" and await wait_for(lambda: src()["plugins"]["unchecked"]), "no check for the plugin index with the button")
        o = await restart_and_look()
        check(o.get("state") == "ok", f"lists without signatures taken: {o.get('state')} {o.get('error', o.get('err'))}")
        r = post(page, "plugins_url=\nplugins_unchecked=0\n")
        check(r.get("sources") == "applied" and src()["level"] == 0, "the project's plugin index again at once")
        check("plugin index is the owner's own" in open(log).read() and "NO SIGNATURE CHECKED" in open(log).read(), "said in the log at start")
        await enc.disconnect()
    finally:
        stop.set(); proc.terminate(); proc.wait(); fj.shutdown(); site.shutdown(); project_index.shutdown()
        if fails: print(open(log).read()[-4000:])
        shutil.rmtree(tmp)
    print("all good" if not fails else f"{fails} FAILED")
    sys.exit(1 if fails else 0)


asyncio.run(main())
