#!/usr/bin/env python3
"""Plays Music Assistant's side of Sendspin against build/hassmic-host with the reference server library
(aiosendspin 9.1.1, the version in Music Assistant 2.10.4): dials the player, approves it unpaired, streams a sine,
sends a volume command, and checks what reaches the player's audio backend.  Then pairing, a second server and unpairing.  Beside it, at
once, players of their own for a restart of the server (the player asks for mDNS announcements until one is back,
issue #28) and for unpaired access off (the default)."""
import re, asyncio, contextvars, logging, math, os, signal, struct, subprocess, sys, tempfile, time, traceback
import numpy as np
from aiosendspin.noise import Identity, InMemoryServerPairingStore, decode_token
from aiosendspin.noise.pairing import PairingAttempt
from aiosendspin.models.types import PairMethod, MediaCommand
from aiosendspin.server import SendspinServer, AudioFormat, ClientAddedEvent

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import wait_port
PORT, PORT2, PORT3 = 16958, 16960, 16963        # the three players' Sendspin ports


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


def player(state, port, http, env):
    """a hassmic-host with Sendspin on port, its log in state/log; no push updates, arbitration or settings page: two run
    at once"""
    log = open(os.path.join(state, "log"), "w+")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-p", str(http), "-z", str(port), "-L", "-o", "0", "-a", "0", "-W", "0"],
                            env=env, stderr=log)
    wait_port(port, http)
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
    server = SendspinServer(loop, ident, "fake-ma", pairing_store=store)
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
        client.add_event_listener(lambda c, e: events.append(type(e).__name__))
        check(client.role("player@v1") is None, "no role before the operator approves the unpaired device")
        await server.trust_unpaired(cid)
        await until(lambda: client.role("player@v1") is not None and client.role("controller@v1") is not None)
        player_ = client.role("player@v1")
        check(player_ is not None and client.role("controller@v1") is not None, "player and controller roles active after approval")
        await until(lambda: player_.required_lead_time_ms == 300)
        check((player_.required_lead_time_ms, player_.min_buffer_ms, player_.static_delay_ms) == (300, 300, 0), f"initial client/state received: lead {player_.required_lead_time_ms}, buffer {player_.min_buffer_ms}, delay {player_.static_delay_ms}")

        fmt = AudioFormat(sample_rate=48000, bit_depth=16, channels=2)
        ctl_events = []
        client.group.add_event_listener(lambda g, e: ctl_events.append(type(e).__name__))
        client.group.group_role("controller").set_supported_commands([MediaCommand.PLAY, MediaCommand.PAUSE, MediaCommand.NEXT])
        stream = client.group.start_stream()
        n, phase = 4800, 0
        for i in range(40):                                # 4 s of 440 Hz
            k = np.arange(phase, phase + n); s = (8000 * np.sin(2 * np.pi * 440 * k / 48000)).astype("<i2")
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
        await server.initiate_pairing(cid, PairingAttempt(method=PairMethod.PAIRING_PSK, pairing_psk=tok.pairing_psk))
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
        other = SendspinServer(loop, Identity.generate(), "intruder", pairing_store=InMemoryServerPairingStore())
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
    server = SendspinServer(loop, ident, "fake-ma", pairing_store=store); server.add_event_listener(on_event)
    try:
        await server.start_server(port=16962, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{PORT3}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        cid = await asyncio.wait_for(added.get(), 15)
        await server.trust_unpaired(cid)                # paired, as the part above does it: the server comes back to it
        tok = decode_token(subprocess.run([f"{ROOT}/build/hassmic-host", "-T"], env=env, capture_output=True, text=True).stdout.strip())
        await server.initiate_pairing(cid, PairingAttempt(method=PairMethod.PAIRING_PSK, pairing_psk=tok.pairing_psk))
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
        server = SendspinServer(loop, ident, "fake-ma", pairing_store=store)
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
    server = SendspinServer(asyncio.get_running_loop(), Identity.generate(), "fake-ma", pairing_store=InMemoryServerPairingStore())
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
        await server.initiate_pairing(cid, PairingAttempt(method=PairMethod.PAIRING_PSK, pairing_psk=tok.pairing_psk))
        await until(lambda: (c := server.get_client(cid)) is not None and c.role("player@v1") is not None, 10)
        check(server.get_client(cid).role("player@v1") is not None, "unpaired access off: plays once paired with the token")
    finally:
        proc.terminate(); await server.close()


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
        return lines
    for lines in await asyncio.gather(part(paired), part(server_restart), part(unpaired_off)):
        print("\n".join(lines))
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

asyncio.run(main())
