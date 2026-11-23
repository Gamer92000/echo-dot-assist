# Echo 1 / misto on EchoRoot

> **Vibe-coded:** this experimental port was developed with AI assistance and
> requires independent review. Hardware testing so far covers one Echo 1.

This branch adapts echo-dot-assist to the first-generation Amazon Echo (`misto`)
running EchoRoot. It uses the existing `audioproxyd`, `audioCtrl`, `buttond` and
`ledd`; it does not install EchoRoot, unlock the device or redistribute Amazon
firmware, libraries, wake-word models or sounds.

## Status

On one Echo 1, the author observed ESPHome/Home Assistant, local wake word,
the action button, voice replies, Sendspin, Drop In, web access, Wi-Fi scanning
and switching, and the native volume wheel. Drop In queue pacing reduced the
perceived delay; this does not measure end-to-end or DAC latency. A failed Wi-Fi
switch to a nonexistent network returned to the previous connection; one
successful switch was confirmed. Wi-Fi persistence after reboot was not tested.

BlueZ 4.101 exposes A2DP source/sink and AVRCP SDP records. The HA pairing
switch changes BlueZ's pairable/discoverable settings while leaving the radio
powered. Pairing a phone, reconnecting a bonded phone, and audio profiles still
need end-to-end testing. **BLE proxy support is not established.** Pairing remains
open until the switch is turned off. The ring supports a limited set of effects;
the stock volume animation remains under `buttond`.

## Build

The `misto` make target requires a Linux ARM musl cross compiler and static
Opus and zlib installations, including their headers. With a suitable compiler
on PATH and `MISTO_DEPS` pointing to their common prefix:

```sh
make DEVICE=misto misto MISTO_CC=arm-linux-musleabi-gcc MISTO_DEPS=/path/to/deps
```

The executable is `build/misto/hassmic`. This repository does not include a
prebuilt Echo 1 release. Static binary distribution requires a review of the
third-party licence and relinking requirements, including freeaptx (LGPL).

## EchoRoot integration

`echoroot/` contains the service and helper scripts tested in the existing
`/home/root/echo1-assist` installation. The launcher expects the binary, the
configured microWakeWord files, and the scripts next to one another. It enables
the ESPHome API, Sendspin, Drop In and web ports. The Wi-Fi helper retains the
previous profile and attempts recovery if a new connection or DHCP fails;
switching can interrupt access and the device may obtain a different address.

These files are an **experimental reference integration**, not a safe general
installer: the existing installation, local state and service configuration
must be backed up before adaptation. Do not overwrite a working `run-local.sh`
or settings with a generic copy. The old archive installer is not part of this
branch. The `echoroot/tests/` tests use mocks, not real hardware.

The source snapshot used for the initial port was based on upstream commit
`28ff81c812e3f86911ec5ce30767fb4b4494f4df`. Review this branch against the
current upstream before proposing it for merging.
