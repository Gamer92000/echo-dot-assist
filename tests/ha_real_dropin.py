#!/usr/bin/env python3
"""The Drop In voice blueprint in a real Home Assistant, against build/hassmic-host Echos, end to end: sentences go to
/api/conversation/process as if said to an Echo, the blueprint finds the Echos, calls one through the ESPHome action
(in a run of its own), reads what came of it, and answers; the test checks the reply, how long it took, and that the
call started or not on both Echos.

Isolated: Home Assistant, the Echos and this driver run in a network namespace of their own (unshare -rn), whose only
interface is its own loopback; nothing can be reached from the LAN, and nothing of it reaches the LAN.  Home Assistant
gets a temporary config directory, binds 127.0.0.1 (its http settings live in .storage since 2026.10), is onboarded
through its API, and adds the Echos through the ESPHome config flow with their keys.

Echos: Office and Hall (this build), Küche (the released build of HASSMIC_OLD_REF, built from git: Echos that only
say "Drop In"), Guest (this build, a name whose slug in Home Assistant is not its node: the blueprint's second action
name).  Areas Office, Küche, Hall.

Home Assistant: its own venv, not .venv (about 1 GB with what its integrations need):
    .venv/bin/python tests/ha_real_dropin.py --setup        # build/ha-venv: homeassistant==HA_VERSION and the
                                                             # requirements of the integrations it loads here (uv)
    .venv/bin/python tests/ha_real_dropin.py                 # skips (exit 0) when there is no build/ha-venv,
                                                             # or when user namespaces are not allowed
HASSMIC_HA_PYTHON names another Python with Home Assistant; HASSMIC_OLD_REF another released build (none: no old Echo,
every Echo of this build); HASSMIC_HA_REQUIRED=1 makes a skip a failure (CI)."""
import asyncio, base64, json, os, shutil, signal, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HA_VERSION = "2026.10.1"
HA_PY = os.environ.get("HASSMIC_HA_PYTHON") or os.path.join(ROOT, "build", "ha-venv", "bin", "python")
OLD_REF = os.environ.get("HASSMIC_OLD_REF", "v2026.10.08.213741-beta")
HA_PORT, ARB = 18123, 28996
HA = f"http://127.0.0.1:{HA_PORT}"
# what Home Assistant sets up here besides what it always does (bootstrap.py); their requirements go into the venv
CONFIGURED = ["api", "config", "onboarding", "conversation", "automation", "esphome", "assist_satellite",
              "google_translate"]                     # onboarding sets up its TTS


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond: check.failed = True
check.failed = False


# ------------------------------------------------------------------ setup: Home Assistant's venv

COLLECT = r'''
import json, pathlib, sys, homeassistant, homeassistant.bootstrap as b
comp = pathlib.Path(homeassistant.__file__).parent / "components"
todo = set(sys.argv[1:]) | b.CORE_INTEGRATIONS | b.LOGGING_AND_HTTP_DEPS_INTEGRATIONS | b.FRONTEND_INTEGRATIONS
todo |= {"labs", "recorder", "zeroconf"} | set(b.DISCOVERY_INTEGRATIONS) | b.DEFAULT_INTEGRATIONS
seen, reqs = set(), set()
while todo:
    d = todo.pop()
    if d in seen or not (comp / d / "manifest.json").exists(): continue
    seen.add(d)
    m = json.loads((comp / d / "manifest.json").read_text())
    reqs |= set(m.get("requirements", []))
    todo |= set(m.get("dependencies", [])) | set(m.get("after_dependencies", []))
print("\n".join(sorted(reqs)))
'''


def setup():
    venv = os.path.dirname(os.path.dirname(HA_PY))
    uv = shutil.which("uv") or sys.exit("--setup needs uv (https://docs.astral.sh/uv/)")
    if not os.path.exists(HA_PY):
        subprocess.run([uv, "venv", "--python", "3.14", venv], check=True)
    subprocess.run([uv, "pip", "install", "--python", HA_PY, f"homeassistant=={HA_VERSION}"], check=True)
    reqs = subprocess.run([HA_PY, "-c", COLLECT, *CONFIGURED], check=True, capture_output=True, text=True).stdout.split()
    cons = subprocess.run([HA_PY, "-c", "import homeassistant, os; print(os.path.join(os.path.dirname(homeassistant.__file__), "
                           "'package_constraints.txt'))"], check=True, capture_output=True, text=True).stdout.strip()
    req = os.path.join(venv, "requirements-integrations.txt")
    open(req, "w").write("\n".join(reqs) + "\n")
    subprocess.run([uv, "pip", "install", "--python", HA_PY, "-c", cons, "-r", req], check=True)
    print(f"Home Assistant {HA_VERSION} in {venv}, with {len(reqs)} requirements of its integrations")


# ------------------------------------------------------------------ the namespace

def loopback_up():
    """lo up, and multicast on it: Home Assistant's zeroconf (which the ESPHome config flow and aiohttp's resolver
    need) joins the mDNS group at start and fails without an interface that takes it (ENODEV)"""
    subprocess.run(["ip", "link", "set", "lo", "multicast", "on", "up"], check=True)
    subprocess.run(["ip", "route", "add", "224.0.0.0/4", "dev", "lo"], check=True)


def interfaces():
    """this namespace's (/sys/class/net shows those of whoever mounted sysfs)"""
    import socket
    return sorted(n for _, n in socket.if_nameindex())


def old_build():
    """the released hassmic-host of OLD_REF, built from git once (build/old/<ref>)"""
    if OLD_REF in ("", "none"): return None
    out = os.path.join(ROOT, "build", "old", OLD_REF)
    binary = os.path.join(out, "build", "hassmic-host")
    if os.path.exists(binary): return binary
    if subprocess.run(["git", "-C", ROOT, "rev-parse", "-q", "--verify", OLD_REF + "^{commit}"], capture_output=True).returncode:
        print(f"note: {OLD_REF} is not in this clone (git fetch --tags): every Echo is this build")
        return None
    os.makedirs(out, exist_ok=True)
    tar = subprocess.Popen(["git", "-C", ROOT, "archive", OLD_REF], stdout=subprocess.PIPE)
    subprocess.run(["tar", "-x", "-C", out], stdin=tar.stdout, check=True); tar.wait()
    r = subprocess.run(["make", "-C", out, "build/hassmic-host"], capture_output=True, text=True)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); sys.exit(f"cannot build {OLD_REF}")
    return binary


# ------------------------------------------------------------------ Echos and Home Assistant

class Echo:
    def __init__(self, tmp, name, mac, port, net, binary):
        self.name, self.mac, self.port, self.binary = name, mac, port, binary
        self.state = os.path.join(tmp, f"echo-{port}")
        os.makedirs(os.path.join(self.state, "net", "wlan0"))
        self.key = base64.b64encode(os.urandom(32)).decode()
        open(os.path.join(self.state, "api_key"), "w").write(self.key + "\n")
        open(os.path.join(self.state, "arbitration"), "w").write(f"arbitrate 1\nctr 0\nnet {net}\n")
        open(os.path.join(self.state, "net", "wlan0", "address"), "w").write(mac + "\n")    # name_make, HA's MAC
        self.log = os.path.join(self.state, "log")
        self.proc = None

    def start(self):
        s = self.state
        env = dict(os.environ, HASSMIC_STATE=s, HASSMIC_PLAY=f"{s}/play.raw", HASSMIC_VOIP=f"{s}/voip.raw",
                   HASSMIC_MDNS_FILE=f"{s}/none", HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_TEST_SCORE="500",
                   HASSMIC_MODELS=f"{s}/models")
        # its own mount namespace: /sys/class/net/wlan0/address is this Echo's MAC address
        cmd = (f"mount --bind {s}/net /sys/class/net && exec {self.binary} -P esphome -p {self.port} -n '{self.name}' -L "
               f"-z 0 -o 0 -a {ARB} -i {self.port + 20} -W {self.port + 10}")
        self.proc = subprocess.Popen(["unshare", "-m", "sh", "-c", cmd], env=env, stderr=open(self.log, "a"), stdout=subprocess.DEVNULL)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try: self.proc.wait(5)
            except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()

    def text(self): return open(self.log, errors="replace").read()


class Client:
    def __init__(self, session, token):
        self.s, self.h, self.id, self.changes = session, {"Authorization": f"Bearer {token}"}, 0, []

    async def ws_open(self):
        self.sock = await self.s.ws_connect(f"{HA}/api/websocket")
        await self.sock.receive_json()
        await self.sock.send_json({"type": "auth", "access_token": self.h["Authorization"][7:]})
        assert (await self.sock.receive_json())["type"] == "auth_ok"
        # every state change, in the order Home Assistant had them, on a link of its own
        self.events = await self.s.ws_connect(f"{HA}/api/websocket")
        await self.events.receive_json()
        await self.events.send_json({"type": "auth", "access_token": self.h["Authorization"][7:]})
        await self.events.receive_json()
        await self.events.send_json({"id": 1, "type": "subscribe_events", "event_type": "state_changed"})
        asyncio.ensure_future(self._listen())

    async def _listen(self):
        async for m in self.events:
            d = json.loads(m.data)
            if d.get("type") == "event":
                e = d["event"]["data"]; n = e.get("new_state") or {}
                self.changes.append((time.monotonic(), e["entity_id"], n.get("state"), (n.get("attributes") or {}).get("event_type")))

    async def ws(self, **msg):
        self.id += 1
        await self.sock.send_json({"id": self.id, **msg})
        while True:
            r = await self.sock.receive_json()
            if r.get("id") == self.id: break
        if not r.get("success"): raise RuntimeError(f"{msg}: {r}")
        return r.get("result")

    async def get(self, path):
        async with self.s.get(f"{HA}{path}", headers=self.h) as r: return await r.json()

    async def post(self, path, data):
        async with self.s.post(f"{HA}{path}", headers=self.h, json=data) as r:
            t = await r.text()
            try: return json.loads(t)
            except ValueError: return t

    async def state(self, eid):
        r = await self.get(f"/api/states/{eid}")
        return r.get("state") if isinstance(r, dict) else None

    async def say(self, text, device, lang="en"):
        t = time.monotonic()
        r = await self.post("/api/conversation/process", {"text": text, "language": lang, "device_id": device})
        try: return r["response"]["speech"]["plain"]["speech"], time.monotonic() - t
        except (KeyError, TypeError): return f"?? {r}", time.monotonic() - t

    def seen(self, eid, since): return [st for t, e, st, _ in self.changes if e == eid and t >= since]


async def onboard(session):
    async with session.post(f"{HA}/api/onboarding/users", json={"client_id": f"{HA}/", "name": "Test", "username": "test",
                                                                "password": "test-test-test", "language": "en"}) as r:
        code = (await r.json())["auth_code"]
    async with session.post(f"{HA}/auth/token", data={"grant_type": "authorization_code", "code": code, "client_id": f"{HA}/"}) as r:
        tok = (await r.json())["access_token"]
    for step in ("core_config", "analytics"):
        async with session.post(f"{HA}/api/onboarding/{step}", headers={"Authorization": f"Bearer {tok}"}, json={}) as r: pass
    return tok


async def add_esphome(c, e):
    f = await c.post("/api/config/config_entries/flow", {"handler": "esphome", "show_advanced_options": False})
    for _ in range(6):
        if not isinstance(f, dict): raise RuntimeError(f"flow: {f}")
        if f.get("type") == "create_entry": return
        if f.get("type") == "abort": raise RuntimeError(f"flow aborted: {f}")
        step = f.get("step_id")
        data = {"host": "127.0.0.1", "port": e.port} if step == "user" else {"noise_psk": e.key} if step == "encryption_key" else {}
        f = await c.post(f"/api/config/config_entries/flow/{f['flow_id']}", data)
    raise RuntimeError(f"flow did not finish: {f}")


async def until(cond, secs):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if await cond(): return True
        await asyncio.sleep(0.1)
    return False


def write_config(conf):
    os.makedirs(os.path.join(conf, ".storage"))
    os.makedirs(os.path.join(conf, "blueprints", "automation", "hassmic"))
    shutil.copy(os.path.join(ROOT, "blueprints", "automation", "drop_in.yaml"), os.path.join(conf, "blueprints", "automation", "hassmic"))
    open(os.path.join(conf, "configuration.yaml"), "w").write(
        "homeassistant:\n  name: Test\n  unit_system: metric\n  time_zone: UTC\n  latitude: 0\n  longitude: 0\n  elevation: 0\n"
        "  country: DE\n  internal_url: http://127.0.0.1:%d\napi:\nconfig:\nonboarding:\nconversation:\n"
        "automation: !include automations.yaml\nlogger:\n  default: warning\n  logs:\n    homeassistant.components.esphome: info\n"
        % HA_PORT)
    open(os.path.join(conf, "automations.yaml"), "w").write(
        "- id: echo_drop_in_test\n  alias: Echo Drop In\n  trace:\n    stored_traces: 100\n  use_blueprint:\n"
        "    path: hassmic/drop_in.yaml\n    input: {}\n")
    # Home Assistant 2026.10 keeps http's settings here (an http: block in YAML is migrated into it): 127.0.0.1 only
    json.dump({"version": 2, "minor_version": 2, "key": "http", "data": {
        "stable": {"server_port": HA_PORT, "server_host": ["127.0.0.1"], "cors_allowed_origins": [], "login_attempts_threshold": -1,
                   "ip_ban_enabled": False, "ssl_profile": "modern", "use_x_frame_options": True,
                   "created_at": "2026-10-11T00:00:00+00:00", "error": None, "error_message": None},
        "pending": None, "yaml_migration_done": True}}, open(os.path.join(conf, ".storage", "http"), "w"))


async def run(tmp, old):
    import aiohttp
    conf = os.path.join(tmp, "ha")
    write_config(conf)
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    new = os.path.join(ROOT, "build", "hassmic-host")
    office = Echo(tmp, "Echo Dot 3 a1b2c3", "02:00:00:a1:b2:c3", 26190, net, new)
    kitchen = Echo(tmp, "Echo Dot 3 d4e5f6", "02:00:00:d4:e5:f6", 26191, net, old or new)
    guest = Echo(tmp, "Echo Dot 3 0a0b0cŁ", "02:00:00:0a:0b:0c", 26192, net, new)     # its slug is not its node
    hall = Echo(tmp, "Echo Dot 3 7a8b9c", "02:00:00:7a:8b:9c", 26193, net, new)
    echos = (office, kitchen, guest, hall)
    halog = open(os.path.join(tmp, "ha.log"), "w")
    hass = subprocess.Popen([HA_PY, "-m", "homeassistant", "-c", conf, "--skip-pip"], stdout=halog, stderr=subprocess.STDOUT)
    for e in echos: e.start()
    try:
        async with aiohttp.ClientSession() as session:
            for _ in range(240):
                try:
                    async with session.get(f"{HA}/api/onboarding") as r:
                        if r.status == 200: break
                except aiohttp.ClientError: pass
                if hass.poll() is not None: raise RuntimeError("Home Assistant ended")
                await asyncio.sleep(0.5)
            c = Client(session, await onboard(session)); await c.ws_open()
            check(interfaces() == ["lo"], f"Home Assistant up; the namespace has no interface but loopback ({interfaces()})")
            for e in echos: await add_esphome(c, e)
            for _ in range(100):
                devs = {d["name"]: d for d in await c.ws(type="config/device_registry/list") if d.get("manufacturer") == "hassmic"}
                if len(devs) == len(echos): break
                await asyncio.sleep(0.2)
            check(len(devs) == len(echos), f"the Echos in Home Assistant: {sorted(devs)}")
            od, kd, gd, hd = (devs[e.name]["id"] for e in echos)
            ents = await c.ws(type="config/entity_registry/list")
            def ent(d, kind, suffix): return next((x["entity_id"] for x in ents if x["device_id"] == d and x["entity_id"].startswith(kind + ".")
                                                   and x["entity_id"].endswith(suffix)), None)
            osens, ksens, hsens, gsens = (ent(d, "sensor", "_drop_in") for d in (od, kd, hd, gd))
            ores, kres = ent(od, "event", "_drop_in_result"), ent(kd, "event", "_drop_in_result")
            check(osens and ksens and hsens and ores, f"Drop In sensors and the office's Drop In result: {osens} {ksens} {hsens} {ores}")
            check((kres is None) == bool(old), f"  the released build has no Drop In result ({kres})")
            areas = {}
            for n in ("Office", "Küche", "Hall"): areas[n] = (await c.ws(type="config/area_registry/create", name=n))["area_id"]
            for d, a in ((od, "Office"), (kd, "Küche"), (hd, "Hall")): await c.ws(type="config/device_registry/update", device_id=d, area_id=areas[a])
            # the office's result entity renamed: the blueprint finds it by its event types
            await c.ws(type="config/entity_registry/update", entity_id=ores, new_entity_id="event.buero_anrufe")
            ores = "event.buero_anrufe"
            for s in (osens, ksens, hsens, gsens):
                check(await until(lambda s=s: _is(c, s, "idle"), 15), f"{s} idle")
            await asyncio.sleep(3)                      # the Echos find each other in their network

            for _ in range(60):                         # the automation's trigger is attached once HA has started
                r, t = await c.say("drop in nowhere", od)
                if r == "There is no Echo in nowhere": break
                await asyncio.sleep(1)
            check(r == "There is no Echo in nowhere", f"the blueprint answers: {r!r}")
            end = {d: ent(d, "button", "_end_drop_in") for d in (od, kd, gd, hd)}
            async def hang_up(d, sens):
                await c.post("/api/services/button/press", {"entity_id": end[d]})
                await until(lambda: _is(c, sens, "idle"), 8); await asyncio.sleep(1)

            # calls that start
            r, t = await c.say("drop in küche", od)
            check(r == "Calling Küche" and t < 1.5, f"'drop in küche' to the office (this build, the old one called): {r!r} in {t:.2f} s")
            check(await until(lambda: _is(c, ksens, "connected"), 3) and await c.state(osens) == "connected", "  connected on both")
            r, t = await c.say("drop in hall", od)
            check(r == "This Echo is in a call already", f"  again during the call: {r!r}")
            await hang_up(od, osens)
            r, t = await c.say("verbinde mit hall", kd, "de")
            check(r == "Ich verbinde mit Hall" and await until(lambda: _is(c, hsens, "connected"), 3),
                  f"German, the released build calling this one: {r!r} in {t:.2f} s")
            await hang_up(kd, ksens)
            r, t = await c.say("drop in office", od)
            check(r == "This is the Echo in Office", f"its own room: {r!r}")

            # refusals: told at once, and why; the office's "Drop In" showed calling first
            dnd = {d: ent(d, "switch", "_do_not_disturb") for d in (kd, hd)}
            await c.post("/api/services/switch/turn_on", {"entity_id": dnd[hd]}); await asyncio.sleep(0.5)
            t0 = time.monotonic()
            r, t = await c.say("drop in hall", od)
            check(r == "Hall has do not disturb on" and t < 1.5, f"Hall in do not disturb: {r!r} in {t:.2f} s")
            check(c.seen(osens, t0)[:2] == ["calling", "idle"], f"  the office's Drop In: {c.seen(osens, t0)}")
            check([x[3] for x in c.changes if x[1] == ores and x[0] >= t0] == ["do_not_disturb"], "  and its Drop In result: do_not_disturb")
            r, t = await c.say("drop in hall", kd)
            check(r == "The call to Hall did not go through" if old else r == "Hall has do not disturb on",
                  f"  the same from Küche ({'released' if old else 'this'} build): {r!r} in {t:.2f} s")
            await c.post("/api/services/switch/turn_off", {"entity_id": dnd[hd]})
            await c.post("/api/services/switch/turn_on", {"entity_id": dnd[kd]}); await asyncio.sleep(0.5)
            r, t = await c.say("drop in küche", od)
            check(r == "Küche has do not disturb on" and t < 1.5, f"Küche ({'released' if old else 'this'} build) in do not disturb: {r!r} in {t:.2f} s")
            await c.post("/api/services/switch/turn_off", {"entity_id": dnd[kd]})
            mute = ent(od, "switch", "_mute")
            await c.post("/api/services/switch/turn_on", {"entity_id": mute}); await asyncio.sleep(0.5)
            r, t = await c.say("drop in hall", od)
            check(r == "This Echo's microphones are off" and t < 1.5, f"the office muted: {r!r} in {t:.2f} s")
            await c.post("/api/services/switch/turn_off", {"entity_id": mute}); await asyncio.sleep(0.5)

            # the guest's action is only under its second name: the first is not found in a run of its own
            r, t = await c.say("drop in hall", gd)
            check(r == "Calling Hall" and 1.9 < t < 3.5, f"'drop in hall' to the guest (action under model + MAC): {r!r} in {t:.2f} s")
            check(await until(lambda: _is(c, hsens, "connected"), 3), "  connected")
            await hang_up(hd, hsens)

            # gone: Home Assistant shows it unavailable, the office still has it in its network
            hall.stop()
            check(await until(lambda: _is(c, hsens, "unavailable"), 10), "Hall stopped: unavailable")
            r, t = await c.say("drop in hall", od)
            check(r == "Hall is offline" and 3.9 < t < 6, f"  'drop in hall': {r!r} in {t:.2f} s")

            tr = await c.ws(type="trace/list", domain="automation", item_id="echo_drop_in_test")
            errs = sorted({x.get("error") for x in tr if x.get("error")})
            check(errs == ["Action esphome.echo_dot_3_0a0b0cl_drop_in not found"],
                  f"{len(tr)} runs; with an error only the guest's call run under the first name: {errs}")
            repairs = [i["issue_id"] for i in (await c.ws(type="repairs/list_issues")).get("issues", [])
                       if i["issue_id"] != "deprecated_method"]           # Home Assistant Core installs, any
            check(repairs == ["automation.echo_drop_in_service_not_found_esphome.echo_dot_3_0a0b0cl_drop_in"],
                  f"  and a repair naming that action: {repairs}")
    finally:
        for e in echos: e.stop()
        hass.send_signal(signal.SIGTERM)
        try: hass.wait(30)
        except subprocess.TimeoutExpired: hass.kill(); hass.wait()
        if check.failed:
            for e in echos: print(f"--- {e.name} log tail\n" + "".join(e.text().splitlines(True)[-25:]))
            print("--- Home Assistant log tail\n" + "".join(open(os.path.join(tmp, "ha.log"), errors="replace").readlines()[-40:]))


async def _is(c, eid, st): return await c.state(eid) == st


def skip(why):
    """exit 0, unless HASSMIC_HA_REQUIRED is set (CI: a skip there is a failure)"""
    print(("FAILED, not run: " if os.environ.get("HASSMIC_HA_REQUIRED") else "skipped: ") + why)
    sys.exit(1 if os.environ.get("HASSMIC_HA_REQUIRED") else 0)


def main():
    if "--setup" in sys.argv: setup(); return
    if os.environ.get("HASSMIC_IN_NETNS") != "1":
        if not os.path.exists(HA_PY):
            skip(f"no Home Assistant at {HA_PY} (.venv/bin/python tests/ha_real_dropin.py --setup makes one)")
        if not os.path.exists(os.path.join(ROOT, "build", "hassmic-host")): sys.exit("make host first")
        if not shutil.which("ip"): skip("no ip (iproute2)")
        if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode:
            skip("unshare -rn is not allowed here (user namespaces; Ubuntu: sysctl kernel.apparmor_restrict_unprivileged_userns=0)")
        old = old_build()
        env = dict(os.environ, HASSMIC_IN_NETNS="1", HASSMIC_OLD_HOST=old or "")
        sys.exit(subprocess.run(["unshare", "-rn", sys.executable, os.path.abspath(__file__), *sys.argv[1:]], env=env).returncode)
    loopback_up()
    tmp = tempfile.mkdtemp(prefix="ha-dropin-")
    try: asyncio.run(run(tmp, os.environ.get("HASSMIC_OLD_HOST") or None))
    finally: shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
