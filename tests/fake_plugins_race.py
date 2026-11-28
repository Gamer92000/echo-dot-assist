#!/usr/bin/env python3
"""The one install slot of the plugins (web.c "the install slot"; docs/plugins.md 5.4 "As built", 13's seventh read):
who may take it from whom, and what the owner's press of the action button installs.  build/hassmic-host under
build/plugrun-host, two approved browsers, a stand-in plugin index (tests/plugsite.py) and the PC that pushes, as
tests/fake_plugins_fetch.py.

What was wrong (a review, 2026-11-19): /api/plugins/install looked at the slot, let go of the lock for its check of
the file, and armed the press without looking again.  A second browser's fetch in that window took the slot, its end
wrote "checked" over the wait, and the press the owner gave the first browser's dialog installed whatever the second
had uploaded by then.  HASSMIC_PLUGIN_ASK_DELAY=<ms>:<file> holds that window open for as long as a test needs
(between /install's check and its ask, while the file is there), HASSMIC_PLUGIN_SLOT=<checked ms>:<upload ms>:<fetch ms> the slot's timeouts short.

With the timeouts as they are: in that window another browser's fetch and upload, and a push, are refused; the
press installs the first browser's package.  Since the eighth read /install's look at the file is in CHECKING,
which nobody takes, so the window is closed whatever the timeouts: the same browser's other upload in it is
refused too.  Two browsers at once: one has the slot, the other is told.  With the timeouts run out (a dialog left
open): another browser's fetch and a push each take the slot before the first browser's /install, which is told,
and the press installs nothing of anyone's.
The eighth read (2026-11-20): the press is the wait's it was armed for (an install waits, a browser nobody approved
asks to log in three times, the press installs the plugin and approves nobody; an install asked while such a login
waits is held up for that login's time and no longer; the ring each time, HASSMIC_FAKE_LED); and the decision
about the button is made where the files are written (the plugin removed between a check that said "no button"
and the install: on the page, under a push, HASSMIC_PLUGIN_OWN_DELAY).  A fetch is given up after its total time;
a cancelled fetch leaves the index's list as it was.  (The Echo fetches nothing by itself: no hassmic update breaks a
plugin.)
The list's pace: what a page asks by itself ("look") reaches the index once an hour whether it found the list or not,
a press ("again") every HASSMIC_PLUGIN_FRESH's second number of seconds.
The push port (ota.c): a request line that does not come is ended after 5 s and holds nobody else up; a bundle
that does not come after 10 s; one unsigned bundle an address (tests/fake_ota_port.py has the rest)."""
import json, os, resource, signal, subprocess, sys, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser
import plugsite
from plugsite import plugcheck

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, WEB, GH, OTA = 16891, 16892, 16893, 16894
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
    for k in ("author", "stranger", "update", "evil"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    site = plugsite.Site(top, GH)
    man = json.load(open(f"{ROOT}/tests/plugins/testwake.json"))
    made = [0]
    def package(official=True, **changes):
        """the author's; for an official one the index's listing added, as a package from the site has it"""
        made[0] += 1
        m = dict(man, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, f"p{made[0]}.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/testwake", os.path.join(top, "author.key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        if official:
            plugcheck.list_package(out, site.key, out + ".o", arch="x86_64")
            out += ".o"
        return open(out, "rb").read()
    def publish(**assets):
        """the index with these packages (<id>_<version> -> a listed package: the site lists it anew, from the author's own)"""
        plugs = {}
        for n, d in assets.items(): plugs.setdefault(n.rsplit("_", 1)[0], []).append(plugsite.unlisted(d))
        site.build(plugs)

    hold = os.path.join(top, "ask-hold"); ASK = f"20000:{hold}"     # in_window's window, held while `hold` is there
    def start(fresh="3600:0", **more):
        more.setdefault("HASSMIC_LOGIN_MS", "60000:300")       # (browsers log in one after the other here: web.c keeps others back 0.3 s after a press, not 30)
        e = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
                 HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(state, "models"),
                 HASSMIC_FAKE_WHISPER="1", HASSMIC_LOG=log, HASSMIC_CLOCK_SYNCED="0", HASSMIC_PLUGINS_PUB=site.pub, HASSMIC_PLUGIN_INDEX=site.base,
                 HASSMIC_UPDATE_PUB=os.path.join(top, "update.pub"), HASSMIC_ADB_OPEN=os.path.join(state, "adb-open.root"),
                 HASSMIC_UPDATE_API=f"{site.base}repos/o/r", HASSMIC_UPDATE_DOWNLOAD=f"{site.base}dl",
                 HASSMIC_PLUGIN_FRESH=fresh, HASSMIC_PLUGIN_LEAST="oldplug:5", HASSMIC_PLUGIN_RETRY="3000", **more)
        return subprocess.Popen([f"{ROOT}/build/plugrun-host", "-L", "--", os.environ.get("HASSMIC_BIN", f"{ROOT}/build/hassmic-host"), "-P", "esphome", "-p", str(PORT), "-n", "Echo Race",
                                 "-L", "-z", "0", "-o", str(OTA), "-a", "0", "-W", str(WEB)], env=e, stderr=open(log, "a"),
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    installed = lambda pid="testwake": os.path.exists(os.path.join(state, "plugins", pid, "pkg", "manifest.json"))
    version = lambda: (plug(b) or {}).get("version")
    dls = site.downloads


    import socket
    side = lambda **kw: package(official=False, id=kw.pop("id", "sideplug"), name=kw.pop("name", "Side"), **kw)
    accepted = lambda n0: "accepted with the button" in open(log, errors="replace").read()[n0:]
    def push(pkg, key="update"):
        path = os.path.join(top, "push.hmp"); open(path, "wb").write(pkg)
        r = subprocess.run(PCTOOL + ["plugin", "127.0.0.1", str(OTA), os.path.join(top, key + ".key"), path], capture_output=True, text=True)
        return r.returncode, r.stdout.strip()
    def in_window(who, meanwhile):
        """WHO asks /api/plugins/install, which HASSMIC_PLUGIN_ASK_DELAY holds between its check and its ask while the
        file `hold` is there; MEANWHILE runs once it is held, and the window ends after it (CI 2026-11-24: a push begun
        0.5 s into a window of 1.5 s reached the slot after it).  (status, answer) of the install, and what MEANWHILE gave"""
        res = {}; open(hold, "w").close(); n0 = len(open(log, errors="replace").read())
        t = threading.Thread(target=lambda: res.update(a=post(who, "/api/plugins/install"))); t.start()
        try:
            check(logged(log, "the install's ask held", n0, 10), "the install is held between its check and its ask")
            m = meanwhile()
        finally: os.unlink(hold)
        t.join()
        return res["a"], m
    def press_for_nothing(what, *nothing):
        n0 = len(open(log).read())
        proc.send_signal(signal.SIGUSR2); time.sleep(1.5)
        check(not accepted(n0) and not any(os.path.exists(os.path.join(state, "plugins", p)) for p in nothing), f"{what}: the press installs nothing ({', '.join(nothing)})")
    told = lambda r: "took this one's place" in r.get("error", "")

    v1 = package()
    proc = start(HASSMIC_PLUGIN_ASK_DELAY=ASK)
    try:
        wait_port(PORT, WEB, OTA)
        b = Browser(WEB); check(b.login_with_button(proc), "logged in with the button")
        b2 = Browser(WEB); check(b2.login_with_button(proc), "a second browser, approved too")
        b.set(plugins="on")
        publish(testwake_1=v1); look(b)

        # ---- the review's interleaving, the slot's timeouts as they are: the window is closed
        f = fetch(b, "testwake")
        check(f.get("state") == "checked" and f.get("check", {}).get("button") is True, f"A has a fetched, checked official package: {f.get('state')} {f.get('error')}")
        evil = side(id="evilplug", name="Evil")
        (st, r), (m1, m2, m3) = in_window(b, lambda: (post(b2, "/api/plugins/fetch", "testwake"), post(b2, "/api/plugins/upload/begin", str(len(evil))), post(b2, "/api/plugins/cancel")))
        check(m1[0] == 400 and "is being checked" in m1[1].get("error", "") and m2[0] == 400 and "is being checked" in m2[1].get("error", ""),
              f"between A's check and its ask (the slot is CHECKING: nobody's to take), B's fetch and B's upload are refused, each with why: {m1} {m2}")
        check(st == 200 and r.get("install") == "waiting" and r.get("holds") == "plugin" and m3[0] == 400 and get(b, "/api/plugins").get("install", {}).get("state") == "waiting",
              f"A's install waits for the button, and B's cancel ends nothing of A's: {r} {m3}")
        st2, r2 = upload(b2, evil); st3, r3 = post(b2, "/api/plugins/fetch", "testwake")
        check(st2 == 400 and st3 == 400 and "waits for the action button" in r2.get("error", "") and get(b2, "/api/plugins").get("install", {}).get("state") == "none",
              f"while it waits B brings nothing, and sees none of it: {r2} {r3}")
        n0 = len(open(log).read())
        proc.send_signal(signal.SIGUSR2)
        check(until(lambda: get(b, "/api/plugins").get("install", {}).get("state") == "installed") and installed() and not installed("evilplug") and accepted(n0),
              "the press installs what A's dialog listed, and nothing of B's")
        check(until(lambda: (plug(b) or {}).get("state") == "running", 20), "and it runs")

        # ---- a push in the window: refused; and the same browser checking another package: the first install overtaken
        st, r = upload(b, side())
        (st, r), (rc, out) = in_window(b, lambda: push(package(version=2)))
        check(rc == 1 and "is being checked" in out and version() == 1 and st == 200 and r.get("install") == "waiting", f"a push in the window: refused, A's install waits: {out} {r}")
        post(b, "/api/plugins/cancel")
        check(get(b, "/api/plugins").get("install", {}).get("state") == "none" and not os.path.exists(os.path.join(state, "plugins", ".stage")), "cancelled: the wait and the file gone")
        st, r = upload(b, side())
        # the same browser with another package in the window (another tab): before the eighth read /install looked at
        # the file in CHECKED, where its own browser may begin anew, and the first install was overtaken; now the slot
        # is nobody's to take while the file is looked at, its own browser's neither
        (st, r), m = in_window(b, lambda: upload(b, side(name="Another one")))
        check(m[0] == 400 and "is being checked" in m[1].get("error", "") and st == 200 and r.get("install") == "waiting",
              f"A begins another upload while its install's look at the file is on its way: refused, the install waits for what its dialog listed: {m} {r}")
        st, r = post(b, "/api/plugins/upload/0", b"x" * 2000)
        check(st == 400 and get(b, "/api/plugins").get("install", {}).get("state") == "waiting", f"and a piece sent while it waits goes nowhere: {r}")
        post(b, "/api/plugins/cancel")
        press_for_nothing("cancelled", "sideplug")

        # ---- two browsers at once: one has the slot, the other is told
        res = {}; pk = side()
        ts = [threading.Thread(target=lambda w=w, n=n: res.update({n: post(w, "/api/plugins/upload/begin", str(len(pk)))})) for w, n in ((b, "a"), (b2, "b"))]
        [t.start() for t in ts]; [t.join() for t in ts]
        won, lost = (b, b2) if res["a"][0] == 200 else (b2, b); lr = res["b" if won is b else "a"]
        check(sorted((res["a"][0], res["b"][0])) == [200, 400] and "installing a plugin just now" in lr[1].get("error", ""), f"two uploads begun at once: one is taken, the other told why not: {res}")
        for off in range(0, len(pk), 200000): post(won, f"/api/plugins/upload/{off}", pk[off:off + 200000])
        st, r = post(won, "/api/plugins/check"); st1, r1 = post(lost, "/api/plugins/upload/0", pk[:1000]); st2, r2 = post(won, "/api/plugins/install")
        check(st == 200 and st1 == 400 and st2 == 200 and r2.get("install") == "waiting", f"the one that has it goes on to its dialog and the button; the other's pieces go nowhere: {r1} {r2}")
        proc.send_signal(signal.SIGUSR2)
        check(until(lambda: get(won, "/api/plugins").get("install", {}).get("state") == "installed") and installed("sideplug"), "the press installs it")
        st, r = upload(lost, side(id="thirdplug", name="Third"))
        check(st == 200 and r.get("id") == "thirdplug", f"and then the other one's turn comes: {r.get('id')} {r.get('error')}")
        post(lost, "/api/plugins/cancel")

        # ---- the push port: a line that does not come holds nobody up and is ended; one unsigned bundle at a time
        s = socket.create_connection(("127.0.0.1", OTA)); s.sendall(b"HMOTA-"); t0 = time.monotonic()
        rc, out = push(package(version=2))
        check(rc == 0 and version() == 2 and time.monotonic() - t0 < 4, f"a push while another connection sends no line: taken all the same: {out}")
        s.settimeout(10); ans = s.recv(100); dt = time.monotonic() - t0; s.close()
        check(ans == b"FAILED bad request\n" and 4 < dt < 8, f"the line that did not come within 5 s: ended ({dt:.1f} s, {ans!r})")
        s = socket.create_connection(("127.0.0.1", OTA)); s.sendall(b"HMOTA-PUSH1 16777216\n" + bytes(64) + b"x" * 1000); t0 = time.monotonic()
        time.sleep(0.3)
        s2 = socket.create_connection(("127.0.0.1", OTA)); s2.sendall(b"HMOTA-PUSH1 2000\n"); s2.settimeout(5); ans2 = s2.recv(100); s2.close()
        s.settimeout(15); ans = s.recv(100); dt = time.monotonic() - t0; s.close()
        check(b"FAILED busy: an update from this address is being pushed" in ans2, f"a second bundle from the address of one that no signature was checked for yet: {ans2!r} (tests/fake_ota_port.py has the rest of the port)")
        check(ans == b"FAILED upload incomplete\n" and 9 < dt < 13, f"a bundle of 16 MB said and not sent: ended after its 10 s ({dt:.1f} s, {ans!r})")
        slow = socket.create_connection(("127.0.0.1", OTA)); slow.sendall(b"HMOTA-ADB1\n"); slow.settimeout(3); nonce = slow.recv(100); t0 = time.monotonic()
        slow.settimeout(14); ans = slow.recv(100); dt = time.monotonic() - t0; slow.close()
        check(nonce.startswith(b"NONCE ") and ans == b"FAILED no signature\n" and 9 < dt < 13, f"a challenge nobody answers: ended after 10 s ({dt:.1f} s)")

        # ==== the timeouts run out at once (a dialog left open for longer than the slot keeps it): taken, and told
        proc.terminate(); proc.wait()
        proc = start(fresh="0:0", HASSMIC_PLUGIN_ASK_DELAY=ASK, HASSMIC_PLUGIN_SLOT="200:200:4000"); wait_port(PORT, WEB, OTA)
        publish(testwake_3=package(version=3)); look(b)
        # (The takeover is before A's /install now, not inside it: from its look at the file on, the slot is CHECKING,
        # which no timeout opens.  Shown first: timeouts run out, and still nothing is taken in the window)
        st, r = upload(b, side(id="newplug", name="New")); time.sleep(0.4)
        (st, r), m = in_window(b, lambda: (post(b2, "/api/plugins/fetch", "testwake"), push(package(version=3))))
        check(m[0][0] == 400 and "is being checked" in m[0][1].get("error", "") and m[1][0] == 1 and st == 200 and r.get("install") == "waiting" and version() == 2,
              f"the slot's times long run out, and in the window of A's install neither B's fetch nor a push takes it: {m} {r}")
        post(b, "/api/plugins/cancel")
        st, r = upload(b, side(id="newplug", name="New")); time.sleep(0.4)
        m = post(b2, "/api/plugins/fetch", "testwake"); st, r = post(b, "/api/plugins/install")
        check(m[0] == 200 and st == 400 and told(r), f"A's dialog was left too long and B's fetch took the slot: A's install is told it was overtaken: {m} {r}")
        check(until(lambda: get(b2, "/api/plugins/fetch").get("state") == "checked"), "B's fetch ends as checked, B's")
        st, r = upload(b2, evil)
        check(st == 200 and r.get("id") == "evilplug" and r.get("tier") == "sideload", f"B uploads and checks a sideload of its own: {r.get('id')} {r.get('error')}")
        press_for_nothing("B's sideload in the slot, A's dialog the last the owner saw", "evilplug", "newplug")
        ia = get(b, "/api/plugins").get("install", {})
        check(version() == 2 and ia.get("state") == "failed" and told(ia) and get(b2, "/api/plugins").get("install", {}).get("state") == "checked",
              f"A's page is told why, B's package still waits for B's own dialog: {ia}")
        post(b2, "/api/plugins/cancel")
        st, r = upload(b, side(id="newplug", name="New")); time.sleep(0.4)
        rc, out = push(package(version=3)); st, r = post(b, "/api/plugins/install")
        check(rc == 0 and version() == 3 and st == 400 and told(r), f"a push takes a slot left that long: installed, and A's install is told: {out} {r}")
        press_for_nothing("after the push", "newplug")

        # ---- a fetch as a whole has its time (here 4 s); a cancelled fetch leaves the list as it was
        publish(testwake_4=package(version=4)); o = look(b)
        site.slow = 9; t0 = time.monotonic()
        f = fetch(b, "testwake", 15); dt = time.monotonic() - t0
        check(f.get("state") == "failed" and "took longer than 4 s" in f.get("error", "") and dt < 7 and version() == 3, f"a download slower than the fetch's whole time: given up ({dt:.1f} s): {f.get('error')!r}")
        site.slow = 0; site.slow_api = 3
        st, r = post(b2, "/api/plugins/fetch", "testwake"); time.sleep(0.5); st2, r2 = post(b2, "/api/plugins/cancel")
        check(st == 200 and st2 == 200 and get(b2, "/api/plugins/fetch").get("state") == "none", f"a fetch cancelled during its look: its slot is free at once: {r2}")
        o2 = until(lambda: (lambda o: o if o.get("state") != "looking" else None)(get(b, "/api/plugins/index")), 10) or {}
        check(o2.get("state") == "ok" and [(p["id"], p["here"]["version"]) for p in o2.get("plugins", [])] == [("testwake", 4)], f"and the list every browser reads is as it was: {o2}")
        site.slow_api = 0
        check(logged(log, "the fetch of testwake was cancelled"), "said in the log")

        # ==== the press is the wait's it was armed for (the eighth read; web.c "what waits for the action button").
        # Times here: a wait 3 s, strangers kept back 1.5 s after an approved browser's wait
        proc.terminate(); proc.wait()
        leds = os.path.join(top, "ledctrl.log")
        # (as after a reboot: within one boot the waits withdrawn above keep a browser that is not approved back for
        # their minute over a restart too, state/web_wait; tests/fake_web.py one_wait has that)
        with open(os.path.join(top, "boot-2"), "w") as f: f.write("another-boot\n")
        proc = start(fresh="0:0", HASSMIC_FAKE_LED=leds, HASSMIC_LOGIN_MS="3000:1500", HASSMIC_PLUGIN_ASK_DELAY=ASK, HASSMIC_PLUGIN_OWN_DELAY="1500",
                     HASSMIC_BOOT_ID=os.path.join(top, "boot-2")); wait_port(PORT, WEB, OTA)
        text = lambda n0=0: open(log, errors="replace").read()[n0:]
        ring = lambda: [l.split()[-2] + " " + l.split()[-1] for l in open(leds).read().splitlines() if "authenticated_setup_mode" in l or l.endswith(" wait")]
        inst = lambda w=None: get(w or b, "/api/plugins").get("install", {})
        # the review's sequence: an install waits, a stranger asks to log in three times, the owner presses
        n0 = len(text()); open(leds, "w").close()
        st, r = upload(b, side(id="pressplug", name="Press")); st, r = post(b, "/api/plugins/install")
        check(st == 200 and r == {"install": "waiting", "holds": "plugin"} and until(lambda: ring()[-1:] == ["-s wait"]), f"an install waits for the button: the ring shows the plugin's own pattern ({r} {ring()})")
        x = Browser(WEB); got = [x.ask() for _ in range(3)]
        check(all(g["login"] == "refused" and g.get("why") == "busy" for g in got), f"a browser nobody approved asks to log in three times: busy each time, and not armed ({got})")
        check(inst() .get("state") == "waiting" and inst().get("holds") == "plugin" and ring() == ["-s wait"] and "login waits" not in text(n0) and "withdrawn" not in text(n0),
              f"...the install's wait is as it was: its page still waits, told that the press is the plugin's, and the ring never changed ({ring()})")
        proc.send_signal(signal.SIGUSR2)
        check(until(lambda: inst().get("state") == "installed") and installed("pressplug") and accepted(n0) and "login approved" not in text(n0) and x.call("GET", "/api/state")[0] == 401,
              "the press installs the plugin and approves nobody")
        a = x.ask(); proc.send_signal(signal.SIGUSR2); time.sleep(0.4)
        check(a.get("why") == "busy" and "login approved" not in text(n0) and "login waits" not in text(n0), f"asked right after the press, and a second press: still nobody's login ({a})")
        # the other way: his login is armed first.  It is not ended for the install, and holds it up once, for its own time
        time.sleep(1.6); n1 = len(text())
        check(x.login() == "waiting" and until(lambda: ring()[-1:] == ["-s authenticated_setup_mode"]), "after that time his login is armed like any other")
        st, r = upload(b, side(id="nextplug", name="Next")); st, r = post(b, "/api/plugins/install")
        check(st == 200 and r == {"install": "busy", "holds": "login"} and inst().get("state") == "checked" and x.login() == "waiting" and "-s wait" not in ring()[1:],
              f"an install asked meanwhile: busy, told that a login has the button, its package kept; the login is not ended ({r})")
        check(logged(log, "login ran out without the button", n1, 5), "the login's time ends (nobody pressed for it)")
        got = [x.ask() for _ in range(2)]; st, r = post(b, "/api/plugins/install"); got.append(x.ask())
        check(r == {"install": "waiting", "holds": "plugin"} and all(g["login"] == "refused" for g in got) and got[-1].get("why") == "busy" and text(n1).count("login waits") == 1,
              f"then the install is armed at once, though the stranger asked first and keeps asking: unsigned asks hold an install up for one login's time, no longer ({r} {got})")
        proc.send_signal(signal.SIGUSR2)
        check(until(lambda: inst().get("state") == "installed") and installed("nextplug") and "login approved" not in text(n0), "and the press installs it")

        # ---- the decision about the button is made where the files are written (plugin_store.c ps_stage_install)
        st, r = upload(b, package(version=7))
        check(st == 200 and r.get("button") is False and r.get("update") is True, f"an update of testwake that asks for nothing new: no button, says the check ({r.get('button')} {r.get('error')})")
        n2 = len(text())
        (st, r), m = in_window(b, lambda: post(b, "/api/plugins/testwake/remove", "remove"))
        check(m[0] == 200 and st == 400 and "needs the action button now" in r.get("error", "") and not os.path.exists(os.path.join(state, "plugins", "testwake")) and "installed (" not in text(n2),
              f"the plugin removed between that check and the install: nothing is installed without a press, and nothing accepted ({r})")
        st, r = post(b, "/api/plugins/install")
        check(inst().get("state") == "waiting" and r == {"install": "waiting", "holds": "plugin"}, f"the package is still there, and installing it now waits for the button ({r})")
        proc.send_signal(signal.SIGUSR2)
        check(until(lambda: inst().get("state") == "installed") and until(lambda: version() == 7), "which installs it")
        # a push
        n2 = len(text()); res = {}
        t = threading.Thread(target=lambda: res.update(p=push(package(version=8)))); t.start()
        seen = logged(log, "(test) testwake checked", n2, 10); m = post(b, "/api/plugins/testwake/remove", "remove"); t.join()
        check(seen and m[0] == 200 and res["p"][0] == 1 and "no longer what was accepted" in res["p"][1] and not os.path.exists(os.path.join(state, "plugins", "testwake")),
              f"the same under a push: removed between its check and its install, the push is refused and installs nothing ({res['p'][1]})")

        # ==== the list's pace is the Echo's: a page's own look once an hour whatever came of it, a press every 2 s here
        proc.terminate(); proc.wait()
        proc = start(fresh="3600:2"); wait_port(PORT, WEB, OTA)
        hits = lambda: len([h for h in site.hits if not h.startswith("/repos/")])
        ask = lambda how: (post(b, "/api/plugins/index", how), until(lambda: (lambda o: o if o.get("state") not in ("looking", None) else None)(get(b, "/api/plugins/index")), 20) or {})[1]
        site.down = True; site.hits.clear()
        o = ask("look"); n = hits()
        check(o.get("state") == "unreachable" and n >= 1, f"a page opened, the index out of reach: asked, and said so: {o.get('state')} {n}")
        o = ask("look"); o = ask("look"); o = ask("again")
        check(hits() == n and o.get("state") == "unreachable" and o.get("error") and o.get("age", -1) >= 0, f"opened again and again, and \"Look again\" within its 2 s: the index is not asked, the failure stands with its reason and age: {o} {hits()}")
        site.down = False; time.sleep(2.2)
        o = ask("look")
        check(hits() == n and o.get("state") == "unreachable", "a page's own look after those 2 s: still not, the hour is the Echo's rule")
        o = ask("again"); n2 = hits()
        check(n2 > n and o.get("state") == "ok", f"\"Look again\" after its 2 s: asked: {o.get('state')}")
        o = ask("look"); o = ask("again")
        check(hits() == n2, "and then neither asks again at once")
        st, r = post(b, "/api/plugins/index", "anything")
        check(st == 400 and hits() == n2, f"anything else is no look: {r}")
    finally:
        proc.terminate(); proc.wait(); site.shutdown()
        if check.failed: print(open(log, errors="replace").read()[-8000:])
        subprocess.run(["rm", "-rf", top])
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


main()
