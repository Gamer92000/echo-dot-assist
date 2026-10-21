#!/usr/bin/env python3
"""The light ring (ring.c, issue #26) against build/hassmic-host, as Home Assistant: the animation set hassmic -G makes
from stock's files (stand-ins written here: no Amazon file), the light entity it offers once that set is installed,
what goes to ledctrl (HASSMIC_FAKE_LED), the Echo's own patterns before the light, and the listening ring turned by
the beam (HASSMIC_FAKE_BEAM) and turned again when the talker moves; the light's fill on a layer per LED and its rotation (issue #57)."""
import asyncio, json, os, re, shutil, signal, struct, subprocess, sys, tempfile, time
from aioesphomeapi import APIClient, LightInfo, LightState, NumberInfo, NumberState, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = f"{ROOT}/build/hassmic-host"
PORT = 16981


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def until(cond, timeout=5.0):
    end = asyncio.get_running_loop().time() + timeout
    while not cond() and asyncio.get_running_loop().time() < end:
        await asyncio.sleep(0.02)
    return cond()


def stock(etc):
    """Stand-ins for stock's files in the shape the Echos have (docs: PLAN.md "Light ring"): the listening ring opens
    and loops lit, its end goes dark and finishes; layers 5 to 0, names first-listed first"""
    d = os.path.join(etc, "led-resources"); os.makedirs(d)
    px = lambda lit: ",".join("00F" if i in lit else "000000" for i in range(12))
    with open(os.path.join(d, "active-start.animation"), "w") as f:
        f.write(f"20:{px({10, 11})}\n20:{px({9, 10, 11, 0})}\nloop\n1000:" + ",".join(["00F"] * 10 + ["0FF", "0FF"]) + "\n")
    with open(os.path.join(d, "active-end.animation"), "w") as f:
        f.write(f"100:{px(set(range(10)))}\n100:{px({4, 5})}\nloop\n1000:{px(set())}\n")
    with open(os.path.join(d, "volume_step-05.animation"), "w") as f:     # 2 s lit, then a black loop: finished
        f.write(f"2000:{px({0, 1, 2})}\nloop\n1000:{px(set())}\n")
    layers = {"5": ["volume_step-05"], "4": ["active-end", "active-start", "anim_start_error_short"], "3": ["ready-timer"],
              "2": ["call_incoming"], "1": ["mics-off_on"], "0": ["zzz_rainbow"]}
    with open(os.path.join(d, "layer_config_common.json"), "w") as f:
        json.dump({"Comment1": "stand-in", "numberOfLayers": 6, "Layers": layers}, f, indent=2)


async def main():
    tmp = tempfile.mkdtemp(); etc = os.path.join(tmp, "etc"); state = os.path.join(tmp, "state"); os.makedirs(state)
    stock(etc)
    leds, beam = os.path.join(tmp, "ledctrl.log"), os.path.join(tmp, "beam")
    env = dict(os.environ, HASSMIC_LED_ROOT=etc, HASSMIC_FAKE_LED=leds, HASSMIC_FAKE_BEAM=beam, HASSMIC_STATE=state,
               HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(tmp, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(tmp, "mdns"), HASSMIC_FAKE_ADDRS=os.path.join(tmp, "addrs"),
               HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(tmp, "models"))
    with open(env["HASSMIC_FAKE_ADDRS"], "w") as f: f.write("127.0.0.1\n")
    with open(os.path.join(state, "adopted"), "w") as f: f.write("ha\n")     # set up: no setup spinner on the ring
    with open(beam, "w") as f: f.write("3\n")
    open(env["HASSMIC_PLAY"], "wb").close()

    # The set, as root's main.sh leds() makes it: hassmic -G into a directory, then into the system's
    out = os.path.join(tmp, "out"); os.makedirs(out)
    r = subprocess.run([HOST, "-G", out], env=env, capture_output=True, text=True)
    names = sorted(os.listdir(out))
    whole = json.load(open(os.path.join(out, "layer_config_common.json")))
    conf = whole["Layers"]
    colours = [f"{h:02d}{s}{l}" for h in range(24) for s in "fs" for l in range(3)] + [f"w{l}" for l in range(3)]
    check(r.returncode == 0 and len(names) == 36 + 588 + 3 + 12 * 147 + 1 + 2, f"-G: the set ({len(names)} files): {r.stderr.strip()}")
    check(whole["numberOfLayers"] == 18 and sorted(conf, key=int) == [str(i) for i in range(18)]
          and conf["16"][:3] == ["hassmic_listen_0", "hassmic_listen_1", "hassmic_listen_2"] and conf["16"][36:] == ["active-end", "active-start", "anim_start_error_short"]
          and conf["12"][0] == "zzz_rainbow" and conf["12"][-1] == "hassmic_rainbow_2" and len(conf["12"]) == 1 + 591
          and conf["17"] == ["hassmic_assist_05", "volume_step-05"] and conf["13"] == ["mics-off_on"]
          and all(sorted(conf[str(l)]) == sorted(f"hassmic_led_{l}_{c}" for c in colours) for l in range(12)),
          "stock's layer config with ours added, its layers moved up by 12: the turned rings first in layer 4, the light last in layer 0, "
          "the Assistant's volume step before stock's; under them a layer per LED with its 147 colours")
    check(open(os.path.join(out, "hassmic_led_3_00f0.animation")).read() == "loop\n1000:" + ",".join("FF0000" if i == 3 else "000000" for i in range(12)) + "\n",
          "a LED's own: red on LED 3, the others black")
    check(open(os.path.join(out, "hassmic_assist_05.animation")).read()
          == "2000:" + ",".join(["00FFFF"] * 3 + ["000000"] * 9) + "\nloop\n1000:" + ",".join(["000000"] * 12) + "\n",
          "the Assistant's volume step: stock's, its lit LEDs in cyan at their brightness")
    held = open(os.path.join(out, "hassmic_hold_3.animation")).read()
    check(held == "loop\n1000:" + ",".join(["00F", "0FF", "0FF"] + ["00F"] * 9) + "\n",
          f"the open ring turned by 3: its light-blue pair on LEDs 1 and 2, as stock turns it ({held.strip()})")
    check(open(os.path.join(out, "hassmic_solid_00f0.animation")).read() == "loop\n1000:" + ",".join(["FF0000"] * 12) + "\n"
          and open(os.path.join(out, "hassmic_blink_w2.animation")).read().count("1F1F1F") == 12, "palette: red at full, white at the dimmest level blinking")
    for n in names:
        if n.endswith(".animation"):
            for l in open(os.path.join(out, n)).read().splitlines():
                if l != "loop" and not re.fullmatch(r"\d+:[0-9A-F]{3,6}(,[0-9A-F]{3,6}){11}", l): check(False, f"{n}: bad line {l!r}"); break
    r2 = subprocess.run([HOST, "-G", os.path.join(tmp, "out2")], env=dict(env, HASSMIC_LED_ROOT=os.path.join(tmp, "nowhere")), capture_output=True, text=True)
    check(r2.returncode != 0, "-G without stock's layer config: refused")

    def install():
        d = os.path.join(etc, "led-resources")
        if not os.path.exists(os.path.join(etc, "layer_config_common.json.pre-hassmic")):
            shutil.copy(os.path.join(d, "layer_config_common.json"), os.path.join(etc, "layer_config_common.json.pre-hassmic"))
        for n in names:
            if n != "digest": shutil.copy(os.path.join(out, n), os.path.join(d, n))
        shutil.copy(os.path.join(out, "digest"), os.path.join(etc, "hassmic-leds.digest"))

    def start():
        open(leds, "w").close()
        return subprocess.Popen([HOST, "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-W", "0", "-a", "0", "-z", "0", "-o", "0"], env=env)

    def log(): return [l.split() for l in open(leds).read().splitlines()]

    async def connect():
        for _ in range(100):
            try:
                c = APIClient("127.0.0.1", PORT, None); await c.connect(login=True); return c
            except Exception: await asyncio.sleep(0.1)
        raise RuntimeError("no connection")

    # Not installed: stock's only, no light in Home Assistant
    proc = start()
    try:
        c = await connect()
        ents, _ = await c.list_entities_services()
        check(not [e for e in ents if isinstance(e, LightInfo)], "set not installed: no light in Home Assistant")
        await c.disconnect()
    finally:
        proc.terminate(); proc.wait()

    install()
    os.makedirs(os.path.join(tmp, "out3"))
    r3 = subprocess.run([HOST, "-G", os.path.join(tmp, "out3")], env=env, capture_output=True, text=True)
    check(r3.returncode == 0 and open(os.path.join(tmp, "out3", "digest")).read() == open(os.path.join(out, "digest")).read(),
          "-G again once installed: stock's config from the copy kept beside it, the same set")

    proc = start()
    try:
        c = await connect()
        ents, services = await c.list_entities_services()
        li = next((e for e in ents if isinstance(e, LightInfo)), None)
        check(li is not None and li.object_id == "light_ring" and list(li.effects) == ["None", "Pulse", "Spin", "Blink", "Rainbow"]
              and list(li.supported_color_modes) == [35], f"installed: the light, RGB with effects: {li}")
        states = []; c.subscribe_states(states.append)
        st = lambda: ([s for s in states if isinstance(s, LightState)] or [None])[-1]
        await until(lambda: st() is not None)
        check(st() and st().state is False, "off at the start")

        def light_names(): return [(op, n) for op, n in log() if n.startswith("hassmic_") and not re.match(r"hassmic_(listen|hold|end)_", n)]
        open(leds, "w").close()
        c.light_command(li.key, state=True, rgb=(1.0, 0.0, 0.0), color_brightness=1.0, brightness=1.0)
        await until(lambda: light_names() == [("-s", "hassmic_solid_00f0")] and st().state)
        check(light_names() == [("-s", "hassmic_solid_00f0")] and st().state and (st().red, st().green, st().blue) == (1.0, 0.0, 0.0),
              f"red on: hassmic_solid_00f0 on the ring, the state as asked ({light_names()})")
        open(leds, "w").close()
        c.light_command(li.key, state=True, rgb=(1.0, 0.5, 0.0), brightness=0.5, effect="Pulse")
        # the ring's calls and the state to HA are separate paths: wait for both (CI once read the state first)
        await until(lambda: len(light_names()) == 2 and st().effect == "Pulse")
        check(light_names() == [("-s", "hassmic_pulse_02f1"), ("-u", "hassmic_solid_00f0")] and st().effect == "Pulse",
              f"orange at half, pulsing: the new one first, then the old one off ({light_names()})")
        open(leds, "w").close()
        c.light_command(li.key, rgb=(0.6, 0.6, 1.0), brightness=0.2, effect="None")
        await until(lambda: len(light_names()) == 2)
        check(light_names()[0] == ("-s", "hassmic_solid_16s2"), f"pale blue, dim: the half-saturated hue, dimmest level ({light_names()})")

        # The Echo's own: the wake word.  The light goes before the open ring comes, turned by the beam (3); the talker
        # moves (beam 7): the open ring there, without opening again; the run ends: its end turned, then the light back
        async def nothing(*a): pass
        started = asyncio.Event()
        async def handle_start(*a): started.set(); return 0
        c.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        await asyncio.sleep(0.3); open(leds, "w").close()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        await until(lambda: ["-s", "hassmic_listen_3"] in log())
        l = log()
        check(["-u", "hassmic_solid_16s2"] in l and ["-s", "hassmic_listen_3"] in l
              and l.index(["-u", "hassmic_solid_16s2"]) < l.index(["-s", "hassmic_listen_3"]),
              f"wake word: the light off first, then the listening ring turned by beam 3 ({l})")
        with open(beam, "w") as f: f.write("7\n")
        await until(lambda: ["-s", "hassmic_hold_7"] in log())
        l = log()
        check(["-s", "hassmic_hold_7"] in l and ["-u", "hassmic_listen_3"] in l, f"the talker moved: the open ring at 7, the old one gone ({l})")
        await asyncio.sleep(0.6)
        check(not [x for x in log() if x[0] == "-s" and x[1].startswith("hassmic_") and not re.match(r"hassmic_(listen|hold|end)_", x[1])],
              "the light stays off while the ring is open")
        open(leds, "w").close()
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "nothing"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await until(lambda: ["-s", "hassmic_solid_16s2"] in log(), 5)
        l = log()
        check(["-u", "hassmic_hold_7"] in l and ["-s", "hassmic_end_7"] in l and ["-s", "hassmic_solid_16s2"] in l
              and l.index(["-s", "hassmic_end_7"]) < l.index(["-s", "hassmic_solid_16s2"]),
              f"run over: the ring closes turned by 7, then the light is back ({l})")

        # A flash (Home Assistant's "flash: short", 2 s): blinking in the colour asked for, then as before
        open(leds, "w").close()
        c.light_command(li.key, state=True, rgb=(0.0, 1.0, 0.0), flash_length=2.0)     # at the light's brightness
        await until(lambda: ["-s", "hassmic_blink_08f2"] in log() and st().effect == "None")
        check(["-s", "hassmic_blink_08f2"] in log() and abs(st().green - 0.6) < 1e-6 and st().effect == "None",
              f"flash: green blinking; the light's state unchanged ({log()}, {st()})")
        await until(lambda: ["-s", "hassmic_solid_16s2"] in log(), 4)
        check(["-s", "hassmic_solid_16s2"] in log() and ["-u", "hassmic_blink_08f2"] in log(), "after the flash: the light as before")

        open(leds, "w").close()
        c.light_command(li.key, effect="Rainbow")
        await until(lambda: ["-s", "hassmic_rainbow_2"] in log() and st().effect == "Rainbow")     # ledctrl and HA's state: either first
        check(["-s", "hassmic_rainbow_2"] in log() and st().effect == "Rainbow", "rainbow at the light's level")
        open(leds, "w").close()
        c.light_command(li.key, state=False)
        await until(lambda: ["-u", "hassmic_rainbow_2"] in log() and st().state is False)
        check(log() == [["-u", "hassmic_rainbow_2"]] and st().state is False, f"off: off the ring ({log()})")
        c.light_command(li.key, state=True)
        await until(lambda: ["-s", "hassmic_rainbow_2"] in log() and st().effect == "Rainbow")     # ledctrl and HA's state: either first

        # The fill: each LED's own pattern for a part of the ring, the whole ring's at 100 %; every new one on before an
        # old one goes.  The light is pale blue at the dimmest level (16s2) under the rainbow effect
        nums = {e.object_id: e for e in ents if isinstance(e, NumberInfo)}
        fill, rot = nums.get("light_ring_fill"), nums.get("light_ring_rotation")
        check(fill and rot and (fill.min_value, fill.max_value, fill.unit_of_measurement) == (0, 100, "%")
              and (rot.max_value, rot.entity_category) == (11, 1), f"the fill (%) and the rotation (0-11, a config entity): {fill} {rot}")
        num = lambda e: ([s.state for s in states if isinstance(s, NumberState) and s.key == e.key] or [None])[-1]
        await until(lambda: num(fill) == 100 and num(rot) == 0)
        check(num(fill) == 100 and num(rot) == 0, f"at the start: all of it, from stock's start ({num(fill)}, {num(rot)})")
        led = lambda op, *ids: [[op, f"hassmic_led_{i}_16s2"] for i in ids]
        open(leds, "w").close()
        c.number_command(fill.key, 50)
        await until(lambda: len(log()) == 7 and num(fill) == 50)
        check(log() == led("-s", 0, 1, 2, 3, 4, 11) + [["-u", "hassmic_rainbow_2"]] and num(fill) == 50,
              f"50 %: six LEDs from where the volume starts (11), then the whole ring off ({log()})")
        open(leds, "w").close()
        c.number_command(rot.key, 3)
        await until(lambda: len(log()) == 6 and num(rot) == 3)
        check(log() == led("-s", 5, 6, 7) + led("-u", 0, 1, 11) and num(rot) == 3, f"turned by 3: LEDs 2 to 7, the new ones first ({log()})")
        check("ring_rotation=3" in open(os.path.join(state, "config")).read().split(), "the rotation saved in state/config")
        open(leds, "w").close()
        c.number_command(fill.key, 1)
        await until(lambda: len(log()) == 5)
        check(log() == led("-u", 3, 4, 5, 6, 7), f"1 %: one LED, rounded up ({log()})")
        open(leds, "w").close()
        c.number_command(fill.key, 0)
        await until(lambda: log() and num(fill) == 0)
        check(log() == led("-u", 2) and st().state, f"0 %: off the ring, still on in Home Assistant ({log()})")
        open(leds, "w").close()
        c.number_command(fill.key, 100)
        await until(lambda: log() and num(fill) == 100)
        check(log() == [["-s", "hassmic_rainbow_2"]], f"100 %: the whole ring, with its effect ({log()})")
        open(leds, "w").close()
        c.number_command(fill.key, 25)
        await until(lambda: len(log()) == 4)
        check(log() == led("-s", 2, 3, 4) + [["-u", "hassmic_rainbow_2"]], f"25 %: three LEDs ({log()})")

        # LED by LED (the action): from the fill's start (LED 2 with the rotation 3), at the light's level; dark ones off
        svc = next((v for v in services if v.name == "light_ring_leds"), None)
        check(svc is not None and [(a.name, int(a.type)) for a in svc.args] == [("colors", 7)], f"the action: colors, a string array ({svc})")
        open(leds, "w").close()
        await c.execute_service(svc, {"colors": ["#ff0000", "", "0000FF", "nonsense"]})
        await until(lambda: len(log()) == 5)
        check(log() == [["-s", "hassmic_led_2_00f2"], ["-s", "hassmic_led_4_16f2"]] + led("-u", 2, 3, 4),
              f"red on LED 2, LED 3 dark, blue on LED 4, nonsense dark ({log()})")
        open(leds, "w").close()
        c.light_command(li.key, rgb=(0.6, 0.6, 1.0))
        await until(lambda: len(log()) == 5)
        check(log() == led("-s", 2, 3, 4) + [["-u", "hassmic_led_2_00f2"], ["-u", "hassmic_led_4_16f2"]],
              f"a colour from Home Assistant: one colour again, the fill's ({log()})")
        await c.disconnect()
    finally:
        proc.terminate(); proc.wait()

    # A restart with the light on: ledcontroller still plays it, so the new hassmic takes it off
    with open(os.path.join(state, "ring-light")) as f: left = f.read().split()
    proc = start()
    try:
        await until(lambda: all(["-u", n] in log() for n in left))
        check(left == ["hassmic_led_2_16s2", "hassmic_led_3_16s2", "hassmic_led_4_16s2"] and all(["-u", n] in log() for n in left),
              f"restart: the fill left on the ring taken off ({left})")
    finally:
        proc.terminate(); proc.wait()
    # No room for the LEDs' own layers (root's leds() then makes the set with HASSMIC_RING_LEDS=0): stock's 6 layers, the
    # light in Home Assistant, no fill, rotation or LED by LED
    small = os.path.join(tmp, "small"); os.makedirs(small)
    r = subprocess.run([HOST, "-G", small], env=dict(env, HASSMIC_RING_LEDS="0"), capture_output=True, text=True)
    sconf = json.load(open(os.path.join(small, "layer_config_common.json")))
    check(r.returncode == 0 and len(os.listdir(small)) == 36 + 588 + 3 + 1 + 2 and sconf["numberOfLayers"] == 6
          and sconf["Layers"]["0"][-1] == "hassmic_rainbow_2", f"-G without the LEDs' own: stock's 6 layers, the light's set ({r.stderr.strip()})")
    d = os.path.join(etc, "led-resources")
    for n in os.listdir(d):
        if n.startswith("hassmic_"): os.unlink(os.path.join(d, n))
    for n in os.listdir(small):
        if n != "digest": shutil.copy(os.path.join(small, n), os.path.join(d, n))
    shutil.copy(os.path.join(small, "digest"), os.path.join(etc, "hassmic-leds.digest"))
    proc = start()
    try:
        c = await connect()
        ents, services = await c.list_entities_services()
        check([e for e in ents if isinstance(e, LightInfo)] and not [e for e in ents if isinstance(e, NumberInfo) and e.object_id.startswith("light_ring_")]
              and not [v for v in services if v.name == "light_ring_leds"], "the set without them taken as ours: the light, no fill, rotation or action")
        await c.disconnect()
    finally:
        proc.terminate(); proc.wait()
    shutil.rmtree(tmp)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

asyncio.run(main())
