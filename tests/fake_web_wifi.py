#!/usr/bin/env python3
"""Switching Wi-Fi networks from the settings page, end to end: build/hassmic-host (web.c, wifi.c) with a browser written
from the protocol (tests/webclient.py: the password sealed with Python's BLAKE2b), and root's side played by the real
scripts/device/wifi.sh against stand-ins for wpa_cli and friends (tests/fake_wifi_tools.py).  Scans with wpa_supplicant's
escapes, the PSK as PBKDF2 makes it, a wrong password, a network out of reach, no DHCP, a silent router, an open
network, a second password for a known network: what is saved, and that a failed switch goes back and saves nothing.
A fixed address (issue #191): hassmic's refusals, one kept once a connection reached it, every way back (taken by
another device, a silent router, nothing reaching it, a link from before that proves nothing), netwatch's keep (the
Dots: dhcpcd-static over wifisvc's dhcpcd-wlan0), DHCP again, and a switch of network around a fixed address.
Run twice: the Dots' world (wpa_supplicant) and checkers' (ANDROID=1: WifiService through the wifictl.dex
calls, the same fake world answering as the framework would, app_process included).  The Dots' run also holds issue #90:
main.sh's psk_only (the entry wifisvc rewrites, cut back) and the setup's scripts/device/wifi-join.sh on WPA3."""
import shutil, hashlib, json, os, re, signal, socket, stat, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser, req

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
    for t in ("wpa_cli", "app_process", "ifconfig", "getprop", "setprop", "dhcpcd", "ping", "stop", "start", "ip"):     # argv[0] tells the tool which one it is
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
                   "lease": {"ip": "192.168.1.20", "gw": "192.168.1.1"}, "props": {"result": "ok", "reason": "BOUND"},
                   **({} if android else {"svc": {"dhcpcd-wlan0": "running"}, "carrier": True})}, f)   # init's dhcpcd, on a network
    with open(arp, "w") as f:                      # the router of the network it is on: an entry that stays (Dot 2)
        f.write("IP address       HW type     Flags       HW address            Mask     Device\n"
                "192.168.1.1      0x1         0x2         aa:bb:cc:00:00:01     *        wlan0\n")
    with open(route, "w") as f:                    # its default route, little-endian as /proc/net/route has it
        f.write("Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
                "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n")
    W = lambda: json.load(open(world))
    renv = dict(os.environ, PATH=f"{fake}:{os.environ['PATH']}", FAKE_WIFI_WORLD=world, WIFI_ARP=arp, WIFI_ROUTE=route,
                WIFI_SCAN_SECS="0", WIFI_STEP="0.1", WIFI_JOIN_SECS="6", WIFI_ADDR_SECS="5", WIFI_BACK_SECS="6",
                WIFI_TCP=os.path.join(tmp, "tcp"), WIFI_REACH_SECS="8",
                **({"ANDROID": "1"} if android else {}))     # looks: the stand-ins answer at the third
    # root's folder: wifi.sh with what it finds next to it (wifictl.dex, runas: a stand-in that hands the uid to the fake
    # framework)
    dev = os.path.join(tmp, "dev"); os.makedirs(dev)
    for f in ("wifi.sh", "wifictl.dex"): shutil.copy(f"{ROOT}/scripts/device/{f}", dev)
    with open(os.path.join(dev, "runas"), "w") as f:
        f.write('#!/bin/sh\ncase "$1" in system) export FAKE_WIFI_UID=1000;; *) export FAKE_WIFI_UID=$1;; esac\nshift 2; exec "$@"\n')
    os.chmod(os.path.join(dev, "runas"), 0o755)
    root = lambda: subprocess.Popen(["sh", f"{dev}/wifi.sh", "take", state, out], env=renv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
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
        wait_port(16980, WEB)
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
        check(b.state()["wifi"] == {"ssid": "HomeNet", "ip": "192.168.1.20", "fixed": False}, "the page's state: the network now")

        # ---- a fixed address (issue #191)
        tcp = renv["WIFI_TCP"]
        def links(*rows):                          # /proc/net/tcp: local and remote "a.b.c.d:port" in hex, state
            hx = lambda a: "".join("%02X" % int(x) for x in reversed(a.split(":")[0].split("."))) + ":%04X" % int(a.split(":")[1])
            with open(tcp, "w") as f:
                f.write("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n")
                for i, (l, r, st) in enumerate(rows):
                    f.write(f"   {i}: {hx(l)} {hx(r)} {st} 00000000:00000000 00:00000000 00000000  1000        0 {1000 + i} 1 0000000000000000\n")
        links(("192.168.1.20:28931", "192.168.1.77:50100", "01"))
        def addr(spec):
            st, _, data = b.call("POST", "/api/wifi/address", spec.encode()); return st, json.loads(data)
        def wifi(): return json.loads(b.call("GET", "/api/wifi")[2])
        for spec, word in (("fixed 192.168.1.50 24 10.0.0.1 192.168.1.1 -", "router is not in"),
                           ("fixed 192.168.1.0 24 192.168.1.1 192.168.1.1 -", "network's own"),
                           ("fixed 192.168.1.255 24 192.168.1.1 192.168.1.1 -", "network's own"),
                           ("fixed 192.168.1.50 31 192.168.1.1 192.168.1.1 -", "subnet mask"),
                           ("fixed 192.168.01.50 24 192.168.1.1 192.168.1.1 -", "not one a device"),
                           ("fixed 127.0.0.5 8 127.0.0.1 192.168.1.1 -", "not one a device"),
                           ("fixed 192.168.1.1 24 192.168.1.1 192.168.1.1 -", "router's own"),
                           ("fixed 192.168.1.50 24 192.168.1.1 224.0.0.1 -", "DNS"),
                           ("fixed 192.168.1.50 24 192.168.1.1", "bad request"), ("auto now", "bad request"), ("dhcp", "bad request")):
            st, r = addr(spec)
            check(st == 400 and word in r.get("error", "") and not os.path.exists(reqf), f"refused by hassmic: {spec!r} ({r.get('error')})")
        w = wifi()
        check(w["current"]["netmask"] == "255.255.255.0" and w["current"]["gateway"] == "192.168.1.1" and w["address"] == {"mode": "auto"}
              and w["address_result"] is None, f"what the page fills in: the mask and router the Echo has; DHCP now ({w['current']})")
        def addr_run(spec, reach=None):
            """ask, let root run; reach: a connection to write into /proc/net/tcp once root waits for one"""
            st, r = addr(spec)
            assert st == 200, r
            p = root(); seen = []
            for _ in range(200):
                time.sleep(0.05)
                a = wifi()["address_result"]
                if a and a["id"] == r["id"]: seen.append(a["state"])
                if reach and a and a["state"] == "waiting": links(*reach); reach = None
                if p.poll() is not None: break
            o, _ = p.communicate(timeout=60); rootlog.append(o)
            return r["id"], wifi(), seen, o
        on = lambda: W()["svc"] if not android else {}
        rid, w, seen, o = addr_run("fixed 192.168.1.50 24 192.168.1.1 192.168.1.1 9.9.9.9",
                                   reach=[("192.168.1.20:28931", "192.168.1.77:50100", "06"), ("192.168.1.50:28931", "192.168.1.77:50102", "06")])
        a = w["address_result"]; wd = W()
        check(a == {"id": rid, "state": "ok", "ip": "192.168.1.50", "reason": ""} and "waiting" in seen,
              f"fixed: on it, waited until a connection reached it (the page's, in TIME_WAIT), kept ({seen[-3:]})")
        check(w["address"] == {"mode": "fixed", "ip": "192.168.1.50", "prefix": 24, "gateway": "192.168.1.1", "dns": ["192.168.1.1", "9.9.9.9"],
                               "ssid": "HomeNet", "here": True} and b.state()["wifi"]["fixed"] is True and w["current"]["ip"] == "192.168.1.50",
              f"root's record: the address on HomeNet; the page's state says fixed ({w['address']})")
        if android:
            n0 = next(n for n in wd["networks"] if n["id"] == 0)
            check(n0.get("ipconf") == {"ip": "192.168.1.50", "prefix": 24, "gw": "192.168.1.1", "dns": ["192.168.1.1", "9.9.9.9"]}
                  and "app_process reassoc" in wd["calls"], "Android: the network's IP configuration, taken by a reassociation")
        else:
            check(wd["static_starts"][-1] == ["192.168.1.50", "255.255.255.0", "192.168.1.1", "192.168.1.1", "9.9.9.9"]
                  and wd["svc"]["dhcpcd-wlan0"] == "stopped" and wd["svc"]["dhcpcd-static"] == "running" and wd["addrs"] == ["192.168.1.50"],
                  "the Dots: dhcpcd-static with the values in the properties, in place of dhcpcd-wlan0, its address alone on wlan0")
        record = open(os.path.join(out, "address")).read()
        check(record == f"fixed 192.168.1.50 24 192.168.1.1 192.168.1.1 9.9.9.9 {H('HomeNet')}\n", f"the record, root's ({record.strip()})")
        n_static = len(W().get("static_starts", []))
        with open(world) as f: wd = json.load(f)
        wd["taken"] = ["192.168.1.99"]
        with open(world, "w") as f: json.dump(wd, f)
        rid, w, seen, o = addr_run("fixed 192.168.1.99 24 192.168.1.1 192.168.1.1 -")
        check(w["address_result"]["state"] == "failed" and w["address_result"]["reason"] == "inuse" and len(W().get("static_starts", [])) == n_static
              and w["address"]["ip"] == "192.168.1.50", "an address another device answers at: refused before anything changed")
        for spec, why, reach in (("fixed 192.168.1.70 24 192.168.1.254 192.168.1.1 -", "nogateway", None),   # nobody at that router
                                 # nothing connects to it; the Echo's own connection out from it proves nothing
                                 ("fixed 192.168.1.60 24 192.168.1.1 192.168.1.1 -", "unreached", [("192.168.1.60:41234", "192.168.1.5:8095", "01")]),
                                 # the same address: the link from before (there when it was set) counts not
                                 ("fixed 192.168.1.50 24 192.168.1.1 8.8.4.4 -", "unreached", "before")):
            if reach == "before": links(("192.168.1.50:28931", "192.168.1.77:50102", "06")); reach = None
            rid, w, seen, o = addr_run(spec, reach=reach)
            a = w["address_result"]; wd = W()
            back = (next(n for n in wd["networks"] if n["id"] == 0).get("ipconf") or {}).get("dns") == ["192.168.1.1", "9.9.9.9"] if android \
                else wd["static_starts"][-1] == ["192.168.1.50", "255.255.255.0", "192.168.1.1", "192.168.1.1", "9.9.9.9"]
            check(a["state"] == "failed" and a["reason"] == why and a["ip"] == "192.168.1.50" and back
                  and open(os.path.join(out, "address")).read() == record, f"{spec}: {why}, back on the fixed address it had, the record as it was")
        if not android:
            keep = lambda: subprocess.run(["sh", f"{dev}/wifi.sh", "keep", state, out], env=renv, capture_output=True, text=True, timeout=30).stdout
            with open(world) as f: wd = json.load(f)                  # a boot: wifisvc's dhcpcd-wlan0, its lease
            wd["svc"] = {"dhcpcd-wlan0": "running", "dhcpcd-static": "stopped"}; wd["addrs"] = ["192.168.1.20"]; wd["props"].update(result="ok", reason="BOUND", ipaddress="")
            with open(world, "w") as f: json.dump(wd, f)
            o = keep(); wd = W()
            check("set again" in o and wd["svc"] == {"dhcpcd-wlan0": "stopped", "dhcpcd-static": "running"} and wd["addrs"] == ["192.168.1.50"],
                  f"keep (netwatch) after a boot: dhcpcd-wlan0 gives way to the fixed address ({o.strip()!r})")
            n = len(wd["static_starts"])
            check(keep() == "" and len(W()["static_starts"]) == n, "keep again: nothing to do")
            with open(world) as f: wd = json.load(f)
            wd["ssid_fails"] = True; wd["svc"]["dhcpcd-static"] = "stopped"
            with open(world, "w") as f: json.dump(wd, f)
            check(keep() == "" and len(W()["static_starts"]) == n and W()["svc"]["dhcpcd-wlan0"] == "stopped",
                  "keep while the network's name cannot be read (wpa_cli says FAIL): the link left as it is")
            with open(world) as f: wd = json.load(f)
            wd["ssid_fails"] = False; wd["svc"]["dhcpcd-static"] = "running"
            with open(world, "w") as f: json.dump(wd, f)
            with open(os.path.join(out, "address"), "w") as f: f.write(record.replace(H("HomeNet"), H("Elsewhere")))
            o = keep(); wd = W()
            check("DHCP" in o and wd["svc"]["dhcpcd-wlan0"] == "running" and wd["svc"]["dhcpcd-static"] == "stopped" and wd["addrs"] == ["192.168.1.20"],
                  "keep on a network the fixed address is not for: DHCP")
            with open(os.path.join(out, "address"), "w") as f: f.write(record)
            os.makedirs(os.path.join(out, "lock")); open(os.path.join(out, "lock", "pid"), "w").write(f"{os.getpid()}\n")
            open(os.path.join(out, "lock", "boot"), "w").write(open("/proc/sys/kernel/random/boot_id").read())
            check(keep() == "" and W()["svc"]["dhcpcd-wlan0"] == "running", "keep while a run of wifi.sh has the link: nothing")
            shutil.rmtree(os.path.join(out, "lock"))
            check("set again" in keep() and W()["addrs"] == ["192.168.1.50"], "and the fixed address again once it is through")
            main_sh = open(f"{ROOT}/scripts/system/main.sh").read()
            nw = re.search(r"^netwatch\(\) \{.*?^\}$", main_sh, re.M | re.S).group(0)
            check(re.search(r"init\.svc\.\$WIFI_SERVICE\)\" != running \]; \} && sh \$D/wifi\.sh keep$", nw, re.M), "netwatch hands the link to keep once wifisvc is stopped")
        rid, w, seen, o = addr_run("auto")
        a = w["address_result"]; wd = W()
        check(a["state"] == "ok" and a["ip"] == "192.168.1.20" and w["address"] == {"mode": "auto"} and not os.path.exists(os.path.join(out, "address"))
              and b.state()["wifi"]["fixed"] is False, f"DHCP again: its lease, the record gone ({a})")
        if android: check("ipconf" not in next(n for n in wd["networks"] if n["id"] == 0), "Android: the network's IP configuration DHCP again")
        else: check(wd["svc"] == {"dhcpcd-wlan0": "running", "dhcpcd-static": "stopped"} and wd["addrs"] == ["192.168.1.20"], "the Dots: dhcpcd-wlan0 again")
        # the fixed address before does not come back up either (the router stopped answering there): DHCP, the record gone
        rid, w, seen, o = addr_run("fixed 192.168.1.50 24 192.168.1.1 192.168.1.1 -", reach=[("192.168.1.50:16700", "192.168.1.2:41010", "01")])
        check(w["address_result"]["state"] == "ok", "fixed at 192.168.1.50 (reached on Wyoming's port)")
        with open(world) as f: wd = json.load(f)
        next(a for a in wd["air"] if a["hex"] == H("HomeNet"))["arp"] = False
        with open(world, "w") as f: json.dump(wd, f)
        with open(arp, "w") as f: f.write("IP address       HW type     Flags       HW address            Mask     Device\n")   # its entry gone stale
        rid, w, seen, o = addr_run("fixed 192.168.1.70 24 192.168.1.1 192.168.1.1 -")
        check(w["address_result"]["state"] == "failed" and w["address_result"]["reason"] == "nogateway" and w["address"] == {"mode": "auto"}
              and not os.path.exists(os.path.join(out, "address")) and "does not come back either" in o,
              f"the address before does not come back either: DHCP, the record gone ({w['address_result']})")
        with open(world) as f: wd = json.load(f)
        next(a for a in wd["air"] if a["hex"] == H("HomeNet"))["arp"] = True
        with open(world, "w") as f: json.dump(wd, f)
        # fixed again, for the switches below: a failed one comes back to it, one that works leaves it behind
        rid, w, seen, o = addr_run("fixed 192.168.1.50 24 192.168.1.1 192.168.1.1 -", reach=[("192.168.1.50:26053", "192.168.1.2:41000", "01")])
        check(w["address_result"]["state"] == "ok", "fixed again (Home Assistant's connection reached it)")
        st, r = addr("auto"); line = open(reqf).read()
        check(st == 200 and line == f"address {r['id']} auto\n", "the request for root: one line")
        os.remove(reqf)

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
        check(w["current"]["ip"] == "192.168.1.50" and w["address"]["here"], f"and on its fixed address there ({w['current']['ip']})")
        if android:
            check(wd["current"] == 0 and [n["id"] for n in wd["networks"]] == [0, 1] and [n["disabled"] for n in wd["networks"]] == [0, 0]
                  and not any(c.endswith("save_config") for c in wd["calls"])
                  and any(c == "app_process enable 0 keep" for c in wd["calls"]),
                  "the new network removed, the old ones enabled again (the framework's store is the saved state)")
        else:
            check(wd["current"] == 0 and [n["id"] for n in wd["networks"]] == [0, 1, 2] and [n["disabled"] for n in wd["networks"]] == [0, 0, 2]
                  and wd["saved"] == [home, attic, p2p] and not any(c.endswith("save_config") for c in wd["calls"]),
                  "the new network removed, the others enabled again, the saved configuration never touched")

        # ---- out of reach, no address, a silent router (HomeNet's IP configuration aside: the fixed address above)
        plain = lambda nets: [{k: v for k, v in n.items() if k not in ("ipconf", "ipconf_on")} for n in nets]
        for name, why in (("Nowhere", "notfound"), ("NoDHCP", "noaddress"), ("Silent", "nogateway")):
            st, r = join(name, None); root_run()
            res = json.loads(b.call("GET", "/api/wifi")[2])["result"]; wd = W()
            check(st == 200 and res["id"] == r["id"] and res["state"] == "failed" and res["reason"] == why and res["back"] == "HomeNet"
                  and wd["current"] == 0 and plain(wd["saved"]) == ([home, attic] if android else [home, attic, p2p]),
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
        check(w["address"] == {"mode": "auto"} and not os.path.exists(os.path.join(out, "address")) and any("was the old network's" in o for o in rootlog),
              "the fixed address was HomeNet's: on the new network DHCP")

        # ---- the same network with another password (the router's changed): replaced, not added
        st, r = join(KITCHEN, "f" * 64); assert open(reqf).read().split()[3] == "f" * 64
        with open(world) as f: wd = json.load(f)
        next(a for a in wd["air"] if a["hex"] == H(KITCHEN))["psk"] = "f" * 64
        with open(world, "w") as f: json.dump(wd, f)
        root_run(); wd = W()
        k = [n for n in wd["saved"] if n["ssid"] == H(KITCHEN)]
        check(len(k) == 1 and k[0]["psk"] == "f" * 64 and json.loads(b.call("GET", "/api/wifi")[2])["result"]["state"] == "ok",
              "the PSK as 64 hex digits taken as it is; the network's older entry replaced")

        # ---- the network it is on, its password typed wrong: back on it with the password it had (WifiService
        # changes an entry of the same name in place: its old key goes back in)
        st, r = join(KITCHEN, "kitchen typo"); root_run(); wd = W()
        res = json.loads(b.call("GET", "/api/wifi")[2])["result"]
        k = [n for n in wd["saved"] if n["ssid"] == H(KITCHEN)]
        check(res["state"] == "failed" and res["reason"] == "wrongkey" and res["back"] == KITCHEN
              and len(k) == 1 and k[0]["psk"] == "f" * 64 and wd["current"] == k[0]["id"],
              "the same network, a wrong password: back on it, its saved password the one from before")
        check(not any("f" * 64 in o for o in rootlog), "the stored key in no log")

        # ---- WifiService's network saved by another user (Amazon's setup, a stock update): root may not change it, the
        # system user may
        if android:
            with open(world) as f: wd = json.load(f)
            next(n for n in wd["networks"] if n["ssid"] == H(KITCHEN))["creator"] = 1010
            with open(world, "w") as f: json.dump(wd, f)
            st, r = join(KITCHEN, "f" * 64); root_run(); wd = W()
            res = json.loads(b.call("GET", "/api/wifi")[2])["result"]
            adds = [c for c in wd["calls"] if c.startswith("app_process add")]
            check(res["state"] == "ok" and len(adds) >= 2 and adds[-1] == adds[-2],
                  f"a network root may not change: changed as the system user, on it ({res['state']})")

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
        subprocess.run(["sh", f"{dev}/wifi.sh", "clean", state, out], env=renv, check=True)
        check(not os.path.exists(reqf) and not os.path.exists(os.path.join(out, "lock")), "clean (firewall service start): the old request and the dead run's lock gone")
        with open(reqf, "w") as f: f.write("join 5 6869;reboot ffff\n")
        o = root_run()
        check("not understood" in o and not os.path.exists(reqf) and not any("reboot" in c for c in W()["calls"][-3:]), "a request that is not hex: dropped")

        # ---- issue #90: what wifisvc makes of a saved network, main.sh netwatch's psk_only; the setup's join on WPA3
        if not android:
            mixed = {"id": 0, "ssid": H("Mixed"), "psk": PSK("mixed pass", "Mixed"), "key_mgmt": "WPA-PSK WPA-PSK-SHA256", "priority": 0, "disabled": 0}
            def world_set(**kw):
                w = W(); w.update(kw)
                with open(world, "w") as f: json.dump(w, f)
            air_mixed = {"hex": H("Mixed"), "psk": PSK("mixed pass", "Mixed"), "flags": "[WPA2-PSK+SAE+PSK-SHA256-CCMP][ESS]",
                         "bss": [["aa:00:00:00:00:09", 2472, -37]], "ip": "10.6.6.6", "gw": "10.6.6.1"}
            main_sh = open(f"{ROOT}/scripts/system/main.sh").read()
            fn = re.search(r"^wpa\(\) \{.*?^psk_only\(\) \{.*?^\}$", main_sh, re.M | re.S).group(0)
            check(re.search(r"^        psk_only$", re.search(r"^netwatch\(\) \{.*?^\}$", main_sh, re.M | re.S).group(0), re.M), "netwatch looks every round, the address or not")
            def looks(n=1, down=3, service="wifisvc", socks="/none", tail=""):
                """main.sh netwatch's calls, n of them, the 30 s before them counted in down"""
                sh = f"WLAN=wlan0 WPA_SOCKETS={socks} WIFI={out} WIFI_SERVICE={service} down={down}\n{fn}\n" + "psk_only\n" * n + tail
                return subprocess.run(["sh", "-c", sh], env=renv, capture_output=True, text=True, timeout=30).stdout
            cut = lambda: W()["networks"][0]["key_mgmt"] == "WPA-PSK"
            world_set(networks=[dict(mixed)], current=0, target=None, air=W()["air"] + [air_mixed], saved=[])
            check(looks(tail='echo "down=$down"\n') == "down=0\n" and not cut() and W()["current"] == 0, "associated, the address or not: the entry and the link left alone, the count back at 0")
            world_set(current=None)
            check(looks(n=3, down=0) == "" and not cut(), "not on the network for less than 30 s: left alone")
            check(looks(service="") == "" and not cut(), "no wifisvc (Android's WifiService owns its networks): left alone")
            os.makedirs(os.path.join(out, "lock"))
            check(looks() == "" and not cut(), "while wifi.sh switches: left alone")
            os.rmdir(os.path.join(out, "lock"))
            world_set(calls=[])
            o = looks()
            check("network 0 cut back to WPA-PSK" in o and cut(), f"30 s off on PSK-SHA256: cut back ({o.strip()!r})")
            check(all(" -g@android:wpa_wlan0 IFNAME=wlan0 " in c for c in W()["calls"] if c.startswith("wpa_cli")), "no socket for the interface (a reconfigure): through the global one")
            check(W()["saved"] == [], "in memory only: nothing saved")
            r = subprocess.run(["wpa_cli", "status"], env=renv, capture_output=True, text=True).stdout
            check("wpa_state=COMPLETED" in r, "and it associates")
            world_set(current=None)
            check(looks() == "", "the next round has nothing left to do")
            sockets = os.path.join(tmp, "sockets"); os.makedirs(sockets)
            s = socket.socket(socket.AF_UNIX); s.bind(os.path.join(sockets, "wlan0"))
            world_set(networks=[dict(mixed)], current=None, target=None, calls=[])
            check("cut back" in looks(socks=sockets) and all(" -i wlan0 -p " in c for c in W()["calls"] if c.startswith("wpa_cli")), "with the interface's socket: through that")
            s.close()

            with open(os.path.join(fake, "iptables"), "w") as f: f.write("#!/bin/sh\necho '-A OUTPUT -j hassmic_out'\n")
            os.chmod(os.path.join(fake, "iptables"), 0o755)
            join = lambda ssid, pw: subprocess.run(["sh", f"{ROOT}/scripts/device/wifi-join.sh", ssid, pw], env=dict(renv, WIFI_JOIN_SECS="1"),
                                                   capture_output=True, text=True, timeout=30)
            def on_air(flags):
                world_set(networks=[], current=None, target=None, next_id=0, save_fails=False,
                          air=[{"hex": H("Only3"), "psk": "not a PSK network", "flags": flags, "bss": [["aa:00:00:00:00:05", 5200, -50]], "ip": "10.9.9.9", "gw": "10.9.9.1"}])
            on_air("[WPA2-SAE-CCMP][ESS]")
            p = join("Only3", "whatever!")
            check(p.returncode == 1 and "WPA3 only" in p.stdout and W()["networks"] == [], f"a WPA3-only network: says so, nothing kept ({p.stdout.strip()!r})")
            on_air("[WPA2-PSK+SAE+PSK-SHA256-CCMP][ESS]")
            p = join("Only3", "whatever!")
            check(p.returncode == 1 and "WPA3" not in p.stdout and "password is probably wrong" in p.stdout, f"mixed, the wrong password: as before ({p.stdout.strip()!r})")
            p = join("Elsewhere", "whatever!")
            check(p.returncode == 1 and "WPA3" not in p.stdout and "no network named" in p.stdout, f"a name not on the air: as before ({p.stdout.strip()!r})")
    finally:
        proc.send_signal(signal.SIGTERM); proc.wait()
    if check.failed:
        print(open(log).read()); print("".join(rootlog)); print(f"FAILED ({mode})"); raise SystemExit(1)


for android in (False, True):
    main(android)
print("PASS")
