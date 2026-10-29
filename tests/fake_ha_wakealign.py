#!/usr/bin/env python3
"""Home Assistant's side of the ESPHome native API against build/hassmic-host (tests/esphome_common.py): the wake word's
end by the aligner (main.c align_resolve, align_thread, word_tail_end; issue #79) with the models of wakealign/.  The
file for the language spoken (the page's, else the time zone's, else the word's own; en-CA as en-US), the request after
the 800 ms of silence either way; an aligner too slow (HASSMIC_FAKE_ALIGN_SLOW) is waited for 1 s past its window, then
the level decides, and a wake word while it still runs goes by the level at once; the speaker adaptation in use and
saved from the 5th request.  HASSMIC_FAKE_KEYWORD: the PC's wake word has a level, so its end is looked for.

The capture: "Alexa" in a German voice (testdata/alexa_omnivoice.raw: OmniVoice's designed voice "female, young adult,
moderate pitch", tools/speechcorpus de-f-young, its "Alexa." that canary-1b-v2 heard as such, from 50 ms before MFA's
start to MFA's end) once every 2.5 s.  The aligner takes the first "Alexa" in its
window, so each wake word (SIGUSR1: ended 128 ms before the capture thread sees it) is sent for 200 ms before one ends,
by the capture's clock: the log's "request from capture sample" at its boot+ time (CLOCK_BOOTTIME), 16 samples a ms.
The German aligner puts its end 70 ms before MFA's for every Pryon's end from 400 ms before the word's to 200 ms after
it (the English ones, in a German voice, find none near), so the end and the adaptation are checked in German."""
import asyncio, glob, os, re, tempfile, time
import numpy as np
from aioesphomeapi import VoiceAssistantEventType as Ev
from esphome_common import ROOT, LEAD_IN, check, until, sync, home_assistant, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17191, 17192, 17193, 17194, 17195
SLOW = tempfile.mktemp(suffix=".ms")
PERIOD = 40000                                  # samples: the loop
WORD = np.fromfile(os.path.join(ROOT, "testdata", "alexa_omnivoice.raw"), "<i2"); WORD_END = len(WORD)


def capture(path): np.concatenate([WORD, np.zeros(PERIOD - WORD_END, "<i2")]).tofile(path)


def wake_after_word(e):
    """SIGUSR1 timed for 200 ms before an "Alexa" ends by the last request's capture sample and time; at once without one"""
    m = re.findall(r"boot\+(\d+\.\d+) whisper: request from capture sample (\d+)", e.log())
    if m:
        b, smp = float(m[-1][0]), int(m[-1][1]); ring = smp + (time.clock_gettime(time.CLOCK_BOOTTIME) - b) * 16000
        wait = ((WORD_END - 3200 + 2048 - ring) % PERIOD) / 16000
        time.sleep(wait if wait > 0.3 else wait + PERIOD / 16000)
    e.wake()


async def request(e, ha, want):
    """one wake word, until the log has want (a regex) and Home Assistant 0.25 s of the request after the silence;
    (the log's new lines, seconds from the start of the run until that audio) or None"""
    va = ha.va; mark = len(e.log())
    va.started.clear(); va.mic.clear(); va.stopped.clear()
    wake_after_word(e)
    await asyncio.wait_for(va.started.wait(), 5)
    t0 = asyncio.get_running_loop().time()
    ok = await until(lambda: re.search(want, e.log()[mark:]) and len(va.mic) >= LEAD_IN + 8000, 6)
    secs = asyncio.get_running_loop().time() - t0
    ha.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await sync(ha.cli); await asyncio.sleep(0.3)
    return (e.log()[mark:], secs) if ok else (e.log()[mark:], None)


def zone(e, z): open(os.path.join(e.state, "timezone"), "w").write(z + "\n")      # as clock.c leaves it for root


async def main(e):
    ha = await home_assistant(e)
    await request(e, ha, r"by the level|the word ends")         # where the capture is: the next ones timed by it
    check(re.search(r"aligned by \S*/wakealign/alexa-en-US\.wa \(\? spoken\)", e.log()),
          "no language, no time zone: the firmware's alexa by alexa-en-US.wa")
    for z, lang, want in (("Europe/London", "en", r"alexa-en-GB\.wa \(en-GB spoken\)"),
                          ("Europe/London", "de", r"alexa-de-DE\.wa \(de-DE spoken\)")):
        zone(e, z); e.page.set(speech_language=lang)
        lines, secs = await request(e, ha, want)
        check(secs is not None, f"time zone {z}, language {lang}: {want.replace(chr(92), '')}")
    lines, secs = await request(e, ha, r"the word ends|by the level")
    check(re.search(r"the word ends [-+]\d+ ms from Pryon's end \(aligned in \d+ ms\)", lines) and secs is not None,
          f"the request from the word's end as aligned, after the silence: {re.findall(r'wake: (?:the word|the aligner).*', lines)}")
    # too slow: the level after 1 s past the window (0.5 s after the end), the request still comes
    open(SLOW, "w").write("3000\n")
    lines, secs = await request(e, ha, r"by the level instead")
    check(secs is not None and "the aligner took over 1 s" in lines and "the word ends" not in lines and secs < 2.5,
          f"an aligner too slow: the level decides 1 s past its window, the request after {secs if secs is None else round(secs, 2)} s")
    # and while that run still goes on (3 s): the next wake word by the level at once
    lines, secs = await request(e, ha, r"by the level instead")
    check(secs is not None and "one was busy" in lines and secs < 1.5,
          f"a wake word while the aligner still runs: by the level at once ({secs if secs is None else round(secs, 2)} s)")
    open(SLOW, "w").write("0\n"); await asyncio.sleep(3)
    # en-CA has en-US's files (the same models)
    zone(e, "Canada/Eastern"); e.page.set(speech_language="auto")
    lines, secs = await request(e, ha, r"aligned by \S*/alexa-en-US\.wa \(en-CA spoken\)")
    check(secs is not None, "time zone Canada/Eastern, no language: alexa-en-US.wa (en-CA spoken)")
    # the speaker adaptation: one statistics file for German (every German word's), from each request with an end near
    # Pryon's (the one above among them); in use and saved from the 5th
    zone(e, "Europe/Berlin")
    for _ in range(8):
        if "speaker adaptation in use" in e.log(): break
        await request(e, ha, r"the word ends|by the level")
    use = re.search(r"speaker adaptation in use \((\d+) requests\)", e.log()); saved = glob.glob(os.path.join(e.state, "wakealign-*.adapt"))
    check(use and int(use[1]) >= 5 and len(saved) == 1 and os.path.getsize(saved[0]) > 500000,
          f"speaker adaptation in use from the 5th request it learnt from ({use and use[0]}), saved: {[os.path.basename(f) for f in saved]}")


if __name__ == "__main__":
    cap = tempfile.mktemp(suffix=".raw"); capture(cap)
    try: run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), {"HASSMIC_CAP": cap, "HASSMIC_WAKEALIGN": os.path.join(ROOT, "wakealign"),
                                                                   "HASSMIC_FAKE_KEYWORD": "1", "HASSMIC_FAKE_ALIGN_SLOW": SLOW}, log=True)
    finally:
        for f in (cap, SLOW):
            if os.path.exists(f): os.unlink(f)
