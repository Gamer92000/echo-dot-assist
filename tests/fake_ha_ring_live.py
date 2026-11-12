#!/usr/bin/env python3
"""The light ring live (ringlive.c) against build/hassmic-host, as Home Assistant: the LED driver's frame as a file
(HASSMIC_FAKE_FRAME, 12 x RRGGBB as a Dot 3's), the brightness table and the set stand-ins (HASSMIC_LED_ROOT, stock's
from tests/fake_ha_ring.py).  Off: none of its entities.  On: a light per LED, the four actions and the design select;
an LED and a whole frame on the driver at stock's own most (FD at level 100, never more), from where the fill starts;
the Echo's own patterns first (the wake word's ring, mute): not one write meanwhile, ours back after; a frame
ledcontroller wrote over put back; an animation and a design of the library played by the Echo, the select's state;
Home Assistant's "Light ring" drawn live under the LEDs (its exact colour, the live effects, a transition); stop: the
dark frame; the settings page's "Show on the Echo" (POST /api/ring/live)."""
import asyncio, io, json, math, os, shutil, signal, struct, subprocess, sys, tempfile, threading, wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from aioesphomeapi import (APIClient, LightInfo, LightState, MediaPlayerInfo, NumberInfo, SelectInfo, SelectState, SwitchInfo,
                           VoiceAssistantEventType as Ev)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_ha_ring import stock                  # stock's stand-in animations and layer config
from webclient import Browser, wait_port

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = f"{ROOT}/build/hassmic-host"
PORT, WEB = 16985, 16986
DARK = "0" * 72 + "\n"


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def until(cond, timeout=5.0):
    end = asyncio.get_running_loop().time() + timeout
    while not cond() and asyncio.get_running_loop().time() < end:
        await asyncio.sleep(0.02)
    return cond()


def table(path):
    """led_mute_ring_config.json's shape; black's pwm 0.993 at level 100 as donut's"""
    rows = lambda top: [{"pwm": round(0.122 + (top - 0.122) * i / 100, 3), "current": 3 if i < 60 else 0} for i in range(102)]
    with open(path, "w") as f:
        json.dump({"cap_color": [{"color": "black", "cap_color_number": 0}, {"color": "white", "cap_color_number": 1}],
                   "rgbTable": {"black": rows(0.993), "white": rows(0.5)}}, f)


def kick_wav(secs=4, rate=48000):
    """a kick drum (60 Hz, 80 ms) at 120 bpm: what "Music beat" should pump to"""
    b = io.BytesIO(); w = wave.open(b, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
    fr = bytearray()
    for i in range(secs * rate):
        t = i / rate; ph = t % 0.5
        v = 0.7 * math.sin(2 * math.pi * 60 * t) * (1 - ph / 0.08) if ph < 0.08 else 0.0
        fr += struct.pack("<h", int(v * 32767))
    w.writeframes(bytes(fr)); w.close()
    return b.getvalue()


def pixels(text):
    return [text[i * 6:i * 6 + 6] for i in range(12)]


async def main():
    tmp = tempfile.mkdtemp(); etc = os.path.join(tmp, "etc"); state = os.path.join(tmp, "state"); os.makedirs(state)
    stock(etc); table(os.path.join(etc, "led_mute_ring_config.json"))
    frame, leds, beam = os.path.join(tmp, "frame"), os.path.join(tmp, "ledctrl.log"), os.path.join(tmp, "beam")
    als = os.path.join(tmp, "alsparams")
    with open(als, "w") as f: f.write("alsgain=03 alstime=690 capcolor=0\n")
    env = dict(os.environ, HASSMIC_LED_ROOT=etc, HASSMIC_FAKE_LED=leds, HASSMIC_FAKE_BEAM=beam, HASSMIC_STATE=state,
               HASSMIC_FAKE_FRAME=frame, HASSMIC_FAKE_ALSPARAMS=als,
               HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(tmp, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(tmp, "mdns"), HASSMIC_FAKE_ADDRS=os.path.join(tmp, "addrs"),
               HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(tmp, "models"))
    with open(env["HASSMIC_FAKE_ADDRS"], "w") as f: f.write("127.0.0.1\n")
    with open(os.path.join(state, "adopted"), "w") as f: f.write("ha\n")
    with open(beam, "w") as f: f.write("0\n")
    with open(frame, "w") as f: f.write(DARK)
    open(env["HASSMIC_PLAY"], "wb").close()
    os.makedirs(os.path.join(state, "ring"))
    with open(os.path.join(state, "ring", "1"), "w") as f:      # a design of the library, as the settings page saves it
        f.write("name Sunrise\nleds 12\nloop 0\n200:FF8000," + ",".join(["000000"] * 11) + "\n200:000000,FF8000," + ",".join(["000000"] * 10) + "\n")
    # the set installed, so Home Assistant's "Light ring" is there to take the ring from
    out = os.path.join(tmp, "out"); os.makedirs(out)
    subprocess.run([HOST, "-G", out], env=env, check=True, capture_output=True)
    shutil.copy(os.path.join(etc, "led-resources", "layer_config_common.json"), os.path.join(etc, "layer_config_common.json.pre-hassmic"))
    for n in os.listdir(out):
        if n != "digest": shutil.copy(os.path.join(out, n), os.path.join(etc, "led-resources", n))
    shutil.copy(os.path.join(out, "digest"), os.path.join(etc, "hassmic-leds.digest"))

    def start():
        open(leds, "w").close()
        return subprocess.Popen([HOST, "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-W", str(WEB), "-a", "0", "-z", "0", "-o", "0"],
                                env=env, stderr=open(os.path.join(tmp, "log"), "a"))

    async def connect():
        for _ in range(100):
            try:
                c = APIClient("127.0.0.1", PORT, None); await c.connect(login=True); return c
            except Exception: await asyncio.sleep(0.1)
        raise RuntimeError("no connection")

    def read():
        """the stand-in frame; a read that met hassmic's write half way (truncated, not yet written) again"""
        for _ in range(50):
            t = open(frame).read()
            if len(t) == 73: return t
        return t

    # An Echo where hassmic may not write the ring (every other model: no ring_frame; a Dot 3 before root's main.sh
    # gave the file): "Light ring live" set on in its config all the same, and nothing of it anywhere
    env_off = dict(env); env_off.pop("HASSMIC_FAKE_FRAME")
    with open(os.path.join(state, "config"), "w") as f: f.write("ring_live=on\n")
    p0 = subprocess.Popen([HOST, "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-W", "0", "-a", "0", "-z", "0", "-o", "0"],
                          env=env_off, stderr=open(os.path.join(tmp, "log-off"), "w"))
    try:
        c = await connect()
        ents, services = await c.list_entities_services()
        ring0 = next((e for e in ents if isinstance(e, LightInfo) and e.object_id == "light_ring"), None)
        check(not [e for e in ents if e.object_id.startswith("light_ring_led_") or e.object_id == "light_ring_design"]
              and not [s for s in services if s.name in ("light_ring_frame", "light_ring_animation", "light_ring_design", "light_ring_stop")]
              and ring0 is not None and list(ring0.effects) == ["None", "Pulse", "Spin", "Blink", "Rainbow"],
              f"not writable here: no live entity or action, \"Light ring\" as before ({ring0 and list(ring0.effects)})")
        await c.disconnect()
    finally:
        p0.terminate(); p0.wait()
    check(read() == DARK and "drawn by the live ring" not in open(os.path.join(tmp, "log-off")).read(), "  the ring's frame untouched, the palette's light as before")
    os.unlink(os.path.join(state, "config"))

    # Off (the default): nothing of it in Home Assistant, the frame untouched
    proc = start()
    try:
        c = await connect()
        ents, services = await c.list_entities_services()
        check(not [e for e in ents if e.object_id.startswith("light_ring_led_")] and not [s for s in services if s.name.startswith("light_ring_frame")],
              "\"Light ring live\" off: no LED lights, no actions")
        await c.disconnect()
    finally:
        proc.terminate(); proc.wait()
    check(read() == DARK, "  the frame untouched")

    with open(os.path.join(state, "config"), "a") as f: f.write("ring_live=on\nled_auto_brightness=off\nled_brightness=100\n")
    proc = start()
    try:
        c = await connect()
        ents, services = await c.list_entities_services()
        lights = sorted([e for e in ents if isinstance(e, LightInfo) and e.object_id.startswith("light_ring_led_")], key=lambda e: int(e.object_id.split("_")[-1]))
        ring = next((e for e in ents if isinstance(e, LightInfo) and e.object_id == "light_ring"), None)
        check(ring is not None, f"  the set installed: \"Light ring\" there too ({[e.object_id for e in ents if isinstance(e, LightInfo)][:3]})")
        if ring is None: print(open(os.path.join(tmp, "log")).read()[-3000:]); raise SystemExit(1)
        sel = next((e for e in ents if isinstance(e, SelectInfo) and e.object_id == "light_ring_design"), None)
        svc = {s.name: s for s in services}
        check(len(lights) == 12 and lights[0].name == "Light ring LED 1" and list(lights[0].supported_color_modes) == [35],
              f"on: 12 lights, RGB ({[e.name for e in lights][:2]}...)")
        check(all(n in svc for n in ("light_ring_frame", "light_ring_animation", "light_ring_design", "light_ring_stop"))
              and [a.name for a in svc["light_ring_frame"].args] == ["colors", "brightness", "timeout"], f"  the four actions ({sorted(svc)})")
        check(sel is not None and list(sel.options) == ["None", "Sunrise"], f"  the design select: None and the library ({sel and sel.options})")
        states = []; c.subscribe_states(states.append)
        def last(key, kind): return ([s for s in states if isinstance(s, kind) and s.key == key] or [None])[-1]
        await until(lambda: last(lights[0].key, LightState) is not None)
        check(last(lights[0].key, LightState).state is False and last(sel.key, SelectState).state == "None", "  all off, no design")

        # one LED: LED 1 is where the fill starts (the driver's 11)
        c.light_command(lights[0].key, state=True, rgb=(1.0, 0.0, 0.0), brightness=1.0)
        await until(lambda: pixels(read())[11] == "FD0000")      # after the light's short fade
        px = pixels(read())
        check(px[11] == "FD0000" and px[:11] == ["000000"] * 11, f"LED 1 red: the driver's LED 11 at FD, stock's most at level 100 ({px})")
        await until(lambda: last(lights[0].key, LightState).state)
        check(last(lights[0].key, LightState).state and last(lights[0].key, LightState).red == 1.0, "  its state on, red")
        c.light_command(lights[0].key, brightness=0.5)
        await until(lambda: pixels(read())[11] == "360000")
        check(pixels(read())[11] == "360000", f"  at half: 0.5^2.2 x 255 x 0.993 = 36 ({pixels(read())[11]})")

        # a whole frame from the action
        await c.execute_service(svc["light_ring_frame"], {"colors": ["#0000ff", "", "00ff00"], "brightness": 255, "timeout": 0})
        await until(lambda: pixels(read())[11] == "0000FD")
        px = pixels(read())
        check(px[11] == "0000FD" and px[0] == "000000" and px[1] == "00FD00", f"light_ring_frame: blue on LED 1, dark 2, green 3 ({px})")
        await until(lambda: last(lights[2].key, LightState) and last(lights[2].key, LightState).state)
        check(last(lights[0].key, LightState).blue == 1.0 and not last(lights[1].key, LightState).state and last(lights[2].key, LightState).state,
              "  the lights' states follow")
        ours = read()

        # The Echo's own first: the wake word's ring.  ledcontroller writes it (here: the test), hassmic not once
        async def nothing(*a): pass
        started = asyncio.Event()
        async def handle_start(*a): started.set(); return 0
        c.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        await asyncio.sleep(0.3)
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        await asyncio.sleep(0.2)
        theirs = "0000FF" * 10 + "00FFFF" * 2 + "\n"
        with open(frame, "w") as f: f.write(theirs)
        await asyncio.sleep(0.8)
        check(read() == theirs, "wake word: the listening ring is the Echo's, live writes nothing")
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "nothing"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await until(lambda: read() == ours, 5)
        check(read() == ours, "  the ring closed: ours back")

        # muted: the red ring is the Echo's
        mute = next(e for e in ents if isinstance(e, SwitchInfo) and e.object_id == "mute")
        c.switch_command(mute.key, True); await asyncio.sleep(0.4)
        with open(frame, "w") as f: f.write("FF0000" * 12 + "\n")
        await asyncio.sleep(0.6)
        check(read() == "FF0000" * 12 + "\n", "muted: live writes nothing")
        c.switch_command(mute.key, False)
        await until(lambda: read() == ours, 3)
        check(read() == ours, "  unmuted: ours back")

        # ledcontroller writes over it without hassmic knowing (a pattern another daemon set): put back
        with open(frame, "w") as f: f.write(DARK)
        await until(lambda: read() == ours, 2)
        check(read() == ours, "written over: put back within a check")

        # a design of the library, played by the Echo, as drawn (LED k = the driver's k)
        c.select_command(sel.key, "Sunrise")
        await until(lambda: pixels(read())[0] == "FD3700")
        check(pixels(read())[0] == "FD3700", f"design \"Sunrise\" from the select: its first frame (FF8000 -> FD3700) ({pixels(read())[:2]})")
        await until(lambda: pixels(read())[1] == "FD3700", 2)
        check(pixels(read())[1] == "FD3700", "  its second frame 200 ms later")
        await until(lambda: last(sel.key, SelectState).state == "Sunrise")
        check(last(sel.key, SelectState).state == "Sunrise", "  the select says it plays")
        c.select_command(sel.key, "None")
        await until(lambda: read() == ours)
        check(read() == ours, "  None: the LEDs' own frame again")

        # an animation from Home Assistant, once: the LEDs' frame after it
        await c.execute_service(svc["light_ring_animation"], {"frames": ["100:FFFFFF," + ",".join(["000000"] * 11),
                                                                   "100:000000,FFFFFF," + ",".join(["000000"] * 10)], "repeat": 1, "brightness": 255})
        await until(lambda: pixels(read())[11] == "FDFDFD")
        check(pixels(read())[11] == "FDFDFD", "light_ring_animation: white on LED 1 (the fill's start)")
        await until(lambda: read() == ours, 2)
        check(read() == ours, "  once through, then the LEDs' own again")

        # Home Assistant's "Light ring" drawn live, under the LEDs' own lights: its exact colour, no palette pattern
        check(list(ring.effects)[:5] == ["None", "Pulse", "Spin", "Blink", "Rainbow"] and "Fire" in ring.effects and "Comet" in ring.effects,
              f"\"Light ring\" offers the live ring's effects too ({list(ring.effects)})")
        open(leds, "w").close()
        c.light_command(ring.key, state=True, rgb=(1.0, 0.5, 0.0), brightness=1.0)
        await until(lambda: pixels(read())[5] == "FD3600")
        px = pixels(read())
        check(px[5] == "FD3600" and px[11] == "0000FD" and px[1] == "00FD00", f"\"Light ring\" orange: exact (0.5 green: FD3600, not the palette's), the LEDs' own over it ({px})")
        check(not [l for l in open(leds).read().split() if l.startswith("hassmic_solid") or l.startswith("hassmic_pulse")], "  no palette pattern to ledctrl")
        await until(lambda: last(lights[0].key, LightState).state and last(ring.key, LightState).state)
        check(last(lights[0].key, LightState).state and last(ring.key, LightState).state, "  both on in Home Assistant")
        # every effect, its state back in Home Assistant whole (the longest names once cut the state message, and
        # Home Assistant dropped the link and came back, again and again)
        bad = []
        for eff in ring.effects:
            c.light_command(ring.key, effect=eff)
            if not await until(lambda: last(ring.key, LightState).effect == eff, 3): bad.append(eff)
        check(not bad and c._connection is not None and c._connection.is_connected,
              f"  each of the {len(ring.effects)} effects comes back as Home Assistant's state, the link up ({bad or 'all'})")
        c.light_command(ring.key, effect="Fire")
        await asyncio.sleep(0.5); a = read(); await asyncio.sleep(0.3)
        check(a != read() and pixels(a)[5][4:] == "00", f"  Fire: it flickers by itself, warm ({pixels(a)[5]})")
        # "Music beat" on Home Assistant's media (play_media): quiet before, pumping with the kick while it plays
        wav = kick_wav()
        class H(BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.send_header("Content-Length", str(len(wav))); self.end_headers()
                self.wfile.write(wav)
            def log_message(self, *a): pass
        httpd = ThreadingHTTPServer(("127.0.0.1", 16987), H); threading.Thread(target=httpd.serve_forever, daemon=True).start()
        c.light_command(lights[0].key, state=False); c.light_command(lights[2].key, state=False)
        c.light_command(ring.key, effect="Music beat", rgb=(1.0, 1.0, 1.0), brightness=1.0)
        def bright5():                                 # LED 6's brightest channel (white takes the effect's own colours)
            px = pixels(read())[5]; return max(int(px[i:i + 2], 16) for i in (0, 2, 4))
        await asyncio.sleep(0.8)
        quiet = max(bright5() for _ in range(3))
        check(quiet <= 0x10, f"Music beat, nothing playing: a quiet breath ({quiet:02X})")
        mp = next(e for e in ents if isinstance(e, MediaPlayerInfo))
        c.media_player_command(mp.key, media_url="http://127.0.0.1:16987/kick.wav")
        await until(lambda: bright5() >= 0x80, 6)       # HA's media: fetched and decoded first
        vals = []
        for _ in range(int(2.4 / 0.02)):
            await asyncio.sleep(0.02); vals.append(bright5())
        lo, hi = min(vals), max(vals)
        flashes = 1 + sum(1 for a, b in zip(vals, vals[1:]) if b >= 0x80 and a < 0x80)
        check(hi >= 0xA0 and lo <= 0x40 and 4 <= flashes <= 6, f"  the kick plays: it pumps with it (between {lo:02X} and {hi:02X}, {flashes} kicks in 2.4 s at 120 bpm)")
        httpd.shutdown()
        await asyncio.sleep(2.5)
        c.light_command(ring.key, effect="None", rgb=(1.0, 0.5, 0.0), transition_length=0.0)
        await until(lambda: pixels(read())[5] == "FD3600")
        await asyncio.sleep(0.1)
        c.light_command(ring.key, state=False, transition_length=1.0)
        await asyncio.sleep(0.5); mid = pixels(read())[5]
        await until(lambda: pixels(read())[5] == "000000", 3)
        check(mid not in ("000000", "FD3600") and pixels(read())[5] == "000000", f"  off with a transition of 1 s: fades ({mid} half way), then dark")
        c.light_command(lights[0].key, state=False); c.light_command(lights[2].key, state=False)
        c.light_command(lights[4].key, state=True, rgb=(0.0, 0.0, 1.0), brightness=1.0)
        await until(lambda: pixels(read())[3] == "0000FD" and pixels(read())[11] == "000000")
        check(pixels(read())[3] == "0000FD", "LED 5 = the driver's 3")

        # a frame with a timeout, then stop
        await c.execute_service(svc["light_ring_frame"], {"colors": ["ffffff"], "brightness": 128, "timeout": 1})
        await until(lambda: pixels(read())[11] == "373737")
        check(pixels(read())[11] == "373737", "a frame at brightness 128: (128/255)^2.2 x 255 x 0.993 = 37")
        await until(lambda: read() == DARK, 3)
        check(read() == DARK and not last(lights[0].key, LightState).state, "  its timeout: dark, the light off")
        await c.execute_service(svc["light_ring_frame"], {"colors": ["ff0000"], "brightness": 255, "timeout": 0})
        await until(lambda: read() != DARK)
        await c.execute_service(svc["light_ring_stop"], {})
        await until(lambda: read() == DARK)
        check(read() == DARK and not os.path.exists(os.path.join(state, "ring-live")), "light_ring_stop: dark, nothing of ours left")
        await c.execute_service(svc["light_ring_frame"], {"colors": ["nonsense"], "brightness": 255, "timeout": 0})
        await asyncio.sleep(0.4)
        check(read() == DARK, "  a colour that is none: refused, nothing written")

        # the settings page: a draft shown on the ring at once (no save, no Apply), its name in Home Assistant's select
        # where it is the library's, stopped again
        wait_port(WEB)
        b = Browser(WEB)
        check(b.login_with_button(proc), "the settings page logged in")
        st, _, body = b.call("GET", "/api/ring")
        check(st == 200 and json.loads(body).get("live") == {"on": True, "playing": ""}, f"GET /api/ring says the live ring is on ({json.loads(body).get('live')})")
        draft = "name Sunrise\nleds 12\nloop 0\n500:00FF00," + ",".join(["000000"] * 11) + "\n"
        st, _, body = b.call("POST", "/api/ring/live", draft.encode())
        await until(lambda: pixels(read())[0] == "00FD00")
        check(st == 200 and pixels(read())[0] == "00FD00", f"POST /api/ring/live: the draft on the ring, as drawn ({pixels(read())[:2]})")
        await until(lambda: last(sel.key, SelectState).state == "Sunrise")
        check(json.loads(b.call("GET", "/api/ring")[2])["live"]["playing"] == "Sunrise" and last(sel.key, SelectState).state == "Sunrise",
              "  the page and Home Assistant's select say what plays")
        st, _, body = b.call("POST", "/api/ring/live", b"leds 12\n1:FF\n")
        check(st == 400 and "frame" in json.loads(body)["error"], f"  not a design: refused ({body})")
        st, _, _ = b.call("POST", "/api/ring/live", b"")
        await until(lambda: read() == DARK)
        check(st == 200 and read() == DARK, "  an empty body stops it")
        # hassmic stopped (init's SIGTERM: a restart, an update) while an LED shows: the dark frame before it ends
        c.light_command(lights[0].key, state=True, rgb=(0.0, 1.0, 0.0), brightness=1.0)
        await until(lambda: pixels(read())[11] == "00FD00")
        await c.disconnect()
        proc.send_signal(signal.SIGTERM); proc.wait()
        check(read() == DARK, "SIGTERM with an LED on: dark before it ended")
    finally:
        proc.terminate(); proc.wait()

    # ... and while muted: the red ring is the Echo's, left alone; the next start leaves it too (the marker holds what
    # was ours, and the ring shows something else now)
    mute_ring = "680000" * 12 + "\n"
    proc = start()
    try:
        c = await connect()
        ents, _ = await c.list_entities_services()
        lights = sorted([e for e in ents if isinstance(e, LightInfo) and e.object_id.startswith("light_ring_led_")], key=lambda e: int(e.object_id.split("_")[-1]))
        mute = next(e for e in ents if isinstance(e, SwitchInfo) and e.object_id == "mute")
        c.light_command(lights[0].key, state=True, rgb=(0.0, 1.0, 0.0), brightness=1.0)
        await until(lambda: pixels(read())[11] == "00FD00")
        await asyncio.sleep(0.3)
        c.switch_command(mute.key, True); await asyncio.sleep(0.5)
        with open(frame, "w") as f: f.write(mute_ring)         # ledcontroller's mics-off ring
        await c.disconnect()
        proc.send_signal(signal.SIGTERM); proc.wait()
        check(read() == mute_ring, "SIGTERM while muted: the mute's red ring left alone")
    finally:
        proc.terminate(); proc.wait()
    proc = start()
    try:
        c = await connect(); await asyncio.sleep(0.5)
        check(read() == mute_ring, "the next start: not ours on the ring any more, left alone")
        ents, _ = await c.list_entities_services()
        mute = next(e for e in ents if isinstance(e, SwitchInfo) and e.object_id == "mute")
        c.switch_command(mute.key, False); await asyncio.sleep(0.3)
        await c.disconnect()
    finally:
        proc.terminate(); proc.wait()
    log = open(os.path.join(tmp, "log")).read()
    check("light ring live:" in log and "is no colour" in log, "the log says what it did")
    if not check.failed: shutil.rmtree(tmp, ignore_errors=True)
    else: print("kept:", tmp)
    print("FAILED" if check.failed else "all ok")
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
