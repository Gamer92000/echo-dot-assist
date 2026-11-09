#!/usr/bin/env python3
"""Sounds of one's own in place of Alexa's (sounds.c, the settings page's Sounds card) against build/hassmic-host:
the page is told which sounds there are, which of Alexa's this "firmware" has (stand-ins in HASSMIC_EARCONS) and how
long each may be; Alexa's handed out as a WAV for the page's loudness match; uploads only as the page makes them and
within each sound's length; "Play"; the wake word playing one's own where Alexa's played; a new upload played from the
next time on; deleting it (Alexa's again); kept over a restart; refused unsigned; a switch per sound, Home Assistant's
"Wake sound" for the wake sound alone, what an Echo with it off keeps after the update, and an export of then imported
or given as a preset.  The page's loudness check is
tests/unit/web_loudness_test.py's."""
import asyncio, io, json, os, re, signal, struct, subprocess, sys, tempfile, time, urllib.request, wave
from aioesphomeapi import APIClient, SwitchInfo

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import Browser, wait_port
PORT, WEB = 17151, 17152
WAKE_PEAK, RING_PEAK, OWN_PEAK = 7000, 9000, 11111     # the stand-ins for Alexa's wake sound and ringtone, one's own

def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def until(cond, timeout=5.0):
    end = asyncio.get_running_loop().time() + timeout
    while not cond() and asyncio.get_running_loop().time() < end:
        await asyncio.sleep(0.02)
    return cond()


def wav(rate, channels, seconds, level):
    b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(channels); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(struct.pack("<h", level) * int(rate * seconds) * channels)
    return b.getvalue()



def stock_curve(state):
    """the Echo's own volume curve, not even steps (on by default): this test's sounds play at full scale, unscaled"""
    p = os.path.join(state, "config")
    if "even_volume=" not in (open(p).read() if os.path.exists(p) else ""):
        with open(p, "a") as f: f.write("even_volume=off\n")

class Echo:
    def __init__(self, tmp, **extra):
        self.tmp, self.state = tmp, os.path.join(tmp, "state")
        os.makedirs(self.state, exist_ok=True)
        if not os.path.exists(os.path.join(self.state, "adopted")): open(os.path.join(self.state, "adopted"), "w").write("ha\n")
        stock_curve(self.state)                                 # levels here are full scale: fake_ha_volumes.py has even steps
        earcons = os.path.join(tmp, "earcons") + "/"     # Alexa's wake sound (0.65 s, as donut's) and incoming ringtone only
        os.makedirs(earcons, exist_ok=True)
        open(earcons + "ui_wakesound.wav", "wb").write(wav(48000, 1, 0.65, WAKE_PEAK))
        open(earcons + "comms_call_incoming_ringtone.wav", "wb").write(wav(48000, 1, 1.0, RING_PEAK))
        self.log = open(os.path.join(tmp, "log"), "a+")
        addrs = os.path.join(tmp, "addrs"); open(addrs, "w").write("127.0.0.1\n")
        env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(tmp, "play.raw"),
                   HASSMIC_MDNS_FILE=os.path.join(tmp, "mdns"), HASSMIC_FAKE_ADDRS=addrs, HASSMIC_ARB_ADDR="127.255.255.255",
                   HASSMIC_MODELS=os.path.join(tmp, "models"), HASSMIC_LED_ROOT=os.path.join(tmp, "etc"), HASSMIC_EARCONS=earcons, **extra)
        keys = os.path.join(tmp, "keypad")
        if not os.path.exists(keys): os.mkfifo(keys)
        self.keypad = os.open(keys, os.O_RDWR)                # opened first: hassmic's open() would wait for a writer
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-W", str(WEB), "-b", keys],
                                     env=env, stderr=self.log)

    def text(self): self.log.seek(0); return self.log.read()
    def count(self, pattern): return len(re.findall(pattern, self.text()))
    def stop(self): self.proc.terminate(); self.proc.wait(5); self.log.close(); os.close(self.keypad)
    def key(self, code):
        os.write(self.keypad, struct.pack("llHHi", 0, 0, 1, code, 1)); time.sleep(0.1)
        os.write(self.keypad, struct.pack("llHHi", 0, 0, 1, code, 0)); time.sleep(0.3)

    def page(self):
        wait_port(PORT, WEB)
        b = Browser(WEB)
        if not b.login_with_button(self.proc): raise RuntimeError("page login")
        return b

    async def connect(self):
        for _ in range(100):
            try: c = APIClient("127.0.0.1", PORT, None); await c.connect(login=True); break
            except Exception: await asyncio.sleep(0.1)
        else: raise RuntimeError("no connection")
        self.switches = {x.object_id: x for x in (await c.list_entities_services())[0] if isinstance(x, SwitchInfo)}
        self.started = asyncio.Event()
        async def start(*a): self.started.set(); return 0
        async def nothing(*a): return 0
        c.subscribe_voice_assistant(handle_start=start, handle_stop=nothing, handle_audio=nothing, handle_announcement_finished=nothing)
        await asyncio.sleep(0.3)
        return c


def played(samples, peak): return rf"earcon: {samples} samples @ 48000 Hz, peak {peak}\n"


async def main():
    tmp = tempfile.mkdtemp()
    e = Echo(tmp)
    try:
        page = e.page(); c = await e.connect()
        snd = lambda: {x["id"]: x for x in page.state()["sounds"]}
        s = snd()
        check(list(s) == ["wake", "touch", "mics_off", "mics_on", "volume", "bluetooth_on", "bluetooth_off", "identify", "drop_in",
                          "call_on", "call_off", "ring_in", "ring_out"], f"the page is told every sound one's own can stand in for: {list(s)}")
        check(s["wake"] == {"id": "wake", "own_ms": 0, "stock": True, "max_s": 2} and s["ring_in"]["stock"] and s["ring_in"]["max_s"] == 30
              and not s["mics_off"]["stock"] and s["volume"]["max_s"] == 2,
              f"whose plays, whether Alexa's is on this firmware, how long one may be: {s['wake']}, {s['ring_in']}, {s['mics_off']}")

        # Alexa's as a WAV, for the page to measure; none where the firmware has none
        st, _, data = page.call("GET", "/api/sound/wake/stock")
        with wave.open(io.BytesIO(data)) as w:
            ok = (w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()) == (48000, 1, 2, 31200)
            pk = max(abs(x) for x in struct.unpack(f"<{w.getnframes()}h", w.readframes(w.getnframes())))
        check(st == 200 and ok and pk == WAKE_PEAK, f"Alexa's wake sound handed out as it plays: {st}, 31200 frames, peak {pk}")
        st, _, _ = page.call("GET", "/api/sound/mics_off/stock")
        check(st == 404, f"none of Alexa's for it on this firmware: {st}")

        # uploads: only what the page makes, within each sound's length
        for path, body, why, what in (("wake", wav(48000, 1, 2.5, 1000), "2 s at most", "a 2.5 s wake sound"),
                                      ("call_on", wav(48000, 1, 2.5, 1000), "2 s at most", "a 2.5 s chime that ducks a call's start"),
                                      ("bluetooth_on", wav(48000, 1, 5.5, 1000), "5 s at most", "a 5.5 s chime"),
                                      ("wake", wav(44100, 1, 0.5, 1000), "48000 Hz", "44.1 kHz"),
                                      ("ring_in", wav(48000, 2, 0.5, 1000), "2 channels", "stereo"),
                                      ("wake", wav(48000, 1, 0.05, 1000), "too short", "50 ms"),
                                      ("nope", wav(48000, 1, 0.5, 1000), "no such sound", "an unknown sound")):
            st, _, data = page.call("POST", f"/api/sound/{path}", body)
            check(st == 400 and why in json.loads(data).get("error", ""), f"upload of {what} refused: {st} {data[:120]!r}")
        check(not os.path.exists(os.path.join(e.state, "sounds", "wake.wav")) and snd()["wake"]["own_ms"] == 0, "nothing kept of them")
        st, _, data = page.call("POST", "/api/sound/ring_in", wav(48000, 1, 25, 1000))
        check(st == 200 and snd()["ring_in"]["own_ms"] == 25000, f"a ringtone may be longer: 25 s taken ({st})")

        # one's own wake sound: Play, then the wake word plays it where Alexa's played
        st, _, _ = page.call("POST", "/api/sound/wake", wav(48000, 1, 0.5, OWN_PEAK))
        check(st == 200 and snd()["wake"]["own_ms"] == 500 and os.path.exists(os.path.join(e.state, "sounds", "wake.wav")),
              f"a 0.5 s wake sound of one's own taken: {snd()['wake']}")
        n = e.count(played(24000, OWN_PEAK))
        st, _, _ = page.call("POST", "/api/sound/wake/play")
        check(st == 200 and await until(lambda: e.count(played(24000, OWN_PEAK)) == n + 1, 2), "Play: one's own, once")
        n, stock = e.count(played(24000, OWN_PEAK)), e.count(played(31200, WAKE_PEAK))
        e.proc.send_signal(signal.SIGUSR1)                                   # the wake word
        await asyncio.wait_for(e.started.wait(), 5)
        check(await until(lambda: e.count(played(24000, OWN_PEAK)) == n + 1, 2) and e.count(played(31200, WAKE_PEAK)) == stock,
              "the wake word plays one's own, not Alexa's")

        # replaced: the new one from the next time on; Play of one with none of Alexa's: one's own still plays
        page.call("POST", "/api/sound/wake", wav(48000, 1, 0.25, OWN_PEAK - 1))
        page.call("POST", "/api/sound/wake/play")
        check(await until(lambda: e.count(played(12000, OWN_PEAK - 1)) == 1, 2), "a new upload plays from the next time on")
        page.call("POST", "/api/sound/mics_off", wav(48000, 1, 0.3, OWN_PEAK))
        page.call("POST", "/api/sound/mics_off/play")
        check(await until(lambda: e.count(played(14400, OWN_PEAK)) == 1, 2) and snd()["mics_off"]["own_ms"] == 300,
              "one's own plays where the firmware has none of Alexa's")

        # a switch per sound.  Home Assistant's "Wake sound" is the wake sound's alone, and off keeps the one chosen
        setting = lambda name: next(x["value"] for x in page.state()["settings"] if x["name"] == name)
        page.call("POST", "/api/sound/volume", wav(48000, 1, 0.1, 500))     # none of Alexa's for it here
        e.key(138)                                                           # KEY_HELP: the action button ends the run above
        n = e.count(played(12000, OWN_PEAK - 1)); stock = e.count(played(31200, WAKE_PEAK))
        c.switch_command(e.switches["wake_sound"].key, False)
        check(await until(lambda: not setting("wake_sound")), "Home Assistant's switch: the wake sound off")
        check(setting("sound_volume") and setting("sound_touch"), "and only the wake sound")
        e.started.clear(); e.proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(e.started.wait(), 5); await asyncio.sleep(0.5)
        check(e.count(played(12000, OWN_PEAK - 1)) == n and e.count(played(31200, WAKE_PEAK)) == stock, "the wake word: heard, silent")
        e.key(138)
        n0 = len(re.findall(r"sound: volume", e.text()))
        e.key(115)                                                           # KEY_VOLUMEUP
        check(await until(lambda: len(re.findall(r"sound: volume", e.text())) == n0 + 1, 2), "the volume keys still sound")
        page.call("POST", "/api/sound/wake/play")
        check(await until(lambda: e.count(played(12000, OWN_PEAK - 1)) == n + 1, 2), "Play still plays it, asked for")
        c.switch_command(e.switches["wake_sound"].key, True); await until(lambda: setting("wake_sound"))
        e.started.clear(); e.proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(e.started.wait(), 5)
        check(await until(lambda: e.count(played(12000, OWN_PEAK - 1)) == n + 2, 2) and e.count(played(31200, WAKE_PEAK)) == stock,
              "on again: the one chosen, one's own, not Alexa's")
        page.set(sound_volume="off")
        e.key(115); await asyncio.sleep(0.4)
        check(len(re.findall(r"sound: volume", e.text())) == n0 + 1 and setting("wake_sound"), "the volume keys' own switch, the wake sound's left alone")
        check("sound_volume=off" in open(os.path.join(e.state, "config")).read(), "saved")
        page.call("POST", "/api/set", b"".join(f"{k}=off\n".encode() for k in ("wake_sound", "sound_touch", "sound_mics_off", "sound_mics_on", "sound_volume", "sound_bluetooth_on", "sound_bluetooth_off")))
        check(not any(setting(k) for k in ("wake_sound", "sound_touch", "sound_bluetooth_off")), "All off is those settings in one request")
        page.call("POST", "/api/set", b"".join(f"{k}=on\n".encode() for k in ("wake_sound", "sound_touch", "sound_mics_off", "sound_mics_on", "sound_volume", "sound_bluetooth_on", "sound_bluetooth_off")))

        # a long chime gives way, and is not kept in memory but played again
        e.key(138)                                                           # ends the wake word's run above
        page.call("POST", "/api/sound/identify", wav(48000, 1, 4, 3333))
        page.call("POST", "/api/sound/identify/play"); await asyncio.sleep(0.3)
        cuts = e.count(r"earcon: cut at")
        e.started.clear(); e.proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(e.started.wait(), 5)
        check(await until(lambda: e.count(r"earcon: cut at") == cuts + 1, 1), "a 4 s chime: cut by the wake word, not played out")
        e.key(138)
        peak = r"earcon: 192000 samples @ 48000 Hz, until stopped, peak 3333\n"; n = e.count(peak)
        page.call("POST", "/api/sound/identify/play")
        check(await until(lambda: e.count(peak) == n + 1, 2), "played again from its file")
        page.call("POST", "/api/sound/identify", b""); await asyncio.sleep(0.2)

        # packs: what is in use saved, one made by hand, each in use by turns; a bad upload refused
        packs = lambda: json.loads(page.call("GET", "/api/sound-pack")[2])
        st, _, data = page.call("POST", "/api/sound-pack/save", b"Mine"); mine = json.loads(data)["id"]
        p = next(x for x in packs()["packs"] if x["id"] == mine)
        check(st == 200 and p["name"] == "Mine" and {"wake", "ring_in", "mics_off"} <= set(p["sounds"]) and p["bytes"] > 2_000_000, f"what is in use saved as a pack: {p}")
        beeps = json.loads(page.call("POST", "/api/sound-pack/new", 'Bips "\\ ü'.encode())[2])["id"]
        check(next(x for x in packs()["packs"] if x["id"] == beeps)["name"] == 'Bips "\\ ü', "a name as it was typed")
        w = wav(48000, 1, 0.5, OWN_PEAK - 2)
        st, _, _ = page.call("POST", f"/api/sound-pack/{beeps}/file/wake", w)
        check(st == 200 and page.call("GET", f"/api/sound-pack/{beeps}/file/wake")[2] == w, "a sound into a pack, back unchanged")
        for path, body, what in ((f"/api/sound-pack/{beeps}/file/wake", wav(48000, 1, 2.5, 1000), "2.5 s of wake sound"),
                                 (f"/api/sound-pack/{beeps}/file/nope", w, "an unknown sound"),
                                 (f"/api/sound-pack/{beeps}/file/..%2f..%2fconfig", w, "a path"),
                                 ("/api/sound-pack/99/file/wake", w, "an unknown pack"), ("/api/sound-pack/0/apply", b"", "pack 0"),
                                 ("/api/sound-pack/new", b"a\nb", "a name with a newline"), ("/api/sound-pack/new", b"", "no name")):
            check(page.call("POST", path, body)[0] == 400, f"refused: {what}")
        check(snd()["wake"]["own_ms"] == 250, "a pack changes nothing before it is used")
        st, _, _ = page.call("POST", f"/api/sound-pack/{beeps}/apply")
        check(st == 200 and snd()["wake"]["own_ms"] == 500 and snd()["ring_in"]["own_ms"] == 0, f"used: its sounds, Alexa's for the rest: {snd()['wake']}")
        n = e.count(played(24000, OWN_PEAK - 2)); page.call("POST", "/api/sound/wake/play")
        check(await until(lambda: e.count(played(24000, OWN_PEAK - 2)) == n + 1, 2), "and it plays")
        st, _, _ = page.call("POST", f"/api/sound-pack/{mine}/apply")
        check(st == 200 and snd()["wake"]["own_ms"] == 250 and snd()["ring_in"]["own_ms"] == 25000, "the saved one back")
        st, _, _ = page.call("POST", f"/api/sound-pack/{beeps}/delete")
        check(st == 200 and [x["id"] for x in packs()["packs"]] == [mine] and not os.path.exists(os.path.join(e.state, "sound-packs", str(beeps))), "deleted, its folder gone")
        ids = [json.loads(page.call("POST", "/api/sound-pack/new", f"p{i}".encode())[2]).get("id") for i in range(15)]
        check(len(packs()["packs"]) == 16 and page.call("POST", "/api/sound-pack/new", b"one more")[0] == 400, "16 packs at most")
        for i in ids: page.call("POST", f"/api/sound-pack/{i}/delete")

        # kept over a restart
        await c.disconnect(); e.stop()
        e = Echo(tmp); page = e.page()
        check(snd()["wake"]["own_ms"] == 250 and snd()["ring_in"]["own_ms"] == 25000, f"kept over a restart: {snd()['wake']}")

        # deleted: Alexa's again
        st, _, _ = page.call("POST", "/api/sound/wake", b"")
        check(st == 200 and snd()["wake"]["own_ms"] == 0 and not os.path.exists(os.path.join(e.state, "sounds", "wake.wav")),
              f"deleting it: {st} {snd()['wake']}")
        n = e.count(played(31200, WAKE_PEAK))
        page.call("POST", "/api/sound/wake/play")
        check(await until(lambda: e.count(played(31200, WAKE_PEAK)) == n + 1, 2), "then Alexa's plays again")

        check([x["name"] for x in json.loads(page.call("GET", "/api/sound-pack")[2])["packs"]] == ["Mine"], "the pack kept over a restart")

        # signed only
        for method, path in (("POST", "/api/sound/wake"), ("POST", "/api/sound/wake/play"), ("GET", "/api/sound/wake/stock"),
                             ("GET", "/api/sound-pack"), ("POST", "/api/sound-pack/new"), ("POST", "/api/sound-pack/1/apply")):
            try: st = urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{WEB}{path}", method=method, data=b"x" if method == "POST" else None), timeout=5).status
            except urllib.error.HTTPError as x: st = x.code
            check(st == 401, f"{method} {path} unsigned: refused ({st})")

        # the library's cap, and the room the partition keeps
        e.stop(); full = tempfile.mkdtemp(); os.makedirs(os.path.join(full, "state"))
        e = Echo(full, HASSMIC_SOUNDPACK_MAX="300000"); page = e.page()
        i = json.loads(page.call("POST", "/api/sound-pack/new", b"cap")[2])["id"]
        check(page.call("POST", f"/api/sound-pack/{i}/file/wake", wav(48000, 1, 2, 1000))[0] == 200
              and page.call("POST", f"/api/sound-pack/{i}/file/wake", wav(48000, 1, 1.5, 1000))[0] == 200, "within the cap, a sound replaced counts once")
        st, _, data = page.call("POST", f"/api/sound-pack/{i}/file/touch", wav(48000, 1, 2, 1000))
        check(st == 400 and b"library is full" in data, f"past the cap: {st} {data[:80]!r}")
        page.call("POST", "/api/sound/wake", wav(48000, 1, 2, 1000)); page.call("POST", "/api/sound/touch", wav(48000, 1, 2, 1000))
        page.call("POST", f"/api/sound-pack/{i}/delete")
        st, _, data = page.call("POST", "/api/sound-pack/save", b"over")
        check(st == 400 and b"library is full" in data and not os.path.exists(os.path.join(full, "state", "sound-packs", "1")), f"what is in use past the cap: refused, nothing left ({st})")
        i = json.loads(page.call("POST", "/api/sound-pack/new", b"cap")[2])["id"]
        e.stop(); e = Echo(full, HASSMIC_SOUNDPACK_FREE_MIN=str(1 << 62)); page = e.page()
        st, _, data = page.call("POST", f"/api/sound-pack/{i}/file/touch", wav(48000, 1, 0.5, 1000))
        check(st == 400 and b"not enough room" in data, f"the partition nearly full: {st} {data[:80]!r}")
        st, _, data = page.call("POST", "/api/sound-pack/new", b"more")
        check(st == 400 and b"not enough room" in data, "no new pack either")

        # an Echo with "Wake sound" off from before the switches: as quiet as it was, Bluetooth's chimes (never covered) on
        e.stop(); old = tempfile.mkdtemp(); os.makedirs(os.path.join(old, "state"))
        open(os.path.join(old, "state", "config"), "w").write("wake_sound=off\n")
        e = Echo(old); page = e.page()
        setting = lambda name: next(x["value"] for x in page.state()["settings"] if x["name"] == name)
        check(not any(setting(k) for k in ("wake_sound", "sound_touch", "sound_mics_off", "sound_mics_on", "sound_volume")), "wake_sound=off: the sounds it covered off")
        check(setting("sound_bluetooth_on") and setting("sound_bluetooth_off"), "Bluetooth's stay on: it never covered them")
        check("sound_touch=off" in open(os.path.join(old, "state", "config")).read(), "written, so once only")
        page.set(sound_touch="on"); e.stop()
        e = Echo(old); page = e.page()
        check(setting("sound_touch") and not setting("wake_sound"), "one turned on after that stays on")

        # an export of then (no lines for the switches) imported on the page: the same.  Not a newer export, which lists
        # them, nor a wake_sound line sent alone (the page's switch)
        names = ("wake_sound", "sound_touch", "sound_mics_off", "sound_mics_on", "sound_volume", "sound_bluetooth_on", "sound_bluetooth_off")
        all_on = lambda: page.call("POST", "/api/set", b"".join(f"{k}=on\n".encode() for k in names))
        all_on()
        new = page.call("GET", "/api/export")[2].decode().replace("\nwake_sound=on\n", "\nwake_sound=off\n")
        then = "".join(l for l in new.splitlines(True) if l.split("=")[0] not in names[1:])
        lines = sum(1 for l in then.splitlines() if "=" in l and not l.startswith("#"))
        r = json.loads(page.call("POST", "/api/set", then.encode())[2])
        check(not any(setting(k) for k in names[:5]) and r["applied"] == lines + 4 and not r["errors"],
              f"an export from before the switches imported: the four off with the wake sound ({r['applied']} for {lines} lines)")
        check(setting("sound_bluetooth_on") and setting("sound_bluetooth_off") and "sound_volume=off" in open(os.path.join(old, "state", "config")).read(),
              "Bluetooth's on; saved")
        r = json.loads(page.call("POST", "/api/set", then.encode())[2])
        check(r["applied"] == lines and not r["errors"], f"the same again, the four already off: none counted twice ({r['applied']} for {lines} lines)")
        all_on(); page.call("POST", "/api/set", new.encode())
        check(not setting("wake_sound") and all(setting(k) for k in names[1:]), "a newer export, its switches on: they stay on")
        all_on(); page.set(wake_sound="off")
        check(not setting("wake_sound") and all(setting(k) for k in names[1:]), "wake_sound=off alone: the wake sound alone")
        # the same file given to scripts/setup.sh --preset (state/preset, applied at start)
        all_on(); open(os.path.join(old, "state", "preset"), "w").write(then); e.stop()
        e = Echo(old); page = e.page()
        check(os.path.exists(os.path.join(old, "state", "preset.applied")) and not any(setting(k) for k in names[:5])
              and setting("sound_bluetooth_on"), "as a preset: the four off as well")
    finally:
        try: e.stop()
        except Exception: pass
        if check.failed: print("--- hassmic's log ---\n" + open(os.path.join(tmp, "log")).read()[-6000:])
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
