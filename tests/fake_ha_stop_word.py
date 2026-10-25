#!/usr/bin/env python3
""""<wake word>, stop" (Pryon's second keyword; SIGHUP here) between two Echos of one arbitration network
(tests/arbecho.py).  Always: a run the wake word opened by cutting a THINKING run is cancelled (start=false alone), so
Home Assistant's late STT_START opens nothing, and a "stop" during the round that ended something (a ring, a THINKING
run) keeps the winner from opening one.  Setting stop_ends_listening (off by default): off, a "stop" out of
silence leaves its run to Home Assistant; on, it ends listening quietly, or keeps the round's winner from starting,
also when the Echo was listening already; a "stop" 4.5 s after the wake word is not that wake word's."""
import asyncio, os
from aioesphomeapi import VoiceAssistantEventType as Ev, VoiceAssistantTimerEventType as Tm
from arbecho import Echo, HA, check, until, start, run

key = os.urandom(32)
a, b = Echo("Echo Kitchen", 16971, 500, net=(2, key)), Echo("Echo Hall", 16972, 900, net=(2, key))
for e in (a, b): e.env["HASSMIC_ARB_RIVALS"] = "0"     # each hears alone here: the other is waited for all the same


async def main():
    ha = HA([a, b])
    await start((a, b), ha)
    check(await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 10), "both in one network")
    aborts = lambda e, since: [x for _, x in e.stops[since:]]

    async def in_round(*es):                        # "<wake word>, stop" with the "stop" during the round: sent once each
        heard = lambda e: e.text().count("arbitration: heard it,")      # Echo has its wake word's round going
        n = [heard(e) for e in es]
        for e in es: e.wake()
        check(await until(lambda: all(heard(e) > k for e, k in zip(es, n)), 1), "  the round started")
        for e in es: e.stop()
        await asyncio.sleep(1.5)

    async def woke(e):                              # a wake word heard by this Echo alone: its run listens
        n = len(e.starts); e.wake(); check(await until(lambda: len(e.starts) == n + 1, 3), f"{e.name}: the wake word starts a run")

    async def thinking_then_stop(e):
        """thinking, then "<wake word>, stop" once the new run started: the wake word cancels and listens anew, its
        "stop" drops that run.  Returns what the log said after the stop"""
        await woke(e)
        e.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None); await asyncio.sleep(0.5)
        await woke(e)
        s, n = len(e.stops), len(e.text()); e.stop(); await until(lambda: len(e.stops) > s, 2)
        e.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None); await asyncio.sleep(0.5)    # as HA, late
        return s, e.text()[n:]

    # Off (the default), as before
    sb = len(b.stops); await woke(b); b.stop(); await asyncio.sleep(0.5)
    check(len(b.stops) == sb and "stop: listening ended" not in b.text(), "off: \"<wake word>, stop\" out of silence is a command for Home Assistant")
    b.end_pipeline(); await asyncio.sleep(0.5)
    # the fix that does not hang on the setting: the run is cancelled, not ended (end of audio first); ended, the late
    # STT_START put the Echo back to listening with the mic closed, and Home Assistant carried out the "stop"
    sa, after = await thinking_then_stop(a)
    check("stop: pipeline dropped" in after and aborts(a, sa) == [True] and "-> listening" not in after,
          f"off: thinking, wake word, stop: the new run is cancelled and a late STT_START opens nothing (stops {aborts(a, sa)})")
    await woke(a); a.end_pipeline(); await asyncio.sleep(0.5)                   # idle: the next wake word starts a run
    # the same when the "stop" comes during the round (the other Echo did not hear it: the full window), before any run:
    # it ends the ring and nothing starts (it started and listened, with the "stop" in its replayed audio)
    b.cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t1", "tea", 60, 0, False)
    check(await until(lambda: "alarm: tea rings" in b.text(), 2), "off: a timer rings on the hall")
    await asyncio.sleep(1.5)                        # the kitchen's last claim no longer counts for the hall's round
    nb, sb = len(b.starts), len(b.stops); await in_round(b)
    check("alarm: off (stop)" in b.text() and len(b.starts) == nb and len(b.stops) == sb and "stop: came during the round, nothing started" in b.text(),
          f"off: ringing, \"<wake word>, stop\" during the round: the ring ends and nothing starts ({len(b.starts) - nb} started)")
    # and to a THINKING run: the winner cancels it and listens no more
    await woke(a); a.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None); await asyncio.sleep(0.5)
    na, sa = len(a.starts), len(a.stops); await in_round(a)
    check(len(a.starts) == na and aborts(a, sa) == [True], f"off: thinking, \"<wake word>, stop\" during the round: cancelled, none listens anew (stops {aborts(a, sa)})")

    for e in (a, b): e.page.set(stop_ends_listening="on")
    await asyncio.sleep(1.5)                        # the kitchen's last claim no longer counts for the hall's next round

    # out of silence, the run started: it ends at once, quietly (start=false alone)
    sb = len(b.stops); await woke(b); b.stop(); await until(lambda: len(b.stops) > sb, 2); await asyncio.sleep(0.3)
    check(aborts(b, sb) == [True] and "stop: listening ended" in b.text(), f"on: \"<wake word>, stop\" out of silence: the run is cancelled (stops {aborts(b, sb)})")
    # "stop" during the round (the other Echo did not hear it: the full window): nothing starts
    came = lambda e: e.text().count("stop: came during the round, nothing started")
    await asyncio.sleep(0.5); nb, sb, cb = len(b.starts), len(b.stops), came(b)
    await in_round(b)
    check(len(b.starts) == nb and len(b.stops) == sb and came(b) == cb + 1,
          "on: \"stop\" during the round: nothing starts")
    # both heard "<wake word>, stop": no run left anywhere
    # (the other's claim can settle the round at once: then the winner's run starts and its "stop" ends it)
    ended = lambda: sum(e.text().count(t) for e in (a, b) for t in ("stop: came during the round, nothing started", "stop: listening ended"))
    na, nb, sa, sb, ea = len(a.starts), len(b.starts), len(a.stops), len(b.stops), ended()
    await in_round(a, b)
    check(len(a.starts) - na == len(a.stops) - sa and len(b.starts) - nb == len(b.stops) - sb and ended() > ea,
          f"on: both heard \"<wake word>, stop\": no run left ({len(a.starts) - na}/{len(b.starts) - nb} started)")
    # listening already (a wrong wake), then "<wake word>, stop": that run ends, none listens anew
    await woke(a); await asyncio.sleep(2.8); na, sa = len(a.starts), len(a.stops)
    await in_round(a)
    check(len(a.starts) == na and aborts(a, sa) == [True], f"on: listening, then \"<wake word>, stop\": the run ends, none listens anew (stops {aborts(a, sa)})")
    # thinking, then "<wake word>, stop" during the round: cancelled, and nothing listens anew
    await woke(a); a.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None); await asyncio.sleep(0.5)
    na, sa = len(a.starts), len(a.stops); await in_round(a)
    check(len(a.starts) == na and aborts(a, sa) == [True], f"on: thinking, then \"<wake word>, stop\": cancelled, none listens anew (stops {aborts(a, sa)})")
    # and with the new run started already: the same as off
    sa, after = await thinking_then_stop(a)
    check("stop: pipeline dropped" in after and aborts(a, sa) == [True] and "-> listening" not in after,
          "on: thinking, wake word, stop: the new run is cancelled and a late STT_START opens nothing")
    # "stop" long after the wake word (the engine knows it only right behind one): the run goes on
    await asyncio.sleep(0.5); sb, ended = len(b.stops), b.text().count("stop: listening ended"); await woke(b); await asyncio.sleep(4.5)
    b.stop(); await asyncio.sleep(0.5)
    check(len(b.stops) == sb and b.text().count("stop: listening ended") == ended, "on: \"stop\" 4.5 s after the wake word: the run goes on")
    b.end_pipeline(); await asyncio.sleep(0.5)


run(main, (a, b))
