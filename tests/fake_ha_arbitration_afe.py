#!/usr/bin/env python3
"""Wake word arbitration (tests/arbecho.py) with the front end's score read off the capture thread (issue #77): two
Echos in one network, the kitchen one with a stand-in front end (HASSMIC_FAKE_AFE, audio_file.c) that answers after
250 ms, then after 900 ms.  The capture goes on meanwhile (on an Echo Show 5 it stood still and lost what was said),
the claim carries the front end's score once it is in, the mic stream's after 500 ms without it, a reading left
behind ends the front end's utterance state again, and with arbitration off nothing is read."""
import asyncio, os
from arbecho import LEAD_IN, Echo, HA, check, until, start, run

key = os.urandom(32)
a, b = Echo("Echo Kitchen", 16961, 500, net=(2, key)), Echo("Echo Living Room", 16962, 900, net=(2, key))
a.env["HASSMIC_FAKE_AFE"] = "250:1000000:9999"       # 1000 * log10(1000001 / 10000) = 2000


async def main():
    ha = HA([a, b])
    await start((a, b), ha)
    ok = await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 10)
    check(ok, "started in one network")

    # the front end answers in time: its score goes, and the capture went on while it was read
    await asyncio.sleep(1)
    a.wake(); b.wake()
    await until(lambda: a.starts or b.starts, 3); await asyncio.sleep(1)
    check(len(a.starts) == 1 and not b.starts, f"the front end's 2000 beats 900 (the mic stream's 500 would not): {len(a.starts)} / {len(b.starts)}")
    check("score 2000 from the front end (500 from the mic stream)" in a.text(), "the claim waited for the front end's score")
    # sent from the ring: the 128 ms of SIGUSR1's lag, then what the capture took while the front end was read (250 ms,
    # 250 short of AFE_WAIT_MS: five 50 ms blocks, three at least on a busy runner), up to the next live block.  On the capture
    # thread the reading held all of it up, and the ring had only the lag and a block at most (2048 + 0..800 samples).
    # The request's 800 ms of silence comes first (main.c lead_in)
    ahead = sum(a.chunks[:a.chunks.index(1600)]) - LEAD_IN if 1600 in a.chunks else None
    check(ahead is not None and 4096 + 3 * 1600 <= ahead <= 4096 + 12 * 1600, f"the capture went on while the front end was read: {ahead} bytes ahead of the live stream")
    a.end_pipeline(); await asyncio.sleep(0.5)

    # arbitration off: no round, so the front end is not asked at all (it was before: a reading for nothing)
    await a.arbitrate(False)
    n, na = a.text().count("front end: voice energy"), len(a.starts)
    a.wake()
    ok = await until(lambda: len(a.starts) == na + 1, 3)
    check(ok and "arbitration off" in a.text() and a.text().count("front end: voice energy") == n, "arbitration off: answered without asking the front end")
    a.end_pipeline(); await asyncio.sleep(0.5)
    await a.arbitrate(True)
    ok = await until(lambda: b.st("arbitration_peers") == 1, 5)
    check(ok, "arbitration on again")

    # the front end too slow: the claim goes with the mic stream's score, and the late reading is ended again
    try: await a.cli.disconnect()
    except Exception: pass
    a.proc.terminate(); a.proc.wait()
    a.env["HASSMIC_FAKE_AFE"] = "900:1000000:9999"
    a.start(); await asyncio.sleep(0.5); await a.connect(ha)
    ok = await until(lambda: a.st("arbitration_peers") == 1, 10)
    check(ok, "the kitchen Echo back in the network")
    await asyncio.sleep(1)
    na, nb, at = len(a.starts), len(b.starts), len(a.text())     # the log goes on from the first run
    a.wake(); b.wake()
    await until(lambda: "front end: stream stopped" in a.text()[at:], 3)
    t = a.text()[at:]
    check("no score from the front end within 500 ms" in t and len(a.starts) == na and len(b.starts) == nb + 1,
          f"no score within 500 ms: claimed with 500, the living room's 900 answers: {len(a.starts) - na} / {len(b.starts) - nb}")
    check(0 <= t.find("another Echo answers") < t.find("front end: stream stopped"),
          "the reading that came after the round lost: the front end's utterance state ended again")
    b.end_pipeline(); await asyncio.sleep(0.5)

run(main, (a, b))
