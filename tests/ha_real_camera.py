#!/usr/bin/env python3
"""custom_components/echo_local's camera (camera.py, tsmux.py; docs/camera.md) in a real Home Assistant against
build/hassmic-host with the PC's stand-in camera (tests/fake_camera.py): the entity on the Echo's device, off as the
Echo's setting is; camera.turn_on switches the Echo's camera on; a still through Home Assistant's camera proxy comes
from the Echo's H.264 by way of the relay's MPEG-TS and ffmpeg; the Echo's camera process ends after the relay's
linger; camera.turn_off switches it off on the Echo and stills stop.

Same isolation and Home Assistant as tests/ha_real_dropin.py (its helpers; build/ha-venv from its --setup).  Needs
ffmpeg on the PC (Home Assistant's ffmpeg integration runs it for stills): skipped without."""
import asyncio, base64, json, os, re, shutil, signal, subprocess, sys, tempfile

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
    vidlog = os.path.join(tmp, "video.log")
    os.environ.update(HASSMIC_FAKE_CAMERA=os.path.join(hr.ROOT, "tests", "fake_camera.py"), FAKE_CAMERA_LOG=camlog,
                      HASSMIC_FAKE_VIDEO=os.path.join(hr.ROOT, "tests", "fake_video.py"), FAKE_VIDEO_LOG=vidlog)
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
            check(st.get("attributes", {}).get("shows_video") is True, "it shows a call's picture (shows_video): the card offers its camera")

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

            # Drop In from a dashboard (call.py): the card's websocket, opened with a signed path, as the card opens it
            sp = await c.ws(type="auth/sign_path", path=f"/api/echo_local/call/{eid}", expires=30)
            n0 = starts()
            async with session.ws_connect(f"{hr.HA}{sp['path']}") as w:
                got, audio, video, sent = [], 0, [], 0
                t0 = asyncio.get_running_loop().time()
                async def feed():
                    nonlocal sent
                    while True:
                        await w.send_bytes(b"A" + bytes(640)); sent += 1; await asyncio.sleep(0.02)
                fd = asyncio.ensure_future(feed())
                while asyncio.get_running_loop().time() - t0 < 4:
                    try: m = await asyncio.wait_for(w.receive(), 1)
                    except TimeoutError: continue
                    if m.type == aiohttp.WSMsgType.TEXT: got.append(json.loads(m.data))
                    elif m.type == aiohttp.WSMsgType.BINARY and m.data[:1] == b"A": audio += 1
                    elif m.type == aiohttp.WSMsgType.BINARY and m.data[:1] == b"V": video.append(m.data[1])
                    else: break
                await w.send_str(json.dumps({"hangup": True}))
                fd.cancel()
            check(got[:1] == [{"state": "connected"}] and audio > 100 and video[:1] == [1] and 2 in video,
                  f"Drop In from the card's websocket: connected, the Echo's sound ({audio} frames) and picture ({len(video)} records)")
            async def hung(): return "Home Assistant hung up" in echo.text()
            check(await hr.until(hung, 5) and await hr.until(lambda: _ended(starts, stops), 5), "hung up from the card: the call ends on the Echo, the camera too")
            async with session.get(f"{hr.HA}/api/echo_local/call/{eid}") as r:
                check(r.status == 401, f"the call's websocket without a signed path: 401 ({r.status})")
            async with session.get(f"{hr.HA}/echo_local/echo-local-call-card.js") as r:
                check(r.status == 200 and "echo-local-call-card" in await r.text(), "the call card is served")

            r = await card_in_chrome(session, c.h["Authorization"][7:], eid, echo, vidlog)
            if r is not None:
                check(r.get("state") == "connected" and r.get("picture") and r.get("lit", 0) > 1000,
                      f"the card in Chrome: connected, the Echo's picture drawn through WebCodecs ({r})")
                check(r.get("muted") and r.get("mute_shown") and r.get("muted_peak", 1) < 50,
                      f"the microphone muted at the start, its button over the picture: nothing played on the Echo (peak {r.get('muted_peak')})")
                check(r.get("unmuted") and r.get("voip_peak", 0) > 1000, f"unmuted, the browser's microphone (Chrome's fake one, a beep) plays on the Echo: peak {r.get('voip_peak')}")
                check(r.get("cam_hidden_without") and r.get("cam_shown"), f"the camera button: only where the Echo shows the picture ({r.get('cam_hidden_without')}, {r.get('cam_shown')})")
                check(r.get("video_recs", 0) >= 10 and r.get("video_first") == "2",
                      f"the browser's camera (Chrome's fake one) through WebCodecs to the Echo's screen: {r.get('video_recs')} records, the first a key frame ({r.get('video_first')}), {r.get('cam_msg')!r}")
                check(r.get("frost") == [True, False], f"the Echo's picture frosted at first (it did not ring), clear 3 s on ({r.get('frost')})")
                check(r.get("hung"), "Hang up in the card: the call ends on the Echo")
                check(r.get("video_stopped"), "and the Echo's player goes with it")
                pl = r.get("plain") or {}
                check(pl.get("state") == "connected" and pl.get("frames", 0) > 20 and 0.2 < pl.get("peak", 0) < 0.3 and "https" in pl.get("msg", "") and pl.get("mute_hidden"),
                      f"as a plain http page (no AudioWorklet, WebCodecs, microphone): the Echo's sound through a ScriptProcessor, the card says why only that ({pl})")
                check(r.get("plain_hung"), "and that call hung up too")

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


async def card_in_chrome(session, token, eid, echo, vidlog):
    """echo-local-call-card in headless Chrome (a fake camera and microphone for the page, ours for the Echo): a call,
    what it shows and what the Echo plays, then Hang up.  None without Chrome"""
    chrome = shutil.which("google-chrome") or shutil.which("chromium") or shutil.which("chromium-browser")
    if not chrome:
        print("note: no Chrome here, the call card is not tried"); return None
    prof = tempfile.mkdtemp(prefix="call-chrome-")
    proc = subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--remote-debugging-port=9335", f"--user-data-dir={prof}",
                             "--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream", "--autoplay-policy=no-user-gesture-required",
                             "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                async with session.get("http://127.0.0.1:9335/json") as r:
                    tabs = await r.json(); break
            except Exception: await asyncio.sleep(0.2)
        page = next(t for t in tabs if t.get("type") == "page")
        ws = await session.ws_connect(page["webSocketDebuggerUrl"], max_msg_size=0)
        n = [0]
        async def cdp(method, **params):
            n[0] += 1; i = n[0]
            await ws.send_json({"id": i, "method": method, "params": params})
            while True:
                m = await ws.receive_json()
                if m.get("id") == i: return m.get("result", m.get("error"))
        async def js(expr):
            r = await cdp("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True)
            if r and r.get("exceptionDetails"): return {"error": json.dumps(r["exceptionDetails"])[:400]}
            return (r.get("result") or {}).get("value")
        await cdp("Page.enable"); await cdp("Runtime.enable")
        await cdp("Page.navigate", url=f"{hr.HA}/echo_local/call-test-404"); await asyncio.sleep(1)
        # the card needs of hass only callWS (auth/sign_path) and states: Home Assistant's own websocket, from the
        # page's origin (the card's websocket URL is relative to it)
        await js(f"""(async () => {{
            const ws = new WebSocket(location.origin.replace('http', 'ws') + '/api/websocket'); let id = 0; const wait = {{}};
            await new Promise((ok) => {{ ws.onmessage = (e) => {{ const m = JSON.parse(e.data);
                if (m.type === 'auth_required') ws.send(JSON.stringify({{ type: 'auth', access_token: {json.dumps(token)} }}));
                else if (m.type === 'auth_ok') ok();
                else if (m.id && wait[m.id]) {{ wait[m.id](m.result); delete wait[m.id]; }} }}; }});
            window._hass = {{ states: {{}}, callWS: (msg) => new Promise((ok) => {{ id++; wait[id] = ok; ws.send(JSON.stringify({{ ...msg, id }})); }}) }};
            customElements.define('home-assistant', class extends HTMLElement {{}});   // the card defines itself after HA's root
            await import('/echo_local/echo-local-call-card.js'); await customElements.whenDefined('echo-local-call-card');
        }})()""")
        voip = os.path.join(echo.state, "voip.raw"); v0 = os.path.getsize(voip) if os.path.exists(voip) else 0
        out = await js(f"""(async () => {{
            const c = document.createElement('echo-local-call-card'); c.setConfig({{ entity: {json.dumps(eid)} }});
            c.hass = window._hass; document.body.appendChild(c); window._card = c;
            await c.start();
            for (let i = 0; i < 80 && !c.gotPicture; i++) await new Promise(r => setTimeout(r, 100));
            window._frost0 = c.shadowRoot.querySelector('canvas').classList.contains('frost');
            await new Promise(r => setTimeout(r, 1500));
            const cv = c.shadowRoot.querySelector('canvas'), d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data;
            let lit = 0; for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 120) lit++;
            const m = c.shadowRoot.querySelector('.mute');
            return {{ state: c.state, picture: c.gotPicture, lit, w: cv.width, msg: c.shadowRoot.querySelector('.msg').textContent, listen: c.listenOnly || '',
                      muted: c.muted, mute_shown: !m.hidden && getComputedStyle(m).display !== 'none' }};
        }})()""") or {}
        import array
        def peak(a, b): return max((abs(x) for x in array.array("h", open(voip, "rb").read()[a:b][: (b - a) // 2 * 2])), default=0) if os.path.exists(voip) else 0
        v1 = os.path.getsize(voip) if os.path.exists(voip) else 0
        out["muted_peak"] = peak(v0, v1)                # muted at the start: the Echo plays nothing of the browser
        await js("(async () => { window._card.shadowRoot.querySelector('.mute').click(); await new Promise(r => setTimeout(r, 1500)); })()")
        out["unmuted"] = await js("!window._card.muted")
        v2 = os.path.getsize(voip) if os.path.exists(voip) else 0
        out["voip_peak"] = peak(v1, v2)
        # the camera: hidden while the entity does not say shows_video, then on (Chrome's fake camera)
        out["cam_hidden_without"] = await js("window._card.shadowRoot.querySelector('.cam').hidden")
        out["cam_shown"] = await js(f"""(async () => {{ window._hass.states[{json.dumps(eid)}] = {{ attributes: {{ shows_video: true }} }};
            window._card.live(true); const b = window._card.shadowRoot.querySelector('.cam');
            const shown = !b.hidden; b.click(); await new Promise(r => setTimeout(r, 3000)); return shown; }})()""")
        out["cam_msg"] = await js("window._card.shadowRoot.querySelector('.msg').textContent")
        out["frost"] = await js("[window._frost0, window._card.shadowRoot.querySelector('canvas').classList.contains('frost')]")
        recs = [l.split() for l in open(vidlog).read().splitlines() if l.startswith("rec")] if os.path.exists(vidlog) else []
        out["video_recs"], out["video_first"] = len(recs), recs[0][1] if recs else None
        await js("window._card.stop('')")
        async def hung(): return echo.text().count("Home Assistant went") + echo.text().count("Home Assistant hung up") >= 2
        out["hung"] = await hr.until(hung, 5)
        async def vstop():
            import fake_video
            return os.path.exists(vidlog) and fake_video.gone(vidlog)
        out["video_stopped"] = await hr.until(vstop, 5)
        # a second call as a page over plain http sees it: no AudioWorklet, no WebCodecs, no microphone (the test's page
        # is 127.0.0.1, a secure context, so they are taken away by hand).  The Echo's sound through the
        # ScriptProcessor fallback, and the card says what is missing (the first version failed with "No sound here")
        n0 = echo.text().count("Home Assistant went") + echo.text().count("Home Assistant hung up")
        out["plain"] = await js(f"""(async () => {{
            for (const k of ['audioWorklet']) Object.defineProperty(BaseAudioContext.prototype, k, {{ get: () => undefined }});
            delete window.VideoDecoder; Object.defineProperty(navigator, 'mediaDevices', {{ value: undefined }});
            Object.defineProperty(window, 'isSecureContext', {{ value: false }});
            const c = window._card; await c.start();
            if (!c.ws) return {{ msg: c.shadowRoot.querySelector('.msg').textContent }};
            let peak = 0, runs = 0, frames = 0, inpeak = 0; const n = c.nodes[0], f = n.onaudioprocess, push = c.player.push;
            n.onaudioprocess = (e) => {{ f(e); runs++; for (const v of e.outputBuffer.getChannelData(0)) peak = Math.max(peak, Math.abs(v)); }};
            // the stand-in Echo's call sound is silence: each of its frames, as it comes, swapped for a 440 Hz tone
            let t = 0;
            c.player.push = (x) => {{ frames++; for (const v of x) inpeak = Math.max(inpeak, Math.abs(v));
                const y = new Int16Array(x.length); for (let i = 0; i < y.length; i++, t++) y[i] = 8000 * Math.sin(2 * Math.PI * 440 * t / 16000);
                push(y); }};
            for (let i = 0; i < 50 && c.state !== 'connected'; i++) await new Promise(r => setTimeout(r, 100));
            await new Promise(r => setTimeout(r, 1500));
            const r = {{ state: c.state, peak, runs, frames, inpeak, rate: c.ctx.sampleRate, msg: c.shadowRoot.querySelector('.msg').textContent,
                         mute_hidden: c.shadowRoot.querySelector('.mute').hidden }};
            c.stop(''); return r;
        }})()""")
        async def hung2(): return echo.text().count("Home Assistant went") + echo.text().count("Home Assistant hung up") >= n0 + 2
        out["plain_hung"] = await hr.until(hung2, 5)
        return out
    finally:
        proc.terminate()
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
        shutil.rmtree(prof, ignore_errors=True)


async def _ended(starts, stops): return stops() == starts()


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
