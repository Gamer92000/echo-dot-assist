#!/usr/bin/env python3
"""Plays Home Assistant's side of the ESPHome native API against build/hassmic-host, using the reference
`aioesphomeapi` client (the library Home Assistant itself uses), so framing and protobuf layout are checked by the real parser."""
import asyncio, base64, io, math, os, random, re, signal, struct, subprocess, sys, tempfile, threading, time, wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from aioesphomeapi import SelectInfo, SelectState, NumberInfo, SwitchInfo, NumberState, SwitchState, TextSensorInfo, TextSensorState, SensorInfo, SensorState
from aioesphomeapi import APIClient, MediaPlayerInfo, MediaPlayerEntityState, VoiceAssistantEventType as Ev, VoiceAssistantTimerEventType as Tm
from aioesphomeapi import ZERO_NOISE_PSK, EventInfo, BinarySensorInfo, BinarySensorState
from aioesphomeapi.model import Event
from aioesphomeapi.core import InvalidEncryptionKeyAPIError, RequiresEncryptionAPIError

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import Browser
PORT, HTTP_PORT, WEB = 16953, 16954, 16955


def tone(rate, seconds, freq=440):
    return b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * freq * i / rate))) for i in range(int(rate * seconds)))


def wav_bytes(rate, seconds):
    b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(tone(rate, seconds))
    return b.getvalue()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = wav_bytes(48000, 3.0 if "s=3" in self.path else 0.5)
        if self.path.endswith(".mp3"):                  # what Home Assistant sends for a TTS announcement before any pipeline ran
            body = subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=1", "-ar", "24000",
                                   "-ac", "1", "-f", "mp3", "-"], capture_output=True, check=True).stdout
        self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.end_headers()   # no length: like a transcoding proxy
        if "late" in self.path:                         # streamed TTS while the LLM still works: the audio comes later
            self.wfile.flush(); time.sleep(6)
        try: self.wfile.write(body)
        except OSError: pass                            # the Echo hung up
    def log_message(self, *a): pass


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def main():
    play = tempfile.mktemp(suffix=".raw")
    settings = tempfile.mktemp(suffix=".settings")
    state = tempfile.mkdtemp(); mdns = os.path.join(state, "hassmic.service")
    with open(settings, "w") as f: f.write("3 9 4.00 0 1 1 0 en\n")         # from before the mic level: gain values for HA
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_SETTINGS=settings, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=play,
               HASSMIC_MDNS_FILE=mdns, HASSMIC_ARB_ADDR="127.255.255.255",      # arbitration beacons stay on this PC
               HASSMIC_MODELS=os.path.join(state, "models"),
               HASSMIC_ADB_OPEN=os.path.join(state, "adb-open.root"),          # on the Echo: in a directory only root writes
               HASSMIC_LUX=os.path.join(state, "calibrated_lux"),              # the light sensor's sysfs file
               HASSMIC_FAKE_SOUND="dogBark",                                    # every ~10 s window "hears" a dog (sound_none.c)
               HASSMIC_FAKE_WHISPER="1",                                        # a model, and every request whispered (whisper_none.c)
               HASSMIC_FAKE_WIFI=os.path.join(state, "rx_stat"),                # what the Wi-Fi driver answers RX_STAT (wifimotion.c)
               HASSMIC_CLOCK_SYNCED="0")                                        # an Echo whose clock nobody set (clock.c)
    def conf():                                      # state/config: name=value lines (settings.c)
        try: return dict(l.strip().split("=", 1) for l in open(os.path.join(state, "config")) if "=" in l and not l.startswith("#"))
        except OSError: return {}
    rx_stat = lambda rcpi: open(env["HASSMIC_FAKE_WIFI"], "w").write(f"RX Stat:\nRX SNR (dB)          = 32\nRCPI RX0             = {rcpi}\n")
    rx_stat(112)
    with open(env["HASSMIC_LUX"], "w") as f: f.write("67\n")
    for m in ("echo-de", "computer-en-US"):             # installed wake word models (the PC build loads none of them)
        os.makedirs(os.path.join(state, "models", m)); open(os.path.join(state, "models", m, "pryon.manifest"), "w").close()
    with open(mdns, "w") as f:                          # what main.sh does at boot
        subprocess.run([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-S"], env=env, stdout=f, check=True)
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-L", "-W", str(WEB)], env=env)
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    await asyncio.sleep(0.5)
    try:
        page = Browser(WEB)                                 # the settings page: what is no longer in Home Assistant
        check(page.login_with_button(proc), "settings page: logged in with the action button")
        pv = lambda name: next((x["value"] for x in page.state()["settings"] if x["name"] == name), None)
        cli = APIClient("127.0.0.1", PORT, None)
        await cli.connect(login=True)
        info = await cli.device_info()
        check(info.name == "echo-dot" and info.friendly_name == "Echo Dot", f"device info: {info.name!r} / {info.friendly_name!r}")
        check(info.voice_assistant_feature_flags == 61, f"voice assistant feature flags = {info.voice_assistant_feature_flags}")
        check(info.webserver_port == WEB, f"device info: settings page port {info.webserver_port} (Home Assistant's \"Visit\" link)")
        entities, _ = await cli.list_entities_services()
        mp = [e for e in entities if isinstance(e, MediaPlayerInfo)]
        check(len(mp) == 1 and len(mp[0].supported_formats) == 2 and mp[0].supported_formats[1].sample_rate == 48000,
              f"media player entity with formats: {[(f.format, f.sample_rate, int(f.purpose)) for f in mp[0].supported_formats] if mp else None}")
        states = []
        cli.subscribe_states(states.append)
        by = {e.object_id: e for e in entities}
        check("noise_suppression_level" not in by and "auto_gain" not in by and "mic_volume_multiplier" not in by
              and "mic_level" not in by and "noise_reduction" not in by
              and isinstance(by.get("mute"), SwitchInfo) and isinstance(by.get("wake_sound"), SwitchInfo), "settings entities listed: the lean set")
        ps = {x["name"]: x for x in page.state()["settings"]}
        check((ps["mic_level"]["min"], ps["mic_level"]["max"]) == (-35, -15) and ps["noise_reduction"]["choices"] == ["off", "low", "medium", "high"],
              "mic level and noise reduction on the settings page")
        tok = by.get("sendspin_pairing_token")
        check(isinstance(tok, TextSensorInfo) and tok.disabled_by_default and int(tok.entity_category) == 2, "Sendspin pairing token entity: diagnostic, disabled by default")
        check("soc_temperature" not in by and "cpu_usage" not in by and page.state()["diag"]["cpu"] is not None,
              "diagnostics on the settings page, not in Home Assistant")
        await asyncio.sleep(0.3)
        check(pv("mic_level") == -26, f"mic level at its default after a settings file from before it: {pv('mic_level')}")
        eqs = [by.get(k) for k in ("equalizer_bass", "equalizer_mid", "equalizer_treble")]
        await asyncio.sleep(0.3)
        check(all(isinstance(e, NumberInfo) and e.min_value == -6 and e.max_value == 6 and e.step == 1 and e.unit_of_measurement == "dB" for e in eqs)
              and any(isinstance(x, NumberState) and x.key == eqs[0].key and x.state == 0 for x in states), "equalizer entities listed, flat on PC")
        cli.number_command(eqs[0].key, 4); cli.number_command(eqs[2].key, -9)
        await asyncio.sleep(0.5)
        check(any(isinstance(x, NumberState) and x.key == eqs[0].key and x.state == 4 for x in states)
              and any(isinstance(x, NumberState) and x.key == eqs[2].key and x.state == -6 for x in states), "equalizer commands reflected, clamped to -6..+6")
        check(pv("noise_reduction") == 0, "noise reduction off by default")
        lux, lauto, lbright = by.get("illuminance"), by.get("led_auto_brightness"), by.get("led_brightness")
        check(isinstance(lux, SensorInfo) and lux.device_class == "illuminance" and lux.unit_of_measurement == "lx" and int(lux.state_class) == 1
              and isinstance(lauto, SwitchInfo) and isinstance(lbright, NumberInfo) and (lbright.min_value, lbright.max_value) == (0, 100),
              "light sensor and LED brightness entities listed")
        last = lambda k, t: ([x.state for x in states if isinstance(x, t) and x.key == k] or [None])[-1]
        check(last(lux.key, SensorState) == 67 and last(lauto.key, SwitchState) is True and last(lbright.key, NumberState) == 80,
              f"illuminance from the sensor file, auto brightness on as in stock: {last(lux.key, SensorState)} lx, "
              f"auto {last(lauto.key, SwitchState)}, level {last(lbright.key, NumberState)}")
        page.set(mic_level=-20)
        cli.switch_command(by["wake_sound"].key, False); await asyncio.sleep(0.5)
        check(pv("mic_level") == -20 and any(isinstance(x, SwitchState) and x.key == by["wake_sound"].key and x.state is False for x in states),
              "setting commands reflected in state (page and Home Assistant)")
        cli.switch_command(by["wake_sound"].key, True); await asyncio.sleep(0.3)
        want = subprocess.run([f"{ROOT}/build/hassmic-host", "-T"], env=env, capture_output=True, text=True).stdout.strip()
        got = [x.state for x in states if isinstance(x, TextSensorState) and x.key == tok.key]
        check(got == [want] and want.startswith("SP:0") and len(want) > 100, f"token state equals `hassmic -T`: {want[:16]}…")
        wu = by.get("web_ui_address")
        check("ip_address" not in by and isinstance(wu, TextSensorInfo) and not wu.disabled_by_default and int(wu.entity_category) == 2
              and [x.state for x in states if isinstance(x, TextSensorState) and x.key == wu.key] == [f"http://127.0.0.1:{WEB}"],
              "Web UI address entity: diagnostic, enabled, the settings page at the address Home Assistant connected to")
        apeers = by.get("arbitration_peers")
        check("arbitration_id" not in by and "join_arbitration_network" not in by and pv("arbitration") == 1 and isinstance(apeers, SensorInfo),
              "arbitration on by default (the page), peers sensor, no ID entity, no join switch")
        svcs = (await cli.list_entities_services())[1]
        check([(v.name, [(x.name, int(x.type)) for x in v.args]) for v in svcs] == [("arbitration_key", [("network", 3), ("key", 3)])],
              "action \"arbitration_key\" (network, key: strings) for other Echos to hand over their network")
        check(conf().get("noise_reduction") == "off" and conf().get("mic_level") == "-20", f"settings persisted: {conf()}")
        cfg = await cli.get_voice_assistant_configuration(5)
        avail = sorted((w.id, w.wake_word, list(w.trained_languages)) for w in cfg.available_wake_words)
        check(avail == [("alexa", "Alexa", ["en"]), ("computer-en-US", "Computer", ["en"]), ("echo-de", "Echo", ["de"])]
              and list(cfg.active_wake_words) == ["alexa"] and cfg.max_active_wake_words == 1, f"wake words: all installed ones offered, Alexa active: {avail}")

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
        check(handle_start.audio == (0, 0, 1.0), f"neutral audio settings in the request (the gain is applied on the Echo): {handle_start.audio}")
        await asyncio.sleep(1.0)
        peak = max(abs(v) for v in struct.unpack(f"<{len(mic) // 2}h", mic[:len(mic) // 2 * 2])) if mic else 0
        check(len(mic) > 16000, f"mic audio streamed: {len(mic)} bytes in 1 s")
        check(8000 < peak <= 29100, f"mic audio brought up to speech level, peaks limited below full scale: peak {peak}")

        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
        await asyncio.sleep(0.3); n = len(mic); await asyncio.sleep(0.4)
        check(len(mic) == n, "mic stream stops after STT_VAD_END")
        wh = by.get("last_request_whispered")
        whs = [x for x in states if isinstance(x, BinarySensorState) and wh and x.key == wh.key]
        check(isinstance(wh, BinarySensorInfo) and whs and whs[0].missing_state and whs[-1].state is True and not whs[-1].missing_state,
              f"\"Last request whispered\": unknown until a request, on at its VAD end, before the transcript: {[(x.state, x.missing_state) for x in whs]}")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "0"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_START, {"text": "Done"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(2.0)
        check(os.path.getsize(play) == 48000 and finished == [True], f"reply fetched from the TTS_END url and reported: {os.path.getsize(play)} bytes, finished={finished}")

        # another wake word, picked in Home Assistant: kept, and named in the pipeline request (HA's duplicate check keys on it)
        await cli.set_voice_assistant_configuration(["echo-de"]); await asyncio.sleep(0.5)
        cfg = await cli.get_voice_assistant_configuration(5)
        saved = open(os.path.join(state, "wake_word")).read().strip()
        check(list(cfg.active_wake_words) == ["echo-de"] and saved == "echo-de", f"wake word switched from Home Assistant and kept: {list(cfg.active_wake_words)}, {saved}")
        started.clear(); proc.send_signal(signal.SIGUSR1)
        await asyncio.wait_for(started.wait(), 5)
        check(handle_start.args == (1, "Echo"), f"pipeline request names the new wake word: {handle_start.args}")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await asyncio.sleep(0.5)
        await cli.set_voice_assistant_configuration(["alexa"]); await asyncio.sleep(0.3)

        # A second client beside "Home Assistant": gets answers, sees and causes state changes, cannot take the voice assistant.
        cli2 = APIClient("127.0.0.1", PORT, None)
        await asyncio.wait_for(cli2.connect(login=True), 5)
        info2 = await asyncio.wait_for(cli2.device_info(), 5)
        ent2, _ = await cli2.list_entities_services()
        check(info2.name == "echo-dot" and len(ent2) == len(entities), "second client is served while the first stays connected")
        states2 = []; cli2.subscribe_states(states2.append)
        started2 = asyncio.Event()
        async def start2(*a): started2.set(); return 0
        async def stop2(*a): pass
        cli2.subscribe_voice_assistant(handle_start=start2, handle_stop=stop2)
        await asyncio.sleep(0.3); n1 = len(states)
        cli2.switch_command(by["wake_sound"].key, False)
        await asyncio.sleep(0.5)
        ws = lambda sts: any(isinstance(x, SwitchState) and x.key == by["wake_sound"].key and x.state is False for x in sts)
        check(ws(states[n1:]) and ws(states2), "a change made by one client reaches both")
        cli2.switch_command(by["wake_sound"].key, True); await asyncio.sleep(0.3)
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(not started2.is_set(), "the voice assistant stays with the first subscriber")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await cli2.disconnect(); await asyncio.sleep(0.5)
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(True, "first client unaffected when the second one leaves")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await asyncio.sleep(0.5)

        # A reply that asks a follow-up question (continue_conversation) must still be interruptible by the wake word:
        # the reply is cut and the new pipeline starts at once, not after the full second of audio.
        before = os.path.getsize(play); finished.clear(); started.clear()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "which light"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/long.wav?s=3"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(0.6); started.clear()
        t0 = asyncio.get_running_loop().time(); proc.send_signal(signal.SIGUSR1)       # "Alexa" while it talks
        await asyncio.wait_for(started.wait(), 5); dt = asyncio.get_running_loop().time() - t0
        played = (os.path.getsize(play) - before) / 96000
        check(dt < 1.0 and played < 2.0, f"wake word interrupts a continue-conversation reply: new pipeline after {dt:.2f} s, {played:.2f} s of 3 s played")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await asyncio.sleep(0.5)

        # "Stop" (SIGHUP here), the second keyword of the wake word model: ends what makes noise, never starts a pipeline.
        started.clear(); proc.send_signal(signal.SIGHUP); await asyncio.sleep(0.6)
        check(not started.is_set(), '"stop" out of silence starts nothing')

        async def reply_3s():               # a 3 s reply that would listen again afterwards
            proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
            cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "which light"})
            cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
            cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/long.wav?s=3"})
            cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
            await asyncio.sleep(0.6); started.clear()

        before = os.path.getsize(play); started.clear(); await reply_3s()
        proc.send_signal(signal.SIGHUP); await asyncio.sleep(1.5)
        played = (os.path.getsize(play) - before) / 96000
        check(not started.is_set() and played < 2.0, f'"stop" cuts a reply and nothing listens afterwards: {played:.2f} s of 3 s played')

        before = os.path.getsize(play); started.clear(); await reply_3s()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)    # "<wake word>, stop": wake word cuts and listens,
        stopped.clear(); started.clear(); proc.send_signal(signal.SIGHUP); await asyncio.sleep(0.8)  # "stop" drops that pipeline
        played = (os.path.getsize(play) - before) / 96000
        check(stopped and not started.is_set() and played < 2.0, f'"<wake word>, stop" during a reply: pipeline dropped, {played:.2f} s of 3 s played (stops {stopped}, restarted {started.is_set()})')
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "stt-no-text-recognized", "message": "dropped"})
        await asyncio.sleep(0.4)
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(True, "wake word works again after a dropped pipeline")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await asyncio.sleep(0.5)

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

        dnd = by.get("do_not_disturb")
        check(isinstance(dnd, SwitchInfo), "do not disturb switch listed")
        cli.switch_command(dnd.key, True); await asyncio.sleep(0.3)
        check(any(isinstance(s, SwitchState) and s.key == dnd.key and s.state for s in states)
              and conf().get("do_not_disturb") == "on", f"do not disturb on and persisted: {conf()}")
        check(conf().get("bluetooth_announcement_language", "en") == "en", f"Bluetooth announcement language persisted as its code: {conf()}")
        before = os.path.getsize(play)
        res = await cli.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
        check(not res.success and os.path.getsize(play) == before, "do not disturb drops announcements")
        cli.switch_command(dnd.key, False); await asyncio.sleep(0.3)
        res = await cli.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
        check(res.success and os.path.getsize(play) - before == 48000, "announcements play again once it is off")

        cli.media_player_command(mp[0].key, volume=0.3)
        await asyncio.sleep(0.5)
        check(any(isinstance(s, MediaPlayerEntityState) and abs(s.volume - 0.3) < 0.01 for s in states), "volume command reflected in state")

        cli.switch_command(by["mute"].key, True); await asyncio.sleep(0.3)
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.sleep(0.6)
        check(not started.is_set(), "mute switch blocks triggers")
        cli.switch_command(by["mute"].key, False); await asyncio.sleep(0.3)

        # Debug access: on the settings page only, with a press of its own (tests/fake_web.py)
        check("debug_access_adb" not in by, "no debug access switch in Home Assistant")

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
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await asyncio.sleep(0.3)

        # Encryption, in the order Home Assistant runs it: plaintext connection, DeviceInfo says "supported, provisionable",
        # key sent over a zero-PSK Noise connection, then only that key gets in.
        info = await cli.device_info()
        check(info.api_encryption_supported and info.api_encryption_provisionable, "device info: encryption supported and provisionable")
        check("api_encryption_supported=Noise_NNpsk0" in open(mdns).read() and "api_encryption=" not in open(mdns).read(), "mDNS without a key: api_encryption_supported only")
        key = base64.b64encode(os.urandom(32))
        check(await asyncio.wait_for(cli.noise_encryption_set_key(key), 5) is False, "key refused over plaintext")
        try:
            bad = APIClient("127.0.0.1", PORT, None, noise_psk=base64.b64encode(os.urandom(32)).decode()); await asyncio.wait_for(bad.connect(), 5); ok = False
        except InvalidEncryptionKeyAPIError as e: ok = e.received_name == "echo-dot"
        check(ok, "a random key before provisioning: invalid key, server hello names the device")
        prov = APIClient("127.0.0.1", PORT, None, noise_psk=ZERO_NOISE_PSK)
        await asyncio.wait_for(prov.connect(), 5)
        check(await asyncio.wait_for(prov.noise_encryption_set_key(key), 5) is True, "key accepted over the zero-PSK connection")
        await prov.disconnect()
        check(open(os.path.join(state, "api_key")).read().strip() == key.decode() and oct(os.stat(os.path.join(state, "api_key")).st_mode & 0o777) == "0o600",
              "key stored in state/api_key, mode 600")
        check("api_encryption=Noise_NNpsk0" in open(mdns).read(), "mDNS service file rewritten: api_encryption")
        try: await asyncio.wait_for(cli.device_info(), 3); ok = False
        except Exception: ok = True
        check(ok, "the plaintext connection from before is closed on its next request")
        for psk, err, what in ((None, RequiresEncryptionAPIError, "plaintext"), (ZERO_NOISE_PSK, InvalidEncryptionKeyAPIError, "zero PSK")):
            try: c = APIClient("127.0.0.1", PORT, None, noise_psk=psk); await asyncio.wait_for(c.connect(), 5); ok = False
            except err: ok = True
            check(ok, f"with a key: {what} connection refused ({err.__name__})")

        enc = APIClient("127.0.0.1", PORT, None, noise_psk=key.decode())
        await asyncio.wait_for(enc.connect(login=True), 5)
        info = await enc.device_info()
        check(info.name == "echo-dot" and info.api_encryption_supported and not info.api_encryption_provisionable, "encrypted: device info, no longer provisionable")
        ent3, _ = await enc.list_entities_services()
        check(len(ent3) == len(entities), "encrypted: entities listed")
        # the time: asked of Home Assistant over the keyed link only (aioesphomeapi answers it), handed to root
        clock = os.path.join(state, "clock")
        check(not os.path.exists(clock), "no time taken from the plaintext connection before the key")
        enc.subscribe_states(lambda s: None)
        for _ in range(30):
            if os.path.exists(clock): break
            await asyncio.sleep(0.1)
        try: t, at = open(clock).read().split(); t = int(t)
        except (OSError, ValueError): t, at = 0, ""
        boot = time.clock_gettime(time.CLOCK_BOOTTIME)
        check(abs(t - time.time()) <= 2 and re.fullmatch(r"\d+\.\d\d", at) and 0 <= boot - float(at) < 5,
              f"keyed link: Home Assistant's time in state/clock for root, with the boot clock as /proc/uptime words it: {t} {at}")
        started.clear(); mic.clear()
        enc.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio, handle_announcement_finished=handle_finished)
        await asyncio.sleep(0.3); proc.send_signal(signal.SIGUSR1)
        await asyncio.wait_for(started.wait(), 5); await asyncio.sleep(1.0)
        check(len(mic) > 16000, f"encrypted: mic audio streamed: {len(mic)} bytes in 1 s")
        enc.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await asyncio.sleep(0.3)
        page.set(noise_reduction="medium"); await asyncio.sleep(0.3)
        check(conf().get("noise_reduction") == "medium", f"noise reduction set to medium and persisted: {conf()}")
        started.clear(); mic.clear(); proc.send_signal(signal.SIGUSR1)
        await asyncio.wait_for(started.wait(), 5); await asyncio.sleep(1.0)
        peak = max(abs(v) for v in struct.unpack(f"<{len(mic) // 2}h", mic[:len(mic) // 2 * 2])) if mic else 0
        check(len(mic) > 16000 and len(mic) % 320 == 0 and 8000 < peak <= 29100,
              f"with noise reduction: mic audio streamed in whole 10 ms frames at speech level: {len(mic)} bytes in 1 s, peak {peak}")
        enc.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await asyncio.sleep(0.3)
        page.set(noise_reduction="off"); await asyncio.sleep(0.3)
        before = os.path.getsize(play)
        res = await enc.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
        check(res.success and os.path.getsize(play) - before == 48000, "encrypted: announcement played")

        check(await asyncio.wait_for(enc.noise_encryption_set_key(b""), 5) is True and not os.path.exists(os.path.join(state, "api_key")),
              "empty key from the keyed connection clears it (Home Assistant deleting the device)")
        check("api_encryption_supported=" in open(mdns).read(), "mDNS back to api_encryption_supported")
        await enc.disconnect()
        c = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(c.connect(login=True), 5)
        check((await c.device_info()).api_encryption_provisionable, "plaintext accepted again, provisionable again")
        st3 = []; c.subscribe_states(st3.append); await asyncio.sleep(0.3)    # here: the sleeps would move the mic checks' place in the capture loop
        last3 = lambda k, t: ([x.state for x in st3 if isinstance(x, t) and x.key == k] or [None])[-1]
        with open(env["HASSMIC_LUX"], "w") as f: f.write("68\n")                # flicker: not sent
        await asyncio.sleep(1.5)
        with open(env["HASSMIC_LUX"], "w") as f: f.write("150\n")
        await asyncio.sleep(1.5)
        sent = [x.state for x in st3 if isinstance(x, SensorState) and x.key == lux.key]
        check(sent == [67, 150], f"illuminance sent on a real change, not on flicker: {sent}")
        c.number_command(lbright.key, 30)
        await asyncio.sleep(0.5)
        check(last3(lbright.key, NumberState) == 30 and last3(lauto.key, SwitchState) is False, "a fixed LED level switches auto brightness off")
        check(conf().get("led_auto_brightness") == "off" and conf().get("led_brightness") == "30", f"LED brightness kept in the settings file: {conf()}")
        c.switch_command(lauto.key, True)
        await asyncio.sleep(0.5)
        check(last3(lauto.key, SwitchState) is True and conf().get("led_auto_brightness") == "on", "auto brightness switched on again")
        # A feature switched on the page closes the link; Home Assistant comes back and lists entities again
        async def relist(old):
            try: await old.disconnect()
            except Exception: pass
            await asyncio.sleep(0.5)
            n = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(n.connect(login=True), 5)
            e, _ = await n.list_entities_services(); sts = []; n.subscribe_states(sts.append); await asyncio.sleep(0.3)
            return n, {x.object_id: x for x in e}, sts
        # sound detection: off by default, no entity; on, one event entity reporting what the detector hears (here
        # sound_none.c's dog, once per ~10 s window of the capture), kept in the settings file
        check("sound" not in by and "sound_detection" not in by and pv("sound_detection") == 0, "sound detection off by default: no entity in Home Assistant")
        page.set(sound_detection="on")
        c, cby, st3 = await relist(c)
        ev = cby.get("sound")
        check(isinstance(ev, EventInfo)
              and list(ev.event_types) == ["smoke_or_co_alarm", "glass_break", "dog_bark", "baby_cry", "snoring", "cough", "water", "beeping_appliance"]
              and conf().get("sound_detection") == "on", f"sound detection switched on on the page: the event entity, kept in the settings file: {conf()}")
        for _ in range(26):
            if [x for x in st3 if isinstance(x, Event)]: break
            await asyncio.sleep(0.5)
        evs = [(x.key, x.event_type) for x in st3 if isinstance(x, Event)]
        check(evs[:1] == [(ev.key, "dog_bark")], f"the detector's dogBark arrives as event dog_bark within a window: {evs}")
        # a window in which the Echo played something itself is dropped: here an announcement
        n0 = len(evs)
        async def no_pipeline(*a): return 0
        async def nothing(*a): pass
        unsub = c.subscribe_voice_assistant(handle_start=no_pipeline, handle_stop=nothing, handle_audio=nothing,
                                    handle_announcement_finished=nothing)      # announcements answer the assistant's client
        await asyncio.sleep(0.3)
        res = await c.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
        await asyncio.sleep(9)
        evs = [(x.key, x.event_type) for x in st3 if isinstance(x, Event)]
        check(res.success and len(evs) == n0, f"no sound event for the window with the announcement in it: {evs[n0:]}")
        for _ in range(50):
            if len([x for x in st3 if isinstance(x, Event)]) > n0: break
            await asyncio.sleep(0.5)
        check(len([x for x in st3 if isinstance(x, Event)]) > n0, "sound events again once the Echo has been quiet for a window")
        unsub()
        page.set(sound_detection="off")
        c, cby, st3 = await relist(c)
        check("sound" not in cby and conf().get("sound_detection") == "off", "sound detection switched off again: the entity gone")
        # Wi-Fi motion (experimental): off by default, the sensor unknown while off; on, a still level is no motion, a
        # wobbling one is, and it clears after the hold (3 s in the PC build, 30 s on the Echo)
        check("wifi_motion" not in cby and "wifi_motion_detection" not in cby and pv("wifi_motion") == 0 and pv("wifi_motion_sensitivity") == 5,
              "Wi-Fi motion off by default (sensitivity 5): no entity in Home Assistant")
        page.set(wifi_motion="on")
        c, cby, st3 = await relist(c)
        wbs, wsn = cby.get("wifi_motion"), cby.get("wifi_motion_sensitivity")
        check(isinstance(wbs, BinarySensorInfo) and wbs.device_class == "motion" and "experimental" in wbs.name
              and isinstance(wsn, NumberInfo) and (wsn.min_value, wsn.max_value) == (1, 10) and int(wsn.entity_category) == 1,
              "Wi-Fi motion on: its entities listed, named experimental")
        wstate = lambda: ([x for x in st3 if isinstance(x, BinarySensorState) and x.key == wbs.key] or [None])[-1]
        await asyncio.sleep(3)
        check(wstate() and not wstate().missing_state and wstate().state is False
              and conf().get("wifi_motion") == "on" and conf().get("wifi_motion_sensitivity") == "5",
              f"switched on: a steady level is no motion, kept in the settings file: {conf()}")
        for _ in range(60):                                 # someone walking through the path: 6 s
            rx_stat(random.randint(106, 118)); await asyncio.sleep(0.1)
            if wstate().state: break
        check(wstate().state is True, "a wobbling level is motion")
        rx_stat(112)
        await asyncio.sleep(7)
        check(wstate().state is False, "motion clears after the hold")
        c.number_command(wsn.key, 8)
        await asyncio.sleep(0.5)
        check(last3(wsn.key, NumberState) == 8 and conf().get("wifi_motion_sensitivity") == "8", "sensitivity set and kept")
        page.set(wifi_motion="off")
        c, cby, st3 = await relist(c)
        check("wifi_motion" not in cby and conf().get("wifi_motion") == "off", "switched off again: the entities gone")
        # the real assistant again, with mic and replies
        c.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio,
                                    handle_announcement_finished=handle_finished)
        await asyncio.sleep(0.3)
        # (at the end: these sleeps would move the mic checks' place in the capture loop)
        # The action button (SIGUSR2) while Home Assistant thinks: the run is aborted (as a Voice PE's button does), and what
        # Home Assistant still sends for it plays nothing.  The wake word instead aborts and listens again.
        async def thinking():
            proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
            c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
            c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn off everything"})
            await asyncio.sleep(0.3); stopped.clear(); started.clear()
        def late_reply():                    # the aborted run's reply, already on the way
            c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
            c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        before = os.path.getsize(play); started.clear(); await thinking()
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.5)
        late_reply(); await asyncio.sleep(1.0)
        check(stopped == [True] and not started.is_set() and os.path.getsize(play) == before,
              f"button while thinking: run aborted, nothing listens, its late reply not played (stops {stopped}, {os.path.getsize(play) - before} bytes played)")
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(True, "wake word works again after a cancelled run")
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await asyncio.sleep(0.5)

        before = os.path.getsize(play); started.clear(); await thinking()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        late_reply(); await asyncio.sleep(1.0)
        check(stopped == [True] and os.path.getsize(play) == before,
              f"wake word while thinking: run aborted, new one started, the old reply not played (stops {stopped}, {os.path.getsize(play) - before} bytes played)")
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(2.0)
        check(os.path.getsize(play) - before == 48000, f"the new run's reply plays: {os.path.getsize(play) - before} bytes")

        # Streamed TTS: the reply's fetch already waits on Home Assistant (first words out, tool calls still running) when
        # the wake word cancels.  The fetch is cut: the new run's reply is not refused as busy, the old one never plays.
        before = os.path.getsize(play); started.clear()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, {"url": f"http://127.0.0.1:{HTTP_PORT}/late.wav"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn off everything"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"})
        await asyncio.sleep(0.5); stopped.clear(); started.clear()
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await asyncio.sleep(2.0)
        check(stopped == [True] and os.path.getsize(play) - before == 48000,
              f"wake word while a streamed reply is fetched: fetch cut, the new run's reply plays (stops {stopped}, {os.path.getsize(play) - before} bytes)")
        await asyncio.sleep(6)
        check(os.path.getsize(play) - before == 48000, f"the cancelled run's reply never plays: {os.path.getsize(play) - before} bytes")
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        check(True, "and the wake word works after it")
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
        await asyncio.sleep(0.5)
        await c.disconnect()
    finally:
        proc.terminate(); httpd.shutdown()
        for f in (play, settings):
            if os.path.exists(f): os.unlink(f)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

asyncio.run(main())
