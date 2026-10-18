#!/usr/bin/env python3
"""Home Assistant's side of the ESPHome native API against build/hassmic-host (tests/esphome_common.py), part three:
features switched on the settings page, each closing the link so Home Assistant lists entities again: sound
detection (windows of 3 s here, HASSMIC_FAKE_SOUND_WINDOW: the model's are 10 s), Wi-Fi motion."""
import asyncio, random
from aioesphomeapi import NumberInfo, NumberState, EventInfo, BinarySensorInfo, BinarySensorState
from aioesphomeapi import APIClient
from aioesphomeapi.model import Event
from esphome_common import check, until, sync, home_assistant, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17111, 17112, 17113, 17114, 17115


async def main(e):
    page, conf, pv, rx_stat = e.page, e.conf, e.pv, e.rx_stat
    ha = await home_assistant(e)
    c, by, st3 = ha.cli, ha.by, ha.states
    last3 = lambda k, t: ([x.state for x in st3 if isinstance(x, t) and x.key == k] or [None])[-1]
    # A feature switched on the page closes the link; Home Assistant comes back and lists entities again
    async def relist(old):
        try: await old.disconnect()
        except Exception: pass
        n = APIClient("127.0.0.1", PORT, None); await asyncio.wait_for(n.connect(login=True), 5)
        es, _ = await n.list_entities_services(); sts = []; n.subscribe_states(sts.append); await sync(n)
        return n, {x.object_id: x for x in es}, sts
    # sound detection: off by default, no entity; on, one event entity reporting what the detector hears
    # (sound_none.c's dog, once per window of the capture: 3 s here), kept in the settings file
    check("sound" not in by and "sound_detection" not in by and pv("sound_detection") == 0, "sound detection off by default: no entity in Home Assistant")
    page.set(sound_detection="on")
    c, cby, st3 = await relist(c)
    ev = cby.get("sound")
    check(isinstance(ev, EventInfo)
          and list(ev.event_types) == ["smoke_or_co_alarm", "glass_break", "dog_bark", "baby_cry", "snoring", "cough", "water", "beeping_appliance"]
          and conf().get("sound_detection") == "on", f"sound detection switched on on the page: the event entity, kept in the settings file: {conf()}")
    for _ in range(16):
        if [x for x in st3 if isinstance(x, Event)]: break
        await asyncio.sleep(0.5)
    evs = [(x.key, x.event_type) for x in st3 if isinstance(x, Event)]
    check(evs[:1] == [(ev.key, "dog_bark")], f"the detector's dogBark arrives as event dog_bark within a window: {evs}")
    # a window in which the Echo played something itself is dropped: here an announcement
    n0 = len(evs)
    async def no_pipeline(*a): return 0
    async def nothing(*a): pass
    unsub = c.subscribe_voice_assistant(handle_start=no_pipeline, handle_stop=nothing, handle_audio=nothing,
                                handle_announcement_finished=nothing)      # announcements answer the assistant's client
    await sync(c)
    res = await c.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "x")
    await asyncio.sleep(2)                                                 # windows that end within 3 s of it
    evs = [(x.key, x.event_type) for x in st3 if isinstance(x, Event)]
    check(res.success and len(evs) == n0, f"no sound event for the window with the announcement in it: {evs[n0:]}")
    for _ in range(20):
        if len([x for x in st3 if isinstance(x, Event)]) > n0: break
        await asyncio.sleep(0.5)
    check(len([x for x in st3 if isinstance(x, Event)]) > n0, "sound events again once the Echo has been quiet for a window")
    unsub()
    page.set(sound_detection="off")
    c, cby, st3 = await relist(c)
    check("sound" not in cby and conf().get("sound_detection") == "off", "sound detection switched off again: the entity gone")
    # Wi-Fi motion (experimental): off by default, the sensor unknown while off; on, a still level is no motion, a
    # wobbling one is, and it clears after the hold (3 s in the PC build, 30 s on the Echo)
    check("wifi_motion" not in cby and "wifi_motion_detection" not in cby and pv("wifi_motion") == 0 and pv("wifi_motion_sensitivity") == 5,
          "Wi-Fi motion off by default (sensitivity 5): no entity in Home Assistant")
    page.set(wifi_motion="on")
    c, cby, st3 = await relist(c)
    wbs, wsn = cby.get("wifi_motion"), cby.get("wifi_motion_sensitivity")
    check(isinstance(wbs, BinarySensorInfo) and wbs.device_class == "motion" and "experimental" in wbs.name
          and isinstance(wsn, NumberInfo) and (wsn.min_value, wsn.max_value) == (1, 10) and int(wsn.entity_category) == 1,
          "Wi-Fi motion on: its entities listed, named experimental")
    wstate = lambda: ([x for x in st3 if isinstance(x, BinarySensorState) and x.key == wbs.key] or [None])[-1]
    await until(lambda: wstate() and not wstate().missing_state)
    check(wstate() and not wstate().missing_state and wstate().state is False
          and conf().get("wifi_motion") == "on" and conf().get("wifi_motion_sensitivity") == "5",
          f"switched on: a steady level is no motion, kept in the settings file: {conf()}")
    for _ in range(60):                                 # someone walking through the path: 6 s
        rx_stat(random.randint(106, 118)); await asyncio.sleep(0.1)
        if wstate().state: break
    check(wstate().state is True, "a wobbling level is motion")
    rx_stat(112)
    await until(lambda: wstate().state is False, 10)
    check(wstate().state is False, "motion clears after the hold")
    c.number_command(wsn.key, 8)
    await until(lambda: last3(wsn.key, NumberState) == 8 and conf().get("wifi_motion_sensitivity") == "8")
    check(last3(wsn.key, NumberState) == 8 and conf().get("wifi_motion_sensitivity") == "8", "sensitivity set and kept")
    page.set(wifi_motion="off")
    c, cby, st3 = await relist(c)
    check("wifi_motion" not in cby and conf().get("wifi_motion") == "off", "switched off again: the entities gone")

run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), {"HASSMIC_FAKE_SOUND_WINDOW": "3000"})
