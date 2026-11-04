#!/usr/bin/env python3
"""Replies in the Echo's own voice (own_voice_replies) against build/hassmic-host with a stand-in voice
(HASSMIC_FAKE_SPEECH=1, speech_none.c: a tone of 20 ms per character at 16 kHz, so 640 bytes of playback per
character), Home Assistant's side of the ESPHome API (tests/esphome_common.py) and of the Echo's own Wyoming TTS server
(port 16710) through the `wyoming` package, as Home Assistant's wyoming integration (tts.py) speaks to it: streaming
synthesis, synthesize-start / -chunk / the whole text again as "synthesize" / -stop, read until synthesize-stopped.

  - a streamed reply whose TTS is this Echo: each sentence said as its text comes, Home Assistant gets 100 ms of
    silence, the reply's URL is never fetched, the run ends after the last sentence and not between two;
  - a reply Home Assistant does not stream: said whole at the TTS start as before, the synthesis that follows it made
    for Home Assistant (its file cache keeps it), said once;
  - a streamed reply whose TTS is elsewhere: nothing comes, the TTS start says it whole, learnt: the next streamed run
    claims nothing, and a synthesis in it is someone else's (real audio);
  - "<wake word>, stop" and a barge-in during a streamed reply: the rest not said, Home Assistant's stream still ends;
  - a plain client's streamed synthesis: real audio, one audio-start, every sentence."""
import asyncio, os
from aioesphomeapi import VoiceAssistantEventType as Ev
from wyoming.client import AsyncTcpClient
from wyoming.tts import Synthesize, SynthesizeStart, SynthesizeChunk, SynthesizeStop, SynthesizeStopped
from wyoming.audio import AudioStart, AudioChunk, AudioStop
import esphome_common
from esphome_common import check, until, sync, home_assistant, run, port_open

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17311, 17312, 17313, 17314, 17315
WYTTS = 16710
BPC = 640                                   # bytes of playback per character of the stand-in voice


async def tts_client():
    c = AsyncTcpClient("127.0.0.1", WYTTS); await c.connect(); return c


async def tts_answer(c, timeout=10):
    """what the server answers a stream, read as Home Assistant reads it: until synthesize-stopped"""
    got = {"starts": [], "audio": bytearray(), "stops": 0, "stopped": False}
    while True:
        ev = await asyncio.wait_for(c.read_event(), timeout)
        if ev is None: break
        if AudioStart.is_type(ev.type): got["starts"].append(AudioStart.from_event(ev).rate)
        elif AudioChunk.is_type(ev.type): got["audio"].extend(AudioChunk.from_event(ev).audio)
        elif AudioStop.is_type(ev.type): got["stops"] += 1
        elif SynthesizeStopped.is_type(ev.type): got["stopped"] = True; break
    return got


async def main(e):
    play, log = e.play, e.log
    check(await until(lambda: port_open(WYTTS)), "the Echo's own TTS server listens (stand-in voice)")
    e.page.set(own_voice_replies="on", error_speech="off")    # the test pipelines' errors unsaid: only replies play
    ha = await home_assistant(e)
    cli, va = ha.cli, ha.va
    url = lambda n: f"http://127.0.0.1:{HTTP_PORT}/{n}.wav"
    idle_count = lambda: log().count("-> idle")
    played = lambda: os.path.getsize(play)

    async def thinking(n):
        """the wake word, and Home Assistant's run up to its agent: the Echo THINKING"""
        va.started.clear(); e.wake()
        check(await until(va.started.is_set, 5), f"{n}: the wake word starts a run")
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, {"url": url(n)})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "tell me something"})
        await sync(cli)
        check(await until(lambda: log().count("-> thinking") > thinking.n), f"{n}: thinking")
        thinking.n = log().count("-> thinking")
    thinking.n = 0

    def end_run(n, text):
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_START, {"text": text})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": url(n)})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)

    # 1. a streamed reply, the pipeline's TTS this Echo: said sentence by sentence as the LLM writes it
    s1, s2, s3 = "Hello there.", "This is the second sentence.", "And the last one"
    full = f"{s1} {s2} {s3}"
    await thinking("own1")
    idle0, before = idle_count(), played()
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"}); await sync(cli)
    c = await tts_client()
    await c.write_event(SynthesizeStart().event())
    await c.write_event(SynthesizeChunk(text="Hello ").event())
    await c.write_event(SynthesizeChunk(text="there. ").event())
    check(await until(lambda: played() - before >= len(s1) * BPC, 3), "streamed: the first sentence plays before the rest of the text came")
    await asyncio.sleep(0.6)                          # its end played, nothing more queued: the run must go on
    check(idle_count() == idle0 and played() - before == len(s1) * BPC,
          f"streamed: the first sentence's end does not end the run ({played() - before} bytes)")
    await c.write_event(SynthesizeChunk(text=s2 + " And the ").event())
    await c.write_event(SynthesizeChunk(text="last one").event())
    await c.write_event(Synthesize(text=full).event())          # Home Assistant's whole text beside the stream
    await c.write_event(SynthesizeStop().event())
    got = await tts_answer(c); await c.disconnect()
    check(got["stopped"] and len(got["starts"]) == 1 and len(got["audio"]) == 3200 and not any(got["audio"]),
          f"streamed: Home Assistant gets 100 ms of silence and synthesize-stopped ({len(got['audio'])} bytes, starts {got['starts']})")
    end_run("own1", full)
    total = (len(s1) + len(s2) + len(s3)) * BPC
    check(await until(lambda: idle_count() > idle0, 8), "streamed: the run ends")
    check(played() - before == total, f"streamed: every sentence said once ({played() - before} of {total} bytes)")
    check(not [p for p in esphome_common.requested if "own1" in p], "streamed: the reply's URL never fetched")
    check(log().count("speech: \"" + s3) == 1 and log().count("speech: \"" + full) == 0, "streamed: the whole text not said again at the TTS start")

    # 1b. a streamed reply that asks back (continue_conversation, sent while it still plays): the next run starts only
    # after its last sentence
    q1, q2 = "I can do that.", "Which room do you mean?"
    await thinking("own1b")
    before = played()
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"}); await sync(cli)
    c = await tts_client()
    await c.write_event(SynthesizeStart().event()); await c.write_event(SynthesizeChunk(text=f"{q1} {q2}").event())
    await c.write_event(SynthesizeStop().event())
    got = await tts_answer(c); await c.disconnect()
    va.started.clear()
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
    end_run("own1b", f"{q1} {q2}")
    check(await until(va.started.is_set, 5), "follow-up: the next run starts")
    at_start = played() - before
    check(at_start == (len(q1) + len(q2)) * BPC, f"follow-up: ...after the whole reply ({at_start} of {(len(q1) + len(q2)) * BPC} bytes)")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"}); await sync(cli)
    await asyncio.sleep(0.3)

    # 2. a reply Home Assistant does not stream: said whole at the TTS start, the synthesis after it made for HA
    short = "Done, the light is on."
    await thinking("own2")
    idle0, before = idle_count(), played()
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_START, {"text": short}); await sync(cli)
    c = await tts_client()                         # HA's async_set_message after the event: its one-chunk stream
    await c.write_event(SynthesizeStart().event()); await c.write_event(SynthesizeChunk(text=short).event())
    await c.write_event(Synthesize(text=short).event()); await c.write_event(SynthesizeStop().event())
    got = await tts_answer(c); await c.disconnect()
    check(got["stopped"] and got["starts"] == [16000] and len(got["audio"]) == len(short) * BPC and any(got["audio"]),
          f"whole: Home Assistant's synthesis made as before (it caches it) ({len(got['audio'])} bytes)")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_TTS_END, {"url": url("own2")})
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    check(await until(lambda: idle_count() > idle0, 5), "whole: the run ends")
    check(played() - before == len(short) * BPC, f"whole: said once ({played() - before} bytes)")
    check(not [p for p in esphome_common.requested if "own2" in p], "whole: the reply's URL not fetched")

    # 3. a streamed reply whose TTS is elsewhere (Piper): nothing comes to the Echo; said whole at the TTS start
    other = "The weather today is sunny with a light breeze from the west."
    await thinking("own3")
    idle0, before = idle_count(), played()
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"}); await sync(cli)
    await asyncio.sleep(0.5)
    end_run("own3", other)
    check(await until(lambda: idle_count() > idle0, 5), "elsewhere: the run ends")
    check(played() - before == len(other) * BPC, f"elsewhere: said whole at the TTS start ({played() - before} bytes)")
    check(not [p for p in esphome_common.requested if "own3" in p], "elsewhere: the reply's URL not fetched")
    check("the pipeline's TTS is elsewhere" in log(), "elsewhere: learnt")
    # ...the next streamed run expects nothing: a synthesis meanwhile (tts.speak for another speaker) is not the reply
    await thinking("own4")
    idle0, before = idle_count(), played()
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"}); await sync(cli)
    check(await until(lambda: "said whole at its end (the pipeline's TTS is elsewhere)" in log()), "learnt: the next streamed run waits for nothing")
    c = await tts_client(); spk = "Dinner is ready."
    await c.write_event(SynthesizeStart().event()); await c.write_event(SynthesizeChunk(text=spk).event()); await c.write_event(SynthesizeStop().event())
    got = await tts_answer(c); await c.disconnect()
    check(len(got["audio"]) == len(spk) * BPC and any(got["audio"]), f"learnt: another caller's synthesis gets its audio ({len(got['audio'])} bytes)")
    end_run("own4", other)
    check(await until(lambda: idle_count() > idle0, 5), "learnt: the run ends")
    check(played() - before == len(other) * BPC, f"learnt: only the reply said here ({played() - before} bytes)")

    # 4. the pipeline's TTS switched back to this Echo: the 20th streamed run since it was learnt looks again (main.c
    # OWN_RECHECK) and its synthesis is claimed.  "<wake word>, stop" during that reply
    waits = lambda: log().count("waiting for its sentences on the Echo's own voice")
    w0 = waits()
    for k in range(18):                                # streamed runs 2..19 since: ended by an error at once
        await thinking(f"skip{k}")
        n = idle_count()
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"})
        cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"}); await sync(cli)
        await until(lambda: idle_count() > n, 2)
    check(waits() == w0, "learnt: 19 streamed runs claim nothing")
    await thinking("own5")
    idle0, before = idle_count(), played()
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"}); await sync(cli)
    check(await until(lambda: waits() == w0 + 1), "every 20th streamed run looks again")
    long1 = "This sentence is long enough to be cut while the voice is saying it to the room."
    c = await tts_client()
    await c.write_event(SynthesizeStart().event()); await c.write_event(SynthesizeChunk(text=long1 + " ").event())
    check(await until(lambda: played() - before > 4 * BPC, 3), "stop: the reply plays")
    os.kill(e.proc.pid, 1)                             # SIGHUP: "<wake word>, stop" (Pryon's second keyword)
    check(await until(lambda: idle_count() > idle0, 3), "stop: the run ends at once")
    cut = played() - before
    await c.write_event(SynthesizeChunk(text="More words that must not be said. ").event())
    await c.write_event(SynthesizeStop().event())
    got = await tts_answer(c); await c.disconnect()
    await asyncio.sleep(0.5)
    check(cut < len(long1) * BPC and played() - before == cut, f"stop: the rest not said ({cut} of {len(long1) * BPC} bytes)")
    check(got["stopped"] and not any(got["audio"]), "stop: Home Assistant's stream still ends (silence)")
    check("this Echo again" in log(), "the pipeline's TTS is this Echo again: learnt")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"}); await sync(cli)

    # 5. a barge-in during a streamed reply: the rest dropped, the wake word's new run listens
    await thinking("own6")
    before = played()
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"}); await sync(cli)
    c = await tts_client()
    await c.write_event(SynthesizeStart().event()); await c.write_event(SynthesizeChunk(text=long1 + " ").event())
    check(await until(lambda: played() - before > 4 * BPC, 3), "barge-in: the reply plays")
    va.started.clear(); e.wake()
    check(await until(va.started.is_set, 3), "barge-in: the wake word starts a new run")
    cut = played() - before
    await c.write_event(SynthesizeChunk(text="More words that must not be said. ").event())
    await c.write_event(SynthesizeStop().event())
    got = await tts_answer(c); await c.disconnect()
    await asyncio.sleep(0.5)
    check(played() - before == cut and cut < len(long1) * BPC, f"barge-in: the rest not said ({cut} bytes)")
    check(got["stopped"], "barge-in: Home Assistant's stream still ends")
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"}); await sync(cli)
    await asyncio.sleep(0.3)

    # 6. a plain client streaming (tts.speak to another speaker): real audio, one audio-start, every sentence
    before = played()
    a, b = "Good morning.", "It is seven o'clock"
    c = await tts_client()
    await c.write_event(SynthesizeStart().event())
    for piece in ("Good ", "morning. It is ", "seven o'clock"): await c.write_event(SynthesizeChunk(text=piece).event())
    await c.write_event(Synthesize(text=a + " " + b).event()); await c.write_event(SynthesizeStop().event())
    got = await tts_answer(c); await c.disconnect()
    check(got["stopped"] and got["starts"] == [16000] and got["stops"] == 1 and len(got["audio"]) == (len(a) + len(b)) * BPC,
          f"plain client: its audio, one audio-start ({len(got['audio'])} of {(len(a) + len(b)) * BPC} bytes, {got['starts']})")
    check(played() == before, "plain client: nothing played on the Echo")
    # and a whole text without a stream, as before streaming
    c = await tts_client(); await c.write_event(Synthesize(text=a).event())
    got = await tts_answer_plain(c); await c.disconnect()
    check(len(got) == len(a) * BPC, f"plain client: \"synthesize\" alone as before ({len(got)} bytes)")


async def tts_answer_plain(c):
    audio = bytearray()
    while True:
        ev = await asyncio.wait_for(c.read_event(), 10)
        if ev is None or AudioStop.is_type(ev.type): return audio
        if AudioChunk.is_type(ev.type): audio.extend(AudioChunk.from_event(ev).audio)


run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), {"HASSMIC_FAKE_SPEECH": "1"}, log=True)
