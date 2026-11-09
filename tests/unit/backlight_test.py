#!/usr/bin/env python3
"""main.sh's backlight block (issue #126): the Echo Show's brightness settings from state/config into Android's own
settings, on the PC with a stand-in for `settings`; and adaptive colour (issue #162) through a stand-in for `service`.
Root's script with paths under /data: the text is cut out and those
paths moved to a temporary folder, as kiosk_test.py does."""
import os, stat, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PKG = "me.jxl.kiosk_satellite"
fails = 0


def check(what, ok, extra=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else f"\n     {extra}"))
    fails += 0 if ok else 1


SETTINGS = r'''#!/bin/sh
# settings get|put system <key> [value]: one file per key, every call logged; $T/fail-<get|put> makes that call fail
# (a Java call under load), its count in it going down
echo "$*" >> "$T/calls"
f="$T/store/$2_$3"
if [ -s "$T/fail-$1" ]; then
    n=$(cat "$T/fail-$1"); echo $((n - 1)) > "$T/fail-$1"; [ "$n" -gt 0 ] && exit 1
fi
case "$1" in
get) if [ -f "$f" ]; then cat "$f"; else echo null; fi;;
put) printf '%s\n' "$4" > "$f"                     # and SettingsProvider's file, as Android writes it within 200 ms
     x="$T/system/users/0/settings_system.xml"; { grep -v "name=\"$3\"" "$x"; echo "  <setting id=\"1\" name=\"$3\" value=\"$4\" package=\"root\" />"; } > "$x.new"; mv "$x.new" "$x";;
esac
'''

# service call ambientdisplayinfo 2 (get) | 1 i32 <0|1> (set), printed as Android 9's service does; the state in
# $T/ambient, every call logged in $T/svc-calls; $T/svc-missing: no such service (system_server not up)
SERVICE = r'''#!/bin/sh
echo "$*" >> "$T/svc-calls"
[ "$1 $2" = "call ambientdisplayinfo" ] || exit 1
[ -f "$T/svc-missing" ] && { echo "service: Service ambientdisplayinfo does not exist"; exit 0; }
case "$3" in
1) [ -f "$T/svc-refuse" ] && { echo "Result: Parcel(00000000 ffffffff   '........')"; exit 0; }
   [ "$4" = i32 ] && printf '%s\n' "$5" > "$T/ambient"; echo "Result: Parcel(00000000 00000000   '........')";;
2) echo "Result: Parcel(00000000 0000000$(cat "$T/ambient")   '........')";;
esac
'''

text = open(ROOT + "/scripts/system/main.sh").read()
block = text[text.index("# >>> backlight"):text.index("# <<< backlight")]


def run(tmp, config, call, stock=None, xml="", fail=None, ambient="1", missing=False):
    """the block sourced, then `call`; returns (store, calls, alexa-settings, output).  fail: {"get"|"put": n} calls fail;
    ambient: the adaptive colour service's state at the start (svc() reads it and its calls afterwards), missing: none"""
    for d in ("bin", "store", "hassmic/state", "system/users/0", "app"):
        os.makedirs(os.path.join(tmp, d), exist_ok=True)
    for k, n in (fail or {}).items():
        with open(f"{tmp}/fail-{k}", "w") as f: f.write(f"{n}\n")
    for f in ("calls", "hassmic/alexa-settings", "hassmic/adaptive-colour", "hassmic/state/adaptive-colour-request",
              "fail-get", "fail-put", "svc-calls", "svc-missing", "svc-refuse"):
        if f.startswith("fail") and f[5:] in (fail or {}): continue
        if os.path.exists(os.path.join(tmp, f)): os.remove(os.path.join(tmp, f))
    for f in os.listdir(tmp + "/store"): os.remove(os.path.join(tmp, "store", f))
    for k, v in (stock or {}).items():
        with open(f"{tmp}/store/system_{k}", "w") as f: f.write(v + "\n")
    with open(tmp + "/hassmic/state/config", "w") as f: f.write(config)
    with open(tmp + "/system/users/0/settings_system.xml", "w") as f: f.write(xml)
    with open(tmp + "/ambient", "w") as f: f.write(ambient + "\n")
    if missing: open(tmp + "/svc-missing", "w").close()
    for name, body in (("settings", SETTINGS), ("service", SERVICE)):
        p = f"{tmp}/bin/{name}"
        with open(p, "w") as f: f.write(body)
        os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR)
    sh = (block.replace("/data/local/hassmic", tmp + "/hassmic").replace("/data/system", tmp + "/system")
          .replace("/data/app", tmp + "/app"))
    with open(tmp + "/b.sh", "w") as f: f.write(f"KIOSK_PACKAGE={PKG}\n" + sh + "\n" + call + "\n")
    env = dict(os.environ, T=tmp, PATH=tmp + "/bin:" + os.environ["PATH"])
    o = subprocess.run(["sh", tmp + "/b.sh"], capture_output=True, text=True, env=env)
    if o.returncode or o.stderr: print(o.stdout, o.stderr, file=sys.stderr)
    store = {f.split("_", 1)[1]: open(os.path.join(tmp, "store", f)).read().strip() for f in os.listdir(tmp + "/store")}
    calls = open(tmp + "/calls").read().splitlines() if os.path.exists(tmp + "/calls") else []
    saved = open(tmp + "/hassmic/alexa-settings").read() if os.path.exists(tmp + "/hassmic/alexa-settings") else ""
    return store, calls, saved, o.stdout


def svc(tmp):
    """the adaptive colour service after a run: its state, and the sets it was asked for"""
    calls = open(tmp + "/svc-calls").read().splitlines() if os.path.exists(tmp + "/svc-calls") else []
    return open(tmp + "/ambient").read().strip(), [c for c in calls if " ambientdisplayinfo 1 " in c]


STOCK = {"screen_brightness_mode": "1", "screen_brightness": "238"}
START = CHANGE = "backlight_set $(bl_now)"

with tempfile.TemporaryDirectory() as tmp:
    st, calls, saved, _ = run(tmp, "", START, STOCK)
    check("nothing saved: manual 120, as every Show ran before", st.get("screen_brightness_mode") == "0" and st.get("screen_brightness") == "120", st)
    check("stock's values kept for alexa-on.sh", "system screen_brightness_mode 1" in saved and "system screen_brightness 238" in saved, saved)
    check("no adjustment written without adaptive", "screen_auto_brightness_adj" not in st, st)

    st, _, _, _ = run(tmp, "screen_brightness_auto=on\nscreen_brightness_adjust=35\n", START, STOCK)
    check("adaptive: mode 1, the adjustment as -1..1", st.get("screen_brightness_mode") == "1" and st.get("screen_auto_brightness_adj") == "0.35", st)
    check("adaptive leaves the level as it was", st.get("screen_brightness") == "238", st)
    for adj, want in (("-100", "-1.00"), ("5", "0.05"), ("-7", "-0.07"), ("0", "0.00"), ("100", "1.00")):
        st, _, _, _ = run(tmp, f"screen_brightness_auto=on\nscreen_brightness_adjust={adj}\n", START, STOCK)
        check(f"adjustment {adj} written {want}", st.get("screen_auto_brightness_adj") == want, st)

    for lvl, want in (("100", "255"), ("1", "3"), ("47", "120"), ("50", "128")):
        st, _, _, _ = run(tmp, f"screen_brightness={lvl}\n", START, STOCK)
        check(f"level {lvl} % = {want}", st.get("screen_brightness") == want, st)
    # the daemon's file: anything but a short whole number is the default, out of range is clamped
    for bad, want in (("99999", "120"), ("-5", "3"), ("1;reboot", "120"), ("4-2", "120"), ("", "120"), ("-", "120"), ("200", "255")):
        st, _, _, out = run(tmp, f"screen_brightness={bad}\n", START, STOCK)
        check(f"level {bad!r}: {want}", st.get("screen_brightness") == want, (st, out))
    st, _, _, _ = run(tmp, "screen_brightness_auto=on\nscreen_brightness_adjust=-500\n", START, STOCK)
    check("adjustment out of range clamped", st.get("screen_auto_brightness_adj") == "-1.00", st)
    st, _, _, _ = run(tmp, "screen_brightness=30\nscreen_brightness=80\n", START, STOCK)
    check("the last line wins, as hassmic reads it", st.get("screen_brightness") == "204", st)

    # the loop itself, two rounds: sleep stands in, the config changed between them
    # sleep's second call changes the config, its fourth ends the loop (as `exit`: the loop is `while :`)
    LOOP = ("getprop() { echo 1; }; n=0; sleep() { n=$((n + 1)); [ $n = 2 ] && echo $1 >> $T/hassmic/state/config; [ $n -lt 4 ] || exit 0; }\n"
            "BACKLIGHT=1; backlight")
    st, _, _, out = run(tmp, "screen_brightness=80\n", LOOP.replace("$1", "screen_brightness=60"), STOCK)
    check("a new level in the loop: set, and logged", st.get("screen_brightness") == "153" and out.splitlines() == ["backlight: 80 %", "backlight: 60 %"], (st, out))
    st, _, _, out = run(tmp, "", LOOP.replace("$1", "screen_brightness_auto=on"), STOCK)
    check("adaptive switched on in the loop", st.get("screen_brightness_mode") == "1" and "adaptive, adjusted 0" in out, (st, out))
    st, _, _, out = run(tmp, "screen_brightness=80\n", LOOP.replace("$1", "screen_brightness_adjust=40"), STOCK)
    check("the adjustment alone, manual: the level stays", st.get("screen_brightness") == "204" and out.count("backlight: 80 %") == 2, (st, out))
    st, calls, _, out = run(tmp, "", "getprop() { echo 1; }; n=0; sleep() { n=$((n + 1)); [ $n -lt 5 ] || exit 0; }\nBACKLIGHT=1; backlight", STOCK)
    check("nothing changed: no Java call after the first round", [c for c in calls if c.startswith("put")] == ["put system screen_brightness_mode 0",
          "put system screen_brightness 120"] and out.strip() == "backlight: 47 %", (calls, out))
    # set by something else (an app): set again, three times at most until the setting changes
    DRIFT = ("getprop() { echo 1; }; n=0; sleep() { n=$((n + 1)); [ $n -lt 12 ] || exit 0; [ $n -lt 10 ] || return 0\n"
             "  [ $((n % 2)) = 1 ] && settings put system screen_brightness 137; [ $n = 9 ] && echo screen_brightness=50 >> $T/hassmic/state/config; }\n"
             "BACKLIGHT=1; backlight")
    st, calls, _, out = run(tmp, "", DRIFT, STOCK)
    check("changed elsewhere: set again three times, then left, until the setting changes", out.count("changed elsewhere, set again") == 3
          and out.count("left to it from now on") == 1 and out.strip().endswith("backlight: 50 %") and st.get("screen_brightness") == "128", (st, out))
    # a set that fails while setting back a change from elsewhere: no drift counted, tried again until it takes
    DFAIL = ("getprop() { echo 1; }; n=0; sleep() { n=$((n + 1)); [ $n -lt 9 ] || exit 0\n"
             "  [ $n = 1 ] && { settings put system screen_brightness 137; echo 4 > $T/fail-put; }; }\n"
             "BACKLIGHT=1; backlight")
    st, calls, _, out = run(tmp, "", DFAIL, STOCK)
    check("a failed set after a change from elsewhere: retried, not given up", st.get("screen_brightness") == "120"
          and out.count("changed elsewhere, set again") == 1 and "left to it" not in out, (st, out))
    # a Java call that fails: nothing noted for alexa-on.sh, tried again 2 s later
    st, _, saved, out = run(tmp, "", LOOP.replace("$1", "x=1"), STOCK, fail={"get": 1})
    check("settings get failing: not noted as null, set on the next round", "null" not in saved and "system screen_brightness_mode 1" in saved
          and st.get("screen_brightness") == "120" and "could not be read" in out, (st, saved, out))
    st, calls, _, out = run(tmp, "", LOOP.replace("$1", "x=1"), STOCK, fail={"put": 1})
    # failing for good: said once, not every round; after 10 in a row every 30 s
    PERSIST = ("getprop() { echo 1; }; n=0; sleep() { echo $1 >> $T/sleeps; n=$((n + 1)); [ $n -lt 14 ] || exit 0; }\n"
               "rm -f $T/sleeps; BACKLIGHT=1; backlight")
    st, calls, _, out = run(tmp, "", PERSIST, STOCK, fail={"put": 1000})
    sleeps = open(tmp + "/sleeps").read().split()
    check("settings put failing for good: said once, tried each round, backing off after 10",
          out.count("could not be set") == 1 and calls.count("put system screen_brightness_mode 0") == 14
          and sleeps == ["2"] * 9 + ["30"] * 5, (out, calls.count("put system screen_brightness_mode 0"), sleeps))
    st, calls, _, out = run(tmp, "", LOOP.replace("$1", "x=1"), STOCK, fail={"put": 1})
    check("and once it takes, said so", "backlight: 47 %" in out, out)
    check("settings put failing: tried again", st.get("screen_brightness_mode") == "0" and st.get("screen_brightness") == "120"
          and calls.count("put system screen_brightness_mode 0") == 2 and "could not be set" in out, (st, calls, out))

    st, calls, _, _ = run(tmp, "", START, STOCK, xml='<setting id="1" name="screen_brightness_mode" value="0" />\n'
                                                     '<setting id="2" name="screen_brightness" value="120" />\n')
    check("already so in SettingsProvider's file: no Java call", calls == [], calls)

    _, _, saved, _ = run(tmp, "", START + "\n" + "echo screen_brightness=90 >> $T/hassmic/state/config\n" + CHANGE, STOCK)
    check("stock's value kept once, not what we set later", saved.count("screen_brightness ") == 1 and "238" in saved, saved)
    _, _, saved, _ = run(tmp, "screen_brightness_auto=on\n", START, {})
    check("a key never set before is kept as null (alexa-on.sh deletes it)", "system screen_auto_brightness_adj null" in saved, saved)

    # adaptive colour: the loop's rounds; sleep's second call runs $1, its sixth ends the loop
    CLOOP = ("getprop() { echo 1; }; n=0; sleep() { n=$((n + 1)); [ $n = 2 ] && { $1; }; [ $n -lt 6 ] || exit 0; }\n"
             "BACKLIGHT=1; ADAPTIVE_COLOUR=1; backlight")
    _, _, saved, out = run(tmp, "", CLOOP.replace("$1", ":"), STOCK)
    check("adaptive colour: off by default, stock's on saved for restore", svc(tmp) == ("0", ["call ambientdisplayinfo 1 i32 0"])
          and "service ambientdisplayinfo 1\n" in saved and "adaptive colour: off" in out, (svc(tmp), saved, out))
    check("confirmed readback published for the settings page", open(tmp + "/hassmic/adaptive-colour").read() == "0\n")
    _, _, saved, out = run(tmp, "", CLOOP.replace("$1", ":"), STOCK, ambient="0")
    check("already off before upgrade: stays off without a Binder set", svc(tmp) == ("0", []) and "service" not in saved, (svc(tmp), saved, out))
    _, _, saved, out = run(tmp, "screen_adaptive_colour=off\n", CLOOP.replace("$1", ":"), STOCK)
    check("adaptive colour off: set once, logged", svc(tmp) == ("0", ["call ambientdisplayinfo 1 i32 0"])
          and out.count("adaptive colour: off") == 1, (svc(tmp), out))
    check("stock's value kept for alexa-on.sh", "service ambientdisplayinfo 1\n" in saved, saved)
    restore = open(ROOT + "/scripts/device/alexa-on.sh").read().split("SETTINGS=", 1)[1].split("# the rows", 1)[0]
    restore = "SETTINGS=" + restore.replace("/data/local/hassmic", tmp + "/hassmic")
    with open(tmp + "/restore.sh", "w") as f: f.write(restore)
    restored = subprocess.run(["sh", tmp + "/restore.sh"], env=dict(os.environ, T=tmp, PATH=tmp + "/bin:" + os.environ["PATH"]), capture_output=True)
    check("alexa-on.sh restores stock's colour and brightness", restored.returncode == 0 and svc(tmp)[0] == "1"
          and open(tmp + "/store/system_screen_brightness").read().strip() == "238"
          and not os.path.exists(tmp + "/hassmic/alexa-settings"), restored.stderr)
    _, _, _, _ = run(tmp, "screen_adaptive_colour=off\n", CLOOP.replace("$1", ":"), STOCK, ambient="0")
    check("already off: not set", svc(tmp) == ("0", []), svc(tmp))
    run(tmp, "screen_adaptive_colour=off\nscreen_adaptive_colour=on\n", CLOOP.replace("$1", ":"), STOCK, ambient="0")
    check("the last line wins, as hassmic reads it", svc(tmp) == ("1", ["call ambientdisplayinfo 1 i32 1"]), svc(tmp))
    _, _, saved, out = run(tmp, "screen_adaptive_colour=off\n", CLOOP.replace("$1", "echo screen_adaptive_colour=on >> $T/hassmic/state/config"), STOCK)
    check("switched on again in the loop: set, logged; stock's value kept once", svc(tmp) == ("1", ["call ambientdisplayinfo 1 i32 0", "call ambientdisplayinfo 1 i32 1"])
          and out.splitlines() == ["backlight: 47 %", "adaptive colour: off", "adaptive colour: on"] and saved.count("service ") == 1, (svc(tmp), out, saved))
    run(tmp, "screen_adaptive_colour=off\n", CLOOP.replace("$1", "echo 1 > $T/ambient"), STOCK)
    check("switched on elsewhere (fosservices after a video): left alone until the setting changes",
          svc(tmp) == ("1", ["call ambientdisplayinfo 1 i32 0"]), svc(tmp))
    check("external state change shown by readback", open(tmp + "/hassmic/adaptive-colour").read() == "1\n")
    run(tmp, "", CLOOP.replace("$1", "echo 1 > $T/ambient; touch $T/hassmic/state/adaptive-colour-request"), STOCK)
    check("explicit off again after an external change: reapplied, request consumed",
          svc(tmp) == ("0", ["call ambientdisplayinfo 1 i32 0"] * 2)
          and not os.path.exists(tmp + "/hassmic/state/adaptive-colour-request"), svc(tmp))
    MLOOP = ("getprop() { echo 1; }; n=0; sleep() { n=$((n + 1)); [ $n = 8 ] && echo screen_adaptive_colour=on >> $T/hassmic/state/config\n"
             "  [ $n = 9 ] && rm $T/svc-missing; [ $n -lt 11 ] || exit 0; }\nBACKLIGHT=1; ADAPTIVE_COLOUR=1; backlight")
    _, _, saved, out = run(tmp, "screen_adaptive_colour=off\n", MLOOP, STOCK, missing=True)
    gets = open(tmp + "/svc-calls").read().splitlines()
    check("no service: failure said once, nothing noted; readback recovers when the service appears", gets == ["call ambientdisplayinfo 2"] * 11
          and out.count("could not be switched off") == 1 and "service" not in saved
          and svc(tmp) == ("1", []), (gets, out, saved))
    late = CLOOP.replace("$1", ":").replace("[ $n = 2 ] && { :; };", "[ $n = 7 ] && rm $T/svc-missing;").replace("-lt 6", "-lt 10")
    run(tmp, "", late, STOCK, missing=True)
    check("default off applied even if the service starts late", svc(tmp) == ("0", ["call ambientdisplayinfo 1 i32 0"]), svc(tmp))
    _, _, _, out = run(tmp, "", "touch $T/svc-refuse\n" + CLOOP.replace("$1", ":"), STOCK)
    check("refused write: five tries, actual state stays on, failure logged once", svc(tmp) == ("1", ["call ambientdisplayinfo 1 i32 0"] * 5)
          and open(tmp + "/hassmic/adaptive-colour").read() == "1\n" and "adaptive colour: off" not in out
          and out.count("could not be switched off") == 1, (svc(tmp), out))
    retry = CLOOP.replace("[ $n = 2 ]", "[ $n = 6 ]").replace("-lt 6", "-lt 9").replace(
        "$1", "rm $T/svc-refuse; touch $T/hassmic/state/adaptive-colour-request")
    run(tmp, "", "touch $T/svc-refuse\n" + retry, STOCK)
    check("failed off can be retried explicitly with the same preference", svc(tmp) == ("0", ["call ambientdisplayinfo 1 i32 0"] * 6), svc(tmp))
    run(tmp, "", CLOOP.replace("$1", "touch $T/svc-missing"), STOCK)
    check("service disappears: stale readback removed", not os.path.exists(tmp + "/hassmic/adaptive-colour"))
    run(tmp, "screen_adaptive_colour=off\n", "getprop() { echo 1; }; n=0; sleep() { n=$((n + 1)); [ $n -lt 4 ] || exit 0; }\nBACKLIGHT=1; backlight", STOCK)
    check("no ADAPTIVE_COLOUR (every other Show): never asked", not os.path.exists(tmp + "/svc-calls"), svc(tmp))

# board.backlight (the settings) and device.conf's BACKLIGHT (root applying them) say the same; neither without a screen
for m in sorted(os.listdir(ROOT + "/devices")):
    conf, board, mk = (f"{ROOT}/devices/{m}/{f}" for f in ("device.conf", "board.c", "device.mk"))
    if os.path.exists(conf) and os.path.exists(board):
        a = "\nBACKLIGHT=1" in open(conf).read(); b = ".backlight = 1" in open(board).read()
        scr = "src/hassmic/screen.c" in open(mk).read()
        check(f"{m}: board.backlight and BACKLIGHT agree, with a screen", a == b == scr, f"{a} {b} {scr}")
        c = "\nADAPTIVE_COLOUR=1" in open(conf).read(); d = ".adaptive_colour = 1" in open(board).read()
        check(f"{m}: board.adaptive_colour and ADAPTIVE_COLOUR agree, only with a backlight", c == d and (a or not c), f"{c} {d} {a}")

sys.exit(1 if fails else 0)
