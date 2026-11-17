#!/usr/bin/env python3
"""-w remote: a run Home Assistant ends with an error (its wake word engine gone: "no_wake_word" at once) is asked for
again after a wait, 1 s doubling to 30 s, not at once: on 2026-11-16 an Echo Dot asked 510 times a second until Home
Assistant's core gave up.  A run that ends well brings the next at once and the wait back to 1 s.  The error ring shows
once for failures in a row, and not in do not disturb.  Muted, no run waits for the wake word; unmuted, one does again."""
import asyncio, os, subprocess, sys, tempfile, time
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from esphome_common import check, until, sync, port_open

PORT = int(os.environ.get("HASSMIC_TEST_PORT", "16991"))


async def main():
    state = tempfile.mkdtemp()
    leds = os.path.join(state, "leds")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
               HASSMIC_PLAY=os.path.join(state, "play.raw"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_FAKE_LED=leds,
               HASSMIC_MODELS=os.path.join(state, "models"), HASSMIC_MDNS_FILE=os.path.join(state, "mdns"))
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Dot", "-w", "remote"],
                            env=env, stderr=open(os.path.join(state, "log"), "w"))
    errors = lambda: open(leds).read().count("anim_start_error_short") if os.path.exists(leds) else 0
    try:
        await until(lambda: port_open(PORT))
        cli = APIClient("127.0.0.1", PORT, None)
        await cli.connect(login=True)
        es, _ = await cli.list_entities_services()
        by = {x.object_id: x for x in es}
        starts = []
        fail = {"on": True}

        async def handle_start(conv_id, f, settings, phrase):
            starts.append(time.monotonic())
            if fail["on"]:      # as Home Assistant without its wake word engine: the run ends at once
                cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
                cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "no_wake_word", "message": "No wake word detected"})
                cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
            return 0

        async def nothing(*a): pass
        cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=nothing, handle_audio=nothing)
        await sync(cli)
        t0 = time.monotonic()
        await asyncio.sleep(7.6)
        n = len(starts)
        gaps = [round(b - a, 1) for a, b in zip(starts, starts[1:])]
        check(4 <= n <= 5, f"engine gone: {n} runs in 7.6 s (at once, then after 1, 2 and 4 s), not hundreds: gaps {gaps}")
        check(all(g >= 0.8 for g in gaps) and gaps[1:3] and gaps[1] >= 1.6 * gaps[0] * 0.9, f"the wait doubles: {gaps}")
        check(errors() == 1, f"the error ring once for the failures in a row: {errors()}")

        # the engine is back: the next run waits for the wake word; it ends well, the one after starts at once
        fail["on"] = False
        await until(lambda: len(starts) > n, timeout=10)
        k = len(starts)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_WAKE_WORD_END, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": ""})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "stt-no-text-recognized", "message": "nothing"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        t1 = time.monotonic()
        await until(lambda: len(starts) > k, timeout=3)
        check(len(starts) > k and starts[k] - t1 < 0.5, f"after a run that ended well the next waits for the wake word at once: {starts[k] - t1 if len(starts) > k else None}")

        # failing again: 1 s, not where the wait had got to
        fail["on"] = True; m = len(starts)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "no_wake_word", "message": "No wake word detected"})
        await until(lambda: len(starts) > m, timeout=3)
        g = starts[m] - starts[m - 1] if len(starts) > m else None
        check(g is not None and 0.8 <= g <= 1.6, f"a new run of failures starts with 1 s again: {g}")
        check(errors() == 2, f"and shows the error ring once more: {errors()}")

        # do not disturb: failures show no ring
        fail["on"] = False
        await until(lambda: len(starts) > m + 1, timeout=5)
        cli.switch_command(by["do_not_disturb"].key, True); await sync(cli); await asyncio.sleep(0.3)
        fail["on"] = True; e0 = errors()
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "no_wake_word", "message": "No wake word detected"})
        await asyncio.sleep(2.5)
        check(errors() == e0, f"in do not disturb no error ring: {errors() - e0}")
        cli.switch_command(by["do_not_disturb"].key, False)

        # muted: the run waiting for the wake word ends and none follows; unmuted, one starts
        fail["on"] = False
        await until(lambda: True); await asyncio.sleep(35)              # past the longest wait: one run waits
        p = len(starts)
        cli.switch_command(by["mute"].key, True); await sync(cli); await asyncio.sleep(2.5)
        check(len(starts) == p, f"muted: no run starts: {len(starts) - p}")
        cli.switch_command(by["mute"].key, False); await sync(cli)
        await until(lambda: len(starts) > p, timeout=3)
        check(len(starts) == p + 1, f"unmuted: one run waits for the wake word again: {len(starts) - p}")
        await cli.disconnect()
    finally:
        proc.terminate(); proc.wait()
        print(open(os.path.join(state, "log")).read()[-1500:] if check.failed else "", end="")
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
