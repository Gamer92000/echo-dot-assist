#!/usr/bin/env python3
"""On the PC: `net.lan`, `net.internet` and `log.read.full`, and more plugins at once than phase 1's two,
with a stand-in (tests/plugins/testnet.c as "testlan", its mode `reach`) under build/plugrun-host and
build/hassmic-host, this script as the settings page and as the servers the plugin may and may not reach.

The servers are other hosts behind a veth pair, in a network namespace of theirs (tests/peerhost.py, which
tests/run.py makes for this test): 10.250.0.2 private, 198.51.100.7 outside the local network; the Echo's own side is
10.250.0.1, and 10.251.0.0/24 an address range a "local" route delivers to the Echo itself over lo.  Run alone, the
script starts itself in such a namespace.

The install: each capability's line with its entries' protocols and ports, the whole log's line; a package with
log.read.full that the index does not list as official refused (a sideload, a community listing), and one whose
listing is gone after the install not started, its card saying why; request.audio beside net.lan and net.internet:
the second press, its screen naming the internet and your network.  net.lan: an address on the network the plugin
names, at its entry's ports (TCP 16996; UDP any but 5555), reached; outside the network, the Echo's own address, an
address routed to the Echo itself, loopback, a name (no lookup), a port the entry does not list refused, each with its
code; DNS's ports (53, 853) refused under "any" and in the manifest (the router asks the internet).  net.internet: an
address outside the network reached and said so, one on the network refused (that is net.lan's), a name without an
address NOT_FOUND, loopback by name refused.  log.read.full: what the page shows, what was said included, root's
lines whatever they say, addresses as they are, the pairing token blanked; never a sealed line or a wake word
plugin's.  Places: five plugins running at once; one whose memory limit does not fit beside them (96 MB, 128 between
them) waits for a place, its card says so, and it starts by itself once others stopped."""
import asyncio, json, os, shutil, signal, subprocess, sys, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser
import fake_plugins as fp
import fake_plugins_net as fn

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk"))
import plugcheck
check = fp.check
LAN, OUT, OWN, LOCAL, UDP, TCP = fn.LAN, fn.OUT, fn.OWN, fn.LOCAL, fn.UDP, fn.TCP


async def main():
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log")
    for k in ("author", "index"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    man = json.load(open(f"{ROOT}/tests/plugins/testlan.json"))
    wman = json.load(open(f"{ROOT}/tests/plugins/testwake.json"))

    def package(base=man, program="testnet", tier="official", **changes):
        """the index's listing as TIER ("official", "community"), or none (a sideload)"""
        m = dict(base, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, "p.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/{program}", os.path.join(top, "author.key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        if not tier: return open(out, "rb").read()
        if tier == "official": plugcheck.list_package(out, os.path.join(top, "index.key"), os.path.join(top, "o.hmp"), arch="x86_64")
        else:
            r = subprocess.run([sys.executable, f"{ROOT}/sdk/plugcheck.py", "list", out, os.path.join(top, "index.key"), os.path.join(top, "o.hmp"), "--tier", tier, "--arch", "x86_64"], capture_output=True, text=True)
            if r.returncode: raise SystemExit(r.stderr)
        return open(os.path.join(top, "o.hmp"), "rb").read()

    def install(b, proc, pkg, pid, presses=1):
        st, r = fp.upload(b, pkg)
        if st != 200: return st, r
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        if presses == 2:
            fp.until(lambda: (fp.post(b, "/api/plugins/install")[1] or {}).get("install") in ("waiting", "second", None), 5)
            proc.send_signal(signal.SIGUSR2)
        fp.until(lambda: (fp.plug(b, pid) or {}).get("accepted"), 10)
        return st, r

    lan, out = fn.Server(LAN), fn.Server(OUT)
    # (community plugins on, as the Plugins page's switch leaves them: a community listing is tried here)
    open(os.path.join(state, "config"), "a").write("plugins_community=on\n")
    slow = os.path.join(top, "slow-install")         # (while it is there: the core hears of an install 2 s late)
    proc = fp.start(state, log, env={"HASSMIC_PLUGIN_LINES": "400", "HASSMIC_PLUGIN_INSTALLED_DELAY": f"2000:{slow}"})
    plog = lambda: [l.split("plugin testlan: ", 1)[1] for l in open(log, errors="replace").read().splitlines() if "plugin testlan: " in l]
    def says(text, n0=0, secs=10): return fp.until(lambda: any(text in l for l in plog()[n0:]), secs)
    try:
        wait_port(fp.PORT, fp.WEB)
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "logged in with the button")
        b.set(plugins="on")

        # ---- the install: what it may do, line by line
        st, r = fp.upload(b, package())
        lines = [c["line"] for c in r.get("caps", [])]
        check(st == 200 and r.get("button") is True and r.get("requires", {}).get("net.lan") == 1 and "api" not in r, f"a new plugin waits for the button (what it requires in the check's answer): {st} {r.get('error')}")
        check("Connects to any device on your network (TCP, port 16996; UDP, any port)" in lines and "Connects to the internet (TCP, port 16996)" in lines
              and "Reads this Echo's whole log, including what was said" in lines, f"one line per capability, each entry's protocol and ports: {lines}")
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(fp.until(lambda: (fp.plug(b, "testlan") or {}).get("accepted")), "the press installs it")
        # the whole log is the index's official plugins' only: a sideload and a community listing refused at install
        for tier, word in ((None, "a sideload"), ("community", "a community listing")):
            st, r = fp.upload(b, package(tier=tier, id="fulllog"))
            check(st == 400 and "official plugins only" in r.get("error", ""), f"log.read.full, {word}: refused at install: {r.get('error')}")
            fp.post(b, "/api/plugins/cancel")
        # what the manifest may say of the two (net.lan, net.internet: { "proto", "ports" } entries, read strictly)
        for caps, why in (({"net.lan": [{"proto": "tcp", "ports": [5555]}]}, "not 5555"), ({"net.lan": [{"proto": "tcp", "ports": "all"}]}, "or \"any\""),
                          ({"net.lan": [{"proto": "udp", "ports": [53]}]}, "asking the internet (DNS)"), ({"net.discover": {"udp": 853, "to": "broadcast"}}, "asking the internet (DNS)"),
                          ({"net.internet": [{"proto": "tcp", "ports": [443], "tls": True}]}, "nothing else"), ({"net.lan": [80]}, "nothing else"),
                          ({"wake": True, "files": 1, "net.internet": [{"proto": "tcp", "ports": [443]}]}, "way out")):
            st, r = fp.upload(b, package(id="badlan", capabilities=caps, settings=[]))
            check(st == 400 and why in r.get("error", ""), f"refused: {why}: {r.get('error')}")
            fp.post(b, "/api/plugins/cancel")
        # request.audio with a way out: the second press, its screen naming both
        st, r = fp.upload(b, package(id="hearlan", capabilities={"request.audio": True, "net.lan": [{"proto": "tcp", "ports": [80]}], "net.internet": [{"proto": "tcp", "ports": [443]}]}, settings=[]))
        check(st == 200 and r.get("second") == "This plugin hears every request and can send what it hears to the internet and your network.",
              f"request.audio with net.lan and net.internet: the second press, and where it can send what it hears: {r.get('second')}")
        fp.post(b, "/api/plugins/cancel")

        # ---- net.lan, net.internet: where the plugin names, each refusal with its code
        def reach(text, want, secs=15):
            n0 = len(plog())
            fp.post(b, "/api/plugins/testlan/set", f"reach={text}")
            return says(f"reach {text}: {want}", n0, secs)
        check(fp.until(lambda: (fp.plug(b, "testlan") or {}).get("state") == "running", 20), "the plugin runs")
        check(reach(f"lan 0 {LAN} {TCP}", "ok (0)") and fp.until(lambda: lan.has(f"hello lan 0 {LAN} {TCP}", "tcp")),
              f"net.lan: a device on the network at its entry's port, TCP: reached: {plog()[-2:]}")
        check(fp.logged(log, f"plugins: testlan: connected to {LAN}:{TCP} (TCP, a device on the network (net.lan))"), "said in the log as net.lan's")
        check(reach(f"lan 1 {LAN} {UDP}", "ok (0)") and fp.until(lambda: lan.has(f"hello lan 1 {LAN} {UDP}", "udp")), "net.lan's UDP entry with any port: reached")
        for text, why, word in ((f"lan 0 {OUT} {TCP}", 2, "an address outside the network"), (f"lan 0 {OWN} {TCP}", 2, "the Echo's own address"),
                                (f"lan 0 {LOCAL} {TCP}", 2, "an address routed to the Echo itself (a local route, over lo)"), (f"lan 0 127.0.0.1 {TCP}", 2, "loopback"),
                                (f"lan 0 localhost {TCP}", 2, "a name (no lookup for net.lan)"), (f"lan 0 {LAN} 22", 2, "a port its entry does not list"),
                                (f"lan 1 {LAN} 5555", 2, "port 5555 under \"any\""), (f"lan 1 {LAN} 53", 2, "DNS's port 53 under \"any\" (the router asks the internet)"),
                                (f"lan 1 {LAN} 853", 2, "DNS over TLS's 853 under \"any\""), (f"lan 2 {LAN} {TCP}", 2, "an entry it has not")):
            worker = lambda: sum("plugins: testlan: no connection to a device on the network" in l for l in open(log, errors="replace"))
            w0 = worker()
            check(reach(text, f"refused ({why})"), f"net.lan, {word}: refused ({why}): {plog()[-1:]}")
            # (the broker's refusals, before any worker: a name never reaches the lookup, whatever it would resolve to;
            # "localhost" would have been refused by the worker as loopback too, N5 of a review of 7be9343)
            if word not in ("the Echo's own address", "an address routed to the Echo itself (a local route, over lo)"):
                check(worker() == w0, f"... by the broker, no worker asked: {word}")
        check(reach(f"internet 0 {OUT} {TCP}", "ok (0)") and fp.until(lambda: out.has(f"hello internet 0 {OUT} {TCP}", "tcp")),
              "net.internet: an address outside the network reached")
        check(fp.logged(log, f"plugins: testlan: connected to {OUT}:{TCP} (TCP, outside the local network: net.internet)"), "said in the log as net.internet's")
        check(reach(f"internet 0 {LAN} {TCP}", "refused (2)"), "net.internet: an address on the network refused (that is net.lan's, N3 of a review of 7be9343)")
        for text, why, word in (("internet 0 nothing.invalid %d" % TCP, 6, "a name without an address"), (f"internet 0 localhost {TCP}", 2, "loopback by name"),
                                (f"internet 0 {OWN} {TCP}", 2, "the Echo's own address"), (f"internet 0 {OUT} 22", 2, "a port its entry does not list")):
            check(reach(text, f"refused ({why})"), f"net.internet, {word}: refused ({why}): {plog()[-1:]}")
        check((fp.plug(b, "testlan") or {}).get("state") == "running", "and the plugin runs on")

        # ---- log.read.full: the page's view, but never a wake word plugin's lines
        open(slow, "w").close()
        st, r = fp.upload(b, package(wman, "testwake")); fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(fp.until(lambda: (fp.plug(b, "testwake") or {}).get("accepted")), "(a wake word plugin beside it)")
        # P4 of a review of 7be9343: chosen as the engine the moment the page shows it accepted, before the broker told
        # the core of it (2 s late here): a value the setting takes, its list read anew
        r = b.set(wake_engine="plugin:testwake")
        check(r.get("applied") == 1 and "not a value it takes" not in json.dumps(r), f"a wake word plugin chosen as the engine the moment it is accepted: taken: {r}")
        os.unlink(slow)
        b.set(wake_engine="amazon")
        n0 = len(plog())
        fp.post(b, "/api/plugins/testlan/set", f"reach=lan 0 {LAN} {TCP} keep")
        check(says(f"reach lan 0 {LAN} {TCP} keep: ok (0)", n0), "(its log goes to the server on the network)")
        mark = f"full-{os.getpid()}"
        fn.say(log, f"boot+100.123 {mark} public line")
        fn.say(log, f"boot+100.124 (private) transcript: {mark} what was said")
        fn.say(log, f"boot+100.125 bluetooth: {mark} paired aa:bb:cc:dd:ee:ff")
        fn.say(log, f"boot+100.126 sendspin: {mark} pairing token SP:0ABCDEFGH234")
        fn.say(log, f"boot+100.127 plugin testwake: {mark} a wake plugin's own line")
        fn.say(log, f"boot+100.128 (sealed) plugins: gone: {mark} a sealed line")
        fn.say(log, f"boot+100.13 wifi: {mark} joined MyHomeNetwork")
        fn.control(log, lan, "after the full log's lines")
        got = [l for _, l in lan.lines if mark in l]
        check(any("(private) transcript: " + mark in l for l in got) and any("joined MyHomeNetwork" in l for l in got) and any("aa:bb:cc:dd:ee:ff" in l for l in got)
              and any("SP:0*********" in l for l in got) and not any("SP:0ABC" in l for l in got),
              f"the whole log: what was said (its mark with it), root's lines whatever they say, addresses as they are, the pairing token blanked: {got}")
        check(not any("a wake plugin's own line" in l or "a sealed line" in l for l in got), f"never a wake word plugin's line or a sealed one: {got}")
        # refused at the start too: its listing gone (put there by other means than the page), it is no official plugin
        lst = os.path.join(state, "plugins", "testlan", "pkg", "listing.sig"); kept = open(lst, "rb").read(); os.unlink(lst)
        fp.post(b, "/api/plugins/testlan/enable", "off"); fp.post(b, "/api/plugins/testlan/enable", "on")
        p = fp.until(lambda: (lambda q: q if q and "official plugins only" in (q.get("detail") or "") else None)(fp.plug(b, "testlan")), 10) or fp.plug(b, "testlan") or {}
        check("official plugins only" in (p.get("detail") or "") and p.get("state") != "running", f"without its listing: not started, the card says why: {p.get('state')} {p.get('detail')}")
        open(lst, "wb").write(kept)
        fp.post(b, "/api/plugins/testlan/enable", "off"); fp.post(b, "/api/plugins/testlan/enable", "on")
        check(fp.until(lambda: (fp.plug(b, "testlan") or {}).get("state") == "running", 20), "its listing back: it runs again")

        # ---- places: more than phase 1's two at once, and one that waits for its memory's place
        base = dict(man, capabilities={"storage": 1}, settings=man["settings"])
        for i in range(3):
            st, r = install(b, proc, package(base, id=f"spare{i}", name=f"Spare {i}"), f"spare{i}")
            check(st == 200 and fp.until(lambda: (fp.plug(b, f"spare{i}") or {}).get("accepted")), f"(spare{i} installed: {r.get('error')})")
        running = lambda: sorted(p["id"] for p in fp.plugins(b).get("plugins", []) if p.get("state") == "running")
        check(fp.until(lambda: len(running()) == 5, 20), f"five plugins run at once (phase 1 ran two): {running()}")
        o = fp.plugins(b)
        check(o.get("run_max") == 8 and o.get("run_mem") == 128, f"the page: up to 8 at once, 128 MB between them: {o.get('run_max')} {o.get('run_mem')}")
        st, r = install(b, proc, package(dict(base, memory_mb=96), id="bigone", name="Big one"), "bigone")
        p = fp.until(lambda: (lambda q: q if q and q.get("state") == "waiting" else None)(fp.plug(b, "bigone")), 10) or fp.plug(b, "bigone") or {}
        check(p.get("state") == "waiting" and "waits for a place" in (p.get("detail") or "") and "128 MB" in (p.get("detail") or "") and len(running()) == 5,
              f"a sixth that may use 96 MB: no place for it beside five of 16 MB (176 over 128): it waits, and says why: {p.get('state')} {p.get('detail')}")
        check(fp.logged(log, "plugins: bigone: waits for a place"), "said in the log")
        for pid in ("spare0", "spare1", "spare2"): fp.post(b, f"/api/plugins/{pid}/enable", "off")
        check(fp.until(lambda: (fp.plug(b, "bigone") or {}).get("state") == "running", 20), f"three switched off (32 MB left taken): it starts by itself: {(fp.plug(b, 'bigone') or {}).get('state')}")
        fp.post(b, "/api/plugins/spare0/enable", "on")
        p = fp.until(lambda: (lambda q: q if q and q.get("state") == "waiting" else None)(fp.plug(b, "spare0")), 10) or fp.plug(b, "spare0") or {}
        check(p.get("state") == "waiting", f"one switched on again with the 96 MB one running: it waits now: {p.get('state')} {p.get('detail')}")
        fp.post(b, "/api/plugins/bigone/enable", "off")
        check(fp.until(lambda: (fp.plug(b, "spare0") or {}).get("state") == "running", 20), "the big one off: it starts")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
    if check.failed:
        print("---- log"); print(open(log, errors="replace").read()[-8000:])
    shutil.rmtree(top, ignore_errors=True)              # (its own folder, by its name: nothing else in /tmp)
    print("plugins-lan: " + ("FAILED" if check.failed else "all passed"))
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    if not fn.reachable():
        if os.environ.get("HASSMIC_NETNS_DONE"): sys.exit("fake_plugins_lan: no other hosts in this namespace (tests/run.py netns(peer=True))")
        import run
        os.environ["HASSMIC_NETNS_DONE"] = "1"
        os.execvp("unshare", run.netns([sys.executable, os.path.abspath(__file__)], peer=True))
    try: asyncio.run(main())
    finally:                                            # the other hosts' socket and its folder (tests/run.py made them)
        d = os.path.dirname(os.environ.get("HASSMIC_PEER_SOCK", "")) if fn.reachable() else ""
        if d and os.path.basename(d).startswith("hassmic-peer."):
            for f in os.listdir(d): os.unlink(os.path.join(d, f))
            os.rmdir(d)
