#!/usr/bin/env python3
"""Home Assistant's side of the ESPHome native API against build/hassmic-host (tests/esphome_common.py), part two:
timers ringing and what stops them, a new run after an error, encryption provisioned by Home Assistant (key, mDNS,
refusals, clearing), the time and zone over the keyed link, noise reduction, LED brightness and the light sensor."""
import asyncio, base64, os, re, signal, time
from aioesphomeapi import NumberState, SwitchState, TextSensorInfo, TextSensorState, BinarySensorInfo, BinarySensorState, SensorInfo, SensorState
from aioesphomeapi import APIClient, MediaPlayerCommand, VoiceAssistantEventType as Ev, VoiceAssistantTimerEventType as Tm, ZERO_NOISE_PSK
from aioesphomeapi.core import InvalidEncryptionKeyAPIError, RequiresEncryptionAPIError
from esphome_common import check, until, sync, peak, speech, home_assistant, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17101, 17102, 17103, 17104, 17105


async def main(e):
    proc, state, env, play, mdns, page, conf = e.proc, e.state, e.env, e.play, e.mdns, e.page, e.conf
    ha = await home_assistant(e)
    va = ha.va; started, stopped, mic = va.started, va.stopped, va.mic
    handle_start, handle_stop, handle_audio, handle_finished = va.handle_start, va.handle_stop, va.handle_audio, va.handle_finished
    cli, entities, by, states, last, mp = ha.cli, ha.entities, ha.by, ha.states, ha.last, ha.mp
    lauto, lbright = by.get("led_auto_brightness"), by.get("led_brightness")

    # Timers: "Timer ringing" + "Ringing timers" (names) for automations; several at once; a cancel is for another one
    ring, names = by.get("timer_ringing"), by.get("ringing_timers")
    check(isinstance(ring, BinarySensorInfo) and isinstance(names, TextSensorInfo) and int(names.entity_category) == 2
          and last(ring.key, BinarySensorState) is False and last(names.key, TextSensorState) == "",
          "timer entities listed (names: diagnostic), not ringing")
    ringing = lambda: (last(ring.key, BinarySensorState), last(names.key, TextSensorState))
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t1", "tea", 60, 0, False)
    await until(lambda: ringing() == (True, "tea"))
    n = len(states)
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t2", "", 300, 0, False)
    await until(lambda: ringing() == (True, "tea, 5 min"))
    check(ringing() == (True, "tea, 5 min") and not any(isinstance(s, BinarySensorState) and s.key == ring.key and not s.state for s in states[n:]),
          f"a second timer joins the ring, named by its length: {ringing()}")
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_CANCELLED, "t3", "eggs", 360, 200, False)
    await sync(cli); await asyncio.sleep(0.3)
    check(ringing() == (True, "tea, 5 min"), f"cancelling a timer that still runs leaves the ring alone: {ringing()}")
    # Only an explicit stop ends a ring.  The wake word alone puts it in the background: a pipeline as usual
    started.clear(); stopped.clear(); proc.send_signal(signal.SIGUSR1)       # "wake word"
    await asyncio.wait_for(started.wait(), 5); await asyncio.sleep(0.3)
    check(ringing() == (True, "tea, 5 min"), f"the wake word alone does not stop a ring, it listens: {ringing()}")
    proc.send_signal(signal.SIGHUP)                                            # its "stop": the ring and that pipeline end
    await until(lambda: ringing() == (False, "") and stopped)
    check(ringing() == (False, "") and stopped == [True], f'"<wake word>, stop" stops every timer at once and cancels its pipeline: {ringing()}, stops {stopped}')
    # cancelled, not ended: what Home Assistant still sends of that run (its STT_START) leaves the Echo idle, so the
    # next wake word starts a run (ended, the late STT_START put it back to listening with the mic closed)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None); await sync(cli)
    started.clear(); proc.send_signal(signal.SIGUSR1)
    check(await until(started.is_set, 3), "after the dropped run, Home Assistant's late STT_START opens nothing: the next wake word starts a run")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)      # as Home Assistant: after a cancelled run
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(cli)

    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t6", "eggs", 360, 0, False)
    await until(lambda: ringing() == (True, "eggs"))
    started.clear(); proc.send_signal(signal.SIGUSR2)                          # the action button: stops it, no pipeline
    await until(lambda: ringing() == (False, "")); await asyncio.sleep(0.5)
    check(not started.is_set() and ringing() == (False, ""), f"the button only stops a ring: {ringing()}")

    # an engine without the "stop" keyword (microWakeWord): the transcript "Stop." stops it and cancels the run
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t7", "tea", 60, 0, False)
    await until(lambda: ringing() == (True, "tea"))
    started.clear(); stopped.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "what time is it"})
    await sync(cli); await asyncio.sleep(0.3)
    check(ringing() == (True, "tea") and not stopped, f"another question during a ring leaves it ringing: {ringing()}, stops {stopped}")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(cli)
    started.clear(); stopped.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "Stop."})
    await until(lambda: ringing() == (False, "") and stopped)
    check(ringing() == (False, "") and stopped == [True], f'transcript "Stop." stops the ring and cancels the run: {ringing()}, stops {stopped}')
    started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)     # the next run counts again
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(cli)

    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t4", "pasta", 600, 0, False)
    await until(lambda: ringing() == (True, "pasta"))
    cli.media_player_command(mp[0].key, command=MediaPlayerCommand.STOP)
    await until(lambda: ringing() == (False, ""))
    check(ringing() == (False, ""), f"the media player's stop stops a ring from Home Assistant: {ringing()}")

    # "Stop ringing" (issue #21): the ring only, not what the media player plays
    stop_btn = by.get("stop_ringing")
    check(stop_btn is not None and not int(stop_btn.entity_category), "Stop ringing button listed, on dashboards (no entity category)")
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t8", "pizza", 900, 0, False)
    await until(lambda: ringing() == (True, "pizza"))
    cli.button_command(stop_btn.key)
    await until(lambda: ringing() == (False, ""))
    check(ringing() == (False, ""), f'"Stop ringing" stops a ring from Home Assistant: {ringing()}')

    # timer_stop_any (issue #21): "Stopp den Timer", or anything else said during a ring, stops it and cancels the run
    page.set(timer_stop_any="on")
    check(conf().get("timer_stop_any") == "on", f"any request stops a ring: saved {conf().get('timer_stop_any')}")
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t9", "tea", 60, 0, False)
    await until(lambda: ringing() == (True, "tea"))
    started.clear(); stopped.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "Stopp den Timer"})
    await until(lambda: ringing() == (False, "") and stopped)
    check(ringing() == (False, "") and stopped == [True], f'with timer_stop_any "Stopp den Timer" stops the ring and cancels the run: {ringing()}, stops {stopped}')
    started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)     # without a ring: to Home Assistant
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    stopped.clear()
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "what time is it"})
    await sync(cli); await asyncio.sleep(0.3)
    check(not stopped, f"with timer_stop_any and no ring, a request goes on as usual: stops {stopped}")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(cli)
    page.set(timer_stop_any="off")

    page.set(timer_ring=1)
    check(conf().get("timer_ring") == "1", f"ring time saved: {conf().get('timer_ring')}")
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t5", "", 90, 0, False)
    await until(lambda: ringing() == (True, "90 s"))
    t0 = time.monotonic(); await until(lambda: ringing() == (False, ""), 5)
    check(ringing() == (False, "") and time.monotonic() - t0 < 3, f"rings out after the ring time: {time.monotonic() - t0:.1f} s")
    page.set(timer_ring=60)

    proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "stt-no-text-recognized", "message": "nothing heard"})
    await sync(cli)
    started.clear(); proc.send_signal(signal.SIGUSR1); await asyncio.wait_for(started.wait(), 5)
    check(True, "new pipeline after an error")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(cli)

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

    enc = APIClient("127.0.0.1", PORT, None, noise_psk=key.decode(), timezone="Europe/Berlin")
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
    # and its zone, as its rule (the PC has no bionic tzdata: the rule itself; tz_ref.py checks the names)
    from aioesphomeapi.timezone import iana_to_posix_tz
    try: tz = open(os.path.join(state, "timezone")).read()
    except OSError: tz = ""
    check(tz == iana_to_posix_tz("Europe/Berlin") + "\n", f"keyed link: Home Assistant's time zone in state/timezone for root: {tz!r}")
    started.clear(); mic.clear()
    enc.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio, handle_announcement_finished=handle_finished)
    await sync(enc); proc.send_signal(signal.SIGUSR1)
    await asyncio.wait_for(started.wait(), 5); await until(lambda: len(mic) > 16000)
    check(len(mic) > 16000, f"encrypted: mic audio streamed: {len(mic)} bytes")
    enc.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(enc)
    page.set(noise_reduction="medium")
    check(conf().get("noise_reduction") == "medium", f"noise reduction set to medium and persisted: {conf()}")
    started.clear(); mic.clear(); proc.send_signal(signal.SIGUSR1)
    await asyncio.wait_for(started.wait(), 5); await speech(mic); pk = peak(mic)
    check(len(mic) > 16000 and len(mic) % 320 == 0 and 8000 < pk <= 29100,
          f"with noise reduction: mic audio streamed in whole 10 ms frames at speech level: {len(mic)} bytes, peak {pk}")
    enc.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(enc)
    page.set(noise_reduction="off")
    before = os.path.getsize(play)
    res = await enc.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
    check(res.success and os.path.getsize(play) - before == 48000, "encrypted: announcement played")

    check(await asyncio.wait_for(enc.noise_encryption_set_key(b""), 5) is True and not os.path.exists(os.path.join(state, "api_key")),
          "empty key from the keyed connection clears it (Home Assistant deleting the device)")
    check("api_encryption_supported=" in open(mdns).read(), "mDNS back to api_encryption_supported")
    await enc.disconnect()
    c = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(c.connect(login=True), 5)
    check((await c.device_info()).api_encryption_provisionable, "plaintext accepted again, provisionable again")
    st3 = []; c.subscribe_states(st3.append)
    last3 = lambda k, t: ([x.state for x in st3 if isinstance(x, t) and x.key == k] or [None])[-1]
    c.number_command(lbright.key, 30)
    await until(lambda: last3(lbright.key, NumberState) == 30 and last3(lauto.key, SwitchState) is False
                and conf().get("led_brightness") == "30")
    check(last3(lbright.key, NumberState) == 30 and last3(lauto.key, SwitchState) is False, "a fixed LED level switches auto brightness off")
    check(conf().get("led_auto_brightness") == "off" and conf().get("led_brightness") == "30", f"LED brightness kept in the settings file: {conf()}")
    c.switch_command(lauto.key, True)
    await until(lambda: last3(lauto.key, SwitchState) is True and conf().get("led_auto_brightness") == "on")
    check(last3(lauto.key, SwitchState) is True and conf().get("led_auto_brightness") == "on", "auto brightness switched on again")
    # A feature switched on the page closes the link; Home Assistant comes back and lists entities again
    async def relist(old):
        try: await old.disconnect()
        except Exception: pass
        n = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(n.connect(login=True), 5)
        es, _ = await n.list_entities_services(); sts = []; n.subscribe_states(sts.append); await sync(n)
        return n, {x.object_id: x for x in es}, sts
    # the light sensor: switched on, the illuminance entity, sent on a real change, not on flicker
    page.set(light_sensor="on")
    c, cby, st3 = await relist(c)
    lux = cby.get("illuminance")
    check(isinstance(lux, SensorInfo) and lux.device_class == "illuminance" and lux.unit_of_measurement == "lx" and int(lux.state_class) == 1
          and conf().get("light_sensor") == "on", "light sensor switched on on the page: illuminance listed, kept in the settings file")
    await until(lambda: last3(lux.key, SensorState) is not None)
    with open(env["HASSMIC_LUX"], "w") as f: f.write("68\n")                # flicker: not sent
    await asyncio.sleep(1.5)                                               # the sensor is read once a second
    with open(env["HASSMIC_LUX"], "w") as f: f.write("150\n")
    await until(lambda: last3(lux.key, SensorState) == 150)
    sent = [x.state for x in st3 if isinstance(x, SensorState) and x.key == lux.key]
    check(sent == [67, 150], f"illuminance from the sensor file, sent on a real change, not on flicker: {sent}")

run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT))
