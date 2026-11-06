#!/usr/bin/env python3
"""-w remote barge-in: once the run that produced a reply is over, the Echo asks Home Assistant for the wake word stage
(flags 3) while the reply still plays; a wake word then cuts the reply and the same run goes on as the request: no new\nrequest, no abort, and the audio keeps flowing to it.  A wake-only run that fails during the reply is requested again."""
import asyncio, os, subprocess, sys, tempfile, threading, time
from http.server import ThreadingHTTPServer
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from esphome_common import Handler, check, until, sync, port_open, wav_bytes

PORT, HTTP_PORT = 16995, 16996


class Slow(Handler):
    """A reply that arrives in real time, so it is still playing when the wake word comes."""
    def do_GET(self):
        body = wav_bytes(48000, 3.0)
        self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.end_headers()
        try:
            for i in range(0, len(body), 9600):             # 0.1 s of audio each
                self.wfile.write(body[i:i + 9600]); self.wfile.flush(); time.sleep(0.1)
        except OSError: pass                                # the Echo hung up


async def main():
    state = tempfile.mkdtemp()
    play = os.path.join(state, "play.raw")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
               HASSMIC_PLAY=play, HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_MODELS=os.path.join(state, "models"), HASSMIC_MDNS_FILE=os.path.join(state, "mdns"))
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-w", "remote"], env=env)
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Slow)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    ev = lambda t, d=None: cli.send_voice_assistant_event(t, d)
    reply = lambda: (ev(Ev.VOICE_ASSISTANT_RUN_START), ev(Ev.VOICE_ASSISTANT_WAKE_WORD_END), ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "hello"}),
                     ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "0"}),
                     ev(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"}), ev(Ev.VOICE_ASSISTANT_RUN_END))
    try:
        await until(lambda: port_open(PORT))
        cli = APIClient("127.0.0.1", PORT, None)
        await cli.connect(login=True)
        flags = []; stops = []; audio = [0]
        async def handle_start(conv_id, f, settings, phrase): flags.append(f); return 0
        async def handle_stop(abort): stops.append(abort)
        async def handle_audio(data, data2=b""): audio[0] += len(data)
        cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio)
        await sync(cli)
        await until(lambda: flags == [3])
        check(flags == [3], f"-w remote: the first request asks for the wake word stage: {flags}")

        reply()
        await until(lambda: len(flags) == 2)
        check(flags == [3, 3], f"the reply is still playing: the wake word stage is requested again: {flags}")

        await until(lambda: os.path.exists(play) and os.path.getsize(play) > 20000)     # playing
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_WAKE_WORD_END)
        await asyncio.sleep(0.5)
        before = audio[0]
        await asyncio.sleep(1.5)
        check(os.path.getsize(play) < 0.8 * 288000, f"the reply is cut: {os.path.getsize(play)} bytes of 288000 played")
        check(flags == [3, 3] and not stops, f"the run that heard the wake word goes on as the request: {flags} {stops}")
        check(audio[0] > before, "the microphone keeps going to that run")
        ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "stop"}); ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "0"})
        ev(Ev.VOICE_ASSISTANT_RUN_END)
        await until(lambda: len(flags) == 3)
        check(flags == [3, 3, 3], f"the request ended: back to the wake word stage: {flags}")

        reply()
        await until(lambda: len(flags) == 4)
        ev(Ev.VOICE_ASSISTANT_ERROR, {"code": "wake-stream-failed", "message": "x"}); ev(Ev.VOICE_ASSISTANT_RUN_END)
        await until(lambda: len(flags) == 5)
        check(flags == [3, 3, 3, 3, 3], f"a wake-only run that failed during the reply is requested again: {flags}")
        await cli.disconnect()
    finally:
        proc.terminate(); httpd.shutdown()


asyncio.run(main())
print("FAILED" if check.failed else "all good")
sys.exit(1 if check.failed else 0)
