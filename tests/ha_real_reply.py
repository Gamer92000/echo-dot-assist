#!/usr/bin/env python3
"""Replies on another speaker (replyspk.c) in a real Home Assistant, the parts only it can show: the template that finds
the media player of an Echo's area (its own device by MAC, its area, the other players there), the actions answered
with a call id ("Allow the device to perform Home Assistant actions" off: no answer, a repair; on: answered), and
media_player.play_media as an announcement reaching that player, which plays it.

Three Echos (build/hassmic-host): Bath and Speaker in the area Bath, Kitchen in Kitchen.  Bath's announcements go to
"the speaker in this room": Home Assistant names Speaker's media player (not Bath's own, not Kitchen's), and an
announcement to Bath's satellite plays on Speaker, through Home Assistant's media proxy (its ffmpeg: imageio-ffmpeg's,
which --setup adds to build/ha-venv), with nothing of it on Bath.

Isolated as tests/ha_real_dropin.py: a network namespace of its own with only loopback, Home Assistant in
build/ha-venv (that test's --setup; this one's adds ffmpeg).  Skips (exit 0) without them; HASSMIC_HA_REQUIRED=1 makes
a skip a failure."""
import asyncio, base64, io, json, math, os, shutil, signal, struct, subprocess, sys, tempfile, threading, time, wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import ha_real_dropin as hr
from webclient import Browser
HTTP, RATE = 26399, 16000


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def wav(secs):
    n = int(RATE * secs); b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(6000 * math.sin(2 * math.pi * 440 * i / RATE))) for i in range(n)))
    return b.getvalue()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = wav(2.0)
        self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a): pass


class Echo(hr.Echo):
    """ha_real_dropin's, with a port of its own for other Echos' replies (-r), and its play file to read"""
    def start(self):
        s = self.state
        env = dict(os.environ, HASSMIC_STATE=s, HASSMIC_PLAY=f"{s}/play.raw", HASSMIC_VOIP=f"{s}/voip.raw",
                   HASSMIC_MDNS_FILE=f"{s}/none", HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_TEST_SCORE="500",
                   HASSMIC_MODELS=f"{s}/models")
        open(f"{s}/play.raw", "wb").close()
        cmd = (f"mount --bind {s}/net /sys/class/net && exec {self.binary} -P esphome -p {self.port} -n '{self.name}' -L "
               f"-z 0 -o 0 -a {hr.ARB} -i {self.port + 20} -r {self.port + 30} -W {self.port + 10}")
        self.proc = subprocess.Popen(["unshare", "-m", "sh", "-c", cmd], env=env, stderr=open(self.log, "a"), stdout=subprocess.DEVNULL)
    def played(self): return os.path.getsize(f"{self.state}/play.raw")


def ffmpeg_bin():
    r = subprocess.run([hr.HA_PY, "-c", "import imageio_ffmpeg as f; print(f.get_ffmpeg_exe())"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def write_config(conf, ffmpeg):
    os.makedirs(os.path.join(conf, ".storage"))
    open(os.path.join(conf, "configuration.yaml"), "w").write(
        "homeassistant:\n  name: Test\n  unit_system: metric\n  time_zone: UTC\n  latitude: 0\n  longitude: 0\n  elevation: 0\n"
        "  country: DE\n  internal_url: http://127.0.0.1:%d\napi:\nconfig:\nonboarding:\nconversation:\n"
        "ffmpeg:\n  ffmpeg_bin: %s\nlogger:\n  default: warning\n  logs:\n    homeassistant.components.esphome: info\n" % (hr.HA_PORT, ffmpeg))
    json.dump({"version": 2, "minor_version": 2, "key": "http", "data": {
        "stable": {"server_port": hr.HA_PORT, "server_host": ["127.0.0.1"], "cors_allowed_origins": [], "login_attempts_threshold": -1,
                   "ip_ban_enabled": False, "ssl_profile": "modern", "use_x_frame_options": True,
                   "created_at": "2026-10-11T00:00:00+00:00", "error": None, "error_message": None},
        "pending": None, "yaml_migration_done": True}}, open(os.path.join(conf, ".storage", "http"), "w"))


async def run(tmp, ffmpeg):
    import aiohttp
    conf = os.path.join(tmp, "ha")
    write_config(conf, ffmpeg)
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    new = os.path.join(ROOT, "build", "hassmic-host")
    bath = Echo(tmp, "Echo Dot 3 a1b2c3", "02:00:00:a1:b2:c3", 26290, net, new)
    speaker = Echo(tmp, "Echo Dot 3 d4e5f6", "02:00:00:d4:e5:f6", 26291, net, new)
    kitchen = Echo(tmp, "Echo Dot 3 7a8b9c", "02:00:00:7a:8b:9c", 26292, net, new)
    echos = (bath, speaker, kitchen)
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP), Handler); threading.Thread(target=httpd.serve_forever, daemon=True).start()
    halog = open(os.path.join(tmp, "ha.log"), "w")
    hass = subprocess.Popen([hr.HA_PY, "-m", "homeassistant", "-c", conf, "--skip-pip"], stdout=halog, stderr=subprocess.STDOUT)
    for e in echos: e.start()
    try:
        async with aiohttp.ClientSession() as session:
            for _ in range(240):
                try:
                    async with session.get(f"{hr.HA}/api/onboarding") as r:
                        if r.status == 200: break
                except aiohttp.ClientError: pass
                if hass.poll() is not None: raise RuntimeError("Home Assistant ended")
                await asyncio.sleep(0.5)
            c = hr.Client(session, await hr.onboard(session)); await c.ws_open()
            check(hr.interfaces() == ["lo"], f"Home Assistant up; the namespace has no interface but loopback ({hr.interfaces()})")
            for e in echos: await hr.add_esphome(c, e)
            for _ in range(100):
                devs = {d["name"]: d for d in await c.ws(type="config/device_registry/list") if d.get("manufacturer") == "hassmic"}
                if len(devs) == len(echos): break
                await asyncio.sleep(0.2)
            check(len(devs) == len(echos), f"the Echos in Home Assistant: {sorted(devs)}")
            bd, sd, kd = (devs[e.name]["id"] for e in echos)
            ents = await c.ws(type="config/entity_registry/list")
            def ent(d, kind): return next((x["entity_id"] for x in ents if x["device_id"] == d and x["entity_id"].startswith(kind + ".")), None)
            bmp, smp, kmp, bsat = ent(bd, "media_player"), ent(sd, "media_player"), ent(kd, "media_player"), ent(bd, "assist_satellite")
            check(bmp and smp and kmp and bsat, f"media players and Bath's satellite: {bmp} {smp} {kmp} {bsat}")
            have = {a["name"]: a["area_id"] for a in await c.ws(type="config/area_registry/list")}    # onboarding made some
            areas = {n: have.get(n) or (await c.ws(type="config/area_registry/create", name=n))["area_id"] for n in ("Bath", "Kitchen")}
            for d, a in ((bd, "Bath"), (sd, "Bath"), (kd, "Kitchen")): await c.ws(type="config/device_registry/update", device_id=d, area_id=areas[a])

            # Bath: announcements to the room's speaker.  Actions are off for a new device in Home Assistant
            bath.page = Browser(bath.port + 10)
            check(bath.page.login_with_button(bath.proc), "Bath's settings page logged in")
            def rs(): return bath.page.state()["reply"]
            st, _, _ = bath.page.call("POST", "/api/reply", b"room")
            bath.page.set(reply_speaker_announcements="on", reply_speaker_delay="300")
            check(st == 200 and await hr.until(lambda: _now(rs()["actions"] is False), 10),
                  f"actions not allowed yet: the room's question went unanswered, the page says so: {rs()['actions']} {rs()['room']}")
            repairs = [i["issue_id"] for i in (await c.ws(type="repairs/list_issues")).get("issues", [])]
            check(any("service_calls_not_allowed" in r or r.startswith("service_calls") for r in repairs) or any("services" in r for r in repairs),
                  f"Home Assistant raised its repair for it: {repairs}")

            # allowed: the entry reloads, the link is new, the room is asked again
            entry = next(x for x in await c.get("/api/config/config_entries/entry?domain=esphome") if x["title"] == bath.name or bath.name in x["title"])
            f = await c.post("/api/config/config_entries/options/flow", {"handler": entry["entry_id"]})
            f = await c.post(f"/api/config/config_entries/options/flow/{f['flow_id']}", {"allow_service_calls": True})
            check(isinstance(f, dict) and f.get("type") == "create_entry", f"Bath allowed to perform actions (options flow): {f.get('type') if isinstance(f, dict) else f}")
            check(await hr.until(lambda: _now(rs()["actions"] is True and rs()["room"]["entity"] == smp), 20),
                  f"the template names Speaker's player for Bath's area, not Bath's own nor Kitchen's: {rs()['room']}")
            r = rs()
            check(r["room"]["area"] == "Bath" and r["room"]["others"] == 0 and (r["player_info"] or {}).get("state") not in (None, "", "unavailable"),
                  f"  the area by name, no other player there, its state subscribed: {r['room']} {r['player_info']}")

            # an announcement to Bath's satellite plays on Speaker
            await asyncio.sleep(1)
            b0, s0 = bath.played(), speaker.played()
            await c.post("/api/services/assist_satellite/announce",
                               {"entity_id": bsat, "media_id": f"http://127.0.0.1:{HTTP}/reply.wav", "preannounce": False})
            await hr.until(lambda: _now(speaker.played() - s0 > 150000), 10); await asyncio.sleep(1)
            sp = speaker.played() - s0
            check(150000 <= sp <= 230000, f"the announcement played on Speaker: {sp} bytes (2 s at 48 kHz from Home Assistant's proxy: 192000)")
            check(bath.played() == b0, f"  nothing of it on Bath: {bath.played() - b0} bytes")
            # Bath waits the announcement out where it plays (its length, the player's start): its speaking state lasts as long
            def stamp(l): return time.mktime(time.strptime(l[:19], "%Y-%m-%d %H:%M:%S")) + float("0" + l[19:23])
            lines = bath.text().splitlines()
            a0 = max((i for i, l in enumerate(lines) if "media: announce" in l), default=None)
            a1 = next((l for l in lines[a0:] if "speaking -> idle" in l), None) if a0 is not None else None
            held = stamp(a1) - stamp(lines[a0]) if a1 else -1
            check(2.0 <= held <= 4.0, f"  Bath held its speaking state while Speaker played it: {held:.2f} s (2 s and the 0.3 s start)")
            last = rs()["last"] or {}
            check(last.get("result") == "played" and last.get("where") == smp, f"  Bath's page: {last}")
            check("announcement" in speaker.text() or "media: http" in speaker.text(), "  Speaker was told by Home Assistant's media player")
    finally:
        httpd.shutdown()
        for e in echos: e.stop()
        hass.send_signal(signal.SIGTERM)
        try: hass.wait(30)
        except subprocess.TimeoutExpired: hass.kill(); hass.wait()
        if check.failed:
            for e in echos: print(f"--- {e.name} log tail\n" + "".join(e.text().splitlines(True)[-30:]))
            print("--- Home Assistant log tail\n" + "".join(open(os.path.join(tmp, "ha.log"), errors="replace").readlines()[-40:]))


async def _true(v): return v
def _now(v): return _true(v)


def skip(why):
    print(("FAILED, not run: " if os.environ.get("HASSMIC_HA_REQUIRED") else "skipped: ") + why)
    sys.exit(1 if os.environ.get("HASSMIC_HA_REQUIRED") else 0)


def main():
    if "--setup" in sys.argv:
        hr.setup()
        subprocess.run([shutil.which("uv") or "uv", "pip", "install", "--python", hr.HA_PY, "imageio-ffmpeg==0.8.1"], check=True)
        return
    if os.environ.get("HASSMIC_IN_NETNS") != "1":
        if not os.path.exists(hr.HA_PY): skip(f"no Home Assistant at {hr.HA_PY} (.venv/bin/python tests/ha_real_reply.py --setup makes one)")
        if not ffmpeg_bin(): skip("no ffmpeg in Home Assistant's venv (.venv/bin/python tests/ha_real_reply.py --setup adds it)")
        if not os.path.exists(os.path.join(ROOT, "build", "hassmic-host")): sys.exit("make host first")
        if not shutil.which("ip"): skip("no ip (iproute2)")
        if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode:
            skip("unshare -rn is not allowed here (user namespaces; Ubuntu: sysctl kernel.apparmor_restrict_unprivileged_userns=0)")
        sys.exit(subprocess.run(["unshare", "-rn", sys.executable, os.path.abspath(__file__), *sys.argv[1:]],
                                env=dict(os.environ, HASSMIC_IN_NETNS="1")).returncode)
    hr.loopback_up()
    tmp = tempfile.mkdtemp(prefix="ha-reply-")
    try: asyncio.run(run(tmp, ffmpeg_bin()))
    finally: shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
