#!/usr/bin/env python3
"""The log to a syslog server and to ESPHome's log subscribers (logfwd.c) against build/hassmic-host: the settings from
the page (a browser of tests/webclient.py), a UDP and a TCP server of our own on lo, and Home Assistant's side
(aioesphomeapi's subscribe_logs).  What boot.log gets from root (main.sh) is played by lines this test appends.

UDP: RFC 5424 from the line's own time, severity from its words, the node name as host, the token blanked, the test
line from the page; the server gone (a closed port: refused) holds the log, which follows once it is back; a rotation
(copy + truncate, as main.sh) while it is away loses nothing of what boot.log.1 kept; "boot+" times in real time only
after this boot's "== satellite start".  A restart of hassmic carries on where it was.  TCP: octet counting, RFC 3164
with a newline after each, nothing lost over a server restart.  Off: nothing more, the place forgotten.  ESPHome:
dump_config, lines as ESPHome's look at their level, a level that filters, unsubscribe by level NONE."""
import asyncio, json, os, re, shutil, signal, socket, subprocess, sys, tempfile, threading, time
from aioesphomeapi import APIClient, LogLevel
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, WEB = 16991, 16992
UDP_PORT, TCP_PORT, CLOSED_PORT = 16993, 16994, 16995
BOOT = "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0"
RE5424 = re.compile(r"<(\d+)>1 (\S+) (\S+) hassmic - - - (.*)", re.S)


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def until(cond, secs=10):
    end = time.monotonic() + secs
    while not cond() and time.monotonic() < end: time.sleep(0.05)
    return cond()


class Udp:
    """a syslog server on UDP: every datagram, decoded"""
    def __init__(self, port):
        self.port, self.got, self.s = port, [], None
        self.open()

    def open(self):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("127.0.0.1", self.port)); self.s.settimeout(0.1)
        self.run = True
        self.t = threading.Thread(target=self.loop, daemon=True); self.t.start()

    def loop(self):
        while self.run:
            try: self.got.append(self.s.recv(65536))
            except socket.timeout: continue
            except OSError: return

    def close(self):
        self.run = False; self.t.join(); self.s.close()

    def texts(self): return [g.decode("utf-8", "replace") for g in self.got]
    def has(self, needle): return any(needle in t for t in self.texts())


class Tcp:
    """a syslog server on TCP: frames by octet counting (RFC 6587), or lines (3164)"""
    def __init__(self, port):
        self.port, self.got, self.raw = port, [], b""
        self.open()

    def open(self):
        self.ls = socket.socket(); self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind(("127.0.0.1", self.port)); self.ls.listen(4); self.ls.settimeout(0.1)
        self.run = True; self.conns = []
        self.t = threading.Thread(target=self.loop, daemon=True); self.t.start()

    def loop(self):
        while self.run:
            try: c, _ = self.ls.accept(); c.settimeout(0.1); self.conns.append(c); threading.Thread(target=self.read, args=(c,), daemon=True).start()
            except socket.timeout: continue
            except OSError: return

    def read(self, c):
        buf = b""
        while self.run:
            try: d = c.recv(65536)
            except socket.timeout: continue
            except OSError: return
            if not d: return
            self.raw += d; buf += d
            while buf:
                m = re.match(rb"(\d+) ", buf)
                if m and buf[:1] != b"<":
                    n = int(m.group(1)); s = m.end()
                    if len(buf) < s + n: break
                    self.got.append(buf[s:s + n]); buf = buf[s + n:]
                elif b"\n" in buf:
                    l, buf = buf.split(b"\n", 1); self.got.append(l)
                else: break

    def close(self):
        self.run = False
        for c in self.conns:
            try: c.shutdown(socket.SHUT_RDWR)
            except OSError: pass
            c.close()
        self.ls.close(); self.t.join()

    def texts(self): return [g.decode("utf-8", "replace") for g in self.got]
    def has(self, needle): return any(needle in t for t in self.texts())


def append(log, *lines):
    """root's lines, as main.sh's ">> $LOG" writes them"""
    with open(log, "a") as f:
        for l in lines: f.write(l + "\n")


def utc():
    t = time.time(); return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(t)) + ".%03dZ" % (int(t * 1000) % 1000)


def start(state, log):
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(state, "models"),
               HASSMIC_FAKE_WHISPER="1", HASSMIC_LOG=log, HASSMIC_SYSLOG_SAVE_MS="300", HASSMIC_BOOT_ID=BOOT)
    # O_APPEND, as main.sh's ">> $LOG": this test's own lines and a rotation's truncate land where they should
    p = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Syslog", "-L", "-z", "0", "-o", "0",
                          "-a", "16996", "-W", str(WEB)], env=env, stderr=open(log, "a"))
    wait_port(PORT, WEB)
    return p


async def esphome_logs():
    """Home Assistant's side: subscribe_logs, with dump_config, at INFO; then at WARN; then NONE"""
    cli = APIClient("127.0.0.1", PORT, None); await cli.connect(login=True)
    got = []
    unsub = cli.subscribe_logs(lambda m: got.append((m.level, m.message.decode("utf-8", "replace"))), log_level=LogLevel.LOG_LEVEL_INFO, dump_config=True)
    await asyncio.sleep(0.5)
    return cli, got, unsub


async def main():
    state = tempfile.mkdtemp(); log = os.path.join(state, "boot.log")
    open(log, "w").close()
    udp = Udp(UDP_PORT)
    proc = start(state, log)
    try:
        b = Browser(WEB)
        check(b.login_with_button(proc), "logged in by the button")
        st = b.state()
        srv = {s["name"]: s for s in st["settings"] if s["name"].startswith("syslog")}
        check(set(srv) == {"syslog", "syslog_server", "syslog_protocol", "syslog_format"}, f"the four settings: {sorted(srv)}")
        check(srv["syslog_server"]["type"] == "text" and srv["syslog_server"]["value"] == "" and srv["syslog_server"]["max"] == 128,
              f"syslog_server is text, empty, 128 at most: {srv['syslog_server']}")
        check(srv["syslog"]["value"] == 0 and st["syslog"]["state"] == "off", f"off by default: {st['syslog']}")

        r = b.set(syslog_server="bad host")
        check("syslog_server" in r.get("errors", "") and "not a valid host name" in r["errors"], f"a server with a space refused: {r}")
        r = b.set(syslog_server="host:70000")
        check("syslog_server" in r.get("errors", ""), "a port past 65535 refused")
        st, _, _ = b.call("POST", "/api/syslog/test", b"test")
        check(st == 409, f"the test line needs it on with a server: {st}")

        r = b.set(syslog="on")
        check(not r.get("errors") and b.state()["syslog"]["state"] == "no_server", "on without a server: says so")
        r = b.set(syslog_server=f"127.0.0.1:{UDP_PORT}")
        check(not r.get("errors"), f"server set: {r}")
        check(until(lambda: udp.has("syslog: sending to 127.0.0.1:%d via UDP, RFC 5424" % UDP_PORT)), "its own first line arrives (after the probe)")
        m = RE5424.match(udp.texts()[0]) if udp.got else None
        check(m and m.group(1) == "30" and re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z", m.group(2)) and m.group(3) == "echo-syslog",
              f"RFC 5424: <daemon.info>1, its time to the ms in UTC, the node name: {udp.texts()[:1]}")
        check(until(lambda: b.state()["syslog"]["state"] == "up"), f"state up: {b.state()['syslog']}")
        check(b.state()["syslog"]["server"] == f"127.0.0.1:{UDP_PORT}" and b.state()["syslog"]["protocol"] == "udp", "status names server and protocol")
        cfg = open(os.path.join(state, "config")).read()
        check(f"syslog=on\nsyslog_server=127.0.0.1:{UDP_PORT}\nsyslog_protocol=udp\nsyslog_format=rfc5424\n" in cfg, "kept in state/config")
        st, _, data = b.call("GET", "/api/export")
        check(f"syslog_server=127.0.0.1:{UDP_PORT}" in data.decode(), "in an export: one log server for the house")

        st, _, data = b.call("POST", "/api/syslog/test", b"test")
        check(st == 200 and json.loads(data)["syslog"]["state"] == "up", f"test line asked for: {st}")
        check(until(lambda: udp.has("syslog: test line from the settings page of Echo Syslog")), "the test line arrives")

        # root's lines: their own time, severity from their words, the token blanked, a line longer than a datagram
        t = utc()
        append(log, f"{t} == firewall rules wrong", f"{t} settings: cannot write x", f"{t} pairing SP:0ABCDEF234", f"{t} " + "ü" * 1000, "no stamp at all")
        check(until(lambda: udp.has("no stamp at all")), "root's lines arrive")
        tx = udp.texts()
        def one(needle): return next((RE5424.match(x) for x in tx if needle in x), None)
        m = one("firewall rules wrong")
        check(m and m.group(1) == "29" and m.group(2) == t.replace(" ", "T"), f"'==': notice, the line's own time: {m and m.group(0)}")
        m = one("cannot write x")
        check(m and m.group(1) == "28", "a failure's words: warning")
        check(one("pairing SP:0*********") and not udp.has("SP:0ABCDEF234"), "the pairing token blanked")
        long = next((g for g in udp.got if "ü".encode() * 10 in g), b"")
        check(0 < len(long) <= 1400 and long.decode("utf-8", "strict"), f"a long line: one datagram of {len(long)} bytes, whole characters")
        m = one("no stamp at all")
        check(m and m.group(2) == "-", "no stamp: no time (the server's)")

        # "boot+": real time after this boot's start line, not after another boot's
        append(log, f"boot+40.5 == satellite start, uptime 40s, factory from /system/hassmic, boot 11111111-2222-3333-4444-555555555555",
               "boot+41.25 line of another boot",
               f"boot+42.0 == satellite start, uptime 42s, factory from /system/hassmic, boot {BOOT}", "boot+43.5 line of this boot")
        check(until(lambda: udp.has("line of this boot")), "boot+ lines arrive")
        tx = udp.texts()
        m = one("line of another boot")
        check(m and m.group(2) == "-" and m.group(4).startswith("boot+41.25 "), f"another boot: no time, its boot+ kept: {m and m.group(0)}")
        m = one("line of this boot")
        bt = time.clock_gettime(time.CLOCK_BOOTTIME); exp = time.time() - (bt - 43.5)
        ok = m and m.group(2) != "-" and m.group(4) == "line of this boot"
        if ok:
            got = time.mktime(time.strptime(m.group(2)[:19], "%Y-%m-%dT%H:%M:%S")) - time.timezone
            ok = abs(got - exp) < 3
        check(ok, f"this boot: boot+ in real time: {m and m.group(0)}")

        # the server away (refused): held, then all of it once back, in order
        udp.close()
        n0 = len(udp.got)
        append(log, *[f"{utc()} held {i}" for i in range(1, 6)])
        check(until(lambda: b.state()["syslog"]["state"] == "down", 15), f"down while refused: {b.state()['syslog']}")
        s = b.state()["syslog"]
        check(s["behind"] > 0 and "refused" in s["error"].lower(), f"says why and how much waits: {s}")
        # rotated meanwhile, as main.sh does: copy, then truncate; then more
        shutil.copy(log, log + ".1"); open(log, "r+").truncate(0)
        append(log, *[f"{utc()} after rotation {i}" for i in range(1, 4)])
        udp.open()
        check(until(lambda: udp.has("after rotation 3"), 25), "back: what waited follows")
        tx = [x for x in udp.texts()[n0:]]
        order = [m for m in (x.split("hassmic - - - ", 1)[-1] for x in tx) if m.startswith(("held ", "after rotation "))]
        want = [f"held {i}" for i in range(2, 6)] + [f"after rotation {i}" for i in range(1, 4)]
        check(order[-len(want):] == want and len(order) - len(want) <= 1,
              f"held lines (the first may be what the refusal was for), then boot.log.1's rest, then the new boot.log, in order: {order}")
        check(udp.has("is reachable again"), "and the log says it was reached again")

        # a restart of hassmic: carries on where it was; nothing from before is sent again
        time.sleep(0.6)                                  # the place saved (HASSMIC_SYSLOG_SAVE_MS)
        proc.kill(); proc.wait()
        append(log, f"{utc()} hassmic exited rc=-9, restart in 3 s")
        n1 = len(udp.got)
        proc = start(state, log)
        check(until(lambda: udp.has("hassmic exited rc=-9")), "the lines written while hassmic was down follow after its restart")
        check(until(lambda: any("resuming where the last run stopped" in x for x in udp.texts()[n1:])), "it says it carries on")
        tx = udp.texts()[n1:]
        again = [m for m in (x.split("hassmic - - - ", 1)[-1] for x in tx) if m.startswith(("held ", "after rotation "))]
        check(not again, f"nothing from before the place again: {again}")
        b = Browser(WEB, b.sk)

        # ESPHome: dump_config, lines at their level, a level that filters, NONE ends it
        cli, got, unsub = await esphome_logs()
        await asyncio.sleep(0.3)
        check(any(l == LogLevel.LOG_LEVEL_CONFIG and m.startswith("[C][hassmic]: hassmic ") for l, m in got), f"dump_config: [C] lines: {got[:2]}")
        check(any(m.startswith("[C][hassmic]: syslog: {") for _, m in got), "dump_config names the syslog state")
        append(log, f"{utc()} esphome sees this", f"{utc()} esphome: something failed", f"{utc()} token SP:0ABC234")
        for _ in range(60):
            if any("token" in m for _, m in got): break
            await asyncio.sleep(0.05)
        check((LogLevel.LOG_LEVEL_INFO, "[I][hassmic]: esphome sees this") in got, "an info line, without its time, ESPHome's look")
        check((LogLevel.LOG_LEVEL_WARN, "[W][hassmic]: esphome: something failed") in got, "a warning at WARN")
        check(any(m == "[I][hassmic]: token SP:0******" for _, m in got), "the token blanked there too")
        unsub()
        got2 = []
        cli.subscribe_logs(lambda m: got2.append((m.level, m.message.decode())), log_level=LogLevel.LOG_LEVEL_WARN)
        await asyncio.sleep(0.3)
        append(log, f"{utc()} only info", f"{utc()} refused at warn")
        for _ in range(60):
            if got2: break
            await asyncio.sleep(0.05)
        await asyncio.sleep(0.3)
        check([m for _, m in got2] == ["[W][hassmic]: refused at warn"], f"subscribed at WARN: warnings only: {got2}")
        cli.subscribe_logs(lambda m: got2.append(m), log_level=LogLevel.LOG_LEVEL_NONE)
        await asyncio.sleep(0.3); n = len(got2)
        append(log, f"{utc()} after none failed")
        await asyncio.sleep(0.8)
        check(len(got2) == n, "level NONE: nothing more")
        await cli.disconnect()

        # TCP: octet counting; a server restart loses nothing
        tcp = Tcp(TCP_PORT)
        r = b.set(syslog_protocol="tcp", syslog_server=f"127.0.0.1:{TCP_PORT}")
        check(not r.get("errors"), f"TCP: {r}")
        check(until(lambda: tcp.has("syslog: now sending to 127.0.0.1:%d via TCP" % TCP_PORT)), "TCP: its line arrives")
        check(re.match(rb"\d+ <\d+>1 ", tcp.raw) is not None, f"octet counting: {tcp.raw[:40]}")
        check(until(lambda: b.state()["syslog"]["state"] == "up"), "TCP up")
        tcp.close()
        append(log, *[f"{utc()} tcp held {i}" for i in range(1, 6)])
        check(until(lambda: b.state()["syslog"]["state"] == "down", 15), f"TCP down: {b.state()['syslog']}")
        tcp.got = []; tcp.open()
        check(until(lambda: tcp.has("tcp held 5"), 25), "TCP back: what waited follows")
        held = [x.split("hassmic - - - ", 1)[-1] for x in tcp.texts() if "tcp held" in x]
        check(held == [f"tcp held {i}" for i in range(1, 6)], f"TCP: every one of them, once, in order: {held}")

        # RFC 3164 over TCP: a newline after each
        r = b.set(syslog_format="rfc3164")
        check(until(lambda: any(re.fullmatch(r"<30>[A-Z][a-z]{2} [ \d]\d \d\d:\d\d:\d\d echo-syslog hassmic: .*", x) for x in tcp.texts())),
              f"RFC 3164: <pri>Mmm dd hh:mm:ss host hassmic: text: {tcp.texts()[-1:]}")

        # off: nothing more, the place forgotten
        r = b.set(syslog="off")
        check(until(lambda: b.state()["syslog"]["state"] == "off") and not os.path.exists(os.path.join(state, "syslog.pos")), "off: the place forgotten")
        n = len(tcp.got)
        append(log, f"{utc()} after off")
        time.sleep(1)
        check(not tcp.has("after off"), "off: nothing more goes")
        tcp.close()

        # the page carries the words
        js = open(f"{ROOT}/web/app.js").read()
        check(all(f"{k}:" in js for k in ("syslog", "syslog_server", "syslog_protocol", "syslog_format")), "the page has words for each")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
        udp.run = False
        if check.failed:
            print("--- log"); print(open(log, errors="replace").read()[-6000:])
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
