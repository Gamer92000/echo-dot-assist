#!/usr/bin/env python3
"""Plays Home Assistant's side of the Wyoming satellite protocol against build/hassmic-host,
using the reference `wyoming` library so framing and message schemas are checked by the real parser."""
import asyncio, math, os, signal, struct, subprocess, sys, tempfile

from wyoming.audio import AudioChunk, AudioStart, AudioStop
from wyoming.asr import Transcript
from wyoming.client import AsyncTcpClient
from wyoming.info import Describe, Info
from wyoming.ping import Ping, Pong
from wyoming.pipeline import PipelineStage, RunPipeline
from wyoming.satellite import RunSatellite
from wyoming.snd import Played

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 16799


async def expect(client, pred, what, timeout=5):
    async def loop():
        while True:
            ev = await client.read_event()
            assert ev is not None, f"connection closed while waiting for {what}"
            if pred(ev.type):
                return ev
    return await asyncio.wait_for(loop(), timeout)


async def main():
    play = tempfile.mktemp(suffix=".raw")
    env = dict(os.environ, HASSMIC_PLAY=play)
    qemu = "--qemu" in sys.argv          # ARM build + stock Pryon model under qemu-arm; wake word comes from the audio file
    if qemu:
        env["HASSMIC_CAP"] = f"{ROOT}/testdata/alexa_espeak.raw"
        cmd = [f"{ROOT}/tools/qrun.sh", "-t", "120", f"{ROOT}/build/hassmic-qemu"]
    else:
        cmd = [f"{ROOT}/build/hassmic-host"]
    proc = subprocess.Popen(cmd + ["-p", str(PORT), "-n", "Test Dot"], env=env, start_new_session=True)
    try:
        await asyncio.sleep(6 if qemu else 0.3)
        async with AsyncTcpClient("127.0.0.1", PORT) as client:
            await client.write_event(Describe().event())
            info = Info.from_event(await expect(client, Info.is_type, "info"))
            assert info.satellite and info.satellite.name == "Test Dot", info
            assert info.satellite.supports_trigger
            print("ok   info:", info.satellite.name, info.satellite.version)

            await client.write_event(Ping(text="x").event())
            await expect(client, Pong.is_type, "pong")
            print("ok   ping/pong")

            await client.write_event(RunSatellite().event())
            await asyncio.sleep(0.2)
            if not qemu:
                proc.send_signal(signal.SIGUSR1)                  # simulated wake word
            rp = RunPipeline.from_event(await expect(client, RunPipeline.is_type, "run-pipeline", timeout=60))
            assert rp.start_stage == PipelineStage.ASR and rp.end_stage == PipelineStage.TTS, rp
            print("ok   run-pipeline:", rp.start_stage, "->", rp.end_stage, "wake word:", rp.wake_word_name)

            total = 0
            for _ in range(10):
                ch = AudioChunk.from_event(await expect(client, AudioChunk.is_type, "audio-chunk"))
                assert (ch.rate, ch.width, ch.channels) == (16000, 2, 1)
                total += len(ch.audio)
            print("ok   mic audio:", total, "bytes")

            await client.write_event(Transcript(text='turn on the "light"').event())
            tone = b"".join(struct.pack("<h", int(8000 * math.sin(i * 0.1))) for i in range(22050))
            await client.write_event(AudioStart(rate=22050, width=2, channels=1).event())
            for i in range(0, len(tone), 2048):
                await client.write_event(AudioChunk(rate=22050, width=2, channels=1, audio=tone[i:i + 2048]).event())
            await client.write_event(AudioStop().event())

            await expect(client, Played.is_type, "played", timeout=5)
            print("ok   played")
            try:                                                   # mic must be quiet again until next wake
                await expect(client, AudioChunk.is_type, "no audio", timeout=0.5)
                raise AssertionError("still streaming after pipeline end")
            except asyncio.TimeoutError:
                print("ok   streaming stopped")

            if not qemu:                                           # barge-in: wake during a 4 s answer
                proc.send_signal(signal.SIGUSR1)
                await expect(client, RunPipeline.is_type, "run-pipeline")
                await client.write_event(Transcript(text="long answer please").event())
                await client.write_event(AudioStart(rate=22050, width=2, channels=1).event())
                for i in range(0, len(tone) * 4, 2048):
                    await client.write_event(AudioChunk(rate=22050, width=2, channels=1, audio=(tone * 4)[i:i + 2048]).event())
                await client.write_event(AudioStop().event())
                await asyncio.sleep(0.5)
                t0 = asyncio.get_running_loop().time()
                proc.send_signal(signal.SIGUSR1)
                await expect(client, Played.is_type, "played after barge-in", timeout=2)
                await expect(client, RunPipeline.is_type, "second run-pipeline", timeout=2)
                await expect(client, AudioChunk.is_type, "mic audio after barge-in", timeout=2)
                print(f"ok   barge-in: playback cut and new pipeline after {asyncio.get_running_loop().time() - t0:.2f}s")

        assert open(play, "rb").read().startswith(tone), "playback data mismatch"
        print("ok   TTS audio reached playback backend intact")
    finally:
        os.killpg(proc.pid, signal.SIGKILL); proc.wait()
        if os.path.exists(play):
            os.unlink(play)


asyncio.run(main())
print("PASS")
