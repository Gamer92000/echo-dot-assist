#!/usr/bin/env python3
"""The filter patch (tools/gp712/enhack.py's "enhack": 2015 frames, Enh-Acks, frame pending) against the frames the
Echo 4 hears of a busy 2006 channel, by rxloss.py's measure (each sender's MAC sequence numbers: what was missed, not
how much came, which the traffic decides): unpatched, patched, unpatched, patched, the same length each.  Receive
only; nothing is sent on the channel.

    tools/gp712/rxpatch.py [channel] [seconds each]
The radio off on the Echo; build/laser/gprcp, rcpbridge and hassmic_gp8051.ko in /data/local/tmp."""
import socket
import subprocess
import sys
import time

import enhack as eh
import rxloss as rl
import timedtx as t
import twoway as w


def main():
    channel = int(sys.argv[1]) if len(sys.argv) > 1 else 11
    sec = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    eh.binaries()
    eh.HOLD_MS = (sec + 4) * 1000
    args = eh.module_args("enhack")
    t.adb(f"rm -f {t.SOCK}; (/data/local/tmp/rcpbridge {t.SOCK} /data/local/tmp/gprcp > /data/local/tmp/gprcp-test.log 2>&1 &)")
    time.sleep(0.5)
    subprocess.run(["adb", "forward", f"tcp:{t.PORT}", f"localfilesystem:{t.SOCK}"], check=True, capture_output=True)
    tot = {"unpatched": [0, 0], "patched": [0, 0]}
    try:
        e = w.Radio(socket.create_connection(("127.0.0.1", t.PORT)), "Echo")
        w.setup(e, channel); e.ask(w.SET, w.P_PROMISC, b"\x02")
        for rnd in range(2):
            m, n = rl.report(f"unpatched ({rnd + 1}), {sec} s", rl.listen(e, sec))
            tot["unpatched"][0] += m; tot["unpatched"][1] += n
            o = subprocess.run(["adb", "shell", "cd /data/local/tmp; insmod hassmic_gp8051.ko " + args], capture_output=True, text=True)
            if o.returncode:
                print("insmod:", o.stderr.strip()[:200]); return
            time.sleep(0.5)
            m, n = rl.report(f"patched ({rnd + 1}), {sec} s", rl.listen(e, sec))
            tot["patched"][0] += m; tot["patched"][1] += n
            r = subprocess.run(["adb", "shell", "for i in $(seq 1 %d); do grep -q 'STATE = [2-]' /proc/hassmic_gp8051 && break; sleep 1; done; "
                                "grep -E 'STATE|6a30' /proc/hassmic_gp8051 | tr '\\n' ' '; rmmod hassmic_gp8051" % (sec + 15)],
                               capture_output=True, text=True).stdout
            print("   module:", r.strip())
        for k, (m, n) in tot.items():
            print(f"{k}: missed {m} of {n} ({100 * m / max(1, n):.1f} %)")
        print("driver re-inits since boot:", t.adb("dmesg | grep -c 'Thread re-init'").strip())
    finally:
        w.end_echo()
        t.adb("rmmod hassmic_gp8051 2>/dev/null")


if __name__ == "__main__":
    main()
