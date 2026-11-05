#!/usr/bin/env python3
"""-w remote: a reply that asks a question (continue_conversation) is answered without a wake word.  The first request
asks Home Assistant for the wake word stage (flags 3, USE_VAD | USE_WAKE_WORD); the follow-up must not (flags 1), or
Home Assistant waits for a wake word that nobody says and ignores the answer."""
import asyncio, os, subprocess, sys, tempfile, threading
from http.server import ThreadingHTTPServer
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from esphome_common import Handler, check, until, sync, port_open

PORT, HTTP_PORT = 16997, 16998


async def main():
    state = tempfile.mkdtemp()
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
               HASSMIC_PLAY=os.path.join(state, "play.raw"), HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_MODELS=os.path.join(state, "models"), HASSMIC_MDNS_FILE=os.path.join(state, "mdns"))
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-w", "remote"], env=env)
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        await until(lambda: port_open(PORT))
        cli = APIClient("127.0.0.1", PORT, None)
        await cli.connect(login=True)
        flags = []; started = asyncio.Event()
        async def handle_start(conv_id, f, settings, phrase): flags.append(f); started.set(); return 0
        async def nothing(*a): pass
        cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        await sync(cli)
        await asyncio.wait_for(started.wait(), 5)
        check(flags == [3], f"-w remote: the first request asks for the wake word stage: {flags}")

        started.clear()
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_WAKE_WORD_END, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "which light"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await until(lambda: len(flags) >= 3)      # [3, 3]: the wake word listens during the reply, then the follow-up
        check(flags == [3, 3, 1], f"the follow-up to a question has no wake word stage: {flags}")

        started.clear()
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "the kitchen one"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "0"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await until(lambda: len(flags) >= 4)
        check(flags == [3, 3, 1, 3], f"after an answer that ends the conversation: the wake word stage again: {flags}")
        await cli.disconnect()
    finally:
        proc.terminate(); httpd.shutdown()


asyncio.run(main())
print("FAILED" if check.failed else "all good")
sys.exit(1 if check.failed else 0)
