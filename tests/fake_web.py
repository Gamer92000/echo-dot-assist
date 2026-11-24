#!/usr/bin/env python3
"""The settings page (web.c) against build/hassmic-host: a browser's side written from the protocol (tests/webclient.py:
X25519 and BLAKE2b from Python's libraries, not web/crypto.js), plus Home Assistant (aioesphomeapi) watching the same
Echo.  Login by the action button (SIGUSR2), signed reads and writes, replays and forged signatures, two logins at once,
export and import, the settings file from before names, the speaker gain, features (entities in Home Assistant only while on), adb with
a press of its own (a request withdrawn by "off" or a revoke), revoking; a login left waiting runs out by itself (asked again,
a new one waits), and a restart clears its ring and names a new start.  One wait for the button at a time (one_wait: two
logins at once contested, nobody else armed in the time after a login was contested, ran out or was approved, a stranger's
asks against an approved browser's adb wait and the other way round, adb's "off" from another browser, with what the ring
shows; a press that served nothing holds strangers back anew, and so does a restart within the boot) and the count
of unsigned asks (ask_rate: the armed key's own not counted, an ask over the count still contests).  Files of the
page's that are no regular files (a FIFO at state/web_clients), and the list of the button's browsers kept in step
with the browsers let in (files)."""
import asyncio, gzip, json, os, re, signal, subprocess, sys, tempfile, threading, time
from aioesphomeapi import APIClient
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser, req

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, WEB = 16971, 16972
PORT2, WEB2 = 16975, 16976          # a second Echo of its own: logins that run out, a restart


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


async def until(cond, secs=5):
    """cond() polled until true or secs passed: on a loaded machine (every test at once, tests/run.py) hassmic's file
    writes and its log, which comes through a reader process, can trail the answer by more than a fixed sleep"""
    end = time.monotonic() + secs
    while not cond() and time.monotonic() < end: await asyncio.sleep(0.05)


async def ha_connect():
    """Home Assistant: connect, list, subscribe; returns (client, object_id -> info, key -> state)"""
    ha = APIClient("127.0.0.1", PORT, None); await ha.connect(login=True)
    ents, services = await ha.list_entities_services(); states = {}
    ha.subscribe_states(lambda x: states.__setitem__(x.key, getattr(x, "state", None)))
    await asyncio.sleep(0.4)
    return ha, {e.object_id: e for e in ents} | {s.name: s for s in services}, states


async def main():
    state = tempfile.mkdtemp()
    with open(os.path.join(state, "settings"), "w") as f: f.write("2 -22 1 0 0 1 0 de 2 1 -1 1 0 5 0\n")   # the file before names
    with open(os.path.join(state, "preset"), "w") as f: f.write("# from another Echo's page\ndo_not_disturb=on\nscreen_adaptive_colour=on\nno_such_thing=1\n")   # scripts/setup.sh --preset
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(state, "models"),
               HASSMIC_FAKE_WHISPER="1", HASSMIC_ADB_OPEN=os.path.join(state, "adb-open.root"),
               HASSMIC_FAKE_SOUND_MODEL=os.path.join(state, "aed-model"),           # which sound model there is (sound_none.c)
               HASSMIC_LOG=os.path.join(state, "log"),                              # boot.log, for the page's viewer
               HASSMIC_CLOCK_SYNCED="0",                                            # stamps: seconds since boot (clock.c)
               HASSMIC_FAKE_SPEAKER_GAIN=os.path.join(state, "speaker-gain"),       # an Echo Dot 2's codec control (spkgain.c)
               HASSMIC_FAKE_ADAPTIVE_COLOUR=os.path.join(state, "adaptive-colour"), # root's confirmed Binder readback
               HASSMIC_LOGIN_MS="60000:300")        # browsers log in right after another's press here: others kept back 0.3 s, not 30 (one_wait has the rule)
    with open(os.path.join(state, "speaker-gain"), "w") as f: f.write("6\n")          # stock: what the HAL's speaker path sets
    log = os.path.join(state, "log")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Web", "-L", "-z", "0", "-o", "0",
                             "-a", "16973", "-W", str(WEB)], env=env, stderr=open(log, "w"))
    rq = lambda *a, **k: req(WEB, *a, **k)
    try:
        wait_port(PORT, WEB)
        cfg = lambda: dict(l.strip().split("=", 1) for l in open(os.path.join(state, "config")) if "=" in l and not l.startswith("#"))
        c0 = cfg()
        check(c0.get("noise_reduction") == "medium" and c0.get("mic_level") == "-22" and c0.get("sound_detection") == "on",
              f"the settings file from before names moved to state/config: {c0}")

        check(c0.get("do_not_disturb") == "on" and os.path.exists(os.path.join(state, "preset.applied")) and not os.path.exists(os.path.join(state, "preset"))
              and "preset applied, 2 settings; not taken:" in open(log).read() and "no_such_thing" in open(log).read(),
              "a preset (setup.sh --preset): applied once at start, the unknown line reported, the file kept as preset.applied")
        st, h, body = rq("GET", "/")
        check(st == 200 and h.get("Content-Encoding") == "gzip" and b"<title>Echo settings</title>" in gzip.decompress(body), "the page, gzip'd")
        for src in re.findall(r'<script src="([^"]+)"', gzip.decompress(body).decode()):     # one missing from the Makefile's WEB: a dead page
            st, h, b = rq("GET", src)
            check(st == 200 and h.get("Content-Encoding") == "gzip" and len(gzip.decompress(b)) > 1000, f"the page's script {src}")
        st, h, body = rq("GET", "/crypto.js")
        check(st == 200 and b"blake2b" in gzip.decompress(body), "its scripts")
        b = Browser(WEB)
        check(b.hello["name"] == "Echo Web" and len(b.hello["pub"]) == 64 and re.fullmatch(r"[0-9a-f]{16}", b.hello.get("start", "")),
              f"hello: {b.hello['name']}, {b.hello['model']}, start {b.hello.get('start')}")
        check(b.hello.get("port") == WEB, f"hello names the page's own port (the other Echos are called on it, not a proxy's): {b.hello.get('port')}")
        st, h, _ = rq("OPTIONS", "/api/set", headers={"Origin": "http://192.0.2.1:28931", "Access-Control-Request-Method": "POST",
                                                     "Access-Control-Request-Headers": "x-hm-pub,x-hm-ctr,x-hm-mac"})
        check(st == 204 and h.get("Access-Control-Allow-Origin") == "*" and "X-HM-Mac" in h.get("Access-Control-Allow-Headers", ""),
              "CORS preflight: another Echo's page may call this one (the signature is what counts)")

        check(b.call("GET", "/api/state")[0] == 401, "not logged in: no state")
        check(b.login() == "waiting" and b.login() == "waiting", "login waits for the button")
        proc.send_signal(signal.SIGUSR2); await until(lambda: "login approved" in open(log).read())    # the action button
        check(b.login() == "approved" and "login approved" in open(log).read(), "the action button approved it")
        # the log viewer: signed, the Sendspin pairing token blanked (the page is plain HTTP)
        with open(log + ".1", "w") as f: f.write("== log rotated\nsendspin: pairing token SP:0ABCDEF234567\n")
        st, _, l0 = b.call("GET", "/api/log/0"); st1, _, l1 = b.call("GET", "/api/log/1")
        check(re.search(rb"^boot\+\d+\.\d{3} web: login approved", l0, re.M), "log lines stamped: seconds since boot while the clock is not set")
        check(st == 200 and st1 == 200 and b"login approved" in l0 and b"SP:0ABC" not in l1 and re.search(rb"pairing token SP:0\*{12}\n", l1),
              "the log and its older part, signed, with the pairing token blanked")
        check(Browser(WEB).call("GET", "/api/log/0")[0] == 401 and b.call("GET", "/api/log/2")[0] == 404, "not logged in: no log; only parts 0 and 1")
        s = b.state() or {}
        names = {x["name"]: x for x in s.get("settings", [])}
        check(names.get("mic_level", {}).get("value") == -22 and names["noise_reduction"]["choices"][names["noise_reduction"]["value"]] == "medium",
              f"signed: the state, settings as stored ({len(names)} settings)")
        check(any(c["me"] for c in s.get("clients", [])), "this browser among the approved ones")
        check("cpu" in s.get("diag", {}) and "soc_temp" in s.get("diag", {}), f"diagnostics on the page: {s.get('diag')}")
        check({n for n, x in names.items() if x["feature"]} >= {"arbitration", "sound_detection", "whisper_detection"},
              "features: arbitration, sound detection, whisper detection (Bluetooth, Wi-Fi motion: not on this PC)")

        # Adaptive colour (issue #162): root discovers the service beside hassmic and publishes actual readback.
        colour = os.path.join(state, "adaptive-colour")
        actual = lambda: next((x for x in b.state()["settings"] if x["name"] == "screen_adaptive_colour"), None)
        check("screen_adaptive_colour" not in names and cfg().get("screen_adaptive_colour") == "on"
              and "screen_adaptive_colour: no such setting" not in open(log).read(),
              "adaptive colour: preset on retained before service discovery, toggle still hidden")
        check(b.set(screen_adaptive_colour="on").get("applied") == 0, "unavailable adaptive colour: write refused")
        with open(colour, "w") as f: f.write("0\n")
        check(actual() is not None and actual()["value"] == 0 and actual()["group"] == "Lights",
              "service discovered after page load: adaptive colour appears with confirmed off state")
        r = b.set(screen_adaptive_colour="on")
        check(r.get("applied") == 1 and cfg().get("screen_adaptive_colour") == "on" and actual()["value"] == 0,
              "adaptive colour enabled: preference saved; pending or failed Binder write still displays actual off")
        b.set(mic_level=-22)
        check(cfg().get("screen_adaptive_colour") == "on", "saving another setting preserves the requested colour while Binder trails")
        args = proc.args
        proc.terminate(); proc.wait()
        proc = subprocess.Popen(args, env=env, stderr=open(log, "a")); wait_port(PORT, WEB)
        b = Browser(WEB)
        check(b.login_with_button(proc) and actual()["value"] == 0, "after restart: the toggle still shows confirmed device state")
        b.set(mic_level=-22)
        check(cfg().get("screen_adaptive_colour") == "on", "after restart: explicit on preference retained despite off readback")
        with open(colour, "w") as f: f.write("1\n")
        check(actual()["value"] == 1, "successful readback: toggle shows on")
        r = b.set(screen_adaptive_colour="off")
        check(r.get("applied") == 1 and cfg().get("screen_adaptive_colour") == "off" and actual()["value"] == 1,
              "adaptive colour disabled: preference saved, toggle waits for readback")
        os.remove(os.path.join(state, "adaptive-colour-request"))
        r = b.set(screen_adaptive_colour="off")
        check(r.get("applied") == 1 and os.path.exists(os.path.join(state, "adaptive-colour-request")),
              "explicit off again asks root to retry even though the saved preference is unchanged")
        check("screen_adaptive_colour=off\n" in b.call("GET", "/api/export")[2].decode(),
              "export keeps the requested colour rather than a temporary device override")
        with open(colour, "w") as f: f.write("0\n")
        check(actual()["value"] == 0, "disable readback: toggle shows off")
        os.remove(colour)
        b.set(mic_level=-22)
        check(actual() is None and cfg().get("screen_adaptive_colour") == "off", "service lost: toggle hidden, preference retained")
        with open(colour, "w") as f: f.write("not a boolean\n")
        check(actual() is None, "malformed readback: toggle hidden")
        with open(colour, "w") as f: f.write("0\n")

        # Root can lose the service while one response is serializing its settings. The iteration must stay stable.
        stop = threading.Event()
        def flap_colour():
            while not stop.is_set():
                os.remove(colour)
                with open(colour + ".tmp", "w") as f: f.write("0\n")
                os.replace(colour + ".tmp", colour)
        flapper = threading.Thread(target=flap_colour); flapper.start()
        try:
            snapshots = [[x["name"] for x in b.state()["settings"]] for _ in range(50)]
            check(all(len(s) == len(set(s)) and "mic_level" in s and "even_volume" in s for s in snapshots),
                  "service changes during serialization: complete settings snapshots, no duplicates or crash")
        finally:
            stop.set(); flapper.join()

        # Home Assistant: the lean list, and what the page changes
        ha, by, states = await ha_connect()
        page_only = {"mic_level", "noise_reduction", "sound_detection", "debug_access_adb", "join_arbitration_network", "online_updates",
                     "bluetooth_announcements", "bluetooth_announcement_language", "soc_temperature", "cpu_usage", "wifi_motion_detection",
                     "speaker_gain", "screen_adaptive_colour"}
        check(not page_only & set(by) and {"wake_sound", "mute", "do_not_disturb", "firmware", "equalizer_bass", "led_brightness"} <= set(by),
              f"Home Assistant lists the lean set only: {sorted(set(by))}")
        check("sound" in by and "last_request_whispered" in by and "arbitration_peers" in by and "arbitration_key" in by,
              "features that are on: their entities (sound, whisper, arbitration)")
        r = b.set(mic_level=-30, wake_sound="off"); await until(lambda: states.get(by["wake_sound"].key) is False)
        check(r.get("applied") == 2 and cfg().get("mic_level") == "-30" and cfg().get("wake_sound") == "off", f"set two, saved: {r}")
        check(states.get(by["wake_sound"].key) is False, f"Home Assistant shows it at once: {states.get(by['wake_sound'].key)}")
        r = b.set(mic_level=5, nope=1, noise_reduction="loud")
        check(r["applied"] == 0 and r["errors"].count("\n") == 3 and cfg().get("mic_level") == "-30", f"out of range, unknown, wrong choice: refused: {r['errors']!r}")

        # a feature off: Home Assistant is sent away and its entities are gone when it comes back; on again: back
        lost = asyncio.Event(); ha.set_on_stop = None
        r = b.set(sound_detection="off"); await until(lambda: "entities changed: links closed" in open(log).read())
        check(r.get("applied") == 1 and "entities changed: links closed" in open(log).read(), "a feature switched: the link to Home Assistant closed")
        try: await ha.disconnect()
        except Exception: pass
        ha, by, states = await ha_connect()
        check("sound" not in by and "last_request_whispered" in by, "reconnected: sound detection off, no \"Sound\" entity; the others stay")
        b.set(whisper_detection="off", sound_detection="on"); await asyncio.sleep(0.8)
        try: await ha.disconnect()
        except Exception: pass
        ha, by, states = await ha_connect()
        check("sound" in by and "last_request_whispered" not in by, "whisper off and sound on: the list follows")
        log_n = len(open(log).read())
        b.set(sound_detection="on"); await asyncio.sleep(0.5)
        check("links closed" not in open(log).read()[log_n:], "the same value again: Home Assistant left alone")

        # sound detection's model: which one the page names, and a start that fails because it does not load
        mf = os.path.join(state, "aed-model")
        check(b.state()["sound"] == {"model": "firmware", "failed": False}, f"sound model: the firmware's: {b.state()['sound']}")
        open(mf, "w").write("newer\n")
        check(b.state()["sound"]["model"] == "newer", "Amazon's newer model installed: the page names it")
        open(mf, "w").write("broken\n")
        b.set(sound_detection="off"); await asyncio.sleep(0.5); b.set(sound_detection="on")
        await until(lambda: b.state()["sound"]["failed"] and "model did not load), switched off" in open(log).read())
        st = b.state(); on = next(x["value"] for x in st["settings"] if x["name"] == "sound_detection")
        check(st["sound"]["failed"] and on == 0 and cfg().get("sound_detection") == "off" and any("could not start" in w for w in st["warnings"])
              and "model did not load), switched off" in open(log).read(),
              "the model does not load: switched off (page, settings file), and the page says why")
        try: await ha.disconnect()
        except Exception: pass
        ha, by, states = await ha_connect()
        check("sound" not in by, "...and Home Assistant no longer lists \"Sound\"")
        open(mf, "w").write("firmware\n")
        b.set(sound_detection="on"); await until(lambda: not b.state()["sound"]["failed"] and cfg().get("sound_detection") == "on")
        st = b.state()
        check(not st["sound"]["failed"] and not any("could not start" in w for w in st["warnings"]) and cfg().get("sound_detection") == "on",
              "it loads again: on, the warning gone")
        try: await ha.disconnect()
        except Exception: pass
        ha, by, states = await ha_connect()

        # the speaker gain (spkgain.c): dB over stock on the page; put back whenever the HAL writes stock, else left alone
        spk = os.path.join(state, "speaker-gain"); ctl = lambda: open(spk).read().strip()
        g = next((x for x in b.state()["settings"] if x["name"] == "speaker_gain"), {})
        check(g.get("group") == "Sound" and g.get("value") == 0 and (g.get("min"), g.get("max")) == (0, 12) and not g.get("export")
              and ctl() == "6", f"speaker gain on the page: +0 dB, up to +12, not exported; the control left at stock: {g}")
        r = b.set(speaker_gain=6); await until(lambda: ctl() == "12")
        check(r.get("applied") == 1 and ctl() == "12" and cfg().get("speaker_gain") == "6", "+6 dB: the control at 12, saved")
        open(spk, "w").write("6\n"); await until(lambda: ctl() == "12" and "speaker gain: +6 dB (fake 12, was 6)" in open(log).read())
        check(ctl() == "12" and "speaker gain: +6 dB (fake 12, was 6)" in open(log).read(), "the HAL's speaker path put stock back: set again")
        open(spk, "w").write("11\n"); await asyncio.sleep(0.6)
        check(ctl() == "11", "the 3.5 mm jack's level (11): left alone")
        open(spk, "w").write("6\n"); await until(lambda: ctl() == "12")
        check(ctl() == "12", "back on the speaker: +6 dB again")
        r = b.set(speaker_gain=13)
        check(r["applied"] == 0 and ctl() == "12", f"+13 dB: out of range, refused: {r['errors']!r}")
        b.set(speaker_gain=0); await until(lambda: ctl() == "6")
        check(ctl() == "6" and cfg().get("speaker_gain") == "0", "+0 dB: stock again")

        # signatures
        body = b"wake_sound=on\n"; hd = b.headers("POST", "/api/set", body)
        check(rq("POST", "/api/set", body, hd)[0] == 200, "signed write")
        check(rq("POST", "/api/set", body, hd)[0] == 401 and cfg().get("wake_sound") == "on", "the same request again (replay): refused")
        # the page's first calls go out together and arrive in any order: each counter counts once, whichever comes first;
        # one left behind more than the window (64 taken) counts no more
        c0 = b.ctr; b.ctr += 3
        late, early = b.headers("GET", "/api/artifacts", b"", ctr=c0 + 1), b.headers("GET", "/api/artifacts", b"", ctr=c0 + 2)
        check(rq("GET", "/api/artifacts", b"", early)[0] == 200 and rq("GET", "/api/artifacts", b"", late)[0] == 200,
              "two requests that crossed on the way: both taken")
        check(rq("GET", "/api/artifacts", b"", late)[0] == 401 and rq("GET", "/api/artifacts", b"", early)[0] == 401, "either again (replay): refused")
        kept = b.headers("GET", "/api/artifacts", b"")                  # signed, held back while newer ones pass: the floor
        for _ in range(65): b.call("GET", "/api/artifacts")             # rises to the first of them once 65 are in
        check(rq("GET", "/api/artifacts", b"", kept)[0] == 401, "held back past 65 newer ones: refused")
        kept = b.headers("GET", "/api/artifacts", b"")
        for _ in range(60): b.call("GET", "/api/artifacts")
        check(rq("GET", "/api/artifacts", b"", kept)[0] == 200 and rq("GET", "/api/artifacts", b"", kept)[0] == 401, "held back past 60: still taken, once")
        check(b.call("POST", "/api/set", b"wake_sound=off\n", key=os.urandom(32))[0] == 401, "wrong key: refused")
        hd = b.headers("POST", "/api/set", b"wake_sound=off\n")
        check(rq("POST", "/api/set", b"mic_level=-15\n", hd)[0] == 401 and cfg().get("mic_level") == "-30", "body changed after signing: refused")
        x = Browser(WEB)
        check(x.call("GET", "/api/state")[0] == 401, "a browser that was never approved: refused")

        # adb: a press of its own
        adb_req = os.path.join(state, "adb-request")
        r = json.loads(b.call("POST", "/api/adb", b"on")[2])
        check(r["adb"] == "waiting" and not os.path.exists(adb_req) and b.state()["adb"]["waiting"], "adb asked: waits for the button, nothing asked of root yet")
        proc.send_signal(signal.SIGUSR2); await until(lambda: os.path.exists(adb_req) and "adb over Wi-Fi approved with the button" in open(log).read())
        check(os.path.exists(adb_req) and "adb over Wi-Fi approved with the button" in open(log).read(), "the button: root asked to open adb")
        b.call("POST", "/api/adb", b"off"); await asyncio.sleep(0.3)
        check(not os.path.exists(adb_req) or open(adb_req).read().strip() in ("0", "close", ""), "closed again")
        check(x.call("POST", "/api/adb", b"on")[0] == 401, "adb from a browser never approved: refused")
        # a request that waits goes with "off", and with the login of the browser that asked: the next press opened adb
        # for 30 minutes all the same, and the page went on saying it waited for the button
        c = Browser(WEB)
        check(c.login_with_button(proc), "a second browser logged in")
        r1, r2 = json.loads(c.call("POST", "/api/adb", b"on")[2]), json.loads(c.call("POST", "/api/adb", b"off")[2])
        os.remove(adb_req)                                              # "off" asked root to close: taken
        await until(lambda: "adb request withdrawn" in open(log).read())
        check(r1["adb"] == "waiting" and r2["adb"] == "closed" and not b.state()["adb"]["waiting"] and "adb request withdrawn" in open(log).read(),
              "adb asked, then \"off\" before the press: the request withdrawn, the page no longer waits")
        proc.send_signal(signal.SIGUSR2); await until(lambda: os.path.exists(adb_req), 1)
        check(not os.path.exists(adb_req), "...and the next press asks root for nothing")
        r1 = json.loads(c.call("POST", "/api/adb", b"on")[2])
        r2 = json.loads(b.call("POST", "/api/revoke", c.pub.hex().encode())[2])
        check(r1["adb"] == "waiting" and r2.get("revoked") is True and not b.state()["adb"]["waiting"],
              "adb asked, then that browser's login revoked: the request withdrawn as well")
        proc.send_signal(signal.SIGUSR2); await until(lambda: os.path.exists(adb_req), 1)
        check(not os.path.exists(adb_req), "...and the next press asks root for nothing")

        # export / import
        st, h, text = b.call("GET", "/api/export"); text = text.decode()
        keys = {l.split("=")[0] for l in text.splitlines() if "=" in l and not l.startswith("#")}
        check(st == 200 and "mic_level" in keys and "sound_detection" in keys and "mute" not in keys and "do_not_disturb" not in keys
              and "Echo Web" not in text.split("\n", 1)[1], f"export: settings and features, not this Echo's state or name ({len(keys)} lines)")
        b.set(mic_level=-20, noise_reduction="off")
        st, _, body = b.call("POST", "/api/set", text.encode())
        r = json.loads(body)
        check(r["applied"] == len(keys) and not r["errors"] and cfg().get("mic_level") == "-30" and cfg().get("noise_reduction") == "medium",
              f"import: all {r['applied']} back")

        # the name (main.c name_make): the model and the MAC's end, set nowhere; names of before are not taken
        sf = lambda f: os.path.join(state, f)
        mdns = lambda *a: subprocess.run([f"{ROOT}/build/hassmic-host", "-P", "esphome", *a, "-S"], env=env, capture_output=True, text=True).stdout
        check(req(WEB, "POST", "/api/name", b"0 Kitchen")[0] == 404 and not os.path.exists(sf("name")), "no rename on the page any more")
        with open(sf("name"), "w") as f: f.write("Old Name\n")
        with open(sf("node"), "w") as f: f.write("old-node\n")
        m = mdns()
        check("Old Name" not in m and "old-node" not in m and re.search(r"friendly_name=Echo Dot 3( [0-9a-f]{6})?<", m)
              and re.search(r"<name>echo-dot-3(-[0-9a-f]{6})?</name>", m), "without -n: the model's name (and the MAC's end), not the old one")
        os.remove(sf("name")); os.remove(sf("node"))

        # the note (issue #153): one line under the name, in the unsigned hello (the login page shows it), set signed
        hello = lambda: json.loads(rq("GET", "/api/hello")[2])
        check(hello().get("note") == "" and not os.path.exists(sf("note")), "no note at first: an empty one in hello")
        check(rq("POST", "/api/note", b"Kitchen")[0] == 401 and hello()["note"] == "", "note: unsigned refused")
        st, _, body = b.call("POST", "/api/note", '  Kitchen,\n by the "window"\t '.encode())
        want = 'Kitchen,  by the "window"'
        check(st == 200 and json.loads(body).get("note") == want and hello()["note"] == want and (b.state() or {}).get("device", {}).get("note") == want
              and open(sf("note")).read() == want + "\n", f"note set: one line, trimmed, in hello and the state: {body!r}")
        st, _, body = b.call("POST", "/api/note", ("x" + "ä" * 50).encode())
        check(st == 200 and hello()["note"] == "x" + "ä" * 39, "a long note: cut at 80 bytes, before the character the limit splits")
        b.call("POST", "/api/note", "Küche".encode() + b"\xc3")
        check(hello()["note"] == "Küche", "a body that ends inside a character: that character left out")
        b.call("POST", "/api/note", "Wohnzimmer, Regal".encode())
        args = proc.args
        proc.terminate(); proc.wait()
        # (as after a reboot: within one boot the adb waits withdrawn above keep a browser that is not approved back
        # for their minute over a restart too, state/web_wait; one_wait's "restart" has that)
        with open(os.path.join(state, "boot-2"), "w") as f: f.write("another-boot\n")
        env = dict(env, HASSMIC_BOOT_ID=os.path.join(state, "boot-2"))
        proc = subprocess.Popen(args, env=env, stderr=open(log, "a")); wait_port(PORT, WEB)
        b = Browser(WEB)
        check(hello()["note"] == "Wohnzimmer, Regal" and b.login_with_button(proc), "the note after a restart")
        check(b"Wohnzimmer" not in b.call("GET", "/api/export")[2], "not in the settings file: it is this Echo's")
        st, _, body = b.call("POST", "/api/note", b"  ")
        check(st == 200 and json.loads(body).get("note") == "" and hello()["note"] == "" and not os.path.exists(sf("note")), "an empty note clears it")

        # identify (main.c core_identify): from the page, and Home Assistant's button
        check(rq("POST", "/api/identify")[0] == 401, "identify: unsigned refused")
        st, _, body = b.call("POST", "/api/identify")
        await until(lambda: open(log).read().count("identify: rainbow") == 1)
        check(st == 200 and json.loads(body).get("identify") is True and open(log).read().count("identify: rainbow") == 1, "identify from the page")
        ha2, by2, _ = await ha_connect()
        btn = by2.get("identify")
        check(btn is not None and btn.device_class == "identify" and int(btn.entity_category) == 2, "Home Assistant: an Identify button (diagnostic)")
        if btn is not None:
            ha2.button_command(btn.key); await until(lambda: open(log).read().count("identify: rainbow") == 2)
            check(open(log).read().count("identify: rainbow") == 2, "pressed in Home Assistant: identifies")

        # restart (main.c core_reboot): root reboots on state/reboot (main.sh ota_watch)
        rb = sf("reboot")
        check(rq("POST", "/api/reboot", b"reboot")[0] == 401 and not os.path.exists(rb), "reboot: unsigned refused")
        check(b.call("POST", "/api/reboot", b"yes")[0] == 400 and not os.path.exists(rb), "reboot: only with the body \"reboot\"")
        st, _, body = b.call("POST", "/api/reboot", b"reboot")
        await until(lambda: os.path.exists(rb))       # the answer goes out first (web.c), the request to root right after it
        check(st == 200 and json.loads(body).get("reboot") is True and open(rb).read() == "settings page\n", "reboot from the page: root asked")
        os.remove(rb)
        rbtn = by2.get("restart")
        check(rbtn is not None and rbtn.device_class == "restart" and int(rbtn.entity_category) == 1, "Home Assistant: a Restart button (configuration)")
        if rbtn is not None:
            ha2.button_command(rbtn.key); await until(lambda: os.path.exists(rb))
            check(os.path.exists(rb) and open(rb).read() == "Home Assistant's button\n", "pressed in Home Assistant: root asked to reboot")
            if os.path.exists(rb): os.remove(rb)
        try: await ha2.disconnect()
        except Exception: pass

        # (two browsers at once, and what comes after a login ended: one_wait() below, on an Echo with short times)
        # revoke
        w = Browser(WEB)
        check(w.login_with_button(proc), "another browser logged in")
        check(json.loads(b.call("POST", "/api/revoke", w.pub.hex().encode())[2]).get("revoked") is True and w.call("GET", "/api/state")[0] == 401,
              "revoke another browser: refused from then on")
        await asyncio.sleep(0.4)                        # (the quiet time after w's own press, 0.3 s here: it is a stranger again)
        check(w.login() == "waiting", "...and when it asks to log in again, it waits for the button (it was told \"approved\", and kept out)")
        check(json.loads(b.call("POST", "/api/revoke", b.pub.hex().encode())[2]).get("revoked") is True, "revoke this browser")
        check(b.call("GET", "/api/state")[0] == 401, "revoked: refused")
        try: await ha.disconnect()
        except Exception: pass

        await login_runs_out()
        await one_wait()
        await ask_rate()
        await files()
    finally:
        proc.terminate(); proc.wait()
    if check.failed:
        print(open(log).read()); print("FAILED"); raise SystemExit(1)
    print("PASS")

async def login_runs_out():
    """A page closed while its login waits: the ring's authenticated_setup_mode goes off when the login runs out, with no
    request asking about it (it stayed on all night on an Echo 2, 2026-10-25), and a restart unsets what the last run
    left on.  /api/hello names each start, so the page tells the new process from the old one (web/app.js waitBack)"""
    state = tempfile.mkdtemp(); leds = os.path.join(state, "ledctrl.log"); log = os.path.join(state, "log")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_FAKE_LED=leds,
               HASSMIC_LOGIN_MS="1500")
    run = lambda: subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT2), "-n", "Echo Two", "-L", "-z", "0",
                                    "-o", "0", "-a", "16977", "-W", str(WEB2)], env=env, stderr=open(log, "a"))
    lines = lambda: open(leds).read().splitlines() if os.path.exists(leds) else []
    proc = run()
    try:
        wait_port(PORT2, WEB2)
        check("-u authenticated_setup_mode" in lines(), "start: a login's ring the last run left waiting is unset")
        b = Browser(WEB2); first = b.hello.get("start")
        check(b.login() == "waiting" and "-s authenticated_setup_mode" in lines(), "a login waits: the ring says so")
        open(leds, "w").close()
        await until(lambda: "-u authenticated_setup_mode" in lines() and "login ran out without the button" in open(log).read())
        check("-u authenticated_setup_mode" in lines() and "login ran out without the button" in open(log).read(),
              "the page went away: the login runs out by itself, the ring off, with nothing asking")
        check(b.login() == "refused", "...and the browser is told so when it asks again")
        check(b.login() == "waiting", "asked once more (the page's Try again): a new login waits (it was refused at once until another key asked)")
        proc.send_signal(signal.SIGUSR2); await until(lambda: "login approved" in open(log).read())
        check(b.login() == "approved", "...and the button approves it")
        check(all(s["name"] != "screen_adaptive_colour" for s in b.state()["settings"]),
              "device without adaptive colour: no toggle")
        c = Browser(WEB2); a = c.ask()
        await asyncio.sleep(0.8)                        # (no other browser's login is armed for half a wait after a press: one_wait)
        check(a.get("why") == "busy" and c.login() == "waiting", "another browser asks: not armed right after that press, armed a moment later")
        await until(lambda: open(log).read().count("login ran out without the button") == 2)
        await asyncio.sleep(3.5)                        # past web.c's REFUSED_MS (3 s) after it ran out, nobody asking meanwhile
        check(c.login() == "waiting", "asked again long after it ran out (the page opened again): a new login waits, not \"refused\" at once")
        proc.terminate(); proc.wait(); proc = run(); wait_port(PORT2, WEB2)
        again = Browser(WEB2).hello.get("start")
        check(again and first and again != first, f"restarted: /api/hello names another start ({first} -> {again})")
    finally:
        proc.terminate(); proc.wait()


async def one_wait():
    """One wait for the action button at a time, and a press is not turned from the wait it was armed for to another
    browser (web.c "what waits for the action button"; two reviews, 2026-11-20.  The first: a second key asking ended
    whatever waited and took its place, and the owner's press approved it.  The second: that held for an approved
    browser's waits only; after a login had ended, contested, run out or approved, the next key was armed at once and
    got the press that was on its way: cases a, b, c below, each with the second browser left asking).  Times here: a
    wait 4 s, and 2 s of quiet after every wait in which no other browser that is not approved is armed; a contested
    login is held for the first one's 4 s and those 2 s.  The ring: what ledctrl was asked (HASSMIC_FAKE_LED)"""
    state = tempfile.mkdtemp(); leds = os.path.join(state, "ledctrl.log"); log = os.path.join(state, "log"); adb_req = os.path.join(state, "adb-request")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_FAKE_LED=leds,
               HASSMIC_ADB_OPEN=os.path.join(state, "adb-open.root"), HASSMIC_LOGIN_MS="4000:2000")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT2), "-n", "Echo Two", "-L", "-z", "0",
                             "-o", "0", "-a", "16977", "-W", str(WEB2)], env=env, stderr=open(log, "a"))
    text = lambda: open(log).read()
    ring = lambda: [l.split()[-2] + " " + l.split()[-1] for l in open(leds).read().splitlines() if "authenticated_setup_mode" in l or l.endswith(" wait")] if os.path.exists(leds) else []
    last = lambda: (ring() or ["none"])[-1]
    armed = lambda: ring().count("-s authenticated_setup_mode")
    approved = lambda: text().count("login approved")
    out = lambda b: b.call("GET", "/api/state")[0] == 401
    ON, OFF = "-s authenticated_setup_mode", "-u authenticated_setup_mode"

    pressed = [0.0]

    async def press():
        """a press, and whether it went on to the assistant (served no wait)"""
        n = text().count("button: "); proc.send_signal(signal.SIGUSR2); await until(lambda: text().count("button: ") > n, 1.5)
        pressed[0] = time.monotonic()
        return text().count("button: ") > n

    async def quiet(t):
        """until T, and until the quiet time after the last press that served nothing: such a press holds strangers
        back for 2 s of its own (whoever pressed may press again)"""
        await asyncio.sleep(max(0, t - time.monotonic(), pressed[0] + 2.3 - time.monotonic()))

    async def asking(b, secs, every=0.3):
        """b left asking for SECS: the reasons it was given ("waiting" where it was armed)"""
        end = time.monotonic() + secs; got = []
        while time.monotonic() < end: a = b.ask(); got.append(a.get("why") or a["login"]); await asyncio.sleep(every)
        return got
    try:
        wait_port(PORT2, WEB2); await asyncio.sleep(0.3)
        # ---- a: contested, then one of the two left asking alone
        y, z = Browser(WEB2), Browser(WEB2)
        t0 = time.monotonic()
        check(y.login() == "waiting" and last() == ON, "a login waits: armed, the ring shows it")
        a = z.ask()
        check(a["login"] == "refused" and a.get("why") == "contested" and 4 <= a.get("retry_s", 0) <= 6,
              f"a second browser asks meanwhile: told it is contested, and for how long: the first one's whole time and the quiet time after it ({a})")
        await until(lambda: last() == OFF)
        check(last() == OFF, "...the ring goes dark: a press approves nobody now")
        a = y.ask()
        check(a["login"] == "refused" and a.get("why") == "contested", "...and the first is told the same, not \"waiting\" (its page says so; its owner may be on his way already)")
        went = await press()
        check(went and approved() == 0 and out(y) and out(z), "the press: nobody is approved, and it is the assistant's as any press (not swallowed)")
        # the first one's page asks no more; the second keeps asking alone, past the ten seconds' worth (0.7 s here) that
        # used to be the whole hold: it was armed then, with the ring the first one's owner was walking towards
        got = await asking(z, t0 + 3.6 - time.monotonic())
        went = await press()                             # the owner arrives: within the minute his page gave him
        got += await asking(z, t0 + 5.4 - time.monotonic())
        went2 = await press()                            # ...or a little late
        check(set(got) == {"contested"} and armed() == 1 and went and went2 and approved() == 0 and out(z),
              f"a: the second browser left asking alone while the first one's owner walks over: never armed in the first one's time or the quiet time after it, and his presses let nobody in ({len(got)} asks)")
        a = z.ask(); b2 = z.ask()
        check(a.get("why") == "contested" and b2.get("why") == "contested" and b2.get("retry_s", 9) <= 2, f"...its own asks make that time no longer ({b2})")
        # the press at 5.4 s found nothing to serve: the record is held for 2 s from it, not only from the arming
        # (web_press.py case 1: armed at 6.0 s, the owner's next press let the stranger in)
        got = await asking(z, t0 + 6.9 - time.monotonic())
        check(set(got) == {"contested"} and armed() == 1, f"...and the owner's late press holds it back for the quiet time once more: not armed at the first one's 6 s ({len(got)} asks)")
        await quiet(t0 + 6.2)
        t1 = time.monotonic()
        check(z.login() == "waiting" and last() == ON and armed() == 2, f"...and once it is over ({t1 - t0:.1f} s after the first was armed) the one that asks alone has its turn: a login as any other")

        # ---- b: a login runs out, and another browser asks at once
        await until(lambda: text().count("login ran out without the button") == 1, 6)
        x = Browser(WEB2); a = x.ask()
        went = await press()                             # the late press, for the login that just ran out
        got = await asking(x, min(1.2, t1 + 5.6 - time.monotonic()))
        check(a["login"] == "refused" and a.get("why") == "busy" and 1 <= a.get("retry_s", 0) <= 2 and set(got) == {"busy"} and armed() == 2 and went and approved() == 0 and out(x) and out(z),
              f"b: another browser asking as a login runs out, and left asking: not armed in the quiet time after it, the late press lets nobody in ({a})")
        # (web_press.py case 2: the late press at ~4.2 s, the stranger armed at 6.0 s, the second press let it in)
        got = await asking(x, pressed[0] + 1.7 - time.monotonic()); went = await press()
        got += await asking(x, 1.5)
        check(set(got) == {"busy"} and armed() == 2 and went and approved() == 0, f"...nor for the quiet time after that press, nor after the next: each press that served nothing keeps strangers back anew ({len(got)} asks)")
        await quiet(t1 + 6.3)
        check(x.login() == "waiting" and last() == ON and armed() == 3, "...after the quiet time it is armed as any login")

        # ---- c: a login approved, and another browser asks at once
        tp = time.monotonic(); proc.send_signal(signal.SIGUSR2); await until(lambda: approved() == 1); went = False
        check(not went and approved() == 1 and x.login() == "approved", "the press approves that one")
        s2 = Browser(WEB2); a = s2.ask()
        went = await press()                             # the second press of someone not sure the first one took
        got = await asking(s2, min(1.2, tp + 1.6 - time.monotonic()))
        check(a["login"] == "refused" and a.get("why") == "busy" and 1 <= a.get("retry_s", 0) <= 2 and set(got) == {"busy"} and armed() == 3 and went and approved() == 1 and out(s2),
              f"c: another browser asking right after a login was approved, and left asking: not armed in the quiet time, a second press lets nobody in ({a}, {len(got)} asks)")
        await quiet(tp + 2.3)
        t2 = time.monotonic()
        check(s2.login() == "waiting" and armed() == 4, "...after the quiet time it is armed as any login")
        # its own "Try again" after its time: at once, inside the quiet time that keeps the others out
        await until(lambda: text().count("login ran out without the button") == 2, 6)
        o = Browser(WEB2); a = o.ask(); b2 = s2.ask(); t3 = time.monotonic(); c2 = s2.ask()
        check(a.get("why") == "busy" and b2.get("why") == "timeout" and c2["login"] == "waiting" and armed() == 5,
              f"a login that ran out, asked again by its own browser: armed at once (the page's Try again), while another's is not ({a})")
        a = o.ask()
        check(a.get("why") == "contested" and last() == OFF, "...and the other one asking now contests it, as two at once do")

        # ---- an approved browser's wait (adb) against a stranger's asks.  It is armed over the contested record
        open(leds, "w").close()
        r = json.loads(x.call("POST", "/api/adb", b"on")[2])
        check(r == {"adb": "waiting", "holds": "adb"} and last() == ON, f"adb asked by an approved browser while that contest is held: armed at once ({r})")
        got = [o.ask() for _ in range(3)]
        check(all(g["login"] == "refused" and g.get("why") == "busy" for g in got), f"a stranger asks to log in three times meanwhile: busy each time ({got[0]})")
        st = x.state()
        check(st["adb"]["waiting"] and st["button"]["holds"] == "adb" and st["button"]["mine"] and st["button"]["left_s"] > 0 and OFF not in ring()
              and "withdrawn" not in text(),
              "...the adb wait is as it was: still armed, the ring never went off, and the page is told what the press serves")
        went = await press(); await until(lambda: os.path.exists(adb_req))
        await until(lambda: "adb over Wi-Fi approved with the button" in text(), 3)     # (the log: through its reader process)
        check(not went and os.path.exists(adb_req) and "adb over Wi-Fi approved with the button" in text() and approved() == 1 and out(o),
              "the press opens adb and approves nobody")
        a = o.ask(); went = await press()
        check(a["login"] == "refused" and a.get("why") == "busy" and 1 <= a.get("retry_s", 0) <= 6 and went and approved() == 1 and last() == OFF,
              f"right after it the stranger still cannot be armed: a second press finds nothing of his ({a})")
        await asyncio.sleep(1.0)
        check(o.ask().get("why") == "busy", "...nor a second later")
        await quiet(t3 + 6.3)            # (the contest's own time: s2's second arming + 6 s)
        check(o.login() == "waiting" and last() == ON, "after the quiet time, and the contest's, his login is one like any other: armed, shown")

        # ---- the other way: a stranger's login is armed, an approved browser wants the button
        r = json.loads(x.call("POST", "/api/adb", b"on")[2]); st = x.state()
        check(r == {"adb": "busy", "holds": "login"} and o.login() == "waiting" and st["button"]["holds"] == "login" and not st["button"]["mine"] and not st["adb"]["waiting"],
              f"adb asked while that login waits: busy, told what the press would serve, and the login is not ended for it ({r})")
        await until(lambda: text().count("login ran out without the button") == 3, 6)
        a = o.ask(); b2 = o.ask(); st = x.state()
        check(a.get("why") in ("timeout", "busy") and b2.get("why") == "busy" and st["button"]["holds"] == "none",
              "once it ran out the stranger's own \"Try again\" is not armed (an approved browser was told busy for it), and the page is told the button holds no login any more")
        ta = time.monotonic(); r = json.loads(x.call("POST", "/api/adb", b"on")[2])
        check(r == {"adb": "waiting", "holds": "adb"} and o.ask().get("why") == "busy", "...so the approved browser's next ask (the page asks by itself then) is armed, and the stranger cannot take the button before it")
        # "off": only the browser that armed the wait withdraws it
        r = json.loads(x.call("POST", "/api/adb", b"off")[2])
        await until(lambda: text().count("adb request withdrawn") >= 1, 3)    # (the log comes through its reader process: after the answer)
        check(r["adb"] == "closed" and text().count("adb request withdrawn") == 1 and o.ask().get("why") == "busy",
              "its owner's own \"off\" ends that wait (and strangers are kept back after it)")
        await asyncio.sleep(max(0, ta + 6.2 - time.monotonic()))            # (a wait withdrawn: its whole time and the quiet time, as its page promised)
        check(o.login_with_button(proc) and approved() == 2, "a second browser let in with the button, once the quiet time after that wait is over")
        r = json.loads(x.call("POST", "/api/adb", b"on")[2]); r2 = json.loads(o.call("POST", "/api/adb", b"off")[2]); st = x.state()
        check(r == {"adb": "waiting", "holds": "adb"} and r2["adb"] == "closed" and st["adb"]["waiting"] and st["button"]["mine"] and text().count("adb request withdrawn") == 1,
              f"adb \"off\" from another approved browser: adb is closed, the wait the first one armed stays ({r2})")
        if os.path.exists(adb_req): os.remove(adb_req)
        went = await press(); await until(lambda: os.path.exists(adb_req))
        check(not went and os.path.exists(adb_req), "...and the press its owner walked over for opens adb")

        # ---- a restart within the boot (an update, a crash) forgets no wait (web_press.py case 3)
        await quiet(time.monotonic() + 2.3)
        own = Browser(WEB2); t4 = time.monotonic()
        check(own.login() == "waiting", "a login waits")
        await asyncio.sleep(0.5)
        proc.kill(); proc.wait(); proc = subprocess.Popen(proc.args, env=env, stderr=open(log, "a")); wait_port(PORT2, WEB2); await asyncio.sleep(0.3)
        n0 = approved(); st = Browser(WEB2); a = st.ask(); went = await press()
        await until(lambda: "something waited for the action button when hassmic last ended" in text(), 3)
        check(a["login"] == "refused" and a.get("why") == "busy" and 3 <= a.get("retry_s", 0) <= 6 and went and approved() == n0 and out(st) and "something waited for the action button when hassmic last ended" in text(),
              f"hassmic restarted while it waited: a stranger's first ask is not armed, and the press on its way lets nobody in ({a})")
        got = await asking(st, 1.0)
        check(set(got) == {"busy"}, "...nor while it keeps asking")
        await quiet(t4 + 6.3)
        check(st.login() == "waiting", "...once the wait's own time and the quiet time are over, counted from before the restart, a login is taken as ever")
        # ...and not a pause after every start: nothing waited, nothing is held
        await until(lambda: text().count("login ran out without the button") >= 4, 6); await asyncio.sleep(2.3)
        proc.kill(); proc.wait(); proc = subprocess.Popen(proc.args, env=env, stderr=open(log, "a")); wait_port(PORT2, WEB2); await asyncio.sleep(0.3)
        check(Browser(WEB2).login() == "waiting", "a restart with nothing waiting and no quiet time running: the first to ask is armed at once")
        # another boot: its clock is another, the file says nothing
        with open(os.path.join(state, "boot-2"), "w") as f: f.write("another-boot\n")
        proc.kill(); proc.wait(); proc = subprocess.Popen(proc.args, env=dict(env, HASSMIC_BOOT_ID=os.path.join(state, "boot-2")), stderr=open(log, "a")); wait_port(PORT2, WEB2); await asyncio.sleep(0.3)
        check(Browser(WEB2).login() == "waiting", "after a reboot (another boot id) what waited before it holds nobody back")
        # no boot id to read (it wrote nothing then, and said nothing): said once, the file names "-", and a restart
        # holds the wait by the clock alone
        noid = dict(env, HASSMIC_BOOT_ID=os.path.join(state, "no-such-file"))
        proc.kill(); proc.wait(); proc = subprocess.Popen(proc.args, env=noid, stderr=open(log, "a")); wait_port(PORT2, WEB2); await asyncio.sleep(0.3)
        own = Browser(WEB2); t5 = time.monotonic()
        await until(lambda: "this boot's id cannot be read" in text(), 3)
        check(own.login() == "waiting" and text().count("this boot's id cannot be read") == 1 and open(os.path.join(state, "web_wait")).read().startswith("- "),
              "the boot's id cannot be read: said in the log, a login is armed as ever (what the other boot left holds nobody), and the file names no boot")
        await asyncio.sleep(0.5)
        proc.kill(); proc.wait(); proc = subprocess.Popen(proc.args, env=noid, stderr=open(log, "a")); wait_port(PORT2, WEB2); await asyncio.sleep(0.3)
        n0 = approved(); st = Browser(WEB2); a = st.ask(); went = await press()
        check(a["login"] == "refused" and a.get("why") == "busy" and 3 <= a.get("retry_s", 0) <= 6 and went and approved() == n0 and out(st),
              f"...and hassmic restarted while it waited: the stranger's first ask is not armed, the press lets nobody in ({a})")
        await quiet(t5 + 6.3)
        # ---- a press that came too late for a login is a press in vain for its own key too: no "Try again" armed
        # at once after it (`again` was the one exception to "QUIET_MS from each press")
        late = Browser(WEB2); n0 = text().count("login ran out without the button")
        check(late.login() == "waiting", "a login waits")
        await until(lambda: text().count("login ran out without the button") == n0 + 1, 6)
        went = await press(); a = late.ask(); a2 = late.ask(); n1 = approved(); went2 = await press()
        check(went and a.get("why") == "timeout" and a2["login"] == "refused" and a2.get("why") == "busy" and went2 and approved() == n1 and out(late),
              f"its time ran out, then a press (the assistant's): its own Try again is not armed at once, and the next press lets nobody in ({a} {a2})")
        await quiet(time.monotonic())
        check(late.login() == "waiting", "...after the quiet time from that press it is armed as any login")
        check(proc.poll() is None, "hassmic still runs")
    finally:
        proc.terminate(); proc.wait()
    if check.failed: print(open(log).read())


async def ask_rate():
    """Unsigned asks to log in are counted per source address and overall; over the count they are refused and arm
    nothing (here 5 an address and 12 in all, per 10 s).  Not counted: the asks of the key whose login is armed.  And an
    ask over the count from another key still turns an armed login contested (web_press.py case 4: a stranger kept its
    own login armed and used the count of all up with its own "waiting" asks; the owner's open page, told "rate",
    never contested it)"""
    state = tempfile.mkdtemp(); leds = os.path.join(state, "ledctrl.log"); log = os.path.join(state, "log")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_FAKE_LED=leds, HASSMIC_LOGIN_RATE="5:12",
               HASSMIC_LOGIN_MS="4000:1000")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT2), "-n", "Echo Two", "-L", "-z", "0",
                             "-o", "0", "-a", "16977", "-W", str(WEB2)], env=env, stderr=open(log, "a"))
    text = lambda: open(log).read()
    dark = lambda: "-u authenticated_setup_mode" in open(leds).read().split("-s authenticated_setup_mode")[-1]
    try:
        wait_port(PORT2, WEB2); await asyncio.sleep(0.3)
        a, o = Browser(WEB2), Browser(WEB2)
        got = [a.ask(src=f"127.0.0.{2 + i % 3}") for i in range(40)]
        check(all(g["login"] == "waiting" for g in got) and "more asks to log in than" not in text(),
              "forty asks from three addresses by the browser whose login is armed: \"waiting\" each time, and none of them counted")
        r = o.ask(src="127.0.0.9")
        check(r["login"] == "refused" and r.get("why") == "contested" and dark(), f"...so another browser's ask is still taken, and contests it: the ring goes dark ({r})")
        await asyncio.sleep(5.3)                         # the contest's time: 4 s + 1 s
        # the count used up by keys that are not armed; then one is, and an ask over the count meets it
        t0 = time.monotonic(); junk = [Browser(WEB2) for _ in range(3)]
        check(a.ask(src="127.0.0.2")["login"] == "waiting", "a login is armed again")
        got = [junk[i % 3].ask(src="127.0.0.7") for i in range(5)]
        over = junk[0].ask(src="127.0.0.7"); own = a.ask(src="127.0.0.7")
        check(got[0].get("why") == "contested" and over["login"] == "refused" and over.get("why") == "rate" and 1 <= over.get("retry_s", 0) <= 10,
              f"five asks from one address are taken, the sixth is not ({over})")
        await asyncio.sleep(max(0, t0 + 5.3 - time.monotonic()))
        check(a.ask(src="127.0.0.2")["login"] == "waiting" and not dark(), "once that contest is over the first browser is armed anew")
        n0 = text().count("a second browser asked")
        over = o.ask(src="127.0.0.7"); own = a.ask(src="127.0.0.2")
        await until(lambda: text().count("a second browser asked") >= n0 + 1, 3)
        check(over.get("why") == "rate" and dark() and own.get("why") == "contested" and text().count("a second browser asked") == n0 + 1,
              f"another browser's ask over the count: refused for the count, and it still contests the login that waits: the ring dark, its browser told ({over}, {own})")
        for _ in range(4): o.ask(src="127.0.0.7")
        check(text().count("a second browser asked") == n0 + 1, "...said once in the log for that login, however many more come over the count")
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.4)
        check("login approved" not in text(), "...and a press lets nobody in")
        # the count of all: eight addresses' worth
        await asyncio.sleep(10.2)
        got = [junk[i % 3].ask(src=f"127.0.0.{20 + i // 4}") for i in range(12)]; over = junk[1].ask(src="127.0.0.30")
        check(all(g.get("why") != "rate" for g in got) and over.get("why") == "rate", f"twelve from all addresses together: the thirteenth is not taken, from whichever address ({over})")
        await asyncio.sleep(0.3)
        check(1 <= text().count("more asks to log in than") <= 3, "said in the log once a window, not per ask")
    finally:
        proc.terminate(); proc.wait()
    if check.failed: print(open(log).read())


async def files():
    """What the page reads at start is regular files or nothing (a FIFO at state/web_clients or state/web_button had
    web_start wait for a writer for good, with its lock held); state/web_button is written again when a browser of the
    button's is pushed out of the list of 32, so that it is none after a restart when it came back on nothing but a
    voucher's worth (here: put back by hand into web_clients, as a login on a voucher would), and at a start that
    finds a line in it naming no browser of the list"""
    state = tempfile.mkdtemp(); log = os.path.join(state, "log")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_LOGIN_MS="4000:1")
    run = lambda: subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT2), "-n", "Echo Two", "-L", "-z", "0",
                                    "-o", "0", "-a", "16977", "-W", str(WEB2)], env=env, stderr=open(log, "a"))
    sf = lambda n: os.path.join(state, n)
    os.mkfifo(sf("web_clients")); os.mkfifo(sf("web_button")); os.mkfifo(sf("web_wait"))
    proc = run()
    try:
        wait_port(PORT2, WEB2, secs=5); up = True
        try: b = Browser(WEB2)
        except OSError: up = False
        check(up and b.login_with_button(proc), "FIFOs where the page keeps its browsers, the button's and its waits: it starts all the same, and a browser is let in")
        import stat
        check(stat.S_ISREG(os.stat(sf("web_clients")).st_mode) and stat.S_ISREG(os.stat(sf("web_button")).st_mode), "...and the lists are files again")
        check(b.pub.hex() in open(sf("web_button")).read(), "the browser let in with the button is listed as one")
        # 32 more with the button: the first is pushed out of the list, and out of the button's
        for i in range(32):
            if not Browser(WEB2).login_with_button(proc): check(False, f"browser {i + 2} let in"); break
            await asyncio.sleep(0.05)
        check(b.call("GET", "/api/state")[0] == 401 and b.pub.hex() not in open(sf("web_clients")).read(), "after 32 more the first browser is out of the list")
        check(b.pub.hex() not in open(sf("web_button")).read(), "...and out of the button's list with it (it stayed there: back on a voucher, a restart made it a browser of the button's again)")
        proc.terminate(); proc.wait()
        lines = open(sf("web_clients")).read().splitlines()
        with open(sf("web_clients"), "w") as f: f.write("\n".join(lines[1:] + [f"{b.pub.hex()} {b.ctr} back on a voucher"]) + "\n")
        proc = run(); wait_port(PORT2, WEB2)
        me = next((c for c in (b.state() or {}).get("clients", []) if c["me"]), None)
        check(me is not None and me["vouched"] is True, f"...so after a restart, in the list again without the button, it counts as let in on a voucher ({me})")
        # A line of web_button that names no browser of the list (a revoke or a push out whose rewrite of the file
        # failed; here the browser taken out of web_clients by hand above, whose line stayed): gone with the start
        # that finds it, so that key back on a voucher is none of the button's after the start after that
        gone = lines[0].split()[0]
        check(gone not in open(sf("web_button")).read() and "named a browser that is not approved here" in open(log).read(),
              "a line of the button's list that names no approved browser: the list is written again without it at the start")
        proc.terminate(); proc.wait()
        lines = open(sf("web_clients")).read().splitlines()
        with open(sf("web_clients"), "w") as f: f.write("\n".join(lines[1:] + [f"{gone} 0 the stale line's"]) + "\n")
        proc = run(); wait_port(PORT2, WEB2)
        it = next((c for c in (b.state() or {}).get("clients", []) if c["label"] == "the stale line's"), None)
        check(it is not None and it["vouched"] is True, f"...and that key in the list again, a restart later: let in on a voucher, not with the button ({it})")
    finally:
        proc.terminate(); proc.wait()
    if check.failed: print(open(log).read())


asyncio.run(main())
