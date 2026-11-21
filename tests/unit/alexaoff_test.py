#!/usr/bin/env python3
"""alexa-off.sh's Android settings (device.conf ANDROID_SETTINGS and the setup flags): what it sets and what it notes for
alexa-on.sh, on the PC with a stand-in for `settings`, including `settings get` failing (Java under load; issue #189's
review: a failed read was noted as "null", which alexa-on.sh turns into deleting stock's value).  The block is cut out
of the script and its paths under /data moved to a temporary folder, as backlight_test.py does."""
import os, stat, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
fails = 0


def check(what, ok, extra=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else f"\n     {extra}"))
    fails += 0 if ok else 1


# settings get|put <ns> <key> [value]: one file per key, every call logged; $T/fail-get holds how many gets fail next
# ("all": every one), as a Java call under load does: nothing on stdout
SETTINGS = r'''#!/bin/sh
echo "$*" >> "$T/calls"
f="$T/store/$2_$3"
if [ "$1" = get ] && [ -s "$T/fail-get" ]; then
    n=$(cat "$T/fail-get")
    [ "$n" = all ] && exit 1
    echo $((n - 1)) > "$T/fail-get"; [ "$n" -gt 0 ] && exit 1
fi
case "$1" in
get) if [ -f "$f" ]; then cat "$f"; else echo null; fi;;
put) printf '%s\n' "$4" > "$f"                     # and SettingsProvider's file, as Android writes it within 200 ms
     x="$T/system/users/0/settings_$2.xml"; touch "$x"
     { grep -v "name=\"$3\"" "$x"; echo "  <setting id=\"1\" name=\"$3\" value=\"$4\" package=\"root\" />"; } > "$x.new"
     mv "$x.new" "$x";;
esac
'''

text = open(ROOT + "/scripts/device/alexa-off.sh").read()
block = text[text.index("SETTINGS=/data/local/hassmic/alexa-settings"):]
block = block[:block.index("\nfi\n") + 4]
WANT = "global/bluetooth_on=0 system/smartconn_status=enabled global/captive_portal_mode=0"


def run(tmp, stock, fail=None, packages="com.amazon.ds2.oobe.efd"):
    """the block with stock's values in the store (key -> value, "ns_key" names); returns (store, calls, noted, out)"""
    for d in ("bin", "store", "hassmic", "system/users/0"):
        os.makedirs(os.path.join(tmp, d), exist_ok=True)
    for f in ("calls", "fail-get"):
        if os.path.exists(os.path.join(tmp, f)): os.remove(os.path.join(tmp, f))
    for k, v in stock.items():
        with open(f"{tmp}/store/{k}", "w") as f: f.write(v + "\n")
    if fail is not None:
        with open(f"{tmp}/fail-get", "w") as f: f.write(f"{fail}\n")
    with open(f"{tmp}/bin/settings", "w") as f: f.write(SETTINGS)
    os.chmod(f"{tmp}/bin/settings", stat.S_IRWXU)
    script = block.replace("/data/local/hassmic", tmp + "/hassmic").replace("/data/system", tmp + "/system")
    env = dict(os.environ, T=tmp, PATH=f"{tmp}/bin:" + os.environ["PATH"], SETUP_PACKAGES=packages,
               ANDROID_SETTINGS=WANT)
    out = subprocess.run(["sh", "-c", script], env=env, capture_output=True, text=True, timeout=60).stdout
    store = {k: open(f"{tmp}/store/{k}").read().strip() for k in os.listdir(f"{tmp}/store")}
    calls = open(f"{tmp}/calls").read().split("\n")[:-1] if os.path.exists(f"{tmp}/calls") else []
    noted = open(f"{tmp}/hassmic/alexa-settings").read().split("\n")[:-1] \
        if os.path.exists(f"{tmp}/hassmic/alexa-settings") else []
    return store, calls, noted, out


OURS = {"global_device_provisioned": "1", "secure_user_setup_complete": "1", "global_bluetooth_on": "0",
        "system_smartconn_status": "enabled", "global_captive_portal_mode": "0"}
STOCK = {"global_device_provisioned": "0", "secure_user_setup_complete": "0", "global_bluetooth_on": "1"}

# stock: two flags and Bluetooth set, smartconn_status and captive_portal_mode unset
with tempfile.TemporaryDirectory() as t:
    store, calls, noted, out = run(t, STOCK)
    check("every setting set", store == OURS, store)
    check("stock's values noted, unset ones as null",
          noted == ["global device_provisioned 0", "secure user_setup_complete 0", "global bluetooth_on 1",
                    "system smartconn_status null", "global captive_portal_mode null"], noted)
    store, calls, noted2, out = run(t, {})
    check("again: no call at all (SettingsProvider's file has them)", calls == [], calls)
    check("again: nothing noted twice", noted2 == noted, noted2)

# one get fails, the next answers: asked again, stock's value noted as it is
with tempfile.TemporaryDirectory() as t:
    store, calls, noted, out = run(t, STOCK, fail=1)
    check("a failed read asked again", calls[:2] == ["get global device_provisioned", "get global device_provisioned"],
          calls)
    check("then noted as stock had it", noted[0] == "global device_provisioned 0", noted)
    check("every setting set", store == OURS, store)

# Java down for good: never "null" for a value that could not be read; the setup flags set all the same, without
# stock's value noted; the other keys left for the next run; the first key asked three times, every other once
with tempfile.TemporaryDirectory() as t:
    store, calls, noted, out = run(t, STOCK, fail="all")
    check("nothing noted", noted == [], noted)
    check("setup flags set all the same",
          store["global_device_provisioned"] == "1" and store["secure_user_setup_complete"] == "1", store)
    check("other keys left as they were", all(store.get(k) == STOCK.get(k) for k in
          ("global_bluetooth_on", "system_smartconn_status", "global_captive_portal_mode")), store)
    gets = [c for c in calls if c.startswith("get ")]
    per = {k: gets.count(f"get {k.replace('_', ' ', 1)}") for k in OURS}
    check("three reads of the first key, one of each other",
          per == {"global_device_provisioned": 3, "secure_user_setup_complete": 1, "global_bluetooth_on": 1,
                  "system_smartconn_status": 1, "global_captive_portal_mode": 1}, per)
    check("said so", out.count("could not be read: set without") == 2 and out.count("left for the next run") == 3, out)
    # Java back: the keys left over now noted and set, the flags (ours by now) not noted any more
    store, calls, noted, out = run(t, {})
    check("next run: the rest noted", noted == ["global bluetooth_on 1", "system smartconn_status null",
                                                "global captive_portal_mode null"], noted)
    check("next run: every setting set", store == OURS, store)

# a value of stock's already ours: nothing noted, nothing set
with tempfile.TemporaryDirectory() as t:
    store, calls, noted, out = run(t, dict(STOCK, global_captive_portal_mode="0"))
    check("stock's own value: not noted", "global captive_portal_mode 0" not in noted, noted)
    check("nor set", "put global captive_portal_mode 0" not in calls, calls)

# no setup packages (the Dots' kind): the flags left alone
with tempfile.TemporaryDirectory() as t:
    store, calls, noted, out = run(t, STOCK, packages="")
    check("no setup packages: flags untouched", store["global_device_provisioned"] == "0", store)

# every Android model (device.conf ANDROID=1) has Android's captive portal test off (issue #189)
for d in sorted(os.listdir(ROOT + "/devices")):
    c = os.path.join(ROOT, "devices", d, "device.conf")
    if not os.path.exists(c): continue
    env = subprocess.run(["sh", "-c", '. "$1" > /dev/null 2>&1; echo "$ANDROID"; echo "$ANDROID_SETTINGS"', "sh", c],
                         capture_output=True, text=True).stdout.split("\n")
    if env[0] == "1":
        # Android 5.1 (rook) calls it captive_portal_detection_enabled
        key = "captive_portal_detection_enabled" if d == "rook" else "captive_portal_mode"
        check(f"{d}: global/{key}=0 in ANDROID_SETTINGS", f"global/{key}=0" in env[1].split(), env[1])

print("alexa-off settings:", "all ok" if not fails else f"{fails} failed")
sys.exit(1 if fails else 0)
