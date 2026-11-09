#!/usr/bin/env python3
"""Kiosk Satellite: lockdown.sh's rules and their loading, main.sh's kiosk block, the install (scripts/lib/kiosk.sh,
apkcert.py, setup's step, kiosk-install.sh), on the PC with stand-ins for Android's tools, adb and iptables.

Both are root's scripts on the Echo, with paths under /data: the text is cut out and those paths moved to a temporary
folder.  Rule text as "iptables -S" prints it, since `wrong()` compares it word for word."""
import hashlib, json, os, shutil, stat, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PKG = "me.jxl.kiosk_satellite"
fails = 0


def check(what, ok, extra=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else f"\n     {extra}"))
    fails += 0 if ok else 1


def tool(bin_, name, body):
    p = os.path.join(bin_, name)
    with open(p, "w") as f:
        f.write("#!/bin/sh\n" + body + "\n")
    os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR)


def moved(text, tmp):
    return (text.replace("/data/local/hassmic", tmp + "/hassmic").replace("/data/local/tmp", tmp + "/ltmp")
            .replace("/data/system", tmp + "/system").replace("/data/app", tmp + "/app").replace("/data/data", tmp + "/appdata"))


def lockdown(tmp, conf, config, pkglist):
    """the rules for this state: every line of hassmic_out and of keep"""
    os.makedirs(tmp + "/hassmic/state", exist_ok=True); os.makedirs(tmp + "/system", exist_ok=True)
    with open(tmp + "/hassmic/state/config", "w") as f: f.write(config)
    with open(tmp + "/system/packages.list", "w") as f: f.write(pkglist)
    with open(tmp + "/device.conf", "w") as f: f.write(conf)
    t = open(ROOT + "/scripts/device/lockdown.sh").read()
    t = t[:t.index('OTA_ONLY=; [ "$1" = ota-only ]')]
    t = moved(t, tmp).replace('CONF="${0%/*}/device.conf"', f'CONF="{tmp}/device.conf"')
    t += "\nkiosk_state; DNS=; rules iptables; keep iptables\n"
    with open(tmp + "/l.sh", "w") as f: f.write(t)
    return subprocess.run(["sh", tmp + "/l.sh"], capture_output=True, text=True).stdout


UID_RULE = "-A hassmic_out -m owner --uid-owner 10042 -j RETURN"
LIST = f"com.other 10001 0 /data/x default 3003\n{PKG} 10042 0 /data/y default 3003\n"
CONF = f"KIOSK_PACKAGE={PKG}\n"

with tempfile.TemporaryDirectory() as tmp:
    o = lockdown(tmp, CONF, "", LIST)
    check("no saved setting means internet on", UID_RULE in o, o)
    rules = o.splitlines()
    check("before the group rule and the DROP", rules.index(UID_RULE) < rules.index("-A hassmic_out -m owner --gid-owner 3990 -j RETURN") < rules.index("-A hassmic_out -j DROP"), o)
    check("ports from private addresses", "-A INPUT -s 192.168.0.0/16 -i wlan0 -p tcp -m tcp --dport 2324 -j ACCEPT" in o
          and "-A INPUT -s 10.0.0.0/8 -i wlan0 -p tcp -m tcp --dport 6053 -j ACCEPT" in o, o)
    check("not from multicast", "224.0.0.0/4 -i wlan0 -p tcp" not in o, o)
    check("off takes the rule out", "--uid-owner 10042" not in lockdown(tmp, CONF, "kiosk_internet=off\n", LIST))
    o = lockdown(tmp, CONF, "", "com.other 10001 0 /x default 3003\n")
    check("not installed: no uid rule and no ports", "--uid-owner 10042" not in o and "2324" not in o and "6053" not in o, o)
    o = lockdown(tmp, CONF, "kiosk_internet=off\n", LIST)
    check("internet off: its ports stay", "--dport 2324" in o and "--uid-owner" not in o.replace("--uid-owner 0", ""), o)
    o = lockdown(tmp, CONF, "", f"me_jxl.kiosk_satellite 10190 0 /x default 3003\n{PKG}x 10191 0 /x default 3003\n{LIST}")
    check("a name that only looks like it: not taken", UID_RULE in o and "10190" not in o and "10191" not in o, o)
    o = lockdown(tmp, CONF, "", "me_jxl.kiosk_satellite 10190 0 /x default 3003\n")
    check("and it alone admits nothing", "2324" not in o and "10190" not in o, o)
    o = lockdown(tmp, "", "", LIST)
    check("a model without the package: nothing", "--uid-owner 10042" not in o and "2324" not in o, o)

# ------------------------------------------------------------------ the same through load, check and apply

FAKE_IPT = r'''#!/usr/bin/env python3
import json, os, sys
st = os.environ["IPT_DIR"] + "/" + os.path.basename(sys.argv[0]).replace("-restore", "") + ".json"
s = json.load(open(st)) if os.path.exists(st) else {"pol": "ACCEPT", "rules": [], "chains": []}
def cmd(a):
    if a[:1] == ["-N"]: s["chains"].append(a[1])
    elif a[:1] == ["-F"]: s["rules"] = [r for r in s["rules"] if not r.startswith("-A " + a[1] + " ")]
    elif a[:1] == ["-P"]: s["pol"] = a[2]
    elif a[:1] == ["-A"]: s["rules"].append(" ".join(a))
    elif a[:1] == ["-I"]:
        r = "-A " + a[1] + " " + " ".join(a[3:]); i = next((n for n, x in enumerate(s["rules"]) if x.startswith("-A " + a[1] + " ")), 0)
        s["rules"].insert(i, r)
    elif a[:1] == ["-D"]:
        r = "-A " + " ".join(a[1:])
        if r not in s["rules"]: sys.exit(1)
        s["rules"].remove(r)
    else: sys.exit(2)
a = [x for x in sys.argv[1:] if x not in ("-w", "--noflush")]
if sys.argv[0].endswith("-restore"):
    for l in sys.stdin.read().split("\n"):
        if l.startswith(":hassmic_out"): s["chains"] = ["hassmic_out"]; cmd(["-F", "hassmic_out"])
        elif l.startswith(":INPUT"): s["pol"] = l.split()[1]
        elif l[:2] in ("-A", "-D", "-I"): cmd(l.split())
elif a[:1] == ["-S"]:
    if a[1:] == ["hassmic_out"]: print("\n".join(["-N hassmic_out"] + [r for r in s["rules"] if r.startswith("-A hassmic_out ")]))
    elif a[1:] == ["OUTPUT"]: print("\n".join(["-P OUTPUT ACCEPT"] + [r for r in s["rules"] if r.startswith("-A OUTPUT ")]))
    else: print("\n".join(["-P INPUT " + s["pol"]] + s["rules"]))
    sys.exit(0)
else: cmd(a)
json.dump(s, open(st, "w"))
'''


def firewall(tmp, steps):
    """one shell over lockdown.sh's functions with stand-in iptables: returns its output"""
    b = tmp + "/bin"; os.makedirs(b, exist_ok=True)
    for n in ("iptables", "iptables-restore", "ip6tables", "ip6tables-restore"):
        with open(f"{b}/{n}", "w") as f: f.write(FAKE_IPT)
        os.chmod(f"{b}/{n}", 0o755)
    t = open(ROOT + "/scripts/device/lockdown.sh").read()
    t = t[:t.index('OTA_ONLY=; [ "$1" = ota-only ]')]
    t = moved(t, tmp).replace('CONF="${0%/*}/device.conf"', f'CONF="{tmp}/device.conf"')
    with open(tmp + "/f.sh", "w") as f: f.write(t + "\nHAVE6=1; DNS=; ADB=1\n" + steps + "\n")
    return subprocess.run(["sh", tmp + "/f.sh"], capture_output=True, text=True,
                          env={**os.environ, "PATH": b + ":" + os.environ["PATH"], "IPT_DIR": tmp}).stdout


with tempfile.TemporaryDirectory() as tmp:
    os.makedirs(tmp + "/hassmic/state"); os.makedirs(tmp + "/system")
    with open(tmp + "/device.conf", "w") as f: f.write(CONF)
    with open(tmp + "/hassmic/state/config", "w") as f: f.write("")
    def state(listed, config=""):
        with open(tmp + "/system/packages.list", "w") as f: f.write(LIST if listed else "")
        with open(tmp + "/hassmic/state/config", "w") as f: f.write(config)
    def rules(): return json.load(open(tmp + "/iptables.json"))["rules"]
    def ports(): return [r for r in rules() if "--dport 2324" in r or "--dport 6053" in r]
    for name, load in (("load_once", 'kiosk_state; load_once iptables; load_once ip6tables'), ("load_each", 'kiosk_state; load_each iptables; load_each ip6tables')):
        for j in ("iptables.json", "ip6tables.json"):
            if os.path.exists(f"{tmp}/{j}"): os.unlink(f"{tmp}/{j}")
        state(True)
        o = firewall(tmp, f"{load}; wrong && echo RIGHT")
        check(f"{name}: installed, ports and uid in, and the check agrees", "RIGHT" in o and len(ports()) == 8 and UID_RULE in rules(), o + str(rules()))
        check(f"{name}: no port rule in IPv6", "--dport 2324" not in open(tmp + "/ip6tables.json").read())
        state(False)
        o = firewall(tmp, "wrong || echo NOTICED")
        check(f"{name}: uninstall noticed", "NOTICED" in o, o)
        o = firewall(tmp, f"{load}; wrong && echo RIGHT")
        check(f"{name}: uninstalled, ports and uid out, and the check agrees", "RIGHT" in o and not ports() and UID_RULE not in rules(), o + str(ports()))
        state(True, "kiosk_internet=off\n")
        o = firewall(tmp, f"{load}; wrong && echo RIGHT")
        check(f"{name}: internet off, ports in, uid out", "RIGHT" in o and len(ports()) == 8 and UID_RULE not in rules(), o)
        state(True)
        o = firewall(tmp, "wrong || echo NOTICED")
        check(f"{name}: internet on again noticed", "NOTICED" in o, o)
        state(True, "kiosk_internet=off\n")
        firewall(tmp, load)
        state(False, "kiosk_internet=off\n")
        o = firewall(tmp, "wrong || echo NOTICED")
        check(f"{name}: uninstalled with internet off: its ports alone noticed", "NOTICED" in o and "ports admitted" in o, o)

# ------------------------------------------------------------------ main.sh's block


class Echo:
    def __init__(self, tmp):
        self.tmp, self.log = tmp, tmp + "/calls"
        self.bin = tmp + "/bin"
        os.makedirs(self.bin); os.makedirs(tmp + "/hassmic/state"); os.makedirs(tmp + "/app")
        self.top("org.hassmic.home/android.app.Activity")
        for t in ("pm", "appops", "dpm", "content"):
            tool(self.bin, t, f'echo "{t} $*" >> {self.log}')
        tool(self.bin, "monkey", f'echo "monkey $*" >> {self.log}; [ ! -f {tmp}/no-monkey ]')
        tool(self.bin, "dumpsys", f'''case "$1" in
deviceidle) echo "dumpsys $*" >> {self.log};;
window) [ "$2" = policy ] && cat {tmp}/rotation;;
*) cat {tmp}/top;; esac''')
        tool(self.bin, "settings", f'echo "settings $*" >> {self.log}')
        self.rotation(1)
        tool(self.bin, "getprop", "echo 1")
        tool(self.bin, "am", f'echo "am $*" >> {self.log}')
        tool(self.bin, "stat", 'echo " u0_a154"')                      # the app's folder's owner (stat -c %U)
        tool(self.bin, "sleep", "exit 0")
        tool(tmp, "runas", 'shift 2; exec "$@"')
        t = open(ROOT + "/scripts/system/main.sh").read()
        block = t[t.index("# >>> kiosk"):t.index("# <<< kiosk")]
        self.block = f"D={tmp}\nKIOSK_PACKAGE={PKG}\nKIOSK_ROTATION=1\nBACKLIGHT=1\n" + moved(block, tmp)

    def top(self, what):
        with open(self.tmp + "/top", "w") as f: f.write(f"    mResumedActivity: ActivityRecord{{1 u0 {what} t1}}\n")

    def rotation(self, n):
        with open(self.tmp + "/rotation", "w") as f: f.write(f"      mCurrentRotation={n}\n" if n != "" else "")

    def install(self, n):
        for d in os.listdir(self.tmp + "/app"): os.rmdir(f"{self.tmp}/app/{d}")
        if n: os.mkdir(f"{self.tmp}/app/{PKG}-{n}")

    def config(self, text):
        with open(self.tmp + "/hassmic/state/config", "w") as f: f.write(text)

    def ticks(self, script):
        """script: shell lines with `t` = one tick; one shell throughout, as the loop is.  Returns (output, calls)"""
        if os.path.exists(self.log): os.unlink(self.log)
        with open(self.tmp + "/k.sh", "w") as f: f.write(self.block + "\nt() { kiosk_tick; }\n" + script + "\nwait\n")
        r = subprocess.run(["sh", self.tmp + "/k.sh"], capture_output=True, text=True,
                           env={**os.environ, "PATH": self.bin + ":" + os.environ["PATH"]})
        calls = open(self.log).read() if os.path.exists(self.log) else ""
        return r.stdout + r.stderr, calls


with tempfile.TemporaryDirectory() as tmp:
    e = Echo(tmp)
    o, c = e.ticks("t")
    check("no app: nothing touched", c == "" and not os.path.exists(tmp + "/hassmic/kiosk-app"), c + o)

    e.install(1)
    o, c = e.ticks("t")
    check("installed: the page is told", os.path.exists(tmp + "/hassmic/kiosk-app"))
    check("granted without a boot", f"appops set {PKG} SYSTEM_ALERT_WINDOW allow" in c, c)
    check("mic and camera stay off", f"pm revoke {PKG} android.permission.RECORD_AUDIO" in c
          and f"appops set {PKG} CAMERA deny" in c and "pm grant " + PKG + " android.permission.CAMERA" not in c, c)
    check("opened over the home screen", "monkey -p " + PKG in c and "opened" in o, c + o)
    check("update helper asked", f"content://{PKG}.update-helper/start" in c, c)

    e.config("kiosk_mic=on\nkiosk_camera=on\n")
    tool(e.bin, "pm", f'echo "pm $*" >> {e.log}; [ "$1" != path ] || [ -f {tmp}/registered ]')
    e.install(3)
    e.top("org.hassmic.home/android.app.Activity"); e.rotation(0)
    o, c = e.ticks("t")
    check("folder before the package manager knows it: no grants", "pm grant" not in c, c)
    check("and the rest goes on", "settings put" in c and "monkey" in c, c)
    o, c = e.ticks("t; " * 6)
    check("pm once a minute while it says no", c.count("pm path") == 1, c)
    o, c = e.ticks("t; " * 7)
    check("and then again", c.count("pm path") == 2, c)
    e.rotation(1)
    open(tmp + "/registered", "w").close()
    e.install(4)
    o, c = e.ticks("t")
    check("settings on: granted", f"pm grant {PKG} android.permission.RECORD_AUDIO" in c
          and f"pm grant {PKG} android.permission.CAMERA" in c, c)
    e.config("")

    o, c = e.ticks("t; " * 3)
    check("no second open inside the half minute", c.count("monkey") == 1 and o.count("opened") == 1, c + o)
    open(tmp + "/no-monkey", "w").close()
    o, c = e.ticks("t; " * 10)
    check("it cannot be opened: said once, and not as opened", o.count("could not be opened") == 1 and "satellite: opened" not in o and c.count("monkey") == 3, c + o)
    os.unlink(tmp + "/no-monkey")
    o, c = e.ticks("t; " * 9)
    check("half a minute, then a minute: not sooner", c.count("monkey") == 2, c)
    o, c = e.ticks("t; " * 10)
    check("half a minute, then a minute: not later", c.count("monkey") == 3, c)
    o, c = e.ticks("t; " * 4)
    check("half a minute after an open: to the tick", c.count("monkey") == 2, c)
    check("an app that does not stay: logged once", o.count("opened") == 1, o)

    e.top(PKG + "/.MainActivity")
    o, c = e.ticks("t; t")
    check("the app on screen: left alone", "monkey" not in c, c)
    home = "org.hassmic.home/android.app.Activity"
    show = lambda what: f'echo "    mResumedActivity: ActivityRecord{{1 u0 {what} t1}}" > {tmp}/top'
    o, c = e.ticks("; ".join([show(home), "t", show(PKG + "/.Main"), "t; t; t; t", show(home), "t"]))
    check("it ends after a good open: opened at the next look", c.count("monkey") == 2, c)
    e.top(PKG + "/.MainActivity")
    o, c = e.ticks("t; t")
    check("no new grants while the install is the same", c.count("SYSTEM_ALERT_WINDOW") == 1, c)

    # its own adaptive brightness off (issue #126): as its user, the app stopped first (it reads them at its start)
    prefs = f"{tmp}/appdata/{PKG}/shared_prefs/FlutterSharedPreferences.xml"; os.makedirs(os.path.dirname(prefs), exist_ok=True)
    OFF = '    <boolean name="flutter.ks.screen.adaptive_brightness" value="false" />'
    def put_prefs(body):
        if os.path.exists(prefs): os.chmod(prefs, 0o644)
        with open(prefs, "w") as f: f.write("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n" + body + "</map>\n")
    put_prefs('    <string name="flutter.ks.x">a</string>\n    <boolean name="flutter.ks.screen.adaptive_brightness" value="true" />\n    <long name="flutter.ks.y" value="-1" />\n')
    o, c = e.ticks("t")
    p = open(prefs).read()
    check("the app's adaptive brightness on: stopped, switched off, the rest kept", f"am force-stop {PKG}" in c and OFF in p and "value=\"true\"" not in p
          and 'name="flutter.ks.x">a<' in p and 'name="flutter.ks.y" value="-1"' in p and p.endswith("</map>\n") and "switched off" in o, p + c + o)
    o, c = e.ticks("t; t")
    check("off already: the app left alone", "am " not in c and "adaptive" not in o, c + o)
    put_prefs('    <string name="flutter.ks.x">a</string>\n')
    o, c = e.ticks("t")
    check("no such key yet (a new install): added", f"am force-stop {PKG}" in c and OFF in open(prefs).read() and open(prefs).read().endswith("</map>\n"), open(prefs).read() + c)
    with open(prefs, "w") as f: f.write("<map>\n    <boolean name=\"flutter.ks.screen.adaptive_brightness\" value=\"true\" />\n")
    o, c = e.ticks("t")
    check("a file being written (no </map>): not now", "am " not in c, c)
    put_prefs('    <boolean name="flutter.ks.screen.adaptive_brightness" value="true" />\n'); os.chmod(prefs, 0o444)
    o, c = e.ticks("t; t; t")
    check("cannot be written: the app stopped once, said once", c.count("am force-stop") == 1 and o.count("could not be switched off") == 1, c + o)
    os.chmod(prefs, 0o644)
    put_prefs('    <boolean name="flutter.ks.screen.adaptive_brightness" value="true" />\n')
    with open(prefs + ".bak", "w") as f: f.write("<map>\n    <boolean name=\"flutter.ks.screen.adaptive_brightness\" value=\"true\" />\n</map>\n")
    e.ticks("kiosk_bfail=; t")
    check("a .bak beside it (Android would take it) goes with the write", not os.path.exists(prefs + ".bak") and OFF in open(prefs).read(), os.listdir(os.path.dirname(prefs)))
    put_prefs('    <boolean name="flutter.ks.screen.adaptive_brightness" value="true" />\n')
    tool(tmp, "runas", 'echo "runas: unknown group $2" >&2; exit 1')
    o, c = e.ticks("kiosk_bfail=; t; t")
    check("runas failing: not taken for work to do, the app not stopped, said once", "am " not in c and o.count("could not be looked at") == 1, c + o)
    tool(tmp, "runas", 'shift 2; exec "$@"')
    tool(e.bin, "stat", 'echo " u0_a1*"')
    o, c = e.ticks("t")
    check("an owner that is no plain app user's name: left alone", "am " not in c, c)
    tool(e.bin, "stat", 'echo " u0_a154"')
    put_prefs(OFF + "\n")
    with open(prefs + ".bak", "w") as f: f.write("<map>\n</map>\n")
    o, c = e.ticks("kiosk_bfail=; t")
    check("off, but a .bak beside it: written again, the .bak gone", f"am force-stop {PKG}" in c and not os.path.exists(prefs + ".bak"), c)
    o, c = e.ticks(f"BACKLIGHT=; t")
    check("a model without the backlight settings: left alone", "am " not in c, c)
    put_prefs(OFF + "\n")

    o, c = e.ticks(f"t; t; rmdir {tmp}/app/{PKG}-1; mkdir {tmp}/app/{PKG}-2; t")
    check("a reinstall is granted again without a boot", c.count("SYSTEM_ALERT_WINDOW") == 2, c)
    e.install(2)

    o, c = e.ticks(f"t; " + f'echo "    mResumedActivity: ActivityRecord{{1 u0 org.hassmic.home/android.app.Activity t1}}" > {tmp}/top; ' + "t")
    check("the app ended: opened again", "monkey" in c, c)

    o, c = e.ticks("t; " * 32)
    check("helper asked again after 5 min", c.count("update-helper") == 2, c)

    # the screen UI runs (state/screen-ui.<pid>: hassmic's pid in the name): it opens the app, as its home app; grants
    # and rotation go on
    e.top(home)
    other = subprocess.Popen(["sleep", "30"])                      # (a pid of another process: a hassmic that died, its pid taken)
    open(tmp + f"/hassmic/state/screen-ui.{other.pid}", "w").close()
    o, c = e.ticks("t")
    check("a marker whose pid is no hassmic's: opened here", "monkey" in c, c)
    os.unlink(tmp + f"/hassmic/state/screen-ui.{other.pid}"); other.kill(); other.wait()
    e.ticks("t; t; t; t; t; t")                                     # (past the ticks it skips after opening)
    os.makedirs(tmp + "/bin-hm", exist_ok=True)
    if not os.path.exists(tmp + "/bin-hm/hassmic"): os.symlink(shutil.which("sleep"), tmp + "/bin-hm/hassmic")
    sleeper = subprocess.Popen([tmp + "/bin-hm/hassmic", "30"])    # (named as hassmic is: its comm)
    open(tmp + f"/hassmic/state/screen-ui.{sleeper.pid}", "w").close()
    e.rotation(0)
    o, c = e.ticks("t; t; t")
    check("the screen UI runs: the app is its to open", "monkey" not in c and "settings put system user_rotation 1" in c, c + o)
    open(tmp + f"/hassmic/state/screen-ui.{sleeper.pid}.tmp0", "w").close()
    o, c = e.ticks("t")
    check("a temp file beside it (statefile.c's, listed last): its name is no pid, the pid there still", "monkey" not in c, c)
    os.unlink(tmp + f"/hassmic/state/screen-ui.{sleeper.pid}.tmp0")
    sleeper.kill(); sleeper.wait()
    o, c = e.ticks("t")
    check("its pid gone (a crash, an old file): opened here again", "monkey" in c, c)
    os.unlink(tmp + f"/hassmic/state/screen-ui.{sleeper.pid}"); e.rotation(1)

    o, c = e.ticks("t")
    check("landscape: settings left alone", "settings" not in c, c)
    e.rotation(0)
    o, c = e.ticks("t")
    check("portrait: landscape put back", "settings put system user_rotation 1" in c
          and "settings put system accelerometer_rotation 0" in c, c)

    e.rotation("")
    o, c = e.ticks("t; t")
    check("no rotation in dumpsys: said once", o.count("names no rotation") == 1 and "settings" not in c, c + o)
    e.rotation(1)

    real = subprocess.run(["sh", "-c", "command -v sleep"], capture_output=True, text=True).stdout.strip()
    tool(e.bin, "content", f'echo $$ > {tmp}/hung; exec {real} 60')
    gone = f"{real} 0.5; [ -r /proc/$old/stat ] && ! grep -q ') Z ' /proc/$old/stat && echo CHAIN-LIVES"
    o, c = e.ticks(f"kiosk_ask_helper; {real} 0.5; old=$(cat {tmp}/hung); kiosk_ask_helper; {gone}; exit 0")
    check("left over from the last round: said, and gone with its children", "no answer, asked again" in o and "CHAIN-LIVES" not in o, o)
    o, c = e.ticks(f"old=$(cat {tmp}/hung); kiosk_ask_helper; {gone}; kiosk_stale")
    check("and after a restart of the loop too", "no answer" in o and "CHAIN-LIVES" not in o, o)
    other = subprocess.Popen(["setsid", real, "30"])
    with open(tmp + "/hassmic/kiosk-app.asking", "w") as f: f.write(str(other.pid))
    o, c = e.ticks("kiosk_stale")
    check("a pid used by something else again: left alone", other.poll() is None and "no answer" not in o, o)
    for junk in ("", "abc", "99999999"):
        with open(tmp + "/hassmic/kiosk-app.asking", "w") as f: f.write(junk)
        o, c = e.ticks("kiosk_stale")
        check(f"pid file {junk!r}: nothing happens", "no answer" not in o and "rror" not in o, o)
    other.kill(); other.wait()
    os.unlink(tmp + "/hassmic/kiosk-app.asking")
    tool(e.bin, "content", 'echo started')
    tool(e.bin, "sleep", f'''n=$(cat {tmp}/n 2>/dev/null || echo 0); echo $((n+1)) > {tmp}/n; [ $n -lt 3 ] || exit 1
[ $n = 1 ] && echo kiosk_mic=on > {tmp}/hassmic/state/config; exit 0''')
    e.config(""); os.path.exists(tmp + "/n") and os.unlink(tmp + "/n")
    o, c = e.ticks("kiosk_perm_sync")
    check("the setting reaches the app within a round", f"pm grant {PKG} android.permission.RECORD_AUDIO" in c and "kiosk_mic=on" in o, c + o)
    tool(e.bin, "sleep", "exit 0")
    for said, want in (("Kiosk Satellite update helper started. Run this", ": started"), ("Kiosk Satellite update helper is already running.", ""), ("Could not start", "Could not start")):
        tool(e.bin, "content", f'echo "echo {said}"')
        os.path.exists(tmp + "/hassmic/kiosk-app.said") and os.unlink(tmp + "/hassmic/kiosk-app.said")
        o, c = e.ticks(f"kiosk_ask_helper; until [ -f {tmp}/hassmic/kiosk-app.said ]; do :; done; kiosk_helper_said")
        check(f"helper says {said!r}", (want in o) if want else "update helper" not in o, o)
    e.config("")
    tool(e.bin, "sleep", f'''n=$(cat {tmp}/n 2>/dev/null || echo 0); echo $((n+1)) > {tmp}/n; [ $n -lt 3 ] || exit 1
[ $n = 1 ] && echo kiosk_mic=on > {tmp}/hassmic/state/config; exit 0''')
    os.unlink(tmp + "/n"); os.unlink(tmp + "/hassmic/kiosk-app")
    o, c = e.ticks("kiosk_perm_sync")
    check("no app: a change of setting touches nothing", "pm grant" not in c, c + o)
    open(tmp + "/hassmic/kiosk-app", "w").close(); tool(e.bin, "sleep", "exit 0")

    helper = subprocess.Popen(["bash", "-c", f"exec -a ks-update-helper-10999 {real} 30"])
    os.makedirs(tmp + "/ltmp/ks-update-helper-10999"); open(tmp + "/ltmp/ks-update-helper-10999.log", "w").close()
    open(tmp + "/ltmp/other", "w").close()
    o, c = e.ticks("t")
    check("installed: its update helper left alone", helper.poll() is None and os.path.isdir(tmp + "/ltmp/ks-update-helper-10999"), o)
    e.install(0)
    o, c = e.ticks("t")
    check("uninstalled: the page is told", not os.path.exists(tmp + "/hassmic/kiosk-app"))
    try: helper.wait(timeout=5)
    except subprocess.TimeoutExpired: helper.kill(); helper.wait()
    check("uninstalled: its update helper ended, said once", helper.returncode == -9 and o.count("uninstalled") == 1, o)
    check("uninstalled: its folder and log gone, nothing else", os.listdir(tmp + "/ltmp") == ["other"], str(os.listdir(tmp + "/ltmp")))
    o, c = e.ticks("t; t")
    check("and nothing more after", "uninstalled" not in o and c == "", c + o)

# ------------------------------------------------------------------ scripts/lib/kiosk.sh: the download


def apk(*certs, block=0x7109871a, extra=b"", comment=b"", locator=False, v3=None, pair_tail=b"", signers_tail=b"", signer_tail=b"",
        magic=b"APK Sig Block 42", size_add=0, twice=False, cd_off=0, cd_size=0):
    """a zip with an APK Signing Block: one signer per certificate given, a tuple for a signer with several, () for none;
    the rest bends it for the checks of apkcert.py"""
    lp = lambda b: struct.pack("<I", len(b)) + b
    chain = lambda c: b"".join(lp(x) for x in (c if isinstance(c, tuple) else (c,)))
    def signer(c, mid=b""):
        return lp(lp(lp(b"") + lp(chain(c))) + mid + lp(b"") + lp(b"") + signer_tail)
    pair = struct.pack("<I", block) + lp(b"".join(signer(c) for c in certs)) + signers_tail
    pairs = struct.pack("<Q", len(pair)) + pair
    if twice: pairs += pairs
    if v3 is not None:
        p = struct.pack("<I", 0xf05368c0) + lp(signer(v3, b"\0" * 8))
        pairs += struct.pack("<Q", len(p)) + p
    pairs += pair_tail
    size = struct.pack("<Q", len(pairs) + 24)
    sig = struct.pack("<Q", len(pairs) + 24 + size_add) + pairs + size + magic
    head = b"x" * 40 + extra
    zip64 = b"PK\x06\x07" + b"\0" * 16 if locator else b""
    return head + sig + zip64 + b"PK\x05\x06" + b"\0" * 8 + struct.pack("<II", cd_size or len(zip64), len(head) + len(sig) + cd_off) + struct.pack("<H", len(comment)) + comment


GOOD = bytes.fromhex("3082")


def sh(cmd, tmp, **env):
    return subprocess.run(["bash", "-c", cmd], cwd=ROOT, capture_output=True, text=True,
                          env={**os.environ, "PATH": tmp + "/bin:" + os.environ["PATH"], **env})


def fetch(release, data=None, said="Success", env=None):
    data = apk(GOOD) if data is None else data
    with tempfile.TemporaryDirectory() as tmp:
        b = tmp + "/bin"; os.makedirs(b); os.makedirs(tmp + "/out")
        with open(tmp + "/rel", "w", encoding="utf-8") as f: json.dump(release, f, ensure_ascii=False)
        with open(tmp + "/apk", "wb") as f: f.write(data)
        tool(b, "adb", f'''echo "adb $*" >> {tmp}/calls
case "$*" in install*) echo "{said}";; esac''')
        tool(b, "curl", f'''case "$*" in
*example.test*) cat {tmp}/rel;;
*) while [ "$1" != -o ]; do shift; done; cp {tmp}/apk "$2";; esac''')
        r = subprocess.run(["bash", "-c", f". scripts/lib/kiosk.sh; KIOSK_CERT={hashlib.sha256(GOOD).hexdigest()}; OUT={tmp}/out; kiosk_get"], cwd=ROOT, capture_output=True, text=True,
                           env={**os.environ, "PATH": b + ":" + os.environ["PATH"], "KIOSK_RELEASES": "https://example.test/latest", "PYTHONIOENCODING": "cp1252", **(env or {})})
        calls = open(tmp + "/calls").read() if os.path.exists(tmp + "/calls") else ""
        return r.returncode, calls, r.stderr


SUM = hashlib.sha256(apk(GOOD)).hexdigest()
URL = "https://github.com/jxlarrea/kiosk-satellite/releases/download/1/"
def asset(name="kiosk-satellite-1.armeabi-v7a.apk", digest="sha256:" + SUM, url=URL + "a.apk"):
    return {"name": name, "digest": digest, "browser_download_url": url}


c, calls, e = fetch({"assets": [asset("kiosk-satellite-1.arm64-v8a.apk"), asset()]})
check("newest armeabi-v7a installed", c == 0 and "install -r" in calls and "armeabi-v7a" in calls and "arm64" not in calls, calls + e)
c, calls, e = fetch({"assets": [asset(digest="sha256:" + "0" * 64)]})
check("wrong checksum: not installed", c != 0 and "install" not in calls and "wrong checksum" in e, calls + e)
c, calls, e = fetch({"assets": [asset(digest=None)]})
check("no digest: not installed", c != 0 and not calls, calls + e)
c, calls, e = fetch({"assets": [asset("kiosk-satellite-../../x.armeabi-v7a.apk")]})
check("a name with a path: refused", c != 0 and not calls, calls + e)
c, calls, e = fetch({"assets": [asset(url="https://evil.test/a.apk")]})
check("another host: refused", c != 0 and not calls, calls + e)
c, calls, e = fetch({"assets": []})
check("no asset: not installed", c != 0 and not calls, calls + e)
c, calls, e = fetch({"assets": [asset(url=URL + "a b.apk")]})
check("an address with a space: refused", c != 0 and not calls, calls + e)
c, calls, e = fetch({"assets": [asset()]}, said="Failure [INSTALL_FAILED]")
check("adb says Failure, exit 0: not installed", c != 0 and "uninstall" not in calls, calls + e)
def signed(data):
    return fetch({"assets": [asset(digest="sha256:" + hashlib.sha256(data).hexdigest())]}, data)


c, calls, e = signed(apk(b"\x30\x83"))
check("another signer: never reaches the Echo", c != 0 and not calls and "signed by" in e, calls + e)
c, calls, e = signed(apk(GOOD, b"\x30\x83"))
check("a second signer: never reaches the Echo", c != 0 and not calls, calls + e)
c, calls, e = signed(b"PK\x05\x06" + b"\0" * 18)
check("no signing block: never reaches the Echo", c != 0 and not calls, calls + e)
c, calls, e = signed(apk(GOOD, block=0x1234))
check("no v2 block (Android would go by the JAR signature): refused", c != 0 and not calls, calls + e)
c, calls, e = signed(apk(b"\x30\x83", comment=apk(GOOD)[40:]))
check("a good block in the zip comment, another for Android: refused", c != 0 and not calls, calls + e)
# Android's record: the real one, for the other signer.  In its comment a good block and a record pointing at it, with a
# comment length that lets only a reader going by the last PK\5\6 take it
outer = apk(b"\x30\x83")
fake = apk(GOOD)[40:-22]
record = b"PK\x05\x06" + b"\0" * 8 + struct.pack("<II", 0, len(outer) + len(fake)) + struct.pack("<H", 1)
c, calls, e = signed(outer[:-2] + struct.pack("<H", len(fake) + len(record)) + fake + record)
check("two end records: refused", c != 0 and not calls, calls + e)
for what, bent in (("bytes after the pairs", dict(pair_tail=b"\0\0\0")), ("bytes after the signers", dict(signers_tail=b"\0\0\0")),
                   ("bytes after a signer", dict(signer_tail=b"\0\0\0")), ("no block magic", dict(magic=b"x" * 16)),
                   ("sizes that differ", dict(size_add=8)), ("a block twice", dict(twice=True)),
                   ("the directory elsewhere", dict(cd_off=4)), ("a directory that does not fit", dict(cd_size=4)), ("v3 by another signer", dict(v3=b"\x30\x83"))):
    c, calls, e = signed(apk(GOOD, **bent))
    check(f"{what}: refused", c != 0 and not calls, calls + e)
c, calls, e = signed(apk())
check("no signer: refused", c != 0 and not calls, calls + e)
c, calls, e = signed(apk(()))
check("a signer without a certificate: refused", c != 0 and not calls, calls + e)
c, calls, e = signed(apk(GOOD, v3=GOOD))
check("v2 and v3 by the pinned signer", c == 0 and "install -r" in calls, calls + e)
c, calls, e = signed(apk(GOOD, comment=b"x"))
check("a zip comment: refused", c != 0 and not calls, calls + e)
c, calls, e = signed(apk(GOOD, locator=True))
check("zip64: refused", c != 0 and not calls, calls + e)
c, calls, e = signed(apk((GOOD, b"\x30\x83")))
check("another certificate in the signer's chain: refused", c != 0 and not calls, calls + e)
c, calls, e = fetch({"body": "\u03c1", "assets": [asset()]})
check("release notes the locale cannot read", c == 0, calls + e)

# keyboard, setup's step and the script
with tempfile.TemporaryDirectory() as tmp:
    b = tmp + "/bin"; os.makedirs(b)
    tool(b, "curl", 'while [ "$1" != -o ]; do shift; done; echo kb > "$2"')
    tool(b, "sleep", "exit 0")
    tool(b, "adb", f'''echo "adb $*" >> {tmp}/calls
case "$*" in install*) echo Success;;
"get-state") echo device;;
*getprop*) echo checkers;;
*"ime list"*) n=$(cat {tmp}/n 2>/dev/null || echo 0); echo $((n+1)) > {tmp}/n; [ $n -lt 2 ] || echo "rkr.simplekeyboard.inputmethod/.latin.LatinIME";;
esac''')
    sumk = hashlib.sha256(b"kb\n").hexdigest()
    r = sh(f". scripts/lib/kiosk.sh; KEYBOARD_SHA={sumk}; OUT={tmp}; keyboard_get", tmp)
    calls = open(tmp + "/calls").read()
    check("keyboard waits until Android lists it", r.returncode == 0 and calls.count("ime list") == 3 and calls.index("ime list") < calls.index("ime enable"), calls + r.stderr)
    r = sh(f". scripts/lib/kiosk.sh; KEYBOARD_SHA={'0' * 64}; OUT={tmp}; rm -f {tmp}/simple*; keyboard_get", tmp)
    check("keyboard: wrong checksum refused", r.returncode != 0 and "wrong checksum" in r.stderr, r.stderr)

    stub = f"""set +e; . scripts/lib/setup.sh; OUT={tmp}; ask() {{ echo "ask: $1"; [ "$1" = "$WANT" ]; }}; task() {{ echo "task: $1"; shift; "$@"; }}
wait_adb() {{ :; }}; keyboard_get() {{ echo KB; }}; kiosk_get() {{ echo KS; return ${{KS_FAIL:-0}}; }}; install_screen_apps; echo end"""
    r = sh(stub, tmp, WANT="Install Kiosk Satellite?")
    env_ = open(tmp + "/setup.env").read() if os.path.exists(tmp + "/setup.env") else ""
    check("setup, yes: both, keyboard not asked, final screen told", "KB" in r.stdout and "KS" in r.stdout and "Simple Keyboard?" not in r.stdout and "SAT_KIOSK=1" in env_, r.stdout + r.stderr)
    os.path.exists(tmp + "/setup.env") and os.unlink(tmp + "/setup.env")
    r = sh(stub, tmp, WANT="Install Kiosk Satellite?", KS_FAIL="1")
    check("setup, install fails: warned, final screen not told", "kiosk-install.sh" in r.stdout + r.stderr and not os.path.exists(tmp + "/setup.env"), r.stdout + r.stderr)
    r = sh(stub, tmp, WANT="Install Simple Keyboard?")
    check("setup, no: keyboard alone offered", "KS" not in r.stdout.replace("KS ", "") and "KB" in r.stdout, r.stdout)

    open(tmp + "/setup.env", "w").write("SAT_KIOSK=1\nSAT_IP=10.0.0.9\n")
    r = sh(f"set +e; . scripts/lib/setup.sh; OUT={tmp}; MODEL_NAME=Show; done_screen", tmp)
    check("final screen: the app's page", "http://10.0.0.9:2324" in r.stdout, r.stdout + r.stderr)
    os.unlink(tmp + "/setup.env")
    r = sh(f"set +e; . scripts/lib/setup.sh; OUT={tmp}; MODEL_NAME=Show; done_screen", tmp)
    check("final screen: nothing without the app", ":2324" not in r.stdout, r.stdout)

# kiosk-install.sh, on a copy with the test's certificate pinned: its OUT is under the repository
with tempfile.TemporaryDirectory() as tmp:
    os.makedirs(tmp + "/repo"); b = tmp + "/bin"; os.makedirs(b)
    shutil.copytree(ROOT + "/scripts", tmp + "/repo/scripts"); shutil.copytree(ROOT + "/devices", tmp + "/repo/devices")
    shutil.copytree(ROOT + "/keys", tmp + "/repo/keys")
    p = tmp + "/repo/scripts/lib/kiosk.sh"; t = open(p).read()
    open(p, "w").write(t.replace("KIOSK_CERT=2a77", "KIOSK_CERT=" + hashlib.sha256(GOOD).hexdigest() + " #"))
    data = apk(GOOD)
    with open(tmp + "/apk", "wb") as f: f.write(data)
    with open(tmp + "/rel", "w") as f: json.dump({"assets": [asset(digest="sha256:" + hashlib.sha256(data).hexdigest())]}, f)
    tool(b, "curl", f'''case "$*" in
*example.test*) cat {tmp}/rel;;
*) while [ "$1" != -o ]; do shift; done; cp {tmp}/apk "$2";; esac''')
    def echo_adb(product, state="device"):
        tool(b, "adb", f'''case "$*" in
get-state) echo {state};;
*getprop*) echo {product};;
install*) echo Success; echo "$*" >> {tmp}/installs;;
*ifconfig*) echo "inet addr:10.0.0.9 ";;
*"ime list"*) echo rkr.simplekeyboard.inputmethod/.latin.LatinIME;;
esac''')
    def script():
        return subprocess.run(["sh", tmp + "/repo/scripts/kiosk-install.sh"], capture_output=True, text=True,
                              env={**os.environ, "PATH": b + ":" + os.environ["PATH"], "KIOSK_RELEASES": "https://example.test/latest", "DEVICE": ""})
    echo_adb("checkers")
    r = script()
    check("script: the app installed, the address said", "kiosk-satellite-1" in open(tmp + "/installs").read() and "http://10.0.0.9:2324" in r.stdout, r.stdout + r.stderr)
    donut = subprocess.run(["sh", "-c", f". {ROOT}/devices/donut/device.conf; echo $PRODUCT"], capture_output=True, text=True).stdout.strip()
    echo_adb(donut); os.unlink(tmp + "/installs")
    r = script()
    check("script: a model without a screen refused", r.returncode != 0 and "no screen" in r.stderr and not os.path.exists(tmp + "/installs"), r.stdout + r.stderr)
    echo_adb("x", "offline")
    r = script()
    check("script: no Echo said", r.returncode != 0 and "no Echo on adb" in r.stderr, r.stderr)

# kiosk_remove: the device admin dropped by disabling its receiver, then the uninstall.  dumpsys as Android 7 prints it,
# through an adb that ends lines with \r\n; the admin goes `gone` looks after the disable (None: never)
ADMIN2 = PKG + "/me.jxl.kiosk_satellite.OtherAdmin"
def remove(gone=2, installed=True, uninstall="Success", disable="new state: disabled", admins=(PKG + "/.KioskAdminReceiver",), script=False):
    with tempfile.TemporaryDirectory() as tmp:
        b = tmp + "/bin"; os.makedirs(b)
        tool(b, "sleep", "exit 0")
        listed = ": ; " + "".join(f"printf '    {a}:\\r\\n      uid=10183\\r\\n      policies:\\r\\n        force-lock\\r\\n'; " for a in admins)
        tool(b, "adb", f'''echo "adb $*" >> {tmp}/calls
case "$*" in
get-state) echo device;;
*getprop*) echo checkers;;
"shell pm path {PKG}") {"echo package:/data/app/" + PKG + "-1/base.apk" if installed else ":"};;
"shell dumpsys device_policy")
    printf 'Current Device Policy Manager state:\\r\\n\\r\\n  Enabled Device Admins (User 0, provisioningState: 0):\\r\\n'
    n=$(cat {tmp}/n 2>/dev/null || echo 0)
    if [ -f {tmp}/off ]; then echo $((n+1)) > {tmp}/n; fi
    if [ ! -f {tmp}/off ] || {"true" if gone is None else f"[ $n -lt {gone} ]"}; then {listed}fi
    printf '  Device Owner: none\\r\\n';;
"shell pm disable"*) echo "{disable}"; [ "{disable}" = "new state: disabled" ] && : > {tmp}/off;;
"shell pm enable"*) rm -f {tmp}/off;;
"shell pm uninstall {PKG}") n=$(cat {tmp}/n 2>/dev/null || echo 0)
    if {"true" if not admins else f"[ -f {tmp}/off ] && [ $n -gt {gone if gone is not None else 10**6} ]"}; then echo "{uninstall}"; else echo "Failure [DELETE_FAILED_DEVICE_POLICY_MANAGER]"; exit 1; fi;;
esac''')
        env = {**os.environ, "PATH": b + ":" + os.environ["PATH"]}
        if script:
            r = subprocess.run(["sh", "scripts/kiosk-install.sh", "--uninstall"], cwd=ROOT, capture_output=True, text=True, env={**env, "DEVICE": ""})
        else:
            r = subprocess.run(["bash", "-c", "set -e; . scripts/lib/kiosk.sh; kiosk_remove"], cwd=ROOT, capture_output=True, text=True, env={**env, "KIOSK_PACKAGE": PKG})
        calls = open(tmp + "/calls").read() if os.path.exists(tmp + "/calls") else ""
        return r.returncode, calls, r.stdout + r.stderr

c, calls, o = remove()
check("uninstall: the admin's receiver disabled, then the app uninstalled", c == 0 and f"pm disable {PKG}/.KioskAdminReceiver\n" in calls
      and calls.index("pm disable") < calls.index("pm uninstall"), calls + o)
check("uninstall: not before Android dropped the admin", calls.count("dumpsys device_policy") == 4, calls)
c, calls, o = remove(admins=(PKG + "/.KioskAdminReceiver", ADMIN2))
check("uninstall: every admin of the app, by dumpsys", c == 0 and calls.count("pm disable") == 2 and f"pm disable {ADMIN2}\n" in calls, calls + o)
c, calls, o = remove(gone=None)
check("uninstall: the admin stays: given back, not uninstalled, said", c != 0 and f"pm enable {PKG}/.KioskAdminReceiver" in calls
      and "pm uninstall" not in calls and "by hand" in o, calls + o)
c, calls, o = remove(disable="Error: java.lang.SecurityException")
check("uninstall: Android does not disable it: stopped", c != 0 and "pm uninstall" not in calls and "did not disable" in o, calls + o)
c, calls, o = remove(admins=(PKG + "/.KioskAdminReceiver", ADMIN2), disable="no")
check("uninstall: the first disable fails: nothing left disabled", c != 0 and calls.count("pm disable") == 1, calls + o)
c, calls, o = remove(installed=False)
check("uninstall: not installed: nothing touched", c == 0 and "pm disable" not in calls and "pm uninstall" not in calls and "not installed" in o, calls + o)
c, calls, o = remove(uninstall="Failure [DELETE_FAILED_INTERNAL_ERROR]")
check("uninstall: Android's failure said, exit 1", c != 0 and "DELETE_FAILED_INTERNAL_ERROR" in o, o)
c, calls, o = remove(admins=())
check("uninstall: no admin: uninstalled at once", c == 0 and "pm disable" not in calls and "pm uninstall" in calls, calls + o)
c, calls, o = remove(script=True)
check("script --uninstall: uninstalled, no download", c == 0 and "pm uninstall" in calls and "install -r" not in calls and "Done." in o, calls + o)

# board.kiosk and device.conf say the same
for m in sorted(os.listdir(ROOT + "/devices")):
    conf, board = f"{ROOT}/devices/{m}/device.conf", f"{ROOT}/devices/{m}/board.c"
    if os.path.exists(conf) and os.path.exists(board):
        a = "\nKIOSK_PACKAGE=" in open(conf).read(); b = ".kiosk = 1" in open(board).read()
        check(f"{m}: board.kiosk and KIOSK_PACKAGE agree", a == b, f"{a} {b}")

sys.exit(1 if fails else 0)
