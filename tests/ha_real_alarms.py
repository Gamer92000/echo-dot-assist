#!/usr/bin/env python3
"""custom_components/echo_local (the Echo's alarms in Home Assistant) in a real Home Assistant against build/hassmic-host:
the config flow (the address, then the Echo's action button lets Home Assistant in), the alarm switches on the same
device as the esphome integration's entities (the MAC), the actions add_alarm / edit_alarm / delete_alarm, a switch
turned off, the dashboard card served, and Home Assistant let in again after the Echo forgot it.

Same isolation and Home Assistant as tests/ha_real_dropin.py (its helpers; build/ha-venv from its --setup)."""
import asyncio, base64, json, os, shutil, signal, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ha_real_dropin as hr
from ha_real_dropin import check, HA_PORT


def write_config(conf):
    os.makedirs(os.path.join(conf, ".storage"))
    shutil.copytree(os.path.join(hr.ROOT, "custom_components", "echo_local"), os.path.join(conf, "custom_components", "echo_local"))
    open(os.path.join(conf, "configuration.yaml"), "w").write(
        "homeassistant:\n  name: Test\n  unit_system: metric\n  time_zone: UTC\n  latitude: 0\n  longitude: 0\n  elevation: 0\n"
        "  country: DE\n  internal_url: http://127.0.0.1:%d\napi:\nconfig:\nonboarding:\nfrontend:\n"
        "logger:\n  default: warning\n  logs:\n    custom_components.echo_local: debug\n" % HA_PORT)
    json.dump({"version": 2, "minor_version": 2, "key": "http", "data": {
        "stable": {"server_port": HA_PORT, "server_host": ["127.0.0.1"], "cors_allowed_origins": [], "login_attempts_threshold": -1,
                   "ip_ban_enabled": False, "ssl_profile": "modern", "use_x_frame_options": True,
                   "created_at": "2026-10-11T00:00:00+00:00", "error": None, "error_message": None},
        "pending": None, "yaml_migration_done": True}}, open(os.path.join(conf, ".storage", "http"), "w"))


async def add_echo_local(c, echo, press=True):
    """the config flow: the address, then the button (SIGUSR2 on the PC); the result"""
    f = await c.post("/api/config/config_entries/flow", {"handler": "echo_local", "show_advanced_options": False})
    f = await c.post(f"/api/config/config_entries/flow/{f['flow_id']}", {"host": "127.0.0.1", "port": echo.port + 10})
    if f.get("step_id") != "approve": return f
    first = f
    f = await c.post(f"/api/config/config_entries/flow/{f['flow_id']}", {})            # Submit before the press
    early = f.get("errors")
    if press: echo.proc.send_signal(signal.SIGUSR2)
    await asyncio.sleep(0.5)
    f = await c.post(f"/api/config/config_entries/flow/{f['flow_id']}", {})
    f["_first"], f["_early"] = first, early
    return f


async def run(tmp):
    import aiohttp
    conf = os.path.join(tmp, "ha")
    write_config(conf)
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    echo = hr.Echo(tmp, "Echo Show 5 a1b2c3", "02:00:00:a1:b2:c3", 26190, net, os.path.join(hr.ROOT, "build", "hassmic-host"))
    halog = open(os.path.join(tmp, "ha.log"), "w")
    hass = subprocess.Popen([hr.HA_PY, "-m", "homeassistant", "-c", conf, "--skip-pip"], stdout=halog, stderr=subprocess.STDOUT,
                            cwd=conf)       # not the repository: python -m puts the working directory on sys.path, and its
                                            # custom_components/ (hassmic's integration) would hide the config's
    echo.start()
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
            await hr.add_esphome(c, echo)
            check(await hr.until(lambda: _has(echo, "voice assistant: subscribed"), 20), "the Echo in Home Assistant (esphome)")

            f = await add_echo_local(c, echo)
            check(f.get("_first", {}).get("step_id") == "approve" and "a1b2c3" in json.dumps(f.get("_first", {}).get("description_placeholders")),
                  f"the flow asks for the button on that Echo: {f.get('_first')}")
            check(f.get("_early") == {"base": "not_approved"}, f"Submit before the press: not let in yet ({f.get('_early')})")
            check(f.get("type") == "create_entry" and f.get("title") == "Echo Show 5 a1b2c3", f"after the press: added ({f.get('type')}, {f.get('title')})")
            again = await add_echo_local(c, echo, press=False)
            check(again.get("type") == "abort" and again.get("reason") == "already_configured", f"the same Echo again: already added ({again})")

            await c.ws(type="call_service", domain="echo_local", service="add_alarm",
                       service_data={"time": "07:30:00", "days": ["mon", "fri"], "label": "Gym"})
            async def sw():
                reg = await c.ws(type="config/entity_registry/list")
                return [x for x in reg if x["platform"] == "echo_local" and x["entity_id"].startswith("switch.")], reg
            ok = await hr.until(lambda: _n(sw, 1), 15)
            alarms, reg = await sw()
            check(ok and len(alarms) == 1, f"add_alarm: a switch for it ({[a['entity_id'] for a in alarms]})")
            eid = alarms[0]["entity_id"] if alarms else ""
            st = await c.get(f"/api/states/{eid}")
            a = st.get("attributes", {}) if isinstance(st, dict) else {}
            check(st.get("state") == "on" and a.get("time") == "07:30" and a.get("days") == ["mon", "fri"] and a.get("label") == "Gym"
                  and a.get("next_ring"), f"its state and attributes: {st.get('state')} {a}")
            # Home Assistant 2026 keeps each integration's devices apart: hassmic's carries the esphome one's MAC (the
            # card finds "Alarm ringing" through it)
            esp_dev = {x["device_id"] for x in reg if x["platform"] == "esphome"}
            devs = {d["id"]: d for d in await c.ws(type="config/device_registry/list")}
            mine = devs.get(alarms[0]["device_id"]) if alarms else None
            check(mine and any(["mac", "02:00:00:a1:b2:c3"] == list(x) for x in mine["connections"])
                  and any(["mac", "02:00:00:a1:b2:c3"] == list(x) for d in esp_dev for x in devs[d]["connections"]),
                  f"its device has the Echo's MAC, as the esphome integration's has ({mine and mine['connections']})")
            check(_alarms(echo)[0]["label"] == "Gym" and _alarms(echo)[0]["days"] == 0x22, f"on the Echo: {_alarms(echo)}")

            await c.ws(type="call_service", domain="switch", service="turn_off", target={"entity_id": eid})
            offok = await hr.until(lambda: _is(c, eid, "off"), 10)
            check(offok and not _alarms(echo)[0]["enabled"], f"switched off: kept on the Echo, off ({offok}, {_alarms(echo)}, {await c.state(eid)})")
            await c.ws(type="call_service", domain="echo_local", service="edit_alarm", target={"entity_id": eid},
                       service_data={"time": "06:45:00", "days": [], "label": "Flight"})
            check(await hr.until(lambda: _is(c, eid, "on"), 10), "edit_alarm: on again")
            e0 = _alarms(echo)[0]
            check(e0["hour"] == 6 and e0["min"] == 45 and e0["days"] == 0 and e0["label"] == "Flight" and e0["enabled"],
                  f"edit_alarm: once at 6:45, \"Flight\" ({e0})")
            await c.ws(type="call_service", domain="echo_local", service="delete_alarm", target={"entity_id": eid})
            check(await hr.until(lambda: _n(sw, 0), 15) and _alarms(echo) == [], "delete_alarm: gone on the Echo and in Home Assistant")

            async with session.get(f"{hr.HA}/echo_local/echo-local-alarms-card.js") as r:
                body = await r.text()
            check(r.status == 200 and "echo-local-alarms-card" in body, f"the dashboard card is served ({r.status})")
            await c.post("/api/onboarding/integration", {"client_id": f"{hr.HA}/", "redirect_uri": f"{hr.HA}/?auth_callback=1"})
            async with session.get(f"{hr.HA}/lovelace/0") as r:  # every dashboard loads it: the frontend's page names it
                page = await r.text()
            check("/echo_local/echo-local-alarms-card.js" in page,
                  f"the frontend's page loads the card on every dashboard ({r.status}: {[l for l in page.splitlines() if 'import' in l or 'extra' in l.lower()][:6]})")

            # the Echo forgot Home Assistant (its login revoked on the page, a factory reset): the next call asks for
            # the button again, through a repair
            clients = os.path.join(echo.state, "web_clients")
            open(clients, "w").close()
            echo.stop(); echo.start()
            flows = []
            for _ in range(100):                        # the next poll (every 30 s) finds it out
                flows = [x for x in await c.ws(type="config_entries/flow/progress") if x["handler"] == "echo_local"]
                if flows: break
                await asyncio.sleep(0.5)
            check(flows and flows[0]["context"].get("source") == "reauth" and flows[0]["step_id"] == "reauth_confirm",
                  f"forgotten: Home Assistant asks to be let in again ({flows})")
            check("login waits" not in echo.text().split("web: login waits")[-1] and echo.text().count("login waits for the action button") == 1,
                  "and the Echo waits for no button until someone submits it")
            f = await c.post(f"/api/config/config_entries/flow/{flows[0]['flow_id']}", {})
            echo.proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.5)
            f = await c.post(f"/api/config/config_entries/flow/{f['flow_id']}", {})
            check(f.get("type") == "abort" and f.get("reason") == "reauth_successful", f"Submit, the button: let in again ({f.get('type')}, {f.get('reason')})")
    finally:
        echo.stop()
        hass.send_signal(signal.SIGTERM)
        try: hass.wait(20)
        except subprocess.TimeoutExpired: hass.kill()
        if check.failed:
            print("--- Echo log tail\n" + "".join(echo.text().splitlines(True)[-30:]))
            print("--- Home Assistant log tail\n" + "".join(open(os.path.join(tmp, "ha.log"), errors="replace").readlines()[-60:]))


def _alarms(echo):
    try:
        return [dict(zip(("id", "hour", "min", "days", "enabled", "once", "snoozed", "fired"), map(int, l.split()[:8])),
                     enabled=bool(int(l.split()[4])), label=" ".join(l.split()[8:]))
                for l in open(os.path.join(echo.state, "alarms")) if not l.startswith("#")]
    except OSError:
        return []
async def _has(echo, text): return text in echo.text()
async def _is(c, eid, st): return await c.state(eid) == st
async def _n(sw, n): return len((await sw())[0]) == n


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
    tmp = tempfile.mkdtemp(prefix="ha-alarms-")
    try: asyncio.run(run(tmp))
    finally: shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
