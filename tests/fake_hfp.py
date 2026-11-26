#!/usr/bin/env python3
"""Phone calls over Bluetooth (hfp.c, issue #233), end to end: build/hassmic-host with its Bluetooth on a controller played
here on a pseudo terminal (HASSMIC_BT_DEV), and behind that controller a phone written from the specs: a paired link
(its key in state/bt_keys), L2CAP, an SDP search for the Hands-Free record, RFCOMM as the multiplexer's initiator (credit
based flow control, modem status), the audio gateway's side of the AT exchange and its call indicators, and the call's
eSCO link with its audio as SCO data over HCI (the PC's backend, HASSMIC_SCO=hci: 8 kHz s16, CVSD the controller's).
Home Assistant over the keyed link: the "Phone call", "Phone caller" and "Phone" sensors, answering, ending, a call
dialled from an action, a call taken on the handset and brought here, do not disturb leaving the ring to the phone,
"<wake word>, abnehmen" answering (no setting: Home Assistant answers a phone's call anyway),
no audio link taken without a call (it would carry the microphone).
The call's audio both ways: the phone's 1 kHz on the Echo's Voip stream (HASSMIC_VOIP), the Echo's call microphone
(HASSMIC_CALL_CAP, 700 Hz) in the SCO data the Echo sends.  Remote volume (+VGS both ways, none taken outside a call),
a second call (+CCWA, the action button takes it, +CLCC names the calls, ending one brings the held one back: "Phone
call waiting", "Switch phone call").  Then the phone again with codec negotiation: AT+BAC, +BCS 2, the link transparent,
mSBC both ways (frames made and read by BlueZ's sbcenc/sbcdec -m; skipped without them), "take the call here" as
AT+BCC.  Then an Echo with the feature off: no record, no RFCOMM."""
import asyncio, base64, math, os, queue, shutil, signal, struct, subprocess, sys, tempfile, threading, time, tty

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, WEB, ARB = 17090, 17091, 17092
PHONE = bytes.fromhex("a1b2c3d4e5f6")[::-1]          # its address as the air has it (least significant octet first)
ACL, SCO = 0x0010, 0x0020                          # the handles this controller gives the phone's links
NAME = "Pixel Test"


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def wait(fn, secs=5):
    end = time.time() + secs
    while time.time() < end:
        r = fn()
        if r: return r
        time.sleep(0.05)
    return fn()


_crc = []
for i in range(256):
    v = i
    for _ in range(8): v = (v >> 1) ^ 0xe0 if v & 1 else v >> 1
    _crc.append(v)
def fcs(b):
    f = 0xff
    for x in b: f = _crc[f ^ x]
    return 0xff - f


def tone_share(pcm, rate, f):
    """level (dBFS) and the share of the power within 30 Hz of f: Hann windows of 0.25 s, averaged (a frame dropped
    where the call's clock and the link's part breaks the phase, not the tone)"""
    x = np.frombuffer(pcm[:len(pcm) // 2 * 2], "<i2").astype(float)
    n = rate // 4
    if len(x) < n: return -200.0, 0.0
    band = tot = 0.0
    for s in range(0, len(x) - n + 1, n):
        Y = np.abs(np.fft.rfft(x[s:s + n] * np.hanning(n))) ** 2; fr = np.fft.rfftfreq(n, 1 / rate)
        band += Y[np.abs(fr - f) <= 30].sum(); tot += Y.sum()
    return 10 * np.log10(np.mean(x ** 2) / 32768 ** 2 + 1e-20), band / (tot + 1e-20)


class Phone:
    """The Echo's Bluetooth chip as hassmic sees it through /dev/stpbt (H4), with a phone behind it"""
    def __init__(self):
        self.m, self.s = os.openpty()
        tty.setraw(self.m); tty.setraw(self.s)
        self.path = os.ttyname(self.s)
        self.lock = threading.Lock(); self.buf = b""; self.acl_rx = {}; self.cmds = queue.Queue()
        self.sig = queue.Queue(); self.chans = {}                # L2CAP: signalling packets; our CID -> its frames
        self.sco_out = bytearray(); self.sco_count = 0; self.sco_on = False
        self.at = queue.Queue(); self.dlc_cred = 0
        self.calls = []; self.clcc_asked = 0                         # what it answers AT+CLCC with
        self.refuse = set(); self.refuse_completes = False           # commands this controller refuses (and ends)
        self.next_cid = 0x40; self.ident = 1
        threading.Thread(target=self.reader, daemon=True).start()

    def send(self, b):
        with self.lock: os.write(self.m, b)

    def event(self, code, par): self.send(bytes([4, code, len(par)]) + par)

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
                    self.event(0x13, struct.pack("<BHH", 1, h, 1))         # Number Of Completed Packets
                    r = self.acl_rx.get(h, b"")
                    r = data if pb != 1 else r + data
                    if len(r) >= 4 and len(r) >= 4 + (r[0] | r[1] << 8):
                        l, cid = struct.unpack("<HH", r[:4]); self.l2(cid, r[4:4 + l]); r = b""
                    self.acl_rx[h] = r
                elif t == 3:
                    if len(self.buf) < 4 or len(self.buf) < 4 + self.buf[3]: break
                    h = (self.buf[1] | self.buf[2] << 8) & 0xfff
                    if h == SCO and self.sco_on: self.sco_out += self.buf[4:4 + self.buf[3]]; self.sco_count += 1
                    self.buf = self.buf[4 + self.buf[3]:]
                else: self.buf = self.buf[1:]

    def command(self, op, par):
        ret = bytes(64)
        if op == 0x1005: ret = struct.pack("<HBHH", 1021, 64, 8, 4)             # Read Buffer Size
        elif op == 0x2002: ret = struct.pack("<HB", 251, 8)
        if op in self.refuse:
            self.event(0x0f, struct.pack("<BBH", 0x12, 1, op))                 # Command Status: invalid parameters
            if op == 0x043e and self.refuse_completes:                           # and, as a chip might, the link's end too
                self.event(0x2c, bytes([0x12]) + struct.pack("<H", 0) + PHONE + bytes([2, 0, 0]) + struct.pack("<HH", 0, 0) + bytes([0]))
        elif op in (0x0405, 0x0409, 0x0419, 0x0411, 0x0413, 0x0428, 0x0429, 0x042a, 0x043e, 0x0406):
            self.event(0x0f, struct.pack("<BBH", 0, 1, op))                    # Command Status
        else:
            self.event(0x0e, struct.pack("<BHB", 1, op, 0) + ret)
        self.cmds.put((op, par))
        if op == 0x042a and par[:6] == PHONE:                                    # a rejected link ends as the spec has it
            self.event(0x2c, bytes([par[6] if len(par) > 6 else 0x0d]) + struct.pack("<H", 0) + PHONE + bytes([2, 0, 0]) + struct.pack("<HH", 0, 0) + bytes([0]))
        if op == 0x0419 and par[:6] == PHONE:                                    # Remote Name Request
            self.event(0x07, bytes([0]) + PHONE + NAME.encode().ljust(248, b"\0"))

    def expect_cmd(self, op, secs=5):
        end = time.time() + secs
        while time.time() < end:
            try: o, p = self.cmds.get(timeout=max(0.01, end - time.time()))
            except queue.Empty: break
            if o == op: return p
        return None

    # ------------------------------------------------ L2CAP
    def acl(self, cid, pdu):
        l2 = struct.pack("<HH", len(pdu), cid) + pdu
        self.send(bytes([2]) + struct.pack("<HH", ACL | 0x2000, len(l2)) + l2)

    def l2(self, cid, pdu):
        if cid == 1: self.sig.put(pdu)
        elif cid in self.chans: self.chans[cid].put(pdu)

    def sig_cmd(self, code, data):
        self.ident = self.ident % 255 + 1
        self.acl(1, bytes([code, self.ident]) + struct.pack("<H", len(data)) + data)
        return self.ident

    def sig_get(self, code, secs=3):
        end = time.time() + secs
        while time.time() < end:
            try: p = self.sig.get(timeout=max(0.01, end - time.time()))
            except queue.Empty: break
            if p[0] == code: return p
        return None

    def open_chan(self, psm):
        """our L2CAP channel to the Echo: (our CID, its CID), or (our CID, None) with the refusal's result"""
        cid = self.next_cid; self.next_cid += 1; self.chans[cid] = queue.Queue()
        self.sig_cmd(0x02, struct.pack("<HH", psm, cid))
        r = self.sig_get(0x03)
        while r and struct.unpack("<HHHH", r[4:12])[2] == 1: r = self.sig_get(0x03)    # pending (encryption)
        if not r: return cid, None, -1
        dcid, scid, res, _ = struct.unpack("<HHHH", r[4:12])
        if res: return cid, None, res
        cr = self.sig_get(0x04)                                                  # its Configuration Request
        if cr: self.acl(1, bytes([0x05, cr[1]]) + struct.pack("<HHHH", 6, dcid, 0, 0))
        self.sig_cmd(0x04, struct.pack("<HH", dcid, 0))
        self.sig_get(0x05)
        return cid, dcid, 0

    def chan_get(self, cid, secs=3):
        try: return self.chans[cid].get(timeout=secs)
        except queue.Empty: return None

    # ------------------------------------------------ links
    def connect(self):
        self.event(0x04, PHONE + bytes([0x0c, 0x02, 0x5a, 1]))                 # Connection Request: a smartphone, ACL
        p = self.expect_cmd(0x0409)
        if p is None: return False
        self.event(0x03, bytes([0]) + struct.pack("<H", ACL) + PHONE + bytes([1, 1]))   # complete, encrypted (bonded)
        return True

    def sco_request(self, air=2):
        self.event(0x04, PHONE + bytes([0x0c, 0x02, 0x5a, 2]))                 # Connection Request: eSCO
        while True:
            try: o, p = self.cmds.get(timeout=3)
            except queue.Empty: return None
            if o in (0x0429, 0x042a, 0x043e): break
        if o == 0x042a: return "refused"
        self.sco_complete(air)
        return p

    def sco_complete(self, air=2):
        """Synchronous Connection Complete: eSCO, air mode 2 CVSD or 3 transparent (mSBC)"""
        n = 60 if air == 3 else 60
        self.event(0x2c, bytes([0]) + struct.pack("<H", SCO) + PHONE + bytes([2, 12, 2]) + struct.pack("<HH", n, n) + bytes([air]))
        self.sco_on = True

    def sco_feed_raw(self, packets, every):
        """SCO packets as they are, one each EVERY seconds"""
        t0 = time.time()
        for n, pk in enumerate(packets):
            self.send(bytes([3]) + struct.pack("<HB", SCO, len(pk)) + pk)
            d = t0 + (n + 1) * every - time.time()
            if d > 0: time.sleep(d)

    def sco_close(self):
        self.sco_on = False
        self.event(0x05, bytes([0]) + struct.pack("<H", SCO) + bytes([0x13]))

    def sco_feed(self, secs, freq=1000, amp=9000):
        """the phone's audio: 8 kHz s16 in 48-byte packets at its pace"""
        k = 0; t0 = time.time(); n = 0
        while time.time() - t0 < secs:
            pcm = struct.pack("<24h", *(int(amp * math.sin(2 * math.pi * freq * (k + i) / 8000)) for i in range(24)))
            k += 24
            self.send(bytes([3]) + struct.pack("<HB", SCO, 48) + pcm)
            n += 1
            d = t0 + n * 0.003 - time.time()
            if d > 0: time.sleep(d)

    # ------------------------------------------------ RFCOMM (we are the initiator: commands C/R 1, responses 0)
    def rf(self, dlci, cr, ctrl, info=b"", credits=None):
        h = bytes([dlci << 2 | cr << 1 | 1, ctrl | (0x10 if credits is not None else 0)])
        ln = bytes([len(info) << 1 | 1]) if len(info) < 128 else bytes([(len(info) & 0x7f) << 1, len(info) >> 7])
        body = h + ln + (bytes([credits]) if credits is not None else b"") + info
        f = fcs(h if ctrl & 0xef == 0xef else h + ln)
        self.acl(self.rdcid, body + bytes([f]))

    def rf_get(self, secs=3):
        """the next RFCOMM frame from the Echo: (dlci, cr, ctrl, info), its AT text kept apart"""
        end = time.time() + secs
        while time.time() < end:
            p = self.chan_get(self.rcid, max(0.01, end - time.time()))
            if p is None: return None
            dlci, cr, ctrl = p[0] >> 2, p[0] >> 1 & 1, p[1]
            if p[2] & 1: ln, h = p[2] >> 1, 3
            else: ln, h = p[2] >> 1 | p[3] << 7, 4
            cred = p[h] if ctrl == 0xff and dlci else None
            info = p[h + (cred is not None):h + (cred is not None) + ln]
            if dlci and ctrl & 0xef == 0xef:
                if cred: self.dlc_cred += cred
                if info:
                    for line in info.decode(errors="replace").replace("\n", "\r").split("\r"):
                        if line: self.at.put(line)
                        if line.startswith("AT+VGS="): self.rf(2, 1, 0xef, b"\r\nOK\r\n")   # as every phone: noted
                        if line == "AT+CLCC":                                    # its list of calls, whenever asked
                            self.clcc_asked += 1
                            self.rf(2, 1, 0xef, "".join(f"\r\n+CLCC: {c}\r\n" for c in self.calls).encode() + b"\r\nOK\r\n")
                    self.rf(2, 1, 0xef, credits=1)                               # its credit back
                continue
            return dlci, cr, ctrl, info
        return None

    def at_expect(self, want, secs=5):
        end = time.time() + secs
        while time.time() < end:
            self.rf_get(0.05)
            try: l = self.at.get(timeout=0.01)
            except queue.Empty: continue
            if l.startswith(want): return l
            self.at_seen.append(l)
        return None

    def at_send(self, *lines):
        for l in lines: self.rf(2, 1, 0xef, f"\r\n{l}\r\n".encode())

    def pump(self, secs):
        end = time.time() + secs
        while time.time() < end:
            self.rf_get(0.05)
            try:
                while True: self.at_seen.append(self.at.get_nowait())
            except queue.Empty: pass


def mux(p):
    """RFCOMM up to the hands-free DLC: (ok, what the Echo answered)"""
    p.rf(0, 1, 0x3f)                                                               # SABM, DLCI 0
    ua = p.rf_get()
    if not ua or ua[2] != 0x73: return False, f"mux: {ua}"
    p.rf(0, 1, 0xef, bytes([0x83, 0x11, 2, 0xf0, 7, 0, 127, 0, 0, 7]))           # PN for DLCI 2: credit flow, k 7
    pn = p.rf_get()
    if not pn or pn[3][:1] != b"\x81" or pn[3][3] != 0xe0: return False, f"PN: {pn}"
    p.dlc_cred = pn[3][9] & 7
    p.rf(2, 1, 0x3f)                                                               # SABM, DLCI 2
    ua = p.rf_get()
    if not ua or ua[0] != 2 or ua[2] != 0x73: return False, f"DLC: {ua}"
    msc = p.rf_get()                                                               # the Echo's MSC
    if not msc or msc[3][:1] != b"\xe3": return False, f"MSC: {msc}"
    p.rf(0, 1, 0xef, bytes([0xe1]) + msc[3][1:])                                   # answered
    p.rf(0, 1, 0xef, bytes([0xe3, 0x05, 0x0b, 0x8d]))                              # and ours
    r = p.rf_get()
    return bool(r and r[3][:1] == b"\xe1"), f"MSC answer: {r}"


def slc(p, brsf=871 & ~0x200):
    """the audio gateway's side of the service level connection: what the hands-free sent of everything it should.
    871: three-way, EC/NR, voice recognition, reject, enhanced call status, extended errors, codec negotiation (less it
    for the CVSD part: an AG without codec negotiation)"""
    got = []
    l = p.at_expect("AT+BRSF="); got.append(l); p.at_send(f"+BRSF: {brsf}", "OK")
    if brsf & 0x200:
        l = p.at_expect("AT+BAC="); got.append(l == "AT+BAC=1,2" and l); p.at_send("OK")
    l = p.at_expect("AT+CIND=?"); got.append(l)
    p.at_send('+CIND: ("call",(0,1)),("callsetup",(0-3)),("service",(0-1)),("signal",(0-5)),("roam",(0,1)),("battchg",(0-5)),("callheld",(0-2))', "OK")
    l = p.at_expect("AT+CIND?"); got.append(l); p.at_send("+CIND: 0,0,1,4,0,5,0", "OK")
    l = p.at_expect("AT+CMER=3,0,0,1"); got.append(l); p.at_send("OK")
    l = p.at_expect("AT+CHLD=?"); got.append(l); p.at_send("+CHLD: (0,1,1x,2,2x,3,4)", "OK")
    for c in ("AT+CLIP=1", "AT+CCWA=1", "AT+CMEE=1", "AT+NREC=0"):
        l = p.at_expect(c); got.append(l); p.at_send("OK")
    return got


def msbc_tools():
    return shutil.which("sbcenc") and shutil.which("sbcdec")


def msbc_tone(tmp, freq, secs):
    """mSBC frames (57 bytes each) of a tone at 16 kHz, by BlueZ's encoder"""
    au = os.path.join(tmp, f"tone{freq}.au")
    with open(au, "wb") as f:
        f.write(struct.pack(">6I", 0x2e736e64, 24, 0xffffffff, 3, 16000, 1))
        f.write(struct.pack(f">{16000 * secs}h", *(int(9000 * math.sin(2 * math.pi * freq * i / 16000)) for i in range(16000 * secs))))
    b = subprocess.run(["sbcenc", "-m", au], capture_output=True).stdout
    return [b[i:i + 57] for i in range(0, len(b) - 56, 57)]


def msbc_read(tmp, air):
    """the H2-framed mSBC the Echo sent, decoded by BlueZ's decoder: (16 kHz PCM, frames found)"""
    frames, i = [], 0
    while i + 60 <= len(air):
        if air[i] == 1 and air[i + 1] in (0x08, 0x38, 0xc8, 0xf8) and air[i + 2] == 0xad: frames.append(air[i + 2:i + 59]); i += 60
        else: i += 1
    src, out = os.path.join(tmp, "echo.msbc"), os.path.join(tmp, "echo.au")
    open(src, "wb").write(b"".join(frames))
    subprocess.run(["sbcdec", "-m", "-f", out, src], capture_output=True)
    b = open(out, "rb").read() if os.path.exists(out) else b""
    if len(b) < 24: return b"", len(frames)
    x = b[int.from_bytes(b[4:8], "big"):]
    return b"".join(x[k + 1:k + 2] + x[k:k + 1] for k in range(0, len(x) - 1, 2)), len(frames)


async def ha(port, key):
    cli = APIClient("127.0.0.1", port, None, noise_psk=key)
    await cli.connect(login=True)
    ents, svcs = await cli.list_entities_services()
    by = {e.object_id: e for e in ents}
    states = {}
    def on(s):
        e = next((x for x in ents if x.key == s.key), None)
        if e: states[e.object_id] = getattr(s, "state", None)
    cli.subscribe_states(on)
    return cli, by, svcs, states


async def until(fn, secs=5):
    end = time.time() + secs
    while time.time() < end:
        if fn(): return True
        await asyncio.sleep(0.05)
    return fn()


def echo(state, config, voip, mic, extra=None, arb=ARB):
    with open(os.path.join(state, "config"), "w") as f: f.write(config)
    key = base64.b64encode(os.urandom(32)).decode()
    with open(os.path.join(state, "api_key"), "w") as f: f.write(key + "\n")
    with open(os.path.join(state, "bt_keys"), "w") as f: f.write("a1b2c3d4e5f6 4 " + "11" * 16 + "\n")     # paired before
    ph = Phone()
    log = os.path.join(state, "log")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(state, "models"),
               HASSMIC_ADB_OPEN=os.path.join(state, "adb-open"), HASSMIC_BT_DEV=ph.path, HASSMIC_SCO="hci",
               HASSMIC_VOIP=voip, HASSMIC_CALL_CAP=mic, **(extra or {}))
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Phone", "-L", "-z", "0", "-o", "0",
                             "-a", str(arb), "-W", str(WEB)], env=env, stderr=open(log, "w"))
    ph.at_seen = []
    return proc, ph, key, log


async def main():
    tmp = tempfile.mkdtemp(); state = os.path.join(tmp, "state"); os.makedirs(state)
    voip, mic = os.path.join(tmp, "voip.raw"), os.path.join(tmp, "mic.raw")
    with open(mic, "wb") as f:                                                    # the Echo's call microphone: 700 Hz
        f.write(struct.pack(f"<{16000 * 4}h", *(int(8000 * math.sin(2 * math.pi * 700 * i / 16000)) for i in range(16000 * 4))))
    # without arbitration, so without Drop In: call.c's playout and call microphone are no Drop In's (issue #239: they
    # were started by dropin_start alone, and a call where it did not run had no audio either way)
    proc, ph, key, log = echo(state, "bluetooth_calls=on\n", voip, mic, arb=0)
    try:
        check(wait(lambda: any(o == 0x0c1a and p[:1] == b"\x02" for o, p in list(ph.cmds.queue)), 10), "page scan on: a paired device may connect")
        cod = wait(lambda: next((p for o, p in list(ph.cmds.queue) if o == 0x0c24), None), 3)
        check(cod is not None and cod[2] & 0x40, f"class of device with telephony ({cod.hex() if cod else None})")
        check(ph.connect(), "the phone's link accepted")
        cid, dcid, res = ph.open_chan(1)
        check(dcid is not None, "SDP channel open")
        req = bytes([0x06, 0, 1]) + struct.pack(">H", 15) + bytes([0x35, 3, 0x19, 0x11, 0x1e]) + struct.pack(">H", 0xffff) + bytes([0x35, 5, 0x0a, 0, 0, 0xff, 0xff, 0])
        ph.acl(dcid, req)
        r = ph.chan_get(cid)
        check(r is not None and r[0] == 0x07 and bytes([0x19, 0x11, 0x1e]) in r and bytes([0x19, 0x00, 0x03, 0x08, 0x01]) in r,
              "SDP: a Hands-Free record, RFCOMM channel 1")
        check(r is not None and bytes([0x09, 0x01, 0x07]) in r and bytes([0x09, 0x03, 0x11, 0x09, 0x00, 0x37]) in r,
              "SDP: HFP 1.7, features EC/NR, three-way, CLI, remote volume, wideband speech")
        ph.rcid, _, _ = (None, None, None)
        ph.rcid, ph.rdcid, res = ph.open_chan(3)
        check(ph.rdcid is not None, "RFCOMM's L2CAP channel open")
        ok, what = mux(ph)
        check(ok, f"RFCOMM: multiplexer, credit flow, our DLC and modem status ({what})")
        got = slc(ph)
        check(all(got) and got[0] == "AT+BRSF=183", f"the service level connection: {got}")

        cli, by, svcs, st = await ha(PORT, key)
        names = set(by)
        check({"phone_call", "phone_caller", "phone", "answer_phone_call", "end_phone_call", "phone_call_here"} <= names, "Home Assistant: the phone's entities listed")
        check(any(s.name == "phone_call" for s in svcs), "Home Assistant: the phone_call action listed")
        check(await until(lambda: st.get("phone_call") == "idle" and st.get("phone") == NAME), f"Home Assistant: idle, the phone named ({st.get('phone_call')}, {st.get('phone')})")

        # no call: an audio link the phone opens (its voice assistant, an app) does not get the Echo's microphone
        check(ph.sco_request() == "refused", "no call: the phone's audio link refused")

        # an incoming call: rings here, answered with the action button, its audio both ways
        ph.at_send("+CIEV: 2,1", "RING", '+CLIP: "+4930123456",145,,,"Alice"')
        check(await until(lambda: st.get("phone_call") == "ringing" and st.get("phone_caller") == "Alice"), f"ringing, Alice calls ({st.get('phone_call')}, {st.get('phone_caller')})")
        check(wait(lambda: "call: ringing" in open(log).read() or "call: ringing Alice" in open(log).read(), 3), "the Echo rings")
        proc.send_signal(signal.SIGUSR2)                                          # the action button
        check(ph.at_expect("ATA") is not None, "the action button answers: ATA")
        ph.at_send("OK", "+CIEV: 1,1", "+CIEV: 2,0")
        acc = ph.sco_request()
        check(acc not in (None, "refused") and acc[16:18] == b"\x60\x00", f"the audio link accepted, CVSD ({acc.hex() if isinstance(acc, bytes) else acc})")
        check(await until(lambda: st.get("phone_call") == "connected"), f"connected ({st.get('phone_call')})")
        vgs = ph.at_expect("AT+VGS=", 3)                                       # remote volume: the Echo's call volume, with the audio
        check(vgs is not None, f"the call's volume to the phone with its audio: {vgs}")
        start = os.path.getsize(voip) if os.path.exists(voip) else 0
        feeder = threading.Thread(target=ph.sco_feed, args=(4,)); feeder.start()
        await asyncio.sleep(4.2); feeder.join()
        db, share = tone_share(bytes(ph.sco_out[-16000 * 2:]), 8000, 700)
        check(ph.sco_count > 1000 and share > 0.8, f"the Echo's microphone to the phone: {ph.sco_count} packets, 700 Hz {100 * share:.0f} % at {db:.1f} dBFS")
        played = open(voip, "rb").read()[start:] if os.path.exists(voip) else b""
        db, share = tone_share(played[-16000 * 2 * 2:], 16000, 1000)
        check(len(played) > 16000 * 2 and share > 0.8, f"the phone's audio on the Voip stream: {len(played)} bytes, 1 kHz {100 * share:.0f} % at {db:.1f} dBFS")

        # remote volume: the phone's keys move the call's volume, ours go to it
        ph.at_seen.clear(); ph.at_send("+VGS: 3")
        check(wait(lambda: "phone: volume 3 of 15 from the phone: 20" in open(log).read(), 3), "the phone's +VGS: 3 sets the call's volume to 20")
        ph.pump(0.5)
        check(not any(l.startswith("AT+VGS") for l in ph.at_seen), f"its own volume not sent back to it ({ph.at_seen})")
        mp = next(e for e in by.values() if e.object_id == "media_player" or type(e).__name__ == "MediaPlayerInfo")
        cli.media_player_command(mp.key, volume=0.8)
        l = ph.at_expect("AT+VGS=", 3)
        check(l == "AT+VGS=12", f"the Echo's volume at 80: {l} to the phone")

        # a second call: it waits, the action button takes it (this one held), +CLCC names both, ending brings the other back
        alice, bob = '"+4930123456",145,"Alice"', '"+4917000",145,"Bob"'
        ph.at_seen.clear(); asked = ph.clcc_asked
        ph.calls = [f"1,1,0,0,0,{alice}", f"2,1,5,0,0,{bob}"]
        ph.at_send("+CIEV: 2,1", '+CCWA: "+4917000",145,1,"Bob"')
        check(await until(lambda: st.get("phone_call_waiting") == "Bob" and st.get("phone_call") == "connected" and st.get("phone_caller") == "Alice"),
              f"Bob waits: Phone call waiting ({st.get('phone_call_waiting')}, {st.get('phone_call')}, {st.get('phone_caller')})")
        proc.send_signal(signal.SIGUSR2)
        check(ph.at_expect("AT+CHLD=2") is not None, "the action button with a call waiting: AT+CHLD=2")
        ph.calls = [f"1,1,1,0,0,{alice}", f"2,1,0,0,0,{bob}"]
        ph.at_send("OK", "+CIEV: 2,0", "+CIEV: 7,1")
        check(await until(lambda: st.get("phone_caller") == "Bob" and st.get("phone_call_waiting") == "Alice"),
              f"Bob talks, Alice on hold ({st.get('phone_caller')}, {st.get('phone_call_waiting')})")
        check(ph.clcc_asked > asked, f"the calls listed (enhanced call status): AT+CLCC {ph.clcc_asked - asked} times")
        cli.button_command(by["switch_phone_call"].key)
        check(ph.at_expect("AT+CHLD=2") is not None, "Switch phone call: AT+CHLD=2")
        ph.calls = [f"1,1,0,0,0,{alice}", f"2,1,1,0,0,{bob}"]
        ph.at_send("OK", "+CIEV: 7,1")
        check(await until(lambda: st.get("phone_caller") == "Alice" and st.get("phone_call_waiting") == "Bob"),
              f"swapped: Alice talks, Bob on hold ({st.get('phone_caller')}, {st.get('phone_call_waiting')})")
        cli.button_command(by["end_phone_call"].key)
        check(ph.at_expect("AT+CHLD=1") is not None, "End phone call beside a held one: AT+CHLD=1")
        ph.calls = [f"2,1,0,0,0,{bob}"]
        ph.at_send("OK", "+CIEV: 7,0")
        check(await until(lambda: st.get("phone_caller") == "Bob" and st.get("phone_call_waiting") == "" and st.get("phone_call") == "connected"),
              f"Bob back, nothing waiting ({st.get('phone_caller')}, {st.get('phone_call_waiting')!r}, {st.get('phone_call')})")

        # a third caller while Bob talks, taken; then she hangs up: Bob, held, stays the call (also where a phone drops
        # the call indicator to 0 with one held)
        carol = '"+4915000",145,"Carol"'
        ph.calls = [f"2,1,0,0,0,{bob}", f"3,1,5,0,0,{carol}"]
        ph.at_send("+CIEV: 2,1", '+CCWA: "+4915000",145,1,"Carol"')
        check(await until(lambda: st.get("phone_call_waiting") == "Carol"), f"Carol waits ({st.get('phone_call_waiting')})")
        cli.button_command(by["switch_phone_call"].key)
        check(ph.at_expect("AT+CHLD=2") is not None, "Switch phone call takes her: AT+CHLD=2")
        ph.calls = [f"2,1,1,0,0,{bob}", f"3,1,0,0,0,{carol}"]
        ph.at_send("OK", "+CIEV: 7,1", "+CIEV: 2,0")
        check(await until(lambda: st.get("phone_caller") == "Carol" and st.get("phone_call_waiting") == "Bob"), f"Carol talks, Bob on hold ({st.get('phone_caller')}, {st.get('phone_call_waiting')})")
        ph.calls = [f"2,1,1,0,0,{bob}"]
        ph.at_send("+CIEV: 7,2", "+CIEV: 1,0")
        await asyncio.sleep(0.5)
        check(st.get("phone_call") == "connected" and st.get("phone_caller") == "Bob" and st.get("phone_call_waiting") == "",
              f"Carol hung up, the call indicator 0: Bob (held) still the call ({st.get('phone_call')}, {st.get('phone_caller')}, {st.get('phone_call_waiting')!r})")
        ph.calls = [f"2,1,0,0,0,{bob}"]
        ph.at_send("+CIEV: 1,1", "+CIEV: 7,0")
        check(await until(lambda: st.get("phone_call") == "connected" and st.get("phone_caller") == "Bob"), "Bob taken back on the phone")
        ph.calls = []

        cli.button_command(by["end_phone_call"].key)
        check(ph.at_expect("AT+CHUP") is not None, "Home Assistant's End phone call: AT+CHUP")
        ph.at_send("OK", "+CIEV: 1,0"); ph.sco_close()
        check(await until(lambda: st.get("phone_call") == "idle" and st.get("phone_caller") == ""), f"idle again ({st.get('phone_call')}, {st.get('phone_caller')!r})")
        check(wait(lambda: "phone: call audio stopped" in open(log).read(), 3), "the call's audio stopped")
        ph.at_send("+VGS: 9"); ph.pump(0.5)
        check("phone: its volume (9 of 15) not taken outside a call" in open(log).read(), "a +VGS outside a call not taken")

        # a call dialled from Home Assistant: the phone calls, ringback and talk here
        await cli.execute_service(next(s for s in svcs if s.name == "phone_call"), {"number": "+4912345"})
        check(ph.at_expect("ATD+4912345;") is not None, "the phone_call action: ATD+4912345;")
        ph.at_send("OK", "+CIEV: 2,2")
        check(await until(lambda: st.get("phone_call") == "calling"), f"calling ({st.get('phone_call')})")
        ph.at_send("+CIEV: 2,3")
        acc = ph.sco_request()
        check(acc not in (None, "refused"), "the dialled call's audio link accepted")
        ph.at_send("+CIEV: 1,1", "+CIEV: 2,0")
        check(await until(lambda: st.get("phone_call") == "connected"), f"connected ({st.get('phone_call')})")
        ph.at_send("+CIEV: 1,0"); ph.sco_close()                                  # the other side hung up
        check(await until(lambda: st.get("phone_call") == "idle"), f"the far end hung up: idle ({st.get('phone_call')})")
        await cli.execute_service(next(s for s in svcs if s.name == "phone_call"), {"number": "12; ATH"})
        ph.pump(1)
        check(not any(l.startswith("ATD") for l in ph.at_seen), "no number: nothing dialled")

        # a call answered on the handset: nothing here until it is taken here
        ph.at_seen.clear()
        ph.at_send("+CIEV: 2,1", "RING")
        check(await until(lambda: st.get("phone_call") == "ringing"), "ringing again")
        ph.at_send("+CIEV: 1,1", "+CIEV: 2,0")
        check(await until(lambda: st.get("phone_call") == "on the phone"), f"answered on the handset: on the phone ({st.get('phone_call')})")
        cli.button_command(by["phone_call_here"].key)
        p = ph.expect_cmd(0x0428)
        check(p is not None and p[:2] == struct.pack("<H", ACL) and p[12:14] == b"\x60\x00", "Take the phone call here: the Echo sets up the audio link")
        ph.event(0x2c, bytes([0]) + struct.pack("<H", SCO) + PHONE + bytes([2, 12, 2]) + struct.pack("<HH", 60, 60) + bytes([2])); ph.sco_on = True
        check(await until(lambda: st.get("phone_call") == "connected"), f"its audio here: connected ({st.get('phone_call')})")
        ph.sco_close()
        check(await until(lambda: st.get("phone_call") == "on the phone"), "the phone took the audio back: on the phone")
        ph.at_send("+CIEV: 1,0")
        check(await until(lambda: st.get("phone_call") == "idle"), "ended on the handset: idle")

        # do not disturb: the phone rings alone, its audio link is still taken when the phone sends it here
        dnd = next(e for e in by.values() if e.object_id == "do_not_disturb")
        cli.switch_command(dnd.key, True); await asyncio.sleep(0.3)
        before = open(log).read().count("call: ringing")
        ph.at_send("+CIEV: 2,1", "RING")
        check(await until(lambda: st.get("phone_call") == "ringing"), "do not disturb: Home Assistant still sees it ring")
        await asyncio.sleep(0.5)
        check(open(log).read().count("call: ringing") == before, "do not disturb: the Echo does not ring")
        ph.at_seen.clear(); proc.send_signal(signal.SIGUSR2); ph.pump(1)
        check("ATA" not in ph.at_seen, "do not disturb: the action button answers nothing")
        ph.at_send("+CIEV: 2,0"); cli.switch_command(dnd.key, False)
        check(await until(lambda: st.get("phone_call") == "idle"), "the caller gave up: idle")

        # "<wake word>, abnehmen" while the phone rings (issue #240): Home Assistant answers a phone's call anyway
        # ("Answer phone call"), so answer_by_voice (off here) is not asked, nor what began the run
        starts = []
        async def h_start(conv, flags, settings, phrase): starts.append(time.monotonic()); return 0
        async def h_stop(abort): pass
        async def h_audio(data, *_): pass
        unsub = cli.subscribe_voice_assistant(handle_start=h_start, handle_stop=h_stop, handle_audio=h_audio)
        await asyncio.sleep(0.5)
        ph.at_seen.clear(); ph.at_send("+CIEV: 2,1", "RING")
        check(await until(lambda: st.get("phone_call") == "ringing"), "ringing")
        l0 = len(open(log).read()); proc.send_signal(signal.SIGUSR1)
        check(await until(lambda: starts, 3), "the wake word while the phone rings: a request")
        for e, d in ((Ev.VOICE_ASSISTANT_RUN_START, None), (Ev.VOICE_ASSISTANT_STT_START, None), (Ev.VOICE_ASSISTANT_STT_END, {"text": "Abnehmen."})):
            cli.send_voice_assistant_event(e, d)
        check(ph.at_expect("ATA") is not None and "said answer: the call answered" in open(log).read()[l0:], "\"<wake word>, abnehmen\": ATA")
        ph.at_send("OK", "+CIEV: 1,1", "+CIEV: 2,0"); await asyncio.sleep(0.3); ph.at_send("+CIEV: 1,0")
        check(await until(lambda: st.get("phone_call") == "idle"), "ended on the handset: idle")
        unsub(); await asyncio.sleep(0.3)

        # the phone again, now with codec negotiation: mSBC
        ph.rf(2, 1, 0x53)                                                        # DISC of the hands-free DLC
        ua = ph.rf_get()
        check(ua is not None and ua[0] == 2 and ua[2] == 0x73, f"DISC answered UA ({ua})")
        check(await until(lambda: st.get("phone_call") == "no phone"), f"the DLC closed: no phone ({st.get('phone_call')})")
        ok, what = mux(ph)
        got = slc(ph, 871)
        check(ok and all(got) and got[1] == "AT+BAC=1,2", f"again with codec negotiation: {got}")
        check(await until(lambda: st.get("phone_call") == "idle"), f"idle ({st.get('phone_call')})")
        ph.at_seen.clear()
        ph.at_send("+CIEV: 2,1", "RING")
        check(await until(lambda: st.get("phone_call") == "ringing"), "ringing")
        cli.button_command(by["answer_phone_call"].key)
        check(ph.at_expect("ATA") is not None, "Answer phone call: ATA")
        ph.at_send("OK", "+CIEV: 1,1", "+CIEV: 2,0", "+BCS: 2")
        check(ph.at_expect("AT+BCS=2") is not None, "the phone picks mSBC: AT+BCS=2"); ph.at_send("OK")
        acc = ph.sco_request(air=3)
        check(isinstance(acc, bytes) and acc[16:18] == b"\x63\x00" and acc[19:21] == b"\x88\x03",
              f"the audio link accepted transparent, EV3 or 2-EV3 ({acc.hex() if isinstance(acc, bytes) else acc})")
        check(await until(lambda: st.get("phone_call") == "connected"), f"connected ({st.get('phone_call')})")
        check(wait(lambda: "phone: call audio runs (hci, mSBC)" in open(log).read(), 3), "the call's audio runs as mSBC")
        if msbc_tools():
            ph.sco_out.clear(); start = os.path.getsize(voip) if os.path.exists(voip) else 0
            frames = msbc_tone(tmp, 1000, 4)
            h2 = (0x08, 0x38, 0xc8, 0xf8)
            pk = [bytes([1, h2[i & 3]]) + f + b"\0" for i, f in enumerate(frames)]
            feeder = threading.Thread(target=ph.sco_feed_raw, args=(pk, 0.0075)); feeder.start()
            await asyncio.sleep(4.2); feeder.join()
            pcm, nf = msbc_read(tmp, bytes(ph.sco_out))
            db, share = tone_share(pcm[-16000 * 2 * 2:], 16000, 700)
            check(nf > 400 and share > 0.8, f"mSBC to the phone: {nf} frames, 700 Hz {100 * share:.0f} % at {db:.1f} dBFS (sbcdec -m)")
            played = open(voip, "rb").read()[start:] if os.path.exists(voip) else b""
            db, share = tone_share(played[-16000 * 2 * 2:], 16000, 1000)
            check(len(played) > 16000 * 2 and share > 0.8, f"mSBC from the phone on the Voip stream: {len(played)} bytes, 1 kHz {100 * share:.0f} % at {db:.1f} dBFS")
        else: print("skip mSBC's audio: sbcenc/sbcdec (sbc-tools) missing")
        ph.sco_close()
        check(await until(lambda: st.get("phone_call") == "on the phone"), "the phone took the audio back: on the phone")
        cli.button_command(by["phone_call_here"].key)
        check(ph.at_expect("AT+BCC") is not None, "Take the phone call here, codecs negotiated: AT+BCC (the phone sets the link up)")
        ph.at_send("OK", "+BCS: 1")
        check(ph.at_expect("AT+BCS=1") is not None, "the phone picks CVSD this time: AT+BCS=1"); ph.at_send("OK")
        acc = ph.sco_request(air=2)
        check(isinstance(acc, bytes) and acc[16:18] == b"\x60\x00", "accepted as CVSD")
        check(await until(lambda: st.get("phone_call") == "connected"), f"connected ({st.get('phone_call')})")
        check(wait(lambda: "phone: call audio runs (hci, CVSD)" in open(log).read().split("AT+BCC")[-1], 3), "its audio as CVSD")
        ph.at_send("+CIEV: 1,0"); ph.sco_close()
        check(await until(lambda: st.get("phone_call") == "idle"), "ended: idle")
        await cli.disconnect()
    finally:
        proc.terminate(); proc.wait()
    if check.failed: print("     the Echo's log, last lines:\n" + "".join("       " + l for l in open(log, errors="replace").read().splitlines(True)[-50:]))

    # a chip that codes mSBC itself (the PCM models): the accept is Enhanced Accept, 63 bytes through a2dp.c's queue (it
    # was 24 bytes once and dropped it), exactly what the Echos took (docs/re-hfp.md); a link that then fails turns the
    # phone to CVSD (AT+BAC=1)
    state3 = os.path.join(tmp, "state3"); os.makedirs(state3)
    proc, ph, key, log = echo(state3, "bluetooth_calls=on\n", voip, mic, {"HASSMIC_SCO_CHIP_MSBC": "1"})
    try:
        wait(lambda: any(o == 0x0c1a for o, p in list(ph.cmds.queue)), 10)
        check(ph.connect(), "chip mSBC: the phone's link accepted")
        ph.rcid, ph.rdcid, res = ph.open_chan(3)
        ok, what = mux(ph); got = slc(ph, 871)
        check(ok and all(got), f"chip mSBC: service level connection with codecs ({got})")
        ph.at_send("+BCS: 2")                                                    # mSBC picked, then links with no call
        check(ph.at_expect("AT+BCS=2") is not None, "chip mSBC: mSBC picked before a call"); ph.at_send("OK")
        check(ph.sco_request(air=3) == "refused" and ph.sco_request(air=3) == "refused", "chip mSBC: two links with no call refused")
        ph.pump(1)
        check(not any(l.startswith("AT+BAC") for l in ph.at_seen), "chip mSBC: refused links are no mSBC failures")
        ph.at_send("+CIEV: 2,1", "RING"); time.sleep(0.5)
        proc.send_signal(signal.SIGUSR2)
        check(ph.at_expect("ATA") is not None, "chip mSBC: answered")
        ph.at_send("OK", "+CIEV: 1,1", "+CIEV: 2,0", "+BCS: 2")
        check(ph.at_expect("AT+BCS=2") is not None, "chip mSBC: AT+BCS=2"); ph.at_send("OK")
        t_ata = time.time()
        req = lambda: ph.event(0x04, PHONE + bytes([0x0c, 0x02, 0x5a, 2]))
        fail = bytes([0x1f]) + struct.pack("<H", SCO) + PHONE + bytes([2, 0, 0]) + struct.pack("<HH", 0, 0) + bytes([3])
        good = bytes([0]) + struct.pack("<H", SCO) + PHONE + bytes([2, 12, 2]) + struct.pack("<HH", 60, 60) + bytes([3])
        no_bac = lambda: not any(l.startswith("AT+BAC") for l in ph.at_seen)
        req(); p = ph.expect_cmd(0x043e)
        want = (PHONE + struct.pack("<II", 8000, 8000) + bytes([5, 0, 0, 0, 0, 5, 0, 0, 0, 0]) + struct.pack("<HHII", 60, 60, 32000, 32000)
                + bytes([4, 0, 0, 0, 0, 4, 0, 0, 0, 0]) + struct.pack("<HH", 16, 16) + bytes([2, 2, 0, 0, 1, 1, 16, 16]) + struct.pack("<HHB", 13, 0x0380, 2))
        check(p == want, f"chip mSBC: Enhanced Accept, {len(p) if p else 0} bytes, the measured parameters ({p.hex() if p else None})")
        # 1: the answered call's link fails: one failure, and the answer's window once more for the phone's retry
        time.sleep(max(0, t_ata + 3 - time.time())); ph.event(0x2c, fail)    # 3 s in: re-armed, it runs to ~9 s
        time.sleep(max(0, t_ata + 7.5 - time.time())); ph.pump(0.2)
        check("answered here, the phone kept the audio" not in open(log).read() and no_bac(),
              "chip mSBC: a failed link keeps the answer open past its 6 s, no CVSD yet")
        # 2: a good mSBC link starts the count anew
        req(); ph.expect_cmd(0x043e); ph.event(0x2c, good); ph.sco_on = True
        check(wait(lambda: "phone: audio link up (handle 0x020, mSBC)" in open(log).read(), 3), "chip mSBC: then a good mSBC link")
        ph.sco_close(); ph.pump(0.5)
        # a plain SCO link asked for with mSBC agreed: accepted, and carried, as CVSD
        ph.event(0x04, PHONE + bytes([0x0c, 0x02, 0x5a, 0])); p = ph.expect_cmd(0x0429)
        check(p is not None and p[16:18] == b"\x60\x00", "chip mSBC: a plain SCO link is accepted as CVSD")
        ph.event(0x2c, fail[:9] + bytes([0, 0, 0]) + struct.pack("<HH", 0, 0) + bytes([2])); ph.pump(0.5)
        # 3: refused outright and ended by the chip as well: one failure, not two
        ph.refuse = {0x043e}; ph.refuse_completes = True
        req(); ph.expect_cmd(0x043e); ph.pump(1); ph.refuse = set()
        check(wait(lambda: "a2dp: Enhanced Accept refused (0x12)" in open(log).read(), 3), "chip mSBC: Enhanced Accept refused by the chip, told to hfp.c")
        check(no_bac(), "chip mSBC: refused and ended counts once (and the good link reset the count): no CVSD yet")
        # 4: another failure: two in a row, CVSD offered alone
        req(); check(ph.expect_cmd(0x043e) is not None, "chip mSBC: retried as mSBC")
        ph.event(0x2c, fail)
        check(ph.at_expect("AT+BAC=1", 5) == "AT+BAC=1", "chip mSBC: two failures in a row, CVSD offered alone (AT+BAC=1)"); ph.at_send("OK")
        ph.at_send("+BCS: 1")
        check(ph.at_expect("AT+BCS=1") is not None, "chip mSBC: the phone's retry as CVSD"); ph.at_send("OK")
        acc = ph.sco_request(air=2)
        check(isinstance(acc, bytes) and len(acc) == 21 and acc[16:18] == b"\x60\x00", "chip mSBC: then accepted as CVSD")
    finally:
        proc.terminate(); proc.wait()
    if check.failed: print("     the Echo's log, last lines:\n" + "".join("       " + l for l in open(log, errors="replace").read().splitlines(True)[-30:]))

    # the feature off: no record, no RFCOMM, no entities
    state2 = os.path.join(tmp, "state2"); os.makedirs(state2)
    proc, ph, key, log = echo(state2, "bluetooth_calls=off\n", voip, mic)
    try:
        wait(lambda: any(o == 0x0c1a for o, p in list(ph.cmds.queue)), 10)
        check(ph.connect(), "off: the phone's link accepted (A2DP)")
        cid, dcid, res = ph.open_chan(1)
        req = bytes([0x06, 0, 1]) + struct.pack(">H", 15) + bytes([0x35, 3, 0x19, 0x11, 0x1e]) + struct.pack(">H", 0xffff) + bytes([0x35, 5, 0x0a, 0, 0, 0xff, 0xff, 0])
        ph.acl(dcid, req); r = ph.chan_get(cid)
        check(r is not None and bytes([0x19, 0x11, 0x1e]) not in r, "off: no Hands-Free record")
        _, d, res = ph.open_chan(3)
        check(d is None and res == 2, f"off: RFCOMM refused (result {res})")
        cli, by, svcs, st = await ha(PORT, key)
        check("phone_call" not in by, "off: no phone entities in Home Assistant")
        await cli.disconnect()
    finally:
        proc.terminate(); proc.wait()
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    asyncio.run(main())
