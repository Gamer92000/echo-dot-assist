#!/usr/bin/env python3
"""Assembles scripts/device/wifictl.dex, checked into the repository, from nothing but this file: a one-class
Java program, built as Dalvik bytecode, that talks to the WifiService binder on the models where Android's
framework owns the Wi-Fi (checkers, the Echo Show 5 1st gen).  That is how those models join a network
without their touchscreen: what stock's own setup apps do (com.amazon.kindle.otter.oobe and
com.amazon.ds2.oobe.efd put an android.net.wifi.WifiConfiguration in), only from root's shell, so the guided
setup and the settings page's network switch can do it too.  On the Echo it runs as

    CLASSPATH=<dir>/wifictl.dex app_process / Wifictl <verb> [args]

Reflection only (Class.forName/getMethod/getField): the dex references java.* classes alone, so it loads on
any Android 7 and ART's verifier has nothing to resolve but the platform.  The WifiService methods are the
ones of the firmware's own framework dex (boot-framework.oat of 8149): addOrUpdateNetwork(WifiConfiguration)I,
enableNetwork(IZ)Z, getConfiguredNetworks()List, getConnectionInfo()WifiInfo, getScanResults(String)List,
reconnect()V, removeNetwork(I)Z, setWifiEnabled(String,Z)Z (Fire OS names the caller), startScan(ScanSettings,
WorkSource)V.  Output is shaped like wpa_cli's, so scripts/device/wifi.sh can serve both worlds.  Verbs:

    status                          wpa_state=, id=, ssid=, ip_address=, freq=     (wpa_cli status)
    list_networks                   the configured networks, wpa_cli list_networks's shape
    keys                            id, a tab, the stored preSharedKey as it is (- for none), per network:
                                    root reads it to put a key back that an add of the same name replaced
    scan_results                    bssid / frequency / signal level / flags / ssid
    scan                            OK when the framework took it (wpa_cli scan)
    add <ssid hex> <psk|->          addOrUpdateNetwork, prints the network id; psk = the WPA PSK itself
                                    (64 hex digits, as wifi.c derives it), - = open
    enable <id> <only|keep>         enableNetwork(id, disableOthers)
    select <id>                     enableNetwork(id, true) and reconnect()
    remove <id>                     removeNetwork(id)
    reassociate                     reconnect()
    wifi-on                         setWifiEnabled(null, true)
    name <hex>                      the decoded text, for the shell's messages
    volume-max                      the voice call, system and music streams at their maximum, through
                                    IAudioService (no Wi-Fi needed): hassmic's own gain per player is the
                                    volume, Android's may not take more off (main.sh, every satellite start)

The Java source it stands for:

    public class Wifictl {
        public static void main(String[] a) {
            if (a.length < 1) { System.err.println("wifictl: no verb"); System.exit(2); }
            if (a[0].equals("volume-max")) { volmax(); return; }
            Object w = wifi();
            if (w == null) { System.err.println("wifictl: no wifi service"); System.exit(2); }
            String v = a[0];
            if (v.equals("status")) status(w);
            else if (v.equals("list_networks")) list(w);
            else if (v.equals("keys")) keys(w);
            else if (v.equals("scan_results")) scanres(w);
            else if (v.equals("scan")) startscan(w);
            else if (v.equals("add")) add(w, a);
            else if (v.equals("enable")) enable(w, a);
            else if (v.equals("select")) select(w, a);
            else if (v.equals("remove")) remove(w, a);
            else if (v.equals("reassociate")) reassociate(w);
            else if (v.equals("wifi-on")) wifion(w);
            else if (v.equals("name")) name(a);
            else { System.err.println("wifictl: unknown " + v); System.exit(2); }
        }
        static Object wifi() {
            Object b = sinv("android.os.ServiceManager", "getService", "java.lang.String", "wifi");
            return b == null ? null
                : sinv("android.net.wifi.IWifiManager$Stub", "asInterface", "android.os.IBinder", b);
        }
        static Class cls(String n) {             // Class.forName knows no primitive types
            if (n.equals("int")) return Integer.TYPE;
            if (n.equals("boolean")) return Boolean.TYPE;
            return Class.forName(n);
        }
        static Object inv(Object o, String n, String[] ps, Object[] as) {   // an instance method, by name
            Class[] cs = new Class[ps.length];
            for (int i = 0; i < ps.length; i++) cs[i] = cls(ps[i]);
            java.lang.reflect.Method m = o.getClass().getMethod(n, cs);
            m.setAccessible(true);                // the binder proxy class is private
            return m.invoke(o, as);
        }
        static Object sinv(String c, String n, String p, Object a) {        // a static one, one argument
            Class k = Class.forName(c);
            Class[] cs = { cls(p) };
            Object[] as = { a };
            return k.getMethod(n, cs).invoke(null, as);
        }
        static Object w1(Object w, String n, String p, Object a) { return inv(w, n, new String[]{p}, new Object[]{a}); }
        static Object w2(Object w, String n, String p1, String p2, Object a1, Object a2) {
            return inv(w, n, new String[]{p1, p2}, new Object[]{a1, a2});
        }
        static Object os(Object o, String n) { return inv(o, n, new String[0], new Object[0]); }
        static int iz(Object o, String n) { return ((Integer) os(o, n)).intValue(); }
        static Object fget(Object o, String f) { return o.getClass().getField(f).get(o); }
        static void fset(Object o, String f, Object v) { o.getClass().getField(f).set(o, v); }
        static String cat(Object a, Object b) { return new StringBuilder().append(a).append(b).toString(); }
        static void p(String s) { System.out.println(s); }
        static void e(String s) { System.err.println(s); }
        static void ps(Object o) { System.out.println(String.valueOf(o)); }
        static String ip(int i) {
            StringBuilder s = new StringBuilder();
            s.append(i & 255).append('.').append((i >>> 8) & 255).append('.').append((i >>> 16) & 255)
             .append('.').append((i >>> 24) & 255);
            return s.toString();
        }
        static byte[] unhex(String s) {
            byte[] b = new byte[s.length() / 2];
            for (int i = 0; i < b.length; i++) b[i] = (byte) ((d(s.charAt(2 * i)) << 4) | d(s.charAt(2 * i + 1)));
            return b;
        }
        static int d(int c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }
        static String hex2s(String s) { return new String(unhex(s)); }

        static void status(Object w) {
            Object info = os(w, "getConnectionInfo");
            Object sup = os(info, "getSupplicantState").toString();
            if (sup.equals("FOUR_WAY_HANDSHAKE")) sup = "4WAY_HANDSHAKE";  // wpa_cli's name for it
            p(cat("wpa_state=", sup));
            p(cat("id=", os(info, "getNetworkId")));
            p(cat("ssid=", os(info, "getSSID")));
            int x = iz(info, "getIpAddress");
            if (x != 0) p(cat("ip_address=", ip(x)));
            x = iz(info, "getFrequency");
            if (x != 0) p(cat("freq=", Integer.valueOf(x)));
        }
        static void list(Object w) {
            p("network id / ssid / bssid / flags");
            for (Object c : (java.util.List) os(w, "getConfiguredNetworks")) {
                int st = ((Integer) fget(c, "status")).intValue();          // Status: 0 on it, 1 disabled, 2 enabled
                Object fl = st == 0 ? "[CURRENT]" : st == 1 ? "[DISABLED]" : "";
                p(cat(cat(cat(cat(fget(c, "networkId"), "\\t"), fget(c, "SSID")), "\\tany\\t"), fl));
            }
        }
        static void keys(Object w) {
            for (Object c : (java.util.List) os(w, "getPrivilegedConfiguredNetworks")) {
                Object k = fget(c, "preSharedKey");
                p(cat(cat(fget(c, "networkId"), "\\t"), k == null ? "-" : k));
            }
        }
        static void scanres(Object w) {
            p("bssid / frequency / signal level / flags / ssid");
            for (Object r : (java.util.List) w1(w, "getScanResults", "java.lang.String", null)) {
                Object s = "";
                s = cat(s, fget(r, "BSSID")); s = cat(s, "\\t"); s = cat(s, fget(r, "frequency")); s = cat(s, "\\t");
                s = cat(s, fget(r, "level")); s = cat(s, "\\t");
                s = cat(s, fget(r, "capabilities")); s = cat(s, "\\t"); s = cat(s, fget(r, "SSID"));
                p(s);
            }
        }
        static void startscan(Object w) {
            w2(w, "startScan", "android.net.wifi.ScanSettings", "android.os.WorkSource", null, null);
            p("OK");
        }
        static void add(Object w, String[] a) {
            Object c = Class.forName("android.net.wifi.WifiConfiguration").newInstance();
            fset(c, "hiddenSSID", Boolean.TRUE);       // found even when the name is not broadcast
            fset(c, "SSID", cat(cat("\\"", hex2s(a[1])), "\\""));
            if (!a[2].equals("-")) {
                fset(c, "preSharedKey", a[2]);         // the PSK itself: 64 hex digits, unquoted
                ((java.util.BitSet) fget(c, "allowedKeyManagement")).set(1);  // KeyMgmt.WPA_PSK
            }
            ps(w1(w, "addOrUpdateNetwork", "android.net.wifi.WifiConfiguration", c));
        }
        static void enable(Object w, String[] a) {
            Object r = w2(w, "enableNetwork", "int", "boolean",
                          Integer.valueOf(Integer.parseInt(a[1])), Boolean.valueOf(a[2].equals("only")));
            p(Boolean.TRUE.equals(r) ? "OK" : "FAIL");
        }
        static void select(Object w, String[] a) {
            Object r = w2(w, "enableNetwork", "int", "boolean",
                          Integer.valueOf(Integer.parseInt(a[1])), Boolean.TRUE);
            os(w, "reconnect");
            p(Boolean.TRUE.equals(r) ? "OK" : "FAIL");
        }
        static void remove(Object w, String[] a) {
            Object r = w1(w, "removeNetwork", "int", Integer.valueOf(Integer.parseInt(a[1])));
            p(Boolean.TRUE.equals(r) ? "OK" : "FAIL");
        }
        static void reassociate(Object w) { os(w, "reconnect"); p("OK"); }
        static void wifion(Object w) {
            Object r = w2(w, "setWifiEnabled", "java.lang.String", "boolean", null, Boolean.TRUE);
            p(Boolean.TRUE.equals(r) ? "OK" : "FAIL");
        }
        static void name(String[] a) { p(hex2s(a[1])); }
        static void volmax() {                    // as root: AppOps knows uid 0 as package "root"
            Object b = sinv("android.os.ServiceManager", "getService", "java.lang.String", "audio");
            Object s = sinv("android.media.IAudioService$Stub", "asInterface", "android.os.IBinder", b);
            for (int k : new int[]{ 0, 1, 3 }) {     // STREAM_VOICE_CALL (Drop In), STREAM_SYSTEM (sounds), STREAM_MUSIC
                Object max = w1(s, "getStreamMaxVolume", "int", Integer.valueOf(k));
                inv(s, "setStreamVolume", new String[]{ "int", "int", "int", "java.lang.String" },
                    new Object[]{ Integer.valueOf(k), max, Integer.valueOf(0), "root" });
                p(cat(cat(Integer.valueOf(k), "="), max));
            }
            p("OK");
        }
    }

python3 tools/mkwifictl.py            rebuild scripts/device/wifictl.dex
python3 tools/mkwifictl.py --check    only re-read and check the one in the tree
    --framework=firmware/checkers/rootfs   also against the firmware's boot class path (make unit does it
                                           when the firmware is unpacked)

The assembler, writer, reader and checks are tools/dexasm.py's (shared with tools/mkringbar.py).  What the
writer guarantees (ART's DexFileVerifier on the Echo is strict): id tables sorted as the format wants (strings
by content, the rest by their indices), one class LWifictl; with 30 static methods and no fields, no debug info,
no tries.  read() parses the result back, checks checksum and signature, walks the map, decodes every
instruction and runs a type-flow check over every method (verify(): what ART's method verifier would refuse of
this subset), before the file is written out.  --framework resolves every class, method and field the dex
names, and every one its reflection asks for (REFLECTED), in the firmware's own boot*.oat.  What the code
computes is tests/unit/wifictl_run.py's: it interprets the dex against a fake WifiService (make unit).
qemu-user cannot run the firmware's ART, so none of this has run on an Echo."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dexasm import (Code, Program, AGET_OBJ, APUT_OBJ, APUT_BYTE, VIRT, DIRECT, STATIC, IFACE,  # noqa: E402
                    STATIC_RANGE, ADD, AND, OR, ADD8, OR8, SHL8, USHR8, IFNE, IFGE, IFLT, IFEQZ, IFNEZ, IFGTZ,
                    SB, STR, CLS, OBJ, LST, BST, INTG, CLSARR, STRARR, OBJARR, BYTES)
import dexasm                                                  # noqa: E402

KEY_WPA_PSK = 1                              # WifiConfiguration.KeyMgmt.WPA_PSK (--framework checks it)
ME = "LWifictl;"

# the class's own methods: (name, params, return)
SELF = [
    ("main",  (STRARR,), "V"),
    ("wifi",  (), OBJ),
    ("cls",   (STR,), CLS),
    ("inv",   (OBJ, STR, STRARR, OBJARR), OBJ),
    ("sinv",  (STR, STR, STR, OBJ), OBJ),
    ("w1",    (OBJ, STR, STR, OBJ), OBJ),
    ("w2",    (OBJ, STR, STR, STR, OBJ, OBJ), OBJ),
    ("iz",    (OBJ, STR), "I"),
    ("os",    (OBJ, STR), OBJ),
    ("fget",  (OBJ, STR), OBJ),
    ("fset",  (OBJ, STR, OBJ), "V"),
    ("cat",   (OBJ, OBJ), STR),
    ("p",     (STR,), "V"),
    ("e",     (STR,), "V"),
    ("ps",    (OBJ,), "V"),
    ("ip",    ("I",), STR),
    ("unhex", (STR,), BYTES),
    ("hex2s", (STR,), STR),
    ("status", (OBJ,), "V"),
    ("list",  (OBJ,), "V"),
    ("keys",  (OBJ,), "V"),
    ("scanres", (OBJ,), "V"),
    ("startscan", (OBJ,), "V"),
    ("add",   (OBJ, STRARR), "V"),
    ("enable", (OBJ, STRARR), "V"),
    ("select", (OBJ, STRARR), "V"),
    ("remove", (OBJ, STRARR), "V"),
    ("reassociate", (OBJ,), "V"),
    ("wifion", (OBJ,), "V"),
    ("name", (STRARR,), "V"),
    ("volmax", (), "V"),
]
SELF_BY_NAME = {n: (ps, r) for n, ps, r in SELF}


# ---------------------------------------------------------------- the methods, as bytecode
def emit():
    m = []

    def main():
        # a=0 t=1 w=2 x=3 r=4
        c = Code(5, 1)
        c.array_length(1, 0)
        c.ifz(IFGTZ, 1, "args")
        c.const_string(3, "wifictl: no verb")
        c.sget_obj(4, "sys.err"); c.invoke(VIRT, "println", [4, 3])
        c.const_4(3, 2); c.invoke(STATIC, "sys.exit", [3]); c.ret_void()
        c.label("args")
        c.aget_at(3, 0, 0)
        c.const_string(1, "volume-max"); c.invoke(VIRT, "str.eq", [3, 1]); c.move_result(1)
        c.ifz(IFEQZ, 1, "wifi")
        c.invoke(STATIC, "volmax", []); c.ret_void()
        c.label("wifi")
        c.invoke(STATIC, "wifi", []); c.move_result(2, True)
        c.ifz(IFNEZ, 2, "svc")
        c.const_string(3, "wifictl: no wifi service")
        c.sget_obj(4, "sys.err"); c.invoke(VIRT, "println", [4, 3])
        c.const_4(3, 2); c.invoke(STATIC, "sys.exit", [3]); c.ret_void()
        c.label("svc")
        c.aget_at(3, 0, 0)                                    # v = a[0]
        for verb, name in [("status", "status"), ("list_networks", "list"), ("scan_results", "scanres"),
                           ("keys", "keys"), ("scan", "startscan"), ("add", "add"), ("enable", "enable"),
                           ("select", "select"), ("remove", "remove"), ("reassociate", "reassociate"),
                           ("wifi-on", "wifion"), ("name", "name")]:
            c.const_string(1, verb)
            c.invoke(VIRT, "str.eq", [3, 1]); c.move_result(1)
            c.ifz(IFEQZ, 1, verb)
            ps = SELF_BY_NAME[name][0]                        # (w), (w, a) or (a)
            c.invoke(STATIC, name, [{OBJ: 2, STRARR: 0}[t] for t in ps]); c.ret_void()
            c.label(verb)
        c.const_string(1, "wifictl: unknown ")
        c.invoke(STATIC, "cat", [1, 3]); c.move_result(1, True)
        c.sget_obj(4, "sys.err"); c.invoke(VIRT, "println", [4, 1])
        c.const_4(1, 2); c.invoke(STATIC, "sys.exit", [1])
        c.ret_void()
        m.append(("main", c))

    def wifi():
        # b=0 t=1 p=2 a=3
        c = Code(4, 0)
        c.const_string(0, "android.os.ServiceManager")
        c.const_string(1, "getService")
        c.const_string(2, "java.lang.String")
        c.const_string(3, "wifi")
        c.invoke(STATIC, "sinv", [0, 1, 2, 3]); c.move_result(0, True)
        c.ifz(IFNEZ, 0, "ok")
        c.const_4(0, 0); c.ret(0, True)                       # no service: null
        c.label("ok")
        c.const_string(1, "android.net.wifi.IWifiManager$Stub")
        c.const_string(2, "asInterface")
        c.const_string(3, "android.os.IBinder")
        c.invoke(STATIC, "sinv", [1, 2, 3, 0]); c.move_result(0, True)
        c.ret(0, True)
        m.append(("wifi", c))

    def cls():
        # n=0 t=1
        c = Code(2, 1)
        c.const_string(1, "int"); c.invoke(VIRT, "str.eq", [0, 1]); c.move_result(1)
        c.ifz(IFEQZ, 1, "z")
        c.sget_obj(1, "int.TYPE"); c.ret(1, True)
        c.label("z")
        c.const_string(1, "boolean"); c.invoke(VIRT, "str.eq", [0, 1]); c.move_result(1)
        c.ifz(IFEQZ, 1, "l")
        c.sget_obj(1, "bool.TYPE"); c.ret(1, True)
        c.label("l")
        c.invoke(STATIC, "cls.forName", [0]); c.move_result(1, True)
        c.ret(1, True)
        m.append(("cls", c))

    def inv():
        # o=0 n=1 ps=2 as=3 cs=4 m=5 i=6 len=7 r=8
        c = Code(9, 4)
        c.array_length(7, 2)
        c.new_array(4, 7, CLSARR)
        c.const_4(6, 0)
        c.label("loop")
        c.if2(IFGE, 6, 7, "done")
        c.aget(8, 2, 6, AGET_OBJ)
        c.invoke(STATIC, "cls", [8]); c.move_result(8, True)
        c.aput(8, 4, 6, APUT_OBJ)
        c.binop_lit8(ADD8, 6, 6, 1)
        c.goto_("loop")
        c.label("done")
        c.invoke(VIRT, "obj.getcls", [0]); c.move_result(7, True)
        c.invoke(VIRT, "cls.getm", [7, 1, 4]); c.move_result(5, True)
        c.const_4(7, 1); c.invoke(VIRT, "m.setacc", [5, 7])   # the binder proxy class is private
        c.invoke(VIRT, "m.invoke", [5, 0, 3]); c.move_result(7, True)
        c.ret(7, True)
        m.append(("inv", c))

    def sinv():
        # c=0 n=1 p=2 a=3 k=4 cs=5 as=6 m=7 z=8
        c = Code(9, 4)
        c.invoke(STATIC, "cls.forName", [0]); c.move_result(4, True)
        c.invoke(STATIC, "cls", [2]); c.move_result(7, True)
        c.const_4(8, 1); c.new_array(5, 8, CLSARR); c.const_4(8, 0); c.aput(7, 5, 8, APUT_OBJ)
        c.const_4(8, 1); c.new_array(6, 8, OBJARR); c.const_4(8, 0); c.aput(3, 6, 8, APUT_OBJ)
        c.invoke(VIRT, "cls.getm", [4, 1, 5]); c.move_result(7, True)
        c.const_4(8, 0)                                       # null: the static call
        c.invoke(VIRT, "m.invoke", [7, 8, 6]); c.move_result(7, True)
        c.ret(7, True)
        m.append(("sinv", c))

    def w1():
        # w=0 n=1 p=2 a=3 ps=4 as=5 r=6
        c = Code(7, 4)
        c.const_4(6, 1); c.new_array(4, 6, STRARR); c.const_4(6, 0); c.aput(2, 4, 6, APUT_OBJ)
        c.const_4(6, 1); c.new_array(5, 6, OBJARR); c.const_4(6, 0); c.aput(3, 5, 6, APUT_OBJ)
        c.invoke(STATIC, "inv", [0, 1, 4, 5]); c.move_result(6, True)
        c.ret(6, True)
        m.append(("w1", c))

    def w2():
        # w=0 n=1 p1=2 p2=3 a1=4 a2=5 ps=6 as=7 r=8
        c = Code(9, 6)
        c.const_4(8, 2); c.new_array(6, 8, STRARR)
        c.const_4(8, 0); c.aput(2, 6, 8, APUT_OBJ); c.const_4(8, 1); c.aput(3, 6, 8, APUT_OBJ)
        c.const_4(8, 2); c.new_array(7, 8, OBJARR)
        c.const_4(8, 0); c.aput(4, 7, 8, APUT_OBJ); c.const_4(8, 1); c.aput(5, 7, 8, APUT_OBJ)
        c.invoke(STATIC, "inv", [0, 1, 6, 7]); c.move_result(8, True)
        c.ret(8, True)
        m.append(("w2", c))

    def iz():
        # o=0 n=1 r=2
        c = Code(3, 2)
        c.invoke(STATIC, "os", [0, 1]); c.move_result(2, True)
        c.check_cast(2, INTG)
        c.invoke(VIRT, "int.intval", [2]); c.move_result(2)
        c.ret(2, False)
        m.append(("iz", c))

    def os():
        # o=0 n=1 ps=2 as=3 r=4
        c = Code(5, 2)
        c.const_4(4, 0); c.new_array(2, 4, STRARR)
        c.const_4(4, 0); c.new_array(3, 4, OBJARR)
        c.invoke(STATIC, "inv", [0, 1, 2, 3]); c.move_result(4, True)
        c.ret(4, True)
        m.append(("os", c))

    def fget():
        # o=0 n=1 k=2 f=3
        c = Code(4, 2)
        c.invoke(VIRT, "obj.getcls", [0]); c.move_result(2, True)
        c.invoke(VIRT, "cls.getf", [2, 1]); c.move_result(3, True)
        c.invoke(VIRT, "f.get", [3, 0]); c.move_result(2, True)
        c.ret(2, True)
        m.append(("fget", c))

    def fset():
        # o=0 n=1 v=2 k=3 f=4
        c = Code(5, 3)
        c.invoke(VIRT, "obj.getcls", [0]); c.move_result(3, True)
        c.invoke(VIRT, "cls.getf", [3, 1]); c.move_result(4, True)
        c.invoke(VIRT, "f.set", [4, 0, 2])
        c.ret_void()
        m.append(("fset", c))

    def cat():
        # a=0 b=1 sb=2 r=3
        c = Code(4, 2)
        c.new_instance(2, SB); c.invoke(DIRECT, "SB.new", [2])
        c.invoke(VIRT, "SB.appendO", [2, 0])
        c.invoke(VIRT, "SB.appendO", [2, 1])
        c.invoke(VIRT, "SB.to", [2]); c.move_result(3, True)
        c.ret(3, True)
        m.append(("cat", c))

    def p(err=False):
        # s=0 out=1
        c = Code(2, 1)
        c.sget_obj(1, "sys.err" if err else "sys.out")
        c.invoke(VIRT, "println", [1, 0])
        c.ret_void()
        m.append(("e" if err else "p", c))

    def ps():
        # o=0 out=1 s=2
        c = Code(3, 1)
        c.invoke(STATIC, "str.valueO", [0]); c.move_result(2, True)
        c.sget_obj(1, "sys.out")
        c.invoke(VIRT, "println", [1, 2])
        c.ret_void()
        m.append(("ps", c))

    def ip():
        # i=0 sb=1 x=2 m=3
        c = Code(4, 1)
        c.new_instance(1, SB); c.invoke(DIRECT, "SB.new", [1])
        c.const_16(3, 255)
        c.binop(AND, 2, 0, 3); c.invoke(VIRT, "SB.appendI", [1, 2])
        c.const_string(2, "."); c.invoke(VIRT, "SB.appendO", [1, 2])
        c.binop_lit8(USHR8, 2, 0, 8); c.binop(AND, 2, 2, 3); c.invoke(VIRT, "SB.appendI", [1, 2])
        c.const_string(2, "."); c.invoke(VIRT, "SB.appendO", [1, 2])
        c.binop_lit8(USHR8, 2, 0, 16); c.binop(AND, 2, 2, 3); c.invoke(VIRT, "SB.appendI", [1, 2])
        c.const_string(2, "."); c.invoke(VIRT, "SB.appendO", [1, 2])
        c.binop_lit8(USHR8, 2, 0, 24); c.binop(AND, 2, 2, 3); c.invoke(VIRT, "SB.appendI", [1, 2])
        c.invoke(VIRT, "SB.to", [1]); c.move_result(2, True)
        c.ret(2, True)
        m.append(("ip", c))

    def unhex():
        # s=0 n=1 b=2 i=3 ch=4 d=5 t=6 v=7
        c = Code(8, 1)
        c.invoke(VIRT, "str.length", [0]); c.move_result(1)
        c.binop_lit8(USHR8, 1, 1, 1)
        c.new_array(2, 1, BYTES)
        c.const_4(3, 0)
        c.label("loop")
        c.if2(IFGE, 3, 1, "done")
        # the high digit: d(s.charAt(2i))
        c.binop(ADD, 6, 3, 3)
        c.invoke(VIRT, "str.charat", [0, 6]); c.move_result(4)
        c.binop_lit8(ADD8, 5, 4, -48)
        c.const_16(6, 10); c.if2(IFLT, 5, 6, "h")
        c.binop_lit8(OR8, 5, 4, 32); c.binop_lit8(ADD8, 5, 5, -87)
        c.label("h")
        # the low digit: d(s.charAt(2i+1))
        c.binop(ADD, 6, 3, 3); c.binop_lit8(ADD8, 6, 6, 1)
        c.invoke(VIRT, "str.charat", [0, 6]); c.move_result(4)
        c.binop_lit8(ADD8, 7, 4, -48)
        c.const_16(6, 10); c.if2(IFLT, 7, 6, "l")
        c.binop_lit8(OR8, 7, 4, 32); c.binop_lit8(ADD8, 7, 7, -87)
        c.label("l")
        c.binop_lit8(SHL8, 6, 5, 4)
        c.binop(OR, 7, 6, 7)
        c.int_to_byte(6, 7)
        c.aput(6, 2, 3, APUT_BYTE)
        c.binop_lit8(ADD8, 3, 3, 1)
        c.goto_("loop")
        c.label("done")
        c.ret(2, True)
        m.append(("unhex", c))

    def hex2s():
        # s=0 b=1 r=2
        c = Code(3, 1)
        c.invoke(STATIC, "unhex", [0]); c.move_result(1, True)
        c.new_instance(2, STR); c.invoke(DIRECT, "str.new_b", [2, 1])
        c.ret(2, True)
        m.append(("hex2s", c))

    def status():
        # w=0 info=1 sup=2 x=3 t=4
        c = Code(5, 1)
        c.const_string(2, "getConnectionInfo"); c.invoke(STATIC, "os", [0, 2]); c.move_result(1, True)
        c.const_string(2, "getSupplicantState"); c.invoke(STATIC, "os", [1, 2]); c.move_result(2, True)
        c.invoke(VIRT, "obj.tostr", [2]); c.move_result(2, True)
        c.const_string(4, "FOUR_WAY_HANDSHAKE"); c.invoke(VIRT, "str.eq", [2, 4]); c.move_result(4)
        c.ifz(IFEQZ, 4, "sup")
        c.const_string(2, "4WAY_HANDSHAKE")
        c.label("sup")
        c.const_string(3, "wpa_state="); c.invoke(STATIC, "cat", [3, 2]); c.move_result(2, True)
        c.invoke(STATIC, "p", [2])
        c.const_string(2, "getNetworkId"); c.invoke(STATIC, "os", [1, 2]); c.move_result(2, True)
        c.const_string(3, "id="); c.invoke(STATIC, "cat", [3, 2]); c.move_result(2, True)
        c.invoke(STATIC, "p", [2])
        c.const_string(2, "getSSID"); c.invoke(STATIC, "os", [1, 2]); c.move_result(2, True)
        c.const_string(3, "ssid="); c.invoke(STATIC, "cat", [3, 2]); c.move_result(2, True)
        c.invoke(STATIC, "p", [2])
        c.const_string(2, "getIpAddress"); c.invoke(STATIC, "iz", [1, 2]); c.move_result(3)
        c.ifz(IFEQZ, 3, "ipdone")
        c.invoke(STATIC, "ip", [3]); c.move_result(2, True)
        c.const_string(3, "ip_address="); c.invoke(STATIC, "cat", [3, 2]); c.move_result(2, True)
        c.invoke(STATIC, "p", [2])
        c.label("ipdone")
        c.const_string(2, "getFrequency"); c.invoke(STATIC, "iz", [1, 2]); c.move_result(3)
        c.ifz(IFEQZ, 3, "fdone")
        c.invoke(STATIC, "int.value", [3]); c.move_result(2, True)
        c.const_string(3, "freq="); c.invoke(STATIC, "cat", [3, 2]); c.move_result(2, True)
        c.invoke(STATIC, "p", [2])
        c.label("fdone")
        c.ret_void()
        m.append(("status", c))

    def list():
        # w=0 nets=1 it=2 c=3 ido=4 ssid=5 t=6 st=7 row=8 tab=9 anyt=10
        c = Code(11, 1)
        c.const_string(2, "getConfiguredNetworks"); c.invoke(STATIC, "os", [0, 2]); c.move_result(1, True)
        c.check_cast(1, LST)
        c.const_string(2, "network id / ssid / bssid / flags"); c.invoke(STATIC, "p", [2])
        c.invoke(IFACE, "list.iter", [1]); c.move_result(2, True)
        c.label("loop")
        c.invoke(IFACE, "it.has", [2]); c.move_result(6)
        c.ifz(IFEQZ, 6, "done")
        c.invoke(IFACE, "it.next", [2]); c.move_result(3, True)
        c.const_string(4, "networkId"); c.invoke(STATIC, "fget", [3, 4]); c.move_result(4, True)
        c.const_string(5, "SSID"); c.invoke(STATIC, "fget", [3, 5]); c.move_result(5, True)
        c.ifz(IFNEZ, 5, "ssid")
        c.const_string(5, "")
        c.label("ssid")
        c.const_string(6, "status"); c.invoke(STATIC, "fget", [3, 6]); c.move_result(6, True)
        c.check_cast(6, INTG); c.invoke(VIRT, "int.intval", [6]); c.move_result(7)
        c.ifz(IFNEZ, 7, "d")
        c.const_string(6, "[CURRENT]"); c.goto_("f")
        c.label("d")
        c.const_4(6, 1); c.if2(IFNE, 7, 6, "e")
        c.const_string(6, "[DISABLED]"); c.goto_("f")
        c.label("e")
        c.const_string(6, "")
        c.label("f")
        c.const_string(9, "\t")
        c.invoke(STATIC, "cat", [4, 9]); c.move_result(8, True)
        c.invoke(STATIC, "cat", [8, 5]); c.move_result(8, True)
        c.const_string(10, "\tany\t")
        c.invoke(STATIC, "cat", [8, 10]); c.move_result(8, True)
        c.invoke(STATIC, "cat", [8, 6]); c.move_result(8, True)
        c.invoke(STATIC, "p", [8])
        c.goto_("loop")
        c.label("done")
        c.ret_void()
        m.append(("list", c))

    def keys():
        # w=0 nets=1 it=2 c=3 id=4 k=5 t=6
        c = Code(7, 1)
        c.const_string(2, "getPrivilegedConfiguredNetworks"); c.invoke(STATIC, "os", [0, 2]); c.move_result(1, True)
        c.check_cast(1, LST)
        c.invoke(IFACE, "list.iter", [1]); c.move_result(2, True)
        c.label("loop")
        c.invoke(IFACE, "it.has", [2]); c.move_result(6)
        c.ifz(IFEQZ, 6, "done")
        c.invoke(IFACE, "it.next", [2]); c.move_result(3, True)
        c.const_string(4, "networkId"); c.invoke(STATIC, "fget", [3, 4]); c.move_result(4, True)
        c.const_string(5, "preSharedKey"); c.invoke(STATIC, "fget", [3, 5]); c.move_result(5, True)
        c.ifz(IFNEZ, 5, "k")
        c.const_string(5, "-")
        c.label("k")
        c.const_string(6, "\t")
        c.invoke(STATIC, "cat", [4, 6]); c.move_result(4, True)
        c.invoke(STATIC, "cat", [4, 5]); c.move_result(4, True)
        c.invoke(STATIC, "p", [4])
        c.goto_("loop")
        c.label("done")
        c.ret_void()
        m.append(("keys", c))

    def scanres():
        # w=0 rs=1 it=2 r=3 s=4 t=5 u=6
        c = Code(7, 1)
        c.const_string(2, "getScanResults")
        c.const_string(3, "java.lang.String")
        c.const_4(4, 0)
        c.invoke(STATIC, "w1", [0, 2, 3, 4]); c.move_result(1, True)
        c.check_cast(1, LST)
        c.const_string(2, "bssid / frequency / signal level / flags / ssid"); c.invoke(STATIC, "p", [2])
        c.invoke(IFACE, "list.iter", [1]); c.move_result(2, True)
        c.label("loop")
        c.invoke(IFACE, "it.has", [2]); c.move_result(5)
        c.ifz(IFEQZ, 5, "done")
        c.invoke(IFACE, "it.next", [2]); c.move_result(3, True)
        c.const_string(4, "")
        for f in ("BSSID", "frequency", "level", "capabilities", "SSID"):
            c.const_string(5, f); c.invoke(STATIC, "fget", [3, 5]); c.move_result(6, True)
            c.invoke(STATIC, "cat", [4, 6]); c.move_result(4, True)
            if f != "SSID":
                c.const_string(5, "\t")
                c.invoke(STATIC, "cat", [4, 5]); c.move_result(4, True)
        c.invoke(STATIC, "p", [4])
        c.goto_("loop")
        c.label("done")
        c.ret_void()
        m.append(("scanres", c))

    def startscan():
        # w=0, w2's arguments 1..6 (w copied: a range may not wrap past the parameter)
        c = Code(7, 1)
        c.move(1, 0, True)
        c.const_string(2, "startScan")
        c.const_string(3, "android.net.wifi.ScanSettings")
        c.const_string(4, "android.os.WorkSource")
        c.const_4(5, 0); c.const_4(6, 0)
        c.invoke_range(STATIC_RANGE, "w2", 1, 6)
        c.const_string(1, "OK"); c.invoke(STATIC, "p", [1])
        c.ret_void()
        m.append(("startscan", c))

    def add():
        # w=0 a=1 c=2 s=3 t=4 km=5
        c = Code(6, 2)
        c.const_string(2, "android.net.wifi.WifiConfiguration")
        c.invoke(STATIC, "cls.forName", [2]); c.move_result(2, True)
        c.invoke(VIRT, "cls.newi", [2]); c.move_result(2, True)
        c.const_string(3, "hiddenSSID"); c.sget_obj(4, "bool.T")
        c.invoke(STATIC, "fset", [2, 3, 4])
        c.aget_at(3, 1, 1)
        c.invoke(STATIC, "hex2s", [3]); c.move_result(3, True)
        c.const_string(4, chr(34))
        c.invoke(STATIC, "cat", [4, 3]); c.move_result(3, True)
        c.invoke(STATIC, "cat", [3, 4]); c.move_result(3, True)
        c.const_string(4, "SSID"); c.invoke(STATIC, "fset", [2, 4, 3])
        c.aget_at(3, 1, 2)
        c.const_string(4, "-"); c.invoke(VIRT, "str.eq", [3, 4]); c.move_result(4)
        c.ifz(IFNEZ, 4, "skip")
        c.const_string(4, "preSharedKey"); c.invoke(STATIC, "fset", [2, 4, 3])
        c.const_string(3, "allowedKeyManagement")
        c.invoke(STATIC, "fget", [2, 3]); c.move_result(3, True)
        c.check_cast(3, BST)
        c.const_4(4, KEY_WPA_PSK); c.invoke(VIRT, "bits.set", [3, 4])
        c.label("skip")
        c.const_string(3, "addOrUpdateNetwork")
        c.const_string(4, "android.net.wifi.WifiConfiguration")
        c.invoke(STATIC, "w1", [0, 3, 4, 2]); c.move_result(3, True)
        c.invoke(STATIC, "ps", [3])
        c.ret_void()
        m.append(("add", c))

    def okfail(c, r, t):
        c.sget_obj(t, "bool.T")
        c.invoke(VIRT, "obj.eq", [t, r]); c.move_result(t)
        c.ifz(IFEQZ, t, "fail")
        c.const_string(t, "OK"); c.invoke(STATIC, "p", [t]); c.ret_void()
        c.label("fail")
        c.const_string(t, "FAIL"); c.invoke(STATIC, "p", [t])
        c.ret_void()

    def enable():
        # w=0 a=1 id=2 box=3 r=4 t=5 w2 args 6..11
        c = Code(12, 2)
        c.aget_at(2, 1, 1)
        c.invoke(STATIC, "int.parse", [2]); c.move_result(2)
        c.invoke(STATIC, "int.value", [2]); c.move_result(3, True)
        c.aget_at(4, 1, 2)
        c.const_string(5, "only"); c.invoke(VIRT, "str.eq", [4, 5]); c.move_result(5)
        c.invoke(STATIC, "bool.value", [5]); c.move_result(4, True)
        c.move(6, 0, True); c.const_string(7, "enableNetwork")
        c.const_string(8, "int"); c.const_string(9, "boolean")
        c.move(10, 3, True); c.move(11, 4, True)
        c.invoke_range(STATIC_RANGE, "w2", 6, 6); c.move_result(4, True)
        okfail(c, 4, 5)
        m.append(("enable", c))

    def select():
        # w=0 a=1 id=2 box=3 r=4 t=5 w2 args 6..11
        c = Code(12, 2)
        c.aget_at(2, 1, 1)
        c.invoke(STATIC, "int.parse", [2]); c.move_result(2)
        c.invoke(STATIC, "int.value", [2]); c.move_result(3, True)
        c.move(6, 0, True); c.const_string(7, "enableNetwork")
        c.const_string(8, "int"); c.const_string(9, "boolean")
        c.move(10, 3, True); c.sget_obj(11, "bool.T")
        c.invoke_range(STATIC_RANGE, "w2", 6, 6); c.move_result(4, True)
        c.const_string(5, "reconnect"); c.invoke(STATIC, "os", [0, 5])
        okfail(c, 4, 5)
        m.append(("select", c))

    def remove():
        # w=0 a=1 box=2 r=3 t=4
        c = Code(5, 2)
        c.aget_at(2, 1, 1)
        c.invoke(STATIC, "int.parse", [2]); c.move_result(2)
        c.invoke(STATIC, "int.value", [2]); c.move_result(2, True)
        c.const_string(3, "removeNetwork"); c.const_string(4, "int")
        c.invoke(STATIC, "w1", [0, 3, 4, 2]); c.move_result(3, True)
        okfail(c, 3, 4)
        m.append(("remove", c))

    def reassociate():
        # w=0 t=1
        c = Code(2, 1)
        c.const_string(1, "reconnect"); c.invoke(STATIC, "os", [0, 1])
        c.const_string(1, "OK"); c.invoke(STATIC, "p", [1])
        c.ret_void()
        m.append(("reassociate", c))

    def name():
        # a=0 s=1
        c = Code(2, 1)
        c.aget_at(1, 0, 1)                             # hex in, text out (for the shell's say lines)
        c.invoke(STATIC, "hex2s", [1]); c.move_result(1, True)
        c.invoke(STATIC, "p", [1])
        c.ret_void()
        m.append(("name", c))

    def volmax():
        # s=0 k=1 kk=2 n=3 t=4 max=5 i=6 ps=7 x=8 as=9
        c = Code(10, 0)
        c.const_string(0, "android.os.ServiceManager"); c.const_string(1, "getService")
        c.const_string(2, "java.lang.String"); c.const_string(3, "audio")
        c.invoke(STATIC, "sinv", [0, 1, 2, 3]); c.move_result(3, True)
        c.const_string(0, "android.media.IAudioService$Stub"); c.const_string(1, "asInterface")
        c.const_string(2, "android.os.IBinder")
        c.invoke(STATIC, "sinv", [0, 1, 2, 3]); c.move_result(0, True)
        for k in (0, 1, 3):                                  # STREAM_VOICE_CALL, STREAM_SYSTEM, STREAM_MUSIC
            c.const_4(1, k); c.invoke(STATIC, "int.value", [1]); c.move_result(2, True)
            c.const_string(3, "getStreamMaxVolume"); c.const_string(4, "int")
            c.invoke(STATIC, "w1", [0, 3, 4, 2]); c.move_result(5, True)
            c.const_4(6, 4); c.new_array(7, 6, STRARR); c.new_array(9, 6, OBJARR)
            for i, t in enumerate(("int", "int", "int", "java.lang.String")):
                c.const_4(6, i); c.const_string(8, t); c.aput(8, 7, 6, APUT_OBJ)
            c.const_4(6, 0); c.aput(2, 9, 6, APUT_OBJ)
            c.const_4(6, 1); c.aput(5, 9, 6, APUT_OBJ)
            c.const_4(8, 0); c.invoke(STATIC, "int.value", [8]); c.move_result(8, True)
            c.const_4(6, 2); c.aput(8, 9, 6, APUT_OBJ)
            c.const_string(8, "root"); c.const_4(6, 3); c.aput(8, 9, 6, APUT_OBJ)
            c.const_string(3, "setStreamVolume")
            c.invoke(STATIC, "inv", [0, 3, 7, 9])
            c.const_string(3, "="); c.invoke(STATIC, "cat", [2, 3]); c.move_result(3, True)
            c.invoke(STATIC, "cat", [3, 5]); c.move_result(3, True)
            c.invoke(STATIC, "p", [3])
        c.const_string(1, "OK"); c.invoke(STATIC, "p", [1])
        c.ret_void()
        m.append(("volmax", c))

    def wifion():
        # w=0, w2's arguments 1..6 (as startscan)
        c = Code(7, 1)
        c.move(1, 0, True)
        c.const_string(2, "setWifiEnabled")
        c.const_string(3, "java.lang.String")
        c.const_string(4, "boolean")
        c.const_4(5, 0); c.sget_obj(6, "bool.T")
        c.invoke_range(STATIC_RANGE, "w2", 1, 6); c.move_result(1, True)
        okfail(c, 1, 2)
        m.append(("wifion", c))

    for f in (main, wifi, cls, inv, sinv, w1, w2, iz, os, fget, fset, cat, p, ps, ip, unhex, hex2s,
              status, list, keys, scanres, startscan, add, enable, select, remove, reassociate, wifion, name, volmax):
        f()
    p(err=True)
    return m


# ---------------------------------------------------------------- against the firmware's own framework
# What the reflection calls ask the framework for, by name (class, method or field, parameter or field types,
# static): checked against the boot class path of the firmware, so a signature typed wrong shows up on the PC.
REFLECTED = [
    ("Landroid/os/ServiceManager;", "getService", ["Ljava/lang/String;"], True),
    ("Landroid/net/wifi/IWifiManager$Stub;", "asInterface", ["Landroid/os/IBinder;"], True),
    ("Landroid/net/wifi/IWifiManager;", "getConnectionInfo", [], False),
    ("Landroid/net/wifi/IWifiManager;", "getConfiguredNetworks", [], False),
    ("Landroid/net/wifi/IWifiManager;", "getPrivilegedConfiguredNetworks", [], False),
    ("Landroid/net/wifi/IWifiManager;", "getScanResults", ["Ljava/lang/String;"], False),
    ("Landroid/net/wifi/IWifiManager;", "startScan", ["Landroid/net/wifi/ScanSettings;", "Landroid/os/WorkSource;"], False),
    ("Landroid/net/wifi/IWifiManager;", "addOrUpdateNetwork", ["Landroid/net/wifi/WifiConfiguration;"], False),
    ("Landroid/net/wifi/IWifiManager;", "enableNetwork", ["I", "Z"], False),
    ("Landroid/net/wifi/IWifiManager;", "removeNetwork", ["I"], False),
    ("Landroid/net/wifi/IWifiManager;", "reconnect", [], False),
    ("Landroid/net/wifi/IWifiManager;", "setWifiEnabled", ["Ljava/lang/String;", "Z"], False),
    ("Landroid/media/IAudioService$Stub;", "asInterface", ["Landroid/os/IBinder;"], True),
    ("Landroid/media/IAudioService;", "getStreamMaxVolume", ["I"], False),
    ("Landroid/media/IAudioService;", "setStreamVolume", ["I", "I", "I", "Ljava/lang/String;"], False),
    ("Landroid/net/wifi/WifiInfo;", "getSupplicantState", [], False),
    ("Landroid/net/wifi/WifiInfo;", "getNetworkId", [], False),
    ("Landroid/net/wifi/WifiInfo;", "getSSID", [], False),
    ("Landroid/net/wifi/WifiInfo;", "getIpAddress", [], False),
    ("Landroid/net/wifi/WifiInfo;", "getFrequency", [], False),
    ("Landroid/net/wifi/WifiConfiguration;", "<init>", [], False),     # Class.newInstance
]
REFLECTED_FIELDS = [
    ("Landroid/net/wifi/WifiConfiguration;", "hiddenSSID", "Z"),
    ("Landroid/net/wifi/WifiConfiguration;", "SSID", "Ljava/lang/String;"),
    ("Landroid/net/wifi/WifiConfiguration;", "preSharedKey", "Ljava/lang/String;"),
    ("Landroid/net/wifi/WifiConfiguration;", "allowedKeyManagement", "Ljava/util/BitSet;"),
    ("Landroid/net/wifi/WifiConfiguration;", "status", "I"),
    ("Landroid/net/wifi/WifiConfiguration;", "networkId", "I"),
    ("Landroid/net/wifi/ScanResult;", "BSSID", "Ljava/lang/String;"),
    ("Landroid/net/wifi/ScanResult;", "SSID", "Ljava/lang/String;"),
    ("Landroid/net/wifi/ScanResult;", "frequency", "I"),
    ("Landroid/net/wifi/ScanResult;", "level", "I"),
    ("Landroid/net/wifi/ScanResult;", "capabilities", "Ljava/lang/String;"),
]
CONSTANTS = [("Landroid/net/wifi/WifiConfiguration$KeyMgmt;", "WPA_PSK", KEY_WPA_PSK)]

PROG = Program(ME, SELF, reflected=REFLECTED, reflected_fields=REFLECTED_FIELDS, constants=CONSTANTS)


def main():
    dexasm.cli(PROG, emit, __file__, "wifictl.dex")


if __name__ == "__main__":
    main()
