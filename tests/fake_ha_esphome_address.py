#!/usr/bin/env python3
"""Home Assistant's side of the ESPHome native API against build/hassmic-host (tests/esphome_common.py), part five:
a new address of the Echo's (netaddr.c): links over the old one closed, mDNS announced again until Home Assistant is
back."""
import asyncio, os, signal, time
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev
from esphome_common import check, until, sync, home_assistant, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17131, 17132, 17133, 17134, 17135


async def main(e):
    proc, state, env = e.proc, e.state, e.env
    ha = await home_assistant(e)
    va = ha.va; started = va.started
    handle_start, handle_stop, handle_audio = va.handle_start, va.handle_stop, va.handle_audio
    c = ha.cli
    # A new address (issue #17, netaddr.c): the link over the old one is closed at once, though it held the voice
    # assistant; root is asked to announce mDNS again, at once and again while nobody comes back (Home Assistant
    # ignores announcements while it still holds the dead link); back over the new address, the voice assistant is free
    mreq = os.path.join(state, "mdns"); addrs = env["HASSMIC_FAKE_ADDRS"]
    if os.path.exists(mreq): os.remove(mreq)
    async def alive(cl):
        try: await asyncio.wait_for(cl.device_info(), 1.5); return True
        except Exception: return False
    with open(addrs, "w") as f: f.write("10.0.0.2\n")
    t0 = time.monotonic()
    while await alive(c) and time.monotonic() - t0 < 6: await asyncio.sleep(0.3)
    check(not await alive(c), "a new address: the link over the old one is closed (it held the voice assistant)")
    check(os.path.exists(mreq) and open(mreq).read() == "new address\n", "and root is asked to announce mDNS again")
    os.remove(mreq)
    await until(lambda: os.path.exists(mreq), 8)
    check(os.path.exists(mreq) and open(mreq).read().startswith("no Home Assistant for "),
          f"nobody back after the first wait: announced again ({open(mreq).read().strip() if os.path.exists(mreq) else 'nothing'})")
    with open(addrs, "w") as f: f.write("10.0.0.2\n127.0.0.1\n")           # the address the test connects to, "new"
    await asyncio.sleep(2.5)
    c = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(c.connect(login=True), 5)
    c.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio)
    await sync(c); started.clear(); proc.send_signal(signal.SIGUSR1)
    try: await asyncio.wait_for(started.wait(), 5)
    except asyncio.TimeoutError: pass
    check(started.is_set(), "back over the new address: the voice assistant is this link's, the wake word runs a pipeline")
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
    await sync(c)
    if os.path.exists(mreq): os.remove(mreq)
    await asyncio.sleep(6)
    check(not os.path.exists(mreq), "Home Assistant connected: no more announcements")
    await c.disconnect()

run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT))
