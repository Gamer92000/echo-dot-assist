#!/usr/bin/env python3
"""Phase 5 of docs/plugins.md on the PC: a plugin's calls (`call`), with a stand-in phone
(tests/plugins/testcall.c) under build/plugrun-host and build/hassmic-host-screen (the Echo Show's build: the screen's
call view reaches a stand-in UI, tests/fake_screen.py --ui), a second Echo (build/hassmic-host) of the same arbitration
network for Drop In, and this script as the settings page and as Home Assistant (keyed, as once adopted).

The install: call's line and the actions marked "calls" in the owner's words; a manifest whose action is marked
"calls" without the capability, and one with wake beside call, refused.  A call going out from Home Assistant's
action (its token): the screen names the plugin beside the callee, the ring's light, connected; the room (the call
microphone, HASSMIC_CALL_CAP: a 700 Hz tone) reaches the plugin as 20 ms frames, the plugin's own (a 440 Hz tone)
plays on the Voip stream; the music is paused; Home Assistant's Drop In entities stay idle, the plugin's own entity
says it.  Ended by the action button.  A call coming in rings (the ringtone, the light, the screen) and is never
live until a person at the Echo answers: the plugin's early CALL_LIVE refused, no room audio; Home Assistant's and
the card's Answer (actions marked "calls": a token, which only a call going out takes) refused; answered by the
action button (the ringtone stops at the press, before the far side is in), by the screen; ended by the plugin, by
"<wake word>, stop", by "hang up" said in a request (Home Assistant never gets that run), by the screen.  After the
end the plugin's own copy of the room's socket (a dup() it kept) gets nothing more and reads its end closed.  Busy
both ways against Drop In, and a Drop In that ends here with a plugin's ring or call right behind it (no wait between)
never leaves the plugin's call unshown; with Drop In's end in the order before that was fixed
(HASSMIC_DROPIN_LATE_IDLE), the core's own guard alone keeps the ring shown.  Do not disturb and the microphones off refuse it.  Ring limits: three unanswered in the window, the fourth refused, after
the window again.  Every refusal of the plugin's own tries (a call out without a token, with one made up, one used,
one too old; CALL_RINGING_THERE, CALL_LIVE, CALL_END with no call; a second ring).  The plugin crashing mid-call: the
call ends cleanly, and the next one works.  No room audio before an answer or after the end.
Everything stays on loopback (HASSMIC_ARB_ADDR)."""
import asyncio, base64, json, math, os, re, resource, signal, struct, subprocess, sys, tempfile, time, wave
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser
import fake_plugins as fp
from aioesphomeapi import APIClient, Event, TextSensorState, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk"))
import plugcheck
check = fp.check
PA, WA, AA = 16981, 16982, 16983          # the plugin's Echo: ESPHome, the page, Drop In's audio
PB, WB, AB = 16984, 16985, 16986          # the other Echo
ARB, BCAST = 16987, "127.255.255.255"


def level(path, secs):
    """dBFS and pitch (zero crossings) of the last secs of a raw 16 kHz file"""
    try: x = np.fromfile(path, np.int16).astype(float)[-16000 * secs:]
    except OSError: return -200, 0
    if not len(x): return -200, 0
    return 10 * np.log10((x ** 2).mean() / 32768 ** 2 + 1e-20), int(np.count_nonzero(np.diff(np.signbit(x))) / 2 / secs)


async def until(cond, secs=8):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if cond(): return True
        await asyncio.sleep(0.05)
    return bool(cond())


async def main():
    top = tempfile.mkdtemp(); state = os.path.join(top, "state"); os.mkdir(state); log = os.path.join(top, "log")
    bstate = os.path.join(top, "peer"); os.mkdir(bstate); blog = os.path.join(bstate, "log")
    for k in ("author", "index"): plugcheck.otatool.keygen(os.path.join(top, k + ".key"), os.path.join(top, k + ".pub"))
    man = json.load(open(f"{ROOT}/tests/plugins/testcall.json"))
    # stand-ins: stock's incoming ringtone (the PC has none, and rings silently without it), the call microphone (a 700
    # Hz tone at -20 dBFS, as Amazon's call pipeline leaves a talker), a music source to be paused
    earcons = os.path.join(top, "earcons"); os.mkdir(earcons)
    with wave.open(os.path.join(earcons, "comms_call_incoming_ringtone.wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
        w.writeframes((8000 * np.sin(2 * np.pi * 440 * np.arange(48000) / 48000)).astype("<i2").tobytes())
    callcap = os.path.join(top, "callmic.raw")
    (32768 * 10 ** (-20 / 20) * np.sqrt(2) * np.sin(2 * np.pi * 700 * np.arange(32000) / 16000)).astype("<i2").tofile(callcap)
    fake_src, voip, play = os.path.join(state, "fake.src"), os.path.join(state, "voip.raw"), os.path.join(state, "play.raw")
    late_idle = os.path.join(top, "late-idle")     # (while it exists: Drop In's end in the order before C1's fix, 6d)
    # the stand-in screen UI (fake_screen.py's): what hassmic tells it, in a file; what it says, from a fifo
    ui_log, ui_fifo = os.path.join(top, "ui.log"), os.path.join(top, "ui.fifo"); os.mkfifo(ui_fifo)
    runner = os.path.join(top, "ui.sh")
    open(runner, "w").write(f"#!/bin/sh\nexec {sys.executable} {ROOT}/tests/fake_screen.py --ui {top}\n"); os.chmod(runner, 0o755)
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    api_key = base64.b64encode(os.urandom(32)).decode()
    for st_, extra in ((state, "drop_in_answer=auto\n"), (bstate, "")):
        open(os.path.join(st_, "arbitration"), "w").write(f"arbitrate 1\nctr 0\nnet {net}\n")
        open(os.path.join(st_, "config"), "w").write("even_volume=off\n" + extra)        # (full scale: the call's audio as sent)
    open(os.path.join(state, "api_key"), "w").write(api_key + "\n")

    def package(base=man, **changes):
        m = dict(base, **changes); "requires" in changes or m.update(requires=plugcheck.requires_for(m["capabilities"])); mp = os.path.join(top, "m.json"); json.dump(m, open(mp, "w")); out = os.path.join(top, "p.hmp")
        plugcheck.pack(mp, f"{ROOT}/build/testcall", os.path.join(top, "author.key"), out, license_=f"{ROOT}/LICENSE", arch="x86_64")
        plugcheck.list_package(out, os.path.join(top, "index.key"), os.path.join(top, "o.hmp"), arch="x86_64")
        return open(os.path.join(top, "o.hmp"), "rb").read()

    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=play, HASSMIC_VOIP=voip,
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR=BCAST, HASSMIC_MODELS=os.path.join(state, "models"),
               HASSMIC_FAKE_WHISPER="1", HASSMIC_LOG=log, HASSMIC_CLOCK_SYNCED="0", HASSMIC_PLUGINS_PUB=os.path.join(top, "index.pub"),
               HASSMIC_LOGIN_MS="60000:300", HASSMIC_EARCONS=earcons + "/", HASSMIC_CALL_CAP=callcap, HASSMIC_FAKE_SOURCE=fake_src,
               HASSMIC_SCREEN=runner, FAKE_UI_LOG=ui_log, FAKE_UI_FIFO=ui_fifo, HASSMIC_SCREEN_READY_MS="5000",
               HASSMIC_DROPIN_TIMES="3000 4000", HASSMIC_TEST_SCORE="500", HASSMIC_PLUGIN_LINES="1000", HASSMIC_DROPIN_LATE_IDLE=late_idle,
               # rings of 6 s, the far side's answer within 3 s, a call going out taken within 5 s, rings counted over 15 s
               HASSMIC_PLUGIN_CALL_TIMES="6000:3000:5000:15000")
    proc = subprocess.Popen([f"{ROOT}/build/plugrun-host", "-L", "--", f"{ROOT}/build/hassmic-host-screen", "-P", "esphome", "-p", str(PA), "-n", "Echo Plug",
                             "-L", "-z", "0", "-o", "0", "-a", str(ARB), "-i", str(AA), "-W", str(WA)], env=env, stderr=open(log, "a"),
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    benv = dict(os.environ, HASSMIC_STATE=bstate, HASSMIC_PLAY=os.path.join(bstate, "play.raw"), HASSMIC_VOIP=os.path.join(bstate, "voip.raw"),
                HASSMIC_MDNS_FILE=os.path.join(bstate, "none"), HASSMIC_ARB_ADDR=BCAST, HASSMIC_MODELS=os.path.join(bstate, "models"),
                HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_TEST_SCORE="500", HASSMIC_DROPIN_TIMES="3000 4000", HASSMIC_LOGIN_MS="60000:300")
    bproc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PB), "-n", "Echo Peer", "-L", "-z", "0", "-o", "0", "-a", str(ARB),
                              "-i", str(AB), "-W", str(WB)], env=benv, stderr=open(blog, "a"))
    text = lambda p=log: open(p, errors="replace").read()
    plog = lambda: [l.split("plugin testcall: ", 1)[1] for l in text().splitlines() if "plugin testcall: " in l]
    async def says(t, n0=0, secs=8): return await until(lambda: any(t in l for l in plog()[n0:]), secs)
    async def logged(t, n0=0, secs=8, p=log): return await until(lambda: t in text(p)[n0:], secs)
    # what the UI was told, as text (the band's rows are binary and have no line end: a line may follow one on its line)
    ui = lambda: open(ui_log, "rb").read().decode("latin-1") if os.path.exists(ui_log) else ""
    def ui_say(line):
        for _ in range(20):
            try:
                with open(ui_fifo, "w") as q: q.write(line + "\n"); return
            except BrokenPipeError: time.sleep(0.05)
    ha = None
    try:
        wait_port(PA, WA, PB, WB)
        b = Browser(WA); check(b.login_with_button(proc), "the plugin's Echo: logged in with the button")
        bb = Browser(WB); check(bb.login_with_button(bproc), "the other Echo: logged in")
        b.set(plugins="on")

        # ---- the install: call's line, the actions that place calls
        st, r = fp.upload(b, package())
        lines = {c["cap"]: c["line"] for c in r.get("caps", [])}
        check(st == 200 and r.get("button") is True, f"a new plugin waits for the button: {st} {r.get('error')}")
        check(lines.get("call") == "Makes and takes calls: a call coming in is answered only at the Echo, one goes out only when Home Assistant or the settings page asks. Hears the room during a call"
              and lines.get("ha.actions") == 'Home Assistant can tell it to: "Call" (and have it place a call), "Answer" (also a button on its card; and have it place a call), "Ring", "Hang up" (also a button on its card), "Try"',
              f"call's line, and each action that places calls said so: {lines}")
        fp.post(b, "/api/plugins/install"); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: (fp.plug(b, "testcall") or {}).get("accepted")), "the press installs it")
        bad_actions = [dict(a) for a in man["capabilities"]["ha.actions"]]
        for caps, why in ((dict({k: v for k, v in man["capabilities"].items() if k != "call"}), "needs the capability \"call\""),
                          (dict(man["capabilities"], wake=True), "way out")):
            st, r = fp.upload(b, package(id="badcall", capabilities=caps))
            check(st == 400 and why in r.get("error", ""), f"refused: {why}: {r.get('error')}")
            fp.post(b, "/api/plugins/cancel")
        check(await says("ready", 0, 20) and await says("line idle"), f"it runs: {plog()[-4:]}")

        # ---- Home Assistant, keyed: its entities and actions; the Drop In entities of hassmic's own beside them
        states, seq = {}, []
        async def connect():
            for _ in range(50):
                try:
                    c = APIClient("127.0.0.1", PA, None, noise_psk=api_key); await c.connect(login=True); break
                except Exception: await asyncio.sleep(0.2)
            ents, svcs = await c.list_entities_services()
            def on_state(s):
                v = getattr(s, "event_type", None) if isinstance(s, Event) else getattr(s, "state", None)
                states[s.key] = v; seq.append((s.key, v))
            c.subscribe_states(on_state)
            starts = []
            async def handle_start(conv, flags, settings, phrase): starts.append(time.monotonic()); return 0
            async def handle_stop(abort): pass
            async def handle_audio(data, *_): pass
            c.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio)
            return c, {e.object_id: e for e in ents}, {s.name: s for s in svcs}, starts
        ha, by, svc, starts = await connect()
        check(all(f"p8_testcall_{n}" in svc for n in ("call", "answer", "ring", "hang_up", "try")) and "p8_testcall_line" in by and "drop_in" in by,
              f"its actions and entities in Home Assistant, beside Drop In's: {sorted(k for k in svc if k.startswith('p8'))}")
        act = lambda name, **a: ha.execute_service(svc[f"p8_testcall_{name}"], a)
        line = lambda: states.get(by["p8_testcall_line"].key)
        dropin_seen = lambda: {v for k, v in seq if k == by["drop_in"].key}
        bb_dropin = lambda: (bb.state() or {}).get("dropin") or {}
        check(await until(lambda: any(m["node"] == "echo-plug" for m in bb.state()["arbitration"]["members"]), 10), "the two Echos see each other in the network")

        # ---- 1. a call going out, from Home Assistant's action: the token, the screen, the light, the media both ways
        open(fake_src, "w").write("play\n"); await asyncio.sleep(1)          # (a music source plays)
        n0, l0, u0 = len(plog()), len(text()), len(ui())
        await act("call", who="Bob")
        check(await says("action call from 1, with a token", n0) and await says("call out: 0 (0)", n0), f"an action marked calls brings a token, CALL_OUT takes it: {plog()[n0:]}")
        check(await logged("testcall: a call goes out", l0) and await logged("testcall: the call rings on the far side", l0), "hassmic: the call goes out, the far side rings")
        check(await until(lambda: "peer Test phone: Bob\n" in ui()[u0:]) and await until(lambda: "p+ call_outbound_ringing" in ui()[u0:]),
              f"the screen: the plugin's name beside the callee, the call's light going out: {re.findall(r'(?:peer|p[+-]) [^\n]*', ui()[u0:])[:6]}")
        check(await says("live: 0 (0)", n0) and await logged("testcall: the call is live (the far side took it)", l0), "the far side took it: live")
        check(await until(lambda: "p+ call_connected " in ui()[u0:]), "the screen's light: connected")
        check(await until(lambda: open(fake_src).read().strip() == "paused"), "the music paused for the call")
        check(await until(lambda: line() == "live"), f"Home Assistant sees the call by the plugin's own entity: {line()}")
        check((fp.plug(b, "testcall") or {}).get("call") == "live", "its card says it is in a call")
        check(await says("room: 50 frames", n0, 5), f"the room reaches the plugin, 50 frames a second: {[l for l in plog()[n0:] if 'room' in l][:3]}")
        m = [re.search(r"room: (\d+) frames, (-?[\d.]+) dBFS, (\d+) Hz", l) for l in plog()[n0:]]; m = [x for x in m if x]
        # (its first second, before the plugin's own tone plays here: from then on the echo suppressor ducks the room
        # by 12 dB while the far side talks, as in a Drop In; the pitch by zero crossings within frames, a few short)
        check(m and float(m[0][2]) > -25 and 600 <= int(m[0][3]) <= 760, f"... the call microphone's 700 Hz tone, as Amazon's call pipeline hands it ({m[0].group(0) if m else None})")
        await asyncio.sleep(1.5)
        lv, hz = level(voip, 1)
        check(lv > -30 and 400 <= hz <= 480, f"the plugin's 440 Hz plays on the Voip stream ({lv:.1f} dBFS, {hz} Hz)")
        check(dropin_seen() <= {"idle"}, f"Home Assistant's Drop In entities hear nothing of it: {dropin_seen()}")
        # Drop In while the plugin has the call: busy, both ways in
        st_, _, data = b.call("POST", "/api/dropin", b"echo-peer")
        check(st_ == 409 and b"already in another call" in data, f"this Echo's page drops in meanwhile: refused here ({st_} {data})")
        bb.call("POST", "/api/dropin", b"echo-plug")
        check(await until(lambda: bb_dropin().get("result") == "busy", 6), f"the other Echo drops in: busy ({bb_dropin()})")
        # ended by the action button
        n1 = len(plog()); proc.send_signal(signal.SIGUSR2)
        check(await says("hung up by hassmic: 1 after", n1), f"the action button ends it: CALL_HANGUP (the button): {plog()[n1:]}")
        check(await logged("testcall: call ended after", l0) and await until(lambda: "p+ call_connected_end" in ui()[u0:]), "ended: the end's light")
        check(await says("kept copy after CALL_HANGUP: 0 frames later than 0.3 s, its end closed", n1, 5),
              f"the plugin's own copy of the room's socket: nothing after the end, its end closed (C2 of a review of fc43c40): {[l for l in plog()[n1:] if 'kept copy' in l]}")
        n2 = len(plog()); await asyncio.sleep(1.5)
        check(not any("room:" in l for l in plog()[n2:]) and not any("while no call is live" in l for l in plog()), "no room audio after the end")
        check(await until(lambda: line() == "idle"), "its entity: idle")

        # ---- 2. a call coming in: rings, never live before a person answered; the button answers, the plugin ends it
        n0, l0, u0 = len(plog()), len(text()), len(ui())
        await act("ring", who="Mum")
        check(await says("ring: 0 (0)", n0) and await until(lambda: "peer Test phone: Mum\n" in ui()[u0:]) and await until(lambda: "p+ call_incoming" in ui()[u0:]),
              "CALL_INCOMING: it rings, the screen names the plugin beside the caller, the incoming light")
        check(await logged("until stopped, peak 8000", l0, 3), "the ringtone plays (the stand-in's, looped until answered)")
        check((fp.plug(b, "testcall") or {}).get("call") == "ringing", "its card: ringing")
        await act("try", what="live_early")
        check(await says("try live_early: -1 (2)", n0), "CALL_LIVE before anyone answered: DENIED")
        await act("try", what="ring_twice")
        check(await says("try ring_twice: -1 (4)", n0), "a second ring while one rings: BUSY")
        await asyncio.sleep(1)
        check(not any("room" in l for l in plog()[n0:]), "no room audio while it rings")
        # (its far side takes 1.5 s after the answer: the ringtone stops at the press all the same, C4 of a review of fc43c40)
        await act("try", what="slow"); await says("try slow: 1", n0)
        l1, u1 = len(text()), len(ui()); proc.send_signal(signal.SIGUSR2)
        check(await says("answered here", n0), "the action button answers: CALL_ANSWER")
        check(await until(lambda: "p- call_incoming" in ui()[u1:] and "p+ call_connected" in ui()[u1:], 1) and not any("live: " in l for l in plog()[n0:]),
              f"... the ring's light and the screen leave ringing at the press, before the far side is in: {re.findall(r'(?:p[+-]) call[^\n]*', ui()[u1:])}")
        check((fp.plug(b, "testcall") or {}).get("call") == "answered", "its card: answered here, connecting")
        # the cut, as the earcon thread logs it, and the far side's CALL_LIVE (1.5 s after the answer), each waited for:
        # the cut comes before it is live, and no ringtone starts between the two.  It was looked at once after 0.8 s,
        # which a loaded runner's earcon thread did not always make with its line (CI, 2026-11-24)
        await until(lambda: "earcon: cut at" in text()[l1:], 5); await says("live: ", n0, 5)
        seg = text()[l1:]; cut, live = seg.find("earcon: cut at"), seg.find("plugin testcall: live: ")
        check(cut >= 0 and live > cut and "until stopped" not in seg[cut:live],
              f"... and the ringtone is cut at the press, not played again while it connects ({cut}, {live})")
        check(await says("live: 0 (0)", n0, 4) and await logged("testcall: the call is live (answered here)", l0), "then live")
        await act("try", what="slow")
        check(await says("room: 50 frames", n0, 5), "the room reaches it once answered")
        n1 = len(plog()); await act("hang_up")
        check(await says("hang up: 0", n0) and await logged("testcall: call ended after", l0) and await until(lambda: "p+ call_connected_end" in ui()[u0:]),
              "the plugin ends it (CALL_END): the end's light")
        check(await says("kept copy after CALL_END: 0 frames later than 0.3 s, its end closed", n1, 5),
              f"... its own copy of the room's socket: nothing after its CALL_END, its end closed: {[l for l in plog()[n1:] if 'kept copy' in l]}")

        # ---- 3. answered on the screen, ended by "<wake word>, stop"
        n0, l0 = len(plog()), len(text())
        await act("ring", who="Ann"); check(await says("ring: 0 (0)", n0), "rings again")
        ui_say("dropin answer")
        check(await says("live: 0 (0)", n0), "the screen's Answer answers it")
        proc.send_signal(signal.SIGHUP)                          # "<wake word>, stop"
        check(await says("hung up by hassmic: 2", n0), f"\"<wake word>, stop\" ends it (spoken): {plog()[n0:]}")

        # ---- 4. Home Assistant's Answer (an action marked calls: a token) refused, the button answers; ended by "hang
        # up" said in a request.  (C3 of a review of fc43c40: only a person at the Echo answers a call coming in)
        n0, l0 = len(plog()), len(text())
        await act("ring", who="Max"); check(await says("ring: 0 (0)", n0), "rings again")
        await act("answer")
        check(await says("action answer from 1, with a token", n0) and await says("live: -1 (2)", n0),
              f"Home Assistant's Answer (marked calls, its token): CALL_LIVE DENIED, no token answers a call coming in: {plog()[n0:]}")
        await asyncio.sleep(0.5)
        check((fp.plug(b, "testcall") or {}).get("call") == "ringing" and not any("room" in l for l in plog()[n0:]), "... it rings on, nothing of the room")
        proc.send_signal(signal.SIGUSR2)
        check(await says("live: 0 (0)", n0), "the action button answers it")
        # (the far side silent: while it talks the wake word is left to the Echo over there, as in a Drop In)
        await act("try", what="quiet"); await says("try quiet: 1", n0); await asyncio.sleep(1.5)
        k0 = len(starts); proc.send_signal(signal.SIGUSR1)
        check(await until(lambda: len(starts) > k0, 3), "a request starts during the call")
        ha.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        ha.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        ha.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "Hang up."})
        check(await says("hung up by hassmic: 2", n0), "\"hang up\" said in a request ends it")
        await act("try", what="quiet")

        # ---- 5. the card's Answer (the page's button for an action marked calls) refused, the screen answers; ended on
        # the screen
        n0 = len(plog())
        await act("ring", who="Eve"); check(await says("ring: 0 (0)", n0), "rings again")
        st_, r_ = fp.post(b, "/api/plugins/testcall/action", "answer")
        check(st_ == 200 and await says("action answer from 2, with a token", n0) and await says("live: -1 (2)", n0), f"the card's Answer (an approved browser, its token): DENIED ({st_} {r_})")
        ui_say("dropin answer")
        check(await says("live: 0 (0)", n0), "the screen's Answer answers it")
        ui_say("dropin hangup")
        check(await says("hung up by hassmic: 3", n0), "the screen's End ends it")

        # ---- 6. busy the other way: Drop In has the call
        bb.call("POST", "/api/dropin", b"echo-plug")
        check(await until(lambda: bb_dropin().get("state") == "connected", 6), f"a Drop In between the two Echos: {bb_dropin()}")
        n0 = len(plog())
        await act("ring", who="Zed"); await act("call", who="Zed")
        check(await says("ring: -1 (4)", n0) and await says("call out: -1 (4)", n0), f"a ring and a call out while Drop In has it: BUSY: {plog()[n0:]}")
        bb.call("POST", "/api/dropin", b"end")
        check(await until(lambda: bb_dropin().get("state") == "idle", 6), "the Drop In ends")

        # ---- 6b. a Drop In ends here and the plugin rings or calls at once, no wait between (C1 of a review of fc43c40:
        # the claim went with the end, the core heard "idle" up to 20 ms later and took that from the plugin's call: a
        # ring nobody saw or heard, which the next press answered).  Refused busy, or rung and shown; never the Echo's
        # "drop in: idle" after the plugin's call began
        for i, kind in enumerate(("ring", "call", "ring", "call", "ring")):
            bb.call("POST", "/api/dropin", b"echo-plug")
            if not await until(lambda: bb_dropin().get("state") == "connected" and (b.state().get("dropin") or {}).get("state") == "connected", 8):
                check(False, f"a Drop In to end ({bb_dropin()})"); continue
            await asyncio.sleep(0.3)
            n0, l0, u0 = len(plog()), len(text()), len(ui())
            b.call("POST", "/api/dropin", b"end")
            await act(kind, who=f"Quick{i}")
            word = "ring: " if kind == "ring" else "call out: "
            await until(lambda: any(l.startswith(word) for l in plog()[n0:]), 3)
            res = [l for l in plog()[n0:] if l.startswith(word)]
            if kind == "call" and res and res[0].startswith(word + "0"): await says("live: 0 (0)", n0, 3)
            await asyncio.sleep(0.6)
            log_after = text()[l0:]
            began = log_after.find("testcall: a call ")
            ev = re.findall(r'p[+-] call[^\n]*', ui()[u0:])
            want = "call_incoming" if kind == "ring" else "call_connected"
            shown = [e for e in ev if e.split()[1] == want]
            if res and res[0].startswith(word + "0"):
                check(began >= 0 and "drop in: idle" not in log_after[began:] and shown and shown[-1].startswith("p+"),
                      f"{kind} right after the Drop In's end: the plugin's call is shown ({res[0]}; {ev})")
            else: check(res and res[0].startswith(word + "-1 (4)"), f"{kind} right after the Drop In's end: busy ({res})")
            if kind == "ring" and res and res[0].startswith(word + "0"):     # (answered: an unanswered ring would count, 3.6)
                ui_say("dropin answer"); await says("live: 0 (0)", n0, 3)
            await act("hang_up"); await says("hang up: 0", n0, 3)
            await asyncio.sleep(1.2)                            # (BUSY_PER_SEC: the next try's look at the line)

        # ---- 6d. core_call's own guard (a state from a call without the claim is not shown), alone: Drop In's end in the
        # order before C1's fix (HASSMIC_DROPIN_LATE_IDLE: the claim let go at once, the core told "idle" 800 ms later),
        # a ring of the plugin's in between: the late "idle" is not shown, the ring stays (its light, the screen)
        open(late_idle, "w").close()
        bb.call("POST", "/api/dropin", b"echo-plug")
        if await until(lambda: bb_dropin().get("state") == "connected" and (b.state().get("dropin") or {}).get("state") == "connected", 8):
            await asyncio.sleep(0.3)
            n0, l0, u0 = len(plog()), len(text()), len(ui())
            b.call("POST", "/api/dropin", b"end")
            await act("ring", who="Guard")
            rang = await says("ring: 0 (0)", n0, 3)
            await logged("a call that has not the claim: not shown", l0, 3)
            await asyncio.sleep(0.3)
            log_after = text()[l0:]; began = log_after.find("testcall: a call ")
            ev = re.findall(r'p[+-] call[^\n]*', ui()[u0:]); rings = [e for e in ev if e.split()[1] == "call_incoming"]
            check(rang and began >= 0 and "call: a state (idle) from a call that has not the claim: not shown" in log_after[began:]
                  and "drop in: idle" not in log_after[began:] and rings and rings[-1].startswith("p+"),
                  f"Drop In's \"idle\" told after a plugin's ring took the line: not shown, the ring stays ({ev})")
            if rang: ui_say("dropin answer"); await says("live: 0 (0)", n0, 3)
            await act("hang_up"); await says("hang up: 0", n0, 3)
            await asyncio.sleep(1.2)
        else: check(False, f"a Drop In to end ({bb_dropin()})")
        os.unlink(late_idle)

        # ---- 6c. the screen's peer cut to its 63 bytes at a character's edge (C6 of a review of fc43c40: "Test phone: "
        # and 32 two-byte characters is 76 bytes, cut at 63 inside the 26th)
        n0, u0 = len(plog()), len(ui())
        await act("ring", who="\u00f6" * 32)
        check(await says("ring: 0 (0)", n0), "a caller named by 32 two-byte characters rings")
        raw = lambda: open(ui_log, "rb").read()[u0:]
        await until(lambda: b"peer Test phone: " in raw())
        pl = re.search(rb"peer (Test phone: [^\n]*)\n", raw())
        try: peer = pl.group(1).decode("utf-8") if pl else None
        except UnicodeDecodeError: peer = "(not UTF-8)"
        check(peer == "Test phone: " + "\u00f6" * 25, f"the screen shows it cut at a character's edge, whole characters only: {pl.group(1) if pl else None}")
        ui_say("dropin answer"); await says("live: 0 (0)", n0, 3)
        await act("hang_up"); await says("hang up: 0", n0, 3)

        # ---- 7. do not disturb, microphones off
        await asyncio.sleep(0.5)
        n0 = len(plog()); b.set(do_not_disturb="on")
        await act("ring", who="A")
        check(await says("ring: -1 (2)", n0), "do not disturb: refused (DENIED)")
        b.set(do_not_disturb="off"); ha.switch_command(by["mute"].key, True); await asyncio.sleep(0.5)
        n1 = len(plog()); await act("ring", who="A"); await act("call", who="A")
        check(await says("ring: -1 (2)", n1) and await says("call out: -1 (2)", n1), "microphones off: a ring and a call out refused (DENIED)")
        ha.switch_command(by["mute"].key, False); await asyncio.sleep(0.5)

        # ---- 8. the plugin's own refusals
        n0 = len(plog())
        for what, want in (("out_bare", "-1 (2)"), ("out_made_up", "-1 (2)"), ("out_again", "-1 (2)"), ("there_none", "0 (0)"), ("end_none", "-1 (0)"), ("live_none", "-1 (3)")):
            await act("try", what=what)
            check(await says(f"try {what}: {want}", n0), f"{what}: {want}")
        await act("try", what="keep"); await act("call", who="Late")
        check(await says("token kept", n0), "(a token kept)")
        await asyncio.sleep(11)
        await act("try", what="out_old")
        check(await says("try out_old: -1 (2)", n0), "a token older than 10 s: DENIED")

        # ---- 9. ring limits: three unanswered (each rings its 2 s), the fourth refused, after the window again
        n0 = len(plog())
        for i in range(2):                                      # the caller gives up (CALL_END while it rings)
            n1 = len(plog()); await act("ring", who=f"R{i}"); check(await says("ring: 0 (0)", n1), f"ring {i + 1}")
            if not i: t0 = time.monotonic()
            await act("hang_up"); check(await says("hang up: 0", n1), f"ring {i + 1}: the caller gave up")
        n1 = len(plog()); await act("ring", who="R2")
        check(await says("ring: 0 (0)", n1) and await says("hung up by hassmic: 4", n1, 9), "ring 3: not answered, ended after its time (NOT_ANSWERED)")
        await act("ring", who="R3")
        check(await says("ring: -1 (5)", n0), "the fourth with three unanswered in the window: refused (LIMIT)")
        await asyncio.sleep(max(0, 15.5 - (time.monotonic() - t0)))
        n1 = len(plog()); await act("ring", who="R4")
        check(await says("ring: 0 (0)", n1), "after the window: it rings again")
        await act("hang_up"); await says("hang up: 0", n1)

        # ---- 10. the plugin crashes mid-call: the call ends cleanly, the claim goes, the next call works
        n0, l0, u0 = len(plog()), len(text()), len(ui())
        await act("call", who="Bob"); check(await says("live: 0 (0)", n0), "a call live again")
        await act("try", what="crash")
        check(await logged("testcall: call ended after", l0) and await logged("(the plugin ended)", l0), "the plugin gone mid-call: the call ended with it")
        check(await until(lambda: "p+ call_connected_end" in ui()[u0:]), "the screen and the light: ended")
        check(await says("ready", n0, 15), "the plugin started again")
        st_, _, data = b.call("POST", "/api/dropin", b"echo-peer")
        check(st_ == 200 and await until(lambda: (b.state().get("dropin") or {}).get("state") == "connected", 6), f"Drop In from the plugin's Echo works: the claim was let go ({st_} {data})")
        b.call("POST", "/api/dropin", b"end"); await until(lambda: (b.state().get("dropin") or {}).get("state") == "idle", 6)
        await asyncio.sleep(0.5)
        n1 = len(plog()); await act("ring", who="After"); check(await says("ring: 0 (0)", n1), "and a ring of the plugin's")
        await act("hang_up")
        check(not any("while no call is live" in l for l in plog()), "never any room audio outside a live call")
    finally:
        if ha:
            try: await ha.disconnect()
            except Exception: pass
        for p in (proc, bproc):
            p.send_signal(signal.SIGTERM)
            try: p.wait(10)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
        subprocess.run(["pkill", "-f", f"fake_screen.py --ui {top}"])
    if check.failed:
        print(text()[-6000:])
    print("FAILED" if check.failed else "all good")
    return 1 if check.failed else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
