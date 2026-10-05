# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Turns Amazon Echos into Home Assistant voice satellites. Supported so far: Echo Dot 3 (2018, `donut`, Fire OS 6574.1 /
`donut_puffin` NS65741 only); the repo is laid out for more models (`devices/`, see "Models" below). Amazon's audio front end (`mixer` daemon + `libasp`: AEC, beamforming, mic calibration) and wake word engine
(`libpryon.so`) stay; the Alexa client `PuffinApp` is replaced by `hassmic`, a C daemon that is a client of the mixer
through the reverse-engineered C API of `libmixerAPI.so` and speaks the ESPHome native API (default) or Wyoming.

Docs: `README.md` (user-facing usage; install instructions per model in `devices/<codename>/README.md`, guided by
`scripts/setup.sh`), `DEVELOPMENT.md` (architecture, layout, tests, contributing), `PLAN.md` (phases, open issues, every measurement), `CHANGELOG.md`
(user-visible changes by date), `docs/` (reverse-engineering findings: `FINDINGS.md`, `re-platform.md`, `re-pryon.md`, `re-aed.md`, `re-whisper.md`, `re-a2dp-source.md`,
`sendspin-digest.md`).

## Build and test

Device binaries are cross-built with Android NDK r21e (donut: armv7, bionic, API 24) and **link against Amazon's stock
libraries extracted to `firmware/<codename>/rootfs/system/lib`**. Neither the NDK (`toolchain/`) nor the firmware (`firmware/`) is in git;
without them only the host targets build. Extraction steps: `devices/donut/README.md` "2. Unpack the firmware and build".

```sh
make [DEVICE=donut]      # ARM binaries into build/donut/ (hassmic, mixcap, mixplay, pryon_test, aed_test, runas, latency, otatool)
make DEVICE=donut STUBS=1  # same, without firmware/: links against stand-ins from devices/<codename>/stubs/*.syms (byte-identical)
make host                # build/hassmic-host (PC: file audio + no wake word, SIGUSR1 triggers wake; board.c of DEVICE),
                         # build/donut/hassmic-qemu (ARM + real Pryon, run via tools/qrun.sh), build/otatool-host (the Echo's
                         # otatool for the PC, as tests' stand-in for the device side). Needs libopus
make unit                # C unit tests; ws/noise are checked against Python reference impls in .venv
make build/hassmic-host  # single target
```

Protocol/integration tests run `build/hassmic-host` against the real reference libraries (Python in `.venv`:
aioesphomeapi, wyoming, aiosendspin, noiseprotocol, aiohttp):

```sh
.venv/bin/python tests/fake_ha_esphome.py     # ESPHome native API, as Home Assistant
.venv/bin/python tests/fake_ha_arbitration.py # two Echos under one fake HA: wake word arbitration, join paths and security
.venv/bin/python tests/fake_ha.py [--qemu]    # Wyoming; --qemu uses the ARM build + stock Pryon model under qemu-arm
.venv/bin/python tests/fake_ma_sendspin.py    # Sendspin, as Music Assistant
.venv/bin/python tests/fake_web.py            # settings page: login by button, signatures, export/import, HA in step
.venv/bin/python tests/fake_ha_update.py      # online updates: page channel + HA update entity, fake GitHub, root's installer
tests/ota_push_test.sh                        # signed push-update path end to end
tests/otatool_test.sh                         # scripts/otatool.py against the C otatool: same keys, signatures, bundles
```

`.venv` from `tests/requirements.txt`. CI (`.github/workflows/build.yml`, DEVELOPMENT.md "CI and releases"): every model with
`STUBS=1`, PC tests, then a push to `main` publishes `v<version>-beta` (prerelease), a push to `release` publishes
`v<version>`; version = commit time in UTC, `2026.10.02.091530` (`make version`; local builds add `+<commit id>`, CI
passes `RELEASE=1` for the bare one), bundles `hassmic-<codename>.bundle(.sig)`
signed in the `release` job only (secret `RELEASE_SIGNING_KEY` of the GitHub environment `release`, main/release only;
build jobs sign with a throwaway key) with the release key (base64 of `secrets/release.key`; public half `keys/release.pub`,
shipped in every bundle). `tools/mkstubs.sh` regenerates the stub lists from the firmware and checks byte-identity; rerun it
when code starts using another stock library function. Builds are reproducible (`BUILD_TIME` = commit time). Test the
workflow with `act push` (publishing is a dry run under act).

There is no single-test selector: run one unit test by building/running its line from the `unit` target in the Makefile.
`tools/qrun.sh [-t secs] <arm-binary> args` runs a device binary on the PC under qemu-arm against `firmware/$DEVICE/rootfs`
(needs a new PID namespace; bionic mutexes deadlock with pid > 65535).

## Device workflow

- adb shell is root, without authentication. Over Wi-Fi it is closed on an installed Echo (`lockdown.sh` `adb_gate`):
  opened for 30 min from the settings page (an approved browser plus a press of the action button; hassmic writes `state/adb-request`, the firewall
  watcher opens it and marks it with `/data/local/hassmic/adb-open`), by `scripts/adb-wifi.sh [host]` (signs a challenge
  on the push port with `secrets/update.key`; no HA needed), or kept open by `ADB_WIFI=1` in `hassmic.conf`. Then
  `adb connect <echo-ip>:5555`. USB always works.
- `scripts/deploy.sh`: build + push to `/data/local/hassmic` for trial runs (`adb shell sh /data/local/hassmic/run.sh`).
- `scripts/ota-push.sh [host]`: build, sign with `secrets/update.key`, push bundle to TCP 28929 on an installed Echo.
  The Echo verifies against the public key on its system partition and falls back if the new build does not stay up.
  Every update (pushed or online) is promoted without approval as soon as it passes its self test (`main.c`: started,
  wake word engine loaded, ports bound, 1 s of mic audio within 30 s -> `ota_healthy()` -> `state/ota/healthy`):
  `main.sh` (`factory()`, only if that version is installed and its hassmic is what runs) has `sysinstall.sh` write it to
  `/system/hassmic` as the factory copy, bootstrap included (`boot.sh`, `otatool`, `hassmic.rc`, `release.pub`; never
  `update.pub` or `/sepolicy`). Works from older installs: the pushed update brings the code.
- `scripts/install-system.sh`: writes `/system/hassmic/`, init rc and patched SELinux policy from the running OS
  (`sysinstall.sh install`; `otatool remount rw` since toybox cannot: `/dev/root` does not exist). `--twrp` the old way.
  `--uninstall` reverts.
- Logs: `/data/local/hassmic/boot.log`. Config: `/data/local/hassmic/hassmic.conf` (`NAME`, `PROTO`, `ARGS`, `MODE`,
  `ADB_WIFI`; root-owned, 644: root sources it).
  State (API key, BLE bonds, BT keys, Sendspin, settings): `/data/local/hassmic/state/`.
- `kill -TTIN $(pidof hassmic)` toggles recording of the processed mic stream to `state/capture.raw`. `mixcap` cannot
  capture while hassmic runs: the mixer feeds the mic stream to one client only.
- `scripts/mic-compare.sh [-l] [secs]` records micRaw beside that dump and prints speech against noise for both
  (`tools/mic-compare.py`): what the front end does to a sentence. `-l` sets listening mode for the recording.
- `scripts/artifacts.sh [echo-ip]` (logic in `scripts/lib/artifacts.sh`, also the last step of `setup.sh`): Amazon's DAVS
  artifacts, picked in a menu with a checklist per kind (new ones ticked): wake words, installed from `device-logs/models/`
  after loading each with the Echo's `pryon_test`, and other artifacts (the sound detection model), kept on the PC for tests.
  Downloads come from Amazon in one go (stock-online + Alexa app registration, undone afterwards).
- `scripts/probe.sh` checks the device's libraries match the analysed firmware. Everything assumes exactly that version.
- PC scripts that use adb (`deploy`, `probe`, `install-system`, `capture-test`, `mic-compare`) detect the model from `ro.product.device`
  via `scripts/lib/device.sh`; `ota-push.sh` takes `DEVICE` (default donut). With two Echos on adb set `ANDROID_SERIAL`.

## Models

`devices/<codename>/` holds everything model-specific; porting guide in `devices/README.md`:
- `device.mk`: NDK target, audio/wake backends, stock libs to link (included by the Makefile).
- `board.c`: `struct board` (`src/hassmic/board.h`): HA identity, keypad/mute-latch/BT device paths, stock wake word
  and earcon paths, thermal zone, LED volume steps. Linked into every hassmic build, host builds included.
- `device.conf`: shell vars (product/firmware id, probe files, `INSTALL` method, daemon user/groups, stock service
  names). Sourced by PC scripts and shipped to the Echo next to `main.sh`/`lockdown.sh`/`alexa-off.sh`; push bundles
  carry it, and `main.sh` refuses a bundle whose `PRODUCT` is not the Echo's.
- `hassmic.rc`, `sepolicy.rules`: installed by `install-system.sh`.
- `stubs/*.syms`: what our binaries need of each stock library (`tools/mkstubs.sh`), for `make STUBS=1` / CI.
- `README.md` (model facts + install steps by hand) and `setup.sh` (same steps for the guided `scripts/setup.sh`:
  `STEPS` list + `step_<id>` functions using `scripts/lib/setup.sh`; `--dry-run` walks them without running). Keep
  the two in step.
No model `#ifdef`s in shared code: new differences become a board field, a `device.conf` variable or a backend.

## Architecture

`src/hassmic/` is one daemon; `main.c` is the satellite core, the rest plug into it through narrow headers:

- **Core (`main.c`, `core.h`)**: state machine `IDLE/LISTENING/THINKING/SPEAKING`, pipeline timeout, TTS queue with
  barge-in flush, alarms (timers), mute (hardware latch that software can set but never clear, plus a soft mute from HA),
  volume, LED ring, earcons, wake word threshold hints. The front end is told when a command is spoken (`listening()`:
  without it its cancellers remove the talker after 1.5 s). `micdenoise.c` (RNNoise, settings page off/low/medium/high, off by default) then
  `micgain.c`: AGC on the mic audio sent to the pipeline (the stock
  micAsr level is ~30 dB below what STT expects, and HA ignores the ESPHome audio settings); the wake word gets it raw. `core_lock` guards state and client socket writes; `core.h`
  documents per function whether the lock is held.
- **Protocols (`struct proto` in `core.h`)**: `proto_esphome.c` (ESPHome native API incl. Noise encryption provisioned
  by HA, voice assistant, media player, timers, settings entities, Bluetooth proxy messages) and `proto_wyoming.c` +
  `wyoming.c`. Selected with `-P`. The core calls `start/audio/stop/played/...` on the active proto with lock held.
- **Audio backend (`audio.h`)**: `audio_mixer.c` on device (mixer C API: capture, voice/TTS, music, Bluetooth and
  earcon streams mixed by the mixer); `audio_file.c` for PC builds. Swapped at link time in the Makefile.
- **Sound detection (`sound.h`)**: `sound_pryon.c` (Alexa Guard's model on `libpryon.so`: the firmware's, or Amazon's newest
  from `/data/local/hassmic/aed` (`scripts/artifacts.sh`); a second decoder, off unless HA's
  switch is on; ESPHome event entity "Sound"; windows with own playback dropped; `docs/re-aed.md`) or `sound_none.c`
  (host build, `HASSMIC_FAKE_SOUND`). Picked in the Makefile from the wake word backend.
- **Whisper detection (`whisper.h`)**: `whisper_pryon.c` (libpryon `WhisperApi_*`, one detector per request fed from the
  start of streaming, end of utterance on HA's VAD end (`core_mic_off`), result into the ESPHome binary sensor "Last
  request whispered" before STT ends, for the agent's prompt template; `docs/re-whisper.md`) or `whisper_none.c` (host
  build, `HASSMIC_FAKE_WHISPER`). Model from DAVS only, installed by `scripts/artifacts.sh` into
  `/data/local/hassmic/whisper` (not `models/`: it carries a stray `pryon.manifest`); without it, no sensor.
- **Wake word (`wake.h`)**: `wake_pryon.c` (stock `libpryon.so`, headers in `src/include/pryon_api.h`) or `wake_none.c`
  (host build). Also link-time swap.
- **Wake word arbitration (`arb.c`, `arb.h`)**: when several Echos hear the wake word, only the best one answers
  (stock's ESP, done on the LAN; the score is the front end's own wake word energy ratio, as stock reads it). UDP broadcast on 28930, shared network key; a member hands it to a newcomer only
  through Home Assistant (sealed to its X25519 key): on the member's diagnostic text sensor "Arbitration handoff", once
  HA shows the newcomer's key on the entity id it broadcasts (an Echo finds its own id by asking HA for
  `sensor.<node>_arbitration_handoff`, `_2`, `_3`, further while the last one is another device's, up to the subnet's host count; state
  requests need no permission), else as the newcomer's own
  ESPHome action `esphome.<node>_arbitration_key` (needs "Allow the device to perform Home Assistant actions"). Without
  HA: Volume up + Volume down held 2 s on both (`T_PAIR`/`T_GIVE`, exactly one requester). ESPHome only.
- **Music**: `sendspin.c` (Music Assistant Sendspin player over `ws.c`/`noise.c`/`net.c`/`hash.c`, decodes via
  `dr_flac`/`minimp3`/libopus). `a2dp.c` + `a2dp_codecs.c` + `sbc.c` = Bluetooth A2DP sink (SBC, AAC via firmware FFmpeg
  loaded with dlopen, aptX/aptX HD via `freeaptx`) with AVRCP. Only one music source plays at a time (newest wins).
  The other way, playing on a Bluetooth speaker: `a2dp.c` (inquiry, pairing, AVDTP initiator, AVRCP absolute volume)
  + `btout.c`, which stands in for btmanagerd towards the mixer's own A2DP route (LIPC `A2DPSourceConnect`, the A2DP
  HAL's abstract sockets, AIPC service uuid 0 via `libace_aipc.so`; `docs/re-a2dp-source.md`) and SBC-encodes
  (`sbc.c`). While on the speaker the core has a volume of its own (`core_speaker`).
- **Bluetooth**: `ble.c`/`ble_crypto.c` talk raw HCI (`hci.h`) to the controller for the HA Bluetooth proxy (scan, GATT,
  Just Works pairing); Amazon's `btmanagerd` is stopped. A2DP shares the controller; scanning pauses while a phone plays.
- **Settings** (`settings.c`, `settings.h`): one table of the satellite's settings by name (type, range, group,
  exportable or not), used by `proto_esphome.c`, the settings page and exports. Its own ones persist in `state/config`
  (`name=value`; the positional `state/settings` of older versions is read once and moved); arbitration, Sendspin and
  the equalizer stay where their module keeps them. Loaded in `main()` whatever the protocol. Changes from elsewhere
  reach HA through `proto->settings_changed`. Features (`feature` in the table: arbitration, sound, whisper, Wi-Fi
  motion, Bluetooth audio, Bluetooth speaker): their entities are listed only while on (`proto_esphome.c` `listed()`,
  which also gates states); switching one closes the HA links (`proto->entities_changed`), HA re-lists on reconnect and
  deletes what is gone (registry included). HA always has: media player, mute, DND, wake sound, LEDs, EQ, firmware,
  Sendspin token (a secret: never on the page). The rest is page-only; diagnostics too (`diag.c`).
- **Settings page** (`web.c`, `web/`): HTTP on 28931 (`-W`), files of `web/` gzip'd into the binary by `tools/embed.py`
  (`build/web_assets.c`). Login: the browser's X25519 key waits for the action button (`web_approve()` first in
  `on_action`; ring `authenticated_setup_mode`), approved keys in `state/web_clients`, Echo key `state/web_key`.
  Requests signed (BLAKE2b-128 keyed with K over method, path, counter, body; K from X25519), counter per browser.
  Never send secrets: it is plain HTTP. `web/crypto.js` (X25519, BLAKE2b; no `crypto.subtle` on plain HTTP) is checked
  against Python by `tests/unit/web_crypto_test.py` (in `make unit`, needs node).
- **adb over Wi-Fi** (`adbwifi.c`): the settings page only writes a request for root's firewall watcher, as `ota.c` does
  for updates; opening needs an approved browser plus a press of the action button (`web.c`), or (`ota.c`, `HMOTA-ADB1`) a challenge signed with the update key.
- **Wi-Fi motion** (`wifimotion.c`, experimental, off by default): polls the RCPI of the frames from the AP at 10 Hz, scatter
  over 2 s = motion binary sensor. A kernel module of ours (no kprobes on any model) words it like MediaTek's `RX_STAT` in
  `/proc/<module>`: biscuit/radar (gen2 driver, built in, no frame levels) `src/kmod/hassmic_rcpi.c` inline-hooks
  `nicRxProcessDataPacket`; donut (gen4m `wlan_mt76x8_sdio.ko`, arm64 kernel) `src/kmod/hassmic_rcpi4m.c` turns its
  `bl nicRxFillRFB` into a call to a wrapper. Each reading also has a `KIND` (donut: rate from the RX vector; gen2:
  broadcast or not), and `wm_kind_norm` compares it with its kind's own level: APs send each rate at its own power. device.conf `KMOD`, loaded by `main.sh` only once the feature is on
  (`wifi_motion=on` in `state/config`) and the link is up, never unloaded. Built by `make` per device.mk `KVER`/`KARCH`/`KCROSS` against
  `toolchain/linux-<KVER>` + `devices/<codename>/kconfig` (biscuit/radar: IKCONFIG of `boot.img`, `KCONFIG`; donut: a
  fragment on arm64 defconfig, `KFRAG`), with AOSP
  `arm-eabi-4.8` / `aarch64-linux-android-4.9`. Without the module donut falls back to `iwpriv wlan0 driver RX_STAT` (last
  frame from anyone). The MT7668's CSI commands are accepted but the chip firmware sends no data.
- **Push updates**: `ota.c` receives bundles on the device; `src/tools/otatool.c` verifies/unpacks there (monocypher).
  The PC side is `scripts/otatool.py` (stdlib only: keygen, pack/sign, verify/install, push, adb; same CLI), so the PC
  needs no compiler for it; `tests/otatool_test.sh` holds the two byte for byte to each other, and CI verifies every
  bundle with both. `scripts/bundle.sh` packs (ota-push.sh and CI).
- **Prebuilt** (`scripts/lib/build.sh`, `build_binaries`): deploy/install-system/ota-push/setup take the device binaries
  from the release CI published of HEAD (tag `v<commit time>[-beta]` pointing at HEAD, clean tree; bundle verified
  against `keys/release.pub`, marked `build/<codename>/PREBUILT`) where there is no NDK, or with `PREBUILT=1`;
  `PREBUILT=0` always builds. `scripts/probe.sh` compares with `devices/<codename>/probe.md5`, not the unpacked firmware.
- **Online updates** (`update.c`): "Online updates" on the settings page (off default / beta / release, `state/config`) and an
  ESPHome update entity; checks GitHub's releases API, downloads `hassmic-<board.codename>.bundle` through the firmware's
  libcurl (dlopen; sockets via `net_socket` so the egress lock lets them out), hands it to root through `ota_handoff`.
  Root (`main.sh` ota_watch) accepts the owner's `update.pub` or the release key (`release.pub` of the running copy, else
  the system partition). Channel change and install only over the keyed connection.
- `src/include/`: headers for the reversed Amazon libraries (`mixer_api.h`, `pryon_api.h`, `aipc_api.h`) and `netio.h`.
- `src/tools/`: standalone device tools (`mixcap`, `mixplay`, `pryon_test`, `aed_test`, `latency`, `runas` — AIPC refuses uid 0 and
  the image has no `su`; `curlspy`, `hciscan`, `a2dpprobe` not in `all`).

Boot integration (`scripts/system/`, rc in `devices/<codename>/`): `hassmic.rc` (init) starts `boot.sh` (fixed, on /system), which picks the factory
copy or a verified update and runs `main.sh` (updatable): `main.sh firewall` (egress lock re-asserted in a loop, root side
of push updates) and `main.sh satellite` (stops Alexa/updater/telemetry, keeps hassmic running, and every 10 s runs
`lockdown.sh check`, as the firewall service does every 5 s: every rule of `hassmic_out`, the chain first in OUTPUT, INPUT
policy DROP, and the stock rules the satellite needs (`keep` in `lockdown.sh`; stock `firewall.sh` can lose any of its
rules at boot); wrong twice in a row, it loads the rules itself and restarts that service; loading is one
`iptables-restore -w --noflush` call, rule by rule only as fallback). No `hassmic.conf` =
stock behaviour. `scripts/device/` holds on-device helpers (`lockdown.sh` firewall, `alexa-off/on.sh`, `wifi-join.sh`).

Firewall invariant: Amazon's daemons may only reach local addresses; hassmic itself may reach any address (it fetches
TTS/media URLs from HA/MA). `otad`/`ace_otad` (firmware updates) must never get out. Inbound TCP and UDP are only admitted on
16384–32767, so every listening port (26053 ESPHome, 16700 Wyoming, 28928 Sendspin, 28929 OTA, 28931 settings page, UDP 28930 arbitration)
must stay in that range. A stock rule hassmic comes to depend on (INPUT or OUTPUT) goes into `keep`, worded as `iptables -S`
prints it.

`tools/` is PC-side reverse-engineering and firmware tooling (`payload_dump.py`, Thumb disassembly helpers,
`davs-fetch.py` for extra wake word models).

## Conventions

- Feature commits update `CHANGELOG.md` (user-facing, dated, plain language), `PLAN.md` (status, measurements) and the
  README when behaviour visible to users changes.
- Code comments explain *why* with device facts and measurements; match that style and density.
- Proprietary/derived/secret material is git-ignored and must stay out of git: `firmware/` (per model:
  stock image, unpacked rootfs, unlock zips, `re/` disassembly), `toolchain/`, `build/`, `device-logs/`, `secrets/`, `*.bin`, `*.zip`.
- Third-party code in `src/third_party/` keeps its own licence (freeaptx is LGPL, statically linked).
