#!/usr/bin/env python3
"""Home Assistant's side of the ESPHome native API against build/hassmic-host (tests/esphome_common.py), part four:
runs cancelled by the action button or the wake word while Home Assistant thinks or still streams the reply, the
keys as Home Assistant's "Button" event entity, turns in which nothing is said, the "Wake word" event entity and the
"Last heard" / "Last reply" text sensors."""
import asyncio, os, signal, tempfile
from aioesphomeapi import APIClient, EventInfo, MediaPlayerEntityState, TextSensorInfo, TextSensorState, VoiceAssistantEventType as Ev
from aioesphomeapi.model import Event
from esphome_common import KEY_HELP, KEY_VOLUMEUP, KEY_VOLUMEDOWN, Browser, assistant, check, until, sync, home_assistant, run, late_asked, late_done

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17121, 17122, 17123, 17124, 17125


async def main(e):
    proc, play, keyev = e.proc, e.play, e.keyev
    ha = await home_assistant(e)
    va = ha.va; started, stopped = va.started, va.stopped
    c, cby, st3 = ha.cli, ha.by, ha.states
    # The action button (SIGUSR2) while Home Assistant thinks: the run is aborted (as a Voice PE's button does), and what
    # Home Assistant still sends for it plays nothing.  The wake word instead aborts and listens again.
    async def thinking():
        proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn off everything"})
        await sync(c); stopped.clear(); started.clear()
    def late_reply():                    # the aborted run's reply, already on the way
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    before = os.path.getsize(play); started.clear(); await thinking()
    proc.send_signal(signal.SIGUSR2); await until(lambda: stopped)
    late_reply(); await asyncio.sleep(1.0)                                 # nothing may play
    check(stopped == [True] and not started.is_set() and os.path.getsize(play) == before,
          f"button while thinking: run aborted, nothing listens, its late reply not played (stops {stopped}, {os.path.getsize(play) - before} bytes played)")
    started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    check(True, "wake word works again after a cancelled run")
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
    await sync(c)

    before = os.path.getsize(play); started.clear(); await thinking()
    proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    late_reply(); await asyncio.sleep(1.0)
    check(stopped == [True] and os.path.getsize(play) == before,
          f"wake word while thinking: run aborted, new one started, the old reply not played (stops {stopped}, {os.path.getsize(play) - before} bytes played)")
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await until(lambda: os.path.getsize(play) - before >= 48000)
    check(os.path.getsize(play) - before == 48000, f"the new run's reply plays: {os.path.getsize(play) - before} bytes")

    # Streamed TTS: the reply's fetch already waits on Home Assistant (first words out, tool calls still running) when
    # the wake word cancels.  The fetch is cut: the new run's reply is not refused as busy, the old one never plays.
    before = os.path.getsize(play); started.clear()
    proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, {"url": f"http://127.0.0.1:{HTTP_PORT}/late.wav"})
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn off everything"})
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"})
    await until(late_asked.is_set); stopped.clear(); started.clear()          # the fetch waits on "Home Assistant"
    proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP_PORT}/reply.wav"})
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await until(lambda: os.path.getsize(play) - before >= 48000)
    check(stopped == [True] and os.path.getsize(play) - before == 48000,
          f"wake word while a streamed reply is fetched: fetch cut, the new run's reply plays (stops {stopped}, {os.path.getsize(play) - before} bytes)")
    await until(late_done.is_set, 5); await asyncio.sleep(0.5)             # the old reply's audio is out: it must not play
    check(os.path.getsize(play) - before == 48000, f"the cancelled run's reply never plays: {os.path.getsize(play) - before} bytes")
    started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    check(True, "and the wake word works after it")
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
    await sync(c)

    # The buttons in Home Assistant (issue #26): the event entity "Button", beside what each key does on the Echo.  A
    # press is told when let go within 1 s, a hold once at 1 s while still down; both volume keys down are a pair
    # (arbitration), no presses; a press that lets a browser in is not told
    bev = cby.get("button")
    check(isinstance(bev, EventInfo) and bev.device_class == "button" and list(bev.event_types)
          == ["action", "action_hold", "volume_up", "volume_up_hold", "volume_down", "volume_down_hold"],
          f"buttons: the event entity, with a press and a hold of each key: {bev}")
    bevs = lambda: [x.event_type for x in st3 if isinstance(x, Event) and x.key == bev.key]
    vol = lambda: round([x.volume for x in st3 if isinstance(x, MediaPlayerEntityState)][-1], 2)
    n0 = len(bevs()); v0 = vol()
    keyev(KEY_VOLUMEUP, 1); await asyncio.sleep(0.2)
    check(vol() == v0, "volume up down: no step yet (a hold may follow)")
    keyev(KEY_VOLUMEUP, 0)
    await until(lambda: len(bevs()) > n0 and vol() != v0)
    check(bevs()[n0:] == ["volume_up"] and abs(vol() - min(1, v0 + 0.1)) < 0.01,
          f"volume up pressed: one step up when let go, volume_up ({bevs()[n0:]}, {v0} -> {vol()})")
    n0 = len(bevs()); v0 = vol(); keyev(KEY_VOLUMEDOWN, 1)
    await until(lambda: len(bevs()) > n0, 2)
    check(bevs()[n0:] == ["volume_down_hold"], f"volume down held: volume_down_hold at 1 s, still down ({bevs()[n0:]})")
    keyev(KEY_VOLUMEDOWN, 0); await asyncio.sleep(0.5)
    check(bevs()[n0:] == ["volume_down_hold"] and vol() == v0, f"and nothing when let go, the volume as it was ({bevs()[n0:]}, {v0} -> {vol()})")
    n0 = len(bevs()); started.clear(); keyev(KEY_HELP, 1)
    await until(lambda: len(bevs()) > n0, 2)
    keyev(KEY_HELP, 0); await asyncio.sleep(0.5)
    check(bevs()[n0:] == ["action_hold"] and not started.is_set(), f"action held: action_hold, no request ({bevs()[n0:]})")
    n0 = len(bevs())
    keyev(KEY_VOLUMEUP, 1); await asyncio.sleep(0.1); keyev(KEY_VOLUMEDOWN, 1); await asyncio.sleep(1.5)
    v0 = vol(); keyev(KEY_VOLUMEUP, 0); keyev(KEY_VOLUMEDOWN, 0); await asyncio.sleep(0.5)
    check(bevs()[n0:] == [] and vol() == v0, f"both volume keys held: a pair, no presses, holds or volume steps ({bevs()[n0:]})")
    page2 = Browser(WEB); page2.login(); keyev(KEY_HELP, 1); keyev(KEY_HELP, 0)
    await until(lambda: page2.login() == "approved")
    await asyncio.sleep(0.3)
    check(page2.login() == "approved" and bevs()[n0:] == [] and not started.is_set(),
          f"a press that lets a browser in: not told, no request ({bevs()[n0:]})")
    keyev(KEY_HELP, 1); await asyncio.sleep(0.1); keyev(KEY_HELP, 0)
    await until(lambda: len(bevs()) > n0 and started.is_set())
    check(bevs()[n0:] == ["action"] and started.is_set(), f"action pressed: action, and the Echo listens as before ({bevs()[n0:]})")
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
    await sync(c)

    # Nothing said after the wake word (issue #48): Home Assistant's stt-no-text-recognized ends the turn quietly,
    # without the error ring; another error still shows it
    leds = e.env["HASSMIC_FAKE_LED"]
    errs = lambda: open(leds).read().count("anim_start_error_short") if os.path.exists(leds) else 0
    no_text = {"code": "stt-no-text-recognized", "message": "No text recognized"}
    n0 = errs(); started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, no_text)
    await sync(c); await asyncio.sleep(0.3)
    check(errs() == n0, f"nothing said after the wake word: no error ring ({errs() - n0})")
    started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
    await until(lambda: errs() > n0)
    check(errs() == n0 + 1, f"the Echo listens again, and another error shows the error ring ({errs() - n0})")

    # The "Wake word" event entity (issue #48): off by default; switched on on the page, Home Assistant lists it again
    check("wake_word" not in cby and e.pv("wake_events") == 0, "wake word events off by default: no entity in Home Assistant")
    e.page.set(wake_events="on")
    try: await c.disconnect()
    except Exception: pass
    c = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(c.connect(login=True), 5)
    es, _ = await c.list_entities_services(); st3 = []; c.subscribe_states(st3.append)
    va = assistant(c); started = va.started; await sync(c)
    wev = next((x for x in es if x.object_id == "wake_word"), None)
    check(isinstance(wev, EventInfo) and list(wev.event_types) == ["detected", "near_miss", "answered_elsewhere", "no_speech"]
          and e.conf().get("wake_events") == "on", f"wake word events switched on on the page: the event entity, kept in the settings file: {wev}")
    wevs = lambda: [x.event_type for x in st3 if isinstance(x, Event) and x.key == wev.key]
    n0 = len(wevs()); started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, no_text)
    await until(lambda: len(wevs()) >= n0 + 2)
    check(wevs()[n0:] == ["detected", "no_speech"], f"a wake word, then nothing said: detected, no_speech ({wevs()[n0:]})")
    n0 = len(wevs()); started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "duplicate_wake_up_detected", "message": "Duplicate wake-up detected"})
    await until(lambda: len(wevs()) >= n0 + 2)
    check(wevs()[n0:] == ["detected", "answered_elsewhere"], f"a wake word another satellite answers: detected, answered_elsewhere ({wevs()[n0:]})")
    n0 = len(wevs()); proc.send_signal(signal.SIGTTOU)                     # the PC's stand-in for Pryon's near miss
    await until(lambda: len(wevs()) > n0)
    check(wevs()[n0:] == ["near_miss"], f"a near miss: near_miss ({wevs()[n0:]})")
    # Nothing said after the button or a follow-up question: quiet too, but no wake word to tell of
    n0 = len(wevs()); started.clear(); keyev(KEY_HELP, 1); await asyncio.sleep(0.1); keyev(KEY_HELP, 0)
    await asyncio.wait_for(started.wait(), 5)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, no_text)
    await sync(c); await asyncio.sleep(0.3)
    check(wevs()[n0:] == [], f"the button, then nothing said: no wake word event ({wevs()[n0:]})")
    started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "what is on the list"})
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
    started.clear(); c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await asyncio.wait_for(started.wait(), 5)                              # the follow-up listens
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, no_text)
    await sync(c); await asyncio.sleep(0.3)
    check(wevs()[n0:] == ["detected"], f"a follow-up question, then nothing said: only the wake word before it ({wevs()[n0:]})")

    # "Last heard" and "Last reply" (issue #53): off by default; on, the transcript and the answer, both cleared when a
    # run starts (the same request twice is a change), a reply over Home Assistant's 255 characters cut on a character
    check("last_heard" not in cby and e.pv("request_text") == 0, "request text off by default: no sensors in Home Assistant")
    e.page.set(request_text="on")
    try: await c.disconnect()
    except Exception: pass
    c = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(c.connect(login=True), 5)
    es, _ = await c.list_entities_services(); st4 = []; c.subscribe_states(st4.append)
    va = assistant(c); started = va.started; await sync(c)
    by = {x.object_id: x for x in es}
    check(isinstance(by.get("last_heard"), TextSensorInfo) and isinstance(by.get("last_reply"), TextSensorInfo)
          and e.conf().get("request_text") == "on", "request text switched on on the page: both text sensors, kept in the settings file")
    texts = lambda name: [x.state for x in st4 if isinstance(x, TextSensorState) and x.key == by[name].key]
    async def turn(heard, reply):
        started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": heard})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_START, {"text": reply})
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        await sync(c); await asyncio.sleep(0.2)
    n0, r0 = len(texts("last_heard")), len(texts("last_reply"))
    await turn("set a 10 second timer", "Timer set for 10 seconds")
    await turn("set a 10 second timer", "Timer set for 10 seconds")
    check(texts("last_heard")[n0:] == ["", "set a 10 second timer"] * 2 and texts("last_reply")[r0:] == ["", "Timer set for 10 seconds"] * 2,
          f"the same request twice: cleared at each start, then what was heard and answered ({texts('last_heard')[n0:]}, {texts('last_reply')[r0:]})")
    long = "Für " + "ä" * 300
    await turn("erzähl mir was", long)
    got = texts("last_reply")[-1]
    check(len(got) == 255 and got == long[:254] + "…" and texts("last_heard")[-1] == "erzähl mir was",
          f"a reply over 255 characters: 254 of them and an ellipsis, cut on a character ({len(got)}: {got[-4:]!r})")


run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), {"HASSMIC_FAKE_LED": os.path.join(tempfile.mkdtemp(), "ledctrl.log")})
