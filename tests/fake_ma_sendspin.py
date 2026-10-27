#!/usr/bin/env python3
"""Plays Music Assistant's side of Sendspin against build/hassmic-host with the reference server library: dials the
player, approves it unpaired, streams a sine, sends volume and delay commands, and checks what reaches the player's
audio backend.  Then pairing, a second server and unpairing.  Beside it, at once, players of their own for a restart of
the server (the player asks for mDNS announcements until one is back, issue #28), for unpaired access off (the default),
for unpaired access switched on while the server waits, and for "<wake word>, stop" to music (issue #51, with Home Assistant's voice assistant over the ESPHome API beside the
server).

    tests/fake_ma_sendspin.py             aiosendspin 10.2.1 from .venv (Music Assistant 2.10.8): spec 1.0's wire
    tests/fake_ma_sendspin.py --legacy    aiosendspin 9.1.1 (Music Assistant up to 2.10.6): the wire before it, from
                                          build/aiosendspin-9.1.1, which --setup makes (tests/run.py does)

The server is strict either way (allow_noncompliant_clients=False): a deviation the library would only log in Music
Assistant (issue #64) closes the connection here."""
import re, asyncio, contextvars, inspect, logging, math, os, signal, struct, subprocess, sys, tempfile, time, traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB9 = os.path.join(ROOT, "build", "aiosendspin-9.1.1")
# the wheel uv.lock pinned before 10.2.1; pure Python with the same requirements as 10.2.1, so it runs on .venv's
LIB9_REQ = "aiosendspin==9.1.1 --hash=sha256:cb7227f70a8c6cfea450fb92ad26ed6fe3d86e883620700e2cde9372ad8c995b"
LEGACY = "--legacy" in sys.argv


def setup():
    if os.path.isdir(os.path.join(LIB9, "aiosendspin-9.1.1.dist-info")): return
    import shutil
    uv = shutil.which("uv") or sys.exit("--setup needs uv (https://docs.astral.sh/uv/)")
    with tempfile.NamedTemporaryFile("w", suffix=".txt") as req:
        req.write(LIB9_REQ + "\n"); req.flush()
        subprocess.run([uv, "pip", "install", "--python", sys.executable, "--no-deps", "--require-hashes",
                        "--target", LIB9, "-r", req.name], check=True)

if "--setup" in sys.argv: setup(); sys.exit(0)
if LEGACY:
    if not os.path.isdir(os.path.join(LIB9, "aiosendspin")): sys.exit(f"no aiosendspin 9.1.1 in {LIB9}: run {sys.argv[0]} --setup")
    sys.path.insert(0, LIB9)

import numpy as np
import aiosendspin.noise as noise
from aiosendspin.noise import Identity, InMemoryServerPairingStore
from aiosendspin.noise.pairing import PairingAttempt
from aiosendspin.models.types import PairMethod, MediaCommand
from aiosendspin.server import SendspinServer, AudioFormat, ClientAddedEvent
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev

sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import wait_port, Browser
from esphome_common import assistant, sync

# What changed in the library's API between the two versions
decode_token = getattr(noise, "decode_psk_token", None) or noise.decode_token
def server_of(loop, ident, name, store):
    return SendspinServer(loop, ident, name, pairing_store=store, allow_noncompliant_clients=False)
def psk_attempt(tok):
    """pairing with the token; 10.x also wants the client_id it names"""
    kw = {"client_id": tok.client_id} if "client_id" in inspect.signature(PairingAttempt).parameters else {}
    return PairingAttempt(method=PairMethod.PAIRING_PSK, pairing_psk=tok.pairing_psk, **kw)
def delay_of(p): return p.output_delay_ms if hasattr(p, "output_delay_ms") else p.static_delay_ms
def set_delay(p, ms): (p.set_output_delay if hasattr(p, "set_output_delay") else p.set_static_delay)(ms)

PORT, PORT2, PORT3, PORT4, PORT5 = 16958, 16960, 16963, 16965, 16968      # the five players' Sendspin ports


# what a part checks, kept until the end and printed part by part: the parts run at once
out = contextvars.ContextVar("out", default=None)
def check(cond, what):
    line = ("ok   " if cond else "FAIL ") + what
    if out.get() is None: print(line)
    else: out.get().append(line)
    if not cond: check.failed = True
check.failed = False


async def until(cond, secs=5.0):
    """cond() polled until true or secs passed; what it last returned.  cond may be a coroutine function"""
    t0 = time.monotonic()
    while True:
        r = cond()
        if asyncio.iscoroutine(r): r = await r
        if r or time.monotonic() - t0 >= secs: return r
        await asyncio.sleep(0.05)


def player(state, port, http, env, web=0):
    """a hassmic-host with Sendspin on port, its log in state/log; no push updates or arbitration, and no settings page
    unless web: several run at once"""
    log = open(os.path.join(state, "log"), "w+")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-p", str(http), "-z", str(port), "-L", "-o", "0", "-a", "0", "-W", str(web)],
                            env=env, stderr=log)
    wait_port(port, http, *([web] if web else []))
    def text(): log.seek(0); return log.read()
    return proc, text


async def paired(env, state):
    music = env["HASSMIC_MUSIC"]
    open(os.path.join(state, "sendspin.unpaired"), "w").write("1")     # unpaired access switched on for this part
    mreq = os.path.join(state, "mdns")                  # root's cue to restart avahi (main.sh netwatch)
    proc, log = player(state, PORT, 16959, env)
    await asyncio.sleep(0.5)
    loop = asyncio.get_running_loop()
    ident, store = Identity.generate(), InMemoryServerPairingStore()     # what Music Assistant keeps across its restarts
    server = server_of(loop, ident, "fake-ma", store)
    added = asyncio.Queue(); events = []
    def on_event(_srv, ev):
        if isinstance(ev, ClientAddedEvent): added.put_nowait(ev.client_id)
    server.add_event_listener(on_event)
    try:
        await server.start_server(port=16957, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{PORT}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        cid = await asyncio.wait_for(added.get(), 15)
        client = server.get_client(cid)
        check(re.fullmatch(r"Echo Dot 3( [0-9a-f]{6})?", client.info.name or "") and "player@v1" in client.info.supported_roles, f"hello over the encrypted session: {client.info.name!r} {client.info.supported_roles}")
        check(open(os.path.join(state, "sendspin.key"), "rb").read().__len__() == 32, "identity key persisted")
        dialect = "aiosendspin 9 dialect" if LEGACY else "spec 1.0"
        check(await until(lambda: f"encrypted session up (sentinel key, {dialect})" in log()),     # through the log reader
              f"the player speaks the server's dialect: {dialect}")
        client.add_event_listener(lambda c, e: events.append(type(e).__name__))
        check(client.role("player@v1") is None, "no role before the operator approves the unpaired device")
        await server.trust_unpaired(cid)
        await until(lambda: client.role("player@v1") is not None and client.role("controller@v1") is not None)
        player_ = client.role("player@v1")
        check(player_ is not None and client.role("controller@v1") is not None, "player and controller roles active after approval")
        await until(lambda: player_.required_lead_time_ms == 300)
        check((player_.required_lead_time_ms, player_.min_buffer_ms, delay_of(player_)) == (300, 300, 0), f"initial client/state received: lead {player_.required_lead_time_ms}, buffer {player_.min_buffer_ms}, delay {delay_of(player_)}")

        fmt = AudioFormat(sample_rate=48000, bit_depth=16, channels=2)
        ctl_events = []
        client.group.add_event_listener(lambda g, e: ctl_events.append(type(e).__name__))
        client.group.group_role("controller").set_supported_commands([MediaCommand.PLAY, MediaCommand.PAUSE, MediaCommand.NEXT])
        stream = client.group.start_stream()
        n, phase = 4800, 0
        # 4 s of 440 Hz, then 0.3 s of silence: the server's FLAC encoder holds back what does not fill a block of 4608
        # samples until the stream ends, which was the tone's last 64 ms (played 3.936 s of 4, never "out")
        for i in range(43):
            k = np.arange(phase, phase + n); s = (8000 * np.sin(2 * np.pi * 440 * k / 48000) * (i < 40)).astype("<i2")
            phase += n
            stream.prepare_audio(np.repeat(s, 2).tobytes(), fmt)
            await stream.commit_audio()
            await stream.sleep_to_limit_buffer(1_000_000)
            if i == 15: player_.set_volume(30)
            if i == 25: proc.send_signal(signal.SIGUSR2)      # action button while music plays
        # the last second sent is still queued in the player: played out once the file holds silence after the tone
        # (audio_file.c writes at real time)
        def tone_out():
            pcm = np.frombuffer(open(music, "rb").read(), dtype="<i2")
            nz = np.flatnonzero(np.abs(pcm) > 100)
            return len(nz) > 0 and len(pcm) - nz[-1] > 2 * 4800
        await until(lambda: "ControllerPauseEvent" in ctl_events and player_.volume == 30 and tone_out())
        check("ControllerPauseEvent" in ctl_events, f"action button during playback sends controller pause: {ctl_events}")
        check(player_.volume == 30, f"volume command applied and echoed in client/state: {player_.volume}")
        await client.group.stop()
        await until(lambda: "sendspin: stream end" in log())
        check(not os.path.exists(mreq), "a server connected all along: no mDNS announcements")
        set_delay(player_, 120)                         # after the tone: the new delay resyncs, a jump in it
        saved = lambda: open(os.path.join(state, "sendspin.delay")).read().strip() if os.path.exists(os.path.join(state, "sendspin.delay")) else ""
        await until(lambda: delay_of(player_) == 120 and saved() == "120")
        check(delay_of(player_) == 120 and saved() == "120", f"delay command applied, kept and echoed in client/state: {delay_of(player_)} ms, saved {saved()!r}")

        pcm = np.frombuffer(open(music, "rb").read(), dtype="<i2").reshape(-1, 2)[:, 0].astype(float)
        nz = np.flatnonzero(np.abs(pcm) > 100)
        tone = pcm[nz[0]:nz[-1] + 1] if len(nz) else pcm[:0]
        dur = len(tone) / 48000
        check(3.9 <= dur <= 4.05, f"played {dur:.3f} s of tone (sent 4.000 s) as {env['HASSMIC_SENDSPIN_CODECS']}")
        # continuity: a dropped or repeated frame shows as a phase jump of the 440 Hz sine
        if len(tone) > 48000:
            ph = np.unwrap(np.angle(np.fft.ifft(np.fft.fft(tone) * (np.fft.fftfreq(len(tone)) > 0) * 2)))     # analytic signal
            jumps = np.abs(np.diff(ph) - 2 * np.pi * 440 / 48000)
            check(jumps[2000:-2000].max() < 0.3, f"tone is continuous (max phase step error {jumps[2000:-2000].max():.3f} rad)")
        lead = nz[0] / 48000 if len(nz) else -1
        check(0 <= lead < 2.0, f"silence padded before the scheduled start: {lead:.3f} s")

        # ---- pairing with the token, re-handshake to the long-term key, reconnect paired, unpair
        token = subprocess.run([f"{ROOT}/build/hassmic-host", "-T"], env=env, capture_output=True, text=True).stdout.strip()
        tok = decode_token(token)
        check(token.startswith("SP:0") and tok.client_id == cid, f"pairing token decodes to this player: {token[:14]}…")
        await server.initiate_pairing(cid, psk_attempt(tok))
        records = os.path.join(state, "sendspin.records")
        async def done():
            return (len(await server.pairing_store.list_records()) == 1 and os.path.exists(records) and os.path.getsize(records) > 0
                    and "long" in str(server.get_client(cid).connection_security.psk_category).lower())
        await until(done)
        recs = await server.pairing_store.list_records()
        sec = server.get_client(cid).connection_security
        check(len(recs) == 1 and os.path.getsize(records) > 0, "pairing finalised: record on both sides")
        check("long" in str(sec.psk_category).lower(), f"session re-handshaken to the long-term key: {sec.psk_category}")
        await until(lambda: "sendspin: player role active (paired)" in log() and server.get_client(cid).role("player@v1") is not None)
        check(server.get_client(cid).role("player@v1") is not None, "player role active on the paired session")

        # a second server dials while the first one is idle: it never played here, so it is turned away
        other = server_of(loop, Identity.generate(), "intruder", InMemoryServerPairingStore())
        await other.start_server(port=16956, discover_clients=False)
        other.connect_to_client(f"ws://127.0.0.1:{PORT}/sendspin", retry_initial_connection=False, retry_indefinitely=False)
        await until(lambda: "second server turned away" in log())
        await until(lambda: (oc := other.get_client(cid)) is None or not oc.is_connected, 3)
        oc = other.get_client(cid)
        check("second server turned away" in log() and (oc is None or not oc.is_connected),
              "idle second server is turned away (concurrent_attempt)")
        check(server.get_client(cid).is_connected, "first server keeps its connection")
        await other.close()

        await server.unpair(cid)
        await until(lambda: os.path.getsize(records) == 0)
        check(os.path.getsize(records) == 0, "server/unpair deletes the record on the player")
    finally:
        proc.terminate(); await server.close()


async def server_restart(env, state):
    # Music Assistant restarting (issue #28): its session ends, and the new instance only browses mDNS, which may not
    # reach the Echo.  While no server that played here is connected, root is asked to announce again, at growing
    # intervals (HASSMIC_ANNOUNCE_TIMES: 2 s, then 4; 30 s, then doubling, on the Echo); a server back stops it.
    open(os.path.join(state, "sendspin.unpaired"), "w").write("1")
    mreq = os.path.join(state, "mdns")
    proc, log = player(state, PORT3, 16961, env)
    await asyncio.sleep(0.5)
    loop = asyncio.get_running_loop()
    ident, store = Identity.generate(), InMemoryServerPairingStore()     # what Music Assistant keeps across its restarts
    added = asyncio.Queue()
    def on_event(_srv, ev):
        if isinstance(ev, ClientAddedEvent): added.put_nowait(ev.client_id)
    server = server_of(loop, ident, "fake-ma", store); server.add_event_listener(on_event)
    try:
        await server.start_server(port=16962, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{PORT3}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        cid = await asyncio.wait_for(added.get(), 15)
        await server.trust_unpaired(cid)                # paired, as the part above does it: the server comes back to it
        tok = decode_token(subprocess.run([f"{ROOT}/build/hassmic-host", "-T"], env=env, capture_output=True, text=True).stdout.strip())
        await server.initiate_pairing(cid, psk_attempt(tok))
        await until(lambda: "sendspin: player role active (paired)" in log(), 10)
        check("sendspin: player role active (paired)" in log() and not os.path.exists(mreq),
              "another player, paired with its server, which holds its role: no mDNS announcements")
        await server.close()
        await until(lambda: os.path.exists(mreq), 6)
        why = open(mreq).read().strip() if os.path.exists(mreq) else "nothing"
        check(why.startswith("no Sendspin server for "), f"server gone: root asked to announce mDNS again ({why})")
        if os.path.exists(mreq): os.remove(mreq)
        t0 = time.monotonic(); await until(lambda: os.path.exists(mreq), 8); dt = time.monotonic() - t0
        check(os.path.exists(mreq) and dt > 2.5, f"still gone: announced again, after a longer wait ({dt:.1f} s)")
        server = server_of(loop, ident, "fake-ma", store)
        added = asyncio.Queue(); server.add_event_listener(on_event)
        await server.start_server(port=16962, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{PORT3}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        check(await asyncio.wait_for(added.get(), 15) == cid, "the restarted server finds the player again")
        await until(lambda: server.get_client(cid).role("player@v1") is not None)
        check(server.get_client(cid).role("player@v1") is not None, "and it plays again, paired")
        if os.path.exists(mreq): os.remove(mreq)
        await asyncio.sleep(5)                          # past the 4 s the next announcement would have taken
        check(not os.path.exists(mreq), "server back: no more announcements")
    finally:
        proc.terminate(); await server.close()


async def unpaired_off(env, state):
    # ---- unpaired access off, the default (the Home Assistant switch, kept in state/sendspin.unpaired): approving the device
    # unpaired gives it no role; pairing with the token still works and then it plays
    proc, _ = player(state, PORT2, 16954, env)       # no sendspin.unpaired file: the default
    await asyncio.sleep(0.5)
    server = server_of(asyncio.get_running_loop(), Identity.generate(), "fake-ma", InMemoryServerPairingStore())
    added = asyncio.Queue()
    server.add_event_listener(lambda _s, ev: isinstance(ev, ClientAddedEvent) and added.put_nowait(ev.client_id))
    try:
        await server.start_server(port=16955, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{PORT2}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        cid = await asyncio.wait_for(added.get(), 15)
        await server.trust_unpaired(cid)
        await asyncio.sleep(2)                          # the server activates nothing it saw refused in the hello: no event
        check(server.get_client(cid).role("player@v1") is None,
              "unpaired access off: approved unpaired, still no player role")
        tok = decode_token(subprocess.run([f"{ROOT}/build/hassmic-host", "-T"], env=env, capture_output=True, text=True).stdout.strip())
        await server.initiate_pairing(cid, psk_attempt(tok))
        await until(lambda: (c := server.get_client(cid)) is not None and c.role("player@v1") is not None, 10)
        check(server.get_client(cid).role("player@v1") is not None, "unpaired access off: plays once paired with the token")
    finally:
        proc.terminate(); await server.close()


async def unpaired_switch(env, state):
    # ---- unpaired access switched on (on the settings page) while the server waits for a pairing token: the server read
    # the refusal in our hello and does not ask again, so the idle session is closed; on its redial it gets the roles
    proc, log = player(state, 16950, 16951, env, web=16953)     # unpaired access off: the default
    page = Browser(16953)
    check(await asyncio.to_thread(page.login_with_button, proc), "settings page logged in")
    server = server_of(asyncio.get_running_loop(), Identity.generate(), "fake-ma", InMemoryServerPairingStore())
    added = asyncio.Queue()
    server.add_event_listener(lambda _s, ev: isinstance(ev, ClientAddedEvent) and added.put_nowait(ev.client_id))
    try:
        await server.start_server(port=16952, discover_clients=False)
        server.connect_to_client("ws://127.0.0.1:16950/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        cid = await asyncio.wait_for(added.get(), 15)
        await server.trust_unpaired(cid)
        await asyncio.sleep(2)
        check(server.get_client(cid).role("player@v1") is None, "unpaired access off: the server waits, approved unpaired, no role")
        r = await asyncio.to_thread(page.set, sendspin_unpaired="on")
        check(r.get("applied") == 1 and await until(lambda: "sendspin: session closed" in log(), 5),
              f"switched on from the page: the waiting session is closed ({r})")
        check(await until(lambda: (c := server.get_client(cid)) is not None and c.role("player@v1") is not None, 15),
              "the server dials back and plays, unpaired")
    finally:
        proc.terminate(); await server.close()


async def stop_music(env, state, quiet=False):
    """"<wake word>, stop" pauses Sendspin music itself (issue #51): its pipeline goes on, a bare "stop" transcript is
    cancelled, the button resumes; without the keyword (microWakeWord) the transcript "stop" pauses.  That with the
    setting stop_ends_listening off (the default); on (quiet) the same stop also ends the run quietly (main.c stop_word)"""
    open(os.path.join(state, "sendspin.unpaired"), "w").write("1")
    if quiet: open(os.path.join(state, "config"), "w").write("stop_ends_listening=on\n")
    port, http, srv = (PORT5, 16969, 16970) if quiet else (PORT4, 16966, 16964)
    proc, log = player(state, port, http, env)
    loop = asyncio.get_running_loop()
    cli = APIClient("127.0.0.1", http, None)
    await cli.connect(login=True); await cli.list_entities_services()
    va = assistant(cli); await sync(cli)
    server = server_of(loop, Identity.generate(), "fake-ma", InMemoryServerPairingStore())
    added = asyncio.Queue()
    server.add_event_listener(lambda _srv, ev: isinstance(ev, ClientAddedEvent) and added.put_nowait(ev.client_id))
    feeder = None
    try:
        await server.start_server(port=srv, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{port}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        cid = await asyncio.wait_for(added.get(), 15)
        client = server.get_client(cid)
        await server.trust_unpaired(cid)
        await until(lambda: client.role("player@v1") is not None and client.role("controller@v1") is not None)
        ctl = []
        client.group.add_event_listener(lambda g, e: ctl.append(type(e).__name__))
        client.group.group_role("controller").set_supported_commands([MediaCommand.PLAY, MediaCommand.PAUSE])
        fmt = AudioFormat(sample_rate=48000, bit_depth=16, channels=2)

        async def play():                       # 440 Hz until cancelled, at real time
            stream = client.group.start_stream(); phase = 0
            while True:
                k = np.arange(phase, phase + 4800); phase += 4800
                stream.prepare_audio(np.repeat((8000 * np.sin(2 * np.pi * 440 * k / 48000)).astype("<i2"), 2).tobytes(), fmt)
                await stream.commit_audio(); await stream.sleep_to_limit_buffer(500_000)
        async def pause():                      # what Music Assistant does on the controller's pause
            nonlocal feeder
            feeder.cancel(); feeder = None
            n = log().count("sendspin: stream end")
            await client.group.stop(); await until(lambda: log().count("sendspin: stream end") > n)

        feeder = asyncio.create_task(play())
        await until(lambda: "sendspin: stream start" in log())
        va.started.clear(); proc.send_signal(signal.SIGUSR1)          # the wake word over the music
        await asyncio.wait_for(va.started.wait(), 5)
        proc.send_signal(signal.SIGHUP)                               # its "stop"
        await until(lambda: "ControllerPauseEvent" in ctl)
        await asyncio.sleep(0.5)
        if quiet:
            va.started.clear(); await asyncio.sleep(0.5)
            check("ControllerPauseEvent" in ctl and va.stopped == [True] and "stop: listening ended" in log() and not va.started.is_set(),
                  f'stop_ends_listening on: "<wake word>, stop" pauses the music and ends its run quietly: {ctl}, stops {va.stopped}')
            return
        check("ControllerPauseEvent" in ctl and not va.stopped, f'"<wake word>, stop" pauses the music, its pipeline goes on: {ctl}, stops {va.stopped}')
        await pause()
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None); cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "Stop."})
        await until(lambda: va.stopped and "said stop: music paused, run cancelled" in log())
        check(bool(va.stopped) and "said stop: music paused, run cancelled" in log(), f'the transcript "Stop." after it is cancelled, not answered: stops {va.stopped}')
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(cli)

        n = len(ctl); proc.send_signal(signal.SIGUSR2)               # the action button
        await until(lambda: "ControllerPlayEvent" in ctl[n:])
        check("ControllerPlayEvent" in ctl[n:], f"the action button resumes what the stop paused: {ctl[n:]}")

        # "<wake word>, stop the timer": the keyword pauses the music, the rest is Home Assistant's
        n = log().count("sendspin: stream start"); feeder = asyncio.create_task(play())
        await until(lambda: log().count("sendspin: stream start") > n)
        n = len(ctl)
        va.started.clear(); va.stopped.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(va.started.wait(), 5)
        proc.send_signal(signal.SIGHUP)
        await until(lambda: "ControllerPauseEvent" in ctl[n:])
        await pause()
        said = log().count("said stop")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None); cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "stop the timer"})
        await sync(cli); await asyncio.sleep(0.5)
        check("ControllerPauseEvent" in ctl[n:] and not va.stopped and log().count("said stop") == said,
              f'"<wake word>, stop the timer" over music: paused, the transcript goes to Home Assistant: {ctl[n:]}, stops {va.stopped}')
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(cli)

        # an engine without the "stop" keyword (microWakeWord): the transcript "stop" pauses and is cancelled
        n = log().count("sendspin: stream start"); feeder = asyncio.create_task(play())
        await until(lambda: log().count("sendspin: stream start") > n)
        n = len(ctl)
        va.started.clear(); va.stopped.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(va.started.wait(), 5)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None); cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "Stop!"})
        await until(lambda: va.stopped and "ControllerPauseEvent" in ctl[n:])
        check(bool(va.stopped) and "ControllerPauseEvent" in ctl[n:], f'without the keyword, the transcript "Stop!" pauses the music and is cancelled: {ctl[n:]}, stops {va.stopped}')
    finally:
        if feeder: feeder.cancel()
        await cli.disconnect(); proc.terminate(); await server.close()


async def main():
    if "-v" in sys.argv: logging.basicConfig(level=logging.DEBUG)
    want = os.environ.get("CODEC", "flac")         # CODEC=flac|opus|pcm: what the player lists first
    async def part(f):
        lines = []; out.set(lines)
        state = tempfile.mkdtemp()
        env = dict(os.environ, HASSMIC_SENDSPIN_CODECS=want, HASSMIC_STATE=state, HASSMIC_MUSIC=os.path.join(state, "music.raw"),
                   HASSMIC_SETTINGS=os.path.join(state, "settings"), HASSMIC_OUTPUT_LATENCY_MS="0",
                   HASSMIC_ANNOUNCE_TIMES="2 4",    # mDNS again after 2 s without a server that played here (30 s), then 4
                   HASSMIC_FAKE_ADDRS=os.path.join(state, "addrs"))
        # the Echo's addresses: netaddr.c announces only while it has one besides loopback, which tests/run.py's network
        # namespace does not have
        with open(env["HASSMIC_FAKE_ADDRS"], "w") as a: a.write("127.0.0.1\n")
        try: await f(env, state)
        except Exception:
            lines.append("FAIL " + traceback.format_exc().rstrip().replace("\n", "\n     ")); check.failed = True
        if any(l.startswith("FAIL") for l in lines) and os.path.exists(os.path.join(state, "log")):
            tail = open(os.path.join(state, "log"), errors="replace").read().splitlines()[-40:]
            lines.append("     the player's log, last lines:\n" + "\n".join("       " + l for l in tail))
        return lines
    for lines in await asyncio.gather(part(paired), part(server_restart), part(unpaired_off), part(unpaired_switch), part(stop_music),
                                         part(lambda env, state: stop_music(env, state, quiet=True))):
        print("\n".join(lines))
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

asyncio.run(main())
