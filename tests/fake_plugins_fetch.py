#!/usr/bin/env python3
"""Plugins from the plugin index (plugin_fetch.c, plugin_index.c, web.c "packages the Echo brings itself", ota.c
"HMOTA-PLUG1"; docs/plugins.md 5.3 to 5.6, 5.9): build/hassmic-host under build/plugrun-host, this script as a browser
on the settings page (tests/webclient.py), as the index's site (tests/plugsite.py: built by hassmic-plugins' own
tools/index.py where that tree is beside this one, signed with a stand-in index key whose public half the Echo has)
and as the PC that pushes.  Packages as tests/fake_plugins.py makes them: the author's, listed by the index's build.

The list: nothing asked of the site until the page says look, and then four files and nothing that names the Echo;
both signatures, the index's and the revocation list's; a serial lower than one seen, of either list, refused; more
than 32 plugins (60) all listed; per plugin the version this build installs (the highest whose requires this build
meets) and the highest there is, with what of its requires this build lacks.  The fetch: checked as an upload, the button for a new plugin, none for an update that asks
for nothing new, the button for one that asks for more; one at a time, not beside an install that waits, cancelled;
downloaded from <site>/packages/<id>-<version>.hmp whatever the list's path says.  Refused, each with its reason and
nothing installed: the site out of reach, a package whose size or SHA-256 is not the list's, one listed by another
key, one that is another version than the list names, one the build withdrew, one that needs a scope this build lacks
(HASSMIC_PLUGIN_SCOPES: storage and net.lan lowered), one the list does not have.  Revocations: an installed plugin the newest list
withdraws is stopped, kept, and its card says why; such a plugin, and an author key's, refused at install; an
official one is not touched.  The push: an update of what was accepted goes in, anything else comes back with why.
The Echo fetches nothing by itself for an update of hassmic: one never breaks a plugin (sdk/PROTOCOL.md "Versions")."""
import hashlib, json, os, re, resource, signal, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser, req
import plugsite
from plugsite import plugcheck

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, WEB, GH, OTA = 16881, 16882, 16883, 16884
PCTOOL = [sys.executable, f"{ROOT}/scripts/otatool.py"]


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond: check.failed = True
check.failed = False


def logged(log, text, n0=0, secs=8):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if text in open(log, errors="replace").read()[n0:]: return True
        time.sleep(0.05)
    return False


def until(f, secs=8):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        v = f()
        if v: return v
        time.sleep(0.05)
    return f()


def get(b, path):
    st, _, body = b.call("GET", path)
    try: return json.loads(body) if st == 200 else {}
    except ValueError: return {}


def post(b, path, body=b""):
    st, _, data = b.call("POST", path, body if isinstance(body, bytes) else body.encode())
    try: return st, json.loads(data)
    except ValueError: return st, {}


def plug(b, pid="testwake"):
    return next((p for p in get(b, "/api/plugins").get("plugins", []) if p["id"] == pid), None)


def upload(b, pkg):
    st, r = post(b, "/api/plugins/upload/begin", str(len(pkg)))
    if st != 200: return st, r
    for off in range(0, len(pkg), 200000):
        st, r = post(b, f"/api/plugins/upload/{off}", pkg[off:off + 200000])
        if st != 200: return st, r
    return post(b, "/api/plugins/check")


def look(b):
    """the page's "Look again": the Echo asks the index, in a thread of its own"""
    post(b, "/api/plugins/index", "again")
    return until(lambda: (lambda o: o if o.get("state") not in ("looking", None) else None)(get(b, "/api/plugins/index")), 20) or {}


def fetch(b, pid, secs=20):
    """the page's "Install"/"Update": asked, then polled until the Echo has checked the package or says why not"""
    st, r = post(b, "/api/plugins/fetch", pid)
    if st != 200: return {"state": "refused", "error": r.get("error", "")}
    return until(lambda: (lambda s: s if s.get("state") != "fetching" else None)(get(b, "/api/plugins/fetch")), secs) or {}


def main():
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log")
    # (community plugins on, as the Plugins page's switch leaves them: this test installs plugins from a file; the switch itself: fake_plugins_community.py)
    open(os.path.join(state, "config"), "a").write("plugins_community=on\n")
    for k in ("author", "stranger", "update", "evil", "wrongindex", "newindex"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    site = plugsite.Site(top, GH)
    man = json.load(open(f"{ROOT}/tests/plugins/testwake.json"))
    made = [0]
    def package(key="author", program=f"{ROOT}/build/testwake", listed=False, **changes):
        """as the plugin's repository releases it: the author's, no listing (LISTED: the index's listing added, as a
        package from the site has it, for an upload or a push)"""
        made[0] += 1
        m = dict(man, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, f"p{made[0]}.hmp")
        if program is None: program = os.path.join(top, "tiny"); open(program, "wb").write(plugsite.tiny_elf())
        plugcheck.pack(mp, program, os.path.join(top, key + ".key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        if listed:
            plugcheck.list_package(out, site.key, out + ".l", arch="x86_64"); out += ".l"
        return open(out, "rb").read()
    def entry(pid):
        return next(p for p in json.load(open(os.path.join(site.dir, "index.json")))["plugins"] if p["id"] == pid)
    # 60 more plugins than the one installed: a list longer than the releases' 32 ever held
    fillers = {f"plug{i:02d}": [package(program=None, id=f"plug{i:02d}", name=f"Plugin {i}", description=f"Stand-in number {i}.")] for i in range(60)}
    others = {"newapi": [package(program=None, id="newapi", name="New API", capabilities={"files": 1, "storage": 4})],
              "needy": [package(program=None, id="needy", name="Needy", capabilities={"files": 1, "net.lan": [{"proto": "tcp", "ports": [80]}]})],
              "oldplug": [package(program=None, id="oldplug", name="Old", version=3)]}

    def start(fresh="3600:0", **extra):
        # (HASSMIC_PLUGINS_BUILT: the build's own time, from which the floor of a serial taken with the clock not set
        # is bounded, is now, not this tree's last commit of any age; HASSMIC_PLUGIN_DAILY: the daily look and the
        # members' announcements out of the way of the requests counted, but where a check asks for them)
        # (HASSMIC_PLUGIN_SCOPES: a build without net.lan and storage, for "needy" and "newapi", as an older one: every
        # build has every scope)
        # (HASSMIC_PLUGIN_RELIST: a listing that does not hold read for an hour from now, where a check does not ask for it)
        extra = {"HASSMIC_PLUGINS_BUILT": str(int(time.time())), "HASSMIC_PLUGIN_DAILY": "3600000:86400000:3600000", "HASSMIC_PLUGIN_RELIST": "3600000:3600000",
                 "HASSMIC_PLUGIN_SCOPES": "net.lan:0,storage:0", **extra}
        e = dict(os.environ, HASSMIC_LOGIN_MS="60000:300",     # (browsers log in one after the other here: web.c keeps others back 0.3 s after a press, not 30)
                 HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
                 HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(state, "models"),
                 HASSMIC_FAKE_WHISPER="1", HASSMIC_LOG=log, HASSMIC_CLOCK_SYNCED="0", HASSMIC_PLUGINS_PUB=site.pub, HASSMIC_PLUGIN_INDEX=site.base,
                 HASSMIC_UPDATE_PUB=os.path.join(top, "update.pub"), HASSMIC_ADB_OPEN=os.path.join(state, "adb-open.root"),
                 # (online updates ask "GitHub" here, and are answered 404)
                 HASSMIC_UPDATE_API=f"{site.base}repos/o/r", HASSMIC_UPDATE_DOWNLOAD=f"{site.base}dl",
                 HASSMIC_PLUGIN_FRESH=fresh, HASSMIC_PLUGIN_LEAST="oldplug:5", HASSMIC_PLUGIN_RETRY="3000")
        e.update({k: v for k, v in extra.items() if v is not None})      # (over the defaults above: HASSMIC_PLUGINS_PUB too)
        for k, v in extra.items():
            if v is None: e.pop(k, None)        # (None: as the environment has it, or not at all: HASSMIC_CLOCK_SYNCED set)
        return subprocess.Popen([f"{ROOT}/build/plugrun-host", "-L", "--", os.environ.get("HASSMIC_BIN", f"{ROOT}/build/hassmic-host"), "-P", "esphome", "-p", str(PORT), "-n", "Echo Fetch",
                                 "-L", "-z", "0", "-o", str(OTA), "-a", "0", "-W", str(WEB)], env=e, stderr=open(log, "a"),
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    installed = lambda pid="testwake": os.path.exists(os.path.join(state, "plugins", pid, "pkg", "manifest.json"))
    version = lambda: (plug(b) or {}).get("version")
    asked = lambda: [h for h in site.hits if not h.startswith("/repos/")]
    dls = site.downloads
    listed = lambda o, pid: next((p for p in o.get("plugins", []) if p["id"] == pid), None)

    proc = start()
    try:
        wait_port(PORT, WEB, OTA)
        print(f"     (the site built by {'the index tool, ' + plugsite.TOOL if site.used_tool else 'tests/plugsite.py alone: no hassmic-plugins tree beside this one'})")
        b = Browser(WEB); check(b.login_with_button(proc), "logged in with the button")
        b2 = Browser(WEB); check(b2.login_with_button(proc), "a second browser, approved too")

        # ---- nothing is asked of the index until the owner asks
        o = get(b, "/api/plugins/index"); get(b, "/api/plugins")
        time.sleep(1.5)
        check(o.get("state") == "never" and o.get("plugins") == [] and o.get("scopes", {}).get("core") == 1 and o["scopes"].get("net.lan") == 0 and o["scopes"].get("storage") == 0 and not site.hits, f"the list before anyone looked: empty, and no request for it: {o} {site.hits}")
        st, _, _ = Browser(WEB).call("POST", "/api/plugins/index", b"look")
        st2, _, _ = Browser(WEB).call("POST", "/api/plugins/fetch", b"testwake")
        st3, _, _ = Browser(WEB).call("GET", "/api/plugins/index")
        time.sleep(0.5)
        check(st == 401 and st2 == 401 and st3 == 401 and not site.hits, "a browser that is not approved neither reads the list nor makes the Echo ask anything")

        # ---- the index out of reach
        site.down = True
        o = look(b)
        check(o.get("state") == "unreachable" and o.get("plugins") == [] and o.get("error"), f"the index out of reach: the list says so: {o}")
        f = fetch(b, "testwake")
        check(f.get("state") == "failed" and "cannot reach the plugin index" in f.get("error", "") and not installed(), f"and so does a fetch: {f}")
        site.down = False

        # ---- the list: four files, signed; 63 plugins; what this build installs of each
        v1 = package()
        serial = int(time.time()) - 3000
        site.build({"testwake": [v1], **fillers, **others}, serial=serial)
        site.hits.clear()
        o = look(b)
        check(o.get("state") == "ok" and len(o.get("plugins", [])) == 64 and not o.get("dropped") and o.get("serial") == serial,
              f"the list: all 64 plugins, none left out, under the index's serial: {o.get('state')} {o.get('error')} {len(o.get('plugins', []))} {o.get('dropped')}")
        check(sorted(site.hits) == ["/index.json", "/index.json.sig", "/revoked.json", "/revoked.json.sig"],
              f"four requests, for the site's own files: nothing that names this Echo, its version or its plugins: {site.hits}")
        check(site.agents and set(site.agents) == {"hassmic"}, f"each saying \"hassmic\" and no version as what asks (it was hassmic/<version>+<commit>): {sorted(set(site.agents))}")
        t = listed(o, "testwake")
        check(t and t["here"]["version"] == 1 and t["top"]["version"] == 1 and t["tier"] == "official" and t["name"] == "Test wake word" and t["author"] == "hassmic's tests"
              and t["here"]["caps"] == ["files", "wake"] and t["key"] == plugcheck.fingerprint(open(os.path.join(top, "author.pub"), "rb").read()),
              f"a plugin: its name, author, tier, the author key's fingerprint, the version this hassmic would install and what it asks for: {t}")
        n, nd = listed(o, "newapi"), listed(o, "needy")
        check(n and n["here"] is None and n["top"]["needs"] == ["storage 1"] and n["top"]["requires"] == {"core": 1, "files": 1, "storage": 1} and nd and nd["here"] is None and nd["top"]["needs"] == ["net.lan 1"],
              f"one that needs a scope this build lacks (storage), one that needs another (net.lan): nothing to install here, and why: {n} {nd}")
        check(listed(o, "oldplug")["here"] is None and listed(o, "plug59")["here"]["version"] == 1, "one under the build's least version: not installed here either; the 60th filler: listed like the first")
        seen = os.path.join(state, "plugins", ".listed")
        check(os.path.exists(seen) and "testwake 1\n" in open(seen).read() and "plug59 1\n" in open(seen).read() and open(os.path.join(state, "plugins", ".serial")).read() == f"index {serial} {serial}\n",
              "the newest version of each plugin seen listed is kept on file (state/plugins/.listed), and the index's serial (.serial)")
        n = len(site.hits); get(b, "/api/plugins/index"); get(b, "/api/plugins/index")
        check(len(site.hits) == n, "reading the list asks the index nothing")

        # ---- signatures, and serials that only grow
        keep = open(os.path.join(site.dir, "index.json"), "rb").read()
        site.override["index.json"] = keep.replace(b"Stand-in number 7.", b"Stand-in number 8.")
        o = look(b)
        check(o.get("state") == "failed" and "signature does not hold" in o.get("error", "") and o.get("plugins") == [], f"an index changed after it was signed: refused, nothing of it taken: {o.get('error')}")
        site.override.clear()
        site.resign("index.json", keep, key=os.path.join(top, "wrongindex.key"))
        o = look(b)
        check(o.get("state") == "failed" and "signature does not hold" in o.get("error", ""), f"an index signed by another key: refused: {o.get('error')}")
        site.resign("index.json", keep)
        rk = open(os.path.join(site.dir, "revoked.json"), "rb").read()
        site.override["revoked.json"] = rk.replace(b'"entries": []', b'"entries": [ ]')
        o = look(b)
        check(o.get("state") == "failed" and "revocation list's signature" in o.get("error", "") and o.get("plugins") == [], f"a revocation list changed after it was signed: the look fails, the index is not taken without it: {o.get('error')}")
        site.override.clear()
        check(look(b).get("state") == "ok", "(as signed: taken again)")
        older = site.dir
        site.build({"testwake": [v1]}, serial=serial - 1000)
        o = look(b)
        check(o.get("state") == "failed" and "older list" in o.get("error", "") and str(serial) in o.get("error", ""), f"an index under the serial this Echo has seen (signed by the key, served by whoever can write the site): refused: {o.get('error')}")
        site.dir = older
        check(look(b).get("state") == "ok", "(the newest again: taken)")
        # the index and the revocation list of one time: a newer index (made with revocation list 2, which withdraws
        # plug07) served beside the older list (1) is not taken, and plug07 is not offered (a review, 2026-11-21)
        newer = site.build({"testwake": [v1], **fillers, **others}, serial=serial + 10, revoked=[{"id": "plug07", "reason": "it leaks"}], revoked_serial=2)
        check(json.load(open(os.path.join(newer, "index.json")))["revoked_serial"] == 2 and not listed(json.load(open(os.path.join(newer, "index.json"))), "plug07"),
              "(the index tool names the revocation list it made the index with, and leaves out what that list withdraws)")
        for nme in ("revoked.json", "revoked.json.sig"): site.override[nme] = open(os.path.join(older, nme), "rb").read()
        o = look(b)
        check(o.get("state") == "failed" and "made with a newer revocation list (serial 2)" in o.get("error", "") and o.get("plugins") == [] and o.get("revoked_serial") == 0,
              f"a new index beside an older revocation list (the site's files of two times): refused, nothing offered: {o.get('error')}")
        site.override.clear()
        o = look(b)
        check(o.get("state") == "ok" and o.get("revoked_serial") == 2 and not listed(o, "plug07") and listed(o, "plug06"), "the pair as published: taken, plug07 not offered")
        # a redirect is followed to HTTPS only: one to plain HTTP took whatever answered there (a review, 2026-11-21)
        site.redirect["index.json"] = f"{site.base}elsewhere/index.json"; site.override["elsewhere/index.json"] = open(os.path.join(site.dir, "index.json"), "rb").read()
        site.hits.clear(); o = look(b)
        check(o.get("state") in ("unreachable", "failed") and "/elsewhere/index.json" not in site.hits, f"index.json redirected to plain HTTP: not followed: {o.get('state')} {o.get('error')}")
        site.redirect.clear(); site.override.clear()
        check(look(b).get("state") == "ok", "(without the redirect: taken)")

        # ---- a fetch and an install that waits do not go together, either way
        st, r = upload(b, package(listed=True))
        st, r = post(b, "/api/plugins/install")
        check(st == 200 and r.get("install") == "waiting", f"an uploaded package waits for the action button: {r}")
        f = fetch(b2, "testwake"); f1 = fetch(b, "testwake")
        check(f.get("state") == "refused" and "waits for the action button" in f.get("error", "") and f1.get("state") == "refused" and not dls(),
              f"a fetch while it waits: refused, nothing downloaded: {f}")
        post(b, "/api/plugins/cancel")
        site.slow = 4
        st, r = post(b, "/api/plugins/fetch", "testwake")
        check(st == 200 and r.get("fetch") == "fetching" and until(lambda: dls()), f"a fetch: answered at once, the download in the Echo's own time: {r}")
        st1, r1 = post(b2, "/api/plugins/upload/begin", str(len(v1))); st2, r2 = post(b, "/api/plugins/upload/0", v1[:1000]); st3, r3 = post(b2, "/api/plugins/check")
        st4, r4 = post(b2, "/api/plugins/fetch", "testwake"); st5, r5 = post(b2, "/api/plugins/cancel"); st6, r6 = post(b, "/api/plugins/install")
        check(st1 == 400 and st2 == 400 and st3 == 400 and "being fetched" in r1.get("error", "") and "being fetched" in r3.get("error", ""), f"while it fetches, no upload and no check: {r1} {r3}")
        check(st4 == 400 and st5 == 400 and st6 == 400, f"no second fetch, no cancel by another browser, no install of what is not there yet: {r4} {r5} {r6}")
        check(get(b2, "/api/plugins/fetch").get("state") == "none" and get(b, "/api/plugins/fetch").get("state") == "fetching" and get(b, "/api/plugins").get("install", {}).get("state") == "fetching",
              "the fetch is its browser's: the other one sees none")
        st, r = post(b, "/api/plugins/cancel")
        check(st == 200 and until(lambda: get(b, "/api/plugins/fetch").get("state") == "none") and logged(log, "the fetch of testwake was cancelled") and not installed()
              and not os.path.exists(os.path.join(state, "plugins", ".stage")), f"cancelled by its own browser: the download let go, nothing kept: {r}")
        site.slow = 0

        # ---- fetched, checked as an upload is, the button, installed, running.  Right after a publish: every publish
        # lists every package anew (its listing names the new serial), so its size and SHA-256 are not those of the
        # list this Echo keeps for the hour; the package is not the list's, the Echo looks once more and takes the one
        # the new list names (it failed, for up to the hour after each publish: a review, 2026-11-21)
        site.build({"testwake": [v1], **fillers, **others}, serial=serial + 20, revoked=[{"id": "plug07", "reason": "it leaks"}], revoked_serial=2)
        site.hits.clear(); n0 = len(open(log).read())
        f = fetch(b, "testwake"); c = f.get("check", {})
        check(f.get("state") == "checked" and dls() == ["/packages/testwake-1.hmp"] * 2 and site.hits.count("/index.json") == 1 and logged(log, "looked at again", n0, 1),
              f"published since the list was looked at: the package not the list's, one more look, the new list's package taken: {f.get('state')} {f.get('error')} {site.hits}")
        site.hits.clear()
        f = fetch(b, "testwake"); c = f.get("check", {})
        check(f.get("state") == "checked" and c.get("tier") == "official" and c.get("button") is True and c.get("name") == "Test wake word" and c.get("version") == 1 and c.get("update") is False,
              f"fetched and checked: official, a new plugin, to be accepted with the button: {f.get('state')} {f.get('error')} {({k: c.get(k) for k in ('tier', 'button', 'version', 'update')})}")
        check(any("microphone all the time" in l["line"] for l in c.get("caps", [])) and c.get("key"), "the same answer as an upload's check: a line per capability, the author key's fingerprint")
        check(dls() == ["/packages/testwake-1.hmp"], f"downloaded from the site's packages/, by id and version: {dls()}")
        check(not installed(), "nothing installed by the fetch alone")
        st, r = post(b2, "/api/plugins/install")
        check(st == 400 and not installed(), f"another browser cannot have it installed: {r}")
        st, r = post(b, "/api/plugins/install")
        check(st == 200 and r.get("install") == "waiting" and not installed(), f"the install waits for the action button, as an upload's: {r}")
        proc.send_signal(signal.SIGUSR2)
        check(until(lambda: get(b, "/api/plugins").get("install", {}).get("state") == "installed") and installed(), "the press installs it")
        p = plug(b)
        check(p and p["tier"] == "official" and p["accepted"] and p["how"] == "button" and p["version"] == 1, f"installed, official by the index key's listing, accepted with the button: {p and {k: p[k] for k in ('tier', 'accepted', 'how', 'version')}}")
        n0 = len(open(log).read())
        r = b.set(plugins="on")
        check(r.get("applied") == 1 and logged(log, "testwake: running", n0, 20) and until(lambda: (plug(b) or {}).get("state") == "running"), f"switched on, it runs: {(plug(b) or {}).get('state')}")

        # ---- an update that asks for nothing new: no button.  Beside it one that needs a scope this build lacks,
        # which is not downloaded at all (the list says what it requires); and the list's own path for the package is not followed
        v2 = package(version=2, version_name="0.2")
        site.build({"testwake": [v1, v2, package(version=9, capabilities={"wake": True, "files": 1, "storage": 4})], **fillers})
        ix = json.load(open(os.path.join(site.dir, "index.json")))
        for p in ix["plugins"]:
            for v in p["versions"]: v["package"] = "../../evil/" + v["package"]
        site.resign("index.json", plugsite.dump(ix))
        o = look(b); t = listed(o, "testwake")
        check(t and t["here"]["version"] == 2 and t["top"]["version"] == 9 and t["top"]["needs"] == ["storage 1"] and t["here"]["needs"] == [], f"the list: version 2 for this hassmic, 9 the highest (it needs storage, which this build lacks): {t}")
        check("testwake 2\n" in open(seen).read() and "testwake 9" not in open(seen).read(),
              "the version seen listed (the floor a copy from another Echo is held to) is the highest this build runs, not 9 for the next hassmic (a review, 2026-11-21: it refused every copy here)")
        site.hits.clear()
        f = fetch(b, "testwake"); c = f.get("check", {})
        check(f.get("state") == "checked" and c.get("version") == 2 and c.get("button") is False and c.get("update") is True and c.get("tier") == "official",
              f"the highest version this build runs, an update that is what was accepted: no button: {f.get('state')} {f.get('error')} {({k: c.get(k) for k in ('version', 'button', 'update')})}")
        check(dls() == ["/packages/testwake-2.hmp"] and not any("evil" in h for h in site.hits), f"one download, of this build's version, from the address made of id and version, never the list's path: {site.hits}")
        n0 = len(open(log).read())
        st, r = post(b, "/api/plugins/install")
        check(st == 200 and r.get("install") == "installed" and version() == 2 and logged(log, "testwake: running", n0, 20), f"installed at once, and it runs: {r} {version()}")

        # ---- one that asks for more: the button, and what is more
        site.build({"testwake": [v1, v2, package(version=3, capabilities={"wake": True, "files": 2})]})
        look(b); f = fetch(b, "testwake"); c = f.get("check", {}); ch = c.get("changes", [])
        check(f.get("state") == "checked" and c.get("button") is True and c.get("update") is True and len(ch) == 1 and ch[0]["kind"] == "more",
              f"an update that asks for more waits for the button and says what is more: {ch}")
        st, r = post(b, "/api/plugins/install")
        check(st == 200 and r.get("install") == "waiting" and version() == 2, f"not installed without it: {r}")
        post(b, "/api/plugins/cancel")
        check(version() == 2, "cancelled: the version installed stays")

        # ---- refused, each with its reason, nothing installed
        def refused(what, why, pid="testwake", downloads=None, change=None, **plugins):
            if plugins: site.build({k.replace("_", "-"): v for k, v in plugins.items()})
            if change: change()
            look(b); site.hits.clear()
            f = fetch(b, pid, 40)
            ok = f.get("state") == "failed" and why in f.get("error", "") and version() == 2 and (pid == "testwake" or not os.path.exists(os.path.join(state, "plugins", pid)))
            if downloads is not None: ok = ok and len(dls()) == downloads
            st, r = post(b, "/api/plugins/install")
            check(ok and st == 400 and not os.path.exists(os.path.join(state, "plugins", ".stage")), f"{what}: {f.get('error')!r}")
            site.override.clear()
        v4 = package(version=4)
        site.build({"testwake": [v1, v2, v4]}); d4 = open(os.path.join(site.dir, "packages", "testwake-4.hmp"), "rb").read()
        refused("a package whose bytes are not the list's (the same length)", "SHA-256 differs", downloads=1, change=lambda: site.override.update({"packages/testwake-4.hmp": d4[:-600] + b"\1" + d4[-599:]}))
        refused("a package longer than the list says", "larger than the index says", downloads=1, change=lambda: site.override.update({"packages/testwake-4.hmp": d4 + b"\0" * 512}))
        refused("a package shorter than the list says", "size or its SHA-256 differs", downloads=1, change=lambda: site.override.update({"packages/testwake-4.hmp": d4[:-512]}))
        def as_v5():
            """version 4's package under version 5's name, the list made to name its size and digest, signed"""
            ix = json.load(open(os.path.join(site.dir, "index.json")))
            for p in ix["plugins"]:
                for v in p["versions"]:
                    if p["id"] == "testwake" and v["version"] == 4: v.update(version=5, version_name="0.7")
            site.resign("index.json", plugsite.dump(ix)); site.override["packages/testwake-5.hmp"] = open(os.path.join(site.dir, "packages", "testwake-4.hmp"), "rb").read()
        refused("a package that is another version than the list names", "is testwake version 4 inside: not what the index names", change=as_v5, testwake=[v1, v2, v4])
        site.build({"testwake": [v1, v2, v4]}, key=os.path.join(top, "wrongindex.key"))      # (the lists signed by the index key, the listings by another)
        refused("a package listed by another key than the index's", "not signed by the plugin index's key", downloads=1)
        refused("a version this build has withdrawn (its least version)", "was withdrawn: this hassmic runs it from version 5", pid="oldplug", downloads=0, **others, testwake=[v1, v2])
        refused("only a version that needs a scope this build lacks", "needs a newer hassmic (storage 1)", pid="newapi", downloads=0, **others, testwake=[v1, v2])
        refused("one that needs another", "needs a newer hassmic (net.lan 1)", pid="needy", downloads=0, **others, testwake=[v1, v2])
        refused("a version that is not newer than the one installed", "not newer", downloads=1, testwake=[v1, v2])
        refused("a plugin the index does not list", "does not list nosuch", pid="nosuch", downloads=0, testwake=[v1, v2])
        st, r = post(b, "/api/plugins/fetch", "../../etc"); st2, r2 = post(b, "/api/plugins/fetch", "Testwake")
        check(st == 400 and st2 == 400 and "not a plugin's id" in r.get("error", ""), f"what is no plugin's id is not asked for: {r}")
        site.override["index.json"] = b"<html>a captive portal</html>"
        o = look(b)
        check(o.get("state") == "failed" and o.get("error"), f"an answer that is no index: refused, said why: {o.get('error')}")
        site.override.clear()

        # ---- revocations: what is not official, by id or by author key; an installed one stopped and kept
        side = package(id="sideplug", name="Side")
        st, r = upload(b, side); st, r = post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(until(lambda: get(b, "/api/plugins").get("install", {}).get("state") == "installed") and until(lambda: (plug(b, "sideplug") or {}).get("state") == "running", 20),
              "a sideload, installed with the button: it runs")
        n0 = len(open(log).read())
        site.build({"testwake": [v1, v2]}, revoked=[{"id": "sideplug", "reason": "it sends what it hears to its author"}], revoked_serial=5)
        o = look(b)
        p = until(lambda: (lambda p: p if p and p["state"] != "running" else None)(plug(b, "sideplug")), 15) or {}
        check(o.get("state") == "ok" and o.get("revoked_serial") == 5 and p.get("state") in ("stopped", "stopping") and "the plugin index withdrew it: it sends what it hears to its author" in p.get("detail", "")
              and os.path.exists(os.path.join(state, "plugins", "sideplug", "pkg")), f"withdrawn by the newest list: stopped, kept, and its card says why: {p.get('state')} {p.get('detail')}")
        check(logged(log, "sideplug: stopped: the plugin index withdrew it", n0) and (plug(b) or {}).get("state") == "running", "said in the log; the official plugin runs on")
        st, r = upload(b, package(id="sideplug", name="Side", version=2))
        check(st == 400 and "withdrew this version of sideplug" in r.get("error", ""), f"a newer version of it from a file: refused, and why: {r.get('error')}")
        proc.terminate(); proc.wait(); proc = start(); wait_port(PORT, WEB, OTA)
        p = plug(b, "sideplug") or {}
        check(p.get("state") == "stopped" and "withdrew" in p.get("detail", "") and os.path.exists(os.path.join(state, "plugins", ".revoked"))
              and not os.path.exists(os.path.join(state, "plugins", ".revoked.json")) and p.get("withdrawn") == "it sends what it hears to its author",
              f"over a restart: the list kept on file (one file, list and signature), the plugin not started: {p.get('detail')}")
        stranger = open(os.path.join(top, "stranger.pub"), "rb").read().hex()
        site.build({"testwake": [v1, v2]}, revoked=[{"author_key": stranger, "reason": "the key was taken"}, {"id": "sideplug", "reason": "it sends what it hears to its author"}], revoked_serial=6)
        look(b)
        st, r = upload(b, package(key="stranger", id="otherside", name="Other"))
        time.sleep(1)
        check(st == 400 and "the key was taken" in r.get("error", "") and (plug(b, "sideplug") or {}).get("state") != "running", f"an author key withdrawn: its package refused; what the newer list still withdraws stays stopped: {r.get('error')}")
        st, r = upload(b, package(key="stranger", id="otherside", name="Other", listed=True))
        check(st == 400 and "the key was taken" in r.get("error", ""), f"an official listing is withdrawn by the list too (the index key that made it official may): {st} {r.get('error')}")
        site.build({"testwake": [v1, v2]}, revoked_serial=4)
        o = look(b)
        check(o.get("state") == "ok" and o.get("revoked_serial") == 6 and "older than the one this Echo has" in o.get("note", "") and logged(log, "is older (serial 4) than the one this Echo has (6)"),
              f"a revocation list under the serial kept here: refused and said, the look goes on beside the one kept (the index was made with an older one still; the whole look failed): {o.get('state')} {o.get('error')} {o.get('note')}")
        # a newer list that no longer withdraws it: started again (it stayed stopped until the next start of hassmic)
        n0 = len(open(log).read())
        site.build({"testwake": [v1, v2]}, revoked_serial=7)
        o = look(b)
        check(o.get("revoked_serial") == 7 and logged(log, "sideplug: no longer withdrawn: started again", n0) and until(lambda: (plug(b, "sideplug") or {}).get("state") == "running", 15),
              f"a newer list that names it no more: started again, as it was before: {(plug(b, 'sideplug') or {}).get('state')}")
        # past the bound: the whole list refused (it took the first 2048 and left the 2049th, which named sideplug)
        n0 = len(open(log).read())
        site.build({"testwake": [v1, v2]}, revoked_serial=8)       # (the index tool publishes no such list: signed here by the key's holder)
        site.resign("revoked.json", plugsite.dump({"revoked": 1, "serial": 8, "entries": [{"id": f"gone{i:05d}", "reason": "x"} for i in range(2048)] + [{"id": "sideplug", "reason": "entry 2049"}]}))
        o = look(b)
        check(o.get("state") == "failed" and "refused whole" in o.get("error", "") and o.get("revoked_serial") == 7 and (plug(b, "sideplug") or {}).get("state") == "running",
              f"a revocation list of 2049 entries: refused whole and said, the look fails, nothing taken: {o.get('error')}")

        # ---- an official plugin withdrawn (every tier; a leaked author key of an official plugin had no stop), as
        # the wake word engine: down, its words say what to do (6); run anyway with the button, for that list (12)
        r = b.set(wake_engine="plugin:testwake")         # (it has no wake word here: "not loaded", until it is withdrawn)
        n0 = len(open(log).read())
        site.build({"testwake": [v1, v2]}, revoked=[{"id": "testwake", "reason": "the index found a flaw"}], revoked_serial=10)
        o = look(b)
        p = until(lambda: (lambda p: p if p and p["state"] != "running" else None)(plug(b)), 15) or {}
        pl = get(b, "/api/plugins")
        check(o.get("revoked_serial") == 10 and "withdrew it: the index found a flaw" in p.get("detail", "") and p.get("tier") == "official" and p.get("withdrawn") == "the index found a flaw" and not p.get("anyway"),
              f"an official plugin the list names: stopped, kept, its card says why: {p.get('state')} {p.get('detail')}")
        check(pl.get("wake_problem") == 2 and "withdrew it (the index found a flaw)" in pl.get("wake_why", "") and "choose another wake word engine, or run it anyway" in pl.get("wake_why", "")
              and "until it runs again" not in pl.get("wake_why", "") and not logged(log, "Amazon's wake word listens", n0, 0.5),
              f"as the wake word engine: down (never Amazon's), and the words say what the owner can do: {pl.get('wake_why')}")
        st, r = post(b2, "/api/plugins/testwake/anyway", "run")
        check(st == 200 and r.get("anyway") == "waiting" and r.get("holds") == "anyway" and get(b, "/api/state").get("button", {}).get("holds") == "anyway", f"run it anyway: the Echo waits for its action button: {r}")
        st, r = post(b, "/api/plugins/testwake/anyway", "run"); st2, r2 = post(b, "/api/adb", "on")
        check(r.get("anyway") == "busy" and r2.get("adb") == "busy" and r2.get("holds") == "anyway", f"one wait at a time: another browser's ask, and adb, told busy: {r} {r2}")
        post(b, "/api/plugins/testwake/anyway", "cancel")
        check(get(b, "/api/state").get("button", {}).get("holds") == "anyway", "another browser's cancel ends no wait but its own")
        st, r = post(b2, "/api/plugins/sideplug/anyway", "cancel")
        check(r.get("anyway") == "cancelled" and get(b, "/api/state").get("button", {}).get("holds") == "anyway", f"nor its own cancel for another plugin (it did: a review, 2026-11-21): {r}")
        st, r = post(b, "/api/plugins/sideplug/remove", "remove")
        post(b2, "/api/plugins/testwake/anyway", "cancel")
        check(get(b, "/api/state").get("button", {}).get("holds") == "none" and (plug(b) or {}).get("state") != "running", "its own cancel ends it: nothing runs")
        st, r = post(b2, "/api/plugins/testwake/anyway", "run")
        n0 = len(open(log).read()); proc.send_signal(signal.SIGUSR2)
        p = until(lambda: (lambda p: p if p and p.get("state") == "running" else None)(plug(b)), 15) or {}
        check(r.get("anyway") == "waiting" and p.get("anyway") and p.get("withdrawn") and os.path.exists(os.path.join(state, "plugins", "testwake", "anyway"))
              and until(lambda: "withdrew" not in get(b, "/api/plugins").get("wake_why", ""), 15), f"the press: it runs anyway, the engine's words are those of one that runs: {r} {p.get('state')} {get(b, '/api/plugins').get('wake_why')}")
        site.build({"testwake": [v1, v2]}, revoked=[{"id": "testwake", "reason": "the index found a flaw"}, {"id": "plug01", "reason": "x"}], revoked_serial=11)
        look(b)
        p = until(lambda: (lambda p: p if p and p["state"] != "running" else None)(plug(b)), 15) or {}
        check("withdrew it" in p.get("detail", "") and not p.get("anyway"), f"a newer list that names it again: stopped again, the owner asked anew: {p.get('detail')}")
        site.build({"testwake": [v1, v2]}, revoked_serial=12)
        look(b)
        check(until(lambda: (plug(b) or {}).get("state") == "running", 15) and not (plug(b) or {}).get("withdrawn"), "a list that withdraws it no more: it runs")
        b.set(wake_engine="amazon")
        # an update from a lower tier than the version installed is wider: the button, whoever brings it (a review,
        # 2026-11-21: the author key's unlisted package replaced an official plugin from any approved browser)
        st, r = upload(b, package(version=1000000000))
        st2, r2 = post(b, "/api/plugins/install")
        check(st == 200 and r.get("tier") == "sideload" and r.get("button") is True and any("Comes from a file, not listed; the version installed here came from the plugin index, official" in c["line"] for c in r.get("changes", []))
              and r2.get("install") == "waiting" and version() == 2, f"an unlisted package of an official plugin's author key: the button, and why: {r.get('changes')} {r2}")
        post(b, "/api/plugins/cancel")

        # ---- serials, which are times: from the future (4), and under this build's least (5.5)
        now = int(time.time())
        def restart(**extra):
            nonlocal proc
            proc.terminate(); proc.wait(); proc = start(**extra); wait_port(PORT, WEB, OTA)
        restart(HASSMIC_CLOCK_SYNCED=None)                  # (the clock set)
        # (the index tool makes no such list: the key's holder can, signed here)
        def resigned(name, **changes):
            d = json.load(open(os.path.join(site.dir, name))); d.update(changes); site.resign(name, plugsite.dump(d)); return site.dir
        site.build({"testwake": [v1, v2]}); far = resigned("index.json", serial=now + 2 * 86400)
        o = look(b)
        check(o.get("state") == "failed" and "more than a day after this Echo's clock" in o.get("error", ""), f"the clock set: an index from two days ahead refused (one signed list from the far future froze every Echo for good): {o.get('error')}")
        site.build({"testwake": [v1, v2]}); resigned("revoked.json", serial=now + 2 * 86400)
        o = look(b)
        check(o.get("state") == "failed" and "revocation list" in o.get("error", "") and "more than a day" in o.get("error", "") and o.get("revoked_serial") == 12, f"and a revocation list from two days ahead: {o.get('error')}")
        restart(HASSMIC_PLUGINS_BUILT=str(now - 3 * 86400))  # (the clock not set, the build three days old)
        site.dir = far; o = look(b)
        fl = int(open(os.path.join(state, "plugins", ".serial")).read().split()[1])
        check(o.get("state") == "ok" and o.get("serial") == now + 2 * 86400 and fl < now + 3600, f"the clock not set: the same index shown, the floor kept no further than the build's time + a day: {o.get('state')} {o.get('error')} floor {fl - now}")
        site.build({"testwake": [v1, v2]})
        check(look(b).get("state") == "ok", "and the next honest index is taken: no freeze")
        restart(HASSMIC_CLOCK_SYNCED=None)
        site.build({"testwake": [v1, v2]})
        check(look(b).get("state") == "ok", "nor once the clock is set")
        restart(HASSMIC_PLUGINS_LEAST=f"{now + 3600}:0")
        o = look(b)
        check(o.get("state") == "failed" and "than this hassmic takes" in o.get("error", ""), f"an index under this build's least serial (an old signed list handed to an Echo after a factory reset): refused: {o.get('error')}")
        st, r = upload(b, package(version=3, listed=True))
        check(st == 200 and r.get("tier") == "sideload" and r.get("button") is True and any("Listed by an index older than this hassmic takes" in c["line"] for c in r.get("changes", [])),
              f"a package whose listing is older than that: counts as a file's, the button (a version the index dropped stays no official one for good): {r.get('tier')} {r.get('changes')}")
        post(b, "/api/plugins/cancel")
        # ---- the revocation list never goes back (a review, 2026-11-21): taken while the clock was not set, a list
        # left its floor line at the build's time + a day, and with the clock set later an older signed list from
        # between that line and the kept serial was taken over it.  The kept serial is the floor wherever the clock
        # can vouch for it; and a site's list older than the kept one no longer fails the look
        restart(HASSMIC_PLUGINS_BUILT=str(now - 30 * 86400))             # (the clock not set, the build a month old)
        site.build({"testwake": [v1, v2]}, revoked=[{"id": "testwake", "reason": "the newer list"}], revoked_serial=now - 86400)
        o = look(b)
        check(o.get("revoked_serial") == now - 86400 and until(lambda: (lambda p: p and p["state"] != "running")(plug(b)), 15)
              and open(os.path.join(state, "plugins", ".revoked"), "rb").read().split(b"\n", 1)[0] == f"floor {now - 29 * 86400}".encode(),
              "(the clock not set: a list of yesterday taken, testwake withdrawn, the floor line the build's time + a day)")
        restart(HASSMIC_CLOCK_SYNCED=None, HASSMIC_PLUGINS_BUILT=str(now - 30 * 86400))
        site.build({"testwake": [v1, v2]}, revoked_serial=now - 10 * 86400)
        o = look(b); time.sleep(1); p = plug(b) or {}
        check(o.get("state") == "ok" and o.get("revoked_serial") == now - 86400 and "older than the one this Echo has" in o.get("note", "") and p.get("state") != "running"
              and logged(log, f"is older (serial {now - 10 * 86400}) than the one this Echo has", 0, 1),
              f"the clock set: the site's older list refused and said (the log, the list's note); the newer index taken beside the list kept; testwake stays withdrawn: {o.get('state')} {o.get('error')} {o.get('note')} {o.get('revoked_serial')} {p.get('state')}")
        check(open(os.path.join(state, "plugins", ".revoked"), "rb").read().split(b"\n", 1)[0] == f"floor {now - 86400}".encode(), "and the floor line raised to the kept serial")
        site.build({"testwake": [v1, v2]}, revoked_serial=now - 60)
        check(look(b).get("revoked_serial") == now - 60 and until(lambda: (plug(b) or {}).get("state") == "running", 15), "(a newer list: taken, testwake runs)")
        restart()

        # a release whose least revocation serial is above the list kept here: the site's list refused as too old, and
        # the kept one still withdraws what it names, with the site out of reach too (it counted as none: what it
        # withdrew ran, for good where the site was out of reach; a review, 2026-11-21)
        site.build({"testwake": [v1, v2]}, revoked=[{"id": "testwake", "reason": "kept through the update"}], revoked_serial=now - 30)
        look(b)
        check(until(lambda: (lambda p: p and p["state"] != "running")(plug(b)), 15), "(a list that withdraws testwake, kept)")
        restart(HASSMIC_PLUGINS_LEAST=f"0:{now + 3600}")
        o = look(b)
        pl = get(b, "/api/plugins"); p = plug(b) or {}
        check(o.get("state") == "failed" and "than this hassmic takes" in o.get("error", "") and o.get("revoked_serial") == now - 30, f"a revocation list under its least: refused: {o.get('error')}")
        site.down = True; restart(HASSMIC_PLUGINS_LEAST=f"0:{now + 3600}"); time.sleep(1)
        pl = get(b, "/api/plugins"); p = plug(b) or {}
        check(p.get("state") != "running" and "kept through the update" in p.get("detail", "") and pl.get("revoked", {}).get("serial") == now - 30 and pl["revoked"].get("least") == now + 3600
              and "older than this hassmic wants" in pl["revoked"].get("stale", ""),
              f"the site out of reach: the kept list still withdraws testwake, and the page says the list is older than this hassmic wants: {p.get('state')} {pl.get('revoked')}")
        site.down = False
        site.build({"testwake": [v1, v2]}, revoked_serial=now + 3600)
        o = look(b)
        check(o.get("revoked_serial") == now + 3600 and until(lambda: (plug(b) or {}).get("state") == "running", 15), "a list of the least: taken, testwake runs again")
        restart()

        # ---- a week without word on the revocation list, while plugins run: said on the Plugins page and Overview
        # (the older form of .revoked-at: the wall time alone, the clock set)
        proc.terminate(); proc.wait()
        open(os.path.join(state, "plugins", ".revoked-at"), "w").write(f"{now - 8 * 86400}\n")
        restart(HASSMIC_CLOCK_SYNCED=None)
        pl = get(b, "/api/plugins"); ws = (b.state() or {}).get("warnings", [])
        check(pl.get("revoked", {}).get("age_s", 0) >= 8 * 86400 and "8 days ago" in pl.get("revoked", {}).get("stale", "") and any("8 days ago" in w for w in ws),
              f"the Echo last had word on it eight days ago: the Plugins page and Overview say so: {pl.get('revoked')}")
        look(b)
        pl = get(b, "/api/plugins"); ws = (b.state() or {}).get("warnings", [])
        at = open(os.path.join(state, "plugins", ".revoked-at")).read().split()
        check(0 <= pl.get("revoked", {}).get("age_s", -1) < 60 and not pl["revoked"]["stale"] and not any("withdrawn plugins" in w for w in ws) and at[0] == "word" and int(at[1]) >= now,
              f"a look: the word is fresh, the warning gone: {pl.get('revoked')} {at}")
        # the age over restarts where the clock is not set (it was -1, "no warning", after any restart), and with a
        # clock behind the word's time (it was clipped to 0: "fresh")
        for clock, line, what in ((None, f"word 0 {8 * 86400}\n", "the clock not set: the age kept over the restart"),
                                  (None, f"word {now + 3 * 86400} {8 * 86400}\n", "the clock set, behind the word's time: the age counted, not fresh")):
            proc.terminate(); proc.wait()
            open(os.path.join(state, "plugins", ".revoked-at"), "w").write(line)
            restart(**({"HASSMIC_CLOCK_SYNCED": None} if "set," in what else {}))
            pl = get(b, "/api/plugins")
            check(pl.get("revoked", {}).get("age_s", 0) >= 8 * 86400 and "8 days ago" in pl["revoked"].get("stale", ""), f"{what}: the warning: {pl.get('revoked')}")
        proc.terminate(); proc.wait(); os.remove(os.path.join(state, "plugins", ".revoked-at")); restart()
        pl = get(b, "/api/plugins")
        check(pl.get("revoked", {}).get("age_s") == -1 and "does not know when" in pl["revoked"].get("stale", ""), f"no word known at all: a warning too (it counted as fresh): {pl.get('revoked')}")
        look(b)
        # the list is a member's: whether this Echo has one is not told to anyone else on the LAN (it was open)
        st, _, body = req(WEB, "GET", "/api/plugins/revoked"); nf = req(WEB, "GET", "/api/nothing-here")
        check(st == 404 and body == nf[2] and os.path.exists(os.path.join(state, "plugins", ".revoked")), f"GET /api/plugins/revoked without the network's MAC: answered as a path there is none of, though a list is kept: {st} {body[:60]}")

        # ---- the revocation list by itself, once a day (here: every 1.5 s), and nothing else; with plugins off, nothing
        restart(HASSMIC_PLUGIN_DAILY="300:1500:600000")
        site.hits.clear(); site.agents.clear(); time.sleep(4)
        hits = list(site.hits)
        check(hits.count("/revoked.json") >= 2 and hits.count("/revoked.json.sig") >= 2 and not [h for h in hits if not h.startswith("/revoked.json")] and set(site.agents) == {"hassmic"} and logged(log, "the daily look at the revocation list"),
              f"plugins on and one installed: revoked.json and its signature, by themselves, saying only \"hassmic\": {hits}")
        b.set(plugins="off"); time.sleep(0.5); site.hits.clear(); time.sleep(3)
        check(not site.hits, f"plugins off: nothing asked: {site.hits}")
        b.set(plugins="on")
        restart()

        # ---- a second index key (docs/plugins.md 5.2, a new key's first step): the site signed by the new key alone,
        # lists and listings.  A build that reads the first key only takes none of it; one that reads both takes it all,
        # and an update listed by the new key installs as official as any; back on the first key alone, what the new
        # key listed counts as from a file
        newkey = os.path.join(top, "newindex")
        v = version()
        site.build({"testwake": [v1, v2, package(version=v + 1)]}, key=newkey + ".key")
        site.resign("index.json", open(os.path.join(site.dir, "index.json"), "rb").read(), key=newkey + ".key")
        # (the revocation list one second newer: the one a build of both keys keeps is the new key's, K6 below)
        rv = json.load(open(os.path.join(site.dir, "revoked.json"))); rv["serial"] += 1; site.resign("revoked.json", plugsite.dump(rv), key=newkey + ".key")
        o = look(b)
        check(o.get("state") == "failed" and "signature does not hold" in o.get("error", "") and o.get("plugins") == [],
              f"a build that reads the first key only: the site signed by the new key refused whole: {o.get('state')} {o.get('error')}")
        # ---- the daily look takes the index's listing too (a review of 3c5d7ff, R2): an Echo whose owner never opened the
        # Plugins page between the index's switch and the build of the new key alone.  Under that build the first key's
        # listing does not hold: the plugin is a file's, and with community plugins off (the default) not started.  The
        # daily look (its first one after the start; here 0.3 s) sees a listing installed that does not hold, reads the
        # index as well and takes the new key's listing of the same version: official, running, without the page
        lp = os.path.join(state, "plugins", "testwake", "pkg", "listing.sig"); was_l = open(lp, "rb").read()
        cfg = os.path.join(state, "config")
        def community(on):              # (state/config while hassmic is stopped)
            c = open(cfg).read(); open(cfg, "w").write(re.sub(r"(?m)^plugins_community=\w+$", f"plugins_community={on}", c))
        proc.terminate(); proc.wait(); community("off"); site.hits.clear(); n0 = len(open(log).read())
        proc = start(HASSMIC_PLUGINS_PUB=newkey + ".pub", HASSMIC_PLUGIN_DAILY="300:86400000:3600000"); wait_port(PORT, WEB, OTA)
        check(until(lambda: (lambda p: p and p.get("tier") == "official" and p.get("state") == "running")(plug(b)), 20)
              and "/index.json" in site.hits and logged(log, "the daily look reads the plugin index too", n0, 1)
              and logged(log, "testwake version %d: the plugin index's listing taken" % v, n0, 1) and version() == v,
              f"the new key alone, the first key's listing, community off, the page never opened: the daily look reads the index and takes its listing; official, it runs: {plug(b)} {site.hits}")
        # ---- ... and sooner than the daily look (a review of 3fcd319, S2: its first came 5 to 65 min after the start, and
        # one that failed waited a day, the wake word down meanwhile): a listing installed that does not hold has the
        # index read within a minute of the start (0.3 s here), only with a network (HASSMIC_FAKE_ONLINE), at once when
        # the network comes back, and again an hour after a look that failed (3 s here), until the listing is taken.
        # A look the Echo made by itself that failed is not the page's list (S5: PF_AUTO showed it for an hour); and a
        # version the index does not list is no reason to read it again (S4: index.json read for good)
        online = os.path.join(top, "online")
        relist = dict(HASSMIC_PLUGINS_PUB=newkey + ".pub", HASSMIC_PLUGIN_RELIST="300:3000:200:1000", HASSMIC_FAKE_ONLINE=online)
        proc.terminate(); proc.wait(); open(lp, "wb").write(was_l); site.hits.clear(); n0 = len(open(log).read())
        if os.path.exists(online): os.unlink(online)
        proc = start(**relist); wait_port(PORT, WEB, OTA); time.sleep(1.5)
        check(not site.hits and (plug(b) or {}).get("state") != "running" and (plug(b) or {}).get("tier") == "sideload",
              f"a listing that does not hold, no network: nothing asked, the plugin a file's and stopped: {site.hits} {plug(b)}")
        site.down = True; open(online, "w").close()
        check(until(lambda: "/revoked.json" in site.hits, 3) and logged(log, "the plugin index read for it (the network is back)", n0, 1),
              f"the network back: the index read at once (here: out of reach): {site.hits}")
        time.sleep(0.5); o = get(b, "/api/plugins/index"); k = site.hits.count("/revoked.json")
        check(o.get("state") == "never" and k == 1, f"S5: that failure, the Echo's own, leaves the page's list as it was (never looked at here): {o.get('state')} {o.get('error')} {site.hits}")
        site.down = False
        check(until(lambda: (lambda p: p and p.get("tier") == "official" and p.get("state") == "running")(plug(b)), 8) and "/index.json" in site.hits
              and logged(log, "testwake version %d: the plugin index's listing taken" % v, n0, 1) and not logged(log, "the daily look", n0, 1),
              f"read again an hour (3 s) after the look that failed: the listing taken, official, it runs, without the daily look: {plug(b)} {site.hits}")
        k = len(site.hits); time.sleep(3.5)
        check(len(site.hits) == k, f"the listing taken: nothing read again: {site.hits[k:]}")
        # (S4) the version installed not in the index: one read, and no more
        keep = {n: open(os.path.join(site.dir, n), "rb").read() for n in ("index.json", "index.json.sig")}
        ix = json.loads(keep["index.json"])
        for e in ix["plugins"]:
            if e["id"] == "testwake": e["versions"] = [x for x in e["versions"] if x["version"] != v]
        site.resign("index.json", plugsite.dump(ix), key=newkey + ".key")
        proc.terminate(); proc.wait(); open(lp, "wb").write(was_l); site.hits.clear(); n0 = len(open(log).read())
        proc = start(**relist, HASSMIC_PLUGIN_DAILY="9000:86400000:3600000"); wait_port(PORT, WEB, OTA)
        check(until(lambda: "/index.json" in site.hits, 5), f"(the index read for the listing that does not hold: {site.hits})")
        time.sleep(4)
        check(site.hits.count("/index.json") == 1 and (plug(b) or {}).get("tier") == "sideload",
              f"S4: the index does not list the version installed: read once, not again an hour later (3 s) for a listing no look can move: {site.hits}")
        # (T4 of a review of 9cd7a77) ... but the index lists it again (or a revocation is lifted): the daily look (9 s
        # here) reads the index for a listing that does not hold whatever the last look found, and takes it.  It read
        # the revocation list alone, and nothing but the page's look or a restart saw it
        for n, d in keep.items(): open(os.path.join(site.dir, n), "wb").write(d)
        check(logged(log, "the daily look reads the plugin index too", n0, 10) and logged(log, "testwake version %d: the plugin index's listing taken" % v, n0, 5)
              and until(lambda: (plug(b) or {}).get("tier") == "official", 5),
              f"T4: the index lists the version again: the daily look reads it and takes the listing: {(plug(b) or {}).get('tier')} {site.hits}")
        # (T2 of a review of 9cd7a77) the network back within the gap after a look (5 s here, a look every 20 s): the
        # look when the gap is over, not at the next hour's (the return was forgotten)
        proc.terminate(); proc.wait(); open(lp, "wb").write(was_l); site.hits.clear(); n0 = len(open(log).read())
        site.down = True
        proc = start(**dict(relist, HASSMIC_PLUGIN_RELIST="300:20000:200:5000")); wait_port(PORT, WEB, OTA)
        check(until(lambda: "/revoked.json" in site.hits, 3), f"(the first look, the site out of reach: {site.hits})")
        site.down = False; os.unlink(online); time.sleep(0.8); open(online, "w").close()
        check(logged(log, "testwake version %d: the plugin index's listing taken" % v, n0, 8) and site.hits.count("/revoked.json") == 2,
              f"T2: the network back 1 s after a look that failed: read again once the gap is over, the listing taken: {site.hits}")
        # (T3 of a review of 9cd7a77) the index lists the version, and its package brings no listing this Echo can take
        # (here the first key's, which the build of the new key alone does not read): downloaded once, and no more a
        # reason to read the index hourly (3 s) and at every return of the network
        pk = os.path.join(site.dir, "packages", f"testwake-{v}.hmp")
        keep = {n: open(os.path.join(site.dir, n), "rb").read() for n in ("index.json", "index.json.sig", f"packages/testwake-{v}.hmp")}
        f = plugsite.plugcheck_members(keep[f"packages/testwake-{v}.hmp"]); f["listing.sig"] = was_l
        data = plugcheck.tar_bytes([(k, f[k]) for k in ("manifest.json", "manifest.sig", "plugin", "LICENSE", "listing.sig") if k in f]); open(pk, "wb").write(data)
        ix = json.loads(keep["index.json"])
        for e in ix["plugins"]:
            for x in (e["versions"] if e["id"] == "testwake" else []):
                if x["version"] == v: x["size"], x["sha256"] = len(data), hashlib.sha256(data).hexdigest()
        site.resign("index.json", plugsite.dump(ix), key=newkey + ".key")
        proc.terminate(); proc.wait(); open(lp, "wb").write(was_l); site.hits.clear(); n0 = len(open(log).read())
        proc = start(**relist); wait_port(PORT, WEB, OTA)
        check(logged(log, "testwake version %d: the plugin index's listing not taken" % v, n0, 5), f"(its package downloaded, its listing not taken: {site.hits})")
        time.sleep(4)
        check(site.hits.count("/index.json") == 1 and site.downloads() == [f"/packages/testwake-{v}.hmp"] and (plug(b) or {}).get("tier") == "sideload",
              f"T3: a listing the index has and this Echo cannot take: read once, not again an hour later (3 s): {site.hits}")
        for n, d in keep.items(): open(os.path.join(site.dir, n), "wb").write(d)
        os.unlink(online)
        proc.terminate(); proc.wait(); open(lp, "wb").write(was_l); community("on"); site.hits.clear()
        proc = start(HASSMIC_PLUGIN_DAILY="300:86400000:3600000"); wait_port(PORT, WEB, OTA); time.sleep(2)
        check([h for h in site.hits if h.startswith("/revoked.json")] and "/index.json" not in site.hits,
              f"(a build of one key, the listing that key's: the daily look reads the revocation list alone, as before: {site.hits})")
        restart(HASSMIC_PLUGINS_PUB2=newkey + ".pub")
        until(lambda: (plug(b) or {}).get("state") == "running", 20); runs0 = open(log).read().count("testwake: running")
        o = look(b); t = listed(o, "testwake") or {}
        check(o.get("state") == "ok" and (t.get("here") or {}).get("version") == v + 1, f"a build that reads both keys: the new key's lists taken: {o.get('state')} {o.get('error')} {t.get('here')}")
        check((t.get("here") or {}).get("broad") == [] and (t.get("top") or {}).get("broad") == [],
              f"(an official version: no broad flags for the page, which only community versions carry: {t.get('here')} {t.get('top')})")
        # ---- the index's listing of the version installed here (K2 of a review of a30d6ed): the look by a build of both
        # keys finds version v listed anew by the new key, downloads it and takes its listing in place of the first key's:
        # no button, no install, the process left running.  A build of the new key alone (5.2's last step) then has it
        # as official, also with community plugins off (their default); a step back to the first key alone has it as a
        # file's, stopped, and says why, and the revocation list the new key signed is no word there (K6)
        npub = open(newkey + ".pub", "rb").read()
        def by_new():
            l = open(lp, "rb").read()
            return len(l) > 129 and plugcheck.otatool.check(bytes.fromhex(l[:128].decode()), npub, l[129:])
        check(until(by_new, 20) and version() == v and (plug(b) or {}).get("tier") == "official" and logged(log, "testwake version %d: the plugin index's listing taken" % v),
              f"both keys, the site moved to the new one: the listing of version {v} installed here taken from it, by the new key, without a press: {(plug(b) or {}).get('tier')} {version()}")
        o = get(b, "/api/plugins/index"); t = listed(o, "testwake") or {}
        check((plug(b) or {}).get("state") == "running" and open(log).read().count("testwake: running") == runs0 and "relist" not in t,
              f"... the same version, still running, not started again, no install: {(plug(b) or {}).get('state')} {t.get('relist')}")
        proc.terminate(); proc.wait(); community("off"); proc = start(HASSMIC_PLUGINS_PUB=newkey + ".pub"); wait_port(PORT, WEB, OTA)
        check(until(lambda: (lambda p: p and p.get("tier") == "official" and p.get("state") == "running")(plug(b)), 15),
              f"the build of the new key alone, community plugins off (the default): official, it runs: {plug(b)}")
        proc.terminate(); proc.wait(); proc = start(); wait_port(PORT, WEB, OTA)
        p = plug(b) or {}; pl = get(b, "/api/plugins"); stale = pl.get("revoked", {}).get("stale", "")
        check(p.get("tier") == "sideload" and p.get("state") != "running" and "from a file" in p.get("detail", ""),
              f"a step back to the first key alone, community off: a file's, not started, said (5.2): {p.get('state')} {p.get('detail')}")
        check(pl.get("revoked", {}).get("serial") == -1 and "key this hassmic does not read" in stale,
              f"... and the revocation list the new key signed is no list and no word there: the warning at once, saying why (K6): {pl.get('revoked')}")
        proc.terminate(); proc.wait(); community("on"); proc = start(HASSMIC_PLUGINS_PUB2=newkey + ".pub"); wait_port(PORT, WEB, OTA)
        f = fetch(b, "testwake"); c = f.get("check", {})
        check(f.get("state") == "checked" and c.get("tier") == "official" and c.get("button") is False and c.get("version") == v + 1,
              f"its listing by the new key: official, an update that is what was accepted, no button: {f.get('state')} {f.get('error')} {({k: c.get(k) for k in ('tier', 'button', 'version')})}")
        st, r = post(b, "/api/plugins/install")
        check(st == 200 and r.get("install") == "installed" and version() == v + 1 and (plug(b) or {}).get("tier") == "official", f"installed, official: {r} {(plug(b) or {}).get('tier')}")
        restart()
        check((plug(b) or {}).get("tier") == "sideload" and version() == v + 1, f"a build with the first key alone again: what the new key listed counts as from a file (5.2): {(plug(b) or {}).get('tier')}")

        # ---- pushed from the PC after a bundle (ota.c): the update key for the port, the index key's listing for the package
        def push(pkg, key="update", tool=PCTOOL):
            path = os.path.join(top, "push.hmp"); open(path, "wb").write(pkg)
            r = subprocess.run(tool + ["plugin", "127.0.0.1", str(OTA), os.path.join(top, key + ".key"), path], capture_output=True, text=True)
            return r.returncode, r.stdout.strip()
        n0 = len(open(log).read())
        rc, out = push(package(version=10, version_name="1.0", listed=True))
        check(rc == 0 and out == "OK testwake version 10 installed" and version() == 10 and plug(b)["how"].startswith("update: pushed") and logged(log, "testwake: running", n0, 20),
              f"an update that is what was accepted, pushed: installed without the button, and it runs: {out}")
        rc, out = push(package(version=11, listed=True), tool=[f"{ROOT}/build/otatool-host"])
        check(rc == 0 and version() == 11, f"the Echo's own otatool pushes the same way: {out}")
        rc, out = push(package(version=12, capabilities={"wake": True, "files": 2}, listed=True))
        check(rc == 1 and out.startswith("FAILED") and "not what was accepted" in out and version() == 11, f"one that asks for more: refused, the PC is told why: {out}")
        rc, out = push(package(version=12))
        check(rc == 1 and "no listing" in out and version() == 11, f"a sideload: {out}")
        rc, out = push(package(version=12, id="newplug", listed=True))
        check(rc == 1 and "never accepted" in out and not os.path.exists(os.path.join(state, "plugins", "newplug")), f"a plugin nobody accepted on this Echo: {out}")
        rc, out = push(package(version=12, capabilities={"wake": True, "files": 1, "storage": 4}, listed=True))
        check(rc == 1 and "needs a newer hassmic: storage 1" in out and version() == 11, f"one that needs a scope this hassmic lacks: refused, the PC is told why: {out}")
        rc, out = push(package(version=12, listed=True), key="evil")
        check(rc == 1 and "update key" in out and version() == 11, f"pushed with another key than the Echo's update key: {out}")
        st, r = upload(b, package(version=12, capabilities={"wake": True, "files": 2}, listed=True)); st, r = post(b, "/api/plugins/install")
        rc, out = push(package(version=13, listed=True))
        check(r.get("install") == "waiting" and rc == 1 and "waits for the action button" in out and version() == 11, f"while an install waits for the button: {out}")
        post(b, "/api/plugins/cancel")

        # ---- nothing by itself: the Echo looks for no plugin's build at its looks for updates (no update breaks one)
        proc.terminate(); proc.wait()
        cfg = os.path.join(state, "config"); lines = [l for l in open(cfg) if not l.startswith(("wake_engine=", "online_updates="))]
        open(cfg, "w").write("".join(lines) + "wake_engine=plugin:testwake\n")
        site.hits.clear(); n0 = len(open(log).read())
        proc = start(fresh="0:0"); wait_port(PORT, WEB, OTA)
        r = b.set(online_updates="beta"); time.sleep(3)
        check(not dls() and "looking for its build" not in open(log).read()[n0:] and not [h for h in site.hits if h.startswith("/index.json")],
              f"online updates on, the wake word engine a plugin: nothing asked of the index for it at the look for updates: {site.hits}")
    finally:
        proc.terminate(); proc.wait(); site.shutdown()
        if check.failed: print(open(log, errors="replace").read()[-6000:])
        subprocess.run(["rm", "-rf", top])
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


main()
