#!/usr/bin/env python3
"""main.sh's backlight block (issue #126): the Echo Show's brightness settings from state/config into Android's own
settings, on the PC with a stand-in for `settings`.  Root's script with paths under /data: the text is cut out and those
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

text = open(ROOT + "/scripts/system/main.sh").read()
block = text[text.index("# >>> backlight"):text.index("# <<< backlight")]


def run(tmp, config, call, stock=None, xml="", fail=None):
    """the block sourced, then `call`; returns (store, calls, alexa-settings, output).  fail: {"get"|"put": n} calls fail"""
    for d in ("bin", "store", "hassmic/state", "system/users/0", "app"):
        os.makedirs(os.path.join(tmp, d), exist_ok=True)
    for k, n in (fail or {}).items():
        with open(f"{tmp}/fail-{k}", "w") as f: f.write(f"{n}\n")
    for f in ("calls", "hassmic/alexa-settings", "fail-get", "fail-put"):
        if f.startswith("fail") and f[5:] in (fail or {}): continue
        if os.path.exists(os.path.join(tmp, f)): os.remove(os.path.join(tmp, f))
    for f in os.listdir(tmp + "/store"): os.remove(os.path.join(tmp, "store", f))
    for k, v in (stock or {}).items():
        with open(f"{tmp}/store/system_{k}", "w") as f: f.write(v + "\n")
    with open(tmp + "/hassmic/state/config", "w") as f: f.write(config)
    with open(tmp + "/system/users/0/settings_system.xml", "w") as f: f.write(xml)
    p = tmp + "/bin/settings"
    with open(p, "w") as f: f.write(SETTINGS)
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

# board.backlight (the settings) and device.conf's BACKLIGHT (root applying them) say the same; neither without a screen
for m in sorted(os.listdir(ROOT + "/devices")):
    conf, board, mk = (f"{ROOT}/devices/{m}/{f}" for f in ("device.conf", "board.c", "device.mk"))
    if os.path.exists(conf) and os.path.exists(board):
        a = "\nBACKLIGHT=1" in open(conf).read(); b = ".backlight = 1" in open(board).read()
        scr = "src/hassmic/screen.c" in open(mk).read()
        check(f"{m}: board.backlight and BACKLIGHT agree, with a screen", a == b == scr, f"{a} {b} {scr}")

sys.exit(1 if fails else 0)
