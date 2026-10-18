"""What the wake word arbitration tests (tests/fake_ha_arbitration*.py) share: Echos (build/hassmic-host) under a fake
Home Assistant that does what the real one does with ESPHome devices: it connects with each device's API key, fires a
device's events whatever its permission (esphome.tag_scanned: the tag integration keeps tag.<tag id>, state the last
scan's time), answers any device's request for a state (no permission needed), and runs an action a device asks for
("Allow the device to perform Home Assistant actions") that names another device's own action
(esphome.<node>_arbitration_key) on that device.
Everything goes out as loopback broadcast (HASSMIC_ARB_ADDR): nothing of it reaches the LAN.  The network's own pace
(beacons, discovery, tags, key handoff, pairing) runs at SCALE of the real one (HASSMIC_ARB_SCALE, arb.c PACE): at
the real one a join by the action alone waits out half a minute.  Rounds and the window keep their real times."""
import asyncio, base64, datetime, os, re, signal, subprocess, sys, tempfile, time
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import Browser
ARB, KIOSK, BCAST = 28990, 28991, "127.255.255.255"          # KIOSK: stands in for Kiosk Satellite's 2330
SCALE = 0.1
def pace(secs): return secs * SCALE                          # one of arb.c's PACE() times, in s here
SCAN, POLL, ATTEST_WAIT, DISCOVER, BEACON_MAX = pace(10), pace(3), pace(30), pace(5), pace(40)


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def until(cond, secs):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if cond(): return True
        await asyncio.sleep(0.1)
    return cond()


class Echo:
    def __init__(self, name, port, score, net=None):
        """net: the network id it starts in (state/arbitration as a join left it), with a fresh key unless given one"""
        self.name, self.port, self.state = name, port, tempfile.mkdtemp()
        self.web = port + 10                                # the settings page
        self.node = re.sub(r"[^a-z0-9]", "-", name.lower())
        self.log = os.path.join(self.state, "log")
        self.api_key = base64.b64encode(os.urandom(32)).decode()
        with open(os.path.join(self.state, "api_key"), "w") as f: f.write(self.api_key + "\n")     # as Home Assistant provisioned it
        self.env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_SETTINGS=os.path.join(self.state, "settings"),
                        HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(self.state, "play.raw"),
                        HASSMIC_MDNS_FILE=os.path.join(self.state, "none"), HASSMIC_ARB_ADDR=BCAST, HASSMIC_KIOSK_PORT=str(KIOSK),
                        HASSMIC_TEST_SCORE=str(score), HASSMIC_MODELS=os.path.join(self.state, "models"), HASSMIC_ARB_SCALE=str(SCALE))
        os.makedirs(os.path.join(self.state, "models", "echo-de"))
        open(os.path.join(self.state, "models", "echo-de", "pryon.manifest"), "w").close()
        if net is not None:
            nid, key = net if isinstance(net, tuple) else (net, os.urandom(32))
            with open(os.path.join(self.state, "arbitration"), "w") as f:
                f.write(f"join 1\nctr 0\nnet {nid:016x} {base64.b64encode(key).decode()}\n")
        self.starts, self.states, self.allowed, self.chunks = [], {}, True, []

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.port), "-n", self.name, "-L",
                                      "-z", "0", "-o", "0", "-a", str(ARB), "-W", str(self.web)], env=self.env, stderr=open(self.log, "a"))

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
        self.cli = APIClient("127.0.0.1", self.port, None, noise_psk=self.api_key)
        await self.cli.connect(login=True)
        ents, self.services = await self.cli.list_entities_services()
        self.by = {e.object_id: e for e in ents}
        self.cli.subscribe_states(lambda s: self.states.__setitem__(s.key, getattr(s, "state", None)))
        self.cli.subscribe_home_assistant_states_and_services(on_state=lambda s: None, on_service_call=lambda c: ha.action(self, c),
                                                              on_state_sub=lambda e, a: None, on_state_request=lambda e, a: ha.state(self, e, a))
        async def handle_start(conv, flags, settings, phrase): self.starts.append(time.monotonic()); self.chunks = []; return 0
        async def handle_stop(abort): pass
        async def handle_audio(data, *_): self.chunks.append(len(data))
        self.cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio)

    def end_pipeline(self): self.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)


class HA:
    """Home Assistant's esphome manager: device actions run only with the option ticked (otherwise a repair), and
    esphome.<device_info.name with '_'>_<service> is each device's own action."""
    def __init__(self, echos): self.echos, self.calls, self.refused, self.tags_on, self.asked, self.tags, self.scans = echos, [], [], True, [], {}, []
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


async def start(echos, ha, login=True):
    """started, each settings page logged in with the action button, Home Assistant connected"""
    for e in echos: e.start()
    await asyncio.sleep(0.5)
    if login:
        for e in echos: e.page = Browser(e.web); check(e.page.login_with_button(e.proc), f"{e.name}: settings page logged in with the action button")
    for e in echos: await e.connect(ha)


def run(main, echos):
    """main() under asyncio; the Echos stopped whatever happens, their logs printed on a failure"""
    try: asyncio.run(main())
    except Exception:
        import traceback; traceback.print_exc(); check.failed = True
    finally:
        for e in echos:
            if hasattr(e, "proc"): e.proc.terminate()
        for e in echos:
            if hasattr(e, "proc"): e.proc.wait()
    if check.failed:
        for e in echos: print(f"--- {e.name}\n{e.text()}")
        print("FAILED"); raise SystemExit(1)
    print("PASS")
