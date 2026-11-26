#!/usr/bin/env python3
"""Setting the Echo up from a phone (Improv Wi-Fi over Bluetooth LE), end to end: build/hassmic-host with ble.c's
peripheral role talking H4 to a controller played here on a pseudo terminal (HASSMIC_BT_DEV), a phone's GATT client
written from the Bluetooth spec, Improv packets built and parsed by py-improv-ble-client (what Home Assistant's
improv_ble integration uses), and root's side played by the real scripts/device/wifi.sh against tests/fake_wifi_tools.py.
Advertising (service data, name), GATT discovery, reads, notifications, writes in pieces and long (prepared) writes,
device info, identify, scan, no host name command, a wrong password, the switch, the settings page's address back,
adoption ending the setup, an Echo that is set up wanting the action button (for the switch and the scan, once per
switch, within its time, and only while it advertises for a lost network: a phone that connects again the moment a
link ends, or while nothing is advertised, gets nothing and is not kept), whose press is taken only while a phone is
connected and waits (never from the assistant with nobody there), and no stranger's login armed around it; how long
a phone is kept (not authorized and silent, "provisioned", the network back in pieces) and identify not as often as
asked; the factory reset by Home Assistant's button and by holding the action button, and wifi.sh forgetting the
networks.  Then a second Echo, set up and without its network from the start (more()): a login armed beside a
phone's wait ends with it, an approved browser's adb wait does not, a central connecting and leaving writes
state/web_wait once per quiet time, the silence that ends a link counts from what the phone last sent and its address
is served again at once, a failed switch's carrier is not the network's return, a network back for less than a
second at a time counts, a restart of hassmic with a phone's wait showing keeps strangers back.  Then two that are
not set up (taken()): scans do not keep a phone on an Echo that has its network, Home Assistant taking the Echo on
ends a phone's link and makes no wait of it (the owner's login is his press's), a switch under way and its
"provisioned" still end well, a scan beside a switch after the button's time is not taken, and state/web_wait at
the times an Echo runs with (a login twice the quiet time)."""
import asyncio, hashlib, json, os, queue, select, signal, struct, subprocess, sys, tempfile, threading, time, tty

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aioesphomeapi import APIClient
from webclient import Browser
from improv_ble_client import protocol as prot

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, ARB, WEB = 16990, 16992, 16991
SVC = bytes.fromhex("00467768622822724663277478268000")[::-1]            # on the air: least significant octet first
CHR = {n: bytes.fromhex(f"0046776862282272466327747826800{i}")[::-1] for n, i in (("state", 1), ("error", 2), ("rpc", 3), ("result", 4), ("caps", 5))}
H = lambda s: (s if isinstance(s, bytes) else s.encode()).hex()
PSK = lambda pw, ssid: hashlib.pbkdf2_hmac("sha1", pw.encode(), ssid.encode(), 4096, 32).hex()
CONN = 0x0040                                                             # the connection handle we give the phone's link


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


class Controller:
    """The Echo's Bluetooth chip as hassmic sees it through /dev/stpbt: H4 packets.  Every command succeeds; advertising is
    recorded; ACL data is put together into ATT PDUs for the phone below."""
    def __init__(self):
        self.m, self.s = os.openpty()
        tty.setraw(self.m); tty.setraw(self.s)
        self.path = os.ttyname(self.s)
        self.adv = {"on": 0, "data": b"", "rsp": b""}
        self.local_name = b""; self.inquiry_name = b""
        self.att = queue.Queue(); self.lock = threading.Lock(); self.buf = b""; self.rx = b""; self.dropped = 0
        self.up = False                  # a phone's link is up
        self.pending = False             # a phone's LE Create Connection is pending: it connects on the first advertising event
        self.reconnects = 0
        threading.Thread(target=self.reader, daemon=True).start()

    def send(self, b):
        with self.lock: os.write(self.m, b)

    def event(self, code, params): self.send(bytes([4, code, len(params)]) + params)

    def reader(self):
        while True:
            try: self.buf += os.read(self.m, 4096)
            except OSError: return
            while self.buf:
                t = self.buf[0]
                if t == 1:
                    if len(self.buf) < 4 or len(self.buf) < 4 + self.buf[3]: break
                    op, par = self.buf[1] | self.buf[2] << 8, self.buf[4:4 + self.buf[3]]
                    self.buf = self.buf[4 + self.buf[3]:]
                    self.command(op, par)
                elif t == 2:
                    if len(self.buf) < 5: break
                    n = self.buf[3] | self.buf[4] << 8
                    if len(self.buf) < 5 + n: break
                    h, pb, data = (self.buf[1] | self.buf[2] << 8) & 0xfff, self.buf[2] >> 4 & 3, self.buf[5:5 + n]
                    self.buf = self.buf[5 + n:]
                    self.event(0x13, struct.pack("<BHH", 1, h, 1))         # Number Of Completed Packets: the credit back
                    self.rx = data if pb != 1 else self.rx + data
                    if len(self.rx) >= 4 and len(self.rx) >= 4 + (self.rx[0] | self.rx[1] << 8):
                        l, cid = struct.unpack("<HH", self.rx[:4])
                        if cid == 4: self.att.put(self.rx[4:4 + l])
                        self.rx = b""
                else: self.buf = self.buf[1:]

    def command(self, op, par):
        if op == 0x0c13: self.local_name = par.split(b"\0", 1)[0]
        elif op == 0x0c52 and len(par) >= 3 and par[2] == 0x09: self.inquiry_name = par[3:2 + par[1]]
        ret = bytes(64)
        if op == 0x2002: ret = struct.pack("<HB", 251, 8)                  # LE Read Buffer Size: 251 bytes, 8 packets
        elif op == 0x2008: self.adv["data"] = par[1:1 + par[0]]
        elif op == 0x2009: self.adv["rsp"] = par[1:1 + par[0]]
        elif op == 0x200a: self.adv["on"] = par[0]
        self.event(0x0e, struct.pack("<BHB", 1, op, 0) + ret)
        if op == 0x0406:                                                    # Disconnect: the link ends, by the local host
            self.dropped += 1; self.up = False; self.event(0x05, struct.pack("<BHB", 0, par[0] | par[1] << 8, 0x16))
        if op == 0x200a and par[0] == 1 and self.pending and not self.up:
            self.reconnects += 1; self.connect()

    def connect(self, addr=bytes.fromhex("665544332211")):                  # (the central's address: whatever it says)
        self.adv["on"] = 0; self.up = True                                  # the controller stops advertising
        self.event(0x3e, bytes([1, 0]) + struct.pack("<HBB", CONN, 1, 0) + addr + struct.pack("<HHHB", 24, 0, 400, 0))

    def disconnect(self): self.up = False; self.event(0x05, struct.pack("<BHB", 0, CONN, 0x13))

    def adv_fields(self):
        out, d = {}, self.adv["data"]
        while len(d) >= 2 and d[0]:
            out.setdefault(d[1], d[2:1 + d[0]]); d = d[1 + d[0]:]
        return out


class Phone:
    """A GATT client over the controller's link: one request at a time, notifications kept apart"""
    def __init__(self, ctl):
        self.c = ctl; self.notes = []; self.mtu = 23

    def acl(self, pdu):
        l2 = struct.pack("<HH", len(pdu), 4) + pdu
        self.c.send(bytes([2]) + struct.pack("<HH", CONN | 0x2000, len(l2)) + l2)

    def pump(self, timeout):
        try: p = self.c.att.get(timeout=timeout)
        except queue.Empty: return None
        if p[0] == 0x1b: self.notes.append((struct.unpack("<H", p[1:3])[0], p[3:])); return "note"
        return p

    def req(self, pdu, timeout=3):
        self.acl(pdu); end = time.time() + timeout
        while time.time() < end:
            p = self.pump(end - time.time())
            if p is None: break
            if p != "note": return p
        return None

    def note(self, handle, timeout=5):
        end = time.time() + timeout
        while True:
            for i, (h, v) in enumerate(self.notes):
                if h == handle: del self.notes[i]; return v
            if time.time() >= end: return None
            self.pump(min(0.2, end - time.time()))

    def discover(self):
        r = self.req(bytes([0x02]) + struct.pack("<H", 247)); self.mtu = min(247, struct.unpack("<H", r[1:3])[0])
        svcs, h = [], 1
        while True:                                                         # Read By Group Type: primary services
            r = self.req(bytes([0x10]) + struct.pack("<HHH", h, 0xffff, 0x2800))
            if r[0] == 0x01: break
            w = r[1]
            for i in range(2, len(r), w): svcs.append((*struct.unpack("<HH", r[i:i + 4]), r[i + 4:i + w])); h = svcs[-1][1] + 1
            if h > 0xffff or h == 0: break
        chrs, (s, e) = {}, next((a, b) for a, b, u in svcs if u == SVC)
        h = s
        while h <= e:                                                       # Read By Type: characteristic declarations
            r = self.req(bytes([0x08]) + struct.pack("<HHH", h, e, 0x2803))
            if r[0] == 0x01: break
            w = r[1]
            for i in range(2, len(r), w):
                d = r[i:i + w]; chrs[d[5:]] = (d[2], struct.unpack("<H", d[3:5])[0]); h = struct.unpack("<H", d[:2])[0] + 1
        infos, h = {}, s
        while h <= e:                                                       # Find Information: the descriptors
            r = self.req(bytes([0x04]) + struct.pack("<HH", h, e))
            if r[0] == 0x01: break
            w = 4 if r[1] == 1 else 18
            for i in range(2, len(r), w): hh = struct.unpack("<H", r[i:i + 2])[0]; infos[hh] = r[i + 2:i + w]; h = hh + 1
        return svcs, chrs, infos

    def until(self, handle, value, timeout=5):
        """notifications of handle until one says value (each command clears the error first: a 0 comes before)"""
        end = time.time() + timeout
        while (v := self.note(handle, max(0, end - time.time()))) is not None:
            if v == value: return True
        return False

    def write(self, handle, value): return self.req(bytes([0x12]) + struct.pack("<H", handle) + value)
    def read(self, handle): return self.req(bytes([0x0a]) + struct.pack("<H", handle))

    def long_write(self, handle, value, piece=18):
        for off in range(0, len(value), piece):
            r = self.req(bytes([0x16]) + struct.pack("<HH", handle, off) + value[off:off + piece])
            if not r or r[0] != 0x17: return r
        return self.req(bytes([0x18, 1]))


def wait(cond, secs, step=0.05):
    end = time.time() + secs
    while time.time() < end:
        if cond(): return True
        time.sleep(step)
    return cond()


async def ha(fn):
    cli = APIClient("127.0.0.1", PORT, None)
    await asyncio.wait_for(cli.connect(login=True), 5)
    try: return await fn(cli)
    finally: await cli.disconnect()


def adopt_now():
    """Home Assistant takes the Echo on: its voice assistant subscribes"""
    async def adopt(cli):
        cli.subscribe_voice_assistant(handle_start=lambda *a: None, handle_stop=lambda *a: None)
        await asyncio.sleep(0.5)
    asyncio.run(ha(adopt))


class Echo:
    """An Echo with no network from its start, set up (more() below) or not (taken()), with root's wifi.sh behind it.
    ROOT_DELAY: how long root's watcher takes to get to a request (the time a switch is under way); ONLINE_ON_OK: the
    Echo is on a network once wifi.sh said ok, as main()'s"""
    def __init__(self, times, login, adopted=True, root_delay=0, online_on_ok=False):
        self.tmp = tmp = tempfile.mkdtemp()
        self.state, self.out, fake = (os.path.join(tmp, d) for d in ("state", "wifi", "bin"))
        for d in (self.state, self.out, fake): os.makedirs(d)
        for t in ("wpa_cli", "ifconfig", "getprop", "setprop", "dhcpcd", "ping", "stop", "start"):
            os.symlink(f"{ROOT}/tests/fake_wifi_tools.py", os.path.join(fake, t))
        self.world = world = os.path.join(tmp, "world.json"); arp, self.online = os.path.join(tmp, "arp"), os.path.join(tmp, "online")
        air = [{"hex": H("HomeNet"), "psk": PSK("right password", "HomeNet"), "flags": "[WPA2-PSK-CCMP][ESS]", "bss": [["aa:00:00:00:00:01", 2437, -60]], "ip": "192.168.1.20", "gw": "192.168.1.1"},
               {"hex": H("Cafe"), "psk": None, "flags": "[ESS]", "bss": [["aa:00:00:00:00:04", 2462, -80]], "ip": "172.16.0.9", "gw": "172.16.0.1"}]
        json.dump({"networks": [], "next_id": 0, "current": None, "target": None, "air": air, "arp_file": arp, "saved": [],
                   "lease": {"ip": "", "gw": ""}, "props": {"result": "", "reason": ""}, "svc": {}}, open(world, "w"))
        open(arp, "w").write("IP address       HW type     Flags       HW address            Mask     Device\n")
        renv = dict(os.environ, PATH=f"{fake}:{os.environ['PATH']}", FAKE_WIFI_WORLD=world, WIFI_ARP=arp,
                    WIFI_SCAN_SECS="0", WIFI_STEP="0.1", WIFI_JOIN_SECS="3", WIFI_ADDR_SECS="3", WIFI_BACK_SECS="2")
        self.stop_root, self.rootlog, self.root_delay = threading.Event(), [], root_delay
        def root():
            while not self.stop_root.is_set():
                if os.path.exists(os.path.join(self.state, "wifi-request")):
                    time.sleep(self.root_delay)                             # (a test may change it between its requests)
                    r = subprocess.run(["sh", f"{ROOT}/scripts/device/wifi.sh", "take", self.state, self.out], env=renv, capture_output=True, text=True, timeout=60)
                    self.rootlog.append(r.stdout + r.stderr)
                    try:
                        if online_on_ok and open(os.path.join(self.out, "result")).read().split()[1] == "ok": open(self.online, "w").close()
                    except (OSError, IndexError): pass
                time.sleep(0.1)
        threading.Thread(target=root, daemon=True).start()
        if adopted: open(os.path.join(self.state, "adopted"), "w").write("1\n")
        self.ctl = Controller(); self.keys = keys = os.path.join(tmp, "keys"); os.mkfifo(keys)
        self.log = os.path.join(tmp, "log")
        env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_WIFI=self.out, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
                   HASSMIC_PLAY=os.path.join(tmp, "play.raw"), HASSMIC_MDNS_FILE=os.path.join(tmp, "none"), HASSMIC_ARB_ADDR="127.255.255.255",
                   HASSMIC_MODELS=os.path.join(tmp, "models"), HASSMIC_ADB_OPEN=os.path.join(tmp, "adb-open"), HASSMIC_LOG=self.log,
                   HASSMIC_BT_DEV=self.ctl.path, HASSMIC_FAKE_ONLINE=self.online, HASSMIC_IMPROV_TIMES=times, HASSMIC_LOGIN_MS=login)
        self.env = env; self.start()
        self.kfd = os.open(keys, os.O_WRONLY)
        self.hd = self.cccd = None

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Improv", "-L", "-z", "0", "-o", "0",
                                      "-a", str(ARB), "-W", str(WEB), "-b", self.keys], env=self.env, stderr=open(self.log, "a"))
    def restart(self):
        """hassmic ends without a word (a crash) and is started again, as init does, within the boot"""
        self.proc.kill(); self.proc.wait(); time.sleep(0.3); self.start()
        def up():
            try: Browser(WEB); return True
            except Exception: return False
        return wait(up, 8)
    def held_ms(self):
        """for how long state/web_wait keeps strangers back from now, ms (hassmic's clock: CLOCK_MONOTONIC); None: no file"""
        try: return int(open(os.path.join(self.state, "web_wait")).read().split()[1]) - time.monotonic() * 1000
        except (OSError, IndexError, ValueError): return None
    def saved(self): return [bytes.fromhex(n["ssid"]).decode() for n in json.load(open(self.world))["saved"]]

    def press(self):
        for v in (1, 0): os.write(self.kfd, struct.pack("<qqHHi", 0, 0, 1, 138, v))
    def logtext(self): return open(self.log, errors="replace").read()
    def count(self, what): return self.logtext().count(what)
    def adv_state(self):
        try: return prot.ImprovServiceData.from_bytes(self.ctl.adv_fields()[0x16][2:]).state if self.ctl.adv["on"] else None
        except Exception: return None
    def phone(self, addr=bytes.fromhex("665544332211")):
        """a phone connects and subscribes, as the clients do"""
        self.ctl.connect(addr); p = Phone(self.ctl)
        if not self.hd:
            _, chrs, infos = p.discover()
            self.hd = {n: chrs[u][1] for n, u in CHR.items()}
            self.cccd = {n: h + 1 for n, h in self.hd.items() if infos.get(h + 1) == b"\x02\x29"}
        else: p.req(bytes([0x02]) + struct.pack("<H", 247))
        for n in self.cccd: p.write(self.cccd[n], b"\x01\x00")
        return p
    def end(self):
        self.stop_root.set(); self.proc.send_signal(signal.SIGTERM)
        try: self.proc.wait(5)
        except subprocess.TimeoutExpired: self.proc.kill()


class Replaced(threading.Thread):
    """how often a file is replaced (statefile.c: a temp file renamed over it = a new inode each time)"""
    def __init__(self, path): super().__init__(daemon=True); self.path, self.n, self.last = path, 0, None; self.start()
    def run(self):
        while True:
            try: k = os.stat(self.path).st_ino
            except OSError: k = None
            if k and k != self.last: self.n += 1; self.last = k
            time.sleep(0.002)


def more():
    """What a review of 2026-11-20 ran against improv.c and web.c, each of which failed before its fix.  Times: lost 6 s
    (a failed switch's carrier), the button's 4, on a network 3, nothing from a phone for 5: let go; web.c's quiet time 3 s"""
    e = Echo("1 6 4 2 3 5 2", "60000:3000"); ctl = e.ctl
    A = bytes.fromhex("0a0000000001")
    try:
        check(wait(lambda: e.adv_state() == prot.State.AUTHORIZATION_REQUIRED, 12), "a second Echo, set up, no network: advertising, authorization required")
        # ---- an approved browser first (no phone there: the press is its login's), and its adb request beside a
        # phone's wait that comes and goes: that wait stays armed, the press is still its (web.c says why)
        owner = Browser(WEB)
        check(owner.login() == "waiting", "the owner's browser asks to log in: waiting"); e.press()
        check(wait(lambda: owner.ask()["login"] == "approved", 3), "...approved with the button")
        r = json.loads(owner.call("POST", "/api/adb", b"on")[2])
        e.phone(A); time.sleep(0.6); ctl.disconnect(); time.sleep(0.4)
        e.press()
        check(r.get("adb") == "waiting" and wait(lambda: "adb over Wi-Fi approved with the button" in e.logtext(), 3),
              f"an approved browser's adb request, a phone's wait come and gone beside it: still armed, the press opens adb ({r})")
        owner.call("POST", "/api/adb", b"off")
        time.sleep(3.2)                                                     # (the quiet time after that press)

        # ---- a stranger's login armed, then a phone's wait beside it (the same ring) that ends without a press: the
        # login ends with it.  It stayed armed, and the press half a second later, for the phone, let the browser in
        br = Browser(WEB); a0 = br.ask()
        e.phone(A); time.sleep(1.0); ctl.disconnect(); time.sleep(0.4)
        a1 = br.ask(); ba = e.count("button: action"); e.press()
        check(a0["login"] == "waiting" and a1["login"] == "refused" and a1.get("why") == "busy" and "that wait is over: the login is ended" in e.logtext(),
              f"a login armed, a phone's wait beside it come and gone: the login is ended, its browser told busy ({a0} -> {a1})")
        check(wait(lambda: e.count("button: action") > ba, 3) and "login approved (test browser)" not in e.logtext().split("adb over Wi-Fi approved")[1]
              and br.call("GET", "/api/state")[0] == 401, "...and the press after it is the assistant's: that browser is not let in")
        time.sleep(3.2); a2 = br.ask()
        check(a2["login"] == "waiting", f"...its page asks again and is armed once the quiet time is over ({a2})")

        # ---- a central that connects and leaves, over and over (it costs it nothing): each end of its wait moved
        # the time strangers are kept back, and each move was written: a write to the flash per link
        time.sleep(1.2); w = Replaced(os.path.join(e.state, "web_wait")); time.sleep(0.3); n0, t0, cyc = w.n, time.monotonic(), 0
        while time.monotonic() - t0 < 6.5:
            if wait(lambda: ctl.adv["on"], 2, 0.005): ctl.connect(bytes([cyc & 255, 1, 2, 3, 4, 5])); time.sleep(0.12); ctl.disconnect(); cyc += 1; time.sleep(0.05)
        time.sleep(1.3)
        check(cyc >= 15 and e.count("improv: a phone connected") >= cyc and 1 <= w.n - n0 <= 3,
              f"{cyc} links made and ended in 6.5 s by a central nobody pressed for: state/web_wait written {w.n - n0}x (once for the login the first one ended, then one per quiet time, 3 s here)")
        a = Browser(WEB).ask()
        check(a["login"] == "refused" and a.get("why") == "busy", f"...and a stranger's login is kept back after them all the same ({a})")
        left = int(open(os.path.join(e.state, "web_wait")).read().split()[1]) - time.monotonic() * 1000    # (CLOCK_MONOTONIC, as hassmic's)
        check(0 < left <= 6000, f"...and the file holds them back over a restart: for {left / 1000:.1f} s more (the quiet time and as much to spare at the most)")

        # ---- the silence that ends a link counts from what the phone last sent, not from when it connected: Home
        # Assistant connects first and asks for the button only after its menu and its form.  And nothing is held
        # against its address after
        wait(lambda: ctl.adv["on"], 3)
        p = e.phone(A); t0, n0 = time.monotonic(), ctl.dropped
        time.sleep(3.0); st = p.read(e.hd["state"]); t1 = time.monotonic()
        time.sleep(3.6)
        check(st == bytes([0x0b, prot.State.AUTHORIZATION_REQUIRED]) and ctl.up and ctl.dropped == n0,
              f"a phone 6.6 s connected that read the state at 3 s (nothing from it for 5 s: let go): still connected")
        check(wait(lambda: ctl.dropped > n0, 4) and 4.5 <= time.monotonic() - t1 <= 6.6 and "nothing from the phone for 5 s" in e.logtext(),
              f"...let go once nothing has come from it for that long ({time.monotonic() - t1:.1f} s after its read)")
        wait(lambda: ctl.adv["on"], 3); n0 = ctl.dropped
        p = e.phone(A); time.sleep(1.2)
        check(ctl.up and ctl.dropped == n0 and p.read(e.hd["state"]) == bytes([0x0b, prot.State.AUTHORIZATION_REQUIRED]),
              "...and the same phone connecting again at once is served: its address is not turned away")

        # ---- a switch that fails with the carrier up (the network joined, no use: wifi.sh's "no address"): that carrier
        # is not the network's return.  It was: the phone left to ask for the password again, the Echo counted
        # itself on a network a moment ago and stopped advertising for the ten minutes (6 s here), mid-setup
        e.press()
        check(p.until(e.hd["state"], bytes([prot.State.AUTHORIZED])), "the button: authorized")
        p.write(e.hd["rpc"], prot.WiFiSettingsCmd(b"HomeNet", b"wrong password").as_bytes())
        check(p.until(e.hd["state"], bytes([prot.State.PROVISIONING])), "a switch with a wrong password: provisioning")
        open(e.online, "w").close(); off = e.count("advertising stopped")
        check(p.until(e.hd["error"], bytes([prot.Error.UNABLE_TO_CONNECT]), 30), "...a carrier while it runs, then: unable to connect")
        ctl.disconnect(); time.sleep(0.5); os.unlink(e.online)              # (the carrier seen by the look that read the answer, and one after)
        time.sleep(2.5)
        # (waited for: the button's 4 s end about now, and the advertising is off for the moment its state byte changes)
        ads = lambda: ctl.adv["on"] and e.adv_state() in (prot.State.AUTHORIZATION_REQUIRED, prot.State.AUTHORIZED)
        check(wait(ads, 2) and e.count("advertising stopped") == off,
              f"the phone leaves after the failed switch: still advertising for it ({e.adv_state()})")
        p = e.phone(A)
        check(p.read(e.hd["state"]) in (bytes([0x0b, prot.State.AUTHORIZATION_REQUIRED]), bytes([0x0b, prot.State.AUTHORIZED])), "...and the phone that comes back with the password is served")

        # ---- the network back for less than the Echo's second at a time (0.8 s of every 1.95 s; the Echo looks once
        # a second): each look that finds it counts.  Only two looks in a row counted: none here, and the phone,
        # which keeps reading and so is not let go for silence, stayed with the ring's wait and every login's "busy"
        n0, gone, t0 = ctl.dropped, e.count("with nothing to do"), time.monotonic()
        while ctl.dropped == n0 and time.monotonic() - t0 < 24:
            p.read(e.hd["state"]); open(e.online, "w").close(); time.sleep(0.8); os.unlink(e.online); time.sleep(1.15)
        check(ctl.dropped > n0 and e.count("with nothing to do") == gone + 1,
              f"the network back 0.8 s at a time: the phone is let go once the looks that found it make the time for that ({time.monotonic() - t0:.1f} s)")
        if os.path.exists(e.online): os.unlink(e.online)

        # ---- a phone's wait shows, and hassmic ends and starts again within the boot (a crash, an update): whoever
        # the phone's app sent to the Echo is still on his way.  Nothing of a phone's wait was on the flash until it
        # had gone off, so the first stranger to ask after the start was armed, and that press let it in
        check(wait(lambda: e.adv_state() == prot.State.AUTHORIZATION_REQUIRED, 14), "the network gone again: advertising, authorization required")
        p = e.phone(A); time.sleep(1.5); left = e.held_ms(); a = Browser(WEB).ask()
        check(left is not None and 0 < left <= 6000 and a["login"] == "refused",
              f"a phone waits for the button: state/web_wait keeps strangers back over a restart, for {left and round(left / 1000, 1)} s from now (the quiet time and as much to spare at the most)")
        for i in range(4): time.sleep(0.9); p.read(e.hd["state"])           # (it keeps asking: not let go for silence, 5 s here)
        left = e.held_ms()
        check(ctl.up and left is not None and 0 < left <= 6000, f"...and still as the wait goes on, written again once per quiet time ({left and round(left / 1000, 1)} s from now)")
        up = e.restart(); a = Browser(WEB).ask() if up else None
        check(up and a["login"] == "refused" and a.get("why") == "busy" and "something waited for the action button when hassmic last ended" in e.logtext(),
              f"hassmic killed and started again: a stranger's first ask is not armed ({a})")
    finally:
        e.end()
        if check.failed or sys.exc_info()[0]: print("---- hassmic (the second Echo)\n" + e.logtext()[-8000:] + "\n---- root\n" + "".join(e.rootlog)[-3000:])


def taken():
    """Echos that are not set up, and Home Assistant taking them on with a phone still there (a review, 2026-11-20:
    each of these failed before its fix)"""
    A = bytes.fromhex("0a0000000001")
    # ---- (1) the network by other means, a central that stays and scans; taken on with it connected.  Times: lost
    # 1 s, the button's 1 s, "provisioned" 4, on a network 6: let go; a login 6 s and the quiet time 3 (twice, as on an Echo)
    e = Echo("1 1 1 4 6 30 60", "6000:3000", adopted=False); ctl = e.ctl
    try:
        check(wait(lambda: e.adv_state() == prot.State.AUTHORIZED, 10), "a third Echo, not set up, no network: advertising, authorized")
        p = e.phone(A); n0 = ctl.dropped; open(e.online, "w").close(); t0 = time.monotonic(); got = 0; last = 0
        while ctl.dropped == n0 and time.monotonic() - t0 < 14:
            if time.monotonic() - last >= 1:                                # (a write command: no answer waited for, the link may be gone)
                last = time.monotonic(); p.acl(bytes([0x52]) + struct.pack("<H", e.hd["rpc"]) + prot.ScanWifiCmd().as_bytes())
            got += p.note(e.hd["result"], 0.05) is not None
        check(ctl.dropped > n0 and time.monotonic() - t0 <= 9 and got >= 3 and "the phone is still connected with nothing to do" in e.logtext(),
              f"not set up, the network there by other means, a phone asking for a scan every second ({got} answered): let go after the time on a network all the same ({time.monotonic() - t0:.1f} s; 6 s here)")
        if ctl.up: ctl.disconnect()
        os.unlink(e.online)
        check(wait(lambda: e.adv_state() == prot.State.AUTHORIZED, 6), "no network again: advertising again")
        # Home Assistant takes the Echo on with that central connected.  It became "a phone that waits for the button"
        # on an Echo that is online: the ring showed it, the owner's press for his own login went to the central
        p = e.phone(A); n0 = ctl.dropped; open(e.online, "w").close()
        owner = Browser(WEB); a0 = owner.ask(); adopt_now()
        check(a0["login"] == "waiting" and wait(lambda: ctl.dropped > n0, 3) and "Home Assistant took the Echo on with a phone still connected" in e.logtext(),
              "Home Assistant takes the Echo on with a phone still connected (the owner's login armed before): the phone's connection is ended")
        time.sleep(1.2); a1 = owner.ask()
        check(not ctl.adv["on"] and "a phone needs the action button" not in e.logtext() and "that wait is over" not in e.logtext() and a1["login"] == "waiting",
              f"...no wait for a phone shown or ended, nothing advertised (the Echo is on its network), the owner's login still waits ({a1})")
        e.press()
        check(wait(lambda: owner.ask()["login"] == "approved", 3) and "improv: authorized for" not in e.logtext() and "the login (test browser) is not approved" not in e.logtext(),
              "the owner's press: his browser is let in, no phone authorized")
        ctl.connect(A); time.sleep(0.3)
        check(wait(lambda: not ctl.up, 3), "...and a central that connects all the same is not served")

        # ---- (2) set up now.  state/web_wait where a login is twice the quiet time, as on an Echo: a login armed
        # and ended by a phone's wait a second later.  The file said the login's own time, 9 s, and was left at
        # that (rewritten only where "more than twice the quiet time late", which a login of that length never is)
        time.sleep(3.2); os.unlink(e.online)
        check(wait(lambda: e.adv_state() == prot.State.AUTHORIZATION_REQUIRED, 8), "set up, no network for a while: advertising, authorization required")
        br = Browser(WEB); a = br.ask(); f0 = e.held_ms()
        p = e.phone(A); time.sleep(0.5); ctl.disconnect(); time.sleep(0.6); f1 = e.held_ms(); a1 = br.ask()
        check(a["login"] == "waiting" and a1["login"] == "refused" and f0 and f1 and 7000 < f0 <= 9000 and 0 < f1 <= 6000,
              f"a login (6 s, the quiet time 3) ended by a phone's wait a second after it was armed: state/web_wait said {f0 and round(f0 / 1000, 1)} s and now {f1 and round(f1 / 1000, 1)} s from now (the quiet time and as much to spare)")

        # ---- (3) a scan beside a switch.  One press (its time 1 s here), a switch that fails after it, scans asked
        # all the while: none is taken.  The one that came between root's answer and the Echo's look at it was
        check(wait(lambda: ctl.adv["on"], 3), "advertising")
        p = e.phone(A); e.press()
        check(p.until(e.hd["state"], bytes([prot.State.AUTHORIZED])), "the button: authorized")
        # (root 1.5 s late: the switch fails after the button's second, which is what is tested here; wifi.sh alone
        # answered "notfound" in 0.9 s once, inside that second, and the scan after it was rightly answered)
        e.root_delay = 1.5
        asked = len(e.rootlog); p.write(e.hd["rpc"], prot.WiFiSettingsCmd(b"Nowhere", b"some password").as_bytes())
        check(p.until(e.hd["state"], bytes([prot.State.PROVISIONING])), "a switch to a network that is not there: provisioning")
        t0, failed, got, n = time.monotonic(), None, None, 0
        while time.monotonic() - t0 < 25 and not got and (failed is None or time.monotonic() - failed < 2.5):
            p.write(e.hd["rpc"], prot.ScanWifiCmd().as_bytes()); n += 1
            v = p.note(e.hd["result"], 0.15)
            if v: got = prot.parse_result(v)
            if failed is None and "the network switch failed" in e.logtext(): failed = time.monotonic()
        time.sleep(0.5)
        check(failed is not None and failed - t0 > 1.0 and got is None and len(e.rootlog) == asked + 1 and not os.path.exists(os.path.join(e.state, "wifi-request")),
              f"{n} scans asked while the switch ran and after it failed, the button's time over: none answered ({got}), root asked for the switch alone ({len(e.rootlog) - asked})")
    finally:
        e.end()
        if check.failed or sys.exc_info()[0]: print("---- hassmic (the third Echo)\n" + e.logtext()[-8000:] + "\n---- root\n" + "".join(e.rootlog)[-3000:])
    if check.failed: return
    time.sleep(0.5)

    # ---- (4) taken on while the switch that set it up is under way (Home Assistant finds a new Echo within
    # seconds): the phone still hears how it ended and reads the page's address, for the time "provisioned" has;
    # nothing else is the phone's from then.  Times: lost 600 s, "provisioned" 4, on a network 8
    e = Echo("1 600 4 4 8 30 60", "60000:300", adopted=False, root_delay=2.0, online_on_ok=True); ctl = e.ctl
    try:
        check(wait(lambda: e.adv_state() == prot.State.AUTHORIZED, 10), "a fourth Echo, not set up, no network: advertising, authorized")
        p = e.phone(A); n0 = ctl.dropped
        p.write(e.hd["rpc"], prot.WiFiSettingsCmd(b"HomeNet", b"right password").as_bytes())
        check(p.until(e.hd["state"], bytes([prot.State.PROVISIONING])), "the phone sends the network: provisioning")
        adopt_now()
        check(p.until(e.hd["state"], bytes([prot.State.PROVISIONED]), 30), "Home Assistant takes the Echo on while the switch runs: the phone still hears \"provisioned\"")
        v = p.note(e.hd["result"]); res = prot.parse_result(v) if v else None; lg = e.logtext()
        check(isinstance(res, prot.WiFiSettingsRes) and res.redirect_url == f"http://192.168.1.20:{WEB}/".encode() and ctl.up and ctl.dropped == n0
              and 0 < lg.find("took this Echo on") < lg.find("improv: on the network"), f"...and gets the settings page's address ({res})")
        # no wait, no press, no login held back: the Echo is on its network and set up
        owner = Browser(WEB); a = owner.ask(); e.press()
        check(a["login"] == "waiting" and wait(lambda: owner.ask()["login"] == "approved", 3) and "improv: authorized for" not in e.logtext()
              and "a phone needs the action button" not in e.logtext(), f"a browser's login beside it is armed and the press is its ({a}): no phone's wait")
        # the client connects anew between its steps: still served what is left, and nothing more
        ctl.disconnect()
        check(wait(lambda: e.adv_state() == prot.State.PROVISIONED, 3), "the phone leaves: advertising, provisioned")
        p = e.phone(A); n0 = ctl.dropped; asked = len(e.rootlog); r = p.read(e.hd["result"]); res = prot.parse_result(r[1:]) if r and len(r) > 3 else None
        check(p.read(e.hd["state"]) == bytes([0x0b, prot.State.PROVISIONED]) and isinstance(res, prot.WiFiSettingsRes) and res.redirect_url, f"it connects again: provisioned, the address still there to read ({res})")
        p.write(e.hd["rpc"], prot.WiFiSettingsCmd(b"Cafe", b"").as_bytes())
        check(p.until(e.hd["error"], bytes([prot.Error.NOT_AUTHORIZED])), "...its switch to another network: not authorized")
        p.write(e.hd["rpc"], prot.DeviceInfoCmd().as_bytes()); p.until(e.hd["error"], b"\x00")
        p.write(e.hd["rpc"], prot.ScanWifiCmd().as_bytes())
        check(p.until(e.hd["error"], bytes([prot.Error.NOT_AUTHORIZED])) and len(e.rootlog) == asked and e.saved() == ["HomeNet"], "...nor a scan: nothing asked of root, still only HomeNet")
        check(wait(lambda: ctl.dropped > n0, 8) and wait(lambda: not ctl.adv["on"], 3), "\"provisioned\" has had its time: the phone's connection is ended, nothing advertised")
        time.sleep(1.5)
        check(not ctl.adv["on"] and "a phone needs the action button" not in e.logtext() and "that wait is over" not in e.logtext(), "...and it stays so: no wait for a phone was ever shown")
    finally:
        e.end()
        if check.failed or sys.exc_info()[0]: print("---- hassmic (the fourth Echo)\n" + e.logtext()[-8000:] + "\n---- root\n" + "".join(e.rootlog)[-3000:])
    if check.failed: return
    time.sleep(0.5)

    # ---- (5) the switch worked, the phone asks for the list of networks beside "provisioned" (not set up: it may),
    # root is slow with it, and Home Assistant takes the Echo on meanwhile: the scan is nobody's from then and holds
    # nothing.  It held "provisioned", the mode and the link until root answered, and the list was then notified
    # to a central at an Echo that was set up.  Times: "provisioned" 4 s, on a network 40; root's answer to the
    # scan 14 s late (within improv.c's SCAN_SECS, 25)
    e = Echo("1 600 4 4 40 60 60", "60000:300", adopted=False, online_on_ok=True); ctl = e.ctl
    try:
        check(wait(lambda: e.adv_state() == prot.State.AUTHORIZED, 10), "a fifth Echo, not set up, no network: advertising, authorized")
        p = e.phone(A); n0 = ctl.dropped
        p.write(e.hd["rpc"], prot.WiFiSettingsCmd(b"HomeNet", b"right password").as_bytes())
        check(p.until(e.hd["state"], bytes([prot.State.PROVISIONED]), 30), "the phone sends the network: provisioned"); t0 = time.monotonic()
        p.note(e.hd["result"], 2); p.notes.clear()                          # (the switch's own result)
        e.root_delay = 14.0; asked = e.count("improv: no scan")
        p.write(e.hd["rpc"], prot.ScanWifiCmd().as_bytes())
        check(wait(lambda: os.path.exists(os.path.join(e.state, "wifi-request")), 3) and e.count("improv: no scan") == asked and ctl.up,
              "it asks for the list of networks beside \"provisioned\": root is asked (and takes its time)")
        adopt_now(); got = None
        while ctl.dropped == n0 and time.monotonic() - t0 < 22:
            got = got or p.note(e.hd["result"], 0.2)
        dt = time.monotonic() - t0
        check(ctl.dropped > n0 and dt <= 10.5 and "on the network for 4 s and the phone is still connected" in e.logtext(),
              f"Home Assistant takes the Echo on with that scan out: \"provisioned\" has its 4 s and no more, the phone's connection is ended ({dt:.1f} s after it; root's answer comes at 14 s or later)")
        check(wait(lambda: not ctl.adv["on"], 3) and "a phone needs the action button" not in e.logtext(), "...nothing advertised after it: the scan holds no mode either")
        check(got is None, f"...and the list of networks was not sent to it while it was connected ({got})")
    finally:
        e.end()
        if check.failed or sys.exc_info()[0]: print("---- hassmic (the fifth Echo)\n" + e.logtext()[-8000:] + "\n---- root\n" + "".join(e.rootlog)[-3000:])


def main():
    tmp = tempfile.mkdtemp(); state, out, fake = (os.path.join(tmp, d) for d in ("state", "wifi", "bin"))
    for d in (state, out, fake): os.makedirs(d)
    for t in ("wpa_cli", "ifconfig", "getprop", "setprop", "dhcpcd", "ping", "stop", "start"):
        os.symlink(f"{ROOT}/tests/fake_wifi_tools.py", os.path.join(fake, t))
    world, arp, online = os.path.join(tmp, "world.json"), os.path.join(tmp, "arp"), os.path.join(tmp, "online")
    air = [
        {"hex": H("HomeNet"), "psk": PSK("right password", "HomeNet"), "flags": "[WPA2-PSK-CCMP][ESS]", "bss": [["aa:00:00:00:00:01", 2437, -60]], "ip": "192.168.1.20", "gw": "192.168.1.1"},
        {"hex": H("Cafe"), "psk": None, "flags": "[ESS]", "bss": [["aa:00:00:00:00:04", 2462, -80]], "ip": "172.16.0.9", "gw": "172.16.0.1"},
        {"hex": H("Only3"), "psk": PSK("whatever!", "Only3"), "flags": "[RSN-SAE-CCMP][ESS]", "bss": [["aa:00:00:00:00:05", 5200, -50]], "ip": "10.9.9.9", "gw": "10.9.9.1"},
        {"hex": "", "psk": None, "flags": "[ESS]", "bss": [["aa:00:00:00:00:06", 2412, -40]], "ip": "10.8.8.8", "gw": "10.8.8.1"},
    ]
    with open(world, "w") as f:        # a new Echo, or one after a reset: no network saved, on none, no dhcpcd started yet
        json.dump({"networks": [], "next_id": 0, "current": None, "target": None, "air": air, "arp_file": arp, "saved": [],
                   "lease": {"ip": "", "gw": ""}, "props": {"result": "", "reason": ""}, "svc": {}}, f)
    open(arp, "w").write("IP address       HW type     Flags       HW address            Mask     Device\n")
    W = lambda: json.load(open(world))
    renv = dict(os.environ, PATH=f"{fake}:{os.environ['PATH']}", FAKE_WIFI_WORLD=world, WIFI_ARP=arp,
                WIFI_SCAN_SECS="0", WIFI_STEP="0.1", WIFI_JOIN_SECS="6", WIFI_ADDR_SECS="5", WIFI_BACK_SECS="6")     # looks: the stand-ins answer at the third
    stop_root, rootlog = threading.Event(), []
    def root():                        # main.sh's watcher: hands each request to wifi.sh; online once it says ok
        while not stop_root.is_set():
            if os.path.exists(os.path.join(state, "wifi-request")):
                r = subprocess.run(["sh", f"{ROOT}/scripts/device/wifi.sh", "take", state, out], env=renv, capture_output=True, text=True, timeout=60)
                rootlog.append(r.stdout + r.stderr)
                try:
                    if open(os.path.join(out, "result")).read().split()[1] == "ok": open(online, "w").close()
                except (OSError, IndexError): pass
            time.sleep(0.1)
    threading.Thread(target=root, daemon=True).start()

    visible_name = 'Echo Séjour'
    with open(os.path.join(state, "display_name"), "w") as f: f.write(visible_name + "\n")
    ctl = Controller(); keys = os.path.join(tmp, "keys"); os.mkfifo(keys)
    log = os.path.join(tmp, "log")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_WIFI=out, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
               HASSMIC_PLAY=os.path.join(tmp, "play.raw"), HASSMIC_MDNS_FILE=os.path.join(tmp, "none"), HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_MODELS=os.path.join(tmp, "models"), HASSMIC_ADB_OPEN=os.path.join(tmp, "adb-open"), HASSMIC_LOG=log,
               HASSMIC_BT_DEV=ctl.path, HASSMIC_FAKE_ONLINE=online, HASSMIC_IMPROV_TIMES="1 1 4 3 3 18 2",       # (not set up, lost, the button's, provisioned; on a network 3 s: let go; not authorized and nothing from it for 18 s: let go; identify without the button once in 2 s)
               HASSMIC_LOGIN_MS="60000:300",                # (a login asked for right after a press here: web.c's quiet time 0.3 s)
               HASSMIC_HOLD_TIMES="1200 2400")       # the action button's hold stages: warned, reset (5 s, 10 s on the Echo)
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Improv", "-L", "-z", "0", "-o", "0",
                             "-a", str(ARB), "-W", str(WEB), "-b", keys], env=env, stderr=open(log, "w"))
    kfd = os.open(keys, os.O_WRONLY)   # the keypad: input_event structs (x86-64: timeval, type, code, value)
    key = lambda code, v: os.write(kfd, struct.pack("<qqHHi", 0, 0, 1, code, v))
    logtext = lambda: open(log, errors="replace").read()
    phone = Phone(ctl)
    count = lambda what: logtext().count(what)
    adv_state = lambda: prot.ImprovServiceData.from_bytes(ctl.adv_fields()[0x16][2:]).state if ctl.adv["on"] else None
    try:
        # ---- a new Echo with no network: it advertises, authorized
        check(wait(lambda: ctl.adv["on"], 8), "no network, not set up: advertising")
        f = ctl.adv_fields()
        check(f.get(0x07) == SVC, "the advertising names the Improv service")
        sd = f.get(0x16, b"")
        info = prot.ImprovServiceData.from_bytes(sd[2:]) if sd[:2] == b"\x77\x46" else None
        check(info is not None and info.state == prot.State.AUTHORIZED and info.capabilities == prot.Capabilities.IDENTIFY | prot.Capabilities.DEVICE_INFO | prot.Capabilities.SCAN_WIFI,
              "service data 4677: authorized; identify, device info, scan (no host name: the Echo names itself)")
        rsp = ctl.adv["rsp"]
        check(rsp[1] == 0x09 and rsp[2:2 + rsp[0] - 1] == visible_name.encode(), "scan response: saved visible name")
        check(ctl.local_name == visible_name.encode() and ctl.inquiry_name == visible_name.encode(),
              "Bluetooth audio: saved visible name in local name and pairing discovery")
        check("setup: not set up yet" in logtext(), "the log says the setup waits (the setup spinner on the Echo)")

        # ---- the phone connects and finds its way around
        ctl.connect()
        svcs, chrs, infos = phone.discover()
        check(any(u == b"\x00\x18" for _, _, u in svcs) and any(u == SVC for _, _, u in svcs), "GATT: Generic Access and the Improv service")
        check(set(chrs) == set(CHR.values()), "the five Improv characteristics")
        hd = {n: chrs[u][1] for n, u in CHR.items()}
        check(chrs[CHR["rpc"]][0] & 0x08 and chrs[CHR["state"]][0] & 0x12 == 0x12, "RPC command writable, state read + notify")
        cccd = {n: h + 1 for n, h in hd.items() if infos.get(h + 1) == b"\x02\x29"}
        check(set(cccd) == {"state", "error", "result"}, "notification descriptors for state, error, result")
        check(phone.read(hd["caps"]) == b"\x0b\x07" and phone.read(hd["state"]) == b"\x0b\x02", "reads: capabilities 0x07, state authorized")
        r = phone.req(bytes([0x08]) + struct.pack("<HHH", 1, 0xffff, 0x2a00))
        check(r is not None and r[0] == 0x09 and r[4:] == visible_name.encode(), "device name by type (Generic Access)")
        check(phone.write(hd["rpc"] + 5, b"\x00") is not None, "a write past the table is answered")
        for n in cccd: check(phone.write(cccd[n], b"\x01\x00") == b"\x13", f"notifications on: {n}")

        # ---- not set up, and the Echo gets onto a network by other means while the phone stays connected: it is let
        # go after the time for that, as on an Echo that is set up (it was served for as long as it stayed)
        n0 = ctl.dropped; open(online, "w").close(); t0 = time.monotonic()
        check(wait(lambda: ctl.dropped > n0, 8) and "the phone is still connected with nothing to do" in logtext() and wait(lambda: not ctl.adv["on"], 3),
              f"not set up, on a network, the phone still connected: its connection is ended, advertising stopped ({time.monotonic() - t0:.1f} s)")
        if ctl.up: ctl.disconnect()                                         # (a failure: the checks below go on from where they expect to be)
        os.unlink(online)
        check(wait(lambda: ctl.adv["on"], 5), "no network again: advertising again")
        ctl.connect(); phone = Phone(ctl); phone.discover()
        for n in cccd: phone.write(cccd[n], b"\x01\x00")

        # ---- device info, identify
        phone.write(hd["rpc"], prot.DeviceInfoCmd().as_bytes())
        v = phone.note(hd["result"])
        res = prot.parse_result(v) if v else None
        check(isinstance(res, prot.DeviceInfoRes) and res.firmware_name == b"hassmic" and res.device_name == visible_name.encode(),
              f"device info: {res}")
        phone.write(hd["rpc"], prot.IdentifyCmd().as_bytes())
        check(wait(lambda: "identify: rainbow" in logtext(), 3), "identify: the rainbow")

        # ---- scan
        phone.write(hd["rpc"], prot.ScanWifiCmd().as_bytes())
        v = phone.note(hd["result"], 15)
        res = prot.parse_result(v) if v else None
        nets = [(s.decode(), r.decode(), a.decode()) for s, r, a in res.networks] if isinstance(res, prot.ScanWifiRes) else None
        check(nets == [("HomeNet", "-60", "WPA2"), ("Cafe", "-80", "NO")], f"scan: what the Echo can join, strongest first: {nets}")

        # ---- writes in pieces: a long (prepared) write, a packet in two writes.  No names: the Echo names itself
        phone.long_write(hd["rpc"], prot.DeviceInfoCmd().as_bytes(), 2)
        v = phone.note(hd["result"]); res = prot.parse_result(v) if v else None
        check(isinstance(res, prot.DeviceInfoRes), f"device info by long write: {res}")
        pkt = prot.DeviceInfoCmd().as_bytes()
        phone.write(hd["rpc"], pkt[:1]); phone.write(hd["rpc"], pkt[1:])
        v = phone.note(hd["result"]); res = prot.parse_result(v) if v else None
        check(isinstance(res, prot.DeviceInfoRes), "device info in two writes")
        phone.write(hd["rpc"], prot.HostnameCmd(b"kitchen-echo").as_bytes())
        check(phone.until(hd["error"], bytes([prot.Error.UNKNOWN_RPC_COMMAND])), "host name: not offered (unknown command)")
        phone.write(hd["rpc"], b"\x02\x00\x99")
        check(phone.until(hd["error"], bytes([prot.Error.INVALID_RPC_PACKET])), "wrong checksum: invalid packet")
        phone.write(hd["rpc"], prot.UnknownCommand(0x7e, []).as_bytes())
        check(phone.until(hd["error"], bytes([prot.Error.UNKNOWN_RPC_COMMAND])), "unknown command")

        # ---- a wrong password, then the right one
        phone.write(hd["rpc"], prot.WiFiSettingsCmd(b"HomeNet", b"wrong password").as_bytes())
        check(phone.until(hd["error"], b"\x00") and phone.until(hd["state"], bytes([prot.State.PROVISIONING])),
              "provisioning, the unknown command's error cleared")
        check(phone.until(hd["error"], bytes([prot.Error.UNABLE_TO_CONNECT]), 30) and phone.until(hd["state"], bytes([prot.State.AUTHORIZED])),
              "wrong password: unable to connect, authorized again")
        check(W()["saved"] == [], "nothing saved")
        # Home Assistant's app reads the error as it connects: a phone coming back must not see the last one's failure
        ctl.disconnect(); ctl.connect(); phone = Phone(ctl); phone.discover()
        for n in cccd: phone.write(cccd[n], b"\x01\x00")
        check(phone.read(hd["error"]) == b"\x0b\x00" and phone.read(hd["state"]) == b"\x0b\x02", "connected again: no error, authorized")
        phone.write(hd["rpc"], prot.WiFiSettingsCmd(b"HomeNet ", b"right password").as_bytes())   # a phone keyboard's space
        check(phone.until(hd["state"], bytes([prot.State.PROVISIONING])), "again: provisioning")
        check(phone.until(hd["state"], bytes([prot.State.PROVISIONED]), 30), "on the network: provisioned")
        v = phone.note(hd["result"]); res = prot.parse_result(v) if v else None
        check(isinstance(res, prot.WiFiSettingsRes) and res.redirect_url == f"http://192.168.1.20:{WEB}/".encode(), f"the settings page's address back: {res}")
        sv = W()["saved"]
        check(len(sv) == 1 and sv[0]["ssid"] == H("HomeNet") and sv[0]["psk"] == PSK("right password", "HomeNet"), "saved: HomeNet with the PSK")
        check("right password" not in open(world).read() and "right password" not in logtext(), "the password itself is nowhere")
        check("taken without" in logtext(), "the name's trailing space dropped (no network of that name in the scan)")

        # ---- the phone leaves: advertising "provisioned" a while, then not; no restart, no name anywhere
        ctl.disconnect()
        check(wait(lambda: ctl.adv["on"], 3) and prot.ImprovServiceData.from_bytes(ctl.adv_fields()[0x16][2:]).state == prot.State.PROVISIONED,
              "advertising again: provisioned")
        check(wait(lambda: not ctl.adv["on"], 6), "on the network: advertising stops")
        check(not any(os.path.exists(os.path.join(state, f)) for f in ("restart", "name", "node")), "nothing to restart, no name written")
        check(not os.path.exists(os.path.join(state, "adopted")), "still in setup: Home Assistant has not taken it on")

        # ---- Home Assistant takes it on: the setup is over
        async def adopt(cli):
            cli.subscribe_voice_assistant(handle_start=lambda *a: None, handle_stop=lambda *a: None)
            await asyncio.sleep(0.5)
        asyncio.run(ha(adopt))
        check(wait(lambda: os.path.exists(os.path.join(state, "adopted")), 3) and "setup done" in logtext(), "voice assistant subscribed: adopted")

        # ---- set up, network gone: advertising wants the button.  Rebooted meanwhile: init's dhcpcd (-K) started with no
        # carrier, which "dhcpcd -n" would wait for for good (cannoli, 2026-11-15)
        os.unlink(online)
        w = W(); w["carrier"] = False; json.dump(w, open(world, "w"))
        check(wait(lambda: ctl.adv["on"], 8), "set up, no network for a while: advertising")
        check(prot.ImprovServiceData.from_bytes(ctl.adv_fields()[0x16][2:]).state == prot.State.AUTHORIZATION_REQUIRED, "authorization required")
        # a browser's login armed before any phone is connected (here over loopback; on an Echo, the network just back),
        # then the phone's wait begins: both show the same ring, and the press is the phone's, never that browser's
        # (web.c web_approve)
        early = Browser(WEB)
        check(early.login() == "waiting" and logtext().count("login waits") == 1, "a login asked for before the phone connects: armed")
        ctl.connect(); phone = Phone(ctl); phone.discover()
        for n in cccd: phone.write(cccd[n], b"\x01\x00")
        phone.write(hd["rpc"], prot.WiFiSettingsCmd(b"Cafe", b"").as_bytes())
        check(phone.until(hd["error"], bytes([prot.Error.NOT_AUTHORIZED])), "Wi-Fi without the button: not authorized")
        key(138, 1); key(138, 0)
        check(phone.until(hd["state"], bytes([prot.State.AUTHORIZED])), "action button, with that login armed and the phone waiting: the phone is authorized")
        a = early.ask()
        check("login approved" not in logtext() and early.call("GET", "/api/state")[0] == 401 and a["login"] == "refused" and a.get("why") == "busy"
              and "the login (test browser) is not approved by it" in logtext(), f"...and the press let no browser in: that login ended, its browser is told ({a})")
        # the phone's time runs out unused (4 s here): it wants the button again
        check(phone.until(hd["state"], bytes([prot.State.AUTHORIZATION_REQUIRED]), 8), "the authorization runs out: the phone waits for the button again")
        # the button is the phone's now: a browser nobody approved is not armed beside it, so the press cannot let it
        # in (web.c "what waits for the action button")
        stranger = Browser(WEB); a = stranger.ask()
        check(a["login"] == "refused" and a.get("why") == "busy" and logtext().count("login waits") == 1, f"a login asked while the phone waits for the button: busy, not armed ({a})")
        key(138, 1); key(138, 0)                                            # KEY_HELP: the action button
        check(phone.until(hd["state"], bytes([prot.State.AUTHORIZED])), "action button: authorized")
        check("login approved" not in logtext() and stranger.call("GET", "/api/state")[0] == 401, "...and the press approved no browser")
        # The phone is authorized now and its wait is over; the ring would show a login's wait the same.  A stranger's
        # ask is still not armed (it was, half a second after the press), so the second press of somebody not sure
        # the first one took lets nobody in; it is the assistant's
        time.sleep(0.5); a = stranger.ask(); ba = count("button: action")
        check(a["login"] == "refused" and a.get("why") == "busy" and count("login waits") == 1, f"a login asked right after the press for the phone: busy, not armed ({a})")
        key(138, 1); key(138, 0)
        check(wait(lambda: count("button: action") > ba, 3) and "login approved" not in logtext() and stranger.call("GET", "/api/state")[0] == 401 and count("improv: authorized for") == 2,
              "a second press: nobody's wait, the assistant's; no browser approved")
        phone.write(hd["rpc"], prot.WiFiSettingsCmd(b"Cafe", b"").as_bytes())
        check(phone.until(hd["state"], bytes([prot.State.PROVISIONING])) and phone.until(hd["state"], bytes([prot.State.PROVISIONED]), 30),
              "then the switch to an open network works")
        check(len(W()["saved"]) == 1 and W()["saved"][0]["ssid"] == H("Cafe"), "saved: only Cafe")
        a = stranger.ask()
        check(a["login"] == "refused" and a.get("why") == "busy" and count("login waits") == 1, f"a login asked while the phone is shown \"provisioned\": busy, not armed ({a})")
        # one press, one switch: "provisioned" is no authorization (it was, with no end while the phone stayed)
        asked = len(rootlog); phone.notes.clear()
        phone.write(hd["rpc"], prot.WiFiSettingsCmd(b"HomeNet", b"right password").as_bytes())
        check(phone.until(hd["error"], bytes([prot.Error.NOT_AUTHORIZED])), "a second switch after the one the press was for: not authorized")
        phone.write(hd["rpc"], prot.DeviceInfoCmd().as_bytes()); phone.until(hd["error"], b"\x00")
        phone.write(hd["rpc"], prot.ScanWifiCmd().as_bytes())
        check(phone.until(hd["error"], bytes([prot.Error.NOT_AUTHORIZED])), "...nor a scan")
        time.sleep(0.5)
        check(len(rootlog) == asked and [n["ssid"] for n in W()["saved"]] == [H("Cafe")] and phone.read(hd["state"]) == bytes([0x0b, prot.State.PROVISIONED]),
              "...nothing asked of root, still only Cafe, still shown as provisioned")
        # "provisioned" has its time whoever is connected.  The new network goes away again and the phone stays: it
        # was "provisioned" for good then, no press taken (nothing waited), every switch refused
        n0 = ctl.dropped; os.unlink(online)
        check(wait(lambda: ctl.dropped > n0, 8) and "on the network for 3 s and the phone is still connected" in logtext(),
              "the network gone again and the phone still connected: \"provisioned\" ends after its time, the phone's connection with it")
        if ctl.up: ctl.disconnect()
        check(wait(lambda: adv_state() == prot.State.AUTHORIZATION_REQUIRED, 6), "...advertising: authorization required")
        ctl.connect(); phone = Phone(ctl)
        for n in cccd: phone.write(cccd[n], b"\x01\x00")
        check(phone.read(hd["state"]) == bytes([0x0b, prot.State.AUTHORIZATION_REQUIRED]), "...a phone that connects is asked for the button again")
        key(138, 1); key(138, 0)
        # (the log line waited for: hassmic's stderr reaches the file through its log reader, and the phone's
        # notification came first on a loaded runner; counted exact once there)
        check(phone.until(hd["state"], bytes([prot.State.AUTHORIZED])) and wait(lambda: count("improv: authorized for") >= 3, 3)
              and count("improv: authorized for") == 3, "...and the button works for it: authorized")
        open(online, "w").close(); ctl.disconnect()

        # ---- the network comes back by itself and a phone just stays connected: its wait for the button is not
        # for good (it kept every browser that is not approved "busy", and the ring on, for as long as it stayed)
        check(wait(lambda: not ctl.adv["on"], 8), "on Cafe: advertising stops")
        os.unlink(online)
        check(wait(lambda: ctl.adv["on"] and prot.ImprovServiceData.from_bytes(ctl.adv_fields()[0x16][2:]).state == prot.State.AUTHORIZATION_REQUIRED, 12), "the network gone again: advertising, authorization required")
        # The phone is not authorized and asks for a scan once a second all the while: on an Echo that is set up a
        # scan needs the button as the switch does, nothing is asked of root for it, and it does not begin the
        # time anew (it did all three: such a phone was never let go, and each scan was a write and a scan by root)
        ctl.connect(); phone = Phone(ctl); phone.discover()
        for n in cccd: phone.write(cccd[n], b"\x01\x00")
        time.sleep(0.5)
        lingerer = Browser(WEB); a = lingerer.ask()
        check(a["login"] == "refused" and a.get("why") == "busy", f"a phone connects and is not authorized: a browser's login is not armed beside its wait ({a})")
        scans = len(rootlog)
        phone.notes.clear(); phone.write(hd["rpc"], prot.ScanWifiCmd().as_bytes())
        check(phone.until(hd["error"], bytes([prot.Error.NOT_AUTHORIZED])), "a scan without the button on an Echo that is set up: not authorized")
        open(online, "w").close(); t0 = time.monotonic(); n0 = ctl.dropped; last = 0
        while ctl.dropped == n0 and time.monotonic() - t0 < 10:
            if time.monotonic() - last >= 1:
                last = time.monotonic(); phone.acl(bytes([0x52]) + struct.pack("<H", hd["rpc"]) + prot.ScanWifiCmd().as_bytes())     # (a write command: no answer waited for)
            time.sleep(0.05)
        check(ctl.dropped > n0 and 2 <= time.monotonic() - t0 <= 7 and "the phone is still connected with nothing to do" in logtext(),
              f"the network back and the phone still connected, asking for scans: its connection is ended after the time for that ({time.monotonic() - t0:.1f} s)")
        check(len(rootlog) == scans and not os.path.exists(os.path.join(state, "wifi-request")), "...and none of those scans was asked of root")
        check(wait(lambda: lingerer.ask()["login"] == "waiting", 4), "...and the browser's login is taken again")
        check(wait(lambda: not ctl.adv["on"], 6), "...advertising stopped: the Echo is on its network")

        # ---- nothing advertised (set up, on its network): a link that comes all the same is not served.  A
        # controller that had not yet heard "off", or the advertising ble.c switches on as a link ends: the Echo was
        # in no mode then and its state said "authorized", so the switch was taken with no button
        asked, n0 = len(rootlog), ctl.dropped
        off = lambda what: check(len(rootlog) == asked and not os.path.exists(os.path.join(state, "wifi-request")) and [n["ssid"] for n in W()["saved"]] == [H("Cafe")], what)
        def ask_all(ph):                                                    # (write commands: no answer waited for, the link may be gone)
            ph.acl(bytes([0x52]) + struct.pack("<H", hd["rpc"]) + prot.WiFiSettingsCmd(b"HomeNet", b"right password").as_bytes()); time.sleep(0.15)
            ph.acl(bytes([0x52]) + struct.pack("<H", hd["rpc"]) + prot.ScanWifiCmd().as_bytes()); time.sleep(0.15)
        ctl.connect(); phone = Phone(ctl); ask_all(phone)
        check(wait(lambda: ctl.dropped > n0, 3) and "while nothing is advertised: its connection is ended" in logtext(), "a phone connects while nothing is advertised: its connection is ended")
        time.sleep(1.5)
        off("...and its switch and its scan were not taken: nothing asked of root, still only Cafe")
        check(not ctl.adv["on"] and not ctl.up, "...and that link's end switched no advertising on")

        # ---- which press is the phone's: only one made while a phone is connected and waits (the ring shows it).
        # With nobody connected it is the music's or the assistant's, as on any day (it was taken: an Echo ten minutes
        # offline swallowed every press, opened the button's time for whoever came next and advertised "authorized")
        os.unlink(online)
        check(wait(lambda: adv_state() == prot.State.AUTHORIZATION_REQUIRED, 12), "the network gone again: advertising, authorization required")
        # (the browser's login from above is still armed and no phone waits: the first press is that login's)
        key(138, 1); key(138, 0)
        check(wait(lambda: "login approved" in logtext(), 3) and "authorized for" not in logtext().split("login approved")[1], "a press while a login waits and no phone does: the login's, not the phone's setup's")
        time.sleep(0.5); ba, au = count("button: action"), count("improv: authorized for"); key(138, 1); key(138, 0)
        check(wait(lambda: count("button: action") > ba, 3), "the action button with no phone connected: the assistant's, Home Assistant's event and all")
        time.sleep(1.2)
        check(count("improv: authorized for") == au and adv_state() == prot.State.AUTHORIZATION_REQUIRED, "...nothing authorized by it, and the advertising does not say so")
        ctl.connect(); phone = Phone(ctl)
        for n in cccd: phone.write(cccd[n], b"\x01\x00")
        check(phone.read(hd["state"]) == bytes([0x0b, prot.State.AUTHORIZATION_REQUIRED]), "a phone that connects after that press: authorization required")
        phone.write(hd["rpc"], prot.WiFiSettingsCmd(b"HomeNet", b"right password").as_bytes())
        check(phone.until(hd["error"], bytes([prot.Error.NOT_AUTHORIZED])), "...and its switch: not authorized")
        # the time a press opens is the Echo's: the phone that was there, or one that connects within it, none after
        key(138, 1); key(138, 0)
        check(phone.until(hd["state"], bytes([prot.State.AUTHORIZED])) and wait(lambda: count("improv: authorized for") >= au + 1, 3)
              and count("improv: authorized for") == au + 1, "the action button with the phone connected and waiting: authorized")
        ctl.disconnect()
        check(wait(lambda: adv_state() == prot.State.AUTHORIZED, 2), "the phone leaves within the button's time: advertised as authorized")
        ctl.connect(); phone = Phone(ctl)
        check(phone.read(hd["state"]) == bytes([0x0b, prot.State.AUTHORIZED]), "a phone that connects within the button's time: authorized")
        ctl.disconnect(); time.sleep(4.5)                                   # (4 s here)
        ctl.connect(); phone = Phone(ctl); phone.write(cccd["error"], b"\x01\x00")
        check(phone.read(hd["state"]) == bytes([0x0b, prot.State.AUTHORIZATION_REQUIRED]), "one that connects after it: authorization required")
        phone.write(hd["rpc"], prot.WiFiSettingsCmd(b"HomeNet", b"right password").as_bytes())
        check(phone.until(hd["error"], bytes([prot.Error.NOT_AUTHORIZED])), "...and its switch: not authorized")
        ctl.disconnect()

        # ---- how long a phone that is not authorized is kept.  (a) The network back in pieces, never the whole time
        # in a row (1.9 s of 3 s here): the time on a network counts together.  It counted from the last return, so
        # a network that went for a moment now and then kept the phone, the ring's wait and every login's "busy"
        A, B = bytes.fromhex("0a0000000001"), bytes.fromhex("0b0000000002")
        check(wait(lambda: adv_state() == prot.State.AUTHORIZATION_REQUIRED, 5), "advertising, authorization required")
        ctl.connect(A); phone = Phone(ctl)
        for n in cccd: phone.write(cccd[n], b"\x01\x00")
        n0, gone, t0 = ctl.dropped, count("with nothing to do"), time.monotonic()
        while ctl.dropped == n0 and time.monotonic() - t0 < 16:
            open(online, "w").close()
            if wait(lambda: ctl.dropped > n0, 1.9): break
            os.unlink(online); time.sleep(1.2)                              # (longer than the Echo's look: it sees the network gone)
        check(ctl.dropped > n0 and wait(lambda: count("with nothing to do") == gone + 1, 5) and "nothing from the phone for" not in logtext(),
              f"the network back for 1.9 s at a time: the phone is let go once that makes the time for it ({time.monotonic() - t0:.1f} s)")
        if ctl.up: ctl.disconnect()
        if os.path.exists(online): os.unlink(online)
        # (b) No network at all, and a phone that stays and sends nothing more (left on a table): the link is ended
        # after the time for that, counted from the last thing it sent (advertising is off while it is up: nobody
        # else finds the Echo, the ring shows its wait).  One that keeps asking is kept: more() below, and improv.c's
        # header says that nothing bounds it
        check(wait(lambda: adv_state() == prot.State.AUTHORIZATION_REQUIRED, 8), "the network gone: advertising, authorization required")
        ctl.connect(A); phone = Phone(ctl); n0, t0 = ctl.dropped, time.monotonic()
        for n in cccd: phone.write(cccd[n], b"\x01\x00")
        k0 = count("identify: rainbow")
        for i in range(4): phone.write(hd["rpc"], prot.IdentifyCmd().as_bytes()); time.sleep(0.2)
        time.sleep(0.5)
        check(count("identify: rainbow") - k0 == 1, f"identify from a phone that is not authorized, 4 asked within a second: one sound and rainbow ({count('identify: rainbow') - k0})")
        # (the line reaches the log's file through hassmic's log reader, a process of its own: under load after the
        # controller saw the link end.  Read at once, it was not there yet, and the run failed at 18.6 s: a flake that a
        # review of a30d6ed found under tests/run.py -j)
        dropped = wait(lambda: ctl.dropped > n0, 24); dt = time.monotonic() - t0
        said = wait(lambda: "nothing from the phone for 18 s" in logtext(), 5)
        check(dropped and 16.5 <= dt <= 23 and said, f"no network, a phone that is not authorized and silent: its connection is ended after the time for that ({dt:.1f} s)")
        if ctl.up: ctl.disconnect()
        check(wait(lambda: adv_state() == prot.State.AUTHORIZATION_REQUIRED, 3), "...and the advertising goes on: another phone finds the Echo")
        # the same address again at once: served.  It was turned away for a minute, and Home Assistant's adapter or
        # proxy has one address: the owner's own flow ended in "cannot connect"
        n1 = ctl.dropped; ctl.connect(A); phone = Phone(ctl); phone.req(bytes([0x02]) + struct.pack("<H", 247))     # (the MTU: the device info is one notification)
        for n in cccd: phone.write(cccd[n], b"\x01\x00")
        phone.write(hd["rpc"], prot.DeviceInfoCmd().as_bytes())
        v = phone.note(hd["result"]); res = prot.parse_result(v) if v else None
        time.sleep(1.2)
        check(isinstance(res, prot.DeviceInfoRes) and ctl.up and ctl.dropped == n1, "the same address again at once: served")
        key(138, 1); key(138, 0)
        check(phone.until(hd["state"], bytes([prot.State.AUTHORIZED])), "...and the button authorizes it")
        ctl.disconnect(); wait(lambda: ctl.adv["on"], 2); ctl.connect(B); phone = Phone(ctl); time.sleep(1.2)
        check(ctl.up and phone.read(hd["state"]) == bytes([0x0b, prot.State.AUTHORIZED]), "within the button's time any phone is served, another one too")
        ctl.disconnect(); open(online, "w").close()
        check(wait(lambda: not ctl.adv["on"], 6), "on the network: advertising stops")

        # ---- the network comes back and the phone connects again on the first advertising after each end of its
        # link.  (a) it ends the link itself: the advertising its end switched on, before the worker took the data
        # away, let it back in with the Echo in no mode: authorized, never let go (it joined and saved a network
        # with no press).  (b) it waits to be let go and comes back: the time was counted per link, anew each time.
        # Either way now: one time in all per return of the network, then no link, no advertising, nothing taken
        late = Browser(WEB)
        for how in ("ends its link itself", "waits to be let go"):
            if not ctl.up:
                os.unlink(online)
                check(wait(lambda: ctl.adv["on"] and prot.ImprovServiceData.from_bytes(ctl.adv_fields()[0x16][2:]).state == prot.State.AUTHORIZATION_REQUIRED, 12), "the network gone again: advertising, authorization required")
                ctl.connect(); phone = Phone(ctl)
            asked, n0, ctl.reconnects = len(rootlog), ctl.dropped, 0
            ctl.pending = True; open(online, "w").close(); t0 = time.monotonic()
            if how == "ends its link itself": time.sleep(0.3); ctl.disconnect()
            took = None
            while time.monotonic() - t0 < 9:                                # (3 s here; a second time of its own would end at 6 s + a look)
                if ctl.up: ask_all(Phone(ctl))
                elif not ctl.adv["on"] and took is None and time.monotonic() - t0 > 0.5: took = time.monotonic() - t0
                if ctl.up: took = None
                time.sleep(0.05)
            ctl.pending = False
            check(not ctl.up and not ctl.adv["on"] and took is not None and took <= 5.5,
                  f"the network back, a phone that {how} and connects again at once: no link and no advertising after the one time for it ({took and round(took, 1)} s, back {ctl.reconnects}x, let go {ctl.dropped - n0}x)")
            off("...and no switch or scan of its taken on the way: nothing asked of root, still only Cafe")
            check(wait(lambda: late.ask()["login"] == "waiting", 3), f"...and a browser's login is not held up by it ({late.ask()})")

        # ---- factory reset: Home Assistant's button, the action button held
        async def press(cli):
            ents, _ = await cli.list_entities_services()
            b = next(e for e in ents if e.object_id == "factory_reset")
            cli.button_command(b.key); await asyncio.sleep(0.5)
            return b
        b = asyncio.run(ha(press))
        check(b.entity_category == 1 and b.device_class == "restart", "Factory reset: a configuration button")
        rf = os.path.join(state, "reset")
        check(wait(lambda: os.path.exists(rf), 3) and open(rf).read() == "Home Assistant's button\n", "pressed: state/reset for root")
        os.unlink(rf)
        key(138, 1); time.sleep(1.8); key(138, 0)
        check(wait(lambda: "let go: no reset" in logtext(), 2) and not os.path.exists(rf), "held past the warning: warned, no reset")
        key(138, 1); time.sleep(3.0)
        check(os.path.exists(rf) and open(rf).read() == "action button held 10 s\n", "held past the reset stage: reset")
        key(138, 0)

        # ---- root's part of it: every saved network forgotten
        r = subprocess.run(["sh", f"{ROOT}/scripts/device/wifi.sh", "saved"], env=renv, capture_output=True, text=True)
        check(r.stdout.strip() == "1", "wifi.sh saved: 1")
        subprocess.run(["sh", f"{ROOT}/scripts/device/wifi.sh", "forget", state, out], env=renv, capture_output=True, timeout=20)
        r = subprocess.run(["sh", f"{ROOT}/scripts/device/wifi.sh", "saved"], env=renv, capture_output=True, text=True)
        check(W()["saved"] == [] and W()["networks"] == [] and r.stdout.strip() == "0", "wifi.sh forget: none saved, none known")
    finally:
        stop_root.set()
        proc.send_signal(signal.SIGTERM)
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
        if check.failed or sys.exc_info()[0]: print("---- hassmic\n" + logtext()[-6000:] + "\n---- root\n" + "".join(rootlog)[-3000:])
    if not check.failed: time.sleep(0.5); more()
    if not check.failed: time.sleep(0.5); taken()
    if check.failed: sys.exit(1)
    print("all Improv checks passed")


if __name__ == "__main__":
    main()
