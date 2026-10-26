#!/usr/bin/env python3
"""scripts/wifi-join.sh on a model with INSTALL=boot (checkers): the guided setup's Wi-Fi join through WifiService,
against a fake adb whose "shell" runs here, with the stand-ins of tests/fake_wifi_tools.py (app_process answering as
wifictl.dex over the fake framework, ifconfig with the address Android's DHCP gives).  A fresh Echo with nothing saved:
the PSK made on the PC, add retried while the radio comes up, a wrong password, an open network, a passphrase of the
wrong length, a radio that never comes up; and a network saved before by another user (a stock update over Wi-Fi),
which root may not change: as the system user through runas."""
import hashlib, json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
failed = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: failed.append(what)


H = lambda s: s.encode().hex()
PSK = lambda pw, ssid: hashlib.pbkdf2_hmac("sha1", pw.encode(), ssid.encode(), 4096, 32).hex()
HOME = "Home \"net\" ü"                                              # a quote and UTF-8: nothing may pass a shell

# runas for the fake Echo: its first two arguments are user and group; the uid reaches the fake framework
RUNAS = r'''#!/bin/sh
case "$1" in system) export FAKE_WIFI_UID=1000;; *) export FAKE_WIFI_UID=$1;; esac
shift 2; exec "$@"
'''

ADB = r'''#!/bin/sh
# adb for tests/fake_wifi_join.py: the Echo is this PC, its shell sh with the fake tools first in PATH
case "$1" in
get-state) echo device;;
push) exit 0;;
shell) shift
    case "$*" in "getprop ro.product.device") echo checkers; exit 0;; esac
    exec sh -c "$(printf %s "$*" | sed "s#/data/local/hassmic/#$FAKE_D/#g")";;     # the Echo's folder is a tmp one
*) echo "fake adb: $*" >&2; exit 1;;
esac
'''


def run(tmp, conf, world_extra=None, nets=None, runas=False):
    fake = os.path.join(tmp, "bin")
    os.makedirs(fake, exist_ok=True)
    d = os.path.join(tmp, "d")                                  # /data/local/hassmic
    os.makedirs(d, exist_ok=True)
    if runas:
        with open(os.path.join(d, "runas"), "w") as f: f.write(RUNAS)
        os.chmod(os.path.join(d, "runas"), 0o755)
    for t in ("app_process", "ifconfig"):
        p = os.path.join(fake, t)
        if not os.path.exists(p): os.symlink(f"{ROOT}/tests/fake_wifi_tools.py", p)
    with open(os.path.join(fake, "adb"), "w") as f: f.write(ADB)
    os.chmod(os.path.join(fake, "adb"), 0o755)
    world = os.path.join(tmp, "world.json")
    air = [{"hex": H(HOME), "psk": PSK("home password", HOME), "flags": "[WPA2-PSK-CCMP][ESS]",
            "bss": [["aa:00:00:00:00:01", 2437, -60]], "ip": "192.168.1.20", "gw": "192.168.1.1"},
           {"hex": H("Cafe"), "psk": None, "flags": "[ESS]", "bss": [["aa:00:00:00:00:02", 2412, -70]],
            "ip": "10.0.0.7", "gw": "10.0.0.1"}]
    w = {"networks": nets or [], "next_id": 0, "current": None, "target": None, "air": air, "saved": [],
         "arp_file": os.path.join(tmp, "arp"), "route_file": os.path.join(tmp, "route"),
         "lease": {"ip": "", "gw": ""}, "props": {}}
    w.update(world_extra or {})
    with open(world, "w") as f: json.dump(w, f)
    cf = os.path.join(tmp, "wifi.conf")
    with open(cf, "w") as f: f.write(conf)
    env = dict(os.environ, PATH=f"{fake}:{os.environ['PATH']}", FAKE_WIFI_WORLD=world, INSTALL="boot", WIFI_CONF=cf, FAKE_D=d,
               WIFI_JOIN_SECS="10", WIFI_ADD_SECS="4")
    env.pop("DEVICE", None); env.pop("ANDROID_SERIAL", None)
    p = subprocess.run(["sh", f"{ROOT}/scripts/wifi-join.sh"], env=env, capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout + p.stderr, json.load(open(world))


def main():
    tmp = tempfile.mkdtemp()
    rc, out, w = run(tmp, f"{HOME}\nhome password\n", {"radio_off": 2})
    adds = [c for c in w["calls"] if c.startswith("app_process add ")]
    net = w["networks"][0] if w["networks"] else {}
    check(rc == 0 and "ip_address=192.168.1.20" in out, f"joins, the address Android's DHCP gives ({out.strip()!r})")
    check(len(adds) == 3 and adds[-1] == f"app_process add {H(HOME)} {PSK('home password', HOME)}",
          "add retried while the radio comes up; the SSID as hex, the PSK made on the PC (64 hex)")
    check(net.get("psk") == PSK("home password", HOME) and net.get("key_mgmt") == "WPA-PSK" and w["current"] == net.get("id"),
          "saved as a WPA PSK, and on it")
    check("home password" not in " ".join(w["calls"]), "the passphrase itself reaches the Echo nowhere")
    check(any(c == "app_process wifi-on" for c in w["calls"]) and any(c.startswith("app_process select ") for c in w["calls"]),
          "radio on, then select")

    rc, out, w = run(tempfile.mkdtemp(), f"{HOME}\nwrong password\n")
    check(rc == 1 and "no address after 10 s" in out and "4WAY_HANDSHAKE" in out, f"a wrong password: no address, and why ({out.strip()!r})")

    rc, out, w = run(tempfile.mkdtemp(), "Cafe\n\n")
    check(rc == 0 and w["networks"][0].get("key_mgmt") == "NONE" and "app_process add %s -" % H("Cafe") in w["calls"],
          "no passphrase: an open network (-)")

    rc, out, w = run(tempfile.mkdtemp(), f"{HOME}\nshort\n")
    check(rc == 1 and "8 to 63" in out and not any(c.startswith("app_process add") for c in w.get("calls", [])),
          "a passphrase of 5 characters: refused on the PC, nothing added")

    rc, out, w = run(tempfile.mkdtemp(), f"{HOME}\nhome password\n", {"radio_off": 99})
    check(rc == 1 and "did not add the network" in out and "-1" in out, "a radio that never comes up: gives up, says what it got")

    # saved before by another user (uid 1010: Amazon's setup app, or the stock update's): root's add is refused (-1)
    old = [{"id": 0, "disabled": 0, "priority": 0, "ssid": H(HOME), "psk": PSK("old password", HOME), "key_mgmt": "WPA-PSK",
            "creator": 1010}]
    rc, out, w = run(tempfile.mkdtemp(), f"{HOME}\nhome password\n", {"next_id": 1}, nets=old, runas=True)
    net = w["networks"][0] if w["networks"] else {}
    check(rc == 0 and "ip_address=192.168.1.20" in out and len(w["networks"]) == 1 and net.get("psk") == PSK("home password", HOME),
          f"a network another user saved: updated as the system user, joined ({out.strip()!r})")
    rc, out, w = run(tempfile.mkdtemp(), f"{HOME}\nhome password\n", {"next_id": 1}, nets=old)
    check(rc == 1 and "did not add the network" in out, "the same without runas on the Echo: gives up, says what it got")

    if failed:
        print(f"FAILED: {len(failed)}"); sys.exit(1)
    print("PASS")


main()
