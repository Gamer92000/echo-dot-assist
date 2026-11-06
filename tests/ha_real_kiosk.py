#!/usr/bin/env python3
"""The Echo Show's kiosk set up from Home Assistant (custom_components/echo_local kiosk.py, src/hassmic/kiosk.c) in a real
Home Assistant against build/hassmic-host: the options flow, the kiosk's own user (local only, not an administrator)
and its token, sealed to the Echo and read back by the app's way (GET /kiosk/config from 127.0.0.1, the token only
there), the screens (Home: hassmic:home, drawn by the screen app; a dashboard), the token taking Home Assistant's
websocket; the entry removed, the user and its token gone.  The sidebar panel (the screen's options, the Echo's apps): tests/ha_real_panel.py.

Same isolation and Home Assistant as tests/ha_real_alarms.py.  Chrome: google-chrome or chromium on PATH (else that
part is skipped)."""
import asyncio, base64, json, os, shutil, signal, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ha_real_dropin as hr
import ha_real_alarms as ra
from ha_real_dropin import check, HA_PORT


def write_config(conf):
    ra.write_config(conf)
    with open(os.path.join(conf, "configuration.yaml"), "a") as f:
        # things to put on the wall: lights (brightness), a switch, a cover, climate, a scene, a door
        f.write("demo:\n")


async def api_kiosk_app(c, entry, app):
    """the options again, with another screen app"""
    o = await c.post("/api/config/config_entries/options/flow", {"handler": entry["entry_id"]})
    o = await c.post(f"/api/config/config_entries/options/flow/{o['flow_id']}",
                     {"kiosk": True, "kiosk_screens": ["wall", "dashboard"], "kiosk_default": "wall", "kiosk_return": 90, "kiosk_admin": False,
                      "kiosk_app": app})
    await asyncio.sleep(2)


async def run(tmp):
    import aiohttp
    conf = os.path.join(tmp, "ha")
    write_config(conf)
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    echo = hr.Echo(tmp, "Echo Show 5 a1b2c3", "02:00:00:a1:b2:c3", 26190, net, os.path.join(hr.ROOT, "build", "hassmic-host"))
    halog = open(os.path.join(tmp, "ha.log"), "w")
    hass = subprocess.Popen([hr.HA_PY, "-m", "homeassistant", "-c", conf, "--skip-pip"], stdout=halog, stderr=subprocess.STDOUT, cwd=conf)
    # the home app's data folder (kiosk.c kiosk_peer_ok): its owner is the uid the config is handed to, this test's
    apps = os.path.join(tmp, "apps"); os.makedirs(os.path.join(apps, "io.github.nt_f.echowall"))
    os.environ["HASSMIC_APP_DATA"] = apps
    echo.start()
    web = f"http://127.0.0.1:{echo.port + 10}"
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
            f = await ra.add_echo_local(c, echo)
            check(f.get("type") == "create_entry", f"hassmic added ({f.get('type')})")
            entry = next(e for e in await c.get("/api/config/config_entries/entry") if e["domain"] == "echo_local")

            # rooms for the demo's things
            await asyncio.sleep(2)
            areas = {a["name"]: a["area_id"] for a in await c.ws(type="config/area_registry/list")}
            for n in ("Kitchen", "Living Room"):
                if n not in areas: areas[n] = (await c.ws(type="config/area_registry/create", name=n))["area_id"]
            reg = {e["entity_id"]: e for e in await c.ws(type="config/entity_registry/list")}
            place = {"light.ceiling_lights": "Living Room", "light.kitchen_lights": "Kitchen", "light.bed_light": "Living Room",
                     "cover.kitchen_window": "Kitchen", "climate.hvac": "Living Room", "switch.decorative_lights": "Living Room"}
            for eid, a in place.items():
                if eid not in reg: continue
                # Home Assistant 2026: an entity named by its device takes its device's area
                if reg[eid].get("device_id"): await c.ws(type="config/device_registry/update", device_id=reg[eid]["device_id"], area_id=areas[a])
                else: await c.ws(type="config/entity_registry/update", entity_id=eid, area_id=areas[a])
            check(sum(e in reg for e in place) >= 4, f"the demo's lights, cover, climate, switch ({[e for e in place if e in reg]})")

            # the options: the kiosk on, the wall panel and the Overview
            o = await c.post("/api/config/config_entries/options/flow", {"handler": entry["entry_id"]})
            check(o.get("step_id") == "init" and "http://127.0.0.1" in json.dumps(o.get("description_placeholders")),
                  f"the options: the screen's, with the address the Echo gets ({o.get('description_placeholders')})")
            o = await c.post(f"/api/config/config_entries/options/flow/{o['flow_id']}",
                             {"kiosk": True, "kiosk_screens": ["wall", "dashboard"], "kiosk_default": "wall", "kiosk_return": 90,
                              "kiosk_admin": False, "kiosk_dashboard": "lovelace", "kiosk_custom_name": "",
                              "kiosk_custom_url": "", "kiosk_ha_url": "", "kiosk_app": "io.github.nt_f.echowall",
                              "kiosk_app_settings": "# the HomeView's photos\nimmich.url = http://192.168.1.9:2283\nimmich.key=s3cret-immich-key\n"})
            check(o.get("type") == "create_entry", f"saved ({o.get('type')})")

            cfg = {}
            for _ in range(60):
                try:
                    async with session.get(f"{web}/kiosk/config/front") as r:
                        cfg = await r.json()
                    if cfg.get("token"): break
                except aiohttp.ClientError: pass
                await asyncio.sleep(0.5)
            tok = cfg.get("token", "")
            async with session.get(f"{web}/kiosk/config", headers={"Origin": "http://127.0.0.1:18123"}) as r:
                check(r.status == 200 and not any(h.lower().startswith("access-control") for h in r.headers),
                      f"the app's config has no CORS headers: no page's script may read it ({[h for h in r.headers if 'ontrol' in h]})")
            async with aiohttp.ClientSession(connector=aiohttp.TCPConnector(local_addr=("127.0.0.2", 0))) as other:
                async with other.get(f"{web}/kiosk/config") as r:
                    t = await r.text()
            check(r.status == 404 and "token" not in t, f"from another address than 127.0.0.1: not found ({r.status})")
            async with session.get(f"{web}/kiosk/state") as r:
                st_ = await r.json(content_type=None) if r.status == 200 else {}
            check(r.status == 200 and "v" in st_ and not any(h.lower().startswith("access-control") for h in r.headers),
                  f"the state feed for the screen app ({r.status}, {sorted(st_)[:4]})")
            async with aiohttp.ClientSession(connector=aiohttp.TCPConnector(local_addr=("127.0.0.2", 0))) as other:
                async with other.get(f"{web}/kiosk/state") as r:
                    check(r.status == 404, f"the state feed from another address: not found ({r.status})")
            async with session.get(f"{web}/kiosk/ring/snooze") as r:
                check(r.status == 200, f"the home app's ring tapped: taken ({r.status})")
            async with aiohttp.ClientSession(connector=aiohttp.TCPConnector(local_addr=("127.0.0.2", 0))) as other:
                async with other.get(f"{web}/kiosk/ring/stop") as r:
                    check(r.status == 404, f"the ring's stop from another address: not found ({r.status})")
            check(tok and cfg.get("ha") == hr.HA, f"the Echo has the address and a token ({cfg.get('ha')}, {len(tok)} chars)")
            check([s["name"] for s in cfg.get("screens", [])] == ["Home", "Home Assistant"]
                  and cfg["screens"][0]["url"] == "hassmic:home" and cfg["screens"][1]["url"] == f"{hr.HA}/lovelace"
                  and cfg.get("default") == 0 and cfg.get("return") == 90, f"its screens: {cfg.get('screens')}, default {cfg.get('default')}, return {cfg.get('return')}")
            check((cfg.get("settings") or {}) == {"immich.url": "http://192.168.1.9:2283", "immich.key": "s3cret-immich-key"},
                  f"the screen app's settings, sealed on the way, in its config ({cfg.get('settings')})")
            st = open(os.path.join(echo.state, "kiosk")).read()
            check(oct(os.stat(os.path.join(echo.state, "kiosk")).st_mode & 0o777) == "0o600" and tok in st, "state/kiosk: 0600, the token in it")
            check(tok not in echo.text() and "s3cret-immich-key" not in echo.text(), "the token and the settings are not in the log")
            # another screen app (one of the owner's, not installed here): the config goes to nobody else
            await api_kiosk_app(c, entry, "org.example.screen")
            async with session.get(f"{web}/kiosk/config") as r:
                check(r.status == 404, f"another screen app chosen: the config is not handed to this uid any more ({r.status})")
            check("home app org.example.screen" in echo.text(),
                  "hassmic took the new screen app")
            os.makedirs(os.path.join(apps, "org.example.screen"))
            async with session.get(f"{web}/kiosk/config") as r:
                j = await r.json() if r.status == 200 else {}
            check(r.status == 200 and j.get("app") == "org.example.screen" and j.get("token") == tok, f"its folder ours: handed to us again ({r.status}, {j.get('app')})")
            await api_kiosk_app(c, entry, "")
            # the token only on loopback: the signed API (the integration's, the page's) never hands it out (display_test)
            async with session.get(f"{web}/api/kiosk") as r:
                check(r.status == 401, f"GET /api/kiosk unsigned: refused ({r.status})")
            users = {u["name"]: u for u in await c.ws(type="config/auth/list")}
            ku = users.get("Echo Show 5 a1b2c3 kiosk")
            check(ku and not ku.get("is_admin") and ku.get("local_only") and ku.get("group_ids") == ["system-users"],
                  f"the kiosk's user: not an administrator, local only ({ku and {x: ku.get(x) for x in ('is_admin', 'local_only', 'group_ids')}})")
            w = await session.ws_connect(f"{hr.HA}/api/websocket")
            await w.receive_json(); await w.send_json({"type": "auth", "access_token": tok})
            check((await w.receive_json()).get("type") == "auth_ok", "the token takes Home Assistant's websocket")
            await w.close()
            # options again, the kiosk off: the Echo's setting off, the user kept
            o = await c.post("/api/config/config_entries/options/flow", {"handler": entry["entry_id"]})
            o = await c.post(f"/api/config/config_entries/options/flow/{o['flow_id']}", {"kiosk": False, "kiosk_screens": ["wall"], "kiosk_default": "wall",
                             "kiosk_return": 0, "kiosk_admin": False})
            check(o.get("type") == "create_entry", "the kiosk switched off")
            # removed: its user and the token gone
            await session.delete(f"{hr.HA}/api/config/config_entries/entry/{entry['entry_id']}", headers=c.h)
            await asyncio.sleep(1)
            users = {u["name"] for u in await c.ws(type="config/auth/list")}
            w = await session.ws_connect(f"{hr.HA}/api/websocket")
            await w.receive_json(); await w.send_json({"type": "auth", "access_token": tok})
            gone = (await w.receive_json()).get("type") == "auth_invalid"
            await w.close()
            check("Echo Show 5 a1b2c3 kiosk" not in users and gone, f"the entry removed: the kiosk's user and token gone ({gone})")
    finally:
        echo.stop()
        hass.send_signal(signal.SIGTERM)
        try: hass.wait(30)
        except subprocess.TimeoutExpired: hass.kill()
        if check.failed:
            print(open(os.path.join(tmp, "ha.log")).read()[-6000:])
            print(echo.text()[-3000:])


def main():
    if os.environ.get("HASSMIC_IN_NETNS") != "1":
        if not os.path.exists(hr.HA_PY):
            hr.skip(f"no Home Assistant at {hr.HA_PY} (.venv/bin/python tests/ha_real_dropin.py --setup makes one)")
        if not os.path.exists(os.path.join(hr.ROOT, "build", "hassmic-host")): sys.exit("make host first")
        if not shutil.which("ip"): hr.skip("no ip (iproute2)")
        if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode:
            hr.skip("unshare -rn is not allowed here (user namespaces)")
        env = dict(os.environ, HASSMIC_IN_NETNS="1")
        sys.exit(subprocess.run(["unshare", "-rn", sys.executable, os.path.abspath(__file__), *sys.argv[1:]], env=env).returncode)
    hr.loopback_up()
    tmp = tempfile.mkdtemp(prefix="ha-kiosk-")
    try: asyncio.run(run(tmp))
    finally: shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
