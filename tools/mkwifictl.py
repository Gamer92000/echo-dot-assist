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
    scan_results                    bssid / frequency / signal level / flags / ssid
    scan                            OK when the framework took it (wpa_cli scan)
    add <ssid hex> <psk hex|->      addOrUpdateNetwork, prints the network id
    enable <id> <only|keep>         enableNetwork(id, disableOthers)
    select <id>                     enableNetwork(id, true) and reconnect()
    remove <id>                     removeNetwork(id)
    reassociate                     reconnect()
    wifi-on                         setWifiEnabled(null, true)
    name <hex>                      the decoded text, for the shell's messages

The Java source it stands for:

    public class Wifictl {
        public static void main(String[] a) {
            if (a.length < 1) { System.err.println("wifictl: no verb"); System.exit(2); }
            Object w = wifi();
            if (w == null) { System.err.println("wifictl: no wifi service"); System.exit(2); }
            String v = a[0];
            if (v.equals("status")) status(w);
            else if (v.equals("list_networks")) list(w);
            else if (v.equals("scan_results")) scanres(w);
            else if (v.equals("scan")) startscan(w);
            else if (v.equals("add")) add(w, a);
            else if (v.equals("enable")) enable(w, a);
            else if (v.equals("select")) select(w, a);
            else if (v.equals("remove")) remove(w, a);
            else if (v.equals("reassociate")) reassociate(w);
            else if (v.equals("wifi-on")) wifion(w);
            else { System.err.println("wifictl: unknown " + v); System.exit(2); }
        }
        static Object wifi() {
            Object b = sinv("android.os.ServiceManager", "getService", "java.lang.String", "wifi");
            return b == null ? null
                : sinv("android.net.wifi.IWifiManager$Stub", "asInterface", "android.os.IBinder", b);
        }
        static Object inv(Object o, String n, String[] ps, Object[] as) {   // an instance method, by name
            Class[] cs = new Class[ps.length];
            for (int i = 0; i < ps.length; i++) cs[i] = Class.forName(ps[i]);
            return o.getClass().getMethod(n, cs).invoke(o, as);
        }
        static Object sinv(String c, String n, String p, Object a) {        // a static one, one argument
            Class k = Class.forName(c);
            Class[] cs = { Class.forName(p) };
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
                int st = ((Integer) fget(c, "status")).intValue();          // 1 on it, 2 disabled
                Object fl = st == 1 ? "[CURRENT]" : st == 2 ? "[DISABLED]" : "";
                p(cat(cat(cat(cat(fget(c, "networkId"), "\\t"), fget(c, "SSID")), "\\tany\\t"), fl));
            }
        }
        static void scanres(Object w) {
            p("bssid / frequency / signal level / flags / ssid");
            for (Object r : (java.util.List) w1(w, "getScanResults", "java.lang.String", null)) {
                Object s = fget(r, "BSSID");
                s = cat(s, "\\t"); s = cat(s, fget(r, "frequency")); s = cat(s, "\\t");
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
                fset(c, "preSharedKey", cat(cat("\\"", hex2s(a[2])), "\\""));
                ((java.util.BitSet) fget(c, "allowedKeyManagement")).set(0);  // WPA_PSK
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
    }

python3 tools/mkwifictl.py            rebuild scripts/device/wifictl.dex
python3 tools/mkwifictl.py --check    only re-read and check the one in the tree

What the writer guarantees (ART's DexFileVerifier on the Echo is strict): id tables sorted as the format wants
(strings by content, the rest by their indices), one class LWifictl; with 27 static methods and no fields, no
debug info, no tries.  read() parses the result back, checks checksum and signature, walks the map, decodes
every instruction and follows every branch target, before the file is written out."""
import hashlib
import struct
import sys
import zlib

# ------------------------------------------------------------------ Dalvik assembler
class Code:
    """One static method's instructions: 16-bit units, labels patched at the end.  Only what Wifictl needs:
    no *-wide, no try/catch, no registers above 15 where a format asks for nibbles.  assemble() leaves the
    instruction list alone, so it can run twice: once to collect the ids, once to resolve them."""
    def __init__(self, nregs, ins):
        self.nregs, self.ins = nregs, ins
        self.units, self.labels, self.branches, self.outs = [], {}, [], 0

    def label(self, name):
        self.labels[name] = len(self.units)

    def _branch(self, units, name):                         # the offset goes into the last unit
        self.units += units
        self.branches.append((len(self.units) - 1, name))

    def const_string(self, v, s):                           # 21c
        self.units += [(v << 8) | 0x1a, ("str", s)]

    def const_4(self, v, n):                                # 11n, signed 4-bit literal
        assert -8 <= n <= 7 and v < 16
        self.units += [((n & 0xf) << 12) | (v << 8) | 0x12]

    def const_16(self, v, n):                               # 21s
        self.units += [(v << 8) | 0x13, n & 0xffff]

    def move(self, a, b, obj=False):                        # 12x
        assert a < 16 and b < 16
        self.units += [((b & 0xf) << 12) | (a << 8) | (0x07 if obj else 0x01)]

    def move_result(self, v, obj=False):                    # 11x
        self.units += [(v << 8) | (0x0c if obj else 0x0a)]

    def check_cast(self, v, t):                             # 21c
        self.units += [(v << 8) | 0x1f, ("type", t)]

    def new_instance(self, v, t):                           # 21c
        self.units += [(v << 8) | 0x22, ("type", t)]

    def new_array(self, a, b, t):                           # 22c: a = new t[b]
        assert a < 16 and b < 16
        self.units += [((b & 0xf) << 12) | (a << 8) | 0x23, ("type", t)]

    def array_length(self, a, b):                           # 12x
        assert a < 16 and b < 16
        self.units += [((b & 0xf) << 12) | (a << 8) | 0x21]

    def aget(self, a, b, c, op):                            # 23x: a = b[c]
        self.units += [(a << 8) | op, (c << 8) | b]

    def aput(self, a, b, c, op):                            # 23x: b[c] = a
        self.units += [(a << 8) | op, (c << 8) | b]

    def goto_(self, name):                                  # 20t: goto/16
        self._branch([0x0029, 0], name)

    def if2(self, op, a, b, name):                          # 22t
        assert a < 16 and b < 16
        self._branch([((b & 0xf) << 12) | ((a & 0xf) << 8) | op, 0], name)

    def ifz(self, op, a, name):                             # 21t
        self._branch([(a << 8) | op, 0], name)

    def invoke(self, op, m, regs):                          # 35c: 0-4 argument registers
        assert 0 <= len(regs) <= 4
        self.outs = max(self.outs, len(regs))
        c, d, e, f = (list(regs) + [0] * 4)[:4]
        self.units += [(len(regs) << 12) | op, ("method", m), c | (d << 4) | (e << 8) | (f << 12)]

    def invoke_range(self, op, m, first, n):                # 3rc: n consecutive registers from first
        self.outs = max(self.outs, n)
        self.units += [(n << 8) | op, ("method", m), first]

    def sget(self, v, f):                                   # 21c
        self.units += [(v << 8) | 0x60, ("field", f)]

    def binop(self, op, a, b, c):                           # 23x: a = b <op> c
        self.units += [(a << 8) | op, (c << 8) | b]

    def binop_lit8(self, op, a, b, n):                      # 22b: a = b <op> n
        assert -128 <= n <= 127
        self.units += [(a << 8) | op, (b << 8) | (n & 0xff)]

    def int_to_byte(self, a, b):                            # 12x
        assert a < 16 and b < 16
        self.units += [((b & 0xf) << 12) | (a << 8) | 0x8e]

    def ret_void(self):
        self.units += [0x0e]

    def ret(self, v, obj):                                  # 11x
        self.units += [(v << 8) | (0x11 if obj else 0x0f)]

    def assemble(self, tables):
        units = list(self.units)
        for at, name in self.branches:
            off = self.labels[name] - (at - 1)              # relative to the branch instruction itself
            assert -32768 <= off <= 32767, f"branch to {name} out of range"
            units[at] = off & 0xffff
        return struct.pack("<HHHHII", self.nregs, self.ins, self.outs, 0, 0, len(units)) + \
            b"".join(struct.pack("<H", u if isinstance(u, int) else tables.index(u)) for u in units)


AGET_OBJ, APUT_OBJ, APUT_BYTE = 0x44, 0x4d, 0x4f
VIRT, DIRECT, STATIC, IFACE, STATIC_RANGE = 0x6e, 0x70, 0x71, 0x72, 0x77
ADD, AND, OR, SHL = 0x90, 0x95, 0x96, 0x98
ADD8, OR8, SHL8, USHR8 = 0xe0, 0xe6, 0xe8, 0xea
IFNE, IFGE, IFLT, IFEQZ, IFNEZ = 0x33, 0x35, 0x34, 0x38, 0x39

J = "Ljava/lang/"
SB, STR = J + "StringBuilder;", J + "String;"
CLS, OBJ, MTH, FLD = J + "Class;", J + "Object;", J + "reflect/Method;", J + "reflect/Field;"
ITR, LST, BST = J + "Iterator;", J + "List;", J + "BitSet;"
INTG, BOOL, SYS = J + "Integer;", J + "Boolean;", J + "System;"
PSTRM = J + "io/PrintStream;"
CLSARR, STRARR, OBJARR = "[Ljava/lang/Class;", "[Ljava/lang/String;", "[Ljava/lang/Object;"
BYTES = "[B"
ME = "LWifictl;"

# every java.* method the class calls directly: key -> (class, method, params, return)
METHODS = {
    "SB.new":     (SB, "<init>", ((), "V")),
    "SB.appendO": (SB, "append", ((OBJ,), SB)),
    "SB.appendI": (SB, "append", (("I",), SB)),
    "SB.to":      (SB, "toString", ((), STR)),
    "println":    (PSTRM, "println", ((STR,), "V")),
    "str.eq":     (STR, "equals", ((OBJ,), "Z")),
    "str.new_b":  (STR, "<init>", ((BYTES,), "V")),
    "str.charat": (STR, "charAt", (("I",), "C")),
    "str.length": (STR, "length", ((), "I")),
    "str.valueO": (STR, "valueOf", ((OBJ,), STR)),
    "int.parse":  (INTG, "parseInt", ((STR,), "I")),
    "int.value":  (INTG, "valueOf", (("I",), INTG)),
    "int.intval": (INTG, "intValue", ((), "I")),
    "bool.value": (BOOL, "valueOf", (("Z",), BOOL)),
    "cls.forName": (CLS, "forName", ((STR,), CLS)),
    "cls.newi":   (CLS, "newInstance", ((), OBJ)),
    "cls.getm":   (CLS, "getMethod", ((STR, CLSARR), MTH)),
    "cls.getf":   (CLS, "getField", ((STR,), FLD)),
    "obj.getcls": (OBJ, "getClass", ((), CLS)),
    "obj.tostr":  (OBJ, "toString", ((), STR)),
    "obj.eq":     (OBJ, "equals", ((OBJ,), "Z")),
    "m.invoke":   (MTH, "invoke", ((OBJ, OBJARR), OBJ)),
    "f.get":      (FLD, "get", ((OBJ,), OBJ)),
    "f.set":      (FLD, "set", ((OBJ, OBJ), "V")),
    "it.has":     (ITR, "hasNext", ((), "Z")),
    "it.next":    (ITR, "next", ((), OBJ)),
    "list.iter":  (LST, "iterator", ((), ITR)),
    "bits.set":   (BST, "set", (("I",), "V")),
    "sys.exit":   (SYS, "exit", (("I",), "V")),
}
FIELDS = {
    "sys.out": (SYS, "out", PSTRM),
    "sys.err": (SYS, "err", PSTRM),
    "bool.T":  (BOOL, "TRUE", BOOL),
}
# the class's own methods: (name, params, return)
SELF = [
    ("main",  (STRARR,), "V"),
    ("wifi",  (), OBJ),
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
    ("scanres", (OBJ,), "V"),
    ("startscan", (OBJ,), "V"),
    ("add",   (OBJ, STRARR), "V"),
    ("enable", (OBJ, STRARR), "V"),
    ("select", (OBJ, STRARR), "V"),
    ("remove", (OBJ, STRARR), "V"),
    ("reassociate", (OBJ,), "V"),
    ("wifion", (OBJ,), "V"),
    ("name", (STRARR,), "V"),
]
SELF_BY_NAME = {n: (ps, r) for n, ps, r in SELF}


def shorty(p):
    def one(t):
        return t[0] if t[0] in "VIZCB" else "L"
    return one(p[1]) + "".join(one(t) for t in p[0])


class Collector:
    """first pass: note every string/type/proto/field/method mentioned, order doesn't matter"""
    def __init__(self):
        self.s, self.t, self.p, self.f, self.m = [], [], [], [], []

    def index(self, u):
        kind, v = u
        if kind == "str":
            if v not in self.s: self.s.append(v)
            return 0
        if kind == "type":
            self.index(("str", v))
            if v not in self.t: self.t.append(v)
            return 0
        if kind == "field":
            for x in FIELDS[v][0::2]:
                self.index(("type", x))
            self.index(("str", FIELDS[v][1]))
            if FIELDS[v] not in self.f: self.f.append(FIELDS[v])
            return 0
        if kind in ("method", "self"):                        # a java.* method, or one of ours
            if v in METHODS:
                c, n, (ps, r) = METHODS[v]
                self._method((c, n, tuple(ps), r))
            else:
                ps, r = SELF_BY_NAME[v]
                self._method((ME, v, ps, r))
            return 0
        raise ValueError(kind)

    def _method(self, m):
        self.index(("type", m[0]))
        self.index(("str", m[1]))
        self._proto((tuple(m[2]), m[3]))
        if m not in self.m: self.m.append(m)

    def _proto(self, p):
        for q in p[0]:
            self.index(("type", q))
        self.index(("type", p[1]))
        if p not in self.p: self.p.append(p)


class Tables:
    """second pass: sorted, with indices"""
    def __init__(self, coll):
        self.s = sorted(set(coll.s) | {shorty(p) for p in coll.p})
        s2i = {s: i for i, s in enumerate(self.s)}
        self.t = sorted(set(coll.t), key=s2i.get)
        t2i = {t: i for i, t in enumerate(self.t)}
        self.p = sorted(set(coll.p), key=lambda p: (t2i[p[1]], [t2i[x] for x in p[0]]))
        p2i = {p: i for i, p in enumerate(self.p)}
        self.f = sorted(set(coll.f), key=lambda f: (t2i[f[0]], s2i[f[1]], t2i[f[2]]))
        self.m = sorted(set(coll.m), key=lambda m: (t2i[m[0]], s2i[m[1]], p2i[(tuple(m[2]), m[3])]))
        self.s2i, self.t2i, self.p2i = s2i, t2i, p2i
        self.f2i = {f: i for i, f in enumerate(self.f)}
        self.m2i = {m: i for i, m in enumerate(self.m)}
        assert len(self.s) < 0x10000 and len(self.m) < 0x10000
        for st in self.s:                                     # every string data item fits one uleb byte
            assert len(st) < 128, st

    def index(self, u):
        kind, v = u
        if kind == "str":
            return self.s2i[v]
        if kind == "type":
            return self.t2i[v]
        if kind == "field":
            return self.f2i[FIELDS[v]]
        if kind in ("method", "self"):
            if v in METHODS:
                c, n, (ps, r) = METHODS[v]
                return self.m2i[(c, n, tuple(ps), r)]
            return self.m2i[(ME, v, SELF_BY_NAME[v][0], SELF_BY_NAME[v][1])]
        raise ValueError(kind)


def uleb(n):
    b = bytearray()
    while True:
        x = n & 0x7f
        n >>= 7
        b.append(x | (0x80 if n else 0))
        if not n:
            return bytes(b)


# ---------------------------------------------------------------- the methods, as bytecode
def emit():
    m = []

    def main():
        # a=0 t=1 w=2 x=3 r=4
        c = Code(5, 1)
        c.array_length(1, 0)
        c.ifz(IFGE, 1, "args")
        c.const_string(3, "wifictl: no verb")
        c.sget(4, "sys.err"); c.invoke(VIRT, "println", [4, 3])
        c.const_4(3, 2); c.invoke(STATIC, "sys.exit", [3]); c.ret_void()
        c.label("args")
        c.invoke(STATIC, "wifi", []); c.move_result(2, True)
        c.ifz(IFNEZ, 2, "svc")
        c.const_string(3, "wifictl: no wifi service")
        c.sget(4, "sys.err"); c.invoke(VIRT, "println", [4, 3])
        c.const_4(3, 2); c.invoke(STATIC, "sys.exit", [3]); c.ret_void()
        c.label("svc")
        c.aget(3, 0, 0, AGET_OBJ)                             # v = a[0]
        for verb, name in [("status", "status"), ("list_networks", "list"), ("scan_results", "scanres"),
                           ("scan", "startscan"), ("add", "add"), ("enable", "enable"),
                           ("select", "select"), ("remove", "remove"), ("reassociate", "reassociate"),
                           ("wifi-on", "wifion"), ("name", "name")]:
            c.const_string(1, verb)
            c.invoke(VIRT, "str.eq", [3, 1]); c.move_result(1)
            c.ifz(IFEQZ, 1, verb)
            c.invoke(STATIC, name, [2]); c.ret_void()
            c.label(verb)
        c.const_string(1, "wifictl: unknown ")
        c.invoke(STATIC, "cat", [1, 3]); c.move_result(1, True)
        c.sget(4, "sys.err"); c.invoke(VIRT, "println", [4, 1])
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

    def inv():
        # o=0 n=1 ps=2 as=3 cs=4 m=5 i=6 len=7 r=8
        c = Code(9, 4)
        c.array_length(7, 2)
        c.const_4(6, 1)
        c.new_array(4, 6, CLSARR)
        c.const_4(6, 0)
        c.label("loop")
        c.if2(IFGE, 6, 7, "done")
        c.aget(8, 2, 6, AGET_OBJ)
        c.invoke(STATIC, "cls.forName", [8]); c.move_result(8, True)
        c.aput(8, 4, 6, APUT_OBJ)
        c.binop_lit8(ADD8, 6, 6, 1)
        c.goto_("loop")
        c.label("done")
        c.invoke(VIRT, "obj.getcls", [0]); c.move_result(7, True)
        c.invoke(VIRT, "cls.getm", [7, 1, 4]); c.move_result(5, True)
        c.invoke(VIRT, "m.invoke", [5, 0, 3]); c.move_result(7, True)
        c.ret(7, True)
        m.append(("inv", c))

    def sinv():
        # c=0 n=1 p=2 a=3 k=4 cs=5 as=6 m=7 z=8
        c = Code(9, 4)
        c.invoke(STATIC, "cls.forName", [0]); c.move_result(4, True)
        c.invoke(STATIC, "cls.forName", [2]); c.move_result(7, True)
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
        c.sget(1, "sys.err" if err else "sys.out")
        c.invoke(VIRT, "println", [1, 0])
        c.ret_void()
        m.append(("e" if err else "p", c))

    def ps():
        # o=0 out=1 s=2
        c = Code(3, 1)
        c.invoke(STATIC, "str.valueO", [0]); c.move_result(2, True)
        c.sget(1, "sys.out")
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
        c.const_4(6, 1); c.new_array(2, 6, BYTES)
        c.const_4(3, 0)
        c.label("loop")
        c.if2(IFGE, 3, 1, "done")
        # the high digit: d(s.charAt(2i))
        c.binop_lit8(ADD8, 6, 3, 3)
        c.invoke(VIRT, "str.charat", [0, 6]); c.move_result(4)
        c.binop_lit8(ADD8, 5, 4, -48)
        c.const_16(6, 10); c.if2(IFLT, 5, 6, "h")
        c.binop_lit8(OR8, 5, 4, 32); c.binop_lit8(ADD8, 5, 5, -87)
        c.label("h")
        # the low digit: d(s.charAt(2i+1))
        c.binop_lit8(ADD8, 6, 3, 3); c.binop_lit8(ADD8, 6, 6, 1)
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
        c.const_4(6, 1); c.if2(IFNE, 7, 6, "d")
        c.const_string(6, "[CURRENT]"); c.goto_("f")
        c.label("d")
        c.const_4(6, 2); c.if2(IFNE, 7, 6, "e")
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
        # w=0 n=1 p1=2 p2=3 a1=4 a2=5
        c = Code(6, 1)
        c.const_string(1, "startScan")
        c.const_string(2, "android.net.wifi.ScanSettings")
        c.const_string(3, "android.os.WorkSource")
        c.const_4(4, 0); c.const_4(5, 0)
        c.invoke_range(STATIC_RANGE, "w2", 0, 6)
        c.const_string(1, "OK"); c.invoke(STATIC, "p", [1])
        c.ret_void()
        m.append(("startscan", c))

    def add():
        # w=0 a=1 c=2 s=3 t=4 km=5
        c = Code(6, 2)
        c.const_string(2, "android.net.wifi.WifiConfiguration")
        c.invoke(STATIC, "cls.forName", [2]); c.move_result(2, True)
        c.invoke(VIRT, "cls.newi", [2]); c.move_result(2, True)
        c.const_string(3, "hiddenSSID"); c.sget(4, "bool.T")
        c.invoke(STATIC, "fset", [2, 3, 4])
        c.aget(3, 1, 1, AGET_OBJ)
        c.invoke(STATIC, "hex2s", [3]); c.move_result(3, True)
        c.const_string(4, chr(34))
        c.invoke(STATIC, "cat", [4, 3]); c.move_result(3, True)
        c.invoke(STATIC, "cat", [3, 4]); c.move_result(3, True)
        c.const_string(4, "SSID"); c.invoke(STATIC, "fset", [2, 4, 3])
        c.aget(3, 1, 2, AGET_OBJ)
        c.const_string(4, "-"); c.invoke(VIRT, "str.eq", [3, 4]); c.move_result(4)
        c.ifz(IFNEZ, 4, "skip")
        c.invoke(STATIC, "hex2s", [3]); c.move_result(3, True)
        c.const_string(4, chr(34))
        c.invoke(STATIC, "cat", [4, 3]); c.move_result(3, True)
        c.invoke(STATIC, "cat", [3, 4]); c.move_result(3, True)
        c.const_string(4, "preSharedKey"); c.invoke(STATIC, "fset", [2, 4, 3])
        c.const_string(3, "allowedKeyManagement")
        c.invoke(STATIC, "fget", [2, 3]); c.move_result(3, True)
        c.check_cast(3, BST)
        c.const_4(4, 0); c.invoke(VIRT, "bits.set", [3, 4])
        c.label("skip")
        c.const_string(3, "addOrUpdateNetwork")
        c.const_string(4, "android.net.wifi.WifiConfiguration")
        c.invoke(STATIC, "w1", [0, 3, 4, 2]); c.move_result(3, True)
        c.invoke(STATIC, "ps", [3])
        c.ret_void()
        m.append(("add", c))

    def okfail(c, r, t):
        c.sget(t, "bool.T")
        c.invoke(VIRT, "obj.eq", [t, r]); c.move_result(t)
        c.ifz(IFEQZ, t, "fail")
        c.const_string(t, "OK"); c.invoke(STATIC, "p", [t]); c.ret_void()
        c.label("fail")
        c.const_string(t, "FAIL"); c.invoke(STATIC, "p", [t])
        c.ret_void()

    def enable():
        # w=0 a=1 id=2 box=3 r=4 t=5 w2 args 6..11
        c = Code(12, 2)
        c.aget(2, 1, 1, AGET_OBJ)
        c.invoke(STATIC, "int.parse", [2]); c.move_result(2)
        c.invoke(STATIC, "int.value", [2]); c.move_result(3, True)
        c.aget(4, 1, 2, AGET_OBJ)
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
        c.aget(2, 1, 1, AGET_OBJ)
        c.invoke(STATIC, "int.parse", [2]); c.move_result(2)
        c.invoke(STATIC, "int.value", [2]); c.move_result(3, True)
        c.move(6, 0, True); c.const_string(7, "enableNetwork")
        c.const_string(8, "int"); c.const_string(9, "boolean")
        c.move(10, 3, True); c.sget(11, "bool.T")
        c.invoke_range(STATIC_RANGE, "w2", 6, 6); c.move_result(4, True)
        c.const_string(5, "reconnect"); c.invoke(STATIC, "os", [0, 5])
        okfail(c, 4, 5)
        m.append(("select", c))

    def remove():
        # w=0 a=1 box=2 r=3 t=4
        c = Code(5, 2)
        c.aget(2, 1, 1, AGET_OBJ)
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
        c.aget(1, 0, 1, AGET_OBJ)                             # hex in, text out (for the shell's say lines)
        c.invoke(STATIC, "hex2s", [1]); c.move_result(1, True)
        c.invoke(STATIC, "p", [1])
        c.ret_void()
        m.append(("name", c))

    def wifion():
        # w=0 n=1 p1=2 p2=3 a1=4 a2=5
        c = Code(6, 1)
        c.const_string(1, "setWifiEnabled")
        c.const_string(2, "java.lang.String")
        c.const_string(3, "boolean")
        c.const_4(4, 0); c.sget(5, "bool.T")
        c.invoke_range(STATIC_RANGE, "w2", 0, 6); c.move_result(1, True)
        okfail(c, 1, 2)
        m.append(("wifion", c))

    for f in (main, wifi, inv, sinv, w1, w2, iz, os, fget, fset, cat, p, ps, ip, unhex, hex2s,
              status, list, scanres, startscan, add, enable, select, remove, reassociate, wifion, name):
        f()
    p(err=True)
    assert {n for n, _ in m} == {n for n, _, _ in SELF}, \
        [n for n, _, _ in SELF if n not in {x for x, _ in m}] or [n for n, _ in m if n not in {x for x, _, _ in SELF}]
    return m


def build(codes):
    coll = Collector()
    for n, ps, r in SELF:                                     # defined, whether or not referenced
        coll._method((ME, n, ps, r))
    coll.index(("type", J + "Object;"))                       # the superclass
    for _, c in codes:
        c.assemble(coll)                                      # pass 1: collect
    tabs = Tables(coll)
    blobs = [c.assemble(tabs) for _, c in codes]              # pass 2: resolve
    ns, nt, np, nf, nm, ncd = len(tabs.s), len(tabs.t), len(tabs.p), len(tabs.f), len(tabs.m), 1

    # ---- offsets
    o = 112
    so, o = o, o + 4 * ns
    to, o = o, o + 4 * nt
    po, o = o, o + 12 * np
    fo, o = o, o + 8 * nf
    mo, o = o, o + 8 * nm
    cdo, o = o, o + 32 * ncd
    data_off = o
    tl_off = {}
    for p in tabs.p:
        if p[0]:
            tl_off[p] = o
            o = (o + 4 + 2 * len(p[0]) + 3) & ~3            # zero-padded to 4, like libdex wants
    o = (o + 3) & ~3
    code_offs = []
    for b in blobs:
        code_offs.append(o)
        o += (len(b) + 3) & ~3
    sd = {}
    for s in tabs.s:
        sd[s] = o
        o += 2 + len(s)
    o = (o + 3) & ~3
    class_data_off = o

    # ---- class data: every method direct and static, sorted by method id
    enc = sorted((tabs.m2i[(ME, n, SELF_BY_NAME[n][0], SELF_BY_NAME[n][1])],
                  0x0009 if n == "main" else 0x0008, co) for (n, _), co in zip(codes, code_offs))
    cd = uleb(0) + uleb(0) + uleb(len(enc)) + uleb(0)
    prev = 0
    for idx, acc, co in enc:
        cd += uleb(idx - prev) + uleb(acc) + uleb(co)
        prev = idx
    o = class_data_off + len(cd)
    o = (o + 3) & ~3
    map_off = o

    # ---- serialize
    out = bytearray(112)
    for s in tabs.s:
        out += struct.pack("<I", sd[s])
    for t in tabs.t:
        out += struct.pack("<I", tabs.s2i[t])
    for p in tabs.p:
        out += struct.pack("<III", tabs.s2i[shorty(p)], tabs.t2i[p[1]], tl_off.get(p, 0))
    for f in tabs.f:                                         # class_idx, type_idx, name_idx
        out += struct.pack("<HHI", tabs.t2i[f[0]], tabs.t2i[f[2]], tabs.s2i[f[1]])
    for mc, mn, mp, mr in tabs.m:
        out += struct.pack("<HHI", tabs.t2i[mc], tabs.p2i[(tuple(mp), mr)], tabs.s2i[mn])
    out += struct.pack("<IIIIIIII", tabs.t2i[ME], 0x0001, tabs.t2i[J + "Object;"], 0,
                       0xFFFFFFFF, 0, class_data_off, 0)
    for p in tabs.p:
        if p[0]:
            blob = struct.pack("<I", len(p[0])) + b"".join(struct.pack("<H", tabs.t2i[t]) for t in p[0])
            out += blob + b"\0" * (-len(blob) % 4)          # padded to 4, as laid out above
    for b in blobs:
        out += b + b"\0" * (((len(b) + 3) & ~3) - len(b))
    for s in tabs.s:
        out += uleb(len(s)) + s.encode() + b"\0"
    while len(out) % 4:
        out += b"\0"
    out += cd
    while len(out) % 4:
        out += b"\0"
    entries = [(0x0000, 1, 0), (0x0001, ns, so), (0x0002, nt, to), (0x0003, np, po),
               (0x0004, nf, fo), (0x0005, nm, mo), (0x0006, ncd, cdo)]
    if tl_off:
        entries.append((0x1001, len(tl_off), min(tl_off.values())))
    entries.append((0x2001, len(blobs), code_offs[0]))
    entries.append((0x2002, ns, sd[tabs.s[0]]))
    entries.append((0x2000, 1, class_data_off))
    entries.append((0x1000, 1, map_off))
    entries.sort(key=lambda e: e[2])
    out += struct.pack("<I", len(entries)) + b"".join(struct.pack("<HHII", t, 0, n, f) for t, n, f in entries)

    # ---- header
    file_size = len(out)
    struct.pack_into("<20I", out, 32, file_size, 112, 0x12345678, 0, 0, map_off,
                     ns, so, nt, to, np, po, nf, fo, nm, mo, ncd, cdo, file_size - data_off, data_off)
    out[12:32] = hashlib.sha1(bytes(out[32:])).digest()
    struct.pack_into("<I", out, 8, zlib.adler32(bytes(out[12:])) & 0xffffffff)
    out[:8] = b"dex\n035\0"
    return bytes(out)


# ---------------------------------------------------------------- checking the result
SIZES = {}                                                     # opcode -> 16-bit units
for op in (0x01, 0x07, 0x0a, 0x0c, 0x0e, 0x0f, 0x11, 0x12, 0x21, 0x8e):
    SIZES[op] = 1
for op in list(range(0x13, 0x1e)) + [0x1f, 0x22, 0x23, 0x29] + list(range(0x32, 0x3e)) + \
        list(range(0x44, 0x52)) + [0x60] + list(range(0x90, 0xb0)) + list(range(0xd0, 0xd8)) + \
        list(range(0xe0, 0xeb)):
    if op not in SIZES:
        SIZES[op] = 2
for op in range(0x6e, 0x79):
    SIZES[op] = 3


def read(data):
    """parse the dex back and walk it; raises on anything ART would refuse"""
    assert data[:8] == b"dex\n035\0", data[:8]
    (file_size, hs, endian, _, _, map_off, ns, so, nt, to, np, po, nf, fo, nm, mo, ncd, cdo,
     data_size, data_off) = struct.unpack("<20I", data[32:112])
    assert file_size == len(data) and hs == 112 and endian == 0x12345678
    assert zlib.adler32(data[12:]) & 0xffffffff == struct.unpack("<I", data[8:12])[0]
    assert hashlib.sha1(data[32:]).digest() == data[12:32]
    u16 = lambda o: struct.unpack("<H", data[o:o + 2])[0]
    u32 = lambda o: struct.unpack("<I", data[o:o + 4])[0]
    # the map: every entry where it says, offsets ascending
    n = u32(map_off)
    seen = [(u16(map_off + 4 + 12 * i), u32(map_off + 4 + 12 * i + 4), u32(map_off + 4 + 12 * i + 8))
            for i in range(n)]                                 # (type, size, offset)
    assert [e[2] for e in seen] == sorted(e[2] for e in seen)
    where = {t: (sz, f) for t, sz, f in seen}
    assert where[0x0001][0] == ns and where[0x0002][0] == nt and where[0x0003][0] == np
    assert where[0x0005][0] == nm and where[0x0006][0] == ncd and where[0x1000][0] == 1
    # ids sorted, strings readable, every offset inside the file
    strs = []
    for i in range(ns):
        o = u32(so + 4 * i)
        l = data[o]; o += 1
        assert data[o + l] == 0
        strs.append(data[o:o + l].decode())
    assert strs == sorted(strs)
    tids = [u32(to + 4 * i) for i in range(nt)]
    assert tids == sorted(tids)
    # class data: the method list, code offsets
    def uleb_at(o):
        r = s = 0
        while True:
            b = data[o]; o += 1
            r |= (b & 0x7f) << s
            if b < 0x80:
                return r, o
            s += 7
    cd_off = u32(cdo + 24)                                   # the class_def's class_data_off
    sf, iff, dm, vm = 0, 0, 0, 0
    o = cd_off
    sf, o = uleb_at(o); iff, o = uleb_at(o); dm, o = uleb_at(o); vm, o = uleb_at(o)
    assert (sf, iff, vm) == (0, 0, 0) and dm == len(SELF)
    prev = 0
    for _ in range(dm):
        d, o = uleb_at(o); a, o = uleb_at(o); co, o = uleb_at(o)
        assert co >= where[0x2001][1] and co < len(data)
        regs, insz, outsz, tries, dbg, isz = struct.unpack("<HHHHII", data[co:co + 16])
        assert tries == 0 and dbg == 0 and insz <= regs and regs <= 16 and outsz <= 6
        i = co + 16
        end = i + 2 * isz
        while i < end:
            op = u16(i) & 0xff
            assert op in SIZES, hex(op)
            u = SIZES[op]
            if op == 0x29 or 0x32 <= op <= 0x3d:             # a branch: its target must stay inside
                off = struct.unpack("<h", data[i + 2:i + 4])[0]
                assert 0 <= (i - co - 16) // 2 + off < isz
            i += 2 * u
        assert i == end
        prev += d
    return len(data)


def main():
    codes = emit()
    blob = build(codes)
    read(blob)                                                 # nothing leaves before it parses back
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    default = __file__.replace("tools/mkwifictl.py", "scripts/device/wifictl.dex")
    if "--check" in sys.argv:
        have = open(args[0] if args else default, "rb").read()
        assert have == blob, f"{args[0] if args else default} is not what tools/mkwifictl.py builds"
        print(f"ok {args[0] if args else default} ({len(blob)} bytes, as assembled)")
        return
    out = args[0] if args else default
    open(out, "wb").write(blob)
    print(f"{out}: {len(blob)} bytes")


if __name__ == "__main__":
    main()
