#!/usr/bin/env python3
"""Echo Local's switch "Replies in the Echo's own voice" (own_voice_replies) in a real Home Assistant against
build/hassmic-host with the stand-in voice (HASSMIC_FAKE_SPEECH=1: the setting is listed only where the Echo has a voice):
the switch appears on the Echo's device, starts off as the Echo's setting does, and turning it on and off changes the
Echo's setting (its export says so) and the switch's state follows.

Same isolation and Home Assistant as tests/ha_real_alarms.py (its setup)."""
import asyncio, base64, json, os, shutil, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ha_real_dropin as hr
from ha_real_dropin import check
from ha_real_alarms import write_config, add_echo_local


def _setting(echo, name):
    """the Echo's own setting as its state file has it (state/config: name=value)"""
    try:
        for line in open(os.path.join(echo.state, "config")):
            k, _, v = line.strip().partition("=")
            if k == name:
                return v
    except OSError:
        pass
    return None


async def run(tmp):
    import aiohttp
    conf = os.path.join(tmp, "ha")
    write_config(conf)
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    os.environ["HASSMIC_FAKE_SPEECH"] = "1"         # a voice of the Echo's own, so the setting is there (speech_none.c)
    echo = hr.Echo(tmp, "Echo Show 5 a1b2c3", "02:00:00:a1:b2:c3", 26190, net, os.path.join(hr.ROOT, "build", "hassmic-host"))
    halog = open(os.path.join(tmp, "ha.log"), "w")
    hass = subprocess.Popen([hr.HA_PY, "-m", "homeassistant", "-c", conf, "--skip-pip"], stdout=halog, stderr=subprocess.STDOUT, cwd=conf)
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
            f = await add_echo_local(c, echo)
            check(f.get("type") == "create_entry", f"Echo Local added ({f.get('type')})")

            async def find():
                reg = await c.ws(type="config/entity_registry/list")
                return next((x["entity_id"] for x in reg if x["platform"] == "echo_local"
                             and (x.get("unique_id") or "").endswith("-setting-own_voice_replies")), None)
            async def found(): return await find() is not None
            ok = await hr.until(found, 30)
            eid = await find()
            check(ok and eid, f"the switch is there on an Echo with a voice ({eid})")
            st = await c.get(f"/api/states/{eid}") if eid else {}
            check(st.get("state") == "off", f"it starts off, as the Echo's setting ({st.get('state')})")

            async def is_(want): return (await c.get(f"/api/states/{eid}")).get("state") == want
            await c.ws(type="call_service", domain="switch", service="turn_on", target={"entity_id": eid})
            check(await hr.until(lambda: is_("on"), 10), "turned on: the switch says on")
            check(_setting(echo, "own_voice_replies") == "on", f"and the Echo's setting is on ({_setting(echo, 'own_voice_replies')})")
            await c.ws(type="call_service", domain="switch", service="turn_off", target={"entity_id": eid})
            check(await hr.until(lambda: is_("off"), 10), "turned off: the switch says off")
            check(_setting(echo, "own_voice_replies") == "off", f"and the Echo's setting is off ({_setting(echo, 'own_voice_replies')})")
    finally:
        echo.stop()
        hass.terminate()
        try: hass.wait(30)
        except subprocess.TimeoutExpired: hass.kill()


def main():
    if os.environ.get("HASSMIC_IN_NETNS") != "1":
        if not os.path.exists(hr.HA_PY): hr.skip("no build/ha-venv (tests/ha_real_dropin.py --setup)")
        if not os.path.exists(os.path.join(hr.ROOT, "build", "hassmic-host")): sys.exit("make host first")
        if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode:
            hr.skip("unshare -rn is not allowed here (user namespaces)")
        env = dict(os.environ, HASSMIC_IN_NETNS="1")
        sys.exit(subprocess.run(["unshare", "-rn", sys.executable, os.path.abspath(__file__), *sys.argv[1:]], env=env).returncode)
    hr.loopback_up()
    tmp = tempfile.mkdtemp(prefix="ha-own-voice-")
    try: asyncio.run(run(tmp))
    finally: shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
