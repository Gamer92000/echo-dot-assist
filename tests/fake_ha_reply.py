#!/usr/bin/env python3
"""Replies on another speaker (replyspk.c): three Echos (build/hassmic-host) of one arbitration network, each under a
fake Home Assistant that runs actions as the real one does (an answer to each call with a call id, at once for a
refusal; nothing at all with "Allow the device to perform Home Assistant actions" off), and keeps two media players.

Bath asks; Kitchen takes other Echos' replies; Hall does not (reply_from_others off).  The replies are WAVs served
here at 16 kHz mono, so what an Echo played is 32000 bytes a second of its play file (audio_file.c, real time).

Checked: replies here by default; on Kitchen: played there, nothing here, the run here ending with the reply there
(a question's follow-up starts after it), the wake word here cutting it there, refusals and a vanished Echo playing it
here; on a Home Assistant player: play_media as an announcement, the reply's length plus the start time, a refusal, an
unavailable player and actions not allowed playing it here, the cut sending media_stop; the room's player as Home
Assistant names it; Home Assistant's select and text; announcements only with the setting; the settings page's card.
Everything goes out on loopback (HASSMIC_ARB_ADDR): nothing of it reaches the LAN."""
import asyncio, base64, io, json, os, re, signal, struct, subprocess, sys, tempfile, threading, time, wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev, SelectInfo, TextInfo

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import wait_port, Browser
ARB, BCAST, HTTP = 28998, "127.255.255.255", 26399
RATE = 16000


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def until(cond, secs=5.0):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if cond(): return True
        await asyncio.sleep(0.05)
    return cond()


def wav(secs, freq=440):
    import math
    n = int(RATE * secs); b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(6000 * math.sin(2 * math.pi * freq * i / RATE))) for i in range(n)))
    return b.getvalue()


class Handler(BaseHTTPRequestHandler):
    """/reply.wav?s=<seconds>: a reply that long; /missing.wav: 404"""
    fetched = []
    def do_GET(self):
        u = urlparse(self.path); Handler.fetched.append(u.path)
        if u.path != "/reply.wav": self.send_response(404); self.end_headers(); return
        q = parse_qs(u.query)
        time.sleep(float(q.get("wait", ["0"])[0]))            # Home Assistant still rendering it
        body = wav(float(q.get("s", ["1"])[0]))
        self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a): pass


def url(secs, wait=0): return f"http://127.0.0.1:{HTTP}/reply.wav?s={secs}" + (f"&wait={wait}" if wait else "")


class Echo:
    def __init__(self, name, port, net, config=""):
        self.name, self.port, self.state = name, port, tempfile.mkdtemp()
        self.web, self.reply = port + 10, port + 30
        self.node = re.sub(r"[^a-z0-9]", "-", name.lower())
        self.log = os.path.join(self.state, "log")
        self.play = os.path.join(self.state, "play.raw")
        open(self.play, "wb").close()
        self.api_key = base64.b64encode(os.urandom(32)).decode()
        with open(os.path.join(self.state, "api_key"), "w") as f: f.write(self.api_key + "\n")
        with open(os.path.join(self.state, "config"), "w") as f: f.write("even_volume=off\n" + config)
        with open(os.path.join(self.state, "arbitration"), "w") as f: f.write(f"arbitrate 0\nctr 0\nnet {net}\n")
        self.env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_PLAY=self.play, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
                        HASSMIC_MDNS_FILE=os.path.join(self.state, "none"), HASSMIC_ARB_ADDR=BCAST, HASSMIC_TEST_SCORE="500",
                        HASSMIC_MODELS=os.path.join(self.state, "models"))
        # Home Assistant's side: what it was asked to run, what it answers, its media players (state, name)
        self.calls, self.allowed, self.refuse, self.subs = [], True, None, []
        self.players = {"media_player.bath_sonos": ("idle", "Bath Sonos"), "media_player.garage": ("unavailable", "Garage")}
        self.room = {"area": "Bath", "players": ["media_player.bath_sonos"], "names": ["Bath Sonos"]}
        self.starts = []

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.port), "-n", self.name, "-L",
                                      "-z", "0", "-o", "0", "-a", str(ARB), "-i", "0", "-r", str(self.reply), "-W", str(self.web)],
                                     env=self.env, stderr=open(self.log, "a"))
        wait_port(self.port, self.web)

    def text(self): return open(self.log, errors="replace").read()
    def played(self): return os.path.getsize(self.play)
    def wake(self): self.proc.send_signal(signal.SIGUSR1)

    def action(self, call):
        """HA's esphome manager: refused without the option (a repair, no answer); with a call id answered once it ran"""
        if call.is_event or not self.allowed: return
        self.calls.append((time.monotonic(), call.service, dict(call.data), dict(call.data_template), call.wants_response))
        if not call.call_id: return
        if call.service == "hassmic.check_actions":
            self.cli.send_homeassistant_action_response(call.call_id, False, "Action hassmic.check_actions not found", b"")
        elif call.service == "media_player.browse_media" and call.wants_response:
            self.cli.send_homeassistant_action_response(call.call_id, True, "", json.dumps({"response": self.room}).encode())
        elif call.service == "media_player.play_media":
            if self.refuse: self.cli.send_homeassistant_action_response(call.call_id, False, self.refuse, b"")
            else: self.cli.send_homeassistant_action_response(call.call_id, True, "", b"")
        else: self.cli.send_homeassistant_action_response(call.call_id, True, "", b"")

    def state_sub(self, entity, attribute):
        """a subscription: the state now, as Home Assistant sends it (nothing for an entity it does not have)"""
        self.subs.append((entity, attribute))
        if entity in self.players:
            st, nm = self.players[entity]
            self.cli.send_home_assistant_state(entity, attribute, nm if attribute == "friendly_name" else st)

    async def connect(self):
        self.cli = APIClient("127.0.0.1", self.port, None, noise_psk=self.api_key)
        await self.cli.connect(login=True)
        ents, _ = await self.cli.list_entities_services()
        self.by = {e.object_id: e for e in ents}
        self.states = {}
        self.cli.subscribe_states(lambda s: self.states.__setitem__(s.key, getattr(s, "state", None)))
        self.cli.subscribe_home_assistant_states_and_services(on_state=lambda s: None, on_service_call=self.action,
                                                              on_state_sub=self.state_sub, on_state_request=self.state_sub)
        self.started = asyncio.Event()
        async def handle_start(conv, flags, settings, phrase): self.starts.append(time.monotonic()); self.started.set(); return 0
        self.stops = []
        async def handle_stop(abort): self.stops.append((time.monotonic(), abort))
        async def handle_audio(data, *_): pass
        self.finished = []
        async def handle_finished(msg): self.finished.append(msg.success)
        self.cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio,
                                           handle_announcement_finished=handle_finished)

    async def reconnect(self):
        try: await self.cli.disconnect()
        except Exception: pass
        await asyncio.sleep(0.3); await self.connect()

    def ev(self, kind, data=None): self.cli.send_voice_assistant_event(kind, data)

    async def ask(self, reply_url, cont=False):
        """a run here: the wake word, then Home Assistant's pipeline up to the reply; when the reply's address went out"""
        self.started.clear(); self.wake()
        await asyncio.wait_for(self.started.wait(), 5)
        self.ev(Ev.VOICE_ASSISTANT_RUN_START); self.ev(Ev.VOICE_ASSISTANT_STT_START); self.ev(Ev.VOICE_ASSISTANT_STT_VAD_END)
        self.ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "what time is it"})
        self.ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1" if cont else "0"})
        self.ev(Ev.VOICE_ASSISTANT_TTS_START, {"text": "It is noon"})
        self.started.clear(); t = time.monotonic()
        self.ev(Ev.VOICE_ASSISTANT_TTS_END, {"url": reply_url}); self.ev(Ev.VOICE_ASSISTANT_RUN_END)
        return t

    def end_run(self):
        """the follow-up run heard nothing: it ends quietly"""
        self.ev(Ev.VOICE_ASSISTANT_RUN_START); self.ev(Ev.VOICE_ASSISTANT_ERROR, {"code": "stt-no-text-recognized", "message": "nothing"})

    def reply_state(self): return self.page.state()["reply"]
    def set_reply(self, spec):
        st, _, data = self.page.call("POST", "/api/reply", spec.encode())
        return st, json.loads(data)


async def settled(e, quiet=0.5, secs=15):
    """the play file stopped growing: what it played, and when it stopped"""
    end = time.monotonic() + secs; n, t = -1, time.monotonic()
    while time.monotonic() < end:
        m = e.played()
        if m != n: n, t = m, time.monotonic()
        elif time.monotonic() - t >= quiet: return n, t
        await asyncio.sleep(0.02)
    return n, t


async def main(bath, kitchen, hall):
    loop = asyncio.get_running_loop()
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP), Handler); threading.Thread(target=httpd.serve_forever, daemon=True).start()
    for e in (bath, hall): e.start()                         # Kitchen comes later: an Echo new to the network
    for e in (bath, hall):
        e.page = Browser(e.web); check(e.page.login_with_button(e.proc), f"{e.name}: settings page logged in")
        await e.connect()
    await asyncio.sleep(1.5)
    sel = bath.by.get("replies_play_on")
    check(isinstance(sel, SelectInfo) and sel.options == ["This Echo", "Speaker in this room", "Home Assistant player"],
          f"Home Assistant's select \"Replies play on\": no other Echo takes replies yet (Hall does not): {sel and sel.options}")
    kitchen.start(); kitchen.page = Browser(kitchen.web); check(kitchen.page.login_with_button(kitchen.proc), "Kitchen: settings page logged in")
    await kitchen.connect()

    # ---- what the Echos say of each other
    check(await until(lambda: {x["node"]: x["accepts"] for x in bath.reply_state()["echos"]} == {"kitchen": True, "hall": False}, 8),
          f"Bath knows Kitchen takes replies and Hall does not: {bath.reply_state()['echos']}")
    r = bath.reply_state()
    check(r["mode"] == "here" and r["running"] and r["accept"] and not r["announce"] and r["delay"] == 1500 and bath.page.state()["reply_proto"] == "esphome",
          f"defaults: replies here, other Echos' taken, announcements stay, a player's start 1.5 s: {r}")
    check(await until(lambda: "known=kitchen" in open(os.path.join(bath.state, "reply_speaker")).read() if os.path.exists(os.path.join(bath.state, "reply_speaker")) else False, 30)
          and "entities changed" not in bath.text(), "Kitchen learnt and kept for Home Assistant's select, without closing its link")
    # chosen on the page before Home Assistant lists it: listed again at once (the link closes, Home Assistant is back)
    st, r = bath.set_reply("echo kitchen")
    check(st == 200 and await until(lambda: "not in Home Assistant's select yet: listed again" in bath.text()), "chosen on the page: Home Assistant lists the select again")
    await asyncio.sleep(0.5); await bath.reconnect()             # as Home Assistant comes back by itself
    sel, txt = bath.by.get("replies_play_on"), bath.by.get("reply_player")
    check(isinstance(sel, SelectInfo) and sel.options == ["This Echo", "Speaker in this room", "Home Assistant player", "kitchen"]
          and isinstance(txt, TextInfo), f"Home Assistant: the select \"Replies play on\" and the text \"Reply player\": {sel and sel.options}")
    check("entities changed" not in kitchen.text() + hall.text(), "the other Echos' links never closed")
    bath.set_reply("here")
    await until(lambda: bath.states.get(sel.key) == "This Echo")
    check(bath.states.get(sel.key) == "This Echo" and bath.states.get(txt.key) == "", "their states: This Echo, no player")

    # ---- here, by default
    p0 = bath.played(); await bath.ask(url(1)); n, _ = await settled(bath)
    check(n - p0 == 32000, f"by default the reply plays here: {n - p0} bytes")

    # ---- on Kitchen
    st, r = bath.set_reply("echo kitchen")
    check(st == 200 and r["mode"] == "echo" and r["node"] == "kitchen", f"settings page: replies on Kitchen: {st} {r.get('mode')}")
    await until(lambda: bath.states.get(sel.key) == "kitchen")
    check(bath.states.get(sel.key) == "kitchen", "Home Assistant's select follows: kitchen")
    b0, k0 = bath.played(), kitchen.played()
    t = await bath.ask(url(2), cont=True)
    await asyncio.wait_for(bath.started.wait(), 10); follow = bath.starts[-1]
    kn, kend = await settled(kitchen)
    check(kn - k0 == 64000, f"the reply played on Kitchen, whole: {kn - k0} bytes")
    check(bath.played() == b0, f"nothing of it here: {bath.played() - b0} bytes")
    check(follow - t >= 2.0 and follow >= kend - 0.6, f"the question's follow-up here starts once Kitchen has played it: {follow - t:.2f} s after the reply's address, {follow - kend:+.2f} s against Kitchen's end")
    check(bath.reply_state()["last"]["result"] == "played" and "playing bath's reply" in kitchen.text(),
          f"the page says where it played: {bath.reply_state()['last']}")
    bath.end_run(); await asyncio.sleep(0.5)

    # the wake word here cuts it there, and starts the next request
    k0 = kitchen.played()
    await bath.ask(url(6)); await asyncio.sleep(1.5)
    bath.started.clear(); bath.wake()
    await asyncio.wait_for(bath.started.wait(), 5)
    kn, _ = await settled(kitchen)
    check(32000 <= kn - k0 <= 2.6 * 32000, f"the wake word here stopped the reply on Kitchen: {(kn - k0) / 32000:.2f} s of 6 played")
    check(await until(lambda: "asked to stop" in kitchen.text()) and bath.reply_state()["last"]["result"] == "cut", "Kitchen was asked to stop; the page says cut")
    bath.end_run(); await asyncio.sleep(0.5)

    # Kitchen busy with a run of its own: here instead
    kitchen.started.clear(); kitchen.wake(); await asyncio.wait_for(kitchen.started.wait(), 5)
    b0, k0 = bath.played(), kitchen.played()
    await bath.ask(url(1)); n, _ = await settled(bath)
    check(n - b0 == 32000 and kitchen.played() == k0, f"Kitchen busy: the reply here ({n - b0} bytes), none there")
    check("busy" in bath.reply_state()["last"]["why"], f"and the page says why: {bath.reply_state()['last']}")
    kitchen.end_run(); await asyncio.sleep(0.5)

    # Kitchen with do not disturb on: here (a bedroom Echo stays quiet at night)
    kitchen.page.set(do_not_disturb="on")
    b0, k0 = bath.played(), kitchen.played()
    await bath.ask(url(1)); n, _ = await settled(bath)
    check(n - b0 == 32000 and kitchen.played() == k0 and "do not disturb" in bath.reply_state()["last"]["why"],
          f"Kitchen in do not disturb: here ({n - b0} bytes): {bath.reply_state()['last']}")
    kitchen.page.set(do_not_disturb="off")

    # a timer ringing on Kitchen: here, not over the ring
    from aioesphomeapi import VoiceAssistantTimerEventType as Tm
    kitchen.cli.send_voice_assistant_timer_event(Tm.VOICE_ASSISTANT_TIMER_FINISHED, "t1", "tea", 60, 0, False)
    await asyncio.sleep(0.5)
    b0 = bath.played(); await bath.ask(url(1)); n, _ = await settled(bath, secs=6)
    check(n - b0 == 32000 and "busy" in bath.reply_state()["last"]["why"], f"a timer ringing on Kitchen: here: {bath.reply_state()['last']}")
    kitchen.cli.button_command(kitchen.by["stop_ringing"].key); await asyncio.sleep(0.5)

    # the wake word on Kitchen while the reply is still on its way there (Home Assistant rendering it): Kitchen's own
    # request goes on untouched, the reply plays here
    b0, k0, s0 = bath.played(), kitchen.played(), len(kitchen.stops)
    await bath.ask(url(1, wait=1.5)); await asyncio.sleep(0.5)
    kitchen.started.clear(); kitchen.wake(); await asyncio.wait_for(kitchen.started.wait(), 5)
    await until(lambda: bath.played() > b0, 8); n, _ = await settled(bath)
    check(n - b0 == 32000 and kitchen.played() == k0, f"Kitchen's own wake word first: the reply here ({n - b0} bytes), none there ({kitchen.played() - k0})")
    check("given up" in kitchen.text() and "busy before it began" in bath.reply_state()["last"]["why"], f"  and why: {bath.reply_state()['last']}")
    check(kitchen.stops[s0:] == [], f"  Kitchen's request went on untouched: {kitchen.stops[s0:]}")
    kitchen.end_run(); await asyncio.sleep(0.5)

    # stopped there (Kitchen's own wake word cuts what it plays): no follow-up opens here, whoever spoke talks to Kitchen
    n0 = len(bath.starts)
    await bath.ask(url(5), cont=True); await asyncio.sleep(1.5)
    kitchen.started.clear(); kitchen.wake(); await asyncio.wait_for(kitchen.started.wait(), 5)
    await asyncio.sleep(2.5)
    check(len(bath.starts) == n0 + 1 and bath.reply_state()["last"]["why"] == "stopped there",
          f"cut on Kitchen: no follow-up here ({len(bath.starts) - n0 - 1} opened): {bath.reply_state()['last']}")
    kitchen.end_run(); await asyncio.sleep(0.5)

    # Kitchen hangs mid-reply (its process stopped: TCP alone would not tell): Bath stops waiting within seconds
    await bath.ask(url(20)); await asyncio.sleep(1.5)
    kitchen.proc.send_signal(signal.SIGSTOP); t = time.monotonic()
    ok = await until(lambda: bath.reply_state()["last"]["why"] == "the link to it broke", 12)
    kitchen.proc.send_signal(signal.SIGCONT)
    check(ok and time.monotonic() - t < 9, f"Kitchen hung: Bath gave up waiting after {time.monotonic() - t:.1f} s: {bath.reply_state()['last']}")
    await settled(kitchen, secs=25); await kitchen.reconnect()

    # Hall does not take replies: chosen anyway (the page greys it), its replies play here
    st, r = bath.set_reply("echo hall")
    await until(lambda: "hall is not in Home Assistant's select yet" in bath.text()); await asyncio.sleep(0.5)
    await bath.reconnect()                                       # Home Assistant lists it again (not in its select before)
    b0 = bath.played(); await bath.ask(url(1)); n, _ = await settled(bath)
    check(st == 200 and n - b0 == 32000 and "off there" in bath.reply_state()["last"]["why"], f"Hall refuses: here ({n - b0} bytes): {bath.reply_state()['last']}")
    st, r = bath.set_reply("echo nowhere")
    check(st == 409 and "no Echo" in r["error"], f"an Echo nobody knows is refused: {st} {r}")
    st, r = bath.set_reply("echo bath")
    check(st == 409, f"and this Echo itself: {r}")

    # announcements: here unless the setting sends them along
    bath.set_reply("echo kitchen")
    b0, k0 = bath.played(), kitchen.played()
    res = await bath.cli.send_voice_assistant_announcement_await_response(url(1), 10)
    n, _ = await settled(bath)
    check(res.success and n - b0 == 32000 and kitchen.played() == k0, f"an announcement stays here by default: {n - b0} bytes")
    bath.page.set(reply_speaker_announcements="on")
    b0 = bath.played()
    res = await bath.cli.send_voice_assistant_announcement_await_response(url(1), 10)
    kn, _ = await settled(kitchen)
    check(res.success and kn - k0 == 32000 and bath.played() == b0, f"with \"Announcements play there too\" on Kitchen: {kn - k0} bytes, here {bath.played() - b0}")
    bath.page.set(reply_speaker_announcements="off")

    # Kitchen gone: here, without a long wait
    kitchen.proc.terminate(); kitchen.proc.wait()
    b0 = bath.played(); t = await bath.ask(url(1)); await until(lambda: bath.played() > b0)
    late = time.monotonic() - t; n, _ = await settled(bath)
    check(n - b0 == 32000 and late < 3, f"Kitchen gone: the reply here, {late:.1f} s late")
    kitchen.start(); await kitchen.connect()

    # ---- a Home Assistant player
    bath.page.set(reply_speaker_delay="500")
    st, r = bath.set_reply("player media_player.bath_sonos")
    check(st == 200 and r["mode"] == "player", f"replies on media_player.bath_sonos: {st}")
    check(await until(lambda: ("media_player.bath_sonos", "friendly_name") in bath.subs and (bath.reply_state()["player_info"] or {}).get("name") == "Bath Sonos"),
          f"its state and name subscribed, on the page: {bath.reply_state()['player_info']}")
    check(await until(lambda: bath.reply_state()["actions"] is True), "Home Assistant runs this Echo's actions (the probe answered)")
    b0, ncall = bath.played(), len(bath.calls)
    t = await bath.ask(url(2), cont=True)
    await asyncio.wait_for(bath.started.wait(), 10); follow = bath.starts[-1]
    pm = [c for c in bath.calls[ncall:] if c[1] == "media_player.play_media"]
    check(len(pm) == 1 and pm[0][2] == {"entity_id": "media_player.bath_sonos", "media_content_id": url(2), "media_content_type": "music"}
          and pm[0][3] == {"announce": "{{ true }}"}, f"play_media as an announcement on the player: {pm}")
    check(bath.played() == b0, f"nothing of it here: {bath.played() - b0} bytes")
    check(2.3 <= follow - t <= 3.3, f"the follow-up after the reply's 2 s and the player's 0.5 s start: {follow - t:.2f} s")
    bath.end_run(); await asyncio.sleep(0.5)

    # cut here: the player is stopped too
    ncall = len(bath.calls)
    await bath.ask(url(6)); await asyncio.sleep(1.5)
    bath.started.clear(); bath.wake(); await asyncio.wait_for(bath.started.wait(), 5)
    check(await until(lambda: any(c[1] == "media_player.media_stop" and c[2] == {"entity_id": "media_player.bath_sonos"} for c in bath.calls[ncall:])),
          "the wake word here: media_stop on the player")
    bath.end_run(); await asyncio.sleep(0.5)

    # refused by Home Assistant: here
    bath.refuse = "Entity media_player.bath_sonos does not support this service."
    b0 = bath.played(); await bath.ask(url(1)); n, _ = await settled(bath)
    check(n - b0 == 32000 and "does not support" in bath.reply_state()["last"]["why"], f"play_media refused: here ({n - b0} bytes): {bath.reply_state()['last']}")
    bath.refuse = None

    # unavailable: here, not even asked
    bath.set_reply("player media_player.garage"); await asyncio.sleep(0.5)
    b0, ncall = bath.played(), len(bath.calls)
    await bath.ask(url(1)); n, _ = await settled(bath)
    check(n - b0 == 32000 and not [c for c in bath.calls[ncall:] if c[1] == "media_player.play_media"] and "unavailable" in bath.reply_state()["last"]["why"],
          f"an unavailable player: here, not asked: {bath.reply_state()['last']}")
    bath.set_reply("player media_player.nothing"); await asyncio.sleep(0.5)
    b0 = bath.played(); await bath.ask(url(1)); n, _ = await settled(bath)
    check(n - b0 == 32000 and "no such media player" in bath.reply_state()["last"]["why"], f"one Home Assistant has not: here: {bath.reply_state()['last']}")
    st, r = bath.set_reply("player light.kitchen")
    check(st == 409, f"only a media player's entity id: {r}")

    # Home Assistant's text: the player named there, replies go there; emptied, this Echo again
    bath.cli.text_command(txt.key, "media_player.bath_sonos")
    check(await until(lambda: bath.reply_state()["mode"] == "player" and bath.reply_state()["player"] == "media_player.bath_sonos"
                      and bath.states.get(sel.key) == "Home Assistant player"), "Home Assistant's text names the player: replies there")
    bath.cli.text_command(txt.key, "")
    check(await until(lambda: bath.reply_state()["mode"] == "here" and bath.states.get(sel.key) == "This Echo"), "emptied: this Echo again")

    # actions not allowed: the probe goes unanswered, replies here
    bath.allowed = False; await bath.reconnect()
    bath.set_reply("player media_player.bath_sonos")
    check(await until(lambda: bath.reply_state()["actions"] is False, 8), "actions off in Home Assistant: the page says so")
    b0 = bath.played(); await bath.ask(url(1)); n, _ = await settled(bath)
    check(n - b0 == 32000 and "Allow the device" in bath.reply_state()["last"]["why"], f"and the reply plays here: {bath.reply_state()['last']}")
    bath.allowed = True; await bath.reconnect()

    # ---- the room's player, as Home Assistant finds it (the template itself: tests/ha_real_reply.py)
    bath.cli.select_command(sel.key, "Speaker in this room")
    check(await until(lambda: bath.reply_state()["room"]["entity"] == "media_player.bath_sonos" and bath.reply_state()["room"]["area"] == "Bath"),
          f"Home Assistant names the room's player: {bath.reply_state()['room']}")
    rq = [c for c in bath.calls if c[1] == "media_player.browse_media"]
    check(rq and rq[-1][4] and "mac" in rq[-1][3].get("entity_id", ""), "asked with a response template, about this Echo's own player by its MAC")
    b0, ncall = bath.played(), len(bath.calls)
    await bath.ask(url(1)); await asyncio.sleep(2.5)
    check(bath.played() == b0 and any(c[1] == "media_player.play_media" and c[2]["entity_id"] == "media_player.bath_sonos" for c in bath.calls[ncall:]),
          "the reply goes to the room's player")
    bath.room = {"area": "Bath", "players": [], "names": []}
    bath.set_reply("room"); await until(lambda: bath.reply_state()["room"]["entity"] == "")
    b0 = bath.played(); await bath.ask(url(1)); n, _ = await settled(bath)
    check(n - b0 == 32000, f"a room without another player: here ({n - b0} bytes): {bath.reply_state()['last']}")

    # a select of Kitchen from Home Assistant
    bath.cli.select_command(sel.key, "kitchen")
    check(await until(lambda: bath.reply_state()["mode"] == "echo" and bath.reply_state()["node"] == "kitchen"), "Home Assistant's select picks Kitchen")
    check(json.load(open(os.path.join(bath.state, "reply_speaker"))) if False else "node=kitchen" in open(os.path.join(bath.state, "reply_speaker")).read(),
          "kept in state/reply_speaker")
    httpd.shutdown()


def run():
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    bath, kitchen, hall = Echo("Bath", 26300, net), Echo("Kitchen", 26301, net), Echo("Hall", 26302, net, "reply_from_others=off\n")
    echos = (bath, kitchen, hall)
    try: asyncio.run(main(*echos))
    except Exception:
        import traceback; traceback.print_exc(); check.failed = True
    finally:
        for e in echos:
            if hasattr(e, "proc") and e.proc.poll() is None: e.proc.terminate(); e.proc.wait(); e.proc.returncode = "terminated"
    if check.failed:
        for e in echos: print(f"--- {e.name} (exit {e.proc.returncode if hasattr(e, 'proc') else '-'})\n{e.text()[-6000:]}")
        print("FAILED"); raise SystemExit(1)
    print("PASS")


if __name__ == "__main__":
    run()
