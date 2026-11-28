#!/usr/bin/env python3
"""board.c's ring_frame, or ring_led_color and ring_led_power (hassmic writes the ring live, ringlive.c), and
device.conf's LED_FRAME (root's main.sh lets it) name the same files on every model (ring_led_color's %d as LED_FRAME's
glob *), files in sysfs, and only on a model with a light ring (no screen)."""
import os, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
fails = 0
for m in sorted(os.listdir(f"{ROOT}/devices")):
    d = f"{ROOT}/devices/{m}"
    if not os.path.isfile(f"{d}/board.c"):
        continue
    board = open(f"{d}/board.c").read()
    conf = open(f"{d}/device.conf").read()
    mk = open(f"{d}/device.mk").read() if os.path.isfile(f"{d}/device.mk") else ""
    fr = re.search(r'\.ring_frame\s*=\s*"([^"]*)"', board)
    col = re.search(r'\.ring_led_color\s*=\s*"([^"]*)"', board)
    pw = re.search(r'\.ring_led_power\s*=\s*\(const char \*const\[\]\)\{([^}]*)\}', board)
    want = ([fr.group(1)] if fr else []) + ([col.group(1).replace("%d", "*")] if col else []) + \
           (re.findall(r'"([^"]*)"', pw.group(1)) if pw else [])
    c = re.search(r'^LED_FRAME=(?:"([^"]*)"|(\S*))', conf, re.M)
    have = (c.group(1) if c.group(1) is not None else c.group(2)).split() if c else []
    b, c = " ".join(want), " ".join(have)
    ok = want == have and not (fr and col) and (not want or (all(f.startswith("/sys/") for f in want)
         and not re.search(r'^RING\s*[:?]?=.*ring_band', mk, re.M)))
    print(f"{'ok  ' if ok else 'FAIL'} {m}: board.c {b or '-'}, device.conf {c or '-'}")
    fails += not ok
print("ringlive_conf: " + ("FAILED" if fails else "all ok"))
sys.exit(1 if fails else 0)
