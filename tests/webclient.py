"""A browser's side of the settings page (web.c), written from the protocol with Python's X25519 and BLAKE2b: for tests
that change settings the way a user now does (features, page-only settings)."""
import hashlib, http.client, json, os, signal, time
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat


def req(port, method, path, body=b"", headers=None, src=None):
    """SRC: the source address to ask from (127.0.0.x: the Echo counts unsigned asks per address)"""
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=15, source_address=(src, 0) if src else None)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse(); data = r.read(); c.close()
    return r.status, dict(r.getheaders()), data


def wait_port(*ports, secs=20):
    """until each port takes a connection: a hassmic just started binds them within ~0.3 s, but with every test starting
    at once (tests/run.py on CI's four CPUs) it took more than the 0.6 s the tests used to sleep"""
    import socket
    end = time.monotonic() + secs
    for p in ports:
        while time.monotonic() < end:
            try: socket.create_connection(("127.0.0.1", p), 0.2).close(); break
            except OSError: time.sleep(0.05)


class Browser:
    def __init__(self, port, sk=None):
        self.port = port
        self.hello = json.loads(req(port, "GET", "/api/hello")[2])
        self.sk = sk or X25519PrivateKey.generate()
        self.pub = self.sk.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        dev = bytes.fromhex(self.hello["pub"])
        shared = self.sk.exchange(X25519PublicKey.from_public_bytes(dev))
        self.k = hashlib.blake2b(b"hassmic web 1" + dev + self.pub, key=shared, digest_size=32).digest()
        self.ctr = int(time.time() * 1e6)

    def at(self, port):
        """the same browser key towards another Echo (a page calling it cross-origin)"""
        return Browser(port, self.sk)

    def through(self, via, label="test browser", nonce=None, voucher=None):
        """log in here through another Echo of the network this browser is approved on (web.c vouchers): 'approved' or not"""
        nonce = nonce or json.loads(req(self.port, "POST", "/api/vouch/nonce", self.pub.hex().encode())[2])["nonce"]
        st, _, data = via.call("POST", "/api/vouch/issue", f"{self.hello['pub']} {self.pub.hex()} {nonce}".encode())
        if st != 200: return json.loads(data).get("error")
        v = json.loads(data)
        body = f"{self.pub.hex()} {nonce} {voucher or v['voucher']} {v['via'].encode().hex()} {label}".encode()
        return json.loads(req(self.port, "POST", "/api/vouch/login", body)[2])["login"]

    def ask(self, label="test browser", src=None):
        """an unsigned ask to log in, the Echo's answer whole: {"login": "approved" | "waiting" | "refused"} and, refused,
        "why" ("timeout", "contested", "busy", "rate") and "retry_s" (web.c login_ask)"""
        return json.loads(req(self.port, "POST", "/api/login", f"{self.pub.hex()} {label}".encode(), src=src)[2])

    def login(self, label="test browser"):
        return self.ask(label)["login"]

    def login_with_button(self, proc, busy_secs=10):
        """ask, press the action button (SIGUSR2) once the Echo says the press is this login's, and wait until approved.
        Asked again only while it says "busy" (a wait just ended: web.c keeps browsers that are not approved back for a
        moment after every one; the tests that log in right after one start their Echo with a short quiet time,
        HASSMIC_LOGIN_MS=<wait>:<quiet>), for BUSY_SECS at the most.  Any other refusal ("contested", "rate",
        "timeout") is a failure: a test that expects none must not pass by asking and pressing again, as it did when
        every "refused" was asked over"""
        pressed = False; end = time.monotonic() + 8; busy_until = time.monotonic() + busy_secs
        while time.monotonic() < end:
            a = self.ask(); s = a["login"]
            if s == "approved": return True
            if s == "waiting" and not pressed: proc.send_signal(signal.SIGUSR2); pressed = True
            elif s == "refused":
                if pressed or a.get("why") != "busy" or time.monotonic() > busy_until:
                    print(f"login_with_button: refused ({a}){' after the press' if pressed else ''}"); return False
                end = time.monotonic() + 8            # (the eight seconds are for the press to take, from "waiting" on)
            time.sleep(0.1)
        return False

    def headers(self, method, path, body, ctr=None, key=None):
        if ctr is None: self.ctr += 1; ctr = self.ctr
        mac = hashlib.blake2b(f"{method}\n{path}\n{ctr}\n".encode() + body, key=key or self.k, digest_size=16).hexdigest()
        return {"X-HM-Pub": self.pub.hex(), "X-HM-Ctr": str(ctr), "X-HM-Mac": mac}

    def seal(self, ctr, data):
        """web.c unseal's other half: XOR with BLAKE2b blocks keyed from K, over the request's counter"""
        ek = hashlib.blake2b(b"hassmic seal 1", key=self.k, digest_size=32).digest()
        ks = b"".join(hashlib.blake2b(f"{ctr}\n".encode() + bytes([i]), key=ek, digest_size=64).digest() for i in range((len(data) + 63) // 64))
        return bytes(a ^ b for a, b in zip(data, ks))

    def seal2(self, ctr, data, purpose=b"hassmic seal 2", nonce=b""):
        """web.c seal2: any length, block i over the counter, "\n", the nonce (where there is one) and i as four bytes;
        purpose b"hassmic seal back 3" and the answer's nonce open what an Echo answers under the counter of the request
        it answers"""
        ek = hashlib.blake2b(purpose, key=self.k, digest_size=32).digest()
        ks = b"".join(hashlib.blake2b(f"{ctr}\n".encode() + nonce + i.to_bytes(4, "big"), key=ek, digest_size=64).digest() for i in range((len(data) + 63) // 64))
        return bytes(a ^ b for a, b in zip(data, ks))

    def open_sealed(self, ctr, answer):
        """an Echo's sealed answer ("hassmic sealed 3 <nonce hex>\n", then seal2 under it): its plain bytes, None when
        it is not one"""
        head, nl, rest = answer.partition(b"\n")
        if not nl or not head.startswith(b"hassmic sealed 3 ") or len(head) != 17 + 32: return None
        return self.seal2(ctr, rest, b"hassmic seal back 3", bytes.fromhex(head[17:].decode()))

    def call(self, method, path, body=b"", **kw):
        """a signed request; the answer to one that passed must be signed with the same key over our counter (web.c).
        body may be a function of the counter (what it seals is bound to that request)"""
        if callable(body):
            self.ctr += 1; c = self.ctr; body = body(c)
            hd = self.headers(method, path, body, ctr=c)
            st, h, data = req(self.port, method, path, body, hd)
            want = hashlib.blake2b(f"RESP\n{c}\n".encode() + data, key=self.k, digest_size=16).hexdigest()
            if st != 401 and h.get("X-HM-Mac") != want: raise AssertionError(f"answer to {method} {path} ({st}) is not signed right")
            return st, h, data
        hd = self.headers(method, path, body, **kw)
        st, h, data = req(self.port, method, path, body, hd)
        if st != 401 and "key" not in kw and "ctr" not in kw:
            want = hashlib.blake2b(f"RESP\n{hd['X-HM-Ctr']}\n".encode() + data, key=self.k, digest_size=16).hexdigest()
            if h.get("X-HM-Mac") != want: raise AssertionError(f"answer to {method} {path} ({st}) is not signed right")
        return st, h, data

    def set(self, **values):
        """set("mic_level=-20") style through keyword arguments: set(mic_level=-20, wifi_motion="on")"""
        body = "".join(f"{k}={v}\n" for k, v in values.items()).encode()
        st, _, data = self.call("POST", "/api/set", body)
        return json.loads(data) if st == 200 else {"status": st}

    def state(self):
        st, _, data = self.call("GET", "/api/state")
        return json.loads(data) if st == 200 else None
