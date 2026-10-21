#!/usr/bin/env python3
"""The light ring's designs (ringdesign.c, ring.c; issue #38) on build/hassmic-host: the library from the settings page
(tests/webclient.py as the browser), refusals of what is not a design, the assignments, "Apply" with this test as root
(main.sh ota_watch: hassmic -G into a folder, the set copied into the stand-in system partition, the answer in
HASSMIC_LEDS_RESULT), what -G makes of the designs (listening turned by each LED and held at its loop, colours through
gamma, a design just before the stock name it replaces, volume steps in a design's colours), what plays then (the
listening ring turned by the beam, HASSMIC_FAKE_BEAM; Home Assistant's light set again after the ring's restart), root
failing (the applied designs go back to what is installed), an assigned design that does not parse (Apply refuses it),
assignments that cannot be written (the draft stays), packs' ids written in one spelling, the size cap, the library's
cap, deletes, big bodies only from approved keys, a start with designs installed, and an applied file that is a link
(not read: root reads it)."""
import asyncio, json, math, os, shutil, signal, subprocess, sys, tempfile, threading, time
from aioesphomeapi import APIClient, LightInfo, VoiceAssistantEventType as Ev
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser, req

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = f"{ROOT}/build/hassmic-host"
PORT, WEB = 16983, 16984


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def until(cond, secs=5.0):
    end = time.monotonic() + secs
    while not cond() and time.monotonic() < end: await asyncio.sleep(0.05)
    return cond()


def stock(etc):
    """stand-ins for stock's files (fake_ha_ring.py's, with thinking, talking and a call ring)"""
    d = os.path.join(etc, "led-resources"); os.makedirs(d)
    px = lambda lit: ",".join("00F" if i in lit else "000000" for i in range(12))
    files = {
        "active-start": f"20:{px({10, 11})}\nloop\n1000:" + ",".join(["00F"] * 10 + ["0FF", "0FF"]) + "\n",
        "active-end": f"100:{px(set(range(10)))}\nloop\n1000:{px(set())}\n",
        "active-thinking": "loop\n100:" + ",".join(["00F", "0FF"] * 6) + "\n",
        "active-talking": "100:" + ",".join(["0ff"] * 12) + "\n100:" + ",".join(["00F"] * 12) + "\n",
        "volume_step-05": "2000:" + ",".join(["FFF"] * 3 + ["888"] + ["000"] * 8) + "\nloop\n1000:" + ",".join(["000"] * 12) + "\n",
    }
    for n, t in files.items():
        with open(os.path.join(d, n + ".animation"), "w") as f: f.write(t)
    layers = {"5": ["volume_step-05"], "4": ["active-end", "active-start", "anim_start_error_short", "active-thinking", "active-talking"],
              "3": ["ready-timer"], "2": ["call_incoming"], "1": ["mics-off_on"], "0": ["zzz_rainbow"]}
    with open(os.path.join(d, "layer_config_common.json"), "w") as f:
        json.dump({"Comment1": "stand-in", "numberOfLayers": 6, "Layers": layers}, f, indent=2)


def design(name, frames, loop=0, leds=12):
    return (f"name {name}\nleds {leds}\nloop {'once' if loop < 0 else loop}\n" +
            "".join(f"{ms}:" + ",".join(px) + "\n" for ms, px in frames))


def duty(c):
    """ring.c's gamma: what a colour as seen is in the file"""
    return int(math.floor(math.pow(c / 255, 2.2) * 255 + 0.5))


def page_names():
    """the page's scripts share one global scope: a second top-level function of a name silently replaces the first
    (a packshot helper once took the players' frameAt, and every ring on the page stood still)"""
    import collections, glob, re
    seen = collections.Counter()
    for f in glob.glob(f"{ROOT}/web/*.js"):
        seen.update(re.findall(r"^(?:async )?function (\w+)|^(?:const|let|var) (\w+)", open(f).read(), re.M))
    dup = sorted({a or b for (a, b), n in seen.items() if n > 1})
    check(not dup, "the page's scripts declare each top-level name once" + (f" (twice: {', '.join(dup)})" if dup else ""))


async def main():
    page_names()
    tmp = tempfile.mkdtemp(); etc = os.path.join(tmp, "etc"); state = os.path.join(tmp, "state"); os.makedirs(state)
    stock(etc)
    leds, beam, result, log = (os.path.join(tmp, n) for n in ("ledctrl.log", "beam", "leds.result", "log"))
    env = dict(os.environ, HASSMIC_LED_ROOT=etc, HASSMIC_FAKE_LED=leds, HASSMIC_FAKE_BEAM=beam, HASSMIC_STATE=state,
               HASSMIC_LEDS_RESULT=result, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(tmp, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(tmp, "mdns"), HASSMIC_FAKE_ADDRS=os.path.join(tmp, "addrs"),
               HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(tmp, "models"), HASSMIC_LOG=log)
    with open(env["HASSMIC_FAKE_ADDRS"], "w") as f: f.write("127.0.0.1\n")
    with open(os.path.join(state, "adopted"), "w") as f: f.write("ha\n")
    with open(beam, "w") as f: f.write("3\n")
    open(env["HASSMIC_PLAY"], "wb").close()
    lr = os.path.join(etc, "led-resources")

    def install(fail=None):
        """root's leds(): -G into a folder, then into the system's (sysinstall.sh leds) with stock's config kept"""
        out = os.path.join(tmp, "out"); shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
        r = subprocess.run([HOST, "-G", out], env=env, capture_output=True, text=True)
        if r.returncode or fail: return "failed " + (fail or r.stderr.strip())
        if not os.path.exists(os.path.join(etc, "layer_config_common.json.pre-hassmic")):
            shutil.copy(os.path.join(lr, "layer_config_common.json"), os.path.join(etc, "layer_config_common.json.pre-hassmic"))
        for n in os.listdir(lr):
            if n.startswith("hassmic_") and not os.path.exists(os.path.join(out, n)): os.remove(os.path.join(lr, n))
        for n in os.listdir(out):
            if n != "digest": shutil.copy(os.path.join(out, n), os.path.join(lr, n))
        shutil.copy(os.path.join(out, "digest"), os.path.join(etc, "hassmic-leds.digest"))
        return "ok"

    root = {"fail": None, "seen": 0, "stop": False, "pause": False}
    def root_loop():
        """main.sh ota_watch: state/leds-request -> leds() -> leds.result"""
        req = os.path.join(state, "leds-request")
        while not root["stop"]:
            if os.path.exists(req) and not root["pause"]:
                rid = open(req).read().strip(); os.remove(req); root["seen"] += 1
                with open(result + ".new", "w") as f: f.write(f"{rid} {install(root['fail'])}\n")
                os.rename(result + ".new", result)
            time.sleep(0.1)
    threading.Thread(target=root_loop, daemon=True).start()

    def start():
        open(leds, "w").close()
        return subprocess.Popen([HOST, "-P", "esphome", "-p", str(PORT), "-n", "Echo Ring", "-W", str(WEB), "-a", "0", "-z", "0", "-o", "0"],
                                env=env, stderr=open(log, "a"))

    def ledlog(): return [l.split() for l in open(leds).read().splitlines()]
    def anim(n): return open(os.path.join(lr, n + ".animation")).read()

    check(install() == "ok", "the set without designs installed, as at a first start")
    proc = start()
    try:
        wait_port(PORT, WEB)
        b = Browser(WEB)
        check(b.login_with_button(proc), "logged in with the button")
        st = lambda: json.loads(b.call("GET", "/api/ring")[2])
        s = st()
        avail = {x["id"]: x["available"] for x in s["slots"]}
        check(s["installed"] and s["leds"] == 12 and s["state"] == "applied" and s["designs"] == [] and s["max"] == 100,
              f"installed, 12 LEDs, nothing to apply, no designs: {s['state']}")
        check(avail["listen"] and avail["think"] and avail["volume"] and avail["call_in"] and not avail["call_live"] and not avail["timer_short"],
              f"slots available where stock's config lists their pattern: {[k for k, v in avail.items() if v]}")
        stk = b.call("GET", "/api/ring/stock/listen")
        check(stk[0] == 200 and stk[2].decode() == open(os.path.join(lr, "active-start.animation")).read(), "stock's listening ring for the designer")
        check(b.call("GET", "/api/ring/stock/volume")[2].decode().startswith("step 05\n2000:FFF"), "volume: stock's steps, each named")

        # refusals: what is not a design
        R = "FF0000"; B = "0000FF"; K = "000000"; W = "FFFFFF"
        bad = [("not a design's line", "hello\n"), ("leds before the frames", "name x\n100:" + ",".join([R] * 12) + "\n"),
               ("one RRGGBB per LED", design("x", [(100, [R] * 11)])), ("colours are RRGGBB", design("x", [(100, ["GG0000"] + [R] * 11)])),
               ("<1..60000 ms>", design("x", [(0, [R] * 12)])), ("needs a frame", "name x\nleds 12\n"),
               ("loop starts after", design("x", [(100, [R] * 12)], loop=1)), ("control character", "name a\x01b\nleds 12\n100:" + ",".join([R] * 12) + "\n"),
               ("bad leds", design("x", [(100, [R] * 25)], leds=25))]
        for why, text in bad:
            stt, _, body = b.call("POST", "/api/ring/design/0", text.encode())
            check(stt == 400 and why in json.loads(body)["error"], f"refused ({why}): {body.decode()}")
        check(b.call("POST", "/api/ring/design/101", design("x", [(100, [R] * 12)]).encode())[0] == 400, "refused: no design 101")

        # the library: a listening ring (opens, then a loop of two frames), thinking, a volume colour
        listen = design("Red listening", [(50, [R] + [K] * 11), (200, [R] * 6 + [K] * 6), (300, [K] * 6 + [R] * 6)], loop=1)
        think = design("Spinner", [(80, [W] + [K] * 11), (80, [K, W] + [K] * 10)])
        vol = design("Green volume", [(100, ["00FF00"] * 6 + ["808080"] * 6)], loop=-1)
        ids = [json.loads(b.call("POST", "/api/ring/design/0", t.encode())[2]).get("id") for t in (listen, think, vol)]
        check(ids == [1, 2, 3], f"three designs saved: {ids}")
        lib = b.call("GET", "/api/ring/designs")[2].decode()
        check(lib.startswith("design 1\nname Red listening\nleds 12\nloop 1\n50:FF0000,") and "design 3\nname Green volume\nleds 12\nloop once\n" in lib,
              "the library as text, each design as saved")
        stt, _, body = b.call("POST", "/api/ring/design/2", design("Spinner", [(80, [W] + [K] * 11), (80, [K, W] + [K] * 10), (80, [K, K, W] + [K] * 9)]).encode())
        check(stt == 200 and json.loads(body)["id"] == 2, "a design saved again under its id")
        stt, _, body = b.call("POST", "/api/ring/design/50", think.encode())
        check(stt == 400 and "no such design" in json.loads(body)["error"] and not os.path.exists(os.path.join(state, "ring", "50")),
              f"refused: saved under an id no design has (one deleted meanwhile is not made again) ({body.decode()})")
        check(b.call("POST", "/api/ring/assign", b"call_live 1\n")[0] == 400, "refused: a pattern stock's config does not list here")
        check(b.call("POST", "/api/ring/assign", b"think 9\n")[0] == 400, "refused: a design that does not exist")
        check(b.call("POST", "/api/ring/assign", b"listen 1\nend 1\nthink 2\nvolume 3\nassist_volume 3\n")[0] == 200, "assigned")
        s = st()
        check(s["state"] == "pending" and {x["id"]: x["design"] for x in s["slots"]}["think"] == 2 and not any(x["live"] for x in s["slots"]),
              f"pending: drafted, nothing on the ring yet ({s['state']})")
        check(not os.path.exists(os.path.join(state, "ring-applied")), "nothing applied before Apply")
        # the assignments' file cannot be written (a folder in its place: rename fails, root of a namespace or not):
        # refused, and what the page shows is what is still drafted, not what was asked for
        af = os.path.join(state, "ring", "assign"); kept = open(af).read(); os.remove(af); os.makedirs(os.path.join(af, "x"))
        stt, _, body = b.call("POST", "/api/ring/assign", b"think 0\n")
        s = st()
        check(stt == 400 and "cannot write" in json.loads(body)["error"] and {x["id"]: x["design"] for x in s["slots"]}["think"] == 2,
              f"assignments not written: refused, the draft as it was ({body.decode()})")
        shutil.rmtree(af)
        with open(af, "w") as f: f.write(kept)

        # Home Assistant's light on: it must be back after the ring's daemon restarts
        ha = APIClient("127.0.0.1", PORT, None); await ha.connect(login=True)
        ents, _ = await ha.list_entities_services()
        li = next(e for e in ents if isinstance(e, LightInfo))
        ha.light_command(li.key, state=True, rgb=(1.0, 0.0, 0.0), color_brightness=1.0, brightness=1.0)
        check(await until(lambda: ["-s", "hassmic_solid_00f0"] in ledlog()), "Home Assistant's light on")

        open(leds, "w").close()
        check(b.call("POST", "/api/ring/apply")[0] == 200, "apply")
        check(await until(lambda: st()["state"] == "applied", 8), f"root installed them: {st()['state']} {st()['error']}")
        s = st()
        live = {x["id"]: (x["live"], x["applied"]) for x in s["slots"]}
        check(live["listen"] == (True, 1) and live["think"] == (True, 2) and live["volume"] == (True, 3) and live["talk"] == (False, 0),
              f"live where assigned: {live}")
        check(await until(lambda: ["-s", "hassmic_solid_00f0"] in ledlog()), f"the light set again on the restarted ring ({ledlog()})")

        # what -G made of them
        dr, dk = "FF0000", "000000"
        check(anim("hassmic_s_listen_0") == "50:" + ",".join([dr] + [dk] * 11) + "\nloop\n200:" + ",".join([dr] * 6 + [dk] * 6) + "\n300:" + ",".join([dk] * 6 + [dr] * 6) + "\n",
              "listening: the design as drawn at turn 0, its loop from frame 2")
        check(anim("hassmic_s_listen_3").startswith("50:" + ",".join([dk] * 3 + [dr] + [dk] * 8) + "\nloop\n200:" + ",".join([dk] * 3 + [dr] * 6 + [dk] * 3)),
              "listening turned by 3: LED i shows what LED i - 3 did")
        check(anim("hassmic_s_hold_3") == "loop\n200:" + ",".join([dk] * 3 + [dr] * 6 + [dk] * 3) + "\n300:" + ",".join([dr] * 3 + [dk] * 6 + [dr] * 3) + "\n",
              "held at turn 3: only its loop")
        check(anim("hassmic_s_think").startswith("loop\n80:FFFFFF,000000"), "thinking: repeats as a whole")
        g = "%02X" % duty(128)
        check(anim("hassmic_s_volume_05") == "2000:" + ",".join(["00FF00"] * 3 + ["00" + "%02X" % (255 * 0x88 // 255) + "00"] + ["000000"] * 8 + []) + "\nloop\n1000:" + ",".join(["000000"] * 12) + "\n"
              and anim("hassmic_assist_05").startswith("2000:00FF00,00FF00,00FF00,"), f"volume: stock's step with the design's colours, as bright as stock's ({anim('hassmic_s_volume_05').splitlines()[0]})")
        check(g == "38", f"gamma: 128 as seen is 0x38 on the driver ({g})")
        conf = json.load(open(os.path.join(lr, "layer_config_common.json")))["Layers"]
        l4 = conf["16"]                 # stock's 4: stock's layers moved up by the 12 of the LEDs' own (ring.c relayer)
        check(l4[:36] == [f"hassmic_{k}_{i}" for k in ("listen", "hold", "end") for i in range(12)]
              and l4[36:60] == [f"hassmic_s_{k}_{i}" for k in ("listen", "hold") for i in range(12)] and l4[60:72] == [f"hassmic_s_end_{i}" for i in range(12)]
              and l4[72:] == ["active-end", "active-start", "anim_start_error_short", "hassmic_s_think", "active-thinking", "active-talking"],
              "layer 4: the turned rings first, the designs' turned ones after them, thinking's just before stock's")
        check(conf["17"] == ["hassmic_s_volume_05", "hassmic_assist_05", "volume_step-05"], f"layer 5: the volume steps before stock's ({conf['17']})")

        # what plays: the wake word, the ring turned by the beam; the talker moves; thinking; the end turned
        async def nothing(*a): pass
        started = asyncio.Event()
        async def handle_start(*a): started.set(); return 0
        ha.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        await asyncio.sleep(0.3); open(leds, "w").close()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(await until(lambda: ["-s", "hassmic_s_listen_3"] in ledlog()), f"wake word: the design turned by beam 3 ({ledlog()})")
        with open(beam, "w") as f: f.write("7\n")
        check(await until(lambda: ["-s", "hassmic_s_hold_7"] in ledlog() and ["-u", "hassmic_s_listen_3"] in ledlog()), "the talker moved: held at 7")
        open(leds, "w").close()
        ha.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        ha.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "nothing"})
        check(await until(lambda: ["-s", "hassmic_s_think"] in ledlog()), f"thinking: the design ({ledlog()})")
        ha.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        check(await until(lambda: ["-s", "hassmic_s_end_7"] in ledlog() and ["-u", "hassmic_s_think"] in ledlog()), f"over: the end turned by 7 ({ledlog()})")
        check(not [x for x in ledlog() if x[1] in ("active-thinking", "active-start")], "stock's own not played where designs are")

        # root fails: the applied file goes back to what is installed
        applied0 = open(os.path.join(state, "ring-applied")).read()
        b.call("POST", "/api/ring/assign", b"think 0\n")
        check(st()["state"] == "pending", "stock's thinking again: pending")
        root["fail"] = "the system partition has room for 10 kB"
        b.call("POST", "/api/ring/apply")
        check(await until(lambda: st()["state"] == "failed", 8) and "room for 10 kB" in st()["error"], f"root failed: shown ({st()['error']})")
        check(open(os.path.join(state, "ring-applied")).read() == applied0, "the applied designs back to what is installed")
        root["fail"] = None
        b.call("POST", "/api/ring/apply")
        check(await until(lambda: st()["state"] == "applied", 8) and "hassmic_s_think" not in json.load(open(os.path.join(lr, "layer_config_common.json")))["Layers"]["16"]
              and not os.path.exists(os.path.join(lr, "hassmic_s_think.animation")), "again: thinking is stock's, its file gone")

        # too big for the system partition: refused before root is asked
        big = design("Long", [(1000, [("%02X" % (i % 256)) * 3] * 12) for i in range(1000)])
        bid = json.loads(b.call("POST", "/api/ring/design/0", big.encode())[2])["id"]
        b.call("POST", "/api/ring/assign", f"listen {bid}\nend {bid}\n".encode())
        n0 = root["seen"]
        stt, _, body = b.call("POST", "/api/ring/apply")
        check(stt == 400 and "shorten" in json.loads(body)["error"] and root["seen"] == n0, f"1000 frames for listening and its end: too big ({body.decode()})")
        b.call("POST", "/api/ring/assign", b"listen 1\nend 1\n")
        check(b.call("POST", "/api/ring/delete/%d" % bid)[0] == 200, "deleted")

        # an assigned design that does not parse (written by an older build, or the flash): Apply refuses it by its
        # slot, never installs stock's animation there and says done
        d1 = os.path.join(state, "ring", "1"); kept = open(d1).read()
        with open(d1, "w") as f: f.write("name broken\nleds 12\n")
        n0 = root["seen"]
        stt, _, body = b.call("POST", "/api/ring/apply")
        check(stt == 400 and "listen: design 1 is broken" in json.loads(body)["error"] and root["seen"] == n0 and st()["state"] != "installing",
              f"a broken design assigned: Apply refused, root not asked ({body.decode()})")
        with open(d1, "w") as f: f.write(kept)
        check(st()["state"] == "applied", "the design back: nothing to apply")

        # theme packs: which library design each animation shows, kept as the Echo writes them, refused when not whole
        pack = "hassmic ring pack 2\nname Night\nauthor Tester\nabout Dim and warm\nshot glow\nhas 1\nhas 2\nhas 3\nuse listen 1\nuse think 2\nuse talk stock\nuse other_model 3\n"
        stt, _, body = b.call("POST", "/api/ring/pack/0", pack.encode())
        pid = json.loads(body).get("id")
        packs = b.call("GET", "/api/ring/packs")[2].decode()
        check(stt == 200 and pid == 1 and packs == "pack 1\n" + pack + "\n", f"a pack kept as sent, another model's animation kept ({body.decode()})")
        badp = [("as the Echo keeps it", "hassmic ring pack 1\nname x\nuse listen stock\n"), ("needs a name", "hassmic ring pack 2\nuse listen stock\n"),
                ("no design 77", "hassmic ring pack 2\nname x\nhas 77\nuse listen 77\n"), ("used twice", "hassmic ring pack 2\nname x\nuse listen stock\nuse listen 1\n"),
                ("bad design id", "hassmic ring pack 2\nname x\nuse listen 0\n"), ("not a pack's line", "hassmic ring pack 2\nname x\ndesign\n"),
                ("bad shot", "hassmic ring pack 2\nname x\nshot Hero\n"),
                ("not in the pack", "hassmic ring pack 2\nname x\nhas 1\nuse listen 2\n"), ("in it twice", "hassmic ring pack 2\nname x\nhas 1\nhas 1\n"),
                ("in the pack \"Night\" already", "hassmic ring pack 2\nname x\nhas 2\n")]
        for why, text in badp:
            stt, _, body = b.call("POST", "/api/ring/pack/0", text.encode())
            check(stt == 400 and why in json.loads(body)["error"], f"pack refused ({why}): {body.decode()}")
        stt, _, body = b.call("POST", "/api/ring/pack/1", pack.replace("name Night", "name Night 2").encode())
        check(stt == 200 and json.loads(body)["id"] == 1 and "name Night 2" in b.call("GET", "/api/ring/packs")[2].decode(), "a pack saved again under its id")
        empty_pack = b"hassmic ring pack 2\nname Empty\nuse listen stock\n"
        got = [b.call("POST", "/api/ring/pack/0", empty_pack)[0] for _ in range(49)]
        stt, _, body = b.call("POST", "/api/ring/pack/0", empty_pack)
        check(got.count(200) == 49 and stt == 400 and "50 packs" in json.loads(body)["error"], f"50 packs at most ({body.decode()})")
        check(all(b.call("POST", f"/api/ring/pack-delete/{k}")[0] == 200 for k in range(2, 51)) and b.call("GET", "/api/ring/packs")[2].decode().count("\npack ") == 0,
              "packs deleted, one left")
        # has and use lines kept as the Echo spells them: a deleted design is found in them by its id
        oid = json.loads(b.call("POST", "/api/ring/design/0", design("odd", [(100, [R] * 12)]).encode())[2])["id"]
        stt, _, body = b.call("POST", "/api/ring/pack/0", f"hassmic ring pack 2\nname Odd\nhas  0{oid}\nuse think  +{oid}\nuse talk   stock\n".encode())
        opid = json.loads(body).get("id")
        packs = b.call("GET", "/api/ring/packs")[2].decode()
        check(stt == 200 and f"pack {opid}\nhassmic ring pack 2\nname Odd\nhas {oid}\nuse think {oid}\nuse talk stock\n" in packs,
              f"a pack's ids written as the Echo spells them ({packs!r})")
        b.call("POST", f"/api/ring/delete/{oid}")
        packs = b.call("GET", "/api/ring/packs")[2].decode()
        check(f"pack {opid}\nhassmic ring pack 2\nname Odd\nuse talk stock\n" in packs, f"its design deleted: the pack names it no more ({packs!r})")
        b.call("POST", f"/api/ring/pack-delete/{opid}")
        bigp = ("hassmic ring pack 2\nname x\nabout " + "y" * 20000 + "\n").encode()
        check(req(WEB, "POST", "/api/ring/pack/0", bigp, b.headers("POST", "/api/ring/pack/0", bigp))[0] == 413, "a pack larger than a request: refused unread")

        # deleting an assigned design: that slot is stock's again
        b.call("POST", "/api/ring/delete/3")
        s = st()
        check({x["id"]: x["design"] for x in s["slots"]}["volume"] == 0 and s["state"] == "pending" and [d["id"] for d in s["designs"]] == [1, 2],
              "an assigned design deleted: stock's there again, pending")
        packs = b.call("GET", "/api/ring/packs")[2].decode()
        check("use other_model 3" not in packs and "has 3" not in packs and "has 1\nhas 2\nuse listen 1\nuse think 2\nuse talk stock\n" in packs,
              f"a deleted design leaves the packs naming it ({packs!r})")

        # the library's cap; a new design does not take an id the ring still shows (3: the volume's, installed)
        small = design("s", [(100, [R] * 12)])
        first = b.call("POST", "/api/ring/design/0", small.encode())
        check(first[0] == 200 and json.loads(first[2])["id"] == 4, f"a new design: not id 3, which the installed volume names ({first[2].decode()})")
        got = [b.call("POST", "/api/ring/design/0", small.encode())[0] for _ in range(98)]
        stt, _, body = b.call("POST", "/api/ring/design/0", small.encode())
        check(got.count(200) == 97 and stt == 400 and "100 designs" in json.loads(body)["error"], f"100 designs at most, 3 last ({got.count(200)} more, then {body.decode()})")
        # a design's big body only from an approved browser: anyone else is refused before it is read
        big_body = ("name x\nleds 12\nloop 0\n" + ("100:" + ",".join([R] * 12) + "\n") * 900).encode()
        stranger = Browser(WEB)
        check(req(WEB, "POST", "/api/ring/design/0", big_body, stranger.headers("POST", "/api/ring/design/0", big_body))[0] == 413,
              "a big body from a browser not approved: refused unread")
        check(b.call("POST", "/api/ring/design/0", big_body)[0] == 400, "the same from the approved one: read (the library is full)")
        bogus = dict(b.headers("POST", "/api/ring/design/0", big_body), **{"X-HM-Mac": "00" * 16})
        check(req(WEB, "POST", "/api/ring/design/0", big_body, bogus)[0] == 401, "an approved browser's key with a wrong MAC: read, then refused")
        # the other big bodies (an artifact's piece, a microWakeWord model) as well: refused unread from a stranger
        mid = b"x" * 20000
        for path in ("/api/artifact/chunk/wake:x/f/0", "/api/mww/add/x"):
            check(req(WEB, "POST", path, mid, stranger.headers("POST", path, mid))[0] == 413, f"{path}: a big body from a browser not approved: refused unread")
            check(req(WEB, "POST", path, mid, dict(b.headers("POST", path, mid), **{"X-HM-Mac": "00" * 16}))[0] == 401, f"{path}: from an approved key: read, the MAC checked")
        await ha.disconnect()
    finally:
        proc.terminate(); proc.wait()

    # a start with designs installed: they play from the start
    proc = start()
    try:
        await until(lambda: open(log).read().count("our animations are installed") == 2, 15)
        line = open(log).read().split("our animations are installed")[-1].split("\n")[0]
        check("designs in 4 places)" in line, f"a new start: the designs installed are live ({line.strip()})")
    finally:
        proc.terminate(); proc.wait()

    applied_f, installed_f = os.path.join(state, "ring-applied"), os.path.join(state, "ring-installed")
    rd = lambda f: open(f).read() if os.path.exists(f) else None
    async def again():
        p = start(); wait_port(PORT, WEB); return p
    check(rd(installed_f) == rd(applied_f) and "slot volume 3" in rd(installed_f), "what root installed is kept as installed")

    # A request still out when hassmic restarts: root answers it after the start, and that start takes the answer
    proc = await again()
    try:
        root["pause"] = True
        b.call("POST", "/api/ring/assign", b"listen 2\n")
        b.call("POST", "/api/ring/apply")
        check(st()["state"] == "installing", "applied while root is busy: installing")
        proc.terminate(); proc.wait(); proc = await again()
        check(st()["state"] == "installing", "restarted with the request still out: still installing")
        root["pause"] = False
        check(await until(lambda: st()["state"] == "applied", 8), f"root answered after the restart: applied ({st()['state']})")
        check({x["id"]: x["applied"] for x in st()["slots"]}["listen"] == 2 and "slot listen 2" in rd(installed_f), "listening's new design installed")

        # Asked for, and then nothing: root took the request and never answered, hassmic restarted.  The draft goes
        # back, so the next boot does not try it again and the ring stays ours
        root["pause"] = True
        b.call("POST", "/api/ring/assign", b"listen 1\n")
        b.call("POST", "/api/ring/apply")
        proc.terminate(); proc.wait()
        os.remove(os.path.join(state, "leds-request"))
        proc = await again()
        s = st()
        check(s["state"] == "failed" and "were not installed" in s["error"] and rd(applied_f) == rd(installed_f) and "slot listen 2" in rd(applied_f)
              and s["installed"], f"a draft left without a request: back to what is installed, said so ({s['state']}: {s['error']})")
        root["pause"] = False
    finally:
        proc.terminate(); proc.wait()

    # Root installed what was asked for while hassmic was not running (a boot): the next start takes it as installed
    draft = "hassmic ring applied 1\nslot think 2\n" + rd(installed_f).split("slot listen 2\n")[1].split("end\n")[0] + "end\n"
    with open(applied_f, "w") as f: f.write(draft)
    check(install() == "ok", "root installs the asked-for designs at a boot")
    n0 = open(log).read().count("our animations are installed")
    proc = start()
    try:
        await until(lambda: open(log).read().count("our animations are installed") > n0, 15)
        line = open(log).read().split("our animations are installed")[-1].split("\n")[0]
        check("designs in 1 place)" in line and rd(installed_f) == draft, f"a start after root installed them: taken as installed ({line.strip()})")
    finally:
        proc.terminate(); proc.wait()
    root["stop"] = True

    # root reads state/ring-applied: never through a link
    ap = os.path.join(state, "ring-applied"); real = os.path.join(tmp, "elsewhere")
    shutil.move(ap, real); os.symlink(real, ap)
    out = os.path.join(tmp, "out-link"); os.makedirs(out)
    r = subprocess.run([HOST, "-G", out], env=env, capture_output=True, text=True)
    check(r.returncode == 0 and not [n for n in os.listdir(out) if n.startswith("hassmic_s_")] and "designs in 0 places" in r.stderr,
          f"-G with state/ring-applied a link: not read, no designs ({r.stderr.strip()})")
    shutil.rmtree(tmp)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

asyncio.run(main())
