#!/usr/bin/env python3
"""Music Assistant's own API as hassmic uses it for the screen's music pages (mass.c), against build/hassmic-host-screen
with a stand-in UI (HASSMIC_SCREEN, as in tests/fake_screen.py) and a stand-in Music Assistant server (aiohttp: the
websocket API at /ws, its imageproxy) that answers as Music Assistant 2.10 does (models from music-assistant-models):
  - the settings page (web.c /api/mass): address and token from a logged-in browser, the token sealed, never in
    /api/state, kept in state/mass 0600; unsigned or unsealed: refused; the token kept for another address, https:
    refused
  - the Screen page (/api/screen): the apps the UI listed, the home app picked there (state/screen_home) to the UI
  - a token refused (or a login answered false): "ma auth"; the right one: "ma up", the server's info first, "auth"
    before anything else; idle past the receive timeout on a server with no heartbeat: kept by the Echo's own pings
  - the players: this Echo found by Sendspin's client id, or as the output protocol of the universal player around it
    (that one, shown, and it stays: no flipping between the two; found after 140 protocol players, which are not
    kept, and 140 speakers, more than it keeps), who may join its group (can_group_with), group
    players flagged, hidden, private, protocol and light players left out; "pl." once all are told
  - this Echo's queue (get_active_queue): "pq"; queue_updated events change it, queue_items_updated: "qc"
  - player_updated events: a line only when what the UI shows changes (not for every second of play)
  - lists: the library page by page (limit/offset, a short page ends it), an album's tracks by its URI, search
    results grouped (artists, albums, tracks...), the queue with the current item marked; subtitles as the UI wants;
    a long listing in pieces ("partial", as Music Assistant's async generators answer) told whole; a playlist asked
    whole before schema 93 (no paging there); an answer over 4 MB that list's error, the connection kept
  - the UI's commands: play_media (play/next/add; radio as a radio_playlist:// of the item, from schema 34), play_index (a position or an item's id; one past any queue refused), delete_item, clear, shuffle, repeat,
    set_members (add/remove, the older group/ungroup where set_members is unknown), ungroup, volume_set,
    group_volume, transfer
  - images: the key asked for at the UI's size comes from /imageproxy/<its proxy_id>?size=160&fmt=jpeg, with the token
  - a UI that hangs while the Echo is idle (nothing to write to it): its pings unanswered, it is started again
  - Sendspin's server suggests its address (the page's "found") once paired only, never unpaired (any host on the LAN,
    with unpaired access on); the page's address set, nothing at all goes to the suggested one, nor is the token logged
  - the connection lost: a list still waiting ends with an error, players go, "ma error", and it comes back
"""
import asyncio, json, os, re, subprocess, sys, tempfile, threading, time

if len(sys.argv) > 1 and sys.argv[1] == "--ui":           # the stand-in UI, as hassmic runs it
    log, fifo = os.environ["FAKE_UI_LOG"], os.environ["FAKE_UI_FIFO"]
    with open(log, "ab", buffering=0) as f:
        f.write(f"#start band={os.environ.get('HMUI_BAND')}\n".encode())
        out = threading.Lock()
        def tell(b):
            with out: sys.stdout.buffer.write(b); sys.stdout.buffer.flush()
        hung = threading.Event()
        def answer():                                    # bytes as they come (the test sends some that are no UTF-8)
            while True:
                with open(fifo, "rb") as q:
                    for line in q:
                        if line == b"#hang\n": hung.set(); continue   # (for the test: alive, reading nothing)
                        tell(line)
        threading.Thread(target=answer, daemon=True).start()
        tell(b"ready 960 470\n")
        carry = b""
        while True:
            if hung.is_set(): time.sleep(1); continue
            b = sys.stdin.buffer.read1(65536)
            if not b: break
            f.write(b)
            buf = carry + b                                   # hassmic's pings, answered as the UI's main loop does
            for m in re.finditer(rb"ping (\d+)\n", buf): tell(b"pong " + m.group(1) + b"\n")   # (also right after a blob)
            carry = buf[-24:]
    sys.exit(0)

import io, stat
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser, req
from aiohttp import web, WSMsgType
from PIL import Image
from aiosendspin import noise
from aiosendspin.noise import Identity, InMemoryServerPairingStore
from aiosendspin.noise.pairing import PairingAttempt
from aiosendspin.models.types import PairMethod
from aiosendspin.server import SendspinServer, ClientAddedEvent

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MA_PORT, SS_PORT, WEB = 18095, 16978, 28941
GOOD = "eyJhbGciOiJIUzI1NiJ9." + "x" * 300 + ".sig"              # long, as Music Assistant's JWTs are


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def lines(path):
    """the text lines the UI got, images cut out (img <key> <n> <size> / art <n> + n bytes)"""
    b = open(path, "rb").read() if os.path.exists(path) else b""
    out, i = [], 0
    while i < len(b):
        j = b.find(b"\n", i)
        if j < 0: break
        l = b[i:j].decode("utf-8", "replace"); i = j + 1
        out.append(l)
        m = re.fullmatch(r"img \S+ (\d+)(?: \d+)?", l) or re.fullmatch(r"art (\d+)", l)
        if m: out.append(("BYTES", b[i:i + int(m.group(1))])); i += int(m.group(1))
        m = re.fullmatch(r"band (\d+)", l)
        if m: i += 4 * int(m.group(1))
    return out


def text(path): return [l for l in lines(path) if isinstance(l, str)]


async def until(cond, secs):
    t0 = time.monotonic()
    while not cond() and time.monotonic() - t0 < secs: await asyncio.sleep(0.05)
    return cond()


def album(i):
    return {"item_id": str(i), "provider": "library", "name": f"Album {i:02d}", "version": "", "media_type": "album", "uri": f"library://album/{i}",
            "year": 1990 + i % 30, "favorite": False, "artists": [{"item_id": "1", "provider": "library", "name": "Björk", "media_type": "artist", "uri": "library://artist/1"}],
            "metadata": {"images": [{"type": "thumb", "path": f"/music/a{i}.jpg", "provider": "filesystem", "remotely_accessible": False, "proxy_id": f"px{i}"}]},
            "provider_mappings": [{"item_id": str(i), "provider_domain": "filesystem", "provider_instance": "fs", "available": True}]}


def track(i, alb=None):
    return {"item_id": str(100 + i), "provider": "library", "name": f"Song {i}", "version": "Live" if i == 2 else "", "media_type": "track",
            "uri": f"library://track/{100 + i}", "duration": 200 + i, "artists": [{"name": "Björk"}, {"name": "Thom"}],
            "album": {"item_id": "3", "name": "Homogenic", "media_type": "album", "uri": "library://album/3",
                      "image": {"type": "thumb", "path": "/music/hom.jpg", "provider": "filesystem", "remotely_accessible": False, "proxy_id": "hom"}},
            "metadata": {"images": []}}


class FakeMA:
    def __init__(self):
        self.cmds, self.ws, self.bad_set_members, self.img_auth, self.img_ids = [], None, False, [], []
        self.self_id, self.merged, self.stall, self.extra = None, False, False, 0
        self.queue = {"queue_id": "", "active": True, "display_name": "Kitchen + 1", "available": True, "items": 12, "shuffle_enabled": False,
                      "repeat_mode": "off", "current_index": 3, "state": "playing", "elapsed_time": 42}

    def players(self):
        me = self.self_id
        if self.merged:                                  # Music Assistant 2.10's own: the Sendspin player a protocol player
            return [{"player_id": f"proto{i}", "provider": "airplay", "type": "protocol", "name": f"AirPlay {i}", "available": True,
                     "enabled": True, "hide_in_ui": False, "playback_state": "idle", "group_members": [], "can_group_with": []}
                    for i in range(self.extra)] + [{"player_id": f"spk{i}", "provider": "sonos", "type": "player", "name": f"Speaker {i}",
                     "available": True, "enabled": True, "hide_in_ui": False, "playback_state": "idle", "volume_level": 20,
                     "group_members": [], "can_group_with": []} for i in range(self.extra)] + [   # (a big house's: more speakers
                                                                                                  # than the Echo keeps, this one last)
                {"player_id": me, "provider": "sendspin", "type": "protocol", "name": "Kitchen Show (Sendspin)", "available": True, "enabled": True,
                 "hide_in_ui": False, "playback_state": "playing", "volume_level": 45, "volume_control": "native", "group_members": [],
                 "can_group_with": [], "output_protocols": []},
                {"player_id": "up_kitchen", "provider": "universal_player", "type": "player", "name": "Kitchen Show", "available": True, "enabled": True,
                 "hide_in_ui": False, "playback_state": "playing", "volume_level": 45, "volume_control": "native", "group_members": [],
                 "can_group_with": ["dot2"], "output_protocols": [{"output_protocol_id": me, "name": "Sendspin", "protocol_domain": "sendspin"}]},
                {"player_id": "dot2", "provider": "sendspin", "type": "player", "name": "Living Dot", "available": True, "enabled": True,
                 "hide_in_ui": False, "playback_state": "idle", "volume_level": 30, "volume_control": "native", "group_members": [], "can_group_with": []},
                {"player_id": "anchor", "provider": "builtin", "type": "player", "name": "Internal", "available": True, "enabled": True,
                 "hide_in_ui": False, "private": True, "playback_state": "idle", "volume_level": 0, "group_members": [], "can_group_with": []},
                {"player_id": "hue", "provider": "hue", "type": "light", "name": "Hue sync", "available": True, "enabled": True,
                 "hide_in_ui": False, "playback_state": "idle", "group_members": [], "can_group_with": []},
            ]
        return [
            {"player_id": me, "provider": "sendspin", "type": "player", "name": "Kitchen Show", "available": True, "enabled": True,
             "hide_in_ui": False, "playback_state": "playing", "volume_level": 45, "volume_muted": False, "volume_control": "native",
             "group_members": [me, "dot2"], "group_volume": 38, "synced_to": None, "active_group": None, "can_group_with": ["dot2", "sonos-1", "sendspin"],
             "current_media": {"title": "Jóga", "artist": "Björk"}, "output_protocols": []},
            {"player_id": "dot2", "provider": "sendspin", "type": "player", "name": "Living Dot", "available": True, "enabled": True,
             "hide_in_ui": False, "playback_state": "playing", "volume_level": 30, "volume_control": "native", "group_members": [],
             "synced_to": me, "can_group_with": [], "current_media": None},
            {"player_id": "sonos-1", "provider": "sonos", "type": "player", "name": "Sonos Move", "available": True, "enabled": True,
             "hide_in_ui": False, "playback_state": "paused", "volume_level": 22, "volume_control": "native", "group_members": [],
             "synced_to": None, "can_group_with": [], "current_media": {"title": "Teardrop", "artist": "Massive Attack"}},
            {"player_id": "airplay-x", "provider": "airplay", "type": "player", "name": "Old AirPlay", "available": True, "enabled": True,
             "hide_in_ui": True, "playback_state": "idle", "volume_level": 10, "group_members": [], "can_group_with": []},
            {"player_id": "all", "provider": "player_group", "type": "group", "name": "Whole house", "available": True, "enabled": True,
             "hide_in_ui": False, "playback_state": "idle", "volume_level": 50, "volume_control": "native", "group_members": [], "can_group_with": []},
        ]

    async def send(self, obj):
        if self.ws is not None and not self.ws.closed: await self.ws.send_str(json.dumps(obj))

    async def handle(self, request):
        ws = web.WebSocketResponse(); await ws.prepare(request); self.ws = ws; self.req = request
        await ws.send_str(json.dumps({"server_id": "fake-ma", "server_version": "2.10.8", "schema_version": 65, "min_supported_schema_version": 24,
                                      "base_url": f"http://127.0.0.1:{MA_PORT}", "homeassistant_addon": False, "onboard_done": True}))
        authed = False
        async for msg in ws:
            if msg.type != WSMsgType.TEXT: continue
            m = json.loads(msg.data); mid, cmd, a = m["message_id"], m["command"], m.get("args") or {}
            self.cmds.append((cmd, a))
            if cmd == "auth":
                if a.get("token") == GOOD: authed = True; await ws.send_str(json.dumps({"message_id": mid, "result": {"authenticated": True, "user": {"username": "me"}}}))
                elif a.get("token") == "falsy": await ws.send_str(json.dumps({"message_id": mid, "result": False}))   # (no error, no login)
                else: await ws.send_str(json.dumps({"message_id": mid, "error_code": 20, "details": "Invalid or expired token"}))
                continue
            if not authed:
                await ws.send_str(json.dumps({"message_id": mid, "error_code": 20, "details": "Authentication required"})); continue
            if self.stall: continue                          # an answer that never comes
            r = await self.answer(cmd, a)
            if isinstance(r, Exception): await ws.send_str(json.dumps({"message_id": mid, "error_code": 999, "details": str(r)}))
            elif cmd == "music/podcasts/podcast_episodes":            # an async generator: 500 a message, "partial", then the rest
                while len(r) > 500: await ws.send_str(json.dumps({"message_id": mid, "result": r[:500], "partial": True})); r = r[500:]
                await ws.send_str(json.dumps({"message_id": mid, "result": r}))
            else: await ws.send_str(json.dumps({"message_id": mid, "result": r}))
        return ws

    async def answer(self, cmd, a):
        if cmd == "players/all": return self.players()
        if cmd == "player_queues/get_active_queue": self.queue["queue_id"] = a["player_id"]; return self.queue
        if cmd == "music/albums/library_items":
            n = 75; return [album(i) for i in range(a["offset"], min(n, a["offset"] + a["limit"]))]
        if cmd == "music/albums/album_tracks": return [track(i) for i in range(5)] if a["item_id"] == "3" and a["provider_instance_id_or_domain"] == "library" else Exception("no such album")
        if cmd == "music/playlists/playlist_tracks": return [track(i) for i in range(70)]   # schema < 93: no paging, all of it
        if cmd == "music/artists/artist_tracks" and a.get("item_id") == "tiny":       # 300 000 values: under 4 MB, too many
            return [0] * 300000
        if cmd == "music/artists/artist_tracks" and a.get("item_id") == "long":       # a URI longer than the UI's lines take
            return [dict(track(1), uri="filesystem_local://track/" + "x" * 300), track(2)]
        if cmd == "music/artists/artist_tracks" and a.get("item_id") == "huge":       # a big artist's, whole: over 4 MB
            return [dict(track(i), metadata={"description": "x" * 2000, "images": []}) for i in range(2500)]
        if cmd == "music/podcasts/podcast_episodes":
            return [{"name": f"Episode {i}", "media_type": "podcast_episode", "uri": f"library://podcast_episode/{i}", "duration": 1800,
                     "podcast": {"name": "The Show"}} for i in range(1100)]
        if cmd == "music/search":
            return {"artists": [{"name": "Björk", "media_type": "artist", "uri": "library://artist/1", "metadata": {"images": []}}],
                    "albums": [album(3)], "tracks": [track(1), track(2)], "playlists": [], "radio": [{"name": "FIP", "media_type": "radio", "uri": "radiobrowser://radio/fip"}]}
        if cmd == "player_queues/items":
            return [{"queue_id": a["queue_id"], "queue_item_id": f"qi{i}", "name": f"Björk - Song {i}", "duration": 180 + i, "sort_index": i,
                     "media_item": track(i), "image": {"type": "thumb", "path": "/q.jpg", "provider": "filesystem", "proxy_id": f"q{i}"}, "index": i, "available": i != 5}
                    for i in range(a["offset"], 8)]
        if cmd == "players/cmd/set_members" and self.bad_set_members: return Exception("Invalid command: players/cmd/set_members")
        return None

    async def image(self, request):
        self.img_auth.append(request.headers.get("Authorization", "")); self.img_ids.append(request.match_info["id"])
        size = int(request.query.get("size", "0"))
        if size not in (0, 80, 160, 256, 512, 1024): return web.Response(status=400)
        b = io.BytesIO(); Image.new("RGB", (size or 300, size or 300), (200, 30, 90)).save(b, "JPEG")
        return web.Response(body=b.getvalue(), content_type="image/jpeg")


async def main():
    state = tempfile.mkdtemp(); log = os.path.join(state, "ui.log"); fifo = os.path.join(state, "ui.fifo")
    os.mkfifo(fifo)
    runner = os.path.join(state, "ui.sh")
    # the stand-in UI's command line names this run's state: a pkill of it never hits another test's (tests/run.py -j)
    me_ui = f"{os.path.abspath(__file__)} --ui {state}"
    open(runner, "w").write(f"#!/bin/sh\nexec {sys.executable} {me_ui}\n"); os.chmod(runner, 0o755)
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_MUSIC=os.path.join(state, "music.raw"), HASSMIC_SETTINGS=os.path.join(state, "settings"),
               HASSMIC_SCREEN=runner, FAKE_UI_LOG=log, FAKE_UI_FIFO=fifo, HASSMIC_MASS_QUIET_S="8",   # (no server heartbeat here)
               HASSMIC_SCREEN_HUNG_MS="2000")
    open(os.path.join(state, "adopted"), "w").close()       # set up: no setup spinner, the band dark (an idle Echo)
    open(os.path.join(state, "sendspin.unpaired"), "w").write("1")   # unpaired access on: a Sendspin server below
    ma = FakeMA()
    app = web.Application(); app.router.add_get("/ws", ma.handle); app.router.add_get(r"/imageproxy/{id}", ma.image)
    run = web.AppRunner(app); await run.setup(); site = web.TCPSite(run, "127.0.0.1", MA_PORT); await site.start()
    errlog = []

    def start():
        p = subprocess.Popen([f"{ROOT}/build/hassmic-host-screen", "-p", "16979", "-z", str(SS_PORT), "-L", "-W", str(WEB), "-o", "0"], env=env,
                             stderr=subprocess.PIPE, text=True, errors="replace")
        threading.Thread(target=lambda: [errlog.append(l.rstrip()) for l in p.stderr], daemon=True).start()
        return p

    def say(*ls):                                     # lines the UI says, in one write: a fifo reopened per line races its reader
        for _ in range(20):
            try:
                with open(fifo, "w") as q: q.write("".join(l + "\n" for l in ls)); return
            except BrokenPipeError: time.sleep(0.05)

    def cmds(name): return [a for c, a in ma.cmds if c == name]
    proc = start()
    try:
        await until(lambda: any(re.search(r"sendspin: player (\S+) on", l) for l in errlog), 10)
        ma.self_id = next(re.search(r"sendspin: player (\S+) on", l).group(1) for l in errlog if re.search(r"sendspin: player (\S+) on", l))
        wait_port(WEB)
        b = await asyncio.to_thread(Browser, WEB)
        check(await asyncio.to_thread(b.login_with_button, proc), "logged in with the action button")
        st0 = (await asyncio.to_thread(b.state)).get("mass")
        check(st0 and st0["state"] == "off" and not st0["token"] and st0["url"] == "", f"nothing set, no server seen: off ({st0})")
        url = f"http://127.0.0.1:{MA_PORT}".encode().hex()
        def post(tok):
            return b.call("POST", "/api/mass", lambda c: f"{url} {b.seal(c, tok.encode() + bytes(64 - len(tok) % 64)).hex() if tok not in ('-', '=') else tok}".encode())
        check(req(WEB, "POST", "/api/mass", f"{url} -".encode())[0] == 401, "unsigned: refused")
        st, _, body = await asyncio.to_thread(b.call, "POST", "/api/mass", lambda c: f"- {b.seal(c, GOOD.encode() + bytes(64 - len(GOOD) % 64)).hex()}".encode())
        check(st == 409 and not os.path.exists(os.path.join(state, "mass")), f"a token without an address: refused, nothing kept ({st})")
        st, _, body = await asyncio.to_thread(b.call, "POST", "/api/mass", f"{url} {'41' * 19}4".encode())   # an odd number of hex digits
        check(st == 400, f"a token that is no sealed one: refused ({st})")
        st, _, body = await asyncio.to_thread(post, "wrong")
        await until(lambda: any(l.startswith("ma auth") for l in text(log)), 10)
        check(st == 200 and any(l.startswith("ma auth") for l in text(log)), f"a wrong token from the page: refused, said so: {[l for l in text(log) if l.startswith('ma ')]}")
        check(cmds("auth") and not any(c != "auth" for c, _ in ma.cmds), "auth was the first command, nothing after the refusal")
        open(log, "ab").write(b"#falsy\n"); ma.cmds.clear()
        st, _, body = await asyncio.to_thread(post, "falsy")
        await until(lambda: any(l.startswith("ma auth") for l in text(log)[text(log).index("#falsy"):]), 10)
        check(any(l.startswith("ma auth") for l in text(log)[text(log).index("#falsy"):]) and not any(c != "auth" for c, _ in ma.cmds),
              "a login answered false (no error): refused all the same")
        ma.cmds.clear(); open(log, "ab").write(b"#good\n")
        st, _, body = await asyncio.to_thread(post, GOOD)
        await until(lambda: "pl." in text(log)[text(log).index("#good"):] and any(l.startswith("pq ") for l in text(log)[text(log).index("#good"):]), 10)
        sj = json.dumps(await asyncio.to_thread(b.state))
        check(GOOD not in sj and json.loads(sj)["mass"]["token"] is True and json.loads(sj)["mass"]["state"] == "up" and json.loads(sj)["mass"]["self"] is True,
              "the page: up, this Echo found, a token there but never shown")
        mf = os.path.join(state, "mass")
        check(stat.S_IMODE(os.stat(mf).st_mode) == 0o600 and f"token={GOOD}" in open(mf).read(), "kept in state/mass, 0600")
        st, _, body = await asyncio.to_thread(post, "has space")
        check(st == 400, "not a token (a space): refused")
        other = "http://127.0.0.2:8095".encode().hex()
        st, _, body = await asyncio.to_thread(b.call, "POST", "/api/mass", f"{other} =".encode())
        check(st == 409 and f"url=http://127.0.0.1:{MA_PORT}" in open(mf).read(), f"the token kept for another address: refused, nothing changed ({st})")
        hu = "https://127.0.0.1:8095".encode().hex()
        st, _, body = await asyncio.to_thread(b.call, "POST", "/api/mass", lambda c: f"{hu} {b.seal(c, GOOD.encode() + bytes(64 - len(GOOD) % 64)).hex()}".encode())
        check(st == 400, f"https: refused (the API is plain ws here) ({st})")

        # the Screen page: the apps the UI listed, the home app picked there reaches the UI at once
        say("app me.jxl.kiosk_satellite\tKiosk Satellite\t0", "app com.android.music\tMusic\t1", "app org.fully.kiosk\tFully Kiosk\t0", "apps.")
        await until(lambda: len((b.state() or {}).get("screen", {}).get("apps", [])) == 3, 5)
        sc = (await asyncio.to_thread(b.state))["screen"]
        check([a["pkg"] for a in sc["apps"]] == ["me.jxl.kiosk_satellite", "com.android.music", "org.fully.kiosk"] and sc["apps"][1]["system"] is True
              and sc["home"] is None, f"the page: the apps as listed, Android's own marked, no home app chosen: {sc}")
        st, _, body = await asyncio.to_thread(b.call, "POST", "/api/screen", b"home org.fully.kiosk")
        await until(lambda: "home org.fully.kiosk" in text(log), 5)
        check(st == 200 and json.loads(body)["home"] == "org.fully.kiosk" and "home org.fully.kiosk" in text(log)
              and open(os.path.join(state, "screen_home")).read().strip() == "org.fully.kiosk", "a home app from the page: kept, the UI told")
        st, _, body = await asyncio.to_thread(b.call, "POST", "/api/screen", b"home")
        await until(lambda: text(log)[-1] == "home " or "home " in text(log), 5)
        check(st == 200 and json.loads(body)["home"] == "" and "home " in text(log), "the face")
        st, _, body = await asyncio.to_thread(b.call, "POST", "/api/screen", b"home -")
        await until(lambda: "home -" in text(log)[-5:], 5)
        check(st == 200 and json.loads(body)["home"] is None and not os.path.exists(os.path.join(state, "screen_home")), "none chosen again: the file gone")
        st, _, _ = await asyncio.to_thread(b.call, "POST", "/api/screen", b"home ../etc")
        check(st == 400 and req(WEB, "POST", "/api/screen", b"home x")[0] == 401, "a bad package, an unsigned request: refused")
        t = text(log)
        check(any(l.startswith("ma up Music Assistant 2.10.8") for l in t), f"up: {[l for l in t if l.startswith('ma ')]}")
        check(ma.cmds and ma.cmds[0] == ("auth", {"token": GOOD}), "the long token, first")
        pls = {l.split("\t")[0][3:]: l.split("\t") for l in t if l.startswith("pl ")}
        me = pls.get(ma.self_id)
        check(me and me[1] == "Kitchen Show" and "s" in me[2] and "v" in me[2] and me[3] == "playing" and me[4] == "45"
              and me[6] == f"{ma.self_id},dot2" and me[7] == "Jóga · Björk" and me[8] == "38", f"this Echo's line, its group's volume (its players' average) last: {me}")
        check(pls.get("dot2") and pls["dot2"][8] == "", "a player in no group of its own: no group volume")
        check(pls.get("dot2") and "g" in pls["dot2"][2] and pls["dot2"][5] == ma.self_id, f"the Dot may join and follows it: {pls.get('dot2')}")
        check(pls.get("sonos-1") and "g" in pls["sonos-1"][2], f"the Sonos may join (by its id): {pls.get('sonos-1')}")
        check(pls.get("all") and "l" in pls["all"][2] and "g" not in pls["all"][2], f"the group player: flagged, not joinable: {pls.get('all')}")
        check("airplay-x" not in pls, "a player hidden in the UI is left out")
        pq = [l for l in t if l.startswith("pq ")]
        check(pq and pq[-1] == "pq playing\t0\toff\t3\t12\tKitchen + 1", f"this Echo's queue: {pq[-1:]}")
        check(cmds("player_queues/get_active_queue") == [{"player_id": ma.self_id}], "asked for by this Echo's id")

        # events: a second of play changes nothing shown; a new volume does
        n0 = len([l for l in text(log) if l.startswith("pl ")])
        p = ma.players()[0]; p["elapsed_time"] = 99
        await ma.send({"event": "player_updated", "object_id": ma.self_id, "data": p})
        p = ma.players()[2]; p["volume_level"] = 60
        await ma.send({"event": "player_updated", "object_id": "sonos-1", "data": p})
        await until(lambda: any(l.startswith("pl sonos-1\tSonos Move\tgva\tpaused\t60") for l in text(log)), 5)
        new = [l for l in text(log) if l.startswith("pl ")][n0:]
        check(len(new) == 1 and new[0].startswith("pl sonos-1"), f"one line for the volume, none for the second of play: {new}")
        q = dict(ma.queue, shuffle_enabled=True, repeat_mode="all", current_index=4)
        await ma.send({"event": "queue_updated", "object_id": ma.self_id, "data": q})
        await ma.send({"event": "queue_items_updated", "object_id": ma.self_id, "data": q})
        await until(lambda: "qc" in text(log), 5)
        check("pq playing\t1\tall\t4\t12\tKitchen + 1" in text(log) and "qc" in text(log), "the queue's shuffle, repeat and place; its items changed")
        await ma.send({"event": "player_removed", "object_id": "sonos-1", "data": None})
        await until(lambda: "pl- sonos-1" in text(log), 5)
        check("pl- sonos-1" in text(log), "a player removed")
        await ma.send({"event": "player_added", "object_id": "sonos-1", "data": ma.players()[2]})
        await until(lambda: text(log)[-1].startswith("pl sonos-1"), 5)

        # lists
        say("ml 1 albums 0")
        await until(lambda: any(l.startswith("E 1") for l in text(log)), 5)
        t = text(log); items = [l for l in t if l.startswith("I 1 ")]
        check("L 1 0 -1" in t and len(items) == 60, f"the first page: 60 albums, the total not known yet ({len(items)})")
        check(cmds("music/albums/library_items")[-1] == {"limit": 60, "offset": 0, "order_by": "sort_name"}, "limit, offset, by name")
        f = items[3].split("\t") if len(items) > 3 else []
        check(f and f[0] == "I 1 a" and f[1] == "library://album/3" and f[2] == "Album 03" and f[3] == "Björk · 1993" and re.fullmatch(r"[0-9a-f]{10}", f[4]),
              f"an album's line: {f}")
        key = f[4] if f else ""
        say("ml 2 albums 60")
        await until(lambda: any(l.startswith("E 2") for l in text(log)), 5)
        t = text(log)
        check("L 2 60 75" in t and len([l for l in t if l.startswith("I 2 ")]) == 15, "the short page: the total")
        say("ml 3 album_tracks 0 library://album/3")
        await until(lambda: any(l.startswith("E 3") for l in text(log)), 5)
        tr = [l.split("\t") for l in text(log) if l.startswith("I 3 ")]
        check(len(tr) == 5 and tr[2][2] == "Song 2 (Live)" and tr[2][3] == "Björk, Thom" and tr[2][5] == "202" and tr[0][4],
              f"an album's tracks: version, artists (not the album again), seconds, its album's picture: {tr[2] if len(tr) > 2 else tr}")
        check(cmds("music/albums/album_tracks")[-1] == {"item_id": "3", "provider_instance_id_or_domain": "library"}, "by the URI's parts")
        say("ml 4 search 0 björk")
        await until(lambda: any(l.startswith("E 4") for l in text(log)), 5)
        sr = [l.split("\t")[0] for l in text(log) if l.startswith("I 4 ")]
        check(sr == ["I 4 r", "I 4 a", "I 4 t", "I 4 t", "I 4 o"], f"search: grouped artists, albums, tracks, radio: {sr}")
        check(cmds("music/search")[-1]["search_query"] == "björk", "the query as typed")
        say("ml 5 queue 0")
        await until(lambda: any(l.startswith("E 5") for l in text(log)), 5)
        ql = [l.split("\t") for l in text(log) if l.startswith("I 5 ")]
        check(len(ql) == 8 and ql[0][0] == "I 5 q" and ql[0][1] == "qi0" and ql[4][6] == "c" and ql[5][6] == "x" and ql[1][2] == "Song 1",
              f"the queue: items by their id, the current marked, the unavailable too: {[x[6] for x in ql]}")
        say("ml 6 artist_albums 0 notauri")
        await until(lambda: any(l.startswith("E 6") for l in text(log)), 5)
        check(any(l.startswith("E 6 ") and len(l) > 5 for l in text(log)), "a bad URI: an error, not a request")
        say("ml 7 album_tracks 0 library://album/99")
        await until(lambda: any(l.startswith("E 7") for l in text(log)), 5)
        check("E 7 no such album" in text(log), "Music Assistant's error, as it said it")
        n7 = len([l for l in text(log) if l.startswith("I 7 ")])
        await ma.send({"message_id": "0", "result": [album(9)]}); await ma.send({"message_id": "x", "result": [album(9)]})
        await asyncio.sleep(0.5)
        check(len([l for l in text(log) if l.startswith("I ")]) and len([l for l in text(log) if l.startswith("I 7 ")]) == n7
              and text(log).count("E 7 no such album") == 1, "an answer to no request (message 0, or no number): nothing")
        # a place in a list no list has (the UI's word): refused, never asked of Music Assistant
        n0 = len(cmds("music/albums/library_items")); say("ml 70 albums 2147483647", "ml 71 albums -60")
        await until(lambda: "E 71 no such place in it" in text(log), 5)
        check("E 70 no such place in it" in text(log) and "E 71 no such place in it" in text(log) and len(cmds("music/albums/library_items")) == n0,
              "a place in a list past any list, or before its start: refused")
        say("ml 8 podcast_episodes 0 library://podcast/1")
        await until(lambda: any(l.startswith("E 8") for l in text(log)), 5)
        ep = [l for l in text(log) if re.match(r"[LIE] 8( |$)", l)]
        check(ep[:1] == ["L 8 0 -1"] and len([l for l in ep if l.startswith("I 8 ")]) == 1100 and ep[-1] == "E 8"
              and ep[1].split("\t")[2] == "Episode 0" and ep[-2].split("\t")[2] == "Episode 1099",
              f"a listing in pieces (partial, 500 a message): all of it, in order, one L: {ep[:1]} {len(ep)} lines, {ep[-1:]}")
        say("ml 9 podcast_episodes 1100 library://podcast/1")
        await until(lambda: any(l.startswith("E 9") for l in text(log)), 5)
        check("L 9 1100 1100" in text(log), "and nothing after it")
        say("ml 10 playlist_tracks 0 library://playlist/4")
        await until(lambda: any(l.startswith("E 10") for l in text(log)), 5)
        a = cmds("music/playlists/playlist_tracks")[-1]
        check("L 10 0 70" in text(log) and "limit" not in a and "offset" not in a,
              f"a playlist before schema 93: asked whole (no paging there), the total known: {a}")
        say("ml 11 playlist_tracks 70 library://playlist/4")
        await until(lambda: any(l.startswith("E 11") for l in text(log)), 5)
        check("L 11 70 70" in text(log) and len(cmds("music/playlists/playlist_tracks")) == 1, "no second page asked of it")
        for _ in range(20):                                 # the UI's bytes that are no UTF-8: '?' (Music Assistant would close)
            try:
                with open(fifo, "wb") as q: q.write(b"ml 15 search 0 \xff\xfeab\xc3\n"); break
            except BrokenPipeError: time.sleep(0.05)
        await until(lambda: any(l.startswith("E 15") for l in text(log)), 5)
        check(cmds("music/search")[-1]["search_query"] == "??ab?" and not any(l.startswith("ma error") for l in text(log)),
              f"a search with bytes that are no UTF-8: sent repaired, the session kept: {cmds('music/search')[-1:]}")
        say("ml 12 artist_tracks 0 library://artist/huge")
        await until(lambda: any(l.startswith("E 12") for l in text(log)), 15)
        say("ml 16 artist_tracks 0 library://artist/tiny", "ml 17 artist_tracks 0 library://artist/long")
        await until(lambda: any(l.startswith("E 17") for l in text(log)), 10)
        t = text(log); i17 = [l.split("\t") for l in t if l.startswith("I 17 ")]
        check("E 16 too long for this Echo" in t and len(i17) == 2 and i17[0][1] == "" and i17[1][1] == "library://track/102",
              f"too many values: that list an error; a URI longer than a line takes: the item, not playable: {[x[1][:30] for x in i17]}")
        say("ml 13 albums 0")
        await until(lambda: any(l.startswith("E 13") for l in text(log)), 5)
        t = text(log)
        check("E 12 too long for this Echo" in t and "E 13" in t and not any(l.startswith("ma error") for l in t),
              f"an answer over 4 MB: that list an error, the connection kept: {[l for l in t if l.startswith(('E 12', 'ma '))][-3:]}")

        # commands
        say("mplay play library://album/3", "mplay radio library://track/101", "mplay add library://track/102",
            "mq index 5", "mq index 99999999999", "mq index qi6", "mq delete qi4", "mq shuffle 1", "mq repeat one", "mq clear",
            "mg add sonos-1", "mv dot2 33", "mgv 70", "mhere sonos-1", "mg leave")
        await until(lambda: cmds("players/cmd/ungroup"), 5)
        q = ma.self_id
        check(cmds("player_queues/play_media") == [{"queue_id": q, "media": "library://album/3", "option": "play", "radio_mode": False},
                                                   {"queue_id": q, "media": "radio_playlist://playlist/library://track/101", "option": "play", "radio_mode": False},
                                                   {"queue_id": q, "media": "library://track/102", "option": "add", "radio_mode": False}], "play, radio, add")
        check(cmds("player_queues/play_index") == [{"queue_id": q, "index": 5}, {"queue_id": q, "index": "qi6"}] and cmds("player_queues/delete_item") == [{"queue_id": q, "item_id_or_index": "qi4"}],
              "play from here, remove")
        check(cmds("player_queues/shuffle") == [{"queue_id": q, "shuffle_enabled": True}] and cmds("player_queues/repeat") == [{"queue_id": q, "repeat_mode": "one"}]
              and cmds("player_queues/clear") == [{"queue_id": q}], "shuffle, repeat, clear")
        check(cmds("players/cmd/set_members") == [{"target_player": q, "player_ids_to_add": ["sonos-1"]}], "a speaker into this Echo's group")
        check(cmds("players/cmd/volume_set") == [{"player_id": "dot2", "volume_level": 33}] and cmds("players/cmd/group_volume") == [{"player_id": q, "volume_level": 70}],
              "a speaker's volume, the group's")
        check(cmds("player_queues/transfer") == [{"source_queue_id": "sonos-1", "target_queue_id": q, "auto_play": True}], "what the Sonos played, here")
        check(cmds("players/cmd/ungroup") == [{"player_id": q}], "leaving")
        # the Echo's own from the screen: Do not disturb on, the Echo's setting (the page's, Home Assistant's) and the
        # screen told back; off again
        dnd = lambda: next((x["value"] for x in (b.state() or {}).get("settings", []) if x["name"] == "do_not_disturb"), None)
        open(log, "ab").write(b"#dnd\n"); d0 = await asyncio.to_thread(dnd); say("dnd 1")
        await until(lambda: "dnd 1" in text(log)[text(log).index("#dnd"):], 5)
        d1 = await asyncio.to_thread(dnd)
        check(d1 != d0 and d1 in (True, 1, "1", "on") and "dnd 1" in text(log)[text(log).index("#dnd"):],
              f"Do not disturb from the screen: the Echo's setting, and the screen told ({d0} -> {d1})")
        say("dnd 0"); await until(lambda: "dnd 0" in text(log)[text(log).index("#dnd"):], 5)
        ma.bad_set_members = True; ma.cmds.clear()
        say("mg remove dot2")
        await until(lambda: cmds("players/cmd/ungroup"), 5)
        check(cmds("players/cmd/ungroup") == [{"player_id": "dot2"}], "without set_members: ungroup one by one")
        n0 = len(ma.cmds)
        say("mq bogus 1", "mplay bogus library://track/1", "mg join sonos-1", "mv dot2 101", "mgv -3", "cmd x")
        await asyncio.sleep(0.5)
        check(len(ma.cmds) == n0, f"nothing for what the UI may not say: {ma.cmds[n0:]}")

        # images
        say(f"img {key} 150")
        await until(lambda: any(isinstance(l, tuple) for l in lines(log)), 5)
        got = lines(log); k = next((i for i, l in enumerate(got) if isinstance(l, str) and l.startswith(f"img {key} ") and l.endswith(" 160")), None)
        jpg = got[k + 1][1] if k is not None and k + 1 < len(got) and isinstance(got[k + 1], tuple) else b""
        check(jpg[:2] == b"\xff\xd8" and Image.open(io.BytesIO(jpg)).size == (160, 160), f"the picture at 160 px, JPEG: {len(jpg)} bytes")
        check(ma.img_auth and ma.img_auth[-1] == f"Bearer {GOOD}" and ma.img_ids[-1] == "px3", f"the album's own proxy id, the token along: {ma.img_ids[-1:]}")
        say("img 0123456789 80")
        await until(lambda: "img 0123456789 0 80" in text(log), 5)
        check("img 0123456789 0 80" in text(log), "an unknown key: none (at the size asked)")

        # idle longer than the receive timeout (8 s here, 90 s on the Echo), on a server that sends no pings: ours keep it
        open(log, "ab").write(b"#idle\n"); await asyncio.sleep(20)
        check(not any(l.startswith("ma ") for l in text(log)[text(log).index("#idle"):]), "idle 20 s: the session kept (its own pings)")
        # the UI hangs on an idle Echo (nothing written to it, the band dark): its pings unanswered, it is started again
        open(log, "ab").write(b"#hung\n"); say("#hang"); t0 = time.monotonic()
        await until(lambda: any(l.startswith("#start") for l in text(log)[text(log).index("#hung"):]), 20)
        dt = time.monotonic() - t0
        check(any(l.startswith("#start") for l in text(log)[text(log).index("#hung"):]) and dt < 15, f"a UI hung on an idle Echo: started again ({dt:.1f} s)")
        await until(lambda: "pl." in text(log)[text(log).index("#hung"):], 5)
        # the connection lost with a list on its way
        ma.stall = True; n0 = len(cmds("music/albums/library_items"))
        say("ml 50 albums 0")
        await until(lambda: len(cmds("music/albums/library_items")) > n0, 5)
        await ma.ws.close(); await site.stop()
        await until(lambda: any(l.startswith("ma error") for l in text(log)), 5)
        t = text(log)
        check(any(l.startswith("ma error") for l in t) and any(l.startswith("pl- ") for l in t), "lost: said so, the players gone")
        check("E 50 connection lost" in t, "the list on its way: ended, with why")
        # back by itself, to a Music Assistant that has this Echo as a protocol player under a universal one
        ma.stall = False; ma.merged = True; ma.extra = 140; ma.cmds.clear()
        site = web.TCPSite(run, "127.0.0.1", MA_PORT); await site.start()
        open(log, "ab").write(b"#mark\n")
        await until(lambda: "pl." in text(log)[text(log).index("#mark"):] and cmds("player_queues/get_active_queue"), 20)
        t = text(log)[text(log).index("#mark"):]
        check("pl." in t, "and back by itself")
        pls = {l.split("\t")[0][3:]: l.split("\t") for l in t if l.startswith("pl ")}
        check(pls.get("up_kitchen") and "s" in pls["up_kitchen"][2] and ma.self_id not in pls and not any(p.startswith("proto") for p in pls),
              f"this Echo: the universal player that has it as its output, after 140 protocol players and 140 speakers (more than the Echo keeps), the protocol players not listed: {len(pls)} listed")
        check("anchor" not in pls and "hue" not in pls, f"no private player, no light: {sorted(pls)[-5:]}")
        check(cmds("player_queues/get_active_queue") == [{"player_id": "up_kitchen"}], f"its queue: {cmds('player_queues/get_active_queue')}")
        n0 = len([l for l in text(log) if l.startswith("pl ")])
        for k in range(3):                               # both report every second of play: this Echo stays the one it was
            pp = ma.players()[2 * ma.extra:]
            pp[0]["volume_level"] = pp[1]["volume_level"] = 46 + k
            await ma.send({"event": "player_updated", "object_id": ma.self_id, "data": pp[0]})
            await ma.send({"event": "player_updated", "object_id": "up_kitchen", "data": pp[1]})
        await until(lambda: any(l.startswith("pl up_kitchen\tKitchen Show\ts") and "\t48\t" in l for l in text(log)), 5)
        new = [l for l in text(log) if l.startswith("pl ")][n0:]
        check(len(cmds("player_queues/get_active_queue")) == 1 and all(l.startswith("pl up_kitchen") and "\ts" in l for l in new),
              f"no flip between the two: {new}, queue asked {len(cmds('player_queues/get_active_queue'))}x")
        open(log, "ab").write(b"#gone\n")
        await ma.send({"event": "player_removed", "object_id": "up_kitchen", "data": None})
        await until(lambda: "pq-" in text(log)[text(log).index("#gone"):], 5)
        check("pl- up_kitchen" in text(log)[text(log).index("#gone"):] and "pq-" in text(log)[text(log).index("#gone"):]
              and (await asyncio.to_thread(b.state))["mass"]["self"] is False, "this Echo's player removed: no queue, the page says so")

        # a Sendspin server plays here: its address suggested once it is paired (unpaired: none); the token pinned to
        # the page's address, nothing goes to the suggested one (a recorder on 127.0.0.1:8095)
        seen = []
        async def record(r):
            seen.append(f"{r.method} {r.path_qs} {dict(r.headers)}")
            if r.headers.get("Upgrade", "").lower() != "websocket": return web.Response(status=404)
            ws = web.WebSocketResponse(); await ws.prepare(r)
            async for m in ws: seen.append(str(m.data))
            return ws
        rapp = web.Application(); rapp.router.add_route("*", "/{p:.*}", record)
        rrun = web.AppRunner(rapp); await rrun.setup(); await web.TCPSite(rrun, "127.0.0.1", 8095).start()
        added = asyncio.Queue()
        ss = SendspinServer(asyncio.get_running_loop(), Identity.generate(), "fake-ma", pairing_store=InMemoryServerPairingStore(), allow_noncompliant_clients=False)
        ss.add_event_listener(lambda _s, ev: isinstance(ev, ClientAddedEvent) and added.put_nowait(ev.client_id))
        try:
            await ss.start_server(port=16977, discover_clients=False)
            ss.connect_to_client(f"ws://127.0.0.1:{SS_PORT}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
            cid = await asyncio.wait_for(added.get(), 15)
            await ss.trust_unpaired(cid)
            await until(lambda: any("player role active (unpaired)" in l for l in errlog), 10); await asyncio.sleep(1)
            found = (await asyncio.to_thread(b.state))["mass"]["found"]
            check(any("player role active (unpaired)" in l for l in errlog) and not found, f"an unpaired server plays here: no address suggested ({found!r})")
            tok = (getattr(noise, "decode_psk_token", None) or noise.decode_token)(
                subprocess.run([f"{ROOT}/build/hassmic-host-screen", "-T"], env=env, capture_output=True, text=True).stdout.strip())
            await ss.initiate_pairing(cid, PairingAttempt(method=PairMethod.PAIRING_PSK, pairing_psk=tok.pairing_psk, client_id=tok.client_id))
            await until(lambda: any("player role active (paired)" in l for l in errlog), 10)
            await until(lambda: b.state()["mass"]["found"], 5)
            found = (await asyncio.to_thread(b.state))["mass"]["found"]
            check(found == "127.0.0.1:8095", f"paired: its address suggested ({found!r})")
            n0 = len(cmds("auth"))                       # connected anew after it (the same address saved again): still there
            st, _, _ = await asyncio.to_thread(b.call, "POST", "/api/mass", f"{url} =".encode())
            await until(lambda: len(cmds("auth")) > n0, 10); await asyncio.sleep(2)
            check(st == 200 and len(cmds("auth")) > n0 and not seen,
                  f"connected anew, the token pinned: to the page's address, nothing to the suggested one ({st}, {seen[:2]})")
        finally:
            await ss.close(); await rrun.cleanup()
        check(not any(GOOD in l for l in errlog), "the token never in the log")

        # Music Assistant stops reading (its event loop stuck): the UI's requests wait on the socket in a thread of
        # their own, the UI's answers to the pings are read all the same: not taken for hung
        import socket
        await until(lambda: ma.ws is not None and not ma.ws.closed, 10)
        ma.req.transport.get_extra_info("socket").setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        ma.req.transport.pause_reading(); open(log, "ab").write(b"#stuck\n")
        def flood():                                     # (past what the socket's buffers take, some 4 MB: its sends block)
            for k in range(16): say(*[f"ml {1000 + 500 * k + i} search 0 {'x' * 900}" for i in range(500)])
        fl = asyncio.ensure_future(asyncio.to_thread(flood))
        await asyncio.sleep(3)
        vol = lambda: next((x["value"] for x in (b.state() or {}).get("settings", []) if x["name"] == "volume"), None)
        v0 = await asyncio.to_thread(vol); v1 = 33 if str(v0) != "33" else 34
        await asyncio.to_thread(say, f"vol {v1}"); t0 = time.monotonic()
        while time.monotonic() - t0 < 3 and str(await asyncio.to_thread(vol)) != str(v1): await asyncio.sleep(0.2)
        dv = time.monotonic() - t0
        check(str(await asyncio.to_thread(vol)) == str(v1) and dv < 2, f"meanwhile the Echo's own (the volume from the screen): at once, not behind them ({dv:.1f} s)")
        await asyncio.sleep(3)
        t = text(log)[text(log).index("#stuck"):]
        check(not any(l.startswith("#start") for l in t) and any("commands waiting" in l for l in errlog),
              f"Music Assistant not reading: the UI's requests wait, the UI is not taken for hung ({sum(l.startswith('#start') for l in t)} starts)")
        ma.req.transport.abort()
        await asyncio.wait_for(fl, 30)

        # the light band's height from the page: the UI started again with it (its layer and the overscan), the band's
        # rows to the new one; off: started again without
        for rows in (20, 0):
            open(log, "ab").write(f"#band{rows}\n".encode())
            r = await asyncio.to_thread(b.set, light_band=rows)
            await until(lambda: f"#start band={rows}" in text(log)[text(log).index(f"#band{rows}"):], 15)
            t = text(log)[text(log).index(f"#band{rows}"):]
            if rows: await until(lambda: any(l.startswith("band ") for l in text(log)[text(log).index(f"#band{rows}"):]), 5); t = text(log)[text(log).index(f"#band{rows}"):]
            check(f"#start band={rows}" in t and (not rows or any(l.startswith("band ") for l in t[t.index(f"#start band={rows}"):])),
                  f"light band {rows} rows from the page: the UI started again with it{', its rows to it' if rows else ''} ({r.get('status') if isinstance(r, dict) else r})")
    finally:
        proc.terminate(); await run.cleanup()
        subprocess.run(["pkill", "-f", me_ui])
    if check.failed: print("\n".join(errlog[-40:]))
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)

asyncio.run(main())
