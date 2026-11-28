#!/usr/bin/env python3
"""CSL between the Echo 4 and a sleepy Thread device (PLAN.md "Thread 1.4" 2.1, 2b.3): the Echo's own border router
as root runs it with the setting thread_csl on (radio=thread, thread_role=border_router: hassmic_gptime loaded, its
filter patched, otbr-agent-csl on gprcp; /data/local/hassmic/radio-csl says "on"), a Seeed XIAO nRF52840 as its CSL
child (tools/gp712/xiao-build.sh: OpenThread's CLI on Zephyr's shell, USB CDC ACM; xiao-flash.sh puts it on).  The
child attaches with rx off and a CSL period; the Echo pings it: each ping's request is a CSL transmission into the
child's window.

    tools/gp712/xiaobridge.py <XIAO's tty> <socket> &      (once for the session)
    tools/gp712/csltest.py [--form] <socket> [period ms] [pings] [channel]
--form: a Thread network of the Echo's own for the test (a new dataset on that channel: the one it had is gone);
without, the network it is on.  The GP712 has channels 11 to 25 (26 refused: "Failed to set PAN channel").  The
XIAO is factory reset after the test.  Its port is the bridge's, open for the whole session: the firmware hung (USB
too) when it was closed and opened again."""
import os
import re
import subprocess
import sys
import time


def adb(cmd, timeout=60):
    r = subprocess.run(["adb", "shell", cmd], capture_output=True, text=True, timeout=timeout)
    return r.stdout.replace("\r", "")


OTCTL = None
POLL_MS = int(os.environ.get("CSL_POLL_MS", "236000"))   # the XIAO's poll period (its default): CSL_POLL_MS=500 with period 0, a polling child


def otctl(cmd):
    return adb(f"{OTCTL} -I ot0 {cmd}")


class Xiao:
    """Zephyr's shell through tools/gp712/xiaobridge.py's socket (the port held open by it: see its head): a line
    out, everything until the next prompt back"""

    def __init__(self, path):
        import stat
        if not stat.S_ISSOCK(os.stat(path).st_mode):
            sys.exit(f"{path}: not a socket: run tools/gp712/xiaobridge.py <tty> <socket> first")
        self.path = path
        self.cmd("")

    def cmd(self, line, wait=3.0):
        import socket
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as c:
            c.settimeout(wait + 5)
            c.connect(self.path)
            c.sendall(f"{wait} {line}\n".encode())
            out = b""
            while True:
                d = c.recv(65536)
                if not d:
                    break
                out += d
        text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out.decode(errors="replace"))
        return "\n".join(l for l in text.replace("\r", "").split("\n")
                         if l.strip() and not l.startswith("uart:~$") and l.strip() != line.strip())

    def ok(self, line):
        r = self.cmd(line)
        if not r.rstrip().endswith("Done"):
            print(f"  XIAO {line[:40]}: {r[-80:]}")
        return r


def echo_ready(form, channel):
    global OTCTL
    for d in ("/data/local/hassmic/ota/current", "/system/hassmic"):
        if adb(f"[ -x {d}/ot-ctl ] && echo y").strip() == "y":
            OTCTL = f"{d}/ot-ctl"; break
    csl = adb("cat /data/local/hassmic/radio-csl 2>/dev/null").strip()
    print("Echo:", adb(f"cat {OTCTL.rsplit('/', 1)[0]}/VERSION").strip(), "| radio-csl:", csl or "-", "|",
          adb("grep -h -E '^(FILTER|HOOKED)' /proc/hassmic_gptime 2>/dev/null").strip().replace("\n", ", "))
    if form:
        for c in ("thread stop", "ifconfig down", "dataset init new", f"dataset channel {channel}",
                  "dataset networkname hassmic-csl", "dataset commit active", "ifconfig up", "thread start"):
            otctl(c)
        for _ in range(30):
            time.sleep(1)
            if "leader" in otctl("state"):
                break
    print("Echo:", otctl("state").split()[0], "on channel", otctl("channel").split()[0], "|", otctl("rcp version").split("\n")[0])
    return otctl("dataset active")


def join(x, ds, period_ms):
    """the dataset field by field (Zephyr's shell takes no line as long as its TLVs in hex), then the child's mode
    (rx off when idle: "-") and CSL period.  The child is looked for in the Echo's child table, not asked: the
    firmware before xiao-module/hfxo.c loses its USB as soon as it sleeps (its radio and OpenThread run on)"""
    g = lambda k: re.search(r"^" + k + r": (\S+)", ds, re.M).group(1)
    x.cmd("ot thread stop"); x.cmd("ot ifconfig down")
    for c in ("ot dataset clear", f"ot dataset networkkey {g('Network Key')}", f"ot dataset channel {g('Channel')}",
              f"ot dataset panid {g('PAN ID')}", f"ot dataset extpanid {g('Ext PAN ID')}",
              f"ot dataset meshlocalprefix {g('Mesh Local Prefix').split('/')[0]}", f"ot dataset networkname {g('Network Name')}",
              "ot dataset commit active", "ot mode -", f"ot csl period {period_ms * 1000}",
              f"ot pollperiod {POLL_MS}"):
        x.ok(c)
    ext = (x.cmd("ot extaddr").split() or ["?"])[0]
    x.ok("ot ifconfig up"); x.ok("ot thread start")
    t0 = time.monotonic()
    for _ in range(120):
        time.sleep(1)
        row = [l for l in otctl("child table").splitlines() if ext in l]
        if row:
            break
    print(f"XIAO {ext}: {'in' if row else 'not in'} the Echo's child table after {time.monotonic() - t0:.0f} s")
    return row[0] if row else None


def child_cols(row):
    """the child table's row: RLOC16 and the CSL column (1: synchronized)"""
    f = [c.strip() for c in row.strip().strip("|").split("|")]
    return {"rloc16": f[1], "age": f[3], "ver": f[9], "csl": f[10], "q": f[11]}


def main():
    args = sys.argv[1:]
    form = "--form" in args
    args = [a for a in args if a != "--form"]
    sock = args[0]
    period_ms = int(args[1]) if len(args) > 1 else 500
    pings = int(args[2]) if len(args) > 2 else 20
    channel = int(args[3]) if len(args) > 3 else 20
    x = Xiao(sock)
    print("XIAO:", x.cmd("ot version").split("\n")[0])
    try:
        ds = echo_ready(form, channel)
        row = join(x, ds, period_ms)
        if not row:
            print("no child: the CSL test did not start")
            return
        pfx = re.search(r"^Mesh Local Prefix: (\S+)::/64", ds, re.M).group(1)
        for _ in range(30):                     # the CSL IE in the child's frames: its parent synchronizes on them
            c = child_cols([l for l in otctl("child table").splitlines() if row.split("|")[-2].strip() in l][0])
            if c["csl"] == "1" or not period_ms:
                break
            time.sleep(1)
        print("Echo's child:", c)
        rloc = f"{pfx}:0:ff:fe00:{int(c['rloc16'], 16):x}"
        out = otctl(f"ping {rloc} 16 {pings} {max(1, period_ms // 1000 + 1)} 64 10")
        print("Echo ping:", " | ".join(out.strip().splitlines()[-2:]) if out.strip() else "no answer")
        print("Echo's child:", child_cols([l for l in otctl("child table").splitlines() if row.split("|")[-2].strip() in l][0]))
        print("XIAO csl:", " ".join(x.cmd("ot csl").split()), "| state", x.cmd("ot state").split()[:1])
        print("XIAO mac:", " ".join(x.cmd("ot counters mac").split())[:700])
        print("Echo mac:", " ".join(otctl("counters mac").split())[:500])
    finally:
        try:
            x.cmd("ot thread stop"); x.cmd("ot factoryreset", 5)
        except OSError:
            print("XIAO: no answer at the end (its USB; reset it)")


if __name__ == "__main__":
    main()
