#!/usr/bin/env python3
"""Wake sound removal (earsup.c) end to end: build/hassmic-host with the PC's stand-in for what Amazon's echo canceller
leaves of the wake sound (HASSMIC_FAKE_ECHO: 90 ms after it starts, 20 dB under it) over room noise, once with the
setting off and once on, as Home Assistant hears it through the ESPHome native API."""
import asyncio, os, signal, socket, subprocess, sys, tempfile
import numpy as np
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 16996
RATE = 16000


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def tones(x):
    """per 20 ms window: power at the PC's wake sound's two tones (880 and 1320 Hz, main.c earcon_thread) and in all"""
    w = 320; n = len(x) // w; t = np.arange(w) / RATE; out = []
    for i in range(n):
        s = x[i * w:(i + 1) * w]
        p = sum(abs(np.dot(s, np.exp(-2j * np.pi * f * t))) ** 2 for f in (880, 1320)) / w
        out.append((p, float(np.dot(s, s))))
    return out


async def run(removal, noise):
    state = tempfile.mkdtemp()
    with open(os.path.join(state, "config"), "w") as f: f.write(f"wake_sound_removal={'on' if removal else 'off'}\n")
    log = open(os.path.join(state, "log"), "w+")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=noise, HASSMIC_FAKE_ECHO="90:-20",
               HASSMIC_PLAY=os.path.join(state, "play.raw"), HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_MODELS=os.path.join(state, "models"), HASSMIC_MDNS_FILE=os.path.join(state, "mdns"))
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot"], env=env, stderr=log)
    try:
        for _ in range(100):
            try: socket.create_connection(("127.0.0.1", PORT), 0.2).close(); break
            except OSError: await asyncio.sleep(0.05)
        cli = APIClient("127.0.0.1", PORT, None)
        await cli.connect(login=True)
        mic, started = bytearray(), asyncio.Event()
        async def handle_start(*a): started.set(); return 0
        async def handle_stop(abort): pass
        async def handle_audio(data, *_): mic.extend(data)
        cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio)
        await asyncio.wait_for(cli.device_info(), 5)
        await asyncio.sleep(1.5)                                   # a second of room in the ring before the wake word
        proc.send_signal(signal.SIGUSR1)
        await asyncio.wait_for(started.wait(), 5)
        t0 = asyncio.get_running_loop().time()
        await asyncio.sleep(3)
        got, live = len(mic) // 2, asyncio.get_running_loop().time() - t0
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await cli.disconnect()
    finally:
        proc.terminate(); proc.wait(5)
    log.seek(0)
    return np.frombuffer(bytes(mic[:got * 2]), "<i2").astype(float), live, log.read()


async def main():
    noise = tempfile.mktemp(suffix=".raw")
    rng = np.random.default_rng(1)
    (rng.standard_normal(RATE * 10) * 32768 * 10 ** (-60 / 20)).astype("<i2").tofile(noise)     # room at -60 dBFS
    results = {}
    for removal in (False, True):
        x, live, text = await run(removal, noise)
        w = tones(x)
        peak = max(p for p, _ in w)
        room = float(np.median([a for _, a in w]))
        results[removal] = 10 * np.log10(peak / room)
        name = "on" if removal else "off"
        # 128 ms from the ring ahead of the live stream (main.c stream_ring), then real time; none lost while held
        check(abs(len(x) / RATE - (live + 0.128)) < 0.2, f"{name}: the stream is whole, {len(x) / RATE:.2f} s in {live:.2f} s")
        line = next((l for l in text.splitlines() if "wake sound removal:" in l), "")
        if removal:
            check("echo" in line and "taken off" in line, f"on: the echo found and taken out: {line.split('removal: ')[-1]}")
            ms = int(line.split("echo ")[1].split(" ms")[0]) if "echo " in line else -1
            check(80 <= ms <= 100, f"on: at the delay the PC plays it back with (90 ms): {ms} ms")
        else:
            check(not line, "off: nothing held or taken out")
    print(f"     wake sound tones over the room: {results[False]:.1f} dB off, {results[True]:.1f} dB on")
    check(results[False] - results[True] > 15, f"on: the wake sound {results[False] - results[True]:.1f} dB down in what Home Assistant hears")
    os.remove(noise)
    print("FAIL" if check.failed else "PASS")
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
