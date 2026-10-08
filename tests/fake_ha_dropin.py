#!/usr/bin/env python3
"""Drop In between two Echos (build/hassmic-host x2) of one arbitration network, under a fake Home Assistant, plus a third
Echo outside the network.  The two start with the same network key in state/arbitration, as after a join.
The kitchen Echo's microphone is speech (testdata), the hall's is silence: the call carries the kitchen's voice to the
hall's Voip stream (HASSMIC_VOIP) and nothing back.  Then: answer by button (drop_in_answer=ask), hang up by button,
by "<wake word>, stop", by "auflegen", from Home Assistant, from the settings page; refusals (do not disturb, Drop In
off, an Echo outside the network, a name nobody has); and an Echo that vanishes mid-call.
Everything goes out on loopback (HASSMIC_ARB_ADDR): nothing of it reaches the LAN."""
import asyncio, base64, os, re, signal, subprocess, sys, tempfile, time
import numpy as np
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import Browser
ARB, BCAST = 28994, "127.255.255.255"
SPEECH = f"{ROOT}/testdata/alexa_espeak.raw"


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


class Echo:
    def __init__(self, name, port, net=None, speech=False):
        self.name, self.port, self.state = name, port, tempfile.mkdtemp()
        self.web, self.audio = port + 10, port + 20
        self.node = re.sub(r"[^a-z0-9]", "-", name.lower())
        self.log = os.path.join(self.state, "log")
        self.voip = os.path.join(self.state, "voip.raw")
        self.api_key = base64.b64encode(os.urandom(32)).decode()
        with open(os.path.join(self.state, "api_key"), "w") as f: f.write(self.api_key + "\n")
        if net:                                         # a member already: the network's id and key
            with open(os.path.join(self.state, "arbitration"), "w") as f: f.write(f"arbitrate 1\nctr 0\nnet {net}\n")
        self.env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_PLAY=os.path.join(self.state, "play.raw"), HASSMIC_VOIP=self.voip,
                        HASSMIC_MDNS_FILE=os.path.join(self.state, "none"), HASSMIC_ARB_ADDR=BCAST, HASSMIC_TEST_SCORE="500",
                        HASSMIC_MODELS=os.path.join(self.state, "models"))
        if speech: self.env["HASSMIC_CAP"] = SPEECH
        self.states, self.starts = {}, []

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.port), "-n", self.name, "-L",
                                      "-z", "0", "-o", "0", "-a", str(ARB), "-i", str(self.audio), "-W", str(self.web)],
                                     env=self.env, stderr=open(self.log, "a"))

    def text(self): return open(self.log).read()
    def st(self, oid): return self.states.get(self.by[oid].key) if oid in self.by else None
    def dropin(self): return self.st("drop_in"), self.st("drop_in_with")
    def page_dropin(self): return (self.page.state().get("dropin") or {}).get("state")

    async def connect(self):
        self.states = {}
        self.cli = APIClient("127.0.0.1", self.port, None, noise_psk=self.api_key)
        await self.cli.connect(login=True)
        ents, self.services = await self.cli.list_entities_services()
        self.by = {e.object_id: e for e in ents}
        self.cli.subscribe_states(lambda s: self.states.__setitem__(s.key, getattr(s, "state", None)))
        async def handle_start(conv, flags, settings, phrase): self.starts.append(time.monotonic()); return 0
        async def handle_stop(abort): pass
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


def level(path, secs):
    """dBFS of the last secs of a raw 16 kHz file"""
    try: x = np.fromfile(path, np.int16).astype(float)[-16000 * secs:]
    except OSError: return -200
    return 10 * np.log10((x ** 2).mean() / 32768 ** 2 + 1e-20) if len(x) else -200


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
    a, b = Echo("Kitchen Echo", 26180, net, speech=True), Echo("Hall Echo", 26181, net)
    c = Echo("Garage Echo", 26182)                      # outside the network: it starts one of its own... unless it joins
    for e in (a, b): e.start()
    try:
        await asyncio.sleep(1.5)
        for e in (a, b): await e.connect()
        for e in (a, b): e.page = Browser(e.web); check(e.page.login_with_button(e.proc), f"{e.name}: settings page logged in")
        check(await until(lambda: any(m["node"] == b.node for m in a.page.state()["arbitration"]["members"]), 5), "the two Echos see each other in the network")
        check(any(s.name == "drop_in" for s in a.services), "Home Assistant has the action drop_in")
        check(all(o in a.by for o in ("drop_in", "drop_in_with", "end_drop_in")), "and Drop In, Drop In with, End Drop In")
        check(a.dropin() == ("idle", ""), f"idle at start ({a.dropin()})")

        # 1. a call from Home Assistant, by the hall's name: connected at once (answer: auto)
        await a.call(b.name)
        ok = await until(lambda: a.dropin() == ("connected", b.node) and b.dropin() == ("connected", a.node), 3)
        check(ok, f"the kitchen drops in on the hall: both connected ({a.dropin()}, {b.dropin()})")
        await asyncio.sleep(4)
        lb, la = level(b.voip, 2), level(a.voip, 2)
        check(lb > -45, f"the hall plays the kitchen's voice ({lb:.1f} dBFS)")
        check(la < -70, f"the kitchen plays the hall's silence ({la:.1f} dBFS)")
        m = envelope_match(b.voip, SPEECH)
        check(m > 0.6, f"what the hall plays is the kitchen's speech (envelope match {m:.2f})")
        check(re.search(r"drop in: connected with " + b.node, a.text()) is not None, "the kitchen's log says connected")
        b.press("end_drop_in")
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 2), "End Drop In in Home Assistant hangs up both")
        check(re.search(r"drop in: ended with " + b.node + r" after \d+ s \(" + b.node + " hung up", a.text()) is not None, "the kitchen's log says who hung up")

        # 2. answer: ask.  The hall rings until its action button
        r = b.page.set(drop_in_answer="ask"); check(r["applied"] == 1, "the hall: drop_in_answer=ask on the page")
        await a.call(b.node)
        check(await until(lambda: b.dropin() == ("ringing", a.node) and a.dropin()[0] == "calling", 2), f"the hall rings, the kitchen waits ({b.dropin()}, {a.dropin()})")
        await asyncio.sleep(2)
        check(a.dropin()[0] == "calling", "still ringing 2 s later (not answered by itself)")
        b.proc.send_signal(signal.SIGUSR2)                 # the hall's action button
        check(await until(lambda: a.dropin()[0] == "connected" and b.dropin()[0] == "connected", 2), "the action button answers")
        a.proc.send_signal(signal.SIGHUP)                  # "<wake word>, stop" on the kitchen
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 2), "\"stop\" hangs up")

        # 3. declined with the button while it rings
        await a.call(b.node)
        check(await until(lambda: b.dropin()[0] == "ringing", 2), "ringing again")
        b.proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.3); b.proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 3), "answered and hung up again by button")
        b.page.set(drop_in_answer="auto")

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

        # 5. from the settings page: on and off
        st, _, data = a.page.call("POST", "/api/dropin", b.node.encode())
        check(st == 200, f"the page drops in ({st} {data})")
        check(await until(lambda: a.page_dropin() == "connected", 3), "the page shows it connected")
        st, _, _ = b.page.call("POST", "/api/dropin", b"end")
        check(st == 200 and await until(lambda: a.page_dropin() == "idle", 2), "and the other Echo's page ends it")

        # 6. refusals
        b.cli.switch_command(b.by["do_not_disturb"].key, True)
        await asyncio.sleep(0.3)
        await a.call(b.node)
        check(await until(lambda: "refused: do not disturb is on" in a.text(), 3), "do not disturb refuses it")
        check(await until(lambda: a.dropin()[0] == "idle" and b.dropin()[0] == "idle", 2), "both idle after the refusal")
        b.cli.switch_command(b.by["do_not_disturb"].key, False); await asyncio.sleep(0.3)
        b.page.set(drop_in="off"); await b.reconnect()
        check("drop_in" not in b.by and not any(s.name == "drop_in" for s in b.services), "Drop In off: its entities and action are gone from Home Assistant")
        await a.call(b.node)
        check(await until(lambda: "refused: Drop In is off there" in a.text(), 3), "an Echo with Drop In off refuses")
        b.page.set(drop_in="on"); await b.reconnect()
        await a.call("Nobody Echo")
        check(await until(lambda: 'drop in: Nobody Echo: no Echo "nobody-echo" in this Echo\'s network' in a.text(), 2), "a name nobody has: refused at once")
        c.start(); await asyncio.sleep(6.5)               # outside the network: it makes one of its own after 5 s
        await a.call(c.node)
        check(await until(lambda: f'no Echo "{c.node}" in this Echo\'s network' in a.text(), 2), "an Echo outside the network cannot be called")
        check(a.dropin()[0] == "idle", "and nothing started")

        # 7. the hall vanishes mid-call: the kitchen notices
        await a.call(b.node)
        check(await until(lambda: a.dropin()[0] == "connected", 3), "connected once more")
        b.proc.kill(); b.proc.wait()
        check(await until(lambda: a.dropin()[0] == "idle", 8), "a vanished Echo ends the call (no audio for 5 s)")
        check("lost: no audio for 5 s" in a.text(), "the log says it was lost")
        check(not re.search(r"drop in: send: ", a.text()), "no send errors on the way")
    finally:
        for e in (a, b, c):
            try: e.proc.terminate(); e.proc.wait(timeout=3)
            except Exception: pass
    if check.failed:
        for e in (a, b): print(f"--- {e.name} log (drop in, arbitration):\n" + "".join(l for l in open(e.log) if "drop in" in l or "arbitration" in l or "state:" in l))
    print("FAILED" if check.failed else "all passed")
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
