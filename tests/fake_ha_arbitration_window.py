#!/usr/bin/env python3
"""Wake word arbitration (tests/arbecho.py): the window, learned (issue #27).  The kitchen Echo in a network with a third
member behind a repeater, played here with the network's key, whose claims reach it 300-360 ms after its own: what it
learns, the log, the fixed window instead, a restart."""
import asyncio, base64, hashlib, os, re, socket, struct, time
from arbecho import ARB, BCAST, Echo, HA, Browser, check, until, start, run

a = Echo("Echo Kitchen", 16961, 500, net=2)


async def main():
    ha = HA([a])
    await start((a,), ha)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    send = lambda p: tx.sendto(p, (BCAST, ARB))
    node = lambda s: bytes([len(s)]) + s.encode()
    # It speaks the protocol with the network's key, as a real Echo would
    nid, k64 = a.net().split(); key = base64.b64decode(k64); nb8 = struct.pack("<Q", int(nid, 16))
    mac = hashlib.blake2b(b"hassmic arbitration mac", key=key, digest_size=32).digest()
    fpub = os.urandom(32); fid = fpub[:8]; ctr = [1 << 40]
    def signed(p): return p + hashlib.blake2b(p, key=mac, digest_size=16).digest()
    def fctr(): ctr[0] += 1; return struct.pack("<Q", ctr[0])
    def fbeacon(flags=b""): send(signed(b"HMA1\x01" + fpub + nb8 + node("echo-hallway") + fctr() + flags))
    kw = hashlib.blake2b(b"alexa", digest_size=8).digest()
    def fclaim(score=99999): send(signed(b"HMA1\x04" + nb8 + fid + fctr() + kw + struct.pack("<iBB", score, 0, 0)))
    sn = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sn.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); sn.bind(("", ARB)); sn.setblocking(False)
    apk = a.pub()[:8]
    async def a_claimed(secs=2):                    # the kitchen Echo's own claim (not its "answers"), when it went out
        end = time.monotonic() + secs
        while time.monotonic() < end:
            try: p = sn.recv(512)
            except BlockingIOError: await asyncio.sleep(0.002); continue
            if p[4] == 4 and p[13:21] == apk and p[-17] == 0: return time.monotonic()
        return None
    fbeacon(); await asyncio.sleep(0.5)
    arb = a.page.state()["arbitration"]
    check(any(m["node"] == "echo-hallway" for m in arb["members"]) and arb["window"]["learning"] and arb["window"]["ms"] == 500,
          f"a new member, nothing measured from it yet: the wait while learning, 500 ms: {arb['window']}")
    delays = [300 + 5 * i for i in range(12)]
    for d in delays:                                # it heard it better, its claim late: the kitchen Echo stays quiet
        while True:
            try: sn.recv(512)
            except BlockingIOError: break
        a.wake(); t = await a_claimed()
        if t: await asyncio.sleep(max(0, t + d / 1000 - time.monotonic())); fclaim()
        # past LOOKBACK_MS (1 s) after the late claim, or the next round would count it as one that came before its own
        await asyncio.sleep(1.2)
    arb = a.page.state()["arbitration"]; w = arb["window"]
    hall = next((m for m in arb["members"] if m["node"] == "echo-hallway"), {})
    check(not w["learning"] and w["auto"] and 355 <= (hall.get("late_ms") or 0) <= 400 and hall["late_ms"] + 50 <= w["ms"] <= hall["late_ms"] + 60,
          f"learned from its late claims: waits its 95th percentile plus 50 ms: {w}, hallway {hall.get('late_ms')} ms")
    lines = re.findall(r"arbitration: %s heard it, score 99999, \+(\d+) ms" % fid[:4].hex(), a.text())
    check(len(lines) == len(delays) and all(abs(int(x) - d) < 40 for x, d in zip(lines, delays)),
          f"each claim's lateness in the log, as it was sent: {lines}")
    check(re.search(r"arbitration: window learned: \d+ ms \(95 % of the last \d+ claims came within \d+ ms\)", a.text())
          and "another Echo answers (after" in a.text() and "a better claim in" in a.text(), "the log says what it learned, and why each round ended")
    await asyncio.sleep(0.5)                        # on disk when the last round's sampling (1.5 s) closes
    stored = lambda: open(os.path.join(a.state, "arbitration")).read()
    saved = lambda cond: until(lambda: cond(stored()), 8)     # written at most every 60 s, 6 s at this pace
    check(await saved(lambda t: t.count("delay " + fid.hex()) == len(delays)), "the samples kept in state/arbitration")
    # the hallway Echo did not hear it: the kitchen Echo waits the learned window, then answers
    n = len(a.starts); a.wake(); t = await a_claimed()
    await until(lambda: len(a.starts) > n, 3)
    took = a.starts[-1] - t if t and len(a.starts) > n else -1
    check(w["ms"] / 1000 <= took <= w["ms"] / 1000 + 0.15, f"nobody else heard it: answered after the learned {w['ms']} ms: {took:.3f} s")
    a.end_pipeline(); await asyncio.sleep(0.5)
    # learning off: the fixed window, too short for this network; the late claim comes after the decision
    a.page.set(arbitration_window_auto="off", arbitration_window=200)
    n = len(a.starts); a.wake(); t = await a_claimed()
    if t: await asyncio.sleep(max(0, t + 0.3 - time.monotonic())); fclaim()
    await asyncio.sleep(1.7)
    check(len(a.starts) == n + 1 and "waits up to 200 ms for 1 of 1 (set)" in a.text() and ", after the decision" in a.text()
          and a.page.state()["arbitration"]["window"]["ms"] == 200, "learning off: 200 ms set, the late claim missed (both would answer)")
    a.end_pipeline(); a.page.set(arbitration_window_auto="on"); await asyncio.sleep(0.5)
    # a restart keeps what was learned (once on disk); it counts once the member is heard from again
    await saved(lambda t: t.count("delay " + fid.hex()) == len(delays) + 1)
    await a.cli.disconnect(); a.proc.terminate(); a.proc.wait(); a.start(); await asyncio.sleep(0.5); await a.connect(ha)
    a.page = Browser(a.web); a.page.login_with_button(a.proc)
    fbeacon(); await asyncio.sleep(1)
    w2 = a.page.state()["arbitration"]["window"]
    check(not w2["learning"] and w2["ms"] == w["ms"], f"after a restart: still learned, {w2['ms']} ms")
    fbeacon(b"\x01"); await asyncio.sleep(0.5)          # the hallway Echo switches arbitration off: nobody waits for it
    check(a.page.state()["arbitration"]["window"]["samples"] < len(delays), "a member out of rounds: its samples no longer count")
    # Rivals: a fourth Echo in the attic, nothing known of it yet (the hallway stays out of rounds)
    tpub = os.urandom(32); tid = tpub[:8]
    def tbeacon(): send(signed(b"HMA1\x01" + tpub + nb8 + node("echo-attic") + fctr()))
    def tclaim(score): send(signed(b"HMA1\x04" + nb8 + tid + fctr() + kw + struct.pack("<iBB", score, 0, 0)))
    async def round_(claim=None, late=0.3):         # one wake word; the attic's claim (a score) that late; how long it took
        tbeacon()                                   # its beacons go on (a member counts 30 s here)
        n = len(a.starts); a.wake(); t = await a_claimed()
        if t and claim is not None: await asyncio.sleep(max(0, t + late - time.monotonic())); tclaim(claim)
        await until(lambda: len(a.starts) > n, 3)
        took = a.starts[-1] - t if t and len(a.starts) > n else -1
        a.end_pipeline(); await asyncio.sleep(1.6)  # past MEASURE_MS: the round is kept
        return took
    def decided(): return re.findall(r"arbitration: (this|another) Echo answers \(after \d+ ms: ([^)]*)\)", a.text())[-1]
    # it hears the kitchen's wake words faintly (100 against 500): the prior has it waited for twice, then not
    took = [await round_(100) for _ in range(3)]
    check(0.25 <= took[0] <= 0.4 and 0.25 <= took[1] <= 0.4 and 0 <= took[2] <= 0.12,
          f"waited for while little known (its claim ends the wait), then not: {took}")
    check("waits for nobody (no other Echo came close at this score)" in a.text(), "the log says why")
    check(await saved(lambda t: f"obs {tid.hex()} 500:-400 500:-400 500:-400" in t), "kept in state/arbitration, a line per member")
    took = await round_()
    check(0 <= took <= 0.12, f"it did not hear the next one: answered with the next block: {took:.3f} s")
    # its door opened: a close call (late, so the kitchen answered: the wrong Echo once), and the next round waits
    took = [await round_(99999), await round_()]
    check(0 <= took[0] <= 0.12 and took[1] >= 0.3 and decided() == ("this", "the window's end"),
          f"one close call at 500 and rounds there wait for it again: {took}")
    attic = next((m for m in a.page.state()["arbitration"]["members"] if m["node"] == "echo-attic"), {})
    check(attic.get("rounds") == 6 and attic.get("close") == 1, f"the page: rounds kept, close calls: {attic}")
    # both answered: the attic, too, waited for nobody and sent its "answers" after ours; kept as a close call
    took = []                                       # far quieter claims first, until it is not waited for again
    while not took or took[-1] > 0.12 and len(took) < 30: took.append(await round_(-9000))
    def tanswers(score): send(signed(b"HMA1\x04" + nb8 + tid + fctr() + kw + struct.pack("<iBB", score, 0, 1)))
    n = len(a.starts); a.wake(); t = await a_claimed()
    if t: await asyncio.sleep(max(0, t + 0.2 - time.monotonic())); tclaim(100); tanswers(100)
    await until(lambda: len(a.starts) > n, 3); a.end_pipeline(); await asyncio.sleep(1.6)
    attic = next((m for m in a.page.state()["arbitration"]["members"] if m["node"] == "echo-attic"), {})
    check(took[-1] <= 0.12 and attic.get("close") == 2, f"both answered (its claim 4 dB under ours): a close call: {took}, {attic}")
    check(await saved(lambda t: f"500:-9500 500:0" in t.split("obs " + tid.hex())[1].split("\n")[0]), "kept as 0")
    # the kitchen hears it far louder (1500, 10 dB more): no round near that score, the prior has it wait, the attic wins
    a.env["HASSMIC_TEST_SCORE"] = "1500"
    await a.cli.disconnect(); a.proc.terminate(); a.proc.wait(); a.start(); await asyncio.sleep(0.5); await a.connect(ha)
    a.page = Browser(a.web); a.page.login_with_button(a.proc); tbeacon(); await asyncio.sleep(0.5)
    took = await round_(99999)
    check(took == -1 and decided() == ("another", "a better claim in"), f"an unseen score: waited for, and it won: {took}, {decided()}")
    sn.close()

run(main, (a,))
