"""The Echo's settings page API (src/hassmic/web.c), as a browser speaks it: an X25519 key approved once with the Echo's
action button, every request and answer signed with BLAKE2b keyed from the shared secret.  Plain HTTP on the LAN
(port 28931); nothing secret ever travels, the signatures keep others from acting or answering in the Echo's name."""
from __future__ import annotations

import hashlib
import json
import time

import aiohttp
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding, NoEncryption, PrivateFormat, PublicFormat

DEFAULT_PORT = 28931


class EchoError(Exception):
    """the Echo could not be reached, or answered nonsense"""


class EchoAuthError(EchoError):
    """not (or no longer) approved: a factory reset, the login revoked on the page"""


def new_key() -> str:
    return X25519PrivateKey.generate().private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption()).hex()


class Echo:
    def __init__(self, session: aiohttp.ClientSession, host: str, port: int = DEFAULT_PORT, key: str | None = None):
        self.session, self.host, self.port = session, host, port
        self.sk = X25519PrivateKey.from_private_bytes(bytes.fromhex(key or new_key()))
        self.pub = self.sk.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        self.key = self.sk.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption()).hex()
        self.hello: dict = {}
        self.k: bytes | None = None
        self._ctr = 0

    @property
    def base(self) -> str:
        return f"http://{self.host}:{self.port}"

    async def _req(self, method: str, path: str, body: bytes = b"", headers: dict | None = None):
        try:
            async with self.session.request(method, self.base + path, data=body or None, headers=headers or {},
                                            timeout=aiohttp.ClientTimeout(total=15)) as r:
                return r.status, dict(r.headers), await r.read()
        except (aiohttp.ClientError, TimeoutError) as e:
            raise EchoError(f"{self.host}: {e}") from e

    async def get_hello(self) -> dict:
        """who the Echo is (name, node, model, its key, its MAC); the shared key from it"""
        st, _, data = await self._req("GET", "/api/hello")
        if st != 200:
            raise EchoError(f"{self.host}: /api/hello answered {st}")
        try:
            self.hello = json.loads(data)
            dev = bytes.fromhex(self.hello["pub"])
        except (ValueError, KeyError) as e:
            raise EchoError(f"{self.host}: not an Echo running hassmic") from e
        shared = self.sk.exchange(X25519PublicKey.from_public_bytes(dev))
        self.k = hashlib.blake2b(b"hassmic web 1" + dev + self.pub, key=shared, digest_size=32).digest()
        return self.hello

    async def login(self, label: str = "Home Assistant") -> str:
        """ask to be let in: 'waiting' (for the action button), 'approved' or 'refused'"""
        st, _, data = await self._req("POST", "/api/login", f"{self.pub.hex()} {label}".encode())
        if st != 200:
            raise EchoError(f"{self.host}: /api/login answered {st}")
        return json.loads(data).get("login", "refused")

    def _counter(self) -> int:
        # each request's own, increasing: the Echo takes each once within a window of the last 64 (web.c signed_ok)
        self._ctr = max(self._ctr + 1, int(time.time() * 1e6))
        return self._ctr

    def seal(self, ctr: int, data: bytes) -> bytes:
        """web.c unseal's other half (web/crypto.js seal): XOR with BLAKE2b-512 blocks keyed with EK over the request's
        counter, "\n" and the block's index; EK = BLAKE2b-256 keyed with K over "hassmic seal 1"."""
        ek = hashlib.blake2b(b"hassmic seal 1", key=self.k, digest_size=32).digest()
        out = bytearray(len(data))
        for i in range(0, (len(data) + 63) // 64):
            ks = hashlib.blake2b(f"{ctr}\n".encode() + bytes([i]), key=ek, digest_size=64).digest()
            for j in range(64):
                if i * 64 + j < len(data):
                    out[i * 64 + j] = data[i * 64 + j] ^ ks[j]
        return bytes(out)

    async def call(self, method: str, path: str, body: bytes = b"", ctr: int | None = None) -> bytes:
        if self.k is None:
            await self.get_hello()
        ctr = ctr if ctr is not None else self._counter()
        mac = hashlib.blake2b(f"{method}\n{path}\n{ctr}\n".encode() + body, key=self.k, digest_size=16).hexdigest()
        st, h, data = await self._req(method, path, body, {"X-HM-Pub": self.pub.hex(), "X-HM-Ctr": str(ctr), "X-HM-Mac": mac})
        if st == 401:
            raise EchoAuthError(f"{self.host}: not logged in")
        want = hashlib.blake2b(f"RESP\n{ctr}\n".encode() + data, key=self.k, digest_size=16).hexdigest()
        if h.get("X-HM-Mac") != want:
            raise EchoError(f"{self.host}: an answer without the Echo's signature")
        if st >= 400:
            try:
                why = json.loads(data).get("error", "")
            except ValueError:
                why = ""
            raise EchoError(why or f"{path}: {st}")
        return data

    # ---------------------------------------------------------------- alarms (alarms.h, /api/alarms)

    async def alarms(self) -> dict:
        return json.loads(await self.call("GET", "/api/alarms"))

    async def put_alarm(self, alarm_id: int, hour: int, minute: int, days: int, enabled: bool, label: str = "") -> None:
        label = " ".join(label.split())[:47]
        await self.call("POST", "/api/alarms/put", f"{alarm_id} {hour} {minute} {days} {1 if enabled else 0} {label}".encode())

    async def delete_alarm(self, alarm_id: int) -> None:
        await self.call("POST", f"/api/alarms/{alarm_id}/delete")

    async def snooze(self) -> None:
        await self.call("POST", "/api/alarms/snooze")

    async def stop(self) -> None:
        await self.call("POST", "/api/alarms/stop")

    # ---------------------------------------------------------------- the kiosk (kiosk.h, /api/kiosk)

    async def kiosk(self) -> dict:
        return json.loads(await self.call("GET", "/api/kiosk"))

    async def put_kiosk(self, ha: str, screens: list[tuple[str, str]], default: int, return_s: int, token: str | None,
                        app: str = "", settings: dict[str, str] | None = None) -> None:
        """the screens the Echo Show's kiosk app offers; the token and the screen app's settings (an Immich key: secrets
        too) sealed for this Echo together, in one block (the request is plain HTTP; one key stream per request)"""
        if self.k is None:
            await self.get_hello()
        ctr = self._counter()
        lines = [f"ha {ha}"]
        secret = (f"token {token}\n" if token else "") + "".join(f"set {k} {v}\n" for k, v in (settings or {}).items())
        if secret:
            raw = secret.encode()
            if len(raw) > 2048:
                raise EchoError("the kiosk's login and settings are longer than the Echo takes (2 kB)")
            lines.append("sealed " + self.seal(ctr, raw).hex())
        lines += [f"screen {n}\t{u}" for n, u in screens]
        lines += [f"default {default}", f"return {return_s}"]
        if app:
            lines.append(f"app {app}")
        await self.call("POST", "/api/kiosk", ("\n".join(lines) + "\n").encode(), ctr=ctr)

    # ---------------------------------------------------------------- the owner's Android apps (apps.c, /api/apps)

    APK_PIECE = 262144                  # web.c takes at most this much a request (413 above it)

    async def apps(self) -> dict:
        """{"apps": [{package, version, net}], "install": {state, package, error, have}}; {"apps": [], "none": true}
        where the Echo has no Android (the Dots)"""
        return json.loads(await self.call("GET", "/api/apps"))

    async def app_net(self, package: str, on: bool) -> None:
        await self.call("POST", "/api/apps", f"net {package} {1 if on else 0}".encode())

    async def app_lan(self, package: str, on: bool) -> None:
        """the app reachable from the home network: the ports it listens on, from private addresses (issue #109)"""
        await self.call("POST", "/api/apps", f"lan {package} {1 if on else 0}".encode())

    async def apk_send(self, path: str, progress=None) -> tuple[int, str]:
        """an APK sent in pieces from offset 0 (a new upload: an earlier one dropped), read from the file as it goes
        (up to 200 MB: not held in memory); its size and BLAKE2b-256, which the install names"""
        import asyncio, hashlib
        h, off = hashlib.blake2b(digest_size=32), 0
        loop = asyncio.get_running_loop()
        with open(path, "rb") as f:
            while True:
                piece = await loop.run_in_executor(None, f.read, self.APK_PIECE)
                if not piece:
                    break
                h.update(piece)
                await self.call("POST", f"/api/apps/apk/{off}", piece)
                off += len(piece)
                if progress:
                    progress(off)
        return off, h.hexdigest()

    async def apk_install(self, size: int, digest: str) -> None:
        """root's pm install -r of what was sent; GET /api/apps' "install" says how it went"""
        await self.call("POST", "/api/apps/apk/install", f"{size} {digest}".encode())

    async def app_uninstall(self, package: str) -> None:
        await self.call("POST", "/api/apps/uninstall", package.encode())

    async def set(self, text: str) -> dict:
        """settings as an export has them ("name=value" lines)"""
        return json.loads(await self.call("POST", "/api/set", text.encode()))
