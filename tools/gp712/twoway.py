#!/usr/bin/env python3
"""The Echo 4's radio against a second one (PLAN.md "Thread 1.4" 2b.2, 2.1): an OpenThread RCP of another make
(an SLZB-06 with Thread RCP firmware: Silicon Labs, Thread 1.4 certified, Spinel over TCP) beside gprcp, both raw
over Spinel from this PC, on one channel.

  timing   Each sends a frame timed 30 ms ahead on its own clock, the other timestamps it as it comes (the end of
           the SFD, OpenThread's reference both ways).  A = SLZB's arrival - Echo's target, B = Echo's arrival -
           SLZB's target: the clocks' offset is +theta in one and -theta in the other, so A + B is the Echo's
           transmit error plus its receive timestamp's, the two a CSL parent's schedule stands on (it puts a frame
           at the child's window from the child's frames' timestamps); A and B alone: each way's jitter.
  acks     The SLZB sends the Echo (PAN, short address set) a data frame asking for an ack, as 802.15.4-2006 and as
           -2015 (frame version 2): an Imm-Ack for the first, an Enh-Ack for the second is what Thread 1.2 asks.

On the Echo 4 (adb, root), the radio off, hassmic_gptime loaded, build/laser/gprcp and rcpbridge in /data/local/tmp:
    tools/gp712/twoway.py <SLZB address>[:port] [frames] [channel] [--echo=<host>:<port>: another RCP, for tests]
Nothing but the frames of this test goes out (a PAN of its own, 0x7e57; broadcasts only for timing)."""
import os
import socket
import statistics
import struct
import subprocess
import sys
import time

import timedtx as t
from timedtx import GET, IS, SET, P_CAPS, P_CHAN, P_PHY_ENABLED, P_RAW, P_STATUS, P_STREAM_RAW, P_TIMESTAMP

P_LADDR, P_SADDR, P_PANID, P_PROMISC = 0x34, 0x35, 0x36, 0x38
P_NCP_VERSION, P_RCP_API = 2, 0xB0
PAN = 0x7E57
AHEAD = 30000
PAD = bytes(int(os.environ.get("TW_PAD", "0")))      # TW_PAD=<n>: n more bytes a timing frame (timestamps' reference)


class Radio(t.Rcp):
    """Rcp that keeps the frames the radio hears while it waits for something else"""
    def __init__(self, s, name):
        super().__init__(s)
        self.name, self.rx = name, []

    def frames(self, timeout):
        for f in super().frames(timeout):
            if f[1] == IS and f[2] == P_STREAM_RAW and f[0] == 0:
                self.rx.append(parse_rx(f[3]))
            yield f

    def reset(self):
        self.send(t.CMD_RESET, None, b"\x02")
        for _, c, p, v in self.frames(5):
            if c == IS and p == P_STATUS:
                return
        raise SystemExit(f"{self.name}: no reset")

    def now(self):
        _, v = self.ask(GET, P_TIMESTAMP)
        return struct.unpack("<Q", v[:8])[0]

    def send_frame(self, psdu, delay=0, base=0, retries=0, csma=0, channel=26, timeout=2.0):
        """the transmit's status and the ack frame (None without one)"""
        val = (struct.pack("<H", len(psdu)) + psdu + bytes([channel, 0, retries, csma, 0, 0, 0])
               + struct.pack("<II", delay, base) + bytes([channel, 0x7F]))
        tid = self.send(SET, P_STREAM_RAW, val)
        for ftid, c, p, v in self.frames(timeout + delay / 1e6):
            if ftid == tid and c == IS and p == P_STATUS:
                st, rest = t.unpack_uint(v)
                ack = parse_rx(rest[2:]) if st == 0 and len(rest) > 4 else None
                return st, ack
        return None, None


def parse_rx(v):
    """Spinel's received frame: (psdu, rssi, timestamp us)"""
    n = struct.unpack("<H", v[:2])[0]
    psdu = v[2:2 + n]
    o = 2 + n
    rssi = struct.unpack("<b", v[o:o + 1])[0]
    o += 4                          # rssi, noise floor, flags
    o += 2                          # the PHY struct's length
    ts = struct.unpack("<Q", v[o + 2:o + 10])[0]
    return psdu, rssi, ts


def marked(who, i):
    """a broadcast data frame of this test: PAN 0x7e57, from 0x0e00 (Echo) or 0x5100 (SLZB), "HMTW" + who + i"""
    src = 0x0E00 if who == b"E" else 0x5100
    return struct.pack("<HBHHH", 0x8841, i & 0xFF, PAN, 0xFFFF, src) + b"HMTW" + who + bytes([i & 0xFF]) + PAD + b"\0\0"


def heard(r, who, i, wait=0.5):
    want = b"HMTW" + who + bytes([i & 0xFF])
    end = time.monotonic() + wait
    while True:
        for k, (psdu, rssi, ts) in enumerate(r.rx):
            if want in psdu:
                del r.rx[k]
                return ts, rssi
        if time.monotonic() > end:
            return None, None
        for _ in r.frames(0.02):
            break


def setup(r, channel):
    r.reset()
    _, caps = r.ask(GET, P_CAPS)
    caps, _ = t.unpack_uint(caps)
    _, ver = r.ask(GET, P_NCP_VERSION)
    print(f"{r.name}: {ver.rstrip(bytes(1)).decode(errors='replace')}, caps {caps:#x}"
          f" (transmit timing {'yes' if caps & 0x40 else 'no'}, receive timing {'yes' if caps & 0x80 else 'no'})")
    r.ask(SET, P_PHY_ENABLED, b"\x01")
    r.ask(SET, P_CHAN, bytes([channel]))
    r.ask(SET, P_PANID, struct.pack("<H", PAN))       # the frames' PAN, or the radios' filters drop them
    r.ask(SET, P_RAW, b"\x01")
    return caps


def timing(e, s, frames, channel):
    A, B = [], []
    for i in range(frames):
        te = e.now()
        st, _ = e.send_frame(marked(b"E", i), AHEAD, te & 0xFFFFFFFF, channel=channel)
        rs, _ = heard(s, b"E", i)
        ts_ = s.now()
        st2, _ = s.send_frame(marked(b"S", i), AHEAD, ts_ & 0xFFFFFFFF, channel=channel)
        re_, _ = heard(e, b"S", i)
        if rs is None or re_ is None or st or st2:
            print(f"  {i}: Echo sent {st}, SLZB heard {'yes' if rs else 'no'}; SLZB sent {st2}, Echo heard {'yes' if re_ else 'no'}")
            continue
        a = rs - (te + AHEAD)
        # the Echo's timestamps: its clock's low 32 bits are what the driver keeps; whole from its own now
        re_full = (te & ~0xFFFFFFFF) | (re_ & 0xFFFFFFFF)
        b = re_full - (ts_ + AHEAD)
        A.append(a); B.append(b)
    if len(A) < 2:
        print("timing: too few frames")
        return
    a0, b0 = A[0], B[0]
    da = [x - a0 for x in A]; db = [x - b0 for x in B]; s_ = [x + y for x, y in zip(A, B)]
    print(f"timing, {len(A)} frames each way, {AHEAD} us ahead:")
    print(f"  Echo -> SLZB: spread {min(da):+d} .. {max(da):+d} us (sd {statistics.pstdev(da):.1f})")
    print(f"  SLZB -> Echo: spread {min(db):+d} .. {max(db):+d} us (sd {statistics.pstdev(db):.1f})")
    print(f"  A + B (the Echo's transmit plus receive error): mean {statistics.mean(s_):+.1f} us, "
          f"{min(s_):+d} .. {max(s_):+d} (sd {statistics.pstdev(s_):.1f})")


def acks(e, s, channel):
    e.ask(SET, P_SADDR, struct.pack("<H", 0x0E01))
    e.ask(SET, P_LADDR, bytes.fromhex("0e0e0e0e0e0e0e01"))
    print("acks (SLZB -> Echo, data frames asking for one):")
    for name, fcf in (("2006", 0x8861), ("2015", 0xA861)):
        got = {}
        for i in range(10):
            psdu = struct.pack("<HBHHH", fcf, 0x40 + i, PAN, 0x0E01, 0x5101) + b"HMTWack" + b"\0\0"
            st, ack = s.send_frame(psdu, retries=0, csma=1, channel=channel)
            if ack:
                f = struct.unpack("<H", ack[0][:2])[0]
                kind = "Enh-Ack" if f >> 12 & 3 == 2 else "Imm-Ack"
                key = f"{kind}{' with IEs' if f & 0x200 else ''}{', secured' if f & 8 else ''}, {len(ack[0])} bytes" \
                    if f & 7 == 2 else f"frame type {f & 7}?"
            else:
                key = f"no ack (status {st})"
            got[key] = got.get(key, 0) + 1
        print(f"  {name}: " + "; ".join(f"{n}x {k}" for k, n in got.items()))


def main():
    host = sys.argv[1]
    port = int(host.split(":")[1]) if ":" in host else 6638
    host = host.split(":")[0]
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    channel = int(sys.argv[3]) if len(sys.argv) > 3 else 26
    echo = ("127.0.0.1", t.PORT)
    if len(sys.argv) > 4 and sys.argv[4].startswith("--echo="):        # another radio (this script's own test)
        h, p = sys.argv[4][7:].split(":"); echo = (h, int(p))
    else:
        t.adb(f"rm -f {t.SOCK}; ({os.environ.get('TW_ECHO_ENV', '')} /data/local/tmp/rcpbridge {t.SOCK} /data/local/tmp/gprcp > /data/local/tmp/gprcp-test.log 2>&1 &)")
        time.sleep(0.5)
        subprocess.run(["adb", "forward", f"tcp:{t.PORT}", f"localfilesystem:{t.SOCK}"], check=True, capture_output=True)
    try:
        e = Radio(socket.create_connection(echo), "Echo")
        s = Radio(socket.create_connection((host, port), timeout=5), "SLZB")
        setup(e, channel)
        scaps = setup(s, channel)
        # a first frame from the Echo: its confirm is gprcp's first pair of the chip's clock (timed.c)
        e.send_frame(marked(b"E", 255), channel=channel)
        s.rx.clear()
        if scaps & 0x40:
            timing(e, s, frames, channel)
        else:
            print("timing: the SLZB's RCP has no transmit timing; one way only is no measure of the sum")
        acks(e, s, channel)
    finally:
        if echo == ("127.0.0.1", t.PORT):
            end_echo()


def end_echo():
    subprocess.run(["adb", "forward", "--remove", f"tcp:{t.PORT}"], capture_output=True)
    print(t.adb("pid=$(pidof rcpbridge); [ -n \"$pid\" ] && kill $pid; sleep 1; cat /data/local/tmp/gprcp-test.log"), end="")


if __name__ == "__main__":
    main()
