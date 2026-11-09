#!/usr/bin/env python3
"""Home Assistant's side of the ESPHome native API against build/hassmic-host (tests/esphome_common.py): the end of a
request heard on the Echo (micend.c, issue #66).  The capture, looped: a talker's syllables for 1.5 s at -48 dBFS, the
dishes as loud for 1.5 s, then other voices 20 dB under for 4.5 s, on another pitch (the gain brings them up to speech
level while they are alone, so only the pitch tells them apart in what Home Assistant gets).  In the samples' lowest
bits the front end's records (afemeta.h) as the Echo's micAsr carries them: its VAD hears speech in the talker only.
7.5 s is no whole number of records: each loop moves their phase (a read that lost samples), ring_speech finds it
again.  This Home Assistant never ends a run by itself (no VAD): what ends one is the Echo."""
import asyncio, math, os, random, struct, tempfile
from aioesphomeapi import VoiceAssistantEventType as Ev
from esphome_common import check, until, sync, home_assistant, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17181, 17182, 17183, 17184, 17185
TALKER, DISHES, OTHERS = 1.5, 1.5, 4.5


def capture(path):
    rnd, out = random.Random(1), []
    def syllables(sec, db, hz):                         # a 4 Hz envelope on a tone (tests/unit/micend_test.c)
        a = 32768 * 10 ** (db / 20) * 2 / math.sqrt(0.75)
        for i in range(int(sec * 16000)):
            t = i / 16000; env = 0.5 + 0.5 * math.sin(2 * math.pi * 4 * t)
            out.append(max(-32768, min(32767, int(a * env * env * math.sin(2 * math.pi * hz * t) + rnd.uniform(-8, 8)))))
    def dishes(sec, db):                                # clatters of noise, 4 a second, 75 ms each
        a = 32768 * 10 ** (db / 20) * math.sqrt(3)
        for i in range(int(sec * 16000)):
            out.append(int(rnd.uniform(-a, a)) if i % 4000 < 1200 else int(rnd.uniform(-8, 8)))
    syllables(TALKER, -48, 220); dishes(DISHES, -45); syllables(OTHERS, -68, 600)
    speech = int(TALKER * 16000)
    for f in range(0, len(out) - 127, 128):             # the records (docs/re-vad.md, version 3.3): sync, version,
        bits = [0] * 128                                # magic, VAD at 113-114, checksum
        for at, n, v in ((0, 8, 0xa5), (8, 8, 0x63), (16, 7, 0x68), (113, 2, 3 if f + 128 <= speech else 0)):
            for k in range(n): bits[at + k] = v >> (n - 1 - k) & 1
        ones = sum(bits[:120]) & 0xff
        for k in range(8): bits[120 + k] = ones >> (7 - k) & 1
        for k in range(128): out[f + k] = out[f + k] & ~1 | bits[k]
    with open(path, "wb") as f: f.write(struct.pack(f"<{len(out)}h", *out))


def frames(b):                                          # each 50 ms of what Home Assistant got: dBFS at the talker's
    v = struct.unpack(f"<{len(b) // 2}h", b[:len(b) // 2 * 2])         # pitch, the others', two of neither (the dishes)
    def power(x, hz):                                   # Goertzel, as dBFS of a sine
        k = 2 * math.cos(2 * math.pi * hz / 16000); s1 = s2 = 0.0
        for y in x: s1, s2 = y + k * s1 - s2, s1
        return 10 * math.log10((s1 * s1 + s2 * s2 - k * s1 * s2) * 2 / len(x) ** 2 / 32768 ** 2 + 1e-12)
    return [tuple(power(v[i:i + 800], hz) for hz in (220, 600, 1500, 3000)) for i in range(0, len(v) - 799, 800)]

def talker(f): return f[0] > max(f[1:]) + 15             # a frame with the talker's pitch alone in it
def others(f): return f[1] > max(f[0], f[2], f[3]) + 15

def median(x): x = sorted(x); return x[len(x) // 2] if x else -200


async def main(e):
    page, conf, pv = e.page, e.conf, e.pv
    check(pv("end_of_request") == 0, "end of request heard on the Echo: off by default")
    ha = await home_assistant(e)
    c, va = ha.cli, ha.va
    # off: the others keep the run open as long as Home Assistant's VAD would (here: for ever)
    e.wake()
    await asyncio.wait_for(va.started.wait(), 5)
    await asyncio.sleep(TALKER + DISHES + OTHERS + 1)
    check(not va.stopped, f"off: the Echo leaves the end to Home Assistant: {va.stopped}")
    fr = frames(bytes(va.mic)); gap = median([f[0] for f in fr if talker(f)]) - median([f[1] for f in fr if others(f)])
    check(gap < 17, f"off: the gain brings the others up while they are alone, {gap:.1f} dB under the talker (20 in the room)")
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await sync(c); await asyncio.sleep(0.3)
    page.set(end_of_request="normal")
    check(conf().get("end_of_request") == "normal", f"switched to normal on the page, kept in the settings file: {conf()}")
    # normal: the run ends 0.7 s after the talker, the dishes in between (as loud, no speech to Amazon's VAD) or not, as
    # an end of audio (Home Assistant goes on), not an abort
    va.started.clear(); va.mic.clear(); va.stopped.clear()
    e.wake()
    await asyncio.wait_for(va.started.wait(), 5)
    await until(lambda: va.stopped, TALKER + DISHES + OTHERS + 4)
    check(va.stopped == [False], f"normal: the Echo ended the run itself, with the end of the audio (no abort): {va.stopped}")
    n = len(va.mic); await asyncio.sleep(1)
    check(len(va.mic) == n, f"no audio after that end: {len(va.mic) - n} bytes")
    fr = frames(bytes(va.mic)); last = max((i for i, f in enumerate(fr) if talker(f)), default=-1)
    tail = (len(fr) - 1 - last) * 0.05
    # 0.7 s from the talker's last frame within 9 dB of its level; its last syllable fades under that before it ends
    check(last >= 0 and 0.45 <= tail <= 1.0, f"what Home Assistant got ends {tail:.2f} s after the talker: the dishes did not keep it open")
    gap = median([f[0] for f in fr if talker(f)]) - median([f[1] for f in fr if others(f)])
    check(gap > 17, f"normal: nor does the gain bring them up: {gap:.1f} dB under the talker (20 in the room)")
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await sync(c); await asyncio.sleep(0.3)
    # the next request, right after one ended here: its own (the wake word's tail is looked for first, up to 0.5 s,
    # HASSMIC_FAKE_KEYWORD: the PC's wake word has a level, as Pryon's do), not ended at once by the last one's end
    va.started.clear(); va.mic.clear(); va.stopped.clear()
    e.wake()
    await asyncio.wait_for(va.started.wait(), 5)
    await until(lambda: va.stopped, TALKER + DISHES + OTHERS + 4)
    fr = frames(bytes(va.mic))
    check(va.stopped == [False] and any(talker(f) for f in fr) and len(va.mic) > 32000,
          f"the next request ends after its own talker: {len(va.mic)} bytes, stops {va.stopped}")
    c.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await sync(c)

if __name__ == "__main__":                              # capture() is tests/ha_real_end.py's too
    cap = tempfile.mktemp(suffix=".raw"); capture(cap)
    try: run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), {"HASSMIC_CAP": cap, "HASSMIC_FAKE_KEYWORD": "1"})
    finally: os.unlink(cap)
