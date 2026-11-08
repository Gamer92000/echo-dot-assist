#!/usr/bin/env python3
"""-w remote: a reply that asks a question (continue_conversation) is answered without a wake word.  The first request
asks Home Assistant for the wake word stage (flags 3, USE_VAD | USE_WAKE_WORD); the follow-up must not (flags 1), or
Home Assistant waits for a wake word that nobody says and ignores the answer.  The follow-up is a new request from its
RUN_START on ("Last heard"/"Last reply" cleared: it gets no WAKE_WORD_END), and the action button cutting a reply starts
one the same way: no wake word to wait for after it either."""
import asyncio, os, signal, subprocess, sys, tempfile, threading, time
from http.server import ThreadingHTTPServer
from aioesphomeapi import APIClient, TextSensorState, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from esphome_common import Handler, check, until, sync, port_open, wav_bytes

PORT, HTTP_PORT = 16997, 16998
REPLY = 288000                                          # the slow reply: 3 s at 48 kHz, 16 bit mono


class Slow(Handler):
    """/slow.wav arrives in real time, so it is still playing when the button is pressed; the rest as Handler."""
    def do_GET(self):
        if not self.path.startswith("/slow"): return super().do_GET()
        body = wav_bytes(48000, 3.0)
        self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.end_headers()
        try:
            for i in range(0, len(body), 9600):             # 0.1 s of audio each
                self.wfile.write(body[i:i + 9600]); self.wfile.flush(); time.sleep(0.1)
        except OSError: pass                                # the Echo hung up


async def main():
    state = tempfile.mkdtemp()
    play = os.path.join(state, "play.raw")
    with open(os.path.join(state, "config"), "w") as f: f.write("request_text=on\n")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
               HASSMIC_PLAY=play, HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_MODELS=os.path.join(state, "models"), HASSMIC_MDNS_FILE=os.path.join(state, "mdns"))
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-w", "remote"], env=env)
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Slow)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    ev = lambda t, d=None: cli.send_voice_assistant_event(t, d)
    played = lambda: os.path.getsize(play) if os.path.exists(play) else 0
    try:
        await until(lambda: port_open(PORT))
        cli = APIClient("127.0.0.1", PORT, None)
        await cli.connect(login=True)
        es, _ = await cli.list_entities_services()
        by = {x.object_id: x.key for x in es}
        sts = []; cli.subscribe_states(sts.append)
        text = lambda name: ([x.state for x in sts if isinstance(x, TextSensorState) and x.key == by.get(name)] or [None])[-1]
        flags = []
        async def handle_start(conv_id, f, settings, phrase): flags.append(f); return 0
        async def nothing(*a): pass
        cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        await sync(cli)
        await until(lambda: flags == [3])
        check(flags == [3], f"-w remote: the first request asks for the wake word stage: {flags}")

        # a reply that asks back: the run after it starts at STT
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_WAKE_WORD_END)
        ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
        ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
        ev(Ev.VOICE_ASSISTANT_TTS_START, {"text": "which light"})
        ev(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
        ev(Ev.VOICE_ASSISTANT_RUN_END)
        await until(lambda: len(flags) >= 3)      # [3, 3]: the wake word listens during the reply, then the follow-up
        check(flags == [3, 3, 1], f"the follow-up to a question has no wake word stage: {flags}")
        await sync(cli); await asyncio.sleep(0.2)
        check(text("last_heard") == "turn on the light" and text("last_reply") == "which light",
              f"Last heard/Last reply are the question's: {text('last_heard')!r}, {text('last_reply')!r}")

        # the follow-up's RUN_START begins a new request: "Last heard"/"Last reply" cleared, as a wake word would
        ev(Ev.VOICE_ASSISTANT_RUN_START); await sync(cli); await asyncio.sleep(0.2)
        check(text("last_heard") == "" and text("last_reply") == "",
              f"the follow-up is a new request: Last heard/Last reply cleared: {text('last_heard')!r}, {text('last_reply')!r}")
        ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "the kitchen one"})
        ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "0"})
        ev(Ev.VOICE_ASSISTANT_TTS_START, {"text": "done"})
        ev(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/slow.wav"})
        ev(Ev.VOICE_ASSISTANT_RUN_END)
        await until(lambda: len(flags) >= 4)
        check(flags == [3, 3, 1, 3], f"an answer that ends the conversation: the wake word stage again: {flags}")
        await sync(cli); await asyncio.sleep(0.2)
        check(text("last_heard") == "the kitchen one", f"Last heard is the answer: {text('last_heard')!r}")

        # the action button during that reply: it cuts it, and the run after it listens at once
        start = played()
        await until(lambda: played() - start > 20000)                                   # playing
        proc.send_signal(signal.SIGUSR2)
        await until(lambda: len(flags) >= 5)
        check(played() - start < 0.8 * REPLY, f"the button cut the reply: {played() - start} bytes of {REPLY} played")
        check(flags == [3, 3, 1, 3, 1], f"the run the button starts has no wake word stage: {flags}")
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "what time is it"})
        ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "y", "continue_conversation": "0"}); ev(Ev.VOICE_ASSISTANT_RUN_END)
        await until(lambda: len(flags) >= 6)
        check(flags == [3, 3, 1, 3, 1, 3], f"the button's request ended: the wake word stage again: {flags}")
        await cli.disconnect()
    finally:
        proc.terminate(); httpd.shutdown()


asyncio.run(main())
print("FAILED" if check.failed else "all good")
sys.exit(1 if check.failed else 0)
