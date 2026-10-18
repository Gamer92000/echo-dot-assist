#!/usr/bin/env python3
"""Plays Home Assistant's side of the ESPHome native API against build/hassmic-host (tests/esphome_common.py): device
info, entities and settings, the voice assistant (mic stream, wake words, replies, a second client, barge-in, "stop",
streaming TTS), announcements over HTTP and HTTPS, do not disturb, volume, mute.  The rest of the API in
fake_ha_esphome_{timers,features,buttons,address}.py."""
import asyncio, os, signal, subprocess
from aioesphomeapi import SwitchInfo, NumberInfo, NumberState, SwitchState, TextSensorInfo, TextSensorState, SensorInfo
from aioesphomeapi import APIClient, MediaPlayerInfo, MediaPlayerEntityState, VoiceAssistantEventType as Ev, BinarySensorInfo, BinarySensorState
from esphome_common import ROOT, check, until, sync, peak, speech, settled, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 16953, 16954, 16955, 16966, 16967


async def main(e):
    proc, state, env, play, page, conf, pv = e.proc, e.state, e.env, e.play, e.page, e.conf, e.pv
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
    check(pv("mic_level") == -26, f"mic level at its default after a settings file from before it: {pv('mic_level')}")
    eqs = [by.get(k) for k in ("equalizer_bass", "equalizer_mid", "equalizer_treble")]
    await until(lambda: any(isinstance(x, NumberState) and x.key == eqs[0].key for x in states))
    check(all(isinstance(e, NumberInfo) and e.min_value == -6 and e.max_value == 6 and e.step == 1 and e.unit_of_measurement == "dB" for e in eqs)
          and any(isinstance(x, NumberState) and x.key == eqs[0].key and x.state == 0 for x in states), "equalizer entities listed, flat on PC")
    cli.number_command(eqs[0].key, 4); cli.number_command(eqs[2].key, -9)
    await until(lambda: any(isinstance(x, NumberState) and x.key == eqs[0].key and x.state == 4 for x in states)
                and any(isinstance(x, NumberState) and x.key == eqs[2].key and x.state == -6 for x in states))
    check(any(isinstance(x, NumberState) and x.key == eqs[0].key and x.state == 4 for x in states)
          and any(isinstance(x, NumberState) and x.key == eqs[2].key and x.state == -6 for x in states), "equalizer commands reflected, clamped to -6..+6")
    check(pv("noise_reduction") == 0, "noise reduction off by default")
    lux, lauto, lbright = by.get("illuminance"), by.get("led_auto_brightness"), by.get("led_brightness")
    check(lux is None and pv("light_sensor") == 0
          and isinstance(lauto, SwitchInfo) and isinstance(lbright, NumberInfo) and (lbright.min_value, lbright.max_value) == (0, 100),
          "LED brightness entities listed, the light sensor off by default (no illuminance)")
    last = lambda k, t: ([x.state for x in states if isinstance(x, t) and x.key == k] or [None])[-1]
    await until(lambda: last(lbright.key, NumberState) is not None)
    check(last(lauto.key, SwitchState) is True and last(lbright.key, NumberState) == 80,
          f"auto brightness on as in stock: auto {last(lauto.key, SwitchState)}, level {last(lbright.key, NumberState)}")
    page.set(mic_level=-20)
    cli.switch_command(by["wake_sound"].key, False)
    await until(lambda: last(by["wake_sound"].key, SwitchState) is False and pv("mic_level") == -20)
    check(pv("mic_level") == -20 and any(isinstance(x, SwitchState) and x.key == by["wake_sound"].key and x.state is False for x in states),
          "setting commands reflected in state (page and Home Assistant)")
    cli.switch_command(by["wake_sound"].key, True); await until(lambda: last(by["wake_sound"].key, SwitchState) is True)
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
    check(sorted((v.name, [(x.name, int(x.type)) for x in v.args]) for v in svcs) == [("arbitration_key", [("network", 3), ("key", 3)]), ("drop_in", [("target", 3)])],
          "actions \"arbitration_key\" (network, key: strings) for other Echos to hand over their network, \"drop_in\" (target: string)")
    check(conf().get("noise_reduction") == "off" and conf().get("mic_level") == "-20", f"settings persisted: {conf()}")
    cfg = await cli.get_voice_assistant_configuration(5)
    avail = sorted((w.id, w.wake_word, list(w.trained_languages)) for w in cfg.available_wake_words)
    check(avail == [("alexa", "Alexa", ["en"]), ("alexa-en-US", "Alexa (en-US)", ["en"]), ("computer-de-DE", "Computer (de-DE)", ["de"]),
                    ("computer-en-US", "Computer (en-US)", ["en"]), ("echo-de", "Echo", ["de"])]
          and list(cfg.active_wake_words) == ["alexa"] and cfg.max_active_wake_words == 2,
          f"wake words: all installed ones offered, a name two share with its region (HA's select keys on names), Alexa active, two may listen: {avail}")

    started = asyncio.Event(); mic = bytearray(); chunks = []; stopped = []
    async def handle_start(conv_id, flags, settings, phrase):
        handle_start.args = (flags, phrase); handle_start.audio = (settings.noise_suppression_level, settings.auto_gain, round(settings.volume_multiplier, 2)); started.set(); return 0           # 0 = audio over the API connection
    async def handle_stop(abort): stopped.append(abort)
    async def handle_audio(data, *_): mic.extend(data); chunks.append(len(data))
    finished = []
    async def handle_finished(msg): finished.append(msg.success)
    cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio,
                                  handle_announcement_finished=handle_finished)
    await sync(cli)

    proc.send_signal(signal.SIGUSR1)                                           # "wake word"
    await asyncio.wait_for(started.wait(), 5)
    check(handle_start.args == (1, "Alexa"), f"pipeline request: flags, phrase = {handle_start.args}")
    check(handle_start.audio == (0, 0, 1.0), f"neutral audio settings in the request (the gain is applied on the Echo): {handle_start.audio}")
    await speech(mic); pk = peak(mic)
    check(len(mic) > 16000, f"mic audio streamed: {len(mic)} bytes")
    # the PC's wake word ended 128 ms before it was heard (as Pryon's do): that much from the ring, then the live 50 ms blocks
    ahead = sum(chunks[:chunks.index(1600)]) if 1600 in chunks else None
    check(ahead == 4096, f"what was said between the wake word's end and its detection sent ahead of the live stream: {ahead} bytes")
    check(8000 < pk <= 29100, f"mic audio brought up to speech level, peaks limited below full scale: peak {pk}")

    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
    await sync(cli); n = len(mic); await asyncio.sleep(0.4)
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
    await until(lambda: os.path.getsize(play) == 48000 and finished == [True])
    check(os.path.getsize(play) == 48000 and finished == [True], f"reply fetched from the TTS_END url and reported: {os.path.getsize(play)} bytes, finished={finished}")

    # another wake word, picked in Home Assistant: kept, and named in the pipeline request (HA's duplicate check keys on it)
    await cli.set_voice_assistant_configuration(["echo-de"]); await sync(cli)
    cfg = await cli.get_voice_assistant_configuration(5)
    saved = open(os.path.join(state, "wake_word")).read().strip()
    check(list(cfg.active_wake_words) == ["echo-de"] and saved == "echo-de", f"wake word switched from Home Assistant and kept: {list(cfg.active_wake_words)}, {saved}")
    started.clear(); proc.send_signal(signal.SIGUSR1)
    await asyncio.wait_for(started.wait(), 5)
    check(handle_start.args == (1, "Echo"), f"pipeline request names the new wake word: {handle_start.args}")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(cli)

    # a second wake word ("Wake word 2"): HA sends the ids in the order its selects were set; the first stays first
    await cli.set_voice_assistant_configuration(["computer-en-US", "echo-de"]); await sync(cli)
    cfg = await cli.get_voice_assistant_configuration(5)
    saved = open(os.path.join(state, "wake_word")).read().split()
    check(list(cfg.active_wake_words) == ["echo-de", "computer-en-US"] and saved == ["echo-de", "computer-en-US"],
          f"two wake words from Home Assistant, the first kept first, both saved: {list(cfg.active_wake_words)}, {saved}")
    started.clear(); proc.send_signal(signal.SIGUSR1)                          # the PC's stand-in is the first one's
    await asyncio.wait_for(started.wait(), 5)
    check(handle_start.args == (1, "Echo"), f"the first one's run names it: {handle_start.args}")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(cli)
    await cli.set_voice_assistant_configuration([]); await sync(cli)           # "No wake word" in both selects
    await cli.set_voice_assistant_configuration(["nope", "computer-en-US", "computer-en-US"]); await sync(cli)
    cfg = await cli.get_voice_assistant_configuration(5)
    check(list(cfg.active_wake_words) == ["computer-en-US"] and open(os.path.join(state, "wake_word")).read().split() == ["computer-en-US"],
          f"none picked: kept; unknown ids and doubles dropped, one left: {list(cfg.active_wake_words)}")
    await cli.set_voice_assistant_configuration(["alexa", "echo-de", "computer-de-DE"]); await sync(cli)
    cfg = await cli.get_voice_assistant_configuration(5)
    check(list(cfg.active_wake_words) == ["alexa", "echo-de"], f"at most two: {list(cfg.active_wake_words)}")
    await cli.set_voice_assistant_configuration(["alexa"]); await sync(cli)

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
    await sync(cli2); n1 = len(states)
    cli2.switch_command(by["wake_sound"].key, False)
    ws = lambda sts: any(isinstance(x, SwitchState) and x.key == by["wake_sound"].key and x.state is False for x in sts)
    await until(lambda: ws(states[n1:]) and ws(states2))
    check(ws(states[n1:]) and ws(states2), "a change made by one client reaches both")
    cli2.switch_command(by["wake_sound"].key, True); await until(lambda: last(by["wake_sound"].key, SwitchState) is True)
    started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    check(not started2.is_set(), "the voice assistant stays with the first subscriber")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
    await cli2.disconnect(); await asyncio.sleep(0.5)
    started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    check(True, "first client unaffected when the second one leaves")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
    await sync(cli)

    # A reply that asks a follow-up question (continue_conversation) must still be interruptible by the wake word:
    # the reply is cut and the new pipeline starts at once, not after the full second of audio.
    before = os.path.getsize(play); finished.clear(); started.clear()
    proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "which light"})
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/long.wav?s=3"})
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await until(lambda: os.path.getsize(play) > before); started.clear()
    t0 = asyncio.get_running_loop().time(); proc.send_signal(signal.SIGUSR1)       # "Alexa" while it talks
    await asyncio.wait_for(started.wait(), 5); dt = asyncio.get_running_loop().time() - t0
    played = (os.path.getsize(play) - before) / 96000
    check(dt < 1.0 and played < 2.0, f"wake word interrupts a continue-conversation reply: new pipeline after {dt:.2f} s, {played:.2f} s of 3 s played")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
    await sync(cli)

    # "Stop" (SIGHUP here), the second keyword of the wake word model: ends what makes noise, never starts a pipeline.
    started.clear(); proc.send_signal(signal.SIGHUP); await asyncio.sleep(0.6)
    check(not started.is_set(), '"stop" out of silence starts nothing')

    async def reply_3s():               # a 3 s reply that would listen again afterwards
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "which light"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/long.wav?s=3"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        n = os.path.getsize(play); await until(lambda: os.path.getsize(play) > n); started.clear()

    before = os.path.getsize(play); started.clear(); await reply_3s()
    proc.send_signal(signal.SIGHUP); await settled(play)
    played = (os.path.getsize(play) - before) / 96000
    check(not started.is_set() and played < 2.0, f'"stop" cuts a reply and nothing listens afterwards: {played:.2f} s of 3 s played')

    before = os.path.getsize(play); started.clear(); await reply_3s()
    proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)    # "<wake word>, stop": wake word cuts and listens,
    stopped.clear(); started.clear(); proc.send_signal(signal.SIGHUP)                           # "stop" drops that pipeline
    await until(lambda: stopped); await settled(play)
    played = (os.path.getsize(play) - before) / 96000
    check(stopped and not started.is_set() and played < 2.0, f'"<wake word>, stop" during a reply: pipeline dropped, {played:.2f} s of 3 s played (stops {stopped}, restarted {started.is_set()})')
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "stt-no-text-recognized", "message": "dropped"})
    await sync(cli)
    started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    check(True, "wake word works again after a dropped pipeline")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
    await sync(cli)

    # streaming TTS: URL arrives with RUN_START, playback may begin at INTENT_PROGRESS, long before TTS_END
    before = os.path.getsize(play); finished.clear(); started.clear()
    proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, {"url": f"http://127.0.0.1:{HTTP_PORT}/stream.wav"})
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "tell me a story"})
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"})
    await until(lambda: os.path.getsize(play) > before, 3)
    early = os.path.getsize(play) - before
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/stream.wav"})
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await until(lambda: os.path.getsize(play) - before >= 48000 and finished)
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

    # Home Assistant serving HTTPS itself (issue #30): its URLs are https://, played through libcurl (https.c)
    check(pv("https_verify") == 1, f"HTTPS certificates are checked by default: {pv('https_verify')}")
    before = os.path.getsize(play)
    res = await cli.send_voice_assistant_announcement_await_response(f"https://127.0.0.1:{HTTPS_PORT}/a.wav", 15, "tls")
    check(res.success and os.path.getsize(play) - before == 48000, f"HTTPS announcement with a valid certificate played: {os.path.getsize(play) - before} bytes")
    before = os.path.getsize(play)
    res = await cli.send_voice_assistant_announcement_await_response(f"https://127.0.0.1:{HTTPS_PORT}/tts.mp3", 15, "tls mp3")
    got = os.path.getsize(play) - before
    check(res.success and 44000 <= got <= 52000, f"HTTPS MP3 announcement played: {got} bytes")
    before = os.path.getsize(play)
    res = await cli.send_voice_assistant_announcement_await_response(f"https://127.0.0.1:{HTTPS_PORT}/moved", 15, "redirect")
    check(res.success and os.path.getsize(play) - before == 48000, "HTTPS redirect followed")
    before = os.path.getsize(play)
    res = await cli.send_voice_assistant_announcement_await_response(f"https://127.0.0.1:{SELF_PORT}/a.wav", 15, "self-signed")
    check(not res.success and os.path.getsize(play) == before, "self-signed certificate refused while checks are on")
    page.set(https_verify="off")
    check(conf().get("https_verify") == "off", f"certificate checks off, persisted: {conf().get('https_verify')}")
    res = await cli.send_voice_assistant_announcement_await_response(f"https://127.0.0.1:{SELF_PORT}/a.wav", 15, "self-signed")
    check(res.success and os.path.getsize(play) - before == 48000, "self-signed certificate taken with checks off")
    page.set(https_verify="on")
    before = os.path.getsize(play)
    res = await cli.send_voice_assistant_announcement_await_response("https://127.0.0.1:1/none.wav", 15, "x")
    check(not res.success and os.path.getsize(play) == before, "unreachable HTTPS URL reports failure")

    dnd = by.get("do_not_disturb")
    check(isinstance(dnd, SwitchInfo), "do not disturb switch listed")
    cli.switch_command(dnd.key, True); await until(lambda: last(dnd.key, SwitchState) is True and conf().get("do_not_disturb") == "on")
    check(any(isinstance(s, SwitchState) and s.key == dnd.key and s.state for s in states)
          and conf().get("do_not_disturb") == "on", f"do not disturb on and persisted: {conf()}")
    check(conf().get("bluetooth_announcement_language", "en") == "en", f"Bluetooth announcement language persisted as its code: {conf()}")
    before = os.path.getsize(play)
    res = await cli.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
    check(not res.success and os.path.getsize(play) == before, "do not disturb drops announcements")
    cli.switch_command(dnd.key, False); await until(lambda: last(dnd.key, SwitchState) is False)
    res = await cli.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
    check(res.success and os.path.getsize(play) - before == 48000, "announcements play again once it is off")

    cli.media_player_command(mp[0].key, volume=0.3)
    await until(lambda: any(isinstance(s, MediaPlayerEntityState) and abs(s.volume - 0.3) < 0.01 for s in states))
    check(any(isinstance(s, MediaPlayerEntityState) and abs(s.volume - 0.3) < 0.01 for s in states), "volume command reflected in state")

    cli.switch_command(by["mute"].key, True); await until(lambda: last(by["mute"].key, SwitchState) is True)
    started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.sleep(0.6)
    check(not started.is_set(), "mute switch blocks triggers")
    cli.switch_command(by["mute"].key, False); await until(lambda: last(by["mute"].key, SwitchState) is False)

    # Debug access: on the settings page only, with a press of its own (tests/fake_web.py)
    check("debug_access_adb" not in by, "no debug access switch in Home Assistant")

run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT))
