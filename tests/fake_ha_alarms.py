#!/usr/bin/env python3
"""The Echo's own alarms (alarms.c, main.c core_alarm_*) against build/hassmic-host as Home Assistant sees them
(tests/esphome_common.py): requests answered on the Echo ("set an alarm for 7 AM on weekdays", "what alarms do I have",
"cancel ..."), the answer spoken through assist_satellite.announce, Home Assistant's entities ("Next alarm", "Alarms",
"Alarm ringing", "Snooze alarm", the "Alarm" event) and actions (set_alarm, delete_alarm), an alarm ringing on the Echo's
clock (moved by the test: HASSMIC_FAKE_CLOCK_OFFSET names a file), snoozed by Home Assistant's button, a volume key and
"snooze", stopped by "stop", and the settings page's /api/alarms.  An alarm system's sentences stay Home Assistant's."""
import asyncio, json, os, signal, tempfile, time
from aioesphomeapi import (BinarySensorState, ButtonInfo, EventInfo, TextSensorInfo, TextSensorState, UserService,
                           VoiceAssistantEventType as Ev)
from aioesphomeapi.model import Event
from esphome_common import KEY_VOLUMEUP, check, until, sync, home_assistant, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17201, 17202, 17203, 17204, 17205
# the Echo's zone and this test's the same: move_clock's minutes are the Echo's (CI runs in UTC)
os.environ["TZ"] = "America/New_York"; time.tzset()
OFFSET = os.path.join(tempfile.mkdtemp(), "clock-offset")
open(OFFSET, "w").write("0\n")


def clock_before(t, by=3):
    """the Echo's clock to `by` seconds before the moment t (its clock's epoch)"""
    off = int(open(OFFSET).read())
    open(OFFSET, "w").write(f"{off + int(t - by - (time.time() + off))}\n")


def move_clock(to_second):
    """the Echo's clock to `to_second` of its current minute (the next minute's, if that is past): an alarm set for the
    next minute then rings within a few seconds"""
    off = int(open(OFFSET).read())
    now = time.time() + off
    want = now - now % 60 + to_second
    if want < now: want += 60
    open(OFFSET, "w").write(f"{off + int(want - now)}\n")
    return time.localtime(want)


async def main(e):
    ha = await home_assistant(e)
    c, by, st, va = ha.cli, ha.by, ha.states, ha.va
    _, services = await c.list_entities_services()
    svc = {s.name: s for s in services if isinstance(s, UserService)}
    said = []                                           # what the Echo asked Home Assistant to say
    def action(call):                                   # Home Assistant runs assist_satellite.announce: the Echo plays it
        said.append(call)
        if call.service == "assist_satellite.announce":
            asyncio.get_running_loop().create_task(c.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, call.data.get("message", "")))
    c.subscribe_home_assistant_states_and_services(on_state=lambda s: None, on_service_call=action,
                                                   on_state_sub=lambda *a: None, on_state_request=lambda *a: None)
    await sync(c)

    nxt, lst, ring, snz, ev = by.get("next_alarm"), by.get("alarms"), by.get("alarm_ringing"), by.get("snooze_alarm"), by.get("alarm")
    check(isinstance(nxt, TextSensorInfo) and nxt.device_class == "timestamp", f"\"Next alarm\": a timestamp text sensor ({nxt})")
    check(isinstance(lst, TextSensorInfo) and ring is not None and isinstance(snz, ButtonInfo), "\"Alarms\", \"Alarm ringing\", \"Snooze alarm\"")
    check(isinstance(ev, EventInfo) and list(ev.event_types) == ["ringing", "snoozed", "stopped", "rang_out", "set", "deleted", "not_understood"],
          f"the \"Alarm\" event entity and its types ({ev and list(ev.event_types)})")
    check("set_alarm" in svc and [a.name for a in svc["set_alarm"].args] == ["time", "days", "label"]
          and "delete_alarm" in svc and [a.name for a in svc["delete_alarm"].args] == ["which"], "the actions set_alarm (time, days, label) and delete_alarm (which)")
    evs = lambda: [x.event_type for x in st if isinstance(x, Event) and x.key == ev.key]
    text = lambda k: ha.last(k.key, TextSensorState)
    ringing = lambda: ha.last(ring.key, BinarySensorState)
    announced = lambda: [x.data.get("message") for x in said if x.service == "assist_satellite.announce"]

    def snoozed_at():
        return [a for a in json.loads(e.page.call("GET", "/api/alarms")[2])["alarms"] if a["label"] == "Test"][0]["snoozed"]

    async def request(words):
        """the wake word, then speech to text heard `words`: whether the Echo ended the run itself"""
        va.started.clear(); va.stopped.clear()
        e.wake(); await asyncio.wait_for(va.started.wait(), 5)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": words})
        await sync(c); await asyncio.sleep(0.3)
        mine = bool(va.stopped)
        if not mine:                                    # Home Assistant's to answer: end the run as it would
            c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
            await sync(c)
        await asyncio.sleep(0.2)
        return mine

    # by voice: answered on the Echo, the run Home Assistant started cancelled before its agent runs
    n0 = len(evs())
    check(await request("Set an alarm for 7 a.m. on weekdays."), "\"set an alarm for 7 AM on weekdays\": the Echo's, the run cancelled")
    await until(lambda: announced() and evs()[n0:] == ["set"])
    check(announced()[-1:] == ["Alarm set for 7 AM on weekdays."] and evs()[n0:] == ["set"],
          f"answered through assist_satellite.announce ({announced()}), the event \"set\"")
    check(said and said[-1].data_template.get("preannounce") == "{{ false }}" and "assist_satellite." in said[-1].data_template.get("entity_id", ""),
          "the announcement is on this satellite, without a chime")
    await until(lambda: text(lst) == "7 AM weekdays")
    check(text(lst) == "7 AM weekdays", f"\"Alarms\": {text(lst)!r}")
    nx = text(nxt)
    check(nx and "T07:00:00" in nx and nx[-6] in "+-", f"\"Next alarm\": 7:00 local, with its offset ({nx})")
    check(await request("what alarms do I have"), "\"what alarms do I have\": the Echo's")
    await until(lambda: len(announced()) >= 2)
    check(announced()[-1:] == ["Your alarm is set for 7 AM on weekdays."], f"its answer: {announced()[-1:]}")
    check(await request("Change my alarm to 8 a.m."), "\"change my alarm to 8 AM\": the Echo's")
    await until(lambda: announced()[-1:] == ["Your alarm is now set for 8 AM on weekdays."] and text(lst) == "8 AM weekdays")
    check(announced()[-1:] == ["Your alarm is now set for 8 AM on weekdays."] and text(lst) == "8 AM weekdays",
          f"the same alarm moved, its days kept, no second one ({announced()[-1:]}, {text(lst)!r})")
    check(not await request("disarm the alarm") and not await request("set a timer for 5 minutes"),
          "an alarm system's \"disarm the alarm\" and a timer stay Home Assistant's")
    check(await request("set an alarm"), "\"set an alarm\" without a time: the Echo's")
    await until(lambda: announced()[-1:] == ["Sorry, I didn't catch the time for the alarm."])
    check(announced()[-1:] == ["Sorry, I didn't catch the time for the alarm."] and evs()[-1] == "not_understood", "and it says it missed the time")

    # media requests about this Echo (main.c media_request): its own volume, and "pause" only for what it plays itself
    check(await request("Set the volume to 3."), "\"set the volume to 3\": this Echo's, the run cancelled")
    await until(lambda: e.pv("volume") == 30)
    check(e.pv("volume") == 30, f"volume 3 is 30 % (Alexa's 0..10): {e.pv('volume')}")
    up = await request("turn it up"); await until(lambda: e.pv("volume") == 40)
    check(up and e.pv("volume") == 40, f"\"turn it up\": a step up ({e.pv('volume')})")
    check(not await request("pause") and not await request("next song"), "\"pause\", \"next song\" with nothing playing here: Home Assistant's (the TV)")

    # from Home Assistant: an action, a time due within seconds on the Echo's clock.  No ramp, and 50 points a minute
    # louder: the escalation shows within seconds
    e.page.set(alarm_ramp=0, alarm_louder=50)
    when = move_clock(50)                                 # 10 s before the next minute
    nm = time.localtime(time.mktime(when) + 60)
    n0 = len(evs())
    await c.execute_service(svc["set_alarm"], {"time": f"{nm.tm_hour:02d}:{nm.tm_min:02d}:00", "days": "", "label": "Test"})
    await until(lambda: evs()[n0:] == ["set"])
    check(evs()[n0:] == ["set"], f"set_alarm with a time selector's \"HH:MM:SS\": set ({evs()[n0:]})")
    st_, _, d = e.page.call("GET", "/api/alarms"); d = json.loads(d)
    mine = [a for a in d["alarms"] if a["label"] == "Test"]
    check(st_ == 200 and len(d["alarms"]) == 2 and mine and mine[0]["days"] == 0 and mine[0]["next"] > 0 and d["clock_ok"] and not d["ringing"],
          f"the page lists both, the action's as a once alarm ({d})")
    await until(lambda: "ringing" in evs()[n0:], 20)
    check(evs()[n0:] == ["set", "ringing"] and ringing() is True, f"it rings at its minute: the event, \"Alarm ringing\" on ({evs()[n0:]})")
    names = ha.last(by["ringing_timers"].key, TextSensorState)
    check(names == "Alarm Test", f"\"Ringing timers\" names it by its label: {names!r}")
    check(json.loads(e.page.call("GET", "/api/alarms")[2])["ringing"], "the page sees it ring")

    await until(lambda: "alarm: louder, at" in e.log(), 10)
    check("alarm: louder, at" in e.log(), "unanswered, it gets louder than the timer volume")

    # snoozed by Home Assistant's button: rings again a snooze later (1 min here)
    e.page.set(alarm_snooze=1)
    c.button_command(snz.key)
    await until(lambda: evs()[-1:] == ["snoozed"])
    check(evs()[-1:] == ["snoozed"] and ringing() is False, "\"Snooze alarm\": snoozed, no longer ringing")
    await until(lambda: "alarm: volume back" in e.log())
    check("alarm: volume back" in e.log(), "snoozed: the volume back where it was")
    d = json.loads(e.page.call("GET", "/api/alarms")[2]); mine = [a for a in d["alarms"] if a["label"] == "Test"][0]
    check(mine["snoozed"] > 0 and mine["enabled"], f"the page: snoozed until {mine['snoozed']}")
    clock_before(mine["snoozed"])                         # the snooze's moment comes
    await until(lambda: evs()[-1:] == ["ringing"], 20)
    check(evs()[-1:] == ["ringing"], "it rings again a snooze later")

    # a volume key while it rings snoozes it (the Echo Show has no action button)
    e.keyev(KEY_VOLUMEUP, 1); await asyncio.sleep(0.1); e.keyev(KEY_VOLUMEUP, 0)
    await until(lambda: evs()[-1:] == ["snoozed"])
    check(evs()[-1:] == ["snoozed"], "a volume key snoozes it")
    clock_before(snoozed_at())
    await until(lambda: evs()[-1:] == ["ringing"], 20)
    check(await request("snooze"), "\"<wake word>, snooze\" while it rings: the Echo's")
    await until(lambda: evs()[-1:] == ["snoozed"] and announced()[-1:] == ["Snoozing for 1 minutes."])
    check(evs()[-1:] == ["snoozed"] and announced()[-1:] == ["Snoozing for 1 minutes."], f"snoozed by voice, and said ({announced()[-1:]})")
    clock_before(snoozed_at())
    await until(lambda: evs()[-1:] == ["ringing"], 20)
    check(await request("stop"), "\"stop\" while it rings")
    await until(lambda: evs()[-1:] == ["stopped"])
    d = json.loads(e.page.call("GET", "/api/alarms")[2]); mine = [a for a in d["alarms"] if a["label"] == "Test"][0]
    check(evs()[-1:] == ["stopped"] and ringing() is False and not mine["enabled"], "\"stop\": stopped, and the once alarm is off now")

    # the page's own edits, Home Assistant's delete
    st_, _, _ = e.page.call("POST", "/api/alarms/put", b"0 6 30 65 1 Weekend")
    d = json.loads(e.page.call("GET", "/api/alarms")[2])
    check(st_ == 200 and any(a["label"] == "Weekend" and a["days"] == 65 for a in d["alarms"]), "the page adds one")
    await until(lambda: "6:30 AM weekends (Weekend)" in (text(lst) or ""))
    check("6:30 AM weekends (Weekend)" in (text(lst) or ""), f"\"Alarms\" follows: {text(lst)!r}")
    check(await request("cancel the 6:30 alarm"), "\"cancel the 6:30 alarm\"")
    await until(lambda: announced()[-1:] == ["Your alarm for 6:30 AM on weekends is cancelled."])
    check(announced()[-1:] == ["Your alarm for 6:30 AM on weekends is cancelled."], f"said: {announced()[-1:]}")
    n0 = len(evs())
    await c.execute_service(svc["delete_alarm"], {"which": "all"})
    await until(lambda: evs()[n0:] == ["deleted"])
    await until(lambda: text(lst) == "")
    check(evs()[n0:] == ["deleted"] and text(lst) == "" and json.loads(e.page.call("GET", "/api/alarms")[2])["alarms"] == [],
          "delete_alarm \"all\": none left")
    check(await request("when is my alarm"), "\"when is my alarm\" with none")
    await until(lambda: announced()[-1:] == ["You have no alarms."])
    check(announced()[-1:] == ["You have no alarms."], "\"You have no alarms.\"")
    alog = os.path.join(e.state, "alarms")
    check(os.path.exists(alog), "state/alarms written")


run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), {"HASSMIC_FAKE_CLOCK_OFFSET": OFFSET, "TZ": "America/New_York"}, log=True)
