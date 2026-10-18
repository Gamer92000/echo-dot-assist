#!/usr/bin/env python3
"""Wake word arbitration (tests/arbecho.py), two Echos in one network: a browser let in through it, who answers when
both hear it (the claims on the wire, the decision, the audio from the wake word on), Home Assistant's own duplicate
check, the Echo in a conversation, different wake words, arbitration switched off and on."""
import asyncio, json, os, re, socket, time
from aioesphomeapi import VoiceAssistantEventType as Ev
from arbecho import ARB, Echo, HA, Browser, check, until, start, run
from webclient import req

key = os.urandom(32)
a, b = Echo("Echo Kitchen", 16961, 500, net=(2, key)), Echo("Echo Living Room", 16962, 900, net=(2, key))
# a second "Echo" on the kitchen Echo only: there its names become "Echo (de)" and "Echo (en)" in HA's select
os.makedirs(os.path.join(a.state, "models", "echo-en"))
open(os.path.join(a.state, "models", "echo-en", "pryon.manifest"), "w").close()


async def main():
    ha = HA([a, b])
    await start((a, b), ha)
    ok = await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 10)
    check(ok, "started in one network: each counts the other at once (the beacons' F_HELLO)")

    # logged in on one Echo: in on the other one too, through their network, without its button
    x = Browser(a.web); check(x.login_with_button(a.proc), "a new browser, approved on the kitchen Echo only")
    xb = x.at(b.web)
    check(xb.state() is None and xb.through(x) == "approved" and xb.state() is not None,
          "through the network: let in on the living room Echo, no button pressed there")
    check(any("via Echo Kitchen" in k["label"] for k in xb.state()["clients"]), "listed there as approved through the kitchen Echo")
    y = Browser(a.web); yb = y.at(b.web)                   # never approved anywhere
    check(yb.through(y) == "not logged in" and yb.state() is None, "a browser approved nowhere gets no voucher")
    n = json.loads(req(b.web, "POST", "/api/vouch/nonce", xb.pub.hex().encode())[2])["nonce"]
    z = Browser(a.web); z.login_with_button(a.proc); zb = z.at(b.web)
    check(zb.through(z, voucher="00" * 16) == "refused" and zb.state() is None, "a made-up voucher: refused")
    st, _, data = x.call("POST", "/api/vouch/issue", f"{a.page.hello['pub']} {zb.pub.hex()} {n}".encode())
    body = f"{zb.pub.hex()} {n} {json.loads(data)['voucher']} {json.loads(data)['via'].encode().hex()} z".encode()
    check(json.loads(req(b.web, "POST", "/api/vouch/login", body)[2])["login"] == "refused" and zb.state() is None,
          "a voucher made out for another Echo, or for a nonce handed to another browser: refused")
    n = json.loads(req(b.web, "POST", "/api/vouch/nonce", zb.pub.hex().encode())[2])["nonce"]
    check(zb.through(z, nonce=n) == "approved" and zb.through(z, nonce=n) == "refused", "a nonce counts once")

    # both hear it, the one that heard it better answers, the other stays quiet
    sn = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)                 # listens in on the claims
    sn.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); sn.bind(("", ARB)); sn.setblocking(False)
    await asyncio.sleep(1)
    while True:
        try: sn.recv(512)
        except BlockingIOError: break
    a.wake(); b.wake(); t0 = time.monotonic()
    got = []
    while time.monotonic() - t0 < 2:
        try: p = sn.recv(512); got.append((time.monotonic(), p))
        except BlockingIOError: await asyncio.sleep(0.005)
    sn.close()
    same = {}
    for t, p in got:
        if p[4] == 4: same.setdefault(p, []).append(round((t - min(x for x, q in got if q == p)) * 1000))
    check(len(same) == 3 and all(len(v) == 3 and 20 <= v[1] <= 60 and 65 <= v[2] <= 120 for v in same.values()),
          f"two claims and one \"answers\", each three times, spread over 80 ms (ms after the first): {sorted(same.values())}")
    check(len(b.starts) == 1 and not a.starts, f"both heard it: the better one (900 over 500) answers alone: {len(a.starts)} / {len(b.starts)}")
    check(b.starts and b.starts[0] - t0 < 0.25 and "this Echo answers (after" in b.text() and "every other Echo's claim in" in b.text(),
          f"decided once both claims were in, not after the window (500 ms until learned): {b.starts[0] - t0 if b.starts else -1:.2f} s")
    check("another Echo answers" in a.text() and "earcon" not in a.text(), "the other one: no sound")
    # the winner sends from the ring what was said from the wake word's end (128 ms before it was heard on the PC)
    # through the round (settled within a block or two) up to the next live 50 ms block: 2048 + 0..3200 samples
    await asyncio.sleep(0.3); ahead = sum(b.chunks[:b.chunks.index(1600)]) if 1600 in b.chunks else None
    check(ahead is not None and 4096 <= ahead <= 4096 + 6400, f"nothing said after the wake word is lost to the round: {ahead} bytes ahead of the live stream")
    b.end_pipeline(); await asyncio.sleep(0.5)

    # Home Assistant's own duplicate check (other satellites): no error ring, just back to idle
    a.wake(); await until(lambda: a.starts, 3)
    a.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "duplicate_wake_up_detected", "message": "Duplicate wake-up detected for Alexa"})
    await asyncio.sleep(0.5)
    check(a.starts and "another satellite answers" in a.text() and "pipeline error" not in a.text(), "duplicate_wake_up_detected: quiet, not an error")

    # an Echo in a conversation owns the next wake word, whatever the score
    n = len(b.starts)
    a.wake(); await until(lambda: len(a.starts) == 2, 3)
    a.wake(); b.wake(); await asyncio.sleep(1.5)
    check(len(a.starts) == 2 and len(b.starts) == n, "in a conversation: the other Echo lets it have the wake word")
    a.end_pipeline(); await asyncio.sleep(0.5)

    # different wake words never compete: each Echo answers its own
    await a.cli.set_voice_assistant_configuration(["echo-de"]); await asyncio.sleep(0.5)
    na, nb = len(a.starts), len(b.starts)
    a.wake(); b.wake(); await asyncio.sleep(1.5)
    check(len(a.starts) == na + 1 and len(b.starts) == nb + 1, "\"Echo\" on one, \"Alexa\" on the other: no arbitration between them")
    a.end_pipeline(); b.end_pipeline(); await asyncio.sleep(0.5)
    # the same word under two names ("Echo (de)" where another set shares it, "Echo" where not): they compete
    await b.cli.set_voice_assistant_configuration(["echo-de"]); await asyncio.sleep(0.5)
    na, nb = len(a.starts), len(b.starts)
    a.wake(); b.wake(); await asyncio.sleep(1.5)
    check('"Echo (de)"' in a.text() and len(a.starts) == na and len(b.starts) == nb + 1 and "every other Echo's claim in" in b.text(),
          f"\"Echo (de)\" on one, \"Echo\" on the other: one word, the better one answers alone: {len(a.starts) - na} / {len(b.starts) - nb}")
    b.end_pipeline(); await asyncio.sleep(0.5)
    await b.cli.set_voice_assistant_configuration(["alexa"]); await asyncio.sleep(0.5)
    await a.cli.set_voice_assistant_configuration(["alexa"]); await asyncio.sleep(0.5)

    check(re.search(r"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}Z arbitration: ", a.text(), re.M), "log lines stamped with the time (UTC) once the clock is known")

    # arbitration off: no rounds, but still in the network (the settings pages find each other through it)
    net = a.net()
    await a.arbitrate(False)
    check(a.net() == net and "arbitration_peers" not in a.by and a.services and a.page.state()["arbitration"]["arbitrates"] is False,
          "arbitration off on the page: network key kept, action still listed, peers sensor gone")
    ok = await until(lambda: b.st("arbitration_peers") == 0, 5)
    check(ok and [m["arbitrates"] for m in b.page.state()["arbitration"]["members"]] == [False],
          "the other Echo learns it from the next beacon, sent at once: still a member, no longer counted for rounds")
    na, nb, asked = len(a.starts), len(b.starts), b.text().count("no other Echo to ask")
    a.wake(); b.wake(); await asyncio.sleep(1.5)
    check(len(a.starts) == na + 1 and len(b.starts) == nb + 1 and "arbitration off" in a.text() and b.text().count("no other Echo to ask") > asked,
          "both answer, and neither waits for a round (Home Assistant's check is left)")
    a.end_pipeline(); b.end_pipeline(); await asyncio.sleep(0.5)
    await a.arbitrate(True)
    ok = await until(lambda: b.st("arbitration_peers") == 1, 5)
    check(ok and "takes part in rounds again" in a.text(), "arbitration on again: counted at once")

run(main, (a, b))
