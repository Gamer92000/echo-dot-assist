#!/usr/bin/env python3
"""A Drop In from Home Assistant (dropin.c's far end "home-assistant", web.c GET /api/call; docs/video-dropin.md)
against build/hassmic-host, called by the Echo Local integration's own client (custom_components/echo_local/api.py
Echo.drop_in): connected at once, its tone played on the Echo's Voip stream, the Echo's mic coming back in frames; hung
up from Home Assistant, from the Echo (the settings page's end) and by Home Assistant going; ringing with
drop_in_answer until the action button; refused while busy and with do not disturb; lost when Home Assistant falls
silent; unsigned requests refused."""
import array, asyncio, math, os, random, signal, subprocess, sys, tempfile, time
import aiohttp

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser, req

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "custom_components", "echo_local"))
import api                                                     # noqa: E402
from fake_video import gone as fake_video_gone                 # noqa: E402

PORT, WEB = 16995, 16996


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def until(cond, secs=5):
    end = time.monotonic() + secs
    while not cond() and time.monotonic() < end: await asyncio.sleep(0.05)
    return cond()


def dropin_idle(b):
    st = b.call("GET", "/api/state")
    st = st[2] if isinstance(st, tuple) else st
    return b'"dropin":{"state":"idle"' in (st if isinstance(st, bytes) else str(st).encode())


def tone(i, hz=440, amp=8000):
    """frame i of a sine (20 ms, 16 kHz)"""
    return array.array("h", (int(amp * math.sin(2 * math.pi * hz * (i * 320 + n) / 16000)) for n in range(320))).tobytes()


async def talk(call, secs, frames=None, quiet=False):
    """frames to the Echo every 20 ms for secs (silence when quiet), what comes back meanwhile: (states, audio frames)"""
    states, audio, i = [], 0, 0
    talk.video = []; talk.levels = []
    async def rx():
        nonlocal audio
        while True:
            r = await call.recv()
            if r is None: return
            if r[0] == "S": states.append(r[1])
            elif r[0] == "A":
                audio += 1; talk.levels.append(max(abs(x) for x in array.array("h", r[1])))
            elif r[0] == "V": talk.video.append(r[1])
    t = asyncio.ensure_future(rx())
    t0 = time.monotonic()
    try:
        while time.monotonic() - t0 < secs and not t.done():
            await call.audio(bytes(640) if quiet else tone(i)); i += 1
            await asyncio.sleep(max(0, t0 + i * 0.02 - time.monotonic()))
    except ConnectionError:
        pass
    await asyncio.sleep(0.3)
    t.cancel()
    return states, audio


async def main():
    state = tempfile.mkdtemp(); voip = os.path.join(state, "voip.raw")
    # the stand-in capture with a quiet room under it (-50 dBFS) and 6 s more of only the room: between its words there is
    # something to turn down, and every 8 s of the loop (8.4 + 6 s: at most 4.6 s of speech) has quiet past the gate's hold
    cap = array.array("h", open(f"{ROOT}/testdata/alexa_espeak.raw", "rb").read() + bytes(6 * 32000)); rnd = random.Random(7)
    cap = array.array("h", (max(-32768, min(32767, x + int(rnd.gauss(0, 100)))) for x in cap))
    open(os.path.join(state, "cap.raw"), "wb").write(cap.tobytes())
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=os.path.join(state, "cap.raw"), HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_VOIP=voip, HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_DROPIN_TIMES="4000 1500",             # lost after 1.5 s without Home Assistant's frames
               HASSMIC_FAKE_CAMERA=os.path.join(ROOT, "tests", "fake_camera.py"),
               HASSMIC_FAKE_VIDEO=os.path.join(ROOT, "tests", "fake_video.py"), FAKE_VIDEO_LOG=os.path.join(state, "video.log"))             # lost after 1.5 s without Home Assistant's frames
    log = os.path.join(state, "log")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Call", "-z", "0", "-o", "0",
                             "-a", "16997", "-i", "16998", "-W", str(WEB)], env=env, stderr=open(log, "w"))
    text = lambda: open(log, errors="replace").read()
    try:
        wait_port(PORT, WEB)
        b = Browser(WEB)
        check(b.login_with_button(proc), "logged in (the action button)")
        check(req(WEB, "GET", "/api/call")[0] == 401, "unsigned: 401")
        async with aiohttp.ClientSession() as s:
            echo = api.Echo(s, "127.0.0.1", WEB, b.sk.private_bytes_raw().hex())
            call = await echo.drop_in()
            first = await call.recv()
            check(first == ("S", "connected") and "Home Assistant drops in" in text(), f"connected at once (drop_in_answer off): {first}")
            # busy meanwhile
            try:
                await (await api.Echo(s, "127.0.0.1", WEB, b.sk.private_bytes_raw().hex()).drop_in()).recv(); busy = None
            except api.EchoError as e:
                busy = str(e)
            check(busy == "busy in another Drop In", f"a second call meanwhile: refused ({busy})")
            v0 = os.path.getsize(voip) if os.path.exists(voip) else 0
            states, audio = await talk(call, 2)
            check(audio > 60, f"the Echo's mic comes back: {audio} frames in 2 s")
            check(not talk.video, "the camera setting off: no picture in the call")
            pcm = array.array("h", open(voip, "rb").read()[v0:]) if os.path.exists(voip) else array.array("h")
            peak = max((abs(x) for x in pcm), default=0)
            check(len(pcm) > 16000 and peak > 4000, f"the tone played on the Voip stream: {len(pcm) / 16000:.1f} s, peak {peak}")
            await call.hangup()
            check(await until(lambda: "ended with home-assistant" in text() and "Home Assistant hung up" in text(), 4),
                  "hung up from Home Assistant: the call ends on the Echo")

            # the camera on: the call carries its picture (the stand-in camera's records), config first
            b.call("POST", "/api/set", b"camera=on\n")
            call = await echo.drop_in(); await call.recv()
            states, audio = await talk(call, 3)
            types = [v[0] for v in talk.video]
            check(types[:1] == [api.CAM_CONFIG] and api.CAM_KEY in types and len(types) > 20,
                  f"the camera on: its picture in the call ({len(types)} records, {types[:3]})")
            slog = os.path.join(state, "video.log.self")
            sl = open(slog).read() if os.path.exists(slog) else ""
            check("self=1" in sl and sl.count("rec ") > 20 and "this Show's own picture in the corner" in text(),
                  f"  and in the Echo's own corner, as it leaves (self-view: {sl.count('rec ')} records)")
            check(not os.path.exists(os.path.join(state, "video.log")), "  no full-screen player for it: the browser sent no picture")
            await call.hangup()
            check(await until(lambda: "pictures" in text() and "camera: stream" in text() and "ended: the viewer went" in text(), 4),
                  "and the camera stops with the call")
            check(await until(lambda: fake_video_gone(slog[:-5]), 3), "  the self-view goes with it")
            b.call("POST", "/api/set", b"camera=off\n")

            # the browser's picture on the Echo's screen (video.c, cam.c's play mode; the stand-in player logs what it
            # gets): two deltas before any key frame (dropped), then the test clip as WebCodecs sends it, config inline
            import fake_camera
            config, aus = fake_camera.units(open(fake_camera.SRC, "rb").read())
            vlog = os.path.join(state, "video.log")
            call = await echo.drop_in(); await call.recv()
            async def pictures():
                pts = 10**6
                for key, au in [(False, aus[1][1]), (False, aus[2][1])] + aus * 2:
                    await call.video(2 if key else 3, pts, (config + au) if key else au); pts += 66666
                    await asyncio.sleep(1 / 15)
            pic = asyncio.ensure_future(pictures())
            await talk(call, 4.8, quiet=True); await pic
            recs = [l.split() for l in open(vlog).read().splitlines() if l.startswith("rec")] if os.path.exists(vlog) else []
            check(recs and recs[0][1] == "2" and len(recs) >= len(aus) * 2 - 2,
                  f"the browser's picture reaches the screen's player, from its key frame on ({len(recs)} records, the first {recs[:1]})")
            check(not await call.video(2, 0, bytes(api.EchoCall.VIDEO_MAX)), "a record too long for the Echo is not sent")
            await call.hangup()
            check(await until(lambda: fake_video_gone(vlog) and "the other side's picture ended" in text(), 4),
                  "hung up: the player goes, and the picture with it")
            check("1 of the browser's" not in text() and "of the browser's" in text(), "the call's end in the log counts the browser's pictures")

            # drop_in_gate: off, the mic is never turned down between words; full (the default), it is (the stand-in
            # capture's quiet stretches: no speech in them).  Silence from Home Assistant: no far end to duck for
            peaks = {}
            for g in ("off", "full"):
                r = b.set(drop_in_gate=g); check(r.get("applied") == 1, f"drop_in_gate={g} applied ({r})")
                call = await echo.drop_in(); await call.recv()
                await talk(call, 8, quiet=True)
                lv = sorted(talk.levels[25:]); peaks[g] = lv[len(lv) // 10] if lv else 0     # the quietest tenth's peak
                await call.hangup(); await until(lambda: dropin_idle(b), 3)
            check(peaks["off"] > 4 * max(peaks["full"], 1), f"drop_in_gate off: the room comes through between words, full: turned down ({peaks})")
            st = b.call("GET", "/api/state"); st = st[2] if isinstance(st, tuple) else st
            st = st.decode() if isinstance(st, bytes) else str(st)
            check('"drop_in_gate"' in st and '"full"' in st, "drop_in_gate on the page's state")

            # ended on the Echo (the settings page's "end"): Home Assistant is told "idle"
            call = await echo.drop_in(); await call.recv()
            async def end_soon():
                await asyncio.sleep(1); b.call("POST", "/api/dropin", b"end")
            asyncio.ensure_future(end_soon())
            states, _ = await talk(call, 4)
            check(states[-1:] == ["idle"], f"ended on the Echo: Home Assistant hears it ({states})")

            # Home Assistant goes without a word
            call = await echo.drop_in(); await call.recv(); await talk(call, 0.5); call.close()
            check(await until(lambda: text().count("Home Assistant went") >= 1, 4), "Home Assistant went: the call ends")

            # silent: lost after HASSMIC_DROPIN_TIMES' 1.5 s
            call = await echo.drop_in(); await call.recv()
            states = []
            async def rx():
                while (r := await call.recv()) is not None:
                    if r[0] == "S": states.append(r[1])
            await asyncio.wait_for(rx(), 6)
            check(states[-1:] == ["idle"] and "lost: no audio" in text(), f"no frames from Home Assistant: lost ({states})")

            # ringing until the action button
            b.call("POST", "/api/set", b"drop_in_answer=ask\n")
            call = await echo.drop_in()
            first = await call.recv()
            check(first == ("S", "ringing"), f"drop_in_answer ask: it rings ({first})")
            async def press():
                await asyncio.sleep(0.8); proc.send_signal(signal.SIGUSR2)
            asyncio.ensure_future(press())
            states, audio = await talk(call, 2)
            check(states[:1] == ["connected"] and audio > 20, f"the action button answers ({states}, {audio} frames)")
            await call.hangup()
            b.call("POST", "/api/set", b"drop_in_answer=auto\n")
            await until(lambda: text().count("Home Assistant hung up") >= 2, 3)

            # do not disturb refuses it
            b.call("POST", "/api/set", b"do_not_disturb=on\n")
            try:
                await echo.drop_in(); dnd = None
            except api.EchoError as e:
                dnd = str(e)
            check(dnd == "do not disturb is on", f"do not disturb: refused ({dnd})")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
    if check.failed:
        print(text()[-4000:])
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())
