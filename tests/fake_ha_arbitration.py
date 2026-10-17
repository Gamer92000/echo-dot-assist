#!/usr/bin/env python3
"""Wake word arbitration between two Echos (build/hassmic-host x2) under one Home Assistant, plus a device on the network
that Home Assistant does not know.  The fake Home Assistant does what the real one does with ESPHome devices: it
connects with each device's API key, fires a device's events whatever its permission (esphome.tag_scanned: the tag
integration keeps tag.<tag id>, state the last scan's time), answers any device's request for a state (no permission
needed), and runs an action a device asks for ("Allow the device to perform Home Assistant actions") that names another
device's own action (esphome.<node>_arbitration_key) on that device.
Three ways in: the tags (no permission), the action, and the volume-key pairing (SIGWINCH, no HA).
Then Kiosk Satellite mode: their JSON claims on their port, against each other and a fake kiosk.
Last, media in the room: the fake Home Assistant follows media players as its state tracker does (the state at once,
then on every change, to each subscriber; dropped with the link), and Music Assistant plays on the kitchen Echo over
Sendspin (aiosendspin, as tests/fake_ma_sendspin.py).
Everything goes out as loopback broadcast (HASSMIC_ARB_ADDR): nothing of it reaches the LAN."""
import asyncio, base64, datetime, hashlib, json, os, re, signal, socket, struct, subprocess, sys, tempfile, time
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import Browser, req
from aiosendspin.noise import Identity, InMemoryServerPairingStore
from aiosendspin.server import SendspinServer, AudioFormat, ClientAddedEvent
import numpy as np
ARB, KIOSK, BCAST = 28990, 28991, "127.255.255.255"          # KIOSK: stands in for Kiosk Satellite's 2330


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


class Echo:
    def __init__(self, name, port, score):
        self.name, self.port, self.state = name, port, tempfile.mkdtemp()
        self.web = port + 10                                # the settings page
        self.node = re.sub(r"[^a-z0-9]", "-", name.lower())
        self.log = os.path.join(self.state, "log")
        self.api_key = base64.b64encode(os.urandom(32)).decode()
        with open(os.path.join(self.state, "api_key"), "w") as f: f.write(self.api_key + "\n")     # as Home Assistant provisioned it
        self.env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_SETTINGS=os.path.join(self.state, "settings"),
                        HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(self.state, "play.raw"),
                        HASSMIC_MDNS_FILE=os.path.join(self.state, "none"), HASSMIC_ARB_ADDR=BCAST, HASSMIC_KIOSK_PORT=str(KIOSK), HASSMIC_TEST_SCORE=str(score),
                        HASSMIC_MODELS=os.path.join(self.state, "models"), HASSMIC_MEDIA_HOLD_MS="1500", HASSMIC_MEDIA_MISSING_MS="1000",
                        HASSMIC_MEDIA_AFTER_MS="1000", HASSMIC_MEDIA_EVERY_MS="3000",
                        HASSMIC_MUSIC=os.path.join(self.state, "music.raw"))
        os.makedirs(os.path.join(self.state, "models", "echo-de"))
        open(os.path.join(self.state, "models", "echo-de", "pryon.manifest"), "w").close()
        self.starts, self.states, self.allowed, self.chunks = [], {}, True, []
        self.sendspin = 0                                   # -z: Sendspin's port, 0 none

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.port), "-n", self.name, "-L",
                                      "-z", str(self.sendspin), "-o", "0", "-a", str(ARB), "-W", str(self.web)], env=self.env, stderr=open(self.log, "a"))

    def text(self): return open(self.log).read()
    def net(self):
        try: return re.search(r"^net (\S+ \S+)$", open(os.path.join(self.state, "arbitration")).read(), re.M).group(1)
        except (OSError, AttributeError): return None
    def st(self, oid): return self.states.get(self.by[oid].key) if oid in self.by else None
    def wake(self): self.proc.send_signal(signal.SIGUSR1)
    def pair(self): self.proc.send_signal(signal.SIGWINCH)          # Volume up + Volume down held
    def pub(self):                              # the arbitration identity's public key, from its private one
        sk = base64.b64decode(open(os.path.join(self.state, "arb_key")).read().strip())
        return X25519PrivateKey.from_private_bytes(sk).public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    def tag(self): return "hassmic_" + self.pub().hex()

    async def arbitrate(self, on):
        """arbitration on or off on the settings page; Home Assistant is sent away and lists the entities again"""
        self.page.set(arbitration="on" if on else "off")
        try: await self.cli.disconnect()
        except Exception: pass
        await asyncio.sleep(0.5); await self.connect(self.ha)

    async def reset(self, start=True):
        """a newcomer: restarted without its network key (the network itself cannot be left any more)"""
        try: await self.cli.disconnect()
        except Exception: pass
        self.proc.terminate(); self.proc.wait(); self.states = {}
        p = os.path.join(self.state, "arbitration")
        keep = [l for l in open(p) if not l.startswith("net ")]
        open(p, "w").writelines(keep)
        if start: self.start(); await asyncio.sleep(0.5); await self.connect(self.ha)

    async def connect(self, ha):
        self.ha, self.states = ha, {}
        ha.drop(self)                                   # a new link: Home Assistant's trackers of the old one are gone
        self.cli = APIClient("127.0.0.1", self.port, None, noise_psk=self.api_key)
        await self.cli.connect(login=True)
        ents, self.services = await self.cli.list_entities_services()
        self.by = {e.object_id: e for e in ents}
        self.cli.subscribe_states(lambda s: self.states.__setitem__(s.key, getattr(s, "state", None)))
        self.cli.subscribe_home_assistant_states_and_services(on_state=lambda s: None, on_service_call=lambda c: ha.action(self, c),
                                                              on_state_sub=lambda e, a: ha.sub(self, e, a), on_state_request=lambda e, a: ha.state(self, e, a))
        async def handle_start(conv, flags, settings, phrase): self.starts.append(time.monotonic()); self.chunks = []; return 0
        async def handle_stop(abort): pass
        async def handle_audio(data, *_): self.chunks.append(len(data))
        self.cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio)

    def end_pipeline(self): self.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)


class HA:
    """Home Assistant's esphome manager: device actions run only with the option ticked (otherwise a repair), and
    esphome.<device_info.name with '_'>_<service> is each device's own action."""
    def __init__(self, echos):
        self.echos, self.calls, self.refused, self.tags_on, self.asked, self.tags, self.scans = echos, [], [], True, [], {}, []
        self.subs, self.media = [], {}                  # lasting subscriptions (echo name, entity); media players' states
        self.attr_subs, self.vols = [], {}              # the same of an attribute (echo name, entity, attribute); volume_level
    def sub(self, echo, entity, attribute):
        """a subscription without "once" (async_on_state_subscription): a tracker that sends the state now and on every
        change, until the link closes.  One of an attribute is a tracker of its own"""
        if attribute:
            self.attr_subs.append((echo.name, entity, attribute))
            if attribute == "volume_level" and entity in self.vols: echo.cli.send_home_assistant_state(entity, attribute, self.vols[entity])
            return
        self.subs.append((echo.name, entity))
        if not attribute and entity in self.media: echo.cli.send_home_assistant_state(entity, "", self.media[entity])
    def drop(self, echo):
        self.subs = [x for x in self.subs if x[0] != echo.name]; self.attr_subs = [x for x in self.attr_subs if x[0] != echo.name]
    def set_volume(self, entity, vol):
        """a player's volume_level changes: its attribute's trackers send it, as str(float)"""
        self.vols[entity] = vol
        for name, e, at in self.attr_subs:
            if e == entity and at == "volume_level": next(x for x in self.echos if x.name == name).cli.send_home_assistant_state(entity, at, vol)
    def set_media(self, entity, state):
        """a media player changes: every tracker of it sends the new state (nothing when it stays the same)"""
        if self.media.get(entity) == state: return
        self.media[entity] = state
        for name, e in self.subs:
            if e == entity: next(x for x in self.echos if x.name == name).cli.send_home_assistant_state(entity, "", state)
    def state(self, asker, entity, attribute):
        """async_on_state_request: the current state, nothing for an entity that has none (missing, or disabled)"""
        self.asked.append((asker.name, entity))
        tag = entity[4:] if entity.startswith("tag.") else None
        if self.tags_on and tag in self.tags: asker.cli.send_home_assistant_state(entity, attribute, self.tags[tag])
    def action(self, sender, call):
        if call.is_event:                               # before the permission check, as in HA's esphome manager
            if call.service == "esphome.tag_scanned" and self.tags_on:
                self.scans.append((sender.name, call.data["tag_id"]))
                self.tags[call.data["tag_id"]] = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds")
            return
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
    a, b = Echo("Echo Kitchen", 16961, 500), Echo("Echo Living Room", 16962, 900)
    ha = HA([a, b])
    # a second "Echo" on the kitchen Echo only: there its names become "Echo (de)" and "Echo (en)" in HA's select
    os.makedirs(os.path.join(a.state, "models", "echo-en"))
    open(os.path.join(a.state, "models", "echo-en", "pryon.manifest"), "w").close()
    # each already in a network of its own (as two Echos that started at the same moment would be): they merge
    for e, net in ((a, 5), (b, 2)):
        with open(os.path.join(e.state, "arbitration"), "w") as f:
            f.write(f"join 1\nctr 0\nnet {net:016x} {base64.b64encode(os.urandom(32)).decode()}\n")
    a.start(); b.start()
    await asyncio.sleep(0.5)
    for e in (a, b): e.page = Browser(e.web); check(e.page.login_with_button(e.proc), f"{e.name}: settings page logged in with the action button")
    try:
        await a.connect(ha); await b.connect(ha)
        ak = next((s for s in a.services if s.name == "arbitration_key"), None)
        check(ak is not None and [x.name for x in ak.args] == ["network", "key"] and "arbitration_id" not in a.by,
              "the Echo offers its \"arbitration_key\" action, and no ID entity")
        check("arbitration_handoff" not in a.by, "no \"Arbitration handoff\" entity any more")
        a.allowed = b.allowed = False                   # neither may run actions: the tags alone
        ok = await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 40)
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
        n = len(ha.scans); await asyncio.sleep(12)
        check(len(ha.scans) == n, f"both in one network: no more scans ({len(ha.scans) - n} since)")
        a.allowed = b.allowed = True

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
        await asyncio.sleep(4)
        check((b.name, "tag.hassmic_" + xp.hex()) in ha.asked and "confirms evil" not in a.text()[la:] + b.text()[lb:]
              and "on the LAN" not in a.text()[la:] + b.text()[lb:] and "confirmed by Home Assistant" not in a.text()[la:],
              "a forger with a tag that never changes (a device since removed from HA): not confirmed, nothing handed")
        send(b"HMA1\x01" + xp + struct.pack("<Q", 0) + node("evil"))
        await asyncio.sleep(28)                         # nothing confirmed: the action, after ATTEST_WAIT_MS
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

        # nobody else in a network: the first Echo starts one, the next one gets its key
        await b.reset(start=False)
        await a.reset(); t0 = time.monotonic()
        ok = await until(lambda: a.net(), 10)
        check(ok and "no other Echo found, started network" in a.text() and time.monotonic() - t0 > 4, f"alone: started a network after {time.monotonic() - t0:.1f} s")
        # no tag integration in HA and no permission: no key, HA raises its repair
        a.allowed = False; ha.tags_on = False; ha.tags.clear(); refused = len(ha.refused)
        b.start(); await asyncio.sleep(0.5); await b.connect(ha)
        w = Browser(a.web); w.login_with_button(a.proc)
        check(w.at(b.web).through(w) == "refused", "an Echo outside the network: nobody vouches it in (the button it is)")
        await asyncio.sleep(34)
        check(len(ha.refused) > refused and b.net() is None, "no tags and no \"perform actions\": no key (HA raises its repair)")
        o = [x for x in a.page.state()["arbitration"]["others"] if x["node"] == "echo-living-room"]
        check(o and o[0]["state"] in ("none", "younger") and not o[0]["key_confirmed"], f"settings page: the Echo left outside, and why: {o}")

        # the volume keys on both: the member hands its network to the one Echo that asked
        b.pair(); await asyncio.sleep(1.5); a.pair()
        ok = await until(lambda: b.net() == a.net(), 8)
        check(ok and "pairing: handing network" in a.text() and "(key from pairing)" in b.text(),
              "volume keys on both: joined without Home Assistant")
        await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 35)
        await asyncio.sleep(2)                          # the member's last two copies of its "give" (they would let b back in)

        # two Echos asking at once: the network goes to neither
        await b.reset()
        send(b"HMA1\x05" + xp + struct.pack("<Q", 0) + node("evil"))
        b.pair(); await asyncio.sleep(1.5); a.pair(); await asyncio.sleep(4)
        check(b.net() != a.net() and "pairing refused: 2 Echos asked at once" in a.text(), "a second Echo asking: pairing refused")
        # a forger's "give" while b still pairs: it does not open with b's key
        send(b"HMA1\x06" + b.pub() + struct.pack("<Q", 1) + os.urandom(104))
        await asyncio.sleep(0.5)
        check(b.net() != a.net() and "key from pairing does not open with ours, ignored" in b.text(), "a forged network handed over: ignored")

        # tags back: in without permission
        ha.tags_on = True
        await b.reset()
        ok = await until(lambda: b.net() == a.net(), 40)
        check(ok and "(key from echo-kitchen, confirmed by Home Assistant)" in b.text(), "tags again: joined without permission")
        await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 35)

        # permission and no tags (HA without the tag integration): the action
        ha.tags_on = False; ha.tags.clear(); a.allowed = True; calls = len(ha.calls)
        await b.reset()
        ok = await until(lambda: b.net() == a.net() and a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 60)
        check(ok and any(c[1] == "esphome.echo_living_room_arbitration_key" for c in ha.calls[calls:]),
              f"allowed: the next one joined through the action ({[c[1] for c in ha.calls[calls:]]}, same network {b.net() == a.net()}, "
              f"peers {a.st('arbitration_peers')}/{b.st('arbitration_peers')})")

        # The window, learned (issue #27): a third member behind a repeater, whose claims reach the kitchen Echo 300-360 ms
        # after its own.  It speaks the protocol with the network's key, as a real Echo would
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
            await asyncio.sleep(1.7)                    # past the round's sampling (1.5 s)
        arb = a.page.state()["arbitration"]; w = arb["window"]
        hall = next((m for m in arb["members"] if m["node"] == "echo-hallway"), {})
        check(not w["learning"] and w["auto"] and 355 <= (hall.get("late_ms") or 0) <= 400 and hall["late_ms"] + 50 <= w["ms"] <= hall["late_ms"] + 60,
              f"learned from its late claims: waits its 95th percentile plus 50 ms: {w}, hallway {hall.get('late_ms')} ms")
        lines = re.findall(r"arbitration: %s heard it, score 99999, \+(\d+) ms" % fid[:4].hex(), a.text())
        check(len(lines) == len(delays) and all(abs(int(x) - d) < 40 for x, d in zip(lines, delays)),
              f"each claim's lateness in the log, as it was sent: {lines}")
        check(re.search(r"arbitration: window learned: \d+ ms \(95 % of the last \d+ claims came within \d+ ms\)", a.text())
              and "another Echo answers (after" in a.text() and "a better claim in" in a.text(), "the log says what it learned, and why each round ended")
        check(open(os.path.join(a.state, "arbitration")).read().count("delay " + fid.hex()) == len(delays), "the samples kept in state/arbitration")
        # the hallway Echo did not hear it, nor the living room one: the kitchen Echo waits the learned window, then answers
        n = len(a.starts); a.wake(); t = await a_claimed()
        await until(lambda: len(a.starts) > n, 3)
        took = a.starts[-1] - t if t and len(a.starts) > n else -1
        check(w["ms"] / 1000 <= took <= w["ms"] / 1000 + 0.15, f"nobody else heard it: answered after the learned {w['ms']} ms: {took:.3f} s")
        a.end_pipeline(); await asyncio.sleep(1.7)
        # learning off: the fixed window, too short for this network; the late claim comes after the decision
        a.page.set(arbitration_window_auto="off", arbitration_window=200)
        n = len(a.starts); a.wake(); t = await a_claimed()
        if t: await asyncio.sleep(max(0, t + 0.3 - time.monotonic())); fclaim()
        await asyncio.sleep(1.7)
        check(len(a.starts) == n + 1 and "waits up to 200 ms (set)" in a.text() and ", after the decision" in a.text()
              and a.page.state()["arbitration"]["window"]["ms"] == 200, "learning off: 200 ms set, the late claim missed (both would answer)")
        a.end_pipeline(); a.page.set(arbitration_window_auto="on"); await asyncio.sleep(0.5)
        # a restart keeps what was learned; it counts once the member is heard from again
        await a.cli.disconnect(); a.proc.terminate(); a.proc.wait(); a.start(); await asyncio.sleep(0.5); await a.connect(ha)
        a.page = Browser(a.web); a.page.login_with_button(a.proc)
        fbeacon(); await asyncio.sleep(1)
        w2 = a.page.state()["arbitration"]["window"]
        check(not w2["learning"] and w2["ms"] == w["ms"], f"after a restart: still learned, {w2['ms']} ms")
        fbeacon(b"\x01"); await asyncio.sleep(0.5)          # the hallway Echo switches arbitration off: nobody waits for it
        check(a.page.state()["arbitration"]["window"]["samples"] < len(delays), "a member out of rounds: its samples no longer count")

        # Media in the room (main.c wake_heard): the kitchen Echo (500) hears worse than the living room one (900), but a
        # Sonos plays beside it.  The players are a setting of each Echo; Home Assistant sends their states, over the keyed
        # link only, for good (not once)
        SONOS, RADIO, TV = "media_player.kitchen_sonos", "media_player.kitchen_radio", "media_player.living_room_tv"
        ha.media.update({SONOS: "idle", RADIO: "idle", TV: "off"}); ha.vols[SONOS] = "0.3"
        ROOM = r"room -?\d+\.\d dBFS \(floor -?\d+\.\d\)"     # the media log's measure (main.c room_str)
        media = lambda e: (e.page.state() or {}).get("media") or {}
        setting = lambda e, n: next((x for x in (e.page.state() or {}).get("settings", []) if x["name"] == n), {})
        def drain():
            while True:
                try: sn.recv(512)
                except BlockingIOError: return
        async def round_(settle=1.5):
            """both hear it: who answered ("a", "b", "ab", ""), the kitchen Echo's claim: its score and length"""
            drain(); na, nb = len(a.starts), len(b.starts); claim = None
            a.wake(); b.wake(); end = time.monotonic() + settle
            while time.monotonic() < end:
                try: p = sn.recv(512)
                except BlockingIOError: await asyncio.sleep(0.005); continue
                if claim is None and p[4] == 4 and p[13:21] == apk and p[-17] == 0: claim = p
            won = ("a" if len(a.starts) > na else "") + ("b" if len(b.starts) > nb else "")
            if "a" in won: a.end_pipeline()
            if "b" in won: b.end_pipeline()
            await asyncio.sleep(0.5)
            return won, struct.unpack("<i", claim[37:41])[0] if claim else None, len(claim) if claim else None

        r = [a.page.set(arbitration_media_players=v) for v in ("media_player.Kitchen", "light.kitchen", ",".join(f"media_player.p{i}" for i in range(9)),
                                                                "media_player." + "x" * 81)]
        check(all(x.get("applied") == 0 and "arbitration_media_players" in x.get("errors", "") for x in r) and "at most 8" in r[2].get("errors", "")
              and "not a media player" in r[0].get("errors", "") and not ha.subs, f"not a media player's entity id, or more than 8: refused: {[x.get('errors') for x in r]}")
        r = a.page.set(arbitration_media_players=SONOS)
        ok = await until(lambda: (a.name, SONOS) in ha.subs, 3)
        check(r.get("applied") == 1 and ok and not any(n == b.name for n, _ in ha.subs) and (a.name, SONOS) not in ha.asked
              and f"following {SONOS} in Home Assistant" in a.text(), f"the list set on the page: followed at once, for good (not once): {ha.subs}")
        check(f"arbitration_media_players={SONOS}" in open(os.path.join(a.state, "config")).read().split(), "kept in state/config")
        sp, sa = setting(a, "arbitration_media_players"), setting(a, "arbitration_media_ambient")
        check(sp.get("type") == "text" and sp.get("value") == SONOS and sp.get("export") is False and sp.get("feature") is False
              and sa.get("value") == 50 and (sa.get("min"), sa.get("max")) == (10, 100) and sa.get("export") is True,
              f"settings page: the players (text, not exported), the background (50 %, exported): {sp} {sa}")
        m = media(a)
        check(m.get("pct") == 50 and m.get("here") is None and m.get("players") == [{"id": SONOS, "state": "stopped"}],
              f"status: the player as Home Assistant said at once (idle): {m}")
        won, sc, base_len = await round_()
        check(won == "b" and sc == 500 and "arbitration: no media of this room: score 500" in a.text(), f"no media: the better one answers: {won} {sc}")
        ha.set_media(SONOS, "playing"); await asyncio.sleep(0.3)
        check(f"arbitration: {SONOS} playing" in a.text() and media(a).get("here") == f"{SONOS} playing", f"the Sonos plays: {media(a)}")
        won, sc, _ = await round_()
        check(won == "b" and sc == 801 and f"arbitration: media of this room ({SONOS} playing): ambient counted at 50 %, score 500 -> 801" in a.text(),
              f"background at 50 %: +3 dB, still under the other Echo's 900: {won} {sc}")
        # the media log: the room's level at the change and a while after (1 s here, 5 s on the Echo), the volume as well,
        # and the room's floor on each round's line; logging only, the scores above and below are the same
        t = a.text()
        check(re.search(rf"^.*media: {SONOS} idle -> playing, {ROOM}$", t, re.M) and re.search(rf"^.*media: {SONOS} playing 1 s, {ROOM}$", t, re.M)
              and re.search(r"arbitration: media of this room .*score 500 -> 801; room floor -?\d+\.\d dBFS$", t, re.M),
              "media log: the change, 1 s after, the floor on the round's line")
        check(ha.attr_subs == [(a.name, SONOS, "volume_level")] and f"media: {SONOS} volume" not in t,
              f"its volume followed too, once; the first value is no change: {ha.attr_subs}")
        ha.set_volume(SONOS, "0.45"); await asyncio.sleep(0.2); ha.set_volume(SONOS, "0.5"); await asyncio.sleep(1.3)
        t = a.text()
        check(re.search(rf"media: {SONOS} volume 0\.30 -> 0\.45, {ROOM}$", t, re.M) and re.search(rf"media: {SONOS} volume 0\.45 -> 0\.50, {ROOM}$", t, re.M)
              and len(re.findall(rf"media: {SONOS} volume 0\.50 1 s, {ROOM}$", t, re.M)) == 1 and f"volume 0.45 1 s" not in t,
              "media log: each volume change, one line after the last step")
        await asyncio.sleep(2)
        check(re.search(rf"media: {ROOM}, playing: {SONOS}$", a.text(), re.M), "media log: a line now and then while it plays")
        a.page.set(arbitration_media_ambient=25)
        won, sc, ln = await round_()
        check(won == "a" and sc == 1102 and ln == base_len, f"at 25 %: +6 dB, the kitchen Echo answers; its claim carries 1102, same length: {won} {sc} {ln}/{base_len}")
        ha.set_media(SONOS, "paused"); await asyncio.sleep(0.2)
        won, sc, _ = await round_()
        check(won == "a" and re.search(rf"arbitration: {SONOS} paused \(counts \d+ s more\)", a.text())
              and re.search(rf"media: {SONOS} playing -> paused, {ROOM}$", a.text(), re.M)
              and re.search(rf"media of this room \({SONOS} stopped 0 s ago\)", a.text()), f"paused: still counts for the hold: {won} {sc}")
        won, sc, _ = await round_()
        check(won == "b" and sc == 500, f"after the hold (1.5 s here): no more: {won} {sc}")

        b.page.set(arbitration_media_players=TV); await until(lambda: (b.name, TV) in ha.subs, 3)
        ha.set_media(SONOS, "playing"); ha.set_media(TV, "playing"); await asyncio.sleep(0.3)
        won, sc, _ = await round_()
        check(won == "b" and sc == 1102 and "score 900 -> 1201" in b.text(), f"media in both rooms: each Echo counts its own, the better one answers: {won} {sc}")
        ha.set_media(TV, "off")
        n = len(ha.subs); a.page.set(arbitration_media_players=SONOS); await asyncio.sleep(0.5)
        check(len(ha.subs) == n, "the same list again: nothing followed twice")
        a.page.set(arbitration_media_players=f"{SONOS}, {RADIO}"); await asyncio.sleep(0.5)
        check(ha.subs[n:] == [(a.name, RADIO)] and f"arbitration_media_players={SONOS},{RADIO}" in open(os.path.join(a.state, "config")).read().split(),
              f"one more: only that one followed: {ha.subs[n:]}")
        # listed, removed and listed again on the same link (A, B, A): still followed, so not subscribed twice; the last
        # state Home Assistant sent of it counts at once (the Sonos plays)
        n2 = len(ha.subs); a.page.set(arbitration_media_players=RADIO); a.page.set(arbitration_media_players=f"{SONOS},{RADIO}"); await asyncio.sleep(0.3)
        m = media(a)
        check(ha.subs[n2:] == [] and ha.subs.count((a.name, SONOS)) == 1 and m.get("here") == f"{SONOS} playing"
              and m.get("players") == [{"id": SONOS, "state": "playing"}, {"id": RADIO, "state": "stopped"}],
              f"listed again on the same link: no second subscription, its last state at once: {ha.subs[n2:]} {m}")
        check(sorted(x[1] for x in ha.attr_subs if x[0] == a.name) == sorted([SONOS, RADIO]), f"nor of its volume: {ha.attr_subs}")
        # an id Home Assistant does not have (a typo): it sends nothing, and after a while the page says so
        GHOST = "media_player.kitchen_sonso"
        a.page.set(arbitration_media_players=f"{SONOS},{GHOST}"); await asyncio.sleep(0.2)
        m1 = media(a); await asyncio.sleep(1.2); m2 = media(a)
        check((a.name, GHOST) in ha.subs and m1.get("linked") is True
              and m1.get("players") == [{"id": SONOS, "state": "playing"}, {"id": GHOST, "state": "unknown"}]
              and m2.get("players") == [{"id": SONOS, "state": "playing"}, {"id": GHOST, "state": "missing"}],
              f"an entity Home Assistant does not report: unknown at first, then missing: {m1} {m2}")
        long_ = [f"media_player.{i}" + "z" * 79 for i in range(6)]        # the longest ids there are: 8 of them
        full = ",".join([SONOS, RADIO] + long_)
        a.page.set(arbitration_media_players=full); await asyncio.sleep(0.5)

        # a restart: the stored list followed by itself; the Sonos plays already, so it counts at once.  Music Assistant
        # can play on it from now on (Sendspin)
        a.sendspin = 16965; open(os.path.join(a.state, "sendspin.unpaired"), "w").write("1")
        await asyncio.sleep(1.2)                        # the TV's hold over
        await a.cli.disconnect(); ha.drop(a); a.proc.terminate(); a.proc.wait(); a.start(); await asyncio.sleep(0.5); await a.connect(ha)
        a.page = Browser(a.web); a.page.login_with_button(a.proc)
        ok = await until(lambda: {(a.name, x) for x in full.split(",")} <= set(ha.subs), 3)
        check(ok and setting(a, "arbitration_media_players").get("value") == full and f"following {SONOS}, {RADIO}, {long_[0]}" in a.text(),
              f"restarted: all 8 (the longest ids) followed by itself from state/config: {len([x for x in ha.subs if x[0] == a.name])}")
        await until(lambda: a.st("arbitration_peers") == 1, 10)
        won, sc, _ = await round_()
        check(won == "a" and sc == 1102, f"the Sonos played all along: its state at once, precedence at once (25 % kept): {won} {sc}")

        # Home Assistant gone: what played counts as stopped, for the hold
        await a.cli.disconnect(); ha.drop(a); await asyncio.sleep(0.3)
        m = media(a)
        check(re.fullmatch(rf"{SONOS} stopped [01] s ago", m.get("here") or "") and "counts as stopped, Home Assistant is gone" in a.text(),
              f"Home Assistant gone: the Sonos counts as stopped: {m.get('here')}")
        await asyncio.sleep(1.5)
        m = media(a)
        check(m.get("here") is None and m.get("linked") is False and {x["state"] for x in m.get("players", [])} == {"unknown"},
              f"after the hold: no media, not linked, states unknown (not missing): {m}")
        ha.media[SONOS] = "idle"                        # meanwhile, with nobody following
        await a.connect(ha); await asyncio.sleep(0.5)
        won, sc, _ = await round_()
        check(won == "b" and sc == 500, f"back, the Sonos idle now: no precedence: {won} {sc}")
        a.cli.send_home_assistant_state("media_player.bathroom_radio", "", "playing"); await asyncio.sleep(0.3)
        won, sc, _ = await round_()
        check(won == "b" and sc == 500 and "bathroom_radio" not in a.text(), f"a player not listed: no effect: {won} {sc}")

        # an Echo in a conversation keeps the next wake word against 10 dB
        a.page.set(arbitration_media_ambient=10); ha.set_media(SONOS, "playing"); await asyncio.sleep(0.3)
        nb = len(b.starts); b.wake(); await until(lambda: len(b.starts) == nb + 1, 3)
        na = len(a.starts); a.wake(); b.wake(); await asyncio.sleep(1.5)
        check(len(a.starts) == na and len(b.starts) == nb + 1 and "score 500 -> 1500" in a.text(), "in a conversation: priority beats media (10 % = +10 dB)")
        b.end_pipeline(); await asyncio.sleep(0.5)

        # an Echo of an older build sees a number: the hallway Echo's plain claim just above or below
        a.page.set(arbitration_media_ambient=50); fbeacon(); await asyncio.sleep(0.5)
        res = []
        for fs in (851, 751):
            drain(); n = len(a.starts); a.wake(); t = await a_claimed()
            if t: fclaim(fs)
            await asyncio.sleep(1.7)
            res.append(len(a.starts) > n)
            if len(a.starts) > n: a.end_pipeline(); await asyncio.sleep(0.5)
        check(res == [False, True], f"another Echo's claim, a number as ever: 851 beats the kitchen's 801, 751 does not: {res}")
        fbeacon(b"\x01"); await asyncio.sleep(0.5)

        # Kiosk Satellite mode: e carries it as well (5 dB + 3 dB)
        kio = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        kio.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); kio.bind(("", KIOSK)); kio.setblocking(False)
        for e in (a, b): e.page.set(arbitration_mode="kiosk")
        await until(lambda: all(((e.page.state() or {}).get("arbitration") or {}).get("kiosk_port") for e in (a, b)), 3)
        await asyncio.sleep(0.5)
        while True:
            try: kio.recv(1024)
            except BlockingIOError: break
        nb = len(b.starts); a.wake(); b.wake(); await asyncio.sleep(1.5)
        es = []
        while True:
            try: es.append(json.loads(kio.recv(1024))["e"])
            except BlockingIOError: break
        check(sorted(set(es)) == [8.0, 9.0] and len(b.starts) == nb + 1, f"Kiosk Satellite mode: the kitchen Echo claims 8.0 dB with the Sonos playing: {sorted(set(es))}")
        if len(b.starts) > nb: b.end_pipeline()
        for e in (a, b): e.page.set(arbitration_mode="hassmic")
        await until(lambda: not any(((e.page.state() or {}).get("arbitration") or {}).get("kiosk_port") for e in (a, b)), 3)
        kio.close(); await asyncio.sleep(0.5)

        # its own music: Music Assistant plays on the kitchen Echo (Sendspin), then stops
        ha.set_media(SONOS, "idle"); a.page.set(arbitration_media_ambient=25); await asyncio.sleep(1.6)
        loop = asyncio.get_running_loop(); added = asyncio.Queue()
        server = SendspinServer(loop, Identity.generate(), "fake-ma", pairing_store=InMemoryServerPairingStore())
        server.add_event_listener(lambda _s, ev: added.put_nowait(ev.client_id) if isinstance(ev, ClientAddedEvent) else None)
        playing = asyncio.Event(); stop = asyncio.Event()
        async def play(client):
            fmt = AudioFormat(sample_rate=48000, bit_depth=16, channels=2); stream = client.group.start_stream(); k = 0
            while not stop.is_set():
                s = (4000 * np.sin(2 * np.pi * 440 * np.arange(k, k + 4800) / 48000)).astype("<i2"); k += 4800
                stream.prepare_audio(np.repeat(s, 2).tobytes(), fmt); await stream.commit_audio(); await stream.sleep_to_limit_buffer(500_000)
                playing.set()
            await client.group.stop()
        try:
            await server.start_server(port=16966, discover_clients=False)
            server.connect_to_client(f"ws://127.0.0.1:{a.sendspin}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
            cid = await asyncio.wait_for(added.get(), 15)
            await server.trust_unpaired(cid); await asyncio.sleep(2)
            task = asyncio.create_task(play(server.get_client(cid)))
            await asyncio.wait_for(playing.wait(), 10); await asyncio.sleep(1)
            won, sc, _ = await round_()
            check(won == "a" and sc == 1102 and "media of this room (own playback): ambient counted at 25 %" in a.text()
                  and re.search(rf"media: own playback off -> on, {ROOM}$", a.text(), re.M) and re.search(rf"media: own playback on 1 s, {ROOM}$", a.text(), re.M),
                  f"Music Assistant plays on it: its own playback counts (and is logged): {won} {sc}")
            stop.set(); await task; await asyncio.sleep(0.5)
            won, sc, _ = await round_()
            check(won == "a" and re.search(r"media of this room \(own playback ended [01] s ago\)", a.text()), f"stopped: counts for the hold: {won} {sc}")
            won, sc, _ = await round_()
            check(won == "b" and sc == 500, f"after the hold: no more: {won} {sc}")
        except Exception as ex:
            check(False, f"Sendspin on the kitchen Echo: {ex!r}")
        finally:
            await server.close()
        sn.close()
    finally:
        for e in (a, b): e.proc.terminate()
        for e in (a, b): e.proc.wait()
    if check.failed:
        for e in (a, b): print(f"--- {e.name}\n{e.text()}")
        print("FAILED"); raise SystemExit(1)
    print("PASS")

asyncio.run(main())
