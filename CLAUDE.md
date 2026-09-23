# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Turns an Amazon Echo Dot 3 (2018, `donut`, Fire OS 6574.1 / `donut_puffin` NS65741 only) into a Home Assistant voice
satellite. Amazon's audio front end (`mixer` daemon + `libasp`: AEC, beamforming, mic calibration) and wake word engine
(`libpryon.so`) stay; the Alexa client `PuffinApp` is replaced by `hassmic`, a C daemon that is a client of the mixer
through the reverse-engineered C API of `libmixerAPI.so` and speaks the ESPHome native API (default) or Wyoming.

Docs: `README.md` (user-facing install/usage), `DEVELOPMENT.md` (architecture, layout, tests, contributing), `PLAN.md` (phases, open issues, every measurement), `CHANGELOG.md`
(user-visible changes by date), `docs/` (reverse-engineering findings: `FINDINGS.md`, `re-platform.md`, `re-pryon.md`,
`sendspin-digest.md`).

## Build and test

Device binaries are cross-built with Android NDK r21e (armv7, bionic, API 24) and **link against Amazon's stock libraries
extracted to `firmware/rootfs/system/lib`**. Neither the NDK (`toolchain/`) nor the firmware (`firmware/`) is in git;
without them only the host targets build. Extraction steps are in README "2. Unpack the firmware and build".

```sh
make                     # ARM binaries into build/ (hassmic, mixcap, mixplay, pryon_test, runas, latency, otatool)
make host                # build/hassmic-host (PC: file audio + no wake word, SIGUSR1 triggers wake),
                         # build/hassmic-qemu (ARM + real Pryon, run via tools/qrun.sh), build/otatool-host. Needs libopus
make unit                # C unit tests; ws/noise are checked against Python reference impls in .venv
make build/hassmic-host  # single target
```

Protocol/integration tests run `build/hassmic-host` against the real reference libraries (Python in `.venv`:
aioesphomeapi, wyoming, aiosendspin, noiseprotocol, aiohttp):

```sh
.venv/bin/python tests/fake_ha_esphome.py     # ESPHome native API, as Home Assistant
.venv/bin/python tests/fake_ha.py [--qemu]    # Wyoming; --qemu uses the ARM build + stock Pryon model under qemu-arm
.venv/bin/python tests/fake_ma_sendspin.py    # Sendspin, as Music Assistant
tests/ota_push_test.sh                        # signed push-update path end to end
```

There is no single-test selector: run one unit test by building/running its line from the `unit` target in the Makefile.
`tools/qrun.sh [-t secs] <arm-binary> args` runs a device binary on the PC under qemu-arm against `firmware/rootfs`
(needs a new PID namespace; bionic mutexes deadlock with pid > 65535).

## Device workflow

- adb over Wi-Fi works (`adb connect <echo-ip>:5555`); adb shell is root. USB only needed for TWRP.
- `scripts/deploy.sh`: build + push to `/data/local/hassmic` for trial runs (`adb shell sh /data/local/hassmic/run.sh`).
- `scripts/ota-push.sh [host]`: build, sign with `secrets/update.key`, push bundle to TCP 28929 on an installed Echo.
  The Echo verifies against the public key on its system partition and falls back if the new build does not stay up.
- `scripts/install-system.sh`: writes `/system/hassmic/`, init rc and patched SELinux policy via TWRP. `--uninstall` reverts.
- Logs: `/data/local/hassmic/boot.log`. Config: `/data/local/hassmic/hassmic.conf` (`NAME`, `PROTO`, `ARGS`, `MODE`).
  State (API key, BLE bonds, BT keys, Sendspin, settings): `/data/local/hassmic/state/`.
- `kill -TTIN $(pidof hassmic)` toggles recording of the processed mic stream to `state/capture.raw`. `mixcap` cannot
  capture while hassmic runs: the mixer feeds the mic stream to one client only.
- `scripts/probe.sh` checks the device's libraries match the analysed firmware. Everything assumes exactly that version.

## Architecture

`src/hassmic/` is one daemon; `main.c` is the satellite core, the rest plug into it through narrow headers:

- **Core (`main.c`, `core.h`)**: state machine `IDLE/LISTENING/THINKING/SPEAKING`, pipeline timeout, TTS queue with
  barge-in flush, alarms (timers), mute (hardware latch that software can set but never clear, plus a soft mute from HA),
  volume, LED ring, earcons, wake word threshold hints. `core_lock` guards state and client socket writes; `core.h`
  documents per function whether the lock is held.
- **Protocols (`struct proto` in `core.h`)**: `proto_esphome.c` (ESPHome native API incl. Noise encryption provisioned
  by HA, voice assistant, media player, timers, settings entities, Bluetooth proxy messages) and `proto_wyoming.c` +
  `wyoming.c`. Selected with `-P`. The core calls `start/audio/stop/played/...` on the active proto with lock held.
- **Audio backend (`audio.h`)**: `audio_mixer.c` on device (mixer C API: capture, voice/TTS, music, Bluetooth and
  earcon streams mixed by the mixer); `audio_file.c` for PC builds. Swapped at link time in the Makefile.
- **Wake word (`wake.h`)**: `wake_pryon.c` (stock `libpryon.so`, headers in `src/include/pryon_api.h`) or `wake_none.c`
  (host build). Also link-time swap.
- **Music**: `sendspin.c` (Music Assistant Sendspin player over `ws.c`/`noise.c`/`net.c`/`hash.c`, decodes via
  `dr_flac`/`minimp3`/libopus). `a2dp.c` + `a2dp_codecs.c` + `sbc.c` = Bluetooth A2DP sink (SBC, AAC via firmware FFmpeg
  loaded with dlopen, aptX/aptX HD via `freeaptx`) with AVRCP. Only one music source plays at a time (newest wins).
- **Bluetooth**: `ble.c`/`ble_crypto.c` talk raw HCI (`hci.h`) to the controller for the HA Bluetooth proxy (scan, GATT,
  Just Works pairing); Amazon's `btmanagerd` is stopped. A2DP shares the controller; scanning pauses while a phone plays.
- **Push updates**: `ota.c` receives bundles on the device; `src/tools/otatool.c` is the same code for pack/sign (PC)
  and verify/unpack (device), with monocypher.
- `src/include/`: headers for the reversed Amazon libraries (`mixer_api.h`, `pryon_api.h`) and `netio.h`.
- `src/tools/`: standalone device tools (`mixcap`, `mixplay`, `pryon_test`, `latency`, `runas` — AIPC refuses uid 0 and
  the image has no `su`; `curlspy`, `hciscan` not in `all`).

Boot integration (`scripts/system/`): `hassmic.rc` (init) starts `boot.sh` (fixed, on /system), which picks the factory
copy or a verified update and runs `main.sh` (updatable): `main.sh firewall` (egress lock re-asserted in a loop, root side
of push updates) and `main.sh satellite` (stops Alexa/updater/telemetry, keeps hassmic running). No `hassmic.conf` =
stock behaviour. `scripts/device/` holds on-device helpers (`lockdown.sh` firewall, `alexa-off/on.sh`, `wifi-join.sh`).

Firewall invariant: Amazon's daemons may only reach local addresses; hassmic itself may reach any address (it fetches
TTS/media URLs from HA/MA). `otad`/`ace_otad` (firmware updates) must never get out. Inbound TCP is only admitted on
16384–32767, so every listening port (26053 ESPHome, 16700 Wyoming, 28928 Sendspin, 28929 OTA) must stay in that range.

`tools/` is PC-side reverse-engineering and firmware tooling (`payload_dump.py`, Thumb disassembly helpers,
`davs-fetch.py` for extra wake word models).

## Conventions

- Feature commits update `CHANGELOG.md` (user-facing, dated, plain language), `PLAN.md` (status, measurements) and the
  README when behaviour visible to users changes.
- Code comments explain *why* with device facts and measurements; match that style and density.
- Proprietary/derived/secret material is git-ignored and must stay out of git: `firmware/`, `re/`, `kamakiri/`,
  `boot-root/`, `toolchain/`, `build/`, `device-logs/`, `secrets/`, `*.bin`, `*.zip`.
- Third-party code in `src/third_party/` keeps its own licence (freeaptx is LGPL, statically linked).
