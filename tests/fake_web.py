#!/usr/bin/env python3
"""The settings page (web.c) against build/hassmic-host: a browser's side written from the protocol (tests/webclient.py:
X25519 and BLAKE2b from Python's libraries, not web/crypto.js), plus Home Assistant (aioesphomeapi) watching the same
Echo.  Login by the action button (SIGUSR2), signed reads and writes, replays and forged signatures, two logins at once,
export and import, the settings file from before names, features (entities in Home Assistant only while on), adb with
a press of its own, revoking."""
import asyncio, gzip, json, os, re, signal, subprocess, sys, tempfile
from aioesphomeapi import APIClient
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import Browser, req

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, WEB = 16971, 16972


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


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
    with open(os.path.join(state, "preset"), "w") as f: f.write("# from another Echo's page\ndo_not_disturb=on\nno_such_thing=1\n")   # scripts/setup.sh --preset
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=os.path.join(state, "models"),
               HASSMIC_FAKE_WHISPER="1", HASSMIC_ADB_OPEN=os.path.join(state, "adb-open.root"),
               HASSMIC_FAKE_SOUND_MODEL=os.path.join(state, "aed-model"),           # which sound model there is (sound_none.c)
               HASSMIC_LOG=os.path.join(state, "log"),                              # boot.log, for the page's viewer
               HASSMIC_CLOCK_SYNCED="0")                                            # stamps: seconds since boot (clock.c)
    log = os.path.join(state, "log")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Web", "-L", "-z", "0", "-o", "0",
                             "-a", "16973", "-W", str(WEB)], env=env, stderr=open(log, "w"))
    rq = lambda *a, **k: req(WEB, *a, **k)
    try:
        await asyncio.sleep(0.6)
        cfg = lambda: dict(l.strip().split("=", 1) for l in open(os.path.join(state, "config")) if "=" in l and not l.startswith("#"))
        c0 = cfg()
        check(c0.get("noise_reduction") == "medium" and c0.get("mic_level") == "-22" and c0.get("sound_detection") == "on",
              f"the settings file from before names moved to state/config: {c0}")

        check(c0.get("do_not_disturb") == "on" and os.path.exists(os.path.join(state, "preset.applied")) and not os.path.exists(os.path.join(state, "preset"))
              and "preset applied, 1 setting; not taken:" in open(log).read() and "no_such_thing" in open(log).read(),
              "a preset (setup.sh --preset): applied once at start, the unknown line reported, the file kept as preset.applied")
        st, h, body = rq("GET", "/")
        check(st == 200 and h.get("Content-Encoding") == "gzip" and b"<title>Echo settings</title>" in gzip.decompress(body), "the page, gzip'd")
        st, h, body = rq("GET", "/crypto.js")
        check(st == 200 and b"blake2b" in gzip.decompress(body), "its scripts")
        b = Browser(WEB)
        check(b.hello["name"] == "Echo Web" and len(b.hello["pub"]) == 64, f"hello: {b.hello['name']}, {b.hello['model']}")
        st, h, _ = rq("OPTIONS", "/api/set", headers={"Origin": "http://192.0.2.1:28931", "Access-Control-Request-Method": "POST",
                                                     "Access-Control-Request-Headers": "x-hm-pub,x-hm-ctr,x-hm-mac"})
        check(st == 204 and h.get("Access-Control-Allow-Origin") == "*" and "X-HM-Mac" in h.get("Access-Control-Allow-Headers", ""),
              "CORS preflight: another Echo's page may call this one (the signature is what counts)")

        check(b.call("GET", "/api/state")[0] == 401, "not logged in: no state")
        check(b.login() == "waiting" and b.login() == "waiting", "login waits for the button")
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.5)                     # the action button
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

        # Home Assistant: the lean list, and what the page changes
        ha, by, states = await ha_connect()
        page_only = {"mic_level", "noise_reduction", "sound_detection", "debug_access_adb", "join_arbitration_network", "online_updates",
                     "bluetooth_announcements", "bluetooth_announcement_language", "soc_temperature", "cpu_usage", "wifi_motion_detection"}
        check(not page_only & set(by) and {"wake_sound", "mute", "do_not_disturb", "firmware", "equalizer_bass", "led_brightness"} <= set(by),
              f"Home Assistant lists the lean set only: {sorted(set(by))}")
        check("sound" in by and "last_request_whispered" in by and "arbitration_peers" in by and "arbitration_key" in by,
              "features that are on: their entities (sound, whisper, arbitration)")
        r = b.set(mic_level=-30, wake_sound="off"); await asyncio.sleep(0.5)
        check(r.get("applied") == 2 and cfg().get("mic_level") == "-30" and cfg().get("wake_sound") == "off", f"set two, saved: {r}")
        check(states.get(by["wake_sound"].key) is False, f"Home Assistant shows it at once: {states.get(by['wake_sound'].key)}")
        r = b.set(mic_level=5, nope=1, noise_reduction="loud")
        check(r["applied"] == 0 and r["errors"].count("\n") == 3 and cfg().get("mic_level") == "-30", f"out of range, unknown, wrong choice: refused: {r['errors']!r}")

        # a feature off: Home Assistant is sent away and its entities are gone when it comes back; on again: back
        lost = asyncio.Event(); ha.set_on_stop = None
        r = b.set(sound_detection="off"); await asyncio.sleep(0.8)
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
        b.set(sound_detection="off"); await asyncio.sleep(0.5); b.set(sound_detection="on"); await asyncio.sleep(1.5)
        st = b.state(); on = next(x["value"] for x in st["settings"] if x["name"] == "sound_detection")
        check(st["sound"]["failed"] and on == 0 and cfg().get("sound_detection") == "off" and any("could not start" in w for w in st["warnings"])
              and "model did not load), switched off" in open(log).read(),
              "the model does not load: switched off (page, settings file), and the page says why")
        try: await ha.disconnect()
        except Exception: pass
        ha, by, states = await ha_connect()
        check("sound" not in by, "...and Home Assistant no longer lists \"Sound\"")
        open(mf, "w").write("firmware\n")
        b.set(sound_detection="on"); await asyncio.sleep(1.5)
        st = b.state()
        check(not st["sound"]["failed"] and not any("could not start" in w for w in st["warnings"]) and cfg().get("sound_detection") == "on",
              "it loads again: on, the warning gone")
        try: await ha.disconnect()
        except Exception: pass
        ha, by, states = await ha_connect()

        # signatures
        body = b"wake_sound=on\n"; hd = b.headers("POST", "/api/set", body)
        check(rq("POST", "/api/set", body, hd)[0] == 200, "signed write")
        check(rq("POST", "/api/set", body, hd)[0] == 401 and cfg().get("wake_sound") == "on", "the same request again (replay): refused")
        check(b.call("POST", "/api/set", b"wake_sound=off\n", key=os.urandom(32))[0] == 401, "wrong key: refused")
        hd = b.headers("POST", "/api/set", b"wake_sound=off\n")
        check(rq("POST", "/api/set", b"mic_level=-15\n", hd)[0] == 401 and cfg().get("mic_level") == "-30", "body changed after signing: refused")
        x = Browser(WEB)
        check(x.call("GET", "/api/state")[0] == 401, "a browser that was never approved: refused")

        # adb: a press of its own
        adb_req = os.path.join(state, "adb-request")
        r = json.loads(b.call("POST", "/api/adb", b"on")[2])
        check(r["adb"] == "waiting" and not os.path.exists(adb_req) and b.state()["adb"]["waiting"], "adb asked: waits for the button, nothing asked of root yet")
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.6)
        check(os.path.exists(adb_req) and "adb over Wi-Fi approved with the button" in open(log).read(), "the button: root asked to open adb")
        b.call("POST", "/api/adb", b"off"); await asyncio.sleep(0.3)
        check(not os.path.exists(adb_req) or open(adb_req).read().strip() in ("0", "close", ""), "closed again")
        check(x.call("POST", "/api/adb", b"on")[0] == 401, "adb from a browser never approved: refused")

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

        # two at once
        y, z = Browser(WEB), Browser(WEB)
        check(y.login() == "waiting" and z.login() == "refused" and y.login() == "refused", "two browsers asking at once: both refused")
        proc.send_signal(signal.SIGUSR2); await asyncio.sleep(0.5)
        check(y.call("GET", "/api/state")[0] == 401 and z.call("GET", "/api/state")[0] == 401, "and the button approves neither")

        # revoke
        check(json.loads(b.call("POST", "/api/revoke", b.pub.hex().encode())[2]).get("revoked") is True, "revoke this browser")
        check(b.call("GET", "/api/state")[0] == 401, "revoked: refused")
        try: await ha.disconnect()
        except Exception: pass
    finally:
        proc.terminate(); proc.wait()
    if check.failed:
        print(open(log).read()); print("FAILED"); raise SystemExit(1)
    print("PASS")

asyncio.run(main())
