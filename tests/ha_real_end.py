#!/usr/bin/env python3
"""The end of a request heard on the Echo (micend.c, issue #66) in a real Home Assistant: the Echo ends the audio
itself (VoiceAssistantAudio end=true, proto_esphome.c end_audio), and Home Assistant must take that as the end of
speech and go on (speech-to-text's result, the intent), not as an abort.  aioesphomeapi maps end=true to
handle_stop(False) and start=false to handle_stop(True); this checks what Home Assistant's pipeline makes of it.

Speech-to-text is a stand-in (custom_components/hassmic_fake, a legacy provider): it reads the audio stream to its
end, notes how much came and when it ended, and answers "what time is it" (one of Home Assistant's own sentences);
text-to-speech too (0.1 s of silence: the namespace has no internet, and an ESPHome satellite's pipeline needs one).  The Echo plays tests/fake_ha_end.py's capture: a talker, dishes as
loud with the front end's records saying no speech, other voices 20 dB under; setting end_of_request=normal.

Same isolation and Home Assistant as tests/ha_real_dropin.py (its helpers; build/ha-venv from its --setup)."""
import asyncio, base64, json, os, shutil, signal, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ha_real_dropin as hr
from ha_real_dropin import check, HA_PORT

STT = '''
import time
from homeassistant.components.stt import (AudioBitRates, AudioChannels, AudioCodecs, AudioFormats, AudioSampleRates,
                                          Provider, SpeechResult, SpeechResultState)
LOG = %r

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
        with open(LOG, "a") as f: f.write("start %%.3f\\n" %% time.monotonic())
        async for chunk in stream: n += len(chunk)
        with open(LOG, "a") as f: f.write("end %%.3f %%d\\n" %% (time.monotonic(), n))
        return SpeechResult("what time is it", SpeechResultState.SUCCESS)
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


def write_config(conf, stt_log):
    os.makedirs(os.path.join(conf, ".storage"))
    comp = os.path.join(conf, "custom_components", "hassmic_fake")
    os.makedirs(comp)
    json.dump({"domain": "hassmic_fake", "name": "hassmic's stand-in speech-to-text and text-to-speech", "version": "1.0", "documentation": "",
               "codeowners": [], "requirements": [], "dependencies": [], "iot_class": "local_push"}, open(os.path.join(comp, "manifest.json"), "w"))
    open(os.path.join(comp, "__init__.py"), "w").write("")
    open(os.path.join(comp, "stt.py"), "w").write(STT % stt_log)
    open(os.path.join(comp, "tts.py"), "w").write(TTS)
    open(os.path.join(conf, "configuration.yaml"), "w").write(
        "homeassistant:\n  name: Test\n  unit_system: metric\n  time_zone: UTC\n  latitude: 0\n  longitude: 0\n  elevation: 0\n"
        "  country: DE\n  internal_url: http://127.0.0.1:%d\napi:\nconfig:\nonboarding:\nconversation:\nassist_pipeline:\n"
        "stt:\n  - platform: hassmic_fake\ntts:\n  - platform: hassmic_fake\nlogger:\n  default: warning\n  logs:\n    homeassistant.components.esphome: info\n"
        "    homeassistant.components.assist_pipeline: debug\n" % HA_PORT)
    json.dump({"version": 2, "minor_version": 2, "key": "http", "data": {
        "stable": {"server_port": HA_PORT, "server_host": ["127.0.0.1"], "cors_allowed_origins": [], "login_attempts_threshold": -1,
                   "ip_ban_enabled": False, "ssl_profile": "modern", "use_x_frame_options": True,
                   "created_at": "2026-10-11T00:00:00+00:00", "error": None, "error_message": None},
        "pending": None, "yaml_migration_done": True}}, open(os.path.join(conf, ".storage", "http"), "w"))


async def run(tmp):
    import aiohttp
    from fake_ha_end import capture
    conf, stt_log, cap = os.path.join(tmp, "ha"), os.path.join(tmp, "stt.log"), os.path.join(tmp, "cap.raw")
    write_config(conf, stt_log)
    capture(cap)
    os.environ.update(HASSMIC_CAP=cap, HASSMIC_FAKE_KEYWORD="1")          # Echo.start passes the environment on
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    echo = hr.Echo(tmp, "Echo Dot 3 a1b2c3", "02:00:00:a1:b2:c3", 26190, net, os.path.join(hr.ROOT, "build", "hassmic-host"))
    open(os.path.join(echo.state, "config"), "w").write("end_of_request=normal\n")
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
            check(hr.interfaces() == ["lo"], f"Home Assistant up; the namespace has no interface but loopback ({hr.interfaces()})")
            for _ in range(60):                         # assist_pipeline's commands come once it is set up
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
            await hr.add_esphome(c, echo)
            check(await hr.until(lambda: _has(echo, "voice assistant: subscribed"), 20), "the Echo in Home Assistant, its voice assistant subscribed")
            # Home Assistant's own end after 1.25 s of quiet ("Finished speaking detection" relaxed), the Echo's after 0.7:
            # whichever of the two is first must be the Echo's, whatever HA's VAD makes of the synthetic sounds
            vad = next((x["entity_id"] for x in await c.ws(type="config/entity_registry/list")
                        if x.get("unique_id", "").endswith("-vad_sensitivity")), None)
            check(bool(vad), f"Home Assistant's finished speaking detection select: {vad}")
            await c.ws(type="call_service", domain="select", service="select_option", target={"entity_id": vad},
                       service_data={"option": "relaxed"})
            # the wake word 0.3 s before the talker comes round in the loop (a run that starts in the quiet part has
            # HA's VAD hear a start and an end before the talker: not this test's case).  Where the Echo is in the loop
            # from its own count: the PC's capture is slower than the clock (50 ms of sleep per block, and an empty
            # block at each rewind, audio_file.c), so the time since its start drifts by tenths of seconds while Home
            # Assistant starts
            n = os.path.getsize(cap) // 2
            loop = (n // 800 + 1) * 800                 # whole blocks, the last short or empty
            at, t = await capture_position(echo)
            wait = ((loop - at % loop) % loop) / 16000 - 0.3
            while wait < 0.2: wait += loop / 16000
            await asyncio.sleep(wait - (time.monotonic() - t))
            echo.proc.send_signal(signal.SIGUSR1)                            # the wake word
            ok = await hr.until(lambda: _has(echo, "end of request: heard here"), 20)
            t_echo = time.monotonic()
            check(ok, "the Echo ended the request itself (end of request: heard here)")
            await hr.until(lambda: _stt(stt_log, "end"), 5)
            ends = [l.split() for l in _lines(stt_log) if l.startswith("end ")]
            check(bool(ends) and abs(float(ends[0][1]) - t_echo) < 1.0 and int(ends[0][2]) > 32000,
                  f"Home Assistant's speech-to-text stream ended with it: {ends[:1]}, the Echo's end seen at {t_echo:.3f}")
            check(await hr.until(lambda: _has(echo, "transcript: what time is it"), 10), "the transcript came back to the Echo")
            events = []
            async def run_ended():                      # Home Assistant's record of the run, once it has ended
                nonlocal events
                runs = (await c.ws(type="assist_pipeline/pipeline_debug/list", pipeline_id=p["id"]))["pipeline_runs"]
                events = (await c.ws(type="assist_pipeline/pipeline_debug/get", pipeline_id=p["id"],
                                     pipeline_run_id=runs[-1]["pipeline_run_id"]))["events"] if runs else []
                return any(e["type"] in ("run-end", "error") for e in events)
            await hr.until(run_ended, 10)
            kinds = [e["type"] for e in events]
            stt_end = next((e["data"] for e in events if e["type"] == "stt-end"), {})
            intent = next((e["data"] for e in events if e["type"] == "intent-end"), {})
            speech = (((intent.get("intent_output") or {}).get("response") or {}).get("speech") or {}).get("plain", {}).get("speech")
            check("stt-end" in kinds and "intent-end" in kinds and "run-end" in kinds and "error" not in kinds,
                  f"the pipeline went on to the intent, no abort, no error: {kinds}")
            check("stt-vad-start" in kinds and "stt-vad-end" not in kinds,
                  "  Home Assistant's own VAD heard speech start and no end (the dishes and the others): the Echo's end it was")
            check((stt_end.get("stt_output") or {}).get("text") == "what time is it" and bool(speech),
                  f"  its text and Home Assistant's answer: {stt_end.get('stt_output')}, {speech!r}")
            check(await hr.until(lambda: _has(echo, "-> idle"), 10) and "cancelled" not in echo.text(),
                  "the Echo back to idle (reply played), nothing cancelled")
    finally:
        echo.stop(); hass.terminate()
        try: hass.wait(20)
        except subprocess.TimeoutExpired: hass.kill()
        if check.failed:
            print("--- Echo log tail\n" + "".join(echo.text().splitlines(True)[-40:]))
            print("--- Home Assistant log tail\n" + "".join(open(os.path.join(tmp, "ha.log"), errors="replace").readlines()[-40:]))


async def capture_position(echo):
    """the Echo's capture sample next to be read and when: what SIGTTIN's dump logs as it starts (stopped again)"""
    seen = echo.text().count("capture dump: on")
    echo.proc.send_signal(signal.SIGTTIN)
    while echo.text().count("capture dump: on") == seen: await asyncio.sleep(0.005)
    t = time.monotonic()
    line = [l for l in echo.text().splitlines() if "capture dump: on" in l][-1]
    echo.proc.send_signal(signal.SIGTTIN)
    return int(line.rsplit(" ", 1)[1]) + 800, t             # the sample of the block logged, and its 800


def _lines(path):
    try: return open(path).read().splitlines()
    except OSError: return []
async def _has(echo, text): return text in echo.text()
async def _stt(path, what): return any(l.startswith(what + " ") for l in _lines(path))


def main():
    if os.environ.get("HASSMIC_IN_NETNS") != "1":
        if not os.path.exists(hr.HA_PY):
            hr.skip(f"no Home Assistant at {hr.HA_PY} (.venv/bin/python tests/ha_real_dropin.py --setup makes one)")
        if not os.path.exists(os.path.join(hr.ROOT, "build", "hassmic-host")): sys.exit("make host first")
        if not shutil.which("ip"): hr.skip("no ip (iproute2)")
        if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode:
            hr.skip("unshare -rn is not allowed here (user namespaces)")
        env = dict(os.environ, HASSMIC_IN_NETNS="1")
        sys.exit(subprocess.run(["unshare", "-rn", sys.executable, os.path.abspath(__file__), *sys.argv[1:]], env=env).returncode)
    hr.loopback_up()
    tmp = tempfile.mkdtemp(prefix="ha-end-")
    try: asyncio.run(run(tmp))
    finally: shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
