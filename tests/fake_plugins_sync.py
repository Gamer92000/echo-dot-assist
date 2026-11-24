#!/usr/bin/env python3
"""A plugin copied from one Echo to another: what the owner accepted with the button on one is installed on the next
without its button (web.c "sync"; docs/plugins.md 5.4 "the acceptance travels").  Three build/hassmic-host under
build/plugrun-host: Src and Tgt in one arbitration network (its key is the test's too, so the test can write the
vouchers a member that was taken over could), Out in another.  The settings page as tests/webclient.py's browser,
which does what web/plugins.js does: the package read from the source member by member and packed again, uploaded
to the target, the target's nonce, the source's voucher, the install; then the settings and the files.

The whole way: Tgt installs without a press, `accepted` says "synced, voucher named echo-src", the log, the page's
note (kept over a restart) and Home Assistant's "Plugin" event `synced` say so; settings and an item with its
fields follow; a secret does not, no answer of the source carries it, and none is taken in clear; an item both
have has one digest.
The announcement: a note is not taken off by the browser that brought the plugin, nor by one let in on a voucher:
their "dismiss" arms a wait for Tgt's action button HELD for a second (SIGURG here), which takes off the notes that
were there when it was armed and not one that came after; a press while it is armed goes where it would have gone
(the assistant; a Drop In call that rings is answered by it), and the hold that serves it is no "Button" event; a
browser let in with Tgt's own button that did not bring the plugin dismisses at once.  That wait is armed three
times an hour on an Echo (HASSMIC_PLUGIN_NOTE_AGAIN: the hour), whoever asks and for whichever notes, also over a
restart.  Notes are never pushed out: at the bound (HASSMIC_PLUGIN_NOTES, 3 here) a further sync wants the button.
The note is on the flash before the install: a "pending" line left by a hassmic that ended in between stands when
the package it was written for is in the plugin's folder and goes when it is not.  A package is in its folder whole
or not at all (an update that fails between its program and its manifest, HASSMIC_PLUGIN_FAIL_PACKAGE), a program
that is not its manifest's is not started, and no version older than one that was installed on Tgt before comes on
a voucher (state/plugins/.installed).  More "done" lines in the notes' file than notes stand; the file as a FIFO.  The event `synced` is held while Home Assistant has no link, in
the run and (through the note) over a restart, and is not said twice.  With plugins switched off on Tgt, and for a
version older than one Tgt has seen listed in the plugin index (state/plugins/.listed), it is the button.
What does not go without Tgt's own button, each ending in "button" with why, nothing installed, and the button then
working: a voucher used twice, one for another Echo, for another browser, for another plugin, for another manifest,
from an Echo outside the network, from a member Tgt has not heard, a nonce run out, Kiosk Satellite mode (either
end), a sideload (also with a voucher the network's key signs: the taken-over member).  A browser approved on the
source only gets nowhere on the target.  Updates: what Tgt accepted or less needs neither button nor voucher; wider
than Tgt accepted goes on the source's voucher; a plugin installed on Tgt under another author's key is never
replaced; never back.  The install slot: another browser's upload during a synced install is refused, and the same
browser's second package in that window leaves the synced install overtaken, nothing installed.
HASSMIC_PLUGIN_NONCE_MS: a nonce's life, short; HASSMIC_PLUGIN_ASK_DELAY: the window between the install's check
and its decision, as tests/fake_plugins_race.py uses it."""
import asyncio, base64, hashlib, io, json, os, resource, shutil, signal, subprocess, sys, tarfile, tempfile, threading, time
from aioesphomeapi import APIClient, Event
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser, req
import plugsite

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk"))
import plugcheck
ARB, KIOSK = 16869, 16868
NONCE_MS, ASK_MS = 4000, 600
MEMBERS = ("manifest.json", "manifest.sig", "plugin", "LICENSE", "listing.sig")


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond: check.failed = True
check.failed = False


def until(f, secs=10):
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


class Echo:
    def __init__(self, top, name, port, net, **env):
        self.name, self.port, self.web = name, port, port + 1
        self.node = name.lower().replace(" ", "-")
        self.state = os.path.join(top, self.node); os.mkdir(self.state); self.log = os.path.join(top, self.node + ".log")
        # (community plugins on, as the Plugins page's switch leaves them: a sideload is to ask for the button, not be refused; fake_plugins_community.py has the switch)
        open(os.path.join(self.state, "config"), "a").write("plugins_community=on\n")
        nid, key = net
        with open(os.path.join(self.state, "arbitration"), "w") as f: f.write(f"join 1\nctr 0\nnet {nid:016x} {base64.b64encode(key).decode()}\n")
        self.env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(self.state, "play.raw"),
                        HASSMIC_MDNS_FILE=os.path.join(self.state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_ARB_SCALE="0.1", HASSMIC_KIOSK_PORT=str(KIOSK),
                        HASSMIC_MODELS=os.path.join(self.state, "models"), HASSMIC_FAKE_WHISPER="1", HASSMIC_LOG=self.log, HASSMIC_CLOCK_SYNCED="0",
                        HASSMIC_PLUGINS_PUB=os.path.join(top, "index.pub"), HASSMIC_PLUGIN_RETRY="3000",
                        HASSMIC_LOGIN_MS="60000:300",           # browsers log in right after a press here (web.c keeps other browsers back after one: 0.3 s, not 30)
                        HASSMIC_PLUGIN_NOTE_AGAIN="1",          # (waits for the hold that takes notes off: three in this time, so any number here; the hour is the last part's)
                        **env)

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/plugrun-host", "-L", "--", os.environ.get("HASSMIC_BIN", f"{ROOT}/build/hassmic-host"), "-P", "esphome", "-p", str(self.port),
                                      "-n", self.name, "-L", "-z", "0", "-o", "0", "-a", str(ARB), "-i", str(self.port + 10), "-W", str(self.web)], env=self.env, stderr=open(self.log, "a"),
                                     preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        wait_port(self.port, self.web)

    def stop(self):
        self.proc.terminate()
        try: self.proc.wait(5)
        except Exception: self.proc.kill()

    def text(self): return open(self.log, errors="replace").read()
    def press(self): self.proc.send_signal(signal.SIGUSR2)
    def held(self): self.proc.send_signal(signal.SIGURG)        # the action button has been down for a second now (main.c: as buttons.c tells it)
    def hold(self): time.sleep(1.05); self.held()               # ... pressed now and held for that second
    def has(self, pid="testwake"): return os.path.exists(os.path.join(self.state, "plugins", pid, "pkg", "manifest.json"))
    def how(self, pid="testwake"):
        try: return next(l[4:].strip() for l in open(os.path.join(self.state, "plugins", pid, "accepted")) if l.startswith("how "))
        except (OSError, StopIteration): return None
    def pressed(self, n0): return "accepted with the button" in self.text()[n0:]


def carried(b, pid="testwake", change=None):
    """the package as the page carries it: the source's members, piece by piece, packed again (web/tar.js's ustar)"""
    # (until it is listed: under the load of every test at once a plugin just installed was not yet, once)
    s = until(lambda: next((p for p in (get(b, "/api/plugins/sync") or {}).get("plugins", []) if p["id"] == pid), None))
    assert s, f"{pid}: not listed for a copy"
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for m in s["pkg"]:
            d = b""
            for off in range(0, m["size"], 150000):
                st, _, piece = b.call("GET", f"/api/plugins/{pid}/pkg/{m['name']}/{off}/{min(150000, m['size'] - off)}")
                assert st == 200, (st, piece)
                d += piece
            if change: d = change(m["name"], d)
            if d is None: continue
            ti = tarfile.TarInfo(m["name"]); ti.size = len(d); t.addfile(ti, io.BytesIO(d))
    return out.getvalue(), s


def mac(webkey, target, nonce, browser, node, pid, manifest, author, accepted):
    """plugin_store.c ps_voucher, from its description"""
    return hashlib.blake2b(b"hassmic plugin acceptance 1" + bytes.fromhex(target) + bytes.fromhex(nonce) + browser + node.encode() + b"\0" + pid.encode() + b"\0" +
                           bytes.fromhex(manifest) + bytes.fromhex(author) + bytes.fromhex(accepted), key=webkey, digest_size=16).hexdigest()


def nonce_of(bt):
    st, r = post(bt, "/api/plugins/accept/nonce")
    return r.get("nonce") if st == 200 else None


def issue(bs, bt, nonce, pid="testwake", target=None):
    return post(bs, "/api/plugins/accept/issue", f"{target or bt.hello['pub']} {nonce} {pid}")


def synced(bt, node, nonce, voucher):
    return post(bt, "/api/plugins/install", f"synced {node} {nonce} {voucher}")


def sync(bs, bt, pkg, pid="testwake"):
    """all of it as the page does it: upload and check on the target, its nonce, the source's voucher, the install.
    (check's answer, install's status, install's answer)"""
    st, info = upload(bt, pkg)
    if st != 200: return info, st, info
    n = nonce_of(bt); st, v = issue(bs, bt, n, pid)
    if st != 200: return info, st, v
    st, r = synced(bt, v["node"], n, v["voucher"])
    return info, st, r


async def main():
    top = tempfile.mkdtemp()
    for k in ("author", "index", "stranger"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    man = json.load(open(f"{ROOT}/tests/plugins/testwake.json"))
    made = [0]
    def package(official=True, key="author", program=f"{ROOT}/build/testwake", **changes):
        made[0] += 1
        m = dict(man, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, f"p{made[0]}.hmp")
        plugcheck.pack(mp, program, os.path.join(top, key + ".key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        if official:
            plugcheck.list_package(out, os.path.join(top, "index.key"), out + ".o", arch="x86_64")
            out += ".o"
        return open(out, "rb").read()
    netkey = os.urandom(32); webkey = hashlib.blake2b(b"hassmic web voucher 1", key=netkey, digest_size=32).digest()
    src = Echo(top, "Echo Src", 16861, (0x51, netkey))
    failfloor = os.path.join(top, "failfloor")         # while it exists Tgt cannot write down the versions installed there
    tgt = Echo(top, "Echo Tgt", 16863, (0x51, netkey), HASSMIC_PLUGIN_NONCE_MS=str(NONCE_MS), HASSMIC_PLUGIN_ASK_DELAY=str(ASK_MS), HASSMIC_PLUGIN_NOTES="3",
               HASSMIC_PLUGIN_FAIL_FLOOR=failfloor)
    out = Echo(top, "Echo Out", 16865, (0x77, os.urandom(32)))
    echos = [src, tgt, out]
    ha = None
    def with_button(e, b, pkg, what):
        """installed on E the ordinary way: uploaded, checked, asked for, the action button pressed"""
        st, r = upload(b, pkg); st2, r2 = post(b, "/api/plugins/install")
        if r2.get("install") == "waiting": e.press()
        ok = st == 200 and st2 == 200 and until(lambda: get(b, "/api/plugins").get("install", {}).get("state") == "installed")
        check(ok, f"{what}: {r.get('error') or r2}")
        return ok
    def remove(b, pid="testwake"):
        # (the notes: by the browser Tgt's own button let in, which brought none of them)
        post(b, f"/api/plugins/{pid}/remove", "remove"); post(bown, "/api/plugins/synced", "dismiss all")
    notes = lambda b: get(b, "/api/plugins").get("synced", [])
    async def ha_connect():
        c = None
        for _ in range(40):
            try: c = APIClient("127.0.0.1", tgt.port, None); await c.connect(login=True); break
            except Exception: c = None; await asyncio.sleep(0.25)
        es, _ = await c.list_entities_services(); got = []; c.subscribe_states(got.append)
        return c, got
    def wants_button(what, st, r, why, pid="testwake"):
        """refused as a sync: the answer says "button" and why, nothing is installed and nothing waits for a press"""
        ok = st == 200 and r.get("install") == "button" and why in r.get("why", "") and not tgt.has(pid) and get(bt, "/api/plugins").get("install", {}).get("state") == "checked"
        check(ok, f"{what}: this Echo's own button ({r.get('why') or r})")
        return ok
    def then_button(what, pid="testwake"):
        """... and the package, still checked, goes in the ordinary way: asked for, pressed"""
        n0 = len(tgt.text()); st, r = post(bt, "/api/plugins/install")
        if r.get("install") == "waiting": tgt.press()
        check(st == 200 and r.get("install") == "waiting" and until(lambda: get(bt, "/api/plugins").get("install", {}).get("state") == "installed") and tgt.has(pid)
              and tgt.how(pid) == "button" and tgt.pressed(n0), f"{what}: then its button installs it, accepted as \"button\" ({r}, {tgt.how(pid)})")
    try:
        for e in echos: e.start()
        bs = Browser(src.web); check(bs.login_with_button(src.proc), "Src: a browser approved with its button")
        heard = lambda b, node: any(m["node"] == node for m in ((b.state() or {}).get("arbitration") or {}).get("members", []))
        check(until(lambda: heard(bs, "echo-tgt"), 20), "Src and Tgt are members of one network and have heard each other")
        bt = bs.at(tgt.web); check(bt.through(bs) == "approved", "Tgt: the same browser let in through Src (a member vouches for it, as the page does)")
        check(until(lambda: heard(bt, "echo-src"), 20), "(Tgt has heard Src)")
        bo = bs.at(out.web); check(bo.login_with_button(out.proc), "Out, an Echo of another network: the browser approved with its own button")
        bown = Browser(tgt.web); check(bown.login_with_button(tgt.proc), "Tgt: another browser, let in with Tgt's own button")
        # (and a third, for later: no other browser's login is armed in the time after a wait was withdrawn, a minute here)
        time.sleep(0.4); bx = Browser(tgt.web); check(bx.login_with_button(tgt.proc), "Tgt: a third browser let in with its button")
        cl = {c["pub"]: c for c in (bown.state() or {}).get("clients", [])}
        check(cl.get(bt.pub.hex(), {}).get("vouched") is True and cl.get(bown.pub.hex(), {}).get("vouched") is False, f"Tgt knows which of the two came in on a voucher: {[(c['label'], c.get('vouched')) for c in cl.values()]}")
        for b in (bs, bt, bo): b.set(plugins="on")
        # (a plugin with `button` on Tgt: it hears what Home Assistant's "Button" hears, and so never the hold
        # that dismisses a note, below; tests/fake_plugins_button.py has the rest of it)
        bman = json.load(open(f"{ROOT}/tests/plugins/testbutton.json"))
        with_button(tgt, bt, package(program=f"{ROOT}/build/testbutton", **bman), "Tgt: a plugin that hears the buttons, installed with its button")
        check(until(lambda: (plug(bt, "testbutton") or {}).get("state") == "running", 20), "... and it runs there")
        bheard = lambda n0: [l.split("plugin testbutton: ", 1)[1] for l in tgt.text()[n0:].splitlines() if "plugin testbutton: button " in l]
        # (Drop In between Src and Tgt, answered with the button: for the press that a browser makes happen, below.
        # Switched on here, before Home Assistant connects: a feature switched closes its links)
        bt.set(drop_in="on", drop_in_answer="ask"); bs.set(drop_in="on")
        ha = None
        for _ in range(40):
            try: ha = APIClient("127.0.0.1", tgt.port, None); await ha.connect(login=True); break
            except Exception: ha = None; await asyncio.sleep(0.25)
        es, _ = await ha.list_entities_services(); states = []; ha.subscribe_states(states.append)
        ev = next(e for e in es if e.object_id == "plugin")
        evs = lambda: [s.event_type for s in states if isinstance(s, Event) and s.key == ev.key]
        async def heard_event(n, what, secs=8):
            end = time.monotonic() + secs
            while time.monotonic() < end and len(evs()) < n: await asyncio.sleep(0.05)
            return len(evs()) == n and evs()[-1] == what

        # ---- the source: installed and accepted with its button, a setting, a secret, an item with its fields
        v1 = package()
        with_button(src, bs, v1, "Src: version 1, official, installed with its own button")
        check(until(lambda: (plug(bs) or {}).get("state") == "running", 20), "... and it runs there")
        post(bs, "/api/plugins/testwake/set", "note=from the kitchen")
        SECRET = b"s3cret-of-src-0451"
        st, _, _ = bs.call("POST", "/api/plugins/testwake/secret/pin", lambda c: bs.seal(c, SECRET + b"\0" * (64 - len(SECRET))).hex().encode())
        check(st == 200 and next(s for s in plug(bs)["settings"] if s["name"] == "pin").get("set") is True, "Src: a secret is set")
        seen = []                                       # every answer of the source from here on: the secret is in none
        call0 = bs.call
        def call(*a, **k):
            r = call0(*a, **k); seen.append(r[2]); return r
        bs.call = call
        for path, body in (("begin", b""), ("chunk/.model/0", b"MODEL Burst of sound!\n"), ("chunk/.json/0", b'{"a":1}'), ("done", b"")): st, r = post(bs, f"/api/plugins/testwake/item/models/burst/{path}", body)
        check(st == 200 and r.get("checked") is True, f"Src: an item of two files, checked by the plugin: {r}")
        post(bs, "/api/plugins/testwake/item/models/burst/field", "label=Kitchen burst"); post(bs, "/api/plugins/testwake/item/models/burst/field", "level=1234")

        # ---- the whole way, as the page does it
        pkg, ssync = carried(bs)
        check(set(plugcheck.members(os.path.join(top, f"p{made[0]}.hmp.o"))) == {m["name"] for m in ssync["pkg"]} and
              hashlib.blake2b(plugcheck.members(os.path.join(top, f"p{made[0]}.hmp.o"))["manifest.json"], digest_size=32).hexdigest() == ssync["manifest"],
              "the source hands its package out member by member, and names it by its manifest's digest")
        st, info = upload(bt, pkg)
        check(st == 200 and info.get("tier") == "official" and info.get("button") is True and info.get("update") is False, f"packed again and uploaded to Tgt: its check takes it for the official package, new here: the button: {info.get('error') or (info.get('tier'), info.get('button'))}")
        n1 = nonce_of(bt); st, vch = issue(bs, bt, n1)
        check(st == 200 and vch.get("node") == "echo-src" and vch.get("manifest") == ssync["manifest"], f"Tgt's nonce, Src's voucher for it: {vch}")
        check(vch.get("voucher") == mac(webkey, bt.hello["pub"], n1, bs.pub, "echo-src", "testwake", vch["manifest"], vch["key"], vch["accepted"]),
              "the voucher is the MAC its description gives, made here in Python with the network's page key")
        n0 = len(tgt.text()); st, r = synced(bt, vch["node"], n1, vch["voucher"])
        check(st == 200 and r.get("install") == "installed" and tgt.has() and not tgt.pressed(n0), f"Tgt installs it without a press of its button: {r}")
        check(tgt.how() == "synced, voucher named echo-src" and (plug(bt) or {}).get("how") == "synced, voucher named echo-src" and plug(bt).get("accepted") is True, f"`accepted` on Tgt says how, and of the Echo only that the voucher named it: {tgt.how()}")
        check(until(lambda: (plug(bt) or {}).get("state") == "running", 20), f"and it runs on Tgt: {(plug(bt) or {}).get('state')} {(plug(bt) or {}).get('detail')}")
        said = tgt.text()[n0:]
        check("WITHOUT this Echo's action button" in said and "a voucher of this network that named echo-src" in said and "not proof of where the plugin came from" in said and f"(key {bt.pub.hex()[:16]}...)" in said,
              "said in Tgt's log: without the button, the Echo the voucher named and that this is no proof, the browser by label and key")
        note = notes(bt)
        check(len(note) == 1 and note[0]["id"] == "testwake" and note[0]["node"] == "echo-src" and note[0]["name"] == "Test wake word" and "test browser via Echo Src" in note[0]["by"] and note[0]["version"] == 1,
              f"the page's note: which plugin, which Echo the voucher named, which browser: {note}")
        check(note[0]["at"] == 0 and note[0]["n"] > 0, f"its time is 0 where the clock was not set (HASSMIC_CLOCK_SYNCED=0), not a date in 1970; it has a number to dismiss it by: {note[0]}")
        check(note[0]["dismiss"] == "button" and notes(bown)[0]["dismiss"] == "now",
              "the note says who can take it off by a request: not the browser that brought the plugin, the one Tgt's button let in can")
        check(note[0].get("key") == bt.pub.hex()[:16] and any(c["pub"][:16] == note[0]["key"] and c["me"] for c in bt.state()["clients"]),
              f"the note names the browser by the first eight bytes of its key too, which its label's author did not choose: the same as in the list of browsers ({note[0].get('key')})")
        check((bt.state() or {}).get("plugins_synced") == note, "... on every page (the state has it too, which the other Echos' pages read)")
        line = open(os.path.join(tgt.state, "plugins", ".synced")).read()
        check(bt.pub.hex() in line and " done " in line, "the note's file names the browser by its key, and the install as done")
        check(await heard_event(1, "synced"), f"Home Assistant: the \"Plugin\" entity's event `synced`, once, and no `installed` beside it: {evs()}")
        check("vouched to another Echo that testwake was accepted here" in src.text(), "Src's log says it vouched")

        # settings: every row but the secret; the secret stays where it is
        rows = lambda b: {s["name"]: s for s in plug(b)["settings"]}
        for name, s in rows(bs).items():
            if s["type"] not in ("files", "secret") and rows(bt)[name].get("value") != s["value"]: post(bt, f"/api/plugins/testwake/set", f"{name}={s['value']}")
        check(rows(bt)["note"]["value"] == "from the kitchen" and rows(bt)["mode"]["value"] == rows(bs)["mode"]["value"], "the settings follow, row by row")
        check(rows(bs)["pin"].get("set") is True and "value" not in rows(bs)["pin"] and rows(bt)["pin"].get("set") is False,
              "the secret does not: the source never says it, and Tgt's row is \"not set here\" (set: false)")
        st, r = post(bt, "/api/plugins/testwake/set", "pin=sent-in-clear")
        check(st == 400 and "not taken in clear" in r.get("error", "") and rows(bt)["pin"].get("set") is False and b"sent-in-clear" not in open(os.path.join(tgt.state, "plugins", "testwake", "config"), "rb").read(),
              f"a secret sent in clear as a setting is refused and not kept: {r}")
        st, _, _ = bt.call("POST", "/api/plugins/testwake/secret/pin", lambda c: bt.seal(c, b"tgt-own" + b"\0" * 57).hex().encode())
        check(st == 200 and rows(bt)["pin"].get("set") is True, "sealed, as the page sends it, it is taken")
        # files: by digest; an item both have is not sent again
        digests = lambda b: next(p for p in get(b, "/api/plugins/sync")["plugins"] if p["id"] == "testwake")["items"]
        ds = digests(bs)
        check(list(ds) == ["models/burst"] and digests(bt) == {}, f"the source's item by its digest, none on Tgt yet: {ds}")
        it = rows(bs)["models"]["items"][0]
        post(bt, "/api/plugins/testwake/item/models/burst/begin")
        for f in it["files"]:
            st, _, data = bs.call("GET", f"/api/plugins/testwake/file/models/burst/{f['suffix']}")
            post(bt, f"/api/plugins/testwake/item/models/burst/chunk/{f['suffix']}/0", data)
        st, r = post(bt, "/api/plugins/testwake/item/models/burst/done")
        for k, v in it["fields"].items(): post(bt, f"/api/plugins/testwake/item/models/burst/field", f"{k}={v}")
        ti = rows(bt)["models"]["items"]
        check(st == 200 and r.get("checked") is True and len(ti) == 1 and ti[0]["fields"] == it["fields"] == {"label": "Kitchen burst", "level": "1234"} and ti[0]["title"] == it["title"],
              f"the item goes through Tgt's own upload and the plugin's own check there, its fields after it: {r} {ti}")
        check(digests(bt) == ds, "now both have it under one digest: the page has nothing of it to send again")
        post(bs, "/api/plugins/testwake/item/models/burst/begin"); post(bs, "/api/plugins/testwake/item/models/burst/chunk/.model/0", b"MODEL Burst, another take\n"); post(bs, "/api/plugins/testwake/item/models/burst/done")
        check(digests(bs) != ds and digests(bs)["models/burst"] != digests(bt)["models/burst"], "another file under the same name on Src is another digest")
        bs.call = call0
        check(not any(SECRET in x or SECRET.hex().encode() in x for x in seen) and len(seen) > 12, f"none of Src's {len(seen)} answers on the way carried the secret")

        # ---- the announcement: who takes a note off, and how
        b2s = Browser(src.web); b2s.login_with_button(src.proc); b2t = b2s.at(tgt.web); check(b2t.through(b2s) == "approved", "(a second browser, approved on Src and let in on Tgt through it)")
        st, r = post(bt, "/api/plugins/synced", f"dismiss {note[0]['n']}")
        check(st == 200 and r == {"dismiss": "waiting", "holds": "note"} and notes(bt) == note and get(bt, "/api/plugins")["install"]["holds"] == "note",
              f"the browser that brought the plugin asks to dismiss its note: not taken off, Tgt waits for its action button: {r}")
        st, r = post(b2t, "/api/plugins/synced", "dismiss all")
        check(st == 200 and r == {"dismiss": "busy", "holds": "note"}, f"another browser's ask meanwhile: busy, the armed wait is as it was: {r}")
        # the wait is the hold's: a press while it is armed is the assistant's, as without it (the browser that armed
        # the wait can make a press happen: a call rung on this Echo, below)
        n0 = len(tgt.text()); tgt.press()
        check(until(lambda: "button: action\n" in tgt.text()[n0:], 3) and notes(bt) == note and get(bt, "/api/plugins")["install"]["holds"] == "note" and "dismissed" not in tgt.text()[n0:]
              and until(lambda: bheard(n0) == ["button 1 1"], 3),
              "a press of the action button while that wait is armed: the assistant's, as any press (and the button plugin's); the note stands and the wait is still armed")
        st, r = post(bt, "/api/plugins/synced", "keep"); n0 = len(tgt.text()); tgt.hold(); time.sleep(0.6)
        check(st == 200 and notes(bt) == note and "withdrawn (kept)" in tgt.text() and "dismissed" not in tgt.text()[n0:] and "button: action_hold" in tgt.text()[n0:]
              and until(lambda: bheard(n0) == ["button 1 2"], 3),
              "\"keep\" withdraws the wait: the button held then takes nothing off, and is Home Assistant's hold event (and the button plugin's) as ever")
        # a call rung on Tgt by the very browser whose wait is armed: the press answers the call
        dstate = lambda b: ((b.state() or {}).get("dropin") or {}).get("state")
        st, r = post(bt, "/api/plugins/synced", f"dismiss {note[0]['n']}"); st2, c = post(bs, "/api/dropin", "echo-tgt")
        rang = until(lambda: dstate(bt) == "ringing", 5); n0 = len(tgt.text()); tgt.press()
        check(r.get("dismiss") == "waiting" and rang and until(lambda: dstate(bt) not in ("ringing", "idle", None), 5) and notes(bt) == note and "dismissed" not in tgt.text()[n0:],
              f"a Drop In rung on Tgt while its browser's wait for the note is armed: the press answers the call ({dstate(bt)}), it does not dismiss the note ({r}, {c})")
        post(bs, "/api/dropin", "end"); until(lambda: dstate(bt) == "idle", 5); post(bt, "/api/plugins/synced", "keep")
        st, r = post(b2t, "/api/plugins/synced", "dismiss all")
        check(st == 200 and r.get("dismiss") == "waiting" and notes(bt) == note, f"a second browser that came in on a voucher (a member's key makes as many as it likes): the button too: {r}")
        # ... and what the press takes off is what was there when it was armed: a plugin copied meanwhile keeps its note
        other = package(id="otherplug", name="Other")
        with_button(src, bs, other, "Src: another official plugin, installed with its button")
        opkg, _ = carried(bs, "otherplug"); info, st, r = sync(bs, bt, opkg, "otherplug")
        check(st == 200 and r.get("install") == "installed" and [x["id"] for x in notes(bt)] == ["testwake", "otherplug"], f"(while that wait is armed, another plugin is copied to Tgt: two notes, none replaced: {r} {notes(bt)})")
        newer = next(x["n"] for x in notes(bt) if x["id"] == "otherplug")
        st, r = post(b2t, "/api/plugins/synced", f"dismiss {newer}"); st, r2 = post(b2t, "/api/plugins/synced", "dismiss all"); st, r3 = post(b2t, "/api/plugins/synced", f"dismiss {note[0]['n']}")
        check(r == {"dismiss": "busy", "holds": "note"} and r2 == {"dismiss": "busy", "holds": "note"} and r3 == {"dismiss": "waiting", "holds": "note"},
              f"the browser whose wait is armed asks about the note that came after, or about all: not \"waiting\" (the hold will not take that one), busy with its own wait; about the one it was armed for: waiting ({r} {r2} {r3})")
        n0 = len(tgt.text()); tgt.hold()
        check(until(lambda: [x["id"] for x in notes(bt)] == ["otherplug"]) and "testwake version 1 came here without this Echo's action button was dismissed with the action button held" in tgt.text()[n0:]
              and "button: action_hold" not in tgt.text()[n0:] and (time.sleep(1), bheard(n0))[1] == [],
              f"the button held: the note it was armed for is gone, the one that came after it stays, and Home Assistant's \"Button\" hears no hold, nor does the button plugin: {notes(bt)} {bheard(n0)}")
        # the note survives a restart of Tgt (and who came in on a voucher is still known)
        note = notes(bt)
        await ha.disconnect(force=True); ha = None
        tgt.stop(); tgt.start()
        check(until(lambda: notes(bt) == note), "after a restart of Tgt the note is still there")
        st, r = post(bt, "/api/plugins/synced", "dismiss all")
        check(st == 200 and r.get("dismiss") == "waiting" and notes(bt) == note, f"and its browser still cannot take it off by a request: {r}")
        post(bt, "/api/plugins/synced", "keep")
        st, r = post(bown, "/api/plugins/synced", f"dismiss {note[0]['n']}")
        check(st == 200 and r.get("dismiss") == "done" and notes(bt) == [] and not os.path.exists(os.path.join(tgt.state, "plugins", ".synced")) and "let in with this Echo's button" in tgt.text(),
              f"the browser Tgt's own button let in, which did not bring it: dismissed at once, gone from the file too: {r}")
        st, r = post(bown, "/api/plugins/synced", "dismiss 99")
        check(st == 200 and r.get("dismiss") == "none", f"(no such note: {r})")
        st, _, _ = req(tgt.web, "POST", "/api/plugins/synced", b"dismiss all")
        check(st == 401, "(dismissing is an approved browser's: an unsigned request is turned away)")
        check(until(lambda: heard(bt, "echo-src"), 20), "(Tgt hears Src again)")
        remove(bt, "otherplug")
        # a browser of the button's that brought the plugin itself does not dismiss its own note either
        bxs = bx.at(src.web); check(bxs.through(bx) == "approved" or bxs.login_with_button(src.proc), "(a browser let in with Tgt's button, and on Src)")
        remove(bt); info, st, r = sync(bxs, bx, pkg)
        check(st == 200 and r.get("install") == "installed" and notes(bx)[0]["dismiss"] == "button" and notes(bown)[0]["dismiss"] == "now", f"(it copies testwake to Tgt: {r})")
        st, r = post(bx, "/api/plugins/synced", "dismiss all")
        check(st == 200 and r.get("dismiss") == "waiting" and len(notes(bx)) == 1, f"its own note: the button, though Tgt's button let this browser in: {r}")
        post(bx, "/api/plugins/synced", "keep")

        # ---- none pushed out: at the bound the next one wants the button (3 here)
        for i in range(2):
            post(bt, "/api/plugins/testwake/remove", "remove"); info, st, r = sync(bs, bt, pkg)
            check(st == 200 and r.get("install") == "installed" and len(notes(bt)) == 2 + i, f"(copied again: {2 + i} notes, each kept: {r})")
        post(bt, "/api/plugins/testwake/remove", "remove"); first = [x["n"] for x in notes(bt)]
        info, st, r = sync(bs, bt, pkg)
        wants_button("three notes nobody dismissed, and a fourth plugin copied", st, r, "3 plugins were copied to this Echo without its action button")
        check([x["n"] for x in notes(bt)] == first, "and the three notes are the three there were: none made room")
        post(bt, "/api/plugins/cancel"); remove(bt)

        # ---- plugins switched off on Tgt: its button
        st, info = upload(bt, pkg); bt.set(plugins="off")
        n = nonce_of(bt); st, v = issue(bs, bt, n); st, r = synced(bt, "echo-src", n, v["voucher"])
        wants_button("plugins switched off on Tgt", st, r, "plugins are switched off on this Echo")
        bt.set(plugins="on"); post(bt, "/api/plugins/cancel")

        # ---- Home Assistant's event is held until its link is up: in the run ...
        info, st, r = sync(bs, bt, pkg)
        check(st == 200 and r.get("install") == "installed" and "testwake synced (held for Home Assistant until its link is up)" in tgt.text(), f"copied while Home Assistant has no link: the event is held: {r}")
        ha, states = await ha_connect()
        await heard_event(4, "synced"); await asyncio.sleep(0.5)
        check(evs() == ["synced"] * 4, f"Home Assistant connects: `synced` comes then, once for each of the four plugins copied since it had no link (the refused one not among them): {evs()}")
        await asyncio.sleep(1.5)                        # (the note is marked told within a second)
        await ha.disconnect(force=True); ha = None
        tgt.stop(); tgt.start()
        ha, states = await ha_connect(); await asyncio.sleep(2)
        check(evs() == [] and len(notes(bt)) == 1, f"after a restart it is not said a second time (the note is still there): {evs()}")
        await ha.disconnect(force=True); ha = None
        # ... and over a restart, through the note
        check(until(lambda: heard(bt, "echo-src"), 20), "(Tgt hears Src again)")
        remove(bt); info, st, r = sync(bs, bt, pkg)
        tgt.stop(); tgt.start()
        ha, states = await ha_connect()
        check(st == 200 and r.get("install") == "installed" and await heard_event(1, "synced"), f"copied without a link, then Tgt restarted: Home Assistant hears `synced` from the note when it connects: {r} {evs()}")
        check(until(lambda: heard(bt, "echo-src"), 20), "(Tgt hears Src again)")

        # ---- what does not go without Tgt's button
        remove(bt); pkg, _ = carried(bs)
        # the same voucher again
        st, info = upload(bt, pkg); st, r = synced(bt, "echo-src", n1, vch["voucher"])
        wants_button("the voucher of before, a second time (its nonce is used up)", st, r, "used before")
        # a voucher Src made for another Echo
        n = nonce_of(bt); st, v = issue(bs, bt, n, target=bo.hello["pub"]); st, r = synced(bt, "echo-src", n, v["voucher"])
        wants_button("a voucher Src made for another Echo's key", st, r, "for this Echo, this browser and this very package")
        # the nonce again after a refusal: used up whatever came of it
        st, v = issue(bs, bt, n); st, r = synced(bt, "echo-src", n, v["voucher"])
        wants_button("a good voucher for a nonce a refused try used up", st, r, "used before")
        # another browser's voucher
        n = nonce_of(bt); st, v = issue(b2s, b2t, n); st, r = synced(bt, "echo-src", n, v["voucher"])
        wants_button("a voucher Src gave another browser", st, r, "this browser")
        n2 = nonce_of(b2t); st, v = issue(bs, bt, n2); st, r = synced(bt, "echo-src", n2, v["voucher"])
        wants_button("a nonce Tgt gave another browser", st, r, "not one this Echo gave this browser")
        # a nonce that ran out
        n = nonce_of(bt); st, v = issue(bs, bt, n); time.sleep(NONCE_MS / 1000 + 0.3); st, r = synced(bt, "echo-src", n, v["voucher"])
        wants_button(f"a nonce older than {NONCE_MS / 1000:.0f} s", st, r, "older than")
        # from a member Tgt has not heard: the network's key signs it (the test's copy), the node is nobody's
        n = nonce_of(bt); ghost = mac(webkey, bt.hello["pub"], n, bt.pub, "echo-ghost", "testwake", vch["manifest"], vch["key"], vch["accepted"]); st, r = synced(bt, "echo-ghost", n, ghost)
        wants_button("a voucher under the network's key in the name of an Echo Tgt has never heard", st, r, "echo-ghost is no Echo of this network that this Echo has heard")
        n = nonce_of(bt); own = mac(webkey, bt.hello["pub"], n, bt.pub, "echo-tgt", "testwake", vch["manifest"], vch["key"], vch["accepted"]); st, r = synced(bt, "echo-tgt", n, own)
        wants_button("... and in Tgt's own name", st, r, "is no Echo of this network")
        await asyncio.sleep(0.05)
        # from an Echo of another network: its voucher is under another key
        with_button(out, bo, v1, "Out: version 1 installed with its own button")
        n = nonce_of(bt); st, v = issue(bo, bt, n); st2, r = synced(bt, v.get("node", "echo-out"), n, v.get("voucher", "00" * 16))
        check(st == 200 and v.get("node") == "echo-out", f"(Out vouches, for its own network: {v})")
        wants_button("a voucher of an Echo outside the network", st2, r, "not one of this network")
        # Kiosk Satellite mode, either end
        bt.set(arbitration_mode="kiosk")
        n = nonce_of(bt); st, v = issue(bs, bt, n); st, r = synced(bt, "echo-src", n, v["voucher"])
        wants_button("Tgt in Kiosk Satellite mode", st, r, "Kiosk Satellite way")
        bt.set(arbitration_mode="hassmic"); bs.set(arbitration_mode="kiosk")
        n = nonce_of(bt); st, v = issue(bs, bt, n)
        check(st == 400 and "Kiosk Satellite way" in v.get("error", ""), f"Src in Kiosk Satellite mode vouches for nothing: {v}")
        mode = lambda: next((m.get("mode") for m in (bt.state() or {}).get("arbitration", {}).get("members", []) if m["node"] == "echo-src"), None)
        check(until(lambda: mode() == "kiosk", 15), f"(Tgt has heard that Src is in that mode: {mode()})")
        n = nonce_of(bt); k = mac(webkey, bt.hello["pub"], n, bt.pub, "echo-src", "testwake", vch["manifest"], vch["key"], vch["accepted"]); st, r = synced(bt, "echo-src", n, k)
        wants_button("... and a voucher in its name all the same", st, r, "echo-src settles wake words the Kiosk Satellite way")
        bs.set(arbitration_mode="hassmic")
        check(until(lambda: mode() == "hassmic", 15), f"(Src back in the Echos' own mode, and Tgt has heard it: {mode()})")
        st, v = issue(bs, bt, n, pid="nothere")
        check(st == 400 and "nothing of it to vouch for" in v.get("error", ""), f"Src vouches for no plugin it has not got: {v}")
        # ... and after all of these the package is still there, checked, and Tgt's own button installs it
        then_button("after every refusal")
        check(get(bt, "/api/plugins").get("synced") == [], "no note: nothing came without the button")
        remove(bt)

        await asyncio.sleep(0.05)
        # another plugin and another manifest than the voucher names
        st, info = upload(bt, other); n = nonce_of(bt); st, v = issue(bs, bt, n); st, r = synced(bt, "echo-src", n, v["voucher"])
        wants_button("Src's voucher for testwake, and another official plugin uploaded", st, r, "this very package", pid="otherplug")
        v2 = package(version=2, capabilities={"wake": True, "files": 2})
        st, info = upload(bt, v2); n = nonce_of(bt); st, v = issue(bs, bt, n); st, r = synced(bt, "echo-src", n, v["voucher"])
        wants_button("Src's voucher for version 1, and version 2 uploaded (another manifest)", st, r, "this very package")
        post(bt, "/api/plugins/cancel")

        # a browser approved on the source only
        b3 = Browser(src.web); b3.login_with_button(src.proc); b3t = b3.at(tgt.web)
        st, _, _ = b3t.call("POST", "/api/plugins/accept/nonce"); st2, _, _ = b3t.call("POST", "/api/plugins/upload/begin", str(len(pkg)).encode())
        check(st == 401 and st2 == 401 and not tgt.has(), "a browser approved on Src only: Tgt gives it no nonce and takes no package from it")

        await asyncio.sleep(0.05)
        # ---- a sideload never syncs: with Src's honest voucher, and with one the network's key signs
        side = package(official=False, id="sideplug", name="Side")
        with_button(src, bs, side, "Src: a sideload installed with its own button")
        spkg, ss = carried(bs, "sideplug")
        info, st, r = sync(bs, bt, spkg, "sideplug")
        check(info.get("tier") == "sideload", f"(Tgt's check: a sideload: {info.get('tier')})")
        wants_button("a sideload, with Src's own voucher that it was accepted there", st, r, "not listed as official by the plugin index", pid="sideplug")
        then_button("the sideload", pid="sideplug")
        check(get(bt, "/api/plugins").get("synced") == [], "and no note for it")
        # a member that was taken over: the listing taken off the official package, a voucher of its own making
        remove(bt, "sideplug")
        bare, _ = carried(bs, change=lambda n, d: None if n == "listing.sig" else d)
        st, info = upload(bt, bare); n = nonce_of(bt)
        st2, v = issue(bs, bt, n); forged = mac(webkey, bt.hello["pub"], n, bt.pub, "echo-src", "testwake", vch["manifest"], vch["key"], vch["accepted"])
        check(st == 200 and info.get("tier") == "sideload" and forged == v["voucher"], "(the official package without its listing: a sideload to Tgt; the voucher holds all the same)")
        st, r = synced(bt, "echo-src", n, forged)
        wants_button("a voucher that holds, for a package the index key did not list", st, r, "not listed as official by the plugin index")
        evil, _ = carried(bs, change=lambda n, d: d[:-1] + bytes([d[-1] ^ 1]) if n == "plugin" else d)
        st, info = upload(bt, evil)
        check(st == 400 and "not the one the manifest names" in info.get("error", "") and not tgt.has(), f"the official package with another program in it: refused by Tgt's check before any voucher is asked: {info}")

        await asyncio.sleep(0.05)
        # ---- the install slot's rules hold for a synced install
        pkg, _ = carried(bs)
        st, info = upload(bt, pkg); n = nonce_of(bt); st, v = issue(bs, bt, n); res = {}
        t = threading.Thread(target=lambda: res.update(a=synced(bt, "echo-src", n, v["voucher"]))); t.start(); time.sleep(ASK_MS / 1000 / 3)
        m1 = post(b2t, "/api/plugins/upload/begin", str(len(side))); m2 = post(b2t, "/api/plugins/fetch", "testwake"); t.join()
        check(m1[0] == 400 and "is being checked" in m1[1].get("error", "") and m2[0] == 400 and res["a"][1].get("install") == "installed" and tgt.how() == "synced, voucher named echo-src",
              f"another browser's upload and fetch between a synced install's check and its decision are refused, and the sync goes through: {m1[1]} {m2[1]} {res['a']}")
        remove(bt)
        st, info = upload(bt, pkg); n = nonce_of(bt); st, v = issue(bs, bt, n); res = {}
        t = threading.Thread(target=lambda: res.update(a=synced(bt, "echo-src", n, v["voucher"]))); t.start(); time.sleep(ASK_MS / 1000 / 3)
        # (until the eighth read /install looked at the file in CHECKED, where its own browser may begin anew, and the
        # synced install was overtaken; now the slot is nobody's to take from that look on)
        m = upload(bt, side); t.join()
        check(m[0] == 400 and "is being checked" in m[1].get("error", "") and res["a"][0] == 200 and res["a"][1].get("install") == "installed" and tgt.how() == "synced, voucher named echo-src" and not tgt.has("sideplug"),
              f"the same browser's other package in that window: refused, and the synced install is the package the voucher was for: {m[1]} {res['a']}")
        tgt.press(); time.sleep(0.8)
        check(not tgt.has("sideplug") and get(bt, "/api/plugins").get("install", {}).get("state") == "installed", "and a press then installs nothing: none was asked for")
        remove(bt)

        # ---- updates
        await asyncio.sleep(0.3); states.clear()        # (what Home Assistant was sent so far is read only while this waits)
        info, st, r = sync(bs, bt, pkg)
        check(st == 200 and r.get("install") == "installed" and tgt.how() == "synced, voucher named echo-src", f"(version 1 on Tgt again, synced: {r})")
        check(await heard_event(1, "synced"), f"(Home Assistant heard it: {evs()})")
        # the same powers, a higher version: neither button nor voucher is looked at
        with_button(src, bs, package(version=2), "Src: version 2, the same powers")
        p2, _ = carried(bs); n0 = len(tgt.text()); st, info = upload(bt, p2)
        check(st == 200 and info.get("button") is False and info.get("update") is True, f"version 2 on Tgt: what it accepted, no button: {info.get('error') or info.get('button')}")
        st, r = synced(bt, "echo-src", "00" * 16, "00" * 16)
        check(st == 200 and r.get("install") == "installed" and tgt.how() == "update: nothing new to accept" and plug(bt)["version"] == 2 and not tgt.pressed(n0),
              f"installed as any such update, whatever voucher came with it: {r} {tgt.how()}")
        check(await heard_event(2, "updated") and get(bt, "/api/plugins").get("synced", [{}])[0].get("version") == 1, f"Home Assistant: `updated`; the note is still the sync's: {evs()}")
        # wider than Tgt accepted: Src's owner accepted that with a button there, and the voucher carries it
        with_button(src, bs, package(version=3, capabilities={"wake": True, "files": 2}), "Src: version 3, which may keep more files, accepted with Src's button")
        p3, _ = carried(bs); n0 = len(tgt.text()); st, info = upload(bt, p3)
        check(st == 200 and info.get("button") is True and info.get("update") is True and any(c["kind"] == "more" for c in info.get("changes", [])), f"version 3 on Tgt: more than it accepted, the button: {info.get('changes')}")
        st, r = synced(bt, "echo-src", "00" * 16, "00" * 16)
        check(st == 200 and r.get("install") == "button" and plug(bt)["version"] == 2, f"without a voucher that holds: the button, version 2 keeps running: {r}")
        n = nonce_of(bt); st, v = issue(bs, bt, n); st, r = synced(bt, "echo-src", n, v["voucher"])
        check(st == 200 and r.get("install") == "installed" and plug(bt)["version"] == 3 and tgt.how() == "synced, voucher named echo-src" and not tgt.pressed(n0), f"with Src's voucher: installed without Tgt's button: {r}")
        note = get(bt, "/api/plugins").get("synced", [])
        check(await heard_event(3, "synced") and [x["version"] for x in note] == [1, 3], f"announced as the first one was: `synced`, and a note of its own for version 3 beside version 1's: {evs()} {note}")
        # never back
        st, info = upload(bt, p2)
        check(st == 400 and "not newer than the one installed" in info.get("error", ""), f"version 2 after 3: refused by Tgt's check, voucher or not: {info}")
        # a plugin installed on Tgt under another author's key is never replaced by a sync
        remove(bt)
        with_button(tgt, bt, package(official=False, key="stranger", version=1), "Tgt: a testwake by another author, installed with Tgt's button")
        info, st, r = sync(bs, bt, p3)
        check(st == 400 and "another author's key" in info.get("error", "") and plug(bt)["version"] == 1 and tgt.how() == "button", f"Src's testwake does not take its place: refused before any voucher: {info}")
        check(await heard_event(5, "installed") and evs()[-2:] == ["removed", "installed"] and get(bt, "/api/plugins").get("synced") == [], f"nothing synced, nothing said to be: {evs()}")

        # ---- request.audio with a way out (docs/plugins.md 3.3, 5.4): the voucher stands for the first press, never the
        # second, which is Tgt's own, on its screen; installed then as with the button, no note (a person pressed there)
        lman = dict(json.load(open(f"{ROOT}/tests/plugins/testreq.json")), id="listener", name="Listener")
        lman["capabilities"] = dict(lman["capabilities"], **{"net.udp": 1}); lman["requires"] = plugcheck.requires_for(lman["capabilities"])
        made[0] += 1; mp = os.path.join(top, "l.json"); json.dump(lman, open(mp, "w")); lo = os.path.join(top, f"l{made[0]}.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/testreq", os.path.join(top, "author.key"), lo, license_=f"{ROOT}/LICENSE", arch="x86_64")
        plugcheck.list_package(lo, os.path.join(top, "index.key"), lo + ".o", arch="x86_64")
        st, r = upload(bs, open(lo + ".o", "rb").read()); post(bs, "/api/plugins/install"); src.press()
        check(st == 200 and r.get("second") and until(lambda: get(bs, "/api/plugins").get("install", {}).get("state") == "second"), f"Src: a listener with a way out, the first press: the second asked for ({r.get('second')})")
        post(bs, "/api/plugins/install"); src.press()
        check(until(lambda: (plug(bs, "listener") or {}).get("accepted")), "Src: its second press installs it")
        lp, _ = carried(bs, "listener"); nn0 = len(notes(bt)); n0 = len(tgt.text())
        info, st, r = sync(bs, bt, lp, "listener")
        check(st == 200 and r.get("install") == "second" and r.get("second") == "This plugin hears every request and can send what it hears to your network." and not tgt.has("listener")
              and get(bt, "/api/plugins").get("install", {}).get("state") == "second", f"synced to Tgt: the voucher holds, and Tgt wants its own press, the second, on its screen: {r}")
        tgt.press(); time.sleep(1)
        check(not tgt.has("listener"), "a press before the screen asked for it installs nothing")
        st, r = post(bt, "/api/plugins/install"); tgt.press()
        check(r.get("install") == "waiting" and until(lambda: tgt.has("listener") and (plug(bt, "listener") or {}).get("accepted")) and tgt.how("listener") == "button"
              and len(notes(bt)) == nn0 and "(its second press)" in tgt.text()[n0:], f"asked for, one press of Tgt's: installed as with its button, no note ({r}, {tgt.how('listener')})")
        remove(bt, "listener")
        # a voucher that stops holding between the two presses (Tgt cannot write down what was installed there: no
        # voucher is taken until it can): Tgt's press for the second installs nothing, and then both presses are Tgt's
        # own (M2 of a review of b294fa0: the voucher's second was asked for again and again, refused each time)
        sm = dict(lman, id="spare", name="Spare", capabilities={"request.text": True}, requires={"core": 1, "request.text": 1}); made[0] += 1
        mp = os.path.join(top, "s.json"); json.dump(sm, open(mp, "w")); so = os.path.join(top, f"s{made[0]}.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/testreq", os.path.join(top, "author.key"), so, license_=f"{ROOT}/LICENSE", arch="x86_64")
        plugcheck.list_package(so, os.path.join(top, "index.key"), so + ".o", arch="x86_64")
        with_button(src, bs, open(so + ".o", "rb").read(), "(Src: another plugin, to be found on Tgt later)")
        lp, _ = carried(bs, "listener"); n0 = len(tgt.text())
        info, st, r = sync(bs, bt, lp, "listener")
        check(st == 200 and r.get("install") == "second" and not tgt.has("listener"), f"(the listener synced to Tgt again: its second press asked for: {r})")
        open(failfloor, "w").close()
        shutil.copytree(os.path.join(src.state, "plugins", "spare"), os.path.join(tgt.state, "plugins", "spare"))
        # (a scan, through a plugin Tgt has: it finds the new folder, and its version cannot be written down)
        there = [x["id"] for x in get(bt, "/api/plugins").get("plugins", []) if x["id"] not in ("listener", "spare")]
        check(there, f"(Tgt has a plugin to switch on again: {there})")
        if there: post(bt, f"/api/plugins/{there[0]}/enable", "on")
        check(until(lambda: "could not be written" in tgt.text()[n0:]), "(Tgt cannot write down a version installed there now: no voucher holds)")
        slot = lambda: get(bt, "/api/plugins").get("install", {}).get("state")
        st, r = post(bt, "/api/plugins/install"); tgt.press()
        check(r.get("install") == "waiting" and until(lambda: slot() == "checked") and not tgt.has("listener"), f"the press for the voucher's second: nothing installed, the package checked again ({r}, {slot()})")
        st, r = post(bt, "/api/plugins/install"); tgt.press()
        check(r.get("install") == "waiting" and until(lambda: slot() == "second") and not tgt.has("listener"), f"then Tgt's own first press: the second asked for ({r}, {slot()})")
        st, r = post(bt, "/api/plugins/install"); tgt.press()
        check(r.get("install") == "waiting" and until(lambda: tgt.has("listener") and (plug(bt, "listener") or {}).get("accepted")) and tgt.how("listener") == "button",
              f"and Tgt's own second press installs it, accepted with its button ({r}, {slot()}, {tgt.how('listener')})")
        os.remove(failfloor)
        remove(bt, "listener"); remove(bt, "spare"); post(bs, "/api/plugins/listener/remove", "remove"); post(bs, "/api/plugins/spare/remove", "remove")

        # ---- hassmic ended between a note and its install; and a version older than one Tgt saw listed
        await ha.disconnect(force=True); ha = None
        tgt.stop()
        with open(os.path.join(tgt.state, "plugins", ".synced"), "w") as f:
            # (the first as a build before the digest wrote it: settled by the version; the third names another package
            # of the same version: the one in the folder is not the one it was written for)
            f.write(f"0 1 testwake echo-src\tTest wake word\ta browser\t{bt.pub.hex()}\t41 pending 0\n0 7 ghostplug echo-src\tGhost\ta browser\t{bt.pub.hex()}\t42 pending 0\n"
                    f"0 1 testwake echo-src\tTest wake word\ta browser\t{bt.pub.hex()}\t43 pending 0 {'ab' * 32}\n")
        with open(os.path.join(tgt.state, "plugins", ".listed"), "w") as f: f.write("testwake 9\n")
        n0 = len(tgt.text()); tgt.start()
        note = until(lambda: notes(bt))
        check([(x["id"], x["version"], x["n"]) for x in note] == [("testwake", 1, 41)] and "testwake version 1 on a voucher was under way when hassmic ended, and its package is in the plugin's folder: its note stands" in tgt.text()[n0:]
              and "ghostplug version 7 on a voucher was under way when hassmic ended, and its package is not in the plugin's folder: its note is dropped" in tgt.text()[n0:]
              and "testwake version 1 on a voucher was under way when hassmic ended, and its package is not in the plugin's folder: its note is dropped" in tgt.text()[n0:],
              f"notes left \"pending\" by a hassmic that ended: the one whose plugin is installed stands; the one of a plugin that is not, and the one written for another package than the folder has, go: {note}")
        ha, states = await ha_connect()
        check(await heard_event(1, "synced"), f"and Home Assistant hears of the one that stands: {evs()}")
        check(until(lambda: heard(bt, "echo-src"), 20), "(Tgt hears Src again)")
        remove(bt); info, st, r = sync(bs, bt, p3)
        wants_button("version 3, and Tgt has seen version 9 in the plugin index (state/plugins/.listed)", st, r, "older than version 9, which this Echo has seen in the plugin index")
        then_button("the older version")

        # ---- the notes once more, on a Tgt started with an hour of 4 s between waits for the button, a sync that can be
        # held between its note and its install (a file), and a store whose last write can fail (another)
        third = package(id="thirdplug", name="Third")
        with_button(src, bs, third, "(Src: a third official plugin)"); tpkg, _ = carried(bs, "thirdplug")
        delay = os.path.join(top, "sync-delay"); fail = os.path.join(top, "fail-accepted"); synfile = os.path.join(tgt.state, "plugins", ".synced")
        ids = lambda b: [x["id"] for x in notes(b)]
        pending = lambda pid: os.path.exists(synfile) and any(pid in l and " pending " in l for l in open(synfile))
        for pid in ("testwake", "otherplug"): remove(bt, pid)
        await ha.disconnect(force=True); ha = None
        failpkg = os.path.join(top, "fail-package")
        tgt.stop(); tgt.env.update(HASSMIC_PLUGIN_NOTE_AGAIN="4000", HASSMIC_PLUGIN_SYNC_DELAY=f"5000:{delay}", HASSMIC_PLUGIN_FAIL_ACCEPTED=fail, HASSMIC_PLUGIN_FAIL_PACKAGE=failpkg); tgt.start()
        check(until(lambda: heard(bt, "echo-src"), 20) and notes(bt) == [], "(Tgt started again, no note stands)")

        def held_sync(bfrom, bto, package_, pid):
            """a sync of PID to Tgt by BTO, held between its note and its install: the thread and where its answer goes"""
            open(delay, "w").close(); res = {}
            st, info = upload(bto, package_); n = nonce_of(bto); st, v = issue(bfrom, bto, n, pid)
            t = threading.Thread(target=lambda: res.update(a=synced(bto, v["node"], n, v["voucher"]))); t.start()
            return t, res, until(lambda: pending(pid), 5)

        # a wait for the button armed while another sync's note is pending (a review, 2026-11-20: the press took off
        # the note of a plugin no page had shown, since "all" was every number up to the newest, the pending one's too)
        info, st, r = sync(bs, bt, opkg, "otherplug")
        t, res, was = held_sync(bs, bt, tpkg, "thirdplug")
        shown = ids(b2t); st, r = post(b2t, "/api/plugins/synced", "dismiss all")
        os.remove(delay); t.join(); n0 = len(tgt.text())
        check(was and shown == ["otherplug"] and r == {"dismiss": "waiting", "holds": "note"} and res["a"][1].get("install") == "installed" and ids(bt) == ["otherplug", "thirdplug"],
              f"\"dismiss all\" asked while another plugin's note is pending (its install under way, on no page yet): the wait is for the note that showed ({shown}, {r}, {res.get('a')})")
        tgt.hold()
        check(until(lambda: ids(bt) == ["thirdplug"]) and "thirdplug version 1 came here without this Echo's action button was dismissed" not in tgt.text()[n0:] and tgt.has("thirdplug"),
              f"...and the hold takes off that one only: the note of the plugin that came meanwhile stands ({ids(bt)})")
        # the same by a request alone: the browser of Tgt's button brings a plugin and says "dismiss all" meanwhile
        post(bt, "/api/plugins/otherplug/remove", "remove")
        t, res, was = held_sync(bxs, bx, opkg, "otherplug")
        st, r = post(bx, "/api/plugins/synced", "dismiss all")
        os.remove(delay); t.join()
        check(was and r == {"dismiss": "done", "holds": "none"} and res["a"][1].get("install") == "installed" and ids(bx) == ["otherplug"] and notes(bx)[0]["dismiss"] == "button",
              f"a browser of Tgt's button says \"dismiss all\" while the note of a plugin it brings itself is pending: the other's note goes, its own stands once the install is done ({r}, {ids(bx)})")

        # the wait for the hold is not there for the asking: three an hour (4 s here) on this Echo, whoever asks, for
        # whichever notes, withdrawn ones counted (two reviews, 2026-11-20: armed again every minute; then "once for a
        # note never asked about, then once an hour", which a note made anew each round got around, and which the first
        # to ask spent for everybody)
        own = notes(bx)[0]["n"]; time.sleep(4.2); got = []
        for b in (bx, bx, b2t):
            st, r = post(b, "/api/plugins/synced", "dismiss all"); got.append(r); post(b, "/api/plugins/synced", "keep")
        st, r4 = post(bx, "/api/plugins/synced", "dismiss all"); st, r5 = post(b2t, "/api/plugins/synced", f"dismiss {own}")
        check(all(g == {"dismiss": "waiting", "holds": "note"} for g in got) and r4.get("dismiss") == "later" and 1 <= r4.get("retry_s", 0) <= 4 and r5.get("dismiss") == "later"
              and get(bx, "/api/plugins")["install"].get("holds") != "note",
              f"the wait to take a note off armed three times, each withdrawn at once, by the browser that brought the plugin and another vouched one: the fourth, by either: later, nothing armed ({got[0]} {r4} {r5})")
        # a note made anew is no way around it: the plugin removed and copied again, its fresh note asked about
        post(bt, "/api/plugins/thirdplug/remove", "remove"); info, st, r = sync(bs, bt, tpkg, "thirdplug")
        new = [x["n"] for x in notes(bt) if x["id"] == "thirdplug"]; st, r2 = post(bt, "/api/plugins/synced", f"dismiss {new[-1]}" if new else "dismiss all")
        n0 = len(tgt.text()); tgt.press(); tgt.hold(); time.sleep(0.6)
        check(r.get("install") == "installed" and len(new) == 1 and r2.get("dismiss") == "later" and ids(bt) == ["otherplug", "thirdplug"] and "dismissed" not in tgt.text()[n0:],
              f"a plugin removed and copied again has a note never asked about: its wait is \"later\" all the same, and neither a press nor a hold takes anything off ({r2})")
        st, r = post(bown, "/api/plugins/synced", f"dismiss {new[-1]}" if new else "dismiss 1")
        check(r.get("dismiss") == "done" and ids(bt) == ["otherplug"], f"...while a browser let in with Tgt's own button, which brought neither, still takes one off at once ({r})")
        ww = open(os.path.join(tgt.state, "web_wait")).read().split()
        check(len(ww) == 5 and all(int(x) > 0 for x in ww[2:]), f"(when the three were armed is on file, with the boot they were armed in: {ww[1:]})")
        tgt.stop(); tgt.start(); st, r = post(bx, "/api/plugins/synced", "dismiss all")
        check(r.get("dismiss") == "later" and 1 <= r.get("retry_s", 0) <= 4 and ids(bx) == ["otherplug"] and notes(bx)[0]["n"] == own, f"a restart (any approved browser can cause one) shortens nothing: still later ({r})")
        with open(os.path.join(top, "boot-2"), "w") as f: f.write("another-boot\n")
        tgt.stop(); tgt.env.update(HASSMIC_BOOT_ID=os.path.join(top, "boot-2")); tgt.start(); st, r = post(bx, "/api/plugins/synced", "dismiss all")
        check(r.get("dismiss") == "later" and 3 <= r.get("retry_s", 0) <= 4, f"...nor does a reboot (another boot, its clock begun anew): the hour starts over ({r})")
        time.sleep(4.2); st, r = post(b2t, "/api/plugins/synced", "dismiss all"); n0 = len(tgt.text()); tgt.held(); time.sleep(0.6)
        check(r == {"dismiss": "waiting", "holds": "note"} and ids(bx) == ["otherplug"] and "held from before the wait" in tgt.text()[n0:] and "button: action_hold" in tgt.text()[n0:]
              and get(b2t, "/api/plugins")["install"]["holds"] == "note",
              f"after that hour the wait is armed again; a hold that began before it was armed is not its: the note stands, the hold is Home Assistant's, the wait still armed ({r})")
        tgt.hold()
        check(until(lambda: notes(bx) == []), "...and the button held from after it takes the note off")
        st, r = post(bown, "/api/plugins/synced", f"dismiss {own}")
        check(r.get("dismiss") == "none", f"a number names one note for good: the one that is gone is \"none\", whatever comes later ({r})")

        # a note taken off before Home Assistant took its event: the event is kept on file and outlives a restart
        check(until(lambda: heard(bt, "echo-src"), 20), "(Tgt hears Src again)")
        post(bt, "/api/plugins/thirdplug/remove", "remove"); info, st, r = sync(bs, bt, tpkg, "thirdplug")
        st, r2 = post(bown, "/api/plugins/synced", "dismiss all")
        check(r.get("install") == "installed" and r2.get("dismiss") == "done" and notes(bt) == [] and any("thirdplug" in l and " gone 0 " in l for l in open(synfile)),
              f"copied while Home Assistant has no link, its note dismissed at once: on no page, kept on file for the event ({r2})")
        tgt.stop(); tgt.start()
        ha, states = await ha_connect()
        # (five: this one, and the four copied above since Home Assistant's link went, each dismissed before it came back)
        check(await heard_event(5, "synced") and evs() == ["synced"] * 5 and notes(bt) == [] and until(lambda: not os.path.exists(synfile), 5),
              f"Tgt restarted, Home Assistant connects: it hears `synced` all the same, once for each, and then the file is done with them: {evs()}")
        await ha.disconnect(force=True); ha = None

        # an install that fails after the store wrote the package: the files are there, so the note stands
        check(until(lambda: heard(bt, "echo-src"), 20), "(Tgt hears Src again)")
        post(bt, "/api/plugins/otherplug/remove", "remove"); open(fail, "w").close(); n0 = len(tgt.text())
        info, st, r = sync(bs, bt, opkg, "otherplug"); os.remove(fail)
        check(r.get("install") != "installed" and tgt.has("otherplug") and ids(bt) == ["otherplug"] and "failed after its package was put into its folder" in tgt.text()[n0:],
              f"the store fails at its last write, the package already in its folder: \"not installed\" is the answer, and the note stands ({r}, {ids(bt)})")
        post(bown, "/api/plugins/synced", "dismiss all")

        # an update on a voucher that fails between its program and its manifest (a review, 2026-11-20: the new program
        # stood under the old manifest, the note was dropped, and after a restart the plugin ran)
        pk = os.path.join(tgt.state, "plugins", "otherplug", "pkg")
        whole = lambda: hashlib.blake2b(open(os.path.join(pk, "plugin"), "rb").read(), digest_size=32).hexdigest() == json.load(open(os.path.join(pk, "manifest.json")))["digest"]
        post(bt, "/api/plugins/otherplug/remove", "remove"); info, st, r = sync(bs, bt, opkg, "otherplug"); post(bown, "/api/plugins/synced", "dismiss all")
        prog2 = os.path.join(top, "testwake2"); open(prog2, "wb").write(open(f"{ROOT}/build/testwake", "rb").read() + b"\0" * 64)
        o2 = package(id="otherplug", name="Other", version=2, program=prog2, capabilities={"wake": True, "files": 2})
        with_button(src, bs, o2, "(Src: version 2 of the other plugin, another program, wider)"); o2pkg, _ = carried(bs, "otherplug")
        open(failpkg, "w").close(); info, st, r2 = sync(bs, bt, o2pkg, "otherplug"); os.remove(failpkg)
        check(r.get("install") == "installed" and info.get("button") is True and r2.get("install") != "installed" and "cannot write" in (r2.get("error") or ""),
              f"version 2, wider, on a voucher, and the store fails after the program is written, before the manifest: not installed ({r2})")
        check(json.load(open(os.path.join(pk, "manifest.json")))["version"] == 1 and whole() and not os.path.exists(pk + ".new") and not os.path.exists(pk + ".old") and ids(bt) == [] and plug(bt, "otherplug")["version"] == 1,
              "...and the folder has what it had, whole: version 1's manifest with version 1's program, nothing beside it; nothing came, so no note stands")
        # ...and a folder whose program is not its manifest's (as that failure left it, or written by hand) is not run
        tgt.stop(); open(os.path.join(pk, "plugin"), "wb").write(open(prog2, "rb").read())
        n0 = len(tgt.text()); tgt.start()
        check(until(lambda: "otherplug: the program in its folder is not the one its manifest names" in tgt.text()[n0:], 15) and (plug(bt, "otherplug") or {}).get("state") != "running"
              and "not the one its manifest names" in ((plug(bt, "otherplug") or {}).get("detail") or ""),
              f"a program in the folder that is not the one the manifest names: not started after a restart, and its card says why ({(plug(bt, 'otherplug') or {}).get('detail')})")
        check(until(lambda: heard(bt, "echo-src"), 20), "(Tgt hears Src again)")
        post(bt, "/api/plugins/otherplug/remove", "remove"); info, st, r = sync(bs, bt, o2pkg, "otherplug")
        check(r.get("install") == "installed" and whole() and plug(bt, "otherplug")["version"] == 2 and ids(bt) == ["otherplug"], f"(without the fault version 2 goes in whole, with its note: {r})")
        post(bown, "/api/plugins/synced", "dismiss all")

        # never back, also past a removal (a review, 2026-11-20: Remove takes no button, and "never back" was against
        # what is installed: version 2 installed with Tgt's button, removed by a vouched browser, version 1 copied in)
        post(bt, "/api/plugins/thirdplug/remove", "remove")
        with_button(tgt, bown, package(id="thirdplug", name="Third", version=2), "(Tgt: version 2 of the third plugin, with its own button)")
        post(bt, "/api/plugins/thirdplug/remove", "remove"); until(lambda: not tgt.has("thirdplug"))
        info, st, r = sync(bs, bt, tpkg, "thirdplug")
        wants_button("version 1 from Src after version 2 was installed on Tgt and removed", st, r, "older than version 2, which was installed on this Echo before", "thirdplug")
        check(open(os.path.join(tgt.state, "plugins", ".installed")).read().count("thirdplug 2\n") == 1, "(the highest version installed is on file)")
        post(bt, "/api/plugins/cancel")

        # more "done" lines in the file than notes stand (a review, 2026-11-20: 24 of them and "dismiss all" wrote past
        # the list of numbers a dismissal carries)
        tgt.stop(); key = os.urandom(32).hex()
        with open(synfile, "w") as f:
            for i in range(24): f.write(f"0 1 testwake echo-src\tTest\tsomeone\t{key}\t{1000 + i} done 1 0\n")
        n0 = len(tgt.text()); tgt.start(); shown = len(notes(bown)); st, r = post(bown, "/api/plugins/synced", "dismiss all"); time.sleep(0.5)
        check(shown == 16 and st == 200 and r.get("dismiss") == "done" and notes(bown) == [] and tgt.proc.poll() is None and "names more than 16 notes that stand" in tgt.text()[n0:],
              f"24 \"done\" lines in the notes' file: 16 stand, the rest is dropped and said, and \"dismiss all\" takes them off ({shown}, {r})")
        # ...and every note kept has a place for its event: 16 that stand and 6 dismissed ones, none said to Home
        # Assistant yet (the core held 16, and the oldest were dropped at every start)
        tgt.stop()
        with open(synfile, "w") as f:
            for i in range(16): f.write(f"0 1 testwake echo-src\tTest\tsomeone\t{key}\t{2000 + i} done 0 -\n")
            for i in range(6): f.write(f"0 1 testwake echo-src\tTest\tsomeone\t{key}\t{3000 + i} gone 0 -\n")
        n0 = len(tgt.text()); tgt.start(); ha, states = await ha_connect()
        check(await heard_event(22, "synced", 15) and "the oldest is dropped" not in tgt.text()[n0:] and len(notes(bown)) == 16,
              f"22 notes Home Assistant has not heard of, 6 of them dismissed: it hears `synced` for each when it connects, none dropped ({len(evs())})")
        await ha.disconnect(force=True); ha = None
        post(bown, "/api/plugins/synced", "dismiss all")
        check(until(lambda: not os.path.exists(synfile), 5), "(and with all of them told and dismissed the file is gone)")

        # a note of a build that kept no browser's key, a line nobody can read, and who counts as let in with the button
        tgt.stop()
        with open(synfile, "w") as f: f.write("0 1 testwake echo-src\tTest wake word\ta browser\nwhat is this\n")
        n0 = len(tgt.text()); tgt.start(); note = until(lambda: notes(bown)); st, r = post(bown, "/api/plugins/synced", "dismiss all"); post(bown, "/api/plugins/synced", "keep")
        check(len(note) == 1 and note[0]["dismiss"] == "button" and note[0]["n"] > 1000 and r == {"dismiss": "waiting", "holds": "note"} and len(notes(bown)) == 1
              and "line 2 of" in tgt.text()[n0:] and "cannot be read" in tgt.text()[n0:],
              f"a note whose browser is not known is the button's also for a browser of Tgt's button; the line that cannot be read is dropped and said ({note}, {r})")
        wb = os.path.join(tgt.state, "web_button"); listed = open(wb).read().split()
        check(bown.pub.hex() in listed and bx.pub.hex() in listed and bt.pub.hex() not in listed and b2t.pub.hex() not in listed, "state/web_button lists the browsers Tgt's own button let in, and no other")
        tgt.stop(); os.remove(wb)
        with open(os.path.join(tgt.state, "web_vouched"), "w") as f: f.write("not a key\n")
        tgt.start(); cl = (bown.state() or {}).get("clients", [])
        check(len(cl) >= 4 and all(c.get("vouched") is True for c in cl) and notes(bown)[0]["dismiss"] == "button" and not os.path.exists(os.path.join(tgt.state, "web_vouched")),
              "the file gone (or an older build's list in its place): every browser counts as let in on a voucher, none as the button's")

        # the notes' file is there and no file: nothing comes without the button until it can be read
        tgt.stop(); os.path.exists(synfile) and os.remove(synfile); os.mkfifo(synfile); n0 = len(tgt.text()); tgt.start()
        check(until(lambda: heard(bt, "echo-src"), 20) and "cannot be read as the notes of plugins copied here (it is no regular file)" in tgt.text()[n0:], "state/plugins/.synced as a FIFO: Tgt starts all the same and says so")
        post(bt, "/api/plugins/otherplug/remove", "remove"); info, st, r = sync(bs, bt, o2pkg, "otherplug")
        wants_button("a plugin copied while Tgt cannot read its notes", st, r, "could not read its notes of plugins copied here", "otherplug")
        post(bt, "/api/plugins/cancel")

        # ---- the plugin index's revocation list from Echo to Echo (docs/plugins.md 5.6): Src looks at the index, tells
        # the members its serial (arb's message, here every second), Tgt, which cannot reach the index, fetches the
        # list from Src's settings port, checks it itself and stops what it withdraws; Out, in another network, hears
        # nothing of it.  Plugins off: nothing told
        site = plugsite.Site(top, 16867)                # (top/index.key: the key the Echos take)
        tgt.stop(); os.path.exists(synfile) and os.remove(synfile)
        for e, idx in ((src, site.base), (tgt, "http://127.0.0.1:9/"), (out, "http://127.0.0.1:9/")):
            # (HASSMIC_PLUGIN_MEMBER: one fetch from a member in 1.5 s, not ten minutes, until the part below that counts them)
            e.stop(); e.env.update(HASSMIC_PLUGIN_INDEX=idx, HASSMIC_PLUGIN_DAILY="3600000:86400000:1000", HASSMIC_PLUGIN_FRESH="0:0", HASSMIC_PLUGIN_MEMBER="1500"); e.start()
        for b_ in (bs, bt): b_.set(plugins="on")
        if not tgt.has(): with_button(tgt, bt, package(), "(testwake on Tgt again)")
        check(until(lambda: heard(bt, "echo-src"), 20) and until(lambda: (plug(bt) or {}).get("state") == "running", 20), "(Tgt hears Src, and runs testwake)")
        # (the handover is for members: X-HM-Ticket one of the tickets the Echo told in its announcement, X-HM-Ctr a
        # nonce, X-HM-Mac BLAKE2b-128 keyed with a key of the network's over both, plugin_fetch.c member_mac)
        import socket, struct
        rkey = hashlib.blake2b(b"hassmic plugin revoked 2", key=netkey, digest_size=32).digest()
        def member_headers(ticket, key=rkey, nonce=None):
            nonce = nonce or os.urandom(8).hex()
            return {"X-HM-Ticket": ticket, "X-HM-Ctr": nonce, "X-HM-Mac": hashlib.blake2b(f"GET /api/plugins/revoked\n{ticket}\n{nonce}".encode(), key=key, digest_size=16).hexdigest()}
        def told(e, secs=6):
            """E's announcement as any host of the LAN reads it on arb's port (it is authenticated, not hidden): (serial,
            port, ticket), or None"""
            ls = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ls.bind(("", ARB)); ls.settimeout(0.3)
            end = time.monotonic() + secs
            try:
                while time.monotonic() < end:
                    try: d, _ = ls.recvfrom(2000)
                    except OSError: continue
                    if d[:5] == b"HMA1\x07" and len(d) >= 38 + 19 + 16 and d[37] == 19 and d[38] == 0x43:
                        serial, port = struct.unpack(">QH", d[39:49])
                        if port == e.web: return serial, port, d[49:57].hex()
            finally: ls.close()
            return None
        def member_get(e, key=rkey, ticket=None):
            return req(e.web, "GET", "/api/plugins/revoked", headers=member_headers(ticket or os.urandom(8).hex(), key))
        st, _, body = member_get(src)
        nf = req(src.web, "GET", "/api/nothing-here")
        check(st == 404 and body == nf[2], f"Src has no revocation list yet: its settings port says so, as for any path it has not ({st})")
        now = int(time.time()); n0t, n0o = len(tgt.text()), len(out.text())
        site.build({}, revoked=[{"id": "testwake", "reason": "handed on by a member"}], revoked_serial=now)
        post(bs, "/api/plugins/index", "again")
        check(until(lambda: get(bs, "/api/plugins/index").get("revoked_serial") == now, 15), "Src: the list taken from the index")
        tk = told(src) or (0, 0, "")
        check(tk[0] == now and tk[1] == src.web, f"Src's announcement, read on arb's port without the network's key: its serial and settings port (said in docs/SECURITY.md) {tk[:2]}")
        h = member_headers(tk[2]); st, _, body = req(src.web, "GET", "/api/plugins/revoked", headers=h)
        check(st == 200 and body[128:129] == b"\n" and json.loads(body[129:])["serial"] == now, "Src's settings port hands it out to a member: the index key's signature and the list as the site had them")
        st3, _, b3 = req(src.web, "GET", "/api/plugins/revoked", headers=h)
        st4, _, b4 = member_get(src, ticket=os.urandom(8).hex())
        check(st3 == 404 and b3 == nf[2] and st4 == 404,
              f"the same request again (one seen on the way): refused, as is one naming a ticket Src never told (a replay was answered for ever, with the list as it is now: a review of 6f39dd8): {st3} {st4}")
        st1, _, b1 = req(src.web, "GET", "/api/plugins/revoked"); st2, _, b2 = member_get(src, key=os.urandom(32), ticket=tk[2])
        check(st1 == 404 and st2 == 404 and b1 == nf[2] and b2 == nf[2],
              f"to anyone else (no MAC, or one of another network's key) as for a path it has not: whether it has plugins is not told by the settings port (it was open: a review, 2026-11-21): {st1} {st2}")
        p = until(lambda: (lambda p: p if p and "withdrew it: handed on by a member" in p.get("detail", "") else None)(plug(bt)), 20) or {}
        check("the revocation list taken from echo-src" in tgt.text()[n0t:] and p.get("state") != "running" and get(bt, "/api/plugins").get("revoked", {}).get("serial") == now,
              f"Tgt, without the index: the list from Src, and testwake stopped by it: {p.get('detail')}")
        check("revocation list" not in out.text()[n0o:], "Out, in another network: nothing")
        # plugins off on Tgt: it asks nothing and takes nothing; on again, Src's next word brings the list
        bt.set(plugins="off"); n0t = len(tgt.text())
        site.build({}, revoked=[{"id": "testwake", "reason": "handed on by a member"}, {"id": "otherplug", "reason": "too"}], revoked_serial=now + 1)
        post(bs, "/api/plugins/index", "again")
        check(until(lambda: get(bs, "/api/plugins/index").get("revoked_serial") == now + 1, 15), "(Src: a newer list)")
        time.sleep(3)
        check("taken from echo-src" not in tgt.text()[n0t:] and get(bt, "/api/plugins").get("revoked", {}).get("serial") == now, "Tgt with plugins off: nothing asked of Src, nothing taken")
        bt.set(plugins="on")
        check(until(lambda: get(bt, "/api/plugins").get("revoked", {}).get("serial") == now + 1, 15), "on again: Src's next word, and the newer list taken")

        # ---- what a member can make of the handover, and what it cannot (a review of 2026-11-21 reproduced each): the
        # test speaks for members of its own (it has the network's key, as a member taken over has): their word on
        # arb's channel (T_MSG 0x43), their settings port a stand-in that hands over what it is given
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
        T0 = now; sk_index = plugsite.otatool.secret(os.path.join(top, "index.key"))
        def handover(serial, entries):
            d = plugsite.dump({"revoked": 1, "serial": serial, "entries": entries})
            return plugsite.otatool.sign(sk_index, plugsite.SIGN_REVOKED + d).hex().encode() + b"\n" + d
        both = [{"id": "testwake", "reason": "handed on by a member"}, {"id": "otherplug", "reason": "too"}]
        class Member:
            def __init__(self, port, body=None, delay=0, redirect=None, host="127.0.0.1"):
                self.body, self.hits, self.delay, self.redirect, self.port, self.id = body, [], delay, redirect, port, os.urandom(8)
                m = self
                class H(BaseHTTPRequestHandler):
                    def log_message(self, *a): pass
                    def do_GET(self):
                        m.hits.append((time.monotonic(), dict(self.headers)))
                        if m.delay: time.sleep(m.delay)
                        try:
                            if m.redirect: self.send_response(302); self.send_header("Location", m.redirect); self.send_header("Content-Length", "0"); self.end_headers(); return
                            self.send_response(200 if m.body else 404); self.send_header("Content-Length", str(len(m.body or b""))); self.end_headers(); self.wfile.write(m.body or b"")
                        except OSError: pass
                self.srv = ThreadingHTTPServer((host, port), H); threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        mackey = hashlib.blake2b(b"hassmic arbitration mac", key=netkey, digest_size=32).digest(); ctr = [1 << 41]
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1); tx.bind(("127.0.0.1", 0))
        def tell(m, serial, mid=None, via=None):
            ctr[0] += 1
            pk = b"HMA1\x07" + struct.pack("<Q", 0x51) + (mid or m.id) + b"\xff" * 8 + struct.pack("<Q", ctr[0]) + bytes([19]) + bytes([0x43]) + struct.pack(">QH", serial, m.port) + os.urandom(8)
            (via or tx).sendto(pk + hashlib.blake2b(pk, key=mackey, digest_size=16).digest(), ("127.255.255.255", ARB))
        rserial = lambda: get(bt, "/api/plugins").get("revoked", {}).get("serial")
        def restart_tgt(**env):
            tgt.stop()
            for k, v in env.items():
                if v is None: tgt.env.pop(k, None)
                else: tgt.env[k] = v
            tgt.start()
        # a member that spends a ticket's nonces refuses nobody: the last 64 taken with each ticket are a ring, the
        # oldest dropped (a cap of 64 a ticket, and a member that took them as each ticket was told had every honest
        # request refused: a review of a6c5a00); a request sent again is still refused while its nonce is in the ring.
        # (Src told every 30 s here, so that its ticket outlives the requests under load)
        src.stop(); src.env.update(HASSMIC_PLUGIN_DAILY="3600000:86400000:30000"); src.start()
        tk = told(src, 10) or (0, 0, "")
        spent = [member_headers(tk[2]) for _ in range(70)]
        sts = [req(src.web, "GET", "/api/plugins/revoked", headers=h)[0] for h in spent]
        st5, _, b5 = member_get(src, ticket=tk[2])
        st6 = req(src.web, "GET", "/api/plugins/revoked", headers=spent[-1])[0]
        st7 = req(src.web, "GET", "/api/plugins/revoked", headers=spent[7])[0]
        st8 = req(src.web, "GET", "/api/plugins/revoked", headers=spent[6])[0]
        check(tk[2] and sts.count(200) == 70 and st5 == 200 and b5[128:129] == b"\n",
              f"a member spends 70 nonces of one ticket: all answered, and an honest request after them still is (it was refused: a review of a6c5a00): {sts.count(200)} {st5}")
        check(st6 == 404 and st7 == 404 and st8 == 200,
              f"a request sent again while its nonce is among the ticket's last 64 taken: refused (the newest, and the oldest still kept); one out of them answered again, as docs/SECURITY.md says: {st6} {st7} {st8}")
        # a list taken while Tgt's clock is not set (its build a month old), from a member.  (Src stopped: it would ask
        # the test's members too.  One fetch per member in ten minutes, as built)
        src.stop()
        restart_tgt(HASSMIC_PLUGINS_BUILT=str(T0 - 30 * 86400), HASSMIC_PLUGIN_MEMBER=None)
        # a member's word from an address that is not on a local network (here 198.51.100.9, TEST-NET-2, put on lo):
        # not taken, nothing fetched from there (a review of 6631974, 6: the source address and the message's port
        # were fetched from, wherever they were).  The stand-in member listens on every address: a fetch would reach it
        # (tests/run.py puts the address on lo, LO_ADDRS; alone, it is tried here)
        subprocess.run(["ip", "addr", "add", "198.51.100.9/32", "dev", "lo"], capture_output=True)
        txf = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); txf.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try: txf.bind(("198.51.100.9", 0)); far = True
        except OSError as x: far = x; txf.close()
        if far is True:
            mf = Member(16859, handover(T0 + 2, both + [{"id": "thirdplug", "reason": "three"}]), host="0.0.0.0"); r0 = rserial()
            for _ in range(3): tell(mf, T0 + 2, via=txf); time.sleep(0.3)
            time.sleep(3)
            check(not mf.hits and rserial() == r0, f"a member's word from outside the local network (198.51.100.9): nothing fetched from there, nothing taken: {len(mf.hits)} {rserial()}")
            mf.srv.shutdown(); txf.close()
        else:
            print(f"skip: a member's word from outside the network (198.51.100.9 is not on lo here: {far}; tests/run.py puts it there)")
        m1 = Member(16858, handover(T0 + 2, both + [{"id": "thirdplug", "reason": "three"}]))
        tell(m1, T0 + 2)
        check(until(lambda: rserial() == T0 + 2, 15) and len(m1.hits) == 1 and m1.hits[0][1].get("X-HM-Mac") and m1.hits[0][1].get("X-HM-Ctr"),
              f"the clock not set: a newer list from a member taken, asked for with the network's MAC: {rserial()} {[h for _, h in m1.hits]}")
        head = open(os.path.join(tgt.state, "plugins", ".revoked"), "rb").read().split(b"\n", 1)[0]
        check(head == f"floor {T0 + 1}".encode(), f"its floor line: the one before it, which the clock had vouched for (it fell to the build's time + a day, and a list older than the vouched one could be taken later: a review of 6f39dd8): {head!r}")
        # the clock set: an older signed list announced as newer is no longer taken (it was: the floor line held, a
        # month under the kept serial, and what the newer list withdrew ran again)
        restart_tgt(HASSMIC_CLOCK_SYNCED=None)
        n0 = len(tgt.text())
        m2 = Member(16857, handover(T0 - 5 * 86400, []))
        tell(m2, T0 + 100)
        check(until(lambda: "not taken" in tgt.text()[n0:] or rserial() != T0 + 2, 15) and rserial() == T0 + 2 and "older" in tgt.text()[n0:] and len(m2.hits) == 1
              and (plug(bt) or {}).get("state") != "running",
              f"the clock set: an older list a member hands over refused, and said; testwake stays stopped: {rserial()} {(plug(bt) or {}).get('state')} {len(m2.hits)}")
        check(until(lambda: open(os.path.join(tgt.state, "plugins", ".revoked"), "rb").read().split(b"\n", 1)[0] == f"floor {T0 + 2}".encode(), 5),
              "and the floor line raised to the kept serial, once the clock vouches for it")
        # one fetch per member in ten minutes whatever serial it names; the same list from a member is no word
        atp = os.path.join(tgt.state, "plugins", ".revoked-at")
        tgt.stop(); open(atp, "w").write(f"word {T0 - 8 * 86400} {8 * 86400}\n"); tgt.start()
        st0 = get(bt, "/api/plugins").get("revoked", {}); ino0 = os.stat(atp).st_ino
        m3 = Member(16856, handover(T0 + 2, both + [{"id": "thirdplug", "reason": "three"}]))
        t1 = time.monotonic(); k = 0
        while time.monotonic() - t1 < 4: k += 1; tell(m3, T0 + 1000 + k); time.sleep(0.02)
        time.sleep(0.5)
        st1 = get(bt, "/api/plugins").get("revoked", {})
        check(len(m3.hits) == 1 and os.stat(atp).st_ino == ino0, f"{k} announcements of ever newer serials in 4 s, the same list handed back: {len(m3.hits)} fetch, .revoked-at not written (it was a fetch and a write each)")
        check(st0.get("stale") and st1.get("stale") and st1.get("age_s", 0) >= 8 * 86400, f"and the week's warning stays: a member's same list is no word that it is the newest: {st0.get('age_s')} -> {st1.get('age_s')}")
        # one member that holds every request does not keep another's newer list out
        slow = Member(16855, b"", delay=14); honest = Member(16854, handover(T0 + 3, both))
        t1 = time.monotonic(); k = 0
        while time.monotonic() - t1 < 20 and rserial() != T0 + 3:
            k += 1; tell(slow, T0 + 10 ** 6 + k)
            if k % 20 == 1: tell(honest, T0 + 3)
            time.sleep(0.1)
        check(rserial() == T0 + 3 and len(honest.hits) >= 1 and len(slow.hits) <= 1,
              f"a member that holds every request and tells over and over: asked once (5 s at most), and the honest member's newer list taken {time.monotonic() - t1:.1f} s after: {len(slow.hits)} {len(honest.hits)}")
        # no redirect followed for a member's list (one was followed to any HTTPS address a member named)
        lst = socket.socket(); lst.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); lst.bind(("127.0.0.1", 16853)); lst.listen(4); lst.settimeout(0.2); conns = []
        def accept():
            end = time.monotonic() + 8
            while time.monotonic() < end:
                try: c, _ = lst.accept(); conns.append(1); c.close()
                except OSError: pass
        th = threading.Thread(target=accept); th.start()
        r = Member(16852, redirect="https://127.0.0.1:16853/api/plugins/revoked"); n0 = len(tgt.text())
        tell(r, T0 + 4)
        until(lambda: "not had" in tgt.text()[n0:], 10); th.join()
        check(len(r.hits) == 1 and not conns and "HTTP 302" in tgt.text()[n0:], f"a member's redirect: not followed ({len(conns)} connections to where it pointed)")
        lst.close()
        # a member that names a new id with every message (arb's ids are a member's own word): the fetches from
        # members bounded in all, and the log's lines on them (it was one fetch and one line each: 496 in 5 s, a
        # review of 6f39dd8).  (HASSMIC_PLUGIN_MEMBER: as built, ten minutes a place and four at once, but one more
        # each second, not each 20 s, so that the places waiting are asked within the test's time)
        restart_tgt(HASSMIC_PLUGIN_MEMBER="600000:4:1000")
        rot = Member(16851, b""); n0 = len(tgt.text()); t1 = time.monotonic(); k = 0
        while time.monotonic() - t1 < 5: k += 1; tell(rot, T0 + 10 ** 6 + k, mid=os.urandom(8)); time.sleep(0.01)
        time.sleep(0.3)
        fetched, lines = len(rot.hits), tgt.text()[n0:].count("not had")
        check(fetched <= 4 + 6 and lines <= 5, f"{k} messages in 5 s, each under a new id: {fetched} fetches (four at once, one a second after), {lines} lines in the log (five)")
        honest2 = Member(16850, handover(T0 + 5, both)); tell(honest2, T0 + 5)
        check(until(lambda: rserial() == T0 + 5, 30) and until(lambda: "lines on members' lists left out before it" in tgt.text()[n0:], 5),
              f"the places that waited asked at that pace, then the honest member's newer list taken, and what the log left out counted in its line: {rserial()} {len(rot.hits)}")
        # a member that names an honest member's id from another port (it rewrote the place's address, and the honest
        # member's newer list was never asked for: the same review): a place of its own
        busy = Member(16849, b"", delay=4.5); honest3 = Member(16848, handover(T0 + 6, both)); spoof = Member(16847, b"")
        time.sleep(1.2)
        tell(busy, T0 + 2 * 10 ** 6); time.sleep(0.3)
        tell(honest3, T0 + 6); spoof.id = honest3.id; tell(spoof, T0 + 2 * 10 ** 6 + 1)
        check(until(lambda: rserial() == T0 + 6, 15) and len(honest3.hits) == 1,
              f"a member sending under the honest member's id from another port: the honest member asked all the same, its list taken: {len(honest3.hits)} {len(spoof.hits)} {rserial()}")
        for m in (m1, m2, m3, slow, honest, r, rot, honest2, busy, honest3, spoof): m.srv.shutdown()
        restart_tgt(HASSMIC_CLOCK_SYNCED="0")
        site.shutdown()
    finally:
        if ha:
            try: await ha.disconnect(force=True)
            except Exception: pass
        for e in echos:
            if hasattr(e, "proc"): e.stop()
        if check.failed:
            for e in echos: print(f"--- {e.name}\n{e.text()[-5000:]}")
    print("FAIL" if check.failed else "PASS")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__": asyncio.run(main())
