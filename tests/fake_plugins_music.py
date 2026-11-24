#!/usr/bin/env python3
"""Phase 4 of docs/plugins.md on the PC: music, the volume, the Echo's state, UDP ports of a plugin's own, the search
for its server, proposals and Home Assistant's entities and actions, with a stand-in player
(tests/plugins/testmusic.c: a tone) under build/plugrun-host and build/hassmic-host, this script as the settings page,
as Home Assistant (keyed, as it is once adopted), as Music Assistant (Sendspin, aiosendspin), as a second music
source (main.c's stand-in, HASSMIC_FAKE_SOURCE: a phone over Bluetooth cannot play on a PC) and as the devices on the
network and outside it.

The other hosts are a network namespace behind a veth pair (tests/peerhost.py, made by tests/run.py for this test):
10.250.0.2 private, 198.51.100.7 outside the local network; the Echo's side is 10.250.0.1.  Run alone, the script
starts itself in such a namespace.

The install: a line per capability with its parameters (its ports, where its search goes, each entity and action by
its label), the button; the refusals of a manifest (wake with music, an action with arguments on the card, a search
to a unicast address, five ports, an entity's key this version does not read).  Music: the plugin's stream plays on
the Echo's plugin stream (audio_file.c's file, HASSMIC_PLUG_MUSIC) as stereo; MUSIC_POS reaches it and its "Position"
sensor follows in Home Assistant; it is ducked while a run listens and back after; the action button pauses and
resumes it, "<wake word>, stop" pauses it, Home Assistant's media player (pause and play offered while a music plugin
is listed) pauses, resumes and stops it, its state PLAYING/PAUSED/IDLE.  The newest source wins: the stand-in source
pauses it and it pauses the stand-in; Sendspin pauses it and it pauses Sendspin (the controller's pause); a second
music plugin takes the speaker from the first.  The volume: a plugin's VOLUME_SET is the Echo's volume (Home
Assistant's media player has it), Home Assistant's volume reaches the plugin.  Its state: listening, idle.  UDP: its
port reached from a private address and answered, not from a public one; its search answered by a device on the
network, the answer proposed for its server row, the card shows it, the owner's click writes it and the plugin starts
again.  Entities: listed with the manifest's names, states, a switch, a number, a select set and a button pressed from
Home Assistant, an event fired; an action called by Home Assistant with its arguments, the card's button for an action
marked for it.  Every refusal: the plugin's own tries (a public peer, the Echo's own address, port 5555, a public or
wrong-row proposal, a state out of range, an event type not listed, a second stream, a rate it does not take, a
volume flood, a port opened twice, searches faster than one a second), each answered and said.  Switched off: its
entities and the media player's pause gone from Home Assistant's list; removed: its ports file with it."""
import asyncio, base64, json, math, os, shutil, signal, socket, struct, subprocess, sys, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser
import fake_plugins as fp
from aioesphomeapi import (APIClient, ButtonInfo, Event, EventInfo, MediaPlayerCommand, MediaPlayerEntityState, MediaPlayerInfo, NumberInfo, NumberState,
                           SelectInfo, SelectState, SensorInfo, SensorState, SwitchInfo, SwitchState, TextSensorInfo, TextSensorState,
                           VoiceAssistantEventType as Ev)
from aioesphomeapi.model import UserService
from esphome_common import assistant, sync

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk"))
import plugcheck
import peerhost
check = fp.check
LAN, OUT, OWN = "10.250.0.2", "198.51.100.7", "10.250.0.1"
SS_PORT, SS_SERVER = 16993, 16994
DISCOVER = 16997
ZERO_NOISE_PSK = base64.b64encode(bytes(32)).decode()


def reachable():
    return bool(os.environ.get("HASSMIC_PEER_SOCK")) and os.path.exists(os.environ["HASSMIC_PEER_SOCK"])


def rms_db(raw):
    """the level of stereo s16 in dBFS"""
    n = len(raw) // 2
    if not n: return -200.0
    v = struct.unpack(f"<{n}h", raw[:n * 2])
    return 20 * math.log10(max(1e-9, math.sqrt(sum(x * x for x in v) / n) / 32768))


def tail(path, secs, rate=16000):
    """the last SECS of what plays (stereo s16 at RATE), the file as it grows at playback speed"""
    want = int(secs * rate) * 4
    with open(path, "rb") as f:
        f.seek(0, 2); size = f.tell(); f.seek(max(0, size - want) // 4 * 4)
        return f.read()


async def main():
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log")
    for k in ("author", "index"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    man = json.load(open(f"{ROOT}/tests/plugins/testmusic.json"))
    plug_raw, fake_src = os.path.join(state, "plug.raw"), os.path.join(state, "fake.src")

    def package(base=man, **changes):
        m = dict(base, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, "p.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/testmusic", os.path.join(top, "author.key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        plugcheck.list_package(out, os.path.join(top, "index.key"), os.path.join(top, "o.hmp"), arch="x86_64")
        return open(os.path.join(top, "o.hmp"), "rb").read()

    proc = fp.start(state, log, extra=("-z", str(SS_PORT)), env={"HASSMIC_PLUG_MUSIC": plug_raw, "HASSMIC_FAKE_SOURCE": fake_src,
                                                                  "HASSMIC_MUSIC": os.path.join(state, "music.raw"), "HASSMIC_OUTPUT_LATENCY_MS": "0",
                                                                  "HASSMIC_SETTINGS": os.path.join(state, "settings"),
                                                                  "HASSMIC_PLUGIN_LINES": "1000"})      # (it says a line a second: the test reads them all)
    open(os.path.join(state, "sendspin.unpaired"), "w").write("1")
    plog = lambda pid="testmusic": [l.split(f"plugin {pid}: ", 1)[1] for l in open(log, errors="replace").read().splitlines() if f"plugin {pid}: " in l]
    def says(text, n0=0, secs=10, pid="testmusic"): return fp.until(lambda: any(text in l for l in plog(pid)[n0:]), secs)
    # (the waits yield to asyncio: a wait that slept the thread held aioesphomeapi's reader, and Home Assistant's states
    # arrived only after it)
    async def until(pred, secs=8):
        end = time.monotonic() + secs
        while time.monotonic() < end:
            if pred(): return True
            await asyncio.sleep(0.05)
        return bool(pred())
    async def asays(text, n0=0, secs=10, pid="testmusic"): return await until(lambda: any(text in l for l in plog(pid)[n0:]), secs)
    async def alogged(lg, text, n0=0, secs=8): return await until(lambda: text in open(lg, errors="replace").read()[n0:], secs)
    ha = server = feeder = None
    try:
        wait_port(fp.PORT, fp.WEB)
        b = Browser(fp.WEB)
        check(b.login_with_button(proc), "logged in with the button")
        b.set(plugins="on")

        # ---- the install: a line per capability, its parameters filled in
        st, r = fp.upload(b, package())
        lines = {c["cap"]: c["line"] for c in r.get("caps", [])}
        check(st == 200 and r.get("button") is True, f"a new plugin waits for the button: {st} {r.get('error')}")
        check(lines.get("audio.music") == "Plays sound on this Echo's speaker, whenever it likes" and lines.get("audio.volume") == "Changes this Echo's volume"
              and lines.get("state") == "Sees when this Echo listens and speaks"
              and lines.get("net.udp") == "Can be reached from your network, and sends to devices on it (1 UDP port of its own)"
              and lines.get("net.discover") == "Searches your network for its server (UDP port 16997, to every device on it), and can be reached from your network on a UDP port of its own, where the answers come"
              and lines.get("ha.entities", "").startswith('Shows in Home Assistant: "Position" (a number), "Volume" (a number), "Assistant" (a text), "Loud" (a switch)')
              and lines.get("ha.actions") == 'Home Assistant can tell it to: "Search for the server" (also a button on its card), "Set the volume", "Try"',
              f"one line per capability, in the owner's words, with its parameters: {lines}")
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: (fp.plug(b, "testmusic") or {}).get("accepted")), "the press installs it")
        for caps, why in ((dict(man["capabilities"], wake=True), "way out"),
                          (dict(man["capabilities"], **{"ha.actions": [{"name": "go", "label": "Go", "page": True, "args": [{"name": "n", "type": "int"}]}]}), "without arguments"),
                          (dict(man["capabilities"], **{"net.discover": {"udp": 3483, "to": "10.250.0.2"}}), "multicast group"),
                          (dict(man["capabilities"], **{"net.udp": 5}), "1 to 4 ports"),
                          (dict(man["capabilities"], **{"ha.entities": [{"name": "a", "kind": "switch", "label": "A", "unit": "W"}]}), "a key this version does not read")):
            st, r = fp.upload(b, package(id="badmusic", capabilities=caps))
            check(st == 400 and why in r.get("error", ""), f"refused: {why}: {r.get('error')}")
            fp.post(b, "/api/plugins/cancel")

        # ---- it runs: its ports, its address, its stream
        check(await asays("music open: ok (0)", 0, 20), f"it runs and opens a stream: {plog()[-6:]}")
        check(await asays("udp open: 1 port 28940 (0)") and await asays("discover open: 1 port 28941 (0)"), f"its UDP port and its search's, the plugins' range: {plog()[:8]}")
        check(await asays(f"net {OWN}/255.255.255.0 up 1"), "NET: the Echo's address")
        check(await alogged(log, "audio: plugin stream 16000 Hz, 2 ch") and await until(lambda: os.path.exists(plug_raw) and os.path.getsize(plug_raw) > 64000, 10),
              "what it writes plays on the plugin stream, at its rate, as stereo")
        lvl = rms_db(tail(plug_raw, 0.5))
        check(-25 < lvl < -14, f"the tone as written (-12 dBFS peak, a sine's RMS 3 dB under), the volume's gain on top: {lvl:.1f} dBFS")
        ports = open(os.path.join(state, "plugins", "testmusic", "ports")).read().split()
        check(ports == ["28940", "28941"] and (fp.plug(b, "testmusic") or {}).get("ports") == [28940, 28941], f"its ports kept in its folder, on its card: {ports}")

        # ---- before Home Assistant set a key, any client on the network may connect and list: a plugin's switch and its
        # actions are not its to use (a review of efde36e: they were where the Echo had no key yet), as Drop In's
        un = APIClient("127.0.0.1", fp.PORT, None, noise_psk=ZERO_NOISE_PSK); await un.connect(login=True)
        uents, usvcs = await un.list_entities_services(); n0 = len(plog()); lg0 = len(open(log, errors="replace").read())
        uby = {e.object_id: e for e in uents}; usvc = {x.name: x for x in usvcs}
        if "p9_testmusic_loud" in uby: un.switch_command(uby["p9_testmusic_loud"].key, True)
        if "p9_testmusic_try" in usvc: await un.execute_service(usvc["p9_testmusic_try"], {"what": "close"})
        check("p9_testmusic_loud" in uby and await alogged(log, "plugin: a command refused, it did not come over the keyed connection", lg0)
              and await alogged(log, "plugin: an action refused, it did not come over the keyed connection", lg0) and not await asays("set loud", n0, 1) and not await asays("action try", n0, 0.2),
              "a client that is not Home Assistant's keyed link (no key set yet): neither a plugin's switch nor its action reaches it")
        await un.disconnect()
        check(fp.plugins(b).get("ha_key") == 0, "K4: the page hears that no key is set (the card's line: Home Assistant's commands not taken yet)")
        # ---- Home Assistant, keyed: its entities and actions, the media player
        key = base64.b64encode(os.urandom(32))
        prov = APIClient("127.0.0.1", fp.PORT, None, noise_psk=ZERO_NOISE_PSK); await prov.connect()
        await prov.noise_encryption_set_key(key); await prov.disconnect()
        states = []
        async def connect():
            for _ in range(50):
                try:
                    c = APIClient("127.0.0.1", fp.PORT, None, noise_psk=key.decode()); await c.connect(login=True); break
                except Exception: await asyncio.sleep(0.2)
            ents, svcs = await c.list_entities_services(); states.clear(); c.subscribe_states(states.append)
            va = assistant(c); await sync(c)
            return c, ents, svcs, va
        ha, ents, svcs, va = await connect()
        check(fp.plugins(b).get("ha_key") == 1, "K4: ... and once Home Assistant set it, that it is (the line gone)")
        by = {e.object_id: e for e in ents}
        last = lambda e, t: ([s for s in states if isinstance(s, t) and s.key == e.key] or [None])[-1]
        mp = next(e for e in ents if isinstance(e, MediaPlayerInfo))
        mps = lambda: (last(mp, MediaPlayerEntityState) or MediaPlayerEntityState()).state
        check(isinstance(by.get("p9_testmusic_position"), SensorInfo) and by["p9_testmusic_position"].name == "Plugin 9 testmusic: Position" and by["p9_testmusic_position"].unit_of_measurement == "s"
              and isinstance(by.get("p9_testmusic_assistant"), TextSensorInfo) and isinstance(by.get("p9_testmusic_loud"), SwitchInfo)
              and isinstance(by.get("p9_testmusic_level"), NumberInfo) and by["p9_testmusic_level"].max_value == 10
              and isinstance(by.get("p9_testmusic_mode"), SelectInfo) and list(by["p9_testmusic_mode"].options) == ["tone", "noise"]
              and isinstance(by.get("p9_testmusic_ping"), ButtonInfo) and isinstance(by.get("p9_testmusic_pressed"), EventInfo) and list(by["p9_testmusic_pressed"].event_types) == ["ping", "pong"],
              f"its entities in Home Assistant, named by the manifest (\"p<length of the id>_<plugin>_<name>\", \"Plugin <length of the id> <id>: <label>\": never an entity id of hassmic's own): {sorted(k for k in by if k.startswith('p'))}")
        check(len({e.key for e in ents}) == len(ents) and all(e.key >= 0x40000000 for e in ents if e.object_id.startswith("p9_")),
              "every key its own, the plugin's in a range above hassmic's own (given once and kept: state/plugins/.hakeys)")
        svc = {s.name: s for s in svcs}
        check("p9_testmusic_set_volume" in svc and [a.name for a in svc["p9_testmusic_set_volume"].args] == ["percent"] and "p9_testmusic_search" in svc,
              f"its actions, Home Assistant's esphome.<node>_p_<plugin>_<name>: {sorted(svc)}")
        check(mp.feature_flags & 1 and mp.feature_flags & 16384, f"a music plugin listed: the media player offers pause and play ({mp.feature_flags})")
        check(await until(lambda: mps() == 2, 5), f"and says playing: {mps()}")
        check(await until(lambda: (last(by["p9_testmusic_position"], SensorState) or SensorState()).state >= 2, 6), "MUSIC_POS reaches it: its position in Home Assistant counts up")
        check(await asays("state idle muted 0 volume"), "STATE: what the Echo does, after READY")

        # ---- its entities and actions from Home Assistant
        n0 = len(plog())
        ha.switch_command(by["p9_testmusic_loud"].key, True); ha.number_command(by["p9_testmusic_level"].key, 7); ha.select_command(by["p9_testmusic_mode"].key, "noise")
        ha.button_command(by["p9_testmusic_ping"].key)
        check(await asays("set loud = on", n0) and await asays("set level = 7", n0) and await asays("set mode = noise", n0) and await asays("set ping = ", n0), f"a switch, a number, a select, a button: ENTITY_SET: {plog()[n0:]}")
        check(await until(lambda: (last(by["p9_testmusic_loud"], SwitchState) or SwitchState()).state is True and (last(by["p9_testmusic_level"], NumberState) or NumberState()).state == 7
                       and (last(by["p9_testmusic_mode"], SelectState) or SelectState()).state == "noise"), "... and their states back from the plugin")
        check(await until(lambda: any(isinstance(s, Event) and s.key == by["p9_testmusic_pressed"].key and s.event_type == "ping" for s in states)), "its event entity fires")
        ha.number_command(by["p9_testmusic_level"].key, 12)
        await asyncio.sleep(0.5)
        check(not await asays("set level = 12", n0, 1), "a number past its limits: not passed on")

        # ---- the Echo's state, the duck
        before = rms_db(tail(plug_raw, 0.4)); n0 = len(plog())
        va.started.clear(); proc.send_signal(signal.SIGUSR1)
        await asyncio.wait_for(va.started.wait(), 5)
        check(await asays("state listening", n0), f"STATE: listening: {plog()[n0:][-3:]}")
        check(await until(lambda: (last(by["p9_testmusic_assistant"], TextSensorState) or TextSensorState()).state == "listening"), "(its text sensor says so in Home Assistant)")
        await asyncio.sleep(0.6)
        ducked = rms_db(tail(plug_raw, 0.3))
        check(before - ducked > 10, f"ducked while the voice assistant listens, as Sendspin's music: {before:.1f} -> {ducked:.1f} dBFS")
        ha.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None); ha.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(ha)
        check(await asays("state idle", n0, 10), "STATE: idle again")
        await asyncio.sleep(0.8)
        back = rms_db(tail(plug_raw, 0.3))
        check(abs(back - before) < 2, f"and back up after: {back:.1f} dBFS")

        # ---- the button, the stop word, Home Assistant's media player
        n0 = len(plog()); proc.send_signal(signal.SIGUSR2)
        check(await asays("music cmd 1 why 1", n0) and await asays("music state 2", n0), "the action button: MUSIC_CMD pause (the button), and it says paused")
        check(await until(lambda: mps() == 3), f"the media player: paused ({mps()})")
        g0 = os.path.getsize(plug_raw); await asyncio.sleep(0.6)
        check(os.path.getsize(plug_raw) - g0 < 8000, f"nothing more plays while it is paused (what was under way: {os.path.getsize(plug_raw) - g0} bytes)")
        n0 = len(plog()); proc.send_signal(signal.SIGUSR2)
        check(await asays("music cmd 2 why 1", n0) and await asays("music state 1", n0), "pressed again: resumed")
        n0 = len(plog())
        va.started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(va.started.wait(), 5)
        proc.send_signal(signal.SIGHUP)                 # its "stop" (Amazon's second keyword)
        check(await asays("music cmd 1 why 2", n0), f'"<wake word>, stop": paused (the stop word): {plog()[n0:][-3:]}')
        ha.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None); ha.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(ha)
        n0 = len(plog()); proc.send_signal(signal.SIGUSR2)
        check(await asays("music cmd 2 why 1", n0), "the button resumes what the stop paused")
        n0 = len(plog())
        ha.media_player_command(mp.key, command=MediaPlayerCommand.PAUSE)
        check(await asays("music cmd 1 why 4", n0) and await until(lambda: mps() == 3), "Home Assistant's pause")
        ha.media_player_command(mp.key, command=MediaPlayerCommand.PLAY)
        check(await asays("music cmd 2 why 4", n0) and await until(lambda: mps() == 2), "... its play")
        ha.media_player_command(mp.key, command=MediaPlayerCommand.STOP)
        check(await asays("music cmd 3 why 4", n0) and await until(lambda: mps() == 1), "... its stop: the media player idle")
        # (the stream's end: its thread closes the plugin stream, and nothing more is written; M4 of a review of efde36e:
        # the check looked for the stream's start, which the log had from the first)
        lg0 = len(open(log, errors="replace").read())
        check(await alogged(log, "audio: plugin stream closed", lg0 - 4000 if lg0 > 4000 else 0, 5), "... the plugin closes its stream: the plugin stream closed")
        g0 = os.path.getsize(plug_raw); await asyncio.sleep(0.6)
        check(os.path.getsize(plug_raw) == g0, f"... and nothing more written to it ({os.path.getsize(plug_raw) - g0} bytes)")

        # ---- the volume, both ways
        n0 = len(plog())
        await ha.execute_service(svc["p9_testmusic_set_volume"], {"percent": 33})
        check(await asays("action set_volume from 1 (1 args: percent=33)", n0) and await asays("volume set: 0", n0), "Home Assistant's action, its argument: the plugin sets the volume")
        check(await until(lambda: abs(((last(mp, MediaPlayerEntityState) or MediaPlayerEntityState()).volume or 0) - 0.33) < 0.01) and await asays("volume 33", n0),
              "the Echo's volume: Home Assistant's media player has it, VOLUME back to the plugin")
        ha.media_player_command(mp.key, volume=0.6)
        check(await asays("volume 60", n0) and await until(lambda: (last(by["p9_testmusic_volume"], SensorState) or SensorState()).state == 60), "Home Assistant's volume reaches the plugin")

        # ---- the newest source wins: the stand-in (a phone over Bluetooth's place), Sendspin, another plugin
        n0 = len(plog()); await ha.execute_service(svc["p9_testmusic_try"], {"what": "reopen"})
        check(await asays("try reopen: 0", n0) and await until(lambda: mps() == 2), "playing again")
        n0 = len(plog()); open(fake_src, "w").write("play\n")
        check(await alogged(log, "fake source: plays") and await asays("music cmd 1 why 3", n0), "another source starts: the plugin is told to pause (another source)")
        check(await until(lambda: mps() == 3), "(paused)")
        ha.media_player_command(mp.key, command=MediaPlayerCommand.PLAY)
        check(await until(lambda: open(fake_src).read().strip() == "paused") and await alogged(log, "fake source: paused"), "the plugin plays again: the other source is paused")
        # Sendspin, as Music Assistant streams to it
        from aiosendspin.noise import Identity, InMemoryServerPairingStore
        from aiosendspin.server import SendspinServer, AudioFormat, ClientAddedEvent
        from aiosendspin.models.types import MediaCommand
        import numpy as np
        loop = asyncio.get_running_loop()
        server = SendspinServer(loop, Identity.generate(), "fake-ma", pairing_store=InMemoryServerPairingStore(), allow_noncompliant_clients=False)
        added = asyncio.Queue(); server.add_event_listener(lambda _s, ev: isinstance(ev, ClientAddedEvent) and added.put_nowait(ev.client_id))
        await server.start_server(port=SS_SERVER, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{SS_PORT}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        cid = await asyncio.wait_for(added.get(), 15); client = server.get_client(cid); await server.trust_unpaired(cid)
        for _ in range(100):
            if client.role("player@v1") is not None and client.role("controller@v1") is not None: break
            await asyncio.sleep(0.05)
        ctl = []; client.group.add_event_listener(lambda g, e: ctl.append(type(e).__name__))
        client.group.group_role("controller").set_supported_commands([MediaCommand.PLAY, MediaCommand.PAUSE])
        fmt = AudioFormat(sample_rate=48000, bit_depth=16, channels=2)
        async def play():
            stream = client.group.start_stream(); phase = 0
            while True:
                k = np.arange(phase, phase + 4800); phase += 4800
                stream.prepare_audio(np.repeat((8000 * np.sin(2 * np.pi * 440 * k / 48000)).astype("<i2"), 2).tobytes(), fmt)
                await stream.commit_audio(); await stream.sleep_to_limit_buffer(500_000)
        n0 = len(plog()); feeder = asyncio.create_task(play())
        check(await asyncio.to_thread(fp.logged, log, "sendspin: stream start", 0, 15) and await asyncio.to_thread(says, "music cmd 1 why 3", n0, 10),
              "Sendspin starts: the plugin is told to pause")
        ha.media_player_command(mp.key, command=MediaPlayerCommand.PLAY)
        for _ in range(100):
            if "ControllerPauseEvent" in ctl: break
            await asyncio.sleep(0.1)
        check("ControllerPauseEvent" in ctl, f"the plugin plays again: Sendspin's group paused (the controller's pause), as for a phone: {ctl}")
        feeder.cancel(); feeder = None; await client.group.stop()

        # ---- UDP of its own: reached from the network, not from outside it
        lan = peerhost.peer_socket("udp", LAN, 0); out = peerhost.peer_socket("udp", OUT, 0)
        lan.settimeout(5); out.settimeout(1)
        n0 = len(plog())
        lan.sendto(b"hello", (OWN, 28940)); out.sendto(b"from outside", (OWN, 28940))
        try: echo = lan.recvfrom(100)[0]
        except socket.timeout: echo = b""
        check(await asays(f"udp from {LAN}: hello", n0) and echo == b"echo:hello", f"a datagram from a private address: heard, and answered (SENDTO): {echo}")
        await asyncio.sleep(1)
        check(not await asays("from outside", n0, 1), "one from outside the network: never heard (the socket's own filter; the firewall's rule on an Echo)")
        # the search, from its card's button; a device on the network answers it; the answer proposed for its server row
        dev = peerhost.peer_socket("udp", "0.0.0.0", DISCOVER); dev.settimeout(5)
        def answer():
            try:
                d, a = dev.recvfrom(100)
                if d == b"testmusic?": dev.sendto(b"here", a)
            except OSError: pass
        t = threading.Thread(target=answer, daemon=True); t.start()
        n0 = len(plog())
        st, r = fp.post(b, "/api/plugins/testmusic/action", "search")
        check(st == 200 and await asays("action search from 2", n0), f"the card's button for an action marked for it: {st} {r}")
        check(await asays(f"found {LAN}: here", n0) and await asays(f"propose {LAN}: 0", n0), f"its search answered by a device on the network, proposed: {plog()[n0:][-4:]}")
        p = fp.plug(b, "testmusic") or {}
        check(p.get("proposals") == [{"setting": "server", "value": LAN, "label": "Player server"}] and not [s for s in p.get("settings", []) if s["name"] == "server" and s.get("value")],
              f"its card shows it, nothing written yet: {p.get('proposals')}")
        st, r = fp.post(b, "/api/plugins/testmusic/propose", f"take server {LAN}")
        p = fp.plug(b, "testmusic") or {}
        check(st == 200 and [s.get("value") for s in p.get("settings", []) if s["name"] == "server"] == [LAN] and not p.get("proposals")
              and await alogged(log, "testmusic: the server you entered changed: started again"), f"the owner's click writes it, the plugin starts again: {st} {r}")
        check(await asays("music open: ok", n0, 20), "(and runs again)")

        # ---- every refusal, the plugin's own tries
        # (udp_local, propose_local: A3 of a review of efde36e, an address a "local" route gives the Echo itself)
        tries = (("udp_public", "-1"), ("udp_own", "-1"), ("udp_5555", "-1"), ("propose_public", "-1"), ("propose_row", "-1"),
                 ("open_twice", "-1 (4)"), ("open_bad", "-1 (3)"), ("udp_twice", "-1 (4)"), ("udp_local", "-1"), ("propose_local", "-1"))
        if not os.path.exists(plug_raw) or not await until(lambda: (fp.plug(b, "testmusic") or {}).get("music") == "playing", 2):
            await ha.execute_service(svc["p9_testmusic_try"], {"what": "reopen"})
        check(await until(lambda: (fp.plug(b, "testmusic") or {}).get("music") == "playing"), "(its stream open, playing: its card says so)")
        for what, want in tries:
            n0 = len(plog())
            await asyncio.sleep(1.1 if what.startswith("udp_") else 0)        # (a new UDP peer one a second: refused for where, not for how often)
            await ha.execute_service(svc["p9_testmusic_try"], {"what": what})
            check(await asays(f"try {what}: {want}", n0), f"{what}: refused: {[l for l in plog()[n0:] if l.startswith('try')]}")
        check(not await alogged(log, "proposes 10.251.0.9"), "A3: the Echo's own address by a local route: never proposed, never sent to")
        # H1 of a review of efde36e: text that is not UTF-8 in each form a lead byte alone passes (overlong, half a
        # surrogate pair, past U+10FFFF), each refused; Home Assistant's link stays, and the good state after them
        # arrives over it; a title of the kind cut on the card
        n0 = len(plog()); s0 = len(states); lg0 = len(open(log, errors="replace").read())
        await asyncio.sleep(10.5)                       # (refusals are said once in 10 s: this one's own line)
        await ha.execute_service(svc["p9_testmusic_try"], {"what": "badutf"})
        check(await asays("badutf sent 8", n0) and await alogged(log, "testmusic: said what its entity cannot take (text_sensor assistant): refused", lg0),
              f"H1: every text that is not UTF-8 refused, said in hassmic's log: {[l for l in plog()[n0:] if 'badutf' in l]}")
        check(await until(lambda: (last(by["p9_testmusic_assistant"], TextSensorState) or TextSensorState()).state == "fine ✓", 5)
              and all(x.state in ("idle", "listening", "thinking", "speaking", "fine ✓") for x in states[s0:] if isinstance(x, TextSensorState) and x.key == by["p9_testmusic_assistant"].key),
              "H1: ... none of them reached Home Assistant, its link holds, and the good one after them arrives over it")
        check((fp.plug(b, "testmusic") or {}).get("title") == "Tone ", f"H1: a title that is not UTF-8: cut where it stops being so ({(fp.plug(b, 'testmusic') or {}).get('title')!r})")
        # H6: an empty string is no field on the wire: it reaches the plugin as "", not as 0
        n0 = len(plog())
        await ha.execute_service(svc["p9_testmusic_try"], {"what": ""})
        check(await asays("action try from 1 (1 args: what=)", n0), f"H6: an empty string argument reaches the plugin as \"\": {[l for l in plog()[n0:] if 'action try' in l]}")
        check(await alogged(log, "testmusic: asked to send UDP where it may not"), "said in hassmic's log (once in 10 s, the rest counted)")
        n0 = len(plog()); await ha.execute_service(svc["p9_testmusic_try"], {"what": "volume_flood"})
        check(await until(lambda: any(l.startswith("try volume_flood: ") and 0 < int(l.split(": ")[1].split()[0]) < 20 for l in plog()[n0:])), "twenty volumes at once: some refused (five a second)")
        lv = (last(by["p9_testmusic_level"], NumberState) or NumberState()).state
        await ha.execute_service(svc["p9_testmusic_try"], {"what": "entity_bad"}); await ha.execute_service(svc["p9_testmusic_try"], {"what": "event_bad"})
        await asyncio.sleep(1)
        check((last(by["p9_testmusic_level"], NumberState) or NumberState()).state == lv and not any(isinstance(s, Event) and s.event_type == "nonsense" for s in states),
              "a state past its limits, an event type it does not list: not passed to Home Assistant")
        got = []
        def count():
            dev.settimeout(1.5)
            try:
                while True: got.append(dev.recvfrom(100)[0])
            except OSError: pass
        await asyncio.sleep(1.1); t = threading.Thread(target=count, daemon=True); t.start()
        await ha.execute_service(svc["p9_testmusic_try"], {"what": "broadcast_flood"})
        t.join(4)
        check(len(got) == 1, f"three searches at once: one goes (one a second): {got}")

        # ---- a second music plugin takes the speaker; its ports are its own
        opens0 = open(log, errors="replace").read().count("audio: plugin stream 16000 Hz, 2 ch"); n0 = len(plog())
        st, r = fp.upload(b, package(id="testmusic2", name="Test player 2")); fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: (fp.plug(b, "testmusic2") or {}).get("state") == "running", 20), "a second music plugin, running")
        check(await asays("music open: ok", 0, 20, "testmusic2") and await alogged(log, "testmusic: music: its stream ends, testmusic2 opened one"), "it opens a stream: the first one's ends (one plugin stream)")
        # (M3 of a review of efde36e: that the second one's is what plays now, not only that the first was told: the
        # plugin stream opened anew for it, growing with its audio, and the first one paused as for any newer source)
        check(await until(lambda: open(log, errors="replace").read().count("audio: plugin stream 16000 Hz, 2 ch") > opens0, 5) and await asays("music cmd 1 why 3", n0),
              "... the plugin stream opened anew, for the second one; the first one told another source plays")
        g0 = os.path.getsize(plug_raw); await asyncio.sleep(1.0)
        check(os.path.getsize(plug_raw) - g0 > 32000 and (fp.plug(b, "testmusic2") or {}).get("music") == "playing", f"... and what plays is the second one's ({os.path.getsize(plug_raw) - g0} bytes in a second)")
        check(open(os.path.join(state, "plugins", "testmusic2", "ports")).read().split() == ["28942", "28943"], "its ports: the next free ones, not the first plugin's")
        await ha.disconnect(); ha = None
        fp.post(b, "/api/plugins/testmusic2/remove", "remove")

        # ---- switched off: its entities and the media player's pause gone from the list; on again: back
        fp.post(b, "/api/plugins/testmusic/enable", "off")
        await asyncio.sleep(0.5)
        ha, ents, svcs, va = await connect()
        mp = next(e for e in ents if isinstance(e, MediaPlayerInfo))
        check(not [e for e in ents if e.object_id.startswith("p9_testmusic")] and not [s for s in svcs if s.name.startswith("p9_testmusic")] and not mp.feature_flags & 1,
              "switched off: its entities, its actions and the media player's pause are no longer listed")
        await ha.disconnect(); ha = None
        fp.post(b, "/api/plugins/testmusic/enable", "on"); await asyncio.sleep(0.5)
        ha, ents, svcs, va = await connect()
        check(len([e for e in ents if e.object_id.startswith("p9_testmusic_")]) == 8, "on again: listed again")
        # H4 of a review of efde36e: an update without ha.entities and ha.actions (less than was accepted: no button)
        # closes Home Assistant's links, and its entities are gone from the list read again
        closed = asyncio.Event()
        await ha.disconnect(); ha = None
        ha2 = APIClient("127.0.0.1", fp.PORT, None, noise_psk=key.decode())
        async def stopped(expected): closed.set()
        await ha2.connect(login=True, on_stop=stopped); await ha2.list_entities_services()
        less = {k: v for k, v in man["capabilities"].items() if k not in ("ha.entities", "ha.actions")}
        st, r = fp.upload(b, package(version=2, capabilities=less)); fp.post(b, "/api/plugins/install")
        if r.get("button"): proc.send_signal(signal.SIGUSR2)
        try: await asyncio.wait_for(closed.wait(), 10)
        except asyncio.TimeoutError: pass
        check(closed.is_set(), f"H4: an update that drops its entities: Home Assistant's link closed ({st} {r.get('error')})")
        try: await ha2.disconnect()
        except Exception: pass
        ha, ents, svcs, va = await connect()
        check(not [e for e in ents if e.object_id.startswith("p9_testmusic")] and not [x for x in svcs if x.name.startswith("p9_testmusic")], "H4: ... and read again, its entities and actions gone")
        await ha.disconnect(); ha = None

        # ---- removed: its ports with it
        st, r = fp.post(b, "/api/plugins/testmusic/remove", "remove")
        check(st == 200 and await until(lambda: not os.path.exists(os.path.join(state, "plugins", "testmusic"))), "removed, its ports file with it")
    finally:
        if feeder: feeder.cancel()
        if server:
            try: await server.close()
            except Exception: pass
        if ha:
            try: await ha.disconnect()
            except Exception: pass
        proc.terminate()
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
    if check.failed:
        print("---- log"); print(open(log, errors="replace").read()[-8000:])
        os.makedirs(os.path.join(ROOT, "build", "test-logs"), exist_ok=True)
        shutil.copy(log, os.path.join(ROOT, "build", "test-logs", "plugins-music.hassmic.log"))      # (all of it, for a look)
    shutil.rmtree(top, ignore_errors=True)              # (its own folder, by its name: nothing else in /tmp)
    print("plugins-music: " + ("FAILED" if check.failed else "all passed"))
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    if not reachable():
        if os.environ.get("HASSMIC_NETNS_DONE"): sys.exit("fake_plugins_music: no other hosts in this namespace (tests/run.py netns(peer=True))")
        import run
        os.environ["HASSMIC_NETNS_DONE"] = "1"
        os.execvp("unshare", run.netns([sys.executable, os.path.abspath(__file__)], peer=True))
    try: asyncio.run(main())
    finally:
        d = os.path.dirname(os.environ.get("HASSMIC_PEER_SOCK", "")) if reachable() else ""
        if d and os.path.basename(d).startswith("hassmic-peer."):
            for f in os.listdir(d): os.unlink(os.path.join(d, f))
            os.rmdir(d)
