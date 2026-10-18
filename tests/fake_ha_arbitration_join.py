#!/usr/bin/env python3
"""Wake word arbitration (tests/arbecho.py): a newcomer joins the kitchen Echo's network through the tags (no
permission), through its own action where HA has no tag integration, and again after a crash that took its state file."""
import asyncio, os, re
from arbecho import Echo, HA, check, until, start, run

a, b = Echo("Echo Kitchen", 16961, 500, net=2), Echo("Echo Living Room", 16962, 900)


async def main():
    ha = HA([a, b])
    a.allowed = b.allowed = False                   # the tags alone
    await start((a, b), ha, login=False)
    ok = await until(lambda: b.net() == a.net(), 15)
    check(ok and "(key from echo-kitchen, confirmed by Home Assistant)" in b.text() and not ha.calls and not ha.refused,
          "tags: joined without permission")
    await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 10)

    # permission and no tags (HA without the tag integration): the action
    ha.tags_on = False; ha.tags.clear(); a.allowed = True; calls = len(ha.calls)
    await b.reset()
    ok = await until(lambda: b.net() == a.net() and a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 20)
    check(ok and any(c[1] == "esphome.echo_living_room_arbitration_key" for c in ha.calls[calls:]),
          f"allowed: the next one joined through the action ({[c[1] for c in ha.calls[calls:]]}, same network {b.net() == a.net()}, "
          f"peers {a.st('arbitration_peers')}/{b.st('arbitration_peers')})")

    # A hard crash that takes state/arbitration with it (2026-10-15, a Dot 2): back with no counter, it takes a new
    # identity and K again through the new key's tag.  The other Echo takes its packets at once, rather than drop
    # them as replays of the old id until it restarts itself
    ha.tags_on = True; took = lambda: len(re.findall(r"joined network|moved to the older network", b.text())); joined = took()
    await b.cli.disconnect(); b.proc.kill(); b.proc.wait(); b.states = {}
    old = b.pub(); os.remove(os.path.join(b.state, "arbitration"))
    b.start(); await asyncio.sleep(0.5); await b.connect(ha)
    check(b.pub() != old, "crashed without its state file: a new identity")
    ok = await until(lambda: b.net() == a.net() and b.st("arbitration_peers") == 1 and took() > joined, 20)
    check(ok, f"crashed without its state file: took the key again through its tag (peers {b.st('arbitration_peers')})")
    await asyncio.sleep(1)
    na, nb = len(a.starts), len(b.starts); a.wake(); b.wake()
    await until(lambda: len(b.starts) > nb, 3); await asyncio.sleep(1)
    check(len(b.starts) == nb + 1 and len(a.starts) == na,
          f"both heard it: the crashed one, better (900 over 500), answers alone; its claim counted: {len(a.starts) - na} / {len(b.starts) - nb}")
    b.end_pipeline()

run(main, (a, b))
