#!/usr/bin/env python3
"""Real protocol peers check padding counts, ordering and unchanged command PCM."""
import asyncio, os, random, signal, socket, struct, subprocess, tempfile, time, threading
from http.server import ThreadingHTTPServer
from pathlib import Path
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev
from esphome_common import Handler, port_open, until
from wyoming.audio import AudioChunk
from wyoming.client import AsyncTcpClient
from wyoming.satellite import RunSatellite

ROOT = Path(__file__).resolve().parents[1]
COMMAND_BYTES = 16000
# Constant *power*, not constant PCM: polarity carries a seeded, nonrepeating
# sample-order reference, while AGC sees the same level at every capture phase.
_rng = random.Random(0x50434D)
SOURCE_SIGNS = bytes(_rng.getrandbits(1) for _ in range(160000))
SOURCE_PCM = struct.pack(f"<{len(SOURCE_SIGNS)}h", *(300 if s else -300 for s in SOURCE_SIGNS))


def check_command(audio, reference=None):
    """Align once to the source, then check every sample without resynchronizing.

    Fresh hosts can start capture on different 50 ms blocks. Positive AGC gain
    preserves polarity, so a unique 64-sample anchor locates each run in the
    synthetic source independently of that phase and of the AGC's level changes.
    Only polarity is used for alignment; exact magnitudes are also compared with
    the unpadded run, so gain changes are not normalized away.
    """
    command = audio[:COMMAND_BYTES]
    assert len(command) == COMMAND_BYTES, "short command PCM"
    samples = struct.unpack(f"<{COMMAND_BYTES // 2}h", command)
    assert all(samples), "silence inserted into command PCM"
    signs = bytes(s > 0 for s in samples)
    start = SOURCE_SIGNS.find(signs[:64])
    assert start >= 0, "command onset is not in the synthetic source"
    assert SOURCE_SIGNS.find(signs[:64], start + 1) == -1, "ambiguous command anchor"
    assert signs == SOURCE_SIGNS[start:start + len(signs)], "command PCM lost, duplicated or reordered"
    magnitudes = struct.pack(f"<{len(samples)}h", *(abs(s) for s in samples))
    if reference is not None:
        assert magnitudes == reference, "command PCM gain changed"
    return magnitudes


def check_mutations(audio, reference):
    # Mutate bytes received from the real host, not a stand-in for its output.
    # Leave the onset/anchor intact and retain enough live tail that deletion
    # cannot fail merely because the checker ran out of samples.
    at, size = 2048, 1024
    assert len(audio) >= COMMAND_BYTES + size
    a, b = audio[at:at + size], audio[at + size:at + 2 * size]
    mutations = {
        "dropped": audio[:at] + audio[at + size:],
        "duplicated": audio[:at] + a + audio[at:],
        "reordered": audio[:at] + b + a + audio[at + 2 * size:],
        "dropped+duplicated (same length)": audio[:at] + b + b + audio[at + 2 * size:],
    }
    for name, mutated in mutations.items():
        try:
            check_command(mutated, reference)
        except AssertionError as err:
            assert str(err) == "command PCM lost, duplicated or reordered", f"{name}: wrong rejection: {err}"
            print(f"PASS mutation {name}: rejected real native-host PCM corruption")
        else:
            raise AssertionError(f"checker accepted {name} command chunk")


async def collect(ms, mode="wake", enabled=True):
    with tempfile.TemporaryDirectory() as tmp:
        state = Path(tmp)
        (state / "input.raw").write_bytes(SOURCE_PCM)
        (state / "config").write_text(f"pad_request_audio={'on' if enabled else 'off'}\nrequest_audio_padding={ms}\n")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]
        env = dict(os.environ, HASSMIC_STATE=tmp, HASSMIC_CAP=str(state / "input.raw"),
                   HASSMIC_MODELS=str(state / "models"), HASSMIC_PLAY=str(state / "play.raw"),
                   HASSMIC_MDNS_FILE=str(state / "mdns"), HASSMIC_ARB_ADDR="127.255.255.255")
        proto = "wyoming" if mode == "wyoming" else "esphome"
        args = [str(ROOT / "build/hassmic-host"), "-P", proto, "-p", str(port), "-z", "0", "-o", "0"]
        if mode == "remote": args += ["-w", "remote"]
        with (state / "log").open("w") as log:
            proc = subprocess.Popen(args, env=env, stdout=log, stderr=log)
            cli = None; httpd = None
            try:
                assert await until(lambda: port_open(port)), "host failed to bind"
                if mode == "wyoming":
                    async with AsyncTcpClient("127.0.0.1", port) as cli:
                        await cli.write_event(RunSatellite().event())
                        await asyncio.sleep(1.2)
                        proc.send_signal(signal.SIGUSR1)
                        audio = bytearray()
                        while len(audio) < 16000:
                            ev = await asyncio.wait_for(cli.read_event(), 5)
                            assert ev is not None
                            if AudioChunk.is_type(ev.type): audio.extend(AudioChunk.from_event(ev).audio)
                        return bytes(audio), 0
                cli = APIClient("127.0.0.1", port, None); await cli.connect(login=True)
                audio = bytearray(); started = asyncio.Event(); requests = 0
                async def start(*args):
                    nonlocal requests
                    requests += 1; started.set(); return 0
                async def stop(*args): pass
                async def mic(data, *args): audio.extend(data)
                cli.subscribe_voice_assistant(handle_start=start, handle_stop=stop, handle_audio=mic)
                await cli.device_info()
                if mode == "server":
                    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
                    threading.Thread(target=httpd.serve_forever, daemon=True).start()
                    result = await cli.send_voice_assistant_announcement_await_response(
                        f"http://127.0.0.1:{httpd.server_port}/a.wav", 5, start_conversation=True)
                    assert result.success, "server announcement failed"
                elif mode != "remote":
                    await asyncio.sleep(1.2)
                    proc.send_signal(signal.SIGUSR2 if mode == "button" else signal.SIGUSR1)
                t = time.monotonic()
                await asyncio.wait_for(started.wait(), 5)
                assert await until(lambda: len(audio) >= ms * 32 + COMMAND_BYTES + 2048)
                if mode == "followup":
                    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
                    await cli.device_info(); await asyncio.sleep(0.1)
                    audio.clear(); started.clear()
                    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"continue_conversation": "1"})
                    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
                    await asyncio.wait_for(started.wait(), 5)
                    assert await until(lambda: len(audio) >= 16000)
                assert requests == (2 if mode == "followup" else 1), "duplicate or missing request"
                # Duration is checked by exact bytes, not a timing-sensitive packet count.
                return bytes(audio), time.monotonic() - t
            finally:
                if proto == "esphome" and cli: await cli.disconnect()
                proc.terminate(); proc.wait(timeout=5)
                if httpd: httpd.shutdown(); httpd.server_close()


async def main():
    baseline, _ = await collect(0)
    reference = check_command(baseline)
    for ms in (1, 137, 800, 1000):
        audio, elapsed = await collect(ms)
        prefix = ms * 16000 // 1000 * 2
        assert audio[:prefix] == bytes(prefix), f"{ms} ms: missing exact zero prefix"
        command = audio[prefix:]
        check_command(command, reference)
        # Collecting 0.5 s of live PCM must not cost an extra second for padding.
        assert elapsed < 1.2, f"padding slept instead of sending: {elapsed:.3f}s"
        print(f"PASS wake {ms} ms: {prefix} zero bytes then 16000 ordered command bytes, unchanged gain")
        if ms == 800:
            check_mutations(command, reference)
    for mode in ("button", "followup", "server", "remote", "wyoming"):
        plain, _ = await collect(0, mode)
        padded, _ = await collect(800, mode)
        check_command(padded, check_command(plain))
        print(f"PASS {mode}: no padding, 16000 ordered command bytes, unchanged gain")
    off, _ = await collect(800, enabled=False)
    check_command(off, reference)
    print("PASS off: hidden nonzero config cannot pad")


if __name__ == "__main__":
    asyncio.run(main())
