#!/usr/bin/env python3
"""The light ring live (ringlive.c) against build/hassmic-host, as Home Assistant: the LED driver's frame as a file
(HASSMIC_FAKE_FRAME, 12 x RRGGBB as a Dot 3's), the brightness table and the set stand-ins (HASSMIC_LED_ROOT, stock's
from tests/fake_ha_ring.py).  Off: none of its entities.  On: a light per LED, the four actions and the design select;
an LED and a whole frame on the driver at stock's own most (FD at level 100, never more), from where the fill starts;
the Echo's own patterns first (the wake word's ring, mute): not one write meanwhile, ours back after; a frame
ledcontroller wrote over put back; an animation and a design of the library played by the Echo, the select's state;
Home Assistant's "Light ring" and the live ring taking the ring from each other; stop: the dark frame."""
import asyncio, json, os, shutil, signal, subprocess, sys, tempfile
from aioesphomeapi import (APIClient, LightInfo, LightState, NumberInfo, SelectInfo, SelectState, SwitchInfo,
                           VoiceAssistantEventType as Ev)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_ha_ring import stock                  # stock's stand-in animations and layer config

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = f"{ROOT}/build/hassmic-host"
PORT = 16983
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
        return subprocess.Popen([HOST, "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-W", "0", "-a", "0", "-z", "0", "-o", "0"],
                                env=env, stderr=open(os.path.join(tmp, "log"), "a"))

    async def connect():
        for _ in range(100):
            try:
                c = APIClient("127.0.0.1", PORT, None); await c.connect(login=True); return c
            except Exception: await asyncio.sleep(0.1)
        raise RuntimeError("no connection")

    def read(): return open(frame).read()

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
        await until(lambda: pixels(read())[11] != "000000")
        px = pixels(read())
        check(px[11] == "FD0000" and px[:11] == ["000000"] * 11, f"LED 1 red: the driver's LED 11 at FD, stock's most at level 100 ({px})")
        await until(lambda: last(lights[0].key, LightState).state)
        check(last(lights[0].key, LightState).state and last(lights[0].key, LightState).red == 1.0, "  its state on, red")
        c.light_command(lights[0].key, brightness=0.5)
        await until(lambda: pixels(read())[11] != "FD0000")
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

        # Home Assistant's "Light ring" takes the ring: the live ring off, dark; then an LED takes it back
        c.light_command(ring.key, state=True, rgb=(0.0, 1.0, 0.0), brightness=1.0)
        await until(lambda: read() == DARK and not last(lights[0].key, LightState).state)
        check(read() == DARK and not last(lights[0].key, LightState).state, "\"Light ring\" on: the live ring dark, its lights off")
        c.light_command(lights[4].key, state=True, rgb=(0.0, 0.0, 1.0), brightness=1.0)
        await until(lambda: not last(ring.key, LightState).state and pixels(read())[3] == "0000FD")
        check(not last(ring.key, LightState).state and pixels(read())[3] == "0000FD", "an LED on: \"Light ring\" off, the LED there (LED 5 = the driver's 3)")

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
