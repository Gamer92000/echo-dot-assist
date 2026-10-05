#!/usr/bin/env python3
"""Online updates (update.c) against build/hassmic-host: this script is Home Assistant (aioesphomeapi), GitHub (releases
API and downloads, over plain http) and root's installer loop (main.sh's ota_watch, same otatool commands).  Bundles are
signed with a throwaway release key, the one hassmic is told to check them against (HASSMIC_RELEASE_PUB)."""
import asyncio, base64, json, os, re, shutil, subprocess, sys, tempfile, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from aioesphomeapi import APIClient, SelectInfo, SelectState, ZERO_NOISE_PSK
from aioesphomeapi.model import UpdateInfo, UpdateState, UpdateCommand

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import Browser
PORT, GH_PORT, WEB = 16963, 16964, 16965
OTATOOL = f"{ROOT}/build/otatool-host"              # the Echo's, for root's installer
PCTOOL = [sys.executable, f"{ROOT}/scripts/otatool.py"]    # the PC's, as CI signs releases
fails = 0


def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    fails += not ok


class GitHub(BaseHTTPRequestHandler):
    """/repos/o/r/releases/latest, /repos/o/r/releases?per_page=10, /dl/<tag>/<asset>.  The list in GitHub's order, which
    is not newest first: the release marked latest comes first (2026-10-02: the release before a newer beta)."""
    releases = []           # newest first, as GitHub lists them
    files = {}              # (tag, asset) -> bytes
    hits = []

    def log_message(self, *a): pass

    def send(self, code, body, ctype="application/json"):
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)

    def do_GET(self):
        GitHub.hits.append(self.path)
        if self.headers.get("User-Agent", "").split("/")[0] != "hassmic": return self.send(403, b'{"message":"no user agent"}')
        if self.path == "/repos/o/r/releases/latest":
            rel = [r for r in GitHub.releases if not r["prerelease"]]
            return self.send(200, json.dumps(rel[0]).encode()) if rel else self.send(404, b'{"message":"Not Found"}')
        if self.path == "/repos/o/r/releases?per_page=10":
            latest = [r for r in GitHub.releases if not r["prerelease"]][:1]
            return self.send(200, json.dumps((latest + [r for r in GitHub.releases if r not in latest])[:10]).encode())
        parts = self.path.split("/")
        if len(parts) == 4 and parts[1] == "dl" and (parts[2], parts[3]) in GitHub.files:
            return self.send(200, GitHub.files[(parts[2], parts[3])], "application/octet-stream")
        self.send(404, b"not found")


def release(tag, pre, body):
    # nested objects carry keys of the same name (author.html_url, assets[].name): only the release's own may count
    return {"url": "x", "html_url": f"https://github.com/o/r/releases/tag/{tag}", "tag_name": tag, "name": tag, "prerelease": pre,
            "draft": False, "author": {"login": "ci", "html_url": "https://github.com/ci"},
            "assets": [{"name": "hassmic-donut.bundle", "tag_name": "nope", "body": "nope"}], "body": body}


def installer(state, pub, stop):
    """main.sh's ota_watch, once per request: verify against the release key, unpack, answer"""
    ota = os.path.join(state, "ota"); n = 0
    while not stop.is_set():
        if os.path.exists(os.path.join(ota, "request")):
            os.unlink(os.path.join(ota, "request")); n += 1
            dest = os.path.join(state, f"installed{n}")
            r = subprocess.run([OTATOOL, "install", pub, f"{ota}/bundle", f"{ota}/bundle.sig", dest], capture_output=True, text=True)
            res = f"OK {r.stdout.strip()}" if r.returncode == 0 else f"FAILED {r.stderr.strip().splitlines()[-1]}"
            open(os.path.join(ota, "result.tmp"), "w").write(res + "\n"); os.rename(os.path.join(ota, "result.tmp"), os.path.join(ota, "result"))
        stop.wait(0.2)


async def wait_for(pred, secs=10):
    for _ in range(int(secs * 10)):
        if pred(): return True
        await asyncio.sleep(0.1)
    return pred()


async def main():
    tmp = tempfile.mkdtemp(); state = os.path.join(tmp, "state"); os.makedirs(state)
    pub, sec = os.path.join(tmp, "release.pub"), os.path.join(tmp, "release.key")
    subprocess.run(PCTOOL + ["keygen", sec, pub], check=True)
    subprocess.run(PCTOOL + ["keygen", sec + ".evil", pub + ".evil"], check=True)
    main_sh = os.path.join(tmp, "main.sh"); open(main_sh, "w").write("#!/bin/sh\necho main\n")
    # as CI tags them: v<commit time, UTC>, and -beta behind it for the beta channel; the bundle has the bare version
    for tag, key in (("v2099.01.02.120000-beta", sec), ("v2099.01.01.093000", sec), ("v2099.01.03.000000-beta", sec + ".evil")):
        out = os.path.join(tmp, f"{tag}.bundle")
        subprocess.run(PCTOOL + ["pack", key, tag[1:].removesuffix("-beta"), out, main_sh], check=True, capture_output=True)
        GitHub.files[(tag, "hassmic-donut.bundle")] = open(out, "rb").read()
        GitHub.files[(tag, "hassmic-donut.bundle.sig")] = open(out + ".sig", "rb").read()
    GitHub.releases = [release("v2099.01.02.120000-beta", True, "Release candidate über \"main\".\n" + "x" * 400), release("v2099.01.01.093000", False, "A release.")]
    gh = ThreadingHTTPServer(("127.0.0.1", GH_PORT), GitHub); threading.Thread(target=gh.serve_forever, daemon=True).start()
    stop = threading.Event(); threading.Thread(target=installer, args=(state, pub, stop), daemon=True).start()

    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_SETTINGS=os.path.join(state, "settings"),
               HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(tmp, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(tmp, "hassmic.service"), HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_MODELS=os.path.join(state, "models"), HASSMIC_ADB_OPEN=os.path.join(state, "adb-open.root"),
               HASSMIC_RELEASE_PUB=pub, HASSMIC_UPDATE_API=f"http://127.0.0.1:{GH_PORT}/repos/o/r",
               HASSMIC_UPDATE_DOWNLOAD=f"http://127.0.0.1:{GH_PORT}/dl")
    start = lambda: subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-L", "-W", str(WEB)], env=env,
                                     stderr=open(os.path.join(tmp, "log"), "a"))
    proc = start(); await asyncio.sleep(0.5)
    try:
        cli = APIClient("127.0.0.1", PORT, None); await cli.connect(login=True)
        ents, _ = await cli.list_entities_services(); by = {e.object_id: e for e in ents}
        upd = by.get("firmware")
        page = Browser(WEB)                             # the channel is picked on the settings page
        check(page.login_with_button(proc), "settings page: logged in with the action button")
        chan = lambda: next((x["choices"][x["value"]] for x in page.state()["settings"] if x["name"] == "online_updates"), None)
        pick = lambda c: page.set(online_updates=c)
        opts = next((x["choices"] for x in page.state()["settings"] if x["name"] == "online_updates"), None)
        check("online_updates" not in by and opts == ["off", "beta", "release"], f"'Online updates' on the settings page, not in Home Assistant: {opts}")
        check(isinstance(upd, UpdateInfo) and upd.device_class == "firmware", "update entity, device class firmware")
        states = []; cli.subscribe_states(states.append)
        last = lambda cls, key: ([s for s in states if isinstance(s, cls) and s.key == key] or [None])[-1]
        await wait_for(lambda: last(UpdateState, upd.key))
        u = last(UpdateState, upd.key)
        check(chan() == "off", "off by default")
        check(u and re.fullmatch(r"20\d\d\.\d\d\.\d\d\.\d{6}\+\w+(-dirty)?", u.current_version) and u.latest_version == u.current_version and not u.in_progress,
              f"while off: latest = current = {u and u.current_version}, nothing to install")

        # anybody on the LAN: must not pick where this Echo's software comes from (a browser not approved), nor install (unkeyed)
        Browser(WEB).set(online_updates="beta"); cli.update_command(upd.key, UpdateCommand.INSTALL); await asyncio.sleep(1)
        check(chan() == "off" and not GitHub.hits and not last(UpdateState, upd.key).in_progress,
              "channel change from a browser not approved and install without the key refused, nothing fetched")

        key = base64.b64encode(os.urandom(32))
        prov = APIClient("127.0.0.1", PORT, None, noise_psk=ZERO_NOISE_PSK); await prov.connect()
        await prov.noise_encryption_set_key(key); await prov.disconnect()
        enc = APIClient("127.0.0.1", PORT, None, noise_psk=key.decode()); await enc.connect(login=True)
        states.clear(); enc.subscribe_states(states.append); await asyncio.sleep(0.3)

        pick("beta")
        check(await wait_for(lambda: last(UpdateState, upd.key) and last(UpdateState, upd.key).latest_version == "2099.01.02.120000"),
              "beta: newest of all, the beta; its version without the tag's -beta")
        u = last(UpdateState, upd.key)
        check(u.release_url.endswith("/tag/v2099.01.02.120000-beta") and u.release_summary.startswith("Release candidate über \"main\".")
              and len(u.release_summary) <= 255 and u.release_summary.endswith("...") and u.title == "hassmic",
              f"release page, notes unescaped and cut to {len(u.release_summary)} characters")
        check("/repos/o/r/releases?per_page=10" in GitHub.hits, "beta looks through the list, which GitHub starts with the release")
        check("online_updates=beta" in open(os.path.join(state, "config")).read(), "channel saved (state/config)")

        pick("release")
        check(await wait_for(lambda: last(UpdateState, upd.key).latest_version == "2099.01.01.093000"), "release: the newest that is not a prerelease")
        # a release newer than every beta is the newest on beta too
        GitHub.releases.insert(0, release("v2099.01.05.000000", False, "newer release"))
        pick("beta")
        check(await wait_for(lambda: last(UpdateState, upd.key).latest_version == "2099.01.05.000000"), "beta: a release newer than the betas")
        GitHub.releases.pop(0)

        # install from beta; this script's installer stands in for root
        pick("beta"); await wait_for(lambda: last(UpdateState, upd.key).latest_version == "2099.01.02.120000")
        states.clear()
        enc.update_command(upd.key, UpdateCommand.INSTALL)
        check(await wait_for(lambda: "installed" in (last(UpdateState, upd.key) or UpdateState()).release_summary, 15),
              f"install: {(last(UpdateState, upd.key) or UpdateState()).release_summary!r}")
        prog = [s for s in states if isinstance(s, UpdateState) and s.key == upd.key]
        check(any(s.in_progress and s.has_progress for s in prog) and prog[-1].progress == 100, f"progress shown: {[round(s.progress) for s in prog]}")
        check(open(os.path.join(state, "installed1", "VERSION")).read() == "2099.01.02.120000", "installer got the beta's bundle and verified it with the release key")
        check(any(h == "/dl/v2099.01.02.120000-beta/hassmic-donut.bundle" for h in GitHub.hits), "this model's bundle (board codename) downloaded")

        # a bundle signed with another key never reaches root
        GitHub.releases.insert(0, release("v2099.01.03.000000-beta", True, "evil"))
        enc.update_command(upd.key, UpdateCommand.CHECK)
        await wait_for(lambda: last(UpdateState, upd.key).latest_version == "2099.01.03.000000")
        enc.update_command(upd.key, UpdateCommand.INSTALL)
        check(await wait_for(lambda: "release key" in last(UpdateState, upd.key).release_summary and not last(UpdateState, upd.key).in_progress, 15),
              f"wrong key: {last(UpdateState, upd.key).release_summary!r}")
        check(not os.path.exists(os.path.join(state, "installed2")) and not os.path.exists(os.path.join(state, "ota", "request")), "nothing handed to root")

        # no release yet: a clear word, not an error code
        GitHub.releases = [r for r in GitHub.releases if r["prerelease"]]
        pick("release")
        check(await wait_for(lambda: "no release published yet" in last(UpdateState, upd.key).release_summary), "release channel, nothing released: said so")

        pick("off"); await asyncio.sleep(0.5)
        check("online_updates=off" in open(os.path.join(state, "config")).read(), "off again, saved")

        # the saved channel survives a restart
        pick("release"); await asyncio.sleep(0.5); await enc.disconnect()
        proc.terminate(); proc.wait(); proc = start(); await asyncio.sleep(0.5)
        enc = APIClient("127.0.0.1", PORT, None, noise_psk=key.decode()); await enc.connect(login=True)
        states.clear(); enc.subscribe_states(states.append)
        check(await wait_for(lambda: chan() == "release"), "channel restored after a restart")
        await enc.disconnect()                          # cli: closed by the device once the key was set
    finally:
        stop.set(); proc.terminate(); proc.wait(); gh.shutdown()
        if fails: print(open(os.path.join(tmp, "log")).read()[-3000:])
        shutil.rmtree(tmp)
    print("all good" if not fails else f"{fails} FAILED")
    sys.exit(1 if fails else 0)


asyncio.run(main())
