#!/usr/bin/env python3
"""Plays Lyrion Music Server's side of SlimProto against build/hassmic-host (lyrion.c): its discovery, the control link on
3483, the HTTP streams it writes requests for and its JSON-RPC, as Lyrion 9.2.0 does them (the messages and their order
as a real one sent them to this player, 2026-11-17).  The player off until the page switches it on; HELO; the name;
the Echo's volume first, then Lyrion's, a fade that leaves the Echo's volume alone; PCM, FLAC (whole, and from the middle
of a file as a seek sends it), WAV by its header; gapless from one track to the next; a group's start at a time; pause,
the action button, stop; the setting off again.

Needs port 3483 (Lyrion's): run it through tests/run.py, which gives it a network namespace of its own."""
import asyncio, json, os, re, signal, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import numpy as np
from aiohttp import web
from webclient import wait_port, Browser

SLIM, HTTP, ESPHOME, WEB = 3483, 19000, 16120, 16121
MAC = "02:68:00:00:00:2a"
RATE = 44100

failed = False
def check(cond, what):
    global failed
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond: failed = True

async def until(cond, secs=5.0):
    t0 = time.monotonic()
    while True:
        r = cond()
        if r or time.monotonic() - t0 >= secs: return r
        await asyncio.sleep(0.05)

# ---------------------------------------------------------------- audio to stream

def tone(freq, secs, rate=RATE, amp=8000):
    k = np.arange(int(secs * rate))
    s = (amp * np.sin(2 * np.pi * freq * k / rate)).astype("<i2")
    return np.repeat(s, 2).reshape(-1, 2)

def crc8(b):
    c = 0
    for x in b:
        c ^= x
        for _ in range(8): c = ((c << 1) ^ 0x07) & 0xff if c & 0x80 else (c << 1) & 0xff
    return c

def crc16(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8): c = ((c << 1) ^ 0x8005) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
    return c

def utf8_num(n):                        # FLAC's frame number: UTF-8's coding of an integer
    if n < 0x80: return bytes([n])
    if n < 0x800: return bytes([0xc0 | n >> 6, 0x80 | n & 0x3f])
    return bytes([0xe0 | n >> 12, 0x80 | (n >> 6) & 0x3f, 0x80 | n & 0x3f])

def flac(pcm, block=4096, rate=RATE):
    """a FLAC stream of VERBATIM subframes (every FLAC decoder takes them): header, and the frames apart (a seek starts at one)"""
    n = len(pcm)
    si = struct.pack(">HH", block, block) + b"\0" * 6
    si += struct.pack(">Q", rate << 44 | (2 - 1) << 41 | (16 - 1) << 36 | n) + b"\0" * 16
    head = b"fLaC" + bytes([0x80, 0, 0, 34]) + si
    frames = []
    for i, at in enumerate(range(0, n, block)):
        blk = pcm[at:at + block]
        h = bytes([0xff, 0xf8, 0x79, 0x18]) + utf8_num(i) + struct.pack(">H", len(blk) - 1)   # 16-bit block size, 44.1 kHz; 2 ch, 16 bits
        h += bytes([crc8(h)])
        body = b"".join(b"\x02" + blk[:, c].astype(">i2").tobytes() for c in range(2))
        f = h + body
        frames.append(f + struct.pack(">H", crc16(f)))
    return head, frames

def wav(pcm, rate):
    d = pcm.astype("<i2").tobytes()
    return b"RIFF" + struct.pack("<I", 36 + len(d)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16) + b"data" + struct.pack("<I", len(d)) + d

# ---------------------------------------------------------------- the fake Lyrion

class Lyrion:
    def __init__(self):
        self.streams = {}; self.rpcs = []; self.events = []; self.helo = None; self.names = []; self.resp = []
        self.w = None; self.conn = asyncio.Event(); self.closed = asyncio.Event(); self.got = asyncio.Condition(); self.links = 0

    # discovery: "e" + TLVs asked, "E" + TLVs back (Slim::Networking::Discovery)
    def datagram_received(self, data, addr):
        if not data.startswith(b"e"): return
        tlv = lambda t, v: t + bytes([len(v)]) + v
        self.disc_from = addr
        self.udp.sendto(b"E" + tlv(b"NAME", b"fake-lms") + tlv(b"IPAD", b"127.0.0.1") + tlv(b"JSON", str(HTTP).encode()) + tlv(b"VERS", b"9.2.0"), addr)
    def connection_made(self, t): self.udp = t
    def error_received(self, e): pass

    async def client(self, r, w):
        self.w = w; self.closed.clear(); self.conn.set(); self.links += 1
        try:
            while True:
                h = await r.readexactly(8)
                op, n = h[:4].decode(), struct.unpack(">I", h[4:])[0]
                d = await r.readexactly(n)
                async with self.got:
                    if op == "HELO": self.helo = d
                    elif op == "STAT": self.events.append((d[:4].decode(), d, time.monotonic()))
                    elif op == "SETD": self.names.append(d[1:].rstrip(b"\0").decode())
                    elif op == "RESP": self.resp.append(d.decode(errors="replace"))
                    elif op == "DSCO": self.events.append(("DSCO", d, time.monotonic()))
                    self.got.notify_all()
        except (asyncio.IncompleteReadError, ConnectionError): pass
        self.conn.clear(); self.closed.set()

    def send(self, op, payload=b""):
        m = op.encode() + payload
        self.w.write(struct.pack(">H", len(m)) + m)

    def strm(self, cmd, autostart=b"1", fmt=b"p", size=b"1", rate=b"3", ch=b"2", endian=b"1", arg=0, path=b"", threshold=1, out_threshold=1):
        req = b"GET " + path + b" HTTP/1.0\r\n\r\n" if path else b""
        self.send("strm", cmd + autostart + fmt + size + rate + ch + endian + bytes([threshold, 0, 0, 0, 0, out_threshold, 0])
                  + struct.pack(">IHI", arg, HTTP if path else 0, 0) + req)

    def audg(self, v, adjust=1):
        g = lyrion_gain16(v)
        self.send("audg", struct.pack(">IIBBII", VOLUME_MAP[v], VOLUME_MAP[v], adjust, 255, g, g))

    async def wait(self, name, after=0, secs=5.0):
        """the first status event `name` with index >= after: its index"""
        t0 = time.monotonic()
        async with self.got:
            while True:
                for i in range(after, len(self.events)):
                    if self.events[i][0] == name: return i
                left = secs - (time.monotonic() - t0)
                if left <= 0: return None
                try: await asyncio.wait_for(self.got.wait(), left)
                except asyncio.TimeoutError: return None

    # HTTP: the streams, and JSON-RPC
    async def http_stream(self, req):
        body = self.streams[req.query["t"]]
        resp = web.StreamResponse(headers={"Content-Type": "audio/x-flac"}); await resp.prepare(req)
        for at in range(0, len(body), 65536): await resp.write(body[at:at + 65536])
        return resp
    async def http_rpc(self, req):
        self.rpcs.append(json.loads(await req.text())["params"])
        return web.json_response({"result": {}})

# Lyrion's sums (Slim::Player::SqueezePlay, Squeezebox2): what audg carries for a volume (tests/unit/lyrion_test.c too)
VOLUME_MAP = [0, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5, 5, 6, 6, 7, 8, 9, 9, 10, 11, 12, 13, 14, 15, 16, 16, 17, 18, 19, 20, 22, 23, 24,
              25, 26, 27, 28, 29, 30, 32, 33, 34, 35, 37, 38, 39, 40, 42, 43, 44, 46, 47, 48, 50, 51, 53, 54, 56, 57, 59, 60, 61,
              63, 65, 66, 68, 69, 71, 72, 74, 75, 77, 79, 80, 82, 84, 85, 87, 89, 90, 92, 94, 96, 97, 99, 101, 103, 104, 106,
              108, 110, 112, 113, 115, 117, 119, 121, 123, 125, 127, 128]
def lyrion_gain16(v):
    if v <= 0: return 0
    db = (v - 100) * 37 / 75 if v > 25 else v * 37 / 25 - 74
    m = 10 ** (db / 20)
    return int(m * 256 + 0.5) * 256 if -30 <= db <= 0 else int(m * 65536 + 0.5)

def stat_fields(d):
    """jiffies and elapsed ms of a STAT"""
    return struct.unpack(">I", d[25:29])[0], struct.unpack(">I", d[43:47])[0]

def played(path, rate=RATE):
    """what reached the music backend: the left channel, as floats"""
    pcm = np.frombuffer(open(path, "rb").read(), dtype="<i2") if os.path.exists(path) else np.zeros(0, "<i2")
    return pcm[: len(pcm) // 2 * 2].reshape(-1, 2)[:, 0].astype(float)

def freq_at(x, at, rate=RATE):
    seg = x[at:at + rate // 4]
    return int(np.argmax(np.abs(np.fft.rfft(seg * np.hanning(len(seg))))) * rate / len(seg)) if len(seg) == rate // 4 else 0

async def main():
    lms = Lyrion()
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(lambda: lms, local_addr=("127.0.0.1", SLIM))
    srv = await asyncio.start_server(lms.client, "127.0.0.1", SLIM)
    app = web.Application(); app.router.add_get("/stream", lms.http_stream); app.router.add_post("/jsonrpc.js", lms.http_rpc)
    runner = web.AppRunner(app); await runner.setup(); await web.TCPSite(runner, "127.0.0.1", HTTP).start()

    state = tempfile.mkdtemp(prefix="hassmic-lms-")
    music = os.path.join(state, "music.raw")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_MUSIC=music, HASSMIC_LYRION_MAC=MAC, HASSMIC_LYRION_DISCOVERY="127.0.0.1",
               HASSMIC_OUTPUT_LATENCY_MS="0")
    path = os.path.join(state, "log"); open(path, "w").close(); logf = open(path, "a")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-p", str(ESPHOME), "-z", "0", "-L", "-o", "0", "-a", "0", "-i", "0", "-W", str(WEB), "-n", "Echo Test"],
                            env=env, stderr=logf)
    log = lambda: open(path).read()
    try:
        wait_port(ESPHOME, WEB)
        page = Browser(WEB)
        check(page.login_with_button(proc), "settings page: logged in with the action button")
        st = page.state()
        check(st and st["lyrion"]["on"] is False and st["lyrion"]["state"] == "off", f"the player off by default: {st and st['lyrion']}")
        await asyncio.sleep(1.5)
        check(not lms.conn.is_set(), "off, it neither looks for Lyrion nor links")
        check(page.set(lyrion="on").get("status", 200) == 200, "switched on from the page")
        check(await until(lambda: lms.helo is not None, 8), "on: found by its discovery, linked, HELO")

        # HELO: SqueezePlay's device id, our MAC, Squeezelite's capabilities with the model's name
        h = lms.helo or bytes(64)
        caps = h[36:].decode(errors="replace")
        check(h[0] == 12 and h[2:8] == bytes.fromhex(MAC.replace(":", "")), f"HELO: device 12, the player's id {MAC}")
        for c in ("Model=squeezelite", "AccuratePlayPoints=1", "MaxSampleRate=48000", ",flc", ",pcm", ",mp3"):
            check(c in caps, f"HELO capabilities have {c}")
        check(re.search(r"ModelName=Echo Dot \d \(hassmic\)", caps) is not None, f"HELO names the model: {caps}")
        st = (await asyncio.to_thread(page.state))["lyrion"]
        check(st["state"] == "up" and st["name"] == "fake-lms" and st["server"] == f"127.0.0.1:{HTTP}" and st["id"] == MAC,
              f"the page: connected to fake-lms at its web port, the player's id shown: {st}")

        lms.send("setd", b"\0")
        check(await until(lambda: lms.names[-1:] == ["Echo Test"]), f"setd 0 asked: the Echo's own name ({lms.names})")
        lms.send("setd", b"\0Kitchen\0")
        check(await until(lambda: lms.names[-1:] == ["Kitchen"] and os.path.exists(os.path.join(state, "lyrion.name"))),
              "renamed in Lyrion: kept, and said back")

        # volume: the link's first audg is answered with the Echo's volume; after that Lyrion's is the Echo's once it stays
        echo_vol = (await asyncio.to_thread(page.state))["volume"]
        lms.audg(77)
        check(await until(lambda: any(p[1][:2] == ["mixer", "volume"] for p in lms.rpcs)), "the link's first volume: the Echo's goes to Lyrion")
        check([p for p in lms.rpcs if p[1][:2] == ["mixer", "volume"]][:1] == [[MAC, ["mixer", "volume", str(echo_vol)]]],
              f"  as mixer volume {echo_vol} for this player: {lms.rpcs}")
        lms.audg(47)                                    # 47 and 48 share a gain: the old-style one tells them apart
        check(await until(lambda: (page.state() or {}).get("volume") == 47, 4), "Lyrion's volume 47 is the Echo's once it stays put")
        n_rpc = len(lms.rpcs); mark = len(log())
        for v in (40, 33, 25, 18, 10, 4):               # a pause's fade: 50 ms steps down, then the gain back
            lms.audg(v); await asyncio.sleep(0.05)
        lms.send("audg", struct.pack(">IIBBII", 0, 0, 1, 255, 0, 0)); await asyncio.sleep(0.05)
        lms.audg(47); await asyncio.sleep(1.2)
        check("volume:" not in log()[mark:] and (await asyncio.to_thread(page.state))["volume"] == 47, "a fade leaves the Echo's volume where it was")
        check(len(lms.rpcs) == n_rpc, "  and nothing went back to Lyrion for it")

        # PCM 440 Hz, then FLAC 660 Hz behind it once STMd says the first is decoded: one stream of music, no gap
        a = tone(440, 2.0); b = tone(660, 2.0)
        lms.streams["1"] = a.astype("<i2").tobytes()
        head, frames = flac(b); lms.streams["2"] = head + b"".join(frames)
        ev0 = len(lms.events)
        lms.strm(b"s", path=b"/stream?t=1")
        for e in ("STMc", "STMs", "STMd", "DSCO"):
            check(await lms.wait(e, ev0) is not None, f"track 1 (PCM): {e}")
        check(any(re.match(r"HTTP/1\.[01] 200 ", r) for r in lms.resp), f"  RESP carries the stream's HTTP header: {lms.resp[:1]}")
        lms.strm(b"s", fmt=b"f", size=b"?", rate=b"?", ch=b"?", endian=b"?", path=b"/stream?t=2")
        i_s1 = await lms.wait("STMs", ev0)
        i_s2 = await lms.wait("STMs", i_s1 + 1, 6) if i_s1 is not None else None
        check(i_s2 is not None, "track 2 (FLAC): STMs as its first frames go out")
        await asyncio.sleep(1.2)                        # STMt: the play point every second, elapsed as heard
        ts = [stat_fields(e[1]) for e in lms.events[i_s2 or 0:] if e[0] == "STMt"]
        els = [e for _, e in ts]
        check(len(ts) >= 1 and els == sorted(els) and 500 < els[-1] <= 2100, f"STMt: track 2's elapsed time in it, growing: {ts}")
        check(await lms.wait("STMu", ev0, 6) is not None, "both played out: STMu")
        x = played(music)
        nz = np.flatnonzero(np.abs(x) > 100); x = x[nz[0]:nz[-1] + 1] if len(nz) else x
        check(abs(len(x) / RATE - 4.0) < 0.01, f"4 s of music out (2 s PCM + 2 s FLAC): {len(x) / RATE:.3f} s")
        check(freq_at(x, RATE // 2) == 440 and freq_at(x, int(RATE * 2.5)) == 660, "  440 Hz, then 660 Hz")
        quiet = np.abs(x) < 50                          # a sine passes 0 in a sample or two; a gap is a run
        runs = np.diff(np.flatnonzero(np.diff(np.concatenate([[0], quiet.astype(int), [0]]))))[::2] if quiet.any() else np.zeros(1)
        check(runs.max() <= 3, f"  no gap at the join (longest quiet run {int(runs.max())} samples)")

        # FLAC from the middle of a file (a seek: frames, no "fLaC"), and a 48 kHz WAV by its header
        os.truncate(music, 0)
        lms.streams["3"] = b"".join(frames[10:])
        ev0 = len(lms.events)
        lms.strm(b"s", fmt=b"f", size=b"?", rate=b"?", ch=b"?", endian=b"?", path=b"/stream?t=3")
        check(await lms.wait("STMu", ev0, 6) is not None and "from the middle" in log(), "FLAC from the middle of a file: played (STMu)")
        lms.streams["4"] = wav(tone(880, 1.0, 48000), 48000)
        ev0 = len(lms.events)
        lms.strm(b"s", fmt=b"p", size=b"?", rate=b"?", ch=b"?", endian=b"?", path=b"/stream?t=4")
        check(await lms.wait("STMu", ev0, 6) is not None and "output open, 48000 Hz" in log(), "WAV by its header: 48 kHz, played")

        # a group's start: autostart 0, STMl, then strm u at a time in the player's jiffies
        lms.streams["5"] = tone(440, 3.0).tobytes()
        ev0 = len(lms.events)
        lms.strm(b"s", autostart=b"0", path=b"/stream?t=5")
        il = await lms.wait("STMl", ev0)
        check(il is not None, "autostart 0: STMl once buffered, and it waits")
        await asyncio.sleep(0.3)
        check(await lms.wait("STMs", ev0, 0.1) is None, "  nothing played before strm u")
        jif = stat_fields(lms.events[il][1])[0] if il is not None else 0
        t_u = time.monotonic()
        lms.strm(b"u", arg=(jif + 300 + 600) & 0xffffffff)        # 600 ms from the STMl's stamp, which is ~300 ms old
        check(await lms.wait("STMr", ev0) is not None, "  strm u: STMr")
        i_s = await lms.wait("STMs", ev0, 3)
        took = lms.events[i_s][2] - t_u if i_s is not None else -1
        check(0.3 < took < 1.0, f"  started at its time, not before: {took:.2f} s after strm u")

        # pause (strm p), the action button resumes through JSON-RPC; while playing it pauses
        await asyncio.sleep(0.3)
        ev0 = len(lms.events); lms.strm(b"p", arg=0)
        check(await lms.wait("STMp", ev0) is not None, "strm p: STMp")
        n_rpc = len(lms.rpcs); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: [MAC, ["pause", "0"]] in lms.rpcs[n_rpc:]), "action button while paused: Lyrion asked to play")
        lms.strm(b"u", arg=0)
        check(await lms.wait("STMr", ev0) is not None, "strm u: STMr")
        await asyncio.sleep(0.3)
        n_rpc = len(lms.rpcs); proc.send_signal(signal.SIGUSR2)
        check(await until(lambda: [MAC, ["pause", "1"]] in lms.rpcs[n_rpc:]), "action button while playing: Lyrion asked to pause")
        ev0 = len(lms.events); mark = len(log()); lms.strm(b"q")
        check(await lms.wait("STMf", ev0) is not None and await until(lambda: "lyrion: output closed" in log()[mark:]), "strm q: STMf, the output closed")
        lms.strm(b"t", arg=0x12345678)
        it = await lms.wait("STMt", ev0)
        check(it is not None and lms.events[it][1][47:51] == struct.pack(">I", 0x12345678), "strm t: STMt with the server's stamp as it came")

        # the page's address: kept, and the player links there
        st, _, data = page.call("POST", "/api/lyrion", "127.0.0.1".encode().hex().encode())
        check(st == 200 and json.loads(data)["given"] == "127.0.0.1", f"the page's address saved: {data[:120]}")
        links = lms.links
        check(await until(lambda: lms.links > links, 8), "  the player links again, to it")
        check(open(os.path.join(state, "lyrion.server")).read().strip() == "127.0.0.1", "  in state/lyrion.server")
        st, _, data = page.call("POST", "/api/lyrion", b"6c6d733b726d")          # "lms;rm"
        check(st == 400, "an address with other characters refused")

        # off again: the link goes
        lms.closed.clear()
        page.set(lyrion="off")
        check(await until(lambda: lms.closed.is_set(), 3), "switched off: the link closed")
        check("lyrion=off" in open(os.path.join(state, "config")).read(), "  kept in state/config")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
        if failed: print(open(path).read()[-6000:])
    sys.exit(1 if failed else 0)

asyncio.run(main())
