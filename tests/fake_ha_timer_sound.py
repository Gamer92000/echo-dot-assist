#!/usr/bin/env python3
"""The timer's sound (issue #63) against build/hassmic-host, as Home Assistant and the settings page: stock's timer
tone (a stand-in in HASSMIC_EARCONS) by default, looped until stopped and cut at once by a stop or a pipeline; the
built-in beep at Alexa's level (main.c BEEP_PEAK); stock's alarm tone refused where the firmware has none; a sound of
one's own uploaded as the page makes it (16-bit mono WAV at 48 kHz) and every other upload refused; the page's
"Play"; deleting it; the choice after a restart, and the beep by default on a firmware without the tone.  The page's
loudness check (web/loudness.js) is tests/unit/web_loudness_test.py's."""
import asyncio, io, json, math, os, re, signal, struct, subprocess, sys, tempfile, wave
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev, VoiceAssistantTimerEventType as Tm

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import Browser, wait_port
PORT, WEB = 17141, 17142
TIMER_PEAK, OWN_PEAK = 20000, 12345                     # the stand-in timer tone's level, the uploaded one's
_B, _N = int(re.search(r"#define BEEP_PEAK (\d+)", open(os.path.join(ROOT, "src/hassmic/main.c")).read()).group(1)), 5760
BEEP_PEAK = max(abs(int(_B * min(1.0, min(i, _N - i) / 480) * math.sin(2 * math.pi * (880.0 if i < _N // 2 else 1320.0) * i / 48000))) for i in range(_N))

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
    def __init__(self, tmp, stock=True):
        """stock: the firmware has the timer tone"""
        self.tmp, self.state = tmp, os.path.join(tmp, "state")
        os.makedirs(self.state, exist_ok=True)
        if not os.path.exists(os.path.join(self.state, "adopted")): open(os.path.join(self.state, "adopted"), "w").write("ha\n")
        earcons = os.path.join(tmp, "earcons") + "/"
        os.makedirs(earcons, exist_ok=True)              # stock's timer tone, 1 s; no alarm tone on this "firmware"
        if stock: open(earcons + "system_alerts_melodic_02.wav", "wb").write(wav(48000, 1, 1.0, TIMER_PEAK))
        elif os.path.exists(earcons + "system_alerts_melodic_02.wav"): os.unlink(earcons + "system_alerts_melodic_02.wav")
        keys = os.path.join(tmp, "keypad")
        if not os.path.exists(keys): os.mkfifo(keys)
        self.keypad = os.open(keys, os.O_RDWR)
        self.log = open(os.path.join(tmp, "log"), "a+")
        addrs = os.path.join(tmp, "addrs"); open(addrs, "w").write("127.0.0.1\n")
        env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(tmp, "play.raw"),
                   HASSMIC_MDNS_FILE=os.path.join(tmp, "mdns"), HASSMIC_FAKE_ADDRS=addrs, HASSMIC_ARB_ADDR="127.255.255.255",
                   HASSMIC_MODELS=os.path.join(tmp, "models"), HASSMIC_LED_ROOT=os.path.join(tmp, "etc"), HASSMIC_EARCONS=earcons)
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-W", str(WEB), "-b", keys],
                                     env=env, stderr=self.log)

    def text(self): self.log.seek(0); return self.log.read()
    def config(self): return dict(l.strip().split("=", 1) for l in open(os.path.join(self.state, "config")) if "=" in l and not l.startswith("#"))
    def stop(self): self.proc.terminate(); self.proc.wait(5); os.close(self.keypad); self.log.close()

    def count(self, pattern): return len(re.findall(pattern, self.text()))

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
        ents, _ = await c.list_entities_services()
        self.by = {x.object_id: x for x in ents}
        self.started, self.stopped = asyncio.Event(), []
        async def start(*a): self.started.set(); return 0
        async def stop(*a): self.stopped.append(True)
        async def nothing(*a): return 0
        c.subscribe_voice_assistant(handle_start=start, handle_stop=stop, handle_audio=nothing, handle_announcement_finished=nothing)
        await asyncio.sleep(0.3)
        return c


BEEP = r"earcon: 5760 samples @ 48000 Hz, peak (\d+)"
TIMER = rf"earcon: 48000 samples @ 48000 Hz, until stopped, peak {TIMER_PEAK}"
OWN = rf"earcon: 24000 samples @ 48000 Hz, until stopped, peak {OWN_PEAK}"
CUT = r"earcon: cut at (\d+) ms"


async def main():
    tmp = tempfile.mkdtemp()
    e = Echo(tmp)
    try:
        page = e.page(); c = await e.connect()
        ts = lambda: page.state()["timer_sound"]
        pv = lambda: next(x for x in page.state()["settings"] if x["name"] == "timer_sound")
        s = pv()
        check(s["choices"] == ["timer", "alarm", "beep", "custom"] and s["choices"][s["value"]] == "timer" and s["group"] == "Voice" and s["export"],
              f"timer_sound: a choice of the Voice group, exported, Alexa's timer tone by default: {s}")
        check(ts() == {"custom_ms": 0, "present": ["beep", "timer"]}, f"the page is told which sounds this Echo has: {ts()}")

        # stock's timer tone: looped until stopped, cut at once by the stop
        n, cuts, beeps = e.count(TIMER), e.count(CUT), e.count(BEEP)
        c.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t2", "pasta", 60, 0, False)
        check(await until(lambda: e.count(TIMER) >= n + 2, 4), f"the timer tone rings over and over: {e.count(TIMER) - n} times")
        check(e.count(BEEP) == beeps, "no beep with it")
        await asyncio.sleep(0.3); e.proc.send_signal(signal.SIGUSR2)          # the action button
        check(await until(lambda: e.count(CUT) > cuts, 1), "the action button cuts the tone off at once, mid-sound")
        n = e.count(TIMER); await asyncio.sleep(1.5)
        check(e.count(TIMER) == n, "and it does not come back")

        # the beep: three blips a round, as loud as Alexa's tone
        r = page.set(timer_sound="beep")
        check(r.get("applied") == 1 and e.config().get("timer_sound") == "beep", f"the beep chosen and saved: {r}, {e.config().get('timer_sound')}")
        n = e.count(BEEP)
        c.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t1", "tea", 60, 0, False)
        check(await until(lambda: e.count(BEEP) >= n + 3), "the beep: the built-in triple blip")
        pk = [int(x) for x in re.findall(BEEP, e.text())][-1]
        check(pk == BEEP_PEAK, f"at Alexa's level: peak {pk} (BEEP_PEAK {_B}: {BEEP_PEAK}), the wake sound's stand-in stays at 6000")
        offs = e.count("alarm: off"); e.proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: e.count("alarm: off") > offs, 2), "the button stops it")
        page.set(timer_sound="timer")

        # a pipeline cuts it (speech to text must hear the user) and it comes back after; our own sounds cut it too
        n, cuts = e.count(TIMER), e.count(CUT)
        c.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t3", "eggs", 60, 0, False)
        await until(lambda: e.count(TIMER) > n); await asyncio.sleep(0.3)
        e.started.clear(); e.proc.send_signal(signal.SIGUSR1)                    # the wake word
        await asyncio.wait_for(e.started.wait(), 5)
        check(await until(lambda: e.count(CUT) > cuts, 1), "the wake word's pipeline cuts the ring")
        n = e.count(TIMER); await asyncio.sleep(1.2)
        check(e.count(TIMER) == n, "no ring while the pipeline runs")
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        check(await until(lambda: e.count(TIMER) > n, 3), "the ring comes back after it")
        offs = e.count("alarm: off"); c.button_command(e.by["stop_ringing"].key)
        check(await until(lambda: e.count("alarm: off") > offs, 2), 'Home Assistant\'s "Stop ringing" stops it')

        # stock's alarm tone is not on this "firmware"; one's own needs an upload first
        r = page.set(timer_sound="alarm")
        check(r.get("applied") == 0 and "firmware has no such sound" in r.get("errors", "") and e.config().get("timer_sound") == "timer",
              f"Alexa's alarm tone refused where the firmware has none: {r}")
        r = page.set(timer_sound="custom")
        check(r.get("applied") == 0 and "upload one" in r.get("errors", ""), f"one's own refused before an upload: {r}")

        # uploads: only what the page makes of a file
        for body, why, what in ((wav(44100, 1, 0.5, 1000), "48000 Hz", "44.1 kHz"), (wav(48000, 2, 0.5, 1000), "2 channels", "stereo"),
                                (b"ID3\x04" + b"\0" * 2000, "not a WAV", "an MP3"), (wav(48000, 1, 0.05, 1000), "too short", "50 ms")):
            st, _, data = page.call("POST", "/api/timer-sound", body)
            check(st == 400 and why in json.loads(data).get("error", ""), f"upload of {what} refused: {st} {data[:120]!r}")
        try: st = page.call("POST", "/api/timer-sound", wav(48000, 1, 31, 1000))[0]      # answered before the body is read
        except (ConnectionResetError, BrokenPipeError): st = 413        # closed while we still send: a reset or, on CI, a broken pipe
        check(st == 413, f"upload of 31 s refused as too large: {st}")
        check(ts()["custom_ms"] == 0 and not os.path.exists(os.path.join(e.state, "timer_sound.wav")), "nothing kept of them")
        st, _, data = page.call("POST", "/api/timer-sound", wav(48000, 1, 0.5, OWN_PEAK))
        check(st == 200 and ts()["custom_ms"] == 500 and "custom" in ts()["present"], f"a 0.5 s WAV of 16-bit mono at 48 kHz taken: {st} {ts()}")
        r = page.set(timer_sound="custom")
        check(r.get("applied") == 1 and e.config().get("timer_sound") == "custom", f"then one's own can be chosen: {r}")

        # the page's "Play": one ring of it, at once, without a timer
        n = e.count(OWN)
        st, _, _ = page.call("POST", "/api/timer-sound/play")
        check(st == 200 and await until(lambda: e.count(OWN) == n + 1, 2), "Play: the chosen sound once")
        await asyncio.sleep(1.0)
        check(e.count(OWN) == n + 1, "once only")

        # it rings with it; replacing it plays the new one at the next round
        c.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t4", "pizza", 60, 0, False)
        check(await until(lambda: e.count(OWN) >= n + 3, 3), "a timer rings with one's own sound, over and over")
        page.call("POST", "/api/timer-sound", wav(48000, 1, 0.25, OWN_PEAK - 1))
        check(await until(lambda: "earcon: 12000 samples @ 48000 Hz, until stopped, peak 12344" in e.text(), 3), "a new upload rings from the next round on")
        e.proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.3)

        # deleted while chosen: back to the default
        st, _, _ = page.call("POST", "/api/timer-sound", b"")
        check(st == 200 and ts()["custom_ms"] == 0 and e.config().get("timer_sound") == "timer" and not os.path.exists(os.path.join(e.state, "timer_sound.wav")),
              f"deleting it puts Alexa's timer tone back: {ts()}, {e.config().get('timer_sound')}")

        # the choice after a restart; one's own deleted behind its back: the default at start
        page.set(timer_sound="beep"); await c.disconnect(); e.stop()
        e = Echo(tmp); page = e.page()
        check(pv()["choices"][pv()["value"]] == "beep", "the beep is still chosen after a restart")
        cfg = open(os.path.join(e.state, "config")).read().replace("timer_sound=beep", "timer_sound=custom")
        e.stop(); open(os.path.join(e.state, "config"), "w").write(cfg)
        e = Echo(tmp); page = e.page()
        check(pv()["choices"][pv()["value"]] == "timer", "one's own chosen without its file: Alexa's timer tone at start")
        # a firmware without the tone (as cronos or crown may be): the beep, which is as loud
        e.stop(); e = Echo(tmp, stock=False); page = e.page()
        check(pv()["choices"][pv()["value"]] == "beep" and ts()["present"] == ["beep"], f"no timer tone in the firmware: the beep ({ts()})")
    finally:
        try: e.stop()
        except Exception: pass
        if check.failed: print("--- hassmic's log ---\n" + open(os.path.join(tmp, "log")).read()[-6000:])


asyncio.run(main())
print("FAILED" if check.failed else "all good")
sys.exit(1 if check.failed else 0)
