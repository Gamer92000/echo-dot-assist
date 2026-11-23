#!/usr/bin/env python3
"""Phase 6 of docs/plugins.md on the PC: a copy of each request for a plugin (`request.audio`, `request.text`),
with a stand-in listener (tests/plugins/testreq.c: it says in its log what it got of each request, a CRC-32 of the
audio among it) under build/plugrun-host and build/hassmic-host, this script as the settings page and as Home
Assistant (ESPHome, then Wyoming).

The install: request.audio's and request.text's lines as docs/plugins.md 3.3 words them; with a way out (net.server)
the second press, on its own screen: one press installs nothing, the screen's words, a press without the screen's ask
installs nothing either, the ask and a second press install it.
The copy, ESPHome: byte for byte what Home Assistant got, the lead-in's zeros first, for the wake word (its slot and
phrase), the action button and a follow-up, with noise reduction off and on; nothing before REQ_START, nothing after
REQ_END, nothing outside a run, nothing without a link to Home Assistant; the reasons the copy ends (Home Assistant's
end, an error, cancelled by the button, the soft mute, the link lost, the pipeline's timeout); muted, no microphone
audio leaves the Echo: a follow-up asked for before the mute (continue_conversation), an announcement's
start_conversation and a barge-in queued before it start no run, for Home Assistant or the plugin; a plugin that does
not read loses blocks (counted, the gap in the offsets) and Home Assistant loses none; one that begins reading 3 s
after REQ_START loses nothing of a request's start (the lead-in, a long replay: wake_word_in_request, a slow aligner);
the texts (what was heard, the reply), a "stop" the Echo acted on itself not passed on; request.text alone: texts, no
audio socket, no start; a plugin that sends the request's messages itself: nothing begins, it runs on; its lines in
the log private, and hassmic's lines about it.
3.3's partner: a wake word engine from a file (not official) refused while the listener is on; chosen with the
listener off, the listener then not started, its update not installed, and started again once the engine is
another; after a restart with both, the listener not started.  An update that adds a way out: both presses.
-w remote: nothing before Home Assistant's wake word, then a copy that is a stretch of what Home Assistant got; muted,
the stream to Home Assistant stops, its wake word starts nothing, and it streams again once unmuted.
Wyoming: the copy of a run, its transcript as REQ_TEXT; with -w remote the copy from Home Assistant's detection on,
and a transcript of a run no detection opened is no request's text.
A plugin with every other capability (state, the network's, music, volume, Home Assistant's, calls, storage, the log):
no REQ_* at all.  Two Echos of one arbitration network: only the round's winner's plugin gets a copy."""
import asyncio, base64, json, os, re, resource, shutil, signal, subprocess, sys, tempfile, threading, time, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser
import fake_plugins as fp
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk"))
import plugcheck
check = fp.check
PORT, WEB, WYO = fp.PORT, fp.WEB, 16973
HTTP, PORT2, WEB2, ARB = 16974, 16975, 16976, 16977       # Home Assistant's media; the second Echo, its page; arbitration
LEAD_IN = 800 * 32
LINE_AUDIO = "Hears everything said to this Echo after the wake word or the button"
LINE_TEXT = "Reads what was said to this Echo and what it answered"
END = re.compile(r"run (\d+) end why (\d+) samples (\d+) dropped (\d+): got (\d+) bytes in (\d+) blocks, crc ([0-9a-f]{8}), first offset (\d+), gaps (\d+), outside (\d+)")
START = re.compile(r"run (\d+) start source (\d+) slot (\d+) phrase (.*)")


async def until(cond, secs=8):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if cond(): return True
        await asyncio.sleep(0.05)
    return bool(cond())


def crc(b): return f"{zlib.crc32(bytes(b)) & 0xffffffff:08x}"


async def main():
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log")
    for k in ("author", "index"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    man = json.load(open(f"{ROOT}/tests/plugins/testreq.json"))
    wake_man = json.load(open(f"{ROOT}/tests/plugins/testwake.json"))
    fake_src = os.path.join(state, "fake.src")
    # (community plugins on, as the Plugins page's switch leaves them: this test installs plugins from a file; the switch itself: fake_plugins_community.py)
    open(os.path.join(state, "config"), "w").write("even_volume=off\nplugins_community=on\n")

    def package(base=man, program="testreq", listed=True, **changes):
        m = dict(base, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, "p.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/{program}", os.path.join(top, "author.key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        if not listed: return open(out, "rb").read()
        plugcheck.list_package(out, os.path.join(top, "index.key"), os.path.join(top, "o.hmp"), arch="x86_64")
        return open(os.path.join(top, "o.hmp"), "rb").read()

    # the aligner (wakealign/) with a level for SIGUSR1's wake word, so that its end is looked for, and a slow one
    # (HASSMIC_FAKE_ALIGN_SLOW: its ms) where a long replay is wanted: waited for 1 s past its window
    slow = os.path.join(top, "align.ms"); open(slow, "w").write("0\n")
    env = dict(HASSMIC_PLUGINS_PUB=os.path.join(top, "index.pub"), HASSMIC_FAKE_SOURCE=fake_src, HASSMIC_PLUGIN_LINES="1000",
               HASSMIC_WAKEALIGN=os.path.join(ROOT, "wakealign"), HASSMIC_FAKE_KEYWORD="1", HASSMIC_FAKE_ALIGN_SLOW=slow)
    # Home Assistant's media: a short announcement, and a reply that stalls after its first half second until released
    release = threading.Event(); stalled = threading.Event()
    sys.path.insert(0, os.path.join(ROOT, "tests")); from esphome_common import wav_bytes
    class Media(BaseHTTPRequestHandler):
        def do_GET(self):
            body = wav_bytes(48000, 1.0 if "stall" in self.path else 0.5)
            self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.end_headers()
            try:
                if "stall" in self.path:
                    self.wfile.write(body[:44 + 48000]); self.wfile.flush(); stalled.set(); release.wait(15); body = body[44 + 48000:]
                self.wfile.write(body)
            except OSError: pass
        def log_message(self, *a): pass
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP), Media); threading.Thread(target=httpd.serve_forever, daemon=True).start()
    procs = []
    def launch(*extra):
        p = fp.start(state, log, extra=list(extra), env=env); procs.append(p); return p
    def stop(p):
        p.send_signal(signal.SIGTERM)
        try: p.wait(10)
        except subprocess.TimeoutExpired: p.kill(); p.wait()
    text = lambda lg=None: open(lg or log, errors="replace").read()
    plog = lambda pid="testreq", lg=None: [l.split(f"plugin {pid}: ", 1)[1] for l in text(lg).splitlines() if f"plugin {pid}: " in l]
    async def says(t, n0=0, secs=8, pid="testreq", lg=None): return await until(lambda: any(t in l for l in plog(pid, lg)[n0:]), secs)
    def ends(n0=0, pid="testreq", lg=None): return [m for m in (END.search(l) for l in plog(pid, lg)[n0:]) if m]
    def starts(n0=0, pid="testreq", lg=None): return [m for m in (START.search(l) for l in plog(pid, lg)[n0:]) if m]
    def texts(n0=0, pid="testreq", lg=None): return [l for l in plog(pid, lg)[n0:] if " text " in l]

    proc = launch(); ha = None
    try:
        wait_port(PORT, WEB)
        b = Browser(WEB); check(b.login_with_button(proc), "logged in with the button")
        b.set(plugins="on")

        # ---- the install: the lines in the owner's words, one press without a way out
        st, r = fp.upload(b, package())
        lines = {c["cap"]: c["line"] for c in r.get("caps", [])}
        check(st == 200 and r.get("button") is True and r.get("second") == "", f"a plugin that hears requests, no way out: the button, no second press: {st} {r.get('error')} {r.get('second')!r}")
        check(lines.get("request.audio") == LINE_AUDIO and lines.get("request.text") == LINE_TEXT, f"request.audio's and request.text's lines as 3.3 words them: {lines}")
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: (fp.plug(b, "testreq") or {}).get("accepted")), "one press installs it")
        check(await says("ready, capabilities 0000c000", 0, 15), f"it runs with request.audio and request.text: {plog()[-3:]}")
        pl = fp.plugins(b)
        check(pl.get("hears") == ["testreq"] and (fp.plug(b, "testreq") or {}).get("requests", {}).get("runs") == 0,
              f"the page's top names it (hears every request), its card: no request yet: {pl.get('hears')}")

        # ---- the second press: request.audio with a way out (net.server).  Before Home Assistant is there: a press
        # that installs nothing goes to the assistant, which has nobody to talk to
        srv = dict(man, id="testsrv", name="Test listener with a server", capabilities=dict(man["capabilities"], **{"net.server": [{"setting": "server", "proto": "tcp", "ports": [80]}]}),
                   settings=man["settings"] + [{"name": "server", "label": "Server", "type": "host"}])
        st, r = fp.upload(b, package(srv))
        second = "This plugin hears every request and can send what it hears to the server you enter."
        check(st == 200 and r.get("button") is True and r.get("second") == second, f"with net.server: the check names the second press and its words: {r.get('second')!r}")
        st, r = fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        inst = lambda: fp.plugins(b).get("install", {})
        check(r.get("install") == "waiting" and await until(lambda: inst().get("state") == "second") and inst().get("second") == second and not fp.plug(b, "testsrv"),
              f"the first press installs nothing: the Echo waits for the second, on its screen ({inst()})")
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(1.5)
        check(not fp.plug(b, "testsrv") and inst().get("state") == "second", "a press without the screen's ask (a page that skipped it) installs nothing either")
        st, r = fp.post(b, "/api/plugins/install")
        check(r.get("install") == "waiting" and inst().get("holds") == "plugin", f"the screen asks for the second press: the Echo waits for the button again: {r}")
        proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: (fp.plug(b, "testsrv") or {}).get("accepted")) and fp.logged(log, "(its second press)"), "the second press installs it")
        check((fp.plug(b, "testsrv") or {}).get("second") == second, "its card says where it can send what it hears")
        fp.post(b, "/api/plugins/testsrv/remove", "remove")
        check(await until(lambda: not fp.plug(b, "testsrv")), "(removed again)")

        # ---- request.text alone, beside it
        st, r = fp.upload(b, package(dict(man, id="testtext", name="Test reader", capabilities={"request.text": True})))
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: (fp.plug(b, "testtext") or {}).get("accepted")) and await says("ready, capabilities 00008000", 0, 15, "testtext"), "a plugin with request.text alone installed and running")

        # ---- Home Assistant (plaintext), its voice assistant: each run's audio by itself
        runs = []                       # per run: [audio bytearray, chunk sizes]
        ents = {}                       # per port: Home Assistant's entities of that Echo
        async def connect(port=PORT, sink=None):
            sink = runs if sink is None else sink
            async def handle_start(conv, flags, settings, phrase): sink.append([bytearray(), [], phrase]); return 0
            async def handle_stop(abort): pass
            async def handle_audio(data, *_):
                if sink: sink[-1][0].extend(data); sink[-1][1].append(len(data))
            for _ in range(50):
                try: c = APIClient("127.0.0.1", port, None); await c.connect(login=True); break
                except Exception: await asyncio.sleep(0.2)
            ents[port], _ = await c.list_entities_services()
            c.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio)
            await c.device_info()
            return c
        # no link to Home Assistant: nothing streams a request, so no copy either (the wake word, the button)
        proc.send_signal(signal.SIGUSR1); await asyncio.sleep(1.5); proc.send_signal(signal.SIGUSR2); await asyncio.sleep(1.5)
        check(not starts() and not starts(0, "testtext"), "no link to Home Assistant: the wake word and the button start no request, a plugin gets nothing")
        ha = await connect()
        ev = lambda t, d=None: ha.send_voice_assistant_event(t, d)
        await asyncio.sleep(2)
        check(not starts() and not any("audio outside a run" in l for l in plog()), "nothing outside a run: no request yet, no audio")

        async def run_until_audio(trigger, secs=1.5):
            k = len(runs); trigger()
            await until(lambda: len(runs) > k, 5)
            await until(lambda: len(runs) > k and len(runs[-1][0]) > LEAD_IN + secs * 32000, max(8, secs + 3))
            return runs[-1] if len(runs) > k else None

        def same(r, m, what, why=1):
            ok = r is not None and m is not None and int(m[5]) == len(r[0]) and m[7] == crc(r[0]) and int(m[4]) == 0 and int(m[9]) == 0 and int(m[10]) == 0 \
                 and int(m[3]) * 2 == len(r[0]) and int(m[2]) == why
            check(ok, f"{what}: the copy is what Home Assistant got, byte for byte ({m.group(0) if m else None}; Home Assistant {len(r[0]) if r else None} bytes, crc {crc(r[0]) if r else None})")

        # 1. the wake word: its slot and phrase; Home Assistant ends it (its VAD), then the texts
        n0 = len(plog()); t0 = len(plog("testtext"))
        r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1))
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END)
        check(await until(lambda: ends(n0)), "the end of the request's audio: REQ_END")
        s = starts(n0)
        check(s and s[0][2] == "1" and s[0][3] == "0" and s[0][4] == r[2], f"REQ_START: the wake word, its slot and the phrase Home Assistant got: {s[0].group(0) if s else None}")
        check(r and len(r[0]) > LEAD_IN and not any(r[0][:LEAD_IN]), "Home Assistant's audio opens with the lead-in's zeros")
        same(r, (ends(n0) or [None])[0], "the wake word")
        ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the lights"}); ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "0"})
        ev(Ev.VOICE_ASSISTANT_TTS_START, {"text": "Done, the lights are on"}); ev(Ev.VOICE_ASSISTANT_RUN_END)
        run_no = s[0][1] if s else "?"
        check(await says(f"run {run_no} text heard: turn on the lights", n0) and await says(f"run {run_no} text reply: Done, the lights are on", n0),
              f"REQ_TEXT: what was heard and the reply, with the request's number: {texts(n0)}")
        check(await says(f"run {run_no} text heard: turn on the lights", t0, 5, "testtext") and await says("text reply: Done", t0, 5, "testtext") and not starts(0, "testtext"),
              f"request.text alone: the texts, no request's start and no audio: {plog('testtext')[t0:]}")
        check(re.search(r"\(private\) plugin testreq: run \d+ text heard", text()) is not None and "(private) plugin testtext: " in text(),
              "a listener's lines are private in the log (what it was told may be in them; no log.read plugin gets them)")
        await asyncio.sleep(1)

        # 2. the action button; Home Assistant's error ends it
        n0 = len(plog()); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR2))
        ev(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "the test's end"})
        check(await until(lambda: ends(n0)), "(ended)")
        s = starts(n0)
        check(s and s[0][2] == "2", f"REQ_START: the action button: {s[0].group(0) if s else None}")
        same(r, (ends(n0) or [None])[0], "the button, ended by an error", why=5)
        await asyncio.sleep(1)

        # 3. a follow-up Home Assistant asked for (continue_conversation): a run of its own, the lead-in again
        n0 = len(plog()); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1))
        k = len(runs)
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END); ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "which light"})
        ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"}); ev(Ev.VOICE_ASSISTANT_RUN_END)
        await until(lambda: len(runs) > k, 5); await until(lambda: len(runs) > k and len(runs[-1][0]) > LEAD_IN + 32000, 6)
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END)
        check(await until(lambda: len(ends(n0)) >= 2), "(both ended)")
        s = starts(n0)
        check(len(s) >= 2 and s[1][2] == "3", f"REQ_START of the follow-up: {[x.group(0) for x in s]}")
        same(r, (ends(n0) or [None])[0], "the question's run")
        same(runs[-1] if len(runs) > k else None, (ends(n0)[1:] or [None])[0], "the follow-up")
        ev(Ev.VOICE_ASSISTANT_RUN_END); await asyncio.sleep(1)

        # 4. noise reduction on: the copy is still what Home Assistant got (taken after it)
        b.set(noise_reduction="medium")
        n0 = len(plog()); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1))
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END); await until(lambda: ends(n0))
        same(r, (ends(n0) or [None])[0], "noise reduction on")
        ev(Ev.VOICE_ASSISTANT_RUN_END); b.set(noise_reduction="off"); await asyncio.sleep(1)

        # 4b. a request's start before the plugin reads: the lead-in, then a long replay in one go (the wake word sent
        # too, the aligner waited for 1 s past its window: ~2.2 s of the ring) and the live audio, while the plugin
        # begins reading 3 s after REQ_START (as on an Echo, where it gets REQ_START late): ~4.5 s in its socket,
        # none of it lost (M1 of a review of b294fa0: the socket held 2 s, and the wake word and the command's start
        # were lost; on the PC a plugin that reads at once keeps up with the replay as it is sent)
        b.set(wake_word_in_request="on"); open(slow, "w").write("3000\n"); fp.post(b, "/api/plugins/testreq/set", "late=3000"); await asyncio.sleep(0.5)
        n0 = len(plog()); nl = len(text()); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1), 4)
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END); await until(lambda: ends(n0))
        check("the aligner took over 1 s" in text()[nl:] and "the wake word sent too" in text()[nl:], "(the replay waited for the aligner past its window, the word sent too)")
        same(r, (ends(n0) or [None])[0], "a long replay at the request's start")
        ev(Ev.VOICE_ASSISTANT_RUN_END); b.set(wake_word_in_request="off"); open(slow, "w").write("0\n"); fp.post(b, "/api/plugins/testreq/set", "late=0"); await asyncio.sleep(1)

        # 5. cancelled: the button while it listens
        n0 = len(plog()); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1))
        proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: ends(n0)), "(ended)")
        same(r, (ends(n0) or [None])[0], "cancelled by the button", why=3)
        await asyncio.sleep(1.5)

        # 6. the soft mute (the page's, as Home Assistant's switch): the copy stops with the microphones
        n0 = len(plog()); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1))
        b.set(mute="on")
        check(await until(lambda: ends(n0)), "(ended)")
        m = (ends(n0) or [None])[0]
        check(m is not None and m[2] == "7" and int(m[4]) == 0, f"the soft mute ends the copy (why 7): {m.group(0) if m else None}")
        n1 = len(plog()); proc.send_signal(signal.SIGUSR1); await asyncio.sleep(2)
        check(not starts(n1), "muted: the wake word starts no run, nothing comes")
        b.set(mute="off"); ev(Ev.VOICE_ASSISTANT_RUN_END); await asyncio.sleep(1)

        # 6b. muted, no microphone audio leaves the Echo, also for a run asked for before the mute (C1 of a review of
        # b294fa0).  A question back (continue_conversation), then the mute while the Echo thinks: the run's end starts
        # no follow-up, for Home Assistant or the plugin
        n0 = len(plog()); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1)); k = len(runs)
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END); ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "set a timer"})
        ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
        await asyncio.sleep(0.5); b.set(mute="on"); await asyncio.sleep(0.3); ev(Ev.VOICE_ASSISTANT_RUN_END); await asyncio.sleep(2.5)
        check(len(runs) == k and len(starts(n0)) == 1, f"a follow-up asked for, then muted: no run follows, for Home Assistant ({len(runs) - k}) or the plugin ({[x.group(0) for x in starts(n0)][1:]})")
        # 6c. an announcement with start_conversation on a muted Echo: it plays and is reported, and no run follows
        n0 = len(plog()); k = len(runs); url = f"http://127.0.0.1:{HTTP}/a.wav"
        res = await ha.send_voice_assistant_announcement_await_response(url, 15, "are you there", start_conversation=True); await asyncio.sleep(2)
        check(res is not None and len(runs) == k and not starts(n0), f"muted: start_conversation's announcement plays, and no run follows it ({len(runs) - k} runs, {[x.group(0) for x in starts(n0)]})")
        b.set(mute="off"); await asyncio.sleep(1)
        n0 = len(plog()); k = len(runs)
        res = await ha.send_voice_assistant_announcement_await_response(url, 15, "are you there", start_conversation=True)
        check(await until(lambda: len(runs) > k and starts(n0)) and starts(n0)[0][2] == "3", f"(unmuted, the same: a run follows it, a follow-up's copy: {[x.group(0) for x in starts(n0)]})")
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END); await until(lambda: ends(n0)); ev(Ev.VOICE_ASSISTANT_RUN_END); await asyncio.sleep(1)
        # 6d. a wake word that cut a reply waits for its end to start the next run: muted meanwhile, none starts
        n0 = len(plog()); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1)); k = len(runs)
        stalled.clear(); release.clear()
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END); ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "tell me a story"})
        ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "0"}); ev(Ev.VOICE_ASSISTANT_TTS_START, {"text": "Once upon a time"})
        ev(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP}/stall.wav"}); ev(Ev.VOICE_ASSISTANT_RUN_END)
        check(stalled.wait(5), "(the reply plays, its fetch stalls)"); await asyncio.sleep(0.5)
        nl = len(text()); proc.send_signal(signal.SIGUSR1)
        check(await until(lambda: "barge-in" in text()[nl:], 5), "(the wake word cuts the reply: its run waits for the reply's end)")
        b.set(mute="on"); release.set(); await asyncio.sleep(3)
        check(len(runs) == k and len(starts(n0)) == 1, f"muted before the cut reply ended: the wake word's run does not start ({len(runs) - k} runs, {[x.group(0) for x in starts(n0)][1:]})")
        b.set(mute="off"); await asyncio.sleep(1)

        # 7. a plugin that does not read: it loses blocks, Home Assistant none
        l7 = len(text()); fp.post(b, "/api/plugins/testreq/set", "deaf=1"); await asyncio.sleep(0.5)
        n0 = len(plog()); t = time.monotonic(); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1), 14)
        dt = time.monotonic() - t
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END)
        check(await until(lambda: ends(n0), 10), "(ended)")
        m = (ends(n0) or [None])[0]
        check(m is not None and int(m[4]) > 0 and int(m[5]) + 2 * int(m[4]) == 2 * int(m[3]) and int(m[9]) > 0 and len(r[0]) == 2 * int(m[3]),
              f"a plugin slow to read: blocks dropped and counted, the gap in the offsets, and Home Assistant has all of it: {m.group(0) if m else None}, Home Assistant {len(r[0]) if r else None} bytes")
        check(dt < 17, f"... and on time: {len(r[0]) if r else 0} bytes in {dt:.1f} s")
        check((fp.plug(b, "testreq") or {}).get("requests", {}).get("lost", 0) > 0, "its card counts what it did not take")
        # hassmic's own lines about it are private too: what it did not take is its choice (C3 of a review of b294fa0)
        check(re.search(r"\(private\) plugins: testreq: \d+ of the request's \d+ samples not taken", text()[l7:]) is not None
              and not re.search(r"^\S+ plugins: testreq[: ]", text()[l7:], re.M), f"hassmic's lines about a plugin that hears requests are private: {re.findall(r'^.*plugins: testreq.*$', text()[l7:], re.M)}")
        ev(Ev.VOICE_ASSISTANT_RUN_END); fp.post(b, "/api/plugins/testreq/set", "deaf=0"); await asyncio.sleep(4)

        # 8. "stop" the Echo acted on itself (music paused) is not passed on
        open(fake_src, "w").write("play\n"); await asyncio.sleep(1)
        n0 = len(plog()); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1))
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END); ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "stop"})
        check(await until(lambda: open(fake_src).read().strip() in ("paused", "stopped")), "(the music stopped by the \"stop\")")
        await asyncio.sleep(1)
        check(not any("text heard: stop" in l for l in plog()[n0:] + plog("testtext")), "a \"stop\" the Echo acted on itself: not passed on")
        ev(Ev.VOICE_ASSISTANT_RUN_END); await asyncio.sleep(1)

        # 9. the plugin sends the request's messages itself: refused, nothing begins, it runs on
        n0 = len(plog()); k = len(runs)
        fp.post(b, "/api/plugins/testreq/set", "try=send")
        check(await says("sent REQ_START, REQ_TEXT, REQ_END itself", n0), "(it sent them)")
        await asyncio.sleep(1.5)
        check(len(runs) == k and not starts(n0) and (fp.plug(b, "testreq") or {}).get("state") == "running", "REQ_START, REQ_TEXT, REQ_END from a plugin: nothing begins, it runs on")

        # 10. the link lost mid-run
        n0 = len(plog()); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1))
        await ha.disconnect(); ha = None
        check(await until(lambda: ends(n0)), "(ended)")
        m = (ends(n0) or [None])[0]
        check(m is not None and m[2] == "6", f"the link lost: why 6: {m.group(0) if m else None}")
        ha = await connect(); ev = lambda t, d=None: ha.send_voice_assistant_event(t, d)
        await asyncio.sleep(1)

        # 11. the pipeline's timeout (30 s)
        n0 = len(plog()); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1))
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_START)
        check(await until(lambda: ends(n0), 35), "(ended)")
        m = (ends(n0) or [None])[0]
        check(m is not None and m[2] == "4", f"the pipeline's timeout: why 4: {m.group(0) if m else None}")
        ev(Ev.VOICE_ASSISTANT_RUN_END); await asyncio.sleep(1)
        check(not any("audio outside a run" in l for l in plog()), "never a block outside the run begun")

        # ---- 3.3's partner: a wake word engine from a file and the listener
        st, r = fp.upload(b, package(wake_man, program="testwake", listed=False))
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: (fp.plug(b, "testwake") or {}).get("accepted")), "(a wake word plugin from a file, not the engine)")
        r = b.set(wake_engine="plugin:testwake")
        check("testreq hears every request" in json.dumps(r) and b.state() and fp.logged(log, "wake word: testreq hears every request"),
              f"chosen as the engine while the listener is on: refused, and said why: {r}")
        fp.post(b, "/api/plugins/testreq/enable", "off"); fp.post(b, "/api/plugins/testtext/enable", "off"); await asyncio.sleep(1)
        r = b.set(wake_engine="plugin:testwake")
        check(r.get("applied") == 1, f"the listener off: the engine may be chosen: {r}")
        fp.post(b, "/api/plugins/testreq/enable", "on"); await asyncio.sleep(2)
        p = fp.plug(b, "testreq") or {}
        check(p.get("state") != "running" and "testwake" in p.get("detail", "") and "official" in p.get("detail", ""),
              f"switched on beside that engine: not started, its card says why: {p.get('state')} {p.get('detail')}")
        st, r = fp.upload(b, package(version=2))
        check(st == 400 and "wake word engine is the plugin testwake" in r.get("error", ""), f"its update is not installed beside that engine: {r.get('error')}")
        fp.post(b, "/api/plugins/cancel")
        r = b.set(wake_engine="amazon")
        check(await until(lambda: (fp.plug(b, "testreq") or {}).get("state") == "running", 10), "another engine: the listener starts again")
        # after a restart with both on file (a state copied from elsewhere): the listener is the one not started
        stop(proc)
        cfg = open(os.path.join(state, "config")).read()
        open(os.path.join(state, "config"), "w").write(re.sub(r"wake_engine=[^\n]*", "wake_engine=plugin:testwake", cfg) if "wake_engine=" in cfg else cfg + "wake_engine=plugin:testwake\n")
        nr = len(plog("testwake")); proc = launch(); wait_port(PORT, WEB); b = Browser(WEB); check(b.login_with_button(proc), "(logged in again)")
        await asyncio.sleep(12)          # (plugins other than the wake engine's start 10 s after the start: none of them here)
        p = fp.plug(b, "testreq") or {}
        check(p.get("state") != "running" and "testwake" in p.get("detail", ""), f"a start with both: the listener is not started: {p.get('state')} {p.get('detail')}")
        b.set(wake_engine="amazon")
        check(await until(lambda: (fp.plug(b, "testreq") or {}).get("state") == "running", 10), "(running again)")
        fp.post(b, "/api/plugins/testwake/remove", "remove")

        # ---- an update that adds a way out: the button, both presses
        st, r = fp.upload(b, package(version=2, capabilities=dict(man["capabilities"], **{"net.udp": 1})))
        check(st == 200 and r.get("button") is True and r.get("second") == "This plugin hears every request and can send what it hears to your network."
              and any(c["kind"] == "new" for c in r.get("changes", [])), f"net.udp added to it: the button, and the second press: {r.get('second')!r} {r.get('changes')}")
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: inst().get("state") == "second") and (fp.plug(b, "testreq") or {}).get("version") == 1, "one press: still version 1, the second asked for")
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: (fp.plug(b, "testreq") or {}).get("version") == 2), "the second press: version 2")
        if ha:
            try: await ha.disconnect()
            except Exception: pass
            ha = None
        stop(proc)

        # ---- -w remote: Home Assistant's wake word in the stream; the copy only from it on
        nr = len(plog()); proc = launch("-w", "remote"); wait_port(PORT, WEB)
        check(await says("ready", nr, 20), "(the listener runs again)")
        runs.clear(); ha = await connect(); ev = lambda t, d=None: ha.send_voice_assistant_event(t, d)
        check(await until(lambda: runs and len(runs[-1][0]) > 3 * 32000, 10), "-w remote: Home Assistant gets the stream before any wake word")
        n0 = len(plog())
        check(not starts(n0), "... and the plugin nothing of it")
        sent_before = len(runs[-1][0])
        ev(Ev.VOICE_ASSISTANT_WAKE_WORD_END); await asyncio.sleep(1.5)
        ev(Ev.VOICE_ASSISTANT_STT_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END)
        check(await until(lambda: ends(n0)), "(ended)")
        s = starts(n0); m = (ends(n0) or [None])[0]
        check(s and s[0][2] == "4", f"REQ_START: Home Assistant's wake word: {s[0].group(0) if s else None}")
        ok = False
        if m:
            a, sizes = runs[-1][0], runs[-1][1]; at = 0; want = int(m[5])
            for c in sizes:
                if at + want <= len(a) and crc(a[at:at + want]) == m[7]: ok = at >= sent_before - 4096; break
                at += c
        check(ok and int(m[4]) == 0, f"the copy is a stretch of what Home Assistant got, from its wake word on, none of it before: {m.group(0) if m else None}")
        ev(Ev.VOICE_ASSISTANT_RUN_END)
        # muted (Home Assistant's switch): no microphone audio leaves the Echo, so the stream for Home Assistant's wake
        # word stops too, its wake word starts nothing, and unmuted it streams again (C2 of a review of b294fa0: it
        # streamed through the mute, and Home Assistant's wake word then began a plugin's copy)
        k = len(runs)
        check(await until(lambda: len(runs) > k and len(runs[-1][0]) > 32000, 8), "(the stream for the next wake word)")
        mute = next(e.key for e in ents[PORT] if e.object_id == "mute")
        ha.switch_command(mute, True); await asyncio.sleep(0.5)
        k = len(runs); got = len(runs[-1][0]); n0 = len(plog()); await asyncio.sleep(2)
        check(len(runs) == k and len(runs[-1][0]) - got < 3200, f"-w remote, muted: the stream to Home Assistant stops ({len(runs[-1][0]) - got} bytes in 2 s, {len(runs) - k} runs)")
        ev(Ev.VOICE_ASSISTANT_WAKE_WORD_END); await asyncio.sleep(1.5)
        check(not starts(n0), f"muted: Home Assistant's wake word starts no request, the plugin gets nothing: {[x.group(0) for x in starts(n0)]}")
        ha.switch_command(mute, False)
        check(await until(lambda: len(runs) > k and len(runs[-1][0]) > 32000, 8), "unmuted: the stream for Home Assistant's wake word again")
        cfg = open(os.path.join(state, "config")).read()
        check("wake_engine=homeassistant" not in cfg, f"-w remote in ARGS is not written as the owner's engine by a setting saved meanwhile (the mute): {re.findall(r'wake_engine=.*', cfg)}")
        try: await ha.disconnect()
        except Exception: pass
        ha = None
        stop(proc)

        # ---- Wyoming: the copy, and its transcript as REQ_TEXT
        from wyoming.client import AsyncTcpClient
        from wyoming.audio import AudioChunk
        from wyoming.asr import Transcript
        from wyoming.pipeline import RunPipeline
        from wyoming.satellite import RunSatellite
        nr = len(plog()); proc = launch("-P", "wyoming", "-p", str(WYO)); wait_port(WYO)
        check(await says("ready", nr, 20), "(the listener runs under Wyoming)")
        n0 = len(plog()); audio = bytearray()
        async with AsyncTcpClient("127.0.0.1", WYO) as cl:
            await cl.write_event(RunSatellite().event()); await asyncio.sleep(0.5)
            proc.send_signal(signal.SIGUSR1)
            async def reader():
                while True:
                    e = await cl.read_event()
                    if e is None: return
                    if AudioChunk.is_type(e.type): audio.extend(AudioChunk.from_event(e).audio)
            rt = asyncio.ensure_future(reader())
            await until(lambda: len(audio) > LEAD_IN + 48000, 8)
            await cl.write_event(Transcript(text="what time is it").event())
            check(await until(lambda: ends(n0)), "(ended)")
            await asyncio.sleep(0.5); rt.cancel()
        same([audio], (ends(n0) or [None])[0], "Wyoming")
        check(await says("text heard: what time is it", n0), "Wyoming's transcript: REQ_TEXT heard")
        stop(proc)

        # ---- Wyoming with -w remote: the copy from Home Assistant's detection on, as ESPHome's wake word end (M3 of a
        # review of b294fa0: none, and the transcript went out under the last request's number)
        from wyoming.wake import Detection
        from wyoming.audio import AudioStart, AudioStop
        nr = len(plog()); proc = launch("-P", "wyoming", "-p", str(WYO), "-w", "remote"); wait_port(WYO)
        check(await says("ready", nr, 20), "(the listener runs under Wyoming, -w remote)")
        n0 = len(plog()); audio = bytearray(); sizes = []; pipelines = [0]
        async with AsyncTcpClient("127.0.0.1", WYO) as cl:
            async def reader():
                while True:
                    e = await cl.read_event()
                    if e is None: return
                    if AudioChunk.is_type(e.type): a = AudioChunk.from_event(e).audio; audio.extend(a); sizes.append(len(a))
                    elif e.type == "run-pipeline": pipelines[0] += 1
            rt = asyncio.ensure_future(reader())
            await cl.write_event(RunSatellite().event())
            check(await until(lambda: len(audio) > 64000, 8) and not starts(n0), "-w remote over Wyoming: the stream, and nothing for the plugin before a wake word")
            mark = len(audio)
            await cl.write_event(Detection(name="alexa").event()); await asyncio.sleep(1.5)
            await cl.write_event(Transcript(text="what time is it").event())
            check(await until(lambda: ends(n0)), "(ended)")
            s = starts(n0); m = (ends(n0) or [None])[0]
            check(s and s[0][2] == "4", f"REQ_START: Home Assistant's wake word, over Wyoming: {s[0].group(0) if s else None}")
            ok = False
            if m:
                at = 0; want = int(m[5])
                for c in sizes:
                    if at + want <= len(audio) and crc(audio[at:at + want]) == m[7]: ok = at >= mark - 4096; break
                    at += c
            check(ok and int(m[4]) == 0, f"the copy is a stretch of what Home Assistant got, from its detection on: {m.group(0) if m else None}")
            run_no = s[0][1] if s else "?"
            check(await says(f"run {run_no} text heard: what time is it", n0), f"its transcript: REQ_TEXT with the request's number: {texts(n0)}")
            # the reply plays; then the stream waits for the next wake word, and a transcript of that run (no detection
            # opened a request) is no request's text
            p0 = pipelines[0]
            await cl.write_event(AudioStart(rate=16000, width=2, channels=1).event())
            await cl.write_event(AudioChunk(rate=16000, width=2, channels=1, audio=bytes(6400)).event()); await cl.write_event(AudioStop().event())
            check(await until(lambda: pipelines[0] > p0, 8), "(the reply played: the stream for the next wake word)")
            await asyncio.sleep(0.5); await cl.write_event(Transcript(text="nobody asked").event()); await asyncio.sleep(1.5)
            check(not any("nobody asked" in l for l in plog()[n0:]), f"a transcript of a run no detection opened: not passed as the last request's: {texts(n0)}")
            rt.cancel()
        stop(proc)

        # ---- -D with -w remote over Wyoming (F3 of a review of c4ace24): the client's detection during its reply is a
        # request from there on (a copy), and the reply is the client's to cut, as with a wake word heard on the Echo
        # under -D: it played on (the cut flushed it), and its audio-stop ends nothing of the request
        nr = len(plog()); proc = launch("-P", "wyoming", "-p", str(WYO), "-w", "remote", "-D"); wait_port(WYO)
        check(await says("ready", nr, 20), "(the listener runs under Wyoming, -w remote, -D)")
        n0 = len(plog()); l0 = len(text()); audio = bytearray(); pipelines = [0]; play = os.path.join(state, "play.raw")
        async with AsyncTcpClient("127.0.0.1", WYO) as cl:
            async def reader():
                while True:
                    e = await cl.read_event()
                    if e is None: return
                    if AudioChunk.is_type(e.type): audio.extend(AudioChunk.from_event(e).audio)
                    elif e.type == "run-pipeline": pipelines[0] += 1
            rt = asyncio.ensure_future(reader())
            await cl.write_event(RunSatellite().event())
            check(await until(lambda: len(audio) > 64000, 8), "(-D, -w remote: the stream)")
            await cl.write_event(Detection(name="alexa").event())
            check(await until(lambda: starts(n0)), "(-D: Home Assistant's wake word, a request)")
            await cl.write_event(AudioStart(rate=16000, width=2, channels=1).event())
            sizes = []
            for k in range(25):                                 # 2.5 s of reply, sent as it plays; the wake word after 0.5 s
                await cl.write_event(AudioChunk(rate=16000, width=2, channels=1, audio=b"\1\0" * 1600).event())
                await asyncio.sleep(0.1)
                if k == 4:
                    a0 = len(audio); await cl.write_event(Detection(name="alexa").event())
                    await asyncio.sleep(0.3); sizes.append(os.path.getsize(play) if os.path.exists(play) else 0)
            sizes.append(os.path.getsize(play) if os.path.exists(play) else 0)
            lg = text()[l0:]
            check("left to the client (-D)" in lg and "barge-in" not in lg and sizes[1] > sizes[0] + 16000,
                  f"-D, -w remote: Home Assistant's wake word during the reply leaves the reply to the client: it plays on ({sizes})")
            check(len(audio) > a0 + 32000, f"-D: the microphone streams on through the reply ({len(audio) - a0} bytes)")
            s, e = starts(n0), ends(n0)
            check(len(s) == 2 and s[1][2] == "4" and len(e) == 1 and e[0][1] == s[0][1] and e[0][2] == "3",
                  f"-D: the first request's copy ends (cancelled) and a new one begins at the wake word: {[m.group(0) for m in s + e]}")
            await cl.write_event(AudioStop().event()); await asyncio.sleep(1)
            check(len(ends(n0)) == 1, f"-D: the reply's audio-stop ends nothing of the request it said began: {[m.group(0) for m in ends(n0)]}")
            await cl.write_event(Transcript(text="what time is it").event())
            check(await until(lambda: len(ends(n0)) == 2), "(-D: the transcript ends the request)")
            run_no = s[1][1] if len(s) > 1 else "?"
            check(await says(f"run {run_no} text heard: what time is it", n0), f"-D: its transcript is the new request's text: {texts(n0)}")
            p0 = pipelines[0]
            await cl.write_event(AudioStart(rate=16000, width=2, channels=1).event())
            await cl.write_event(AudioChunk(rate=16000, width=2, channels=1, audio=bytes(6400)).event()); await cl.write_event(AudioStop().event())
            check(await until(lambda: pipelines[0] > p0, 8), "-D: its reply's end ends the run: the stream for the next wake word")
            rt.cancel()
        stop(proc)

        # ---- every other capability granted, request.* not: no REQ_* at all (docs/plugins.md 6)
        mus, wk = (json.load(open(f"{ROOT}/tests/plugins/{n}.json")) for n in ("testmusic", "testwake"))
        allm = dict(man, id="testall", name="Test all but requests",
                    capabilities=dict(mus["capabilities"], call=True, storage=4, files=1, **{"log.read": True}),
                    settings=man["settings"] + [mus["settings"][0], {k: v for k, v in wk["settings"][0].items() if k != "catalog"}])
        nr = len(plog()); proc = launch(); wait_port(PORT, WEB); b = Browser(WEB, b.sk)
        check(await says("ready", nr, 20), "(the listener runs again)")
        st, r = fp.upload(b, package(allm))
        check(st == 200, f"(a plugin with every capability but request.*: checked: {r.get('error')})")
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(await says("ready, capabilities", 0, 20, "testall"), f"(installed, running: {plog('testall')[-2:]})")
        runs.clear(); ha = await connect(); ev = lambda t, d=None: ha.send_voice_assistant_event(t, d); await asyncio.sleep(1)
        n0 = len(plog()); a0 = len(plog("testall")); r = await run_until_audio(lambda: proc.send_signal(signal.SIGUSR1))
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END); ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "what is on"})
        ev(Ev.VOICE_ASSISTANT_TTS_START, {"text": "nothing"}); ev(Ev.VOICE_ASSISTANT_RUN_END)
        check(await until(lambda: ends(n0)) and await says("text reply: nothing", n0), "(the listener: the request's copy and its texts)")
        await asyncio.sleep(1)
        check(not any(re.match(r"run \d+ |audio outside", l) for l in plog("testall")[a0:]), f"the plugin with every other capability: no REQ_* at all: {plog('testall')[a0:]}")
        fp.post(b, "/api/plugins/testall/remove", "remove")
        check(await until(lambda: not fp.plug(b, "testall")), "(removed again)")
        try: await ha.disconnect()
        except Exception: pass
        ha = None
        stop(proc)

        # ---- two Echos of one arbitration network under one Home Assistant, the plugin on both: the round's winner
        # streams the request, and only its plugin gets a copy (docs/plugins.md 6)
        stB, logB = os.path.join(top, "stateB"), os.path.join(top, "logB")
        shutil.copytree(state, stB, ignore=shutil.ignore_patterns("arb_key", "web_wait", "*.sock"))
        nkey = base64.b64encode(os.urandom(32)).decode()
        for d in (state, stB):
            if os.path.exists(os.path.join(d, "arb_key")): os.remove(os.path.join(d, "arb_key"))
            open(os.path.join(d, "arbitration"), "w").write(f"join 1\nctr 0\nnet {0x5e:016x} {nkey}\n")
        arb_env = dict(env, HASSMIC_ARB_SCALE="0.1", HASSMIC_ARB_RIVALS="0")
        open(logB, "a").close(); nr, nrB = len(plog()), 0
        pa = fp.start(state, log, extra=["-a", str(ARB), "-i", str(ARB + 10)], env=dict(arb_env, HASSMIC_TEST_SCORE="900")); procs.append(pa)
        pb = fp.start(stB, logB, extra=["-p", str(PORT2), "-W", str(WEB2), "-n", "Echo Plug B", "-a", str(ARB), "-i", str(ARB + 11)],
                      env=dict(arb_env, HASSMIC_TEST_SCORE="100")); procs.append(pb)
        wait_port(PORT, WEB); wait_port(PORT2, WEB2)
        check(await says("ready", nr, 20) and await says("ready", nrB, 20, lg=logB), "(the listener runs on both)")
        ba, bb = Browser(WEB, b.sk), Browser(WEB2, b.sk)
        heard = lambda br, node: any(x["node"] == node for x in ((br.state() or {}).get("arbitration") or {}).get("members", []))
        check(await until(lambda: heard(ba, "echo-plug-b") and heard(bb, "echo-plug"), 20), "(both Echos members of one network, each heard the other)")
        runs.clear(); runsB = []
        ha = await connect(); haB = await connect(PORT2, runsB); ev = lambda t, d=None: ha.send_voice_assistant_event(t, d)
        await asyncio.sleep(4)                  # (a round needs 3 s heard)
        n0, n0B, l0, l0B = len(plog()), len(plog(lg=logB)), len(text()), len(text(logB))
        pa.send_signal(signal.SIGUSR1); pb.send_signal(signal.SIGUSR1)
        check(await until(lambda: "this Echo answers" in text()[l0:] and "another Echo answers" in text(logB)[l0B:], 8), "(the round: the louder one answers)")
        await until(lambda: runs and len(runs[-1][0]) > LEAD_IN + 32000, 8)
        ev(Ev.VOICE_ASSISTANT_RUN_START); ev(Ev.VOICE_ASSISTANT_STT_VAD_END)
        check(await until(lambda: ends(n0)), "(the winner's request ended)")
        same(runs[-1] if runs else None, (ends(n0) or [None])[0], "the round's winner")
        await asyncio.sleep(1)
        check(not runsB and not starts(n0B, lg=logB), f"the other Echo streams nothing, and its plugin gets no copy: {len(runsB)} runs, {[x.group(0) for x in starts(n0B, lg=logB)]}")
        ev(Ev.VOICE_ASSISTANT_RUN_END)
        try: await haB.disconnect()
        except Exception: pass
    finally:
        httpd.shutdown()
        if ha:
            try: await ha.disconnect()
            except Exception: pass
        for p in procs:
            if p.poll() is None: stop(p)
    if check.failed:
        print(text()[-6000:])
    shutil.rmtree(top, ignore_errors=True)
    print("FAILED" if check.failed else "all good")
    return 1 if check.failed else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
