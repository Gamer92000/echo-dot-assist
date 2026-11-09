#!/usr/bin/env python3
"""Words and timers on the Echo Show's screen (issue #104): what hassmic tells its screen UI (screen.c; the UI's
words.c draws it) against build/hassmic-host-screen with tests/fake_screen.py's stand-in UI and Home Assistant over the
ESPHome API:
  - "Speech on screen", "Speech stays for", "Timers on screen" on the settings page, off by default (8 s), told to the
    UI as "wo"
  - off: nothing of what was said reaches the UI
  - on: the transcript ("wh") and the reply ("wr") as Home Assistant sends them, "ws" when the Echo is idle again (how
    long ago), a new request clears them; switched off, cleared
  - "<wake word>, stop" to a ringing timer is never shown, and takes what showed away
  - running timers as "tl" from Home Assistant's started and updated events (left, length, paused, the name or the
    length for one without), cancelled and finished ones gone; ringing ones as the face has them ("timers")
  - the screen held on (a day's timeout) while a timer runs or rings, not for a paused one, back to 5 min after
  - the UI's "words clear" (tapped away) clears them in hassmic; its "timer stop" stops a ring; its "timers hide" lets
    the screen sleep until the timers change
"""
import asyncio, os, re, sys, tempfile
from aioesphomeapi import VoiceAssistantEventType as Ev, VoiceAssistantTimerEventType as Tm
from esphome_common import check, until, sync, home_assistant, run, ROOT

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17201, 17202, 17203, 17204, 17205

ui_dir = tempfile.mkdtemp(); UI_LOG = os.path.join(ui_dir, "ui.log"); fifo = os.path.join(ui_dir, "ui.fifo")
os.mkfifo(fifo)
runner = os.path.join(ui_dir, "ui.sh")
open(runner, "w").write(f"#!/bin/sh\nexec {sys.executable} {ROOT}/tests/fake_screen.py --ui {ui_dir}\n"); os.chmod(runner, 0o755)


def lines():
    """the text lines the UI got, the band's rows cut out (band <w> + 4 w bytes)"""
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
        m = re.fullmatch(r"art (\d+)", l)
        if m: i += int(m.group(1))
    return out


def since(n): return lines()[n:]
def sleeps(e): return ([int(x) for x in re.findall(r"band: the screen sleeps after (\d+) s", e.log())] or [None])[-1]
def last(prefix, ls=None): return ([l for l in (ls if ls is not None else lines()) if l.startswith(prefix)] or [None])[-1]


async def request(e, ha, heard, reply):
    """a wake word, then Home Assistant's run: the transcript, the reply, its end"""
    cli, va = ha.cli, ha.va
    va.started.clear(); e.wake()
    await asyncio.wait_for(va.started.wait(), 5)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": heard})
    if reply is not None:
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "0"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_START, {"text": reply})
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await sync(cli)


async def main(e):
    ha = await home_assistant(e); cli = ha.cli
    check(await until(lambda: last("wo ") is not None, 8), f"the UI is told the settings: {last('wo ')}")
    check(last("wo ") == "wo 0 0 8" and e.pv("screen_text") == 0 and e.pv("screen_timers") == 0 and e.pv("screen_text_seconds") == 8,
          f"off by default, 8 s: {last('wo ')}, page {e.pv('screen_text')} {e.pv('screen_timers')} {e.pv('screen_text_seconds')}")

    # off: what was said never reaches the UI
    await request(e, ha, "my secret words", "my secret reply")
    await asyncio.sleep(1)
    check(not any("secret" in l for l in lines()), "off: neither the transcript nor the reply goes to the UI")

    e.page.set(screen_text="on", screen_timers="on", screen_text_seconds=5)
    check(await until(lambda: last("wo ") == "wo 1 1 5", 5), f"switched on from the page: {last('wo ')}")
    n = len(lines())
    await request(e, ha, "turn on the light", "Done, the light is on")
    ok = await until(lambda: last("ws ", since(n)) not in (None, "ws -1"), 5)
    got = [l for l in since(n) if l[:3] in ("wh ", "wr ", "ws ")]
    check(ok and "wh turn on the light" in got and "wr Done, the light is on" in got,
          f"the transcript and the reply as they come, then idle: {got}")
    i_h, i_r = got.index("wh turn on the light"), got.index("wr Done, the light is on")
    check(i_h < i_r and int(got[-1].split()[1]) >= 0, f"in order, idle last: {got[-3:]}")
    # a new request takes them away at once
    n = len(lines())
    ha.va.started.clear(); e.wake(); await asyncio.wait_for(ha.va.started.wait(), 5)
    check(await until(lambda: "wh " in since(n) and "wr " in since(n), 5), f"a new request clears them: {since(n)[-6:]}")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None); await sync(cli)

    # a ringing timer: as the face has it; "<wake word>, stop" ends it and is never shown
    n = len(lines())
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t1", "tea", 60, 0, False)
    check(await until(lambda: last("timers ") == "timers tea", 5), f"ringing: {last('timers ')}")
    await request(e, ha, "stop", None)
    check(await until(lambda: last("timers ") == "timers ", 5), f"stopped: {last('timers ')}")
    await asyncio.sleep(0.5)
    check(not any(l.startswith("wh stop") or l.startswith("wh Stop") for l in since(n)), "the \"stop\" that ended it is not shown")

    # running timers: what is left, the length, paused or not; the name, or the length for one without
    n = len(lines())
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_STARTED, "t2", "pizza", 600, 600, True)
    check(await until(lambda: (last("tl ") or "").endswith(" 600 1 pizza"), 5), f"a timer started: {last('tl ')}")
    m = re.fullmatch(r"tl 1 \d+\t(\d+) 600 1 pizza", last("tl ") or "")
    check(m and 595000 <= int(m.group(1)) <= 600000, f"its time left in ms: {last('tl ')}")
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_STARTED, "t3", "", 300, 300, True)
    check(await until(lambda: (last("tl ") or "").startswith("tl 2 ") and (last("tl ") or "").endswith(" 300 1 5 min"), 5),
          f"one without a name: its length: {last('tl ')}")
    check(await until(lambda: sleeps(e) == 86400, 5), f"timers run: the screen held on: sleeps after {sleeps(e)} s")
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_UPDATED, "t2", "pizza", 600, 420, False)
    check(await until(lambda: "\t420000 600 0 pizza" in (last("tl ") or ""), 5), f"paused: what is left, as told: {last('tl ')}")
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_CANCELLED, "t3", "", 300, 300, False)
    check(await until(lambda: sleeps(e) == 300, 5), f"a paused one alone: the screen not held on: sleeps after {sleeps(e)} s")
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_STARTED, "t3", "", 300, 300, True)
    check(await until(lambda: sleeps(e) == 86400, 5), f"one running again: held on: sleeps after {sleeps(e)} s")
    seq = last("tl ").split("\t")[0].split()[2]                  # the list the UI has
    with open(fifo, "wb") as q: q.write(f"timers hide {int(seq) - 1}\n".encode())   # (an older one: nothing hidden)
    await asyncio.sleep(1.5)
    check(sleeps(e) == 86400, f"tapped away on an older list: still held on: sleeps after {sleeps(e)} s")
    with open(fifo, "wb") as q: q.write(f"timers hide {seq}\n".encode())
    check(await until(lambda: sleeps(e) == 300, 5), f"tapped away on the screen: not held on: sleeps after {sleeps(e)} s")
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_UPDATED, "t3", "", 300, 250, True)
    check(await until(lambda: sleeps(e) == 86400, 5), f"a change shows them again: held on: sleeps after {sleeps(e)} s")
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_CANCELLED, "t2", "pizza", 600, 420, False)
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t3", "", 300, 0, False)
    check(await until(lambda: (last("tl ") or "").startswith("tl 0 "), 5), f"cancelled and finished: gone: {last('tl ')}")
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_CANCELLED, "t3", "", 300, 0, False)
    check(await until(lambda: last("timers ") == "timers ", 5), f"its ring gone with it: {last('timers ')}")
    check(await until(lambda: sleeps(e) == 300, 5), f"none left: stock's 5 min again: sleeps after {sleeps(e)} s")
    tls = [l for l in since(n) if l.startswith("tl ")]
    check(len(tls) <= 8, f"told on a change only, not every second: {len(tls)} lines")

    # tapped away on the screen: gone in hassmic too
    await request(e, ha, "what time is it", "It is noon")
    check(await until(lambda: last("wr ") == "wr It is noon", 5), f"shown: {last('wr ')}")
    with open(fifo, "wb") as q: q.write(b"words clear\n")
    check(await until(lambda: last("wh ") == "wh " and last("wr ") == "wr ", 5), f"the UI's words clear: {last('wh ')}, {last('wr ')}")
    # a ring stopped by the UI's Stop
    cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t4", "eggs", 300, 0, False)
    check(await until(lambda: last("timers ") == "timers eggs", 5), f"ringing: {last('timers ')}")
    with open(fifo, "wb") as q: q.write(b"timer stop\n")
    check(await until(lambda: last("timers ") == "timers ", 5), f"the UI's Stop stops it: {last('timers ')}")

    # off again: what showed goes
    await request(e, ha, "what time is it", "It is noon")
    check(await until(lambda: last("wr ") == "wr It is noon", 5), f"shown: {last('wr ')}")
    e.page.set(screen_text="off")
    check(await until(lambda: last("wo ") == "wo 0 1 5" and last("wh ") == "wh " and last("wr ") == "wr ", 5),
          f"switched off: told, the words cleared: {last('wo ')}, {last('wh ')}, {last('wr ')}")


run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT),
    {"HASSMIC_SCREEN": runner, "FAKE_UI_LOG": UI_LOG, "FAKE_UI_FIFO": fifo, "HASSMIC_SCREEN_READY_MS": "5000"},
    log=True, binary="hassmic-host-screen")
