#!/usr/bin/env python3
"""Plays Music Assistant's side of Sendspin against build/hassmic-host with the reference server library
(aiosendspin 9.1.1, the version in Music Assistant 2.10.4): dials the player, approves it unpaired, streams a sine,
sends a volume command, and checks what reaches the player's audio backend."""
import asyncio, logging, math, os, signal, struct, subprocess, sys, tempfile
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
    env = dict(os.environ, HASSMIC_SENDSPIN_CODECS=want, HASSMIC_STATE=state, HASSMIC_MUSIC=music, HASSMIC_SETTINGS=os.path.join(state, "settings"), HASSMIC_OUTPUT_LATENCY_MS="0")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-p", "16959", "-z", str(PORT), "-L"], env=env)
    await asyncio.sleep(0.5)
    loop = asyncio.get_running_loop()
    server = SendspinServer(loop, Identity.generate(), "fake-ma", pairing_store=InMemoryServerPairingStore())
    added = asyncio.Queue(); events = []
    def on_event(_srv, ev):
        if isinstance(ev, ClientAddedEvent): added.put_nowait(ev.client_id)
    server.add_event_listener(on_event)
    try:
        await server.start_server(port=16957, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{PORT}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        cid = await asyncio.wait_for(added.get(), 15)
        client = server.get_client(cid)
        check(client.info.name == "Echo Dot" and "player@v1" in client.info.supported_roles, f"hello over the encrypted session: {client.info.name!r} {client.info.supported_roles}")
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

        await server.unpair(cid)
        await asyncio.sleep(1.5)
        check(os.path.getsize(os.path.join(state, "sendspin.records")) == 0, "server/unpair deletes the record on the player")
    finally:
        proc.terminate(); await server.close()
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

asyncio.run(main())
