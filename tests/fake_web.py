#!/usr/bin/env python3
"""The settings page (web.c) against build/hassmic-host: a browser's side written from the protocol (X25519 and
BLAKE2b from Python's libraries, not web/crypto.js), plus Home Assistant (aioesphomeapi) watching the same settings.
Login by the action button (SIGUSR2), signed reads and writes, replays and forged signatures, two logins at once,
export and import, the settings file from before names, revoking."""
import asyncio, gzip, hashlib, http.client as hc, json, os, signal, subprocess, tempfile, time
from aioesphomeapi import APIClient, NumberState
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, WEB = 16971, 16972


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def req(method, path, body=b"", headers=None):
    c = hc.HTTPConnection("127.0.0.1", WEB, timeout=5)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse(); data = r.read(); c.close()
    return r.status, dict(r.getheaders()), data


class Browser:
    def __init__(self, hello):
        self.sk = X25519PrivateKey.generate()
        self.pub = self.sk.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        dev = bytes.fromhex(hello["pub"])
        shared = self.sk.exchange(X25519PublicKey.from_public_bytes(dev))
        self.k = hashlib.blake2b(b"hassmic web 1" + dev + self.pub, key=shared, digest_size=32).digest()
        self.ctr = int(time.time() * 1e6)

    def login(self, label="test browser"):
        return json.loads(req("POST", "/api/login", f"{self.pub.hex()} {label}".encode())[2])["login"]

    def headers(self, method, path, body, ctr=None, key=None):
        if ctr is None: self.ctr += 1; ctr = self.ctr
        mac = hashlib.blake2b(f"{method}\n{path}\n{ctr}\n".encode() + body, key=key or self.k, digest_size=16).hexdigest()
        return {"X-HM-Pub": self.pub.hex(), "X-HM-Ctr": str(ctr), "X-HM-Mac": mac}

    def call(self, method, path, body=b"", **kw):
        return req(method, path, body, self.headers(method, path, body, **kw))


async def main():
    state = tempfile.mkdtemp()
    with open(os.path.join(state, "settings"), "w") as f: f.write("2 -22 1 0 0 1 0 de 2 1 -1 1 0 5 0\n")   # the file before names
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(state, "models"))
    log = os.path.join(state, "log")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Web", "-L", "-z", "0", "-o", "0",
                             "-a", "16973", "-W", str(WEB)], env=env, stderr=open(log, "w"))
    try:
        await asyncio.sleep(0.6)
        cfg = lambda: dict(l.strip().split("=", 1) for l in open(os.path.join(state, "config")) if "=" in l and not l.startswith("#"))
        c0 = cfg()
        check(c0.get("noise_reduction") == "medium" and c0.get("mic_level") == "-22" and c0.get("bluetooth_announcement_language", "de") == "de"
              and c0.get("sound_detection") == "on", f"the settings file from before names moved to state/config: {c0}")

        st, h, body = req("GET", "/")
        check(st == 200 and h.get("Content-Encoding") == "gzip" and b"hassmic settings" in gzip.decompress(body), "the page, gzip'd")
        st, h, body = req("GET", "/crypto.js")
        check(st == 200 and b"blake2b" in gzip.decompress(body), "its scripts")
        hello = json.loads(req("GET", "/api/hello")[2])
        check(hello["name"] == "Echo Web" and len(hello["pub"]) == 64 and h.get("Access-Control-Allow-Origin") == "*", f"hello: {hello['name']}, {hello['model']}")

        b = Browser(hello)
        check(b.call("GET", "/api/state")[0] == 401, "not logged in: no state")
        check(b.login() == "waiting" and b.login() == "waiting", "login waits for the button")
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.5)                     # the action button
        check(b.login() == "approved" and "login approved" in open(log).read(), "the action button approved it")
        st, _, body = b.call("GET", "/api/state")
        s = json.loads(body) if st == 200 else {}
        names = {x["name"]: x for x in s.get("settings", [])}
        check(st == 200 and names.get("mic_level", {}).get("value") == -22 and names["noise_reduction"]["choices"][names["noise_reduction"]["value"]] == "medium",
              f"signed: the state, settings as stored ({len(names)} settings)")
        check(any(c["me"] for c in s.get("clients", [])), "this browser among the approved ones")

        # Home Assistant sees what the page changes
        ha = APIClient("127.0.0.1", PORT, None); await ha.connect(login=True)
        ents, _ = await ha.list_entities_services(); by = {e.object_id: e for e in ents}; states = {}
        ha.subscribe_states(lambda x: states.__setitem__(x.key, getattr(x, "state", None)))
        await asyncio.sleep(0.5)
        st, _, body = b.call("POST", "/api/set", b"mic_level=-30\nwake_sound=off\n")
        r = json.loads(body); await asyncio.sleep(0.5)
        check(st == 200 and r["applied"] == 2 and cfg().get("mic_level") == "-30" and cfg().get("wake_sound") == "off", f"set two, saved: {r}")
        check(states.get(by["mic_level"].key) == -30, f"Home Assistant shows it at once: {states.get(by['mic_level'].key)}")
        st, _, body = b.call("POST", "/api/set", b"mic_level=5\nnope=1\nnoise_reduction=loud\n")
        r = json.loads(body)
        check(r["applied"] == 0 and r["errors"].count("\n") == 3 and cfg().get("mic_level") == "-30", f"out of range, unknown, wrong choice: refused: {r['errors']!r}")

        # signatures
        body = b"wake_sound=on\n"; hd = b.headers("POST", "/api/set", body)
        check(req("POST", "/api/set", body, hd)[0] == 200, "signed write")
        check(req("POST", "/api/set", body, hd)[0] == 401 and cfg().get("wake_sound") == "on", "the same request again (replay): refused")
        check(b.call("POST", "/api/set", b"wake_sound=off\n", key=os.urandom(32))[0] == 401, "wrong key: refused")
        hd = b.headers("POST", "/api/set", b"wake_sound=off\n");
        check(req("POST", "/api/set", b"mic_level=-15\n", hd)[0] == 401 and cfg().get("mic_level") == "-30", "body changed after signing: refused")
        x = Browser(hello)
        check(x.call("GET", "/api/state")[0] == 401, "a browser that was never approved: refused")

        # export / import
        st, h, text = b.call("GET", "/api/export"); text = text.decode()
        keys = {l.split("=")[0] for l in text.splitlines() if "=" in l and not l.startswith("#")}
        check(st == 200 and "mic_level" in keys and "mute" not in keys and "do_not_disturb" not in keys and "Echo Web" not in text.split("\n", 1)[1],
              f"export: settings, not this Echo's state or name ({len(keys)} lines)")
        b.call("POST", "/api/set", b"mic_level=-20\nnoise_reduction=off\n")
        st, _, body = b.call("POST", "/api/set", text.encode())
        r = json.loads(body)
        check(r["applied"] == len(keys) and not r["errors"] and cfg().get("mic_level") == "-30" and cfg().get("noise_reduction") == "medium",
              f"import: all {r['applied']} back")

        # two at once
        y, z = Browser(hello), Browser(hello)
        check(y.login() == "waiting" and z.login() == "refused" and y.login() == "refused", "two browsers asking at once: both refused")
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.5)
        check(y.call("GET", "/api/state")[0] == 401 and z.call("GET", "/api/state")[0] == 401, "and the button approves neither")

        # revoke
        check(json.loads(b.call("POST", "/api/revoke", b.pub.hex().encode())[2]).get("revoked") is True, "revoke this browser")
        check(b.call("GET", "/api/state")[0] == 401, "revoked: refused")
        await ha.disconnect()
    finally:
        proc.terminate(); proc.wait()
    if check.failed:
        print(open(log).read()); print("FAILED"); raise SystemExit(1)
    print("PASS")

asyncio.run(main())
