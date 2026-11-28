#!/usr/bin/env python3
"""Show a local wake during arbitration while keeping the Assist run on the winning Echo."""
import asyncio
import os
from pathlib import Path

from arbecho import Echo, HA, check, run, start, until

net = (2, os.urandom(32))
a, b = Echo("Echo Kitchen", 16961, 500, net), Echo("Echo Living Room", 16962, 900, net)
a.env["HASSMIC_FAKE_AFE"] = "450:315:99"  # roughly 500, available only after the early ring
for echo in (a, b):
    echo.env["HASSMIC_ARB_RIVALS"] = "0"
    echo.env["HASSMIC_FAKE_LED"] = str(Path(echo.state) / "led")
    (Path(echo.state) / "config").write_text(
        "arbitration_ring=on\narbitration_window_auto=off\narbitration_window=800\n"
    )


# A finite listening design: one lit frame and a dark loop. A win after it expires must play it again.
led_root = Path(a.state) / "etc"
led_resources = led_root / "led-resources"
led_resources.mkdir(parents=True)
(led_resources / "active-start.animation").write_text(
    "200:" + ",".join(["0000FF"] * 12) + "\nloop\n1000:" + ",".join(["000000"] * 12) + "\n"
)
a.env["HASSMIC_LED_ROOT"] = str(led_root)


def ring(echo):
    path = Path(echo.env["HASSMIC_FAKE_LED"])
    return [line for line in path.read_text().splitlines() if line.endswith(" active-start")] if path.exists() else []


async def main():
    await start((a, b), HA([a, b]))
    check(await until(lambda: all(e.st("arbitration_peers") == 1 for e in (a, b)), 10),
          "both Echos joined the arbitration network")
    na, nb = len(ring(a)), len(ring(b))

    a.wake()
    check(await until(lambda: ring(a)[na:] == ["-s active-start"], 0.4) and not a.starts and not b.starts
          and "arbitration: heard it, score" not in a.text(),
          "the first Echo lights before the front end score or a competing claim, with no Assist run yet")

    b.wake()
    check(await until(lambda: len(b.starts) == 1 and ring(a)[na:] == ["-s active-start", "-u active-start"], 2),
          "the better Echo starts Assist and the losing Echo clears its ring")
    check(await until(lambda: b.chunks, 1) and not a.starts and not a.chunks and ring(b)[nb:] == ["-s active-start"],
          "only the winner sends audio, keeping its first light without restarting the animation")
    b.end_pipeline()
    check(await until(lambda: ring(b)[nb:] == ["-s active-start", "-u active-start"], 2),
          "the winner extinguishes the ring at the end of the request")

    # Claims from a wake word within the last second are still taken for the same round.
    await asyncio.sleep(1.1)
    # Switching the setting off restores the previous behaviour.
    a.page.set(arbitration_ring="off")
    na = len(ring(a))
    a.wake()
    await asyncio.sleep(0.25)
    check(len(ring(a)) == na and not a.starts, "with the option off, no ring or Assist starts during the wait")
    check(await until(lambda: len(a.starts) == 1 and ring(a)[na:] == ["-s active-start"], 2),
          "with the option off, normal listening is still shown after arbitration")
    a.end_pipeline()
    check(await until(lambda: ring(a)[na:] == ["-s active-start", "-u active-start"], 2),
          "the normal listening ring clears after the request")

    await asyncio.sleep(1.1)
    a.page.set(arbitration_ring="on")
    na = len(ring(a))
    a.wake()
    check(await until(lambda: len(a.starts) == 2 and ring(a)[na:] == ["-s active-start", "-s active-start"], 2),
          "when the preview animation has already ended, the winner starts the normal listening ring")
    a.end_pipeline()
    check(await until(lambda: ring(a)[-1:] == ["-u active-start"], 2), "the new listening ring clears")

    await asyncio.sleep(1.1)
    na = len(ring(a))
    a.wake()
    check(await until(lambda: ring(a)[na:] == ["-s active-start"], 0.4), "the early ring is on before muting")
    a.page.set(mute="on")
    check(await until(lambda: ring(a)[na:] == ["-s active-start", "-u active-start"], 0.3),
          "muting clears the early ring without waiting for the arbitration window")
    await asyncio.sleep(1.4)
    check(len(a.starts) == 2, "muted during selection: no new Assist run")


run(main, (a, b))
