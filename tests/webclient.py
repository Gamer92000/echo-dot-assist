"""A browser's side of the settings page (web.c), written from the protocol with Python's X25519 and BLAKE2b: for tests
that change settings the way a user now does (features, page-only settings)."""
import hashlib, http.client, json, os, signal, time
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat


def req(port, method, path, body=b"", headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse(); data = r.read(); c.close()
    return r.status, dict(r.getheaders()), data


class Browser:
    def __init__(self, port):
        self.port = port
        self.hello = json.loads(req(port, "GET", "/api/hello")[2])
        self.sk = X25519PrivateKey.generate()
        self.pub = self.sk.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        dev = bytes.fromhex(self.hello["pub"])
        shared = self.sk.exchange(X25519PublicKey.from_public_bytes(dev))
        self.k = hashlib.blake2b(b"hassmic web 1" + dev + self.pub, key=shared, digest_size=32).digest()
        self.ctr = int(time.time() * 1e6)

    def login(self, label="test browser"):
        return json.loads(req(self.port, "POST", "/api/login", f"{self.pub.hex()} {label}".encode())[2])["login"]

    def login_with_button(self, proc):
        """ask, press the action button (SIGUSR2), and wait until approved"""
        self.login(); proc.send_signal(signal.SIGUSR2)
        for _ in range(50):
            if self.login() == "approved": return True
            time.sleep(0.1)
        return False

    def headers(self, method, path, body, ctr=None, key=None):
        if ctr is None: self.ctr += 1; ctr = self.ctr
        mac = hashlib.blake2b(f"{method}\n{path}\n{ctr}\n".encode() + body, key=key or self.k, digest_size=16).hexdigest()
        return {"X-HM-Pub": self.pub.hex(), "X-HM-Ctr": str(ctr), "X-HM-Mac": mac}

    def call(self, method, path, body=b"", **kw):
        return req(self.port, method, path, body, self.headers(method, path, body, **kw))

    def set(self, **values):
        """set("mic_level=-20") style through keyword arguments: set(mic_level=-20, wifi_motion="on")"""
        body = "".join(f"{k}={v}\n" for k, v in values.items()).encode()
        st, _, data = self.call("POST", "/api/set", body)
        return json.loads(data) if st == 200 else {"status": st}

    def state(self):
        st, _, data = self.call("GET", "/api/state")
        return json.loads(data) if st == 200 else None
