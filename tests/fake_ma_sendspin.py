#!/usr/bin/env python3
"""Plays Music Assistant's side of Sendspin against build/hassmic-host with the reference server library
(aiosendspin 9.1.1, the version in Music Assistant 2.10.4): dials the player, approves it unpaired, streams a sine,
sends a volume command, and checks what reaches the player's audio backend.  Then pairing, a second server, a restart of
the server (the player asks for mDNS announcements until one is back, issue #28) and unpairing."""
import re, asyncio, logging, math, os, signal, struct, subprocess, sys, tempfile, time
import numpy as np
from aiosendspin.noise import Identity, InMemoryServerPairingStore, decode_token
from aiosendspin.noise.pairing import PairingAttempt
from aiosendspin.models.types import PairMethod, MediaCommand
from aiosendspin.server import SendspinServer, AudioFormat, ClientAddedEvent

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 16958


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def main():
    if "-v" in sys.argv: logging.basicConfig(level=logging.DEBUG)
    state = tempfile.mkdtemp(); music = os.path.join(state, "music.raw")
    want = os.environ.get("CODEC", "flac")         # CODEC=flac|opus|pcm: what the player lists first
    open(os.path.join(state, "sendspin.unpaired"), "w").write("1")     # unpaired access switched on for the first part
    env = dict(os.environ, HASSMIC_SENDSPIN_CODECS=want, HASSMIC_STATE=state, HASSMIC_MUSIC=music, HASSMIC_SETTINGS=os.path.join(state, "settings"), HASSMIC_OUTPUT_LATENCY_MS="0",
               HASSMIC_ANNOUNCE_TIMES="3 6")       # mDNS again after 3 s without a server that played here (30 s), then 6
    mreq = os.path.join(state, "mdns")                  # root's cue to restart avahi (main.sh netwatch)
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-p", "16959", "-z", str(PORT), "-L"], env=env)
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
        await asyncio.sleep(3.5)
        player = client.role("player@v1")
        check(player is not None and client.role("controller@v1") is not None, "player and controller roles active after approval")
        check((player.required_lead_time_ms, player.min_buffer_ms, player.static_delay_ms) == (300, 300, 0), f"initial client/state received: lead {player.required_lead_time_ms}, buffer {player.min_buffer_ms}, delay {player.static_delay_ms}")

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
            if i == 15: player.set_volume(30)
            if i == 25: proc.send_signal(signal.SIGUSR2)      # action button while music plays
        await asyncio.sleep(2.5)
        fmt_used = player.get_audio_format() if hasattr(player, "get_audio_format") else None
        check("ControllerPauseEvent" in ctl_events, f"action button during playback sends controller pause: {ctl_events}")
        check(player.volume == 30, f"volume command applied and echoed in client/state: {player.volume}")
        await client.group.stop()
        await asyncio.sleep(0.5)
        check(not os.path.exists(mreq), "a server connected all along: no mDNS announcements")

        pcm = np.frombuffer(open(music, "rb").read(), dtype="<i2").reshape(-1, 2)[:, 0].astype(float)
        nz = np.flatnonzero(np.abs(pcm) > 100)
        tone = pcm[nz[0]:nz[-1] + 1] if len(nz) else pcm[:0]
        dur = len(tone) / 48000
        check(3.9 <= dur <= 4.05, f"played {dur:.3f} s of tone (sent 4.000 s) as {want}")
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
        await asyncio.sleep(1.5)
        recs = await server.pairing_store.list_records()
        sec = server.get_client(cid).connection_security
        check(len(recs) == 1 and os.path.getsize(os.path.join(state, "sendspin.records")) > 0, "pairing finalised: record on both sides")
        check("long" in str(sec.psk_category).lower(), f"session re-handshaken to the long-term key: {sec.psk_category}")
        await asyncio.sleep(3.5)
        check(server.get_client(cid).role("player@v1") is not None, "player role active on the paired session")

        # a second server dials while the first one is idle: it never played here, so it is turned away
        other = SendspinServer(loop, Identity.generate(), "intruder", pairing_store=InMemoryServerPairingStore())
        got = asyncio.Queue(); other.add_event_listener(lambda _s, ev: got.put_nowait(type(ev).__name__))
        await other.start_server(port=16956, discover_clients=False)
        other.connect_to_client(f"ws://127.0.0.1:{PORT}/sendspin", retry_initial_connection=False, retry_indefinitely=False)
        await asyncio.sleep(3)
        oc = other.get_client(cid)
        check(oc is None or not oc.is_connected,
              "idle second server is turned away (concurrent_attempt)")
        check(server.get_client(cid).is_connected, "first server keeps its connection")
        await other.close()

        # Music Assistant restarting (issue #28): its session ends, and the new instance only browses mDNS, which may not
        # reach the Echo.  While no server that played here is connected, root is asked to announce again, at growing
        # intervals; a server back stops it.
        async def until(cond, secs):
            t0 = time.monotonic()
            while not cond() and time.monotonic() - t0 < secs: await asyncio.sleep(0.2)
        await server.close()
        await until(lambda: os.path.exists(mreq), 8)
        why = open(mreq).read().strip() if os.path.exists(mreq) else "nothing"
        check(why.startswith("no Sendspin server for "), f"server gone: root asked to announce mDNS again ({why})")
        if os.path.exists(mreq): os.remove(mreq)
        t0 = time.monotonic(); await until(lambda: os.path.exists(mreq), 10); dt = time.monotonic() - t0
        check(os.path.exists(mreq) and dt > 3, f"still gone: announced again, after a longer wait ({dt:.1f} s)")
        server = SendspinServer(loop, ident, "fake-ma", pairing_store=store)
        added = asyncio.Queue(); server.add_event_listener(on_event)
        await server.start_server(port=16957, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{PORT}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        check(await asyncio.wait_for(added.get(), 15) == cid, "the restarted server finds the player again")
        await asyncio.sleep(3)
        check(server.get_client(cid).role("player@v1") is not None, "and it plays again, paired")
        if os.path.exists(mreq): os.remove(mreq)
        await asyncio.sleep(8)
        check(not os.path.exists(mreq), "server back: no more announcements")

        await server.unpair(cid)
        await asyncio.sleep(1.5)
        check(os.path.getsize(os.path.join(state, "sendspin.records")) == 0, "server/unpair deletes the record on the player")
    finally:
        proc.terminate(); await server.close()

    # ---- unpaired access off, the default (the Home Assistant switch, kept in state/sendspin.unpaired): approving the device
    # unpaired gives it no role; pairing with the token still works and then it plays
    proc.wait(5)                                    # its Sendspin port is taken again below
    state2 = tempfile.mkdtemp()                     # no sendspin.unpaired file: the default
    env2 = dict(env, HASSMIC_STATE=state2, HASSMIC_SETTINGS=os.path.join(state2, "settings"))
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-p", "16954", "-z", str(PORT), "-L"], env=env2)
    await asyncio.sleep(0.5)
    server = SendspinServer(loop, Identity.generate(), "fake-ma", pairing_store=InMemoryServerPairingStore())
    added = asyncio.Queue(); server.add_event_listener(on_event)
    try:
        await server.start_server(port=16955, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{PORT}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        cid = await asyncio.wait_for(added.get(), 15)
        await server.trust_unpaired(cid)
        await asyncio.sleep(2)
        check(server.get_client(cid).role("player@v1") is None, "unpaired access off: approved unpaired, still no player role")
        tok = decode_token(subprocess.run([f"{ROOT}/build/hassmic-host", "-T"], env=env2, capture_output=True, text=True).stdout.strip())
        await server.initiate_pairing(cid, PairingAttempt(method=PairMethod.PAIRING_PSK, pairing_psk=tok.pairing_psk))
        await asyncio.sleep(5)
        check(server.get_client(cid).role("player@v1") is not None, "unpaired access off: plays once paired with the token")
    finally:
        proc.terminate(); await server.close()
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

asyncio.run(main())
