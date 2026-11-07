#!/usr/bin/env python3
"""-w remote barge-in: once the run that produced a reply is over, the Echo asks Home Assistant for the wake word stage
(flags 3) while the reply still plays; a wake word then cuts the reply and the same run goes on as the request: no new
request, no abort, and the audio keeps flowing to it.  "<wake word>, stop" leaves the cut reply ended and nothing goes to
the agent.  A wake-only run that ends or fails during the reply is asked for again, three failures in a reply at most
(another satellite taking the wake word is none).
"Last heard" and "Last reply" keep the reply's words while the wake-only run beside it opens."""
import asyncio, os, subprocess, sys, tempfile, threading, time
from http.server import ThreadingHTTPServer
from aioesphomeapi import APIClient, TextSensorState, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from esphome_common import Handler, check, until, sync, port_open, wav_bytes

PORT, HTTP_PORT = 16995, 16996
REPLY = 288000                                          # 3 s at 48 kHz, 16 bit mono


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
    with open(os.path.join(state, "config"), "w") as f: f.write("request_text=on\n")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
               HASSMIC_PLAY=play, HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_MODELS=os.path.join(state, "models"), HASSMIC_MDNS_FILE=os.path.join(state, "mdns"))
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-w", "remote"], env=env)
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Slow)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    ev = lambda t, d=None: cli.send_voice_assistant_event(t, d)
    def request(heard, cont="0"):
        ev(Ev.VOICE_ASSISTANT_STT_END, {"text": heard})
        ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": cont})
    def reply(cont="0"):
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_WAKE_WORD_END); request("hello", cont)
        ev(Ev.VOICE_ASSISTANT_TTS_START, {"text": "hi there"})
        ev(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"}); ev(Ev.VOICE_ASSISTANT_RUN_END)
    played = lambda: os.path.getsize(play) if os.path.exists(play) else 0
    try:
        await until(lambda: port_open(PORT))
        cli = APIClient("127.0.0.1", PORT, None)
        await cli.connect(login=True)
        es, _ = await cli.list_entities_services()
        by = {x.object_id: x.key for x in es}
        sts = []; cli.subscribe_states(sts.append)
        text = lambda name: ([x.state for x in sts if isinstance(x, TextSensorState) and x.key == by.get(name)] or [None])[-1]
        flags = []; stops = []; audio = [0]
        async def handle_start(conv_id, f, settings, phrase): flags.append(f); return 0
        async def handle_stop(abort): stops.append(abort)
        async def handle_audio(data, data2=b""): audio[0] += len(data)
        cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio)
        await sync(cli)
        await until(lambda: flags == [3])
        check(flags == [3], f"-w remote: the first request asks for the wake word stage: {flags}")

        # the wake word during the reply: cut, and the run that heard it is the request
        reply()
        await until(lambda: len(flags) == 2)
        check(flags == [3, 3], f"the reply is still playing: the wake word stage is requested again: {flags}")
        ev(Ev.VOICE_ASSISTANT_RUN_START); await sync(cli); await asyncio.sleep(0.2)
        check(text("last_heard") == "hello" and text("last_reply") == "hi there",
              f"the wake-only run's start leaves Last heard/Last reply alone: {text('last_heard')!r}, {text('last_reply')!r}")
        await until(lambda: played() > 20000)                                           # playing
        ev(Ev.VOICE_ASSISTANT_WAKE_WORD_END)
        await asyncio.sleep(0.5)
        before, cut_at = audio[0], played()
        await asyncio.sleep(1.0)
        check(played() < 0.8 * REPLY and played() - cut_at < 20000, f"the reply is cut: {played()} bytes of {REPLY} played")
        check(flags == [3, 3] and not stops, f"the run that heard the wake word goes on as the request: {flags} {stops}")
        check(audio[0] > before, "the microphone keeps going to that run")
        check(text("last_heard") == "" and text("last_reply") == "", "its wake word starts a new request: Last heard/Last reply cleared")
        request("turn on the light"); ev(Ev.VOICE_ASSISTANT_RUN_END)
        await until(lambda: len(flags) == 3)
        check(flags == [3, 3, 3] and not stops, f"the request ended: back to the wake word stage: {flags} {stops}")
        check(text("last_heard") == "turn on the light", f"Last heard is the request: {text('last_heard')!r}")

        # "<wake word>, stop": the cut reply stays ended and the run is cancelled, nothing for the agent; a follow-up
        # question's reply too, and no second run afterwards for the follow-up
        reply(cont="1")
        await until(lambda: len(flags) == 4)
        ev(Ev.VOICE_ASSISTANT_RUN_START); await until(lambda: played() > cut_at + 20000)
        ev(Ev.VOICE_ASSISTANT_WAKE_WORD_END); await asyncio.sleep(0.3)
        ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "Stop."})
        await until(lambda: stops)
        await asyncio.sleep(0.5)
        check(stops == [True] and flags == [3] * 5, f"\"stop\" after the cut: the run aborted, one wake word run after it: {stops} {flags}")
        stops.clear()

        # a wake-only run during the reply: another satellite took the wake word (not a failure), then one ends without a
        # wake word (RUN_END) and two fail (ERROR): asked for again each time but after the third failure
        start = played()
        reply()
        await until(lambda: len(flags) == 6)
        ev(Ev.VOICE_ASSISTANT_RUN_START)
        ev(Ev.VOICE_ASSISTANT_ERROR, {"code": "duplicate_wake_up_detected", "message": "x"}); ev(Ev.VOICE_ASSISTANT_RUN_END)
        await until(lambda: len(flags) == 7)
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_RUN_END)
        await until(lambda: len(flags) == 8)
        check(flags == [3] * 8, f"answered elsewhere, then ended without a wake word: requested again both times: {flags}")
        for n in range(2):
            ev(Ev.VOICE_ASSISTANT_RUN_START)
            ev(Ev.VOICE_ASSISTANT_ERROR, {"code": "wake-stream-failed", "message": "x"}); ev(Ev.VOICE_ASSISTANT_RUN_END)
            await sync(cli); await asyncio.sleep(0.2)
        check(flags == [3] * 9, f"failed during the reply: requested again once more, not after the third failure: {flags}")
        check(played() - start < 0.8 * REPLY, f"the reply still plays meanwhile: {played() - start} bytes")
        await until(lambda: played() - start >= REPLY and len(flags) == 10, 6)
        check(flags == [3] * 10 and not stops, f"the reply played out: the wake word stage again: {flags} {stops}")
        await cli.disconnect()
    finally:
        proc.terminate(); httpd.shutdown()


asyncio.run(main())
print("FAILED" if check.failed else "all good")
sys.exit(1 if check.failed else 0)
