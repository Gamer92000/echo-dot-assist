#!/usr/bin/env python3
"""Wake word clips (wakeclip.c, the settings page's "Wake word clips" card) against build/hassmic-host: none while off;
a wake word (SIGUSR1) and a near miss (SIGTTOU) kept, each from 1 s before the word to the setting's time after it, the
samples the ring held byte for byte (the capture file, looped as audio_file.c loops it), sealed on the way (web.c
clip_seal: never the room's audio in the clear, opened with the browser's key only), its waveform in the list; none
while muted; labels; a clip deleted, then all; refused unsigned; nothing of it in Home Assistant; a restart keeps the
setting and no clip; off drops them.  The page's unsealing is tests/unit/web_crypto_test.py's."""
import asyncio, hashlib, json, os, signal, subprocess, sys, tempfile, time
from aioesphomeapi import APIClient

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import Browser, wait_port, req
PORT, WEB, ARB = 17161, 17162, 17163
RATE = 16000
CAP = f"{ROOT}/testdata/alexa_espeak.raw"


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def until(cond, secs=5.0):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        v = cond()
        if v: return v
        time.sleep(0.05)
    return cond()


def stream(k, ctr, data):
    """web.c clip_seal's other half, from its description (web/crypto.js stream)"""
    ek = hashlib.blake2b(b"hassmic clip 1", key=k, digest_size=32).digest()
    ks = b"".join(hashlib.blake2b(f"{ctr}\n".encode() + i.to_bytes(4, "little"), key=ek, digest_size=64).digest()
                  for i in range((len(data) + 63) // 64))
    return bytes(a ^ b for a, b in zip(data, ks))


def captured():
    """what audio_file.c hands out, sample by sample: the file in blocks of 800, the last one padded with zeros, again"""
    raw = open(CAP, "rb").read()
    raw = raw[: len(raw) // 2 * 2]
    blocks = (len(raw) // 2 + 799) // 800
    return raw + bytes(blocks * 1600 - len(raw))


class Echo:
    def __init__(self, state):
        self.state, self.log = state, os.path.join(state, "log")
        env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=CAP, HASSMIC_PLAY=os.path.join(state, "play.raw"),
                   HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_CLOCK_SYNCED="0",
                   HASSMIC_MODELS=os.path.join(state, "models"))
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Clips", "-L", "-z", "0",
                                      "-o", "0", "-a", str(ARB), "-W", str(WEB)], env=env, stderr=open(self.log, "a"))
        wait_port(PORT, WEB)

    def stop(self):
        self.proc.send_signal(signal.SIGTERM)
        try: self.proc.wait(5)
        except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()


def clips(b):
    st, _, data = b.call("GET", "/api/wakeclips")
    return json.loads(data) if st == 200 else None


def audio(b, cid):
    """one clip's samples: (sealed bytes as sent, opened)"""
    b.ctr += 1; c = b.ctr
    hd = b.headers("GET", f"/api/wakeclips/{cid}", b"", ctr=c)
    st, h, data = req(WEB, "GET", f"/api/wakeclips/{cid}", b"", hd)
    want = hashlib.blake2b(f"RESP\n{c}\n".encode() + data, key=b.k, digest_size=16).hexdigest()
    if st != 200 or h.get("X-HM-Mac") != want: return st, None, None
    return st, data, stream(b.k, c, data)


def main():
    tmp = tempfile.mkdtemp()
    state = os.path.join(tmp, "state"); os.makedirs(state)
    open(os.path.join(state, "config"), "w").write("wake_sound=off\n")       # nothing played into the fake echo
    echo = Echo(state)
    try:
        b = Browser(WEB)
        check(b.login_with_button(echo.proc), "logged in by the action button")
        c = clips(b)
        check(c is not None and c["on"] is False and c["clips"] == [] and c["rate"] == RATE and c["pre_ms"] == 1000, f"off by default, none: {c and {k: c[k] for k in ('on', 'rate', 'pre_ms')}}")
        s = b.state()
        names = {x["name"]: x for x in s["settings"]}
        check(names.get("wake_clips", {}).get("value") in (0, False) and names.get("wake_clips_after", {}).get("group") == "Wake word",
              "the settings: on the Wake word page")
        echo.proc.send_signal(signal.SIGUSR1); time.sleep(1.6)
        check(clips(b)["clips"] == [], "a wake word while off leaves no clip")

        r = b.set(wake_clips="on", wake_clips_after="0.3s")
        check(r.get("applied") == 2, f"switched on, 0.3 s after the word: {r}")
        st, _, _ = req(WEB, "GET", "/api/wakeclips")
        check(st == 401, "unsigned: refused")

        echo.proc.send_signal(signal.SIGUSR1)
        c = until(lambda: (lambda x: x if x["clips"] and x["clips"][0]["done"] else None)(clips(b)), 5)
        w = c and c["clips"][0]
        check(bool(w) and w["kind"] == "wake" and w["word"] == "test" and w["wall"] == 0 and w["outcome"] == "" and w["label"] == "",
              f"a wake word kept: {w and {k: w[k] for k in ('kind', 'word', 'wall', 'len', 'word_at', 'word_len')}}")
        # SIGUSR1 plays a word of 0.6 s that ended 128 ms ago (main.c), and the clip runs 1 s ahead of it to 0.3 s past it
        check(bool(w) and w["word_at"] == RATE and w["word_len"] == RATE * 6 // 10 - RATE * 128 // 1000
              and w["len"] == RATE + w["word_len"] + RATE * 3 // 10, "1 s before the word, the word, 0.3 s after it")
        check(bool(w) and len(w["env"]) == 192 and w["floor_db"] < 0 and w["word_db"] < 0, "its waveform and levels in the list")
        st, sealed, pcm = audio(b, w["id"]) if w else (0, None, None)
        stream_ = captured(); n = len(stream_) // 2
        want = b"".join(stream_[((w["from"] + i) % n) * 2: ((w["from"] + i) % n) * 2 + 2] for i in range(w["len"])) if w else b""
        check(st == 200 and pcm == want, f"its samples are the capture's from sample {w and w['from']}, byte for byte ({len(pcm or b'')} bytes)")
        check(sealed is not None and sealed != want and sum(sealed) != sum(want), "and they came sealed: not the samples in the clear")
        other = Browser(WEB)                                   # another browser key, not logged in
        st, _, _ = req(WEB, "GET", f"/api/wakeclips/{w['id']}", b"", other.headers("GET", f"/api/wakeclips/{w['id']}", b""))
        check(st == 401, "a browser not approved gets none")

        echo.proc.send_signal(signal.SIGTTOU)
        c = until(lambda: (lambda x: x if len(x["clips"]) == 2 and x["clips"][0]["done"] else None)(clips(b)), 5)
        check(bool(c) and c["clips"][0]["kind"] == "near_miss" and c["clips"][1]["id"] == w["id"], "a near miss kept too, listed first (newest)")
        miss = c and c["clips"][0]

        b.set(mute="on"); time.sleep(0.2)
        echo.proc.send_signal(signal.SIGUSR1); time.sleep(1.6)
        check(len(clips(b)["clips"]) == 2, "muted: none kept")
        b.set(mute="off")

        st, _, data = b.call("POST", "/api/wakeclips/label", f"{w['id']} wake".encode())
        st2, _, _ = b.call("POST", "/api/wakeclips/label", f"{miss['id']} not_wake".encode())
        st3, _, _ = b.call("POST", "/api/wakeclips/label", f"{w['id']} maybe".encode())
        labels = {x["id"]: x["label"] for x in clips(b)["clips"]}
        check(st == 200 and st2 == 200 and st3 == 404 and labels == {w["id"]: "wake", miss["id"]: "not_wake"}, f"marked: {labels}")
        st, _, _ = b.call("POST", "/api/wakeclips/delete", str(miss["id"]).encode())
        c = clips(b)
        check(st == 200 and [x["id"] for x in c["clips"]] == [w["id"]], "one deleted")
        st, _, _ = b.call("GET", f"/api/wakeclips/{miss['id']}")
        check(st == 404, "its audio is gone with it")

        async def ha():
            cl = APIClient("127.0.0.1", PORT, None); await cl.connect(login=True)
            ents, services = await cl.list_entities_services(); await cl.disconnect()
            return [e.object_id for e in ents] + [s.name for s in services]
        ids = asyncio.run(ha())
        check(ids and not [i for i in ids if "clip" in i], f"nothing of it in Home Assistant ({len(ids)} entities and actions)")

        echo.stop(); echo = Echo(state)
        b = Browser(WEB, b.sk)
        c = clips(b)
        check(c["on"] is True and c["after_ms"] == 300 and c["clips"] == [], "after a restart: still on, 0.3 s, and no clip (RAM only)")
        echo.proc.send_signal(signal.SIGUSR1)
        check(bool(until(lambda: clips(b)["clips"], 5)), "a new one kept")
        st, _, _ = b.call("POST", "/api/wakeclips/delete", b"all")
        check(st == 200 and clips(b)["clips"] == [], "delete all")
        echo.proc.send_signal(signal.SIGUSR1)
        until(lambda: clips(b)["clips"], 5)
        b.set(wake_clips="off")
        c = clips(b)
        check(c["on"] is False and c["clips"] == [], "switched off: every clip dropped")
        log = open(echo.log).read()
        check("wake clip" not in log.replace("wake word clips deleted", ""), "nothing about the clips in the log but the deletes")
    finally:
        echo.stop()
    print("FAILED" if check.failed else "all ok")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
