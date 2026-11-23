#!/usr/bin/env python3
"""Phase 2 of docs/plugins.md on the PC: storage, the log and a socket to the owner's server, with a stand-in
forwarder (tests/plugins/testnet.c) under build/plugrun-host and build/hassmic-host, this script as the settings page,
as Home Assistant and as the servers the plugin may and may not reach.

The servers are other hosts behind a veth pair, in a network namespace of theirs (tests/peerhost.py, which tests/run.py
makes for this test): 10.250.0.2, private, 10.250.0.3, a server that never answers, and 198.51.100.7, outside the local
network; the Echo's own side is 10.250.0.1, and 10.251.0.0/24 an address range a "local" route delivers to the Echo
itself over lo.  Run alone, the script starts itself in such a namespace.

The install: one line per capability in the dialog, with its parameters (the server's row, its protocol and ports),
whole however long, the button for a new plugin; a manifest whose server names no host row, or whose server or port
row has a default, refused.  The plugin reaching the server the owner entered and nothing else: the UDP entry at the
port the owner entered, the TCP entry at its listed port, an address outside the network (and said so); loopback, the
Echo's own address, one routed to the Echo itself, an unset server, a name without an address refused; a socket of
its own (the filter), a CONNECT to another host and to a port its entry does not list refused, and said (the second
by the count, once in 10 s); a connect
before READY refused at once (BAD) and the plugin goes on; one to a server that never answers given up within 10 s,
the plugin running on.  The owner changing or clearing the server: the plugin started again, and the old server hears
nothing after.  The log it is given: hassmic's lines, the private ones (what was said, names) left out, root's only by
their event words, a wake word plugin's not at all, MAC addresses and the pairing token blanked; and what hassmic's
own writers put there with Home Assistant driving them (a transcript and a reply with line breaks, a media address
with its token, a timer's name, an error's text: sentinels that must not reach the server).  Its storage: the cursor
kept and followed after a restart (the lines written while it was off arrive), within its kB (a value over refused),
ten writes and then the client keeps the latest and writes it later, a key that is none refused; kept by an update,
replaced whole by another Echo's copy (the page's way: sealed out, sealed in), the copy over its kB refused, a copy
of a wake word plugin's data into it refused, one not sealed refused; wiped by Remove.  Updates that widen what was
accepted (more storage, another port) ask for the button and say so; one that asks for nothing new does not.  The
development flag: a call the sandbox refuses stops the plugin (SIGSYS) and the log names it; without the flag the
same call is answered EPERM."""
import asyncio, io, json, os, shutil, signal, socket, subprocess, sys, tarfile, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser, req
import fake_plugins as fp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk"))
import plugcheck
check = fp.check
LAN, OUT, UDP, TCP = "10.250.0.2", "198.51.100.7", 16995, 16996
SLOW, OWN, LOCAL = "10.250.0.3", "10.250.0.1", "10.251.0.9"
import peerhost


def reachable():
    """the other hosts are there (tests/run.py netns(peer=True), tests/peerhost.py)"""
    return bool(os.environ.get("HASSMIC_PEER_SOCK")) and os.path.exists(os.environ["HASSMIC_PEER_SOCK"])


class Server:
    """what the plugin sends, by line: UDP datagrams, TCP lines; its sockets made in the other hosts' namespace"""
    def __init__(self, host, udp=UDP, tcp=TCP):
        self.lines, self.conns = [], 0
        self.u = peerhost.peer_socket("udp", host, udp)
        self.t = peerhost.peer_socket("tcp", host, tcp, 4)
        threading.Thread(target=self.udp, daemon=True).start(); threading.Thread(target=self.tcp, daemon=True).start()

    def udp(self):
        while True:
            d, _ = self.u.recvfrom(4096); self.lines.append(("udp", d.decode(errors="replace")))

    def tcp(self):
        while True:
            c, _ = self.t.accept(); self.conns += 1
            threading.Thread(target=self.reader, args=(c,), daemon=True).start()

    def reader(self, c):
        buf = b""
        while True:
            d = c.recv(4096)
            if not d: return
            buf += d
            while b"\n" in buf:
                l, buf = buf.split(b"\n", 1); self.lines.append(("tcp", l.decode(errors="replace")))

    def has(self, text, how=None): return any(text in l and (how is None or k == how) for k, l in self.lines)


def say(log, line):
    """a line into the log as a writer of it would have put it there (O_APPEND, as theirs)"""
    with open(log, "a") as f: f.write(line + "\n")


def control(log, lan, where):
    """a public line written after the lines under test, and waited for at the server: the plugin read past all of
    them by then, and none of them having come means none will (Q4 of a review of 0dabfa6: without it the check could
    pass before the pump had looked)"""
    tag = f"CONTROL-{time.monotonic_ns()}"
    say(log, f"boot+900.000 {tag} {where}")
    check(fp.until(lambda: lan.has(tag), 20), f"(a line written {where} reached the server: the plugin read past them)")


async def wyoming_writers(log, lan):
    """the same through Wyoming (proto_wyoming.c): a transcript and a server's error text with sentinels"""
    from wyoming.asr import Transcript
    from wyoming.client import AsyncTcpClient
    from wyoming.error import Error
    from wyoming.satellite import RunSatellite
    async with AsyncTcpClient("127.0.0.1", fp.PORT) as c:
        await c.write_event(RunSatellite().event())
        await c.write_event(Transcript(text="turn on\nSENTINEL-W1 the bedroom light").event())
        await c.write_event(Error(text="Unable to understand\nSENTINEL-W2 what you said", code="intent-failed").event())
        await asyncio.sleep(1.5)
    text = open(log, errors="replace").read()
    check("SENTINEL-W1" in text and "SENTINEL-W2" in text, "(Wyoming: hassmic wrote the transcript and the error text into its log)")
    control(log, lan, "after the Wyoming sentinels")
    check(not [l for _, l in lan.lines if "SENTINEL-W" in l], f"Wyoming: none of them reaches the plugin's server: {[l for _, l in lan.lines if 'SENTINEL-W' in l][:3]}")


async def real_writers(proc, log, lan):
    """S1/S2 of a review of 08399dc: hassmic's own writers, driven as Home Assistant drives them, with sentinels in what
    is said, answered, fetched, named and failed; the log as the plugin gets it reaches LAN: no sentinel may.  What
    the same run writes unmarked (the pipeline's steps) does reach it, and the sentinels are in the log (the page's)"""
    import types
    from aioesphomeapi import VoiceAssistantEventType as Ev, VoiceAssistantTimerEventType as Tm
    from esphome_common import home_assistant, sync
    ha = await home_assistant(types.SimpleNamespace(PORT=fp.PORT)); c = ha.cli
    proc.send_signal(signal.SIGUSR1)                    # the PC's wake word
    await asyncio.wait_for(ha.va.started.wait(), 10)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "remind me\nSENTINEL-T1 about my doctor's appointment"})
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_START, {"text": "Sure. Here is your list:\r\nSENTINEL-R1 call Dr. Miller"})
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{fp.PORT + 7}/api/tts_proxy/SENTINEL-U1-token.wav"})
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await sync(c); await asyncio.sleep(1.5)
    c.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_STARTED, "t9", "SENTINEL-N1 take the pills", 60, 60, True)
    c.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t9", "SENTINEL-N1 take the pills", 60, 0, False)
    await sync(c); await asyncio.sleep(1)
    proc.send_signal(signal.SIGUSR1)
    await asyncio.sleep(1)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "intent-failed", "message": "Unable to understand\nSENTINEL-E1 what you said"})
    await sync(c); await asyncio.sleep(3)              # (the log pump: once a second)
    await c.disconnect()
    text = open(log, errors="replace").read()
    check(all(x in text for x in ("SENTINEL-T1", "SENTINEL-R1", "SENTINEL-U1", "SENTINEL-N1", "SENTINEL-E1")),
          "(hassmic wrote the sentinels into its log: transcript, reply, media address, timer name, error text)")
    control(log, lan, "after hassmic's sentinels")
    check(not [l for _, l in lan.lines if "SENTINEL" in l], f"none of them reaches the plugin's server: {[l for _, l in lan.lines if 'SENTINEL' in l][:3]}")
    check(any("voice assistant: subscribed" in l for _, l in lan.lines), "what the same run wrote unmarked does")


async def main():
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log")
    for k in ("author", "index"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    man = json.load(open(f"{ROOT}/tests/plugins/testnet.json"))
    wman = json.load(open(f"{ROOT}/tests/plugins/testwake.json"))

    def package(base=man, program="testnet", **changes):
        m = dict(base, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, "p.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/{program}", os.path.join(top, "author.key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        plugcheck.list_package(out, os.path.join(top, "index.key"), os.path.join(top, "o.hmp"), arch="x86_64")
        return open(os.path.join(top, "o.hmp"), "rb").read()

    lan, out = Server(LAN), Server(OUT)
    proc = fp.start(state, log, env={"HASSMIC_PLUGIN_STORE": "10:1000", "HASSMIC_PLUGIN_LINES": "400"})   # (it is started some twenty times a minute here)
    plugdir = os.path.join(state, "plugins", "testnet")
    plog = lambda: [l.split("plugin testnet: ", 1)[1] for l in open(log, errors="replace").read().splitlines() if "plugin testnet: " in l]
    def says(text, n0=0, secs=10): return fp.until(lambda: any(text in l for l in plog()[n0:]), secs)
    try:
        wait_port(fp.PORT, fp.WEB)
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "logged in with the button")
        b.set(plugins="on")

        # ---- the install: what it may do, line by line, and the button
        st, r = fp.upload(b, package())
        lines = [c["line"] for c in r.get("caps", [])]
        check(st == 200 and r.get("button") is True and r.get("requires", {}).get("net.server") == 1 and "api" not in r, f"a new plugin waits for the button (what it requires in the check's answer): {st} {r.get('error')}")
        check("Keeps up to 4 kB of its own data on this Echo" in lines and "Reads this Echo's technical log" in lines
              and 'Connects to the server you enter in "Log server" (UDP, the port you enter in "Port"); and the one in "Log server" (TCP, port 16996)' in lines,
              f"one line per capability, the server by the row it is entered in, its protocol and ports: {lines}")
        st, r = fp.post(b, "/api/plugins/install")
        proc.send_signal(signal.SIGUSR2)
        check(fp.until(lambda: (fp.plug(b, "testnet") or {}).get("accepted")), "the press installs it")
        # (a wake word plugin beside it: its lines go to no plugin)
        st, r = fp.upload(b, package(wman, "testwake")); fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(fp.until(lambda: (fp.plug(b, "testwake") or {}).get("accepted")), "and a wake word plugin")
        for bad, why in ((dict(man["capabilities"], **{"net.server": [{"setting": "mode", "proto": "udp", "ports": [514]}]}), "a row of type host"),
                         (dict(man["capabilities"], **{"net.server": [{"setting": "server", "proto": "udp", "ports": [5555]}]}), "not 5555"),
                         (dict(man["capabilities"], **{"net.server": [{"setting": "server", "proto": "sctp", "ports": [514]}]}), "\"tcp\" or \"udp\""),
                         (dict(man["capabilities"], **{"net.server": [{"setting": "server", "proto": "udp", "ports": [514], "any": True}]}), "a key this version does not read"),
                         ({"wake": True, "files": 1, "storage": 1, "log.read": True}, "way out")):
            st, r = fp.upload(b, package(id="badnet", capabilities=bad))
            check(st == 400 and why in r.get("error", ""), f"refused: {why}: {r.get('error')}")
            fp.post(b, "/api/plugins/cancel")
        # the server and its port are the owner's to enter: a default in either row refused (N1 of a review of 08399dc:
        # a public default reached the author's server with nothing typed, the household's log with it)
        for i, row, why in ((0, dict(man["settings"][0], default=OUT), "the row server, where the owner enters the server, has a default"),
                            (1, dict(man["settings"][1], default=514), "the row port, where the owner enters the port, has a default")):
            rows = list(man["settings"]); rows[i] = row
            st, r = fp.upload(b, package(id="badnet", settings=rows))
            check(st == 400 and why in r.get("error", ""), f"refused: {why}: {r.get('error')}")
            fp.post(b, "/api/plugins/cancel")
        # the dialog's lines whole, however long (N3: cut at 300 bytes, a second server was not in what the owner saw)
        lab = lambda n: (f"Server {n}: as a name or an address on your own home network, please" + "x" * 64)[:64]
        p8 = [514, 1514, 5140, 10514, 20514, 30514, 40514, 50514]
        rows = [{"name": "server", "label": lab(1), "type": "host"}, {"name": "backup", "label": lab(2), "type": "host"}]
        caps = {"net.server": [{"setting": "server", "proto": "udp", "ports": p8}, {"setting": "server", "proto": "tcp", "ports": p8},
                               {"setting": "backup", "proto": "tcp", "ports": p8}, {"setting": "backup", "proto": "udp", "ports": p8}]}
        st, r = fp.upload(b, package(id="longnet", settings=rows, capabilities=caps))
        line = next((c["line"] for c in r.get("caps", []) if c.get("cap") == "net.server"), "")
        check(st == 200 and len(line.encode()) > 300 and line.endswith(f'and the one in "{lab(2)}" (UDP, ports 514, 1514, 5140, 10514, 20514, 30514, 40514, 50514)'),
              f"net.server's line in the dialog, {len(line.encode())} bytes, whole to the last server's last port: {line[-120:]}")
        fp.post(b, "/api/plugins/cancel")

        # ---- the server the owner entered, and nothing else
        n0 = len(plog())
        st, r = fp.post(b, "/api/plugins/testnet/set", f"port={UDP}")
        st, r = fp.post(b, "/api/plugins/testnet/set", f"server={LAN}")
        check(st == 200, f"the owner enters the port and the server: {r}")
        check(fp.logged(log, "plugins: testnet: the server you entered changed: started again"), "a server entered: the plugin started again for it")
        check(fp.until(lambda: (fp.plug(b, "testnet") or {}).get("state") == "running", 20), "the plugin runs")
        check(says("follows the log: yes") and says("connect: ok (0)", n0), f"it follows the log and has its socket: {plog()[:8]}")
        check(fp.logged(log, f"plugins: testnet: connected to {LAN}:{UDP} (UDP)"), "the connection is said in the log, the entry's protocol and the port the owner entered")
        # lines as their writers put them: hassmic's (milliseconds), root's (none), the private ones and a wake plugin's
        mark = f"marker-{os.getpid()}"
        say(log, f"boot+100.123 {mark} public line")
        say(log, f"boot+100.124 (private) transcript: {mark} what was said")
        say(log, f"boot+100.125 bluetooth: {mark} paired aa:bb:cc:dd:ee:ff and a2dp 001122aabbcc")
        say(log, f"boot+100.126 sendspin: {mark} pairing token SP:0ABCDEFGH234")
        say(log, f"boot+100.127 plugin testwake: {mark} a wake plugin's own line")
        say(log, f"boot+100.128 plugins: testwake: {mark} what the broker says of it")
        say(log, f"boot+100.12 == {mark} update installed (a root event)")
        say(log, f"boot+100.13 wifi: {mark} joined MyHomeNetwork (root, not an event)")
        say(log, f"no stamp {mark} at all")
        check(fp.until(lambda: lan.has(f"{mark} public line", "udp")), f"hassmic's own line reaches the server the owner entered: {lan.lines[-3:]}")
        time.sleep(1.5)
        got = [l for _, l in lan.lines if mark in l]
        check(not any("what was said" in l or "a wake plugin's own" in l or "what the broker says of it" in l or "MyHomeNetwork" in l or "at all" in l for l in got),
              f"not what was said, a wake word plugin's lines, root's other lines, a line without a stamp: {got}")
        check(any("update installed" in l for l in got), f"root's events pass: {got}")
        check(any("paired **:**:**:**:**:** and a2dp ************" in l for l in got) and any("SP:0*********" in l for l in got),
              f"MAC addresses and the pairing token blanked: {got}")
        st, _, body = b.call("GET", "/api/log/0")
        check(st == 200 and b"(private) transcript" in body and b"aa:bb:cc:dd:ee:ff" in body and b"SP:0*********" in body,
              "the page's own log shows the owner all of it, but for the token")
        await real_writers(proc, log, lan)
        p = fp.plug(b, "testnet")
        check(p and p.get("log") is True and p.get("log_lines", 0) > 0 and p.get("net_ok", -1) >= 0 and p.get("storage_kb") == 4,
              f"its card: it follows the log, lines given to it, connected, 4 kB: {p and {k: p.get(k) for k in ('log', 'log_lines', 'net_ok', 'storage_kb', 'storage_used')}}")

        # ---- storage: the cursor kept; a restart goes on from it
        cur = os.path.join(plugdir, "storage", "cursor")
        check(fp.until(lambda: os.path.exists(cur)), "it keeps its cursor in its storage")
        kept = open(cur).read() if os.path.exists(cur) else ""
        check(len(kept) > 17 and kept[16] == ":" and os.stat(cur).st_mode & 0o777 == 0o600, f"a cursor, kept as the plugin's own: {kept}")
        n0 = len(plog())
        fp.post(b, "/api/plugins/testnet/enable", "off")
        check(fp.until(lambda: (fp.plug(b, "testnet") or {}).get("state") == "stopped"), "switched off")
        say(log, f"boot+101.000 {mark} written while it was off")
        fp.post(b, "/api/plugins/testnet/enable", "on")
        check(says("kept cursor: 0", n0) or says("kept cursor: ", n0), "started again, it reads the cursor it kept")
        check(fp.until(lambda: lan.has(f"{mark} written while it was off")), "and gets what was written while it was off")
        check(sum(1 for _, l in lan.lines if f"{mark} public line" in l) == 1, "nothing twice")

        # ---- what it may not
        n0 = len(plog())
        fp.post(b, "/api/plugins/testnet/set", "mode=probe")
        check(says("own socket: -1 (Operation not permitted)", n0), f"a socket of its own: the filter refuses it: {plog()[n0:]}")
        check(fp.logged(log, "plugins: testnet: asked to connect where it may not (another host than the one entered"), "a CONNECT to another host: refused, said")
        # (the second refusal in 10 s: counted, said by the count; R5 of a review of 0dabfa6: a line each was 200 a second)
        check(fp.logged(log, "plugins: testnet: 1 more requests refused in the last 10 s", secs=15), "a port its entry does not list: refused, said in the count")
        fp.post(b, "/api/plugins/testnet/set", "mode=forward")
        # each a server's row changed: the plugin started again by hassmic itself (no switching off and on here)
        for host, why, word in (("127.0.0.1", "2", "loopback"), ("localhost", "2", "loopback by name"), (OWN, "2", "the Echo's own address"),
                                (LOCAL, "2", "an address routed to the Echo itself (a local route, over lo)"),
                                ("", "8", "no server entered"), ("nothing.invalid", "6", "a name without an address")):
            n0 = len(plog())
            fp.post(b, "/api/plugins/testnet/set", f"server={host}")
            check(says(f"connect: refused ({why})", n0, 20), f"{word}: refused ({why}): {plog()[n0:][-3:]}")
        # outside the local network: the owner's server all the same, said so
        n0 = len(plog())
        fp.post(b, "/api/plugins/testnet/set", f"server={OUT}")
        check(says("connect: ok (0)", n0, 20) and fp.logged(log, f"connected to {OUT}:{UDP} (UDP, outside the local network: the owner's server)"), "a server outside the network the owner entered: reached, and said so")
        say(log, f"boot+102.000 {mark} to the server outside")
        check(fp.until(lambda: out.has(f"{mark} to the server outside")), "lines reach it")
        # TCP, at the entry's listed port; the old server (outside) hears nothing once the owner changed it (N2)
        n0 = len(plog())
        fp.post(b, "/api/plugins/testnet/set", "mode=tcp"); fp.post(b, "/api/plugins/testnet/set", f"server={LAN}")
        check(says("connect: ok (0)", n0, 20) and fp.until(lambda: lan.conns >= 1), "the TCP entry: connected at its port")
        say(log, f"boot+103.000 {mark} over TCP")
        check(fp.until(lambda: lan.has(f"{mark} over TCP", "tcp")), "lines over TCP")
        time.sleep(1)
        check(not out.has(f"{mark} over TCP"), "the server before the change hears nothing after it")
        # the server cleared: started again, nothing connected, nothing to the server that was
        n0 = len(plog())
        fp.post(b, "/api/plugins/testnet/set", "server=")
        check(says("connect: refused (8)", n0, 20), f"the server cleared: no connection (UNSET): {plog()[n0:][-3:]}")
        say(log, f"boot+103.500 {mark} after the server was cleared")
        time.sleep(2)
        check(not lan.has(f"{mark} after the server was cleared") and not out.has(f"{mark} after the server was cleared"), "and no server hears what is written after")
        # a connect in ready, before READY: refused at once (BAD), the plugin goes on and connects from its tick (N4)
        n0 = len(plog())
        fp.post(b, "/api/plugins/testnet/set", "mode=readyconn"); fp.post(b, "/api/plugins/testnet/set", f"server={LAN}")
        check(says("connect in ready: -1 (3)", n0, 20) and says("connect: ok (0)", n0, 10), f"a connect before READY: refused at once (BAD), then from tick: {plog()[n0:][-4:]}")
        check((fp.plug(b, "testnet") or {}).get("state") == "running" and not fp.logged(log, "testnet: gone (it did not say READY", secs=1), "it was not stopped for READY")
        # a server that never answers: given up within 10 s as a whole, the plugin runs on (N4: 10 s an address, and a
        # connect in ready had it stopped, counted as a crash)
        slow = peerhost.peer_socket("tcp", SLOW, TCP, 0); fills = []
        for _ in range(3):
            c = socket.socket(); c.setblocking(False)
            try: c.connect((SLOW, TCP))
            except BlockingIOError: pass
            fills.append(c)
        time.sleep(0.5)
        n0 = len(plog()); t0 = time.monotonic()
        fp.post(b, "/api/plugins/testnet/set", "mode=tcp"); fp.post(b, "/api/plugins/testnet/set", f"server={SLOW}")
        check(says("connect: refused (7)", n0, 16), f"a server that never answers: FAILED: {plog()[n0:][-3:]}")
        took = time.monotonic() - t0
        check(9 <= took <= 14, f"... after the request's 10 s as a whole ({took:.1f} s from the change)")
        check((fp.plug(b, "testnet") or {}).get("state") == "running", "and the plugin runs on")
        for c in fills: c.close()
        slow.close()
        fp.post(b, "/api/plugins/testnet/set", "mode=forward"); fp.post(b, "/api/plugins/testnet/set", f"server={LAN}")

        # ---- storage within its kB and the Echo's writes
        n0 = len(plog())
        fp.post(b, "/api/plugins/testnet/set", "mode=bigput")
        fp.post(b, "/api/plugins/testnet/enable", "off"); fp.until(lambda: (fp.plug(b, "testnet") or {}).get("state") == "stopped"); fp.post(b, "/api/plugins/testnet/enable", "on")
        check(says("bad key: -1 (3)", n0, 20), f"it ran its writes: {plog()[n0:]}")
        pl = plog()[n0:]
        check("big1: 0 (0)" in pl and "big2: -1 (5)" in pl, f"3000 bytes kept; 3000 more over its 4 kB: refused (LIMIT): {[l for l in pl if l.startswith('big')]}")
        counts = [l for l in pl if l.startswith("count ")]
        check(any(l.endswith(": 0") for l in counts) and counts and counts[-1].endswith(": 1"), f"writes in a row: the first ones written, the last kept by the client for later: {counts}")
        check(fp.until(lambda: os.path.exists(os.path.join(plugdir, "storage", "count")) and open(os.path.join(plugdir, "storage", "count")).read() == "11", 15),
              "and written once the Echo takes writes again: the latest value")
        check(fp.logged(log, "testnet: writes its data more often than once a minute"), "said once in the log")
        used = sum(os.path.getsize(os.path.join(plugdir, "storage", f)) for f in os.listdir(os.path.join(plugdir, "storage")))
        check(used <= 4096 and not os.path.exists(os.path.join(plugdir, "storage", "big2")), f"never over its kB: {used} bytes")

        # ---- the development flag
        n0 = len(plog())
        fp.post(b, "/api/plugins/testnet/set", "mode=badcall")
        st, r = fp.post(b, "/api/plugins/testnet/develop", "on")
        check(st == 200 and (fp.plug(b, "testnet") or {}).get("develop") is True, f"the flag on: {r}")
        check(says("the sandbox refused system call 41", n0, 20), f"a refused call stops it, the client names it: {plog()[n0:][-3:]}")
        check(fp.until(lambda: "the sandbox stopped it" in ((fp.plug(b, "testnet") or {}).get("detail") or "")), f"and its card says so: {(fp.plug(b, 'testnet') or {}).get('detail')}")
        n0 = len(plog())
        fp.post(b, "/api/plugins/testnet/develop", "off")
        fp.post(b, "/api/plugins/testnet/enable", "off"); fp.until(lambda: (fp.plug(b, "testnet") or {}).get("state") == "stopped"); fp.post(b, "/api/plugins/testnet/enable", "on")
        check(says("own socket: -1 (Operation not permitted)", n0, 20), "without it the same call is answered EPERM and the plugin goes on")
        fp.post(b, "/api/plugins/testnet/set", "mode=forward")

        # ---- storage from another Echo (the page's copy): out as a tar, in whole, or refused whole
        # sealed for this browser under the request's counter (S4: it may hold a token), its first line the source's word
        # whether it is a wake word plugin's
        got = {}
        st, _, sealed = b.call("GET", "/api/plugins/testnet/storage", lambda c: (got.__setitem__("c", c), b"")[1])
        data = (b.open_sealed(got.get("c"), sealed) or b"") if st == 200 else b""
        head, _, tar = data.partition(b"\n")
        # Q2 of a review of 0dabfa6: every sealed answer under a nonce of the Echo's own (in clear in front of it), and
        # the request taken once: a replay is refused, and two answers to one counter would not share a key stream
        hd = b.headers("GET", "/api/plugins/testnet/storage", b"")
        st1, _, a1 = req(fp.WEB, "GET", "/api/plugins/testnet/storage", b"", hd)
        st2, _, _ = req(fp.WEB, "GET", "/api/plugins/testnet/storage", b"", hd)
        # (a review of 7144548, 5: an id that begins with "storage" is routed as the handler reads it, and its GET is
        # taken once over a restart too; by strstr it was not.  No such plugin here: the route alone, 404 and then 401)
        hb = b.headers("GET", "/api/plugins/storagebox/storage", b"")
        sb1, _, _ = req(fp.WEB, "GET", "/api/plugins/storagebox/storage", b"", hb)
        # (a review of 7144548, 7: hassmic ended by SIGTERM tells its plugins STOP and waits for them, 2 s at most: the
        # stop callback's write lands; before, the launcher killed them as hassmic's socket closed)
        check(fp.until(lambda: (fp.plug(b, "testnet") or {}).get("state") == "running", 20), "(the forwarder runs)")
        n0 = len(plog()); t0 = time.time()
        proc.terminate(); proc.wait(10); took = time.time() - t0
        stopped = os.path.join(plugdir, "storage", "stopped")
        check(says("stop: kept (0, 0)", n0, 2) and os.path.exists(stopped) and os.path.getmtime(stopped) >= t0 - 0.01 and took < 3.5
              and fp.logged(log, "plugins: hassmic ends: ", secs=1),
              f"SIGTERM: the plugin's stop callback ran and its STORE_PUT landed before hassmic ended ({took:.1f} s): {plog()[n0:][-3:]}")
        proc = fp.start(state, log, env={"HASSMIC_PLUGIN_STORE": "10:1000", "HASSMIC_PLUGIN_LINES": "400"}); wait_port(fp.PORT, fp.WEB)
        st3, _, _ = req(fp.WEB, "GET", "/api/plugins/testnet/storage", b"", hd)
        sb3, _, _ = req(fp.WEB, "GET", "/api/plugins/storagebox/storage", b"", hb)
        check(sb1 == 404 and sb3 == 401, f"the storage GET of a plugin whose id begins with \"storage\" (storagebox): taken once over a restart too: {sb1} {sb3}")
        check(st1 == 200 and a1.startswith(b"hassmic sealed 3 ") and a1[:49] != sealed[:49] and st2 == 401 and st3 == 401,
              f"the storage's answer under a nonce of its own (another one each time), a replay refused, after a restart too: {st1} {a1[:49]!r} {st2} {st3}")
        check(fp.until(lambda: (fp.plug(b, "testnet") or {}).get("state") == "running", 20), "(the plugin runs again after the restart)")
        names = tarfile.open(fileobj=io.BytesIO(tar)).getnames() if st == 200 and head == b"hassmic storage 1 wake=0" else []
        check(st == 200 and b"cursor" not in sealed and "cursor" in names and "count" in names, f"what it keeps, sealed, as a tar after its first line: {head} {names}")
        def tar_of(files):
            o = io.BytesIO()
            with tarfile.open(fileobj=o, mode="w", format=tarfile.USTAR_FORMAT) as t:
                for n, d in files.items(): ti = tarfile.TarInfo(n); ti.size = len(d); t.addfile(ti, io.BytesIO(d))
            return o.getvalue()
        def carry(data, wake=0, head=True):
            data = (f"hassmic storage 1 wake={wake}\n".encode() if head else b"") + data
            st, r = fp.post(b, "/api/plugins/testnet/storage/begin", str(len(data)))
            if st != 200: return st, r
            for off in range(0, len(data), 200000):
                st, _, body = b.call("POST", f"/api/plugins/testnet/storage/chunk/{off}", lambda c: b.seal2(c, data[off:off + 200000]))
                if st != 200: return st, json.loads(body or b"{}")
            return fp.post(b, "/api/plugins/testnet/storage/done")
        st, r = carry(tar_of({"big": b"y" * 5000}))
        check(st == 400 and "kB" in r.get("error", "") and open(os.path.join(plugdir, "storage", "count")).read() == "11", f"a copy over its kB: refused whole, nothing replaced: {r}")
        st, r = carry(tar_of({"../x": b"1"}))
        check(st == 400 and not os.path.exists(os.path.join(plugdir, "x")), f"a key that is none: refused: {r}")
        st, r = carry(tar_of({"cursor": b"from-a-wake-plugin"}), wake=1)
        check(st == 400 and "wake word plugin's data" in r.get("error", "") and open(os.path.join(plugdir, "storage", "count")).read() == "11",
              f"a wake word plugin's data into one that is not: refused, nothing replaced (S4): {r}")
        st, r = carry(tar_of({"cursor": b"x"}), head=False)
        check(st == 400 and open(os.path.join(plugdir, "storage", "count")).read() == "11", f"a copy without the source's first line (not as an Echo sends it): refused: {r}")
        n0 = len(plog())
        st, r = carry(tar_of({"cursor": b"start-from-copy", "other": b"z"}))
        check(st == 200 and sorted(os.listdir(os.path.join(plugdir, "storage"))) == ["cursor", "other"], f"a copy in its place, whole: {st} {r} {os.listdir(os.path.join(plugdir, 'storage'))}")
        check(says("kept cursor: start-from-copy", n0, 20), "the plugin stopped for it and started with it")

        # ---- updates: what was accepted or less without the button; more with it, said
        st, r = fp.upload(b, package(version=2, capabilities=dict(man["capabilities"], storage=8)))
        ch = [c["line"] for c in r.get("changes", [])]
        check(st == 200 and r.get("button") is True and any("Keeps up to 8 kB" in l and "was 4 kB" in l for l in ch), f"more storage: the button, said: {ch}")
        fp.post(b, "/api/plugins/cancel")
        caps = dict(man["capabilities"]); caps["net.server"] = [caps["net.server"][0], dict(caps["net.server"][1], ports=[16996, 16997])]
        st, r = fp.upload(b, package(version=2, capabilities=caps))
        ch = [c["line"] for c in r.get("changes", [])]
        check(st == 200 and r.get("button") is True and any("Connects to the server you enter" in l and "not with the parameters you accepted" in l for l in ch), f"another port: the button, said: {ch}")
        fp.post(b, "/api/plugins/cancel")
        st, r = fp.upload(b, package(version=2, capabilities=dict(man["capabilities"], storage=2)))
        check(st == 200 and r.get("button") is False, f"less storage: no button: {r.get('changes')}")
        st, r = fp.post(b, "/api/plugins/install")
        check(fp.until(lambda: (fp.plug(b, "testnet") or {}).get("version") == 2), f"installed without the button: {r}")
        check(open(os.path.join(plugdir, "storage", "cursor")).read() == "start-from-copy", "its data kept by the update")

        # ---- a real wake word plugin's lines (Q4 of a review of 0dabfa6): its own stderr and the broker's lines about it
        # are written sealed (plugin.c log_lines, seal_note), and stay so once it is removed: a forwarder that reads the
        # log again from its start then gets none of them, though no wake plugin of that id is installed any more
        text = open(log, errors="replace").read()
        check("(sealed) plugin testwake: ready on" in text and "(sealed) plugins: testwake: running" in text,
              "a wake word plugin's own lines and the broker's about it: written sealed")
        st, r = fp.post(b, "/api/plugins/testwake/remove", "remove")
        check(st == 200 and fp.until(lambda: not fp.plug(b, "testwake")), f"the wake word plugin removed: {r}")
        n0 = len(plog()); nl = len(lan.lines)
        st, r = carry(tar_of({"cursor": b"start"}))
        check(st == 200 and says("follows the log: yes", n0, 20), f"the forwarder follows the log again from its oldest line: {r}")
        control(log, lan, "after the log read again")
        again = [l for _, l in lan.lines[nl:]]
        check(len(again) > 50 and not [l for l in again if "testwake: ready on" in l or "testwake: running" in l or "testwake: gone" in l],
              f"read again from the start: {len(again)} lines, none of the removed wake plugin's real ones: {[l for l in again if 'testwake' in l and mark not in l][:3]}")
        # ---- Wyoming (proto_wyoming.c): the same writers' sentinels, through the other protocol
        proc.terminate(); proc.wait(10)
        proc = fp.start(state, log, extra=("-P", "wyoming"), env={"HASSMIC_PLUGIN_STORE": "10:1000", "HASSMIC_PLUGIN_LINES": "400"}); wait_port(fp.PORT, fp.WEB)
        check(fp.until(lambda: (fp.plug(b, "testnet") or {}).get("state") == "running", 20), "(hassmic as a Wyoming satellite, the forwarder running)")
        await wyoming_writers(log, lan)

        # ---- remove: its data with it
        st, r = fp.post(b, "/api/plugins/testnet/remove", "remove")
        check(st == 200 and fp.until(lambda: not os.path.exists(plugdir)), f"removed, its storage with it: {r}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
    if check.failed:
        print("---- log"); print(open(log, errors="replace").read()[-8000:])
    shutil.rmtree(top, ignore_errors=True)              # (its own folder, by its name: nothing else in /tmp)
    print("plugins-net: " + ("FAILED" if check.failed else "all passed"))
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    if not reachable():
        # alone: in a namespace of the kind tests/run.py makes, with the other hosts
        if os.environ.get("HASSMIC_NETNS_DONE"): sys.exit("fake_plugins_net: no other hosts in this namespace (tests/run.py netns(peer=True))")
        import run
        os.environ["HASSMIC_NETNS_DONE"] = "1"
        os.execvp("unshare", run.netns([sys.executable, os.path.abspath(__file__)], peer=True))
    try: asyncio.run(main())
    finally:                                            # the other hosts' socket and its folder (tests/run.py made them)
        d = os.path.dirname(os.environ.get("HASSMIC_PEER_SOCK", "")) if reachable() else ""
        if d and os.path.basename(d).startswith("hassmic-peer."):
            for f in os.listdir(d): os.unlink(os.path.join(d, f))
            os.rmdir(d)
