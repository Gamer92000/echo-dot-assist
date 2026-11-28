#!/usr/bin/env python3
"""Where in the CSL child's window the Echo's CSL frames land (PLAN.md "Thread 1.4" 2b.3): the Echo's border router
with the CSL transmitter (otbr-agent-csl on gprcp, started here, not by root), the XIAO its CSL child (csltest.py's
join), and every timed frame moved by a shift that the test sets (gprcp's GPRCP_TX_ADJUST, read when the file
changes): per shift, pings to the child, and gprcp's own line per timed frame (GPRCP_TX_LOG) of how it ended.

    tools/gp712/xiaobridge.py <XIAO's tty> <socket> &
    tools/gp712/cslsweep.py <socket> [period ms] [pings] [shift us ...]
On the Echo (adb, root): the radio off (radio=off: root's radio_watch runs nothing on the radio), hassmic_gptime
loaded; build/laser/gprcp pushed to /data/local/tmp/csl; otbr-agent-csl, ot-ctl and rcpbridge of the running copy.
The network: the one the Echo's border router has in /data/local/hassmic/thread (it forms one if there is none)."""
import os
import re
import subprocess
import sys
import time

import csltest as c
from mkseq_isa import ack_counters

JMPHEX = lambda t: bytes([0x40, 0, t & 0xff, t >> 8]).hex()
TOP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
T = "/data/local/tmp/csl"
ADJ = T + "/adjust"


def adb(cmd, timeout=60):
    return c.adb(cmd, timeout)


def filters():
    m = re.search(r"^FILTER = (\d+)", adb("cat /proc/hassmic_gptime"), re.M)
    return int(m.group(1)) if m else -1


def start():
    """the stack, again until the driver's start patched the filter (FILTER counts the patches: the driver starts
    the chip anew at some opens only, and root's radio_csl writes enhack 0 between)"""
    for _ in range(4):
        before = filters()
        start_once()
        if filters() > before:
            break
        print("Echo: the filter not patched at this start: again")
        stop(); time.sleep(3)


def start_once():
    d = next(p for p in ("/data/local/hassmic/ota/current", "/system/hassmic") if adb(f"[ -x {p}/otbr-agent-csl ] && echo y").strip())
    adb(f"mkdir -p {T}")
    subprocess.run(["adb", "push", os.path.join(TOP, "build/laser/gprcp"), T + "/gprcp"],
                   check=True, capture_output=True)
    adb(f"echo 0 > {ADJ}")
    subprocess.run(["adb", "push", os.path.join(TOP, "build/laser/hassmic_gp8051.ko"), "/data/local/tmp/"], check=True,
                   capture_output=True)
    # the filter patch for this driver start (root's radio_csl writes 0 at every look while CSL is off)
    adb("setsid sh -c 'for i in $(seq 1 100); do echo 1 > /sys/module/hassmic_gptime/parameters/enhack; sleep 0.2; done'"
        " > /dev/null 2>&1 < /dev/null &")     # setsid: the loop outlives this adb shell (its end hangs up the rest)
    adb(f"cd {T}; rm -f rcp.sock; (GPRCP_TX_ADJUST={ADJ} GPRCP_TX_LOG=1 GPRCP_ACK_LOG={os.environ.get("CSL_ACK_LOG", "0")} {d}/rcpbridge {T}/rcp.sock {T}/gprcp > {T}/rcp.log 2>&1 &);"
        f" sleep 1; ({d}/otbr-agent-csl -I ot0 -B wlan0 -s -d 5 --rest-listen-address {T}/rest.sock"
        f" spinel+hdlc+unix://{T}/rcp.sock > {T}/otbr.log 2>&1 &)")
    c.OTCTL = f"{d}/ot-ctl"
    for _ in range(60):
        time.sleep(1)
        st = c.otctl("state")
        if "leader" in st or "router" in st:
            break
    if "leader" not in c.otctl("state"):
        for cmd in ("dataset init new", "dataset channel 20", "dataset networkname hassmic-csl", "dataset commit active",
                    "ifconfig up", "thread start"):
            c.otctl(cmd)
        for _ in range(30):
            time.sleep(1)
            if "leader" in c.otctl("state"):
                break
    print("Echo:", c.otctl("state").split()[0], "on channel", c.otctl("channel").split()[0], "|",
          " ".join(adb("grep -E '^(ENHACK|FILTER) ' /proc/hassmic_gptime").split()))


def peek(a):
    """a chip register read through the test module (build/laser/hassmic_gp8051.ko, pushed by this script)"""
    adb(f"cd /data/local/tmp; insmod hassmic_gp8051.ko nouc=1 peek={a}:1 runms=500 reads=1; "
        "for i in 1 2 3 4 5 6; do grep -q 'STATE = [2-]' /proc/hassmic_gp8051 && break; sleep 1; done")
    r = adb("cat /proc/hassmic_gp8051; rmmod hassmic_gp8051")
    return r


def stop():
    adb("for p in otbr-agent-csl rcpbridge gprcp; do kill $(pidof $p) 2>/dev/null; done; sleep 1")


def main():
    sock = sys.argv[1]
    period_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    pings = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    shifts = [v if v.startswith("poke=") or v == "count-acks" else int(v) for v in sys.argv[4:]] or [0, -425, -200, -650, 200]
    poked, counting = [], []
    x = c.Xiao(sock)
    try:
        start()
        row = c.join(x, c.otctl("dataset active"), period_ms)
        if not row:
            print("no child")
            return
        ext = row.split("|")[-2].strip()
        pfx = re.search(r"^Mesh Local Prefix: (\S+)::/64", c.otctl("dataset active"), re.M).group(1)
        for sh in shifts:
            if sh == "count-acks":          # the filter program counts the acks reaching it (mkseq_isa.ack_counter)
                cs = ack_counters()
                pieces = ["3fd0:0000000000000000", "3fd8:0000000000000000"] + \
                    [f"{at + o:x}:{code[o:o + 32].hex()}" for at, code, _, _ in cs for o in range(0, len(code), 32)]
                hooks = [f"{h:x}:{JMPHEX(at)}" for at, _, h, _ in cs]           # the code first, then each hook
                undo = [f"{h:x}:{o}" for _, _, h, o in reversed(cs)]
                w, w2 = pieces[:len(pieces) // 2], pieces[len(pieces) // 2:] + hooks   # 1024 bytes a parameter
                adb(f"cd /data/local/tmp; insmod hassmic_gp8051.ko nouc=1 write={','.join(w)} write2={','.join(w2)} "
                    f"undo={','.join(undo)} peek=3fd0:8,3fd8:8 runms=40000 reads=1")
                time.sleep(2)
                counting.append(1)
                print("filter program: counting the acks it sees (40 s)")
                continue
            if isinstance(sh, str):         # "poke=<addr>:<mask>:<val>": a chip register for the rounds after it
                p = sh.split("=", 1)[1]
                a, m, v = p.split(":")
                was = re.search(r"%s: ([0-9a-f]{2})" % a, peek(a)).group(1)
                adb(f"cd /data/local/tmp; insmod hassmic_gp8051.ko nouc=1 poke={a}:{m}:{v} unpoke={a}:{m}:{was} runms=300000 reads=1")
                time.sleep(2)
                print(f"chip register {a}: {was} -> {v} (until the module ends: 300 s, or rmmod)")
                poked.append(a)
                continue
            adb(f"echo {sh} > {ADJ}")
            time.sleep(3)
            ch = [l for l in c.otctl("child table").splitlines() if ext in l]
            if not ch:
                print(f"shift {sh:+5d} us: the child is gone"); break
            col = c.child_cols(ch[0])
            n0 = int(adb(f"grep -c 'timed seq' {T}/rcp.log").strip() or 0)
            out = c.otctl(f"ping {pfx}:0:ff:fe00:{int(col['rloc16'], 16):x} 16 {pings} {max(1, period_ms // 1000 + 1)} 64 10")
            got = re.search(r"(\d+) packets received", out)
            lines = adb(f"grep 'timed seq' {T}/rcp.log | tail -n +{n0 + 1}").splitlines()
            ends = [l.split(": ", 2)[2] for l in lines if "acked" in l or "no ack" in l or "failed" in l]
            acked = sum(e.startswith("acked") for e in ends)
            print(f"shift {sh:+5d} us: pings {got.group(1) if got else '?'} of {pings}; timed frames {len(ends)}, "
                  f"acked {acked}; child CSL {col['csl']} before")
            print("   XIAO:", " | ".join(x.cmd("acks").splitlines()[:4])[:400])     # xiao-module/ackcount.c
            if counting:
                r = adb("for i in $(seq 1 45); do grep -q 'STATE = [2-]' /proc/hassmic_gp8051 && break; sleep 1; done; "
                        "cat /proc/hassmic_gp8051; rmmod hassmic_gp8051")
                m = re.search(r"3fd0:((?: [0-9a-f]{2}){8})", r); m2 = re.search(r"3fd8:((?: [0-9a-f]{2}){8})", r)
                if m and m2:
                    b = bytes.fromhex((m.group(1) + m2.group(1)).replace(" ", ""))
                    v = lambda o: b[o] | b[o + 1] << 8
                    print(f"   filter program saw (all, secured): phase 2 {v(0)}, {v(2)};"
                          f" the ack's match {v(4)}, {v(6)}")
                else:
                    print("   filter program:", r.strip()[-300:])
                counting.clear()
            x.cmd("acks clear")
        print("XIAO mac:", " ".join(x.cmd("ot counters mac").split())[:500])
    finally:
        if poked:
            adb("rmmod hassmic_gp8051")
        try:
            x.cmd("ot thread stop"); x.cmd("ot factoryreset", 5)
        except OSError:
            pass
        stop()


if __name__ == "__main__":
    main()
