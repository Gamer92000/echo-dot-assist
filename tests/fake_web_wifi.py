#!/usr/bin/env python3
"""Switching Wi-Fi networks from the settings page, end to end: build/hassmic-host (web.c, wifi.c) with a browser written
from the protocol (tests/webclient.py: the password sealed with Python's BLAKE2b), and root's side played by the real
scripts/device/wifi.sh against stand-ins for wpa_cli and friends (tests/fake_wifi_tools.py).  Scans with wpa_supplicant's
escapes, the PSK as PBKDF2 makes it, a wrong password, a network out of reach, no DHCP, a silent router, an open
network, a second password for a known network: what is saved, and that a failed switch goes back and saves nothing.
Run twice: the Dots' world (wpa_supplicant) and checkers' (INSTALL=boot: WifiService through the wifictl.dex
calls, the same fake world answering as the framework would, app_process included)."""
import hashlib, json, os, signal, stat, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import Browser, req

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = 16981


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False

H = lambda s: (s if isinstance(s, bytes) else s.encode()).hex()
PSK = lambda pw, ssid: hashlib.pbkdf2_hmac("sha1", pw.encode(), ssid if isinstance(ssid, bytes) else ssid.encode(), 4096, 32).hex()
KITCHEN = 'Kitchen "5G"\\'                                          # a quote and a backslash: wpa_supplicant escapes both
EXOTIC = b"Caf\xc3\xa9\tTab"                                         # UTF-8 and a tab: \t in wpa_cli's lines


def main(android=False):
    mode = "checkers' WifiService" if android else "the Dots' wpa_supplicant"
    print(f"--- {mode}")
    tmp = tempfile.mkdtemp(); state, out, fake = (os.path.join(tmp, d) for d in ("state", "wifi", "bin"))
    for d in (state, out, fake): os.makedirs(d)
    for t in ("wpa_cli", "app_process", "ifconfig", "getprop", "setprop", "dhcpcd", "ping", "stop", "start"):     # argv[0] tells the tool which one it is
        os.symlink(f"{ROOT}/tests/fake_wifi_tools.py", os.path.join(fake, t))
    world = os.path.join(tmp, "world.json"); arp = os.path.join(tmp, "arp"); route = os.path.join(tmp, "route")
    home = {"id": 0, "ssid": H("HomeNet"), "psk": PSK("old password", "HomeNet"), "key_mgmt": "WPA-PSK", "priority": 3, "disabled": 0}
    attic = {"id": 1, "ssid": H("Attic"), "psk": PSK("attic pass", "Attic"), "key_mgmt": "WPA-PSK", "priority": 0, "disabled": 0}
    p2p = {"id": 2, "ssid": H("DIRECT-xy"), "key_mgmt": "WPA-PSK", "priority": 0, "disabled": 2}       # a P2P group: not a network (wpa_supplicant's world only)
    nets = [home, attic] if android else [home, attic, p2p]
    air = [
        {"hex": H("HomeNet"), "psk": PSK("old password", "HomeNet"), "flags": "[WPA2-PSK-CCMP][ESS]", "bss": [["aa:00:00:00:00:01", 2437, -60]], "ip": "192.168.1.20", "gw": "192.168.1.1"},
        {"hex": H(KITCHEN), "psk": PSK("kitchen pass", KITCHEN), "flags": "[WPA2-PSK-CCMP][ESS]",
         "bss": [["aa:00:00:00:00:02", 2412, -70], ["aa:00:00:00:00:03", 5180, -48]], "ip": "10.0.5.33", "gw": "10.0.5.1"},
        {"hex": H(EXOTIC), "psk": None, "flags": "[ESS]", "bss": [["aa:00:00:00:00:04", 2462, -80]], "ip": "172.16.0.9", "gw": "172.16.0.1"},
        {"hex": H("Only3"), "psk": PSK("whatever!", "Only3"), "flags": "[RSN-SAE-CCMP][ESS]", "bss": [["aa:00:00:00:00:05", 5200, -50]], "ip": "10.9.9.9", "gw": "10.9.9.1"},
        {"hex": "", "psk": None, "flags": "[ESS]", "bss": [["aa:00:00:00:00:06", 2412, -40]], "ip": "10.8.8.8", "gw": "10.8.8.1"},           # hidden
        {"hex": H("NoDHCP"), "psk": None, "flags": "[ESS]", "bss": [["aa:00:00:00:00:07", 2412, -66]], "ip": "0", "gw": "0", "dhcp": False},
        {"hex": H("Silent"), "psk": None, "flags": "[ESS]", "bss": [["aa:00:00:00:00:08", 2412, -67]], "ip": "10.7.7.7", "gw": "10.7.7.1", "arp": False},
    ]
    with open(world, "w") as f:
        json.dump({"networks": nets, "next_id": 3 if android else 3, "current": 0, "target": None, "air": air,
                   "arp_file": arp, "route_file": route, "saved": list(nets) if android else [home, attic, p2p],
                   "lease": {"ip": "192.168.1.20", "gw": "192.168.1.1"}, "props": {"result": "ok", "reason": "BOUND"}}, f)
    with open(arp, "w") as f:                      # the router of the network it is on: an entry that stays (Dot 2)
        f.write("IP address       HW type     Flags       HW address            Mask     Device\n"
                "192.168.1.1      0x1         0x2         aa:bb:cc:00:00:01     *        wlan0\n")
    with open(route, "w") as f:                    # its default route, little-endian as /proc/net/route has it
        f.write("Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
                "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n")
    W = lambda: json.load(open(world))
    renv = dict(os.environ, PATH=f"{fake}:{os.environ['PATH']}", FAKE_WIFI_WORLD=world, WIFI_ARP=arp, WIFI_ROUTE=route,
                WIFI_SCAN_SECS="0", WIFI_JOIN_SECS="3", WIFI_ADDR_SECS="3", WIFI_BACK_SECS="3",
                **({"INSTALL": "boot"} if android else {}))
    root = lambda: subprocess.Popen(["sh", f"{ROOT}/scripts/device/wifi.sh", "take", state, out], env=renv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    rootlog = []
    def root_run():
        p = root(); o, _ = p.communicate(timeout=60); rootlog.append(o); return o

    log = os.path.join(tmp, "log")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_WIFI=out, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
               HASSMIC_PLAY=os.path.join(tmp, "play.raw"), HASSMIC_MDNS_FILE=os.path.join(tmp, "none"), HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_MODELS=os.path.join(tmp, "models"), HASSMIC_ADB_OPEN=os.path.join(tmp, "adb-open"), HASSMIC_LOG=log)
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", "16980", "-n", "Echo Wifi", "-L", "-z", "0", "-o", "0",
                             "-a", "16982", "-W", str(WEB)], env=env, stderr=open(log, "w"))
    reqf = os.path.join(state, "wifi-request")
    try:
        time.sleep(0.6)
        b = Browser(WEB)
        check(b.login_with_button(proc), "logged in with the action button")
        st, _, body = b.call("GET", "/api/wifi"); w = json.loads(body)
        check(st == 200 and w["current"] is None and w["scan"] is None and w["result"] is None and not w["busy"], "nothing known before root said anything")
        check(b.state().get("wifi", "missing") is None, "the page's state: wifi null while unknown")
        check(req(WEB, "GET", "/api/wifi")[0] == 401 and req(WEB, "POST", "/api/wifi/scan")[0] == 401, "unsigned: refused")

        # ---- scan
        st, _, body = b.call("POST", "/api/wifi/scan"); sid = json.loads(body).get("id")
        check(st == 200 and open(reqf).read() == f"scan {sid}\n", "scan: a request for root")
        st, _, body = b.call("POST", "/api/wifi/scan")
        check(st == 200 and json.loads(body).get("id") == sid, "asked again before root took it: the same scan")
        st, _, body = b.call("POST", "/api/wifi/join", f"{H('X')} -".encode())
        check(st == 409, "a switch while a request waits: 409")
        root_run()
        check(not os.path.exists(reqf) and not os.path.exists(os.path.join(out, "lock")), "root took the request and let go of the lock")
        w = json.loads(b.call("GET", "/api/wifi")[2]); nets = {n["ssid"]: n for n in w["scan"]["networks"]}
        check(w["scan"]["id"] == sid and w["current"]["ssid"] == "HomeNet" and w["current"]["ip"] == "192.168.1.20" and w["current"]["state"] == "COMPLETED",
              f"scan answered, with the network the Echo is on: {w['current']}")
        k = nets.get(KITCHEN, {})
        check(k.get("hex") == H(KITCHEN) and k.get("signal") == -48 and k.get("band") == "both" and k.get("sec") == "psk",
              "escaped name decoded; two access points of one network merged: strongest signal, both bands")
        e = nets.get(EXOTIC.decode(), {})
        check(e.get("hex") == H(EXOTIC) and e.get("sec") == "open" and nets.get("Only3", {}).get("sec") == "sae", "UTF-8 and \\t decoded, open and WPA3-only told apart")
        check(len(nets) == 6 and [n["signal"] for n in w["scan"]["networks"]] == sorted((n["signal"] for n in w["scan"]["networks"]), reverse=True),
              "the hidden one left out, strongest first")
        check(b.state()["wifi"] == {"ssid": "HomeNet", "ip": "192.168.1.20"}, "the page's state: the network now")

        # ---- the password, sealed
        def join(ssid, pw, ctr_body=None):
            def body(ctr):
                if pw is None: return f"{H(ssid)} -".encode()
                p = (pw.encode() if isinstance(pw, str) else pw).ljust(64, b"\0")
                return f"{H(ssid)} {b.seal(ctr, p).hex()}".encode()
            st, _, data = b.call("POST", "/api/wifi/join", body)
            return st, json.loads(data)
        st, r = join(KITCHEN, "short")
        check(st == 400 and "8 to 63" in r.get("error", ""), "a password of 5 characters: refused")
        st, r = join(KITCHEN, "pässword123")
        check(st == 400 and "ASCII" in r.get("error", ""), "not ASCII: refused")
        st, r = join("x" * 33, "kitchen pass")
        check(st == 400 and "32" in r.get("error", ""), "a name of 33 bytes: refused")
        check(b.call("POST", "/api/wifi/join", f"{H(KITCHEN)} zz".encode())[0] == 400, "not hex: bad request")
        st, r = join(KITCHEN, "wrong password")
        line = open(reqf).read().split()
        check(st == 200 and line[:3] == ["join", str(r["id"]), H(KITCHEN)] and line[3] == PSK("wrong password", KITCHEN),
              "the PSK that PBKDF2 makes from the password and the name goes to root, not the password")
        check(stat.S_IMODE(os.stat(reqf).st_mode) == 0o600 and b"wrong password" not in open(log, "rb").read(), "the request 0600, the password in no log")

        # ---- wrong password: back, nothing saved
        p = root(); seen = None
        for _ in range(40):
            time.sleep(0.15)
            w = json.loads(b.call("GET", "/api/wifi")[2])
            if w["result"] and w["result"]["state"] == "switching": seen = w; break
        st, _ = join("Other", None)
        check(seen and seen["busy"] and seen["result"]["id"] == r["id"] and seen["result"]["ssid"] == KITCHEN and st == 409,
              "while root switches: busy, the switch named, a second one refused (409)")
        o, _ = p.communicate(timeout=60); rootlog.append(o)
        w = json.loads(b.call("GET", "/api/wifi")[2]); res = w["result"]; wd = W()
        check(res["state"] == "failed" and res["reason"] == "wrongkey" and res["back"] == "HomeNet" and not w["busy"],
              f"wrong password: failed at the key handshake, back on HomeNet ({res})")
        if android:
            check(wd["current"] == 0 and [n["id"] for n in wd["networks"]] == [0, 1] and [n["disabled"] for n in wd["networks"]] == [0, 0]
                  and not any(c.endswith("save_config") for c in wd["calls"])
                  and any(c == "app_process enable 0 keep" for c in wd["calls"]),
                  "the new network removed, the old ones enabled again (the framework's store is the saved state)")
        else:
            check(wd["current"] == 0 and [n["id"] for n in wd["networks"]] == [0, 1, 2] and [n["disabled"] for n in wd["networks"]] == [0, 0, 2]
                  and wd["saved"] == [home, attic, p2p] and not any(c.endswith("save_config") for c in wd["calls"]),
                  "the new network removed, the others enabled again, the saved configuration never touched")

        # ---- out of reach, no address, a silent router
        for name, why in (("Nowhere", "notfound"), ("NoDHCP", "noaddress"), ("Silent", "nogateway")):
            st, r = join(name, None); root_run()
            res = json.loads(b.call("GET", "/api/wifi")[2])["result"]; wd = W()
            check(st == 200 and res["id"] == r["id"] and res["state"] == "failed" and res["reason"] == why and res["back"] == "HomeNet"
                  and wd["current"] == 0 and wd["saved"] == ([home, attic] if android else [home, attic, p2p]),
                  f"{name}: {why}, back on HomeNet, nothing saved")

        # ---- it works
        st, r = join(KITCHEN, "kitchen pass"); root_run()
        w = json.loads(b.call("GET", "/api/wifi")[2]); res = w["result"]; wd = W()
        new = next((n for n in wd["saved"] if n["ssid"] == H(KITCHEN)), {})
        check(res["state"] == "ok" and res["ip"] == "10.0.5.33" and res["saved"] and w["current"]["ssid"] == KITCHEN,
              f"the right password: on {KITCHEN}, address 10.0.5.33, saved")
        check(new.get("psk") == PSK("kitchen pass", KITCHEN) and new.get("key_mgmt") == "WPA-PSK" and new.get("scan_ssid") == 1,
              "saved as a PSK, found even when hidden")
        check([n["ssid"] for n in wd["saved"]] == ([H(KITCHEN)] if android else [H("DIRECT-xy"), H(KITCHEN)]) and new.get("disabled") == 0,
              "the networks from before forgotten once it works (the P2P group left alone)")

        # ---- the same network with another password (the router's changed): replaced, not added
        st, r = join(KITCHEN, "f" * 64); assert open(reqf).read().split()[3] == "f" * 64
        with open(world) as f: wd = json.load(f)
        next(a for a in wd["air"] if a["hex"] == H(KITCHEN))["psk"] = "f" * 64
        with open(world, "w") as f: json.dump(wd, f)
        root_run(); wd = W()
        k = [n for n in wd["saved"] if n["ssid"] == H(KITCHEN)]
        check(len(k) == 1 and k[0]["psk"] == "f" * 64 and json.loads(b.call("GET", "/api/wifi")[2])["result"]["state"] == "ok",
              "the PSK as 64 hex digits taken as it is; the network's older entry replaced")

        # ---- an open network with an odd name
        st, r = join(EXOTIC, None); root_run(); wd = W()
        e = next((n for n in wd["saved"] if n["ssid"] == H(EXOTIC)), {})
        res = json.loads(b.call("GET", "/api/wifi")[2])["result"]
        check(res["state"] == "ok" and e.get("key_mgmt") == "NONE" and "psk" not in e and res["ssid"] == EXOTIC.decode(), "open network: key_mgmt NONE")

        # ---- a save that fails: on it, but said so (wpa_supplicant's world: the framework saves as it goes)
        if not android:
            with open(world) as f: wd = json.load(f)
            wd["save_fails"] = True
            with open(world, "w") as f: json.dump(wd, f)
            join("HomeNet", "old password"); root_run()
            res = json.loads(b.call("GET", "/api/wifi")[2])["result"]
            check(res["state"] == "ok" and res["saved"] is False and any("could not be written" in o for o in rootlog), "save_config failing: ok, but not saved, and said so")

        # ---- replay, root not there, a run that died
        ctr = b.ctr + 1
        bd = f"{H('HomeNet')} {b.seal(ctr, b'old password'.ljust(64, bytes(1))).hex()}".encode()
        check(b.call("POST", "/api/wifi/join", bd, ctr=ctr)[0] == 200 and b.call("POST", "/api/wifi/join", bd, ctr=ctr)[0] == 401, "a recorded join does not count twice")
        b.ctr = ctr
        with open(os.path.join(out, "result"), "w") as f: f.write(f"77 switching {H('HomeNet')}\n")
        check(json.loads(b.call("GET", "/api/wifi")[2])["result"]["state"] == "interrupted", "\"switching\" without the lock: interrupted (a reboot)")
        os.makedirs(os.path.join(out, "lock")); open(os.path.join(out, "lock", "pid"), "w").write("999999\n")
        subprocess.run(["sh", f"{ROOT}/scripts/device/wifi.sh", "clean", state, out], env=renv, check=True)
        check(not os.path.exists(reqf) and not os.path.exists(os.path.join(out, "lock")), "clean (firewall service start): the old request and the dead run's lock gone")
        with open(reqf, "w") as f: f.write("join 5 6869;reboot ffff\n")
        o = root_run()
        check("not understood" in o and not os.path.exists(reqf) and not any("reboot" in c for c in W()["calls"][-3:]), "a request that is not hex: dropped")
    finally:
        proc.send_signal(signal.SIGTERM); proc.wait()
    if check.failed:
        print(open(log).read()); print("".join(rootlog)); print(f"FAILED ({mode})"); raise SystemExit(1)


for android in (False, True):
    main(android)
print("PASS")
