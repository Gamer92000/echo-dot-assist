#!/usr/bin/env python3
"""Separate volumes (main.c, issue #32) against build/hassmic-host, as Home Assistant: the mixer's volumes as files
(HASSMIC_FAKE_MIXER): music, replies, sounds, timers and calls at volumes of their own, the mixer at the loudest,
Home Assistant's media at the music volume although it plays on the same stream, the volume keys on what plays, the
numbers in Home Assistant, the quieter ones turned down in their PCM, kinds that follow the music some points above or
below it, a restart, a Bluetooth speaker (HASSMIC_FAKE_SPEAKER), the page and exports, and everything as before with the
feature off."""
import asyncio, contextvars, io, json, os, re, signal, socket, struct, subprocess, sys, tempfile, threading, traceback, wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from aioesphomeapi import VoiceAssistantEventType as Ev, APIClient, MediaPlayerInfo, MediaPlayerEntityState, MediaPlayerState, NumberInfo, NumberState
from aioesphomeapi import VoiceAssistantTimerEventType as Tm

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import Browser
HTTP_PORT = 16984
KEY_VOLUMEUP, KEY_VOLUMEDOWN = 115, 114
# The parts below run at once, each with Echos of its own (most of the time goes to audio played at real time and to
# hassmic's own periods): part i's Echos on ESPHome port PORT + 2i, their settings pages one above
PORT = 17100

# what a part checks, kept until the end and printed part by part: the parts run at once
out = contextvars.ContextVar("out", default=None)
def check(cond, what):
    line = ("ok   " if cond else "FAIL ") + what
    if out.get() is None: print(line)
    else: out.get().append(line)
    if not cond: check.failed = True
check.failed = False


async def until(cond, timeout=5.0):
    end = asyncio.get_running_loop().time() + timeout
    while not cond() and asyncio.get_running_loop().time() < end:
        await asyncio.sleep(0.02)
    return cond()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):                                   # silence, 2.5 s of it: long enough to press a key meanwhile
        b = io.BytesIO()
        with wave.open(b, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000); w.writeframes(b"\0\0" * 120000)
        self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.end_headers()
        try: self.wfile.write(b.getvalue())
        except OSError: pass
    def log_message(self, *a): pass


class Echo:
    def __init__(self, part, config, again=None):
        """again: an Echo stopped before, started anew on its state and mixer (a restart)"""
        self.port, self.web = PORT + 2 * part, PORT + 2 * part + 1
        if again: self.tmp, self.state, self.mixer = again.tmp, again.state, again.mixer
        else:
            self.tmp = tempfile.mkdtemp(); self.state = os.path.join(self.tmp, "state"); self.mixer = os.path.join(self.tmp, "mixer")
            os.makedirs(self.state); os.makedirs(self.mixer)
            with open(os.path.join(self.state, "config"), "w") as f: f.write(config)
            with open(os.path.join(self.state, "adopted"), "w") as f: f.write("ha\n")
            self.prop("MainVolume", 40); self.prop("TTSVolume", 40)
        self.speaker = os.path.join(self.tmp, "speaker")
        keys = os.path.join(self.tmp, "keypad")
        if not os.path.exists(keys): os.mkfifo(keys)
        self.keypad = os.open(keys, os.O_RDWR)          # opened first: hassmic's open() would wait for a writer
        self.log = open(os.path.join(self.tmp, "log"), "w+")
        env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_FAKE_MIXER=self.mixer, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
                   HASSMIC_PLAY=os.path.join(self.tmp, "play.raw"), HASSMIC_MDNS_FILE=os.path.join(self.tmp, "mdns"),
                   HASSMIC_FAKE_ADDRS=os.path.join(self.tmp, "addrs"), HASSMIC_ARB_ADDR="127.255.255.255",
                   HASSMIC_MODELS=os.path.join(self.tmp, "models"), HASSMIC_LED_ROOT=os.path.join(self.tmp, "etc"),
                   HASSMIC_FAKE_SPEAKER=self.speaker)
        with open(env["HASSMIC_FAKE_ADDRS"], "w") as f: f.write("127.0.0.1\n")
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.port), "-n", "Echo Dot", "-W", str(self.web),
                                      "-a", "0", "-z", "0", "-o", "0", "-b", keys], env=env, stderr=self.log)

    def prop(self, name, v=None):
        p = os.path.join(self.mixer, name)
        if v is not None:
            with open(p, "w") as f: f.write(f"{v}\n")
        try: return int(open(p).read())
        except (OSError, ValueError): return None

    def key(self, code):
        for v in (1, 0): os.write(self.keypad, struct.pack("llHHi", 0, 0, 1, code, v))     # struct input_event, EV_KEY

    def text(self): self.log.seek(0); return self.log.read()

    def config(self): return dict(l.strip().split("=", 1) for l in open(os.path.join(self.state, "config")) if "=" in l)

    async def connect(self):
        for _ in range(100):
            try:
                c = APIClient("127.0.0.1", self.port, None); await c.connect(login=True); break
            except Exception: await asyncio.sleep(0.1)
        else: raise RuntimeError("no connection")
        self.ents, _ = await c.list_entities_services()
        self.states = []; c.subscribe_states(self.states.append)
        async def nothing(*a): return 0
        c.subscribe_voice_assistant(handle_start=nothing, handle_stop=nothing, handle_audio=nothing, handle_announcement_finished=nothing)
        await asyncio.sleep(0.3)
        return c

    def stop(self): self.proc.terminate(); self.proc.wait(5); os.close(self.keypad); self.log.close()

    def page(self):
        b = Browser(self.web)
        if not b.login_with_button(self.proc): raise RuntimeError("page login")
        return b

    def music_file(self):
        try: return open(os.path.join(self.state, "volume.music")).read().strip()
        except OSError: return None


async def watch(echo, prop, until_done):
    """every value prop takes while until_done runs"""
    seen = []
    async def poll():
        while True:
            v = echo.prop(prop)
            if not seen or seen[-1] != v: seen.append(v)
            await asyncio.sleep(0.02)
    t = asyncio.create_task(poll())
    try: r = await until_done
    finally: t.cancel()
    return seen, r


async def stop_media(echo, c, mp):
    """Home Assistant's media stopped (what it plays is 2.5 s long), and the media player idle again"""
    n = len(echo.states)
    c.media_player_command(mp.key, command=2)
    await until(lambda: [x.state for x in echo.states[n:] if isinstance(x, MediaPlayerEntityState)][-1:] == [MediaPlayerState.IDLE])


async def wake_sound_peak(echo, c):
    n = len(re.findall(r"earcon: .* peak (\d+)", echo.text()))
    echo.proc.send_signal(signal.SIGUSR1)                       # the wake word: the wake sound plays
    await until(lambda: len(re.findall(r"earcon: .* peak (\d+)", echo.text())) > n)
    peaks = re.findall(r"earcon: .* peak (\d+)", echo.text())
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)    # the run ends: nothing busy for a timer's ring
    await asyncio.sleep(0.5)
    return int(peaks[n]) if len(peaks) > n else None


# the Echo's volume curve (main.c vol_db): what a kind at volume v is turned down by under the mixer at top
db = {20: -35.7, 30: -30.5, 40: -26.5, 50: -22.4, 60: -19.5, 70: -18.4, 80: -14.0, 90: -10.0, 100: 0.0}
g = lambda v, top: 10 ** ((db[v] - db[top]) / 20)
near = lambda a, b: abs(a - b) < 0.003


async def part_off(url, full):
    # Off (the default): one volume, as before
    echo = Echo(0, "")
    try:
        c = await echo.connect()
        check(not [e for e in echo.ents if isinstance(e, NumberInfo) and e.object_id == "assistant_volume"], "off: no Assistant volume in Home Assistant")
        mp = next(e for e in echo.ents if isinstance(e, MediaPlayerInfo))
        c.media_player_command(mp.key, volume=0.55)
        await until(lambda: echo.prop("MainVolume") == 55 and echo.prop("TTSVolume") == 55)
        check(echo.prop("MainVolume") == 55 and echo.prop("TTSVolume") == 55, "off: the media player's volume moves both of the mixer's")
        level = await wake_sound_peak(echo, c); full.set_result(level)
        check(level is not None and level > 5000, f"off: the wake sound at full level ({level})")
        c.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t1", "tea", 60, 0, False)
        await until(lambda: echo.text().count("earcon: 5760 samples") >= 4)
        check("alarm stream: " not in echo.text() and echo.prop("AlarmVolume") is None, "off: a timer rings on the Earcon stream, AlarmVolume left alone")
        c.media_player_command(mp.key, command=2)       # stop: ends the ring
        await c.disconnect()
    finally:
        echo.stop()


async def part_separate(url, full):
    gain = lambda what: float(re.findall(rf"audio: {what} gain ([0-9.]+)", echo.text())[-1]) if f"{what} gain" in echo.text() else 1.0
    # Separate: music 40 (MainVolume as it was), replies 60, sounds 20, timers 80.  Both mixer volumes at the loudest
    # (timers, 80), where libasp agrees with them; everything quieter turned down in our PCM by the Echo's curve
    # between its volume and 80 (main.c vol_db)
    echo = Echo(1, "separate_volumes=on\nassistant_volume=60\nsounds_volume=20\ntimer_volume=80\ncall_volume_linked=off\ncall_volume=100\n")
    try:
        c = await echo.connect()
        nums = {e.object_id: e for e in echo.ents if isinstance(e, NumberInfo)}
        check(all(k in nums for k in ("assistant_volume", "sounds_volume", "timer_volume")),
              f"separate: Assistant, Sounds and Timer volume in Home Assistant ({sorted(nums)})")
        num = lambda k: ([x.state for x in echo.states if isinstance(x, NumberState) and x.key == nums[k].key] or [None])[-1]
        await until(lambda: echo.prop("MainVolume") == 80 and echo.prop("TTSVolume") == 80 and near(gain("tts"), g(60, 80)))
        check(echo.prop("MainVolume") == 80 and echo.prop("TTSVolume") == 80,
              "separate: both mixer volumes at the loudest of the four (timers, 80; calls' own 100 counts only during a call)")
        check(near(gain("music"), g(40, 80)) and near(gain("tts"), g(60, 80)),
              f"music at 40 and replies at 60 turned down under 80 in their PCM ({gain('music'):.3f}, {gain('tts'):.3f})")
        check("call_volume" not in nums, "no Call volume in Home Assistant without Drop In")
        mp = next(e for e in echo.ents if isinstance(e, MediaPlayerInfo))
        c.media_player_command(mp.key, volume=0.5)
        await until(lambda: near(gain("music"), g(50, 80)))
        check(echo.prop("MainVolume") == 80 and near(gain("music"), g(50, 80)) and near(gain("tts"), g(60, 80)),
              "the media player moves the music alone: its gain, not the mixer or the replies")
        await until(lambda: echo.music_file() == "50")             # written at most once a second, outside the lock
        check(echo.music_file() == "50", "the music volume kept in state/volume.music (the mixer says nothing of it)")
        c.media_player_command(mp.key, volume=0.9)
        await until(lambda: echo.prop("MainVolume") == 90 and gain("music") == 1.0)
        check(echo.prop("MainVolume") == 90 and echo.prop("TTSVolume") == 90 and gain("music") == 1.0 and near(gain("tts"), g(60, 90)),
              "music the loudest: the mixer follows it, its gain 1, the replies' gain retaken under 90")
        seen, res = await watch(echo, "TTSVolume", c.send_voice_assistant_announcement_await_response(url, 15, "x"))
        check(res.success and seen == [90] and near(gain("tts"), g(60, 90)), f"an announcement: the mixer untouched, its PCM at the replies' 60 ({seen})")
        c.media_player_command(mp.key, media_url=url)
        await until(lambda: gain("tts") == 1.0)
        check(gain("tts") == 1.0, "Home Assistant's media on the same stream: at the music volume (gain 1 under 90)")
        await stop_media(echo, c, mp)                  # not played to its end: another would be dropped as busy
        c.media_player_command(mp.key, media_url=url, announcement=True)     # what tts.speak sends: speech
        await until(lambda: near(gain("tts"), g(60, 90)))
        check(near(gain("tts"), g(60, 90)), f"the media player's announcement (tts.speak): at the replies' 60, not the music's ({gain('tts'):.3f})")
        await stop_media(echo, c, mp)
        snd = await wake_sound_peak(echo, c)
        want = (await full) * g(20, 90)
        check(snd is not None and abs(snd - want) <= 3, f"sounds at 20 under 90: the wake sound {snd} of {full.result()} (want {want:.0f})")
        c.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t1", "tea", 60, 0, False)
        await until(lambda: echo.text().count("earcon: 5760 samples") >= 4)
        peaks = [int(x) for x in re.findall(r"earcon: 5760 samples @ 48000 Hz, peak (\d+)", echo.text())]
        want = (await full) * g(80, 90)
        check(abs(peaks[-1] - want) <= 3, f"timers at 80 under 90: the ring at {peaks[-1]} (want {want:.0f})")
        echo.key(KEY_VOLUMEDOWN)                       # while it rings: the timers' volume
        await until(lambda: num("timer_volume") == 70)
        check(num("timer_volume") == 70 and echo.prop("MainVolume") == 90 and "volume: timers 70" in echo.text(),
              f"a volume key while a timer rings moves the timers' volume ({num('timer_volume')})")
        c.media_player_command(mp.key, command=2)
        await asyncio.sleep(0.5)
        ann = asyncio.ensure_future(c.send_voice_assistant_announcement_await_response(url, 15, "x"))
        await until(lambda: echo.text().count("media: announce") >= 2)
        await asyncio.sleep(0.8)
        echo.key(KEY_VOLUMEUP)                         # during a reply: the replies' volume
        await until(lambda: num("assistant_volume") == 70 and near(gain("tts"), g(70, 90)))
        check(num("assistant_volume") == 70 and near(gain("tts"), g(70, 90)) and echo.prop("TTSVolume") == 90,
              f"a volume key during a reply moves the replies' volume (60 -> 70; {num('assistant_volume')})")
        await ann
        echo.key(KEY_VOLUMEDOWN)                       # nothing plays: the music
        moved = lambda: "volume: 80" in echo.text() and echo.prop("MainVolume") == 80 and echo.prop("TTSVolume") == 80
        await until(moved)                             # every part: the mixer's two volumes and the log come apart (CI)
        check(moved(),
              "a volume key with nothing playing moves the music (90 -> 80: the mixer with it, the loudest still)")
        c.number_command(nums["sounds_volume"].key, 100)
        loud = lambda: (echo.prop("MainVolume") == 100 and echo.prop("TTSVolume") == 100 and near(gain("music"), g(80, 100))
                        and echo.config().get("sounds_volume") == "100")   # the mixer, the gains, state/config: one at a time
        await until(loud)
        check(loud(),
              "Sounds volume 100 from Home Assistant: the mixer at 100, the music turned down under it, kept in state/config")
        echo.prop("TTSVolume", 20)                     # a stock daemon moves it: put back
        await until(lambda: echo.prop("TTSVolume") == 100, 4)
        check(echo.prop("TTSVolume") == 100, "a TTSVolume moved behind our back is put back within 2 s")
        echo.prop("MainVolume", 30)
        await until(lambda: echo.prop("MainVolume") == 100, 4)
        check(echo.prop("MainVolume") == 100, "so is MainVolume, which is not the music volume while separate")
        await c.disconnect()
    finally:
        echo.stop()


async def part_linked(url, full):
    gain = lambda what: float(re.findall(rf"audio: {what} gain ([0-9.]+)", echo.text())[-1]) if f"{what} gain" in echo.text() else 1.0
    # Following the music: replies 20 points above it, sounds 20 below, timers their own 30; music 40 (MainVolume as it was)
    echo = Echo(2, "separate_volumes=on\nassistant_volume_linked=on\nassistant_volume_offset=20\n"
                "sounds_volume_linked=on\nsounds_volume_offset=-20\ntimer_volume=30\n")
    try:
        c = await echo.connect()
        nums = {e.object_id: e for e in echo.ents if isinstance(e, NumberInfo)}
        check("timer_volume" in nums and "assistant_volume" not in nums and "sounds_volume" not in nums,
              f"linked: only the timers' own volume in Home Assistant ({sorted(nums)})")
        await until(lambda: echo.prop("MainVolume") == 60 and near(gain("music"), g(40, 60)))
        check(echo.prop("MainVolume") == 60 and echo.prop("TTSVolume") == 60 and near(gain("music"), g(40, 60)),
              "replies at music + 20 = 60 the loudest: the mixer there, the music under it")
        mp = next(e for e in echo.ents if isinstance(e, MediaPlayerInfo))
        c.media_player_command(mp.key, volume=0.5)
        await until(lambda: echo.prop("MainVolume") == 70 and near(gain("music"), g(50, 70)))
        check(echo.prop("MainVolume") == 70 and echo.prop("TTSVolume") == 70 and near(gain("music"), g(50, 70)),
              "the media player moves the linked replies with the music (50 + 20 = 70)")
        check(near(gain("call"), g(50, 70)), f"calls follow the music by default, as before they had a volume ({gain('call'):.3f})")
        snd = await wake_sound_peak(echo, c)
        want = (await full) * g(30, 70)
        check(snd is not None and abs(snd - want) <= 3, f"linked sounds at 50 - 20 = 30 under 70: the wake sound {snd} (want {want:.0f})")
        ann = asyncio.ensure_future(c.send_voice_assistant_announcement_await_response(url, 15, "x"))
        await until(lambda: echo.text().count("media: announce") >= 1)
        await asyncio.sleep(0.8)
        check(near(gain("tts"), 1.0), "the announcement at the replies' 70, the mixer's own")
        echo.key(KEY_VOLUMEUP)                         # during a reply that follows the music: the music
        await until(lambda: "volume: 60" in echo.text() and echo.prop("MainVolume") == 80)
        check("volume: 60" in echo.text() and echo.prop("MainVolume") == 80 and "volume: assistant" not in echo.text(),
              "a volume key during a linked reply moves the music, the reply with it (60 + 20 = 80)")
        await ann
        await c.disconnect()
    finally:
        echo.stop()


async def part_imported(url, full):
    gain = lambda what: float(re.findall(rf"audio: {what} gain ([0-9.]+)", echo.text())[-1]) if f"{what} gain" in echo.text() else 1.0
    # Switched by an import (state/preset), nothing jumps: replies back to their own volume where they played (music
    # 40 + 20), sounds at 95 linked 55 above the music (past the old +-50, which moved them to 90)
    echo = Echo(3, "separate_volumes=on\nassistant_volume_linked=on\nassistant_volume_offset=20\nsounds_volume=95\n")
    with open(os.path.join(echo.state, "preset"), "w") as f: f.write("assistant_volume_linked=off\nsounds_volume_linked=on\n")
    try:
        c = await echo.connect()
        nums = {e.object_id: e for e in echo.ents if isinstance(e, NumberInfo)}
        cfg = echo.config()
        check(cfg.get("assistant_volume_linked") == "off" and cfg.get("assistant_volume") == "60" and "assistant_volume" in nums,
              f"unlinked: its own volume where it played (60), its number back in Home Assistant ({cfg.get('assistant_volume')})")
        check(cfg.get("sounds_volume_linked") == "on" and cfg.get("sounds_volume_offset") == "55" and "sounds_volume" not in nums,
              f"linked: the offset where it played (95 = 40 + 55), its number gone ({cfg.get('sounds_volume_offset')})")
        await c.disconnect()
    finally:
        echo.stop()


async def part_restart(url, full):
    gain = lambda what: float(re.findall(rf"audio: {what} gain ([0-9.]+)", echo.text())[-1]) if f"{what} gain" in echo.text() else 1.0
    # A restart: music 50 (state/volume.music, not MainVolume, which is the loudest), replies their own 70, sounds
    # 20 below the music, timers their own 40; as they were afterwards.  The config backwards, as by hand: the
    # switches still come first (settings.c), so they seed nothing over the saved values
    echo = Echo(4, "timer_volume=40\nsounds_volume_offset=-20\nsounds_volume_linked=on\nassistant_volume=70\nseparate_volumes=on\n")
    try:
        c = await echo.connect()
        mp = next(e for e in echo.ents if isinstance(e, MediaPlayerInfo))
        c.media_player_command(mp.key, volume=0.5)
        await until(lambda: echo.music_file() == "50" and echo.prop("MainVolume") == 70)
        await c.disconnect()
    finally:
        echo.stop()
    echo = Echo(4, None, again=echo)
    try:
        c = await echo.connect()
        await until(lambda: near(gain("music"), g(50, 70)))
        mp_state = [x for x in echo.states if hasattr(x, "volume")]
        check(echo.prop("MainVolume") == 70 and echo.prop("TTSVolume") == 70 and near(gain("music"), g(50, 70)) and gain("tts") == 1.0,
              f"restarted: the mixer at the replies' 70, the music at 50 under it ({echo.prop('MainVolume')}, {gain('music'):.3f})")
        check(mp_state and abs(mp_state[-1].volume - 0.5) < 0.01, f"  the media player says 50 % ({mp_state[-1].volume if mp_state else None})")
        snd = await wake_sound_peak(echo, c)
        want = (await full) * g(30, 70)
        check(snd is not None and abs(snd - want) <= 3, f"  sounds still 20 below the music: at 30 ({snd}, want {want:.0f})")
        await c.disconnect()
    finally:
        echo.stop()

    # volume.music left over: state/config edited to one volume by hand (or an older build ran meanwhile, which knows no
    # such file) and the volume moved to 30 since.  That is MainVolume's, and the file goes
    cfg_path = os.path.join(echo.state, "config")
    with open(cfg_path) as f: text = f.read()
    check("separate_volumes=on" in text and echo.music_file() == "50", "  (before: separate in state/config, music 50 in its file)")
    with open(cfg_path, "w") as f: f.write(text.replace("separate_volumes=on", "separate_volumes=off"))
    echo.prop("MainVolume", 30); echo.prop("TTSVolume", 30)
    echo = Echo(4, None, again=echo)
    try:
        c = await echo.connect()
        mp_state = [x for x in echo.states if hasattr(x, "volume")]
        check(echo.music_file() is None and mp_state and abs(mp_state[-1].volume - 0.3) < 0.01 and "volume.music left over" in echo.text(),
              f"a left over volume.music with one volume: MainVolume's 30, the file deleted ({mp_state[-1].volume if mp_state else None})")
        await c.disconnect()
    finally:
        echo.stop()
    # the setting given twice, the last line on (added by hand): what loading ends up with decides, the file counts
    with open(cfg_path, "a") as f: f.write("separate_volumes=on\n")
    with open(os.path.join(echo.state, "volume.music"), "w") as f: f.write("45\n")
    echo = Echo(4, None, again=echo)
    try:
        c = await echo.connect()
        mp_state = [x for x in echo.states if hasattr(x, "volume")]
        check(mp_state and abs(mp_state[-1].volume - 0.45) < 0.01 and echo.music_file() == "45",
              f"  separate_volumes off, then on further down: separate, the music 45 from its file ({mp_state[-1].volume if mp_state else None})")
        await c.disconnect()
    finally:
        echo.stop()
    # a last line loading refuses changes nothing: still separate, the file still counts
    with open(cfg_path, "a") as f: f.write("separate_volumes=yes\n")
    echo = Echo(4, None, again=echo)
    try:
        c = await echo.connect()
        mp_state = [x for x in echo.states if hasattr(x, "volume")]
        check(mp_state and abs(mp_state[-1].volume - 0.45) < 0.01 and echo.music_file() == "45",
              f"  then separate_volumes=yes, which loading refuses: still separate, the music 45 ({mp_state[-1].volume if mp_state else None})")
        await c.disconnect()
    finally:
        echo.stop()


async def part_speaker(url, full):
    gain = lambda what: float(re.findall(rf"audio: {what} gain ([0-9.]+)", echo.text())[-1]) if f"{what} gain" in echo.text() else 1.0
    # A Bluetooth speaker (absolute volume) at 70, then separate volumes switched on on the page: what the Echo's own
    # speaker had (40) is the music volume and where the others start, never the speaker's 70
    echo = Echo(5, "")
    try:
        c = await echo.connect()
        b = echo.page()
        with open(echo.speaker, "w") as f: f.write("absolute 70\n")
        await until(lambda: "a Bluetooth speaker sets the volume" in echo.text() and echo.prop("MainVolume") == 100)
        check(echo.prop("MainVolume") == 100, "a Bluetooth speaker with absolute volume: the mixer at full scale")
        r = b.set(separate_volumes="on")
        await asyncio.sleep(1.5)
        check(r.get("applied") == 1 and echo.music_file() is None, f"separate volumes on while it plays: no speaker's volume kept as the music's ({echo.music_file()})")
        cfg = echo.config()
        check(cfg.get("assistant_volume") == "40" and cfg.get("timer_volume") == "40", f"  the others start at the Echo's own 40 ({cfg.get('assistant_volume')})")
        await until(lambda: near(gain("tts"), g(40, 70)))
        check("volume: the speaker at 70" in echo.text() and gain("music") == 1.0 and near(gain("tts"), g(40, 70)),
              f"  separate on the speaker too: it at the loudest (music, 70), replies at 40 turned down under it ({gain('tts'):.3f})")
        b.set(volume=60)                               # the music volume, on the speaker: its volume
        await asyncio.sleep(1.5)                       # the music volume's write would have come by now
        check(echo.music_file() is None and echo.prop("MainVolume") == 100 and "volume: the speaker at 60" in echo.text(),
              "  the music volume set on the page: the speaker at 60, nothing kept as the Echo's music volume, the mixer at full scale")
        b.set(assistant_volume=90)
        await until(lambda: "volume: the speaker at 90" in echo.text())
        check("volume: the speaker at 90" in echo.text() and near(gain("music"), g(60, 90)) and echo.prop("MainVolume") == 100,
              f"  replies at 90 the loudest: the speaker at 90, the music at 60 turned down under it ({gain('music'):.3f})")
        with open(echo.speaker, "w") as f: f.write("absolute 80\n")             # the speaker's own buttons: 90 -> 80
        await until(lambda: b.state().get("volume") == 50)
        check(b.state().get("volume") == 50 and near(gain("music"), g(50, 90)) and "the speaker's own buttons, 80" in echo.text()
              and "volume: the speaker back at 90, the loudest" in echo.text(),
              f"  its own buttons, 10 down: the music 10 down (60 -> 50), the speaker back at the replies' 90 ({b.state().get('volume')})")
        b.set(assistant_volume=40)
        with open(echo.speaker, "w") as f: f.write("none\n")
        await until(lambda: echo.music_file() == "40" and echo.prop("MainVolume") == 40)
        check(echo.music_file() == "40" and echo.prop("MainVolume") == 40 and echo.prop("TTSVolume") == 40,
              f"the speaker gone: music back at 40, kept, the mixer there ({echo.music_file()}, {echo.prop('MainVolume')})")

        # the page sets the music volume and switches a kind to follow it: no jump, its number leaves Home Assistant
        r = b.set(volume=50)
        await until(lambda: echo.music_file() == "50")
        r = b.set(timer_volume_linked="on")
        st = b.state(); vals = {x["name"]: x["value"] for x in st["settings"]}
        check(vals.get("timer_volume_linked") == 1 and vals.get("timer_volume_offset") == -10 and st.get("volume") == 50,
              f"the page: timers follow the music, 10 below it where they played ({vals.get('timer_volume_offset')})")
        await asyncio.sleep(0.5)
        c2 = await echo.connect()
        nums = {e.object_id for e in echo.ents if isinstance(e, NumberInfo)}
        check("timer_volume" not in nums and "assistant_volume" in nums, f"  Home Assistant lists it no more ({sorted(nums)})")

        # an export: how the volumes relate, never a volume itself (the room's)
        st, _, text = b.call("GET", "/api/export")
        keys = {l.split("=")[0] for l in text.decode().splitlines() if "=" in l and not l.startswith("#")}
        want_in = {"separate_volumes"} | {f"{k}_volume_{x}" for k in ("assistant", "sounds", "timer", "call") for x in ("linked", "offset")}
        want_out = {"volume", "assistant_volume", "sounds_volume", "timer_volume", "call_volume"}
        check(want_in <= keys and not (want_out & keys), f"export: the switches and offsets, no volume ({sorted(k for k in keys if 'volume' in k)})")
        await c2.disconnect()
    finally:
        echo.stop()

async def main():
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{HTTP_PORT}/a.wav"

    full = asyncio.get_running_loop().create_future()          # the wake sound at full level: part "off" measures it
    async def part(f):
        lines = []; out.set(lines)
        try: await f(url, full)
        except Exception:
            lines.append("FAIL " + traceback.format_exc().rstrip().replace("\n", "\n     ")); check.failed = True
        finally:
            if f is part_off and not full.done(): full.set_result(None)       # failed before: the others fail, not wait
        return lines
    for lines in await asyncio.gather(*(part(f) for f in (part_off, part_separate, part_linked, part_imported, part_restart, part_speaker))):
        print("\n".join(lines))

    httpd.shutdown()
    print("FAIL" if check.failed else "PASS")
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
