#!/usr/bin/env python3
"""The light ring on binary animations (ring.c "ACS", the Echo Dot 5) against build/hassmic-host, as Home Assistant:
stand-ins for stock's .animation_bin files and index files (written here: no Amazon file), the set hassmic -G makes
from them (ours encoded, the index files with ours after stock's, no palette but the light's two slots), and Home
Assistant's light drawn into those slots through a stand-in for libhmled.so in ledcontroller (an abstract socket,
HASSMIC_LED_SHIM): each change into the slot not shown, then shown, then the old one off; a fill and LED by LED drawn
as one animation; the light off the ring while the Echo's own listening ring shows; without the shim nothing shown."""
import asyncio, json, os, re, shutil, signal, socket, struct, subprocess, sys, tempfile, threading
from aioesphomeapi import APIClient, LightInfo, NumberInfo, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = f"{ROOT}/build/hassmic-host"
PORT = 16983


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def until(cond, timeout=5.0):
    end = asyncio.get_running_loop().time() + timeout
    while not cond() and asyncio.get_running_loop().time() < end:
        await asyncio.sleep(0.02)
    return cond()


def encode(text):
    """the Echo Dot 5's binary form of a text animation (ring.c acs_encode; stock's round-trip so)"""
    frames, loop = [], None
    for l in text.splitlines():
        if l == "loop": loop = len(frames); continue
        ms, px = l.split(":")
        frames.append((int(ms), b"".join(bytes.fromhex(p if len(p) == 6 else "".join(c * 2 for c in p)) for p in px.split(","))))
    out = b"animagictotl" + struct.pack(">I", len(frames))
    for i, (ms, px) in enumerate(frames):
        if i == loop: out += b"loop"
        out += b"fram" + struct.pack(">I", ms) + px
    return out


def decode(b):
    assert b[:12] == b"animagictotl"
    n, p, lines = struct.unpack(">I", b[12:16])[0], 16, []
    while p < len(b):
        if b[p:p + 4] == b"loop": lines.append("loop"); p += 4; continue
        assert b[p:p + 4] == b"fram"
        ms = struct.unpack(">I", b[p + 4:p + 8])[0]
        lines.append(f"{ms}:" + ",".join(b[p + 8 + 3 * i:p + 11 + 3 * i].hex().upper() for i in range(12))); p += 44
    assert len([l for l in lines if l != "loop"]) == n
    return "\n".join(lines) + "\n"


def stock(etc):
    """Stand-ins for stock's files in the Echo Dot 5's shape: numbered binaries, the index files, the layer config as
    JSON and flattened"""
    d = os.path.join(etc, "led-resources"); os.makedirs(d)
    px = lambda lit: ",".join("00F" if i in lit else "000000" for i in range(12))
    anims = {
        "active-start": f"20:{px({10, 11})}\n20:{px({9, 10, 11, 0})}\nloop\n1000:" + ",".join(["00F"] * 10 + ["0FF", "0FF"]) + "\n",
        "active-end": f"100:{px(set(range(10)))}\n100:{px({4, 5})}\nloop\n1000:{px(set())}\n",
        "volume_step-05": f"2000:{px({0, 1, 2})}\nloop\n1000:{px(set())}\n",
        "zzz_rainbow": "loop\n100:" + ",".join(["F00"] * 12) + "\n",
    }
    names = list(anims)
    for i, n in enumerate(names):
        open(os.path.join(d, f"{i}.{n}.animation_bin"), "wb").write(encode(anims[n]))
    layers = {"5": ["volume_step-05"], "4": ["active-end", "active-start"], "3": [], "2": [], "1": [], "0": ["zzz_rainbow"]}
    json.dump({"Comment1": "stand-in", "numberOfLayers": 6, "Layers": layers}, open(os.path.join(d, "layer_config_common.json"), "w"), indent=2)
    open(os.path.join(d, "layer_config_common.txt"), "w").write(flat(layers, 6))
    open(os.path.join(d, "acs_animation_list.txt"), "w").write("".join(f"{i}.{n}.animation_bin\n" for i, n in enumerate(names)))
    open(os.path.join(d, "acs_animation_count.txt"), "w").write(str(len(names)))
    open(os.path.join(d, "acs_animation_name_list.txt"), "w").write("".join(n + "," for n in names))
    open(os.path.join(d, "acs_auto_stop_conf.txt"), "w").write("active-start 0\nactive-end 1\nvolume_step-05 1\nzzz_rainbow 0\n")


def flat(layers, n):
    return f"NumberOfLayers:{n}," + "".join(f"Layer:{k}, Total:{len(layers[str(k)])}," + "".join(x + "," for x in layers[str(k)]) for k in range(n - 1, -1, -1))


class Shim:
    """libhmled.so's side: "<name>\\0<animation_bin>" in, the result as text out; every update kept as text.  One client
    at a time (the real one takes 4): a second connection of hassmic's would wait here until the first is closed.  drop(): ledcontroller started
    again (the connection gone, nothing of ours set in it)"""
    def __init__(self, name):
        self.updates, self.answer, self.conns, self.c = [], "0", 0, None
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); self.s.bind("\0" + name); self.s.listen(4)
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            self.c, _ = self.s.accept(); self.conns += 1
            self.client(self.c)

    def client(self, c):
        while True:
            try: m = c.recv(65536)
            except OSError: m = b""
            if not m: break
            name, data = m.split(b"\0", 1)
            self.updates.append((name.decode(), decode(data)))
            try: c.send(self.answer.encode())
            except OSError: break
        c.close()

    def drop(self):
        if self.c: self.c.shutdown(socket.SHUT_RDWR)


async def main():
    tmp = tempfile.mkdtemp(); etc = os.path.join(tmp, "etc"); state = os.path.join(tmp, "state"); os.makedirs(state)
    stock(etc)
    leds, beam, shimname = os.path.join(tmp, "ledctrl.log"), os.path.join(tmp, "beam"), f"hassmic-led-test-{os.getpid()}"
    env = dict(os.environ, HASSMIC_LED_ROOT=etc, HASSMIC_FAKE_LED=leds, HASSMIC_FAKE_BEAM=beam, HASSMIC_STATE=state,
               HASSMIC_LED_SHIM=shimname, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(tmp, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(tmp, "mdns"), HASSMIC_FAKE_ADDRS=os.path.join(tmp, "addrs"),
               HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(tmp, "models"))
    with open(env["HASSMIC_FAKE_ADDRS"], "w") as f: f.write("127.0.0.1\n")
    with open(os.path.join(state, "adopted"), "w") as f: f.write("ha\n")
    with open(beam, "w") as f: f.write("3\n")
    open(env["HASSMIC_PLAY"], "wb").close()

    # The set, as root's leds() makes it
    out = os.path.join(tmp, "out"); os.makedirs(out)
    r = subprocess.run([HOST, "-G", out], env=env, capture_output=True, text=True)
    files = sorted(os.listdir(out))
    lst = [l for l in open(os.path.join(out, "acs_animation_list.txt")).read().split("\n") if l]
    names = open(os.path.join(out, "acs_animation_name_list.txt")).read().split(",")
    auto = dict(l.split() for l in open(os.path.join(out, "acs_auto_stop_conf.txt")).read().splitlines())
    whole = json.load(open(os.path.join(out, "layer_config_common.json")))
    ours = [n for n in names if n.startswith("hassmic_")]
    check(r.returncode == 0 and len(ours) == 36 + 4 + 1 and not [n for n in ours if re.match(r"hassmic_(solid|pulse|spin|blink|rainbow|led)_", n)],
          f"-G: the listening ring's turns, the light's two slots and the live ring's two, the Assistant's volume step; no palette, no LEDs' own ({len(ours)}: {r.stderr.strip()})")
    check(names[-1] == "" and len(lst) == len(names) - 1 == int(open(os.path.join(out, "acs_animation_count.txt")).read())
          and lst[:4] == ["0.active-start.animation_bin", "1.active-end.animation_bin", "2.volume_step-05.animation_bin", "3.zzz_rainbow.animation_bin"]
          and all(l == f"{i}.{n}.animation_bin" for i, (l, n) in enumerate(zip(lst, names))),
          "the index: stock's lines first and unchanged, ours numbered after them, the names in the same order, a comma after each")
    check(all(f in files for f in lst[4:]) and all(f.endswith(".animation_bin") or f in ("digest", "layer_config_common.json") or f.startswith(("acs_", "layer_config"))
                                                   for f in files), "every animation of ours a numbered binary, nothing in text")
    check(whole["numberOfLayers"] == 6 and whole["Layers"]["0"] == ["zzz_rainbow", "hassmic_light_a", "hassmic_light_b", "hassmic_live", "hassmic_live_b"]
          and whole["Layers"]["4"][:3] == ["hassmic_listen_0", "hassmic_listen_1", "hassmic_listen_2"]
          and open(os.path.join(out, "layer_config_common.txt")).read() == flat(whole["Layers"], 6),
          "the layer config: the slots last in layer 0, the turned rings first in stock's listening layer, flattened as the SCP reads it")
    check(auto["hassmic_light_a"] == auto["hassmic_light_b"] == auto["hassmic_live"] == auto["hassmic_live_b"] == "0" and auto["hassmic_end_3"] == "1" and auto["hassmic_hold_3"] == "0"
          and auto["active-end"] == "1", "auto-stop: stock's kept, ours by the rule (a black loop ends), the slots never")
    held = decode(open(os.path.join(out, next(l for l in lst if ".hassmic_hold_3." in l)), "rb").read())
    check(held == "loop\n1000:" + ",".join(["0000FF", "00FFFF", "00FFFF"] + ["0000FF"] * 9) + "\n",
          f"stock's binary read as text and turned by 3, encoded again ({held.strip()})")

    def install():                          # as root's sysinstall.sh leds: stock's index kept beside, ours in place
        d = os.path.join(etc, "led-resources")
        for f in ("layer_config_common.json", "acs_animation_list.txt", "acs_animation_count.txt", "acs_animation_name_list.txt",
                  "acs_auto_stop_conf.txt", "layer_config_common.txt"):
            if not os.path.exists(os.path.join(etc, f + ".pre-hassmic")): shutil.copy(os.path.join(d, f), os.path.join(etc, f + ".pre-hassmic"))
        for n in files:
            if n != "digest": shutil.copy(os.path.join(out, n), os.path.join(d, n))
        shutil.copy(os.path.join(out, "digest"), os.path.join(etc, "hassmic-leds.digest"))
    install()
    r = subprocess.run([HOST, "-G", "-"], env=env, capture_output=True, text=True)
    check(r.stdout.strip() == open(os.path.join(out, "digest")).read().strip(), "-G again once installed: stock's index from the copies kept, the same set")

    def start():
        open(leds, "w").close()
        return subprocess.Popen([HOST, "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-W", "0", "-a", "0", "-z", "0", "-o", "0"], env=env)

    def log(): return [l.split() for l in open(leds).read().splitlines()]
    def slots(): return [x for x in log() if x[1].startswith("hassmic_light_")]

    async def connect():
        for _ in range(100):
            try:
                c = APIClient("127.0.0.1", PORT, None); await c.connect(login=True); return c
            except Exception: await asyncio.sleep(0.1)
        raise RuntimeError("no connection")

    red = "loop\n1000:" + ",".join(["FF0000"] * 12) + "\n"
    # No shim (ledcontroller without libhmled.so): the light is offered, nothing shows, said once
    proc = start()
    try:
        c = await connect()
        ents, services = await c.list_entities_services()
        li = next((e for e in ents if isinstance(e, LightInfo)), None)
        c.light_command(li.key, state=True, rgb=(1.0, 0.0, 0.0), brightness=1.0)
        await asyncio.sleep(0.8)
        check(li is not None and not slots(), f"no shim: the light offered, nothing set on the ring ({log()})")
        await c.disconnect()
    finally:
        proc.terminate(); proc.wait()
    shim = Shim(shimname)
    proc = start()
    try:
        c = await connect()
        ents, services = await c.list_entities_services()
        li = next((e for e in ents if isinstance(e, LightInfo)), None)
        nums = {e.object_id: e for e in ents if isinstance(e, NumberInfo)}
        check(li is not None and list(li.effects) == ["None", "Pulse", "Spin", "Blink", "Rainbow"] and "light_ring_fill" in nums
              and any(v.name == "light_ring_leds" for v in services), "installed: the light with its effects, the fill, the rotation, LED by LED")
        open(leds, "w").close()
        c.light_command(li.key, state=True, rgb=(1.0, 0.0, 0.0), brightness=1.0, effect="None")
        await until(lambda: slots() and shim.updates)
        check(shim.updates and shim.updates[-1] == ("hassmic_light_a", red) and slots() == [["-s", "hassmic_light_a"]],
              f"red: drawn into slot a, then slot a shown ({shim.updates[-1:]}, {slots()})")
        open(leds, "w").close(); shim.updates.clear()
        c.light_command(li.key, rgb=(1.0, 0.5, 0.0), brightness=0.5, effect="Pulse")
        await until(lambda: len(slots()) == 2)
        up = shim.updates[-1] if shim.updates else ("", "")
        check(up[0] == "hassmic_light_b" and up[1].count("\n") == 25 and slots() == [["-s", "hassmic_light_b"], ["-u", "hassmic_light_a"]],
              f"orange at half, pulsing: 24 frames into slot b, b shown before a goes ({up[0]}, {slots()})")
        open(leds, "w").close(); shim.updates.clear()
        c.number_command(nums["light_ring_fill"].key, 50)
        await until(lambda: len(slots()) == 2)
        up = shim.updates[-1] if shim.updates else ("", "")
        lit = up[1].splitlines()[-1].split(":")[1].split(",") if up[1] else []
        check(up[0] == "hassmic_light_a" and [i for i, p in enumerate(lit) if p != "000000"] == [0, 1, 2, 3, 4, 11],
              f"50 %: one animation, six LEDs from 11 lit, the rest black ({up})")
        open(leds, "w").close(); shim.updates.clear()
        svc = next(v for v in services if v.name == "light_ring_leds")
        await c.execute_service(svc, {"colors": ["#ff0000", "", "0000FF"]})
        await until(lambda: len(slots()) == 2)
        up = shim.updates[-1] if shim.updates else ("", "")
        lit = up[1].splitlines()[-1].split(":")[1].split(",") if up[1] else []
        hue = lambda p: [int(p[k:k + 2], 16) for k in (0, 2, 4)]
        check(up[0] == "hassmic_light_b" and len(lit) == 12 and hue(lit[11])[0] > 0 and hue(lit[11])[1:] == [0, 0] and lit[0] == "000000"
              and hue(lit[1])[2] > 0 and hue(lit[1])[:2] == [0, 0], f"LED by LED at the light's level: red on 11, 0 dark, blue on 1, one animation ({lit})")
        # The Echo's own: the light off the ring before the listening ring comes, turned by the beam; back after
        async def nothing(*a): pass
        started = asyncio.Event()
        async def handle_start(*a): started.set(); return 0
        c.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        await asyncio.sleep(0.3); open(leds, "w").close(); shim.updates.clear()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        await until(lambda: ["-s", "hassmic_listen_3"] in log())
        l = log()
        check(["-u", "hassmic_light_b"] in l and l.index(["-u", "hassmic_light_b"]) < l.index(["-s", "hassmic_listen_3"]) and not shim.updates,
              f"wake word: the slot off first, then the listening ring turned by 3, nothing drawn ({l})")
        open(leds, "w").close()
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "nothing"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await until(lambda: slots(), 5)
        check(slots() and slots()[0][0] == "-s" and shim.updates, f"run over: the light drawn and shown again ({slots()})")
        # ledcontroller started again (root's watcher): the connection hangs up, the light drawn into the new one unasked
        await asyncio.sleep(0.3); open(leds, "w").close(); shim.updates.clear(); conns = shim.conns
        shim.drop()
        await until(lambda: slots() and shim.updates and shim.conns > conns, 3)
        check(slots() and slots()[0][0] == "-s" and shim.updates and shim.conns == conns + 1,
              f"ledcontroller started again: the light drawn and shown again on a new connection ({slots()})")
        open(leds, "w").close()
        c.light_command(li.key, state=False)
        await until(lambda: slots())
        check(len(slots()) == 1 and slots()[0][0] == "-u", f"off: the slot shown taken off ({slots()})")
        # The LED manager refuses (a shim's answer other than 0): nothing switched
        open(leds, "w").close(); shim.answer = "-5"
        c.light_command(li.key, state=True, rgb=(0.0, 1.0, 0.0))
        await asyncio.sleep(0.8)
        check(not slots(), f"refused by the LED manager: no slot shown ({log()})")
        await c.disconnect()
    finally:
        proc.terminate(); proc.wait()
    shutil.rmtree(tmp)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

if __name__ == "__main__":
    asyncio.run(main())
