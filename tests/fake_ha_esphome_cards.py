#!/usr/bin/env python3
"""Cards on the Echo Show's screen (cards.c): what Home Assistant pushes with show_card (the Echo's format, v2:
docs/cards.md), as hassmic tells its screen UI (screen.c, the UI's card.c draws it), against build/hassmic-host-screen with
tests/fake_screen.py's stand-in UI and Home Assistant over the ESPHome API:
  - "Cards on screen" on the settings page, on by default; the actions show_card ("card"), clear_card ("id")
  - a notice: its lines, gone after its timeout; Home Assistant told what shows (esphome.hassmic_card_state); an
    announcement does not take it away; clear_card by id
  - a message's picture: "ci card<seq>.<n>"; the UI's "img" ask fetched from the card's URL and handed over, a failed
    fetch as none, an old card's ask as none
  - entities: rows as sent, a control only where the card asks (a lock's never), no states asked of Home Assistant;
    taps run the action (shown at once); Home Assistant's update in place; an update of a card gone refused
  - buttons: an event esphome.hassmic_card_action {card, action}, never an action; the card goes (keep: stays)
  - weather; clear_card without an id
  - a voice request's card over a notice (hidden while the request runs, back after the request's card), another
    conversation's refused, one long after the request refused
  - bad JSON, version 1, too long, no rows, an unknown type without words, switched off: refused, each told to Home
    Assistant (esphome.hassmic_card_refused)
"""
import asyncio, json, os, re, sys, tempfile, time
from aioesphomeapi import VoiceAssistantEventType as Ev
from esphome_common import check, until, sync, home_assistant, run, ROOT

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17301, 17302, 17303, 17304, 17305

ui_dir = tempfile.mkdtemp(); UI_LOG = os.path.join(ui_dir, "ui.log"); fifo = os.path.join(ui_dir, "ui.fifo")
os.mkfifo(fifo)
runner = os.path.join(ui_dir, "ui.sh")
open(runner, "w").write(f"#!/bin/sh\nexec {sys.executable} {ROOT}/tests/fake_screen.py --ui {ui_dir}\n"); os.chmod(runner, 0o755)


def lines():
    """the text lines the UI got, the band's rows and images cut out"""
    try: b = open(UI_LOG, "rb").read()
    except OSError: return []
    out, i = [], 0
    while i < len(b):
        j = b.find(b"\n", i)
        if j < 0: break
        l = b[i:j].decode("utf-8", "replace"); i = j + 1
        out.append(l)
        m = re.fullmatch(r"band (\d+)", l)
        if m: i += 4 * int(m.group(1))
        m = re.fullmatch(r"art (\d+)", l) or re.fullmatch(r"img \S+ (\d+) \d+", l)
        if m: i += int(m.group(1))
    return out


def card_lines(ls=None):
    """the last card the UI was told: its lines from its "cb" to "ce", or ["cx"]"""
    ls = [l for l in (ls if ls is not None else lines()) if l[:2] in ("cb", "cr", "cw", "cf", "cm", "ci", "cl", "cu", "ce", "cx") and l[2:3] in (" ", "")]
    for k in range(len(ls) - 1, -1, -1):
        if ls[k] == "cx": return ["cx"]
        if ls[k].startswith("cb "): return ls[k:]
    return []


def shown(): c = card_lines(); return bool(c) and c[0].startswith("cb ") and c[-1] == "ce"
def gone(): return card_lines() == ["cx"]
def seq(): return int(card_lines()[0].split()[1])
def row(i): return next((l for l in card_lines() if l.startswith(f"cr {i} ")), None)
def ui(cmd):
    """a line from the UI to hassmic (through the stand-in UI's FIFO, which it reopens after each writer: a write that
    catches it in between gets EPIPE, and goes again)"""
    for _ in range(20):
        try:
            with open(fifo, "wb") as q: q.write((cmd + "\n").encode())
            return
        except BrokenPipeError:
            time.sleep(0.05)
    raise RuntimeError(f"the stand-in UI took no line: {cmd}")


async def main(e):
    ha = await home_assistant(e); cli = ha.cli
    calls, subs, events = [], [], []
    cli.subscribe_home_assistant_states(lambda ent, attr: subs.append((ent, attr or "")))
    cli.subscribe_service_calls(lambda c: (events if c.is_event else calls).append(c))
    svcs = {s.name: s for s in (await cli.list_entities_services())[1]}
    check(e.pv("screen_cards") == 1, f"Cards on screen: on by default: {e.pv('screen_cards')}")
    check("show_card" in svcs and [(a.name, int(a.type)) for a in svcs["show_card"].args] == [("card", 3)]
          and [(a.name, int(a.type)) for a in svcs["clear_card"].args] == [("id", 3)],
          f"actions show_card (card), clear_card (id), each a string: {sorted(svcs)}")
    show = lambda c: cli.execute_service(svcs["show_card"], {"card": json.dumps(c) if not isinstance(c, str) else c})
    clear = lambda i="": cli.execute_service(svcs["clear_card"], {"id": i})

    # a notice: its lines, gone after its timeout
    await show({"v": 2, "id": "door", "type": "message", "title": "Doorbell", "message": "Someone is at the front door", "timeout": 2})
    check(await until(shown, 5), f"a notice: told: {card_lines()}")
    c = card_lines()
    check(re.fullmatch(r"cb \d+ m 0 0\tDoorbell\t", c[0]) and "cm Someone is at the front door" in c, f"its lines: {c}")
    check(await until(gone, 5), f"gone after its 2 s: {card_lines()}")
    st = lambda: [(x.data.get("card"), x.data.get("state")) for x in events if x.service == "esphome.hassmic_card_state"]
    check(await until(lambda: ("door", "shown") in st() and st()[-1] == ("", "gone"), 5),
          f"Home Assistant told what shows: esphome.hassmic_card_state shown, then gone: {st()}")

    # an announcement is no request: the notice stays through it
    await show({"v": 2, "id": "door", "type": "message", "title": "Doorbell", "message": "Ding dong", "timeout": 0})
    check(await until(shown, 5), f"a card until tapped: {card_lines()}")
    res = await cli.send_voice_assistant_announcement_await_response(f"http://127.0.0.1:{HTTP_PORT}/a.wav", 15, "Someone is at the door")
    check(res.success and shown() and "cm Ding dong" in card_lines(), f"an announcement played, the card still there: {card_lines()}")
    await clear("other"); await asyncio.sleep(0.5); check(shown(), "clear_card of another id: it stays")
    await clear("door"); check(await until(gone, 5), "clear_card of its id: gone")

    # a message's picture: fetched when the UI asks for it, by its key; a failed fetch: none
    await show({"v": 2, "id": "pic", "type": "message", "title": "Front door", "message": "Someone is at the door", "timeout": 0,
                "image": f"http://127.0.0.1:{HTTP_PORT}/snapshot.jpg"})
    check(await until(lambda: shown() and any(l.startswith("ci card") for l in card_lines()), 5), f"its picture line: {card_lines()}")
    s = seq(); n0 = len(lines()); ui(f"img card{s}.0 512")
    check(await until(lambda: any(re.fullmatch(rf"img card{s}\.0 (\d+) 512", l) and int(l.split()[2]) > 1000 for l in lines()[n0:]), 5),
          f"the UI's ask: the bytes fetched from the card's URL handed over: {[l for l in lines()[n0:] if l.startswith('img')]}")
    ui(f"img card{s - 1}.0 512")
    check(await until(lambda: f"img card{s - 1}.0 0 512" in lines(), 5), "an old card's ask: none")
    await show({"v": 2, "id": "pic", "type": "message", "title": "Front door", "timeout": 0, "image": f"http://127.0.0.1:{HTTP_PORT}/missing.jpg"})
    check(await until(lambda: shown() and f"ci card{s + 1}.0" in card_lines(), 5), f"a picture alone: {card_lines()}")
    ui(f"img card{s + 1}.0 512")
    check(await until(lambda: f"img card{s + 1}.0 0 512" in lines(), 20), "a fetch that failed: none (the UI keeps its last)")
    ui(f"card clear {s + 1}"); check(await until(gone, 5), "tapped away")

    # entities: rows as Home Assistant words them, a control only where asked (a lock's never)
    await show({"v": 2, "id": "kitchen", "type": "entities", "title": "Kitchen", "subtitle": "Turned on the light", "timeout": 0, "entities": [
        {"entity_id": "light.kitchen", "name": "Kitchen light", "value": "On · 80 %", "control": "dim", "on": True, "level": 80, "focus": True},
        {"entity_id": "switch.coffee", "name": "Coffee maker", "value": "Off", "control": "toggle", "on": False},
        {"entity_id": "sensor.kitchen_temp", "name": "Temperature", "value": "21.5 °C"},
        {"entity_id": "lock.back_door", "name": "Back door", "value": "Locked", "control": "toggle"},
        {"entity_id": "not an id", "name": "bad"}]})
    check(await until(shown, 5), f"entities: told: {card_lines()}")
    check(card_lines()[0].endswith("\tKitchen\tTurned on the light") and " e 4 0\t" in card_lines()[0], f"title, subtitle, 4 rows (the bad one left out): {card_lines()[0]}")
    check(row(0) == "cr 0 s 1 80 1\tKitchen light\tOn · 80 %", f"a dimmable light: switch and level, focus: {row(0)}")
    check(row(1) == "cr 1 t 0 -1 0\tCoffee maker\tOff", f"a switch: {row(1)}")
    check(row(2) == "cr 2 r -1 -1 0\tTemperature\t21.5 °C", f"a sensor, no control asked: read-only: {row(2)}")
    check(row(3) == "cr 3 r -1 -1 0\tBack door\tLocked", f"a lock, a toggle asked: read-only all the same: {row(3)}")
    await asyncio.sleep(1)
    check(not subs, f"no states asked of Home Assistant: its side sends the card again: {subs}")
    calls.clear(); s = seq()
    ui(f"card tap {s} 1 toggle")
    check(await until(lambda: any(x.service == "switch.toggle" and x.data.get("entity_id") == "switch.coffee" for x in calls), 5),
          f"a switch's tap: switch.toggle: {[(x.service, x.data) for x in calls]}")
    check(await until(lambda: row(1) == "cr 1 t 1 -1 0\tCoffee maker\tOff", 5), f"flipped at once (its words Home Assistant's): {row(1)}")
    ui(f"card tap {s} 0 level 40")
    check(await until(lambda: any(x.service == "light.turn_on" and x.data == {"entity_id": "light.kitchen", "brightness_pct": "40"} for x in calls), 5),
          f"a level: light.turn_on brightness_pct 40: {[(x.service, x.data) for x in calls]}")
    n = len(calls)
    ui(f"card tap {s} 3 toggle"); ui(f"card tap {s - 1} 1 toggle"); await asyncio.sleep(1)
    check(len(calls) == n, f"a lock's row and an old card's tap: nothing run: {[(x.service, x.data) for x in calls[n:]]}")
    # Home Assistant's update: in place, its words; a card not shown takes none
    await show({"v": 2, "id": "kitchen", "update": True, "type": "entities", "title": "Kitchen", "timeout": 0, "entities": [
        {"entity_id": "switch.coffee", "name": "Coffee maker", "value": "On", "control": "toggle", "on": True}]})
    check(await until(lambda: row(0) == "cr 0 t 1 -1 0\tCoffee maker\tOn" and seq() == s, 5), f"an update: in place, same card: {card_lines()}")
    ui(f"card clear {s}"); check(await until(gone, 5), "the UI's card clear: gone")
    await show({"v": 2, "id": "kitchen", "update": True, "type": "message", "title": "x"})
    check(await until(lambda: "card: refused: card kitchen: not shown" in e.log(), 5) and gone(), "an update of a card gone: refused")

    # buttons: an event, never an action; the card goes after it (keep: stays)
    await show({"v": 2, "id": "laundry", "type": "list", "title": "Shopping list", "timeout": 0, "items": ["Milk", {"text": "Eggs", "detail": "a dozen"}],
                "buttons": [{"label": "Later", "id": "SNOOZE"}, {"label": "Keep", "id": "K", "keep": True}]})
    check(await until(lambda: shown() and "cu 0\tLater" in card_lines(), 5) and "cl 1\tEggs\ta dozen" in card_lines(), f"a list with buttons: {card_lines()}")
    calls.clear(); events.clear()
    ui(f"card tap {seq()} 1 press")
    check(await until(lambda: any(x.service == "esphome.hassmic_card_action" and x.data == {"card": "laundry", "action": "K"} for x in events), 5),
          f"a press: the event esphome.hassmic_card_action (card, action): {[(x.service, x.data) for x in events]}")
    await asyncio.sleep(0.5); check(shown() and not calls, "keep: the card stays, no action run")
    ui(f"card tap {seq()} 0 press")
    check(await until(lambda: any(x.data.get("action") == "SNOOZE" for x in events), 5) and await until(gone, 5), "a button without keep: event, the card goes")

    # the weather
    await show({"v": 2, "type": "weather", "title": "Weather", "weather": {"condition": "sunny", "temperature": "66°", "detail": "High 68° · Low 55°",
               "forecast": [{"label": "Tonight", "condition": "clear-night", "temps": "55°", "precip": "10 %"},
                            {"label": "Thu", "condition": "rainy", "temps": "70° / 52°", "precip": "60 %"}]}})
    check(await until(shown, 5), f"the weather: told: {card_lines()}")
    c = card_lines()
    check("cw sunny\t66°\tHigh 68° · Low 55°" in c and "cf 1 rainy\tThu\t70° / 52°\t60 %" in c and " w 2 0\t" in c[0], f"now and two days: {c}")
    await clear(); check(await until(gone, 5), f"clear_card without an id: what shows goes: {card_lines()}")

    # a voice request's card over a notice; the notice hidden while the request runs and back after its card
    await show({"v": 2, "id": "washer", "type": "message", "title": "Washer is done", "timeout": 0})
    check(await until(shown, 5), "a notice until tapped away")
    notice = seq()
    va = ha.va
    va.started.clear(); e.wake(); await asyncio.wait_for(va.started.wait(), 5)
    check(await until(gone, 5), f"a request: the notice hidden: {card_lines()}")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the kitchen light"}); await sync(cli)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "c1", "continue_conversation": "0"}); await sync(cli)
    await show({"v": 2, "id": "r1", "scope": "request", "type": "entities", "title": "Kitchen", "conversation_id": "c1", "timeout": 3,
                "entities": [{"entity_id": "light.kitchen", "name": "Light", "value": "On", "control": "toggle", "on": True}]})
    check(await until(lambda: shown() and " e 1 1\tKitchen" in card_lines()[0], 5), f"the request's card: {card_lines()[:1]}")
    await show({"v": 2, "scope": "request", "type": "message", "title": "x", "conversation_id": "c0"})
    check(await until(lambda: "card: refused: for another request" in e.log(), 5), "a card for another conversation: refused")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_START, {"text": "Done"})
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(cli)
    check(await until(lambda: "state: speaking -> idle" in e.log() or "-> idle" in e.log(), 5) and " e 1 1\tKitchen" in (card_lines() or [""])[0],
          "idle: the request's card still there (its 3 s)")
    check(await until(lambda: shown() and seq() == notice, 8), f"after it: the notice back: {card_lines()[:1]}")
    await clear("washer"); check(await until(gone, 5), "cleared")
    await asyncio.sleep(11)                                    # (past the 10 s a request's card may come after its request)
    await show({"v": 2, "scope": "request", "type": "message", "title": "later"})
    check(await until(lambda: "card: refused: no request on this Echo" in e.log(), 5), "a request's card long after the request: refused")

    # refused: bad JSON, version 1, too long, no rows, switched off
    n = len(lines())
    for bad, why in (("{not json", "not a JSON object"), ({"v": 1, "type": "message", "message": "x"}, "card version 1"),
                     ({"v": 2, "type": "message", "message": "x" * 9000}, "longer than 8 KB"), ({"v": 2, "type": "entities", "entities": []}, "entities: none with an entity id"),
                     ({"v": 2, "type": "video"}, 'type "video" unknown')):
        await show(bad)
        check(await until(lambda: f"card: refused: {why}" in e.log(), 5)
              and await until(lambda: any(x.service == "esphome.hassmic_card_refused" and why in x.data.get("reason", "") for x in events), 5),
              f"refused, and Home Assistant told why: {why}")
    check(not any(l.startswith("cb ") for l in lines()[n:]), "none of them told to the UI")
    e.page.set(screen_cards="off")                              # (a feature: the link closes, Home Assistant lists again)
    await asyncio.sleep(1.5)
    cli2 = (await home_assistant(e)).cli
    svcs2 = {s.name for s in (await cli2.list_entities_services())[1]}
    check("show_card" not in svcs2 and "clear_card" not in svcs2, f"switched off: show_card and clear_card no longer listed: {sorted(svcs2)}")
    e.page.set(screen_cards="on")


run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT),
    {"HASSMIC_SCREEN": runner, "FAKE_UI_LOG": UI_LOG, "FAKE_UI_FIFO": fifo, "HASSMIC_SCREEN_READY_MS": "5000"},
    log=True, binary="hassmic-host-screen")
