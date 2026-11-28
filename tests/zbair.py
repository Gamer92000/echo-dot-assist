#!/usr/bin/env python3
"""A shared 802.15.4 medium for the PC's Zigbee tests: one OpenThread RCP per node, as Amazon's ot-rcp answers on the
Echo 4 (Spinel 4.3 in HDLC-lite, RCP API 11: tests/fake_rcp.py has the codec), each on a unix socket (zigbeed's -R) or a
TCP port (zigbee-herdsman's zoh adapter).  What one sends on a channel reaches every node that hears it (LINKS: who
hears whom, at what LQI), through the RCP's address filter (PAN ID, short and extended address; everything while
promiscuous); a unicast that a node takes is acked (frame pending where the sender is in its source match table), one
nobody takes fails as NO_ACK.  Energy scans answer quiet channels.  Every frame goes into a pcap (802.15.4 with FCS)
that tshark reads with the network key.

    air = Air(pcap); air.node("C", eui=0x00124b0000000001, sock=path)  /  air.node("Z", eui=..., tcp=port)
    air.link("C", "R", lqi=200)                                         # both ways
    await air.start()
"""
import asyncio, os, struct, sys, time
from fake_rcp import hdlc, frames, fcs

NCP = b"OPENTHREAD/404cfe2ff ;GP712 ; zbair\x00"


def pui(v):
    o = bytearray()
    while True:
        b = v & 0x7F; v >>= 7
        o.append(b | (0x80 if v else 0))
        if not v: return bytes(o)


def rpui(b, i):
    v = s = 0
    while True:
        x = b[i]; i += 1; v |= (x & 0x7F) << s; s += 7
        if not x & 0x80: return v, i


class Node:
    def __init__(self, air, name, eui):
        self.air, self.name, self.eui = air, name, eui
        self.channel, self.pan, self.short, self.ext = 11, 0xFFFF, 0xFFFE, eui
        self.promisc = False; self.enabled = False; self.raw = False
        self.src_short, self.src_ext = set(), set()
        self.writer = None
        self.sent = 0; self.got = 0

    def send(self, payload):
        if self.writer is not None and not self.writer.is_closing():
            self.writer.write(hdlc(payload))

    # the RCP's address filter
    def takes(self, f):
        if not self.enabled: return False
        if self.promisc: return True
        if len(f) < 5: return False
        fcf = f[0] | f[1] << 8; t = fcf & 7; comp = fcf >> 6 & 1; dm = fcf >> 10 & 3; sm = fcf >> 14 & 3
        k = 3
        if t == 0:                                          # a beacon: by its source PAN, or ours unset
            span = f[k] | f[k + 1] << 8
            return self.pan == 0xFFFF or span == self.pan
        if dm:
            dpan = f[k] | f[k + 1] << 8; k += 2
            if dpan not in (0xFFFF, self.pan): return False
            if dm == 2:
                d = f[k] | f[k + 1] << 8
                return d in (0xFFFF, self.short)
            if dm == 3: return int.from_bytes(f[k:k + 8], "little") == self.ext
        if sm and not comp:
            span = f[k] | f[k + 1] << 8
            return self.pan == span
        return False

    def ack_pending(self, f):
        """the frame is a data request whose source is in our source match table"""
        fcf = f[0] | f[1] << 8; dm = fcf >> 10 & 3; sm = fcf >> 14 & 3; comp = fcf >> 6 & 1
        k = 3
        if dm: k += 2 + (2 if dm == 2 else 8)
        if sm and not comp: k += 2
        src = f[k:k + (2 if sm == 2 else 8)]
        k += len(src)
        if (fcf & 7) != 3 or len(f) <= k or f[k] != 0x04: return False
        v = int.from_bytes(src, "little")
        return v in self.src_short if sm == 2 else v in self.src_ext

    def rx(self, psdu, lqi):
        self.got += 1
        rssi = -100 + lqi * 70 // 255
        md = bytes([self.channel, lqi]) + struct.pack("<Q", int(time.monotonic() * 1e6))
        pl = bytes([0x80, 6]) + pui(0x71) + struct.pack("<H", len(psdu)) + psdu + struct.pack("<bbH", rssi, -100, 0) \
            + struct.pack("<H", len(md)) + md + struct.pack("<H", 1) + b"\0"
        self.send(pl)

    async def handle(self, reader, writer):
        if self.writer is not None:
            try: self.writer.close()
            except Exception: pass
        self.writer = writer
        self.enabled = False; self.raw = False; self.src_short.clear(); self.src_ext.clear()
        buf = bytearray()
        try:
            while True:
                d = await reader.read(4096)
                if not d: break
                buf += d
                for f in frames(buf): self.command(f)
        except (ConnectionError, OSError):
            pass
        if self.writer is writer: self.writer = None

    def reply(self, tid, cmd, prop, data=b""):
        self.send(bytes([0x80 | tid]) + pui(cmd) + pui(prop) + data)

    def command(self, f):
        tid = f[0] & 0x0F
        cmd, i = rpui(f, 1)
        if cmd == 1:                                        # reset
            self.enabled = False; self.raw = False; self.promisc = False
            self.src_short.clear(); self.src_ext.clear()
            self.reply(0, 6, 0, pui(114)); return
        prop, i = rpui(f, i); d = f[i:]
        if cmd == 2:
            val = {1: b"\x04\x03", 2: NCP, 3: b"\x03", 5: pui(5) + pui(12) + pui(34) + pui(64) + pui(65),
                   8: self.eui.to_bytes(8, "big"), 0xB0: pui(11), 0xB1: pui(4), 0x120B: b"\x1f",
                   0x21: bytes([self.channel]), 0x22: bytes(range(11, 26)),
                   0x24: bytes([(-75) & 0xFF]), 0x25: bytes([5]), 0x26: bytes([(-100) & 0xFF]), 0x27: bytes([(-100) & 0xFF])}.get(prop)
            if val is None:
                print(f"zbair: {self.name}: property {prop:#x} asked, not here", file=sys.stderr)
                self.reply(tid, 6, 0, pui(7))                   # PROP_NOT_FOUND
            else: self.reply(tid, 6, prop, val)
            return
        if cmd == 3 and prop == 0x71:
            self.transmit(tid, d); return
        if cmd == 3:
            if prop == 0x20: self.enabled = bool(d[0])
            elif prop == 0x21: self.channel = d[0]
            elif prop == 0x36: self.pan = d[0] | d[1] << 8
            elif prop == 0x35: self.short = d[0] | d[1] << 8
            elif prop == 0x34: self.ext = int.from_bytes(d[:8], "big")          # Spinel's EUI-64s: big-endian
            elif prop == 0x37: self.raw = bool(d[0])
            elif prop == 0x38: self.promisc = bool(d[0])
            elif prop == 0x1304 and not d: self.src_short.clear()
            elif prop == 0x1305 and not d: self.src_ext.clear()
            elif prop == 0x31: self.scan_mask = list(d)
            elif prop == 0x30 and d[:1] == b"\x02":
                if len(getattr(self, "scan_mask", [])) != 1:       # as Amazon's RCP: one channel a scan
                    self.reply(tid, 6, 0, pui(3)); return
                self.reply(tid, 6, prop, d)
                asyncio.get_running_loop().call_later(0.05, self.energy_scan)
                return
            self.reply(tid, 6, prop, d); return
        if cmd in (4, 5):
            if prop == 0x1304: (self.src_short.add if cmd == 4 else self.src_short.discard)(d[0] | d[1] << 8)
            elif prop == 0x1305: (self.src_ext.add if cmd == 4 else self.src_ext.discard)(int.from_bytes(d[:8], "big"))
            self.reply(tid, 7 if cmd == 4 else 8, prop, d); return
        self.reply(tid, 6, 0, pui(0))

    def energy_scan(self):
        for ch in self.scan_mask:
            self.reply(0, 6, 0x39, bytes([ch, (-95 + ch) & 0xFF]))
        self.reply(0, 6, 0x30, b"\x00")

    def transmit(self, tid, d):
        n = d[0] | d[1] << 8
        psdu = bytes(d[2:2 + n])
        ch = d[2 + n] if len(d) > 2 + n else self.channel
        self.sent += 1
        self.air.capture(psdu)
        fcf = psdu[0] | psdu[1] << 8
        ackreq = fcf >> 5 & 1
        taken = []
        for other, lqi in self.air.heard_by(self):
            if other.channel == ch and other.takes(psdu):
                other.rx(psdu, lqi); taken.append(other)
        if not ackreq: st, fp = 0, False
        else:
            dm = fcf >> 10 & 3
            acker = [o for o in taken if not o.promisc or o.short != 0xFFFE]
            if dm and acker: st, fp = 0, any(o.ack_pending(psdu) for o in acker)
            else: st, fp = 17, False
        out = pui(st) + bytes([1 if fp else 0, 0])
        if st == 0:
            ack = bytes([0x12 if fp else 0x02, 0, psdu[2], 0, 0])
            md = bytes([ch, 255]) + struct.pack("<Q", 0)
            out += struct.pack("<H", len(ack)) + ack + struct.pack("<bbH", -40, -100, 0) + struct.pack("<H", len(md)) + md + struct.pack("<H", 1) + b"\0"
        asyncio.get_running_loop().call_later(0.002, lambda: self.reply(tid, 6, 0, out))


class Air:
    def __init__(self, pcap=None):
        self.nodes = {}; self.links = {}; self.servers = []; self.pcap = None
        if pcap:
            self.pcap = open(pcap, "wb")
            self.pcap.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 195))
        self.places = {}

    def node(self, name, eui, sock=None, tcp=None):
        n = Node(self, name, eui); self.nodes[name] = n; self.places[name] = (sock, tcp); return n

    def link(self, a, b, lqi=220):
        self.links[(a, b)] = lqi; self.links[(b, a)] = lqi

    def cut(self, a, b):
        self.links.pop((a, b), None); self.links.pop((b, a), None)

    def heard_by(self, src):
        for (a, b), lqi in list(self.links.items()):
            if a == src.name: yield self.nodes[b], lqi

    def inject(self, name, psdu, channel):
        """a frame sent by a node that has no RCP of its own (a Green Power device, a stranger): psdu with its FCS
        bytes, to everyone in range on that channel through their address filters"""
        n = self.nodes[name]
        self.capture(psdu)
        for other, lqi in self.heard_by(n):
            if other.channel == channel and other.takes(psdu): other.rx(psdu, lqi)

    def scan_mask(self, node):
        return range(11, 26)

    def capture(self, psdu):
        if not self.pcap: return
        f = bytearray(psdu)
        if len(f) >= 2:
            c = fcs(bytes(f[:-2])); f[-2] = c & 0xFF; f[-1] = c >> 8
        t = time.time()
        self.pcap.write(struct.pack("<IIII", int(t), int(t % 1 * 1e6), len(f), len(f)) + bytes(f))
        self.pcap.flush()

    async def start(self):
        for name, (sock, tcp) in self.places.items():
            n = self.nodes[name]
            if sock:
                if os.path.exists(sock): os.unlink(sock)
                self.servers.append(await asyncio.start_unix_server(n.handle, path=sock))
            if tcp:
                self.servers.append(await asyncio.start_server(n.handle, "127.0.0.1", tcp))

    async def stop(self):
        for s in self.servers: s.close()
        for n in self.nodes.values():
            if n.writer: n.writer.close()
        if self.pcap: self.pcap.close()
