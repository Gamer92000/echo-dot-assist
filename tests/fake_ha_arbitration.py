#!/usr/bin/env python3
"""Wake word arbitration between two Echos (build/hassmic-host x2) under one Home Assistant, plus a device on the network
that Home Assistant does not know.  The fake Home Assistant does what the real one does with ESPHome devices: it
connects with each device's API key, keeps their entity states under sensor.<device name>_<entity name>, answers any
device's request for a state (no permission needed), and runs an action a device asks for ("Allow the device to perform
Home Assistant actions") that names another device's own action (esphome.<node>_arbitration_key) on that device.
Three ways in: the handoff entities (no permission), the action, and the volume-key pairing (SIGWINCH, no HA).
Everything goes out as loopback broadcast (HASSMIC_ARB_ADDR): nothing of it reaches the LAN."""
import asyncio, base64, os, re, signal, socket, struct, subprocess, tempfile, time
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARB, BCAST = 28990, "127.255.255.255"


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


class Echo:
    def __init__(self, name, port, score, suffix=""):
        self.name, self.port, self.state, self.suffix = name, port, tempfile.mkdtemp(), suffix
        self.node = re.sub(r"[^a-z0-9]", "-", name.lower())
        self.log = os.path.join(self.state, "log")
        self.api_key = base64.b64encode(os.urandom(32)).decode()
        with open(os.path.join(self.state, "api_key"), "w") as f: f.write(self.api_key + "\n")     # as Home Assistant provisioned it
        self.env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_SETTINGS=os.path.join(self.state, "settings"),
                        HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(self.state, "play.raw"),
                        HASSMIC_MDNS_FILE=os.path.join(self.state, "none"), HASSMIC_ARB_ADDR=BCAST, HASSMIC_TEST_SCORE=str(score),
                        HASSMIC_MODELS=os.path.join(self.state, "models"))
        os.makedirs(os.path.join(self.state, "models", "echo-de"))
        open(os.path.join(self.state, "models", "echo-de", "pryon.manifest"), "w").close()
        self.starts, self.states, self.allowed = [], {}, True

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.port), "-n", self.name, "-L",
                                      "-z", "0", "-o", "0", "-a", str(ARB)], env=self.env, stderr=open(self.log, "w"))

    def text(self): return open(self.log).read()
    def net(self):
        try: return re.search(r"^net (\S+ \S+)$", open(os.path.join(self.state, "arbitration")).read(), re.M).group(1)
        except (OSError, AttributeError): return None
    def st(self, oid): return self.states.get(self.by[oid].key)
    def wake(self): self.proc.send_signal(signal.SIGUSR1)
    def pair(self): self.proc.send_signal(signal.SIGWINCH)          # Volume up + Volume down held
    def entity(self, suffix=None):              # HA's entity id; suffix: HA's for an id another device held first
        return "sensor." + re.sub(r"[^a-z0-9]+", "_", self.name.lower()) + "_arbitration_handoff" + (self.suffix if suffix is None else suffix)
    def pub(self):
        try: return base64.b64decode(self.st("arbitration_handoff").split()[1])
        except (AttributeError, IndexError): return None

    async def connect(self, ha):
        self.cli = APIClient("127.0.0.1", self.port, None, noise_psk=self.api_key)
        await self.cli.connect(login=True)
        ents, self.services = await self.cli.list_entities_services()
        self.by = {e.object_id: e for e in ents}
        self.cli.subscribe_states(lambda s: self.states.__setitem__(s.key, getattr(s, "state", None)))
        self.cli.subscribe_home_assistant_states_and_services(on_state=lambda s: None, on_service_call=lambda c: ha.action(self, c),
                                                              on_state_sub=lambda e, a: None, on_state_request=lambda e, a: ha.state(self, e, a))
        async def handle_start(conv, flags, settings, phrase): self.starts.append(time.monotonic()); return 0
        async def handle_stop(abort): pass
        async def handle_audio(data, *_): pass
        self.cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio)

    def end_pipeline(self): self.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)


class HA:
    """Home Assistant's esphome manager: device actions run only with the option ticked (otherwise a repair), and
    esphome.<device_info.name with '_'>_<service> is each device's own action."""
    def __init__(self, echos, others=None): self.echos, self.calls, self.refused, self.states_on, self.asked, self.others = echos, [], [], True, [], others or {}
    def state(self, asker, entity, attribute):
        """async_on_state_request: the current state, nothing for an entity that has none (missing, or disabled)"""
        self.asked.append((asker.name, entity))
        if entity in self.others: asker.cli.send_home_assistant_state(entity, attribute, self.others[entity])
        for e in self.echos:
            st = e.st("arbitration_handoff") if hasattr(e, "by") else None
            if self.states_on and entity == e.entity() and st: asker.cli.send_home_assistant_state(entity, attribute, st)
    def action(self, sender, call):
        if not sender.allowed: self.refused.append(sender.name); return
        self.calls.append((sender.name, call.service, dict(call.data)))
        for e in self.echos:
            svc = next((s for s in getattr(e, "services", []) if f"esphome.{e.node.replace('-', '_')}_{s.name}" == call.service), None)
            if svc: asyncio.get_running_loop().create_task(e.cli.execute_service(svc, dict(call.data)))


async def until(cond, secs):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if cond(): return True
        await asyncio.sleep(0.2)
    return cond()


async def main():
    a, b = Echo("Echo Kitchen", 16961, 500, "_4"), Echo("Echo Living Room", 16962, 900)
    # other devices named "Echo Kitchen" in HA hold the first three ids: one online, one off, one more online
    other = lambda: "HMA1 " + base64.b64encode(os.urandom(32)).decode()
    ha = HA([a, b], {a.entity(""): other(), a.entity("_2"): "unavailable", a.entity("_3"): other()})
    # each already in a network of its own (as two Echos that started at the same moment would be): they merge
    for e, net in ((a, 5), (b, 2)):
        with open(os.path.join(e.state, "arbitration"), "w") as f:
            f.write(f"join 1\nctr 0\nnet {net:016x} {base64.b64encode(os.urandom(32)).decode()}\n")
    a.start(); b.start()
    await asyncio.sleep(0.5)
    try:
        await a.connect(ha); await b.connect(ha)
        check([s.name for s in a.services] == ["arbitration_key"] and [x.name for x in a.services[0].args] == ["network", "key"]
              and "arbitration_id" not in a.by, "the Echo offers its \"arbitration_key\" action, and no ID entity")
        check(a.by["arbitration_handoff"].entity_category == 2, "\"Arbitration handoff\": a diagnostic entity")
        a.allowed = b.allowed = False                   # neither may run actions: the handoff entities alone
        seen = []
        b.cli.subscribe_states(lambda s: s.key == b.by["arbitration_handoff"].key and seen.append(getattr(s, "state", None)))
        ok = await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 40)
        check(ok and a.net() == b.net() and a.net().startswith("0000000000000002"), f"two networks merged into the older one: {a.net() and a.net()[:16]}")
        check(f"Home Assistant shows our key on {a.entity()}" in a.text() and f"Home Assistant shows our key on {b.entity()}" in b.text(),
              "each Echo found its own handoff entity through Home Assistant")
        check(not any(n == a.name and e.endswith("_handoff_5") for n, e in ha.asked),
              f"three ids held by other devices: found its own at {a.entity()}, and asked no further")
        check(f"Home Assistant shows echo-kitchen's key on {a.entity()}" in b.text()
              and "moved to the older network 0000000000000002 (key offered by echo-living-room)" in a.text() and not ha.calls and not ha.refused,
              "the key went through Home Assistant on the member's handoff entity, without any action")
        key_b64 = re.search(r"^net \S+ (\S+)$", open(os.path.join(a.state, "arbitration")).read(), re.M).group(1)
        check(any(len(x.split()) == 5 for x in seen) and not any(key_b64 in x for x in seen),
              "the network key is not readable in the offer Home Assistant showed")
        await until(lambda: len(b.st("arbitration_handoff").split()) == 2, 5)
        check(len(b.st("arbitration_handoff").split()) == 2, "offer taken down once the other Echo is in")
        a.allowed = b.allowed = True

        # both hear it, the one that heard it better answers, the other stays quiet
        await asyncio.sleep(1)
        a.wake(); b.wake(); t0 = time.monotonic()
        await asyncio.sleep(2)
        check(len(b.starts) == 1 and not a.starts, f"both heard it: the better one (900 over 500) answers alone: {len(a.starts)} / {len(b.starts)}")
        check(b.starts and b.starts[0] - t0 < 1.0, f"answer after {b.starts[0] - t0 if b.starts else -1:.2f} s")
        check("another Echo answers" in a.text() and "earcon" not in a.text(), "the other one: no sound")
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
        await a.cli.set_voice_assistant_configuration(["alexa"]); await asyncio.sleep(0.5)

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
        ent = lambda e: b"HMA1\x02" + xp + bytes([len(e)]) + e.encode()
        send(b"HMA1\x01" + xp + struct.pack("<Q", 0) + node("evil")); send(ent(a.entity()))   # "my entity is that real Echo's"
        await asyncio.sleep(4)
        offers = [c for c in (a.st("arbitration_handoff"), b.st("arbitration_handoff")) if len(c.split()) > 2]
        check((b.name, a.entity()) in ha.asked and not offers and "shows evil's key" not in b.text(),
              "a forged entity id (a real Echo's): Home Assistant shows another key there, so no offer")
        await asyncio.sleep(22)                         # no entity confirmed: the action, after ATTEST_WAIT_MS
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

        # leave: the key is gone, the Echo answers on its own again
        a.cli.switch_command(a.by["join_arbitration_network"].key, False)
        await until(lambda: a.st("join_arbitration_network") is False, 3)
        check(a.net() is None and "left network" in a.text() and a.st("arbitration_peers") == 0, "join switched off: network key wiped")
        na, nb = len(a.starts), len(b.starts)
        a.wake(); b.wake(); await asyncio.sleep(1.5)
        check(len(a.starts) == na + 1 and len(b.starts) == nb + 1, "outside the network: both answer (Home Assistant's check is left)")
        a.end_pipeline(); b.end_pipeline(); await asyncio.sleep(0.5)

        # nobody else in a network: the first to join starts one, the next one gets its key
        b.cli.switch_command(b.by["join_arbitration_network"].key, False)
        await until(lambda: b.net() is None, 3)
        a.cli.switch_command(a.by["join_arbitration_network"].key, True); t0 = time.monotonic()
        ok = await until(lambda: a.net(), 10)
        check(ok and "no other Echo found, started network" in a.text() and time.monotonic() - t0 > 4, f"alone: started a network after {time.monotonic() - t0:.1f} s")
        # no handoff entity to read (disabled, or the device renamed in HA) and no permission: no key, HA raises its repair
        a.allowed = False; ha.states_on = False; refused = len(ha.refused)
        b.cli.switch_command(b.by["join_arbitration_network"].key, True)
        await asyncio.sleep(24)
        check(len(ha.refused) > refused and b.net() is None, "no handoff entity and no \"perform actions\": no key (HA raises its repair)")

        # the volume keys on both: the member hands its network to the one Echo that asked
        b.pair(); await asyncio.sleep(1.5); a.pair()
        ok = await until(lambda: b.net() == a.net(), 8)
        check(ok and "pairing: handing network" in a.text() and "(key from pairing)" in b.text(),
              "volume keys on both: joined without Home Assistant")
        await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 35)
        await asyncio.sleep(2)                          # the member's last two copies of its "give" (they would let b back in)

        # two Echos asking at once: the network goes to neither
        b.cli.switch_command(b.by["join_arbitration_network"].key, False); await until(lambda: b.net() is None, 3)
        b.cli.switch_command(b.by["join_arbitration_network"].key, True)
        send(b"HMA1\x05" + xp + struct.pack("<Q", 0) + node("evil"))
        b.pair(); await asyncio.sleep(1.5); a.pair(); await asyncio.sleep(4)
        check(b.net() != a.net() and "pairing refused: 2 Echos asked at once" in a.text(), "a second Echo asking: pairing refused")
        # a forger's "give" while b still pairs: it does not open with b's key
        send(b"HMA1\x06" + b.pub() + struct.pack("<Q", 1) + os.urandom(104))
        await asyncio.sleep(0.5)
        check(b.net() != a.net() and "key from pairing does not open with ours, ignored" in b.text(), "a forged network handed over: ignored")

        # handoff entity back: in without permission
        ha.states_on = True
        b.cli.switch_command(b.by["join_arbitration_network"].key, False); await until(lambda: b.net() is None, 3)
        b.cli.switch_command(b.by["join_arbitration_network"].key, True)
        ok = await until(lambda: b.net() == a.net(), 20)
        check(ok and "offered by echo-kitchen" in b.text(), "handoff entity readable again: joined without permission")
        await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 35)

        # permission and no handoff entity (an Echo of an older version): the action
        ha.states_on = False; a.allowed = True; calls = len(ha.calls)
        b.cli.switch_command(b.by["join_arbitration_network"].key, False); await until(lambda: b.net() is None, 3)
        b.cli.switch_command(b.by["join_arbitration_network"].key, True)
        ok = await until(lambda: b.net() == a.net() and a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 40)
        check(ok and any(c[1] == "esphome.echo_living_room_arbitration_key" for c in ha.calls[calls:]), "allowed: the next one joined through the action")
    finally:
        for e in (a, b): e.proc.terminate()
        for e in (a, b): e.proc.wait()
    if check.failed:
        for e in (a, b): print(f"--- {e.name}\n{e.text()}")
        print("FAILED"); raise SystemExit(1)
    print("PASS")

asyncio.run(main())
