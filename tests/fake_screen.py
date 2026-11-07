#!/usr/bin/env python3
"""The screen UI's side of hassmic (screen.c) and what Sendspin gives it, against build/hassmic-host-screen with a
stand-in UI (HASSMIC_SCREEN: this file run as --ui, which logs its stdin and answers with what a fifo gives it) and
Music Assistant's side played by the reference server library (aiosendspin of .venv; --legacy: 9.1.1, Music
Assistant up to 2.10.6, from build/aiosendspin-9.1.1 as tests/fake_ma_sendspin.py --setup makes it), strict both:
  - at start the UI gets the Echo's name, the action key's words, the home app ("-": none chosen), the setup state
    and its pattern
  - a model with a screen asks Sendspin for metadata@v1 and artwork@v1 (one channel, album art, JPEG, 400 px, in
    client/state as 1.0 has it), to a server that refuses what MA would only log
  - title/artist/album (any script, emoji too: \\u escapes and raw UTF-8 both), changes only, then cleared; (1.0) the
    next track's scheduled ahead shows at its time, one for now drops a scheduled one
  - artwork larger than one Noise message (1.0's transfer: an announce, then parts; 9.1.1's fragments) arrives whole
    as "art <n>", a JPEG
  - the controller's shuffle state is its own key's, not the "shuffle" among the commands; (1.0) a cover scheduled
    ahead shows at its time, one cancelled (replaced, or with none in its place) never; a cover gone clears it
  - the UI's commands reach the group's controller: next, volume
  - the light band (ring_band.c) goes to the same process: "band <w>" + one row of pixels, the setup spinner's orange
  - the face's brightness in the dark (issue #126): 35 at start, as before the setting; set on the settings page it
    reaches the UI and the export, Home Assistant's number shows it and sets it, a UI started again gets it; a UI given
    up keeps it (the number, and the line in a save)
  - the home app is the page's (tests/fake_ma_api.py), never the UI's word; a UI that dies is started again and gets
    everything anew (after a wait, up less than a minute), so is one alive that takes nothing for 10 s, or does not say
    "ready" in its time (killed; a late "ready" is not the next one's); hassmic's pid for root's kiosk_tick all along,
    until a UI that never stays up is given up; the server gone: "mx"
"""
import asyncio, io, os, random, re, signal, subprocess, sys, tempfile, threading, time

if len(sys.argv) > 1 and sys.argv[1] == "--ui":           # the stand-in UI, as hassmic runs it
    log, fifo = os.environ["FAKE_UI_LOG"], os.environ["FAKE_UI_FIFO"]
    with open(log, "ab", buffering=0) as f:
        f.write(f"#start band={os.environ.get('HMUI_BAND')}\n".encode())
        import threading
        hung = threading.Event(); out = threading.Lock()
        def tell(b):
            with out: sys.stdout.buffer.write(b); sys.stdout.buffer.flush()
        def answer():
            while True:
                with open(fifo, "rb") as q:
                    for line in q:
                        if line == b"#hang\n": hung.set(); continue       # (for the test: alive, reading nothing)
                        tell(line)
        threading.Thread(target=answer, daemon=True).start()
        late = os.path.join(os.path.dirname(fifo), "late")        # (for the test: this one says "ready" past its time,
        slow = os.path.join(os.path.dirname(fifo), "slow")        # hung; this one takes 3 s, reading nothing meanwhile)
        if os.path.exists(late):                              # (what it is sent: read, or that ends as a hang)
            os.unlink(late); threading.Thread(target=lambda: [0 for _ in iter(lambda: sys.stdin.buffer.read1(65536), b"")], daemon=True).start()
            time.sleep(5.5); tell(b"ready 960 470\n"); hung.set()
        else:
            if os.path.exists(slow): os.unlink(slow); time.sleep(3)
            tell(b"ready 960 470\n")
        carry = b""
        while True:
            if hung.is_set(): time.sleep(1); continue
            b = sys.stdin.buffer.read1(65536)
            if not b: break
            f.write(b)
            buf = carry + b                                   # hassmic's pings, answered as the UI's main loop does
            for m in re.finditer(rb"ping (\d+)\n", buf): tell(b"pong " + m.group(1) + b"\n")   # (also right after a blob)
            carry = buf[-24:]
    sys.exit(0)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEGACY = "--legacy" in sys.argv                          # aiosendspin 9.1.1 (Music Assistant up to 2.10.6), as fake_ma_sendspin.py
if LEGACY:
    LIB9 = os.path.join(ROOT, "build", "aiosendspin-9.1.1")
    if not os.path.isdir(os.path.join(LIB9, "aiosendspin")): sys.exit(f"no aiosendspin 9.1.1 in {LIB9}: run tests/fake_ma_sendspin.py --setup")
    sys.path.insert(0, LIB9)

from PIL import Image
from aioesphomeapi import APIClient
from aiosendspin.noise import Identity, InMemoryServerPairingStore
from aiosendspin.models.types import MediaCommand
if not LEGACY: from aiosendspin.models.types import ArtworkSource
from aiosendspin.server import SendspinServer, ClientAddedEvent
from aiosendspin.server.roles.metadata.state import Metadata
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import Browser, wait_port

PORT = 16968
WEB = 16977                                              # the settings page


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def ui_log(path):
    return open(path, "rb").read() if os.path.exists(path) else b""


def lines(path):
    """the text lines the UI got, images cut out (art <n> + n bytes)"""
    b, out, i = ui_log(path), [], 0
    while i < len(b):
        j = b.find(b"\n", i)
        if j < 0: break
        l = b[i:j].decode("utf-8", "replace"); i = j + 1
        out.append(l)
        m = re.fullmatch(r"art (\d+)", l)
        if m: out.append(("IMAGE", b[i:i + int(m.group(1))])); i += int(m.group(1))
        m = re.fullmatch(r"band (\d+)", l)
        if m: out.append(("BAND", b[i:i + 4 * int(m.group(1))])); i += 4 * int(m.group(1))
    return out


async def until(cond, secs):
    t0 = time.monotonic()
    while not cond() and time.monotonic() - t0 < secs: await asyncio.sleep(0.1)
    return cond()


async def main():
    state = tempfile.mkdtemp(); log = os.path.join(state, "ui.log"); fifo = os.path.join(state, "ui.fifo")
    os.mkfifo(fifo)
    runner = os.path.join(state, "ui.sh")
    # the stand-in UI's command line names this run's state: a pkill of it never hits another test's (tests/run.py -j)
    me_ui = f"{os.path.abspath(__file__)} --ui {state}"
    open(runner, "w").write(f"#!/bin/sh\nexec {sys.executable} {me_ui}\n"); os.chmod(runner, 0o755)
    open(os.path.join(state, "sendspin.unpaired"), "w").write("1")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_MUSIC=os.path.join(state, "music.raw"), HASSMIC_SETTINGS=os.path.join(state, "settings"),
               HASSMIC_SCREEN=runner, FAKE_UI_LOG=log, FAKE_UI_FIFO=fifo, HASSMIC_SCREEN_READY_MS="5000",
               HASSMIC_SCREEN_HUNG_MS="2000", HASSMIC_SCREEN_STAYED_MS="20000")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host-screen", "-p", "16969", "-z", str(PORT), "-L", "-W", str(WEB), "-o", "0"], env=env)
    # beside it, an Echo whose UI ends as soon as it starts: given up after 8 (waits of 20 ms here, doubling), the band
    # its own renderer's, no more pid for root's kiosk_tick (it opens the app now)
    gstate = tempfile.mkdtemp(); gui = os.path.join(gstate, "ui.sh")
    # (its Music Assistant: a listener that takes connections and says nothing; given up, the session ends, none again)
    mconns = []
    async def mrecord(r, w): mconns.append(time.monotonic()); await r.read(); w.close()
    msrv = await asyncio.start_server(mrecord, "127.0.0.1", 18097)
    mf = os.path.join(gstate, "mass"); open(mf, "w").write("url=http://127.0.0.1:18097\ntoken=t0ken\n"); os.chmod(mf, 0o600)
    open(gui, "w").write("#!/bin/sh\necho 'ready 960 470'\nsleep 0.6\n"); os.chmod(gui, 0o755)   # (up, then gone: its pid for root a moment)
    open(os.path.join(gstate, "config"), "w").write("face_min_brightness=80\n")        # (kept for its UI, given up or not)
    gproc = subprocess.Popen([f"{ROOT}/build/hassmic-host-screen", "-p", "16973", "-z", "16974", "-L", "-W", "0", "-o", "0", "-a", "0", "-i", "0"],
                             env=dict(env, HASSMIC_STATE=gstate, HASSMIC_MUSIC=os.path.join(gstate, "music.raw"), HASSMIC_SETTINGS=os.path.join(gstate, "settings"),
                                      HASSMIC_SCREEN=gui, HASSMIC_SCREEN_WAIT_MS="20"), stderr=subprocess.PIPE, text=True, errors="replace")
    # and one whose UI comes up and then takes nothing (hung, three times): given up as well
    hstate = tempfile.mkdtemp(); hui = os.path.join(hstate, "ui.sh")
    open(hui, "w").write("#!/bin/sh\necho 'ready 960 470'\nexec sleep 1000\n"); os.chmod(hui, 0o755)
    hproc = subprocess.Popen([f"{ROOT}/build/hassmic-host-screen", "-p", "16975", "-z", "16976", "-L", "-W", "0", "-o", "0", "-a", "0", "-i", "0"],
                             env=dict(env, HASSMIC_STATE=hstate, HASSMIC_MUSIC=os.path.join(hstate, "music.raw"), HASSMIC_SETTINGS=os.path.join(hstate, "settings"),
                                      HASSMIC_SCREEN=hui, HASSMIC_SCREEN_WAIT_MS="20"), stderr=subprocess.PIPE, text=True, errors="replace")
    hlog = []; threading.Thread(target=lambda: [hlog.append(l.rstrip()) for l in hproc.stderr], daemon=True).start()
    glog = []; gave_at = []
    threading.Thread(target=lambda: [(glog.append(l.rstrip()), "given up" in l and gave_at.append(time.monotonic())) for l in gproc.stderr], daemon=True).start()
    gmarked = False                                      # (its pid there while it tries)

    def say(line):                                       # the UI says a line (a fifo reopened per line races its reader)
        for _ in range(20):
            try:
                with open(fifo, "w") as q: q.write(line + "\n"); return
            except BrokenPipeError: time.sleep(0.05)

    loop = asyncio.get_running_loop()
    server = SendspinServer(loop, Identity.generate(), "fake-ma", pairing_store=InMemoryServerPairingStore(), allow_noncompliant_clients=False)
    added = asyncio.Queue()
    server.add_event_listener(lambda _s, ev: isinstance(ev, ClientAddedEvent) and added.put_nowait(ev.client_id))
    try:
        await until(lambda: os.path.exists(os.path.join(gstate, f"screen-ui.{gproc.pid}")), 5)
        gmarked = os.path.exists(os.path.join(gstate, f"screen-ui.{gproc.pid}"))
        await until(lambda: any(isinstance(l, str) and l.startswith("p+ setup-mode") for l in lines(log)), 5)
        got = lines(log)
        check(any(re.fullmatch(r"name Echo Dot 3 [0-9a-f]{6}|name Echo Dot 3", l) for l in got if isinstance(l, str)), f"the name first: {got[:2]}")
        for want in ("act the action button", "home -", "oobe 1"):
            check(want in got, f"at start: {want!r}")
        check(any(isinstance(l, str) and re.fullmatch(r"p\+ setup-mode \d+", l) for l in got), "at start: the setup pattern, with how long ago it came on")
        await until(lambda: "dark 35" in lines(log), 5)
        check("dark 35" in lines(log), "at start: the face's brightness in the dark, 35 % (as before it was a setting)")
        await until(lambda: any(isinstance(l, tuple) and l[0] == "BAND" for l in lines(log)), 5)
        rows = [l[1] for l in lines(log) if isinstance(l, tuple) and l[0] == "BAND"]
        px = [(r[4 * x + 2], r[4 * x + 1], r[4 * x]) for r in rows for x in range(len(r) // 4)]
        top = max(px, key=lambda p: p[0]) if px else (0, 0, 0)     # the runner's brightest LED, at the LED brightness set
        check(rows and len(rows[0]) == 960 * 4 and top[0] > 20 and top[1] < 0.5 * top[0] and top[2] < 0.15 * top[0],
              f"the band through the screen UI: {len(rows)} rows of {len(rows[0]) // 4 if rows else 0}, brightest orange {top}")

        await server.start_server(port=16967, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{PORT}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        cid = await asyncio.wait_for(added.get(), 15)
        client = server.get_client(cid)
        roles = client.info.supported_roles
        check("metadata@v1" in roles and "artwork@v1" in roles, f"metadata and artwork asked for: {roles}")
        if LEGACY:
            ch = client.info.artwork_support.channels if client.info.artwork_support else []
            check(len(ch) == 1 and ch[0].source.value == "album" and ch[0].format.value == "jpeg" and ch[0].media_width == 400,
                  f"one artwork channel, album, JPEG 400 px, in client/hello as 9.1.1 has it: {ch}")
        else: check(client.info.artwork_support is None, "the cover's channel not in client/hello (1.0: client/state)")
        await server.trust_unpaired(cid)
        await until(lambda: client.role("metadata@v1") is not None and client.role("artwork@v1") is not None, 6)
        check(client.role("metadata@v1") is not None and client.role("artwork@v1") is not None, "metadata and artwork roles active")
        if not LEGACY:
            art = client.role("artwork@v1")
            await until(lambda: art is not None and art.get_channel_configs(), 5)
            ch = list(art.get_channel_configs().values()) if art else []
            check(len(ch) == 1 and ch[0].source.value == "album" and ch[0].format.value == "jpeg" and ch[0].width == 400 and ch[0].height == 400,
                  f"one artwork channel, album, JPEG 400 px, from client/state: {ch}")

        ctl = client.group.group_role("controller")
        ctl.set_supported_commands([MediaCommand.PLAY, MediaCommand.PAUSE, MediaCommand.NEXT, MediaCommand.VOLUME, MediaCommand.SEEK, MediaCommand.REPEAT_ALL, MediaCommand.SHUFFLE])
        ctl.set_seek_max_ms(120000)                      # (seek offered only with how far: the server drops it without)
        events = []
        client.group.add_event_listener(lambda g, e: events.append(e))
        meta = client.group.group_role("metadata")
        meta.set_metadata(Metadata(title="Jóga 🎵", artist="Björk", album="Homogenic", track_progress=61000, track_duration=305000, playback_speed=1000))
        await until(lambda: any(isinstance(l, str) and l.startswith("mt ") for l in lines(log)), 5)
        mt = [l for l in lines(log) if isinstance(l, str) and l.startswith("mt ")]
        check(mt and mt[-1] == "mt Jóga 🎵\tBjörk\tHomogenic", f"metadata in UTF-8: {mt[-1:]!r}")
        mp = [l for l in lines(log) if isinstance(l, str) and l.startswith("mp ")]
        m = re.fullmatch(r"mp (\d) (\d+) (\d+) (\d+)", mp[-1]) if mp else None
        check(m and 60000 <= int(m.group(2)) <= 64000 and m.group(3) == "305000" and m.group(4) == "1000", f"progress: {mp[-1:]}")
        mc = [l for l in lines(log) if isinstance(l, str) and l.startswith("mc ")]
        check(mc and all(c in mc[-1] for c in ("play", "pause", "next", "repeat_all", "shuffle")), f"what the controller allows: {mc[-1:]}")
        # aiosendspin 10 offers volume (and seek) only where a player of the group declares them (digest E14)
        offered = mc[-1].split(" ")[3].split(",") if mc else []
        check(mc and mc[-1].split(" ")[-1] == "0", f"shuffle off, though \"shuffle\" is among the commands: {mc[-1:]}")
        ctl.set_shuffle(True)
        await until(lambda: [l for l in lines(log) if isinstance(l, str) and l.startswith("mc ")][-1].endswith(" 1"), 5)
        mc = [l for l in lines(log) if isinstance(l, str) and l.startswith("mc ")]
        check(mc[-1].endswith(" 1"), f"shuffle on: {mc[-1:]}")
        ctl.set_shuffle(False)
        await until(lambda: [l for l in lines(log) if isinstance(l, str) and l.startswith("mc ")][-1].endswith(" 0"), 5)

        meta.set_metadata(Metadata(title="Hunter", artist="Björk", album="Homogenic", track_progress=0, track_duration=255000, playback_speed=1000))
        await until(lambda: [l for l in lines(log) if isinstance(l, str) and l.startswith("mt ")][-1].startswith("mt Hunter"), 5)
        mt = [l for l in lines(log) if isinstance(l, str) and l.startswith("mt ")]
        check(mt[-1] == "mt Hunter\tBjörk\tHomogenic", f"only the title changed: artist and album kept: {mt[-1]!r}")
        meta.set_metadata(Metadata(title="Smile :-}]", artist="Björk", album="Homogenic", track_progress=0, track_duration=255000, playback_speed=1000))
        await until(lambda: [l for l in lines(log) if isinstance(l, str) and l.startswith("mt ")][-1].startswith("mt Smile"), 5)
        mt = [l for l in lines(log) if isinstance(l, str) and l.startswith("mt ")]
        check(mt[-1] == "mt Smile :-}]\tBjörk\tHomogenic", f"brackets in a title are text: {mt[-1]!r}")
        meta.set_metadata(Metadata(title="Hunter", artist="Björk", album="Homogenic", track_progress=0, track_duration=255000, playback_speed=1000))
        await until(lambda: [l for l in lines(log) if isinstance(l, str) and l.startswith("mt ")][-1].startswith("mt Hunter"), 5)

        if not LEGACY:                           # 1.0: scheduled updates
            # the next track's metadata, scheduled 3 s ahead (sent up to 20 s early): shown at its time; one scheduled and
            # then one for now: the one for now at once, the scheduled one never
            t0 = time.monotonic()
            meta.set_metadata(Metadata(title="Next", artist="Björk", album="Homogenic", track_progress=0, track_duration=180000, playback_speed=1000),
                              timestamp_us=server.clock.now_us() + 3_000_000)
            await asyncio.sleep(1)
            mt = [l for l in lines(log) if isinstance(l, str) and l.startswith("mt ")]
            check(mt[-1] == "mt Hunter\tBjörk\tHomogenic", f"scheduled metadata not before its time: {mt[-1]!r}")
            await until(lambda: [l for l in lines(log) if isinstance(l, str) and l.startswith("mt ")][-1].startswith("mt Next"), 5)
            dt = time.monotonic() - t0
            mp = [l for l in lines(log) if isinstance(l, str) and l.startswith("mp ")]
            check(2.5 <= dt <= 4.5 and mp and mp[-1].split()[3] == "180000", f"and at its time: {dt:.2f} s for 3 s, {mp[-1:]}")
            meta.set_metadata(Metadata(title="Later", artist="Björk", album="Homogenic", track_progress=0, track_duration=200000, playback_speed=1000),
                              timestamp_us=server.clock.now_us() + 2_000_000)
            await asyncio.sleep(0.5)
            meta.set_metadata(Metadata(title="Hunter", artist="Björk", album="Homogenic", track_progress=0, track_duration=255000, playback_speed=1000))
            await asyncio.sleep(3)
            mt = [l for l in lines(log) if isinstance(l, str) and l.startswith("mt ")]
            check(mt[-1] == "mt Hunter\tBjörk\tHomogenic" and not any(l.startswith("mt Later") for l in mt), f"metadata for now drops a scheduled one: {mt[-3:]}")

        # artwork: noise, so its JPEG is far over one Noise message (65519 bytes): fragments
        rnd = random.Random(1)
        img = Image.frombytes("RGB", (400, 400), bytes(rnd.getrandbits(8) for _ in range(400 * 400 * 3)))
        await client.group.group_role("artwork").set_album_artwork(img)
        await until(lambda: any(isinstance(l, tuple) and l[0] == "IMAGE" and l[1] for l in lines(log)), 8)
        imgs = [l[1] for l in lines(log) if isinstance(l, tuple) and l[0] == "IMAGE" and l[1]]
        how = "fragments (9.1.1)" if LEGACY else "parts (1.0's transfer)"
        check(imgs and imgs[-1][:2] == b"\xff\xd8" and len(imgs[-1]) > 65519, f"artwork in {how}, whole: {len(imgs[-1]) if imgs else 0} bytes of JPEG")
        if imgs:
            got_img = Image.open(io.BytesIO(imgs[-1]))
            check(got_img.size == (400, 400), f"a 400 px picture: {got_img.size}")

        # the UI's commands
        say("cmd next"); say("cmd volume 40"); say("cmd seek 90000"); say("cmd seek 500000"); say("cmd repeat_all"); say("cmd shuffle")
        await until(lambda: {"ControllerNextEvent", "ControllerRepeatEvent"} <= {type(e).__name__ for e in events}, 5)
        names = [type(e).__name__ for e in events]
        check("ControllerNextEvent" in names, f"next from the screen: {names}")
        vol = [e for e in events if type(e).__name__ == "ControllerVolumeEvent"]
        if "volume" in offered: check(vol and vol[-1].volume == 40, f"volume 40 from the screen: {[getattr(e, 'volume', None) for e in vol]}")
        else: check(not vol, f"volume not offered, none sent: {offered}")
        await until(lambda: len([e for e in events if type(e).__name__ == "ControllerSeekEvent"]) >= 2, 5)
        seek = [getattr(e, "position_ms", None) for e in events if type(e).__name__ == "ControllerSeekEvent"]
        check("seek" in offered and seek[:1] == [90000] and set(seek) == {90000, 120000},     # (a line say() wrote again: twice)
              f"seek to 1:30 from the screen, past how far the server seeks: that far: {seek} ({offered})")
        check(any("Repeat" in n for n in names) and any("Shuffle" in n for n in names), f"repeat and shuffle from the screen: {names}")

        # the face in the dark (issue #126) from the settings page: to the UI and into the export; Home Assistant's number
        # shows it and sets it (saved, on the page too), and follows the page while connected; the UI started again
        # below gets it
        wait_port(WEB)
        b = await asyncio.to_thread(Browser, WEB)
        check(await asyncio.to_thread(b.login_with_button, proc), "the page: logged in with the action button")
        face = lambda: next((x for x in (b.state() or {}).get("settings", []) if x["name"] == "face_min_brightness"), {})
        darks = lambda: [l for l in lines(log) if isinstance(l, str) and l.startswith("dark ")]
        s = await asyncio.to_thread(face)
        check(s.get("value") == 35 and (s.get("min"), s.get("max"), s.get("unit")) == (10, 100, "%") and s.get("export"),
              f"on the page: \"{s.get('label')}\", 35 % of 10..100, exported: {s}")
        r = await asyncio.to_thread(b.set, face_min_brightness=80)
        await until(lambda: darks()[-1:] == ["dark 80"], 5)
        check(darks()[-1:] == ["dark 80"], f"80 on the page: to the UI ({r})")
        r = await asyncio.to_thread(b.set, face_min_brightness=5)
        check(r.get("applied") == 0 and (await asyncio.to_thread(face)).get("value") == 80, f"5 on the page: refused, under the least ({r})")
        st, _, ex = await asyncio.to_thread(b.call, "GET", "/api/export")
        check(st == 200 and "face_min_brightness=80" in ex.decode().splitlines(), "and in the export")
        ha = APIClient("127.0.0.1", 16969, None); await ha.connect(login=True)
        ents, _ = await ha.list_entities_services(); hs = {}
        ha.subscribe_states(lambda x: hs.__setitem__(x.key, getattr(x, "state", None)))
        num = next((e for e in ents if e.object_id == "face_brightness_in_the_dark"), None)
        await until(lambda: num and hs.get(num.key) == 80, 5)
        check(num and (num.name, num.min_value, num.max_value, num.unit_of_measurement) == ("Face brightness in the dark", 10, 100, "%")
              and hs.get(num.key) == 80, f"Home Assistant: a number, at 80 ({num})")
        if num:
            ha.number_command(num.key, 20)
            await until(lambda: darks()[-1:] == ["dark 20"] and hs.get(num.key) == 20, 5)
            s = await asyncio.to_thread(face)
            saved = "face_min_brightness=20\n" in open(os.path.join(state, "config")).read()
            check(darks()[-1:] == ["dark 20"] and hs.get(num.key) == 20 and s.get("value") == 20 and saved,
                  f"20 from Home Assistant: to the UI, shown back, on the page, saved ({s.get('value')}, {saved})")
            ha.number_command(num.key, 5)
            await until(lambda: darks()[-1:] == ["dark 10"] and hs.get(num.key) == 10, 5)
            check(darks()[-1:] == ["dark 10"] and hs.get(num.key) == 10, f"5 from Home Assistant: 10, the least ({darks()[-1:]}, {hs.get(num.key)})")
        await asyncio.to_thread(b.set, face_min_brightness=80)
        await until(lambda: darks()[-1:] == ["dark 80"] and num and hs.get(num.key) == 80, 5)
        check(darks()[-1:] == ["dark 80"] and num and hs.get(num.key) == 80,
              f"80 on the page again: to the UI, and to Home Assistant while it is connected ({hs.get(num.key) if num else None})")
        await ha.disconnect()

        # the UI dies: started again, everything anew (a "home" from it is no command: the page picks the home app)
        say("home org.example.kiosk"); await asyncio.sleep(0.5)
        check(not os.path.exists(os.path.join(state, "screen_home")), "the UI does not pick the home app")
        open(log, "wb").close()
        subprocess.run(["pkill", "-f", me_ui])
        await until(lambda: any(isinstance(l, tuple) and l[0] == "IMAGE" for l in lines(log)), 8)
        got = lines(log)
        check(any(isinstance(l, str) and l.startswith("#start") for l in got), "the UI is started again")
        for want in ("home -", "mt Hunter\tBjörk\tHomogenic", "dark 80"):
            check(want in got, f"and gets {want!r} again")
        ago = [int(l.split()[2]) for l in got if isinstance(l, str) and l.startswith("p+ setup-mode ")]
        check(ago and ago[-1] >= 1000, f"and the setup pattern again, on for a while: {ago} ms")
        check(any(isinstance(l, tuple) and l[0] == "IMAGE" and l[1][:2] == b"\xff\xd8" for l in got), "and the artwork again")
        check(any(isinstance(l, tuple) and l[0] == "BAND" for l in got), "and the band again")

        if not LEGACY:                           # 1.0: scheduled updates
            # the next track's cover, scheduled 3 s ahead (the server sends it up to 20 s early): shown at its time
            open(log, "wb").close()
            img2 = Image.new("RGB", (400, 400), (200, 30, 30))
            t0 = time.monotonic()
            await client.group.group_role("artwork").set_album_artwork(img2, timestamp_us=server.clock.now_us() + 3_000_000)
            await until(lambda: any(isinstance(l, tuple) and l[0] == "IMAGE" and l[1] for l in lines(log)), 8)
            dt = time.monotonic() - t0
            check(2.5 <= dt <= 4.5, f"a scheduled cover at its time: {dt:.2f} s for 3 s")
            # one scheduled, then another for now: that one at once, the scheduled one never (the server cancels it)
            open(log, "wb").close()
            await client.group.group_role("artwork").set_album_artwork(img, timestamp_us=server.clock.now_us() + 3_000_000)
            await asyncio.sleep(0.5)
            await client.group.group_role("artwork").set_album_artwork(img2)
            await asyncio.sleep(4)
            shown = [len(l[1]) for l in lines(log) if isinstance(l, tuple) and l[0] == "IMAGE"]
            check(len(shown) == 1 and shown[0] < 65519, f"a cover for now cancels the scheduled one: {shown}")
            # one scheduled, then cancelled with nothing in its place: never shown
            await asyncio.sleep(1); open(log, "wb").close()
            await client.group.group_role("artwork").set_album_artwork(img2, timestamp_us=server.clock.now_us() + 3_000_000)
            await asyncio.sleep(0.5)
            await client.group.group_role("artwork").cancel_scheduled(ArtworkSource.ALBUM)
            await asyncio.sleep(4)
            shown = [len(l[1]) for l in lines(log) if isinstance(l, tuple) and l[0] == "IMAGE"]
            check(not shown, f"a scheduled cover cancelled, none in its place: never shown {shown}")
        open(log, "wb").close()
        await client.group.group_role("artwork").set_album_artwork(None)
        await until(lambda: ("IMAGE", b"") in lines(log), 5)
        check(("IMAGE", b"") in lines(log), "no cover any more: the old one cleared")
        meta.set_metadata(None)
        await until(lambda: "mt \t\t" in lines(log), 5)
        check("mt \t\t" in lines(log), "metadata cleared")

        # a UI alive that takes nothing (hung): stopped after its time (10 s; here 2: the setup spinner's band fills its
        # pipe, else its pings go unanswered: tests/fake_ma_api.py, an idle Echo) and started again, with what it missed
        say("#hang"); t0 = time.monotonic(); await asyncio.sleep(0.5)
        open(log, "ab").write(b"#mark\n")
        meta.set_metadata(Metadata(title="Isobel", artist="Björk", album="Post", track_progress=0, track_duration=200000, playback_speed=1000))
        await until(lambda: "#start" in "\n".join(l for l in lines(log)[lines(log).index("#mark"):] if isinstance(l, str)), 30)
        dt = time.monotonic() - t0                             # (from the hang: the pipe is full within a second of it)
        after = [l for l in lines(log)[lines(log).index("#mark"):] if isinstance(l, str)]
        check(any(l.startswith("#start") for l in after) and 2 <= dt <= 20, f"a hung UI: stopped and started again ({dt:.1f} s after it hung)")
        await until(lambda: "mt Isobel\tBjörk\tPost" in lines(log)[lines(log).index("#mark"):], 5)
        check("mt Isobel\tBjörk\tPost" in lines(log)[lines(log).index("#mark"):], "and the new one has what the hung one missed")

        # a UI that does not say "ready" in its time (stuck in its start): stopped and started again; its "ready" while it
        # is being stopped is not the next one's (slow to come up: never taken for up before its own, nor for hung)
        await asyncio.sleep(11)                                # (the last one up 10 s, not a minute: HASSMIC_SCREEN_STAYED_MS)
        open(os.path.join(state, "late"), "w").close(); open(os.path.join(state, "slow"), "w").close()
        open(log, "ab").write(b"#mark2\n")
        subprocess.run(["pkill", "-f", me_ui]); t0 = time.monotonic()
        def starts(): return [l for l in lines(log)[lines(log).index("#mark2"):] if isinstance(l, str) and l.startswith("#start")]
        def marked(): return [f for f in os.listdir(state) if f.startswith("screen-ui.")] == [f"screen-ui.{proc.pid}"]
        await until(lambda: len(starts()) >= 1, 10); dt = time.monotonic() - t0
        check(dt >= 0.9, f"a UI that ended within a minute of its start: the next one after a wait ({dt:.1f} s)")
        await asyncio.sleep(1)
        check(marked(), "one starting again: hassmic's pid for root's kiosk_tick all along (no app opened over what was chosen)")
        await until(lambda: len(starts()) >= 2, 30)          # (its 5 s, 2 to go, waits of 4 and 8 from the restarts above)
        dt = time.monotonic() - t0
        check(len(starts()) >= 2 and dt >= 5, f"a UI that never came up: stopped and started again ({len(starts())} starts, {dt:.1f} s)")
        await until(lambda: "mt Isobel\tBjörk\tPost" in lines(log)[lines(log).index("#mark2"):], 5)
        check("mt Isobel\tBjörk\tPost" in lines(log)[lines(log).index("#mark2"):], "and the next one has it all")
        check(marked(), "and, up, still")
        await asyncio.sleep(3)
        check(marked() and len(starts()) == 2, f"and it stays: not taken for hung while it came up ({len(starts())} starts)")

        # one slow to come up (3 s: past the 2 s a UI up may take nothing, within the 5 s it has to come up) with a cover
        # far over the pipe waiting for it: no hang, it stays
        await client.group.group_role("artwork").set_album_artwork(img)    # (the big cover: far over the pipe)
        await asyncio.sleep(21)                                # (up past HASSMIC_SCREEN_STAYED_MS: no wait before the next)
        open(os.path.join(state, "slow"), "w").close(); open(log, "ab").write(b"#mark3\n")
        subprocess.run(["pkill", "-f", me_ui])
        await until(lambda: any(isinstance(l, tuple) and l[0] == "IMAGE" and l[1] for l in lines(log)[lines(log).index("#mark3"):]), 15)
        await asyncio.sleep(3)
        s3 = [l for l in lines(log)[lines(log).index("#mark3"):] if isinstance(l, str) and l.startswith("#start")]
        check(len(s3) == 1 and any(isinstance(l, tuple) and l[0] == "IMAGE" and len(l[1]) > 65519 for l in lines(log)[lines(log).index("#mark3"):]),
              f"a UI slow to come up, a big cover waiting: not taken for hung ({len(s3)} starts)")
        open(log, "wb").close()
        await server.close()
        await until(lambda: "mx" in lines(log), 5)
        check("mx" in lines(log), "server gone: no Music Assistant")
        gave = lambda: any("given up" in l for l in glog)
        await until(gave, 20); await asyncio.sleep(2)
        ended = sum("the UI ended" in l or "the UI is gone" in l for l in glog); ups = sum("screen: up" in l for l in glog)
        band = [l.split("band: ", 1)[1] for l in glog if "band: " in l]
        check(gave() and ended == 8 and ups == 8 and any("ringbar.dex" in b for b in band) and not any(f.startswith("screen-ui.") for f in os.listdir(gstate)) and gmarked,
              f"a UI that never stays up: given up after 8 starts, the band its own renderer's (none on the PC), no pid for root ({ended} ends, {ups} up, {band[-1:]})")
        late = [t - gave_at[0] for t in mconns if gave_at and t > gave_at[0] + 1]
        check(gave_at and mconns and mconns[0] < gave_at[0] and not late,
              f"and no Music Assistant session for a UI that is not there: none after ({len(mconns)} before, {len(late)} after)")
        # what is kept for that UI stays while it is given up (issue #126): its face's brightness in the dark, listed in
        # Home Assistant and in the config that a save writes anew (mute from Home Assistant), for hassmic's next start
        gha = APIClient("127.0.0.1", 16973, None); await gha.connect(login=True)
        gents, _ = await gha.list_entities_services(); gby = {e.object_id: e for e in gents}; gs = {}
        gha.subscribe_states(lambda x: gs.__setitem__(x.key, getattr(x, "state", None)))
        gnum = gby.get("face_brightness_in_the_dark")
        await until(lambda: gnum and gs.get(gnum.key) == 80, 5)
        gha.switch_command(gby["mute"].key, True)
        gcfg = lambda: open(os.path.join(gstate, "config")).read().splitlines()
        await until(lambda: "mute=on" in gcfg(), 5)
        check(gnum and gs.get(gnum.key) == 80 and "mute=on" in gcfg() and "face_min_brightness=80" in gcfg(),
              f"and what is kept for it stays: the face's brightness in the dark, in Home Assistant and in a save after "
              f"({gs.get(gnum.key) if gnum else None}, {[l for l in gcfg() if l.startswith(('mute=', 'face_'))]})")
        await gha.disconnect()
        msrv.close()
        await until(lambda: any("given up" in l for l in hlog), 30)
        hung = sum("takes nothing (hung)" in l for l in hlog)
        check(any("given up" in l for l in hlog) and hung == 3 and not any(f.startswith("screen-ui.") for f in os.listdir(hstate)),
              f"a UI that comes up and hangs, three times: given up too, no pid for root ({hung} hung)")
    finally:
        proc.terminate(); gproc.terminate(); hproc.terminate(); await server.close()
        subprocess.run(["pkill", "-f", me_ui])
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

asyncio.run(main())
