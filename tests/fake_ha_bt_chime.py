#!/usr/bin/env python3
"""A phone connects to the Echo (HASSMIC_FAKE_PHONE plays a2dp.c's report): the chime and the words are two settings.
  - the chime is the sound "Bluetooth connected" / "Bluetooth disconnected" (stock's file, a stand-in here), the words
    "Connected to <phone>" are Home Assistant's, asked as assist_satellite.announce, and "Bluetooth announcements"
    switches only those: with them off the chime still plays (issue: the voice off, the chime kept)
  - each sound's own switch silences its chime and leaves the words"""
import asyncio, os, re, tempfile
from aioesphomeapi import APIClient
from esphome_common import check, until, wav_bytes, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17221, 17222, 17223, 17224, 17225
EARCONS = tempfile.mkdtemp() + "/"
for name, secs in (("state_bluetooth_connected", 0.30), ("state_bluetooth_disconnected", 0.40)):
    open(EARCONS + name + ".wav", "wb").write(wav_bytes(48000, secs))
ON, OFF = 14400, 19200                                    # samples of those two files at 48 kHz, as the Echo logs them


async def main(e):
    phone = e.env["HASSMIC_FAKE_PHONE"]
    c = APIClient("127.0.0.1", PORT, None); await c.connect(login=True)
    await c.list_entities_services()
    asked = []
    c.subscribe_service_calls(lambda call: asked.append(call))
    await asyncio.sleep(0.3)

    chimes = lambda: [int(n) for n in re.findall(r"earcon: (\d+) samples", e.log())]
    async def call(what, name="Pixel 9"):
        n0, a0 = len(chimes()), len(asked)
        open(phone, "w").write(f"{what} {name}\n")
        await until(lambda: not os.path.exists(phone))
        await asyncio.sleep(1.2)                                  # the chime plays, the words are asked after it
        return chimes()[n0:], asked[a0:]

    def said(calls):                                       # the connect words only: a disconnect's wait for the 15 s between two
        return [c.data.get("message") for c in calls if c.service == "assist_satellite.announce" and c.data.get("message", "").startswith("Connected")]

    check(e.pv("bluetooth_announcements") == 1 and e.pv("sound_bluetooth_on") == 1, "Bluetooth announcements and the sounds: on by default")
    sounds, words = await call("connect")
    check(sounds == [ON] and said(words) == ["Connected to Pixel 9"], f"both on: the chime, then the words ({sounds}, {said(words)})")
    sounds, words = await call("disconnect")
    check(sounds == [OFF], f"the other chime when the phone leaves ({sounds}); its words wait for the last one's ({said(words)})")

    # announcements off: the voice is gone, the chime stays
    e.page.set(bluetooth_announcements="off")
    sounds, words = await call("connect")
    check(sounds == [ON] and not said(words), f"announcements off: the chime still plays, no words ({sounds}, {said(words)})")
    sounds, words = await call("disconnect")
    check(sounds == [OFF] and not said(words), f"announcements off: the chime when it leaves too ({sounds}, {said(words)})")

    # the chime's own switches
    e.page.set(sound_bluetooth_on="off")
    sounds, words = await call("connect")
    check(not sounds and not said(words), f"announcements and the connect sound off: silence ({sounds}, {said(words)})")
    sounds, words = await call("disconnect")
    check(sounds == [OFF], f"the connect sound off leaves the disconnect sound ({sounds})")

    # the words alone: after the 15 s Home Assistant is given between two announcements
    e.page.set(bluetooth_announcements="on")
    await asyncio.sleep(15)
    a0 = len(asked)
    sounds, _ = await call("connect")
    await until(lambda: said(asked[a0:]), 20)
    check(not sounds and said(asked[a0:]) == ["Connected to Pixel 9"], f"the connect sound off, the words on: the words only ({sounds}, {said(asked[a0:])})")


phone = tempfile.mktemp(suffix=".phone")
run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT),
    {"HASSMIC_FAKE_PHONE": phone, "HASSMIC_EARCONS": EARCONS, "HASSMIC_BT_DEV": "/dev/null"}, log=True)
