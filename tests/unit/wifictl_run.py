#!/usr/bin/env python3
"""Runs scripts/device/wifictl.dex on the PC with tests/unit/dexrun.py's Dalvik interpreter, against a fake
WifiService behind the reflection.  No ART here (qemu-user cannot run the firmware's), and the type checks of
mkwifictl.py --check say nothing about what the code computes; this does: every verb, its output, and what it
hands WifiService.  The reflection lookups must name the signatures mkwifictl.py's REFLECTED list holds, which
--framework=<rootfs> checks against the firmware's own classes."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dexrun                                                  # noqa: E402
from dexrun import Exit, ROOT                                  # noqa: E402
import mkwifictl as mk                                         # noqa: E402  (dexrun put tools/ on the path)

DEX = os.path.join(ROOT, "scripts", "device", "wifictl.dex")


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
    AS = ("Landroid/net/wifi/IWifiManager;",)

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


class Audio:
    """IAudioService's proxy: stream maxima as 8149's AudioService has them by default"""
    JAVA = "android.media.IAudioService$Stub$Proxy"
    AS = ("Landroid/media/IAudioService;",)

    def __init__(self):
        self.max = {0: 5, 1: 7, 2: 7, 3: 15, 4: 7, 5: 7}
        self.vol = {k: 2 for k in self.max}
        self.calls = []

    def getStreamMaxVolume(self, k): return self.max[k]
    def setStreamVolume(self, k, i, flags, pkg):
        self.calls.append((k, i, flags, pkg)); self.vol[k] = min(i, self.max[k])


class JavaEnv(dexrun.JavaEnv):
    CLASSES = (Config, Info, Scan, Wifi, Audio)
    NAMES = ("android.os.ServiceManager", "android.net.wifi.IWifiManager$Stub", "android.media.IAudioService$Stub",
             "android.os.IBinder", "android.net.wifi.ScanSettings", "android.os.WorkSource")

    def __init__(self, wifi, audio=None):
        super().__init__(mk.PROG)
        self.wifi, self.audio = wifi, audio or Audio()

    def static_call(self, cls, name, args):
        if cls == "android.os.ServiceManager":
            assert args in (["wifi"], ["audio"]); return "binder:" + args[0]
        if cls == "android.net.wifi.IWifiManager$Stub":
            assert args == ["binder:wifi"]; return self.wifi
        if cls == "android.media.IAudioService$Stub":
            assert args == ["binder:audio"]; return self.audio
        return super().static_call(cls, name, args)


def main():
    dex = dexrun.Dex(open(DEX, "rb").read(), mk.PROG)
    check = dexrun.Checks()

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
    au = Audio()
    env = JavaEnv(Wifi(), au)
    dex.run(env, "main", [["volume-max"]])
    check(env.out == ["0=5", "1=7", "3=15", "OK"] and au.calls == [(0, 5, 0, "root"), (1, 7, 0, "root"), (3, 15, 0, "root")],
          "volume-max: voice call, system and music at their maximum, as \"root\" (no Wi-Fi asked)")
    out, _, _ = run(w, "reassociate")
    check(out == ["OK"] and w.calls[-1] == "reconnect", "reassociate: reconnect")
    check.done("wifictl.dex: all verbs as expected")


if __name__ == "__main__":
    main()
