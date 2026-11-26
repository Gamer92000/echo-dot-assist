#!/usr/bin/env python3
"""Drop In between two Echos (build/hassmic-host x2) of one arbitration network, under a fake Home Assistant, plus a third
Echo outside the network.  The two start with the same network key in state/arbitration, as after a join.
The kitchen Echo's microphone is speech (testdata), the hall's is silence: the call carries the kitchen's voice to the
hall's Voip stream (HASSMIC_VOIP) and nothing back.  Then: answer by button (drop_in_answer=ask), by "<wake word>,
answer" (only with answer_by_voice: off, the words go on to Home Assistant), hang up by button,
by "<wake word>, stop", by "auflegen", from Home Assistant, from the settings page; refusals (do not disturb, Drop In
off, an Echo outside the network, a name nobody has); an Echo in an empty room, whose noise must stay there; and an
Echo that vanishes mid-call.  For each call the kitchen
makes: every state on the way ("calling" before a refusal that comes at once) and the "Drop In result" event.
Everything goes out on loopback (HASSMIC_ARB_ADDR): nothing of it reaches the LAN."""
import asyncio, base64, os, re, signal, struct, subprocess, sys, tempfile, time, wave
import numpy as np
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import wait_port, Browser
ARB, BCAST = 28994, "127.255.255.255"
SPEECH = f"{ROOT}/testdata/alexa_espeak.raw"
# a stand-in for stock's incoming ringtone (the PC has none, and plays no ring without it): 1 s of 440 Hz at 8000
EARCONS = tempfile.mkdtemp()
with wave.open(os.path.join(EARCONS, "comms_call_incoming_ringtone.wav"), "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes((8000 * np.sin(2 * np.pi * 440 * np.arange(48000) / 48000)).astype("<i2").tobytes())
# an empty room in micAsr, as a traced call had it (dropin.c NEAR_MIN_DB): a floor of -74 dBFS and a 150 ms sound of -64
# every 0.7 s.  Each sound is 10 dB over the floor, and the gate once opened for it and sent it with the speech gain
QUIET = os.path.join(EARCONS, "quiet_room.raw")
_rng = np.random.default_rng(1)
_x = _rng.standard_normal(16000 * 7) * 32768 * 10 ** (-74 / 20)
for _i in range(0, len(_x), 11200): _x[_i:_i + 2400] *= 10 ** (10 / 20)
_x.astype("<i2").tofile(QUIET)
# the call pipeline's microphone (audio_file.c HASSMIC_CALL_CAP, the Dots' micHfp in call mode): a 700 Hz tone at
# -20 dBFS, as Amazon's AGC leaves a talker; the hall has it, switched off until section 7b
CALL_CAP = os.path.join(EARCONS, "callmic.raw")
(32768 * 10 ** (-20 / 20) * np.sqrt(2) * np.sin(2 * np.pi * 700 * np.arange(32000) / 16000)).astype("<i2").tofile(CALL_CAP)


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


class Echo:
    def __init__(self, name, port, net=None, speech=False, config="", cap=None, call_cap=None):
        self.name, self.port, self.state = name, port, tempfile.mkdtemp()
        self.web, self.audio = port + 10, port + 20
        self.node = re.sub(r"[^a-z0-9]", "-", name.lower())
        self.log = os.path.join(self.state, "log")
        self.voip = os.path.join(self.state, "voip.raw")
        self.api_key = base64.b64encode(os.urandom(32)).decode()
        with open(os.path.join(self.state, "api_key"), "w") as f: f.write(self.api_key + "\n")
        with open(os.path.join(self.state, "config"), "w") as f: f.write("even_volume=off\n" + config)   # full scale: the calls' audio compared as sent
        self.keys = os.path.join(self.state, "keypad"); os.mkfifo(self.keys)
        self.keypad = os.open(self.keys, os.O_RDWR)    # opened first: hassmic's open() would wait for a writer
        if net:                                         # a member already: the network's id and key
            with open(os.path.join(self.state, "arbitration"), "w") as f: f.write(f"arbitrate 1\nctr 0\nnet {net}\n")
        self.env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_PLAY=os.path.join(self.state, "play.raw"), HASSMIC_VOIP=self.voip,
                        HASSMIC_MDNS_FILE=os.path.join(self.state, "none"), HASSMIC_ARB_ADDR=BCAST, HASSMIC_TEST_SCORE="500",
                        HASSMIC_MODELS=os.path.join(self.state, "models"), HASSMIC_EARCONS=EARCONS + "/",
                        HASSMIC_DROPIN_TIMES="3000 4000")       # no answer after 3 s, lost after 4 s without audio (4 and 5): near
                                                                # the Echo's, so a stall of the PC under load does not end a call
        if speech or cap: self.env["HASSMIC_CAP"] = cap or SPEECH
        if call_cap: self.env["HASSMIC_CALL_CAP"] = call_cap
        self.stall = os.path.join(self.state, "callmic.stall")  # exists: the call microphone stalls (audio_file.c)
        self.env["HASSMIC_CALL_CAP_STALL"] = self.stall
        self.voip_fail = os.path.join(self.state, "voip.fail")  # exists: the Voip stream fails (audio_file.c)
        self.env["HASSMIC_VOIP_FAIL"] = self.voip_fail
        self.env["HASSMIC_FAKE_LISTEN"] = "1"                   # "front end: listening 0|1" in the log
        self.trace = os.path.join(self.state, "dropin.trace")
        self.env["HASSMIC_DROPIN_TRACE"] = self.trace
        self.states, self.starts, self.seq = {}, [], []     # seq: (object id, state or event type) as they came
        self.stops = []                                     # abort per run the Echo ended (True: start=false alone)

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.port), "-n", self.name, "-L",
                                      "-z", "0", "-o", "0", "-a", str(ARB), "-i", str(self.audio), "-W", str(self.web), "-b", self.keys],
                                     env=self.env, stderr=open(self.log, "a"))
        wait_port(self.port, self.web)

    def text(self): return open(self.log).read()
    def st(self, oid): return self.states.get(self.by[oid].key) if oid in self.by else None
    def dropin(self): return self.st("drop_in"), self.st("drop_in_with")
    def page_dropin(self): return (self.page.state().get("dropin") or {}).get("state")
    def results(self): return [v for o, v in self.seq if o == "drop_in_result"]
    def states_of(self, oid): return [v for o, v in self.seq if o == oid]

    def on_state(self, s):
        v = getattr(s, "state", None) if hasattr(s, "state") else getattr(s, "event_type", None)
        self.states[s.key] = v
        oid = next((o for o, e in self.by.items() if e.key == s.key), None)
        self.seq.append((oid, v))

    async def connect(self):
        self.states = {}
        self.cli = APIClient("127.0.0.1", self.port, None, noise_psk=self.api_key)
        await self.cli.connect(login=True)
        ents, self.services = await self.cli.list_entities_services()
        self.by = {e.object_id: e for e in ents}
        self.cli.subscribe_states(self.on_state)
        async def handle_start(conv, flags, settings, phrase): self.starts.append(time.monotonic()); return 0
        async def handle_stop(abort): self.stops.append(abort)
        async def handle_audio(data, *_): pass
        self.cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio)

    async def reconnect(self):
        try: await self.cli.disconnect()
        except Exception: pass
        await asyncio.sleep(0.5); await self.connect()

    async def call(self, target):
        svc = next(s for s in self.services if s.name == "drop_in")
        await self.cli.execute_service(svc, {"target": target})

    def press(self, oid): self.cli.button_command(self.by[oid].key)
    def key(self, code):
        for v in (1, 0): os.write(self.keypad, struct.pack("llHHi", 0, 0, 1, code, v))     # struct input_event, EV_KEY


def level(path, secs):
    """dBFS of the last secs of a raw 16 kHz file"""
    try: x = np.fromfile(path, np.int16).astype(float)[-16000 * secs:]
    except OSError: return -200
    return 10 * np.log10((x ** 2).mean() / 32768 ** 2 + 1e-20) if len(x) else -200


def tone(path, secs, hz=700):
    """dBFS of the HZ tone in the last secs of a raw 16 kHz file: each 20 ms frame's own (700 Hz: 14 whole periods)
    and their powers averaged.  Summed over the whole window as one, a frame the far end's jitter buffer lost or
    concealed moves the tone's phase from there on and the sum cancels: the hall's tone measured 1.3 dB under the
    level it was all of (-22.7 dB of -21.4 dBFS) once frames were lost under tests/run.py -j 8, and a few such frames
    cost 6 dB and more.  Per frame a jump costs that frame at most; a tone at another level, or noise beside it, still
    shows (level() then above it)"""
    try: x = np.fromfile(path, np.int16).astype(float)[-16000 * secs:]
    except OSError: return -200
    f = x[:len(x) // 320 * 320].reshape(-1, 320)
    if not len(f): return -200
    a = np.abs((f * np.exp(-2j * np.pi * hz * np.arange(320) / 16000)).sum(1)) * 2 / 320 / np.sqrt(2) / 32768
    return 10 * np.log10((a ** 2).mean() + 1e-24)


def envelope_match(path, ref_path):
    """how well the 20 ms energy envelope of what played matches the speech that was sent (best lag): Opus and the gain
    change the waveform, not the shape of the words"""
    x = np.fromfile(path, np.int16).astype(float); r = np.fromfile(ref_path, np.int16).astype(float)
    env = lambda a: np.log10((a[:len(a) // 320 * 320].reshape(-1, 320) ** 2).mean(1) + 1)
    ex, er = env(x[-16000 * 8:]), env(r)
    er = np.concatenate([er, er])                       # the capture loops
    best = 0
    for lag in range(0, len(er) - len(ex) if len(er) > len(ex) else 1):
        seg = er[lag:lag + len(ex)]
        if len(seg) == len(ex) and seg.std() > 0 and ex.std() > 0: best = max(best, np.corrcoef(seg, ex)[0, 1])
    return best


async def until(cond, secs):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if cond(): return True
        await asyncio.sleep(0.1)
    return False


async def main():
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    # the hall with separate volumes, sounds at 20 under the music's 40 (its ringtone is one of its sounds), calls at
    # their own 40
    a, b = Echo("Kitchen Echo", 26180, net, speech=True), Echo("Hall Echo", 26181, net, call_cap=CALL_CAP,
                                                               config="separate_volumes=on\nsounds_volume=20\ncall_volume_linked=off\ncall_volume=40\ncall_mic=assistant\n")
    c = Echo("Garage Echo", 26182)                      # outside the network: it hears the two, but nobody lets it in
    d = Echo("Bedroom Echo", 26183, net, cap=QUIET)     # an empty room
    for e in (a, b, c, d): e.start()                       # (it runs from the start: time to join, if it wrongly could)
    try:
        await asyncio.sleep(1.5)
        for e in (a, b): await e.connect()
        for e in (a, b): e.page = Browser(e.web); check(e.page.login_with_button(e.proc), f"{e.name}: settings page logged in")
        check(await until(lambda: any(m["node"] == b.node for m in a.page.state()["arbitration"]["members"]), 5), "the two Echos see each other in the network")
        check(any(s.name == "drop_in" for s in a.services), "Home Assistant has the action drop_in")
        check(all(o in a.by for o in ("drop_in", "drop_in_with", "end_drop_in", "drop_in_result")), "and Drop In, Drop In with, End Drop In, Drop In result")
        types = list(getattr(a.by.get("drop_in_result"), "event_types", []))
        check(types[:2] == ["connected", "ringing"] and {"do_not_disturb", "no_answer", "not_found", "busy_here"} <= set(types),
              f"  Drop In result's event types: {types}")
        check(await until(lambda: a.dropin() == ("idle", ""), 5), f"idle at start ({a.dropin()})")    # the states follow the connect

        # 1. a call from Home Assistant, by the hall's name: connected at once (answer: auto)
        mark = len(a.seq)
        await a.call(b.name)
        ok = await until(lambda: a.dropin() == ("connected", b.node) and b.dropin() == ("connected", a.node), 3)
        check(ok, f"the kitchen drops in on the hall: both connected ({a.dropin()}, {b.dropin()})")
        check(await until(lambda: [v for o, v in a.seq[mark:] if o in ("drop_in", "drop_in_result")] == ["calling", "connected", "connected"], 1),
              f"  Home Assistant hears calling, connected, then the result: {a.seq[mark:]}")
        await asyncio.sleep(4)
        lb, la = level(b.voip, 2), level(a.voip, 2)
        check(lb > -45, f"the hall plays the kitchen's voice ({lb:.1f} dBFS)")
        check(la < -70, f"the kitchen plays the hall's silence ({la:.1f} dBFS)")
        m = envelope_match(b.voip, SPEECH)
        check(m > 0.6, f"what the hall plays is the kitchen's speech (envelope match {m:.2f})")
        check("call_volume" in b.by and "call_volume" not in a.by, "Call volume in Home Assistant where calls have their own (the hall), not where they follow the music")
        b.key(114)                                      # volume down during the call: the call's own volume
        check(await until(lambda: "volume: calls 30" in b.text() and "audio: call gain 0.631" in b.text(), 2),
              "volume down during a call moves the call volume (40 -> 30: -4 dB on the curve, the call's gain 0.631)")
        check(re.search(r"drop in: connected with " + b.node, a.text()) is not None, "the kitchen's log says connected")
        b.press("end_drop_in")
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 2), "End Drop In in Home Assistant hangs up both")
        check(re.search(r"drop in: ended with " + b.node + r" after \d+ s \(" + b.node + " hung up", a.text()) is not None, "the kitchen's log says who hung up")

        # 2. answer: ask.  The hall rings until its action button
        r = b.page.set(drop_in_answer="ask"); check(r["applied"] == 1, "the hall: drop_in_answer=ask on the page")
        mark = len(a.seq)
        await a.call(b.node)
        check(await until(lambda: b.dropin() == ("ringing", a.node) and a.dropin()[0] == "calling", 2), f"the hall rings, the kitchen waits ({b.dropin()}, {a.dropin()})")
        check(await until(lambda: a.results()[-1:] == ["ringing"], 1), f"  result: ringing ({a.results()[-1:]})")
        check(re.search(r"drop in: result ringing hall-echo\n", a.text()) is not None, "  (no end of call sound for it)")
        await asyncio.sleep(2)
        check(a.dropin()[0] == "calling", "still ringing 2 s later (not answered by itself)")
        rings = [int(x) for x in re.findall(r"until stopped, peak (\d+)", b.text())]
        want = 8000 * 10 ** ((-35.7 + 26.5) / 20)          # the curve's 20 against 40 (main.c vol_db)
        check(rings and abs(rings[-1] - want) <= 3, f"  the ring at the hall's sounds volume, not the music's ({rings[-1:]} of 8000, want {want:.0f})")
        b.proc.send_signal(signal.SIGUSR2)                 # the hall's action button
        check(await until(lambda: a.dropin()[0] == "connected" and b.dropin()[0] == "connected", 2), "the action button answers")
        check(await until(lambda: [v for o, v in a.seq[mark:] if o == "drop_in_result"] == ["ringing", "connected"], 1), f"  results: ringing, connected ({a.seq[mark:]})")
        a.proc.send_signal(signal.SIGHUP)                  # "<wake word>, stop" on the kitchen
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 2), "\"stop\" hangs up")

        # 3. declined with the button while it rings
        await a.call(b.node)
        check(await until(lambda: b.dropin()[0] == "ringing", 2), "ringing again")
        b.proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.3); b.proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 3), "answered and hung up again by button")
        # declined: End Drop In on the hall while it rings; cancelled: the kitchen gives up while it rings
        await asyncio.sleep(0.3); await a.call(b.node)
        check(await until(lambda: b.dropin()[0] == "ringing", 2), "ringing again")
        b.press("end_drop_in")
        check(await until(lambda: a.results()[-1:] == ["declined"] and a.dropin()[0] == "idle", 2), f"declined on the hall: result {a.results()[-1:]}")
        await asyncio.sleep(0.3); await a.call(b.node)
        check(await until(lambda: b.dropin()[0] == "ringing", 2), "ringing again")
        a.press("end_drop_in")
        check(await until(lambda: a.results()[-1:] == ["cancelled"] and b.dropin()[0] == "idle", 2), f"cancelled on the kitchen: result {a.results()[-1:]}, hall {b.dropin()}")
        # "<wake word>, answer" on the hall while it rings (issue #240): answer_by_voice off, it rings on and Home Assistant
        # gets the words; on, "Réponds !" in a run its own wake word began connects (the follow-up's refusal is
        # fake_plugins_call.py's: the same rule for both)
        def heard(e, t):
            for ev, d in ((Ev.VOICE_ASSISTANT_RUN_START, None), (Ev.VOICE_ASSISTANT_STT_START, None), (Ev.VOICE_ASSISTANT_STT_END, {"text": t})):
                e.cli.send_voice_assistant_event(ev, d)
        await asyncio.sleep(0.3); await a.call(b.node)
        check(await until(lambda: b.dropin()[0] == "ringing", 2), "ringing again")
        l0 = len(b.text()); n0 = len(b.starts); b.proc.send_signal(signal.SIGUSR1)
        check(await until(lambda: len(b.starts) > n0, 2), "the wake word on the hall while it rings: a pipeline starts")
        heard(b, "Answer.")
        check(await until(lambda: "said answer: the call not answered: answering by voice is off" in b.text()[l0:], 2) and b.dropin()[0] == "ringing",
              "answer_by_voice off: \"answer\" answers nothing, it rings on")
        b.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
        a.press("end_drop_in")
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 2), "(the kitchen gives up)")
        b.page.set(answer_by_voice="on")
        await asyncio.sleep(0.3); await a.call(b.node)
        check(await until(lambda: b.dropin()[0] == "ringing", 2), "ringing again")
        l0 = len(b.text()); n0 = len(b.starts); b.proc.send_signal(signal.SIGUSR1)
        check(await until(lambda: len(b.starts) > n0, 2), "(the wake word on the hall)")
        heard(b, "Réponds !")
        check(await until(lambda: a.dropin()[0] == "connected" and b.dropin()[0] == "connected", 2) and "said answer: the call answered" in b.text()[l0:],
              "answer_by_voice on: \"<wake word>, réponds\" connects")
        b.proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 3), "hung up by button")
        b.page.set(answer_by_voice="off")
        b.page.set(drop_in_answer="auto")
        await asyncio.sleep(2)      # (the hall's wake word claimed a round: a wake word on the kitchen within it is the same word)

        # 4. "auflegen" in a request during the call ends it, and Home Assistant never gets the run
        await a.call(b.node)
        check(await until(lambda: a.dropin()[0] == "connected", 3), "connected again")
        n0 = len(a.starts); a.proc.send_signal(signal.SIGUSR1)       # the wake word on the kitchen
        check(await until(lambda: len(a.starts) > n0, 3), "a pipeline starts during the call")
        a.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        a.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        a.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "Auflegen."})
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 2), "\"Auflegen\" hangs up")
        check("said hang up" in a.text(), "the log says why")
        # "Închide apelul": Romanian, capitalised with its mark (compared without marks and case)
        await asyncio.sleep(0.3); await a.call(b.node)
        check(await until(lambda: a.dropin()[0] == "connected", 3), "connected again")
        n0 = len(a.starts); a.proc.send_signal(signal.SIGUSR1)
        check(await until(lambda: len(a.starts) > n0, 3), "a pipeline starts during the call")
        a.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        a.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        a.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": "Închide apelul!"})
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 2), "\"Închide apelul\" hangs up")
        # "<wake word>, stop" in a call, the "stop" after the round: the call ends, and so does the run its wake word
        # opened, as after a ring (it listened on, with the "stop" in its replayed audio)
        await asyncio.sleep(0.3); await a.call(b.node)
        check(await until(lambda: a.dropin()[0] == "connected", 3), "connected again")
        n0, s0 = len(a.starts), len(a.stops); a.proc.send_signal(signal.SIGUSR1)
        check(await until(lambda: len(a.starts) > n0, 3), "a pipeline starts during the call")
        a.proc.send_signal(signal.SIGHUP)
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle" and a.stops[s0:] == [True], 2),
              f"\"<wake word>, stop\" in a call hangs up and cancels its run (stops {a.stops[s0:]})")

        # 5. from the settings page: on and off
        st, _, data = a.page.call("POST", "/api/dropin", b.node.encode())
        check(st == 200, f"the page drops in ({st} {data})")
        check(await until(lambda: a.page_dropin() == "connected", 3), "the page shows it connected")
        pd = a.page.state()["dropin"]
        check(pd.get("result") == "connected" and pd.get("attempt", 0) >= 7, f"  and the last call's result: {pd}")
        st, _, _ = b.page.call("POST", "/api/dropin", b"end")
        check(st == 200 and await until(lambda: a.page_dropin() == "idle", 2), "and the other Echo's page ends it")

        # 6. refusals
        b.cli.switch_command(b.by["do_not_disturb"].key, True)
        await asyncio.sleep(0.3)
        mark, t0 = len(a.seq), time.monotonic()
        await a.call(b.node)
        check(await until(lambda: "refused: do not disturb is on" in a.text(), 3), "do not disturb refuses it")
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 2), "both idle after the refusal")
        check(await until(lambda: a.results()[-1:] == ["do_not_disturb"], 1) and time.monotonic() - t0 < 1,
              f"  result do_not_disturb at once ({time.monotonic() - t0:.2f} s)")
        check([v for o, v in a.seq[mark:] if o == "drop_in"] == ["calling", "idle"],
              f"  Home Assistant saw calling before idle, though the refusal came within the loop's 20 ms: {a.seq[mark:]}")
        check(re.search(r"refused: do not disturb is on\n(.*\n)*.*drop in: idle", a.text()) is not None,
              "  and the core heard of both (the end of call sound goes with idle)")
        b.cli.switch_command(b.by["do_not_disturb"].key, False); await asyncio.sleep(0.3)
        b.page.set(drop_in="off"); await b.reconnect()
        check("drop_in" not in b.by and not any(s.name == "drop_in" for s in b.services), "Drop In off: its entities and action are gone from Home Assistant")
        await a.call(b.node)
        check(await until(lambda: "refused: Drop In is off there" in a.text(), 3), "an Echo with Drop In off refuses")
        check(await until(lambda: a.results()[-1:] == ["drop_in_off"], 1), f"  result drop_in_off ({a.results()[-1:]})")
        b.page.set(drop_in="on"); await b.reconnect()
        mark = len(a.seq)
        await a.call("Nobody Echo")
        check(await until(lambda: 'drop in: Nobody Echo: no Echo "nobody-echo" in this Echo\'s network' in a.text(), 2), "a name nobody has: refused at once")
        check(await until(lambda: a.results()[-1:] == ["not_found"], 1) and "drop_in" not in [o for o, _ in a.seq[mark:]],
              f"  result not_found, and the state never left idle: {a.seq[mark:]}")
        check("drop in: result not_found nobody-echo (refused here: end of call sound)" in a.text(), "  the Echo plays the end of call sound")
        a.cli.switch_command(a.by["mute"].key, True); await asyncio.sleep(0.3)
        await a.call(b.node)
        check(await until(lambda: a.results()[-1:] == ["microphones_off_here"], 2), f"the kitchen muted: result {a.results()[-1:]}")
        a.cli.switch_command(a.by["mute"].key, False); await asyncio.sleep(0.3)
        await a.call(c.node)
        check(await until(lambda: f'no Echo "{c.node}" in this Echo\'s network' in a.text(), 2), "an Echo outside the network cannot be called")
        check(await until(lambda: a.results()[-1:] == ["not_found"], 1), f"  result not_found ({a.results()[-1:]})")
        check(a.dropin()[0] == "idle", "and nothing started")

        # 7. an empty bedroom: its noise stays there.  The kitchen took it for the other side talking (its wake word
        # ignored, its listening mode kept off), and the bedroom switched listening mode on for nothing
        check(await until(lambda: any(m["node"] == d.node for m in a.page.state()["arbitration"]["members"]), 5), "the bedroom is in the network")
        await a.call(d.node)
        check(await until(lambda: a.dropin() == ("connected", d.node), 3), f"the kitchen drops in on the empty bedroom ({a.dropin()})")
        await asyncio.sleep(5)
        lq = level(a.voip, 3)
        check(lq < -66, f"the kitchen plays the bedroom's quiet, not its noise turned up ({lq:.1f} dBFS)")
        rows = [l.split() for l in open(d.trace) if len(l.split()) >= 10]
        loud = [r for r in rows if float(r[1]) > -70]
        check(len(loud) > 20 and not any(r[5] == "1" for r in rows),
              f"the bedroom's sounds ({len(loud)} frames over -70 dBFS) never count as someone talking there")
        a.press("end_drop_in")
        check(await until(lambda: a.dropin()[0] == "idle", 2), "hung up")
        await asyncio.sleep(0.3)

        # 7b. the call pipeline's microphone (call_mic, the Dots' micHfp): the hall sends it as it comes, without the
        # assistant's gain; back to the assistant's (its silence) on the page, at once, in the same call
        r = b.page.set(call_mic="call"); check(r.get("applied") == 1, f"the hall: call_mic=call on the page ({r})")
        await a.call(b.node)
        check(await until(lambda: a.dropin()[0] == "connected", 3), "connected")
        check(await until(lambda: "call: the call pipeline's microphone (micHfp)" in b.text(), 3), "  the hall's log: the call pipeline's microphone")
        # the kitchen's speech muted (its mic sends zeros, the call goes on): while the far end talks, the hall's echo
        # gate ducks it, as it should, and the level measured would depend on where the speech file is
        a.cli.switch_command(a.by["mute"].key, True)
        await asyncio.sleep(3.5)
        tdb = tone(a.voip, 2)
        check(-30 < tdb < -14 and tdb > level(a.voip, 2) - 1, f"  the kitchen plays the hall's 700 Hz at its own level ({tdb:.1f} dB of {level(a.voip, 2):.1f} dBFS; -20 sent)")
        b.page.set(call_mic="assistant")
        check(await until(lambda: "call: switched to the assistant's: the assistant's microphone for the rest of this call" in b.text(), 2),
              "  call_mic=assistant mid-call: the assistant's microphone from then on")
        await asyncio.sleep(2.5)
        check(level(a.voip, 1) < -60, f"  the kitchen plays the hall's silence again ({level(a.voip, 1):.1f} dBFS)")
        a.cli.switch_command(a.by["mute"].key, False)
        a.press("end_drop_in")
        check(await until(lambda: a.dropin()[0] == "idle", 2), "hung up")
        await asyncio.sleep(0.3)

        # 7c. the call microphone stalls mid-call (each read waits 1.5 s for nothing, as the mixer's does): the hall's
        # micAsr fills in at once, so the kitchen hears no gap that would end the call (lost after 4 s here).  A short
        # stall: micHfp back, and the listening mode its tone had switched on goes off while micAsr's silence fills in
        # (a swap of sources once left it on for good).  A long one: after three empty reads the hall gives micHfp up
        # for the rest of the call
        b.page.set(call_mic="call")
        mark = len(b.text())
        await a.call(b.node)
        check(await until(lambda: a.dropin()[0] == "connected", 3), "connected")
        a.cli.switch_command(a.by["mute"].key, True)    # the kitchen silent: the hall's tone is never taken for its echo
        check(await until(lambda: "call: the call pipeline's microphone (micHfp)" in b.text()[mark:], 3), "  the hall sends micHfp")
        check(await until(lambda: "front end: listening 1" in b.text()[mark:], 2), "  the hall's tone switches its listening mode on")
        await asyncio.sleep(0.5)
        smark = len(b.text())
        open(b.stall, "w").close()
        check(await until(lambda: "call: micHfp stalls, the assistant's microphone meanwhile" in b.text()[smark:], 3), "  a short stall: micAsr meanwhile")
        check(await until(lambda: "front end: listening 0" in b.text()[smark:], 2.5), "  micAsr's silence: listening mode off")
        os.remove(b.stall)                              # before the third empty read
        check(await until(lambda: "call: micHfp is back" in b.text()[smark:], 3), "  micHfp is back")
        check(await until(lambda: "front end: listening 1" in b.text()[smark:].split("call: micHfp is back")[-1], 2), "  and its tone switches listening mode on again")
        await asyncio.sleep(2.5)
        tdb = tone(a.voip, 2)
        check(-30 < tdb < -14 and tdb > level(a.voip, 2) - 1 and "delivers nothing" not in b.text()[smark:],
              f"  the kitchen plays the hall's 700 Hz again ({tdb:.1f} dB of {level(a.voip, 2):.1f} dBFS)")     # (the tone, not noise: a frame's 50 Hz bin)
        lmark = len(b.text())
        open(b.stall, "w").close()
        check(await until(lambda: "call: micHfp stalls, the assistant's microphone meanwhile" in b.text()[lmark:], 3), "  the hall's micHfp stalls: micAsr meanwhile")
        check(await until(lambda: "call: micHfp delivers nothing: the assistant's microphone for the rest of this call" in b.text()[lmark:], 7),
              "  three empty reads: micAsr for the rest of the call")
        await asyncio.sleep(1)
        os.remove(b.stall)
        check(a.dropin()[0] == "connected", f"  the call goes on ({a.dropin()})")
        amark = len(a.text())
        a.press("end_drop_in")
        check(await until(lambda: a.dropin()[0] == "idle", 2), "hung up")
        m = await until(lambda: "longest gap" in a.text()[amark:], 2) and re.search(r"longest gap (\d+) ms", a.text()[amark:])
        check(bool(m) and int(m.group(1)) < 800, f"  the kitchen's longest wait for the hall's audio: {m.group(1) if m else '?'} ms (a read alone waits 1500)")
        await asyncio.sleep(0.3)

        # 7d. the hall's Voip stream fails mid-call: call mode is off, so micHfp goes for the rest of the call.  A normal
        # hang-up afterwards says nothing of call mode
        a.cli.switch_command(a.by["mute"].key, False)   # muted, the kitchen could not call
        await asyncio.sleep(0.3)
        mark = len(b.text())
        await a.call(b.node)
        check(await until(lambda: a.dropin()[0] == "connected", 3), "connected")
        check(await until(lambda: "call: the call pipeline's microphone (micHfp)" in b.text()[mark:], 3), "  the hall sends micHfp")
        open(b.voip_fail, "w").close()
        check(await until(lambda: "call: the Voip stream failed" in b.text()[mark:], 2), "  the hall's Voip stream fails")
        check(await until(lambda: "call: call mode ended: the assistant's microphone for the rest of this call" in b.text()[mark:], 2),
              "  micHfp given up: the assistant's microphone")
        # the kitchen talks on (unmuted), but nothing plays at the hall any more: its echo suppressor hears no far end
        await asyncio.sleep(1)                          # what played before the failure: out of the echo window (600 ms)
        n0 = sum(1 for _ in open(b.trace))
        await asyncio.sleep(2)
        far = [float(l.split()[2]) for l in list(open(b.trace))[n0:] if len(l.split()) >= 10]
        check(len(far) > 50 and max(far) <= -99, f"  the hall's echo suppressor: no far end after the failure (loudest {max(far) if far else '?'} dB, {len(far)} frames)")
        os.remove(b.voip_fail)
        a.press("end_drop_in")
        check(await until(lambda: a.dropin()[0] == "idle", 2), "hung up")
        mark = len(b.text())
        await a.call(b.node)
        check(await until(lambda: a.dropin()[0] == "connected", 3), "connected")
        check(await until(lambda: "call: the call pipeline's microphone (micHfp)" in b.text()[mark:], 3), "  the next call sends micHfp again")
        a.press("end_drop_in")
        check(await until(lambda: a.dropin()[0] == "idle", 2), "hung up")
        await asyncio.sleep(1)
        check("call mode ended" not in b.text()[mark:], "  a normal hang-up: no word of call mode ending")
        b.page.set(call_mic="assistant")
        await asyncio.sleep(0.3)

        # 8. the hall vanishes mid-call: the kitchen notices
        await a.call(b.node)
        check(await until(lambda: a.dropin()[0] == "connected", 3), "connected once more")
        await a.call(b.node)
        check(await until(lambda: a.results()[-1:] == ["busy_here"], 1) and a.dropin()[0] == "connected", f"a second call during it: result {a.results()[-1:]}")
        b.proc.kill(); b.proc.wait()
        check(await until(lambda: a.dropin()[0] == "idle", 10), "a vanished Echo ends the call (no audio for 4 s here, 5 on the Echo)")
        check("lost: no audio for 4 s" in a.text(), "the log says it was lost")
        mark, t0 = len(a.seq), time.monotonic()
        await a.call(b.node)                           # still a member (5 min): nobody answers
        check(await until(lambda: a.results()[-1:] == ["no_answer"], 7) and 2.9 < time.monotonic() - t0 < 4.5,
              f"  called again: result no_answer after {time.monotonic() - t0:.1f} s, states {[v for o, v in a.seq[mark:] if o == 'drop_in']}")
        check(not re.search(r"drop in: send: ", a.text()), "no send errors on the way")
    finally:
        for e in (a, b, c, d):
            try: e.proc.terminate(); e.proc.wait(timeout=3)
            except Exception: pass
    if check.failed:
        for e in (a, b): print(f"--- {e.name} log (drop in, arbitration):\n" + "".join(l for l in open(e.log) if "drop in" in l or "arbitration" in l or "state:" in l))
    print("FAILED" if check.failed else "all passed")
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
