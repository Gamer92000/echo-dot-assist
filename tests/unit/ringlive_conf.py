#!/usr/bin/env python3
"""board.c's ring_frame (hassmic writes the ring live, ringlive.c) and device.conf's LED_FRAME (root's main.sh lets it)
name the same file on every model, a file in sysfs, and only on a model with a light ring (no screen)."""
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
    b = re.search(r'\.ring_frame\s*=\s*"([^"]*)"', board)
    c = re.search(r'^LED_FRAME=(\S*)', conf, re.M)
    b, c = b.group(1) if b else "", c.group(1) if c else ""
    ok = b == c and (not b or (b.startswith("/sys/") and not re.search(r'^RING\s*[:?]?=.*ring_band', mk, re.M)))
    print(f"{'ok  ' if ok else 'FAIL'} {m}: board.c {b or '-'}, device.conf {c or '-'}")
    fails += not ok
print("ringlive_conf: " + ("FAILED" if fails else "all ok"))
sys.exit(1 if fails else 0)
