"""The Echo's settings page API (src/hassmic/web.c), as a browser speaks it: an X25519 key approved once with the Echo's
action button, every request and answer signed with BLAKE2b keyed from the shared secret.  Plain HTTP on the LAN
(port 28931); nothing secret ever travels, the signatures keep others from acting or answering in the Echo's name.
The camera's video is the exception, and it travels sealed with a key of its stream's (camera_stream)."""
from __future__ import annotations

import asyncio
import hashlib
import json
import struct
import time
from collections.abc import AsyncIterator

import aiohttp
from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.serialization import Encoding, NoEncryption, PrivateFormat, PublicFormat

DEFAULT_PORT = 28931
# camera.h's record types
CAM_CONFIG, CAM_KEY, CAM_FRAME = 1, 2, 3
CAM_REC_MAX = 4 << 20


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

    async def settings(self) -> dict[str, str]:
        """the Echo's settings as its export has them (GET /api/export: "name=value" lines, # comments)"""
        out = {}
        for line in (await self.call("GET", "/api/export")).decode("utf-8", "replace").splitlines():
            if line and not line.startswith("#") and "=" in line:
                k, _, v = line.partition("=")
                out[k.strip()] = v.strip()
        return out

    # ---------------------------------------------------------------- the camera (camera.h, docs/camera.md)

    async def camera(self) -> dict:
        """{"present", "on", "covered", "streaming"}: present 0 where the Echo has none (an Echo Dot); 404 from older builds"""
        return json.loads(await self.call("GET", "/api/camera"))

    def _signed_headers(self, method: str, path: str) -> tuple[int, dict]:
        ctr = self._counter()
        mac = hashlib.blake2b(f"{method}\n{path}\n{ctr}\n".encode(), key=self.k, digest_size=16).hexdigest()
        return ctr, {"X-HM-Pub": self.pub.hex(), "X-HM-Ctr": str(ctr), "X-HM-Mac": mac}

    async def camera_stream(self) -> AsyncIterator[tuple[int, int, bytes]]:
        """(type, pts in us, H.264 in Annex B) until the Echo ends the stream (the camera switched off, the lens
        covered, the process gone) or the caller stops iterating.  web.c camera_api: the head signed over the server's
        nonce, every record sealed with SK = BLAKE2b-256 keyed with K over "hassmic camera 1\n" ctr "\n" nonce, as
        ChaCha20-Poly1305 with noise.c's nonce (4 zero bytes, the record's number, 8 little-endian ones) and the
        record's length as associated data.  EchoError("busy") when another viewer has it, ("off"), ("covered")."""
        if self.k is None:
            await self.get_hello()
        ctr, headers = self._signed_headers("GET", "/api/camera/stream")
        try:
            r = await self.session.get(self.base + "/api/camera/stream", headers=headers,
                                       timeout=aiohttp.ClientTimeout(total=None, connect=10, sock_read=15))
        except (aiohttp.ClientError, TimeoutError) as e:
            raise EchoError(f"{self.host}: {e}") from e
        try:
            if r.status != 200:
                data = await r.read()
                if r.status == 401:
                    raise EchoAuthError(f"{self.host}: not logged in")
                try:
                    why = json.loads(data).get("error", "")
                except ValueError:
                    why = ""
                raise EchoError(why or f"/api/camera/stream: {r.status}")
            nonce = r.headers.get("X-HM-Nonce", "")
            want = hashlib.blake2b(f"RESP\n{ctr}\n".encode() + nonce.encode(), key=self.k, digest_size=16).hexdigest()
            if len(nonce) != 32 or r.headers.get("X-HM-Mac") != want:
                raise EchoError(f"{self.host}: a camera stream without the Echo's signature")
            sk = hashlib.blake2b(f"hassmic camera 1\n{ctr}\n".encode() + bytes.fromhex(nonce), key=self.k, digest_size=32).digest()
            aead, seq = ChaCha20Poly1305(sk), 0
            while True:
                try:
                    head = await r.content.readexactly(4)
                except asyncio.IncompleteReadError:
                    return                                  # the Echo ended it
                n = struct.unpack("<I", head)[0]
                if n < 9 + 16 or n > CAM_REC_MAX:
                    raise EchoError(f"{self.host}: a camera record of {n} bytes")
                try:
                    pt = aead.decrypt(b"\0\0\0\0" + struct.pack("<Q", seq), await r.content.readexactly(n), head)
                except InvalidTag as e:
                    raise EchoError(f"{self.host}: a camera record not sealed by the Echo") from e
                except asyncio.IncompleteReadError:
                    return
                seq += 1
                yield pt[0], struct.unpack_from("<Q", pt, 1)[0], pt[9:]
        except (aiohttp.ClientError, TimeoutError) as e:
            raise EchoError(f"{self.host}: {e}") from e
        finally:
            r.close()

    # ---------------------------------------------------------------- Drop In from Home Assistant (docs/video-dropin.md)

    async def drop_in(self) -> "EchoCall":
        """web.c GET /api/call: the connection becomes the call.  A plain socket, since aiohttp does not write into a
        request whose answer it reads.  EchoError with the Echo's words when it refuses (busy, do not disturb, ...)."""
        if self.k is None:
            await self.get_hello()
        ctr, headers = self._signed_headers("GET", "/api/call")
        try:
            reader, writer = await asyncio.wait_for(asyncio.open_connection(self.host, self.port), 10)
            writer.write((f"GET /api/call HTTP/1.1\r\nHost: {self.host}:{self.port}\r\nContent-Length: 0\r\n" +
                          "".join(f"{k}: {v}\r\n" for k, v in headers.items()) + "\r\n").encode())
            await writer.drain()
            head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 15)
        except (OSError, TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError) as e:
            raise EchoError(f"{self.host}: {e}") from e
        lines = head.decode(errors="replace").split("\r\n")
        status = int(lines[0].split()[1]) if len(lines[0].split()) > 1 else 0
        h = {k.strip().lower(): v.strip() for k, _, v in (l.partition(":") for l in lines[1:] if ":" in l)}
        if status != 200:
            body = b""
            try:
                body = await asyncio.wait_for(reader.readexactly(int(h.get("content-length", "0"))), 5)
            except (asyncio.IncompleteReadError, TimeoutError, ValueError):
                pass
            writer.close()
            if status == 401:
                raise EchoAuthError(f"{self.host}: not logged in")
            try:
                why = json.loads(body).get("error", "")
            except ValueError:
                why = ""
            raise EchoError(why or f"/api/call: {status}")
        nonce = h.get("x-hm-nonce", "")
        want = hashlib.blake2b(f"RESP\n{ctr}\n".encode() + nonce.encode(), key=self.k, digest_size=16).hexdigest()
        if len(nonce) != 32 or h.get("x-hm-mac") != want:
            writer.close()
            raise EchoError(f"{self.host}: a call without the Echo's signature")
        key = lambda d: hashlib.blake2b(f"hassmic call 1 {d}\n{ctr}\n".encode() + bytes.fromhex(nonce), key=self.k, digest_size=32).digest()
        return EchoCall(reader, writer, key("down"), key("up"))


class EchoCall:
    """One call (web.c call_api): recv() the Echo's records, ('S', state), ('A', 640 bytes of 16 kHz s16le) and
    ('V', (type, pts us, H.264)) while the camera is on, None at the end; audio() a frame to play there (20 ms, sent all along: the call's keepalive); video() this side's picture for its screen; hangup()."""
    FRAME = 640

    def __init__(self, reader, writer, down: bytes, up: bytes):
        self.reader, self.writer = reader, writer
        self._down, self._up = ChaCha20Poly1305(down), ChaCha20Poly1305(up)
        self._dseq = self._useq = 0
        self.state = ""

    async def recv(self):
        try:
            head = await self.reader.readexactly(4)
            n = struct.unpack("<I", head)[0]
            if n < 1 + 16 or n > CAM_REC_MAX + 32:          # ('V': a key frame, tens of kB)
                raise EchoError(f"a call record of {n} bytes")
            pt = self._down.decrypt(b"\0\0\0\0" + struct.pack("<Q", self._dseq), await self.reader.readexactly(n), head)
        except (asyncio.IncompleteReadError, ConnectionError):
            return None
        except InvalidTag as e:
            raise EchoError("a call record not sealed by the Echo") from e
        self._dseq += 1
        t = chr(pt[0])
        if t == "S":
            self.state = pt[1:].decode(errors="replace")
            return ("S", self.state)
        if t == "V" and len(pt) >= 10:
            return ("V", (pt[1], struct.unpack_from("<Q", pt, 2)[0], pt[10:]))
        return (t, pt[1:])

    def _send(self, t: bytes, p: bytes = b"") -> None:
        pt = t + p
        head = struct.pack("<I", len(pt) + 16)
        self.writer.write(head + self._up.encrypt(b"\0\0\0\0" + struct.pack("<Q", self._useq), pt, head))
        self._useq += 1

    async def audio(self, pcm: bytes) -> None:
        if len(pcm) != self.FRAME:
            raise ValueError("a frame is 320 samples")
        self._send(b"A", pcm)
        await self.writer.drain()

    VIDEO_MAX = 512 << 10                               # web.c CALL_UP_MAX: a record longer ends the call there

    async def video(self, typ: int, pts_us: int, data: bytes) -> bool:
        """a record of this side's picture (camera.h's types: 1 config, 2 key, 3 frame, H.264 Annex B) for the Echo's
        screen; False when it is too long to go (a frame of a browser's camera that big is dropped, not the call)"""
        if not 1 <= typ <= 3 or len(data) > self.VIDEO_MAX - 9:
            return False
        self._send(b"V", bytes([typ]) + struct.pack("<Q", pts_us & (2**64 - 1)) + data)
        await self.writer.drain()
        return True

    async def hangup(self) -> None:
        try:
            self._send(b"H")
            await self.writer.drain()
        except ConnectionError:
            pass
        self.close()

    def close(self) -> None:
        self.writer.close()
