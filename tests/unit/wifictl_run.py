#!/usr/bin/env python3
"""Runs scripts/device/wifictl.dex on the PC: a Dalvik interpreter for the instructions tools/mkwifictl.py
writes, the java.* calls it makes, and a fake WifiService behind the reflection.  No ART here (qemu-user cannot
run the firmware's), and the type checks of mkwifictl.py --check say nothing about what the code computes; this
does: every verb, its output, and what it hands WifiService.  The reflection lookups must name the signatures
mkwifictl.py's REFLECTED list holds, which --framework=<rootfs> checks against the firmware's own classes."""
import os
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkwifictl as mk                                         # noqa: E402

DEX = os.path.join(ROOT, "scripts", "device", "wifictl.dex")


class Exit(Exception):
    pass


def i32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x & 0x80000000 else x


# ---------------------------------------------------------------- the Java side the dex reaches
class JClass:
    def __init__(self, name, make=None):
        self.name, self.make = name, make


class JMethod:
    def __init__(self, cls, name, params):
        self.cls, self.name, self.params = cls, name, params


class JField:
    def __init__(self, name):
        self.name = name


class Enum:
    def __init__(self, n):
        self.n = n

    def toString(self):
        return self.n


class Config:                                                  # android.net.wifi.WifiConfiguration
    JAVA = "android.net.wifi.WifiConfiguration"

    def __init__(self):
        self.networkId, self.SSID, self.preSharedKey, self.hiddenSSID = -1, None, None, False
        self.status, self.allowedKeyManagement = 2, set()


class Info:                                                    # android.net.wifi.WifiInfo
    JAVA = "android.net.wifi.WifiInfo"

    def __init__(self, w):
        self.w = w

    def getSupplicantState(self): return Enum(self.w.state)
    def getNetworkId(self): return self.w.current
    def getSSID(self):
        n = self.w.net(self.w.current)
        return n.SSID if n else "<unknown ssid>"
    def getIpAddress(self): return self.w.ip
    def getFrequency(self): return 2437 if self.w.current >= 0 else -1


class Scan:                                                    # android.net.wifi.ScanResult
    JAVA = "android.net.wifi.ScanResult"

    def __init__(self, bssid, freq, level, caps, ssid):
        self.BSSID, self.frequency, self.level, self.capabilities, self.SSID = bssid, freq, level, caps, ssid


class Wifi:
    """IWifiManager's proxy as the reflection reaches it; records what it is asked"""
    JAVA = "android.net.wifi.IWifiManager$Stub$Proxy"
    SIGS = {(n, tuple(ps)) for c, n, ps, static in mk.REFLECTED if c == "Landroid/net/wifi/IWifiManager;"}

    def __init__(self):
        self.nets, self.next, self.current, self.state, self.ip = [], 0, -1, "DISCONNECTED", 0
        self.calls, self.enabled = [], False

    def net(self, i):
        return next((n for n in self.nets if n.networkId == i), None)

    def getConnectionInfo(self): return Info(self)
    def getConfiguredNetworks(self): return list(self.nets)
    def getPrivilegedConfiguredNetworks(self): return list(self.nets)
    def getScanResults(self, pkg):
        return [Scan("aa:00:00:00:00:01", 2437, -60, "[WPA2-PSK-CCMP][ESS]", "Home"),
                Scan("aa:00:00:00:00:02", 5180, -71, "[ESS]", "Café")]
    def startScan(self, settings, ws): self.calls.append("scan")
    def addOrUpdateNetwork(self, c):
        self.calls.append(("add", c.SSID, c.preSharedKey, c.hiddenSSID, sorted(c.allowedKeyManagement)))
        for n in self.nets:
            if n.SSID == c.SSID:
                c.networkId = n.networkId; self.nets[self.nets.index(n)] = c; return c.networkId
        c.networkId, self.next = self.next, self.next + 1
        self.nets.append(c)
        return c.networkId
    def enableNetwork(self, i, others):
        if not self.net(i): return False
        self.calls.append(("enable", i, others)); return True
    def removeNetwork(self, i):
        n = self.net(i)
        if not n: return False
        self.nets.remove(n); self.calls.append(("remove", i)); return True
    def reconnect(self): self.calls.append("reconnect")
    def setWifiEnabled(self, pkg, on): self.enabled = on; self.calls.append(("wifi", pkg, on)); return True


class JavaEnv:
    def __init__(self, wifi):
        self.wifi, self.out, self.err = wifi, [], []
        self.classes = {c.JAVA: c for c in (Config, Info, Scan, Wifi)}

    def jstr(self, o):                                         # String.valueOf / StringBuilder.append(Object)
        if o is None: return "null"
        if o is True: return "true"
        if o is False: return "false"
        if isinstance(o, (int, str)): return str(o)
        if isinstance(o, Enum): return o.n
        raise AssertionError(f"toString of {o!r}")

    def jclass(self, o):
        if isinstance(o, str): return JClass("java.lang.String")
        return JClass(type(o).JAVA, type(o))

    def for_name(self, n):
        if n in ("android.os.ServiceManager", "android.net.wifi.IWifiManager$Stub", "java.lang.String",
                 "android.os.IBinder", "android.net.wifi.ScanSettings", "android.os.WorkSource"):
            return JClass(n)
        if n in self.classes: return JClass(n, self.classes[n])
        raise AssertionError(f"Class.forName({n}): ClassNotFoundException (a primitive is no class name)")

    def get_method(self, cls, name, params):
        ps = tuple({"int": "I", "boolean": "Z"}.get(p.name, "L" + p.name.replace(".", "/") + ";") for p in params)
        if cls.name == Wifi.JAVA:
            assert (name, ps) in Wifi.SIGS, f"IWifiManager.{name}{ps}: not in REFLECTED"
        elif cls.name in ("android.os.ServiceManager", "android.net.wifi.IWifiManager$Stub"):
            assert any(c == "L" + cls.name.replace(".", "/") + ";" and n == name and tuple(p) == ps
                       for c, n, p, st in mk.REFLECTED), f"{cls.name}.{name}{ps}"
        else:
            assert len(ps) == 0 and hasattr(cls.make, name), f"{cls.name}.{name}{ps}"
        return JMethod(cls, name, ps)

    def invoke(self, m, obj, args):
        if m.cls.name == "android.os.ServiceManager":
            assert args == ["wifi"]; return "binder:wifi"
        if m.cls.name == "android.net.wifi.IWifiManager$Stub":
            assert args == ["binder:wifi"]; return self.wifi
        args = [None if p[0] == "L" and a == 0 and not isinstance(a, bool) else a      # const/4 0 is null too
                for p, a in zip(m.params, args)]
        for p, a in zip(m.params, args):                      # unboxed as Method.invoke does
            if p == "I": assert isinstance(a, int) and not isinstance(a, bool), (m.name, a)
            if p == "Z": assert isinstance(a, bool), (m.name, a)
        return getattr(obj, m.name)(*args)

    def call(self, key, args):
        """the java.* methods, by mkwifictl's METHODS key"""
        a = args
        if key == "SB.new": a[0].clear(); return None
        if key == "SB.appendO": a[0].append(self.jstr(a[1])); return a[0]
        if key == "SB.appendI": a[0].append(str(a[1])); return a[0]
        if key == "SB.to": return "".join(a[0])
        if key == "println": (self.out if a[0] == "out" else self.err).append(a[1]); return None
        if key == "str.eq": return a[0] == a[1]
        if key == "str.new_b":
            raise AssertionError("handled in new-instance")
        if key == "str.charat": return ord(a[0][a[1]])
        if key == "str.length": return len(a[0])
        if key == "str.valueO": return self.jstr(a[0])
        if key == "int.parse": return int(a[0])
        if key == "int.value": return a[0]
        if key == "int.intval": return a[0]
        if key == "bool.value": return bool(a[0])
        if key == "cls.forName": return self.for_name(a[0])
        if key == "cls.newi": return a[0].make()
        if key == "cls.getm": return self.get_method(a[0], a[1], a[2])
        if key == "cls.getf":
            known = {n for c, n, t in mk.REFLECTED_FIELDS if c == "L" + a[0].name.replace(".", "/") + ";"}
            assert a[1] in known, f"{a[0].name}.{a[1]}: not in REFLECTED_FIELDS"
            return JField(a[1])
        if key == "obj.getcls": return self.jclass(a[0])
        if key == "obj.tostr": return self.jstr(a[0])
        if key == "obj.eq": return a[0] is a[1] or a[0] == a[1] and type(a[0]) is type(a[1])
        if key == "m.invoke": return self.invoke(a[0], a[1], list(a[2]))
        if key == "m.setacc": return None
        if key == "f.get": return getattr(a[1], a[0].name)
        if key == "f.set":
            old = getattr(a[1], a[0].name)
            assert type(old) is type(a[2]) or old is None or (isinstance(old, bool) and isinstance(a[2], bool)), \
                f"Field.set {a[0].name}: {a[2]!r} for a {type(old).__name__}"
            setattr(a[1], a[0].name, a[2]); return None
        if key == "it.has": return a[0][1] < len(a[0][0])
        if key == "it.next": a[0][1] += 1; return a[0][0][a[0][1] - 1]
        if key == "list.iter": return [a[0], 0]
        if key == "bits.set": a[0].add(a[1]); return None
        if key == "sys.exit": raise Exit(a[0])
        raise AssertionError(key)

    def sfield(self, key):
        return {"sys.out": "out", "sys.err": "err", "bool.T": True,
                "int.TYPE": JClass("int"), "bool.TYPE": JClass("boolean")}[key]


# ---------------------------------------------------------------- the interpreter
class Dex:
    def __init__(self, data):
        self.ids, _ = mk.read(data)                           # the checks of mkwifictl.py first
        strs, types, protos, fields, methods = self.ids
        u32 = lambda o: struct.unpack("<I", data[o:o + 4])[0]
        cdo = struct.unpack("<I", data[100:104])[0]
        o = u32(cdo + 24)

        def uleb(o):
            r = s = 0
            while True:
                b = data[o]; o += 1; r |= (b & 0x7f) << s
                if b < 0x80: return r, o
                s += 7
        _, o = uleb(o); _, o = uleb(o); dm, o = uleb(o); _, o = uleb(o)
        self.code, idx = {}, 0
        for _ in range(dm):
            d, o = uleb(o); _, o = uleb(o); co, o = uleb(o); idx += d
            regs, ins, _, _, _, n = struct.unpack("<HHHHII", data[co:co + 16])
            self.code[methods[idx][2]] = (regs, ins, [struct.unpack("<H", data[co + 16 + 2 * k:co + 18 + 2 * k])[0]
                                                      for k in range(n)])
        # java.* method ids -> mkwifictl's keys, fields likewise
        self.mkeys = {}
        for k, (c, n, (ps, r)) in mk.METHODS.items():
            for i, (mc, (mr, mps), mn) in enumerate(methods):
                if (mc, mn, tuple(mps), mr) == (c, n, tuple(ps), r): self.mkeys[i] = k
        self.fkeys = {i: k for k, f in mk.FIELDS.items() for i, ff in enumerate(fields)
                      if (ff[0], ff[2], ff[1]) == (f[0], f[1], f[2])}

    def run(self, env, name, args, depth=0):
        strs, types, protos, fields, methods = self.ids
        nregs, ins, units = self.code[name]
        r = [None] * nregs
        r[nregs - ins:] = args
        at, res, steps = 0, None, 0
        while True:
            steps += 1
            assert steps < 200000, f"{name}: runs away"
            op, f, o = mk.decode(units, at)
            nxt = at + mk.UNITS[f]
            if op in (0x01, 0x07): r[o[0]] = r[o[1]]
            elif op in (0x0a, 0x0c): r[o[0]] = res
            elif op == 0x0e: return None
            elif op in (0x0f, 0x11): return r[o[0]]
            elif op in (0x12, 0x13): r[o[0]] = o[1]
            elif op == 0x1a: r[o[0]] = strs[o[1]]
            elif op == 0x1f: pass
            elif op == 0x21: r[o[0]] = len(r[o[1]])
            elif op == 0x22: r[o[0]] = [] if types[o[1]] == mk.SB else ("new", types[o[1]])
            elif op == 0x23: r[o[0]] = [0 if types[o[2]] == "[B" else None] * r[o[1]]
            elif op == 0x29: nxt = at + o[0]
            elif 0x32 <= op <= 0x37:
                a, b = r[o[0]], r[o[1]]
                if [a == b, a != b, a < b, a >= b, a > b, a <= b][op - 0x32]: nxt = at + o[2]
            elif 0x38 <= op <= 0x3d:
                a = r[o[0]]
                a = 0 if a is None else (1 if not isinstance(a, int) else a)
                if [a == 0, a != 0, a < 0, a >= 0, a > 0, a <= 0][op - 0x38]: nxt = at + o[1]
            elif op == 0x46: r[o[0]] = r[o[1]][r[o[2]]]
            elif op in (0x4d, 0x4f): r[o[1]][r[o[2]]] = r[o[0]]
            elif op == 0x62: r[o[0]] = env.sfield(self.fkeys[o[1]])
            elif f in ("35c", "3rc"):
                mi, rs = o
                av = [r[x] for x in rs]
                mc, _, mn = methods[mi]
                if mc == "LWifictl;":
                    res = self.run(env, mn, av, depth + 1)
                elif self.mkeys[mi] == "str.new_b":                # new String(byte[]): the platform's UTF-8
                    s = bytes(b & 0xff for b in av[1]).decode("utf-8", "replace")
                    for k, x in enumerate(r):
                        if x is av[0]: r[k] = s
                    res = None
                else:
                    res = env.call(self.mkeys[mi], av)
            elif op in range(0x90, 0x9b):
                a, b = r[o[1]], r[o[2]]
                r[o[0]] = i32({0x90: a + b, 0x95: a & b, 0x96: a | b, 0x98: a << (b & 31)}[op])
            elif 0xd8 <= op <= 0xe2:
                a, n = r[o[1]], o[2]
                r[o[0]] = i32({0xd8: a + n, 0xde: a | n, 0xe0: a << (n & 31),
                               0xe2: (a & 0xffffffff) >> (n & 31)}[op])
            elif op == 0x8d: r[o[0]] = ((r[o[1]] & 0xff) ^ 0x80) - 0x80
            else:
                raise AssertionError(f"{name}: {op:#x} not run here")
            at = nxt


def main():
    dex = Dex(open(DEX, "rb").read())
    fails = []

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond: fails.append(what)

    def run(w, *argv):
        env = JavaEnv(w)
        code = 0
        try:
            dex.run(env, "main", [list(argv)])
        except Exit as e:
            code = e.args[0]
        return env.out, env.err, code

    w = Wifi()
    out, err, code = run(w)
    check(code == 2 and err == ["wifictl: no verb"], "no verb: exit 2")
    out, err, code = run(w, "frob")
    check(code == 2 and err == ["wifictl: unknown frob"], "unknown verb: exit 2")
    out, _, _ = run(w, "name", "4b69746368656e20c3a4")
    check(out == ["Kitchen ä"], "name: hex to text, UTF-8")
    out, _, _ = run(w, "wifi-on")
    check(out == ["OK"] and w.enabled and w.calls[-1] == ("wifi", None, True), "wifi-on: setWifiEnabled(null, true)")
    psk = "0123456789abcdefABCDEF0123456789abcdef0123456789abcdef0123456789"
    out, _, _ = run(w, "add", "486f6d65", psk)
    check(out == ["0"], "add: prints the new id")
    check(w.calls[-1] == ("add", '"Home"', psk, True, [1]),
          "add: SSID quoted, the PSK as given (64 hex, unquoted), hidden, WPA_PSK")
    out, _, _ = run(w, "add", "436166c3a9", "-")
    check(out == ["1"] and w.calls[-1] == ("add", '"Café"', None, True, []), "add: open network, no key")
    out, _, _ = run(w, "add", "486f6d65", "f" * 64)
    check(out == ["0"] and len(w.nets) == 2, "add: the same name replaces")
    out, _, _ = run(w, "select", "1")
    check(out == ["OK"] and w.calls[-2:] == [("enable", 1, True), "reconnect"], "select: enableNetwork(id, true), reconnect")
    out, _, _ = run(w, "enable", "0", "keep")
    check(out == ["OK"] and w.calls[-1] == ("enable", 0, False), "enable keep")
    out, _, _ = run(w, "enable", "0", "only")
    check(out == ["OK"] and w.calls[-1] == ("enable", 0, True), "enable only")
    out, _, _ = run(w, "enable", "7", "keep")
    check(out == ["FAIL"], "enable of no such network: FAIL")
    w.current, w.state, w.ip = 1, "COMPLETED", 0x1401a8c0
    for n in w.nets: n.status = 0 if n.networkId == 1 else 1
    out, _, _ = run(w, "status")
    check(out == ["wpa_state=COMPLETED", "id=1", 'ssid="Café"', "ip_address=192.168.1.20", "freq=2437"],
          "status: wpa_cli's shape, address in dotted form")
    w.state, w.ip = "FOUR_WAY_HANDSHAKE", 0
    out, _, _ = run(w, "status")
    check(out[0] == "wpa_state=4WAY_HANDSHAKE" and not any(x.startswith("ip_address") for x in out),
          "status: FOUR_WAY_HANDSHAKE as wpa_cli names it, no address line without one")
    out, _, _ = run(w, "list_networks")
    check(out == ["network id / ssid / bssid / flags", '0\t"Home"\tany\t[DISABLED]', '1\t"Café"\tany\t[CURRENT]'],
          "list_networks: ids, quoted names, Status 0 current / 1 disabled")
    out, _, _ = run(w, "keys")
    check(out == ["0\t" + "f" * 64, "1\t-"], "keys: id, tab, the stored key as it is, - for none")
    out, _, _ = run(w, "scan_results")
    check(out == ["bssid / frequency / signal level / flags / ssid",
                  "aa:00:00:00:00:01\t2437\t-60\t[WPA2-PSK-CCMP][ESS]\tHome",
                  "aa:00:00:00:00:02\t5180\t-71\t[ESS]\tCafé"], "scan_results: one row per network")
    out, _, _ = run(w, "scan")
    check(out == ["OK"] and w.calls[-1] == "scan", "scan")
    out, _, _ = run(w, "remove", "0")
    check(out == ["OK"] and [n.networkId for n in w.nets] == [1], "remove")
    out, _, _ = run(w, "remove", "0")
    check(out == ["FAIL"], "remove of no such network: FAIL")
    out, _, _ = run(w, "reassociate")
    check(out == ["OK"] and w.calls[-1] == "reconnect", "reassociate: reconnect")
    if fails:
        print(f"{len(fails)} failed")
        sys.exit(1)
    print("wifictl.dex: all verbs as expected")


if __name__ == "__main__":
    main()
