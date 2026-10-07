#!/usr/bin/env python3
"""Setting the Echo up from a phone (Improv Wi-Fi over Bluetooth LE), end to end: build/hassmic-host with ble.c's
peripheral role talking H4 to a controller played here on a pseudo terminal (HASSMIC_BT_DEV), a phone's GATT client
written from the Bluetooth spec, Improv packets built and parsed by py-improv-ble-client (what Home Assistant's
improv_ble integration uses), and root's side played by the real scripts/device/wifi.sh against tests/fake_wifi_tools.py.
Advertising (service data, name), GATT discovery, reads, notifications, writes in pieces and long (prepared) writes,
device info, identify, scan, no host name command, a wrong password, the switch, the settings page's address back,
adoption ending the setup, an Echo that is set up wanting the action button, the factory reset by Home
Assistant's button and by holding the action button, and wifi.sh forgetting the networks."""
import asyncio, hashlib, json, os, queue, select, signal, struct, subprocess, sys, tempfile, threading, time, tty

from aioesphomeapi import APIClient
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
        self.att = queue.Queue(); self.lock = threading.Lock(); self.buf = b""; self.rx = b""
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
        ret = bytes(64)
        if op == 0x2002: ret = struct.pack("<HB", 251, 8)                  # LE Read Buffer Size: 251 bytes, 8 packets
        elif op == 0x2008: self.adv["data"] = par[1:1 + par[0]]
        elif op == 0x2009: self.adv["rsp"] = par[1:1 + par[0]]
        elif op == 0x200a: self.adv["on"] = par[0]
        self.event(0x0e, struct.pack("<BHB", 1, op, 0) + ret)

    def connect(self):
        self.adv["on"] = 0                                                  # the controller stops advertising
        self.event(0x3e, bytes([1, 0]) + struct.pack("<HBB", CONN, 1, 0) + bytes.fromhex("665544332211") + struct.pack("<HHHB", 24, 0, 400, 0))

    def disconnect(self): self.event(0x05, struct.pack("<BHB", 0, CONN, 0x13))

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
    with open(world, "w") as f:        # a new Echo, or one after a reset: no network saved, on none
        json.dump({"networks": [], "next_id": 0, "current": None, "target": None, "air": air, "arp_file": arp, "saved": [],
                   "lease": {"ip": "", "gw": ""}, "props": {"result": "", "reason": ""}}, f)
    open(arp, "w").write("IP address       HW type     Flags       HW address            Mask     Device\n")
    W = lambda: json.load(open(world))
    renv = dict(os.environ, PATH=f"{fake}:{os.environ['PATH']}", FAKE_WIFI_WORLD=world, WIFI_ARP=arp,
                WIFI_SCAN_SECS="0", WIFI_JOIN_SECS="3", WIFI_ADDR_SECS="3", WIFI_BACK_SECS="3")
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

    ctl = Controller(); keys = os.path.join(tmp, "keys"); os.mkfifo(keys)
    log = os.path.join(tmp, "log")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_WIFI=out, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
               HASSMIC_PLAY=os.path.join(tmp, "play.raw"), HASSMIC_MDNS_FILE=os.path.join(tmp, "none"), HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_MODELS=os.path.join(tmp, "models"), HASSMIC_ADB_OPEN=os.path.join(tmp, "adb-open"), HASSMIC_LOG=log,
               HASSMIC_BT_DEV=ctl.path, HASSMIC_FAKE_ONLINE=online, HASSMIC_IMPROV_TIMES="1 3 4 2")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Improv", "-L", "-z", "0", "-o", "0",
                             "-a", str(ARB), "-W", str(WEB), "-b", keys], env=env, stderr=open(log, "w"))
    kfd = os.open(keys, os.O_WRONLY)   # the keypad: input_event structs (x86-64: timeval, type, code, value)
    key = lambda code, v: os.write(kfd, struct.pack("<qqHHi", 0, 0, 1, code, v))
    logtext = lambda: open(log, errors="replace").read()
    phone = Phone(ctl)
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
        check(rsp[1] == 0x09 and rsp[2:2 + rsp[0] - 1] == b"Echo Improv", "scan response: the name")
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
        check(r is not None and r[0] == 0x09 and r[4:] == b"Echo Improv", "device name by type (Generic Access)")
        check(phone.write(hd["rpc"] + 5, b"\x00") is not None, "a write past the table is answered")
        for n in cccd: check(phone.write(cccd[n], b"\x01\x00") == b"\x13", f"notifications on: {n}")

        # ---- device info, identify
        phone.write(hd["rpc"], prot.DeviceInfoCmd().as_bytes())
        v = phone.note(hd["result"])
        res = prot.parse_result(v) if v else None
        check(isinstance(res, prot.DeviceInfoRes) and res.firmware_name == b"hassmic" and res.device_name == b"Echo Improv",
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
        check(phone.until(hd["state"], bytes([prot.State.PROVISIONING])), "provisioning")
        check(phone.until(hd["error"], bytes([prot.Error.UNABLE_TO_CONNECT]), 30) and phone.until(hd["state"], bytes([prot.State.AUTHORIZED])),
              "wrong password: unable to connect, authorized again")
        check(W()["saved"] == [], "nothing saved")
        phone.write(hd["rpc"], prot.WiFiSettingsCmd(b"HomeNet ", b"right password").as_bytes())   # a phone keyboard's space
        check(phone.until(hd["error"], b"\x00") and phone.until(hd["state"], bytes([prot.State.PROVISIONING])), "again: provisioning, error cleared")
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

        # ---- set up, network gone: advertising wants the button
        os.unlink(online)
        check(wait(lambda: ctl.adv["on"], 8), "set up, no network for a while: advertising")
        check(prot.ImprovServiceData.from_bytes(ctl.adv_fields()[0x16][2:]).state == prot.State.AUTHORIZATION_REQUIRED, "authorization required")
        ctl.connect(); phone = Phone(ctl); phone.discover()
        for n in cccd: phone.write(cccd[n], b"\x01\x00")
        phone.write(hd["rpc"], prot.WiFiSettingsCmd(b"Cafe", b"").as_bytes())
        check(phone.until(hd["error"], bytes([prot.Error.NOT_AUTHORIZED])), "Wi-Fi without the button: not authorized")
        key(138, 1); key(138, 0)                                            # KEY_HELP: the action button
        check(phone.until(hd["state"], bytes([prot.State.AUTHORIZED])), "action button: authorized")
        phone.write(hd["rpc"], prot.WiFiSettingsCmd(b"Cafe", b"").as_bytes())
        check(phone.until(hd["state"], bytes([prot.State.PROVISIONING])) and phone.until(hd["state"], bytes([prot.State.PROVISIONED]), 30),
              "then the switch to an open network works")
        check(len(W()["saved"]) == 1 and W()["saved"][0]["ssid"] == H("Cafe"), "saved: only Cafe")
        ctl.disconnect()

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
        key(138, 1); time.sleep(6); key(138, 0)
        check(wait(lambda: "let go: no reset" in logtext(), 2) and not os.path.exists(rf), "held 6 s: warned, no reset")
        key(138, 1); time.sleep(10.5)
        check(os.path.exists(rf) and open(rf).read() == "action button held 10 s\n", "held 10 s: reset")
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
    if check.failed: sys.exit(1)
    print("all Improv checks passed")


if __name__ == "__main__":
    main()
