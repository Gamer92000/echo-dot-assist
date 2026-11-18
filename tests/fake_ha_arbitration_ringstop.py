#!/usr/bin/env python3
""""<wake word>, stop" heard by one Echo ends a timer ringing on another (issue #161, ringstop.c; tests/arbecho.py):
two Echos in one network, the hall's timer rings, only the kitchen Echo hears the talker.  Off (the default) the hall
says nothing of its ring and rings on.  On: it tells every member "ring on" (three copies, then every 2 s), the kitchen
Echo's "stop" (Pryon's keyword once its run listens, during the round, or a bare "stop" transcript) is forwarded and the
hall stops; the kitchen's run is dropped, none starts.  A "stop" with a ring of its own ends only that; the setting
switched off while ringing says "ring off"; a "ring on" that is not followed up runs out; late copies of a stop end no
later ring; a "stop" from another member is ignored with the setting off; the ringing Echo's own run, opened by the
wake word it heard without the "stop", is dropped.  The kitchen's window is fixed at 1 s: a "stop" during the round
lands in it however slow the PC (the learnt one is 500 ms here)."""
import asyncio, hashlib, base64, os, socket, struct, time
from aioesphomeapi import VoiceAssistantEventType as Ev, VoiceAssistantTimerEventType as Tm
from arbecho import ARB, BCAST, Echo, HA, check, until, start, run

key = os.urandom(32)
a, b = Echo("Echo Kitchen", 16981, 500, net=(2, key)), Echo("Echo Hall", 16982, 900, net=(2, key))
for e in (a, b): e.env["HASSMIC_ARB_RIVALS"] = "0"     # the hall never hears it here: waited for all the same (the full window)
HOLD = 6.5                                          # ringstop.c HOLD_MS


async def main():
    ha = HA([a, b])
    await start((a, b), ha)
    check(await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 10), "both in one network")
    a.page.set(arbitration_window_auto="off", arbitration_window=1000)
    sn = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)                 # listens in on the messages
    sn.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); sn.bind(("", ARB)); sn.setblocking(False)
    bpk, apk = b.pub()[:8], a.pub()[:8]
    msgs, beacons = [], []                          # (when, from "a"/"b", payload, to all); when the hall's beacons came

    async def listen(secs):
        end = time.monotonic() + secs
        while time.monotonic() < end:
            try: p = sn.recv(512)
            except BlockingIOError: await asyncio.sleep(0.005); continue
            if p[4] == 1 and p[5:13] == bpk: beacons.append(time.monotonic())     # "HMA1", type 1, public key, ...
            # "HMA1", type 7, net, from, to, counter, length, payload, MAC
            if p[4] == 7 and p[13:21] in (apk, bpk) and p[38] in (0x41, 0x42):
                msgs.append((time.monotonic(), "a" if p[13:21] == apk else "b", p[38:38 + p[37]], p[21:29] == b"\xff" * 8))

    n_ring = [0]
    def ring(e):
        n_ring[0] += 1
        e.cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, f"t{n_ring[0]}", "tea", 60, 0, False)
    rings = lambda e: e.text().count("alarm: tea rings")
    off = lambda e: e.text().count("alarm: off")
    aborts = lambda e, since: [x for _, x in e.stops[since:]]

    async def woke(e):                              # a wake word heard by this Echo alone: its run listens
        n = len(e.starts); e.wake(); check(await until(lambda: len(e.starts) == n + 1, 3), f"  {e.name}: the wake word starts a run")

    async def in_round(e):                          # "<wake word>, stop" with the "stop" during the round
        n = e.text().count("arbitration: heard it,")
        e.wake(); check(await until(lambda: e.text().count("arbitration: heard it,") > n, 1), "  the round started")
        e.stop(); await asyncio.sleep(1.5)

    # off (the default): the hall says nothing of its ring, the kitchen's "stop" stays the kitchen's
    nr = rings(b); ring(b); check(await until(lambda: rings(b) > nr, 2), "the hall's timer rings")
    beacons.clear(); await listen(4.5)       # a beacon every 2-4 s here (arbecho SCALE)
    check(beacons and not [m for m in msgs if m[1] == "b"], f"off: the hall says nothing of its ring ({len(beacons)} beacons of it heard meanwhile): {msgs}")
    ob, sa = off(b), len(a.stops)
    await woke(a); a.stop(); await listen(1)
    check(off(b) == ob and "told to stop" not in a.text(), "off: the kitchen's \"<wake word>, stop\" leaves the hall ringing")
    check(not aborts(a, sa), "  and its run goes on to Home Assistant, as before")
    a.end_pipeline()
    b.page.set(timer_stop_from_others="on"); t0 = time.monotonic()

    # on: the ring told at once (the hall rings already), three copies, then every 2 s
    msgs.clear(); await listen(5.2)
    on = [t - t0 for t, w, p, _ in msgs if w == "b" and p == b"\x41\x01"]
    check(len(on) >= 5 and on[2] - on[0] < 0.45 and all(1.8 < y - x < 2.3 for x, y in zip(on[2:], on[3:])),
          f"on: \"ring on\" three times at once, then every 2 s: {[round(x, 2) for x in on]}")
    check(all(m[3] for m in msgs), "  to every member (to ff..ff)")
    check("ringstop: echo-hall rings, a stop here ends it" in a.text(), "  the kitchen Echo knows")

    # the kitchen's run listens, then "stop": forwarded, the hall stops and says "ring off", the kitchen's run dropped
    sa, na = len(a.stops), len(a.starts)
    await woke(a); msgs.clear(); a.stop(); await listen(1.5)
    check("stop: nothing here, echo-hall rings: told to stop" in a.text() and "stop: pipeline dropped" in a.text(),
          "the kitchen's \"<wake word>, stop\": the hall told, the kitchen's run dropped")
    check(aborts(a, sa) == [True], f"  cancelled (start=false): {aborts(a, sa)}")
    stops = [p for _, w, p, _ in msgs if w == "a" and p[0] == 0x42]
    check(len(stops) == 3 and len(set(stops)) == 1, f"  the stop three times, one id: {[p.hex() for p in stops]}")
    check("alarm: off (stop heard by echo-kitchen)" in b.text() and b.text().count("alarm: off") == ob + 1, "  the hall's ring ends, once")
    hall = [p for _, w, p, _ in msgs if w == "b"]           # a "ring on" of the 2 s beat may come before the "off"s
    check(hall.count(b"\x41\x00") == 3 and b"\x41\x01" not in hall[hall.index(b"\x41\x00"):] if b"\x41\x00" in hall else False,
          f"  and it says \"ring off\" three times, no \"on\" after: {hall}")
    check("ringstop: echo-hall no longer rings" in a.text(), "  which the kitchen hears")

    # nothing rings any more: the kitchen's next "stop" goes nowhere
    await asyncio.sleep(1); msgs.clear(); sa = len(a.stops)
    await woke(a); a.stop(); await listen(1)
    check(not [m for m in msgs if m[1] == "a"] and a.text().count("told to stop") == 1, "nothing rings: no stop sent")
    a.end_pipeline(); await asyncio.sleep(0.5)

    # "stop" during the round (the hall did not hear it: the kitchen's full window): the hall stops, nothing starts here
    nr = rings(b); ring(b); await until(lambda: rings(b) > nr, 2)
    await until(lambda: a.text().count("ringstop: echo-hall rings") >= 2, 2)
    ob, na, sa = off(b), len(a.starts), len(a.stops)
    await in_round(a)
    check(off(b) == ob + 1 and len(a.starts) == na and "stop: came during the round, nothing started" in a.text(),
          f"\"stop\" during the round: the hall stops, no run starts here ({len(a.starts) - na} started)")

    # a bare "stop" transcript (an engine without the keyword, or Pryon missed it): the same
    nr = rings(b); ring(b); await until(lambda: rings(b) > nr, 2); await asyncio.sleep(0.5)
    ob, sa = off(b), len(a.stops)
    await woke(a); a.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "Stop."})
    await until(lambda: off(b) > ob, 2)
    check(off(b) == ob + 1 and "said stop: echo-hall rings: told to stop" in a.text() and aborts(a, sa) == [True],
          f"a \"stop\" transcript: the hall stops, the kitchen's run cancelled ({aborts(a, sa)})")
    await asyncio.sleep(1)

    # the hall heard the wake word too (it wins: a ring claims first) but not the "stop", which only the kitchen heard:
    # the hall's ring ends and the run its wake word opened over the ring is dropped or never starts
    nr = rings(b); ring(b); await until(lambda: rings(b) > nr, 2); await asyncio.sleep(0.5)
    ob, nb, sb, nt = off(b), len(b.starts), len(b.stops), len(b.text())
    a.wake(); b.wake(); await asyncio.sleep(0.2); a.stop(); await asyncio.sleep(2)
    after = b.text()[nt:]
    check(off(b) == ob + 1 and "alarm: off (stop heard by echo-kitchen)" in after, "the hall heard the wake word only: the kitchen's stop ends its ring")
    check(len(b.starts) - nb == len(b.stops) - sb and ("stop: pipeline dropped" in after or "stop: came during the round, nothing started" in after),
          f"  and the hall's own run is dropped or never starts ({len(b.starts) - nb} started, {len(b.stops) - sb} ended)")
    await asyncio.sleep(1)

    # both ring: the kitchen's "stop" ends its own ring only
    nr, nb = rings(a), rings(b); ring(a); ring(b); await until(lambda: rings(a) > nr and rings(b) > nb, 2); await asyncio.sleep(0.5)
    oa, ob, told = off(a), off(b), a.text().count("told to stop")
    await woke(a); a.stop(); await asyncio.sleep(1.5)
    check(off(a) == oa + 1 and off(b) == ob and a.text().count("told to stop") == told, "both ring: the kitchen's \"stop\" ends its own ring only")

    # the setting off while the hall rings: "ring off", and the kitchen forwards no more
    await listen(0.3); msgs.clear(); b.page.set(timer_stop_from_others="off"); await listen(1)    # what came before read first
    hall = [p for _, w, p, _ in msgs if w == "b"]
    check(hall.count(b"\x41\x00") == 3 and b"\x41\x01" not in hall[hall.index(b"\x41\x00"):] if b"\x41\x00" in hall else False,
          f"switched off while ringing: \"ring off\" three times, no \"on\" after: {hall}")
    await woke(a); a.stop(); await asyncio.sleep(1)
    check(a.text().count("told to stop") == told and off(b) == ob, "  the kitchen's \"stop\" forwards nothing, the hall rings on")
    a.end_pipeline(); b.cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_CANCELLED, f"t{n_ring[0]}", "tea", 60, 0, False)

    # a member played here with the network's key: its "ring on" runs out; late copies of a stop end nothing; off, ignored
    nid, k64 = a.net().split(); nk = base64.b64decode(k64); nb8 = struct.pack("<Q", int(nid, 16))
    mac = hashlib.blake2b(b"hassmic arbitration mac", key=nk, digest_size=32).digest()
    ctr = [1 << 40]
    def signed(p): return p + hashlib.blake2b(p, key=mac, digest_size=16).digest()
    def fctr(): ctr[0] += 1; return struct.pack("<Q", ctr[0])
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    send = lambda p: tx.sendto(p, (BCAST, ARB))
    h = os.urandom(32); name = b"echo-office"
    send(signed(b"HMA1\x01" + h + nb8 + bytes([len(name)]) + name + fctr())); await asyncio.sleep(0.5)
    def msg(payload): send(signed(b"HMA1\x07" + nb8 + h[:8] + b"\xff" * 8 + fctr() + bytes([len(payload)]) + payload))
    msg(b"\x41\x01"); await asyncio.sleep(0.3)
    check("ringstop: echo-office rings, a stop here ends it" in a.text(), "a member's \"ring on\" taken")
    await woke(a); a.stop(); await asyncio.sleep(1)                 # the control: within HOLD a stop goes to it
    check("stop: nothing here, echo-office rings: told to stop" in a.text(), "  a stop within its hold is forwarded to it")
    await asyncio.sleep(0.5); msg(b"\x41\x01"); t1 = time.monotonic()
    await asyncio.sleep(HOLD - 0.3 + 0.5); told = a.text().count("told to stop")
    await woke(a); a.stop(); await asyncio.sleep(1)
    check(a.text().count("told to stop") == told, f"  nothing more from it for {time.monotonic() - t1:.1f} s: no longer taken as ringing")
    a.end_pipeline(); await asyncio.sleep(0.5)

    b.page.set(timer_stop_from_others="on"); nr = rings(b); ring(b); await until(lambda: rings(b) > nr, 2); await asyncio.sleep(0.5)
    ob = off(b)
    for _ in range(3): msg(b"\x42\x01\x02\x03\x04")
    await until(lambda: off(b) > ob, 1)
    check("alarm: off (stop heard by echo-office)" in b.text(), "a member's stop ends the hall's ring")
    nr = rings(b); ring(b); await until(lambda: rings(b) > nr, 2); await asyncio.sleep(0.3)
    msg(b"\x42\x01\x02\x03\x04"); await asyncio.sleep(0.5)
    check(off(b) == ob + 1, "  a late copy of it (same id) ends no new ring")
    msg(b"\x42\x05\x06\x07\x08"); await until(lambda: off(b) > ob + 1, 1)
    check(off(b) == ob + 2, "  a new stop (another id) does")
    b.page.set(timer_stop_from_others="off"); nr = rings(b); ring(b); await until(lambda: rings(b) > nr, 2)
    msg(b"\x42\x09\x0a\x0b\x0c"); await asyncio.sleep(0.5)
    check(off(b) == ob + 2, "  off: a stop from a member is ignored")
    sn.close()

run(main, (a, b))
