#!/usr/bin/env python3
"""Request padding through the signed settings page and HA's real ESPHome client."""
from aioesphomeapi import NumberInfo, NumberState
import os, subprocess
from esphome_common import check, home_assistant, run, until, sync, port_open
from webclient import Browser

PORTS = (17211, 17212, 17213, 17214, 17215)


async def restart(e, text=None):
    args = e.proc.args
    e.proc.terminate(); e.proc.wait(timeout=5)
    await until(lambda: not port_open(e.PORT))
    if text is not None:
        with open(os.path.join(e.state, "config"), "w") as f: f.write(text)
    e.proc = subprocess.Popen(args, env=e.env)
    await until(lambda: port_open(e.PORT) and port_open(e.WEB))
    e.page = Browser(e.WEB, e.page.sk)


async def main(e):
    ha = await home_assistant(e)
    check(e.pv("pad_request_audio") == 0 and e.pv("request_audio_padding") == 0,
          "padding defaults off and zero")
    assert e.pv("pad_request_audio") == 0, "missing Home Assistant VAD compatibility feature"
    settings = {s["name"]: s for s in e.page.state()["settings"]}
    toggle, number = settings["pad_request_audio"], settings["request_audio_padding"]
    check(toggle["label"] == "Home Assistant VAD compatibility" and toggle["group"] == "Features" and toggle["feature"]
          and number["label"] == "Request audio padding" and number["group"] == "Features",
          "page metadata places labeled toggle and child under Features")
    check("request_audio_padding" not in ha.by and "pad_request_audio" not in ha.by,
          "off: no HA number or switch")
    e.page.set(pad_request_audio="on")
    check(await until(lambda: not ha.cli.is_connected), "on closes HA link for relisting")
    await ha.cli.disconnect()
    ha = await home_assistant(e)
    n = ha.by.get("request_audio_padding")
    check(isinstance(n, NumberInfo) and n.name == "Request audio padding"
          and n.unit_of_measurement == "ms" and (n.min_value, n.max_value, n.step) == (0, 1000, 1),
          "on: HA number Request audio padding, 0..1000 ms")
    assert n is not None, "missing HA padding number"
    await until(lambda: ha.last(n.key, NumberState) == 800)
    check(ha.last(n.key, NumberState) == 800 and e.pv("request_audio_padding") == 800,
          "off to on seeds 800 ms in page and HA")
    ha.cli.number_command(n.key, 137)
    await sync(ha.cli)
    await until(lambda: ha.last(n.key, NumberState) == 137)
    check(e.conf().get("request_audio_padding") == "137" and e.pv("request_audio_padding") == 137,
          "HA number writes reach persisted settings and page")
    await ha.cli.disconnect()

    # Imports and config files may place the dependent value before its switch.
    text = "request_audio_padding=137\npad_request_audio=on\n"
    e.page.set(pad_request_audio="off")
    e.page.call("POST", "/api/set", text.encode())
    check(e.pv("request_audio_padding") == 137, "reversed import: enabled custom value wins over seed")
    await restart(e, text)
    check(e.pv("pad_request_audio") == 1 and e.pv("request_audio_padding") == 137,
          "reversed config load preserves enabled custom value")
    e.page.set(request_audio_padding=0)
    e.page.set(pad_request_audio="on")
    check(e.pv("request_audio_padding") == 0, "idempotent on preserves explicit zero")
    exported = e.page.call("GET", "/api/export")[2]
    check(b"pad_request_audio=on\n" in exported and b"request_audio_padding=0\n" in exported,
          "export contains enabled explicit zero")
    await restart(e)
    check(e.pv("request_audio_padding") == 0, "restart preserves enabled explicit zero")
    e.page.set(pad_request_audio="off")
    e.page.call("POST", "/api/set", exported)
    check(e.pv("pad_request_audio") == 1 and e.pv("request_audio_padding") == 0,
          "export/import roundtrip preserves enabled explicit zero")
    ha = await home_assistant(e)
    n = ha.by["request_audio_padding"]
    check(ha.last(n.key, NumberState) == 0, "numeric zero keeps HA entity exposed")
    e.page.set(pad_request_audio="on")
    await sync(ha.cli)
    check(ha.cli.is_connected and e.pv("request_audio_padding") == 0,
          "repeated on leaves HA connected and explicit zero unchanged")
    for bad in (-1, 1001, float("nan"), float("inf")):
        ha.cli.number_command(n.key, bad)
        await sync(ha.cli)
    check(e.pv("request_audio_padding") == 0, "HA rejects out of bounds and nonfinite numbers")
    for bad in (-1, 1001, "junk"):
        r = e.page.set(request_audio_padding=bad)
        check(bool(r.get("errors")) and e.pv("request_audio_padding") == 0, "page rejects invalid padding " + str(bad))
    for value in (1, 1000):
        ha.cli.number_command(n.key, value); await sync(ha.cli)
        check(e.pv("request_audio_padding") == value, "HA accepts padding bound " + str(value))
    e.page.set(pad_request_audio="off")
    check(await until(lambda: not ha.cli.is_connected), "off closes HA link for relisting")
    await ha.cli.disconnect()
    ha = await home_assistant(e)
    check("request_audio_padding" not in ha.by and e.pv("request_audio_padding") == 0,
          "off resets zero and hides number")
    ha.cli.number_command(n.key, 400); await sync(ha.cli)
    check(e.pv("request_audio_padding") == 0, "stale hidden HA number cannot write padding")
    e.page.set(request_audio_padding=400)
    check(e.pv("request_audio_padding") == 0 and e.pv("pad_request_audio") == 0,
          "hidden page writes cannot store padding or enable feature")
    await ha.cli.disconnect()
    for text in ("request_audio_padding=400\npad_request_audio=off\n",
                 "pad_request_audio=off\nrequest_audio_padding=400\n"):
        e.page.call("POST", "/api/set", text.encode())
        check(e.pv("request_audio_padding") == 0, "off import normalizes hidden nonzero")
        await restart(e, text)
        check(e.pv("request_audio_padding") == 0, "off config normalizes hidden nonzero")


run(main, PORTS)
