#!/usr/bin/env python3
"""Plugins on the PC (docs/plugins.md): build/hassmic-host under build/plugrun-host, with a stand-in wake word plugin
(tests/plugins/testwake.c: its wake word is a burst of sound in testdata/alexa_espeak.raw), the settings page as a
stand-in browser (tests/webclient.py) and Home Assistant (aioesphomeapi).

Packages: what is refused before anything is kept (a changed program, a signature that does not hold, a program that
is not static, `wake` with a way out, a version not newer), where a package comes from (a listing by the index key,
none), the action button for what was not accepted before and none for an update that asks for nothing new.
An update against what was accepted, parameter by parameter: narrower without the button, wider with it and named;
the author key's fingerprint.  The watchdog: a plugin that does not read its control socket, one that takes more CPU
than it may, by what the launcher reads of it (not the wake engine's).  hassmic's own crashes: the mark a fatal signal
leaves and SIGKILL does not; one that names no plugin not counted, three of them switching nothing off; one that names
a plugin counted and the plugin held back, the wake engine's tried again by itself; three and none but the wake
engine's until switched on again.
Settings and files: values, a secret sealed and never sent back, an item checked by the plugin, one it refuses, fields.
The plugin as the wake word engine: its words in Home Assistant, a run from its detection, detections that cannot be
dropped; and the rule for when it does not listen: installed and down, no wake word at all; missing, Amazon's."""
import asyncio, hashlib, io, json, os, resource, signal, subprocess, sys, tarfile, tempfile, time
from aioesphomeapi import APIClient
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser, req

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk"))
import plugcheck
PORT, WEB = 16991, 16992


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def start(state, log, extra=(), env=None, launcher_fd=None):
    """hassmic under build/plugrun-host; or, LAUNCHER_FD given, alone with that descriptor as its launcher's (a stand-in
    that says nothing, for one)"""
    e = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
             HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(state, "models"),
             HASSMIC_FAKE_WHISPER="1", HASSMIC_LOG=log, HASSMIC_CLOCK_SYNCED="0", HASSMIC_PLUGINS_PUB=os.path.join(state, "..", "index.pub"),
             HASSMIC_PLUGIN_HEARD="6", HASSMIC_PLUGIN_RETRY="3000",
             # (browsers log in one after the other here: web.c keeps others back 0.3 s after a press, not 30)
             HASSMIC_LOGIN_MS="60000:300")
    e.update(env or {})                         # (what a caller sets wins over the defaults above)
    if launcher_fd is not None: e["HASSMIC_PLUGIN_FD"] = str(launcher_fd)
    return subprocess.Popen(([] if launcher_fd is not None else [f"{ROOT}/build/plugrun-host", "-L", "--"]) +
                            [os.environ.get("HASSMIC_BIN", f"{ROOT}/build/hassmic-host"), "-P", "esphome", "-p", str(PORT), "-n", "Echo Plug",
                             "-L", "-z", "0", "-o", "0", "-a", "0", "-W", str(WEB), *extra], env=e, stderr=open(log, "a"),
                            pass_fds=() if launcher_fd is None else (launcher_fd,),
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))     # (one run ends in SIGSEGV: no core of it)


def logged(log, text, n0=0, secs=8):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if text in open(log, errors="replace").read()[n0:]: return True
        time.sleep(0.05)
    return False


def plugins(b):
    st, _, body = b.call("GET", "/api/plugins")
    return json.loads(body) if st == 200 else {}


def plug(b, pid="testwake"):
    return next((p for p in plugins(b).get("plugins", []) if p["id"] == pid), None)


def until(f, secs=8):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        v = f()
        if v: return v
        time.sleep(0.1)
    return f()


def post(b, path, body=b""):
    st, _, data = b.call("POST", path, body if isinstance(body, bytes) else body.encode())
    try: return st, json.loads(data)
    except ValueError: return st, {}


def upload(b, pkg):
    """the page's way: begin, pieces in order, the Echo's check"""
    st, r = post(b, "/api/plugins/upload/begin", str(len(pkg)))
    if st != 200: return st, r
    for off in range(0, len(pkg), 200000):
        st, r = post(b, f"/api/plugins/upload/{off}", pkg[off:off + 200000])
        if st != 200: return st, r
    return post(b, "/api/plugins/check")


def repack(pkg, change):
    """a package with its members changed: change(name, data) -> data, or None to drop the member"""
    out = io.BytesIO()
    with tarfile.open(fileobj=io.BytesIO(pkg)) as t, tarfile.open(fileobj=out, mode="w", format=tarfile.USTAR_FORMAT) as o:
        for m in t.getmembers():
            d = change(m.name, t.extractfile(m).read())
            if d is None: continue
            ti = tarfile.TarInfo(m.name); ti.size = len(d); o.addfile(ti, io.BytesIO(d))
    return out.getvalue()


def item(b, name, files, pid="testwake", lst="models"):
    st, r = post(b, f"/api/plugins/{pid}/item/{lst}/{name}/begin")
    if st != 200: return st, r
    for suffix, data in files.items():
        st, r = post(b, f"/api/plugins/{pid}/item/{lst}/{name}/chunk/{suffix}/0", data)
        if st != 200: return st, r
    return post(b, f"/api/plugins/{pid}/item/{lst}/{name}/done")


def plugin_pids(state):
    """the launcher's children that run a program of this state's plugins folder"""
    out = []
    for p in os.listdir("/proc"):
        if not p.isdigit(): continue
        try:
            if os.readlink(f"/proc/{p}/exe").startswith(os.path.join(state, "plugins")): out.append(int(p))
        except OSError: pass
    return out


async def ha_config():
    ha = APIClient("127.0.0.1", PORT, None); await ha.connect(login=True)
    await ha.list_entities_services()
    cfg = await ha.get_voice_assistant_configuration(5)
    return ha, sorted((w.id, w.wake_word) for w in cfg.available_wake_words), list(cfg.active_wake_words)


async def main():
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log")
    # (community plugins on, as the Plugins page's switch leaves them: this test installs plugins from a file; the switch itself: fake_plugins_community.py)
    open(os.path.join(state, "config"), "a").write("plugins_community=on\n")
    cfg = lambda: dict(l.strip().split("=", 1) for l in open(os.path.join(state, "config")) if "=" in l and not l.startswith("#"))
    # the keys: an author's, the plugin index's (its public half is what the Echo has), a stranger's
    for k in ("author", "index", "stranger"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    man = json.load(open(f"{ROOT}/tests/plugins/testwake.json"))
    def package(official=True, key="author", **changes):
        m = dict(man, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, "p.hmp")
        # as the two repositories make it (docs/plugins.md 5.2, 5.3): the author's package, a sideload as it is, and
        # for an official one the listing the plugin index adds to that very package
        plugcheck.pack(mp, f"{ROOT}/build/testwake", os.path.join(top, key + ".key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        if official:
            plugcheck.list_package(out, os.path.join(top, "index.key"), os.path.join(top, "o.hmp"), arch="x86_64")
            out = os.path.join(top, "o.hmp")
        return open(out, "rb").read()
    pkg = package()

    proc = start(state, log)
    try:
        wait_port(PORT, WEB)
        b = Browser(WEB)
        check(b.login_with_button(proc), "logged in with the button")
        b2 = Browser(WEB)
        check(b2.login_with_button(proc), "a second browser, approved too")
        p = plugins(b)
        check(p.get("launcher") is True and p.get("plugins") == [] and p.get("available") is False and "switched off" in p.get("why", ""),
              f"the launcher is there, the sandbox holds, and plugins are off until switched on: {p}")

        # ---- what is refused before anything is kept
        st, r = upload(b, repack(pkg, lambda n, d: d[:-1] + bytes([d[-1] ^ 1]) if n == "plugin" else d))
        check(st == 400 and "not the one the manifest names" in r.get("error", ""), f"a changed program: {r}")
        st, r = upload(b, repack(pkg, lambda n, d: d.replace(b"Test wake word", b"Best wake word") if n == "manifest.json" else d))
        check(st == 400 and "not signed by the author key" in r.get("error", ""), f"a changed manifest: {r}")
        st, r = upload(b, repack(pkg, lambda n, d: open("/bin/true", "rb").read() if n == "plugin" else d))
        check(st == 400, f"another program in the package: {r}")
        dyn = os.path.join(top, "dyn"); subprocess.run(["cc", "-x", "c", "-", "-o", dyn], input=b"int main(void){return 0;}", check=True)
        check("interpreter" in (plugcheck.elf_check(open(dyn, "rb").read(), "x86_64") or ""), "plugcheck: a dynamic program is told so")
        side = os.path.join(top, "side.hmp"); open(side, "wb").write(package(official=False, name="Not looked at"))
        r = subprocess.run([sys.executable, f"{ROOT}/sdk/plugcheck.py", "list", side, os.path.join(top, "index.key"), os.path.join(top, "l.hmp"), "--tier", "community", "--serial", "7", "--arch", "x86_64"],
                           capture_output=True, text=True)
        r2 = subprocess.run([sys.executable, f"{ROOT}/sdk/plugcheck.py", "show", os.path.join(top, "l.hmp"), "--index-pub", os.path.join(top, "index.pub")], capture_output=True, text=True)
        r3 = subprocess.run([sys.executable, f"{ROOT}/sdk/plugcheck.py", "list", os.path.join(top, "l.hmp"), os.path.join(top, "index.key"), os.path.join(top, "no.hmp"), "--arch", "x86_64"],
                            capture_output=True, text=True)
        check(r.returncode == 0 and "listing: good, community, index serial 7" in r2.stdout and r3.returncode == 1 and "carries a listing already" in r3.stderr
              and not os.path.exists(os.path.join(top, "no.hmp")),
              f"plugcheck list: the index key's listing, which show verifies; never a second one: {r2.stdout.strip()[-80:]} {r3.stderr.strip()[:120]}")
        st, r = upload(b, package(capabilities={"wake": True, "files": 1, "net.lan": [{"proto": "tcp", "ports": "any"}]}))
        check(st == 400 and "way out" in r.get("error", ""), f"wake with a way out: sealed, whoever signed it: {r}")
        st, r = upload(b, package(capabilities={"wake": True, "files": 1, "teleport": True}))
        check(st == 400 and "does not know" in r.get("error", ""), f"a capability this version does not know: {r}")
        st, r = upload(b, repack(pkg, lambda n, d: d[:10] + (b"0" if d[10:11] != b"0" else b"1") + d[11:] if n == "listing.sig" else d))
        check(st == 400 and "plugin index's key" in r.get("error", ""), f"a listing whose signature does not hold: {r}")
        other = os.path.join(top, "other.hmp"); mp = os.path.join(top, "m.json"); json.dump(man, open(mp, "w"))
        plugcheck.pack(mp, f"{ROOT}/build/testwake", os.path.join(top, "author.key"), other, listed=os.path.join(top, "stranger.key"), arch="x86_64")
        st, r = upload(b, open(other, "rb").read())
        check(st == 400 and "plugin index's key" in r.get("error", ""), f"a listing by another key: {r}")
        st, r = upload(b, pkg + b"x" * 600)
        check(st == 200, "bytes after the tar's end do not matter")
        st, r = upload(b, repack(pkg, lambda n, d: d) + b"")
        st2, r2 = post(b, "/api/plugins/upload/begin", str(8 << 20))
        check(st2 == 400 and "at most" in r2.get("error", ""), f"a package over the limit: {r2}")
        # sizes and offsets as numbers of at most 9 digits, nothing after them (a review of 6631974, 7: atol() took
        # "4096x" as 4096, and %ld numbers past what a 32-bit Echo's long holds)
        st2, r2 = post(b, "/api/plugins/upload/begin", "4096x")
        st3, r3 = post(b, "/api/plugins/upload/begin", "4096")
        st4, r4 = post(b, "/api/plugins/upload/1234567890", b"x")
        st5, r5 = post(b, "/api/plugins/upload/123456789", b"x")
        post(b, "/api/plugins/cancel")
        check(st2 == 400 and st3 == 200 and st4 == 400 and "upload is at" not in r4.get("error", "") and st5 == 400 and "upload is at" in r5.get("error", ""),
              f"a size that is not only digits refused, a piece's offset of 10 digits is no piece (one of 9 is): {r2} {r3} {r4} {r5}")
        # an id that is a word of the page's routes (upload, chunk, install ...): refused, as by the index (a review of 6631974, 2)
        for word in ("upload", "chunk", "install"):
            st, r = upload(b, package(id=word))
            check(st == 400 and "settings page's words" in r.get("error", ""), f"a plugin called {word!r}: refused: {r}")
        check(not os.path.exists(os.path.join(state, "plugins", "testwake")), "nothing of all that was kept")

        # ---- a sideload and an official package; the button
        st, r = upload(b, package(official=False))
        check(st == 200 and r.get("tier") == "sideload" and r.get("button") is True, f"no listing: a sideload: {r}")
        fp = hashlib.blake2b(open(os.path.join(top, "author.pub"), "rb").read(), digest_size=32).hexdigest()
        fp = " ".join(fp[i:i + 4] for i in range(0, 16, 4))
        check(r.get("key") == fp and plugcheck.fingerprint(open(os.path.join(top, "author.pub"), "rb").read()) == fp,
              f"with its author key's fingerprint for the page to name, the one plugcheck prints: {r.get('key')}")
        check(r.get("update") is False and r.get("changes") == [], "a new plugin: nothing to hold it against, all of it is accepted")
        st, r = upload(b, pkg)
        lines = [c["line"] for c in r.get("caps", [])]
        check(st == 200 and r.get("tier") == "official" and r.get("button") is True and r.get("name") == "Test wake word", f"the index key's listing: official: {r}")
        check(any("microphone all the time" in l for l in lines) and any("Reads the files you give it" in l and "Wake word models" in l and "1 MB" in l for l in lines),
              f"one line per capability, with its parameters: {lines}")
        # (beside them the words of every widening version required, for the dialog: none, every scope at version 1;
        # plugin_store.c ps_widens_json, held to a later hassmic's versions by make unit-plugin_store's demo)
        check(r.get("widens") == [], f"\"widens\": every scope required at version 1, no version's words: {r.get('widens')}")
        # an install is one browser's: another one's package does not get under this one's dialog and the owner's press
        twin = package(official=False, name="Evil twin")
        # (while the first one's dialog is open the slot is its own: before the seventh read the second browser's
        # upload took it at once, and the first was told at its install; tests/fake_plugins_race.py has what
        # happens once a dialog was left for so long that another may take the slot)
        st2, r2 = upload(b2, twin)
        st3, r3 = post(b2, "/api/plugins/check"); st4, r4 = post(b2, "/api/plugins/install")
        check(st2 == 400 and "installing a plugin just now" in r2.get("error", "") and st3 == 400 and st4 == 400 and "another browser" in r4.get("error", "")
              and not os.path.exists(os.path.join(state, "plugins", "testwake")),
              f"a second browser's package while the first one's dialog is open: not uploaded, not checked, not installed: {r2} {r4}")
        post(b2, "/api/plugins/cancel")
        st, r = upload(b, pkg)
        st, r = post(b, "/api/plugins/install")
        check(st == 200 and r.get("install") == "waiting" and not os.path.exists(os.path.join(state, "plugins", "testwake")), f"a new plugin waits for the action button: {r}")
        st2, r2 = post(b2, "/api/plugins/cancel")
        check(st2 == 400 and plugins(b2).get("install", {}).get("state") == "none" and plugins(b).get("install", {}).get("state") == "waiting",
              f"the wait is the first browser's: the second neither sees nor cancels it: {r2}")
        check(logged(log, "waits for the action button"), "said in the log")
        # the press is for the package the page listed: nothing else gets under it meanwhile
        other = package(official=False, name="Something else")
        st1, r1 = post(b, "/api/plugins/upload/begin", str(len(other))); st2, r2 = post(b, "/api/plugins/upload/0", other[:1000]); st3, r3 = post(b, "/api/plugins/check")
        check(st1 == 400 and st2 == 400 and st3 == 400 and "waits for the action button" in r3.get("error", ""), f"while it waits, no other package can be uploaded or checked: {r1} {r2} {r3}")
        proc.send_signal(signal.SIGUSR2)
        check(until(lambda: plugins(b).get("install", {}).get("state") == "installed"), f"the press installs it: {plugins(b).get('install')}")
        p = plug(b)
        check(p and p["tier"] == "official" and p["accepted"] and p["enabled"] and p["how"] == "button" and p["wake"] and p["state"] == "stopped",
              f"installed, accepted with the button, not running while plugins are off: {p and {k: p[k] for k in ('tier', 'accepted', 'how', 'state', 'detail')}}")
        check(p and p.get("widens") == [] and len(p.get("caps", [])) == 2, f"its card: a line per capability, and \"widens\" beside them (none at version 1): {p and p.get('widens')}")
        acc = open(os.path.join(state, "plugins", "testwake", "accepted")).read()
        check("how button" in acc and "key " + open(os.path.join(top, "author.pub"), "rb").read().hex() in acc, "what was accepted is written down, with the author's key")
        check(os.path.exists(os.path.join(state, "plugins", "testwake", "pkg", "plugin")) and "requires" in json.load(open(os.path.join(state, "plugins", "testwake", "pkg", "manifest.json"))), "the package is kept in pkg/, its manifest with what it requires")

        st, r = upload(b, pkg)
        check(st == 400 and "not newer" in r.get("error", ""), f"the same version again: {r}")
        st, r = upload(b, package(version=2, key="stranger"))
        check(st == 400 and "another author" in r.get("error", ""), f"an update signed by another author's key: {r}")
        st, r = upload(b, package(version=2, capabilities={"wake": True, "files": 2}))
        ch = r.get("changes", [])
        check(st == 200 and r.get("button") is True and r.get("update") is True and len(ch) == 1 and ch[0]["kind"] == "more" and "up to 2 MB), was 1 MB" in ch[0]["line"],
              f"an update that asks for more waits for the button, and says what is more: {ch}")
        post(b, "/api/plugins/cancel")

        # ---- switched on: it runs, in its sandbox
        n0 = len(open(log).read())
        r = b.set(plugins="on")
        check(r.get("applied") == 1 and cfg().get("plugins") == "on", f"the feature switched on: {r}")
        # (plugins other than the wake engine's start 10 s after hassmic, or once its self test passed: none can be why an update fell back)
        check(logged(log, "testwake: running", n0, 15) and until(lambda: (plug(b) or {}).get("state") == "running"), f"the plugin runs: {(plug(b) or {}).get('state')}")
        check(logged(log, "plugin testwake: ready on Echo Plug", n0), "its lines are in the log, under its name")
        pids = plugin_pids(state)
        check(len(pids) == 1 and os.getsid(pids[0]) == pids[0] and os.readlink(f"/proc/{pids[0]}/fd/0") == "/dev/null", f"one process, a session of its own, no stdin: {pids}")
        fds = sorted(os.listdir(f"/proc/{pids[0]}/fd")) if pids else []
        check(fds == ["0", "1", "2", "3"], f"its descriptors: /dev/null twice, its log, the control socket: {fds}")

        # ---- settings: the owner's to write; a secret sealed and never sent back
        n0 = len(open(log).read())
        st, r = post(b, "/api/plugins/testwake/set", "note=hello there")
        check(st == 200 and logged(log, "plugin testwake: setting note=hello there", n0), f"a setting reaches the plugin: {r}")
        st, r = post(b, "/api/plugins/testwake/set", "mode=sideways")
        check(st == 400 and "choices" in r.get("error", ""), f"a value that is not one of a select's: {r}")
        st, r = post(b, "/api/plugins/testwake/set", "volume=3")
        check(st == 400, f"a setting the manifest does not have: {r}")
        n0 = len(open(log).read())
        st, _, body = b.call("POST", "/api/plugins/testwake/secret/pin", lambda c: b.seal(c, b"1234".ljust(16, b"\0")).hex().encode())
        check(st == 200 and logged(log, "plugin testwake: setting pin=(set)", n0), "a secret, sealed for this Echo, reaches the plugin")
        raw = b.call("GET", "/api/plugins")[2]
        s = {x["name"]: x for x in plug(b)["settings"]}
        check(b"1234" not in raw and s["pin"].get("set") is True and "value" not in s["pin"] and s["note"]["value"] == "hello there" and s["mode"]["value"] == "listen",
              f"the page gets every value but the secret: {s['pin']}")
        check(oct(os.stat(os.path.join(state, "plugins", "testwake", "config")).st_mode & 0o777) == "0o600", "the settings' file is the daemon's alone")

        # ---- files: an item the plugin checks, one it refuses
        st, r = item(b, "burst", {".model": b"MODEL Burst of sound!\n"})
        it = {x["id"]: x for x in s["models"]["items"]} if False else {x["id"]: x for x in {y["name"]: y for y in plug(b)["settings"]}["models"]["items"]}
        check(st == 200 and r.get("checked") is True and it.get("burst", {}).get("title") == "Burst of sound" and it["burst"]["checked"],
              f"an upload the plugin takes: kept, named by the plugin (letters, digits and spaces only): {r} {it.get('burst')}")
        check(it["burst"]["fields"].get("level") == "2000", f"the plugin's start value of a field: {it['burst']['fields']}")
        st, r = item(b, "junk", {".model": b"JUNK\n"})
        check(st == 400 and "not a model" in r.get("error", "") and not os.path.exists(os.path.join(state, "plugins", "testwake", "files", "models", "junk")),
              f"one it refuses: not kept, the plugin's reason shown: {r}")
        st, r = item(b, "nomodel", {".json": b"{}"})
        check(st == 400, f"an item without its required file: {r}")
        st, r = item(b, "Bad.Name", {".model": b"MODEL x\n"})
        check(st in (400, 404), f"a name that is no item id: {st}")
        st, r = post(b, "/api/plugins/testwake/item/models/burst/chunk/.exe/0", b"x")
        check(st == 400 and "takes no" in r.get("error", ""), f"a suffix the manifest does not name: {r}")
        st, r = post(b, "/api/plugins/testwake/item/mode/scratch/delete")
        st2, r2 = post(b, "/api/plugins/testwake/item/nosuch/scratch/begin")
        check(st == 400 and st2 == 400, f"an item of a setting that is no file list: refused, nothing to remove: {r} {r2}")
        st, r = item(b, "names", {".model": "MODEL Café 東京のウェイクワードモデルです\n".encode() + b"MODEL \xff\xe3\x81\n"})
        raw = b.call("GET", "/api/plugins")[2]
        t = {x["id"]: x for x in {y["name"]: y for y in json.loads(raw.decode("utf-8"))["plugins"][0]["settings"]}["models"]["items"]}.get("names", {}).get("title")
        check(st == 200 and t == "Café 東京のウェイクワードモデルです", f"a title in other scripts stays whole characters, and what is no character is left out: {t!r}")
        post(b, "/api/plugins/testwake/item/models/names/delete")
        st, r = item(b, "wide", {".model": ("MODEL " + "ж" * 32 + "\n").encode()})
        t = {x["id"]: x for x in {y["name"]: y for y in plug(b)["settings"]}["models"]["items"]}.get("wide", {}).get("title")
        check(st == 200 and t == "ж" * 31, f"a title of 64 bytes ends on a whole character (what carries it on holds 63): {t!r}")
        post(b, "/api/plugins/testwake/item/models/wide/delete")
        st, r = item(b, "big", {".model": b"MODEL Big\n" + b"x" * 20000})
        check(st == 400 and "at most" in r.get("error", ""), f"a file over its size: {r}")
        n0 = len(open(log).read())
        st, r = post(b, "/api/plugins/testwake/item/models/burst/field", "level=3000")
        check(st == 200 and logged(log, "field level=3000", n0), f"a field the owner edits reaches the plugin: {r}")
        st, r = post(b, "/api/plugins/testwake/item/models/burst/field", "level=5")
        check(st == 400, f"a field out of its range: {r}")
        st, _, body = b.call("GET", "/api/plugins/testwake/file/models/burst/.model")
        check(st == 200 and body == b"MODEL Burst of sound!\n", "a file of an item comes back as uploaded")
        st, _, body = b.call("GET", "/api/plugins/testwake/file/models/burst/..%2f..%2fconfig")
        check(st in (400, 404), "no other file through that")
        cat = {y["name"]: y for y in plug(b)["settings"]}["models"].get("catalog", {})
        check(cat.get("items", [{}])[0].get("id") == "burst", f"the manifest's catalog is handed to the page as data: {cat}")

        # ---- the plugin as the wake word engine
        ha, avail, active = await ha_config()
        check(("burst", "Burst of sound") not in avail, f"Amazon's engine: Home Assistant lists its wake words: {avail}")
        ents, _ = await ha.list_entities_services()
        check(not [e for e in ents if e.object_id == "wake_word_engine"], "no \"Wake word engine\" sensor while the engine is hassmic's own")
        try: await ha.disconnect()
        except Exception: pass
        n0 = len(open(log).read())
        r = b.set(wake_engine="plugin:testwake")
        check(r.get("applied") == 1 and cfg().get("wake_engine") == "plugin:testwake" and logged(log, "engine now plugin:testwake", n0), f"the engine switched to the plugin, saved: {r}")
        check(logged(log, "\"burst\" listens", n0), "its word is loaded")
        ha, avail, active = await ha_config()
        check(avail == [("burst", "Burst of sound")] and active == ["burst"], f"Home Assistant lists the plugin's words: {avail} {active}")
        ents, _ = await ha.list_entities_services()
        eng = [e for e in ents if e.object_id == "wake_word_engine"]
        check(len(eng) == 1 and eng[0].device_class == "problem", f"the problem sensor \"Wake word engine\" is listed while the engine is a plugin: {eng}")
        problem = []
        ha.subscribe_states(lambda st: problem.append(st.state) if eng and st.key == eng[0].key else None)
        started = asyncio.Event(); phrases = []
        async def handle_start(conv_id, flags, settings, phrase): phrases.append(phrase); started.set(); return 0
        async def nothing(*a, **k): pass
        ha.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        try: await asyncio.wait_for(started.wait(), 20)
        except asyncio.TimeoutError: pass
        check(started.is_set() and phrases[-1] == "Burst of sound" and logged(log, "wake: burst (plugin testwake, score 200"),
              f"the burst in the audio, heard by the plugin: a run for Home Assistant, named by the word: {phrases}")
        check(plugins(b).get("wake_problem") == 0 and plug(b)["listening"], "no problem to report")
        check(problem and problem[-1] is False, f"and none in Home Assistant: {problem[-3:]}")

        # ---- detections that cannot be: dropped
        for mode, what in (("future", "ending ahead of the stream"), ("late", "2 s old")):
            n0 = len(open(log).read())
            post(b, "/api/plugins/testwake/set", f"mode={mode}")
            check(logged(log, "a detection that cannot be", n0, 25), f"a detection {what}: dropped")
        n0 = len(open(log).read())
        post(b, "/api/plugins/testwake/set", "mode=burst")
        check(logged(log, "taken as broken", n0, 30), "one for every block: one a second at most, and after six in ten minutes (the tests' twenty) the engine is taken as broken")
        post(b, "/api/plugins/testwake/set", "mode=listen")
        check(until(lambda: plugins(b).get("wake_problem") == 0 and (plug(b) or {}).get("listening"), 12), "started again, it listens")
        n0 = len(open(log).read())
        post(b, "/api/plugins/testwake/set", "mode=relist")
        check(logged(log, "its list of wake words keeps changing: taken as broken", n0, 20), "a list of wake words that changes with every block: taken as broken, not told to Home Assistant each time")
        check(open(log).read()[n0:].count("links closed") <= 12, "(Home Assistant was not sent away once per list)")
        post(b, "/api/plugins/testwake/set", "mode=listen")
        check(until(lambda: plugins(b).get("wake_problem") == 0 and (plug(b) or {}).get("listening"), 12), "started again, it listens")
        n0 = len(open(log).read())
        post(b, "/api/plugins/testwake/set", "mode=closed")
        check(logged(log, "it closed its control socket", n0, 5), "a plugin that closes its control socket and sleeps: ended at once")
        post(b, "/api/plugins/testwake/set", "mode=listen")

        # ---- installed and down: no wake word, and never Amazon's
        check(until(lambda: plugins(b).get("wake_problem") == 2, 5), f"the page says that no wake word is heard: {plugins(b).get('wake_why')}")
        st = b.state()
        check(any(w.startswith("The wake word engine is the plugin testwake") and "No wake word is heard" in w for w in st.get("warnings", [])), f"as a warning for every page: {st.get('warnings')}")
        await asyncio.sleep(0.3)
        check(problem and problem[-1] is True, f"and Home Assistant's sensor is on: {problem[-3:]}")
        n0 = len(open(log).read())
        proc.send_signal(signal.SIGUSR1); await asyncio.sleep(0.5)
        check("Amazon's wake word listens" not in open(log).read(), "Amazon's wake word does not stand in for a plugin that is installed")
        # (its third end in ten minutes: started again after 30 s)
        check(until(lambda: plugins(b).get("wake_problem") == 0 and (plug(b) or {}).get("listening"), 45), "started again, it listens again by itself")
        pid = plugin_pids(state)[0]; n0 = len(open(log).read())
        os.kill(pid, signal.SIGKILL)
        check(logged(log, "testwake: gone (killed by signal 9", n0) and logged(log, "started again in", n0), "killed: seen, started again")
        check(until(lambda: plugin_pids(state) and plugin_pids(state)[0] != pid and (plug(b) or {}).get("listening"), 12), "and its word listens again")
        try: await ha.disconnect()
        except Exception: pass

        # ---- a word that does not load, one that never answers
        n0 = len(open(log).read())
        item(b, "broken", {".model": b"MODEL Broken\nBROKEN\n"}); item(b, "slow", {".model": b"MODEL Slow\nSLOW\n"})
        logged(log, 'wake word: slow "Slow"', n0, 6); await asyncio.sleep(0.5)     # a new list reaches the core 2 s apart at most
        ha, avail, active = await ha_config()
        check([i for i, _ in avail] == ["broken", "burst", "slow"], f"three words now: {avail}")
        n0 = len(open(log).read())
        await ha.set_voice_assistant_configuration(["broken"]); 
        check(logged(log, "cannot listen for broken: the model cannot be read", n0) and until(lambda: plugins(b).get("wake_problem") == 2, 5),
              f"a model the plugin cannot load: said, nothing listens, no Alexa: {plugins(b).get('wake_why')}")
        n0 = len(open(log).read())
        await ha.set_voice_assistant_configuration(["slow"])
        check(logged(log, "it did not answer for slow", n0, 9), "a plugin that does not answer within 5 s: the slot failed")
        await ha.set_voice_assistant_configuration(["burst"])
        check(until(lambda: plugins(b).get("wake_problem") == 0, 15), "back to a word that loads: listening")
        try: await ha.disconnect()
        except Exception: pass

        # a signed write is taken once, also over a restart: its counter is written to the flash, but for a piece of an
        # upload (web.c piece_route).  Which one is a piece goes by the whole route: by strstr, a path with "/chunk/" or
        # one under "/api/plugins/upload/" anywhere was one, never kept, and taken again after a restart (a review of
        # 6631974, 2: a plugin named "chunk" or "upload", a list or item named "chunk")
        caught = []
        for path in ("/api/plugins/testwake/item/chunk/x/delete", "/api/plugins/testwake/item/models/chunk/delete", "/api/plugins/upload/remove"):
            hd = b.headers("POST", path, b"remove"); st, _, _ = req(WEB, "POST", path, b"remove", hd); caught.append((path, hd, st))

        # ---- a start with the plugin as the engine: up before the engine opens
        proc.send_signal(signal.SIGTERM); proc.wait(5)
        check(until(lambda: not plugin_pids(state), 5), "hassmic gone: its plugins too")
        n0 = len(open(log).read())
        proc = start(state, log); wait_port(PORT, WEB)
        again = [(path, st, req(WEB, "POST", path, b"remove", hd)[0]) for path, hd, st in caught]
        check(all(st != 401 and st2 == 401 for _, st, st2 in again),
              f"writes whose path has \"/chunk/\" or \"upload/\" in it and are no piece of an upload: taken once, and not again after a restart: {again}")
        text = open(log, errors="replace").read()[n0:]
        check("\"burst\" listens" in text and text.index("\"burst\" listens") < text.index(f"esphome on {PORT}"), "at the next start the plugin listens before hassmic's ports open")
        check("Amazon's" not in text, "and Amazon's engine was not opened in between")
        b = Browser(WEB, b.sk)
        check(plugins(b).get("wake_problem") == 0, "no problem after the start")

        # ---- an update that asks for nothing new: no button
        st, r = upload(b, package(version=2))
        check(st == 200 and r.get("button") is False, f"version 2 with the same capabilities: no button needed: {r}")
        n0 = len(open(log).read())
        st, r = post(b, "/api/plugins/install")
        check(st == 200 and r.get("install") == "installed" and until(lambda: (plug(b) or {}).get("version") == 2 and plug(b)["listening"], 12),
              f"installed at once, running, listening again: {r} {(plug(b) or {}).get('state')}")

        # ---- switched off, removed: missing, and Amazon's wake word stands in
        n0 = len(open(log).read())
        st, r = post(b, "/api/plugins/testwake/enable", "off")
        check(st == 200 and until(lambda: (plug(b) or {}).get("state") == "stopped") and until(lambda: plugins(b).get("wake_problem") == 2, 5), "switched off by the owner: it stops, nothing listens")
        post(b, "/api/plugins/testwake/enable", "on")
        check(until(lambda: plugins(b).get("wake_problem") == 0, 12), "switched on: listening")
        n0 = len(open(log).read())
        st, r = post(b, "/api/plugins/testwake/remove", "remove")
        check(st == 200 and not os.path.exists(os.path.join(state, "plugins", "testwake")) and plug(b) is None, f"removed, with its files: {r}")
        check(logged(log, "Amazon's wake word listens until it is here", n0) and until(lambda: plugins(b).get("wake_problem") == 1, 5),
              f"the engine's plugin is missing: Amazon's wake word stands in, and the page says so: {plugins(b).get('wake_why')}")
        check(cfg().get("wake_engine") == "plugin:testwake", "the setting keeps what the owner chose")
        ha, avail, active = await ha_config()
        check(avail and all(i != "burst" for i, _ in avail), f"Home Assistant lists Amazon's wake words meanwhile: {avail}")
        try: await ha.disconnect()
        except Exception: pass
        n0 = len(open(log).read())
        st, r = upload(b, pkg); st2, r2 = post(b, "/api/plugins/install")
        proc.send_signal(signal.SIGUSR2)
        check(st == 200 and r.get("button") is True and until(lambda: (plug(b) or {}).get("state") == "running", 10), "installed again (the button again: it was removed)")
        item(b, "burst", {".model": b"MODEL Burst\n"})
        check(until(lambda: plugins(b).get("wake_problem") == 0, 15) and logged(log, "\"burst\" listens", n0, 5), "and the Echo goes back to the plugin by itself")

        # ---- five crashes in ten minutes switch a plugin off; not the wake engine's
        b.set(wake_engine="amazon")
        item(b, "crash", {".model": b"MODEL Crash\n"})
        check((plug(b) or {}).get("state") == "running", "an ordinary plugin now")
        for i in range(5):
            pid = until(lambda: (plugin_pids(state) or [0])[0], 40)
            if pid: os.kill(pid, signal.SIGKILL)
            until(lambda: pid not in plugin_pids(state), 5)
            if i < 4: until(lambda: plugin_pids(state), 40)
        check(until(lambda: (plug(b) or {}).get("state") == "crashed", 5) and logged(log, "five times in ten minutes: switched off"), f"five crashes: switched off: {(plug(b) or {}).get('state')}")
        post(b, "/api/plugins/testwake/enable", "on")
        check(until(lambda: (plug(b) or {}).get("state") == "running", 8), "the owner switches it on again")

        # ---- an update against what was accepted (3.3, 5.4): narrower needs no button, wider is named
        st, r = upload(b, package(version=2, capabilities={"wake": True, "files": 2})); post(b, "/api/plugins/install")
        proc.send_signal(signal.SIGUSR2)
        check(r.get("button") is True and until(lambda: (plug(b) or {}).get("version") == 2 and plug(b)["files_mb"] == 2, 10), "version 2 with 2 MB of files: accepted with the button")
        st, r = upload(b, package(version=3))
        check(st == 200 and r.get("button") is False and r.get("update") is True and r.get("changes") == [], f"version 3 with 1 MB again: narrower, no button: {r}")
        st, r = post(b, "/api/plugins/install")
        check(r.get("install") == "installed" and until(lambda: (plug(b) or {}).get("version") == 3 and plug(b)["accepted"] and plug(b)["files_mb"] == 1, 10),
              f"installed at once, and what is accepted is what it has now: {r}")
        st, r = upload(b, package(version=4, capabilities={"wake": True, "files": 2}))
        check(r.get("button") is True and "was 1 MB" in (r.get("changes") or [{}])[0].get("line", ""), f"2 MB after that is more again: {r.get('changes')}")
        post(b, "/api/plugins/cancel")
        st, r = upload(b, package(version=4, settings=man["settings"] + [{"name": "password", "label": "Home Assistant password", "type": "secret"}]))
        ch = r.get("changes", [])
        check(r.get("button") is True and len(ch) == 1 and ch[0]["kind"] == "new" and 'Asks you for "Home Assistant password" (a secret)' in ch[0]["line"], f"a new row that asks for a secret: the button: {ch}")
        post(b, "/api/plugins/cancel")
        st, r = upload(b, package(version=4, name="Best wake word"))
        ch = r.get("changes", [])
        check(r.get("button") is True and len(ch) == 1 and ch[0]["kind"] == "changed" and "Best wake word" in ch[0]["line"], f"another name: the button: {ch}")
        post(b, "/api/plugins/cancel")
        st, r = upload(b, package(version=4, official=False))
        # (where it comes from is part of what was accepted: a step down, official to a file, asks; a review, 2026-11-21)
        check(st == 200 and r.get("button") is True and r.get("tier") == "sideload" and r.get("key") == fp
              and [c["line"] for c in r.get("changes", [])] == ["Comes from a file, not listed; the version installed here came from the plugin index, official"],
              f"nothing changed but its listing gone: a step down from official, the button, and said: {r.get('changes')}")
        post(b, "/api/plugins/cancel")
        # (a row is a row: a switch more is asked for as a secret more is; a review, 2026-11-19)
        st, r = upload(b, package(version=4, settings=man["settings"] + [{"name": "quiet", "label": "Quiet", "type": "bool"}]))
        ch = r.get("changes", [])
        check(r.get("button") is True and len(ch) == 1 and ch[0]["kind"] == "new" and '"Quiet" (a switch)' in ch[0]["line"], f"one more switch: the button: {ch}")
        post(b, "/api/plugins/cancel")
        st, r = upload(b, package(version=4, settings=[dict(x, help="Type your Home Assistant password here") if x["type"] != "files" else x for x in man["settings"]]))
        ch = r.get("changes", [])
        check(r.get("button") is True and ch and all(c["kind"] == "changed" and "no longer as you accepted it" in c["line"] for c in ch), f"what the rows say about themselves rewritten: the button: {ch}")
        post(b, "/api/plugins/cancel")
        st, r = upload(b, package(version=4, memory_mb=64))
        ch = r.get("changes", [])
        check(r.get("button") is False and len(ch) == 1 and ch[0]["kind"] == "limit" and "64 MB of memory" in ch[0]["line"], f"more memory: no button, and a line that says so: {ch}")
        post(b, "/api/plugins/cancel")
        check(until(lambda: (plug(b) or {}).get("state") == "running", 10), "it runs")

        # ---- the watchdog: a plugin that does not read what hassmic sends it
        n0 = len(open(log).read())
        post(b, "/api/plugins/testwake/set", "mode=stuck")
        # (its queue holds what the socket's send buffer does: settings sent until that is full twice over)
        for i in range(int(open("/proc/sys/net/core/wmem_default").read()) // 1000 + 100):
            post(b, "/api/plugins/testwake/set", "note=" + "x" * 400 + str(i))
            if i % 50 == 49 and "does not read what hassmic sends it" in open(log, errors="replace").read()[n0:]: break
        check(logged(log, "testwake: it does not read what hassmic sends it (its queue full for 5 s): ended", n0, 12), "its control socket's queue full for 5 s: ended")
        post(b, "/api/plugins/testwake/set", "mode=listen")
        check(logged(log, "it does not read what hassmic sends it; started again in", n0, 8) and until(lambda: (plug(b) or {}).get("state") == "running", 12), "counted as a crash: started again")

        # ---- the watchdog: CPU (here a quarter of a core over 2 s; a full core over a minute on an Echo)
        proc.send_signal(signal.SIGTERM); proc.wait(5)
        proc = start(state, log, env={"HASSMIC_PLUGIN_CPU": "25:2000"}); wait_port(PORT, WEB); b = Browser(WEB, b.sk)
        check(until(lambda: (plug(b) or {}).get("state") == "running", 20), "an ordinary plugin, running")
        time.sleep(2.5)
        check((plug(b) or {}).get("state") == "running" and "more CPU" not in open(log, errors="replace").read(), "one that waits is left alone")
        n0 = len(open(log).read())
        post(b, "/api/plugins/testwake/set", "mode=spin")
        check(logged(log, "testwake: it used more CPU than a plugin may", n0, 10), "3 s of nothing but CPU: ended")
        post(b, "/api/plugins/testwake/set", "mode=listen")
        check(logged(log, "started again in", n0, 8) and until(lambda: (plug(b) or {}).get("state") == "running", 12), "counted as a crash: started again")
        b.set(wake_engine="plugin:testwake")
        check(until(lambda: (plug(b) or {}).get("listening"), 12), "the wake word engine's plugin now")
        n0 = len(open(log).read())
        post(b, "/api/plugins/testwake/set", "mode=spin"); time.sleep(6)
        check("more CPU" not in open(log, errors="replace").read()[n0:] and (plug(b) or {}).get("state") == "running", "the same 3 s from the engine's plugin: never ended for working")
        post(b, "/api/plugins/testwake/set", "mode=listen"); time.sleep(3.5)

        # ---- hassmic's own crashes (2.6): only a fatal signal leaves a mark
        fall, fallsf = os.path.join(state, "plugins", ".fall"), os.path.join(state, "plugins", ".falls")
        def again(env=None):
            n = len(open(log).read()); pr = start(state, log, env=env); wait_port(PORT, WEB)
            return pr, Browser(WEB, b.sk), n
        proc.kill(); proc.wait(5)
        check(os.path.exists(fall) and os.path.getsize(fall) == 0, "hassmic ended by SIGKILL, as init's stop does: no mark")
        proc, b, n0 = again()
        check("ended in a crash" not in open(log, errors="replace").read()[n0:] and plugins(b).get("falls") == 0 and until(lambda: (plug(b) or {}).get("listening"), 10),
              f"the next start counts nothing: {plugins(b).get('falls')}")
        proc.send_signal(signal.SIGSEGV); rc = proc.wait(5)
        check(rc == -signal.SIGSEGV and open(fall).read() == "fall -\n", f"SIGSEGV: hassmic ends by it as before, and leaves the mark: {rc} {open(fall).read()!r}")
        proc, b, n0 = again()
        check(logged(log, "hassmic's last run ended in a crash, not in a plugin's message: not counted", n0, 3) and plugins(b).get("falls") == 0 and open(fall).read() == "" and not os.path.exists(fallsf),      # (made again, empty, as its plugin started)
              f"the next start says so, takes the mark away and counts nothing against the plugins: {plugins(b).get('falls')}")
        check(until(lambda: (plug(b) or {}).get("listening"), 10) and plugins(b).get("falls_off") is False, "plugins start as before")
        for i in range(2):
            proc.send_signal(signal.SIGTERM); proc.wait(5)
            if i == 0: check(os.path.getsize(fall) == 0, "SIGTERM: no mark")
            open(fall, "w").write("fall -\n")
            proc, b, n0 = again()
        check(plugins(b).get("falls") == 0 and plugins(b).get("falls_off") is False and not os.path.exists(fallsf) and until(lambda: (plug(b) or {}).get("listening"), 10),
              f"three crashes that name no plugin: nothing counted, nothing switched off: {plugins(b).get('falls')}")
        # a mark that names the wake word engine's plugin: held back, and tried again by itself (HASSMIC_PLUGIN_RETRY: 3 s; 5 min on an Echo)
        proc.send_signal(signal.SIGTERM); proc.wait(5)
        open(fall, "w").write("fall testwake\n")       # as the handler writes it when the broker's thread fell in this plugin's message (unit-plugin_broker)
        proc, b, n0 = again()
        p = plug(b) or {}
        check(plugins(b).get("falls") == 1 and open(fallsf).read() == "1\n" and logged(log, "ended in a crash while it read a message of testwake: 1 of 3", n0, 3), f"a mark that names a plugin is counted: {plugins(b).get('falls')}")
        check(p.get("state") == "restarting" and "tried again" in p.get("detail", "") and not plugin_pids(state) and logged(log, "testwake is the wake word engine's plugin: tried again in 3 s", n0, 3),
              f"the wake word engine's plugin: held back, with the reason on its card: {p.get('state')} {p.get('detail')}")
        check(until(lambda: plugins(b).get("wake_problem") == 2, 3), "nothing listens meanwhile, and the page says so")
        check(until(lambda: (plug(b) or {}).get("listening"), 12) and plugins(b).get("wake_problem") == 0, "and it comes back by itself: nobody had to find the page")
        # an ordinary plugin the mark names: not in this run
        b.set(wake_engine="amazon")
        proc.send_signal(signal.SIGTERM); proc.wait(5)
        open(fall, "w").write("fall testwake\n")
        proc, b, n0 = again()
        check(until(lambda: "held back for this run" in (plug(b) or {}).get("detail", ""), 20) and plugins(b).get("falls") == 2 and plug(b).get("state") == "stopped" and not plugin_pids(state),
              f"an ordinary plugin the mark names is held back for this run: {(plug(b) or {}).get('detail')}")
        time.sleep(4)
        check(not plugin_pids(state), "and not tried again by itself")
        post(b, "/api/plugins/testwake/enable", "off"); post(b, "/api/plugins/testwake/enable", "on")
        check(until(lambda: (plug(b) or {}).get("state") == "running", 12), "the owner's own switch, off and on: started")
        proc.send_signal(signal.SIGTERM); proc.wait(5)
        open(fall, "w").write("fall testwake\n")
        proc, b, n0 = again()
        check(plugins(b).get("falls") == 3 and plugins(b).get("falls_off") is True and logged(log, "none but the wake word engine's starts", n0, 3), f"the third in a plugin's message: plugins are off: {plugins(b).get('falls')}")
        b.set(wake_engine="plugin:testwake")
        check(until(lambda: (plug(b) or {}).get("listening"), 15), "the wake word engine's plugin starts all the same (after its 3 s: the mark named it)")
        b.set(wake_engine="amazon")
        post(b, "/api/plugins/testwake/enable", "off"); until(lambda: (plug(b) or {}).get("state") == "stopped", 5); post(b, "/api/plugins/testwake/enable", "on")
        time.sleep(1); p = plug(b) or {}
        check(p.get("state") == "stopped" and "stay off until you switch them on again" in p.get("detail", "") and not plugin_pids(state), f"an ordinary plugin does not: {p.get('state')} {p.get('detail')}")
        st, r = post(b, "/api/plugins/resume")
        check(st == 200 and plugins(b).get("falls") == 0 and plugins(b).get("falls_off") is False and until(lambda: not os.path.exists(fallsf), 3), f"switched on again from the page: the count is gone: {r}")
        check(until(lambda: (plug(b) or {}).get("state") == "running", 20), "and the plugin starts")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()
    if check.failed:
        print("---- log"); print(open(log, errors="replace").read()[-6000:])
    print("plugins: " + ("FAILED" if check.failed else "all passed"))
    sys.exit(1 if check.failed else 0)

if __name__ == "__main__": asyncio.run(main())
