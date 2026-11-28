#!/usr/bin/env python3
"""What the Echo 4's GP712 misses of what it could hear (PLAN.md "Thread 1.4" 2b.2: it missed 1 to 5 of 15 frames of a
second radio a metre away), passively: gprcp promiscuous on a busy channel, every sender's MAC sequence numbers
counted (802.15.4 senders number every frame they send, whoever it is for): a number again is a retransmission, a gap
of g frames that many missed (gaps over 16 left out: the sender away, a restart).  Twice: the Echo's Wi-Fi quiet, and
kept busy by a UDP stream from this PC to a port the Echo's firewall drops (the air time is what counts: the GP712
driver is Qorvo's FEM_COEX build, Wi-Fi and 802.15.4 share the 2.4 GHz front end).

    tools/gp712/rxloss.py <Echo's address> [channel] [seconds each] [MB/s]
The radio off on the Echo; build/laser/gprcp and rcpbridge in /data/local/tmp.  Nothing is sent on the channel."""
import socket
import struct
import subprocess
import sys
import threading
import time

import timedtx as t
import twoway as w


def senders(frames):
    """per (PAN, short source): [frames, retries, missed, rssi sum]"""
    seen = {}
    for psdu, rssi, ts in frames:
        if len(psdu) < 9:
            continue
        fcf = psdu[0] | psdu[1] << 8
        if fcf & 7 not in (1, 3) or (fcf >> 14) & 3 != 2:          # data or command, a short source
            continue
        seq = psdu[2]
        dmode = (fcf >> 10) & 3
        o = 3 + (2 + (2 if dmode == 2 else 8) if dmode else 0)
        pan = psdu[3] | psdu[4] << 8 if dmode else None
        if not fcf & 0x40:                                           # no PAN ID compression: the source PAN
            pan = psdu[o] | psdu[o + 1] << 8; o += 2
        if o + 2 > len(psdu):
            continue
        src = psdu[o] | psdu[o + 1] << 8
        k = (pan, src)
        s = seen.setdefault(k, [0, 0, 0, 0, None])
        s[0] += 1; s[3] += rssi
        if s[4] is not None:
            g = (seq - s[4]) % 256
            if g == 0:
                s[1] += 1
            elif g <= 16:
                s[2] += g - 1
        s[4] = seq
    return seen


def listen(e, sec):
    e.rx.clear(); end = time.monotonic() + sec
    while time.monotonic() < end:
        for _ in e.frames(0.1):
            pass
    return list(e.rx)


def report(name, frames):
    s = senders(frames)
    tot = sum(v[0] - v[1] for v in s.values()); miss = sum(v[2] for v in s.values())
    print(f"{name}: {len(frames)} frames, {len(s)} senders; missed {miss} of {tot + miss} numbered "
          f"({100 * miss / max(1, tot + miss):.1f} %)")
    for k, v in sorted(s.items(), key=lambda x: -x[1][0])[:6]:
        print(f"   PAN {k[0] if k[0] is None else '%04x' % k[0]} src {k[1]:04x}: {v[0]} frames, {v[1]} again, "
              f"{v[2]} missed, {v[3] / v[0]:.0f} dBm")
    return miss, tot + miss


def blast(host, mbps, stop):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    pkt = bytes(1400); per = 1400 / (mbps * 1e6); nxt = time.monotonic()
    while not stop.is_set():
        s.sendto(pkt, (host, 9)); nxt += per
        d = nxt - time.monotonic()
        if d > 0:
            time.sleep(d)


def main():
    host = sys.argv[1]
    channel = int(sys.argv[2]) if len(sys.argv) > 2 else 11
    sec = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    mbps = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0
    t.adb(f"rm -f {t.SOCK}; (/data/local/tmp/rcpbridge {t.SOCK} /data/local/tmp/gprcp > /data/local/tmp/gprcp-test.log 2>&1 &)")
    time.sleep(0.5)
    subprocess.run(["adb", "forward", f"tcp:{t.PORT}", f"localfilesystem:{t.SOCK}"], check=True, capture_output=True)
    try:
        e = w.Radio(socket.create_connection(("127.0.0.1", t.PORT)), "Echo")
        w.setup(e, channel); e.ask(w.SET, w.P_PROMISC, b"\x02")
        report(f"Wi-Fi quiet, {sec} s", listen(e, sec))
        stop = threading.Event(); th = threading.Thread(target=blast, args=(host, mbps, stop), daemon=True); th.start()
        try:
            report(f"Wi-Fi busy ({mbps} MB/s to the Echo), {sec} s", listen(e, sec))
        finally:
            stop.set()
        report(f"Wi-Fi quiet again, {sec} s", listen(e, sec))
    finally:
        w.end_echo()


if __name__ == "__main__":
    main()
