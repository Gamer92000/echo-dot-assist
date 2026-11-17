#!/usr/bin/env python3
"""A phone connects to the Echo (HASSMIC_FAKE_PHONE plays a2dp.c's report): the chime and the words are two settings.
  - the chime is the sound "Bluetooth connected" / "Bluetooth disconnected" (stock's file, a stand-in here); the words
    "Connected to <phone>" are Home Assistant's, asked as assist_satellite.announce
  - "Bluetooth announcements" switches the words only: off, the chime still plays
  - each sound's own switch silences its chime and leaves the words"""
import asyncio, os, re, tempfile
from aioesphomeapi import APIClient
from esphome_common import check, until, wav_bytes, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17221, 17222, 17223, 17224, 17225
EARCONS = tempfile.mkdtemp() + "/"
for name, secs in (("state_bluetooth_connected", 0.30), ("state_bluetooth_disconnected", 0.40)):
    open(EARCONS + name + ".wav", "wb").write(wav_bytes(48000, secs))
ON, OFF = 14400, 19200                                    # samples of those two files at 48 kHz, as the Echo logs them
WORDS = "Connected to Pixel 9"


async def main(e):
    phone = e.env["HASSMIC_FAKE_PHONE"]
    ha = APIClient("127.0.0.1", PORT, None); await ha.connect(login=True)
    await ha.list_entities_services()
    actions = []                                          # Home Assistant actions the Echo asked for
    ha.subscribe_service_calls(actions.append)            # also what lets the Echo ask ("perform Home Assistant actions")
    await asyncio.sleep(0.3)

    def chimes(): return [int(n) for n in re.findall(r"earcon: (\d+) samples", e.log())]
    # The connect words only: a disconnect's words wait out the 15 s the Echo leaves between two announcements and may
    # come during a later step
    def connect_words(asked): return [a.data.get("message") for a in asked
                                      if a.service == "assist_satellite.announce" and a.data.get("message") == WORDS]

    async def phone_event(what):
        """the phone connects or leaves: the chimes played and the connect words asked after it"""
        n0, a0 = len(chimes()), len(actions)
        open(phone, "w").write(f"{what} Pixel 9\n")
        await until(lambda: not os.path.exists(phone))       # taken by the Echo
        await asyncio.sleep(1.2)                              # the chime plays, the words are asked after it
        return chimes()[n0:], connect_words(actions[a0:])

    check(e.pv("bluetooth_announcements") == 1 and e.pv("sound_bluetooth_on") == 1 and e.pv("sound_bluetooth_off") == 1,
          "Bluetooth announcements and both chimes: on by default")
    sounds, words = await phone_event("connect")
    check(sounds == [ON] and words == [WORDS], f"all on: the chime, then the words ({sounds}, {words})")
    sounds, _ = await phone_event("disconnect")
    check(sounds == [OFF], f"the other chime when the phone leaves ({sounds})")

    # announcements off: the words are gone, the chimes stay
    e.page.set(bluetooth_announcements="off")
    sounds, words = await phone_event("connect")
    check(sounds == [ON] and not words, f"announcements off: the chime still plays, no words ({sounds}, {words})")
    sounds, _ = await phone_event("disconnect")
    check(sounds == [OFF], f"announcements off: the chime when it leaves too ({sounds})")

    # the chimes' own switches
    e.page.set(sound_bluetooth_on="off")
    sounds, words = await phone_event("connect")
    check(not sounds and not words, f"announcements and the connect sound off: silence ({sounds}, {words})")
    sounds, _ = await phone_event("disconnect")
    check(sounds == [OFF], f"the connect sound off leaves the disconnect sound ({sounds})")

    # the words alone, after the 15 s between two announcements
    e.page.set(bluetooth_announcements="on")
    await asyncio.sleep(15)
    a0 = len(actions)
    sounds, _ = await phone_event("connect")
    await until(lambda: connect_words(actions[a0:]), 20)
    words = connect_words(actions[a0:])
    check(not sounds and words == [WORDS], f"the connect sound off, the words on: the words only ({sounds}, {words})")


# HASSMIC_BT_DEV: any file that exists makes the Echo one with Bluetooth (ble_present), so the page has its settings
run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT),
    {"HASSMIC_FAKE_PHONE": tempfile.mktemp(suffix=".phone"), "HASSMIC_EARCONS": EARCONS, "HASSMIC_BT_DEV": "/dev/null"}, log=True)
