#!/usr/bin/env python3
"""Wake word arbitration (tests/arbecho.py): media in the room (main.c wake_heard).  The kitchen Echo (500) hears worse
than the living room one (900), but a Sonos plays beside it.  The players are a setting of each Echo; the fake Home
Assistant follows them as its state tracker does (the state at once, then on every change, to each subscriber; dropped
with the link), over the keyed link only.  The background factor, the hold, both rooms, the list changed (A, B, A, a
typo, 8 of the longest ids over a restart), Home Assistant gone, priority, a third member of an older build played here
with the network's key (tests/fake_ha_arbitration_window.py), Kiosk Satellite mode, the media log, and Music Assistant
playing on the kitchen Echo itself over Sendspin (aiosendspin, as tests/fake_ma_sendspin.py)."""
import asyncio, base64, hashlib, json, os, re, socket, struct, time
from arbecho import ARB, KIOSK, BCAST, Echo, HA, Browser, check, until, start, run
from aiosendspin.noise import Identity, InMemoryServerPairingStore
from aiosendspin.server import SendspinServer, AudioFormat, ClientAddedEvent
import numpy as np

key = os.urandom(32)
a, b = Echo("Echo Kitchen", 16961, 500, net=(2, key)), Echo("Echo Living Room", 16962, 900, net=(2, key))
for e in (a, b):                                    # the hold, "missing" and the media log, shortened
    e.env.update(HASSMIC_MEDIA_HOLD_MS="1500", HASSMIC_MEDIA_MISSING_MS="1000", HASSMIC_MEDIA_AFTER_MS="1000",
                 HASSMIC_MEDIA_EVERY_MS="3000", HASSMIC_MUSIC=os.path.join(e.state, "music.raw"))


async def main():
    ha = HA([a, b])
    await start((a, b), ha); t0 = time.monotonic()
    ok = await until(lambda: a.st("arbitration_peers") == 1 and b.st("arbitration_peers") == 1, 10)
    check(ok, "started in one network: each counts the other")
    sn = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)                 # listens in on the claims
    sn.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); sn.bind(("", ARB)); sn.setblocking(False)
    apk = a.pub()[:8]
    # a third member, of an older build, that speaks the protocol with the network's key (as in _window.py); it only
    # beacons for the check of its claims below, so no other round waits for it
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    send = lambda p: tx.sendto(p, (BCAST, ARB))
    node = lambda s: bytes([len(s)]) + s.encode()
    nid, k64 = a.net().split(); nkey = base64.b64decode(k64); nb8 = struct.pack("<Q", int(nid, 16))
    mac = hashlib.blake2b(b"hassmic arbitration mac", key=nkey, digest_size=32).digest()
    fpub = os.urandom(32); fid = fpub[:8]; ctr = [1 << 40]
    def signed(p): return p + hashlib.blake2b(p, key=mac, digest_size=16).digest()
    def fctr(): ctr[0] += 1; return struct.pack("<Q", ctr[0])
    def fbeacon(flags=b""): send(signed(b"HMA1\x01" + fpub + nb8 + node("echo-hallway") + fctr() + flags))
    kw = hashlib.blake2b(b"alexa", digest_size=8).digest()
    def fclaim(score=99999): send(signed(b"HMA1\x04" + nb8 + fid + fctr() + kw + struct.pack("<iBB", score, 0, 0)))
    async def a_claimed(secs=2):                    # the kitchen Echo's own claim (not its "answers"), when it went out
        end = time.monotonic() + secs
        while time.monotonic() < end:
            try: p = sn.recv(512)
            except BlockingIOError: await asyncio.sleep(0.002); continue
            if p[4] == 4 and p[13:21] == apk and p[-17] == 0: return time.monotonic()
        return None

    # Media in the room (main.c wake_heard): the kitchen Echo (500) hears worse than the living room one (900), but a
    # Sonos plays beside it.  The players are a setting of each Echo; Home Assistant sends their states, over the keyed
    # link only, for good (not once)
    SONOS, RADIO, TV = "media_player.kitchen_sonos", "media_player.kitchen_radio", "media_player.living_room_tv"
    ha.media.update({SONOS: "idle", RADIO: "idle", TV: "off"}); ha.vols[SONOS] = "0.3"
    ROOM = r"room -?\d+\.\d dBFS \(floor -?\d+\.\d\)"     # the media log's measure (main.c room_str)
    media = lambda e: (e.page.state() or {}).get("media") or {}
    setting = lambda e, n: next((x for x in (e.page.state() or {}).get("settings", []) if x["name"] == n), {})
    def drain():
        while True:
            try: sn.recv(512)
            except BlockingIOError: return
    async def round_(settle=1.5):
        """both hear it: who answered ("a", "b", "ab", ""), the kitchen Echo's claim: its score and length"""
        drain(); na, nb = len(a.starts), len(b.starts); claim = None
        a.wake(); b.wake(); end = time.monotonic() + settle
        while time.monotonic() < end:
            try: p = sn.recv(512)
            except BlockingIOError: await asyncio.sleep(0.005); continue
            if claim is None and p[4] == 4 and p[13:21] == apk and p[-17] == 0: claim = p
        won = ("a" if len(a.starts) > na else "") + ("b" if len(b.starts) > nb else "")
        if "a" in won: a.end_pipeline()
        if "b" in won: b.end_pipeline()
        await asyncio.sleep(0.5)
        return won, struct.unpack("<i", claim[37:41])[0] if claim else None, len(claim) if claim else None

    r = [a.page.set(arbitration_media_players=v) for v in ("media_player.Kitchen", "light.kitchen", ",".join(f"media_player.p{i}" for i in range(9)),
                                                            "media_player." + "x" * 81)]
    check(all(x.get("applied") == 0 and "arbitration_media_players" in x.get("errors", "") for x in r) and "at most 8" in r[2].get("errors", "")
          and "not a media player" in r[0].get("errors", "") and not ha.subs, f"not a media player's entity id, or more than 8: refused: {[x.get('errors') for x in r]}")
    r = a.page.set(arbitration_media_players=SONOS)
    ok = await until(lambda: (a.name, SONOS) in ha.subs, 3)
    check(r.get("applied") == 1 and ok and not any(n == b.name for n, _ in ha.subs) and (a.name, SONOS) not in ha.asked
          and f"following {SONOS} in Home Assistant" in a.text(), f"the list set on the page: followed at once, for good (not once): {ha.subs}")
    check(f"arbitration_media_players={SONOS}" in open(os.path.join(a.state, "config")).read().split(), "kept in state/config")
    sp, sa = setting(a, "arbitration_media_players"), setting(a, "arbitration_media_ambient")
    check(sp.get("type") == "text" and sp.get("value") == SONOS and sp.get("export") is False and sp.get("feature") is False
          and sa.get("value") == 50 and (sa.get("min"), sa.get("max")) == (10, 100) and sa.get("export") is True,
          f"settings page: the players (text, not exported), the background (50 %, exported): {sp} {sa}")
    m = media(a)
    check(m.get("pct") == 50 and m.get("here") is None and m.get("players") == [{"id": SONOS, "state": "stopped"}],
          f"status: the player as Home Assistant said at once (idle): {m}")
    won, sc, base_len = await round_()
    check(won == "b" and sc == 500 and "arbitration: no media of this room: score 500" in a.text(), f"no media: the better one answers: {won} {sc}")
    await asyncio.sleep(max(0, t0 + 6 - time.monotonic()))     # the media log's room level needs 5 s heard (room_put)
    ha.set_media(SONOS, "playing"); await asyncio.sleep(0.3)
    check(f"arbitration: {SONOS} playing" in a.text() and media(a).get("here") == f"{SONOS} playing", f"the Sonos plays: {media(a)}")
    won, sc, _ = await round_()
    check(won == "b" and sc == 801 and f"arbitration: media of this room ({SONOS} playing): ambient counted at 50 %, score 500 -> 801" in a.text(),
          f"background at 50 %: +3 dB, still under the other Echo's 900: {won} {sc}")
    # the media log: the room's level at the change and a while after (1 s here, 5 s on the Echo), the volume as well,
    # and the room's floor on each round's line; logging only, the scores above and below are the same
    t = a.text()
    check(re.search(rf"^.*media: {SONOS} idle -> playing, {ROOM}$", t, re.M) and re.search(rf"^.*media: {SONOS} playing 1 s, {ROOM}$", t, re.M)
          and re.search(r"arbitration: media of this room .*score 500 -> 801; room floor -?\d+\.\d dBFS$", t, re.M),
          "media log: the change, 1 s after, the floor on the round's line")
    check(ha.attr_subs == [(a.name, SONOS, "volume_level")] and f"media: {SONOS} volume" not in t,
          f"its volume followed too, once; the first value is no change: {ha.attr_subs}")
    ha.set_volume(SONOS, "0.45"); await asyncio.sleep(0.2); ha.set_volume(SONOS, "0.5"); await asyncio.sleep(1.3)
    t = a.text()
    check(re.search(rf"media: {SONOS} volume 0\.30 -> 0\.45, {ROOM}$", t, re.M) and re.search(rf"media: {SONOS} volume 0\.45 -> 0\.50, {ROOM}$", t, re.M)
          and len(re.findall(rf"media: {SONOS} volume 0\.50 1 s, {ROOM}$", t, re.M)) == 1 and f"volume 0.45 1 s" not in t,
          "media log: each volume change, one line after the last step")
    await asyncio.sleep(2)
    check(re.search(rf"media: {ROOM}, playing: {SONOS}$", a.text(), re.M), "media log: a line now and then while it plays")
    a.page.set(arbitration_media_ambient=25)
    won, sc, ln = await round_()
    check(won == "a" and sc == 1102 and ln == base_len, f"at 25 %: +6 dB, the kitchen Echo answers; its claim carries 1102, same length: {won} {sc} {ln}/{base_len}")
    ha.set_media(SONOS, "paused"); await asyncio.sleep(0.2)
    won, sc, _ = await round_()
    check(won == "a" and re.search(rf"arbitration: {SONOS} paused \(counts \d+ s more\)", a.text())
          and re.search(rf"media: {SONOS} playing -> paused, {ROOM}$", a.text(), re.M)
          and re.search(rf"media of this room \({SONOS} stopped 0 s ago\)", a.text()), f"paused: still counts for the hold: {won} {sc}")
    won, sc, _ = await round_()
    check(won == "b" and sc == 500, f"after the hold (1.5 s here): no more: {won} {sc}")

    b.page.set(arbitration_media_players=TV); await until(lambda: (b.name, TV) in ha.subs, 3)
    ha.set_media(SONOS, "playing"); ha.set_media(TV, "playing"); await asyncio.sleep(0.3)
    won, sc, _ = await round_()
    check(won == "b" and sc == 1102 and "score 900 -> 1201" in b.text(), f"media in both rooms: each Echo counts its own, the better one answers: {won} {sc}")
    ha.set_media(TV, "off")
    n = len(ha.subs); a.page.set(arbitration_media_players=SONOS); await asyncio.sleep(0.5)
    check(len(ha.subs) == n, "the same list again: nothing followed twice")
    a.page.set(arbitration_media_players=f"{SONOS}, {RADIO}"); await asyncio.sleep(0.5)
    check(ha.subs[n:] == [(a.name, RADIO)] and f"arbitration_media_players={SONOS},{RADIO}" in open(os.path.join(a.state, "config")).read().split(),
          f"one more: only that one followed: {ha.subs[n:]}")
    # listed, removed and listed again on the same link (A, B, A): still followed, so not subscribed twice; the last
    # state Home Assistant sent of it counts at once (the Sonos plays)
    n2 = len(ha.subs); a.page.set(arbitration_media_players=RADIO); a.page.set(arbitration_media_players=f"{SONOS},{RADIO}"); await asyncio.sleep(0.3)
    m = media(a)
    check(ha.subs[n2:] == [] and ha.subs.count((a.name, SONOS)) == 1 and m.get("here") == f"{SONOS} playing"
          and m.get("players") == [{"id": SONOS, "state": "playing"}, {"id": RADIO, "state": "stopped"}],
          f"listed again on the same link: no second subscription, its last state at once: {ha.subs[n2:]} {m}")
    check(sorted(x[1] for x in ha.attr_subs if x[0] == a.name) == sorted([SONOS, RADIO]), f"nor of its volume: {ha.attr_subs}")
    # an id Home Assistant does not have (a typo): it sends nothing, and after a while the page says so
    GHOST = "media_player.kitchen_sonso"
    a.page.set(arbitration_media_players=f"{SONOS},{GHOST}"); await asyncio.sleep(0.2)
    m1 = media(a); await asyncio.sleep(1.2); m2 = media(a)
    check((a.name, GHOST) in ha.subs and m1.get("linked") is True
          and m1.get("players") == [{"id": SONOS, "state": "playing"}, {"id": GHOST, "state": "unknown"}]
          and m2.get("players") == [{"id": SONOS, "state": "playing"}, {"id": GHOST, "state": "missing"}],
          f"an entity Home Assistant does not report: unknown at first, then missing: {m1} {m2}")
    long_ = [f"media_player.{i}" + "z" * 79 for i in range(6)]        # the longest ids there are: 8 of them
    full = ",".join([SONOS, RADIO] + long_)
    a.page.set(arbitration_media_players=full); await asyncio.sleep(0.5)

    # a restart: the stored list followed by itself; the Sonos plays already, so it counts at once.  Music Assistant
    # can play on it from now on (Sendspin)
    a.sendspin = 16965; open(os.path.join(a.state, "sendspin.unpaired"), "w").write("1")
    await asyncio.sleep(1.2)                        # the TV's hold over
    await a.cli.disconnect(); ha.drop(a); a.proc.terminate(); a.proc.wait(); a.start(); await asyncio.sleep(0.5); await a.connect(ha)
    a.page = Browser(a.web); a.page.login_with_button(a.proc)
    ok = await until(lambda: {(a.name, x) for x in full.split(",")} <= set(ha.subs), 3)
    check(ok and setting(a, "arbitration_media_players").get("value") == full and f"following {SONOS}, {RADIO}, {long_[0]}" in a.text(),
          f"restarted: all 8 (the longest ids) followed by itself from state/config: {len([x for x in ha.subs if x[0] == a.name])}")
    await until(lambda: a.st("arbitration_peers") == 1, 10)
    won, sc, _ = await round_()
    check(won == "a" and sc == 1102, f"the Sonos played all along: its state at once, precedence at once (25 % kept): {won} {sc}")

    # Home Assistant gone: what played counts as stopped, for the hold
    await a.cli.disconnect(); ha.drop(a); await asyncio.sleep(0.3)
    m = media(a)
    check(re.fullmatch(rf"{SONOS} stopped [01] s ago", m.get("here") or "") and "counts as stopped, Home Assistant is gone" in a.text(),
          f"Home Assistant gone: the Sonos counts as stopped: {m.get('here')}")
    await asyncio.sleep(1.5)
    m = media(a)
    check(m.get("here") is None and m.get("linked") is False and {x["state"] for x in m.get("players", [])} == {"unknown"},
          f"after the hold: no media, not linked, states unknown (not missing): {m}")
    ha.media[SONOS] = "idle"                        # meanwhile, with nobody following
    await a.connect(ha); await asyncio.sleep(0.5)
    won, sc, _ = await round_()
    check(won == "b" and sc == 500, f"back, the Sonos idle now: no precedence: {won} {sc}")
    a.cli.send_home_assistant_state("media_player.bathroom_radio", "", "playing"); await asyncio.sleep(0.3)
    won, sc, _ = await round_()
    check(won == "b" and sc == 500 and "bathroom_radio" not in a.text(), f"a player not listed: no effect: {won} {sc}")

    # an Echo in a conversation keeps the next wake word against 10 dB
    a.page.set(arbitration_media_ambient=10); ha.set_media(SONOS, "playing"); await asyncio.sleep(0.3)
    nb = len(b.starts); b.wake(); await until(lambda: len(b.starts) == nb + 1, 3)
    na = len(a.starts); a.wake(); b.wake(); await asyncio.sleep(1.5)
    check(len(a.starts) == na and len(b.starts) == nb + 1 and "score 500 -> 1500" in a.text(), "in a conversation: priority beats media (10 % = +10 dB)")
    b.end_pipeline(); await asyncio.sleep(0.5)

    # an Echo of an older build sees a number: the hallway Echo's plain claim just above or below
    a.page.set(arbitration_media_ambient=50)
    res = []
    for fs in (851, 751):
        fbeacon(); await asyncio.sleep(0.5)             # in rounds now (its beacons lapse after BEACON_MAX, scaled)
        drain(); n = len(a.starts); a.wake(); t = await a_claimed()
        if t: fclaim(fs)
        await asyncio.sleep(1.7)
        res.append(len(a.starts) > n)
        if len(a.starts) > n: a.end_pipeline(); await asyncio.sleep(0.5)
    check(res == [False, True], f"another Echo's claim, a number as ever: 851 beats the kitchen's 801, 751 does not: {res}")
    fbeacon(b"\x01"); await asyncio.sleep(0.5)

    # Kiosk Satellite mode: e carries it as well (5 dB + 3 dB)
    kio = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    kio.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); kio.bind(("", KIOSK)); kio.setblocking(False)
    for e in (a, b): e.page.set(arbitration_mode="kiosk")
    await until(lambda: all(((e.page.state() or {}).get("arbitration") or {}).get("kiosk_port") for e in (a, b)), 3)
    await asyncio.sleep(0.5)
    while True:
        try: kio.recv(1024)
        except BlockingIOError: break
    nb = len(b.starts); a.wake(); b.wake(); await asyncio.sleep(1.5)
    es = []
    while True:
        try: es.append(json.loads(kio.recv(1024))["e"])
        except BlockingIOError: break
    check(sorted(set(es)) == [8.0, 9.0] and len(b.starts) == nb + 1, f"Kiosk Satellite mode: the kitchen Echo claims 8.0 dB with the Sonos playing: {sorted(set(es))}")
    if len(b.starts) > nb: b.end_pipeline()
    for e in (a, b): e.page.set(arbitration_mode="hassmic")
    await until(lambda: not any(((e.page.state() or {}).get("arbitration") or {}).get("kiosk_port") for e in (a, b)), 3)
    kio.close(); await asyncio.sleep(0.5)

    # its own music: Music Assistant plays on the kitchen Echo (Sendspin), then stops
    ha.set_media(SONOS, "idle"); a.page.set(arbitration_media_ambient=25); await asyncio.sleep(1.6)
    loop = asyncio.get_running_loop(); added = asyncio.Queue()
    server = SendspinServer(loop, Identity.generate(), "fake-ma", pairing_store=InMemoryServerPairingStore())
    server.add_event_listener(lambda _s, ev: added.put_nowait(ev.client_id) if isinstance(ev, ClientAddedEvent) else None)
    playing = asyncio.Event(); stop = asyncio.Event()
    async def play(client):
        fmt = AudioFormat(sample_rate=48000, bit_depth=16, channels=2); stream = client.group.start_stream(); k = 0
        while not stop.is_set():
            s = (4000 * np.sin(2 * np.pi * 440 * np.arange(k, k + 4800) / 48000)).astype("<i2"); k += 4800
            stream.prepare_audio(np.repeat(s, 2).tobytes(), fmt); await stream.commit_audio(); await stream.sleep_to_limit_buffer(500_000)
            playing.set()
        await client.group.stop()
    try:
        await server.start_server(port=16966, discover_clients=False)
        server.connect_to_client(f"ws://127.0.0.1:{a.sendspin}/sendspin", retry_initial_connection=True, retry_indefinitely=True)
        cid = await asyncio.wait_for(added.get(), 15)
        await server.trust_unpaired(cid); await asyncio.sleep(2)
        task = asyncio.create_task(play(server.get_client(cid)))
        await asyncio.wait_for(playing.wait(), 10); await asyncio.sleep(1)
        won, sc, _ = await round_()
        check(won == "a" and sc == 1102 and "media of this room (own playback): ambient counted at 25 %" in a.text()
              and re.search(rf"media: own playback off -> on, {ROOM}$", a.text(), re.M) and re.search(rf"media: own playback on 1 s, {ROOM}$", a.text(), re.M),
              f"Music Assistant plays on it: its own playback counts (and is logged): {won} {sc}")
        stop.set(); await task; await asyncio.sleep(0.5)
        won, sc, _ = await round_()
        check(won == "a" and re.search(r"media of this room \(own playback ended [01] s ago\)", a.text()), f"stopped: counts for the hold: {won} {sc}")
        won, sc, _ = await round_()
        check(won == "b" and sc == 500, f"after the hold: no more: {won} {sc}")
    except Exception as ex:
        check(False, f"Sendspin on the kitchen Echo: {ex!r}")
    finally:
        await server.close()
    sn.close()

run(main, (a, b))
