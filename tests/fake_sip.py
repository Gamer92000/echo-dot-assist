#!/usr/bin/env python3
"""The phone (sip.c): build/hassmic-host as an extension of a fake phone system (a PBX on loopback, written here from
RFC 3261, 2617, 3550 and 7587, not from sip.c), under a fake Home Assistant.  The Echo registers with its password by
digest; the PBX calls it (G.711 A-law, then Opus as Asterisk offers it), it rings, is answered from Home Assistant and
with the action button, hears the PBX's tone on its Voip stream (HASSMIC_VOIP) and sends its own microphone (speech) back;
the PBX hangs up, the Echo hangs up ("stop"), a call is cancelled while it rings, declined, refused while busy, while do
not disturb is on, and from anyone but the server.  Then the Echo calls (Home Assistant's phone_call): through a 407,
ringing, answered, hung up; busy; cancelled while it rings.  Then the settings page: the account (never the password
back), a new password, a wrong one (refused), the feature off (unregistered)."""
import asyncio, base64, ctypes, hashlib, os, re, signal, socket, subprocess, sys, tempfile, time
import numpy as np
from aioesphomeapi import APIClient

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import wait_port, Browser
SPEECH = f"{ROOT}/testdata/alexa_espeak.raw"
API, WEB, SIP, PBX, MEDIA = 26090, 26091, 26092, 26095, 26096     # SIP on 26092, RTP on 26093
USER, PASS, REALM = "70", "s3cret pass", "hassmic-test"
# the call pipeline's microphone (HASSMIC_CALL_CAP, audio_file.c's micHfp): a 700 Hz tone at -20 dBFS, as its AGC
# leaves a talker, so what a call sends tells the two microphones apart
CALL_CAP = os.path.join(tempfile.mkdtemp(), "callmic.raw")
(32768 * 10 ** (-20 / 20) * np.sqrt(2) * np.sin(2 * np.pi * 700 * np.arange(32000) / 16000)).astype("<i2").tofile(CALL_CAP)


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


# ---------------------------------------------------------------- G.711 (Sun's g711.c, as ITU-T G.711)
def _alaw(v):
    v >>= 3; mask = 0xd5
    if v < 0: v = -v - 1; mask = 0x55
    seg = next((i for i, e in enumerate((0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff)) if v <= e), 8)
    if seg == 8: return 0x7f ^ mask
    return ((seg << 4) | ((v >> 1 if seg < 2 else v >> seg) & 0x0f)) ^ mask
def _alaw_dec(a):
    a ^= 0x55; t = (a & 0x0f) << 4; seg = (a & 0x70) >> 4
    t = t + 8 if seg == 0 else t + 0x108 if seg == 1 else (t + 0x108) << (seg - 1)
    return t if a & 0x80 else -t
ALAW = np.array([_alaw(v) for v in range(-32768, 32768)], np.uint8)
ALAW_DEC = np.array([_alaw_dec(a) for a in range(256)], np.int16)
alaw = lambda x: ALAW[x.astype(np.int32) + 32768].tobytes()
alaw_dec = lambda b: ALAW_DEC[np.frombuffer(b, np.uint8)]


# ---------------------------------------------------------------- Opus through the system's libopus
opus = ctypes.CDLL("libopus.so.0")
opus.opus_encoder_create.restype = opus.opus_decoder_create.restype = ctypes.c_void_p
opus.opus_encoder_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
opus.opus_decoder_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
opus.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
opus.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]
def opus_encoder(rate):
    e = ctypes.c_int(); enc = opus.opus_encoder_create(rate, 1, 2048, ctypes.byref(e))
    def encode(pcm):
        buf = ctypes.create_string_buffer(400); a = np.ascontiguousarray(pcm, np.int16)
        n = opus.opus_encode(enc, a.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)), len(a), buf, 400)
        return buf.raw[:n]
    return encode
def opus_decoder(rate):
    e = ctypes.c_int(); dec = opus.opus_decoder_create(rate, 1, ctypes.byref(e))
    def decode(data):
        out = np.zeros(rate // 50, np.int16)
        n = opus.opus_decode(dec, data, len(data), out.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)), len(out), 0)
        return out[:max(n, 0)]
    return decode


def db(x):
    x = np.asarray(x, float)
    return 10 * np.log10((x ** 2).mean() / 32768 ** 2 + 1e-20) if len(x) else -200
def tone_db(x, f, rate):
    x = np.asarray(x, float); t = np.arange(len(x)) / rate
    return 20 * np.log10(abs((x * np.exp(-2j * np.pi * f * t)).sum()) * 2 / max(len(x), 1) / np.sqrt(2) / 32768 + 1e-12)
def tone(f, secs, rate, dbfs=-20):
    return (32768 * 10 ** (dbfs / 20) * np.sqrt(2) * np.sin(2 * np.pi * f * np.arange(int(secs * rate)) / rate)).astype(np.int16)


# ---------------------------------------------------------------- SIP messages
class Msg:
    def __init__(self, data):
        text = data.decode(errors="replace")
        head, _, self.body = text.partition("\r\n\r\n")
        lines = head.split("\r\n")
        self.start = lines[0]; self.h = []
        for l in lines[1:]:
            k, _, v = l.partition(":"); self.h.append((k.strip(), v.strip()))
        p = self.start.split(" ", 2)
        self.req = not self.start.startswith("SIP/2.0")
        self.method, self.uri = (p[0], p[1]) if self.req else (None, None)
        self.code = None if self.req else int(p[1])
        self.raw = text
    def get(self, k, all=False):
        v = [b for a, b in self.h if a.lower() == k.lower()]
        return v if all else (v[0] if v else None)
    def cseq(self):
        n, m = self.get("CSeq").split(); return int(n), m
    def branch(self): return re.search(r"branch=([^;,\s]+)", self.get("Via")).group(1)
    def tag(self, k): m = re.search(r"tag=([^;,\s]+)", self.get(k) or ""); return m.group(1) if m else None


def digest_ok(auth, method, password=PASS):
    """an Authorization (or Proxy-) header as RFC 2617 has it, checked from our side"""
    f = dict(re.findall(r'(\w+)="?([^",]*)"?', auth[len("Digest"):]))
    ha1 = hashlib.md5(f"{f['username']}:{f['realm']}:{password}".encode()).hexdigest()
    ha2 = hashlib.md5(f"{method}:{f['uri']}".encode()).hexdigest()
    want = hashlib.md5(f"{ha1}:{f['nonce']}:{f['nc']}:{f['cnonce']}:{f['qop']}:{ha2}".encode() if f.get("qop") else f"{ha1}:{f['nonce']}:{ha2}".encode()).hexdigest()
    return f.get("response") == want and f.get("username") == USER and f.get("realm") == REALM


class Pbx(asyncio.DatagramProtocol):
    """the server: REGISTER answered by itself (a challenge, then the password checked), the rest queued for the test"""
    def __init__(self):
        self.q, self.regs, self.password, self.refuse = [], [], PASS, False
        self.echo = None                            # where the Echo's SIP is (from its REGISTER's Contact)
    def connection_made(self, t): self.t = t
    def datagram_received(self, data, addr):
        m = Msg(data)
        if m.req and m.method == "REGISTER": self.on_register(m, addr); return
        self.q.append((m, addr))
    def respond(self, m, code, reason, addr, extra="", body="", to_tag=None):
        to = m.get("To") + (f";tag={to_tag}" if to_tag and "tag=" not in m.get("To") else "")
        hdr = "".join(f"Via: {v}\r\n" for v in m.get("Via", all=True))
        ct = "Content-Type: application/sdp\r\n" if body else ""
        self.t.sendto((f"SIP/2.0 {code} {reason}\r\n{hdr}From: {m.get('From')}\r\nTo: {to}\r\nCall-ID: {m.get('Call-ID')}\r\n"
                       f"CSeq: {m.get('CSeq')}\r\n{extra}{ct}Content-Length: {len(body)}\r\n\r\n{body}").encode(), addr)
    def on_register(self, m, addr):
        self.regs.append(m)
        auth = m.get("Authorization")
        if self.refuse and auth: self.respond(m, 403, "Forbidden", addr); return
        if not auth or not digest_ok(auth, "REGISTER", self.password):
            self.respond(m, 401, "Unauthorized", addr, f'WWW-Authenticate: Digest realm="{REALM}", nonce="{os.urandom(8).hex()}", qop="auth", algorithm=MD5\r\n')
            return
        exp = m.get("Expires")
        self.echo = addr
        self.respond(m, 200, "OK", addr, f"Contact: <{re.search(r'<([^>]+)>', m.get('Contact')).group(1)}>;expires={exp}\r\n", to_tag="reg")
    def send(self, text, addr): self.t.sendto(text.encode(), addr)
    async def expect(self, pred, secs=3):
        end = time.monotonic() + secs
        while time.monotonic() < end:
            for i, (m, a) in enumerate(self.q):
                if pred(m): del self.q[i]; return m, a
            await asyncio.sleep(0.02)
        return None, None


class Media(asyncio.DatagramProtocol):
    """the PBX's RTP: what the Echo sends, kept by payload type"""
    def __init__(self): self.got, self.src = [], None
    def connection_made(self, t): self.t = t
    def datagram_received(self, data, addr):
        if len(data) >= 12 and data[0] >> 6 == 2: self.got.append((data[1] & 0x7f, data[12:], time.monotonic())); self.src = addr
    async def stream(self, to, pt, frames, step_ts):
        seq, ts, ssrc = int.from_bytes(os.urandom(2), "big"), int.from_bytes(os.urandom(4), "big"), os.urandom(4)
        t0 = time.monotonic()
        for i, f in enumerate(frames):
            hdr = bytes([0x80, pt | (0x80 if i == 0 else 0)]) + ((seq + i) & 0xffff).to_bytes(2, "big") + ((ts + i * step_ts) & 0xffffffff).to_bytes(4, "big") + ssrc
            self.t.sendto(hdr + f, to)
            await asyncio.sleep(max(0, t0 + (i + 1) * 0.02 - time.monotonic()))


def offer(codecs, port=MEDIA):
    """an SDP offer as a PBX makes it: codecs in its order, ("opus", 107) / ("PCMA", 8) / ("PCMU", 0)"""
    rtp = "".join(f"a=rtpmap:{pt} {n}/{'48000/2' if n == 'opus' else '8000'}\r\n" for n, pt in codecs)
    return (f"v=0\r\no=pbx 1 1 IN IP4 127.0.0.1\r\ns=pbx\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio {port} RTP/AVP "
            + " ".join(str(pt) for _, pt in codecs) + f"\r\n{rtp}a=ptime:20\r\na=sendrecv\r\n")


def sdp_media(body):
    m = re.search(r"m=audio (\d+) RTP/AVP ([\d ]+)", body); c = re.search(r"c=IN IP4 ([\d.]+)", body)
    return (c.group(1), int(m.group(1)), [int(x) for x in m.group(2).split()]) if m and c else None


class Call:
    """a call from the PBX to the Echo"""
    n = 0
    def __init__(self, pbx, display="Portero", user="60", codecs=(("PCMA", 8), ("PCMU", 0)), src=None):
        Call.n += 1
        self.pbx, self.cid, self.tag, self.branch = pbx, f"call{Call.n}-{os.urandom(4).hex()}@pbx", os.urandom(4).hex(), "z9hG4bK" + os.urandom(6).hex()
        self.frm = f'"{display}" <sip:{user}@127.0.0.1:{PBX}>;tag={self.tag}'
        self.to = f"<sip:{USER}@127.0.0.1:{SIP}>"; self.codecs, self.src, self.echo_tag = codecs, src, None
    def request(self, method, cseq=1, body="", branch=None, to=None):
        ct = "Content-Type: application/sdp\r\n" if body else ""
        return (f"{method} sip:{USER}@127.0.0.1:{SIP} SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:{PBX};rport;branch={branch or self.branch}\r\n"
                f"Max-Forwards: 70\r\nFrom: {self.frm}\r\nTo: {to or self.to}\r\nCall-ID: {self.cid}\r\nCSeq: {cseq} {method}\r\n"
                f"Contact: <sip:{USER}@127.0.0.1:{PBX}>\r\n{ct}Content-Length: {len(body)}\r\n\r\n{body}")
    def invite(self):
        text = self.request("INVITE", body=offer(self.codecs))
        if self.src: self.src.sendto(text.encode(), ("127.0.0.1", SIP))
        else: self.pbx.send(text, ("127.0.0.1", SIP))
    def mine(self, m): return m.get("Call-ID") == self.cid
    async def response(self, code, method="INVITE", secs=3):
        m, _ = await self.pbx.expect(lambda m: not m.req and self.mine(m) and m.code == code and m.cseq()[1] == method, secs)
        if m and m.tag("To"): self.echo_tag = m.tag("To")
        return m
    def ack(self, cseq=1, branch=None):
        self.pbx.send(self.request("ACK", cseq, branch=branch, to=f"{self.to};tag={self.echo_tag}"), ("127.0.0.1", SIP))
    def bye(self):
        self.pbx.send(self.request("BYE", 2, branch="z9hG4bK" + os.urandom(6).hex(), to=f"{self.to};tag={self.echo_tag}"), ("127.0.0.1", SIP))
    def cancel(self):
        self.pbx.send(self.request("CANCEL"), ("127.0.0.1", SIP))


class Echo:
    def __init__(self):
        self.state = tempfile.mkdtemp()
        self.log = os.path.join(self.state, "log"); self.voip = os.path.join(self.state, "voip.raw")
        self.api_key = base64.b64encode(os.urandom(32)).decode()
        with open(os.path.join(self.state, "api_key"), "w") as f: f.write(self.api_key + "\n")
        with open(os.path.join(self.state, "config"), "w") as f: f.write("even_volume=off\nphone=on\n")
        with open(os.path.join(self.state, "phone"), "w") as f: f.write(f"server=127.0.0.1:{PBX}\nuser={USER}\npassword={PASS.encode().hex()}\n")
        self.keys = os.path.join(self.state, "keypad"); os.mkfifo(self.keys); self.keypad = os.open(self.keys, os.O_RDWR)
        self.env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_PLAY=os.path.join(self.state, "play.raw"), HASSMIC_VOIP=self.voip,
                        HASSMIC_MDNS_FILE=os.path.join(self.state, "none"), HASSMIC_MODELS=os.path.join(self.state, "models"), HASSMIC_CAP=SPEECH,
                        HASSMIC_CALL_CAP=CALL_CAP)
        self.states, self.seq = {}, []

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(API), "-n", "Phone Echo", "-L", "-z", "0", "-o", "0",
                                      "-a", "0", "-c", str(SIP), "-W", str(WEB), "-b", self.keys], env=self.env, stderr=open(self.log, "a"))
        wait_port(API, WEB)

    def text(self): return open(self.log).read()
    def st(self, oid): return self.states.get(self.by[oid].key) if oid in self.by else None
    def phone(self): return self.st("phone"), self.st("phone_with")
    def results(self): return [v for o, v in self.seq if o == "phone_result"]

    def on_state(self, s):
        v = getattr(s, "state", None) if hasattr(s, "state") else getattr(s, "event_type", None)
        self.states[s.key] = v
        self.seq.append((next((o for o, e in self.by.items() if e.key == s.key), None), v))

    async def connect(self):
        self.states = {}
        self.cli = APIClient("127.0.0.1", API, None, noise_psk=self.api_key)
        await self.cli.connect(login=True)
        ents, self.services = await self.cli.list_entities_services()
        self.by = {e.object_id: e for e in ents}
        self.cli.subscribe_states(self.on_state)

    async def reconnect(self):
        try: await self.cli.disconnect()
        except Exception: pass
        await asyncio.sleep(0.5); await self.connect()

    async def call(self, number):
        await self.cli.execute_service(next(s for s in self.services if s.name == "phone_call"), {"number": number})
    def press(self, oid): self.cli.button_command(self.by[oid].key)


def voip_tail(path, secs):
    try: return np.fromfile(path, np.int16)[-16000 * secs:]
    except OSError: return np.zeros(0, np.int16)


async def until(cond, secs):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if cond(): return True
        await asyncio.sleep(0.05)
    return False


async def main():
    loop = asyncio.get_running_loop()
    _, pbx = await loop.create_datagram_endpoint(Pbx, local_addr=("127.0.0.1", PBX))
    _, media = await loop.create_datagram_endpoint(Media, local_addr=("127.0.0.1", MEDIA))
    stranger = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); stranger.bind(("127.0.0.2", 0))
    e = Echo(); e.start()
    try:
        # registration: a challenge, then the password by digest
        check(await until(lambda: any(m.get("Authorization") for m in pbx.regs) and pbx.echo, 5), "the Echo registers, answering the challenge")
        r = next(m for m in pbx.regs if m.get("Authorization"))
        check(r.get("Authorization") and digest_ok(r.get("Authorization"), "REGISTER"), "  its digest is right (RFC 2617, qop=auth)")
        check(f"<sip:{USER}@127.0.0.1:{SIP}>" in (r.get("Contact") or ""), f"  Contact: {r.get('Contact')}")
        check(PASS not in r.raw and PASS.encode().hex() not in r.raw, "  the password itself never goes out")
        await e.connect()
        check(any(s.name == "phone_call" for s in e.services), "Home Assistant has the action phone_call")
        check(all(o in e.by for o in ("phone", "phone_with", "phone_line", "answer_phone", "hang_up_phone", "phone_result")),
              "and Phone, Phone with, Phone line, Answer phone, Hang up phone, Phone result")
        types = list(getattr(e.by.get("phone_result"), "event_types", []))
        check(types[:2] == ["connected", "ringing"] and {"busy", "declined", "cancelled", "no_line"} <= set(types), f"  Phone result's event types: {types}")
        check(await until(lambda: e.st("phone_line") == "registered" and e.phone() == ("idle", ""), 3), f"Phone line registered, Phone idle ({e.st('phone_line')}, {e.phone()})")

        # requests: OPTIONS (a PBX's qualify) from the server; anything from another address refused
        c0 = Call(pbx)
        pbx.send(c0.request("OPTIONS"), ("127.0.0.1", SIP))
        check(await c0.response(200, "OPTIONS") is not None, "OPTIONS from the server: 200")
        cs = Call(pbx, src=stranger); cs.invite(); stranger.settimeout(2)
        try: got = stranger.recv(4000).decode()
        except socket.timeout: got = ""
        check(got.startswith("SIP/2.0 403"), f"an INVITE from another address than the server's: {got.splitlines()[0] if got else 'nothing'}")
        check(e.phone()[0] == "idle", "  and nothing rings")

        # 1. the PBX calls with G.711: rings, answered from Home Assistant
        c = Call(pbx); mark = len(e.seq); c.invite()
        check(await c.response(100) is not None and await c.response(180) is not None, "an INVITE: 100 Trying, 180 Ringing")
        check(await until(lambda: e.phone() == ("ringing", "Portero (60)"), 2), f"Phone ringing, Phone with \"Portero (60)\" ({e.phone()})")
        await asyncio.sleep(1)
        check(e.phone()[0] == "ringing", "  still ringing a second later (never answered by itself)")
        e.press("answer_phone")
        ok = await c.response(200)
        sd = sdp_media(ok.body) if ok else None
        check(sd is not None and sd[2] == [8] and "a=rtpmap:8 PCMA/8000" in ok.body, f"Answer phone: 200 OK, the answer picks A-law, the offer's first ({sd})")
        check(ok is not None and f"sip:{USER}@127.0.0.1:{SIP}" in (ok.get("Contact") or ""), "  with the Echo's Contact")
        c.ack()
        check(await until(lambda: e.phone() == ("connected", "Portero (60)"), 2), f"Phone connected ({e.phone()})")
        check([v for o, v in e.seq[mark:] if o == "phone"] == ["ringing", "connected"], f"  Home Assistant hears ringing, connected ({[v for o, v in e.seq[mark:] if o == 'phone']})")
        to = (sd[0], sd[1])
        await media.stream(to, 8, [alaw(f) for f in tone(1000, 3, 8000).reshape(-1, 160)], 160)
        played = voip_tail(e.voip, 2)
        check(tone_db(played, 1000, 16000) > -24 and db(played) - tone_db(played, 1000, 16000) < 1,
              f"the Echo plays the PBX's 1 kHz tone ({tone_db(played, 1000, 16000):.1f} dB of {db(played):.1f} dBFS; sent at -20)")
        media.got.clear()
        await media.stream(to, 8, [alaw(np.zeros(160, np.int16))] * 150, 160)     # the PBX's side quiet: the Echo's talker passes
        got = [p for pt, p, _ in media.got]
        check(len(got) > 120 and all(pt == 8 for pt, _, _ in media.got) and all(len(p) == 160 for p in got),
              f"the Echo sends A-law RTP, 20 ms a packet ({len(got)} packets in 3 s)")
        sent = np.concatenate([alaw_dec(p) for p in got[-100:]]) if got else np.zeros(0)
        check(db(sent) > -45, f"  with its microphone in it ({db(sent):.1f} dBFS)")
        check(tone_db(sent, 700, 8000) > db(sent) - 1 and -25 < tone_db(sent, 700, 8000) < -16,
              f"  the call pipeline's microphone, without the assistant's gain on it (its 700 Hz at {tone_db(sent, 700, 8000):.1f} dB; -20 sent)")
        check("call: the call pipeline's microphone (micHfp)" in e.text(), "  the log says so")
        check(media.src == ("127.0.0.1", SIP + 1), f"  from its RTP port ({media.src})")
        # a second call while this one runs: busy
        c2 = Call(pbx, display="Desk", user="11"); c2.invite()
        check(await c2.response(486) is not None, "a second call meanwhile: 486 Busy Here")
        c2.ack()
        c.bye()
        check(await c.response(200, "BYE") is not None, "the PBX hangs up: 200 to its BYE")
        check(await until(lambda: e.phone() == ("idle", ""), 2), f"Phone idle ({e.phone()})")
        check(re.search(r"phone: ended with Portero \(60\) after \d+ s \(Portero \(60\) hung up\)", e.text()) is not None, "  the log says who hung up")

        # 2. Opus, as Asterisk offers it (its own payload type, 48 kHz): answered by the action button, ended by "stop"
        c = Call(pbx, codecs=(("opus", 107), ("PCMA", 8))); c.invite()
        check(await c.response(180) is not None and await until(lambda: e.phone()[0] == "ringing", 2), "an Opus call rings")
        e.proc.send_signal(signal.SIGUSR2)                                    # the action button
        ok = await c.response(200); sd = sdp_media(ok.body) if ok else None
        check(sd is not None and sd[2] == [107] and "a=rtpmap:107 opus/48000/2" in ok.body, f"the action button answers: Opus with the offer's type 107 ({sd})")
        c.ack(); to = (sd[0], sd[1]); enc = opus_encoder(48000)
        await media.stream(to, 107, [enc(f) for f in tone(1000, 3, 48000).reshape(-1, 960)], 960)
        played = voip_tail(e.voip, 2)
        check(tone_db(played, 1000, 16000) > -24, f"the Echo plays the Opus tone ({tone_db(played, 1000, 16000):.1f} dB)")
        media.got.clear()
        await media.stream(to, 107, [enc(np.zeros(960, np.int16)) for _ in range(150)], 960)
        dec = opus_decoder(16000)
        sent = np.concatenate([dec(p) for pt, p, _ in media.got if pt == 107][-100:] or [np.zeros(0, np.int16)])
        check(len(media.got) > 120 and all(pt == 107 for pt, _, _ in media.got), f"the Echo sends Opus RTP as type 107 ({len(media.got)} packets)")
        check(db(sent) > -45, f"  its speech decodes ({db(sent):.1f} dBFS)")
        e.proc.send_signal(signal.SIGHUP)                                     # "<wake word>, stop"
        bye, _ = await pbx.expect(lambda m: m.req and m.method == "BYE" and m.get("Call-ID") == c.cid)
        check(bye is not None and bye.tag("From") == c.echo_tag and bye.tag("To") == c.tag, "\"stop\" hangs up: a BYE in the dialog (its tags)")
        if bye: pbx.respond(bye, 200, "OK", ("127.0.0.1", SIP))
        check(await until(lambda: e.phone()[0] == "idle", 2), "Phone idle")

        # 3. cancelled while it rings; 4. declined from Home Assistant
        c = Call(pbx); c.invite()
        check(await c.response(180) is not None, "ringing again")
        c.cancel()
        check(await c.response(200, "CANCEL") is not None and await c.response(487) is not None, "the PBX cancels: 200 to the CANCEL, 487 to the INVITE")
        c.ack()
        check(await until(lambda: e.phone()[0] == "idle", 2), "Phone idle")
        await asyncio.sleep(1.2)
        again = [m for m, _ in pbx.q if m.get("Call-ID") == c.cid and m.code == 487]
        check(not again, "  no 487 again after its ACK")
        c = Call(pbx); c.invite()
        check(await c.response(180) is not None, "ringing again")
        e.press("hang_up_phone")
        check(await c.response(603) is not None, "Hang up phone while it rings: 603 Decline")
        c.ack()
        r = await c.response(603, secs=1.2)
        check(r is None, "  not sent again after its ACK")

        # 5. do not disturb: unavailable
        e.page = Browser(WEB); check(e.page.login_with_button(e.proc), "settings page logged in")
        e.page.set(do_not_disturb="on")
        c = Call(pbx); c.invite()
        check(await c.response(480) is not None, "do not disturb: 480 Temporarily Unavailable")
        c.ack(); e.page.set(do_not_disturb="off")

        # 6. the Echo calls, with the assistant's microphone (call_mic): through the PBX's 407, ringing, answered, hung up
        r = e.page.set(call_mic="assistant")
        check(r.get("applied") == 1, f"call_mic=assistant on the page ({r})")
        mark = len(e.seq)
        await e.call("60")
        inv, a = await pbx.expect(lambda m: m.req and m.method == "INVITE")
        check(inv is not None and inv.uri == f"sip:60@127.0.0.1:{PBX}", f"phone_call 60: an INVITE to {inv.uri if inv else None}")
        sd = sdp_media(inv.body) if inv else None
        check(sd is not None and sd[2] == [111, 8, 0] and "opus/48000/2" in inv.body and "maxplaybackrate=16000" in inv.body,
              f"  offering Opus (16 kHz inside), A-law, mu-law ({sd})")
        check(await until(lambda: e.phone() == ("calling", "60"), 1), f"Phone calling 60 ({e.phone()})")
        pbx.respond(inv, 407, "Proxy Authentication Required", a, f'Proxy-Authenticate: Digest realm="{REALM}", nonce="{os.urandom(8).hex()}", algorithm=MD5\r\n', to_tag="t407")
        ack, _ = await pbx.expect(lambda m: m.req and m.method == "ACK")
        check(ack is not None and ack.cseq() == (inv.cseq()[0], "ACK") and ack.branch() == inv.branch() and ack.tag("To") == "t407",
              "  the 407 is ACKed in its transaction (same branch, CSeq, its To tag)")
        inv2, a = await pbx.expect(lambda m: m.req and m.method == "INVITE")
        check(inv2 is not None and inv2.cseq()[0] == inv.cseq()[0] + 1 and inv2.get("Call-ID") == inv.get("Call-ID"), "  then the INVITE again, CSeq + 1, same call")
        check(inv2 is not None and digest_ok(inv2.get("Proxy-Authorization") or "", "INVITE"), "  with Proxy-Authorization, the digest right (no qop)")
        pbx.respond(inv2, 100, "Trying", a)
        pbx.respond(inv2, 180, "Ringing", a, to_tag="callee")
        check(await until(lambda: e.results()[-1:] == ["ringing"], 2), f"Phone result: ringing ({e.results()[-1:]})")
        await asyncio.sleep(0.5)
        retx = [m for m, _ in pbx.q if m.req and m.method == "INVITE"]
        check(not retx, "  no INVITE again once it rings")
        answer = offer((("PCMA", 8),))
        pbx.respond(inv2, 200, "OK", a, f"Contact: <sip:60@127.0.0.1:{PBX}>\r\n", answer, to_tag="callee")
        ack, _ = await pbx.expect(lambda m: m.req and m.method == "ACK")
        check(ack is not None and ack.cseq() == (inv2.cseq()[0], "ACK") and ack.tag("To") == "callee" and ack.branch() != inv2.branch(),
              "the 200 is ACKed (its own transaction, the callee's tag)")
        check(await until(lambda: e.phone() == ("connected", "60") and e.results()[-1:] == ["connected"], 2), f"Phone connected, result connected ({e.phone()}, {e.results()[-1:]})")
        media.got.clear(); await asyncio.sleep(1)
        check(len(media.got) > 30 and all(pt == 8 for pt, _, _ in media.got), f"  A-law RTP to the answer's address ({len(media.got)} packets in 1 s)")
        await asyncio.sleep(1.5)
        sent = np.concatenate([alaw_dec(p) for pt, p, _ in media.got[-60:]]) if media.got else np.zeros(0)
        check(db(sent) > -45 and tone_db(sent, 700, 8000) < db(sent) - 10,
              f"  the assistant's microphone: the speech, not the call pipeline's tone ({db(sent):.1f} dBFS, 700 Hz {tone_db(sent, 700, 8000):.1f})")
        e.press("hang_up_phone")
        bye, a = await pbx.expect(lambda m: m.req and m.method == "BYE")
        check(bye is not None and bye.uri == f"sip:60@127.0.0.1:{PBX}" and bye.tag("To") == "callee", "Hang up phone: a BYE to the callee's Contact, in the dialog")
        if bye: pbx.respond(bye, 200, "OK", a)
        check(await until(lambda: e.phone() == ("idle", ""), 2), "Phone idle")
        sts = [v for o, v in e.seq[mark:] if o == "phone"]
        while sts[:1] == ["idle"]: sts.pop(0)              # a resend of the states (do not disturb's change) may come first
        check(sts == ["calling", "connected", "idle"], f"  states: {sts}")

        # 7. busy; 8. cancelled while it rings
        await e.call("61")
        inv, a = await pbx.expect(lambda m: m.req and m.method == "INVITE")
        pbx.respond(inv, 486, "Busy Here", a, to_tag="b")
        ack, _ = await pbx.expect(lambda m: m.req and m.method == "ACK")
        check(ack is not None and await until(lambda: e.results()[-1:] == ["busy"] and e.phone()[0] == "idle", 2), f"61 busy: ACKed, result busy ({e.results()[-1:]})")
        await e.call("62")
        inv, a = await pbx.expect(lambda m: m.req and m.method == "INVITE")
        pbx.respond(inv, 180, "Ringing", a, to_tag="r")
        check(await until(lambda: e.results()[-1:] == ["ringing"], 2), "62 rings")
        e.press("hang_up_phone")
        can, a2 = await pbx.expect(lambda m: m.req and m.method == "CANCEL")
        check(can is not None and can.branch() == inv.branch() and can.cseq() == (inv.cseq()[0], "CANCEL") and can.tag("To") is None,
              "Hang up phone while it rings: a CANCEL of that INVITE (its branch, CSeq, no To tag)")
        if can: pbx.respond(can, 200, "OK", a2); pbx.respond(inv, 487, "Request Terminated", a, to_tag="r")
        ack, _ = await pbx.expect(lambda m: m.req and m.method == "ACK")
        check(ack is not None and await until(lambda: e.results()[-1:] == ["cancelled"] and e.phone()[0] == "idle", 2), f"  the 487 ACKed, result cancelled ({e.results()[-1:]})")
        await e.call("not a number")
        check(await until(lambda: e.results()[-1:] == ["not_found"], 2), f"a number with a space is not called: result {e.results()[-1:]}")

        # 9. the settings page: the account, never the password
        ph = e.page.state().get("phone") or {}
        check(ph.get("line") == "registered" and ph.get("user") == USER and ph.get("server") == f"127.0.0.1:{PBX}" and ph.get("password") is True,
              f"the page: line, server, user, a password ({ {k: ph.get(k) for k in ('line', 'server', 'user', 'password')} })")
        _, _, raw = e.page.call("GET", "/api/state")
        check(PASS.encode() not in raw and PASS.encode().hex().encode() not in raw, "  the password never in what the page gets")
        def account(server, user, pw):
            def body(ctr):
                if pw in ("-", "="): p = pw
                else: b = pw.encode(); p = e.page.seal(ctr, b + bytes(-(len(b) + 1) % 64 + 1)).hex()
                return f"account {server.encode().hex()} {user.encode().hex()} {p}".encode()
            return e.page.call("POST", "/api/phone", body)
        n0 = len(pbx.regs); pbx.password = "n3w-one"
        st, _, _ = account(f"127.0.0.1:{PBX}", USER, "n3w-one")
        check(st == 200, f"a new password from the page ({st})")
        check(await until(lambda: any(m.get("Authorization") and digest_ok(m.get("Authorization"), "REGISTER", "n3w-one") for m in pbx.regs[n0:]), 5)
              and await until(lambda: e.st("phone_line") == "registered", 3), "  registered with it")
        check(any(m.get("Expires") == "0" for m in pbx.regs[n0:]), "  after unregistering the old binding (Expires: 0)")
        st, _, data = account(f"127.0.0.1:{PBX}", "bad user", "=")
        check(st == 409, f"a user with a space is refused ({st} {data[:60]})")
        n0 = len(pbx.regs); pbx.refuse = True
        account(f"127.0.0.1:{PBX}", USER, "wrong")
        check(await until(lambda: e.st("phone_line") == "refused", 5), f"a wrong password: Phone line refused ({e.st('phone_line')})")
        await asyncio.sleep(2)
        check(len(pbx.regs) - n0 <= 3, f"  and no flood of tries ({len(pbx.regs) - n0} REGISTERs)")
        pbx.refuse = False; pbx.password = PASS
        account(f"127.0.0.1:{PBX}", USER, PASS)
        check(await until(lambda: e.st("phone_line") == "registered", 5), "the right one again: registered")

        # 10. the feature off: unregistered, the entities gone
        n0 = len(pbx.regs)
        e.page.set(phone="off")
        check(await until(lambda: any(m.get("Expires") == "0" for m in pbx.regs[n0:]), 3), "the phone off: unregistered (Expires: 0)")
        await e.reconnect()
        check("phone" not in e.by and not any(s.name == "phone_call" for s in e.services), "  and its entities gone from Home Assistant")
        check("phone: send:" not in e.text() and "phone: RTP send:" not in e.text(), "no send errors on the way")
    finally:
        e.proc.terminate()
        try: e.proc.wait(5)
        except subprocess.TimeoutExpired: e.proc.kill()
        if check.failed: print(open(e.log).read()[-6000:])
    print("FAILED" if check.failed else "all passed")
    return 1 if check.failed else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
