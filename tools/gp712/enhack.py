#!/usr/bin/env python3
"""Enhanced ACKs from the Echo 4's GP712 (PLAN.md "Thread 1.4" 2.1): the chip's MAC filter, a program of its sequence
processor (docs/re-gp712.md "The chip's own programs"), patched in its RAM by src/kmod/hassmic_gp8051.c so that it
takes 802.15.4-2015 frames (MACFILT 0x6a30 bit 2) and answers those that ask with an Enh-Ack it writes into the
hardware's own ACK buffer (PBM 7), sent at the hardware's turnaround.  Held to a second radio as tools/gp712/twoway.py:

  none      the chip as Amazon's driver sets it up
  noack     2015 frames taken, none answered (the patch's first half)
  enhack    2015 frames answered with an Enh-Ack: FCF 0x2802 (to a short source; 0x2c02 to an extended one), seq,
            the source's PAN and address

Each way: the SLZB sends the Echo 2006 and 2015 data frames asking for an ack (what came back), and the Echo sends
the SLZB 2015 frames asking for one (whether the chip takes the SLZB's Enh-Ack as the ack).

    tools/gp712/enhack.py <SLZB address>[:port] [channel] [variant ...]
build/laser/gprcp, rcpbridge and hassmic_gp8051.ko in /data/local/tmp; the patches' binaries from tools/gp712/
(seq_noack.bin, seq_enhack.bin: made by tools/gp712/mkseq.py).  Every patch is undone by the module after the
variant's test (and by any start of the driver: gpHal_Init uploads the filter again)."""
import os
import socket
import struct
import subprocess
import sys
import threading
import time

import timedtx as t
import twoway as w

HERE = os.path.dirname(os.path.abspath(__file__))
HOLD_MS = 25000                             # how long a variant's patch stays: the test runs within it


def binaries():
    if not all(os.path.exists(os.path.join(HERE, f)) for f in ("seq_noack.bin", "seq_enhack.bin", "seq_2015.bin", "seq_hooks.txt")):
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "mkseq.py")], check=True, capture_output=True)


def module_args(variant):
    if variant == "none":
        return "nouc=1 peek=3fd8:4 runms=%d reads=1" % HOLD_MS
    def chunks(base, blob):
        h = blob.hex()
        return ["%x:%s" % (base + o // 2, h[o:o + 64]) for o in range(0, len(h), 64)]
    wr = ["3fd8:00000000"] + chunks(0x34ac, open(os.path.join(HERE, "seq_%s.bin" % variant), "rb").read())
    hooks = [l.split() for l in open(os.path.join(HERE, "seq_hooks.txt"))]
    if variant == "noack":                                  # the ack's half only (2015 frames by 2006's rules)
        hooks = [h for h in hooks if h[0] in ("3020", "3420")]
    else:                                                   # and the 2015 parser at 0x2c00
        wr += chunks(0x2c00, open(os.path.join(HERE, "seq_2015.bin"), "rb").read())
    wr += ["%s:%s" % (h[0], h[1]) for h in hooks]           # the extensions first, then each hook whole
    undo = ["%s:%s" % (h[0], h[2]) for h in reversed(hooks)] + ["3fd8:00000000"]
    parts, cur = [], []                                     # write, write2, write3: 1024 bytes each at most
    for x in wr:
        if len(",".join(cur + [x])) > 1000:
            parts.append(cur); cur = []
        cur.append(x)
    parts.append(cur)
    assert len(parts) <= 3
    w = " ".join("%s=%s" % (("write", "write2", "write3")[i], ",".join(p)) for i, p in enumerate(parts))
    return ("nouc=1 poke=6a30:04:04 unpoke=6a30:04:00 %s undo=%s peek=3fd8:4,6a30:1 runms=%d reads=1"
            % (w, ",".join(undo), HOLD_MS))


def kinds(r, psdu_fcf, n, channel, dst):
    """the SLZB sends n data frames asking for an ack: what came back"""
    got = {}
    for i in range(n):
        psdu = struct.pack("<HBHHH", psdu_fcf, 0x60 + i, w.PAN, dst, 0x5101) + b"HMTWack" + b"\0\0"
        st, ack = r.send_frame(psdu, retries=0, csma=1, channel=channel)
        if ack:
            f = struct.unpack("<H", ack[0][:2])[0]
            k = ("Enh-Ack" if f >> 12 & 3 == 2 else "Imm-Ack") + " " + ack[0][:-2].hex() if f & 7 == 2 else "type %d" % (f & 7)
            k = k.replace(ack[0][2:3].hex(), "SS", 1)                  # the seq, which differs per frame
        else:
            k = "no ack (%s)" % st
        got[k] = got.get(k, 0) + 1
    return "; ".join("%dx %s" % (v, k) for k, v in got.items())


def variant_test(e, s, variant, channel):
    t.adb("rmmod hassmic_gp8051 2>/dev/null")
    out = {}
    cmd = "cd /data/local/tmp; insmod hassmic_gp8051.ko " + module_args(variant)
    th = threading.Thread(target=lambda: out.setdefault("r", subprocess.run(["adb", "shell", cmd], capture_output=True, text=True)))
    th.start(); th.join()
    time.sleep(1)
    print(f"== {variant}")
    e.rx.clear()
    print("  SLZB -> Echo 2006:", kinds(s, 0x8861, 10, channel, 0x0E01))
    before = len(e.rx)
    print("  SLZB -> Echo 2015:", kinds(s, 0xA861, 10, channel, 0x0E01))
    for _ in e.frames(0.3): pass
    print("  ... the Echo handed up %d of them" % sum(1 for x in e.rx[before:] if b"HMTWack" in x[0] and x[0][1] & 0x30 == 0x20))
    # the Echo asks the SLZB (PAN, short address 0x5101 set there) for an ack of a 2015 frame
    okn = 0
    for i in range(10):
        psdu = struct.pack("<HBHHH", 0xA861, 0x80 + i, w.PAN, 0x5101, 0x0E01) + b"HMTWeck" + b"\0\0"
        st, ack = e.send_frame(psdu, retries=0, csma=1, channel=channel)
        okn += st == 0
    print(f"  Echo -> SLZB 2015, acked: {okn} of 10")
    # the module's last read undoes the patch: waited for (STATE = 2), never unloaded before
    r = subprocess.run(["adb", "shell", "for i in $(seq 1 %d); do grep -q 'STATE = [2-]' /proc/hassmic_gp8051 && break; sleep 1; done; "
                        "cat /proc/hassmic_gp8051; grep -q 'STATE = [2-]' /proc/hassmic_gp8051 && rmmod hassmic_gp8051" % (HOLD_MS // 1000 + 10)],
                       capture_output=True, text=True).stdout
    print("  " + "\n  ".join(l for l in r.splitlines() if l.startswith(("STATE", "  ", "poke", "read"))))


def main():
    host = sys.argv[1]
    port = int(host.split(":")[1]) if ":" in host else 6638
    host = host.split(":")[0]
    channel = int(sys.argv[2]) if len(sys.argv) > 2 else 26
    variants = sys.argv[3:] or ["none", "noack", "enhack"]
    binaries()
    t.adb(f"rm -f {t.SOCK}; (/data/local/tmp/rcpbridge {t.SOCK} /data/local/tmp/gprcp > /data/local/tmp/gprcp-test.log 2>&1 &)")
    time.sleep(0.5)
    subprocess.run(["adb", "forward", f"tcp:{t.PORT}", f"localfilesystem:{t.SOCK}"], check=True, capture_output=True)
    try:
        e = w.Radio(socket.create_connection(("127.0.0.1", t.PORT)), "Echo")
        s = w.Radio(socket.create_connection((host, port), timeout=5), "SLZB")
        w.setup(e, channel); w.setup(s, channel)
        e.ask(w.SET, w.P_SADDR, struct.pack("<H", 0x0E01)); e.ask(w.SET, w.P_LADDR, bytes.fromhex("0e0e0e0e0e0e0e01"))
        s.ask(w.SET, w.P_SADDR, struct.pack("<H", 0x5101)); s.ask(w.SET, w.P_LADDR, bytes.fromhex("5151515151515101"))
        for v in variants:
            variant_test(e, s, v, channel)
    finally:
        w.end_echo()
        t.adb("rmmod hassmic_gp8051 2>/dev/null")


if __name__ == "__main__":
    main()
