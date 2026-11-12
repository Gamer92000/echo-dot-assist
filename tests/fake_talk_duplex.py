#!/usr/bin/env python3
"""-D (duplex) over Wyoming against build/hassmic-host: the microphone keeps streaming while a reply plays, and the run
ends at audio-stop as before.  Without -D the same reply stops the microphone, which is checked too."""
import asyncio, os, signal, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port
from wyoming.audio import AudioChunk, AudioStart, AudioStop
from wyoming.client import AsyncTcpClient
from wyoming.pipeline import RunPipeline
from wyoming.satellite import RunSatellite
from wyoming.snd import Played

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 16798
RATE = 24000


async def mic_chunks_during(client, seconds):
    """Microphone chunks the Echo sends within `seconds`, and whether it said played."""
    n, played = 0, False
    async def loop():
        nonlocal n, played
        while True:
            ev = await client.read_event()
            assert ev is not None, "connection closed"
            if AudioChunk.is_type(ev.type): n += 1
            if Played.is_type(ev.type): played = True
    try: await asyncio.wait_for(loop(), seconds)
    except asyncio.TimeoutError: pass
    return n, played


async def one(duplex):
    state = tempfile.mkdtemp()
    with open(os.path.join(state, "config"), "w") as f: f.write("even_volume=off\n")
    env = dict(os.environ, HASSMIC_PLAY=tempfile.mktemp(suffix=".raw"), HASSMIC_FAKE_LED=tempfile.mktemp(), HASSMIC_STATE=state)
    cmd = [f"{ROOT}/build/hassmic-host", "-P", "wyoming", "-p", str(PORT), "-n", "Test Dot"] + (["-D"] if duplex else [])
    proc = subprocess.Popen(cmd, env=env, start_new_session=True)
    try:
        wait_port(PORT, secs=20)
        async with AsyncTcpClient("127.0.0.1", PORT) as client:
            await client.write_event(RunSatellite().event())
            await asyncio.sleep(0.2)
            proc.send_signal(signal.SIGUSR1)                    # the wake word
            while not RunPipeline.is_type((ev := await asyncio.wait_for(client.read_event(), 5)).type): pass
            before, _ = await mic_chunks_during(client, 0.5)
            assert before > 0, "no microphone after the wake word"

            await client.write_event(AudioStart(rate=RATE, width=2, channels=1).event())
            for _ in range(10):                                  # one second of reply, sent as it plays
                await client.write_event(AudioChunk(rate=RATE, width=2, channels=1, audio=b"\0\0" * (RATE // 10)).event())
                await asyncio.sleep(0.1)
            await mic_chunks_during(client, 0.2)                 # in flight from before audio-start reached it
            during, _ = await mic_chunks_during(client, 0.5)
            if duplex: assert during > 0, "duplex: the microphone stopped when the reply began"
            else: assert during == 0, f"without -D the reply stops the microphone ({during} chunks)"
            if duplex:                                           # the wake word mid-reply is the client's to handle
                played_before = os.path.getsize(env["HASSMIC_PLAY"])
                proc.send_signal(signal.SIGUSR1)
                await asyncio.sleep(0.3)
                for _ in range(5):
                    await client.write_event(AudioChunk(rate=RATE, width=2, channels=1, audio=b"\1\0" * (RATE // 10)).event())
                    await asyncio.sleep(0.1)
                mid, _ = await mic_chunks_during(client, 0.5)
                assert mid > 0, "duplex: the wake word mid-reply stopped the microphone"
                assert os.path.getsize(env["HASSMIC_PLAY"]) > played_before, "duplex: the wake word mid-reply cut playback"

            await client.write_event(AudioStop().event())
            _, played = await mic_chunks_during(client, 2)
            assert played, "no played after audio-stop"
            after, _ = await mic_chunks_during(client, 0.5)
            assert after == 0, "the microphone still streams after the run ended"
            print(f"ok   {'-D' if duplex else 'no -D'}: microphone chunks before {before}, while speaking {during}, after the run 0")
    finally:
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=5)


async def main():
    await one(duplex=False)
    await one(duplex=True)


if __name__ == "__main__":
    asyncio.run(main())
