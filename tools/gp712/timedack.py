#!/usr/bin/env python3
"""Does the GP712 take an ack for a frame it sends timed (timed.c: staged, then ScheduleTimedTx 0x8E)?  The CSL test
(csltest.py) had the XIAO take the Echo's CSL frames and ack them while the Echo counted no ack, and repeated them;
frames sent at once to the same XIAO as a polling child were acked (2026-11-28).  Here the Echo's radio raw over
Spinel (gprcp, as twoway.py), the XIAO listening on a PAN of the test's own (OpenThread's CLI, rx on, detached: its
radio acks frames to its extended address by itself), and the same data frames asking for an ack sent at once and
timed, 2006 (Imm-Ack) and 2015 (Enh-Ack) frames.

    tools/gp712/xiaobridge.py <XIAO's tty> <socket> &
    tools/gp712/timedack.py <socket> [channel] [frames]
On the Echo (adb, root): the radio off (radio=off), hassmic_gptime loaded (timed transmit), gprcp and rcpbridge of the
running copy (/system/hassmic or /data/local/hassmic/ota/current)."""
import os
import re
import socket
import struct
import subprocess
import sys
import time

import csltest
import timedtx as t
import twoway as w

PAN = 0x7E57
IE_SIZES = [int(v) for v in os.environ.get("TA_IE", "6 10 14 18 22").split()]  # the XIAO's ackie (0: none)


def main():
    sock = sys.argv[1]
    channel = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    frames = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    # TA_GPRCP: another gprcp on the Echo (cslsweep.py's /data/local/tmp/csl/gprcp); TA_ENV: its variables
    # (GPRCP_ACK_LOG=1: the acks it hears in /data/local/tmp/gprcp-test.log)
    x = csltest.Xiao(sock)
    # a network key in the XIAO's radio (a dataset of the test's own, no parent anywhere): its Enh-Acks to a secured
    # frame are secured with it (Nordic's driver acks before it checks the frame's MIC: junk will do)
    for c in ("ot thread stop", "ot ifconfig down", "ot dataset clear", "ot dataset networkkey 00112233445566778899aabbccddeeff",
              f"ot dataset channel {channel}", f"ot dataset panid {PAN:#x}", "ot dataset extpanid 0e570e570e570e57",
              "ot dataset meshlocalprefix fd00:e57::", "ot dataset networkname timedack", "ot dataset commit active",
              "ot mode rn", "ot ifconfig up", "ot thread start"):
        x.cmd(c)
    ext = bytes.fromhex(next(w for w in x.cmd("ot extaddr").split() if re.fullmatch(r"[0-9a-f]{16}", w)))   # log lines may come first
    print("XIAO:", x.cmd("ot version").split("\n")[0], "| extaddr", ext.hex(), "| state", x.cmd("ot state").split()[0])
    d = next(p for p in ("/data/local/hassmic/ota/current", "/system/hassmic") if t.adb(f"[ -x {p}/gprcp ] && echo y").strip())
    # the filter patch for this driver start, which comes as our host connects (rcpbridge starts gprcp then): root's
    # radio_csl writes 0 at every look while CSL is off, so 1 again every 0.2 s for 15 s
    filters = lambda: int((re.search(r"^FILTER = (\d+)", t.adb("cat /proc/hassmic_gptime"), re.M) or [0, -1])[1])
    subprocess.run(["adb", "forward", f"tcp:{t.PORT}", f"localfilesystem:{t.SOCK}"], check=True, capture_output=True)
    for attempt in range(5):        # until this driver start patched the filter (the driver starts the chip anew at some opens only)
        before = filters()
        t.adb("setsid sh -c 'for i in $(seq 1 100); do echo 1 > /sys/module/hassmic_gptime/parameters/enhack; sleep 0.2; done'"
            " > /dev/null 2>&1 < /dev/null &")     # setsid: the loop outlives this adb shell (its end hangs up the rest)
        t.adb(f"rm -f {t.SOCK}; ({os.environ.get('TA_ENV', '')} {d}/rcpbridge {t.SOCK} {os.environ.get('TA_GPRCP', d + '/gprcp')}"
              " > /data/local/tmp/gprcp-test.log 2>&1 &)")
        time.sleep(0.5)
        e = w.Radio(socket.create_connection(("127.0.0.1", t.PORT)), "Echo")
        w.PAN = PAN
        w.setup(e, channel)
        if filters() > before:
            break
        print("Echo: the filter not patched at this start: again")
        e.s.close(); t.adb("kill $(pidof rcpbridge) 2>/dev/null; sleep 3")
    try:
        e.ask(w.SET, w.P_SADDR, struct.pack("<H", 0x0E01))
        e.send_frame(w.marked(b"E", 255), channel=channel)     # the first confirm: gprcp's first pair (timed.c)
        print("Echo:", " ".join(t.adb("grep -E '^(ENHACK|FILTER) ' /proc/hassmic_gptime").split()))
        rx0 = x.cmd("ot counters mac")
        rounds = [("2006", 0x9C61, 0), ("2015", 0xAC61, 0)] + [("2015", 0xAC61, n) for n in IE_SIZES] + \
                 [("2006 secured", 0x9C69, 0), ("2015 secured", 0xAC69, 0)]
        ctr = 1
        for name, fcf, ie in rounds:
            if name.startswith("2015"):
                x.cmd("ackie 0e01 0")             # the XIAO's Enh-Acks to us longer by a header IE (xiao-module/ackie.c):
                if ie:                              # cleared first (the driver keeps an address's first entry), 16 at most
                    print("  XIAO:", x.cmd(f"ackie 0e01 {ie}").splitlines()[-1])
            name = f"{name}, ack IE {ie:2d} bytes" if name == "2015" else name
            for how in ("at once", "timed"):
                got = {}
                for i in range(frames):
                    sec = b""
                    if fcf & 0x08:      # level 5 (ENC-MIC-32, Thread's), key id mode 1, key index 1; a junk MIC
                        sec = bytes([0x0D]) + struct.pack("<I", ctr) + b"\x01"; ctr += 1
                    psdu = struct.pack("<HBH", fcf, (0x80 if how == "timed" else 0) + (fcf >> 13 & 1) * 0x40 + i,
                                       PAN) + ext[::-1] + struct.pack("<H", 0x0E01) + sec + b"HMTA" + \
                        (b"\xde\xad\xbe\xef" if sec else b"") + b"\0\0"
                    if how == "timed":
                        st, ack = e.send_frame(psdu, delay=20000, base=e.now() & 0xFFFFFFFF, channel=channel)
                    else:
                        st, ack = e.send_frame(psdu, channel=channel)
                    if ack:
                        f = struct.unpack("<H", ack[0][:2])[0]
                        key = ("Enh-Ack" if f >> 12 & 3 == 2 else "Imm-Ack") + f", {len(ack[0])} bytes"
                    else:
                        key = f"no ack (status {st})"
                    got[key] = got.get(key, 0) + 1
                    time.sleep(0.05)
                last = [l for l in x.cmd("acks").splitlines() if l.startswith("ack -0")]     # xiao-module/ackcount.c
                print(f"  {name} {how}: " + "; ".join(f"{n}x {k}" for k, n in got.items()) +
                      (f"   (the XIAO's last ack: {last[0].split(': ', 1)[1][:60]})" if last else ""))
        rx1 = x.cmd("ot counters mac")
        g = lambda s, k: int(s.split(k + ": ")[1].split()[0])
        print(f"XIAO took {g(rx1, 'RxUnicast') - g(rx0, 'RxUnicast')} unicasts of {2 * len(rounds) * frames}"
              f" (security errors {g(rx1, 'RxErrSec') - g(rx0, 'RxErrSec')})")
        # the same secured 2015 frames once more with the Echo promiscuous: whether the XIAO's Enh-Acks reach the air
        e.ask(w.SET, w.P_PROMISC, b"\x02")
        for kind, fcf in (("unsecured", 0xAC61), ("secured", 0xAC69)):
            e.rx.clear()
            for i in range(frames):
                sec = b""
                if fcf & 0x08:
                    sec = bytes([0x0D]) + struct.pack("<I", ctr) + b"\x01"; ctr += 1
                psdu = struct.pack("<HBH", fcf, 0xC0 + i, PAN) + ext[::-1] + struct.pack("<H", 0x0E01) + sec + b"HMTA" + \
                    (b"\xde\xad\xbe\xef" if sec else b"") + b"\0\0"
                st, ack = e.send_frame(psdu, channel=channel); time.sleep(0.05)
            for _ in e.frames(0.5):
                pass
            acks = [(p, ts) for p, r, ts in e.rx if len(p) >= 3 and p[0] & 7 == 2]
            print(f"promiscuous, 2015 {kind}: {len(e.rx)} frames heard, {len(acks)} acks: " +
                  "; ".join(f"seq {p[2]:#04x} FCF {p[1]:02x}{p[0]:02x} {len(p)} bytes" for p, ts in acks[:4]))
        e.ask(w.SET, w.P_PROMISC, b"\x00")
    finally:
        x.cmd("ot thread stop"); x.cmd("ot factoryreset", 5)
        w.end_echo()


if __name__ == "__main__":
    main()
