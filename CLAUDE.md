# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Turns Amazon Echos into Home Assistant voice satellites. Supported so far: Echo Dot 3 (2018, `donut`, Fire OS 6574.1 /
`donut_puffin` NS65741 only); the repo is laid out for more models (`devices/`, see "Models" below). Amazon's audio front end (`mixer` daemon + `libasp`: AEC, beamforming, mic calibration) and wake word engine
(`libpryon.so`) stay; the Alexa client `PuffinApp` is replaced by `hassmic`, a C daemon that is a client of the mixer
through the reverse-engineered C API of `libmixerAPI.so` and speaks the ESPHome native API (default) or Wyoming.

Docs: `README.md` (short user-facing overview with figures; every feature in detail in `docs/GUIDE.md`, security model in
`docs/SECURITY.md`; figures are CeTZ sources in `docs/img/src/`, rendered light+dark by `tools/figures.sh`; install instructions per model in `devices/<codename>/README.md`, guided by
`scripts/setup.sh`), `DEVELOPMENT.md` (architecture, layout, tests, contributing), `PLAN.md` (phases, open issues, every measurement), `CHANGELOG.md`
(user-visible changes by date), `docs/` (reverse-engineering findings: `FINDINGS.md`, `re-platform.md`, `re-pryon.md`, `re-aed.md`, `re-whisper.md`, `re-a2dp-source.md`, `re-davs-login.md`,
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
.venv/bin/python tests/fake_ha_dropin.py      # Drop In between two Echos: call, answer, hang-up paths, refusals, lost peer
.venv/bin/python tests/fake_ha.py [--qemu]    # Wyoming; --qemu uses the ARM build + stock Pryon model under qemu-arm
.venv/bin/python tests/fake_ma_sendspin.py    # Sendspin, as Music Assistant
.venv/bin/python tests/fake_web.py            # settings page: login by button, signatures, export/import, HA in step
.venv/bin/python tests/fake_web_artifacts.py  # models copied Echo to Echo through the page, root's installer, after a restart
.venv/bin/python tests/fake_web_wifi.py       # Wi-Fi switch: sealed password, PSK, root's wifi.sh against a fake wpa_cli
.venv/bin/python tests/fake_web_mww.py        # microWakeWord: models added/edited/deleted from the page, engine switch,
                                             # real detections on testdata/alexa_espeak.raw, copy as an artifact
.venv/bin/python tests/fake_web_davs.py       # models downloaded from a fake Amazon: login by code pair with the device
                                             # attestation token, DAVS, unpack, staging, install, deregister
.venv/bin/python tests/fake_ha_update.py      # online updates: page channel + HA update entity, fake GitHub, root's installer
.venv/bin/python tests/fake_improv.py         # setup from a phone: Improv over BLE against a fake controller on a pty
                                             # (HASSMIC_BT_DEV), root's wifi.sh, adoption, factory reset (HA, button hold)
tests/ota_push_test.sh                        # signed push-update path end to end
tests/otatool_test.sh                         # scripts/otatool.py against the C otatool: same keys, signatures, bundles
```

`.venv` from `uv sync` (`pyproject.toml` + `uv.lock`; change a version, then `uv lock`). CI (`.github/workflows/build.yml`, DEVELOPMENT.md "CI and releases"): every model with
`STUBS=1`, PC tests, then a push to `main` publishes `v<version>-beta` (prerelease), a push to `release` publishes
`v<version>` (a beta only when some model's `scripts/content-id.sh` differs from the newest release's: no betas for CI or doc
changes); version = commit time in UTC, `2026.10.02.091530` (`make version`; local builds add `+<commit id>`, CI
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
- Logs: `/data/local/hassmic/boot.log`, each line stamped (`clock.c`: UTC once the clock came from HA, else `boot+<s>`;
  hassmic's own through its reader process `hassmic-log`, which `pidof hassmic` lists too; scripts through `say`/`stamped`
  in `main.sh`). The Echo's mksh is 32-bit and has no awk. Config: `/data/local/hassmic/hassmic.conf` (`PROTO`, `ARGS`, `MODE`,
  `ADB_WIFI`; root-owned, 644: root sources it).
  State (API key, BLE bonds, BT keys, Sendspin, settings): `/data/local/hassmic/state/`.
- `kill -TTIN $(pidof hassmic)` toggles recording of the processed mic stream to `state/capture.raw`. `mixcap` cannot
  capture while hassmic runs: the mixer feeds the mic stream to one client only.
- `scripts/mic-compare.sh [-l] [secs]` records micRaw beside that dump and prints speech against noise for both
  (`tools/mic-compare.py`): what the front end does to a sentence. `-l` sets listening mode for the recording.
- `scripts/artifacts.sh [echo-ip]` (logic in `scripts/lib/artifacts.sh`; no longer a step of `setup.sh`, and it opens with a
  warning that the page's "Download from Amazon" is the way meant now; a fallback): Amazon's DAVS
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
  (host build, `HASSMIC_FAKE_SOUND`; `HASSMIC_FAKE_SOUND_MODEL` names the model, `broken` fails to load). `sound_model()`
  says which model it takes, for the page; a start that fails switches the setting off via `settings_set` and sets
  `core_sound_failed()` (page warning). Picked in the Makefile from the wake word backend.
- **Whisper detection (`whisper.h`)**: `whisper_pryon.c` (libpryon `WhisperApi_*`, one detector per request fed from the
  start of streaming, end of utterance on HA's VAD end (`core_mic_off`), result into the ESPHome binary sensor "Last
  request whispered" before STT ends, for the agent's prompt template; `docs/re-whisper.md`) or `whisper_none.c` (host
  build, `HASSMIC_FAKE_WHISPER`). Model from DAVS only, installed by `scripts/artifacts.sh` into
  `/data/local/hassmic/whisper` (not `models/`: it carries a stray `pryon.manifest`); without it, no sensor.
- **Wake word (`wake.h`)**: `wake_pryon.c` (stock `libpryon.so`, headers in `src/include/pryon_api.h`) or `wake_none.c`
  (host build) as the vendor engine, link-time swap; microWakeWord (below) on every build, picked at run time.
- **microWakeWord (`wake.c`, `wake_mww.c`, `mww_features.c`, `mww_model.c`, `mww_store.c`, `mww.h`)**: second wake word
  engine, off by default (setting `wake_engine` amazon/microwakeword, page section "Wake word"; detects much worse than
  Pryon, the page says what it costs).  `wake.c` dispatches by model: a `.json` manifest = microWakeWord, else the vendor
  engine (`wake_vendor`: `wake_pryon.c` / `wake_none.c`); `wake_fed()` is the sample clock across engines.
  `mww_features.c` = TFLite Micro's microfrontend, bit-exact with pymicro-features (tables via double functions:
  bionic's log1pf differs); `mww_model.c` = int8 TFLite interpreter for the 13 ops microWakeWord's streaming models use
  (resource variables, CALL_ONCE), TFLite reference arithmetic, logistic as float LUT; untrusted input (page uploads):
  every flatbuffer offset checked, quantization validated, accumulators bounded (MAX_DOT/MAX_BIAS); fuzzed with
  ASan/UBSan.  Models in `state/mww/<id>/{manifest.json,model.tflite}` (hassmic's own, no root), added/edited/deleted
  via `/api/mww/*` (web.c), copied as artifacts `mww:<id>` (artifacts.c commits them itself).  Wake word list
  (`main.c` `wake_words_scan`) is per engine and rescanned live (`core_wake_engine`, `core_wake_models_changed`; HA
  re-reads it after the link is closed); pick in `state/mww_word`.  No gain on the input (PCAN normalises; measured).
  Lost with it: "stop" keyword, Pryon's threshold hints, front end ESP scores (`wake_afe_times` 0: own SNR score).
  Third value `homeassistant` = `-w remote` from the page: decided at start (`main()` peeks `state/config`), so
  `core_wake_engine` saves and writes `state/restart` (root's `main.sh` restarts); `-w remote` in ARGS
  wins (`core_wake_refused`).  Remote: EV_WAKE_END -> `core_remote_wake` (sound, ring, `listening(1)` until mic off).
  `tests/unit/mww_ref.py` (in `make unit`) checks features and every inference against pymicro-features and TFLite
  BUILTIN_REF on all of pymicro-wakeword's models.
- **Wake word arbitration (`arb.c`, `arb.h`)**: when several Echos hear the wake word, only the best one answers
  (stock's ESP, done on the LAN; the score is the front end's own wake word energy ratio, as stock reads it). UDP broadcast on 28930 (beacons at random 20-40 s, a member counts for 5 min: broadcasts get lost; for 40 s after a start or join the beacons carry F_HELLO and every member answers at once; a newcomer asks every 1.2 s for its first 5 s), shared network key; a member hands it to a newcomer (sealed to its X25519 key) only
  when Home Assistant vouches for both: each Echo that may be waited for reports the tag `hassmic_<pub hex>` as scanned
  (event `esphome.tag_scanned`: HA fires device events without "perform actions"; `tag.<id>` has no area or device in
  it, unlike every entity id, which HA builds from area, parent device, device and entity name), the other reads
  `tag.hassmic_<pub hex>` (state requests need no permission either) and trusts the key once the scan time changed
  after its first read; then `T_GIVE` on the LAN, taken outside pairing only from a confirmed Echo. Else, after 30 s,
  the newcomer's own ESPHome action `esphome.<node>_arbitration_key` (needs "Allow the device to perform Home Assistant
  actions"). Without HA: Volume up + Volume down held 2 s on both (`T_PAIR`/`T_GIVE`, exactly one requester). ESPHome only.
  The network (beacons, key handoff, pairing; tags and action) always runs: the settings pages find each other
  through it. The `arbitration` setting (`arb_arbitrate`) only gates rounds; off, the beacon carries a flags byte after
  the counter and members leave that Echo out of rounds (older builds reject the longer beacon, same effect).
  `arbitration_mode` (`arb_mode`, in `state/config`): `hassmic` (default, the above) or `kiosk`: Kiosk Satellite's
  protocol reimplemented from `docs/kiosk-arbitration.md` (JSON claims on UDP 2330, three copies, loudness only from
  `main.c` `kiosk_score` + `arbitration_offset` dB, `arbitration_window` ±ms, no key, no priority, always waits); the beacon flags it (1 quiet +
  2 kiosk). The socket belongs to arb's loop thread. Page: two cards with pros and cons (`HELP.arbitration_mode.modes`).
- **Music**: `sendspin.c` (Music Assistant Sendspin player over `ws.c`/`noise.c`/`net.c`/`hash.c`, decodes via
  `dr_flac`/`minimp3`/libopus). `a2dp.c` + `a2dp_codecs.c` + `sbc.c` = Bluetooth A2DP sink (SBC, AAC via firmware FFmpeg
  loaded with dlopen, aptX/aptX HD via `freeaptx`) with AVRCP. Only one music source plays at a time (newest wins).
  The other way, playing on a Bluetooth speaker: `a2dp.c` (inquiry, pairing, AVDTP initiator, AVRCP absolute volume)
  + `btout.c`, which stands in for btmanagerd towards the mixer's own A2DP route (LIPC `A2DPSourceConnect`, the A2DP
  HAL's abstract sockets, AIPC service uuid 0 via `libace_aipc.so`; `docs/re-a2dp-source.md`) and SBC-encodes
  (`sbc.c`). While on the speaker the core has a volume of its own (`core_speaker`).
- **Drop In** (`dropin.c`, `dropin.h`): calls between two Echos of the arbitration network. Signalling as signed
  `T_MSG` on arb's socket (`arb_tell`, `hooks->message`; invite/ringing/accept/refuse/bye), audio Opus 16 kHz 20 ms over
  unicast UDP 28932 (`-i`), per-call key from ephemeral X25519 + `arb_derive`. Playback on the mixer's `Voip` stream
  (front end's call mode); mic = micAsr through the pipeline's gain, then a gate (open only while this side talks,
  echo estimate peak-held against the far end's level held after it stops), listening mode only while this side
  talks (`core_dropin_listen`). Core side in `main.c` (`core_dropin`: call LED animations, comms sounds, ringtones via
  `play_earcon_while`; action button answers/ends, "stop"/"hang up"/"auflegen" end; wake word ignored while the far end
  talks). Notifications to the core go through dropin's loop thread: `dropin_call` runs under `core_lock` (ESPHome
  handler). HA: action `drop_in`, "Drop In", "Drop In with", "End Drop In"; settings `drop_in`, `drop_in_answer`;
  page `POST /api/dropin`. Voice: `blueprints/automation/drop_in.yaml`.
- **Bluetooth**: `ble.c`/`ble_crypto.c` talk raw HCI (`hci.h`) to the controller for the HA Bluetooth proxy (scan, GATT,
  Just Works pairing); Amazon's `btmanagerd` is stopped. A2DP shares the controller; scanning pauses while a phone plays,
  and for 1 s from a wake word round (`ble_quiet` in `main.c` `wake_heard`): the scan's share of the antenna costs
  10-30 % of Wi-Fi broadcasts (unicast is retried), i.e. the other Echos' claims.
- **Settings** (`settings.c`, `settings.h`): one table of the satellite's settings by name (type, range, group,
  exportable or not), used by `proto_esphome.c`, the settings page and exports. Its own ones persist in `state/config`
  (`name=value`; the positional `state/settings` of older versions is read once and moved); arbitration, Sendspin and
  the equalizer stay where their module keeps them. Loaded in `main()` whatever the protocol. Changes from elsewhere
  reach HA through `proto->settings_changed`. Features (`feature` in the table: arbitration, sound, whisper, Wi-Fi
  motion, Bluetooth audio, Bluetooth speaker, Drop In): their entities are listed only while on (`proto_esphome.c` `listed()`,
  which also gates states); switching one closes the HA links (`proto->entities_changed`), HA re-lists on reconnect and
  deletes what is gone (registry included). HA always has: media player, mute, DND, wake sound, LEDs, EQ, firmware,
  Web UI address, Sendspin token (a secret: never on the page), Identify. The rest is page-only; diagnostics too (`diag.c`).
- **Settings page** (`web.c`, `web/`): HTTP on 28931 (`-W`), files of `web/` gzip'd into the binary by `tools/embed.py`
  (`build/web_assets.c`). Login: the browser's X25519 key waits for the action button (`web_approve()` first in
  `on_action`; ring `authenticated_setup_mode`), approved keys in `state/web_clients`, Echo key `state/web_key`.
  Requests signed (BLAKE2b-128 keyed with K over method, path, counter, body; K from X25519), counter per browser; the
  answers too (`respond_s`: X-HM-Mac over "RESP\n" + counter + body), and the page refuses an unsigned one ('old').
  Vouchers: members of one arbitration network let in a browser approved on another member (`/api/vouch/nonce`, `issue`
  (signed, at the Echo it is approved on), `login`; MAC with `arb_web_key`, a key derived from K, over target key,
  browser key, one-time nonce and the voucher's name). The page does it by itself for members; a login page opens
  the other Echo's page (`#vouch=`), which asks, checks the asker is a member with that key, and postMessages it back.
  adb still needs that Echo's own button.
  Never send secrets: it is plain HTTP.  Log viewer: `GET /api/log/0|1` (signed) sends `boot.log` / `.1` (`HASSMIC_LOG`,
  tail of 2 MB), with the Sendspin pairing token blanked (`log_redact`; `setup.sh` greps it from the log over adb). The page's words (what each setting does, what a feature adds to HA) live in
  `web/app.js` `HELP`, keyed by setting name: a new setting gets an entry there. Echos section from `arb_status_json`
  (members with name, IP and whether they arbitrate from their signed beacons, others: in no network / younger / older); other Echos' pages are called cross-origin (CORS `*`,
  the signature counts), each approved once with its own button; "make like this Echo" posts this one's export.
  Name: one, made at start and stored nowhere (`main.c` `name_make`): `board.default_name` (the model: "Echo Dot 3")
  plus the Wi-Fi MAC's last 3 bytes ("Echo Dot 3 5695c4", node `echo-dot-3-5695c4`), as ESPHome's name_add_mac_suffix;
  people name the Echo in Home Assistant.  No rename anywhere (page "Name" row only shows it); `-n` only for the PC
  (no MAC).  Old `state/name`/`state/node` are deleted at start, hassmic.conf `NAME` is ignored.  `state/restart`
  (its text says why: wake word engine), which root's `ota_watch` turns into `stop hassmic; start hassmic`.
  Identify (`POST /api/identify`, HA button `identify`, device class identify): `core_identify` sets `zzz_rainbow`
  (same file on every model) for 10 s, cleared by `volume_led_thread`, and queues `SND_IDENTIFY` (stock's
  `state_setup_discovery_beacon`) past the wake sound setting.
  Presets: `scripts/setup.sh --preset <export>` (or asked in `install_satellite`) pushes it to `state/preset`; hassmic
  applies it once at start (`settings_preset`, as an import) and renames it `preset.applied`. `web/crypto.js` (X25519, BLAKE2b; no `crypto.subtle` on plain HTTP) is checked
  against Python by `tests/unit/web_crypto_test.py` (in `make unit`, needs node). The Echos section copies chosen
  settings to chosen Echos (a matrix of exports), and models.
- **Amazon downloads** (`davs.c` + `dha.c`): the settings page's "Download from Amazon".  `dha.c` builds the device
  attestation token `/auth/register` needs, in the two shapes Amazon's MAPs make: a drvV1 JWT (biscuit, radar; RSA
  keymaster through `libacehal_dha.so`, dlopen'd, needs group drmrpc) or donut's drvV3 token around the dhav2
  certificate (field 0x203 of the same HAL; EC key in the TEE — `/dev/trustzone` is system:drmrpc and the cert file
  keystore:keystore, hence drmrpc,keystore in donut's DAEMON_GROUPS), whichever the HAL's key parses as.  `davs.c` does the code pair login (the code on the
  page, register polled with the token, tokens in `state/davs` 0600, never sent to the page), asks DAVS with the
  engine's own compatibility ids (`wake_attributes`), downloads and unpacks the artifact tar.gz (libz), and stages it
  through artifacts.c.  Works against the real Amazon (biscuit 2026-10-06, donut with its drvV3 token 2026-10-09,
  both amazon.de; what it answers in docs/re-davs-login.md, which also has the drvV3 analysis).  The page offers
  every Amazon site with Alexa (davs.c's list), any of the account's region works.
- **Artifacts** (`artifacts.c`): Amazon's models (`wake:<name>` = `models/<name>/`, `sound` = `aed/`, `whisper`): files
  plus two levels of folders (`whisper_components/`, `BDPGeneratedFiles/`, `nttfusionconfig/ntt_conv/`; a file's name is then its path), listed
  with a digest (BLAKE2b-256 over name, size, content per file), read and written in pieces (`ART_CHUNK_MAX`) over
  signed requests, so the page copies them Echo to Echo. Staged in `state/artifacts/<stage>/`; commit checks sizes,
  digest and, for wake word sets, `pryon_test` (next to the binary, `HASSMIC_PRYON_TEST`); install writes
  `state/artifacts/request`. The model folders are root's: `main.sh`'s watcher runs `scripts/device/artifact-install.sh`,
  which reads the staged files as the daemon's user (runas: no link can point root at its files), swaps a fresh
  root-owned folder in and restarts hassmic (the wake word list and the whisper model are read at start).
  `artifact-install.sh migrate` (main.sh, every satellite start): wake word folders under the short name (`echo-de`,
  from installs by hand) renamed to artifacts.sh's (`echo-de-DE`) where the language has one region; `state/wake_word`
  follows.
- **Clock and log stamps** (`clock.c`): `proto_esphome.c` asks HA for the time (GetTimeRequest) on keyed links (on
  subscribe, then every 6 h on pings); `clock_from_ha` leaves `<epoch> <boottime s.cs>` in `state/clock`, root's
  `main.sh` `clock_set` (netwatch) sets clock + RTC and `hassmic.clock.synced`. `clock_log_start()` (first in `main()`
  after options) turns stderr/stdout into a pipe to a double-forked reader that stamps lines and drops known Amazon
  noise; it ignores hassmic's signals and ends at EOF. Never make it hassmic's direct child (Amazon's DHA module waits
  for all children). Host: `HASSMIC_CLOCK_SYNCED=0` plays an unset clock.
- **Wi-Fi switch** (`wifi.c`): the settings page's "Switch network…". `state/wifi-request` ("scan <id>" / "join <id> <ssid
  hex> <psk hex|->") for root (`main.sh` ota_watch) -> `scripts/device/wifi.sh` in the background; answers in root's
  `/data/local/hassmic/wifi/` (`status`, `scan`, `result`, `lock/`; the id ties answer to request). The password comes
  sealed with K (`web.c` unseal, `crypto.js` seal) and only its PSK (PBKDF2) leaves hassmic. wifi.sh saves nothing
  until the Echo is on the new network with a fresh lease (dhcpcd runs with `-K` and ignores network changes: wifi.sh
  marks `dhcp.<if>.result`, runs `dhcpcd -n`, waits for the hook to write it) and its router answers ARP; else it
  removes it and selects the old one. Talks to wpa_supplicant's global socket when the interface's is gone. netwatch
  leaves wifisvc alone while `lock/` exists and runs `wifi.sh status` on every address change; `clean` at the firewall start.
- **Setup (OOBE) and factory reset** (`improv.c`, `main.c`): not set up = from an install or a reset until Home
  Assistant's voice assistant subscribed (`core_link` ready -> `state/adopted`; an existing `state/api_key` counts);
  stock's orange `setup-mode` spinner meanwhile (`core_oobe`).  `improv.c` = Improv Wi-Fi over BLE on `ble.c`'s peripheral role (advertising +
  a GATT server on one link, `conns[SRV]` beside the proxy's slots; `ble_serve`/`ble_advertise`/`ble_server_notify`):
  advertises when not set up and offline 20 s (authorized), or set up and offline 10 min (the action button authorizes,
  `improv_authorize` in `on_action`); Wi-Fi through `wifi.c` -> root's `wifi.sh` like the page's switch; no host name
  or device name commands (the Echo names itself).  `HASSMIC_IMPROV_TIMES`, `HASSMIC_FAKE_ONLINE`
  for tests.  Reset (`core_reset`: action held 10 s via `buttons.c` hold stages, HA button `factory_reset`, page
  `POST /api/reset` "reset") writes `state/reset`; root's `main.sh` ota_watch stops hassmic, `wifi.sh forget`, empties
  state/, starts it.
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
  the image has no `su`; `curlspy`, `hciscan`, `a2dpprobe`, `dha_test` not in `all`).

Boot integration (`scripts/system/`, rc in `devices/<codename>/`): `hassmic.rc` (init) starts `boot.sh` (fixed, on /system), which picks the factory
copy or a verified update and runs `main.sh` (updatable): `main.sh firewall` (egress lock re-asserted in a loop, root side
of push updates) and `main.sh satellite` (stops Alexa/updater/telemetry, keeps hassmic running, and every 10 s runs
`lockdown.sh check`, as the firewall service does every 5 s: every rule of `hassmic_out`, the chain first in OUTPUT, INPUT
policy DROP, and the stock rules the satellite needs (`keep` in `lockdown.sh`; stock `firewall.sh` can lose any of its
rules at boot); wrong twice in a row, it loads the rules itself and restarts that service; loading is one
`iptables-restore -w --noflush` call, rule by rule only as fallback). No `hassmic.conf` =
stock behaviour. `scripts/device/` holds on-device helpers (`lockdown.sh` firewall, `alexa-off/on.sh`, `wifi-join.sh`, `wifi.sh`).

Firewall invariant: Amazon's daemons may only reach local addresses; hassmic itself may reach any address (it fetches
TTS/media URLs from HA/MA). `otad`/`ace_otad` (firmware updates) must never get out. Inbound TCP and UDP are only admitted on
16384–32767, so every listening port (26053 ESPHome, 16700 Wyoming, 28928 Sendspin, 28929 OTA, 28931 settings page, UDP 28930 arbitration)
must stay in that range (UDP 28932 Drop In too). Only exception: UDP 2330 (Kiosk Satellite's fixed port), which `lockdown.sh` admits only while
`state/config` says `arbitration_mode=kiosk` (`kiosk_state`). A stock rule hassmic comes to depend on (INPUT or OUTPUT) goes into `keep`, worded as `iptables -S`
prints it.

`tools/` is PC-side reverse-engineering and firmware tooling (`payload_dump.py`, Thumb disassembly helpers,
`davs-fetch.py` for extra wake word models).

## Conventions

- Feature commits update `CHANGELOG.md` (user-facing, dated, plain language), `PLAN.md` (status, measurements) and
  `docs/GUIDE.md` when behaviour visible to users changes. Keep the README short: a row in its table or a figure, the
  detail in the guide.
- Code comments explain *why* with device facts and measurements; match that style and density.
- Proprietary/derived/secret material is git-ignored and must stay out of git: `firmware/` (per model:
  stock image, unpacked rootfs, unlock zips, `re/` disassembly), `toolchain/`, `build/`, `device-logs/`, `secrets/`, `*.bin`, `*.zip`.
- Third-party code in `src/third_party/` keeps its own licence (freeaptx is LGPL, statically linked).
