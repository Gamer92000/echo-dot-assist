#!/usr/bin/env python3
"""The light ring live on binary animations (the Echo Dot 5) against build/hassmic-host, as Home Assistant: no frame
file there, each frame goes through the stand-in for libhmled.so in ledcontroller (tests/fake_ha_ring_acs.py's Shim,
one client at a time: hassmic keeps to one connection) into the slot "hassmic_live", set once and replaced in place
(the real shim swaps it with "hassmic_live_b" itself, ledshim.c).  At full duty
whatever the ring's level (the LED manager scales by its own tables); the LEDs, "Light ring" drawn live (no palette
slot of its own meanwhile), an effect streaming frames; the Echo's own first: the slot unset before the wake word's
ring and while muted, not one frame meanwhile, set again after; ledcontroller started again (the shim's connection
gone): set again; stop and off: unset; killed outright: the next start unsets it."""
import asyncio, os, shutil, signal, subprocess, sys, tempfile
from aioesphomeapi import APIClient, LightInfo, LightState, SelectInfo, SwitchInfo, VoiceAssistantEventType as Ev

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_ha_ring_acs import stock, Shim

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = f"{ROOT}/build/hassmic-host"
PORT = 16989
SLOT = "hassmic_live"


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def until(cond, timeout=5.0):
    end = asyncio.get_running_loop().time() + timeout
    while not cond() and asyncio.get_running_loop().time() < end:
        await asyncio.sleep(0.02)
    return cond()


async def main():
    tmp = tempfile.mkdtemp(); etc = os.path.join(tmp, "etc"); state = os.path.join(tmp, "state"); os.makedirs(state)
    stock(etc)
    leds, beam, shimname = os.path.join(tmp, "ledctrl.log"), os.path.join(tmp, "beam"), f"hassmic-led-live-{os.getpid()}"
    env = dict(os.environ, HASSMIC_LED_ROOT=etc, HASSMIC_FAKE_LED=leds, HASSMIC_FAKE_BEAM=beam, HASSMIC_STATE=state,
               HASSMIC_LED_SHIM=shimname, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(tmp, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(tmp, "mdns"), HASSMIC_FAKE_ADDRS=os.path.join(tmp, "addrs"),
               HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(tmp, "models"))
    with open(env["HASSMIC_FAKE_ADDRS"], "w") as f: f.write("127.0.0.1\n")
    with open(os.path.join(state, "adopted"), "w") as f: f.write("ha\n")
    with open(beam, "w") as f: f.write("3\n")
    open(env["HASSMIC_PLAY"], "wb").close()
    os.makedirs(os.path.join(state, "ring"))
    with open(os.path.join(state, "ring", "1"), "w") as f:
        f.write("name Sunrise\nleds 12\nloop 0\n200:FF8000," + ",".join(["000000"] * 11) + "\n200:000000,FF8000," + ",".join(["000000"] * 10) + "\n")

    # the set, installed as root's sysinstall.sh leds does
    out = os.path.join(tmp, "out"); os.makedirs(out)
    subprocess.run([HOST, "-G", out], env=env, check=True, capture_output=True)
    d = os.path.join(etc, "led-resources")
    for f in ("layer_config_common.json", "acs_animation_list.txt", "acs_animation_count.txt", "acs_animation_name_list.txt",
              "acs_auto_stop_conf.txt", "layer_config_common.txt"):
        shutil.copy(os.path.join(d, f), os.path.join(etc, f + ".pre-hassmic"))
    for n in os.listdir(out):
        if n != "digest": shutil.copy(os.path.join(out, n), os.path.join(d, n))
    shutil.copy(os.path.join(out, "digest"), os.path.join(etc, "hassmic-leds.digest"))
    with open(os.path.join(state, "config"), "w") as f: f.write("ring_live=on\nled_auto_brightness=off\nled_brightness=50\n")

    shim = Shim(shimname)

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

    def log(): return [l.split() for l in open(leds).read().splitlines()]
    def ops(name): return [x[0] for x in log() if len(x) > 1 and x[1] == name]
    def frames(): return [t for n, t in shim.updates if n == SLOT]
    def px(i=-1):
        f = frames()
        return f[i].splitlines()[-1].split(":")[1].split(",") if f else []

    proc = start()
    try:
        c = await connect()
        ents, services = await c.list_entities_services()
        lights = sorted([e for e in ents if isinstance(e, LightInfo) and e.object_id.startswith("light_ring_led_")], key=lambda e: int(e.object_id.split("_")[-1]))
        ring = next((e for e in ents if isinstance(e, LightInfo) and e.object_id == "light_ring"), None)
        sel = next((e for e in ents if isinstance(e, SelectInfo) and e.object_id == "light_ring_design"), None)
        svc = {s.name: s for s in services}
        check(len(lights) == 12 and ring is not None and "Fire" in ring.effects and sel is not None
              and all(n in svc for n in ("light_ring_frame", "light_ring_animation", "light_ring_design", "light_ring_stop")),
              f"Echo Dot 5: the 12 LED lights, the live effects, the design select and the actions ({len(lights)})")
        check(ops(SLOT) == ["-u"], f"  at the start: what a run killed outright left unset ({ops(SLOT)})")
        states = []; c.subscribe_states(states.append)
        def last(key): return ([s for s in states if isinstance(s, LightState) and s.key == key] or [None])[-1]

        open(leds, "w").close(); shim.updates.clear()
        c.light_command(lights[0].key, state=True, rgb=(1.0, 0.0, 0.0), brightness=1.0)
        await until(lambda: px()[11:] == ["FF0000"])
        check(px()[11:] == ["FF0000"] and px()[:11] == ["000000"] * 11 and frames()[-1].startswith("loop\n1000:"),
              f"LED 1 red: one looped frame into the slot, LED 11 of the ring at FF, full duty at level 50 ({px()})")
        check(ops(SLOT) == ["-s"], f"  the slot set once ({ops(SLOT)})")
        n0 = len(frames())
        c.light_command(lights[0].key, rgb=(0.0, 0.0, 1.0))
        await until(lambda: px()[11:] == ["0000FF"])
        check(px()[11:] == ["0000FF"] and len(frames()) > n0 and ops(SLOT) == ["-s"], "  blue: replaced in place, not set again")
        await until(lambda: last(lights[0].key) and last(lights[0].key).blue == 1.0)

        # "Light ring" drawn live: its exact colour, the LED over it, the palette's slots left alone
        c.light_command(ring.key, state=True, rgb=(1.0, 0.5, 0.0), brightness=1.0, transition_length=0.0)
        await until(lambda: px()[5:6] == ["FF3700"])
        check(px()[5] == "FF3700" and px()[11] == "0000FF", f"\"Light ring\" orange live: 0.5 green exact (37), LED 1 over it ({px()})")
        check(not [x for x in log() if len(x) > 1 and x[1].startswith("hassmic_light_")], "  no slot of the palette's light set")

        # an effect: frames stream into the slot, set only once
        c.light_command(ring.key, effect="Fire")
        await asyncio.sleep(0.3); n0 = len(frames()); await asyncio.sleep(0.5)
        check(len(frames()) - n0 >= 5 and ops(SLOT) == ["-s"], f"Fire: frames stream ({len(frames()) - n0} in 0.5 s), the slot set once")

        # the Echo's own first: unset before the listening ring, nothing while it shows, set again after
        async def nothing(*a): pass
        started = asyncio.Event()
        async def handle_start(*a): started.set(); return 0
        c.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        await asyncio.sleep(0.3); open(leds, "w").close()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        await until(lambda: ["-s", "hassmic_listen_3"] in log())
        l = log()
        check(["-u", SLOT] in l and l.index(["-u", SLOT]) < l.index(["-s", "hassmic_listen_3"]),
              f"wake word: the slot unset first, then the listening ring ({l})")
        await asyncio.sleep(0.2); n0 = len(frames()); await asyncio.sleep(0.6)
        check(len(frames()) == n0 and ops(SLOT) == ["-u"], f"  while it shows: not one frame though Fire moves ({len(frames()) - n0})")
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "nothing"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await until(lambda: ops(SLOT)[-1:] == ["-s"], 5)
        check(ops(SLOT)[-1:] == ["-s"] and len(frames()) > n0, f"  the ring closed: set again, frames again ({ops(SLOT)})")

        # muted: the red ring is the Echo's
        mute = next(e for e in ents if isinstance(e, SwitchInfo) and e.object_id == "mute")
        open(leds, "w").close()
        c.switch_command(mute.key, True)
        await until(lambda: ops(SLOT) == ["-u"])
        await asyncio.sleep(0.2); n0 = len(frames()); await asyncio.sleep(0.5)
        check(ops(SLOT) == ["-u"] and len(frames()) == n0, f"muted: unset, no frame ({ops(SLOT)}, {len(frames()) - n0})")
        c.switch_command(mute.key, False)
        await until(lambda: ops(SLOT)[-1:] == ["-s"], 3)
        check(ops(SLOT) == ["-u", "-s"], f"  unmuted: set again ({ops(SLOT)})")

        # ledcontroller started again (root's watcher): the connection gone, the slot black and unset in the new one
        c.light_command(ring.key, effect="None")
        await asyncio.sleep(0.4); open(leds, "w").close(); conns = shim.conns
        shim.drop()
        await until(lambda: ops(SLOT) == ["-s"] and shim.conns > conns, 3)
        check(ops(SLOT) == ["-s"] and shim.conns == conns + 1 and px()[5] == "FF3700",
              f"ledcontroller started again, the picture still: drawn and set again on a new connection ({ops(SLOT)}, {shim.conns - conns})")

        # a design of the library from the select, through the slot
        c.select_command(sel.key, "Sunrise")
        await until(lambda: px()[0:1] == ["FF3700"])
        check(px()[0] == "FF3700", f"design \"Sunrise\": its frame through the slot ({px()[:2]})")
        c.select_command(sel.key, "None")

        # stop and off: nothing of ours left, the slot unset
        open(leds, "w").close()
        await c.execute_service(svc["light_ring_stop"], {})
        c.light_command(ring.key, state=False, transition_length=0.0)
        await until(lambda: ops(SLOT) == ["-u"], 3)
        check(ops(SLOT) == ["-u"], f"stop, \"Light ring\" off: the slot unset ({ops(SLOT)})")

        # killed outright with ours on: the next start unsets it
        c.light_command(lights[2].key, state=True, rgb=(0.0, 1.0, 0.0), brightness=1.0)
        await until(lambda: ops(SLOT)[-1:] == ["-s"])
        await c.disconnect()
        proc.kill(); proc.wait()
    finally:
        proc.terminate(); proc.wait()
    proc = start()
    try:
        c = await connect()
        await until(lambda: ops(SLOT) == ["-u"], 3)
        check(ops(SLOT) == ["-u"], f"killed with an LED on: the next start unsets the slot ({ops(SLOT)})")
        await c.disconnect()
    finally:
        proc.terminate(); proc.wait()
    lg = open(os.path.join(tmp, "log")).read()
    check("frames into the LED manager's slot" in lg and "cannot write" not in lg, "the log says how it draws, no failed frame")
    if not check.failed: shutil.rmtree(tmp, ignore_errors=True)
    else: print("kept:", tmp)
    print("FAILED" if check.failed else "all ok")
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
