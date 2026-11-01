#!/usr/bin/env python3
"""Sounds of one's own in place of Alexa's (sounds.c, the settings page's Sounds card) against build/hassmic-host:
the page is told which sounds there are, which of Alexa's this "firmware" has (stand-ins in HASSMIC_EARCONS) and how
long each may be; Alexa's handed out as a WAV for the page's loudness match; uploads only as the page makes them and
within each sound's length; "Play"; the wake word playing one's own where Alexa's played; a new upload played from the
next time on; deleting it (Alexa's again); kept over a restart; refused unsigned.  The page's loudness check is
tests/unit/web_loudness_test.py's."""
import asyncio, io, json, os, re, signal, struct, subprocess, sys, tempfile, urllib.request, wave
from aioesphomeapi import APIClient

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


class Echo:
    def __init__(self, tmp):
        self.tmp, self.state = tmp, os.path.join(tmp, "state")
        os.makedirs(self.state, exist_ok=True)
        if not os.path.exists(os.path.join(self.state, "adopted")): open(os.path.join(self.state, "adopted"), "w").write("ha\n")
        earcons = os.path.join(tmp, "earcons") + "/"     # Alexa's wake sound (0.65 s, as donut's) and incoming ringtone only
        os.makedirs(earcons, exist_ok=True)
        open(earcons + "ui_wakesound.wav", "wb").write(wav(48000, 1, 0.65, WAKE_PEAK))
        open(earcons + "comms_call_incoming_ringtone.wav", "wb").write(wav(48000, 1, 1.0, RING_PEAK))
        self.log = open(os.path.join(tmp, "log"), "a+")
        addrs = os.path.join(tmp, "addrs"); open(addrs, "w").write("127.0.0.1\n")
        env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(tmp, "play.raw"),
                   HASSMIC_MDNS_FILE=os.path.join(tmp, "mdns"), HASSMIC_FAKE_ADDRS=addrs, HASSMIC_ARB_ADDR="127.255.255.255",
                   HASSMIC_MODELS=os.path.join(tmp, "models"), HASSMIC_LED_ROOT=os.path.join(tmp, "etc"), HASSMIC_EARCONS=earcons)
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-W", str(WEB)],
                                     env=env, stderr=self.log)

    def text(self): self.log.seek(0); return self.log.read()
    def count(self, pattern): return len(re.findall(pattern, self.text()))
    def stop(self): self.proc.terminate(); self.proc.wait(5); self.log.close()

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
        await c.list_entities_services()
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

        # signed only
        for method, path in (("POST", "/api/sound/wake"), ("POST", "/api/sound/wake/play"), ("GET", "/api/sound/wake/stock")):
            try: st = urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{WEB}{path}", method=method, data=b"x" if method == "POST" else None), timeout=5).status
            except urllib.error.HTTPError as x: st = x.code
            check(st == 401, f"{method} {path} unsigned: refused ({st})")
    finally:
        try: e.stop()
        except Exception: pass
        if check.failed: print("--- hassmic's log ---\n" + open(os.path.join(tmp, "log")).read()[-6000:])
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
