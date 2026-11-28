#!/usr/bin/env python3
"""NOTICE as each model's binaries carry it (scripts/notice.py), and the places that ship it.

Every model gets the opening and "Every model", each "Full text:" replaced by its file whole; only the Echo 4 gets
its radio programs' part, with Qorvo's line; a fork's repository replaces the project's.  And the bundle, the
installers and CI take the file along (nothing here runs those: a tripwire against a list losing it).
"""
import os
import re
import subprocess
import sys

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
fails = 0


def ok(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    fails += not cond


def notice(*args):
    r = subprocess.run([sys.executable, os.path.join(TOP, "scripts", "notice.py"), *args], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def read(path):
    with open(os.path.join(TOP, path), encoding="utf-8") as f:
        return f.read()


source = read("NOTICE")
included = re.findall(r"^Full text: (\S+)$", source, re.M)
ok(len(included) >= 4, "NOTICE names the licence files it takes whole (%d)" % len(included))
models = sorted(d for d in os.listdir(os.path.join(TOP, "devices")) if os.path.isfile(os.path.join(TOP, "devices", d, "device.conf")))
radio = [m for m in models if re.search(r"^RADIO_RCP=\S", read("devices/%s/device.conf" % m), re.M)]
ok(radio == ["laser"], "the models with the radio programs: %s" % radio)
qorvo = "    Copyright (c) 2017-2021, Qorvo Inc.\n"
for m in models:
    rc, out, err = notice(m)
    good = rc == 0 and not re.search(r"^Full text:", out, re.M) and out.startswith("Notices\n=======\n")
    good = good and all(read(p).rstrip("\n") in out for p in included) and "\nEvery model\n===========\n" in out
    ok(good, "%s: made, every file it names in it whole%s" % (m, "" if rc == 0 else ": " + err.strip()))
    ok((qorvo in out) == (m in radio) and ("gprcp" in out) == (m in radio),
       "%s: the radio programs' part %s" % (m, "in it" if m in radio else "left out"))
rc, out, _ = notice("donut", "someone/fork")
ok(rc == 0 and "github.com/someone/fork," in out and "Gamer92000" not in out, "a fork's repository in place of the project's")
rc, _, err = notice("nosuchmodel")
ok(rc != 0 and "unknown model" in err, "an unknown model refused")

# where it goes
bundle = read("scripts/bundle.sh")
ok("\nnotice_make\n" in bundle and "$OUT/NOTICE:644" in bundle, "bundle.sh: written, packed with mode 644")
for s in ("scripts/install-system.sh", "scripts/install-boot.sh", "scripts/deploy.sh"):
    t = read(s)
    ok("notice_make" in t and "$OUT/NOTICE" in t, "%s: written and pushed" % s)
ok(re.search(r"^    device\.conf\|VERSION\|NOTICE\|", read("scripts/system/sysinstall.sh"), re.M) is not None,
   "sysinstall.sh: NOTICE onto /system with mode 644")
ok("build/${{ matrix.device }}/NOTICE" in read(".github/workflows/build.yml"), "CI: in each model's build artifact")
ok("|NOTICE|" in read("scripts/lib/build.sh"), "prebuilt_fetch: the release's own left out (this checkout's is written)")
sys.exit(1 if fails else 0)
