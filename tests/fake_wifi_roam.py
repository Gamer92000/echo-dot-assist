#!/usr/bin/env python3
"""Roaming between the access points of one network (issue #234): root's scripts/device/roam.sh, look by look as
main.sh's netwatch calls it, against the stand-in wpa_cli (tests/fake_wifi_tools.py) with a mesh of three access points.
What the Dots showed (2026-11-25) is in the world: Amazon's BSS picker going elsewhere unless the network is pinned to
the target, a Dot 5's extra country column in scan_results, an access point that takes BSS transition queries.  Held:
nothing while strong; weak for 30 s, then a scan of every channel first, of the network's own channels after; a move only
to one 8 dB stronger and at least -67 dBm, pinned for the move and unpinned after (also a pin a killed netwatch left); a
target that refuses left alone; the 802.11v query where the access point offers it, and its move told apart; a network
the owner pinned left alone; the setting off; wifi.sh's lock; a link lost and joined elsewhere recorded; the record
for the page (wifi.c reads it)."""
import json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False

H = lambda s: s.encode().hex()
A, B, C, D = "aa:00:00:00:00:01", "aa:00:00:00:00:02", "aa:00:00:00:00:03", "aa:00:00:00:00:04"
ODD = 'My "Mesh"\\ 2'                 # a quote, a backslash, spaces: wpa_cli escapes the first two, splits on none
# Extended Capabilities with bit 19 (BSS transition): element 127, 3 octets, the third 0x08
BTM_IE = "0003616263" + "7f03000008"

# the shell side: roam.sh sourced as main.sh does, one look per line read, each answered by its output and a mark
DRIVER = r'''
WIFI=$1; WIFI_SERVICE=wifisvc; CFG=$2
wpa() { wpa_cli -i wlan0 -p /nonexistent "$@" 2>/dev/null; }
cfg() { echo $CFG; }
. "$3"
while read -r cmd; do
    case $cmd in
    look) roam_look;;
    vars) echo "weak=$rm_weak wait=$rm_wait freqs=$rm_freqs next=$rm_next bad=$rm_bad";;
    esac
    echo "--end"
done
'''


class Echo:
    def __init__(self, tmp, world):
        self.tmp, self.world = tmp, os.path.join(tmp, "world.json")
        self.wifi, self.cfg = os.path.join(tmp, "wifi"), os.path.join(tmp, "config")
        os.makedirs(self.wifi, exist_ok=True)
        open(self.cfg, "w").close()
        fake = os.path.join(tmp, "bin"); os.makedirs(fake, exist_ok=True)
        if not os.path.exists(os.path.join(fake, "wpa_cli")): os.symlink(f"{ROOT}/tests/fake_wifi_tools.py", os.path.join(fake, "wpa_cli"))
        self.put(world)
        drv = os.path.join(tmp, "drive.sh"); open(drv, "w").write(DRIVER)
        env = dict(os.environ, PATH=fake + ":" + os.environ["PATH"], FAKE_WIFI_WORLD=self.world)
        # the Echo's shell is mksh (R52): taken where the PC has it (CI installs it), else bash; never dash, which has
        # neither ${v:o:n} nor 16# (Ubuntu's sh)
        self.p = subprocess.Popen([shutil.which("mksh") or "bash", drv, self.wifi, self.cfg, f"{ROOT}/scripts/device/roam.sh"], env=env, text=True,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def get(self): return json.load(open(self.world))
    def put(self, w): json.dump(w, open(self.world, "w"))
    def edit(self, f): w = self.get(); f(w); self.put(w)

    def cmd(self, c):
        self.p.stdin.write(c + "\n"); self.p.stdin.flush(); out = []
        while True:
            l = self.p.stdout.readline()
            if not l or l.rstrip("\n") == "--end": return "\n".join(out)
            out.append(l.rstrip("\n"))

    def look(self, n=1):
        """n looks; the world's calls made meanwhile (wpa_cli's argv, its socket options left out) and the lines said"""
        self.edit(lambda w: w.__setitem__("calls", []))
        said = [self.cmd("look") for _ in range(n)]
        calls = [" ".join(c.split()[5:]) for c in self.get()["calls"] if c.startswith("wpa_cli")]
        return calls, "\n".join(s for s in said if s)

    def until(self, done, n):
        """up to n looks, until DONE(calls, said) of all of them so far holds"""
        calls, said = [], ""
        for _ in range(n):
            c, w = self.look(); calls += c; said += ("\n" if said and w else "") + w
            if done(calls, said): break
        return calls, said

    def record(self):
        try: return open(os.path.join(self.wifi, "roam")).read().splitlines()
        except FileNotFoundError: return []

    def close(self): self.p.stdin.close(); self.p.wait(5)


def world(sig, country=False, ssid="Mesh", extra=(), **kw):
    net = {"id": 0, "ssid": H(ssid), "psk": "\"meshmesh\"", "key_mgmt": "WPA-PSK", "priority": 1, "disabled": 0}
    air = [{"hex": H(ssid), "psk": None, "flags": "[WPA2-PSK-CCMP][ESS]", "ip": "192.168.1.20", "gw": "192.168.1.1",
            "bss": [[A, 5500, sig[0]], [B, 2437, sig[1]], [C, 2462, sig[2]]]},
           {"hex": H("Neighbours"), "psk": None, "flags": "[WPA2-PSK-CCMP][ESS]", "ip": "10.0.0.2", "gw": "10.0.0.1",
            "bss": [["bb:00:00:00:00:01", 2412, -40]]}, *extra]
    # the fake's association takes the network's psk as given when the air names none: on it from the start
    net["key_mgmt"] = "NONE"
    w = {"networks": [net], "next_id": 1, "current": 0, "target": None, "air": air, "bssid": A, "arp_file": "/dev/null",
         "route_file": "/dev/null", "lease": {"ip": "192.168.1.20", "gw": "192.168.1.1"}, "props": {}}
    if country: w["country"] = "DE"
    w.update(kw)
    return w


def set_sig(e, i, v): e.edit(lambda w: w["air"][0]["bss"][i].__setitem__(2, v))


def main():
    tmp = tempfile.mkdtemp(prefix="hassmic-roam.")
    try:
        print("--- strong, then weak: scans, the move pinned (Amazon's picker would go elsewhere)")
        e = Echo(os.path.join(tmp, "a"), world([-60, -62, -80], country=True, picker=C, ssid=ODD,       # a guest network of the same
                 extra=[{"hex": H(ODD), "psk": None, "flags": "[RSN-SAE-CCMP][ESS]", "ip": "0", "gw": "0",   # name, WPA3 only: never a candidate
                         "bss": [[D, 5180, -40]]}]))
        calls, said = e.look()
        check(f"Wi-Fi on {A} (5500 MHz, -60 dBm)" in said, "the access point said at the first look")
        rec = e.record()
        check(rec and rec[0].startswith(f"ap {A} 5500 "), "recorded for the page: the access point and its channel")
        calls, said = e.look(10)
        check(not any(c.startswith(("scan", "roam", "wnm")) for c in calls), "strong (-60 dBm): no scan, no move")
        set_sig(e, 0, -75)
        calls, said = e.look(2)
        check(not any(c.startswith("scan") for c in calls), "weak for 20 s: not yet")
        calls, said = e.look()
        check("scan" in calls, "weak for 30 s: a scan of every channel first (no channel known but its own)")
        check(not any(c.startswith("wnm_bss_query") for c in calls), "no BSS transition query: the access point does not offer it")
        calls, said = e.look()
        i_pin = next((i for i, c in enumerate(calls) if c == f"set_network 0 bssid {B}"), -1)
        i_roam = next((i for i, c in enumerate(calls) if c == f"roam {B}"), -1)
        i_any = next((i for i, c in enumerate(calls) if c == "set_network 0 bssid any"), -1)
        check(0 <= i_pin < i_roam < i_any, "the move: pinned to the target, roam, unpinned (in that order)")
        w = e.get()
        check(w["bssid"] == B, "landed on the target, not where the picker would have gone; not on the WPA3-only one of the same name (-40 dBm)")
        check("bssid" not in w["networks"][0], "nothing left pinned")
        check(not os.path.exists(os.path.join(e.wifi, "roam-pin")), "the pin's mark gone")
        check(f"Wi-Fi moved to {B} (2437 MHz, -62 dBm) from {A} (-75 dBm) in " in said, "the move said, with both signals: " + said)
        rec = e.record()
        check(rec[0].startswith(f"ap {B} 2437 ") and len(rec) == 2 and rec[1].split()[2:9] == ["roam", A, "5500", "-75", B, "2437", "-62"],
              "recorded: the new access point, and the move (roam, from, to, signals)")
        calls, said = e.look()
        check(not said, "no second line for the move the next look")
        print("--- the network's channels only, after the first scan; nothing strong enough")
        set_sig(e, 1, -74); set_sig(e, 0, -70); set_sig(e, 2, -69)
        calls, said = e.look(9)
        scans = [c for c in calls if c.startswith("scan") and c != "scan_results"]
        check(scans and scans[0] == "scan freq=5500,2437,2462" or scans == ["scan freq=2437,5500,2462"] or
              (scans and scans[0].startswith("scan freq=") and set(scans[0][10:].split(",")) == {"5500", "2437", "2462"}),
              "the next scan: the network's own channels alone (the neighbours' 2412 left out): " + str(scans))
        check(not any(c.startswith("roam") for c in calls), "5 dB stronger (-69 against -74): no move")
        check("no access point of the network 8 dB stronger (best: aa:00:00:00:00:03 -69 dBm)" in said, "said why, once")
        calls, said = e.look(6)
        check(len([c for c in calls if c.startswith("scan ")]) == 1 and "no access point" not in said,
              "a minute on: one scan more, nothing said again: " + str([c for c in calls if c.startswith("scan")]))
        calls, said = e.look(36)
        n = [i for i, c in enumerate(calls) if c.startswith("scan ") or c == "scan"]
        check(len(n) == 2, f"nothing found again: the next scans 2 and 4 minutes apart (backing off), not every minute: {len(n)} in 6 min")
        set_sig(e, 1, -60)
        e.look(); set_sig(e, 1, -74)
        calls, said = e.look(4)
        check(sum(c.startswith("scan") and c != "scan_results" for c in calls) == 1, "strong once in between: back to a scan after 30 s")
        print("--- a target that does not take: left alone; a candidate under -67 dBm: none")
        set_sig(e, 0, -64)
        e.edit(lambda w: w.__setitem__("refuse", [A]))
        calls, said = e.until(lambda c, w: "did not take" in w, 12)
        check(f"roam {A}" in calls and "did not take" in said and e.get()["bssid"] == B, "the move that did not take: said, still on the old one")
        check("bssid" not in e.get()["networks"][0], "unpinned after a move that failed too")
        calls, said = e.look(12)
        calls2, said = e.look(48)
        check(not any(c == f"roam {A}" for c in calls + calls2), "the refused target not tried again for 10 minutes")
        calls, said = e.until(lambda c, w: "did not take" in w, 30)
        check(f"roam {A}" in calls and "left alone for 20 min" in said, "after them tried again; refused again: left alone twice as long")
        e.edit(lambda w: w.__setitem__("refuse", []))
        set_sig(e, 2, 50)                                   # a level a driver makes up in a scan (SIGNAL_UNSPEC's 0..100)
        set_sig(e, 0, -90)
        calls, said = e.look(30)
        check(not any(c.startswith("roam") for c in calls), "a scan level that is no dBm (+50): no candidate")
        set_sig(e, 1, -9999)                                           # a level a driver makes up
        calls, said = e.look(30)
        check(not any(c.startswith(("scan", "roam")) for c in calls), "a level a driver makes up (-9999 dBm): nothing done on it")
        set_sig(e, 1, -82); set_sig(e, 2, -68); set_sig(e, 0, -90)
        calls, said = e.look(14)
        check(any(c.startswith("scan ") for c in calls) and not any(c.startswith("roam") for c in calls),
              "14 dB stronger but under -67 dBm (-68 against -82): no move")
        e.close()

        print("--- 802.11v: the access point asked first; its move told apart")
        e = Echo(os.path.join(tmp, "b"), world([-76, -60, -90], ies={A: BTM_IE}, btm_to=B))
        e.look(6)                                      # the first look's access point, and its minute to settle
        calls, said = e.look()
        check(calls.count("wnm_bss_query 16") == 1 and not any(c.startswith("scan") for c in calls),
              "weak for 30 s, the access point offers BSS transition: the query (reason 16, low RSSI), no scan yet")
        calls, said = e.look()
        check(f"Wi-Fi on {B}" in said and "moved by the access point (802.11v)" in said, "the access point's move said as its")
        rec = e.record()
        check(rec[-1].split()[2:4] == ["ap", A], "recorded as the access point's move")
        e.close()
        e = Echo(os.path.join(tmp, "c"), world([-76, -60, -90], ies={A: BTM_IE}))
        e.look(7)
        calls, said = e.look()
        check("scan" in calls and not any(c.startswith("wnm") for c in calls), "the access point did not move it: the scan the look after")
        calls, said = e.look()
        check(e.get()["bssid"] == B, "and the move")
        calls, said = e.look(20)
        check(not any(c.startswith("wnm") for c in calls), "asked once per access point")
        e.close()

        print("--- left alone: the setting off, the owner's pin, wifi.sh's lock; a pin a killed netwatch left")
        e = Echo(os.path.join(tmp, "d"), world([-80, -50, -90]))
        open(e.cfg, "w").write("wifi_roaming=off\n")
        calls, said = e.look(10)
        check(not any(c.startswith(("scan", "roam")) for c in calls) and e.record(), "setting off: no scan, no move; the access point still recorded")
        open(e.cfg, "w").write("wifi_roaming=on\n")
        e.edit(lambda w: w["networks"][0].__setitem__("bssid", A))
        calls, said = e.look(10)
        check(not any(c.startswith(("scan", "roam")) for c in calls) and e.get()["networks"][0].get("bssid") == A,
              "a network the owner pinned to one access point: left there, the pin kept")
        e.edit(lambda w: w["networks"][0].pop("bssid"))
        os.makedirs(os.path.join(e.wifi, "lock"))
        calls, said = e.look(10)
        check(calls == [], "while wifi.sh works (its lock): not a word to wpa_supplicant")
        os.rmdir(os.path.join(e.wifi, "lock"))
        e.edit(lambda w: w["networks"][0].__setitem__("bssid", C))
        open(os.path.join(e.wifi, "roam-pin"), "w").write(f"0 {C}\n")
        calls, said = e.look()
        check(calls[:2] == ["get_network 0 bssid", "set_network 0 bssid any"] and "bssid" not in e.get()["networks"][0] and
              not os.path.exists(os.path.join(e.wifi, "roam-pin")), "a pin left by a netwatch that ended mid-move: taken off first")
        calls, said = e.look(4)
        check(e.get()["bssid"] == B, "then roams as usual")
        e.edit(lambda w: w["networks"][0].__setitem__("bssid", C))
        open(os.path.join(e.wifi, "roam-pin"), "w").write(f"0 {A}\n")       # a mark of another pin (a reboot, ids anew)
        calls, said = e.look()
        check(e.get()["networks"][0].get("bssid") == C and not os.path.exists(os.path.join(e.wifi, "roam-pin")),
              "a mark that names another access point than the network's pin: the pin left (not ours), the mark gone")
        e.edit(lambda w: w["networks"][0].pop("bssid"))
        print("--- a link lost and joined elsewhere: recorded as the supplicant's own")
        e.edit(lambda w: w.__setitem__("bssid", C))
        calls, said = e.look()
        check(f"Wi-Fi on {C} (2462 MHz, -90 dBm), was {B}" in said and e.record()[-1].split()[2:4] == ["self", B], "said and recorded (self)")
        e.edit(lambda w: (w.__setitem__("current", None), w.__setitem__("target", None), w["networks"][0].__setitem__("disabled", 1)))
        calls, said = e.look(5)
        check(not any(c.startswith(("scan", "roam")) for c in calls), "off the network: nothing")
        e.close()

        print("--- an element cut short; another network (wifi.sh's switch)")
        e = Echo(os.path.join(tmp, "e"), world([-76, -60, -90], ies={A: "00036162637f0500"},
                 extra=[{"hex": H("Attic"), "psk": None, "flags": "[WPA2-PSK-CCMP][ESS]", "ip": "10.1.1.2", "gw": "10.1.1.1",
                         "bss": [["cc:00:00:00:00:01", 2412, -50]]}]))
        calls, said = e.until(lambda c, w: any(x.startswith("roam") for x in c), 12)
        check(e.get()["bssid"] == B and not any(x.startswith("wnm") for x in calls),
              "Extended Capabilities claiming more than the dump has: no query, no end of the loop, the move all the same")
        e.edit(lambda w: (w["networks"].append({"id": 1, "ssid": H("Attic"), "key_mgmt": "NONE", "priority": 0, "disabled": 0}),
                          w.__setitem__("current", 1), w.__setitem__("bssid", None)))
        calls, said = e.look()
        rec = e.record()
        check(rec[0].startswith("ap cc:00:00:00:00:01 2412 ") and not any(" self " in l and "cc:00" in l for l in rec[1:]),
              "a switch to another network: its access point recorded, no move between networks")
        e.close()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED" if check.failed else "all passed")
    sys.exit(1 if check.failed else 0)


main()
