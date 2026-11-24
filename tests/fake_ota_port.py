#!/usr/bin/env python3
"""The push port (ota.c) against senders without the key: build/hassmic-host's port, strangers played here from source
addresses of their own (127.0.0.x, bound per socket: the tests' network namespace has only loopback), the owner by
scripts/otatool.py and the Echo's own otatool (build/otatool-host).  Connections that ask for adb and sit out the
challenge: two an address, the rest told "FAILED busy", another address let in beside them; four addresses, the oldest
ended for a fifth; what both otatools print when they are the ones refused.  Bundles without a signature that holds:
16 MB less one byte at once and then silence, from two addresses (the two places in memory): a signed push takes the
slower one's place at once, that sender is told, and the other ends by the floor in ten seconds, not in the 8.7
minutes its burst bought before; a second bundle from the address that holds a place.  A signed bundle of megabytes on
its way at a limited rate while two other addresses keep opening idle connections: not ended for them (it was: the
oldest connection without a checked signature made room, and a bundle is that for all of its body).  A bundle's place
just taken, nothing read of it yet, against the next stranger's bundle: kept for its first two seconds.  A bundle
that was arriving, slowly, when a newcomer took its place: its address's next one has the two seconds all the same.
Four addresses that each send a bundle's request line and nothing more, again as they are ended: the owner's adb and
push get through (a connection whose place was taken counted as one for the ten seconds of its signature's read).
A bundle that arrived whole and whose signature is being checked (made slow: HASSMIC_OTA_CHECK_DELAY) keeps its place
against a newcomer and is installed."""
import os, re, signal, socket, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, OTA = 16973, 16974
O = [sys.executable, os.path.join(ROOT, "scripts/otatool.py")]
E = os.path.join(ROOT, "build/otatool-host")


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond: check.failed = True
check.failed = False


def conn(src):
    s = socket.socket(); s.bind((src, 0)); s.settimeout(5); s.connect(("127.0.0.1", OTA)); return s


def line(s, secs=5):
    """the Echo's next line, "" when it closed without one, None when nothing came in SECS"""
    s.settimeout(secs); b = b""
    try:
        while not b.endswith(b"\n"):
            c = s.recv(1)
            if not c: break
            b += c
    except socket.timeout:
        return None
    except OSError:
        pass
    return b.decode(errors="replace").strip()


def ask_adb(src):
    s = conn(src); s.sendall(b"HMOTA-ADB1\n"); return s, line(s)


def main():
    t = tempfile.mkdtemp(prefix="hm-otaport-"); state = os.path.join(t, "state"); os.mkdir(state); log = os.path.join(t, "log")
    run = lambda *a: subprocess.run(a, cwd=ROOT, capture_output=True, text=True, timeout=120)
    run(*O, "keygen", f"{t}/k.sec", f"{t}/k.pub")
    open(f"{t}/main.sh", "w").write("#!/bin/sh\necho main\n")
    run(*O, "pack", f"{t}/k.sec", "9.9.9+test", f"{t}/good.bundle", f"{t}/main.sh")
    open(f"{t}/blob", "wb").write(os.urandom(2500 << 10))
    run(*O, "pack", f"{t}/k.sec", "9.9.9+big", f"{t}/big.bundle", f"{t}/main.sh", f"{t}/blob")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_ADB_OPEN=f"{t}/adb-open", HASSMIC_UPDATE_PUB=f"{t}/k.pub", HASSMIC_SETTINGS=f"{t}/settings",
               HASSMIC_OTA_CHECK_DELAY=f"3000:{t}/check-slow")   # (while that file exists a bundle's signature takes 3 s longer to check)
    proc = subprocess.Popen([os.path.join(ROOT, "build/hassmic-host"), "-p", str(PORT), "-z", "0", "-o", str(OTA), "-L"], env=env, stderr=open(log, "w"))
    stop = threading.Event()

    def root():     # main.sh's two watchers: the installer's answer to a bundle, the firewall's to adb
        while not stop.is_set():
            if os.path.exists(f"{state}/ota/request"):
                os.remove(f"{state}/ota/request")
                v = run(E, "verify", f"{t}/k.pub", f"{state}/ota/bundle", f"{state}/ota/bundle.sig")
                open(f"{state}/ota/result.tmp", "w").write(("OK " if v.returncode == 0 else "FAILED ") + v.stdout.strip() + "\n")
                os.rename(f"{state}/ota/result.tmp", f"{state}/ota/result")
            if os.path.exists(f"{state}/adb-request"):
                os.remove(f"{state}/adb-request"); open(f"{t}/adb-open", "w").close()
            time.sleep(0.1)
    threading.Thread(target=root, daemon=True).start()
    try:
        for _ in range(200):
            try: socket.create_connection(("127.0.0.1", OTA), timeout=1).close(); break
            except OSError: time.sleep(0.1)
        time.sleep(1.2)                                  # (that look's own connection, closed without a line, is gone)

        # ---- connections before the key
        held = [ask_adb("127.0.0.2") for _ in range(4)]
        got = [l for _, l in held]
        check(all(re.fullmatch(r"NONCE [0-9a-f]{64}", l or "") for l in got[:2]) and all((l or "").startswith("FAILED busy") for l in got[2:]),
              f"four connections from one address that ask for adb and do not sign: two get a challenge, the others are told ({got[2]})")
        t0 = time.monotonic(); s3, l3 = ask_adb("127.0.0.3")
        check(re.fullmatch(r"NONCE [0-9a-f]{64}", l3 or "") and time.monotonic() - t0 < 1, "...and another address gets its challenge at once beside them")
        s3.close()
        open_ = lambda: os.path.exists(f"{t}/adb-open")
        r = run(*O, "adb", "127.0.0.1", str(OTA), f"{t}/k.sec")
        check(r.stdout.strip() == "OK adb over Wi-Fi open for 30 min" and open_(), f"...and the owner opens adb with the key meanwhile: {r.stdout.strip()}")
        os.remove(f"{t}/adb-open")
        for s, _ in held: s.close()
        time.sleep(0.5)
        # four addresses, one connection each: all that may wait before the key; a fifth ends the oldest
        held = [ask_adb(f"127.0.0.{i}") for i in (2, 3, 4, 5)]
        s6, l6 = ask_adb("127.0.0.6")
        check(all(re.fullmatch(r"NONCE [0-9a-f]{64}", l or "") for _, l in held) and re.fullmatch(r"NONCE [0-9a-f]{64}", l6 or ""),
              "four addresses that each hold a connection: a fifth address still gets its challenge")
        l0 = line(held[0][0], 3)
        check((l0 or "").startswith("FAILED busy: others connected") and line(held[1][0], 0.5) is None, f"...the oldest of the four was ended for it and told why, the others were not ({l0})")
        for s, _ in held + [(s6, 0)]: s.close()
        time.sleep(0.5)
        # the owner's own tools, refused: they say why
        mine = [ask_adb("127.0.0.1") for _ in range(2)]
        r1 = run(*O, "adb", "127.0.0.1", str(OTA), f"{t}/k.sec"); r2 = run(E, "adb", "127.0.0.1", str(OTA), f"{t}/k.sec")
        r3 = run(*O, "push", "127.0.0.1", str(OTA), f"{t}/good.bundle", f"{t}/good.bundle.sig"); r4 = run(E, "push", "127.0.0.1", str(OTA), f"{t}/good.bundle", f"{t}/good.bundle.sig")
        outs = [r.stdout.strip() for r in (r1, r2, r3, r4)]
        check(all(o.startswith("FAILED busy") for o in outs) and all(r.returncode == 1 for r in (r1, r2, r3, r4)) and not open_(),
              f"refused for two unsigned connections of its own address, each otatool prints the Echo's reason (adb and push, Python and C): {outs[0]}")
        for s, _ in mine: s.close()
        time.sleep(0.5)

        # ---- bundles nobody signed
        big = 16 << 20; junk = bytes(1 << 20)

        def burst(src):
            s = conn(src); s.settimeout(30)
            s.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64))
            for i in range(16): s.sendall(junk if i < 15 else junk[:-1])
            return s
        a = burst("127.0.0.2"); b = burst("127.0.0.3"); t0 = time.monotonic()
        time.sleep(2.5)                                 # (their bursts are over: neither has sent in the last two seconds)
        t1 = time.monotonic()
        r = run(*O, "push", "127.0.0.1", str(OTA), f"{t}/good.bundle", f"{t}/good.bundle.sig")
        check(r.stdout.strip() == "OK 9.9.9+test" and time.monotonic() - t1 < 5,
              f"two senders that burst 16 MB less a byte and fall silent hold both places: a signed push gets in at once ({r.stdout.strip()}, {time.monotonic() - t1:.1f} s)")
        la, lb = line(a, 14), line(b, 14); dt = time.monotonic() - t0
        took = [l for l in (la, lb) if l and "took this one's place" in l]; fell = [l for l in (la, lb) if l == "FAILED upload incomplete"]
        check(len(took) == 1 and len(fell) == 1 and dt < 14,
              f"...one of them was told its place was taken, the other ended by the floor after {dt:.0f} s of silence, not minutes ({la} / {lb})")
        a.close(); b.close(); time.sleep(0.3)
        # a place held from the owner's own address (a push of his left hanging): said, not waited for
        a = conn("127.0.0.1"); a.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64) + junk)
        time.sleep(0.3)
        r = run(*O, "push", "127.0.0.1", str(OTA), f"{t}/good.bundle", f"{t}/good.bundle.sig")
        check(r.stdout.strip().startswith("FAILED busy: an update from this address"), f"a second bundle from an address that holds a place: {r.stdout.strip()}")
        a.close(); time.sleep(0.5)
        r = run(*O, "push", "127.0.0.1", str(OTA), f"{t}/good.bundle", f"{t}/good.bundle.sig")
        check(r.stdout.strip() == "OK 9.9.9+test", f"...and once that one is gone the push goes through: {r.stdout.strip()}")
        time.sleep(0.5)

        # ---- a signed bundle on its way, and connections that come and sit
        bundle = open(f"{t}/big.bundle", "rb").read(); sig = open(f"{t}/big.bundle.sig", "rb").read()
        idle_stop = threading.Event(); opened = [0]

        def idlers():       # two addresses, about three connects a second each, every connection left idle for 0.6 s
            held = []
            while not idle_stop.is_set():
                for src in ("127.0.0.2", "127.0.0.3"):
                    try: held.append((time.monotonic(), conn(src))); opened[0] += 1
                    except OSError: pass
                    time.sleep(0.17)
                for h in [h for h in held if time.monotonic() - h[0] > 0.6]: h[1].close(); held.remove(h)
            for h in held: h[1].close()
        th = threading.Thread(target=idlers, daemon=True); th.start(); time.sleep(1.0)
        s = conn("127.0.0.1"); t0 = time.monotonic(); sent = 0; cut = None
        try:
            s.sendall(b"HMOTA-PUSH1 %d\n" % len(bundle) + sig)
            while sent < len(bundle): s.sendall(bundle[sent:sent + 65536]); sent += 65536; time.sleep(0.2)     # 320 kB/s
        except OSError as e: cut = e
        l = line(s, 70); dt = time.monotonic() - t0; idle_stop.set(); th.join(); s.close()
        check(cut is None and l == "OK 9.9.9+big" and "signature good" in open(log).read().split("9.9.9+test")[-1] and opened[0] > 20,
              f"a signed bundle of {len(bundle) >> 10} kB sent in {dt:.0f} s while two other addresses opened {opened[0]} idle connections: not ended for them ({l}{', cut: ' + str(cut) if cut else ''})")
        time.sleep(1.2)

        # ---- a place just taken against the next bundle
        feed_stop = threading.Event()
        a = conn("127.0.0.2"); a.settimeout(30); a.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64))

        def feed():         # 1 MB/s of junk: a place whose holder is fast
            try:
                for _ in range(60):
                    if feed_stop.is_set(): break
                    a.sendall(junk[:256 << 10]); time.sleep(0.25)
            except OSError: pass
        tf = threading.Thread(target=feed, daemon=True); tf.start(); time.sleep(2.6)       # (past its own first two seconds: judged by its speed)
        s = conn("127.0.0.1"); s.sendall(b"HMOTA-PUSH1 %d\n" % len(bundle) + sig)        # the owner's, its body still to come
        time.sleep(0.3)
        b = conn("127.0.0.3"); b.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64)); lb = line(b, 3)
        try: s.sendall(bundle)
        except OSError: pass                            # (its place taken and the connection ended: what the check below says)
        l = line(s, 70)
        check((lb or "").startswith("FAILED busy: other updates") and l == "OK 9.9.9+big",
              f"a bundle's place 0.3 s old with nothing read yet, the other place's holder fast: the next stranger's bundle does not take it ({lb}), and the push goes through ({l})")
        feed_stop.set(); tf.join(); a.close(); b.close(); s.close(); time.sleep(0.5)
        # the grace is one per address: a bundle from an address whose last one ended unsigned gives way at once
        b = conn("127.0.0.3"); b.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64)); time.sleep(0.2)
        a = conn("127.0.0.2"); a.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64)); time.sleep(0.2)
        r = run(*O, "push", "127.0.0.1", str(OTA), f"{t}/good.bundle", f"{t}/good.bundle.sig")
        check(r.stdout.strip() == "OK 9.9.9+test", f"...but two addresses whose last bundles ended unsigned get no such grace for their next: a signed push takes a place at once ({r.stdout.strip()})")
        a.close(); b.close(); time.sleep(0.5)

        # ---- a place taken while its bundle was arriving is not held against its address
        feed_stop = threading.Event()

        def feeder(sock, piece, pause):
            def run_():
                try:
                    while not feed_stop.is_set(): sock.sendall(junk[:piece]); time.sleep(pause)
                except OSError: pass
            th = threading.Thread(target=run_, daemon=True); th.start(); return th
        fast = conn("127.0.0.14"); fast.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64)); tf = feeder(fast, 256 << 10, 0.25)    # 1 MB/s: keeps its place
        slow = conn("127.0.0.12"); slow.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64)); ts = feeder(slow, 16 << 10, 0.2)      # 80 kB/s: arriving, under BODY_FAST
        time.sleep(2.6)
        n = conn("127.0.0.13"); n.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64)); ls = line(slow, 3)
        n.close(); time.sleep(0.4)                       # (the newcomer gone again: its place is free)
        slow2 = conn("127.0.0.12"); slow2.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64)); time.sleep(0.3)
        m = conn("127.0.0.15"); m.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64)); lm = line(m, 3); l2 = line(slow2, 0.3)
        check("took this one's place" in (ls or "") and (lm or "").startswith("FAILED busy: other updates") and l2 is None,
              f"a bundle coming in at 80 kB/s loses its place to a newcomer ({ls}); its address's next one still has its two seconds: the next newcomer does not take it ({lm})")
        feed_stop.set(); tf.join(); ts.join()
        for x in (fast, slow, slow2, n, m): x.close()
        time.sleep(1.5)

        # ---- a bundle that arrived whole, its signature being checked (3 s here; tenths of a second for megabytes
        # on an Echo): its place is not given to a newcomer.  It was: the place showed no speed any more, the
        # newcomer took it, and the bundle was handed to the installer all the same, from a connection that no
        # longer counted as a bundle's and could be ended for the next one
        feed_stop = threading.Event()
        fast = conn("127.0.0.34"); fast.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64)); tf = feeder(fast, 256 << 10, 0.25)    # the other place: fast, keeps it
        open(f"{t}/check-slow", "w").close()
        good = open(f"{t}/good.bundle", "rb").read(); gsig = open(f"{t}/good.bundle.sig", "rb").read()
        s = conn("127.0.0.1"); s.sendall(b"HMOTA-PUSH1 %d\n" % len(good) + gsig + good)
        time.sleep(2.4)                                  # (past the place's first two seconds, nothing coming any more: the slowest there is)
        n = conn("127.0.0.33"); n.sendall(b"HMOTA-PUSH1 %d\n" % big + bytes(64)); ln = line(n, 2)
        l = line(s, 70)
        check((ln or "").startswith("FAILED busy: other updates") and l == "OK 9.9.9+test",
              f"a bundle that is whole, its signature being checked: a newcomer's bundle does not take its place ({ln}), and it is installed and told so ({l})")
        os.remove(f"{t}/check-slow"); feed_stop.set(); tf.join()
        for x in (fast, s, n): x.close()
        time.sleep(1.5)

        # ---- addresses that open a bundle, send nothing more and come back when ended (a review, 2026-11-20: a
        # place taken over left its connection counted as a bundle on its way, four of them and nobody got in)
        sit_stop = threading.Event()

        def sitter(src):
            while not sit_stop.is_set():
                x = None
                try:
                    x = conn(src); x.settimeout(15); x.sendall(b"HMOTA-PUSH1 %d\n" % big)
                    while x.recv(4096): pass
                except OSError: pass
                finally:
                    if x: x.close()
                time.sleep(0.05)
        sitters = [threading.Thread(target=sitter, args=(f"127.0.0.{i}",), daemon=True) for i in (22, 23, 24, 25)]
        for th in sitters: th.start()
        time.sleep(3); adb = push = 0; said = set()
        for _ in range(6):
            r = run(*O, "adb", "127.0.0.1", str(OTA), f"{t}/k.sec"); adb += r.stdout.strip().startswith("OK"); said.add(r.stdout.strip())
            if open_(): os.remove(f"{t}/adb-open")
            r = run(*O, "push", "127.0.0.1", str(OTA), f"{t}/good.bundle", f"{t}/good.bundle.sig"); push += r.stdout.strip() == "OK 9.9.9+test"; said.add(r.stdout.strip())
            time.sleep(1)
        sit_stop.set()
        check(adb >= 5 and push >= 3, f"four addresses that each keep a bundle's request line standing and send nothing: the owner's adb by key gets through {adb} times of 6, a signed push {push} of 6 ({sorted(said)})")
        for th in sitters: th.join(20)
        check(proc.poll() is None, "hassmic still runs")
    finally:
        stop.set(); proc.send_signal(signal.SIGTERM)
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
    if check.failed:
        print(open(log).read()[-6000:]); print("FAILED"); return 1
    print("all good"); return 0


if __name__ == "__main__":
    sys.exit(main())
