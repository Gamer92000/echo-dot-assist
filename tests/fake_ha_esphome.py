#!/usr/bin/env python3
"""Plays Home Assistant's side of the ESPHome native API against build/hassmic-host, using the reference
`aioesphomeapi` client (the library Home Assistant itself uses), so framing and protobuf layout are checked by the real parser."""
import asyncio, io, math, os, signal, struct, subprocess, sys, tempfile, threading, wave
from http.server import BaseHTTPRequestHandler, HTTPServer
from aioesphomeapi import SelectInfo, NumberInfo, SwitchInfo, SelectState, NumberState, SwitchState
from aioesphomeapi import APIClient, MediaPlayerInfo, MediaPlayerEntityState, VoiceAssistantEventType as Ev, VoiceAssistantTimerEventType as Tm

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, HTTP_PORT = 16953, 16954


def tone(rate, seconds, freq=440):
    return b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * freq * i / rate))) for i in range(int(rate * seconds)))


def wav_bytes(rate, seconds):
    b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(tone(rate, seconds))
    return b.getvalue()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = wav_bytes(48000, 0.5)
        if self.path.endswith(".mp3"):                  # what Home Assistant sends for a TTS announcement before any pipeline ran
            body = subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=1", "-ar", "24000",
                                   "-ac", "1", "-f", "mp3", "-"], capture_output=True, check=True).stdout
        self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.end_headers()   # no length: like a transcoding proxy
        self.wfile.write(body)
    def log_message(self, *a): pass


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def main():
    play = tempfile.mktemp(suffix=".raw")
    settings = tempfile.mktemp(suffix=".settings")
    env = dict(os.environ, HASSMIC_SETTINGS=settings, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=play)
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-L"], env=env)
    httpd = HTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    await asyncio.sleep(0.5)
    try:
        cli = APIClient("127.0.0.1", PORT, None)
        await cli.connect(login=True)
        info = await cli.device_info()
        check(info.name == "echo-dot" and info.friendly_name == "Echo Dot", f"device info: {info.name!r} / {info.friendly_name!r}")
        check(info.voice_assistant_feature_flags == 61, f"voice assistant feature flags = {info.voice_assistant_feature_flags}")
        entities, _ = await cli.list_entities_services()
        mp = [e for e in entities if isinstance(e, MediaPlayerInfo)]
        check(len(mp) == 1 and len(mp[0].supported_formats) == 2 and mp[0].supported_formats[1].sample_rate == 48000,
              f"media player entity with formats: {[(f.format, f.sample_rate, int(f.purpose)) for f in mp[0].supported_formats] if mp else None}")
        states = []
        cli.subscribe_states(states.append)
        by = {e.object_id: e for e in entities}
        check(isinstance(by.get("noise_suppression_level"), SelectInfo) and list(by["noise_suppression_level"].options) == ["Off", "Low", "Medium", "High", "Max"]
              and isinstance(by.get("auto_gain"), NumberInfo) and by["auto_gain"].max_value == 31 and isinstance(by.get("mic_volume_multiplier"), NumberInfo)
              and isinstance(by.get("mute"), SwitchInfo) and isinstance(by.get("wake_sound"), SwitchInfo), "settings entities listed")
        cli.select_command(by["noise_suppression_level"].key, "High"); cli.number_command(by["auto_gain"].key, 15)
        cli.number_command(by["mic_volume_multiplier"].key, 2.5)
        await asyncio.sleep(0.5)
        check(any(isinstance(x, SelectState) and x.state == "High" for x in states) and any(isinstance(x, NumberState) and x.state == 15 for x in states),
              "setting commands reflected in state")
        check(open(settings).read().split()[:3] == ["3", "15", "2.50"], f"settings persisted: {open(settings).read().strip()!r}")
        cfg = await cli.get_voice_assistant_configuration(5)
        check(list(cfg.active_wake_words) == ["alexa"], f"wake word configuration: {list(cfg.active_wake_words)}")

        started = asyncio.Event(); mic = bytearray(); stopped = []
        async def handle_start(conv_id, flags, settings, phrase):
            handle_start.args = (flags, phrase); handle_start.audio = (settings.noise_suppression_level, settings.auto_gain, round(settings.volume_multiplier, 2)); started.set(); return 0           # 0 = audio over the API connection
        async def handle_stop(abort): stopped.append(abort)
        async def handle_audio(data, *_): mic.extend(data)
        finished = []
        async def handle_finished(msg): finished.append(msg.success)
        cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio,
                                      handle_announcement_finished=handle_finished)
        await asyncio.sleep(0.3)

        proc.send_signal(signal.SIGUSR1)                                           # "wake word"
        await asyncio.wait_for(started.wait(), 5)
        check(handle_start.args == (1, "Alexa"), f"pipeline request: flags, phrase = {handle_start.args}")
        check(handle_start.audio == (3, 15, 2.5), f"audio settings travel with the request: {handle_start.audio}")
        await asyncio.sleep(1.0)
        check(len(mic) > 16000, f"mic audio streamed: {len(mic)} bytes in 1 s")

        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
        await asyncio.sleep(0.3); n = len(mic); await asyncio.sleep(0.4)
        check(len(mic) == n, "mic stream stops after STT_VAD_END")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "0"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_START, {"text": "Done"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(2.0)
        check(os.path.getsize(play) == 48000 and finished == [True], f"reply fetched from the TTS_END url and reported: {os.path.getsize(play)} bytes, finished={finished}")

        # streaming TTS: URL arrives with RUN_START, playback may begin at INTENT_PROGRESS, long before TTS_END
        before = os.path.getsize(play); finished.clear(); started.clear()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, {"url": f"http://127.0.0.1:{HTTP_PORT}/stream.wav"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "tell me a story"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"})
        await asyncio.sleep(0.4)
        early = os.path.getsize(play) - before
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/stream.wav"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(1.5)
        check(early > 0 and os.path.getsize(play) - before == 48000 and finished == [True],
              f"streaming reply starts at INTENT_PROGRESS and plays once: early={early}, total={os.path.getsize(play) - before}, finished={finished}")

        before = os.path.getsize(play)
        res = await cli.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "hello",
                                                                         f"http://127.0.0.1:{HTTP_PORT}/chime.wav")
        check(res.success, "announcement finished with success")
        check(os.path.getsize(play) - before == 2 * 48000, f"chime + announcement played: {os.path.getsize(play) - before} bytes")
        check(any(isinstance(s, MediaPlayerEntityState) and int(s.state) == 2 for s in states), "media player reported PLAYING")

        before = os.path.getsize(play)
        res = await cli.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/tts.mp3", 15, "mp3")
        got = os.path.getsize(play) - before
        check(res.success and 44000 <= got <= 52000, f"MP3 announcement decoded and played: {got} bytes (1 s at 24 kHz = 48000)")

        res = await cli.send_voice_assistant_announcement_await_response("http://127.0.0.1:1/none.wav", 15, "x")
        check(not res.success, "unreachable announcement URL reports failure")

        cli.media_player_command(mp[0].key, volume=0.3)
        await asyncio.sleep(0.5)
        check(any(isinstance(s, MediaPlayerEntityState) and abs(s.volume - 0.3) < 0.01 for s in states), "volume command reflected in state")

        cli.switch_command(by["mute"].key, True); await asyncio.sleep(0.3)
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.sleep(0.6)
        check(not started.is_set(), "mute switch blocks triggers")
        cli.switch_command(by["mute"].key, False); await asyncio.sleep(0.3)

        cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t1", "tea", 60, 0, False)
        await asyncio.sleep(0.5)
        proc.send_signal(signal.SIGUSR1)                                           # button press silences the alarm, no pipeline
        started.clear(); await asyncio.sleep(0.5)
        check(not started.is_set(), "trigger during alarm only stops the alarm")

        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "stt-no-text-recognized", "message": "nothing heard"})
        await asyncio.sleep(0.5)
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(True, "new pipeline after an error")
        await cli.disconnect()
    finally:
        proc.terminate(); httpd.shutdown()
        for f in (play, settings):
            if os.path.exists(f): os.unlink(f)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

asyncio.run(main())
