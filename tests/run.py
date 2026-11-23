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
    ("arb-duck", [PY, "tests/fake_ha_arbitration_duck.py"]),
    ("arb-ringstop", [PY, "tests/fake_ha_arbitration_ringstop.py"]),
    ("stop-word", [PY, "tests/fake_ha_stop_word.py"]),
    ("esphome-features", [PY, "tests/fake_ha_esphome_features.py"]),
    ("esphome-address", [PY, "tests/fake_ha_esphome_address.py"]),
    ("esphome", [PY, "tests/fake_ha_esphome.py"]),
    ("esphome-buttons", [PY, "tests/fake_ha_esphome_buttons.py"]),
    ("esphome-timers", [PY, "tests/fake_ha_esphome_timers.py"]),
    ("end-of-request", [PY, "tests/fake_ha_end.py"]),
    ("wakealign", [PY, "tests/fake_ha_wakealign.py"]),
    ("sendspin", [PY, "tests/fake_ma_sendspin.py"]),
    ("sendspin-9", [PY, "tests/fake_ma_sendspin.py", "--legacy"]),
    ("wifi", [PY, "tests/fake_web_wifi.py"]),
    ("wifi-join", [PY, "tests/fake_wifi_join.py"]),
    ("volumes", [PY, "tests/fake_ha_volumes.py"]),
    ("timer-sound", [PY, "tests/fake_ha_timer_sound.py"]),
    ("sounds", [PY, "tests/fake_web_sounds.py"]),
    ("improv", [PY, "tests/fake_improv.py"]),
    ("bt-proxy", [PY, "tests/fake_ha_bt_proxy.py"]),
    ("dropin", [PY, "tests/fake_ha_dropin.py"]),
    ("unit", ["make", "-s", "-j4", "unit"]),
    ("mww", [PY, "tests/fake_web_mww.py"]),
    ("plugins", [PY, "tests/fake_plugins.py"]),
    ("plugins-ha", [PY, "tests/fake_plugins_ha.py"]),
    ("plugins-fetch", [PY, "tests/fake_plugins_fetch.py"]),
    ("plugins-race", [PY, "tests/fake_plugins_race.py"]),
    ("plugins-sync", [PY, "tests/fake_plugins_sync.py"]),
    ("plugins-selftest", [PY, "tests/fake_plugins_selftest.py"]),
    ("plugins-net", [PY, "tests/fake_plugins_net.py"]),
    ("plugins-lan", [PY, "tests/fake_plugins_lan.py"]),
    ("plugins-music", [PY, "tests/fake_plugins_music.py"]),
    ("plugins-call", [PY, "tests/fake_plugins_call.py"]),
    ("plugins-request", [PY, "tests/fake_plugins_request.py"]),
    ("plugins-button", [PY, "tests/fake_plugins_button.py"]),
    ("plugins-community", [PY, "tests/fake_plugins_community.py"]),
    ("mww-migrate", [PY, "tests/fake_mww_migrate.py"]),
    ("web", [PY, "tests/fake_web.py"]),
    ("update", [PY, "tests/fake_ha_update.py"]),
    ("wakesound", [PY, "tests/fake_ha_wakesound.py"]),
    ("remote-bargein", [PY, "tests/fake_ha_remote_bargein.py"]),
    ("remote-followup", [PY, "tests/fake_ha_remote_followup.py"]),
    ("davs", [PY, "tests/fake_web_davs.py"]),
    ("ota-push", ["sh", "tests/ota_push_test.sh"]),
    ("ota-port", [PY, "tests/fake_ota_port.py"]),
    ("ring", [PY, "tests/fake_ha_ring.py"]),
    ("ring-acs", [PY, "tests/fake_ha_ring_acs.py"]),
    ("ring-designs", [PY, "tests/fake_web_ring.py"]),
    ("ring-live", [PY, "tests/fake_ha_ring_live.py"]),
    ("ring-live-acs", [PY, "tests/fake_ha_ring_live_acs.py"]),
    ("artifacts", [PY, "tests/fake_web_artifacts.py"]),
    ("wyoming", [PY, "tests/fake_ha.py"]),
    ("talk-duplex", [PY, "tests/fake_talk_duplex.py"]),
    ("screen", [PY, "tests/fake_screen.py"]),
    ("screen-9", [PY, "tests/fake_screen.py", "--legacy"]),
    ("screen-words", [PY, "tests/fake_screen_words.py"]),
    ("ma-api", [PY, "tests/fake_ma_api.py"]),
    ("otatool", ["sh", "tests/otatool_test.sh"]),
]
# what a test needs from the network, made here before the namespaces (each a no-op once done); a test whose setup
# failed says so itself
SETUP = {"sendspin-9": [PY, "tests/fake_ma_sendspin.py", "--setup"],      # aiosendspin 9.1.1 beside .venv's
         "screen-9": [PY, "tests/fake_ma_sendspin.py", "--setup"]}
RETRY_MAX = 4           # more failing at once than load makes (at most 2 a run seen): something is broken, no reruns


# Tests that need other hosts (tests/fake_plugins_net.py, fake_plugins_lan.py, fake_plugins_music.py: plugins' sockets): a namespace of theirs behind a veth pair
# (tests/peerhost.py), 10.250.0.2 private and 198.51.100.7 outside the local network, the Echo's side 10.250.0.1.  Not
# addresses a "local" route delivers over lo, as before: hassmic refuses those now, as it must (a review of 08399dc)
PEER_TESTS = ("plugins-net", "plugins-lan", "plugins-music")
# their networks (names kept for the plugins' own repositories' tests, which may import them)
OTHER_HOSTS = ("10.250.0.0/24", "198.51.100.0/24")


def other_socket(kind, host, port, backlog=4):
    """a server's socket on one of the other hosts ("udp" or "tcp", 10.250.0.2, 10.250.0.3 or 198.51.100.7), in a
    namespace made by netns(..., peer=True): the test uses it as its own (tests/peerhost.py peer_socket)"""
    sys.path.insert(0, os.path.join(ROOT, "tests")); import peerhost
    return peerhost.peer_socket(kind, host, port, backlog)


# An address on lo for a test that has to send from one that is not on a local network (plugins-sync: a member's word
# from outside it, which plugin_fetch.c pf_message drops); made here, where the namespace's root still may
LO_ADDRS = {"plugins-sync": "ip addr add 198.51.100.9/32 dev lo && "}


def netns(argv, peer=False, name=""):
    """argv in a network namespace of its own with lo up, as the caller's uid again; multicast on it for zeroconf
    (aiosendspin, avahi stand-ins), which fails without an interface that takes the mDNS group (ENODEV).  PEER: with
    the other hosts (tests/peerhost.py), whose socket's folder is the test's to remove"""
    pre = ""
    if peer:
        import tempfile
        sys.path.insert(0, os.path.join(ROOT, "tests")); import peerhost
        pre = peerhost.shell(os.path.join(tempfile.mkdtemp(prefix="hassmic-peer."), "peer"), PY)
    inner = ("ip link set lo multicast on up && ip route add 224.0.0.0/4 dev lo && " + pre + LO_ADDRS.get(name, "")
             + "exec unshare -U --map-user=%d --map-group=%d \"$@\"" % (os.getuid(), os.getgid()))
    return ["unshare", "-rn", "sh", "-c", inner, "sh", *argv]


def run(name, argv, timeout, logdir):
    log = os.path.join(logdir, name + ".log")
    t0 = time.monotonic()
    with open(log, "w") as f:
        f.write("$ " + shlex.join(argv) + "\n"); f.flush()
        try:
            rc = subprocess.run(netns(argv, peer=name in PEER_TESTS, name=name), cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
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
