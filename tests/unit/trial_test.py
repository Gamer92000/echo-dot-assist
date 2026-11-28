#!/usr/bin/env python3
"""main.sh's test binary rule (issue #251): /data/local/hassmic/hassmic (scripts/deploy.sh) runs in place of the
installed copy only until something newer is installed.  The block between "# >>> trial" and "# <<< trial" is cut out
of the script, its paths under /data moved to a temporary folder, and run with a stand-in `stat` that reports the
change times this test sets (the PC cannot set a file's change time), as backlight_test.py does with its tools."""
import os, stat, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
fails = 0


def check(what, ok, extra=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else f"\n     {extra}"))
    fails += 0 if ok else 1


# stat -c %Z <file>: the time in $T/ctime/<the file's path with / as _>; none (or $T/no-stat): fails, as a missing tool
STAT = r'''#!/bin/sh
[ -f "$T/no-stat" ] && exit 127
[ "$1" = -c ] && [ "$2" = %Z ] || exit 2
[ -e "$3" ] || [ -L "$3" ] || { echo "stat: '$3': No such file or directory" >&2; exit 1; }
f="$T/ctime/$(printf %s "$3" | tr / _)"
[ -f "$f" ] && cat "$f" || exit 1
'''

text = open(ROOT + "/scripts/system/main.sh").read()
block = text[text.index("# >>> trial"):text.index("# <<< trial")]


def run(tmp, trial=None, version=None, call="", no_stat=False, link=None):
    """the block sourced with D=$tmp/cur, then `call`, then BIN printed; TRIAL/VERSION: their change times (None: the
    file is not there); LINK: the test binary is a symbolic link to that path.  Returns (BIN, log, trial still there)"""
    d = os.path.join(tmp, "cur"); h = os.path.join(tmp, "hassmic")
    for x in (d, h, tmp + "/bin", tmp + "/ctime"):
        os.makedirs(x, exist_ok=True)
    for f in os.listdir(tmp + "/ctime"): os.remove(os.path.join(tmp, "ctime", f))
    for p in (h + "/hassmic", h + "/boot.log", d + "/VERSION", tmp + "/no-stat"):
        if os.path.lexists(p): os.remove(p)
    if no_stat: open(tmp + "/no-stat", "w").close()

    def at(path, t):
        with open(tmp + "/ctime/" + path.replace("/", "_"), "w") as f: f.write(f"{t}\n")
    if link is not None:
        os.symlink(link, h + "/hassmic")
    elif trial is not None:
        with open(h + "/hassmic", "w") as f: f.write("#!/bin/sh\n")
        os.chmod(h + "/hassmic", 0o755); at(h + "/hassmic", trial)
    if version is not None:
        with open(d + "/VERSION", "w") as f: f.write("2026.11.26.214508\n")
        at(d + "/VERSION", version)
    p = tmp + "/bin/stat"
    with open(p, "w") as f: f.write(STAT)
    os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR)
    sh = block.replace("/data/local/hassmic", h)
    with open(tmp + "/t.sh", "w") as f:
        f.write(f'D={d}\nLOG={h}/boot.log\nsay() {{ echo "@ $*"; }}\n' + sh + "\n" + (call + ' >> $LOG 2>&1' if call else '') + '\necho "BIN=$BIN"\n')      # ota_watch's stdout is the log
    env = dict(os.environ, T=tmp, PATH=tmp + "/bin:" + os.environ["PATH"])
    o = subprocess.run(["sh", tmp + "/t.sh"], capture_output=True, text=True, env=env)
    if o.returncode or o.stderr: print(o.stdout, o.stderr, file=sys.stderr)
    b = next((l[4:] for l in o.stdout.splitlines() if l.startswith("BIN=")), "")
    log = open(h + "/boot.log").read() if os.path.exists(h + "/boot.log") else ""
    return b.replace(tmp, "$T"), log.replace(tmp, "$T"), os.path.lexists(h + "/hassmic")


tmp = tempfile.mkdtemp()
TRIAL = "$T/hassmic/hassmic"

# the Echo of issue #251: deploy.sh's binary of 2026-11-18, the update of 2026-11-26 installed after it
b, log, there = run(tmp, trial=1795000000, version=1795600000)
check("a test binary older than the installed copy is removed at the start", not there and b == "$T/cur/hassmic", (b, log))
check("... and the log says so, naming the copy that runs now",
      f"== test binary {TRIAL} removed: older than the copy installed after it, 2026.11.26.214508, which runs now" in log, log)
check("... said once", log.count("removed") == 1, log)

# the developer's own trial run: pushed after the copy was installed
b, log, there = run(tmp, trial=1795600001, version=1795600000)
check("a test binary newer than the installed copy runs in its place", there and b == TRIAL, (b, log))
check("... without a word", log == "", log)

b, log, there = run(tmp, trial=1795600000, version=1795600000)
check("the same second: kept (strictly older only)", there and b == TRIAL, (b, log))

b, log, there = run(tmp, trial=1795000000, version=None)
check("no VERSION beside the copy: kept, as before", there and b == TRIAL, (b, log))

b, log, there = run(tmp, trial=1795000000, version=1795600000, no_stat=True)
check("no stat: kept, as before", there and b == TRIAL, (b, log))

b, log, there = run(tmp, trial=None, version=1795600000)
check("no test binary: the copy runs, nothing said", b == "$T/cur/hassmic" and log == "", (b, log))

# an update's install (ota_watch) removes it whatever its time
b, log, there = run(tmp, trial=1795600001, version=1795600000,
                    call='trial_drop "the update 2026.11.28.120000 installed after it runs in its place"')
check("an update's install removes a newer test binary too", not there, log)
check("... and says so", f"== test binary {TRIAL} removed: the update 2026.11.28.120000 installed after it runs in its place" in log, log)

b, log, there = run(tmp, version=1795600000, call='trial_drop "x"')
check("an install with no test binary says nothing", log == "", log)

# a link left there: never run (not executable through a dangling link), removed by an install, not followed
b, log, there = run(tmp, version=1795600000, link=tmp + "/elsewhere")
check("a dangling link is not run", b == "$T/cur/hassmic" and there, (b, log))
open(tmp + "/target", "w").close()
b, log, there = run(tmp, version=1795600000, link=tmp + "/target", call='trial_drop "x"')
check("an install removes a link, not what it points to", not there and os.path.exists(tmp + "/target"), log)

print("trial: " + ("all passed" if not fails else f"{fails} failed"))
sys.exit(1 if fails else 0)
