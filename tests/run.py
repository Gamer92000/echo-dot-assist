#!/usr/bin/env python3
"""Runs the PC tests at once, each in a network namespace of its own, and prints the log of each one that fails.

    tests/run.py [-j N] [-t secs] [--strict] [name ...]      names: see TESTS (default all); -l lists them

The tests mostly wait (audio at real time, timers of hassmic's), so they overlap well beyond the CPUs.  Each one gets a
loopback of its own (unshare -rn, then a user namespace again with the caller's uid, so files and permissions look as
they do outside): the tests bind fixed ports, hassmic its defaults (28928-28932) and the arbitration beacons go to
127.255.255.255, which would otherwise reach the Echos of every other test.  Logs in build/test-logs/<name>.log.
Ubuntu 24.04 lets only programs with an AppArmor profile make user namespaces: sysctl
kernel.apparmor_restrict_unprivileged_userns=0 (CI does).

A test that fails with the others runs again alone afterwards (up to RETRY_MAX of them, one after the other): all at once
on CI's four cores, audio at real time and hassmic's timers run late, and each run a different one failed on that
(2026-10-18..29: arb-join, volumes, ring-designs twice; sendspin, sendspin-9 on a resync of a late player,
timer-sound, esphome-address, web, davs).  Passing alone, it counts as flaky: a warning with its first log
(<name>.first.log), not a failure.  Failing again, it fails.  A race of hassmic's own shows the same way, so a flaky
test is worth a look (arb-join's was one: arb.c ha_since); --strict counts the first run only."""
import argparse, os, shlex, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = os.path.join(ROOT, ".venv", "bin", "python")

# slowest first: with fewer slots than tests the long ones must not start last
TESTS = [
    ("arbitration", [PY, "tests/fake_ha_arbitration.py"]),
    ("arb-window", [PY, "tests/fake_ha_arbitration_window.py"]),
    ("arb-kiosk", [PY, "tests/fake_ha_arbitration_kiosk.py"]),
    ("arb-rounds", [PY, "tests/fake_ha_arbitration_rounds.py"]),
    ("arb-join", [PY, "tests/fake_ha_arbitration_join.py"]),
    ("arb-afe", [PY, "tests/fake_ha_arbitration_afe.py"]),
    ("stop-word", [PY, "tests/fake_ha_stop_word.py"]),
    ("esphome-features", [PY, "tests/fake_ha_esphome_features.py"]),
    ("esphome-address", [PY, "tests/fake_ha_esphome_address.py"]),
    ("esphome", [PY, "tests/fake_ha_esphome.py"]),
    ("esphome-buttons", [PY, "tests/fake_ha_esphome_buttons.py"]),
    ("esphome-timers", [PY, "tests/fake_ha_esphome_timers.py"]),
    ("end-of-request", [PY, "tests/fake_ha_end.py"]),
    ("sendspin", [PY, "tests/fake_ma_sendspin.py"]),
    ("sendspin-9", [PY, "tests/fake_ma_sendspin.py", "--legacy"]),
    ("wifi", [PY, "tests/fake_web_wifi.py"]),
    ("wifi-join", [PY, "tests/fake_wifi_join.py"]),
    ("volumes", [PY, "tests/fake_ha_volumes.py"]),
    ("timer-sound", [PY, "tests/fake_ha_timer_sound.py"]),
    ("improv", [PY, "tests/fake_improv.py"]),
    ("dropin", [PY, "tests/fake_ha_dropin.py"]),
    ("unit", ["make", "-s", "-j4", "unit"]),
    ("mww", [PY, "tests/fake_web_mww.py"]),
    ("web", [PY, "tests/fake_web.py"]),
    ("update", [PY, "tests/fake_ha_update.py"]),
    ("wakesound", [PY, "tests/fake_ha_wakesound.py"]),
    ("davs", [PY, "tests/fake_web_davs.py"]),
    ("ota-push", ["sh", "tests/ota_push_test.sh"]),
    ("ring", [PY, "tests/fake_ha_ring.py"]),
    ("ring-designs", [PY, "tests/fake_web_ring.py"]),
    ("artifacts", [PY, "tests/fake_web_artifacts.py"]),
    ("wyoming", [PY, "tests/fake_ha.py"]),
    ("otatool", ["sh", "tests/otatool_test.sh"]),
]
# what a test needs from the network, made here before the namespaces (each a no-op once done); a test whose setup
# failed says so itself
SETUP = {"sendspin-9": [PY, "tests/fake_ma_sendspin.py", "--setup"]}       # aiosendspin 9.1.1 beside .venv's
RETRY_MAX = 4           # more failing at once than load makes (at most 2 a run seen): something is broken, no reruns


def netns(argv):
    """argv in a network namespace of its own with lo up, as the caller's uid again; multicast on it for zeroconf
    (aiosendspin, avahi stand-ins), which fails without an interface that takes the mDNS group (ENODEV)"""
    inner = "ip link set lo multicast on up && ip route add 224.0.0.0/4 dev lo && exec unshare -U --map-user=%d --map-group=%d \"$@\"" % (os.getuid(), os.getgid())
    return ["unshare", "-rn", "sh", "-c", inner, "sh", *argv]


def run(name, argv, timeout, logdir):
    log = os.path.join(logdir, name + ".log")
    t0 = time.monotonic()
    with open(log, "w") as f:
        f.write("$ " + shlex.join(argv) + "\n"); f.flush()
        try:
            rc = subprocess.run(netns(argv), cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            rc = "timeout"
            f.write(f"\n*** killed after {timeout} s\n")
    return name, rc, time.monotonic() - t0, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=len(TESTS), help="tests at once (default: all)")
    ap.add_argument("-t", type=int, default=300, help="seconds before a test is killed")
    ap.add_argument("-l", action="store_true", help="list the tests")
    ap.add_argument("--strict", action="store_true", help="no reruns: a test that fails with the others fails")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    if a.l:
        for n, argv in TESTS: print(f"{n:16} {shlex.join(argv)}")
        return
    tests = [t for t in TESTS if not a.names or t[0] in a.names]
    unknown = set(a.names) - {n for n, _ in TESTS}
    if unknown: sys.exit(f"no such test: {' '.join(sorted(unknown))} (-l lists them)")
    if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode:
        sys.exit("unshare -rn is not allowed here (user namespaces; Ubuntu: sysctl kernel.apparmor_restrict_unprivileged_userns=0)")
    for n, argv in SETUP.items():
        if any(t[0] == n for t in tests) and subprocess.run(argv, cwd=ROOT).returncode:
            print(f"setup of {n} failed: {shlex.join(argv)}", flush=True)
    logdir = os.path.join(ROOT, "build", "test-logs")
    os.makedirs(logdir, exist_ok=True)
    for f in os.listdir(logdir):
        if f.endswith(".first.log"): os.remove(os.path.join(logdir, f))
    gh = os.environ.get("GITHUB_ACTIONS") == "true"
    t0 = time.monotonic(); failed = []; flaky = []
    with ThreadPoolExecutor(max(1, a.j)) as ex:
        for fut in as_completed([ex.submit(run, n, argv, a.t, logdir) for n, argv in tests]):
            name, rc, secs, log = fut.result()
            print(f"{'ok  ' if rc == 0 else 'FAIL'} {name:16} {secs:6.1f} s" + ("" if rc == 0 else f"  ({rc})"), flush=True)
            if rc != 0: failed.append((name, log))
    if failed and not a.strict and len(failed) <= RETRY_MAX and len(tests) > 1:
        print(f"\nagain, alone: {' '.join(n for n, _ in failed)}", flush=True)
        argvs = dict(tests); again = []
        for name, log in failed:
            first = log[:-len(".log")] + ".first.log"; os.replace(log, first)
            _, rc, secs, log = run(name, argvs[name], a.t, logdir)
            print(f"{'ok  ' if rc == 0 else 'FAIL'} {name:16} {secs:6.1f} s alone" + ("" if rc == 0 else f"  ({rc})"), flush=True)
            if rc == 0: flaky.append((name, first))
            else: again.append((name, log))
        failed = again
    for name, log in flaky:
        print(f"::group::{name} (flaky: its first run)" if gh else f"\n===== {name}, flaky: its first run ({os.path.relpath(log, ROOT)})")
        sys.stdout.write(open(log, errors="replace").read())
        if gh: print("::endgroup::"); print(f"::warning title=flaky test::{name} failed with the others and passed alone (first log: test-logs/{name}.first.log)")
    for name, log in failed:
        print(f"::group::{name}" if gh else f"\n===== {name} ({os.path.relpath(log, ROOT)})")
        sys.stdout.write(open(log, errors="replace").read())
        if gh: print("::endgroup::"); print(f"::error::{name} failed")
    print(f"\n{len(tests) - len(failed)} of {len(tests)} passed in {time.monotonic() - t0:.1f} s" +
          (f"; flaky (passed alone): {' '.join(n for n, _ in flaky)}" if flaky else "") +
          (f"; failed: {' '.join(n for n, _ in failed)}" if failed else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
