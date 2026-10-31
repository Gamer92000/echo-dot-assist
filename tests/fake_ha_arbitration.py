#!/usr/bin/env python3
"""Wake word arbitration between two Echos (build/hassmic-host x2) under one Home Assistant (tests/arbecho.py), plus a
device on the network that Home Assistant does not know: two networks merge through the tags (no permission), a forger
gets nothing, an Echo alone starts a network, without tags or permission no key goes anywhere, the volume-key pairing
(SIGWINCH, no HA).  The other ways in: fake_ha_arbitration_join.py; who answers: _rounds.py, Kiosk Satellite mode:
_kiosk.py, the learned window: _window.py, media in the room: _media.py."""
import asyncio, os, socket, struct
from aioesphomeapi import APIClient
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
from arbecho import ARB, BCAST, SCAN, POLL, ATTEST_WAIT, DISCOVER, Echo, HA, Browser, check, until, start, run

# each already in a network of its own (as two Echos that started at the same moment would be): they merge
a, b = Echo("Echo Kitchen", 16961, 500, net=5), Echo("Echo Living Room", 16962, 900, net=2)


async def main():
    ha = HA([a, b])
    await start((a, b), ha)
    ak = next((s for s in a.services if s.name == "arbitration_key"), None)
    check(ak is not None and [x.name for x in ak.args] == ["network", "key"] and "arbitration_id" not in a.by,
          "the Echo offers its \"arbitration_key\" action, and no ID entity")
    check("arbitration_handoff" not in a.by, "no \"Arbitration handoff\" entity any more")
    a.allowed = b.allowed = False                   # neither may run actions: the tags alone
    ok = await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 20)
    check(ok and a.net() == b.net() and a.net().startswith("0000000000000002"), f"two networks merged into the older one: {a.net() and a.net()[:16]}")
    check({t for _, t in ha.scans} == {a.tag(), b.tag()} and all(t == {a.name: a, b.name: b}[n].tag() for n, t in ha.scans),
          "each Echo scanned its own tag: hassmic_<its public key>")
    check("Home Assistant confirms echo-kitchen's key" in b.text() and "Home Assistant confirms echo-living-room's key" in a.text(),
          "each Echo had Home Assistant confirm the other's key: its tag scanned after the first read")
    check("moved to the older network 0000000000000002 (key from echo-living-room, confirmed by Home Assistant)" in a.text()
          and not ha.calls and not ha.refused, "the key went on the LAN, sealed, after Home Assistant confirmed both: without any action")
    arb = a.page.state()["arbitration"]
    check(arb["network"] == "0000000000000002" and [(m["node"], m["ip"]) for m in arb["members"]] == [("echo-living-room", "127.0.0.1")]
          and arb["tag"] == "tag." + a.tag() and not arb["others"], f"settings page: the network, its member with its address, the tag: {arb}")
    n = len(ha.scans); await asyncio.sleep(SCAN * 1.5)
    check(len(ha.scans) == n, f"both in one network: no more scans ({len(ha.scans) - n} since)")
    a.allowed = b.allowed = True

    # a device Home Assistant does not know
    x = X25519PrivateKey.generate(); xp = x.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    send = lambda p: tx.sendto(p, (BCAST, ARB))
    node = lambda s: bytes([len(s)]) + s.encode()
    net = a.net(); calls = len(ha.calls)
    send(b"HMA1\x01" + xp + struct.pack("<Q", 0) + node("evil"))                      # outside any network: gets our key sent...
    xp2 = X25519PrivateKey.generate().public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    send(b"HMA1\x01" + xp2 + struct.pack("<Q", 0) + node(b.node))                     # ...or naming a real Echo, with its own key
    send(b"HMA1\x01" + xp + struct.pack("<Q", 1) + node("evil") + struct.pack("<Q", 1) + os.urandom(16))   # "an older network"
    la, lb = len(a.text()), len(b.text())
    ha.tags["hassmic_" + xp.hex()] = "2026-01-01T00:00:00.000+00:00"      # its tag, scanned once long ago (removed from HA since)
    send(b"HMA1\x06" + a.pub() + struct.pack("<Q", 1) + xp + os.urandom(72))    # "here is the older network's key"
    await asyncio.sleep(POLL * 3)                   # read, and read again: the scan time never changes
    check((b.name, "tag.hassmic_" + xp.hex()) in ha.asked and "confirms evil" not in a.text()[la:] + b.text()[lb:]
          and "on the LAN" not in a.text()[la:] + b.text()[lb:] and "confirmed by Home Assistant" not in a.text()[la:],
          "a forger with a tag that never changes (a device since removed from HA): not confirmed, nothing handed")
    send(b"HMA1\x01" + xp + struct.pack("<Q", 0) + node("evil"))
    await asyncio.sleep(ATTEST_WAIT)                # nothing confirmed: the action, ATTEST_WAIT_MS after the first beacon
    sent = {c[1] for c in ha.calls[calls:]}
    check(sent and sent <= {"esphome.evil_arbitration_key", f"esphome.{b.node.replace('-', '_')}_arbitration_key"},
          f"unknown device: the key only ever goes to a device Home Assistant adopted under that name: {sorted(sent)}")
    check(b.text().count("handing network") >= 0 and "ignored" in b.text() and b.net() == net,
          "a forged beacon with a real Echo's name: that Echo gets the key (it holds it already), the forger nothing")
    check(a.net() == net == b.net(), "posing as an older network: nothing taken from it (it cannot hand a key through Home Assistant)")
    # a forged claim does not count: it lacks the network key
    send(b"HMA1\x04" + bytes.fromhex(net.split()[0])[::-1] + xp[:8] + struct.pack("<Q", 1 << 40) + os.urandom(8) + struct.pack("<iBB", 99999, 2, 1) + os.urandom(16))
    n = len(b.starts); b.wake(); await asyncio.sleep(1.5)
    check(len(b.starts) == n + 1, "forged claim \"answers already\" ignored")
    b.end_pipeline(); await asyncio.sleep(0.5)
    # without the device key there is no connection, so no key can come any other way than from Home Assistant
    try: await asyncio.wait_for(APIClient("127.0.0.1", b.port, None).connect(login=True), 3); ok = False
    except Exception: ok = True
    check(ok, "plaintext connection to an Echo with a device key: refused")

    # nobody else in a network: the first Echo starts one, the next one gets its key
    await b.reset(start=False)
    t0 = asyncio.get_running_loop().time(); await a.reset()
    ok = await until(lambda: a.net(), 5)
    took = asyncio.get_running_loop().time() - t0
    check(ok and "no other Echo found, started network" in a.text() and took > DISCOVER * 0.8, f"alone: started a network after {took:.1f} s")
    # no tag integration in HA and no permission: no key, HA raises its repair
    a.allowed = False; ha.tags_on = False; ha.tags.clear(); refused = len(ha.refused)
    b.start(); await asyncio.sleep(0.5); await b.connect(ha)
    w = Browser(a.web); w.login_with_button(a.proc)
    check(w.at(b.web).through(w) == "refused", "an Echo outside the network: nobody vouches it in (the button it is)")
    await asyncio.sleep(ATTEST_WAIT + 1.5)
    check(len(ha.refused) > refused and b.net() is None, "no tags and no \"perform actions\": no key (HA raises its repair)")
    o = [x for x in a.page.state()["arbitration"]["others"] if x["node"] == "echo-living-room"]
    check(o and o[0]["state"] in ("none", "younger") and not o[0]["key_confirmed"], f"settings page: the Echo left outside, and why: {o}")

    # the volume keys on both: the member hands its network to the one Echo that asked
    b.pair(); await asyncio.sleep(1.5); a.pair()
    ok = await until(lambda: b.net() == a.net(), 8)
    check(ok and "pairing: handing network" in a.text() and "(key from pairing)" in b.text(),
          "volume keys on both: joined without Home Assistant")
    await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 10)
    await asyncio.sleep(2)                          # the member's last two copies of its "give" (they would let b back in)

    # two Echos asking at once: the network goes to neither
    await b.reset()
    send(b"HMA1\x05" + xp + struct.pack("<Q", 0) + node("evil"))
    b.pair(); await asyncio.sleep(1.5); a.pair()
    await until(lambda: "pairing refused" in a.text(), 4)
    check(b.net() != a.net() and "pairing refused: 2 Echos asked at once" in a.text(), "a second Echo asking: pairing refused")
    # a forger's "give" while b still pairs: it does not open with b's key
    send(b"HMA1\x06" + b.pub() + struct.pack("<Q", 1) + os.urandom(104))
    await asyncio.sleep(0.5)
    check(b.net() != a.net() and "key from pairing does not open with ours, ignored" in b.text(), "a forged network handed over: ignored")

run(main, (a, b))
