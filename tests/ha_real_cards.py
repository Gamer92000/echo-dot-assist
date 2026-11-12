#!/usr/bin/env python3
"""Cards from voice requests in a real Home Assistant (custom_components/hassmic_cards, cards.c, the UI's card.c): the
Echo (build/hassmic-host-screen, tests/fake_screen.py's stand-in UI) is an ESPHome satellite, speech-to-text a stand-in
that says what the test wants, the conversation agent Home Assistant's own (its intents' results go to the chat log as
tool results, as the LLM's do).

  - "turn on the kitchen light": the light on in Home Assistant, and a request's card for this Echo: the light first
    (marked), then the room's other device, in Home Assistant's words; Home Assistant's area name as its title
  - a tap on the coffee maker's switch (the UI's "card tap"): input_boolean.toggle run by Home Assistant (allowed in the
    Echo's ESPHome options), the card in Home Assistant's words after; a change in Home Assistant: the card follows
  - "what is the weather": a weather card, now and the forecast (weather.get_forecasts)
  - a sentence-triggered automation's card (scope request, the Echo that heard it)
  - hassmic_cards.show: a weather-forecast card in day and night periods, a markdown card (its template rendered), a
    picture-entity card of a camera (its snapshot through the integration's token URL, fetched by hassmic, refreshed);
    clear by id; buttons (the Echo's event: a perform_action run, hassmic_cards_action taken by an automation; a press
    from a device the card is not on runs nothing); a card type the Echo cannot draw refused

Same isolation and Home Assistant as tests/ha_real_dropin.py (its helpers; build/ha-venv from its --setup)."""
import asyncio, base64, json, os, re, shutil, signal, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ha_real_dropin as hr
from ha_real_dropin import check, HA_PORT

STT = '''
from homeassistant.components.stt import (AudioBitRates, AudioChannels, AudioCodecs, AudioFormats, AudioSampleRates,
                                          Provider, SpeechResult, SpeechResultState)
TEXT = %r

async def async_get_engine(hass, config, discovery_info=None): return Stand_in()

class Stand_in(Provider):
    @property
    def supported_languages(self): return ["en"]
    @property
    def supported_formats(self): return [AudioFormats.WAV]
    @property
    def supported_codecs(self): return [AudioCodecs.PCM]
    @property
    def supported_bit_rates(self): return [AudioBitRates.BITRATE_16]
    @property
    def supported_sample_rates(self): return [AudioSampleRates.SAMPLERATE_16000]
    @property
    def supported_channels(self): return [AudioChannels.CHANNEL_MONO]
    async def async_process_audio_stream(self, metadata, stream):
        n = 0
        async for chunk in stream:                  # half a second of the request, then what the test wants said
            n += len(chunk)
            if n >= 16000: break
        return SpeechResult(open(TEXT).read().strip(), SpeechResultState.SUCCESS)
'''

TTS = '''
import io, wave
from homeassistant.components.tts import Provider

async def async_get_engine(hass, config, discovery_info=None): return Stand_in()

class Stand_in(Provider):
    name = "hassmic_fake"
    @property
    def default_language(self): return "en"
    @property
    def supported_languages(self): return ["en"]
    def get_tts_audio(self, message, language, options):
        b = io.BytesIO()
        with wave.open(b, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(bytes(3200))
        return "wav", b.getvalue()
'''

HOUSE = '''automation ui: !include automations.yaml
automation:
  - id: ping_button
    triggers: [{trigger: event, event_type: hassmic_cards_action, event_data: {action: PING}}]
    actions: [{action: input_boolean.turn_off, target: {entity_id: input_boolean.kitchen_switch}}]
  - id: foreign_button
    triggers: [{trigger: event, event_type: hassmic_cards_action, event_data: {action: FOREIGN}}]
    actions: [{action: input_boolean.turn_off, target: {entity_id: input_boolean.coffee_maker}}]
  - id: bp_doorbell
    use_blueprint:
      path: hassmic_cards/doorbell_camera.yaml
      input: {doorbell: input_button.doorbell, camera: camera.door, timeout: 20}
  - id: bp_washer
    use_blueprint:
      path: hassmic_cards/appliance_done.yaml
      input: {power: input_number.washer_power, done_below: 10, done_for: 0, snooze: 0.05, running_above: 100, card_id: washer, name: Test washer}
  - id: bp_voice
    use_blueprint:
      path: hassmic_cards/voice_card.yaml
      input: {sentences: ["show me the coffee card"], answer: "There you go", timeout: 5, card: {type: tile, title: Coffee card, entity: input_boolean.coffee_maker}}
  - id: coffee_card
    triggers: [{trigger: conversation, command: "show me the coffee maker"}]
    actions:
      - set_conversation_response: "Here it is"
      - action: hassmic_cards.show
        target: {device_id: "{{ trigger.device_id }}"}
        data: {scope: request, timeout: 3, card: {type: entities, title: Coffee, entities: [input_boolean.coffee_maker]}}
input_button:
  doorbell:
    name: Doorbell
input_number:
  washer_power:
    name: Washer power
    min: 0
    max: 3000
    initial: 500
input_boolean:
  kitchen_switch:
    name: Kitchen Switch
  coffee_maker:
    name: Kitchen Coffee maker
template:
  - light:
      - name: Kitchen Light
        unique_id: kitchen_light_t
        state: "{{ is_state('input_boolean.kitchen_switch', 'on') }}"
        turn_on:
          - action: input_boolean.turn_on
            target: {entity_id: input_boolean.kitchen_switch}
        turn_off:
          - action: input_boolean.turn_off
            target: {entity_id: input_boolean.kitchen_switch}
  - weather:
      - name: Home
        unique_id: home_weather_t
        condition_template: "sunny"
        temperature_template: "{{ 21.4 }}"
        humidity_template: "{{ 40 }}"
        temperature_unit: "°C"
        forecast_daily_template: >-
          {{ [ {'datetime': now().isoformat(), 'condition': 'sunny', 'temperature': 23, 'templow': 12},
               {'datetime': (now() + timedelta(days=1)).isoformat(), 'condition': 'rainy', 'temperature': 18, 'templow': 11,
                'precipitation_probability': 60} ] }}
      - name: Periods
        unique_id: periods_weather_t
        condition_template: "cloudy"
        temperature_template: "{{ 14 }}"
        humidity_template: "{{ 70 }}"
        temperature_unit: "°C"
        forecast_twice_daily_template: >-
          {% set d = today_at('18:00') %}
          {{ [ {'datetime': d.isoformat(), 'is_daytime': false, 'condition': 'cloudy', 'temperature': 9, 'precipitation_probability': 1},
               {'datetime': (d + timedelta(hours=12)).isoformat(), 'is_daytime': true, 'condition': 'rainy', 'temperature': 17,
                'precipitation_probability': 41},
               {'datetime': (d + timedelta(hours=24)).isoformat(), 'is_daytime': false, 'condition': 'rainy', 'temperature': 4,
                'precipitation_probability': 40},
               {'datetime': (d + timedelta(hours=36)).isoformat(), 'is_daytime': true, 'condition': 'cloudy', 'temperature': 10} ] }}
hassmic_cards:
  timeout: 3
  voice_cards: true
'''
DOOR = bytes.fromhex("ffd8ffe000104a46494600010100000100010000ffdb004300080606070605080707070909080a0c140d0c0b0b0c1912130f141d1a1f1e1d1a1c1c"
                     "20242e2720222c231c1c2837292c30313434341f27393d38323c2e333432ffc0000b080001000101011100ffc4001f00000105010101010101000000"
                     "00000000000102030405060708090a0bffc400b5100002010303020403050504040000017d01020300041105122131410613516107227114328191"
                     "a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a636465666768696a"
                     "737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8"
                     "d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffda0008010100003f00fbd3ffd9")     # a 1x1 JPEG: the door's "snapshot"


def write_config(conf, text):
    os.makedirs(os.path.join(conf, ".storage"))
    comp = os.path.join(conf, "custom_components", "hassmic_fake")
    os.makedirs(comp)
    json.dump({"domain": "hassmic_fake", "name": "hassmic's stand-in speech-to-text and text-to-speech", "version": "1.0", "documentation": "https://github.com/Gamer92000/echo-dot-assist",
               "codeowners": [], "requirements": [], "dependencies": [], "iot_class": "local_push"}, open(os.path.join(comp, "manifest.json"), "w"))
    open(os.path.join(comp, "__init__.py"), "w").write("")
    open(os.path.join(comp, "stt.py"), "w").write(STT % text)
    open(os.path.join(comp, "tts.py"), "w").write(TTS)
    shutil.copytree(os.path.join(hr.ROOT, "ha", "custom_components", "hassmic_cards"), os.path.join(conf, "custom_components", "hassmic_cards"))
    door = os.path.join(conf, "door.jpg"); open(door, "wb").write(DOOR)
    shutil.copytree(os.path.join(hr.ROOT, "ha", "blueprints", "automation", "hassmic_cards"),
                    os.path.join(conf, "blueprints", "automation", "hassmic_cards"))
    open(os.path.join(conf, "automations.yaml"), "w").write("[]\n")   # (what the automation editor writes: a device trigger's)
    open(os.path.join(conf, "configuration.yaml"), "w").write(
        "homeassistant:\n  name: Test\n  unit_system: metric\n  time_zone: UTC\n  latitude: 0\n  longitude: 0\n  elevation: 0\n"
        "  country: DE\n  internal_url: http://127.0.0.1:%d\napi:\nconfig:\nonboarding:\nconversation:\nassist_pipeline:\n"
        "stt:\n  - platform: hassmic_fake\ntts:\n  - platform: hassmic_fake\nlogger:\n  default: warning\n  logs:\n"
        "    custom_components.hassmic_cards: debug\n" % HA_PORT + HOUSE)
    json.dump({"version": 2, "minor_version": 2, "key": "http", "data": {
        "stable": {"server_port": HA_PORT, "server_host": ["127.0.0.1"], "cors_allowed_origins": [], "login_attempts_threshold": -1,
                   "ip_ban_enabled": False, "ssl_profile": "modern", "use_x_frame_options": True,
                   "created_at": "2026-10-11T00:00:00+00:00", "error": None, "error_message": None},
        "pending": None, "yaml_migration_done": True}}, open(os.path.join(conf, ".storage", "http"), "w"))


def ui_lines(path):
    """the text lines the UI got, the band's rows and images cut out ("band <w>" + 4 w bytes, "art <n>" + n)"""
    try: b = open(path, "rb").read()
    except OSError: return []
    out, i = [], 0
    while i < len(b):
        j = b.find(b"\n", i)
        if j < 0: break
        l = b[i:j].decode("utf-8", "replace"); i = j + 1
        out.append(l)
        if l.startswith("band ") and l[5:].isdigit(): i += 4 * int(l[5:])
        elif l.startswith("art ") and l[4:].isdigit(): i += int(l[4:])
        elif (m := re.match(r"img (\S+) (\d+) (\d+)$", l)): out.append(b[i:i + int(m.group(2))]); i += int(m.group(2))   # its bytes next
    return out


def last_card(path):
    ls = [l for l in ui_lines(path) if isinstance(l, str) and l[:2] in ("cb", "cr", "cw", "cf", "cm", "ci", "cl", "cu", "ce", "cx") and l[2:3] in (" ", "")]
    for k in range(len(ls) - 1, -1, -1):
        if ls[k] == "cx": return ["cx"]
        if ls[k].startswith("cb "): return ls[k:]
    return []


async def run(tmp):
    import aiohttp
    conf, text = os.path.join(tmp, "ha"), os.path.join(tmp, "say.txt")
    write_config(conf, text)
    ui_dir = os.path.join(tmp, "ui"); os.makedirs(ui_dir)
    ui_log, fifo, runner = os.path.join(ui_dir, "ui.log"), os.path.join(ui_dir, "ui.fifo"), os.path.join(ui_dir, "ui.sh")
    os.mkfifo(fifo)
    open(runner, "w").write(f"#!/bin/sh\nexec {sys.executable} {hr.ROOT}/tests/fake_screen.py --ui {ui_dir}\n"); os.chmod(runner, 0o755)
    os.environ.update(HASSMIC_CAP=os.path.join(hr.ROOT, "testdata", "alexa_espeak.raw"), HASSMIC_SCREEN=runner, FAKE_UI_LOG=ui_log,
                      FAKE_UI_FIFO=fifo, HASSMIC_SCREEN_READY_MS="5000")
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    echo = hr.Echo(tmp, "Echo Show 5 c0ffee", "02:00:00:c0:ff:ee", 26290, net, os.path.join(hr.ROOT, "build", "hassmic-host-screen"))
    halog = open(os.path.join(tmp, "ha.log"), "w")
    hass = subprocess.Popen([hr.HA_PY, "-m", "homeassistant", "-c", conf, "--skip-pip"], stdout=halog, stderr=subprocess.STDOUT)
    echo.start()
    try:
        async with aiohttp.ClientSession() as session:
            for _ in range(240):
                try:
                    async with session.get(f"{hr.HA}/api/onboarding") as r:
                        if r.status == 200: break
                except aiohttp.ClientError: pass
                if hass.poll() is not None: raise RuntimeError("Home Assistant ended")
                await asyncio.sleep(0.5)
            c = hr.Client(session, await hr.onboard(session)); await c.ws_open()
            for _ in range(60):
                try:
                    p = await c.ws(type="assist_pipeline/pipeline/create", conversation_engine="conversation.home_assistant",
                                   conversation_language="en", language="en", name="Stand-in", stt_engine="hassmic_fake",
                                   stt_language="en", tts_engine="hassmic_fake", tts_language="en", tts_voice=None, wake_word_entity=None,
                                   wake_word_id=None)
                    break
                except RuntimeError as err:
                    if "unknown_command" not in str(err): raise
                    await asyncio.sleep(0.5)
            else: raise RuntimeError("assist_pipeline's commands never came: not set up")
            await c.ws(type="assist_pipeline/pipeline/set_preferred", pipeline_id=p["id"])
            # the house: the kitchen's light and coffee maker in the Kitchen area, exposed to Assist
            area = next((x for x in await c.ws(type="config/area_registry/list") if x["name"] == "Kitchen"), None) \
                or await c.ws(type="config/area_registry/create", name="Kitchen")       # (onboarding makes one)
            for e in ("light.kitchen_light", "input_boolean.coffee_maker"):
                await c.ws(type="config/entity_registry/update", entity_id=e, area_id=area["area_id"])
            await c.ws(type="homeassistant/expose_entity", assistants=["conversation"],
                       entity_ids=["light.kitchen_light", "input_boolean.coffee_maker", "weather.home"], should_expose=True)
            # the door's camera: a local_file camera (set up by its config flow: no YAML platform any more) of a JPEG
            f = await c.post("/api/config/config_entries/flow", {"handler": "local_file"})
            r = await c.post(f"/api/config/config_entries/flow/{f['flow_id']}", {"name": "Door", "file_path": os.path.join(conf, "door.jpg")})
            check(isinstance(r, dict) and r.get("type") == "create_entry", f"a camera for the doorbell's card: {r.get('type')}")
            await hr.add_esphome(c, echo)
            check(await hr.until(lambda: _has(echo, "voice assistant: subscribed"), 20), "the Echo in Home Assistant, its voice assistant subscribed")
            entry = next(x for x in await c.get("/api/config/config_entries/entry?domain=esphome"))["entry_id"]
            f = await c.post("/api/config/config_entries/options/flow", {"handler": entry})
            form = {x["name"]: x.get("default") for x in f.get("data_schema", [])}; form["allow_service_calls"] = True
            r = await c.post(f"/api/config/config_entries/options/flow/{f['flow_id']}", form)
            check(isinstance(r, dict) and r.get("type") == "create_entry", f"HA actions allowed in the Echo's ESPHome options: {r}")
            await asyncio.sleep(3); await hr.until(lambda: _has(echo, "voice assistant: subscribed"), 20)   # (the entry reloads)

            # a request that turns something on
            open(text, "w").write("turn on the kitchen light")
            echo.proc.send_signal(signal.SIGUSR1)
            ok = await hr.until(lambda: _has(echo, 'card: request\'s e "Kitchen"'), 20)
            check(ok, "a card for this request: \"Kitchen\", a request's")
            check(await hr.until(lambda: _state(c, "input_boolean.kitchen_switch", "on"), 10), "the light on in Home Assistant")
            card = last_card(ui_log)
            check(bool(card) and card[0].startswith("cb ") and " e 2 1\tKitchen" in card[0], f"told to the UI: 2 rows, a request's: {card[:1]}")
            rows = [l for l in card if l.startswith("cr ")]
            check(len(rows) == 2 and rows[0].startswith("cr 0 t 1 ") and rows[0].endswith("\tLight\tOn") and " 1\tLight" in rows[0]
                  and rows[1].startswith("cr 1 t 0 ") and "\tCoffee maker\tOff" in rows[1],
                  f"the light first (focus, \"Kitchen\" left off its name, HA's words), then the coffee maker: {rows}")
            # a tap on the coffee maker's switch: the action; HA's update follows
            seq = card[0].split()[1]
            with open(fifo, "wb") as q: q.write(f"card tap {seq} 1 toggle\n".encode())
            check(await hr.until(lambda: _state(c, "input_boolean.coffee_maker", "on"), 10), "a tap on its switch: the coffee maker on in Home Assistant")
            check(await hr.until(sync(lambda: any(l.startswith("cr 1 t 1 ") and l.endswith("\tOn") for l in last_card(ui_log))), 10),
                  "  and on the card, in Home Assistant's words (its update)")
            # a change in Home Assistant: the card follows (an update, the same card)
            await c.ws(type="call_service", domain="input_boolean", service="turn_off", service_data={"entity_id": "input_boolean.kitchen_switch"})
            check(await hr.until(sync(lambda: any(l.startswith("cr 0 t 0 ") and l.endswith("\tOff") for l in last_card(ui_log))
                                      and last_card(ui_log)[0].split()[1] == seq), 10), f"the light off in HA: the card follows: {last_card(ui_log)[:2]}")
            check(await hr.until(lambda: _has(echo, 'card: request\'s "Kitchen" gone (its time is up)'), 15), "gone 3 s after the Echo was idle again")

            # the weather
            open(text, "w").write("what is the weather")
            await hr.until(lambda: _has(echo, "-> idle"), 10)
            echo.proc.send_signal(signal.SIGUSR1)
            check(await hr.until(lambda: _has(echo, 'card: request\'s w "Weather"'), 20), "\"what is the weather\": a weather card")
            card = last_card(ui_log)
            check(any(l.startswith("cw sunny\t21°\t↑23°  ↓12°") for l in card) and any(l.startswith("cf 0 rainy\t") and "18° / 11°" in l and "60 %" in l for l in card),
                  f"now and tomorrow: {card}")

            # a sentence-triggered automation's card: a request's, on the Echo that heard it
            idles = echo.text().count("-> idle")
            await hr.until(sync(lambda: echo.text().count("-> idle") > idles), 10); await asyncio.sleep(2)   # (this request's end, not an earlier one)
            open(text, "w").write("show me the coffee maker")
            echo.proc.send_signal(signal.SIGUSR1)
            check(await hr.until(lambda: _has(echo, 'card: request\'s e "Coffee"'), 20), "a sentence trigger's card: a request's, on this Echo")
            idles = echo.text().count("-> idle")
            await hr.until(sync(lambda: echo.text().count("-> idle") > idles), 10); await asyncio.sleep(2)
            open(text, "w").write("show me the coffee card")
            echo.proc.send_signal(signal.SIGUSR1)
            check(await hr.until(lambda: _has(echo, 'card: request\'s e "Coffee card"'), 20), "the voice_card blueprint: its card on the Echo that heard it")
            await hr.until(lambda: _has(echo, 'card: request\'s "Coffee card" gone'), 20)
            # the Show's "Voice cards" switch off: a voice request makes no card there
            vsw = "switch.echo_show_5_c0ffee_voice_cards"
            check(await _state(c, vsw, "on"), "the Show's Voice cards switch: on")
            await c.ws(type="call_service", domain="switch", service="turn_off", service_data={"entity_id": vsw})
            weathers, idles = echo.text().count('card: request\'s w "Weather"'), echo.text().count("-> idle")
            open(text, "w").write("what is the weather")
            echo.proc.send_signal(signal.SIGUSR1)
            await hr.until(sync(lambda: echo.text().count("-> idle") > idles), 20); await asyncio.sleep(2)
            check(echo.text().count('card: request\'s w "Weather"') == weathers, "  off: a voice request, no card")
            await c.ws(type="call_service", domain="switch", service="turn_on", service_data={"entity_id": vsw})

            # a forecast in day and night periods (NWS): one row a day, its high and low; tonight's low in the header
            await c.ws(type="call_service", domain="hassmic_cards", service="show",
                       service_data={"card": {"type": "weather-forecast", "entity": "weather.periods"}, "timeout": 5})
            check(await hr.until(sync(lambda: any(l.startswith("cw cloudy\t14°") for l in last_card(ui_log))), 10), "a forecast in periods: a weather card")
            card = last_card(ui_log)
            check(any(l == "cw cloudy\t14°\t↓9°" for l in card) and any(l.startswith("cf 0 rainy\t") and "\t17° / 4°\t41 %" in l for l in card)
                  and any(l.startswith("cf 1 cloudy\t") and "\t10°\t" in l for l in card) and not any(l.startswith("cf 2 ") for l in card),
                  f"  tonight's low, then a day each with its night: {card}")

            # a markdown card for all Shows (no target)
            await c.ws(type="call_service", domain="hassmic_cards", service="show",
                       service_data={"card": {"type": "markdown", "title": "Doorbell", "content": "**Someone** is at the {{ 'door' }}"}, "timeout": 5})
            check(await hr.until(sync(lambda: any(l == "cm Someone is at the door" for l in last_card(ui_log))), 10), "a markdown card: its template rendered, plain")

            # a doorbell's card: a picture-entity card of the camera, live: its snapshot through the integration's own URL
            await c.ws(type="call_service", domain="hassmic_cards", service="show",
                       service_data={"id": "door", "title": "Front door", "subtitle": "Doorbell", "message": "Someone is at the door", "timeout": 20,
                                     "card": {"type": "picture-entity", "entity": "camera.door", "camera_view": "live", "refresh": 1}})
            check(await hr.until(sync(lambda: any(l.startswith("ci card") for l in last_card(ui_log))), 10), "a camera's card: its picture line")
            card = last_card(ui_log); seq = card[0].split()[1]
            check(card[0].endswith("\tFront door\tDoorbell") and f"ci card{seq}.0" in card and "cm Someone is at the door" in card, f"  title, subtitle, picture: {card}")
            with open(fifo, "wb") as q: q.write(f"img card{seq}.0 512\n".encode())
            def got(): ls = ui_lines(ui_log); return [ls[k + 1] for k, l in enumerate(ls[:-1]) if isinstance(l, str) and l.startswith(f"img card{seq}.0 ")]
            check(await hr.until(sync(lambda: got()), 10) and got()[-1] == DOOR, "  hassmic fetched the snapshot for the UI")
            check(await hr.until(sync(lambda: f"ci card{seq}.1" in last_card(ui_log)), 5), "  a new one asked for after its refresh")
            async with session.get(f"http://127.0.0.1:{HA_PORT}/api/hassmic_cards/camera/nothing") as r: st = r.status
            check(st == 404, f"  a made-up token: {st}")
            await c.ws(type="call_service", domain="hassmic_cards", service="clear", service_data={"id": "other"})
            await asyncio.sleep(1); check(last_card(ui_log)[:1] and last_card(ui_log)[0].split()[1] == seq, "clear of another id: it stays")
            await c.ws(type="call_service", domain="hassmic_cards", service="clear", service_data={"id": "door"})
            check(await hr.until(sync(lambda: last_card(ui_log) == ["cx"]), 10), "hassmic_cards.clear by its id: the card goes")

            # buttons: the Echo's event; the button's own action runs, hassmic_cards_action fires (an automation takes it)
            await c.ws(type="call_service", domain="input_boolean", service="turn_on", service_data={"entity_id": "input_boolean.kitchen_switch"})
            before = (await c.get("/api/states/input_boolean.coffee_maker"))["state"]
            await c.ws(type="call_service", domain="hassmic_cards", service="show",
                       service_data={"id": "btn", "timeout": 30, "card": {"type": "markdown", "title": "Buttons", "content": "Press one"},
                                     "actions": [{"action": "COFFEE", "title": "Coffee", "perform_action": "input_boolean.toggle",
                                                  "data": {"entity_id": "input_boolean.coffee_maker"}, "keep": True},
                                                 {"action": "PING", "title": "Ping"}]})
            check(await hr.until(sync(lambda: "cu 1\tPing" in last_card(ui_log)), 10), f"a card with buttons: {last_card(ui_log)}")
            card = last_card(ui_log); seq = card[0].split()[1]
            with open(fifo, "wb") as q: q.write(f"card tap {seq} 0 press\n".encode())
            want = "off" if before == "on" else "on"
            check(await hr.until(lambda: _state(c, "input_boolean.coffee_maker", want), 10), "  Coffee: its perform_action ran")
            # the same event from a device the card is not on (any ESPHome device may fire esphome.* events): nothing runs
            await c.ws(type="fire_event", event_type="esphome.hassmic_card_action", event_data={"card": "btn", "action": "COFFEE", "device_id": "not-an-echo"})
            await asyncio.sleep(1.5)
            check(await _state(c, "input_boolean.coffee_maker", want), "  the same button from another device: nothing run")
            await asyncio.sleep(0.5); check(last_card(ui_log)[:1] and last_card(ui_log)[0].split()[1] == seq, "  keep: the card stays")
            with open(fifo, "wb") as q: q.write(f"card tap {seq} 1 press\n".encode())
            check(await hr.until(lambda: _state(c, "input_boolean.kitchen_switch", "off"), 10), "  Ping: hassmic_cards_action fired, an automation took it")
            check(await hr.until(sync(lambda: last_card(ui_log) == ["cx"]), 5), "  and the card went")
            await c.ws(type="call_service", domain="input_boolean", service="turn_on", service_data={"entity_id": "input_boolean.coffee_maker"})
            # a button press from a device that is no Echo Show, of a card nobody knows: no hassmic_cards_action
            await c.ws(type="fire_event", event_type="esphome.hassmic_card_action", event_data={"card": "made-up", "action": "FOREIGN", "device_id": "not-an-echo"})
            await asyncio.sleep(1.5)
            check(await _state(c, "input_boolean.coffee_maker", "on"), "  an unknown card's press from another device: no hassmic_cards_action")
            # notify entities (notify.send_message): words from a notification are words, never a template
            for ent in ("notify.echo_show_5_c0ffee_screen", "notify.all_echo_show_screens"):
                check(await hr.until(lambda: _state_is(c, ent), 10), f"the notify entity {ent}")
                feats = (await c.get(f"/api/states/{ent}"))["attributes"].get("supported_features")
                check(feats == 1, f"  it takes a title (NotifyEntityFeature.TITLE): {feats}")
                words = f"{ent} {{{{ states('input_boolean.coffee_maker') }}}} {{% if true %}}x{{% endif %}}"
                await c.ws(type="call_service", domain="notify", service="send_message", target={"entity_id": ent},
                           service_data={"title": "Note", "message": words})
                check(await hr.until(sync(lambda: f"cm {words}" in last_card(ui_log)), 10),
                      f"  its message on the screen as sent, not rendered: {last_card(ui_log)}")
            # a button whose action fails (a script since renamed): logged, and the card still goes
            await c.ws(type="call_service", domain="hassmic_cards", service="show",
                       service_data={"id": "broken", "timeout": 30, "card": {"type": "markdown", "title": "Broken", "content": "x"},
                                     "actions": [{"action": "GONE", "title": "Gone", "perform_action": "script.renamed_long_ago"}]})
            check(await hr.until(sync(lambda: "cu 0\tGone" in last_card(ui_log)), 10), "a button whose action is no more")
            ui_line(fifo, f"card tap {last_card(ui_log)[0].split()[1]} 0 press")
            check(await hr.until(sync(lambda: not any("\tBroken\t" in l for l in last_card(ui_log)[:1])), 10), "  pressed: the card goes all the same")
            check(await hr.until(lambda: _state_is(c, "event.echo_show_5_c0ffee_card_button"), 5)
                  and (await c.get("/api/states/event.echo_show_5_c0ffee_card_button"))["attributes"].get("action") == "GONE",
                  "  and the Card button event entity fired")
            # show_message: plain fields, a button; the response names the card and where it went
            r = await c.ws(type="call_service", domain="hassmic_cards", service="show_message", return_response=True,
                           service_data={"title": "Dryer", "message": "Done drying", "id": "dryer", "button_1": "OK", "timeout": 30})
            resp = (r or {}).get("response") or {}
            check(resp.get("id") == "dryer" and resp.get("shown_on") == ["echo_show_5_c0ffee"] and resp.get("failed") == [],
                  f"show_message's response: {resp}")
            check(await hr.until(sync(lambda: "cm Done drying" in last_card(ui_log) and "cu 0\tOK" in last_card(ui_log)), 10),
                  f"  its card: the words and the button: {last_card(ui_log)}")
            await c.ws(type="call_service", domain="hassmic_cards", service="clear", service_data={"id": "dryer"})
            # a target that names no Echo Show: an error, not every Show
            try:
                await c.ws(type="call_service", domain="hassmic_cards", service="show",
                           service_data={"card": {"type": "markdown", "content": "x"}}, target={"area_id": "nowhere"})
                err = None
            except RuntimeError as e: err = str(e)
            check(err and "no Echo Show" in err, f"a target with no Echo Show: refused ({err and err[-80:]})")
            # an id longer than the Echo keeps (63 bytes): refused here, not cut there into a card nobody knows
            try:
                await c.ws(type="call_service", domain="hassmic_cards", service="show", service_data={"id": "x" * 64, "card": {"type": "markdown", "content": "x"}})
                err = None
            except RuntimeError as e: err = str(e)
            check(err and "63 bytes" in err, f"an id of 64 bytes: refused ({err and err[-80:]})")
            # the integration: a config entry (configuration.yaml's imported), the Echo's own device of ours with its
            # "Card button" and "Card on screen", options
            ce = [x for x in await c.get("/api/config/config_entries/entry?domain=hassmic_cards")]
            check(len(ce) == 1 and ce[0]["state"] == "loaded", f"a config entry, imported from configuration.yaml: {[(x['title'], x['state']) for x in ce]}")
            btn_id, scr_id = "event.echo_show_5_c0ffee_card_button", "sensor.echo_show_5_c0ffee_card_on_screen"
            check(await hr.until(lambda: _state_is(c, scr_id), 10) and await _state_is(c, btn_id),
                  "the Echo's entities: event.echo_show_5_c0ffee_card_button, sensor.echo_show_5_c0ffee_card_on_screen")
            await c.ws(type="call_service", domain="hassmic_cards", service="show",
                       service_data={"id": "shown", "timeout": 30, "card": {"type": "markdown", "title": "On screen", "content": "x"},
                                     "actions": [{"action": "DT", "title": "Device trigger"}]})
            check(await hr.until(lambda: _state(c, scr_id, "shown"), 10), "Card on screen: the card's id while it shows")
            # a device trigger on our device of the Echo (the automation editor's): its button pressed
            reg = await c.ws(type="config/entity_registry/get", entity_id=btn_id)
            trig = await c.ws(type="device_automation/trigger/list", device_id=reg["device_id"])
            check(any(t.get("type") == "card_button" and t.get("domain") == "hassmic_cards" for t in trig), f"a device trigger, card_button: {trig}")
            await c.post("/api/config/automation/config/dt_test", {"alias": "dt test", "triggers": [{"trigger": "device", "domain": "hassmic_cards",
                         "device_id": reg["device_id"], "type": "card_button", "action": "DT"}],
                         "actions": [{"action": "input_boolean.turn_off", "target": {"entity_id": "input_boolean.coffee_maker"}}]})
            await c.ws(type="call_service", domain="automation", service="reload", service_data={})
            await asyncio.sleep(1.5)
            seq = last_card(ui_log)[0].split()[1]
            ui_line(fifo, f"card tap {seq} 0 press")
            check(await hr.until(lambda: _state(c, "input_boolean.coffee_maker", "off"), 10), "  pressed: the device trigger's automation ran")
            st = await c.get(f"/api/states/{btn_id}")
            check(st["attributes"].get("event_type") == "pressed" and st["attributes"].get("action") == "DT" and st["attributes"].get("card") == "shown",
                  f"  and the Card button event entity: {st['attributes']}")
            check(await hr.until(lambda: _state_not(c, scr_id, "shown"), 10), "Card on screen: another (the notice under it) once the button took it away")
            await c.ws(type="call_service", domain="input_boolean", service="turn_on", service_data={"entity_id": "input_boolean.coffee_maker"})
            # a card the Echo refuses: on its sensor, in the logbook's event
            await c.ws(type="call_service", domain="esphome", service="echo_show_5_c0ffee_show_card", service_data={"card": '{"v": 1}'})
            async def refused(): return "version 1" in str((await c.get(f"/api/states/{scr_id}"))["attributes"].get("refused"))
            check(await hr.until(refused, 10), "a card the Echo refused: why, on Card on screen")
            # the card library: a dashboard of views, one card each (built in the card editor), shown by name with variables
            await c.ws(type="lovelace/dashboards/create", url_path="echo-cards", title="Echo cards", mode="storage")
            await c.ws(type="lovelace/config/save", url_path="echo-cards", config={"views": [
                {"path": "laundry", "title": "Laundry", "cards": [{"type": "markdown", "title": "{{ who }} is done",
                                                                     "content": "Ran {{ minutes }} min, the coffee maker {{ states('input_boolean.coffee_maker') }}"}]}]})
            await c.ws(type="call_service", domain="hassmic_cards", service="show",
                       service_data={"card": "laundry", "variables": {"who": "Washer", "minutes": 52}, "timeout": 10})
            check(await hr.until(sync(lambda: "cm Ran 52 min, the coffee maker on" in last_card(ui_log)
                                      and last_card(ui_log)[0].endswith("\tWasher is done\t")), 10), f"a library card by name, its variables: {last_card(ui_log)}")
            try:
                await c.ws(type="call_service", domain="hassmic_cards", service="show", service_data={"card": "nope"})
                err = None
            except RuntimeError as e: err = str(e)
            check(err and "no card 'nope'" in err, f"a library card that is not there: refused ({err and err[-70:]})")
            # diagnostics: the Shows, the live cards (no tokens), the library
            diag = await c.get(f"/api/diagnostics/config_entry/{ce[0]['entry_id']}")
            d = (diag or {}).get("data") or {}
            check([x.get("node") for x in d.get("shows", [])] == ["echo_show_5_c0ffee"] and d.get("library", {}).get("cards") == ["laundry"]
                  and "token" not in json.dumps(d.get("live_cards")), f"diagnostics: {json.dumps(d)[:300]}")
            # options: set from the UI's options flow (the entry reloads); a bad weather pattern is refused there
            def options_form(schema):
                return {x["name"]: ({y["name"]: y.get("default") for y in x["schema"]} if x.get("type") == "expandable" else x.get("default"))
                        for x in schema}
            f = await c.post("/api/config/config_entries/options/flow", {"handler": ce[0]["entry_id"]})
            form = options_form(f.get("data_schema", [])); form["advanced"]["weather_tools"] = "weather(("
            r = await c.post(f"/api/config/config_entries/options/flow/{f['flow_id']}", form)
            check(isinstance(r, dict) and r.get("type") == "form" and (r.get("errors") or {}).get("advanced") == "invalid_pattern",
                  f"the options flow: a bad pattern refused: {r.get('errors') if isinstance(r, dict) else r}")
            form["advanced"]["weather_tools"] = "weather|forecast"; form["timeout"] = 7
            r = await c.post(f"/api/config/config_entries/options/flow/{f['flow_id']}", form)
            check(isinstance(r, dict) and r.get("type") == "create_entry", f"the options flow: {r}")
            # the blueprints: a doorbell's camera card; an appliance done (its Remind me brings it back, Done takes it away)
            await c.ws(type="call_service", domain="input_button", service="press", service_data={"entity_id": "input_button.doorbell"})
            check(await hr.until(sync(lambda: any(l.startswith("ci card") for l in last_card(ui_log)) and "cm Someone is at the door" in last_card(ui_log)), 10),
                  f"the doorbell_camera blueprint: the camera's card: {last_card(ui_log)}")
            await c.ws(type="call_service", domain="hassmic_cards", service="clear", service_data={"id": "doorbell_door"})
            async def washer(w): await c.ws(type="call_service", domain="input_number", service="set_value", service_data={"entity_id": "input_number.washer_power", "value": w})
            await washer(50); await asyncio.sleep(0.5); await washer(2); await asyncio.sleep(2)     # (low from 50: under running_above)
            check(not any("\tTest washer is done\t" in l for l in last_card(ui_log)[:1]), "the appliance_done blueprint: low from under Running above, no card")
            await washer(500); await asyncio.sleep(0.5); await washer(2)
            check(await hr.until(sync(lambda: bool(last_card(ui_log)) and last_card(ui_log)[0].endswith("\tTest washer is done\t" + last_card(ui_log)[0].split("\t")[-1])
                                      and "cu 0\tRemind me (0 min)" in last_card(ui_log)), 10), f"the appliance_done blueprint: its card: {last_card(ui_log)}")
            seq = last_card(ui_log)[0].split()[1]
            ui_line(fifo, f"card tap {seq} 0 press")
            check(await hr.until(sync(lambda: not any(l.startswith(f"cb {seq} ") for l in last_card(ui_log))), 5), "  Remind me: snoozed, the card goes")
            check(await hr.until(sync(lambda: bool(last_card(ui_log)) and "\tTest washer is done\t" in last_card(ui_log)[0]), 15),
                  f"  and comes back after its snooze (3 s): {last_card(ui_log)[:1]}")
            seq = last_card(ui_log)[0].split()[1]
            ui_line(fifo, f"card tap {seq} 1 press")
            check(await hr.until(sync(lambda: not any(l.startswith(f"cb {seq} ") for l in last_card(ui_log))), 10), "  Done: gone")
            try:
                await c.ws(type="call_service", domain="hassmic_cards", service="show", service_data={"card": {"type": "video"}})
                err = None
            except RuntimeError as e: err = str(e)
            check(err and "an Echo Show draws" in err, f"a card type it cannot draw: refused ({err and err[-90:]})")
    finally:
        echo.stop(); hass.terminate()
        try: hass.wait(20)
        except subprocess.TimeoutExpired: hass.kill()
        if check.failed:
            print("--- Echo log tail\n" + "".join(echo.text().splitlines(True)[-40:]))
            print("--- Home Assistant log tail\n" + "".join(open(os.path.join(tmp, "ha.log"), errors="replace").readlines()[-40:]))


async def _has(echo, text, n=1): return echo.text().count(text) >= n
def sync(f):
    """a plain condition as hr.until wants it (awaited)"""
    async def g(): return f()
    return g
async def _state_not(c, entity, not_want):
    try: st = (await c.get(f"/api/states/{entity}")).get("state"); return bool(st) and st != not_want
    except Exception: return False


async def _state_is(c, entity):
    try: return bool((await c.get(f"/api/states/{entity}")).get("state"))
    except Exception: return False


def ui_line(fifo, line):
    """a line from the UI (the stand-in UI reopens its FIFO after each writer: EPIPE in between, and again)"""
    for _ in range(20):
        try:
            with open(fifo, "wb") as q: q.write((line + "\n").encode())
            return
        except BrokenPipeError: time.sleep(0.05)


async def _state(c, entity, want):
    try: return (await c.get(f"/api/states/{entity}"))["state"] == want
    except Exception: return False


def main():
    if os.environ.get("HASSMIC_IN_NETNS") != "1":
        if not os.path.exists(hr.HA_PY):
            hr.skip(f"no Home Assistant at {hr.HA_PY} (.venv/bin/python tests/ha_real_dropin.py --setup makes one)")
        if not os.path.exists(os.path.join(hr.ROOT, "build", "hassmic-host-screen")): sys.exit("make build/hassmic-host-screen first")
        if not shutil.which("ip"): hr.skip("no ip (iproute2)")
        if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode:
            hr.skip("unshare -rn is not allowed here (user namespaces)")
        env = dict(os.environ, HASSMIC_IN_NETNS="1")
        sys.exit(subprocess.run(["unshare", "-rn", sys.executable, os.path.abspath(__file__), *sys.argv[1:]], env=env).returncode)
    hr.loopback_up()
    tmp = tempfile.mkdtemp(prefix="ha-cards-")
    try: asyncio.run(run(tmp))
    finally:
        if os.environ.get("KEEP_TMP"): print("kept", tmp)
        else: shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
