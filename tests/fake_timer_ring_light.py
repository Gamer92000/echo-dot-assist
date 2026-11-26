#!/usr/bin/env python3
"""Assist timer events through the daemon to a fake calibrated LED frame; signed web settings too."""
import asyncio
import json
import os
import tempfile
from pathlib import Path

from aioesphomeapi import VoiceAssistantTimerEventType as Tm
from esphome_common import check, home_assistant, run, sync, until


async def main(e):
    ha = await home_assistant(e)
    cli, page = ha.cli, e.page
    frame = Path(e.env["HASSMIC_FAKE_FRAME"])

    def pixels():
        text = frame.read_text().strip()
        return [text[i:i + 6] for i in range(0, len(text), 6)]

    def lit():
        return sum(p != "000000" for p in pixels())

    async def event(kind, ident, total, left, active=True):
        cli.send_voice_assistant_timer_event(kind, ident, ident, total, left, active)
        await sync(cli)

    started, updated, cancelled = (Tm.VOICE_ASSISTANT_TIMER_STARTED, Tm.VOICE_ASSISTANT_TIMER_UPDATED,
                                   Tm.VOICE_ASSISTANT_TIMER_CANCELLED)
    settings = {s["name"]: s["value"] for s in page.state()["settings"]}
    check("timer_ring_light" in settings and not settings["timer_ring_light"], "timer progress off by default on supported ring")
    page.set(timer_ring_light="on", timer_ring_light_colour="16711680", timer_ring_light_brightness="100")
    await event(started, "long", 120, 120)
    await until(lambda: lit() == 12)
    check(lit() == 12 and all(p[2:] == "0000" for p in pixels()), "full red ring without enabling Light ring live")
    await event(started, "short", 60, 30)
    await until(lambda: lit() == 6)
    check(lit() == 6, "active timer ending first wins")
    await event(updated, "short", 60, 30, False)
    await until(lambda: lit() == 12)
    check(lit() == 12, "paused timer yields to another active timer")
    await event(cancelled, "long", 120, 0, False)
    await until(lambda: lit() == 6)
    values = []
    for _ in range(16):
        values.append(frame.read_text())
        await asyncio.sleep(0.12)
    check(lit() == 6 and len(set(values)) > 3, "only paused timer: frozen fill, slow pulse")
    await event(updated, "short", 60, 30)
    await asyncio.sleep(0.2)
    await event(updated, "short", 60, 90)  # HA's original total remains 60: adjusted length becomes 120
    await until(lambda: lit() == 9)
    check(lit() == 9, "adding 60 s adjusts total to 120 s: 90/120 lights nine LEDs")
    await event(updated, "short", 60, 30)
    await until(lambda: lit() == 6)
    check(lit() == 6, "removing 60 s adjusts total back to 60 s: 30/60 lights six LEDs")
    await event(cancelled, "short", 60, 0, False)
    await until(lambda: lit() == 0)
    check(lit() == 0, "last timer cancelled: ring dark")
    await event(started, "tiny", 2, 2)
    await until(lambda: lit() > 0)
    await until(lambda: lit() == 0)
    check(lit() == 0, "monotonic deadline clears progress even before HA finishes it")
    await event(cancelled, "tiny", 2, 0, False)
    saved = e.conf()
    check(saved.get("timer_ring_light") == "on" and saved.get("timer_ring_light_colour") == "16711680"
          and saved.get("timer_ring_light_brightness") == "100", "web options persisted in state/config")
    page.set(timer_ring_light="off")
    await event(started, "disabled", 60, 60)
    await asyncio.sleep(0.3)
    check(lit() == 0, "disabled: new timers leave the ring alone")
    await event(cancelled, "disabled", 60, 0, False)
    await event(started, "existing", 120, 60)
    page.set(timer_ring_light="on")
    await until(lambda: lit() == 6)
    check(lit() == 6, "enabling progress also follows an already running timer")
    await event(cancelled, "existing", 120, 0, False)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="echo-timer-ring-") as d:
        frame = Path(d, "frame")
        frame.write_text("000000" * 12 + "\n")
        Path(d, "led_mute_ring_config.json").write_text(json.dumps({
            "cap_color": [{"color": "black", "cap_color_number": 0}],
            "rgbTable": {"black": [{"pwm": 0.99, "current": 0}] * 102},
        }))
        run(main, (17501, 17502, 17503, 17504, 17505), {
            "HASSMIC_FAKE_FRAME": str(frame), "HASSMIC_LED_ROOT": d,
            "HASSMIC_FAKE_ALSPARAMS": os.path.join(d, "missing-alsparams"),
        }, log=True)