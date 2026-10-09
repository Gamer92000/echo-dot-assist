#!/usr/bin/env python3
"""Stand-ins for what scripts/device/wifi.sh calls on the Echo (wpa_cli, ifconfig, getprop, setprop, dhcpcd, ping, stop,
start), named by argv[0], over one JSON file (FAKE_WIFI_WORLD): wpa_supplicant's networks and the one it is on, the
networks on the air with their password, DHCP and router, and dhcpcd's lease.  As wpa_supplicant does: a new network
starts disabled, select_network disables the others, nothing reaches the saved configuration before save_config, SSIDs
print with printf_encode.  As the Dot 2 does (2026-10-07): dhcpcd runs with -K, so the lease, the address on wlan0 and
the old router's ARP entry stay through any change of network until "dhcpcd -n" asks again; its hook writes the
dhcp.wlan0.* properties then.  For tests/fake_web_wifi.py."""
import fcntl, hashlib, json, os, sys

WORLD = os.environ["FAKE_WIFI_WORLD"]


def esc(b):
    """wpa_supplicant's printf_encode"""
    out = ""
    for c in b:
        if c in (0x22, 0x5c): out += "\\" + chr(c)
        elif c == 0x1b: out += "\\e"
        elif c == 0x0a: out += "\\n"
        elif c == 0x0d: out += "\\r"
        elif c == 0x09: out += "\\t"
        elif 0x20 <= c < 0x7f: out += chr(c)
        else: out += "\\x%02x" % c
    return out


def value(v):
    """a set_network value: "quoted text" or hex"""
    return v[1:-1].encode() if v.startswith('"') and v.endswith('"') and len(v) >= 2 else bytes.fromhex(v)


def psk_of(net):
    p = net.get("psk")
    if p is None: return None
    if p.startswith('"'): return hashlib.pbkdf2_hmac("sha1", p[1:-1].encode(), bytes.fromhex(net["ssid"]), 4096, 32).hex()
    return p


def air_of(w, net):
    return next((a for a in w["air"] if a["hex"] == net.get("ssid")), None)


def associate(w):
    """what wpa_supplicant gets to by the time anyone asks: on the network it was told to take, or the best enabled one"""
    if w.get("current") is not None: return "COMPLETED"
    tgt = next((n for n in w["networks"] if n["id"] == w.get("target")), None)
    if w.get("settle") and tgt and air_of(w, tgt): w["settle"] -= 1; return "ASSOCIATING"     # a look or two on the way, as on the Echo
    nets = [n for n in w["networks"] if n["id"] == w.get("target")] if w.get("target") is not None else \
        sorted((n for n in w["networks"] if not n["disabled"]), key=lambda n: -n["priority"])
    state = "SCANNING"
    for n in nets:
        a = air_of(w, n)
        if not a: continue
        want = a.get("psk")
        if (want is None and n.get("key_mgmt") == "NONE") or (want is not None and n.get("key_mgmt") == "WPA-PSK" and psk_of(n) == want):
            w["current"] = n["id"]; return "COMPLETED"
        state = "4WAY_HANDSHAKE" if want is not None else "ASSOCIATING"
    return state


def connected(w):
    """(network, its air entry) while on one"""
    associate(w)
    n = next((n for n in w["networks"] if n["id"] == w.get("current")), None)
    return (n, air_of(w, n)) if n else (None, None)


def wpa(w, args):
    cmd, a = args[0], args[1:]
    net = lambda i: next((n for n in w["networks"] if str(n["id"]) == i), None)
    if cmd == "status":
        st = associate(w)
        if st != "COMPLETED": return f"wpa_state={st}\naddress=02:00:00:00:00:01"
        n, air = connected(w)
        lines = [f"bssid={air['bss'][0][0]}", f"freq={air['bss'][0][1]}", f"ssid={esc(bytes.fromhex(n['ssid']))}", f"id={n['id']}",
                 "mode=station", "wpa_state=COMPLETED"]
        if air.get("dhcp", True): lines.append(f"ip_address={air['ip']}")
        return "\n".join(lines + ["address=02:00:00:00:00:01"])
    if cmd == "list_networks":
        rows = ["network id / ssid / bssid / flags"]
        for n in w["networks"]:
            fl = "[CURRENT]" if n["id"] == w.get("current") else "[DISABLED]" if n["disabled"] else ""
            rows.append(f"{n['id']}\t{esc(bytes.fromhex(n.get('ssid', '')))}\tany\t{fl}")
        return "\n".join(rows)
    if cmd == "add_network":
        i = w["next_id"]; w["next_id"] += 1
        w["networks"].append({"id": i, "disabled": 1, "priority": 0}); return str(i)
    if cmd == "set_network":
        n = net(a[0])
        if not n or len(a) != 3: return "FAIL"
        k, v = a[1], a[2]
        if k == "ssid": n["ssid"] = value(v).hex()
        elif k == "psk":
            if not (v.startswith('"') or (len(v) == 64 and all(c in "0123456789abcdef" for c in v))): return "FAIL"
            n["psk"] = v
        elif k in ("priority", "scan_ssid", "disabled"): n[k] = int(v)
        else: n[k] = v
        return "OK"
    if cmd == "get_network":
        n = net(a[0])
        if not n: return "FAIL"
        if a[1] == "ssid":
            b = bytes.fromhex(n["ssid"])
            return f'"{b.decode()}"' if all(0x20 <= c < 0x7f for c in b) else n["ssid"]
        if a[1] == "psk": return "*"
        return str(n.get(a[1], "FAIL"))
    if cmd == "select_network":
        if not net(a[0]): return "FAIL"
        for n in w["networks"]:
            if n["disabled"] != 2: n["disabled"] = 0 if str(n["id"]) == a[0] else 1
        w["current"] = None; w["target"] = int(a[0]); w["settle"] = 2; return "OK"
    if cmd == "enable_network":
        n = net(a[0])
        if not n: return "FAIL"
        n["disabled"] = 0; return "OK"
    if cmd == "remove_network":
        n = net(a[0])
        if not n: return "FAIL"
        w["networks"].remove(n)
        if w.get("current") == n["id"] or w.get("target") == n["id"]: w["current"] = w["target"] = None
        return "OK"
    if cmd == "reassociate": w["current"] = w["target"] = None; return "OK"
    if cmd == "save_config":
        if w.get("save_fails"): return "FAIL"
        w["saved"] = json.loads(json.dumps(w["networks"])); return "OK"
    if cmd == "scan": return "OK"
    if cmd == "scan_results":
        rows = ["bssid / frequency / signal level / flags / ssid"]
        for air in w["air"]:
            for bssid, freq, sig in air["bss"]: rows.append(f"{bssid}\t{freq}\t{sig}\t{air['flags']}\t{esc(bytes.fromhex(air['hex']))}")
        return "\n".join(rows)
    return "UNKNOWN COMMAND"


def wifictl(w, args):
    """scripts/device/wifictl.dex, as WifiService would answer: the framework world of a model with
    INSTALL=boot (checkers).  Its calls land in the same world the wpa_cli fake serves; the shapes it prints
    are wpa_cli's (that is what wifi.sh expects), with WifiService's habits: adding a network with a name
    already there replaces it, the framework saves every change at once (the world is the store), it hands
    out an address with the network it connects to (no dhcpcd), and WifiInfo quotes the name."""
    verb = args[0] if args else ""
    if verb == "wifi-on": return "OK"
    if verb == "name": return bytes.fromhex(args[1]).decode() if len(args) > 1 else ""
    if verb == "scan": return "OK"
    if verb in ("status", "list_networks", "scan_results"):
        if verb == "status":
            st = associate(w)
            n, air = connected(w)
            if st == "COMPLETED" and air and air.get("gw") and air["gw"] not in ("", "0"):
                gwhex = "".join("%02X" % int(x) for x in reversed(air["gw"].split(".")))
                with open(w["route_file"], "w") as r:          # the default route, as /proc/net/route has it
                    r.write("Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
                            f"wlan0\t00000000\t{gwhex}\t0003\t0\t0\t0\t00000000\t0\t0\t0\n")
            if st != "COMPLETED": return "wpa_state=%s\nid=-1" % st
            lines = ["wpa_state=COMPLETED", "id=%d" % n["id"], 'ssid="%s"' % esc(bytes.fromhex(n["ssid"]))]
            if air.get("dhcp", True) and air["ip"] not in ("", "0"):
                lines.append("ip_address=%s" % air["ip"])
            return "\n".join(lines)
        return wpa(w, args)
    if verb == "keys":                                          # getPrivilegedConfiguredNetworks: the stored keys
        return "\n".join("%d\t%s" % (n["id"], n.get("psk") or "-") for n in w["networks"])
    if verb == "add":
        ssid = value(args[1])
        for n in w["networks"]:
            if n.get("ssid") == ssid.hex(): break             # the same name replaces, not a second entry
        else:
            i = w["next_id"]; w["next_id"] += 1
            n = {"id": i, "disabled": 0, "priority": 0, "ssid": ssid.hex()}
            w["networks"].append(n)
        n["scan_ssid"] = 1                                      # wifictl sets hiddenSSID: found when not broadcast
        if args[2] == "-": n.pop("psk", None); n["key_mgmt"] = "NONE"
        else: n["psk"] = args[2]; n["key_mgmt"] = "WPA-PSK"
        w["saved"] = json.loads(json.dumps(w["networks"])); return str(n["id"])
    if verb == "select":
        if not next((n for n in w["networks"] if str(n["id"]) == args[1]), None): return "FAIL"
        for n in w["networks"]:
            if n["disabled"] != 2: n["disabled"] = 0 if str(n["id"]) == args[1] else 1
        w["current"] = None; w["target"] = int(args[1]); w["settle"] = 2; w["saved"] = json.loads(json.dumps(w["networks"])); return "OK"
    if verb == "enable":
        n = next((n for n in w["networks"] if str(n["id"]) == args[1]), None)
        if not n: return "FAIL"
        n["disabled"] = 0
        if len(args) > 2 and args[2] == "only":
            for o in w["networks"]:
                if o["disabled"] != 2 and o is not n: o["disabled"] = 1
        w["saved"] = json.loads(json.dumps(w["networks"])); return "OK"
    if verb == "remove":
        n = next((n for n in w["networks"] if str(n["id"]) == args[1]), None)
        if not n: return "FAIL"
        w["networks"].remove(n)
        if w.get("current") == n["id"] or w.get("target") == n["id"]: w["current"] = w["target"] = None
        w["saved"] = json.loads(json.dumps(w["networks"])); return "OK"
    if verb == "reassociate": w["current"] = w["target"] = None; return "OK"
    return "FAIL"


def main():
    tool = os.path.basename(sys.argv[0]); args = sys.argv[1:]
    if tool == "app_process": args = args[2:]                 # app_process / Wifictl <verb> ...
    with open(WORLD, "r+") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        w = json.load(f); out = ""; rc = 0
        w.setdefault("calls", []).append(" ".join([tool] + args))
        if tool == "wpa_cli":
            while args and (args[0] in ("-i", "-p") or args[0].startswith("-g") or args[0].startswith("IFNAME=")):
                args = args[2:] if args[0] in ("-i", "-p") else args[1:]     # the interface's socket, or the global one
            out = wpa(w, args)
        elif tool == "app_process":
            out = wifictl(w, args)
        elif tool == "ifconfig":                    # the lease's address, whatever network the Echo is on now
            ip = w["lease"]["ip"]
            if os.environ.get("INSTALL") == "boot":
                n, air = connected(w)               # Android's DHCP: the address comes with its network
                ip = air["ip"] if n and air and air.get("dhcp", True) and air["ip"] not in ("", "0") else ""
            out = "wlan0     Link encap:Ethernet  HWaddr 02:00:00:00:00:01"
            if ip: out += f"\n          inet addr:{ip}  Bcast:{ip.rsplit('.', 1)[0]}.255  Mask:255.255.255.0"
        elif tool == "getprop":
            k = args[0] if args else ""
            if k.startswith("dhcp.wlan0."):
                p = k[len("dhcp.wlan0."):]
                out = w["props"].get(p, "") or {"ipaddress": w["lease"]["ip"], "gateway": w["lease"]["gw"]}.get(p, "")
            elif k.startswith("init.svc."): out = "stopped"
        elif tool == "setprop":
            if args[0].startswith("dhcp.wlan0."): w["props"][args[0][len("dhcp.wlan0."):]] = args[1]
        elif tool == "dhcpcd" or (tool == "start" and args[:1] == ["dhcpcd-wlan0"]):   # -n: ask again, or init's service
            n, air = connected(w)                   # starting: a lease only from a network that has DHCP
            if n and air.get("dhcp", True):
                w["props"].update(result="ok", reason="RENEW" if w["lease"]["ip"] == air["ip"] else "BOUND", ipaddress="", gateway="")
                w["lease"] = {"ip": air["ip"], "gw": air["gw"]}
        elif tool == "ping":                        # the ARP cache: entries stay, the one asked for is filled in if there
            n, air = connected(w); rc = 1
            arp = {l.split()[0]: l for l in open(w["arp_file"]).read().splitlines()[1:] if l.strip()} if os.path.exists(w["arp_file"]) else {}
            if n and air.get("arp", True) and air["gw"] == args[-1]:
                arp[air["gw"]] = f"{air['gw']:<16} 0x1         0x2         aa:bb:cc:dd:ee:ff     *        wlan0"; rc = 0
            with open(w["arp_file"], "w") as a:
                a.write("IP address       HW type     Flags       HW address            Mask     Device\n" + "".join(l + "\n" for l in arp.values()))
        # stop, start (other services): nothing to say
        f.seek(0); f.truncate(); json.dump(w, f)
    if out: print(out)
    sys.exit(rc)


main()
