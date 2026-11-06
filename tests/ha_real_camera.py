#!/usr/bin/env python3
"""custom_components/echo_local's camera (camera.py, tsmux.py; docs/camera.md) in a real Home Assistant against
build/hassmic-host with the PC's stand-in camera (tests/fake_camera.py): the entity on the Echo's device, off as the
Echo's setting is; camera.turn_on switches the Echo's camera on; a still through Home Assistant's camera proxy comes
from the Echo's H.264 by way of the relay's MPEG-TS and ffmpeg; the Echo's camera process ends after the relay's
linger; camera.turn_off switches it off on the Echo and stills stop.

Same isolation and Home Assistant as tests/ha_real_dropin.py (its helpers; build/ha-venv from its --setup).  Needs
ffmpeg on the PC (Home Assistant's ffmpeg integration runs it for stills): skipped without."""
import asyncio, base64, os, re, shutil, signal, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ha_real_dropin as hr
from ha_real_dropin import check
from ha_real_alarms import write_config, add_echo_local


async def run(tmp):
    import aiohttp
    conf = os.path.join(tmp, "ha")
    write_config(conf)
    hls = subprocess.run([hr.HA_PY, "-c", "import av"], capture_output=True).returncode == 0
    if hls: open(os.path.join(conf, "configuration.yaml"), "a").write("stream:\n")     # HLS, as default_config has it
    camlog = os.path.join(tmp, "camera.log")
    os.environ.update(HASSMIC_FAKE_CAMERA=os.path.join(hr.ROOT, "tests", "fake_camera.py"), FAKE_CAMERA_LOG=camlog)
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    echo = hr.Echo(tmp, "Echo Show 5 c4c4c4", "02:00:00:c4:c4:c4", 26190, net, os.path.join(hr.ROOT, "build", "hassmic-host"))
    halog = open(os.path.join(tmp, "ha.log"), "w")
    hass = subprocess.Popen([hr.HA_PY, "-m", "homeassistant", "-c", conf, "--skip-pip"], stdout=halog, stderr=subprocess.STDOUT, cwd=conf)
    echo.start()
    starts = lambda: open(camlog).read().count("start") if os.path.exists(camlog) else 0
    stops = lambda: open(camlog).read().count("stop") if os.path.exists(camlog) else 0
    setting = lambda: next((l.strip() for l in open(os.path.join(echo.state, "config")) if l.startswith("camera=")), "") \
        if os.path.exists(os.path.join(echo.state, "config")) else ""
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

            async def cams():
                return [x for x in await c.ws(type="config/entity_registry/list") if x["platform"] == "echo_local" and x["entity_id"].startswith("camera.")]
            async def one(): return len(await cams()) == 1
            check(await hr.until(one, 15), "a camera entity")
            cam = (await cams() or [{}])[0]; eid = cam.get("entity_id", "")
            alarms_dev = {x["device_id"] for x in await c.ws(type="config/entity_registry/list") if x["platform"] == "echo_local"}
            check(len(alarms_dev) == 1, f"on the Echo's device ({eid})")
            st = await c.get(f"/api/states/{eid}")
            check(st.get("state") == "idle" and st.get("attributes", {}).get("lens_covered") is False, f"there and idle: {st.get('state')} {st.get('attributes')}")

            async def still():
                async with session.get(f"{hr.HA}/api/camera_proxy/{eid}", headers=c.h, timeout=aiohttp.ClientTimeout(total=30)) as r:
                    return r.status, await r.read()
            s0, _ = await still()
            check(s0 != 200 and starts() == 0, f"off on the Echo: no still, no camera started ({s0})")

            await c.ws(type="call_service", domain="camera", service="turn_on", target={"entity_id": eid})
            async def on(): return setting() == "camera=on"
            check(await hr.until(on, 10), "camera.turn_on: the Echo's camera setting on")
            s1, img = await still()
            check(s1 == 200 and img[:2] == b"\xff\xd8" and len(img) > 1000 and starts() == 1,
                  f"a still through the camera proxy: JPEG from the Echo's H.264 ({s1}, {len(img)} bytes)")
            s2, img2 = await still()
            check(s2 == 200 and img2[:2] == b"\xff\xd8" and starts() == 1, "a second still within the linger: the same camera run")
            async def gone(): return stops() == starts() == 1
            check(await hr.until(gone, 25), "the Echo's camera process ends after the linger (no viewer)")

            if hls:                                     # live view the way the frontend asks for it: HLS from stream (PyAV)
                n0 = starts()
                r = await c.ws(type="camera/stream", entity_id=eid)
                url = r.get("url", "") if isinstance(r, dict) else ""
                body, seg = "", b""
                for _ in range(60):                     # the playlist fills once stream has its first segment
                    async with session.get(f"{hr.HA}{url}", headers=c.h) as q:
                        body = await q.text() if q.status == 200 else ""
                    sub = next((l for l in body.splitlines() if l and not l.startswith("#")), "")
                    if sub:
                        async with session.get(f"{hr.HA}{url.rsplit('/', 1)[0]}/{sub}", headers=c.h) as q:
                            pl = await q.text() if q.status == 200 else ""
                        # low-latency HLS: parts of a second each (EXT-X-PART), whole segments only every few
                        parts = re.findall(r'URI="\./(segment/[0-9.]+\.m4s)"', pl)
                        if parts:
                            async with session.get(f"{hr.HA}{url.rsplit('/', 1)[0]}/{parts[0]}", headers=c.h) as q:
                                seg = await q.read() if q.status == 200 else b""
                            break
                    await asyncio.sleep(0.5)
                check(url and len(seg) > 1000 and b"moof" in seg[:64] and starts() == n0 + 1,
                      f"live view: HLS from Home Assistant's stream, a part of the Echo's video in fMP4 ({url[:40]}, {len(seg)} bytes)")
                await c.ws(type="call_service", domain="camera", service="turn_off", target={"entity_id": eid})
                async def ended(): return stops() == starts()
                check(await hr.until(ended, 30), "switched off while HLS ran: the Echo's camera process ends")
                await c.ws(type="call_service", domain="camera", service="turn_on", target={"entity_id": eid})
                await hr.until(on, 10)
            else:
                print("skip live view: no PyAV in build/ha-venv (tests/ha_real_dropin.py --setup installs it)")

            await c.ws(type="call_service", domain="camera", service="turn_off", target={"entity_id": eid})
            async def off(): return setting() == "camera=off"
            check(await hr.until(off, 10), "camera.turn_off: the setting off on the Echo")
            n1 = starts(); s3, _ = await still()
            check(s3 != 200 and starts() == n1, f"off: no still, no camera started ({s3})")
    finally:
        echo.stop()
        hass.send_signal(signal.SIGTERM)
        try: hass.wait(20)
        except subprocess.TimeoutExpired: hass.kill()
        if check.failed:
            print("--- Echo log tail\n" + "".join(echo.text().splitlines(True)[-30:]))
            print("--- Home Assistant log tail\n" + "".join(open(os.path.join(tmp, "ha.log"), errors="replace").readlines()[-60:]))


def main():
    if os.environ.get("HASSMIC_IN_NETNS") != "1":
        if not os.path.exists(hr.HA_PY):
            hr.skip(f"no Home Assistant at {hr.HA_PY} (.venv/bin/python tests/ha_real_dropin.py --setup makes one)")
        if not shutil.which("ffmpeg"): hr.skip("no ffmpeg (Home Assistant's stills)")
        if not os.path.exists(os.path.join(hr.ROOT, "build", "hassmic-host")): sys.exit("make host first")
        if not shutil.which("ip"): hr.skip("no ip (iproute2)")
        if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode:
            hr.skip("unshare -rn is not allowed here (user namespaces)")
        env = dict(os.environ, HASSMIC_IN_NETNS="1")
        sys.exit(subprocess.run(["unshare", "-rn", sys.executable, os.path.abspath(__file__), *sys.argv[1:]], env=env).returncode)
    hr.loopback_up()
    tmp = tempfile.mkdtemp(prefix="ha-camera-")
    try: asyncio.run(run(tmp))
    finally: shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
