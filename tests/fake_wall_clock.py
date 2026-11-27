#!/usr/bin/env python3
"""The Echo Wall Clock, end to end: build/hassmic-host with a2dp.c, rfcomm.c and wallclock.c talking H4 to a controller
played here on a pseudo terminal (HASSMIC_BT_DEV), with a clock behind it as an Echo Show's HCI log and
mpjanofsky/echo-wall-clock's PROTOCOL.md show it: its extended inquiry response while it pulses orange, Secure Simple
Pairing (Just Works), L2CAP, its SDP records, an RFCOMM responder (multiplexer, PN with credits, MSC; channel 4), the
Alexa Gadgets frames and its Discover.Response.  Home Assistant is aioesphomeapi with New York's time zone.  The
feature switched on from the page; "Wall clock search" in Home Assistant pairing the clock in one connection; its SDP
read, Discover byte for byte, then the time in New York time, the light, link supervision and sniff mode as the Echo
sets them; a timer started, paused, resumed, rung and stopped; the clock dropping the link and calling back (its SDP
search finds no Serial Port, as on an Echo; the call taken as master; channel 4 again, the running timer told again);
the feature off (link dropped, calls refused); and a clock that paired with another Echo forgotten."""
import asyncio, base64, datetime, os, queue, struct, sys, threading, time, tty
from zoneinfo import ZoneInfo

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aioesphomeapi import APIClient, VoiceAssistantTimerEventType as Tm, ZERO_NOISE_PSK
from esphome_common import ROOT, check, until, sync, run, assistant

PORTS = (17411, 17412, 17413, 17414, 17415)
CLOCK = bytes.fromhex("aabbccddeeff")             # its address on the air (least significant first): ff:ee:dd:cc:bb:aa
CLOCK_ADDR = int.from_bytes(CLOCK, "little")
HANDLE = 0x0041
NY = ZoneInfo("America/New_York")
GOLDEN_DISCOVER = bytes.fromhex("f0 02 00 01 0a 1d 0a 1b 0a 0f 41 6c 65 78 61 2e 44 69 73 63 6f 76 65 72 79 12 08 44 69 73 63 6f 76 65 72 09 91 f1")


# ---------------------------------------------------------------- RFCOMM's FCS (TS 07.10), written here on its own

def _crc_table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8): c = (c >> 1) ^ 0xe0 if c & 1 else c >> 1
        t.append(c)
    return t
CRC = _crc_table()
def fcs(b):
    f = 0xff
    for x in b: f = CRC[f ^ x]
    return 0xff - f


# ---------------------------------------------------------------- protobuf and the gadget frames, the clock's side

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7f; n >>= 7
        out.append(b | 0x80 if n else b)
        if not n: return bytes(out)
def fld(num, v): v = v.encode() if isinstance(v, str) else v; return varint(num << 3 | 2) + varint(len(v)) + v
def parse(b):
    """a protobuf message's length-delimited fields: {number: [bytes, ...]}"""
    out, i = {}, 0
    while i < len(b):
        k, s = 0, 0
        while True:
            c = b[i]; i += 1; k |= (c & 0x7f) << s; s += 7
            if not c & 0x80: break
        if k & 7 == 0:
            while b[i] & 0x80: i += 1
            i += 1; continue
        n, s = 0, 0
        while True:
            c = b[i]; i += 1; n |= (c & 0x7f) << s; s += 7
            if not c & 0x80: break
        out.setdefault(k >> 3, []).append(b[i:i + n]); i += n
    return out

def agt_frame(seq, payload):
    s = (2 + sum(payload)) & 0xffff; body = bytearray()
    for x in payload + bytes([s >> 8, s & 0xff]): body += bytes([0xf2, x ^ 0xf2]) if x in (0xf0, 0xf1, 0xf2) else bytes([x])
    return bytes([0xf0, 2, 0, seq]) + bytes(body) + b"\xf1"

def discover_response():
    ident = fld(1, "102") + fld(5, "AlexaGadgetsEWC") + fld(2, "A28P3XAXJIFE3L")
    endpoint = fld(1, "TEST000000000000") + fld(2, "EchoWallClk") + fld(3, "AlexaGadgetsEWC") + fld(12, ident)
    header = fld(1, "Alexa.Discovery") + fld(2, "Discover.Response")
    return fld(1, fld(1, header) + fld(2, fld(1, endpoint)))


class Clock:
    """What the clock does on its link: L2CAP channels, an RFCOMM responder on channel 4, the gadget's side"""
    def __init__(self, ctl):
        self.c = ctl; self.reset()
        self.refused = 0                          # how often it refused channel 4
        self.directives = []                      # (namespace, name, payload) in the order they came
        self.frames = []                          # the gadget frames as they came, whole
        self.sdp = []                             # SDP answers to our searches
        self.spp_handles = None

    def reset(self):
        self.cid_seq = 0x60; self.chans = {}      # our CID -> {psm, rcid, in, out}
        self.rf_cid = None; self.mux = False; self.dlc_open = False; self.host_init = True
        self.data_dlci = 8; self.calling = False                        # calling: the link is its call
        self.our_credits = 0; self.host_credits = 0; self.rx = b""; self.buf = bytearray(); self.in_frame = False; self.esc = False
        self.sig_id = 0x40; self.pending_sdp = None; self.seq = 0

    # -- L2CAP
    def l2(self, cid, pdu): self.c.acl(HANDLE, struct.pack("<HH", len(pdu), cid) + pdu)
    def sig(self, code, ident, data): self.l2(1, bytes([code, ident]) + struct.pack("<H", len(data)) + data)

    def on_l2(self, cid, pdu):
        if cid == 1:
            while len(pdu) >= 4:
                code, ident, n = pdu[0], pdu[1], struct.unpack("<H", pdu[2:4])[0]; d = pdu[4:4 + n]; pdu = pdu[4 + n:]
                self.on_sig(code, ident, d)
            return
        ch = next((k for k, v in self.chans.items() if k == cid), None)
        if ch is None: return
        if self.chans[ch]["psm"] == 3: self.on_rfcomm(pdu)
        elif self.chans[ch]["psm"] == 1 and pdu[0] == 0x06: self.sdp_serve(ch, pdu)
        elif self.chans[ch]["psm"] == 1: self.on_sdp(pdu)

    def on_sig(self, code, ident, d):
        if code == 0x02:                                                  # connection request: PSM, source CID
            psm, scid = struct.unpack("<HH", d[:4]); ours = self.cid_seq; self.cid_seq += 1
            self.chans[ours] = {"psm": psm, "rcid": scid, "in": False, "out": False}
            self.sig(0x03, ident, struct.pack("<HHHH", ours, scid, 0, 0))
            self.sig_id += 1; self.sig(0x04, self.sig_id, struct.pack("<HHBBH", scid, 0, 1, 2, 672))
        elif code == 0x03:                                                # the answer to ours
            dcid, scid, res = struct.unpack("<HHH", d[:6])
            if res == 0 and scid in self.chans:
                self.chans[scid]["rcid"] = dcid
                self.sig_id += 1; self.sig(0x04, self.sig_id, struct.pack("<HHBBH", dcid, 0, 1, 2, 672))
        elif code == 0x04:                                                # configuration request to our CID
            dcid = struct.unpack("<H", d[:2])[0]
            if dcid in self.chans:
                ch = self.chans[dcid]; self.sig(0x05, ident, struct.pack("<HHH", ch["rcid"], 0, 0)); ch["in"] = True; self.opened(dcid)
        elif code == 0x05:                                                # configuration response
            scid = struct.unpack("<H", d[:2])[0]
            for k, ch in self.chans.items():
                if k == scid: ch["out"] = True; self.opened(k)
        elif code == 0x06:                                                # disconnection request
            dcid, scid = struct.unpack("<HH", d[:4]); self.sig(0x07, ident, d[:4]); self.chans.pop(dcid, None)
            if dcid == self.rf_cid: self.rf_cid = None; self.mux = self.dlc_open = False
        elif code == 0x0a: self.sig(0x0b, ident, struct.pack("<HH", struct.unpack("<H", d[:2])[0], 1))

    def opened(self, cid):
        ch = self.chans[cid]
        if not (ch["in"] and ch["out"]) or ch.get("up"): return
        ch["up"] = True
        if ch["psm"] == 3: self.rf_cid = cid
        if ch["psm"] == 1 and self.pending_sdp and cid == getattr(self, "sdp_mine", None):
            self.l2(ch["rcid"], self.pending_sdp); self.pending_sdp = None

    def open_sdp(self):                           # the clock calling: SDP for a Serial Port (0x1101)
        ours = self.cid_seq; self.cid_seq += 1; self.sdp_mine = ours
        self.chans[ours] = {"psm": 1, "rcid": None, "in": False, "out": False}
        self.pending_sdp = bytes([0x02, 0, 1]) + struct.pack(">H", 8) + bytes([0x35, 0x03, 0x19, 0x11, 0x01, 0x00, 0x10, 0x00])
        self.sig_id += 1; self.sig(0x02, self.sig_id, struct.pack("<HH", 1, ours))

    def sdp_serve(self, ch, pdu):
        """its records, as Amazon's example has them (the clock points the gadget record at 2), in two pieces"""
        def seq(b): return bytes([0x35, len(b)]) + b
        def u16(v): return bytes([0x19, v >> 8, v & 0xff])
        def aid(v): return bytes([0x09, v >> 8, v & 0xff])
        def rec(handle, cls, chn, name):
            return seq(aid(0) + bytes([0x0a]) + handle.to_bytes(4, "big") + aid(1) + seq(cls) + aid(4) + seq(seq(u16(0x0100)) + seq(u16(3) + bytes([8, chn]))) +
                       aid(0x100) + bytes([0x25, len(name)]) + name)
        gadget = bytes([0x1c]) + bytes.fromhex("6088d2b3983a4eed9f945ad1256816b7")
        allrec = rec(0x10000, u16(0x1201), 4, b"SPP Port 4 DATA") + rec(0x10001, gadget, 2, b"") + rec(0x10002, u16(0x1101), 2, b"SPP Port 2 OTA")
        body = bytes([0x36, len(allrec) >> 8, len(allrec) & 0xff]) + allrec
        self.sdp_queries = getattr(self, "sdp_queries", 0) + 1
        cont = pdu[-1]
        off = 0 if cont == 0 else int.from_bytes(pdu[-2:], "big")
        part = body[off:off + 60]; rest = off + 60 < len(body)
        c = bytes([2]) + (off + 60).to_bytes(2, "big") if rest else b"\0"
        param = len(part).to_bytes(2, "big") + part + c
        self.l2(self.chans[ch]["rcid"], bytes([0x07]) + pdu[1:3] + len(param).to_bytes(2, "big") + param)

    def on_sdp(self, pdu):
        if pdu[0] == 0x03:
            cnt = struct.unpack(">H", pdu[7:9])[0]
            self.spp_handles = [pdu[9 + 4 * i:13 + 4 * i].hex() for i in range(cnt)]
            self.sdp.append(self.spp_handles)

    # -- RFCOMM, the responder (the host started the multiplexer) or the host's peer either way
    def rf(self, dlci, ctrl, info=b"", credits=None, cmd=True):
        cr = (0 if self.host_init else 1) if cmd else (1 if self.host_init else 0)
        if ctrl & 0xef == 0xef: cr = 0 if self.host_init else 1               # UIH: who sends it
        h = bytes([dlci << 2 | cr << 1 | 1, ctrl]) + (bytes([len(info) << 1 | 1]) if len(info) < 128 else bytes([(len(info) & 0x7f) << 1, len(info) >> 7]))
        f = h + (bytes([credits]) if credits is not None else b"") + info
        f += bytes([fcs(h[:2] if ctrl & 0xef == 0xef else h)])
        self.l2(self.chans[self.rf_cid]["rcid"], f)

    def mcc(self, typ, c, v): self.rf(0, 0xef, bytes([typ << 2 | c << 1 | 1, len(v) << 1 | 1]) + v)

    def on_rfcomm(self, p):
        dlci, ctrl, pf = p[0] >> 2, p[1] & 0xef, p[1] & 0x10
        if p[2] & 1: n, h = p[2] >> 1, 3
        else: n, h = p[2] >> 1 | p[3] << 7, 4
        has_cr = ctrl == 0xef and pf and dlci == self.data_dlci and self.dlc_open
        info = p[h + has_cr:h + has_cr + n]
        check.fcs_ok = getattr(check, "fcs_ok", True) and fcs(p[:2] if ctrl == 0xef else p[:h]) == p[-1]
        if ctrl == 0x2f:                                                     # SABM
            if dlci == 0: self.mux = True; self.host_init = True; self.rf(0, 0x73, cmd=False)
            elif dlci == 8: self.dlc_open = True; self.rf(8, 0x73, cmd=False)
            else: self.refused += dlci in (8, 9); self.rf(dlci, 0x1f, cmd=False)
        elif ctrl == 0xef and dlci == 0:                                     # a control message
            typ, c, v = info[0] >> 2, info[0] >> 1 & 1, info[2:2 + (info[1] >> 1)]
            if typ == 0x20 and c:                                            # PN: CFC, 7 credits, its MTU
                self.our_credits = v[7] & 7; self.host_credits = 7; self.pn = v
                self.mcc(0x20, 0, bytes([v[0], 0xe0, v[2], 0, v[4], v[5], 0, 7]))
            elif typ == 0x38 and c:                                          # MSC: answered, then ours
                self.mcc(0x38, 0, v); self.mcc(0x38, 1, bytes([v[0], 0x8d]))
        elif ctrl == 0xef and dlci == self.data_dlci:
            if has_cr: self.our_credits += p[h]
            if info:
                self.host_credits -= 1
                if self.host_credits <= 3: self.rf(8, 0xff, b"", credits=7 - self.host_credits); self.host_credits = 7
                self.on_data(info)
        elif ctrl == 0x43: self.rf(dlci, 0x73, cmd=False); self.dlc_open = self.dlc_open and dlci != 8

    def send_data(self, data):
        if self.our_credits <= 0: return False
        self.our_credits -= 1; self.rf(self.data_dlci, 0xef, data); return True

    # -- the gadget
    def on_data(self, data):
        for b in data:
            if b == 0xf0: self.buf = bytearray(); self.in_frame = True; self.raw = bytearray([b]); continue
            if not self.in_frame: continue
            self.raw.append(b)
            if b == 0xf1:
                self.in_frame = False; self.frames.append(bytes(self.raw)); self.on_frame(bytes(self.buf)); continue
            if b == 0xf2: self.esc = True; continue
            if self.esc: b ^= 0xf2; self.esc = False
            self.buf.append(b)

    def on_frame(self, f):
        payload = f[3:-2]
        check.sums_ok = getattr(check, "sums_ok", True) and (2 + sum(payload)) & 0xffff == f[-2] << 8 | f[-1]
        d = parse(parse(payload)[1][0]); hdr = parse(d[1][0])
        ns, name = hdr[1][0].decode(), hdr[2][0].decode(); pl = d.get(2, [b""])[0]
        self.directives.append((ns, name, pl))
        if (ns, name) == ("Alexa.Discovery", "Discover"): self.send_data(agt_frame(self.seq, discover_response())); self.seq += 1


class Controller:
    """The Echo's Bluetooth chip as hassmic sees it through /dev/stpbt, with the clock in radio range"""
    ASYNC = {0x0401, 0x0405, 0x0406, 0x0409, 0x040a, 0x0411, 0x0413, 0x0419, 0x0803, 0x0804}

    def __init__(self):
        self.m, self.s = os.openpty()
        tty.setraw(self.m); tty.setraw(self.s)
        self.path = os.ttyname(self.s)
        self.lock = threading.Lock(); self.buf = b""; self.rx = b""
        self.clock = Clock(self)
        self.orange = False                        # pairing mode: discoverable
        self.reachable = True                     # within range, batteries in
        self.key = None                           # the link key the clock keeps for its partner
        self.forgot = False                       # it paired with another Echo since
        self.link = False; self.encrypted = False
        self.cmds = []; self.scan = None; self.rejected = 0; self.accepted = 0
        self.q = queue.Queue()
        threading.Thread(target=self.reader, daemon=True).start()
        threading.Thread(target=self.worker, daemon=True).start()

    def send(self, b):
        with self.lock: os.write(self.m, b)
    def event(self, code, params): self.send(bytes([4, code, len(params)]) + params)
    def acl(self, handle, data): self.send(bytes([2]) + struct.pack("<HH", handle | 0x2000, len(data)) + data)

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
                    self.q.put(("cmd", op, par))
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
                        if h == HANDLE and self.link: self.q.put(("l2", cid, self.rx[4:4 + l]))
                        self.rx = b""
                else: self.buf = self.buf[1:]

    def worker(self):                             # one thread plays chip and clock, in order
        while True:
            item = self.q.get()
            try:
                if item[0] == "cmd": self.command(item[1], item[2])
                elif item[0] == "l2": self.clock.on_l2(item[1], item[2])
                elif item[0] == "call": item[1]()
            except Exception as e:
                print(f"FAIL the fake controller: {e!r}"); check.failed = True

    def later(self, fn): self.q.put(("call", fn))

    def command(self, op, par):
        self.cmds.append((op, par))
        if op in self.ASYNC: self.event(0x0f, bytes([0, 1]) + struct.pack("<H", op))      # Command Status
        else:
            ret = bytes(64)
            if op == 0x1005: ret = struct.pack("<HBHH", 1021, 64, 8, 0)    # Read Buffer Size: ACL 1021 x 8
            elif op == 0x2002: ret = struct.pack("<HB", 251, 8)
            elif op == 0x0c1a: self.scan = par[0]
            self.event(0x0e, struct.pack("<BHB", 1, op, 0) + ret)
        if op == 0x0401:                                                  # inquiry: the clock answers while orange
            time.sleep(0.3)                                               # (5 s on a chip; never within the command)
            if self.orange:
                eir = bytes([15, 0x09]) + b"EchoWallClk10U" + bytes([17, 0x06]) + bytes.fromhex("b7166825d15a949fed4e3a98b3d28860") + \
                      bytes([11, 0xff]) + bytes.fromhex("49 19 42 08 71 01 10 15 15 fe")
                eir = eir.ljust(240, b"\0")
                self.event(0x2f, bytes([1]) + CLOCK + bytes([1, 0]) + bytes([0x00, 0x1f, 0x00]) + struct.pack("<H", 0x1234) + bytes([0xc4]) + eir)
            self.event(0x01, bytes([0]))
        elif op == 0x0405:                                                # page
            time.sleep(0.1)
            if par[:6] == CLOCK and self.reachable and (self.orange or self.key) and not self.link:
                self.link = True; self.encrypted = False; self.clock.reset()
                self.event(0x03, bytes([0]) + struct.pack("<H", HANDLE) + CLOCK + bytes([1, 0]))
            else: self.event(0x03, bytes([0x04]) + struct.pack("<H", HANDLE) + par[:6] + bytes([1, 0]))
        elif op == 0x0419:
            self.event(0x07, bytes([0]) + par[:6] + b"EchoWallClk10U".ljust(248, b"\0"))
        elif op == 0x0411:                                                # authentication: the stored key, or pairing
            self.event(0x17, CLOCK)                                       # Link Key Request; the answer decides
        elif op == 0x040b:                                                # Link Key Request Reply
            ok = self.key is not None and par[6:22] == self.key and not self.forgot
            self.event(0x06, bytes([0 if ok else 0x06]) + struct.pack("<H", HANDLE))
        elif op == 0x040c:                                                # no key: pair if orange
            if self.orange: self.event(0x31, CLOCK)                        # IO Capability Request
            else: self.event(0x06, bytes([0x05]) + struct.pack("<H", HANDLE))
        elif op == 0x042b:                                                # our IO capability: the clock's, confirm
            check(par[6] == 0x03, "pairing: the Echo says NoInputNoOutput (Just Works)")
            self.event(0x32, CLOCK + bytes([0x03, 0, 0x04]))
            self.event(0x33, CLOCK + struct.pack("<I", 123456))
        elif op == 0x042c:                                                # confirmed: paired
            self.key = os.urandom(16); self.forgot = False
            self.event(0x36, bytes([0]) + CLOCK)
            self.event(0x18, CLOCK + self.key + bytes([4]))
            self.event(0x06, bytes([0]) + struct.pack("<H", HANDLE))
        elif op in (0x042d, 0x0434): self.event(0x06, bytes([0x05]) + struct.pack("<H", HANDLE))
        elif op == 0x0413:
            self.encrypted = True; self.event(0x08, bytes([0]) + struct.pack("<H", HANDLE) + bytes([1]))
        elif op == 0x0406: self.drop(par[2])
        elif op == 0x0409:                                                # the clock's call accepted
            self.accepted += 1; self.link = True; self.encrypted = False; self.clock.reset(); self.clock.calling = True
            self.event(0x03, bytes([0]) + struct.pack("<H", HANDLE) + CLOCK + bytes([1, 0]))
            self.later(self.clock.open_sdp)
        elif op == 0x040a: self.rejected += 1
        elif op == 0x0803 and self.link: self.event(0x14, bytes([0]) + struct.pack("<HBH", HANDLE, 2, 0x0320))        # sniff, 500 ms
        elif op == 0x0804 and self.link: self.event(0x14, bytes([0]) + struct.pack("<HBH", HANDLE, 0, 0))

    def drop(self, why=0x13):
        if not self.link: return
        self.link = False; self.encrypted = False
        self.event(0x05, bytes([0]) + struct.pack("<H", HANDLE) + bytes([why]))

    def call(self):                                # the clock calls its partner (blue flash)
        self.event(0x04, CLOCK + bytes([0x00, 0x1f, 0x00, 1]))


def ops(ctl, op): return [p for o, p in ctl.cmds if o == op]


async def main(e):
    ctl = e.ctl; clk = ctl.clock
    # Home Assistant with a key (the time zone comes only over the keyed link), New York's zone
    prov = APIClient("127.0.0.1", e.PORT, None, noise_psk=ZERO_NOISE_PSK)
    await asyncio.wait_for(prov.connect(), 5)
    key = base64.b64encode(os.urandom(32))
    await asyncio.wait_for(prov.noise_encryption_set_key(key), 5); await prov.disconnect()

    async def ha():
        c = APIClient("127.0.0.1", e.PORT, None, noise_psk=key.decode(), timezone="America/New_York")
        await asyncio.wait_for(c.connect(login=True), 5)
        ents, _ = await c.list_entities_services()
        states = {}
        c.subscribe_states(lambda s: states.__setitem__(s.key, s))
        assistant(c)
        return c, {x.name: x for x in ents}, states

    await until(lambda: ops(ctl, 0x1005), 8)
    cli, ents, states = await ha()
    check("Wall clock search" not in ents and "Wall clock" not in ents, "the feature off: no wall clock entities")
    check(e.pv("wall_clock") == 0, "the page: Echo Wall Clock, off by default")

    r = e.page.set(wall_clock="on")
    check(r.get("status", 200) == 200, f"the page switches the feature on: {r}")
    await asyncio.sleep(1)
    try: await cli.disconnect()
    except Exception: pass
    cli, ents, states = await ha()
    search, status = ents.get("Wall clock search"), ents.get("Wall clock")
    check(search is not None and status is not None, f"on: \"Wall clock search\" and \"Wall clock\" in Home Assistant: {sorted(ents)[:0]}")
    st = lambda: getattr(states.get(status.key), "state", None)
    await until(lambda: st() == "None", 3)
    check(st() == "None", f"no clock paired: \"None\" ({st()!r})")

    # ---- the clock pulses orange; the search, started from the settings page (Home Assistant not needed), pairs it in
    # one connection and opens channel 4
    pw = lambda: e.page.state()["wall_clock"]
    check(pw() == {"present": True, "status": "None", "searching": False, "paired": False}, f"the page: no clock yet ({pw()})")
    ctl.orange = True
    st_, _, _ = e.page.call("POST", "/api/wallclock", b"search")
    check(st_ == 200, f"the page starts the search ({st_})")
    await until(lambda: pw()["searching"] or ops(ctl, 0x0405), 3)
    check(ops(ctl, 0x0401) or pw()["searching"], "searching")
    await until(lambda: len(clk.directives) >= 3, 25)
    check(ops(ctl, 0x0401), "search: an inquiry")
    pg = ops(ctl, 0x0405)
    check(pg and pg[0][:6] == CLOCK and struct.unpack("<H", pg[0][10:12])[0] == 0x1234 | 0x8000, "the clock found by its gadget UUID and paged, with its clock offset")
    check(ctl.key is not None and len(ops(ctl, 0x042c)) == 1, "paired: Just Works confirmed once")
    keys = open(os.path.join(e.state, "bt_keys")).read()
    check(f"{CLOCK_ADDR:012x}" in keys, "its link key in state/bt_keys")
    check(open(os.path.join(e.state, "bt_clock")).read() == f"{CLOCK_ADDR:012x} EchoWallClk10U\n", "state/bt_clock: its address and name")
    check(ops(ctl, 0x0413), "the link encrypted before RFCOMM")
    sv = ops(ctl, 0x0c37)
    check(sv and sv[0][2:4] == bytes([0x00, 0x32]), "link supervision 8 s, as an Echo sets it")
    check(getattr(clk, "sdp_queries", 0) >= 2, f"the Echo read the clock's SDP records first (in {getattr(clk, 'sdp_queries', 0)} pieces)")
    check("its data on channel 4" in e.log(), "its data service found on channel 4 (Generic Networking), not the gadget record's 2")
    check(clk.pn[0] == 8 and clk.pn[1] == 0xf0, f"RFCOMM: PN for DLCI 8 (channel 4), credits asked: {clk.pn.hex()}")
    check(clk.frames and clk.frames[0] == GOLDEN_DISCOVER, "Discover, byte for byte the Echo Dot's")
    names = [(n, x) for n, x, _ in clk.directives]
    check(names[:3] == [("Alexa.Discovery", "Discover"), ("Alexa.Gadget.StateListener", "StateUpdate"), ("Notifications", "ClearIndicator")],
          f"then the time and the light off, in the Echo's order: {names[:3]}")
    s = parse(parse(clk.directives[1][2])[1][0]); tname, tval = s[1][0].decode(), s[2][0].decode()
    t = datetime.datetime.fromisoformat(tval); now = datetime.datetime.now(NY)
    check(tname == "timeinfo" and abs((t - now).total_seconds()) < 5 and t.utcoffset() == now.utcoffset(),
          f"timeinfo in New York time, from Home Assistant's zone: {tval}")
    await until(lambda: st() == "EchoWallClk10U: Connected", 3)
    check(st() == "EchoWallClk10U: Connected", f"\"Wall clock\": connected ({st()!r})")
    check(getattr(states.get(search.key), "state", None) is False, "the search switch off in Home Assistant")
    check(pw() == {"present": True, "status": "EchoWallClk10U: Connected", "searching": False, "paired": True}, f"the page: connected ({pw()})")
    check(getattr(check, "fcs_ok", False) and getattr(check, "sums_ok", False), "every RFCOMM FCS and gadget checksum right")
    ctl.orange = False

    # ---- quiet: sniff mode after 5 s, the Echo's parameters
    await until(lambda: ops(ctl, 0x0803), 8)
    sn = ops(ctl, 0x0803)
    check(sn and sn[0][2:] == bytes.fromhex("1a04200304000100"), f"5 s quiet: sniff mode, 500-656 ms ({sn[0].hex() if sn else None})")

    # ---- a timer: started, paused, resumed, rung, stopped
    n = len(clk.directives)
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_STARTED, "01JTEA", "tea", 300, 300, True)
    await until(lambda: len(clk.directives) > n, 3)
    d = clk.directives[n:]
    check(len(d) == 1 and d[0][:2] == ("Alerts", "SetAlert"), f"timer started: SetAlert {[x[:2] for x in d]}")
    check(ops(ctl, 0x0804), "out of sniff mode for it")
    a = parse(d[0][2]); token, typ, end = a[1][0].decode(), a[2][0].decode(), datetime.datetime.fromisoformat(a[3][0].decode())
    want = datetime.datetime.now(NY) + datetime.timedelta(seconds=300)
    check(typ == "TIMER" and len(token) == 10 and token.isdigit() and abs((end - want).total_seconds()) < 3 and end.utcoffset() == want.utcoffset(),
          f"TIMER, a ten digit token, its end in New York time: {token} {end.isoformat()}")
    n = len(clk.directives)
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_UPDATED, "01JTEA", "tea", 300, 280, False)
    await until(lambda: len(clk.directives) > n, 3)
    d = clk.directives[n:]
    check([x[:2] for x in d] == [("Alerts", "DeleteAlert")] and parse(d[0][2])[1][0].decode() == token, "paused: DeleteAlert, its token")
    n = len(clk.directives)
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_UPDATED, "01JTEA", "tea", 300, 280, True)
    await until(lambda: len(clk.directives) > n, 3)
    d = clk.directives[n:]
    check([x[:2] for x in d] == [("Alerts", "SetAlert")] and parse(d[0][2])[1][0].decode() == token, "resumed: SetAlert, the same token")
    n = len(clk.directives)
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "01JTEA", "tea", 300, 0, False)
    await until(lambda: len(clk.directives) > n, 3)
    d = clk.directives[n:]
    check([x[:2] for x in d] == [("Alexa.Gadget.StateListener", "StateUpdate")] and parse(parse(d[0][2])[1][0])[2][0] == b"active", "it rings: timers = active, once")
    n = len(clk.directives)
    cli.button_command(ents["Stop ringing"].key)
    await until(lambda: len(clk.directives) >= n + 2, 3)
    d = clk.directives[n:]
    check([x[:2] for x in d] == [("Alerts", "DeleteAlert"), ("Alexa.Gadget.StateListener", "StateUpdate")] and parse(parse(d[1][2])[1][0])[2][0] == b"cleared",
          f"stopped: DeleteAlert and timers = cleared {[x[:2] for x in d]}")

    # ---- the clock drops the link and calls back: SDP for a Serial Port, our channel 4 on its link, the timer again
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_STARTED, "01JEGG", "egg", 420, 420, True)
    await until(lambda: clk.directives[-1][1] == "SetAlert", 3)
    ctl.later(lambda: ctl.drop(0x13))
    await until(lambda: st() == "EchoWallClk10U: Not reachable" or st() == "EchoWallClk10U: Connecting", 3)
    check(st() in ("EchoWallClk10U: Not reachable", "EchoWallClk10U: Connecting"), f"the link gone: {st()!r}")
    n = len(clk.directives); clk.frames.clear(); clk.refused = 0
    ctl.later(ctl.call)
    await until(lambda: any(x[1] == "SetAlert" for x in clk.directives[n:]), 20)
    check(ctl.accepted == 1, "its call accepted")
    check(clk.sdp and len(clk.sdp[-1]) == 0, f"its SDP search: no Serial Port here, as an Echo answers ({clk.sdp})")
    acc = ops(ctl, 0x0409)
    check(acc and acc[-1][6] == 0x00, "its call taken as master, as an Echo takes it")
    check(clk.data_dlci == 8 and clk.host_init and clk.refused == 0, "channel 4 in a session of the Echo's: DLCI 8")
    check(clk.frames and clk.frames[0] == GOLDEN_DISCOVER, "Discover again on the new link, seq 1")
    d = [x[:2] for x in clk.directives[n:]]
    check(d[:4] == [("Alexa.Discovery", "Discover"), ("Alexa.Gadget.StateListener", "StateUpdate"), ("Notifications", "ClearIndicator"), ("Alerts", "SetAlert")],
          f"the time, the light, the egg timer told again: {d}")
    await until(lambda: st() == "EchoWallClk10U: Connected", 3)

    # ---- the feature off: the link goes, its calls are refused; on again: paged and back
    e.page.set(wall_clock="off")
    await until(lambda: not ctl.link, 5)
    check(not ctl.link and ops(ctl, 0x0406), "off: the Echo drops the clock's link")
    ctl.later(ctl.call)
    await until(lambda: ctl.rejected, 3)
    check(ctl.rejected == 1 and ctl.accepted == 1, "off: its call refused")
    check(open(os.path.join(e.state, "bt_clock")).read().startswith(f"{CLOCK_ADDR:012x}"), "off: still paired")
    pages = len(ops(ctl, 0x0405)); n = len(clk.directives)
    e.page.set(wall_clock="on")
    await until(lambda: any(x[1] == "SetAlert" for x in clk.directives[n:]), 10)
    check(len(ops(ctl, 0x0405)) > pages and ctl.link, "on again: paged at once, connected")

    # ---- it paired with another Echo since: the key is refused, the clock forgotten (the feature's switch closes
    # Home Assistant's links, as every feature's does: connected again after)
    ctl.forgot = True; ctl.later(lambda: ctl.drop(0x13))
    e.page.set(wall_clock="off"); await asyncio.sleep(0.5); e.page.set(wall_clock="on")
    await until(lambda: not os.path.exists(os.path.join(e.state, "bt_clock")), 15)
    check(not os.path.exists(os.path.join(e.state, "bt_clock")), "the clock refused our key: forgotten")
    try: await cli.disconnect()
    except Exception: pass
    cli, ents, states = await ha()
    status = ents["Wall clock"]
    await until(lambda: getattr(states.get(status.key), "state", None) == "None", 3)
    check(getattr(states.get(status.key), "state", None) == "None", "\"Wall clock\": None")
    pages = len(ops(ctl, 0x0405)); await asyncio.sleep(2)
    check(len(ops(ctl, 0x0405)) == pages, "and not paged any more")

    # ---- paired again, from Home Assistant's switch this time; then "Forget" on the page
    ctl.forgot = False; ctl.key = None; ctl.orange = True
    cli.switch_command(status.key - 1, True)                                       # "Wall clock search" (its key is just before)
    await until(lambda: pw()["status"] == "EchoWallClk10U: Connected", 25)
    check(pw()["paired"] and ctl.link, f"Home Assistant's search pairs it too ({pw()})")
    ctl.orange = False
    st_, _, _ = e.page.call("POST", "/api/wallclock", b"forget")
    await until(lambda: not ctl.link and not os.path.exists(os.path.join(e.state, "bt_clock")), 5)
    check(st_ == 200 and not ctl.link and not os.path.exists(os.path.join(e.state, "bt_clock")), "the page's Forget: link dropped, pairing gone")
    check(f"{CLOCK_ADDR:012x}" not in open(os.path.join(e.state, "bt_keys")).read() if os.path.exists(os.path.join(e.state, "bt_keys")) else True, "and its key")
    await until(lambda: pw()["status"] == "None", 3)
    check(pw() == {"present": True, "status": "None", "searching": False, "paired": False}, f"the page: none again ({pw()})")
    e.page.set(wall_clock="off")
    st_, _, _ = e.page.call("POST", "/api/wallclock", b"search")
    check(st_ == 409, f"the feature off: the page's search refused ({st_})")
    try: await cli.disconnect()
    except Exception: pass


if __name__ == "__main__":
    ctl = Controller()
    async def go(e):
        e.ctl = ctl
        await main(e)
    run(go, PORTS, {"HASSMIC_BT_DEV": ctl.path}, log=True)
