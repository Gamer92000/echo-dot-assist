#!/usr/bin/env python3
"""-D (duplex) over Wyoming against build/hassmic-host: the microphone keeps streaming while a reply plays, and the run
ends at audio-stop as before.  Without -D the same reply stops the microphone, which is checked too.  The action
button still cuts a reply with -D (barge-in, a new run), and -D under ESPHome is ignored."""
import asyncio, contextlib, os, shutil, signal, subprocess, sys, tempfile, time

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


@contextlib.contextmanager
def echo(*args):
    """build/hassmic-host with args, in a folder of its own (state, playback, LEDs, stderr) removed afterwards"""
    tmp = tempfile.mkdtemp()
    state = os.path.join(tmp, "state"); os.mkdir(state)
    with open(os.path.join(state, "config"), "w") as f: f.write("even_volume=off\n")
    play, log = os.path.join(tmp, "play.raw"), os.path.join(tmp, "stderr.log")
    env = dict(os.environ, HASSMIC_PLAY=play, HASSMIC_FAKE_LED=os.path.join(tmp, "led"), HASSMIC_STATE=state)
    with open(log, "w") as err:
        proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-p", str(PORT), "-n", "Test Dot", *args], env=env,
                                start_new_session=True, stderr=err)
    try:
        wait_port(PORT, secs=20)
        assert proc.poll() is None, f"hassmic did not start ({proc.returncode})"
        yield proc, play, log
    finally:
        try:
            proc.send_signal(signal.SIGTERM)
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        finally: shutil.rmtree(tmp, ignore_errors=True)


def logged(log, text, secs=10):
    """Whether text reaches the log within secs: a reader process of hassmic's own writes it (clock.c), later"""
    end = time.monotonic() + secs
    while True:
        with open(log) as f:
            if text in f.read(): return True
        if time.monotonic() > end: return False
        time.sleep(0.05)


async def mic_chunks_during(client, seconds):
    """Microphone chunks the Echo sends within `seconds`, whether it said played, and how many runs it began"""
    n, played, runs = 0, False, 0
    async def loop():
        nonlocal n, played, runs
        while True:
            ev = await client.read_event()
            assert ev is not None, "connection closed"
            if AudioChunk.is_type(ev.type): n += 1
            if Played.is_type(ev.type): played = True
            if RunPipeline.is_type(ev.type): runs += 1
    try: await asyncio.wait_for(loop(), seconds)
    except asyncio.TimeoutError: pass
    return n, played, runs


async def one(duplex):
    with echo("-P", "wyoming", *(["-D"] if duplex else [])) as (proc, play, log):
        async with AsyncTcpClient("127.0.0.1", PORT) as client:
            await client.write_event(RunSatellite().event())
            await asyncio.sleep(0.2)
            proc.send_signal(signal.SIGUSR1)                    # the wake word
            while not RunPipeline.is_type((ev := await asyncio.wait_for(client.read_event(), 5)).type): pass
            before, *_ = await mic_chunks_during(client, 0.5)
            assert before > 0, "no microphone after the wake word"

            await client.write_event(AudioStart(rate=RATE, width=2, channels=1).event())
            for _ in range(10):                                  # one second of reply, sent as it plays
                await client.write_event(AudioChunk(rate=RATE, width=2, channels=1, audio=b"\0\0" * (RATE // 10)).event())
                await asyncio.sleep(0.1)
            await mic_chunks_during(client, 0.2)                 # in flight from before audio-start reached it
            during, *_ = await mic_chunks_during(client, 0.5)
            if duplex: assert during > 0, "duplex: the microphone stopped when the reply began"
            else: assert during == 0, f"without -D the reply stops the microphone ({during} chunks)"
            if duplex:                                           # the wake word mid-reply is the client's to handle
                played_before = os.path.getsize(play)
                proc.send_signal(signal.SIGUSR1)
                await asyncio.sleep(0.3)
                for _ in range(5):
                    await client.write_event(AudioChunk(rate=RATE, width=2, channels=1, audio=b"\1\0" * (RATE // 10)).event())
                    await asyncio.sleep(0.1)
                mid, _, runs = await mic_chunks_during(client, 0.5)  # every event since the wake word, buffered
                assert mid > 0, "duplex: the wake word mid-reply stopped the microphone"
                assert runs == 0, "duplex: the wake word mid-reply began another run"
                assert logged(log, "left to the client (-D)"), "duplex: the wake word mid-reply not left to the client"
                assert os.path.getsize(play) > played_before, "duplex: the wake word mid-reply cut playback"

            await client.write_event(AudioStop().event())
            _, played, _ = await mic_chunks_during(client, 2)
            assert played, "no played after audio-stop"
            after, *_ = await mic_chunks_during(client, 0.5)
            assert after == 0, "the microphone still streams after the run ended"
            print(f"ok   {'-D' if duplex else 'no -D'}: microphone chunks before {before}, while speaking {during}, after the run 0")


async def button():
    """-D: the action button (SIGUSR2) during a reply cuts it, as without -D: played long before the reply's 4 s, a new run"""
    with echo("-P", "wyoming", "-D") as (proc, play, log):
        async with AsyncTcpClient("127.0.0.1", PORT) as client:
            await client.write_event(RunSatellite().event())
            await asyncio.sleep(0.2)
            proc.send_signal(signal.SIGUSR1)
            while not RunPipeline.is_type((await asyncio.wait_for(client.read_event(), 5)).type): pass
            await client.write_event(AudioStart(rate=RATE, width=2, channels=1).event())
            for _ in range(40):                                  # 4 s of reply, sent at once
                await client.write_event(AudioChunk(rate=RATE, width=2, channels=1, audio=b"\1\0" * (RATE // 10)).event())
            await client.write_event(AudioStop().event())
            await asyncio.sleep(0.5)
            t0 = asyncio.get_running_loop().time()
            proc.send_signal(signal.SIGUSR2)
            played = run = False
            async def loop():
                nonlocal played, run
                while not (played and run):
                    ev = await client.read_event()
                    assert ev is not None, "connection closed"
                    if Played.is_type(ev.type): played = True
                    if RunPipeline.is_type(ev.type): run = True
            try: await asyncio.wait_for(loop(), 2)
            except asyncio.TimeoutError: pass
            assert played, "-D: the button did not cut the reply"
            assert run, "-D: no new run after the button cut the reply"
            print(f"ok   -D: the button cut the reply and a new run began after {asyncio.get_running_loop().time() - t0:.2f}s")


def esphome_ignored():
    """-D is Wyoming only: under ESPHome hassmic says so at start"""
    with echo("-P", "esphome", "-D") as (proc, play, log):
        assert logged(log, "-D: Wyoming only, ignored"), "-D under ESPHome: no word that it is ignored"
    print("ok   -D under ESPHome: ignored")


async def main():
    await one(duplex=False)
    await one(duplex=True)
    await button()
    esphome_ignored()


if __name__ == "__main__":
    asyncio.run(main())
