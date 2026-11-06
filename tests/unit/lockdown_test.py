#!/usr/bin/env python3
"""scripts/device/lockdown.sh's per-app rules (lockdown.sh print: the chains as they would be loaded, nothing changed)
against stand-ins of PackageManager's packages.xml, hassmic's state/apps and the kernel's /proc/net tables:
  - "net <package>": that app's uid let out to the internet (hassmic_out), only an app of the owner's in /data/app
  - "lan <package>" (issue #109): the ports that app's uid listens on admitted from private and link-local addresses
    (hassmic_in): TCP listening (0A), UDP bound (07), its own only, never below 1024 or in what is open already, and the
    rules following the port when it moves"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
fails = 0


def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what)
    if not ok: fails += 1


PKG = '<package name="{n}" codePath="{c}" publicFlags="{f}" privateFlags="0" ft="1" it="1" ut="1" version="1" userId="{u}">\n'
HEAD = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"


def sock(i, port, st, uid, v6=False):
    addr = "00000000000000000000000000000000" if v6 else "00000000"
    return f"   {i}: {addr}:{port:04X} {addr}:0000 {st} 00000000:00000000 00:00000000 00000000 {uid:5d}        0 {1000 + i} 1\n"


def run(d):
    env = dict(os.environ, LOCKDOWN_APPS=f"{d}/apps", LOCKDOWN_PACKAGES_XML=f"{d}/packages.xml", LOCKDOWN_PROC_NET=f"{d}/net", WLAN="wlan0")
    r = subprocess.run(["sh", f"{ROOT}/scripts/device/lockdown.sh", "print"], env=env, capture_output=True, text=True, timeout=30)
    return r.stdout.splitlines()


with tempfile.TemporaryDirectory() as d:
    os.makedirs(f"{d}/net")
    open(f"{d}/packages.xml", "w").write("<packages>\n" + "".join(PKG.format(**p) for p in [
        dict(n="com.kiosksatellite", c="/data/app/com.kiosksatellite-1", f=0, u=10050),
        dict(n="org.schabi.newpipe", c="/data/app/org.schabi.newpipe-1", f=0, u=10005),
        dict(n="com.amazon.dlna", c="/system/app/Dlna", f=1, u=10060)]) + "</packages>\n")
    def tables(tcp, udp, tcp6=""):
        open(f"{d}/net/tcp", "w").write(HEAD + tcp); open(f"{d}/net/udp", "w").write(HEAD + udp)
        open(f"{d}/net/tcp6", "w").write(HEAD + tcp6); open(f"{d}/net/udp6", "w").write(HEAD)
    tables(sock(0, 2324, "0A", 10050) + sock(1, 2326, "0A", 10050) + sock(2, 8080, "0A", 10005) + sock(3, 2326, "01", 10050)
           + sock(4, 26053, "0A", 1000) + sock(5, 9000, "0A", 10060),
           sock(0, 1900, "07", 10050) + sock(1, 5353, "07", 10050) + sock(2, 1900, "07", 10060),
           sock(0, 2325, "0A", 10050, v6=True))
    open(f"{d}/apps", "w").write("net org.schabi.newpipe\nlan com.kiosksatellite\nlan com.amazon.dlna\nlan bad;name\n")
    out = run(d)
    check("-A hassmic_out -m owner --uid-owner 10005 -j RETURN" in out, "net: the app's uid let out")
    check("-A INPUT -i wlan0 -j hassmic_in" in out, "INPUT hands on to hassmic_in")
    ins = [l for l in out if l.startswith("-A hassmic_in")]
    ports = sorted({(l.split(" -p ")[1].split()[0], int(l.split("--dport ")[1].split()[0])) for l in ins})
    check(ports == [("tcp", 2324), ("tcp", 2325), ("tcp", 2326), ("udp", 1900)],
          f"lan: the app's own listening TCP ports (IPv4 and IPv6 sockets) and its bound UDP 1900, nothing else ({ports})")
    check("-A hassmic_in -s 192.168.0.0/16 -i wlan0 -p tcp -m tcp --dport 2326 -j ACCEPT" in ins
          and "-A hassmic_in -s 169.254.0.0/16 -i wlan0 -p udp -m udp --dport 1900 -j ACCEPT" in ins,
          "lan: from the private and link-local ranges, worded as iptables -S prints them")
    check(not any("224.0.0.0" in l or "255.255.255.255" in l for l in ins), "lan: never from a multicast or broadcast address")
    check(not any("--dport 9000" in l for l in ins), "lan: a system app's ports never (Amazon's: not in /data/app)")
    check(not any("--dport 8080" in l for l in ins), "lan: an app only allowed the internet is not reachable")
    check(not any("--dport 5353" in l or "--dport 26053" in l for l in ins), "lan: nothing already open (mDNS, hassmic's range) again")
    # the renderer took the next port after a restart: the rules follow
    tables(sock(0, 2324, "0A", 10050) + sock(1, 2327, "0A", 10050), sock(0, 1900, "07", 10050))
    out = run(d)
    ports = sorted({int(l.split("--dport ")[1].split()[0]) for l in out if l.startswith("-A hassmic_in")})
    check(ports == [1900, 2324, 2327], f"lan: the port moved (2326 -> 2327), the rules with it ({ports})")
    open(f"{d}/apps", "w").write("net org.schabi.newpipe\n")
    out = run(d)
    check(not any(l.startswith("-A hassmic_in") for l in out), "lan off: nothing admitted")

print("lockdown: all ok" if not fails else f"lockdown: {fails} FAILED")
sys.exit(1 if fails else 0)
