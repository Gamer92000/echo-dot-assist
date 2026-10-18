#!/usr/bin/env python3
"""Wake word arbitration (tests/arbecho.py) in Kiosk Satellite mode: two Echos of one network switch to Kiosk
Satellite's protocol, their JSON claims on its port (KIOSK stands in for 2330), against each other and a fake kiosk;
malformed claims, a flood, the loudness offset, and back to our own mode."""
import asyncio, json, os, re, socket, time
from arbecho import KIOSK, BCAST, Echo, HA, check, until, start, run

key = os.urandom(32)
a, b = Echo("Echo Kitchen", 16961, 500, net=(2, key)), Echo("Echo Living Room", 16962, 900, net=(2, key))


async def main():
    ha = HA([a, b])
    await start((a, b), ha)
    ok = await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 10)
    check(ok, "started in one network: each counts the other")

    # Kiosk Satellite mode: their claims on their port, loudness only, a kiosk takes part
    kio = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    kio.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); kio.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    kio.bind(("", KIOSK)); kio.setblocking(False)
    def heard():
        out = []
        while True:
            try: out.append(kio.recv(1024))
            except BlockingIOError: return out
    def kiosk(e, p="alexa", kid="00k10sk", **extra):
        d = dict(ks="wake", v=1, id=kid, n=int.from_bytes(os.urandom(4), "little") & 0x3fffffff, p=p, e=e); d.update(extra)
        for _ in range(3): kio.sendto(json.dumps(d).encode(), (BCAST, KIOSK))
    for e in (a, b): e.page.set(arbitration_mode="kiosk")
    ok = await until(lambda: all(e.page.state()["arbitration"]["kiosk_port"] for e in (a, b)), 3)
    arb = a.page.state()["arbitration"]
    check(ok and arb["mode"] == "kiosk" and [m["mode"] for m in arb["members"]] == ["kiosk"]
          and "arbitration_mode=kiosk" in open(os.path.join(a.state, "config")).read().split(),
          "Kiosk Satellite mode on both: port open, the other Echo's beacon says so, in state/config for the firewall")
    await asyncio.sleep(0.5); heard()
    na, nb = len(a.starts), len(b.starts)
    a.wake(); b.wake(); t0 = time.monotonic()
    await asyncio.sleep(2)
    check(len(b.starts) == nb + 1 and len(a.starts) == na, f"the louder one (9 dB over 5) answers alone: {len(a.starts) - na} / {len(b.starts) - nb}")
    check(b.starts and 0.38 < b.starts[-1] - t0 < 1.0, f"after the 400 ms window: {b.starts[-1] - t0 if b.starts else -1:.2f} s")
    claims = [json.loads(x) for x in heard()]
    by = {}
    for c in claims: by.setdefault(c["id"], []).append(c)
    es = sorted(c[0]["e"] for c in by.values())
    check(len(by) == 2 and all(len(l) == 3 and len({c["n"] for c in l}) == 1 for l in by.values()) and es == [5.0, 9.0]
          and all(set(c) == {"ks", "v", "id", "n", "p", "e"} and c["ks"] == "wake" and c["v"] == 1 and c["p"] == "alexa"
                  and re.fullmatch(r"[0-9a-f]{16}", c["id"]) and 0 <= c["n"] < 1 << 30 for c in claims),
          f"their wire format: three copies each, phrase \"alexa\", e in dB: {claims[:2]}")
    b.end_pipeline(); await asyncio.sleep(0.5)

    na, nb = len(a.starts), len(b.starts)
    a.wake(); b.wake(); kiosk(95.0)
    await asyncio.sleep(1.5)
    check(len(a.starts) == na and len(b.starts) == nb and "kiosk claim from 00k10sk: \"alexa\", 95.0 dB" in b.text(),
          "a kiosk that heard it louder: both Echos stay quiet")
    b.wake(); kiosk(99.0, p="Hey  Jarvis")
    await asyncio.sleep(1.5)
    check(len(b.starts) == nb + 1, "a kiosk claiming another phrase: no competition")
    b.end_pipeline(); await asyncio.sleep(0.5)
    nb = len(b.starts)
    b.wake()
    for bad in (dict(e="99"), dict(e=99, v=2), dict(e=99, ks="sleep"), dict(e=99, kid="x" * 65), dict(e=99, n=1.5), dict(e=99, extra={"x": 1})):
        kid = bad.pop("kid", "00bad"); e = bad.pop("e"); kiosk(e, kid=kid, **bad)
    kio.sendto(b'{"ks":"wake","v":1,"id":"00bad","p":"alexa","e":99}', (BCAST, KIOSK))      # no n
    await asyncio.sleep(1.5)
    check(len(b.starts) == nb + 1 and "00bad" not in b.text(), "malformed claims (wrong types, version, kind, long id, missing field): ignored")
    b.end_pipeline(); await asyncio.sleep(0.5)

    # a flood of claims (anyone on the LAN can send them): one log line a second, the rest counted
    before = b.text().count("kiosk claim from 00flood")
    for i in range(200): kiosk(1.0, kid=f"00flood{i}")
    await asyncio.sleep(1.2); kiosk(1.0, kid="00flood-last"); await asyncio.sleep(0.3)
    lines = b.text().count("kiosk claim from 00flood") - before
    check(1 <= lines <= 3 and "more since the last one logged" in b.text(), f"200 claims at once: {lines} log lines, the rest counted")

    # no preference for the Echo in a conversation: the louder one takes the next wake word
    na, nb = len(a.starts), len(b.starts)
    a.wake(); await until(lambda: len(a.starts) == na + 1, 3)
    a.wake(); b.wake(); await asyncio.sleep(1.5)
    check(len(a.starts) == na + 1 and len(b.starts) == nb + 1, "in a conversation: no priority, the louder Echo answers")
    a.end_pipeline(); b.end_pipeline(); await asyncio.sleep(0.5)
    check(a.page.state()["arbitration"]["kiosk_heard_s"] is not None, "settings page: a kiosk's claim was heard")

    # the owner's loudness offset: 9 dB - 6 = 3 dB on the wire, under the other Echo's 5
    b.page.set(arbitration_offset=-6); heard()
    na, nb = len(a.starts), len(b.starts)
    a.wake(); b.wake(); await asyncio.sleep(1.5)
    es = sorted({json.loads(x)["e"] for x in heard()})
    check(len(a.starts) == na + 1 and len(b.starts) == nb and es == [3.0, 5.0], f"offset -6 dB on the louder Echo: the other one answers, claims {es}")
    a.end_pipeline(); b.page.set(arbitration_offset=0); await asyncio.sleep(0.5)

    for e in (a, b): e.page.set(arbitration_mode="hassmic")
    ok = await until(lambda: not any(e.page.state()["arbitration"]["kiosk_port"] for e in (a, b)), 3)
    check(ok and "arbitration_mode=hassmic" in open(os.path.join(a.state, "config")).read().split() and "kiosk port closed" in a.text(),
          "back to our own mode: the kiosk port closed")
    await until(lambda: [m["mode"] for m in a.page.state()["arbitration"]["members"]] == ["hassmic"], 3)
    na, nb = len(a.starts), len(b.starts)
    a.wake(); b.wake(); kiosk(99.0); await asyncio.sleep(1.5)
    check(len(b.starts) == nb + 1 and len(a.starts) == na, "our own mode again: kiosk claims no longer count, the Echos settle it")
    b.end_pipeline(); await asyncio.sleep(0.5)
    kio.close()

run(main, (a, b))
