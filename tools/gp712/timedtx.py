#!/usr/bin/env python3
"""Timed transmit through gprcp as a Thread host asks for it (PLAN.md "Thread 1.4" 2b.2): Spinel's STREAM_RAW with
TxDelayBaseTime + TxDelay, the RCP's own clock from RCP_TIMESTAMP, and the chip's timestamp of each frame from our
module (/proc/hassmic_gptime's TX, turned into the host's clock by its pair) against the target.  The frames: a beacon
request on channel 26, nobody's.

On the Echo 4 (adb, root), the radio off (radio=off), hassmic_gptime loaded, build/laser/gprcp and rcpbridge pushed to
/data/local/tmp:
    tools/gp712/timedtx.py [frames] [ahead ms]
    tools/gp712/timedtx.py scan          the energy scan instead, channel by channel (Spinel's SCAN_STATE ENERGY)
It starts rcpbridge (build/laser/rcpbridge, pushed there too) over adb with that gprcp (a socket in /data/local/tmp, adb forward to it) and ends
it after."""
import socket
import struct
import subprocess
import sys
import time

SCAN, FRAMES, AHEAD_US = False, 20, 30000      # main()'s; the arguments set them (tools/gp712/twoway.py imports this)
SOCK = "/data/local/tmp/gprcp-test.sock"
PORT = 29931


def adb(*a, **kw):
    return subprocess.run(["adb", "shell", *a], capture_output=True, text=True, **kw).stdout


# --- HDLC-lite and Spinel -------------------------------------------------------------------------------------------
def fcs16(b):
    f = 0xFFFF
    for c in b:
        f ^= c
        for _ in range(8):
            f = (f >> 1) ^ 0x8408 if f & 1 else f >> 1
    return f ^ 0xFFFF


def hdlc(b):
    b = b + struct.pack("<H", fcs16(b))
    o = bytearray([0x7E])
    for c in b:
        if c in (0x7E, 0x7D, 0x11, 0x13, 0xF8):
            o += bytes([0x7D, c ^ 0x20])
        else:
            o.append(c)
    return bytes(o + b"\x7e")


def uint(v):          # Spinel's packed unsigned (EXI)
    o = bytearray()
    while True:
        c = v & 0x7F
        v >>= 7
        o.append(c | (0x80 if v else 0))
        if not v:
            return bytes(o)


def unpack_uint(b):
    v = s = i = 0
    while True:
        v |= (b[i] & 0x7F) << s
        s += 7
        i += 1
        if not b[i - 1] & 0x80:
            return v, b[i:]


CMD_RESET, GET, SET, IS = 1, 2, 3, 6
P_STATUS, P_PHY_ENABLED, P_CHAN, P_RAW, P_STREAM_RAW = 0, 0x20, 0x21, 0x37, 0x71
P_CAPS, P_TIMESTAMP = 0x120B, 0x802
P_SCAN_STATE, P_SCAN_MASK, P_SCAN_PERIOD, P_ED_RESULT = 0x30, 0x31, 0x32, 0x39


class Rcp:
    def __init__(self, s):
        self.s, self.buf, self.tid = s, bytearray(), 0

    def send(self, cmd, prop=None, val=b""):
        self.tid = self.tid % 15 + 1
        b = bytes([0x80 | self.tid]) + uint(cmd) + (uint(prop) + val if prop is not None else val)
        self.s.sendall(hdlc(b))
        return self.tid

    def frames(self, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            while b"\x7e" in self.buf:
                i = self.buf.index(b"\x7e")
                raw, self.buf = bytes(self.buf[:i]), self.buf[i + 1:]
                if len(raw) < 3:
                    continue
                o, esc = bytearray(), False
                for c in raw:
                    if esc:
                        o.append(c ^ 0x20); esc = False
                    elif c == 0x7D:
                        esc = True
                    else:
                        o.append(c)
                if fcs16(o[:-2]) != struct.unpack("<H", o[-2:])[0]:
                    continue
                o = bytes(o[:-2])
                cmd, rest = unpack_uint(o[1:])
                prop, val = unpack_uint(rest) if rest else (None, b"")
                yield o[0] & 0x0F, cmd, prop, val
            self.s.settimeout(max(0.01, end - time.monotonic()))
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                continue
            if not d:
                raise SystemExit("the RCP went away")
            self.buf += d

    def ask(self, cmd, prop=None, val=b"", timeout=2.0, want=None):
        t = self.send(cmd, prop, val)
        for tid, c, p, v in self.frames(timeout):
            if tid == t and c == IS and (want is None or p == want):
                return p, v
        raise SystemExit(f"no answer to {cmd} {prop}")


def gptime():
    d = {}
    for l in adb("cat /proc/hassmic_gptime").splitlines():
        k, _, v = l.partition(" = ")
        d[k] = v
    return d


def main():
    adb(f"rm -f {SOCK}; (/data/local/tmp/rcpbridge {SOCK} /data/local/tmp/gprcp > /data/local/tmp/gprcp-test.log 2>&1 &)")
    time.sleep(0.5)
    subprocess.run(["adb", "forward", f"tcp:{PORT}", f"localfilesystem:{SOCK}"], check=True, capture_output=True)
    try:
        s = socket.create_connection(("127.0.0.1", PORT))
        r = Rcp(s)
        r.send(CMD_RESET, None, b"\x02")
        for _, c, p, v in r.frames(3):
            if c == IS and p == P_STATUS:
                break
        _, caps = r.ask(GET, P_CAPS)
        caps, _ = unpack_uint(caps)
        print(f"radio caps {caps:#x}: transmit timing {'on the chip' if caps & 0x40 else 'in software'}")
        r.ask(SET, P_PHY_ENABLED, b"\x01")
        r.ask(SET, P_CHAN, b"\x1a")
        r.ask(SET, P_RAW, b"\x01")
        if SCAN:
            r.ask(SET, P_SCAN_PERIOD, struct.pack("<H", 100))
            for ch in range(11, 27):
                r.ask(SET, P_SCAN_MASK, bytes([ch]))
                r.send(SET, P_SCAN_STATE, b"\x02")
                got = None
                for _, c, p, v in r.frames(2):
                    if c == IS and p == P_ED_RESULT:
                        got = struct.unpack("<Bb", v[:2])
                        break
                print(f"channel {ch}: {got[1] if got else 'no result'} dBm")
            return
        errs = []
        for i in range(FRAMES + 1):
            psdu = bytes([0x03, 0x08, i & 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0, 0])
            _, ts = r.ask(GET, P_TIMESTAMP)
            now = struct.unpack("<Q", ts[:8])[0]
            delay = 0 if i == 0 else AHEAD_US        # the first at once: its confirm is gprcp's first pair
            base = now & 0xFFFFFFFF if delay else 0
            val = (struct.pack("<H", len(psdu)) + psdu + bytes([26, 0, 0, 0, 0, 0, 0])
                   + struct.pack("<II", delay, base) + bytes([26, 0x7F]))
            t = r.send(SET, P_STREAM_RAW, val)
            st = None
            for tid, c, p, v in r.frames(2 + AHEAD_US / 1e6):
                if tid == t and c == IS and p == P_STATUS:
                    st, _ = unpack_uint(v)
                    break
            if not delay:
                continue
            g = gptime()
            tx_host = (int(g["HOST"]) + ((int(g["TX"]) - int(g["CHIP"])) & 0xFFFFFFFF)) & 0xFFFFFFFF
            err = ((tx_host - (base + delay)) + 2**31) % 2**32 - 2**31
            errs.append(err)
            print(f"frame {i}: status {st}, sent {err:+d} us from its target")
        if errs:
            print(f"{len(errs)} timed frames {AHEAD_US} us ahead: mean {sum(errs) / len(errs):+.0f} us, "
                  f"{min(errs):+d} .. {max(errs):+d}")
    finally:
        subprocess.run(["adb", "forward", "--remove", f"tcp:{PORT}"], capture_output=True)
        print(adb("pid=$(pidof rcpbridge); [ -n \"$pid\" ] && kill $pid; sleep 1; cat /data/local/tmp/gprcp-test.log"), end="")


if __name__ == "__main__":
    SCAN = sys.argv[1:2] == ["scan"]
    FRAMES = int(sys.argv[1]) if len(sys.argv) > 1 and not SCAN else 20
    AHEAD_US = int(float(sys.argv[2]) * 1000) if len(sys.argv) > 2 else 30000
    main()
