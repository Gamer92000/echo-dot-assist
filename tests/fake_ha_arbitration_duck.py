#!/usr/bin/env python3
"""Music ducked on the other Echos that heard the wake word (issue #165, duck.c; tests/arbecho.py): three Echos in one
network, two of them hear it.  The one that answers tells the network "on" (three copies, then every 2 s) and "off"
when it is done, to every member at once; the one that lost the round ducks for it until then, the one that never heard
it plays on.  A follow-up question (continue_conversation: one "on" across both runs), Home Assistant's media after
the reply (no duck for it), the setting off, an "off" lost (the duck runs out), an Echo answering alone, and with two
members played here with the network's key: an "on" from an Echo that did not claim in the round (not taken), two
that both answer (down until both are done), and a Drop In invite sent to every member (Drop In never sees it)."""
import asyncio, base64, hashlib, os, re, signal, socket, struct, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from aioesphomeapi import MediaPlayerInfo, VoiceAssistantEventType as Ev
from arbecho import ARB, BCAST, Echo, HA, check, until, start, run
from esphome_common import wav_bytes

key = os.urandom(32)
a, b, c = (Echo("Echo Kitchen", 16961, 500, net=(2, key)), Echo("Echo Living Room", 16962, 900, net=(2, key)),
           Echo("Echo Bedroom", 16963, 100, net=(2, key)))
HOLD = 6.5                                          # duck.c HOLD_MS
HTTP = 16970                                        # replies and media: /s<seconds>.wav


class Wavs(BaseHTTPRequestHandler):
    def do_GET(self):
        body = wav_bytes(48000, float(re.search(r"/s([\d.]+)\.wav", self.path).group(1)))
        self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.send_header("Content-Length", str(len(body))); self.end_headers()
        try: self.wfile.write(body)
        except OSError: pass
    def log_message(self, *a): pass


def ducks(e): return re.findall(r"duck: (.*)", e.text())


async def main():
    ha = HA([a, b, c])
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP), Wavs); threading.Thread(target=httpd.serve_forever, daemon=True).start()
    await start((a, b, c), ha)
    ok = await until(lambda: all(e.st("arbitration_peers") == 2 for e in (a, b, c)), 10)
    check(ok, "three Echos in one network")
    sn = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)                 # listens in on the messages
    sn.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); sn.bind(("", ARB)); sn.setblocking(False)
    bpk = b.pub()[:8]
    msgs, to_all = [], []                           # (when, on): the living room Echo's duck messages; whether each went to all

    async def listen(secs):
        end = time.monotonic() + secs
        while time.monotonic() < end:
            try: p = sn.recv(512)
            except BlockingIOError: await asyncio.sleep(0.005); continue
            # "HMA1", type 7, net, from, to, counter, length, payload, MAC
            if p[4] == 7 and p[13:21] == bpk and p[37] == 2 and p[38] == 0x40:
                to_all.append(p[21:29] == b"\xff" * 8)
                msgs.append((time.monotonic(), p[39]))

    # the kitchen and the living room Echo hear it, the bedroom Echo does not
    a.wake(); b.wake(); t0 = time.monotonic()
    await listen(5.2)                               # the round first: 500 ms while the window is learnt
    check(len(b.starts) == 1 and not a.starts and not c.starts, "the living room Echo answers (900 over 500)")
    on = [t - t0 for t, x in msgs if x == 1]
    check(to_all and all(to_all), "its duck messages go to every member (to ff..ff)")
    check(len(on) >= 5 and on[2] - on[0] < 0.45 and all(1.8 < y - x < 2.3 for x, y in zip(on[2:], on[3:])),
          f"\"on\" three times at once, then every 2 s while it answers: {[round(x, 2) for x in on]}")
    check(ducks(a) == ["echo-living-room answers what we heard too, music down until it is done"],
          f"the kitchen Echo, which heard it too, ducks for it: {ducks(a)}")
    check(not ducks(c), "the bedroom Echo, which never heard it, plays on")
    msgs.clear(); b.end_pipeline(); t1 = time.monotonic()
    await listen(1)
    off = [t - t1 for t, x in msgs if x == 0]
    check(len(off) == 3 and not [x for _, x in msgs if x == 1], f"done: \"off\" three times, no more \"on\": {[round(x, 2) for x in off]}")
    check(ducks(a)[-1:] == ["echo-living-room is done, music back"] and not ducks(c), f"the kitchen Echo's music back: {ducks(a)}")
    await asyncio.sleep(1.5)

    # a follow-up question: the reply, then a new run for the answer; the kitchen Echo stays down for both
    n, na = len(b.starts), len(ducks(a))
    a.wake(); b.wake(); await until(lambda: len(b.starts) > n, 3); await asyncio.sleep(0.3)
    ev = b.cli.send_voice_assistant_event
    ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "turn on the light"})
    ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "1"})
    ev(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP}/s1.5.wav"})
    ev(Ev.VOICE_ASSISTANT_RUN_END, None)
    msgs.clear(); ok = await until(lambda: len(b.starts) > n + 1, 6); await listen(1)
    check(ok and ducks(a)[na:] == ["echo-living-room answers what we heard too, music down until it is done"]
          and not [x for _, x in msgs if x == 0], f"its follow-up: no \"off\" between the two runs, the kitchen Echo down throughout: {ducks(a)[na:]}")
    msgs.clear(); b.end_pipeline(); await listen(1)
    check(ducks(a)[-1] == "echo-living-room is done, music back", "the conversation over: music back")
    await asyncio.sleep(1)

    # "play the radio": the reply, then Home Assistant's media on the living room Echo, which ducks nobody
    mp = next(e for e in b.by.values() if isinstance(e, MediaPlayerInfo))
    n, na, size = len(b.starts), len(ducks(a)), lambda: os.path.getsize(os.path.join(b.state, "play.raw"))
    a.wake(); b.wake(); await until(lambda: len(b.starts) > n, 3); await asyncio.sleep(0.3)
    ev(Ev.VOICE_ASSISTANT_STT_END, {"text": "play the radio"})
    ev(Ev.VOICE_ASSISTANT_INTENT_END, {"conversation_id": "x", "continue_conversation": "0"})
    ev(Ev.VOICE_ASSISTANT_TTS_END, {"url": f"http://127.0.0.1:{HTTP}/s1.wav"})
    ev(Ev.VOICE_ASSISTANT_RUN_END, None)
    await listen(1.8)                               # the reply played (media meanwhile: "busy, dropped")
    b.cli.media_player_command(mp.key, media_url=f"http://127.0.0.1:{HTTP}/s8.wav")
    msgs.clear(); p0 = size(); await listen(4)
    check(size() - p0 > 96000 * 3 and not [x for _, x in msgs if x == 1] and ducks(a)[na:] == [
          "echo-living-room answers what we heard too, music down until it is done", "echo-living-room is done, music back"],
          f"its media plays, no \"on\" for it, the kitchen Echo's music back: {ducks(a)[na:]}, {(size() - p0) / 96000:.1f} s played")
    b.cli.media_player_command(mp.key, command=__import__("aioesphomeapi").MediaPlayerCommand.STOP); await asyncio.sleep(1)

    # the setting off on the kitchen Echo: it plays on
    a.page.set(duck_for_others="off"); n, nb = len(ducks(a)), len(b.starts)
    a.wake(); b.wake(); await listen(2.5)
    check(len(b.starts) == nb + 1 and len(ducks(a)) == n, "\"Duck for other Echos\" off: the kitchen Echo plays on")
    b.end_pipeline(); await listen(1); a.page.set(duck_for_others="on"); await asyncio.sleep(1.5)

    # the answering Echo's "off" never comes (it stops dead here): the duck runs out HOLD after its last "on"
    msgs.clear(); n, nb = len(ducks(a)), len(b.starts)
    a.wake(); b.wake(); await listen(2.5)
    check(len(b.starts) == nb + 1 and ducks(a)[n:] == ["echo-living-room answers what we heard too, music down until it is done"],
          "ducking for it again")
    last = max(t for t, x in msgs if x == 1)
    b.proc.send_signal(signal.SIGSTOP)
    ok = await until(lambda: "nothing from" in a.text(), HOLD + 1.5); t = time.monotonic() - last
    check(ok and ducks(a)[-1] == f"nothing from echo-living-room for {int(HOLD * 1000)} ms, music back" and HOLD - 0.3 < t < HOLD + 0.8,
          f"its \"off\" lost: the music back {t:.1f} s after its last \"on\"")
    n = len(ducks(a)); b.proc.send_signal(signal.SIGCONT); await listen(2.5)
    check(len(ducks(a)) == n, "its \"on\" later (no round of ours near): not taken")
    b.end_pipeline(); await listen(1)

    # an Echo answering what nobody else heard: no round anywhere, nobody ducks
    na, nc, nb = len(ducks(a)), len(ducks(c)), len(b.starts)
    b.wake(); await listen(2.5)
    check(len(b.starts) == nb + 1 and len(ducks(a)) == na and len(ducks(c)) == nc, "only the living room Echo heard it: the others play on")
    b.end_pipeline(); await asyncio.sleep(0.5)

    # the action button starts a run: no round, nobody heard anything, nothing told
    await listen(0.5); msgs.clear(); nb = len(b.starts)      # the last run's "off" copies read first
    b.proc.send_signal(signal.SIGUSR2); ok = await until(lambda: len(b.starts) > nb, 3); await listen(2.5)
    check(ok and not msgs, f"a run from the action button: no \"on\" at all: {msgs}")
    b.end_pipeline(); await listen(1)
    check(not msgs, "nor an \"off\"")

    # two members played here with the network's key; the bedroom Echo answering a wake word of its own meanwhile:
    # the kitchen Echo takes "on" only from Echos that claimed in its round
    nid, k64 = a.net().split(); nk = base64.b64decode(k64); nb8 = struct.pack("<Q", int(nid, 16))
    mac = hashlib.blake2b(b"hassmic arbitration mac", key=nk, digest_size=32).digest()
    ctr = [1 << 40]
    def signed(p): return p + hashlib.blake2b(p, key=mac, digest_size=16).digest()
    def fctr(): ctr[0] += 1; return struct.pack("<Q", ctr[0])
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    send = lambda p: tx.sendto(p, (BCAST, ARB))
    kw = hashlib.blake2b(b"alexa", digest_size=8).digest()          # the wake word heard (fake_ha_arbitration_window.py)
    h1, h2 = os.urandom(32), os.urandom(32)
    for k, name in ((h1, b"echo-hallway"), (h2, b"echo-office")): send(signed(b"HMA1\x01" + k + nb8 + bytes([len(name)]) + name + fctr()))
    await asyncio.sleep(0.5)
    def msg(k, payload, to=b"\xff" * 8): send(signed(b"HMA1\x07" + nb8 + k[:8] + to + fctr() + bytes([len(payload)]) + payload))
    def fduck(k, on): msg(k, bytes([0x40, on]))
    def answers(k): send(signed(b"HMA1\x04" + nb8 + k[:8] + fctr() + kw + struct.pack("<iBB", 99999, 0, 1)))
    na, nc = len(ducks(a)), len(c.starts)
    a.wake(); await asyncio.sleep(0.05); answers(h1)
    await asyncio.sleep(1.6); c.wake()             # past the 1.5 s in which a claim counts in the kitchen's round
    ok = await until(lambda: len(c.starts) > nc, 3); await asyncio.sleep(1)
    check(ok and not a.starts and ducks(a)[na:] == [], f"the bedroom Echo answers its own: its \"on\" not taken: {ducks(a)[na:]}")
    fduck(h2, 1); await asyncio.sleep(0.3)
    check(ducks(a)[na:] == [], "nor one from an Echo that never claimed in the round")
    fduck(h1, 1); await asyncio.sleep(0.3)
    check(ducks(a)[na:] == ["echo-hallway answers what we heard too, music down until it is done"], f"the winner's \"on\" taken: {ducks(a)[na:]}")
    fduck(h1, 0); await asyncio.sleep(0.3)
    check(ducks(a)[-1] == "echo-hallway is done, music back", "and its \"off\"")
    c.end_pipeline(); await asyncio.sleep(2)

    # both members heard it and both answer: down until both are done
    na = len(ducks(a))
    a.wake(); await asyncio.sleep(0.05); answers(h1); answers(h2); await asyncio.sleep(1)
    fduck(h1, 1); fduck(h2, 1); await asyncio.sleep(0.3); fduck(h1, 0); await asyncio.sleep(0.3)
    check(ducks(a)[na:] == ["echo-hallway answers what we heard too, music down until it is done",
                            "echo-office answers what we heard too, music down until it is done",
                            "echo-hallway is done, music stays down for echo-office"], f"two answer: one done, still down for the other: {ducks(a)[na:]}")
    fduck(h2, 0); await asyncio.sleep(0.3)
    check(ducks(a)[-1] == "echo-office is done, music back", "both done: music back")

    # a Drop In invite (44 bytes and more) sent to every member rather than to one: Drop In never sees it
    nd = a.text().count("drop in")
    msg(h1, bytes([1]) + os.urandom(8) + struct.pack("<H", 28932) + os.urandom(32) + bytes([0]))
    await asyncio.sleep(0.5)
    check(a.text().count("drop in") == nd and b.text().count("drop in: call") == 0, "an invite to all: no Echo rings or refuses it")
    msg(h1, bytes([1]) + os.urandom(8) + struct.pack("<H", 28932) + os.urandom(32) + bytes([0]), to=a.pub()[:8])
    ok = await until(lambda: a.text().count("drop in") > nd, 2)
    check(ok, "the same invite to the kitchen Echo alone: Drop In takes it (rings or refuses)")
    sn.close(); httpd.shutdown()

run(main, (a, b, c))
