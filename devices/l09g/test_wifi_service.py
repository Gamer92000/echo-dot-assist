#!/usr/bin/env python3
"""Exercise Wi-Fi switching against a simulated wpa_supplicant and DHCP."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
SERVICE = ROOT / "devices/l09g/wifi-service.sh"
FAKE_WPA = ROOT / "tests/fake_wifi_tools.py"

STUB = r'''#!/usr/bin/env python3
import fcntl, json, os, sys
path=os.environ['FAKE_WIFI_WORLD']; tool=os.path.basename(sys.argv[0]); args=sys.argv[1:]
with open(path,'r+') as f:
    fcntl.flock(f,fcntl.LOCK_EX); w=json.load(f); rc=0; out=''
    n=next((x for x in w['networks'] if x['id']==w.get('current')),None)
    air=next((x for x in w['air'] if n and x['hex']==n['ssid']),None)
    if tool=='dhcpcd':
        if '-k' in args: w['lease']={'ip':'','gw':''}
        elif '-n' in args and air and air.get('dhcp',True): w['lease']={'ip':air['ip'],'gw':air['gw']}
    elif tool=='ip':
        ip=w['lease']['ip']; gw=w['lease']['gw']
        if 'addr' in args and ip: out=f'5: wlan0 inet {ip}/24 scope global wlan0'
        elif 'route' in args and 'default' in args and gw: out=f'default via {gw} dev wlan0'
        elif 'route' in args and ip: out=f'{ip.rsplit(".",1)[0]}.0/24 dev wlan0 scope link'
    elif tool=='ping': rc=0 if air and air.get('arp',True) else 1
    elif tool=='iptables':
        rules=w.setdefault('fw',[])
        lan=args[args.index('-d')+1] if '-d' in args else ''
        if '-C' in args: rc=0 if lan in rules else 1
        elif '-I' in args and lan not in rules: rules.append(lan)
        elif '-D' in args and lan in rules: rules.remove(lan)
    f.seek(0); json.dump(w,f); f.truncate()
    if out: print(out)
sys.exit(rc)
'''

def h(s): return s.encode().hex()
def psk(p, s): return hashlib.pbkdf2_hmac("sha1", p.encode(), s.encode(), 4096, 32).hex()

def scenario(root):
    home = {"id": 0, "ssid": h("Home"), "psk": psk("oldpassword", "Home"), "key_mgmt": "WPA-PSK", "priority": 3, "disabled": 0}
    other = {"id": 1, "ssid": h("Saved"), "psk": psk("savedpassword", "Saved"), "key_mgmt": "WPA-PSK", "priority": 1, "disabled": 0}
    p2p = {"id": 2, "ssid": h("DIRECT-x"), "key_mgmt": "WPA-PSK", "priority": 0, "disabled": 2}
    air = [
        {"hex": h("Home"), "psk": psk("oldpassword", "Home"), "flags": "[WPA2-PSK-CCMP][ESS]", "bss": [["aa:00:00:00:00:01", 2437, -60]], "ip": "192.168.1.20", "gw": "192.168.1.1"},
        {"hex": h("New"), "psk": psk("newpassword", "New"), "flags": "[WPA2-PSK-CCMP][ESS]", "bss": [["aa:00:00:00:00:02", 5180, -40]], "ip": "10.0.5.33", "gw": "10.0.5.1"},
        {"hex": h("NoDHCP"), "psk": None, "flags": "[ESS]", "bss": [["aa:00:00:00:00:03", 2462, -70]], "ip": "0", "gw": "0", "dhcp": False},
        {"hex": h("Silent"), "psk": None, "flags": "[ESS]", "bss": [["aa:00:00:00:00:04", 2462, -70]], "ip": "10.8.0.5", "gw": "10.8.0.1", "arp": False},
    ]
    return {"networks": [home, other, p2p], "saved": [home, other, p2p], "next_id": 3, "current": 0,
            "target": None, "air": air, "lease": {"ip": "192.168.1.20", "gw": "192.168.1.1"},
            "props": {}, "arp_file": str(root / "arp"), "fw": []}

def main():
    with tempfile.TemporaryDirectory(prefix="l09g-wifi-test-") as t:
        root = Path(t); bindir = root / "bin"; bindir.mkdir()
        fake = bindir / "wpa_cli"; shutil.copyfile(FAKE_WPA, fake); fake.chmod(0o755)
        for name in ("ip", "ping", "iptables", "dhcpcd"):
            f = bindir / name; f.write_text(STUB); f.chmod(0o755)
        state = root / "state"; out = root / "out"; state.mkdir(); out.mkdir()
        world = root / "world.json"; arp = root / "arp"; arp.write_text("")
        env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ["PATH"], FAKE_WIFI_WORLD=str(world),
                   HASSMIC_STATE=str(state), HASSMIC_WIFI=str(out), L09G_WIFI_ARP=str(arp),
                   L09G_WIFI_SCAN_WAIT="0", L09G_WIFI_STEP="0.01", L09G_WIFI_JOIN_TRIES="5",
                   L09G_WIFI_DHCP_TRIES="5", L09G_WIFI_BACK_TRIES="5")

        def run(req, initial=None):
            world.write_text(json.dumps(initial or scenario(root)))
            (state / "wifi-request").write_text(req + "\n")
            p = subprocess.run(["sh", str(SERVICE), "once"], env=env, capture_output=True, text=True, timeout=15)
            assert p.returncode == 0, p.stderr
            assert "newpassword" not in p.stdout and psk("newpassword", "New") not in p.stdout
            return json.loads(world.read_text()), p.stdout

        w, _ = run("scan 11")
        scan = (out / "scan").read_text()
        assert scan.startswith("id 11\n") and "New" in scan and "wpa_state=COMPLETED" in (out / "status").read_text()
        print("scan and current network: passed")

        w, _ = run(f"join 21 {h('New')} {psk('newpassword', 'New')}")
        assert (out / "result").read_text().startswith(f"21 ok {h('New')} 10.0.5.33 1")
        assert len(w["saved"]) == 4 and [n["disabled"] for n in w["saved"][:3]] == [0, 0, 2]
        assert w["current"] == 3 and w["saved"][3]["priority"] > 3
        assert w["fw"] == ["10.0.5.0/24"]
        print("successful switch, saved networks and local firewall: passed")

        for ssid, key, expected in (("New", psk("incorrect", "New"), "wrongkey"), ("NoDHCP", "-", "noaddress"), ("Silent", "-", "nogateway")):
            w, _ = run(f"join 31 {h(ssid)} {key}")
            assert f"failed {h(ssid)} {expected}" in (out / "result").read_text()
            assert w["current"] == 0 and len(w["networks"]) == 3 and len(w["saved"]) == 3
            assert [n["disabled"] for n in w["networks"]] == [0, 0, 2]
            assert w["lease"]["ip"] == "192.168.1.20" and w["fw"] == ["192.168.1.0/24"]
            print(f"{expected} and rollback: passed")

        initial = scenario(root); initial["save_fails"] = True
        w, _ = run(f"join 37 {h('New')} {psk('newpassword', 'New')}", initial)
        assert (out / "result").read_text().startswith(f"37 ok {h('New')} 10.0.5.33 0")
        assert len(w["saved"]) == 3
        print("save failure reported without replacing saved networks: passed")

        world.write_text(json.dumps(scenario(root)))
        (state / "wifi-request").write_text(f"join 39 {h('Missing')} -\n")
        slow_env = dict(env, L09G_WIFI_STEP="0.05", L09G_WIFI_JOIN_TRIES="80")
        p = subprocess.Popen(["sh", str(SERVICE), "once"], env=slow_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                if json.loads(world.read_text()).get("target") == 3: break
            except json.JSONDecodeError: pass
            time.sleep(0.02)
        else: p.kill(); raise AssertionError("switch did not reach test candidate")
        p.terminate(); p.communicate(timeout=10)
        w = json.loads(world.read_text())
        assert w["current"] == 0 and len(w["networks"]) == 3
        assert "failed " + h("Missing") + " interrupted" in (out / "result").read_text()
        print("interrupted switch rolls back: passed")

        w, _ = run("join 41 6161;reboot -")
        assert len(w["networks"]) == 3 and w["current"] == 0
        print("malformed request rejected: passed")

if __name__ == "__main__": main()
