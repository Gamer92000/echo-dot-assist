# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Turns Amazon Echos into Home Assistant voice satellites. Supported so far: Echo Dot 3 (2018, `donut`, Fire OS 6574.1 /
`donut_puffin` NS65741 only); the repo is laid out for more models (`devices/`, see "Models" below; the Echo Input, `cupcake`: donut's
platform with the Shows' Android software, one LED, no speaker, running on one, `docs/re-cupcake.md`). Amazon's audio front end (`mixer` daemon + `libasp`: AEC, beamforming, mic calibration) and wake word engine
(`libpryon.so`) stay; the Alexa client `PuffinApp` is replaced by `hassmic`, a C daemon that is a client of the mixer
through the reverse-engineered C API of `libmixerAPI.so` and speaks the ESPHome native API (default) or Wyoming.

Docs: `README.md` (short user-facing overview with figures; every feature in detail in `docs/GUIDE.md`, security model in
`docs/SECURITY.md`; `docs/WINDOWS.md` the Echo Dot 2 from Windows for beginners; figures are CeTZ sources in `docs/img/src/`, rendered light+dark by `tools/figures.sh`; install instructions per model in `devices/<codename>/README.md`, guided by
`scripts/setup.sh`; every Echo on Fire OS or Vega OS and its status in `devices/MODELS.md`), `DEVELOPMENT.md` (architecture, layout, tests, contributing), `PLAN.md` (phases, open issues, every measurement), `CHANGELOG.md`
(user-visible changes by date), `docs/` (reverse-engineering findings: `FINDINGS.md`, `re-platform.md`, `re-pryon.md`, `re-aed.md`, `re-whisper.md`, `re-vad.md`, `re-a2dp-source.md`, `re-hfp.md` (phone calls over Bluetooth: where each chip puts a call's audio), `re-davs-login.md`, `wake-word-end.md` (where the request starts after the wake word: measurements on the speech
corpus, tables generated into `wake-word-end/`), `re-checkers.md`, `re-athena.md`,
`sendspin-digest.md`, `kiosk-satellite.md`: Kiosk Satellite next to hassmic on the Shows).

## Build and test

Device binaries are cross-built with Android NDK r21e (donut: armv7, bionic, API 24) and **link against Amazon's stock
libraries extracted to `firmware/<codename>/rootfs/system/lib`**. Neither the NDK (`toolchain/`) nor the firmware (`firmware/`) is in git;
without them only the host targets build. Extraction steps: `devices/donut/README.md` "2. Unpack the firmware and build".

```sh
make [DEVICE=donut]      # ARM binaries into build/donut/ (hassmic, mixcap, mixplay, pryon_test, aed_test, runas, latency, otatool;
                         # libhmui.so where device.mk has SCREEN, libhmled.so where it has LEDSHIM)
make DEVICE=donut STUBS=1  # same, without firmware/: links against stand-ins from devices/<codename>/stubs/*.syms (byte-identical)
make host                # build/hassmic-host (PC: file audio + no wake word, SIGUSR1 triggers wake, SIGTTOU a near miss, SIGUSR2 the
                         # action button pressed, SIGURG held 1 s; board.c of DEVICE),
                         # build/donut/hassmic-qemu (ARM + real Pryon, run via tools/qrun.sh), build/otatool-host (the Echo's
                         # otatool for the PC, as tests' stand-in for the device side), build/hassmic-host-screen (the same
                         # with screen.c and the band, for tests/fake_screen.py, fake_ma_api.py). Needs libopus
make build/uishot        # the Echo Show's screen UI as PNG pictures, every view (FreeType; build/uishot <dir>)
make unit                # C unit tests; ws/noise are checked against Python reference impls in .venv; the Drop In
                         # blueprint (tests/blueprint_dropin.py); one target each (unit-<name>): make -j unit runs them at once
make build/plugrun-host build/testwake   # the plugins' launcher and the tests' stand-in plugin for the PC (in make host)
make build/hassmic-host  # single target
```

Protocol/integration tests run `build/hassmic-host` against the real reference libraries (Python in `.venv`:
aioesphomeapi, wyoming, aiosendspin, noiseprotocol, aiohttp):

```sh
.venv/bin/python tests/run.py [-j N] [name]  # every test below at once (CI's way), each in a network namespace of its
                                             # own (unshare -rn: fixed ports, hassmic's defaults, broadcasts); logs in
                                             # build/test-logs/, printed for those that fail; -l lists the names;
                                             # a failure under that load runs again alone (passing: flaky, a warning
                                             # and <name>.first.log; --strict: no reruns)
.venv/bin/python tests/fake_ha_esphome.py     # ESPHome native API, as Home Assistant: entities, settings, voice assistant,
                                             # replies, announcements (HTTP, HTTPS); the rest of the API in five parts
                                             # (shared setup tests/esphome_common.py), each with an Echo of its own:
.venv/bin/python tests/fake_ha_esphome_timers.py   #   timers (ringing, the next running), encryption, clock and zone, noise
                                                   #   reduction, LEDs, light sensor
.venv/bin/python tests/fake_ha_esphome_features.py #   sound detection (HASSMIC_FAKE_SOUND_WINDOW: 3 s windows), Wi-Fi motion
.venv/bin/python tests/fake_ha_esphome_buttons.py  #   runs cancelled while HA thinks or streams, the "Button" event entity
.venv/bin/python tests/fake_ha_esphome_address.py  #   a new address: links closed, mDNS announced again
.venv/bin/python tests/fake_ha_end.py         # the end of a request heard on the Echo (micend.c): a talker, dishes as loud
                                             # (no speech in the front end's records written into the capture), quieter
                                             # voices on another pitch; end of audio, not an abort; the gain left alone;
                                             # the next request after one (HASSMIC_FAKE_KEYWORD: SIGUSR1's wake word gets
                                             # a level, so its tail is looked for as on a device)
.venv/bin/python tests/fake_ha_wakealign.py   # the wake word's end by the aligner (wakealign/): the file per language spoken,
                                             # one too slow (HASSMIC_FAKE_ALIGN_SLOW) or busy goes by the level, the
                                             # speaker adaptation in use and saved (a German "Alexa", timed per loop)
.venv/bin/python tests/fake_ha_arbitration.py # two Echos under one fake HA: wake word arbitration, join paths and security
                                             # (_join, _rounds, _kiosk, _window, _afe.py beside it; tests/arbecho.py)
.venv/bin/python tests/fake_ha_dropin.py      # Drop In between two Echos: call, answer, hang-up paths, refusals, lost peer,
                                             # the ringtone at the sounds volume (HASSMIC_EARCONS: stand-in stock sounds)
.venv/bin/python tests/fake_hfp.py            # phone calls over Bluetooth (hfp.c): a phone behind a fake controller (pty,
                                             # HASSMIC_BT_DEV): SDP, RFCOMM, the AT exchange, ring, answer, end, dial, the
                                             # handset and "take it here", do not disturb, the audio both ways as SCO
                                             # data over HCI (HASSMIC_SCO=hci); remote volume, a second call (+CCWA,
                                             # CHLD, +CLCC answered from the fake's call list); codec negotiation and
                                             # mSBC both ways (sbcenc/sbcdec -m); the feature off
.venv/bin/python tests/blueprint_dropin.py    # the Drop In voice blueprint against stand-ins of HA's registries, states,
                                             # actions and template engine: renamed ids, other devices, failures, replies
.venv/bin/python tests/ha_real_end.py         # the Echo's end of audio in a real Home Assistant (stand-in STT and TTS as a
                                             # custom component): HA's STT stream ends with it, intent and reply follow
.venv/bin/python tests/ha_real_dropin.py      # the blueprint in a real Home Assistant (build/ha-venv, made by --setup;
                                             # skips without it) with four Echos, one the released build: replies, timing.
                                             # All of it in an `unshare -rn` namespace with only lo
.venv/bin/python tests/fake_ha.py [--qemu]    # Wyoming; --qemu uses the ARM build + stock Pryon model under qemu-arm
.venv/bin/python tests/fake_talk_duplex.py    # -D over Wyoming (a realtime client with its own barge-in): the mic through
                                             # a reply, a wake word then left to the client, the button still cuts it,
                                             # -D under ESPHome ignored
.venv/bin/python tests/fake_ma_sendspin.py [--legacy]  # Sendspin, as Music Assistant 2.10.8 (aiosendspin 10.2.1, Sendspin 1.0;
                                             # --legacy: 9.1.1 of MA <= 2.10.6, build/aiosendspin-9.1.1 from --setup),
                                             # strict: a deviation MA would only log fails; the output's clock
                                             # 500 ppm slow (HASSMIC_FAKE_MUSIC_PPM) and AudioTrack's burst at the
                                             # start (HASSMIC_FAKE_MUSIC_BURST_MS), followed without a phase step
.venv/bin/python tests/fake_ha_stop_word.py   # "<wake word>, stop" between two Echos: the drop cancelled, stop_ends_listening
.venv/bin/python tests/fake_ha_remote_bargein.py  # -w remote: a wake word run beside each reply, its wake word cuts the reply and
                                             # goes on as the request, "stop" after it, failed runs asked for again (3 at most)
.venv/bin/python tests/fake_screen.py [--legacy]  # the Echo Show's screen UI as hassmic feeds it (stand-in UI): Sendspin
                                             # metadata/artwork (1.0's transfer, scheduled; --legacy: 9.1.1's), the band,
                                             # commands, home app, restarts
.venv/bin/python tests/fake_screen_words.py  # words and timers on the Show's screen (issue #104), as screen.c tells the
                                             # UI: settings, transcript and reply, a "stop" never shown, timers
.venv/bin/python tests/fake_ma_api.py         # Music Assistant's API for the screen (mass.c) against a stand-in server:
                                             # the page's sealed token, auth, players, queue, lists, commands, images
.venv/bin/python tests/fake_web.py            # settings page: login by button, signatures, export/import, HA in step;
                                             # one wait for the button and the time after each, after a press that served
                                             # nothing and over a restart (one_wait), the asks' count, files that are none
.venv/bin/python tests/fake_web_artifacts.py  # models copied Echo to Echo through the page, root's installer, after a restart
.venv/bin/python tests/fake_web_ring.py       # light ring designs: library, assignments, apply with the test as root,
                                             # the turned files -G makes, what plays, root failing, caps
.venv/bin/python tests/fake_web_wifi.py       # Wi-Fi switch: sealed password, PSK, root's wifi.sh against a fake wpa_cli;
                                             # the fixed address (issue #191): refusals, tried, kept once reached, back,
                                             # netwatch's keep (WIFI_TCP: a stand-in /proc/net/tcp)
.venv/bin/python tests/fake_wifi_join.py      # checkers' setup join (wifi-join.sh) through a fake adb and WifiService
.venv/bin/python tests/fake_wifi_roam.py      # roaming between access points (issue #234): root's roam.sh against a fake
                                             # wpa_cli (Amazon's picker, a Dot 5's scan_results, BTM), pinned moves
.venv/bin/python tests/fake_web_mww.py        # microWakeWord: models added/edited/deleted from the page, engine switch,
                                             # real detections on testdata/alexa_espeak.raw, copy as an artifact
.venv/bin/python tests/fake_plugins.py        # plugins (docs/plugins.md): hassmic under build/plugrun-host with a stand-in
                                             # wake word plugin (tests/plugins/testwake.c): packages refused and taken, the
                                             # button, settings, a secret, files and their checks, the plugin as the wake
                                             # word engine, missing against down, crashes, an update, removal
.venv/bin/python tests/fake_plugins_ha.py     # plugins as a person at the Echo and Home Assistant see them: the ring's own
                                             # pattern while an install waits (set, unset by press, cancel, running out;
                                             # HASSMIC_FAKE_LED, HASSMIC_LOGIN_MS), the event entity "Plugin" listed with the
                                             # feature and fired by install, update, removal; a manifest put into state/
.venv/bin/python tests/fake_plugins_fetch.py  # plugins from the plugin index against a stand-in site (tests/plugsite.py:
                                             # built by ../hassmic-plugins' own tools/index.py where that tree is there,
                                             # else by plugsite's builder of the same bytes; HASSMIC_PLUGIN_INDEX,
                                             # HASSMIC_PLUGINS_PUB): the list (asked only when the page says look; four
                                             # files), both signatures, a lower serial refused, 64 plugins listed, API
                                             # and needs, fetch, button, install, updates with and without the button,
                                             # every refusal (size, SHA-256, another key's listing, another version),
                                             # revocations (a plugin stopped and kept, ids and author keys refused,
                                             # official ones too, started again when no longer named, "run it
                                             # anyway"), the index and revocation list of one time, a redirect to
                                             # HTTP, User-Agent, a lower tier, serials from the future and under the
                                             # least (HASSMIC_PLUGINS_LEAST, _BUILT, HASSMIC_CLOCK_SYNCED), a revocation
                                             # list never back (the clock set after one taken without it; the site's
                                             # older: the look goes on), a kept list under a raised least, the week's
                                             # warning (its age over restarts, a clock behind, no word), the daily look
                                             # (HASSMIC_PLUGIN_DAILY), an install right after a publish; cancel, fetch against
                                             # upload; a package pushed (HMOTA-PLUG1, both otatools); versions that
                                             # need a newer hassmic (HASSMIC_PLUGIN_SCOPES lowers this build) passed over,
                                             # named, refused (HASSMIC_PLUGIN_FRESH, _LEAST)
.venv/bin/python tests/fake_plugins_race.py   # the one install slot: another browser's fetch/upload, a push and hassmic's
                                             # own fetch between /install's check and its ask (HASSMIC_PLUGIN_ASK_DELAY),
                                             # refused while a dialog is the browser's, told and nothing installed by the
                                             # press once it was left too long (HASSMIC_PLUGIN_SLOT); the fetch's whole
                                             # time, a cancelled look, the list's pace, the push port's deadlines
.venv/bin/python tests/fake_plugins_net.py    # phase 2 with a stand-in forwarder (tests/plugins/testnet.c): the
                                             # install's lines, the server the owner entered and nothing else (UDP, TCP,
                                             # outside the network; loopback, the Echo's own, a local route, unset, no
                                             # address, another host or port, a socket of its own, a default in the
                                             # server's rows, a connect before READY refused; a change of the server: a
                                             # restart, the old one hears nothing; a server that never answers: 10 s),
                                             # the dialog's lines whole, the log as a plugin gets it (private lines,
                                             # root's, a wake plugin's left out, addresses blanked; hassmic's writers
                                             # driven by a stand-in HA with sentinels), storage (cursor, kB, the bucket,
                                             # update, sealed copy in, a wake plugin's refused, remove), wider updates,
                                             # the development flag, SIGTERM (the stop callback's write lands), the
                                             # storage GET's route (id storagebox); other hosts behind a veth pair (tests/peerhost.py,
                                             # tests/run.py netns(peer=True), other_socket; alone: it makes that
                                             # namespace itself; HASSMIC_PLUGIN_STORE, HASSMIC_PLUGIN_LINES)
.venv/bin/python tests/fake_plugins_lan.py    # tests/plugins/testnet.c as "testlan" (mode reach; PEER test): the
                                             # lines of net.lan, net.internet and log.read.full, the whole log refused for
                                             # a sideload and a community listing and at a start without its listing,
                                             # every CONNECT of both kinds against the other hosts, the whole log against
                                             # sentinels (never a sealed line), five plugins at once, one of 96 MB waiting
                                             # for its place (128 MB) and starting when others stop
.venv/bin/python tests/fake_plugins_music.py  # phase 4 with a stand-in player (tests/plugins/testmusic.c, PEER test):
                                             # the install's lines, refusals of manifests, the plugin stream (rate, stereo,
                                             # level, HASSMIC_PLUG_MUSIC), ducking, button, "stop", HA's media player
                                             # (pause/play/stop, state), volume both ways, newest wins against a stand-in
                                             # source (HASSMIC_FAKE_SOURCE) and Sendspin, UDP from a private and a public
                                             # address, SENDTO, the search answered and proposed, the owner's click,
                                             # STATE, NET, entities and actions in HA, every refusal, a second music
                                             # plugin, switched off/on, removed
.venv/bin/python tests/fake_plugins_call.py   # phase 5 (call) with a stand-in phone (tests/plugins/testcall.c) under the
                                             # Show's build (a stand-in UI: fake_screen.py --ui) and a second Echo: the
                                             # install's lines, a call out from HA's action (its token), one in rung,
                                             # answered by button, screen (HA's and the card's action refused: only a
                                             # person at the Echo answers), never live before; the ringtone stopped at
                                             # the press; the plugin's dup() of the room's socket empty and at EOF after
                                             # the end; a Drop In ended with a plugin's ring right behind it;
                                             # the room (HASSMIC_CALL_CAP) and the plugin's tone (HASSMIC_VOIP) both ways,
                                             # music paused, ended every way, busy against Drop In both ways, DND, mute,
                                             # ring limits (HASSMIC_PLUGIN_CALL_TIMES), every refusal, a crash mid-call
.venv/bin/python tests/fake_plugins_request.py # phase 6 (request.*) with a stand-in listener (tests/plugins/testreq.c: a CRC-32
                                             # of what it got, in its log): the copy byte for byte as HA got it, lead-in
                                             # included (wake word, button, follow-up, noise reduction), every end
                                             # (HA, error, cancelled, muted, link lost, timeout), a slow reader dropping
                                             # while HA loses nothing, texts ("stop" acted on not passed), request.text
                                             # alone, REQ_* sent by a plugin, -w remote, Wyoming; the second press (its
                                             # screen's ask), the partner (engine choice, start, update, a restart)
.venv/bin/python tests/fake_plugins_button.py # scope button: `button` (SIGUSR2/SIGURG reach the stand-in tests/plugins/testbutton.c as
                                             # Home Assistant's "Button" hears them; never a press that served a login's or
                                             # an install's wait), rows of its own on the card (/api/plugins `rows`), `page`
                                             # and `button` in updates, HASSMIC_PLUGIN_SCOPES=button:0 (a build without it)
.venv/bin/python tests/fake_plugins_community.py # community plugins' switch (docs/plugins.md 2.7): off by default, refused at
                                             # check and install, stopped and kept, on and off from an approved browser
                                             # without a press (refused unsigned), exported, an import and a preset; a
                                             # community wake engine; restarts; the state line per domain
                                             # (HASSMIC_PLUGIN_DOMAIN) and the install method (HASSMIC_INSTALL)
.venv/bin/python tests/fake_plugins_selftest.py # hassmic updates beside plugins (5.9): an online update and a push handed
                                             # to root as they come, nothing fetched or held, the wake plugin's mark kept;
                                             # the self test: the owner's off not waited for, a build that needs a newer
                                             # scope (a fall back: HASSMIC_PLUGIN_SCOPES) not either, card and Amazon's word;
                                             # a silent launcher (fake_plugins.start launcher_fd), a changed program held;
                                             # community off; a late pass; root's answers late
.venv/bin/python tests/fake_mww_migrate.py    # the move from the built-in microWakeWord (deprecated) to its plugin: the
                                             # real plugin's PC build from ../hassmic-plugin-microwakeword (skipped without
                                             # it), a sideload of that id refused, the install with the button, models
                                             # COPIED (fields brought into range), the two stores apart, words, switch
                                             # back, the copy beside Amazon's engine and "switch", a copy again (the
                                             # plugin's fields kept, "switch" refused while it listens), ps_mww_own at a
                                             # restart (once, official with wake only, unchecked; nothing done without it;
                                             # both picked words listening after it; mww: artifacts' 2 files)
.venv/bin/python tests/fake_plugins_sync.py   # a plugin copied Echo to Echo (5.4): three Echos, two in one network; the
                                             # voucher, package, settings, files through the API as the page does; no
                                             # press on the target, `accepted` "synced, voucher named", the note (who
                                             # dismisses it, its bound HASSMIC_PLUGIN_NOTES, "pending", the wait for
                                             # the button's hold: SIGURG, three an hour, never a press; a failed
                                             # install, HASSMIC_PLUGIN_FAIL_PACKAGE, state/web_button), HA's `synced`
                                             # held; every refusal ends in "button" (HASSMIC_PLUGIN_NONCE_MS); the slot;
                                             # the index's revocation list from Src to Tgt, which cannot reach the
                                             # index (arb's message, GET /api/plugins/revoked with the network's MAC),
                                             # not to Out, not with plugins off (tests/plugsite.py, HASSMIC_PLUGIN_DAILY,
                                             # _MEMBER); the test's own members on arb's channel (one's word from
                                             # outside the local network dropped: 198.51.100.9 on lo, tests/run.py
                                             # LO_ADDRS): an older list refused
                                             # with the clock set, one fetch per member, a member holding requests, a
                                             # redirect not followed
.venv/bin/python tests/fake_web_davs.py       # models downloaded from a fake Amazon: login by code pair with the device
                                             # attestation token, DAVS, unpack, staging, install, deregister
.venv/bin/python tests/fake_ha_update.py      # online updates: page channel + HA update entity, fake GitHub, root's installer
.venv/bin/python tests/fake_ha_sources.py     # Sources (issue #175): the owner's update server (a stand-in Forgejo) and plugin
                                             # index, a key of the owner's and no check with the button, root's page key
.venv/bin/python tests/fake_ha_wakesound.py   # wake sound removal: the PC's stand-in echo (HASSMIC_FAKE_ECHO) off/on
.venv/bin/python tests/fake_ha_volumes.py     # separate volumes: four of their own or following the music, the mixer at the
                                             # loudest, HA's media at the music's, keys on what plays, gains in dB, a
                                             # restart, a Bluetooth speaker, the page, exports (HASSMIC_FAKE_MIXER,
                                             # HASSMIC_FAKE_SPEAKER)
.venv/bin/python tests/fake_ha_timer_sound.py # the timer's sound (issue #63): stock's tone by default, looped and cut by
                                             # stops and pipelines, the beep at its level, uploads (the page's WAV only),
                                             # Play, delete, restart, the beep without the tone
.venv/bin/python tests/fake_web_sounds.py      # sounds of one's own in place of stock's (Sounds card): the list, stock's
                                             # as a WAV, uploads within each sound's length, Play, the wake word, restart
.venv/bin/python tests/fake_ha_ring.py        # light ring: the animation set from stand-in stock files, HA's light,
                                             # the Echo's own patterns first, the listening ring turned by the beam
.venv/bin/python tests/fake_ha_ring_live.py   # the light ring live: LEDs, frames, animations, designs into a stand-in
                                             # frame file (HASSMIC_FAKE_FRAME), the Echo's own first, "Light ring" vs live
.venv/bin/python tests/fake_ha_ring_acs.py    # the same on binary animations (the Echo Dot 5): the set with stock's
                                             # index files, the light drawn into its two slots through a stand-in shim
                                             # (one client at a time: hassmic keeps one connection), drawn again after ledcontroller
.venv/bin/python tests/fake_ha_ring_live_acs.py # the light ring live there: frames into hassmic_live through the shim,
                                             # set once, unset for the Echo's own and mute, set again after ledcontroller
.venv/bin/python tests/fake_ha_bt_proxy.py     # the Bluetooth proxy switched off and on (issue #152): device info, scan,
                                             # subscriptions, slots and connections refused (fake_improv.py's controller)
.venv/bin/python tests/fake_improv.py         # setup from a phone: Improv over BLE against a fake controller on a pty
                                             # (HASSMIC_BT_DEV), root's wifi.sh, adoption, factory reset (HA, button hold);
                                             # set up: one press one switch, within its time, nothing served while
                                             # nothing is advertised, a phone that reconnects as its link ends
tests/ota_push_test.sh                        # signed push-update path end to end
tests/otatool_test.sh                         # scripts/otatool.py against the C otatool: same keys, signatures, bundles
```

`.venv` from `uv sync` (`pyproject.toml` + `uv.lock`; change a version, then `uv lock`). CI (`.github/workflows/build.yml`, DEVELOPMENT.md "CI and releases"): every model with
`STUBS=1`, PC tests (`tests/run.py`; `ha_real_dropin.py` in a job of its own), then a push to `main` publishes `v<version>-beta` (prerelease), a push to `release` publishes
`v<version>` (a beta only when some model's `scripts/content-id.sh` differs from the newest release's: no betas for CI or doc
changes); version = commit time in UTC, `2026.10.02.091530` (`make version`; local builds add `+<commit id>`, CI
passes `RELEASE=1` for the bare one), bundles `hassmic-<codename>.bundle(.sig)`
signed in the `release` job only (secret `RELEASE_SIGNING_KEY` of the GitHub environment `release`, main/release only;
build jobs sign with a throwaway key) with the release key (base64 of `secrets/release.key`; public half `keys/release.pub`,
shipped in every bundle; `keys/release.conf` names it and the repository, `UPDATE_REPO`; a fork's own releases:
`scripts/fork-release.sh`, `secrets/fork.conf` over it, in CI from the variable `RELEASE_PUBLIC_KEY`, and a fork's
own plugin index from `PLUGIN_INDEX_URL`, `PLUGINS_PUBLIC_KEY`, `PLUGINS_PUBLIC_KEY2`).  No key may be of small order
(`src/include/edkey.h` `ed_key_weak`: the null signature verifies under it for every text; Monocypher's check is
cofactored): the Makefile refuses one (`key_bad`, through `scripts/otatool.py` `weak()`, which also refuses every
encoding `decode()` does not read: six Monocypher reads as small order; no python3: no key passes), and every place
that takes a key asks it (`pi_keys`, a plugin author's at install, `ota.c` `load_key`, both otatools, the index's tool).
`tools/mkstubs.sh` regenerates the stub lists from the firmware and checks byte-identity; rerun it when code starts
using another stock library function. Builds are reproducible (`BUILD_TIME` = commit time). Test the
workflow with `act push` (publishing is a dry run under act).

One unit test: `make unit-<name>` (`UNIT_TESTS` in the Makefile).
`tools/qrun.sh [-t secs] <arm-binary> args` runs a device binary on the PC under qemu-arm against `firmware/$DEVICE/rootfs`
(needs a new PID namespace; bionic mutexes deadlock with pid > 65535; where users may not make one, Ubuntu 24.04+, it
stops and names the sysctl, which the setup's `qemu_arm` asks to set).

## Device workflow

- adb shell is root, without authentication. Over Wi-Fi it is closed on an installed Echo (`lockdown.sh` `adb_gate`):
  opened for 30 min from the settings page (an approved browser plus a press of the action button; hassmic writes `state/adb-request`, the firewall
  watcher opens it and marks it with `/data/local/hassmic/adb-open`), by `scripts/adb-wifi.sh [host]` (signs a challenge
  on the push port with `secrets/update.key`; no HA needed), or kept open by `ADB_WIFI=1` in `hassmic.conf`. Then
  `adb connect <echo-ip>:5555`. USB always works.
- `scripts/deploy.sh`: build + push to `/data/local/hassmic` for trial runs (`adb shell sh /data/local/hassmic/run.sh`).
- `scripts/ota-push.sh [--plugin <file.hmp>]... [host]`: build, sign with `secrets/update.key`, push bundle to TCP 28929 on an installed Echo
  (`--plugin`: an official plugin's package after it, `otatool.py plugin`, ota.c `HMOTA-PLUG1`; "Plugins" below).
  The Echo verifies against the public key on its system partition and falls back if the new build does not stay up.
  Every update (pushed or online) is promoted without approval as soon as it passes its self test (`main.c`: started,
  wake word engine loaded, ports bound, 1 s of mic audio within 30 s -> `ota_healthy()` -> `state/ota/healthy`, which
  names the build (`VERSION_NAME`: the bundle's VERSION); held past the 30 s by the wake word plugin alone,
  `selftest_thread` asks again every 10 s for as long as hassmic runs and passes then, `plugin_selftest_ok(id, late)`;
  nothing while a bundle is with root, `ota_handed` (root silent past the handoff's minute: its answer read once it
  took the request, every 2 s by ota.c's `late_thread` until it came, a "no" lets the self test ask again,
  `HASSMIC_OTA_ANSWER_S`; an answer is the handoff's only with its nonce: `stage()` writes `request` "1 <32 hex>"
  (the handoff changes hands only once bundle and signature are stored, back where the request is not: a push not
  stored never takes the one before's), `main.sh` `ota_nonce` (the only place `request` is deleted: one hassmic writes
  after it is the next look's) echoes "nonce <hex>" (or `-`) as `result`'s second line, `result_read`; a result without
  that line is an older main.sh's, taken: `boot.sh` picks the copy per service, so either copy's hassmic can run
  beside the other's main.sh); root takes `healthy` whenever it comes, `main.sh` `healthy`:
  promoted only where it names `$cur/VERSION`, anything else dropped, and taken before every install (from /);
  `HASSMIC_SELFTEST=<window ms>:<again ms>` for tests):
  `main.sh` (`factory()`, only if that version is installed and its hassmic is what runs) has `sysinstall.sh` write it to
  `/system/hassmic` as the factory copy, bootstrap included (`boot.sh`, `otatool`, `hassmic.rc`, `release.pub`; never
  `update.pub` or `/sepolicy`). Works from older installs: the pushed update brings the code.
  Where the factory copy is in an overlay on /data (device.conf `ROLLBACK`: cannoli), `sysinstall.sh` keeps the copy it
  replaces in `ROLLBACK/previous` (only one `main.sh` `reach_check` saw reached from the network: a connection from
  another address to a port of ours), and the flashed `prepare.sh` steps back after two boots without being reached
  (`ROLLBACK/boots`; main.sh excuses a boot before set-up or without an address, for a version reached before): the unpromoted update, else the
  factory copy to `previous`, else to the flashed copy; each version taken out into `ota/refused`, never installed again.
- The light ring's own animations: `main.sh` installs them itself at satellite start when they are not this build's
  (none where `/system/etc/led-resources` is missing: checkers)
  (`sysinstall.sh leds`, ledcontroller restarted); `install-system.sh --uninstall` puts stock's layer config back.
- `scripts/install-system.sh`: writes `/system/hassmic/`, init rc and patched SELinux policy from the running OS
  (`sysinstall.sh install`; `otatool remount rw` since toybox cannot: `/dev/root` does not exist). `--twrp` the old way.
  `--uninstall` reverts.  `INSTALL=boot` (checkers: no A/B, policy in boot.img) hands over to `scripts/install-boot.sh`:
  the firmware's own boot.img + root adb, `verify` out of fstab and the policy patched (`scripts/mkbootroot.py`,
  `sepolicy.rules` via magiskpolicy under qemu), flashed in amonet's hacked fastboot, then the same `sysinstall.sh`.
  `INSTALL=running` (athena, Android 9: rooted by saiyaku's zip patching system and vendor in place, no boot image of ours)
  is the in-OS path only, with the policy where `device.conf` `SEPOLICY_FILE` says (`/vendor/etc/selinux/precompiled_sepolicy`,
  `sysinstall.sh` remounts `/vendor` only to write it; `.pre-hassmic` beside it) and the Android extras (dex programs, home APK) as `boot`.
  Whether Android's framework runs is `device.conf` `ANDROID=1`, not the install method (cupcake: `twrp-ab` and Android):
  the dex programs, `wifi.sh`'s and `wifi-join.sh`'s WifiService path go by it.
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
- `scripts/probe.sh` checks the device's libraries match the analysed firmware. Everything assumes exactly that version;
  another build goes in only with every `PROBE_FILES` the same and no build-bound image flashed (`device.sh` `firmware_ok`:
  not `INSTALL=boot`), or, `INSTALL=running` (athena), after a question (`HASSMIC_FIRMWARE_OK=1`; setup's `firmware_accept`).
- PC scripts that use adb (`deploy`, `probe`, `install-system`, `capture-test`, `mic-compare`) detect the model from `ro.product.device`
  via `scripts/lib/device.sh`; `ota-push.sh` takes `DEVICE` (default donut). With two Echos on adb set `ANDROID_SERIAL`.

## Models

`devices/<codename>/` holds everything model-specific; porting guide in `devices/README.md`:
- `device.mk`: NDK target, audio/wake backends, stock libs to link (included by the Makefile).
- `board.c`: `struct board` (`src/hassmic/board.h`): HA identity, keypad/mute-latch/BT device paths, stock wake word
  and earcon paths, thermal zone, LED volume steps, volume curve, speaker gain control. Linked into every hassmic build, host builds included.
  `volume_curve` is required and every model must have its own, measured on it (`devices/README.md` "Volume curve");
  until then `board_vol_dot2` stands in under a comment saying so, and `make unit-volcurve` prints a `todo` per such
  model. A new model comes with its own curve or a `PLAN.md` entry saying it is missing.
- `device.conf`: shell vars (product/firmware id, probe files, `INSTALL` method, daemon user/groups, stock service
  names). Sourced by PC scripts and shipped to the Echo next to `main.sh`/`lockdown.sh`/`alexa-off.sh`; push bundles
  carry it, and `main.sh` refuses a bundle whose `PRODUCT` is not the Echo's.
- `hassmic.rc`, `sepolicy.rules`: installed by `install-system.sh`.  After the model's rules every install adds
  hassmic's own SELinux domain (`scripts/system/sepolicy-hassmic.rules`: enforcing, `allow hassmic * * *` less property
  writes, domain changes, ptrace; `sepolicy-mirror.sh` copies to it what other domains may do to su, from the policy
  being patched); `runas -Z u:r:hassmic:s0` (main.sh) enters it by its own context, not the exec's (/data is nosuid).
  The services stay in su, which the unlock makes permissive: there any process may set any property (issue #130).
- `stubs/*.syms`: what our binaries need of each stock library (`tools/mkstubs.sh`), for `make STUBS=1` / CI.
- `README.md` (model facts + install steps by hand) and `setup.sh` (same steps for the guided `scripts/setup.sh`:
  `STEPS` list + `step_<id>` functions using `scripts/lib/setup.sh`; `--dry-run` walks them without running). Keep
  the two in step.  The unlock and boot-root are never run, installed, wrapped or retold by either: they are
  R0rt1z2's, who asks that they are only run by hand as his XDA thread says (kamakiri-donut v1.1.0's LICENSE);
  `step_unlock` names the thread (`unlock_thread`), waits for Enter and checks for TWRP on adb (`twrp_or_unlocked`),
  `step_root` the same for boot-root (`root_thread`, root adb); the README links the thread.  No file of the threads
  is downloaded, read or copied either: magiskpolicy comes from Magisk v30.7's APK (`magiskpolicy_get`, byte for byte
  boot-root's old one), the Shows' userdebug adbd from Google's Android 7.1.1 emulator image (`adbd_get`, its ramdisk
  only, `scripts/zipmember.py`).  Removed 2026-11-09 at his request; do not add any of it back.
No model `#ifdef`s in shared code: new differences become a board field, a `device.conf` variable or a backend.

## Architecture

`src/hassmic/` is one daemon; `main.c` is the satellite core, the rest plug into it through narrow headers:

- **Core (`main.c`, `core.h`)**: state machine `IDLE/LISTENING/THINKING/SPEAKING`, pipeline timeout, TTS queue with
  barge-in flush, alarms (timers; `timer_sound`: stock's timer tone by default (`sounds.c`, libEarconAdapter's
  timerDefault; the blip where the firmware has none), its alarm tone, the blip at the same loudness (`BEEP_PEAK`,
  -16.5 LUFS), or the page's upload `state/timer_sound.wav`, which the browser converts to 48 kHz mono WAV and offers
  to bring to that loudness (`web/loudness.js`, BS.1770; `tests/unit/web_loudness_test.py`): the Echo only checks the
  format; looped by `play_earcon_while`, cut by any stop, a pipeline or our sounds; issue #63; running timers from
  HA's started/updated events kept by id with their end on the monotonic clock, `core_timer_next` the active one
  ending first for the "Next timer" entities, a timer HA never finished dropped `TIMER_STALE_MS` after its end by the
  earcon thread), mute (hardware latch
  that software can set but never clear, plus a soft mute from HA), volume, LED ring, earcons (one's own in place of
  stock's: `state/sounds/<id>.wav`, sounds.c `sound_own_*`, the page's Sounds card), wake word threshold hints.
  Separate volumes (setting `separate_volumes`, issue #32):
  music (`volume`), replies, sounds, timers, Drop In calls at volumes of their own or each following the music's +- an offset in
  points (`<kind>_linked`/`_offset`; linked, its HA number is unlisted); MainVolume and TTSVolume both at the loudest
  (libasp's own volume, which boosts the top end, follows the last of them set), each kind turned down in its PCM by
  the model's curve (`volcurve.c`: `board.volume_curve`, the Dot 2's `board_vol_dot2` standing in where not measured yet, `tests/unit/volcurve_test.c` per model; `audio_music_gain`, `audio_tts_gain`, the earcon thread); HA's `play_media` is music
  though on the TTS stream (`core_tts_begin`'s `media`, `tts_kind`). docs/re-platform.md has the measurements. The front end is told when a command is spoken (`listening()`:
  without it its cancellers remove the talker after 1.5 s). `earsup.c` (wake sound removal: our own UI sounds taken back
  out of the pipeline's audio by a residual echo suppressor keyed to the PCM we play; experimental, setting
  `wake_sound_removal`, off by default; holds the audio from the sound until its echo has passed, then releases it at
  once), then `micdenoise.c` (RNNoise, settings page off/low/medium/high, off by default), then
  `micgain.c`: AGC on the mic audio sent to the pipeline (the stock
  micAsr level is ~30 dB below what STT expects, and HA ignores the ESPHome audio settings); the wake word gets it raw.
  After a wake word the stream starts from the ring at the word's end: `wakealign.c` (issues #72, #79) aligns the word's
  phones (MFA's GMM-HMM, cut down per wake word and spoken language to `<word>-<locale>.wa` (microWakeWord: name slug),
  its GMMs in `<acoustic model>.was` shared by the words of that model, next to the binary or in `HASSMIC_WAKEALIGN`,
  picked per detection by `align_resolve`: setting `speech_language`, else the time zone, else the word's own locale
  (en-US for the firmware's "alexa" and microWakeWord's); en-CA, en-AU, fr-CA take en-US's, en-GB's, fr-FR's files (the
  same models); made by `tools/wakealign/build.sh` from MFA's pretrained models, in git as `wakealign/` (2.5 MB; CC BY
  4.0, credited in `wakealign/README.md`); format 3: 16 Gaussians a pdf, float16, a check per GMM) in [Pryon's end - 1.5 s, + 0.5 s],
  run in `align_thread` (started by `align_ahead`; never on the capture thread, issue #77; the replay waits for
  `align_done`), taken in [-400 ms, +500 ms] of Pryon's end (it may start before it: Echo, Ziggy, the English models
  fire after the word); a pause after the word: the replay from its middle (the aligned silence's, or
  `pause_after`: 80 ms at the room's floor + 6 dB within 200 ms; the corpus: 63 -> 74 % right); speaker adaptation (fMLLR per household and model space, `state/wakealign-<space>.adapt`,
  setting `wake_word_adaptation`, from 5 requests on, forgetting 0.98 a request); `tools/wakealign/walign.py` is its numpy reference
  (`tests/unit/wakealign_ref.py`). Without a model (the firmware's English "alexa" so far, microWakeWord):
  `wordtail.c` (a 100 ms pause 9 dB under the word, waiting up to 500 ms; no pause: never more than a4fe0bd's
  rule left out, often nothing). With a model and no answer near (rejected, too slow, busy): the same without its
  pause (`pause_ok` 0, issue #79: a command's "ts" passes for one); `align_thread` logs what the aligner said.
  Every run HA starts at STT (local wake word, button, follow-ups; not `-w remote`) opens with 800 ms of zeros
  (`lead_in`, issue #80): HA's microVAD is new per run and blind for its first 760 ms, and Parakeet answers "" to audio
  that opens mid-word. Tests: `esphome_common.LEAD_IN` bytes ahead of the ring's replay.
  Setting `wake_word_in_request` (off): the replay from 100 ms ahead of the keyword's begin instead (`replay_whole`,
  `kw_begin`/`kw_end` of the last detection; mww's begin is its end - 1 s); micend counts from the tail's end
  (`talker_from`); `core_transcript` drops a leading wake phrase ("Alexa, stop" = "stop"), and the wake word alone
  ends the run quietly as "nothing said" would have.
  `micend.c` (setting `end_of_request`, experimental, off; issue #66): the request ends on a pause in the voice of
  whoever said the wake word (frames within 12/9/6 dB of the keyword's rms, unless Amazon's DNN VAD says 0,
  1.25/0.7/0.45 s), quieter voices neither keep it open nor lift the gain (`micgain` `talker_min_db`); `proto->end` =
  end of audio only (`stop()`'s `start = false` is an abort to HA).  The VAD: `afemeta.c` decodes the front end's
  record in the lowest bit of every 128 micAsr samples (version 3.3; clock, RMS, playback, mute, DNN VAD 2 bits;
  docs/re-vad.md), `main.c` `ring_speech` reads it from the ring (phase found again after lost samples); none on the
  PC or where a HAL drops the bits (then level only). `core_lock` guards state and client socket writes; `core.h`
  documents per function whether the lock is held.
- **Protocols (`struct proto` in `core.h`)**: `proto_esphome.c` (ESPHome native API incl. Noise encryption provisioned
  by HA, voice assistant, media player, timers, settings entities, Bluetooth proxy messages, API 1.15's device
  capabilities with Sendspin's client id and the pairing token (request and text sensor: keyed link only), so Music
  Assistant pairs through HA; its protobuf in `pb.h`,
  header-only for `tests/unit/pb_test.c` under UBSan; a message that did not fit its buffer (`over`, carried out of
  inner messages by `pb_msg`) is never sent: HA drops the link on a cut one and connects again, over and over) and `proto_wyoming.c` + `wyoming.c`. Selected with `-P`. The
  core calls `start/audio/stop/played/...` on the active proto with lock held.
- **Audio backend (`audio.h`)**: `audio_mixer.c` on device (mixer C API: capture, voice/TTS, music, Bluetooth and
  earcon streams mixed by the mixer); `audio_android.c` where there is no mixer (checkers, Echo Show 5; cronos, its 2nd gen; athena, Echo Show 8 2nd gen; cupcake, Echo Input: OpenSL ES over
  AudioRecord/AudioTrack, the front end inside the audio HAL; device.mk `SYSLIBS` links
  `-lOpenSLES`); `audio_file.c` for PC builds. Swapped at link time in the Makefile.
- **Sound detection (`sound.h`)**: `sound_pryon.c` (Alexa Guard's model on `libpryon.so`: the firmware's, or Amazon's newest
  from `/data/local/hassmic/aed` (`scripts/artifacts.sh`); a second decoder, off unless HA's
  switch is on; ESPHome event entity "Sound"; windows with own playback dropped; `docs/re-aed.md`) or `sound_none.c`
  (host build, `HASSMIC_FAKE_SOUND`; `HASSMIC_FAKE_SOUND_MODEL` names the model, `broken` fails to load). `sound_model()`
  says which model it takes, for the page; a start that fails switches the setting off via `settings_set` and sets
  `core_sound_failed()` (page warning). Picked in the Makefile from the wake word backend.
- **Sound packs** (`soundpack.c`, `soundpack.h`): sets of the Sounds card's sounds in `state/sound-packs/<id>/` (`name`,
  `<sound>.wav`), `/api/sound-pack`, page `packsCard`, a `.soundpack` file = a tar of `pack.json` + WAVs (`web/tar.js`).
  A pack is a whole set; 16 packs, 30 MB, 32 MB left on the partition (`HASSMIC_SOUNDPACK_MAX`, `_FREE_MIN` for tests).
- **Whisper detection (`whisper.h`)**: `whisper_pryon.c` (libpryon `WhisperApi_*`, one detector per request fed from the
  ring at the request's start as HA gets it (`main.c` `whisper_from`: the wake word's tail end; live with `-w remote`),
  end of utterance on HA's VAD end (`core_mic_off`), finished in a thread at once, cut-off 500, result into the ESPHome
  binary sensor "Last request whispered" (unknown from each run's start) before STT ends, for the agent's prompt template; `docs/re-whisper.md`) or `whisper_none.c` (host
  build, `HASSMIC_FAKE_WHISPER`). Model from DAVS only, installed by `scripts/artifacts.sh` into
  `/data/local/hassmic/whisper` (not `models/`: it carries a stray `pryon.manifest`); without it, no sensor.
- **Wake word (`wake.h`)**: `wake_pryon.c` (stock `libpryon.so`, headers in `src/include/pryon_api.h`) or `wake_none.c`
  (host build) as the vendor engine, link-time swap; microWakeWord (below) on every build, picked at run time.
  Two slots (`WAKE_SLOTS`): HA's "Wake word" and "Wake word 2" listen at once, a Pryon decoder (or mww model) each;
  `main.c` `wake_act[]`, one id per line in `state/wake_word`, the run's phrase = the slot that fired (`core_wake_phrase`).
- **microWakeWord (`wake.c`, `wake_mww.c`, `mww_features.c`, `mww_model.c`, `mww_store.c`, `mww.h`)**: second wake word
  engine, off by default (setting `wake_engine` amazon/microwakeword, page section "Wake word"; detects much worse than
  Pryon, the page says what it costs).  **Deprecated** (2026-11-21, docs/plugins.md 5.9): the microWakeWord plugin
  replaces it; kept until *the cut release* (named when it is made); said wherever it is chosen (app.js
  `mwwDeprecated`, the engine's choice "microWakeWord, built in" tagged Deprecated) and in Home Assistant (problem
  sensor `KEY_MWW_DEPRECATED` while it is the engine).  The owner's move, one button on the Wake word page (app.js
  `mwwMigrate`, `mwwSwitch`, `renderMove`): plugins on (off again if nothing got installed; the dialog names the
  other plugins that start), the OFFICIAL plugin from the index with the dialog and the action button as any install
  (one installed already that is not official: named with author and tier, asked; the request says "any"), then
  `POST /api/mww/migrate` "go" (web.c `mww_migrate`, `mww_to_plugin`): each model COPIED through the store's item
  path into the plugin's own list (`files/models/<id>`; fields `cutoff`/`window` from its manifest, what `mww_edit`
  writes, staged with it and brought into the plugin's range first, `adjusted`; kept only if the plugin's FILE_CHECK
  took it, `plugin_item_checked_only`); the engine switched (the words of `state/mww_word` as
  `state/wake_word-microwakeword`, main.c `core_wake_switch`) only where the built-in engine was the engine, else
  `offer` and the page's separate step "switch".  Two stores: `state/mww` is the built-in engine's alone (no
  `list_dir` alias since 2026-11-21: one folder, two owners); an Echo whose plugin read it under a build before gets
  its own copies at the next start (`plugin_store.c` `ps_mww_own` after the first scan: the models with the
  plugin's `checked` mark, once (`state/plugins/.mww-own`), the official plugin with `wake` only, never over a model
  it has, files and fields only (it checks and names them: `look_unchecked`); without that plugin there and read
  nothing is done, marked or stripped; from its pass on the marks are taken out of `state/mww` at every start; a
  slot left empty takes a word of the saved pick the plugin lists later, main.c `core_wake_plugin_changed`); artifacts.c `mww:<id>` lists, reads and keeps `manifest.json`/`model.tflite` only).  A copy again keeps
  the plugin's own fields on a model it has (`mww_to_plugin` `kept`); "switch" is refused while the plugin is the
  engine.  Switching back is the setting.  Nothing installs the plugin by itself.  `wake.c` dispatches by model: a `.json` manifest = microWakeWord, else the vendor
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
  (stock's ESP, done on the LAN; the score is the front end's own wake word energy ratio, as stock reads it, in a
  thread of its own (`afe_thread`; the capture never waits on it, issue #77: `score_wait` claims once it is in or after
  `AFE_WAIT_MS`, `afe_late` ends a reading left behind), and only when a round runs (`arb_rounds`); PC: `HASSMIC_FAKE_AFE`). UDP broadcast on 28930 (beacons at random 20-40 s, a member counts for 5 min: broadcasts get lost; for 40 s after a start or join the beacons carry F_HELLO and every member answers at once; a newcomer asks every 1.2 s for its first 5 s), shared network key; a member hands it to a newcomer (sealed to its X25519 key) only
  when Home Assistant vouches for both: each Echo that may be waited for reports the tag `hassmic_<pub hex>` as scanned
  (event `esphome.tag_scanned`: HA fires device events without "perform actions"; `tag.<id>` has no area or device in
  it, unlike every entity id, which HA builds from area, parent device, device and entity name), the other reads
  `tag.hassmic_<pub hex>` (state requests need no permission either) and trusts the key once the scan time changed
  after its first read; then `T_GIVE` on the LAN, taken outside pairing only from a confirmed Echo. Else, 30 s after it was heard with HA's link up (`ha_since`),
  the newcomer's own ESPHome action `esphome.<node>_arbitration_key` (needs "Allow the device to perform Home Assistant
  actions"). Without HA: Volume up + Volume down held 2 s on both (`T_PAIR`/`T_GIVE`, exactly one requester). ESPHome only.
  Window (issue #27): learned per Echo from how late each member's claims come (first claim per member and round, sampled
  until 1.5 s; last 64 as `delay <id> <ms>` lines in `state/arbitration`): 95th percentile + 50 ms over the members
  counted and per member, 150-1000 ms, 500 until 10 samples; setting `arbitration_window_auto` (off: `arbitration_window`).
  The capture thread decides early once every member that takes part claimed or one beat us (`arb_settled`).
  Rivals (forum #166): per member our last 64 rounds, own score and its claim against it (`rivals`, a line
  `obs <id> <own>:<diff|-> ...` each); a round waits for it (`rival_waits`, `round_.waits`, `waited()`; the window too)
  only above 0.15 close calls (beat ours or within 3 dB), rounds weighted by own-score distance (4 dB) and age (0.99),
  close calls x3, prior 0.5 in 2; both answering counts as close.  Constants from `tools/arbsim.py` (synthetic
  house; redo with real scores); `HASSMIC_ARB_RIVALS=0` (tests) waits for all.  `state/arbitration` written at most
  once a minute (`SAVE_MS`).
  The network (beacons, key handoff, pairing; tags and action) always runs: the settings pages find each other
  through it. The `arbitration` setting (`arb_arbitrate`) only gates rounds; off, the beacon carries a flags byte after
  the counter and members leave that Echo out of rounds (older builds reject the longer beacon, same effect).
  `arbitration_mode` (`arb_mode`, in `state/config`): `hassmic` (default, the above) or `kiosk`: Kiosk Satellite's
  protocol reimplemented from `docs/kiosk-arbitration.md` (JSON claims on UDP 2330, three copies, loudness only from
  `main.c` `kiosk_score` + `arbitration_offset` dB, `arbitration_window` ±ms, no key, no priority, always waits); the beacon flags it (1 quiet +
  2 kiosk). The socket belongs to arb's loop thread. `HASSMIC_ARB_SCALE` (tests, `PACE()`): the network's own timers
  (beacons, discovery, tags, handoff, pairing) at that fraction; rounds and the window keep theirs. Page: two cards with pros and cons (`HELP.arbitration_mode.modes`).
- **Music**: `sendspin.c` (Music Assistant Sendspin player over `ws.c`/`noise.c`/`net.c`/`hash.c`, decodes via
  `dr_flac`/`minimp3`/libopus; in sync by a rate of its own, `resample.c`: a PI servo on the error steering a cubic
  resampler, never frames skipped or repeated, which ticked on the Show 5's clock 350 ppm off; snap past 20 ms; on the
  mixer the queue is a model at the nominal rate steered by `MixerGetNumBytes`, `mixsteer.h`, issue #225; a drift
  learnt per route, the Echo's speaker or a Bluetooth one, kept in `state/sendspin.drift`, held 5 s after a snap; a
  fresh output primed with silence first, `music_prime_us()`: Android's AudioTrack takes its buffer at once as it
  starts, ~137 ms on the Show 5, which the queue leaves out; the A2DP sink resamples through resample.c too). `a2dp.c` + `a2dp_codecs.c` + `sbc.c` = Bluetooth A2DP sink (SBC, AAC via firmware FFmpeg
  loaded with dlopen, aptX/aptX HD via `freeaptx`) with AVRCP. Only one music source plays at a time (newest wins). "<wake word>, stop" pauses it when nothing else was
  stopped (`stop_word`, a bare "stop" transcript in `core_transcript`; issue #51), the action button resumes.
  Ducked (`duck.c` `duck_active()`, issue #165): our own run, our own round from the wake word on, or another member
  answering a wake word we claimed too (its 2-byte `T_MSG` "on" every 2 s / "off" to every member, `arb_tell(NULL)`,
  `hooks->message` `all`: never Drop In's; sent only for runs a won round started, bridged over a follow-up's restart,
  `duck_follow`; HA's media is no run, `speak_media`; taken only from members that claimed in our round,
  `arb_round_claimed`, up to 4, each held 6.5 s past its last "on"); setting `duck_for_others` (on) gates the last two.
  A ring on another Echo (`ringstop.c`, issue #161, setting `timer_stop_from_others` on the ringing Echo, off): it
  broadcasts `T_MSG` 0x41 on/off while it rings; a "stop" (keyword or bare transcript) that ends nothing here sends
  0x42 to all, the ringer stops, and here the music stays and the wake word's run is dropped.
  A "stop" that ended what its wake word cut (ring, Drop In, reply, THINKING run) drops that wake word's run: after the
  round `stop: pipeline dropped` (`wake_cut_ms`), during it `trigger` starts none (`stop_cut`, `stopped_now`).
  Setting `stop_ends_listening` (Voice, page only, off): any "stop" within 4 s of the last detection (`heard_ms`,
  `stop_heard`) also cancels a LISTENING run (`quiet_cancel`, as the button) or keeps the round's winner from starting.
  The other way, playing on a Bluetooth speaker: `a2dp.c` (inquiry, pairing, AVDTP initiator, AVRCP absolute volume)
  + `btout.c`, which stands in for Fluoride towards the firmware's A2DP HAL (its abstract sockets) and SBC-encodes
  (`sbc.c`); the route is the backend's (`audio_a2dp_*` in `audio.h`): the Dots' mixer by LIPC `A2DPSourceConnect`
  plus btmanagerd's AIPC service uuid 0 via `libace_aipc.so` (`docs/re-a2dp-source.md`), Android's AudioPolicy by
  `media.audio_policy` setDeviceConnectionState (`docs/re-checkers.md`; players opened meanwhile wait for the move). While on the speaker the core has a volume of its own (`core_speaker`).
- **Drop In** (`dropin.c`, `dropin.h`): calls between two Echos of the arbitration network. Signalling as signed
  `T_MSG` on arb's socket (`arb_tell`, `hooks->message`; invite/ringing/accept/refuse/bye), audio Opus 16 kHz 20 ms over
  unicast UDP 28932 (`-i`), per-call key from ephemeral X25519 + `arb_derive`; a replayed packet passes the AEAD, so
  `on_audio` takes each frame once (a window of 32 numbers) and only a frame newer than all moves the peer's address
  and `rx_at` (call.c never numbers Drop In's frames anew: a plugin's only).  The claim goes in `tell_core` once its
  queue for the core is empty and no call of Drop In's runs; a full queue never drops the current call's states nor
  the last "idle" (`HASSMIC_DROPIN_LATE_IDLE` for tests: the end in the order before; `make unit-dropin`). Playback on the mixer's `Voip` stream
  (front end's call mode); mic = micHfp where the backend has it (Call audio, below), else micAsr through the pipeline's gain, then a gate (open only while this side talks:
  9 dB over the room and over -58 dBFS, `NEAR_MIN_DB`, or an empty room's noise goes out with the speech gain; echo
  estimate peak-held against the far end's level held after it stops), listening mode only while this side talks
  (`core_call_listen`); the far end talks when it plays over -55 dBFS and 25 dB over its own floor (`far_voice`). Core side in `main.c` (`core_call`, Drop In one of its
  callers: call LED animations, comms sounds, ringtones via
  `play_earcon_while`; action button answers/ends, "stop"/"hang up"/"auflegen" end; wake word ignored while the far end
  talks; `proto->dropin_changed` for Drop In's calls only).  One call on the Echo at a time: call.c's claim
  (`call_claim`/`call_release`/`call_claimed`, an atomic, taken from calling or ringing on, let go by whoever tells
  the core only after its "idle" went through `core_call`, which shows only the claim's `who`); Drop In refuses "busy"
  while another (a plugin) has it, in its old words ("busy in another Drop In": what every released build maps to
  `busy`), and `core_call_button`/`_answer`/`_hangup` go to whoever has it. Notifications to the core go through dropin's loop thread: `dropin_call` runs under `core_lock` (ESPHome
  handler); every state and result is queued (`ev_push`) and told in order (`tell_core`), and an outgoing call's
  first invite goes out only after that, so HA always sees "calling" before a refusal. HA: action `drop_in`, "Drop
  In", "Drop In with", "End Drop In", and the event entity "Drop In result" (`dropin_results`: what came of each call
  this Echo made, refusals here included; `core_dropin_result`); settings `drop_in`, `drop_in_answer`; page
  `POST /api/dropin` (status: `result`, `attempt`). No ESPHome action response (PLAN.md says why). Voice:
  `blueprints/automation/drop_in.yaml` (Echos by manufacturer `hassmic` = project name's first half, the sensor by
  its state, the result entity by its event types, never by entity id; the action named from the reported name, else
  model + MAC, so every board's project must stay `hassmic.<slug of default_name>`; called in a run of its own via
  `automation.trigger`, since ServiceNotFound would end the conversation's run with "Done"; the reply from "Drop In
  result" where the Echo has it, else from the timing of the "Drop In" sensor). Keep the entities' states, event
  types, icons and the action's name stable: released blueprints rely on them; a new result is a new event type.
- **Call audio** (`call.c`, `call.h`): Drop In's audio moved out of `dropin.c` unchanged: the mic gate, echo
  suppressor and gain (`call_mic` from the capture thread, the owner's `send` per encoded 20 ms frame), the jitter
  buffer and playout on the Voip stream (`call_rx`, its thread), Opus 16 kHz; `call_begin`/`call_end` the media's
  generation; `call_begin(who, codec, send, arg)` only for the claim's owner, `CALL_CODEC_OPUS` (Drop In) or
  `CALL_CODEC_PCM` (a plugin's: the gated, gain-corrected frame as s16le, in by number, a missing one the last
  faded by `PCM_FADE` a frame; `make unit-call`). Lock order: call.c's `tx_mu` (one sender at a time; `send` and `core_call_listen` run under it) before
  `core_lock`, `dl` and plugin.c's `cml`, `dl` and plugin.c's `bl` before call.c's `ml`; nobody calls `call_mic` holding `core_lock`, `dl` or `bl`.
  Call microphone (setting `call_mic`, call/assistant; present where `callmic_present()`: the mixer's where
  `board.call_mic`, measured: donut, biscuit; radar and cannoli have the path in AFE.cfg, unmeasured): `micHfp` is
  Amazon's "Voice" pipeline (AFE.cfg: (V)AEC, RES, NR, CNG, AGC -22/-23 dBFS, Tx EQ, limiter; no beamformer) once the
  front end is in call mode (a Voip stream open; on biscuit opening micHfp alone starts it). `callmic_thread` opens it
  after the playout has the Voip stream (`voip_gen`), sends it without micgain and with the gate's duck at -12 dB
  (`SUPPRESS_HFP_DB`, `NEAR_MIN_HFP_DB`), `call_mic` (micAsr) stands aside (`src_hfp`) while micHfp came within
  `HFP_STALE_MS` (`hfp_at`; the mixer's read waits 1.5 s for a block that does not come, the far end gives up after
  5 s); no stream, 3 empty reads or the Voip stream failed (`voip_gen` -1): micAsr for the rest of the call. Record
  streams' format getters answer -1 (mixcap.c): never refuse on that. `audio_file.c`: `HASSMIC_CALL_CAP` (tests: the
  hall's 700 Hz tone in `fake_ha_dropin.py`), `HASSMIC_CALL_CAP_STALL` (a file: while it exists reads stall, 7c),
  `HASSMIC_VOIP_FAIL` (a file: the Voip stream fails, 7d), `HASSMIC_FAKE_LISTEN` (the front end's listening mode
  logged); Android: none.  A swap of sources restarts encoder, gain and gate but keeps listening mode's state
  (`listen_on`, `near_until`): reset, the core never heard "off" and kept the front end listening for the call.
- **Phone calls over Bluetooth** (`hfp.c`, `scoaudio.c`, `cvsd.c`, `msbc.c`; docs/re-hfp.md; issue #233): the Echo as a
  phone's hands-free unit (HFP 1.7 HF), setting `bluetooth_calls` (Features, off).  CVSD everywhere; mSBC (wideband)
  where the host codes the link (`sco_wideband()`: ebc, hci): AT+BAC=1,2, +BCS answered, the link's air mode decides
  (`hfp_sco_up`'s `air`), "take it here" as AT+BCC; `msbc.c` the H2 framing (lost and broken frames), `sbc.c`
  `msbc_decode`/`msbc_encode` (held to libsbc by `make unit-sbc`; `make unit-msbc` the framing).  On a PCM path the
  chip codes mSBC itself where `board.sco_msbc_chip` (measured: cannoli's MT7663, donut's and checkers' MT7668; on by those on cupcake, cronos, crown, athena):
  `sco_accept_params` gives Enhanced Accept (0x043e, data path 1, 16 kHz linear, transport unit 16 (the MT7668 sets
  up no link with 0), T2; byte for byte what `src/tools/scoprobe.c`'s air `msbc -U 16` measured, held by `make
  unit-scoaudio`, and through a2dp.c's command queue (72 bytes a command) by fake_hfp.py with `HASSMIC_SCO_CHIP_MSBC`),
  the PCM devices at 16 kHz; an mSBC link that fails twice (or its Enhanced Accept refused: a2dp.c hands the
  command's status to `hfp_sco_up`) offers CVSD alone (AT+BAC=1) for the rest of the connection.  Remote volume: `+VGS` sets `core_call_volume` only while a
  call's audio is here, main.c's `mix_apply` hands the call's volume to `hfp_call_volume` (stored; upkeep sends
  AT+VGS).  A second call: AT+CHLD=?, AT+CCWA=1, +CLCC where the phone has enhanced call status (the indicators
  cannot say which call is which after a swap: `ind_moved` guesses, +CLCC corrects), AT+CHLD=2 (`hfp_switch`: HA's
  "Switch phone call", the action button with one waiting), AT+CHLD=1 ending beside a held one; HA's "Phone call
  waiting" (`hfp_second`).  On a2dp.c's links (hooks in
  `hci.h`): a Hands-Free SDP record (RFCOMM channel `HFP_CHANNEL`), PSM 3, the class of device's telephony bit and the
  EIR's UUID, all only while it is on; RFCOMM as responder (the phone opens it), credit flow, the AT exchange of the
  service level connection, calls from the phone's indicators.  call.c's claim `CALL_HFP`: ringing (not with do not
  disturb or the mics off: `core_call_refusal`), answered here (`ANSWER_MS` for the audio link), connected (the
  synchronous link up: call.c's media in `CALL_CODEC_PCM`), on the phone (nothing shown).  `core_call_button` /
  `_answer` / `_hangup` reach `hfp_*`; Home Assistant's "Phone call", "Phone caller", "Phone", answer / end / take-here
  buttons and the action `phone_call` (number), answering and dialling over the keyed link only
  (`core_phone_changed` -> `proto->phone_changed`).  No chip moves SCO data over HCI: `scoaudio.c` takes the audio
  where `board.sco` says (`sco_play`/`sco_cap`: PCM devices by name, found in /proc/asound/pcm as MediaTek's HAL does,
  `make unit-scoaudio`; `sco_route` controls, `bt_sco_cmd` the chip's PCM setup after every
  reset, MT7668: 0xfc72 03 10 00 4a), raw ALSA ioctls at 8 kHz (`pcm`: cannoli, donut, checkers; from the firmware
  only, never run: cronos, crown, athena, cupcake: its bootloader, not its device tree, muxes the 2nd I2S), or biscuit's (and radar's, from the firmware)
  BTCVSD `/dev/ebc` with the air's CVSD coded in `cvsd.c` (`ebc`; device.conf group media; the device closed by the
  last of its two threads: a free under a blocked write was a kernel oops and a reboot), or H4 type 3 on the PC
  (`hci`, `HASSMIC_SCO=hci`; `audio_sco_native()` 0 in audio_file.c).  Probes: `src/tools/scoprobe.c` (a
  synchronous link between two Echos on raw HCI), `ebcprobe.c` (`/dev/ebc`).  `make unit-cvsd`, `tests/fake_hfp.py`.
- **Bluetooth**: `ble.c`/`ble_crypto.c` talk raw HCI (`hci.h`) to the controller for the HA Bluetooth proxy (scan, GATT,
  Just Works pairing); Amazon's `btmanagerd` is stopped. A2DP shares the controller; scanning pauses while a phone plays,
  and for 1 s from a wake word round (`ble_quiet` in `main.c` `wake_heard`): the scan's share of the antenna costs
  10-30 % of Wi-Fi broadcasts (unicast is retried), i.e. the other Echos' claims.  After every reset the controller's
  address is checked against the idme's (`board.bt_mac`) and set with MediaTek's 0xfc1a where it differs: cannoli's
  MT7663 is left at 00:0c:43:29:50:01 by its driver, the same on every Echo Dot 5.
- **State files** (`statefile.c`, issue #46): every file hassmic keeps or hands to root is written through
  `statefile_open`/`_commit` or `statefile_write` (temp file, fsync, rename, fsync of the folder): `/data` is ext4,
  a rename without them can reach the flash before the data, and a hard reset left empty files (#36). New writers too.
- **Settings** (`settings.c`, `settings.h`): one table of the satellite's settings by name (type, range, group,
  exportable or not), used by `proto_esphome.c`, the settings page and exports. Its own ones persist in `state/config`
  (`name=value`; the positional `state/settings` of older versions is read once and moved); arbitration, Sendspin and
  the equalizer stay where their module keeps them. Loaded in `main()` whatever the protocol. Changes from elsewhere
  reach HA through `proto->settings_changed`. Features (`feature` in the table: arbitration, sound, whisper, plugins (the "Plugin" event entity), Wi-Fi
  motion, light sensor (Illuminance + auto level), wake word events (`wake_events`: the "Wake word" event entity,
  `core_wake_events`), request text (`request_text`: "Last heard"/"Last reply", cut to HA's 255 characters, `state_text`), Bluetooth proxy (no entities: `proto_esphome.c` `bt_proxy()` gates the device info's proxy fields and
  every proxy request), Bluetooth audio, Bluetooth speaker, Drop In, separate volumes): their entities are listed only while on (`proto_esphome.c` `listed()`,
  which also gates states); switching one closes the HA links (`proto->entities_changed`), HA re-lists on reconnect and
  deletes what is gone (registry included). HA always has: media player, mute, DND, wake sound, LEDs, EQ, firmware,
  Web UI address, Sendspin token (a secret: never on the page), Identify, Light ring (where root installed `ring.c`'s
  set), Button (event entity: presses and 1 s holds, `core_button_events`, from `buttons.c` `event` and `on_action`;
  types stable, automations use them). The rest is page-only; diagnostics too (`diag.c`).
- **Settings page** (`web.c`, `web/`): HTTP on 28931 (`-W`), files of `web/` gzip'd into the binary by `tools/embed.py`
  (`build/web_assets.c`). Login: the browser's X25519 key waits for the action button (`web_approve()` first in
  `on_action`; ring `authenticated_setup_mode`), approved keys in `state/web_clients`, Echo key `state/web_key`.
  One wait for the button at a time (`login`: W_LOGIN, W_ADB, W_PLUGIN, W_NOTE, W_ANYWAY; state 1 armed; web.c "what waits
  for the action button" has the invariants I1, I2, their argument and each boundary: keep that comment true): an
  armed wait ends by the press (W_NOTE: by the button HELD 1 s, `web_hold`, never a press, which goes on as without
  the wait: its asker can make a press happen, a Drop In rung there; main.c `on_held`, buttons.c `handler.held`,
  the volume pair held 1 to 2 s by the clock where `board.action_combo`; only a hold whose button-down came after
  the arming, `web_hold(down_ms)` against `login.at`; a hold that served is not the "Button" entity's: while a
  note's wait is armed `action_hold` does not fire),
  `LOGIN_MS`, or its owner's signed word (adb "off" from the browser that armed it, an
  install's cancel, a note's "keep", a revoke); no request replaces it ("busy"). After ANY wait ended (`wait_over`,
  every kind) no other unapproved key is armed before `strangers_from`: `QUIET_MS` (30 s) after the press, else after
  the wait's arming + `LOGIN_MS`; and `QUIET_MS` after every press that served nothing while a contested record
  stands or that time still runs (`web_approve`'s end: presses are a person's); only the key whose own login ran out
  may ask again at once (`again`, not after a `login.reserved` one, and not once a press served nothing since: such
  a press clears it).  Over a restart within a boot: `state/web_wait`
  (`hold_save`/`hold_load`: boot id, the monotonic time strangers are kept back to, the note waits' times; written
  when a wait is armed and when a press ends one, a contest's with `QUIET_MS` to spare; `HASSMIC_BOOT_ID`; no boot
  id readable: `-`, held by the clock while no further ahead than `LOGIN_MS + 2 QUIET_MS`, the note waits counted
  as armed at each start).  Never written under `lk`: `hold_save` (lk held) leaves it in `hold_out`, `hold_flush`
  (no lock; one writer, `hold_wr_lk`, taking the latest left) writes: the request that armed flushes before it
  answers and waits for a writer that took its wait before it (`hold_flush(1)`: every "waiting" answer), `web_approve`/`web_hold` wake the expiry thread (`hold_poke`); a new caller of `hold_save` does one of
  the two. A second unapproved key while a login waits: contested (state 4: not armed, ring off, the
  press goes to the assistant, both told; held until `QUIET_MS` after the later of the first one's arming +
  `LOGIN_MS` and the last ask by another key than the last, `login.last`). An approved browser's wait (`wait_arm`:
  adb, a plugin, a note, a withdrawn plugin to run anyway) is armed whenever nothing is (also in the quiet time, over a contested record, whose time
  goes into `strangers_from`); asked while a login is armed it is told busy and marks it (`login.reserved`), and
  its page asks again when `/api/state` `button.holds` leaves "login" (app.js `waitNoLogin`). The login page keeps
  asking while "contested", "busy" or "rate". No unsigned login while the button is Improv's business
  (`improv_holds`: set up, a phone connected, or its press's time, a switch or "provisioned" running), nor for
  `QUIET_MS` after Improv's wait went off, pressed or not (`web_quiet`, from main.c's `improv_attention(0)`, with
  improv.c's lock held, only leaves the time in an atomic and signals the expiry thread: it takes no lock and writes
  nothing; `phone_over`, first in `login_expire`, applies it under `lk` before anything looks at the waits; web.c
  never takes improv.c's lock, `improv_waits`/`improv_holds` read atomics); a
  login armed before Improv's wait ends unapproved with that wait: at the press, which goes to Improv (`web_approve`,
  `improv_waits`), or as "busy" when the wait goes off without one (`phone_over`); an approved browser's adb, plugin
  or note wait stays (web.c's section says why).  `state/web_wait` for times anyone can keep moving (a contest, the
  end of a phone's wait, and `QUIET_MS` from now for as long as `improv_holds` says the button is a phone's
  business, poked by `web_phone` from `improv_attention(1)`: a restart ends that wait and must not forget whoever
  is on his way; `hold_save` `moving`) is written with `QUIET_MS` to spare, one write per `QUIET_MS`, and again
  where the file says more than that spare time later than need be (a login ended by a phone's wait: with a LAN
  asker beside a Bluetooth device two more writes per `QUIET_MS` at the most). Unsigned asks counted
  (`ask_rate`: 40 per address, 120 in all per 10 s, 8 addresses; the armed key's own are told "waiting" and not
  counted; an ask over the count is refused "rate" and still turns another key's armed login contested, `contest`).
  `state/web_key`, `web_clients`, `web_button`, `web_wait` are opened as the store opens its files (`ps_fopen`: a
  FIFO there hung `web_start`); `web_button` is written again when a button's browser leaves `clients[]`, and
  by `load_button` at a start that finds a line naming no client (`button_stale` is memory's).
  `/api/login` answers `refused` with `why` (timeout, contested, busy, rate) and `retry_s`; `/api/adb`,
  `/api/plugins/install` and `/api/plugins/<id>/anyway` say `holds` (what the press serves), `/api/state` `button`. A new use of the button arms
  through `wait_arm` and ends through `wait_over`. Not covered, and said in docs/SECURITY.md: a login nobody asked
  for shows the same ring. `HASSMIC_LOGIN_MS=<wait>[:<quiet>]`, `HASSMIC_LOGIN_RATE=<address>:<all>` for tests (a
  test that logs in a second browser after any wait sets a short quiet, and one that does so after a restart with
  a wait behind it starts as another boot; `webclient.py` `login_with_button` asks
  again only while "busy", bounded, and fails on any other refusal).
  Requests signed (BLAKE2b-128 keyed with K over method, path, counter, body; K from X25519), counter per browser,
  each taken once within a window of the last 64 (`signed_ok`: the page's first calls cross on the way; every
  write's counter kept on the flash but a piece of an upload's, `piece_route`, by the whole route: no plugin id is a
  route word, `plugin_store.c` `ps_route_words`, the index's `ROUTE_WORDS` the same list); the
  answers too (`respond_s`: X-HM-Mac over "RESP\n" + counter + body), and the page refuses an unsigned one ('old').
  Vouchers: members of one arbitration network let in a browser approved on another member (`/api/vouch/nonce`, `issue`
  (signed, at the Echo it is approved on), `login`; MAC with `arb_web_key`, a key derived from K, over target key,
  browser key, one-time nonce and the voucher's name). The page does it by itself for members; a login page opens
  the other Echo's page (`#vouch=`), which asks, checks the asker is a member with that key, and postMessages it back.
  adb still needs that Echo's own button.
  Never send secrets: it is plain HTTP.  Log viewer: `GET /api/log/0|1` (signed) sends `boot.log` / `.1` (`HASSMIC_LOG`,
  tail of 2 MB), with the Sendspin pairing token blanked (`log_redact`; `setup.sh` greps it from the log over adb). The page's words (what each setting does, what a feature adds to HA) live in
  `web/app.js`, keyed by setting name: a new setting gets a line in `SUB` (what it is for, a few words: what a row shows),
  an entry in `HELP` (the full words, behind the row's ⓘ, `moreOf`), and a place in `LAYOUT` (the cards of a page; one not
  named comes last in a card of its own, so nothing is lost). Layout: `flow` (cards in as many columns as fit, placed
  once per column count, by height); a card is one list of rows, all alike (`settingRow`, `infoRow`): never rows side by
  side, scanning down a card is what makes a setting findable. Pages: `PAGES` (`groups`: which settings;
  `MOVED`: those that live elsewhere, a parent's children with it), all built at once and hidden, `route()` by
  `#/<page>[/<setting or element id>]` (`#vouch=` is the login's); `FINDABLE` is what the search finds beside settings.
  Echos page from `arb_status_json`
  (members with name, IP and whether they arbitrate from their signed beacons, others: in no network / younger / older); other Echos' pages are called cross-origin (CORS `*`,
  the signature counts), each approved once with its own button; "make like this Echo" posts this one's export.
  Name: one, made at start and stored nowhere (`main.c` `name_make`): `board.default_name` (the model: "Echo Dot 3")
  plus the Wi-Fi MAC's last 3 bytes ("Echo Dot 3 5695c4", node `echo-dot-3-5695c4`), as ESPHome's name_add_mac_suffix;
  people name the Echo in Home Assistant.  No rename anywhere (page "Name" row only shows it; the owner's note beside
  it, `state/note`, issue #153: web.c, in the unsigned hello, not exported); `-n` only for the PC
  (no MAC).  Old `state/name`/`state/node` are deleted at start, hassmic.conf `NAME` is ignored.  `state/restart`
  (its text says why: wake word engine), which root's `ota_watch` turns into `stop hassmic; start hassmic`.
  Identify (`POST /api/identify`, HA button `identify`, device class identify): `core_identify` sets `zzz_rainbow`
  (same file on every model) for 10 s, cleared by `volume_led_thread`, and queues `SND_IDENTIFY` (stock's
  `state_setup_discovery_beacon`) past the wake sound setting.
  Presets: `scripts/setup.sh --preset <export>` (or asked in `install_satellite`) pushes it to `state/preset`; hassmic
  applies it once at start (`settings_preset`, as an import) and renames it `preset.applied`. `web/crypto.js` (X25519, BLAKE2b; no `crypto.subtle` on plain HTTP) is checked
  against Python by `tests/unit/web_crypto_test.py` (in `make unit`, needs node). Compare & copy (`buildCompare`, `drawSync`,
  `applySettings`) and Copy models (`drawModels`) are one table, `web/matrix.js` `matrixTable`: a row per setting or model
  (the exports, ring rows included; other models' words from each Echo's `/api/state`, `d.state`), a column per Echo,
  any cell the source of its row. What is picked is a plan, row -> { from, skip }, by Echo, never by column;
  `hmmatrix` is pure (`tests/unit/web_matrix_test.py`, node): `groups` (Echos that agree on every row both have,
  an Echo joining only a group it agrees with all of), `pick`, `skip`, `column`, `changes` (all Apply reads).
  Past six columns a table narrows and grouping starts on (`grouping`: the stored choice wins); the other Echos are polled
  only while a page shows them (`othersShown`).
- **Amazon downloads** (`davs.c` + `dha.c`): the settings page's "Download from Amazon".  `dha.c` builds the device
  attestation token `/auth/register` needs, in the two shapes Amazon's MAPs make: a drvV1 JWT (biscuit, radar; RSA
  keymaster through `libacehal_dha.so`, dlopen'd, needs group drmrpc) or donut's drvV3 token around the dhav2
  certificate (field 0x203 of the same HAL; EC key in the TEE — `/dev/trustzone` is system:drmrpc and the cert file
  keystore:keystore, hence drmrpc,keystore in donut's DAEMON_GROUPS), whichever the HAL's key parses as; without the
  HAL (checkers) the binder service `amazon.security.dha` through `service call` (`afe_parcel.c`, common to every
  build now) and Java MAP's drvV1 text (version 1, checkers) or drvV3 (version 2, crown: x5c = the chain's first certificate, `/` escaped as org.json does; docs/re-crown.md; athena: the same from `fireosdhaservice`, Android 9's name for the service, `svc_names`).  `davs.c` does the code pair login (the code on the
  page, register polled with the token, tokens in `state/davs` 0600, never sent to the page), asks DAVS with the
  engine's own compatibility ids (`wake_attributes`), downloads and unpacks the artifact tar.gz (libz), and stages it
  through artifacts.c.  Works against the real Amazon (biscuit 2026-10-06, donut with its drvV3 token 2026-10-09,
  crown with its drvV3 2026-10-14, checkers with Java MAP's drvV1 2026-10-13 (whisper not allowlisted for its device type), all amazon.de; what it answers in docs/re-davs-login.md, which also has the drvV3 analysis).  The page offers
  every Amazon site with Alexa (davs.c's list), any of the account's region works.
- **Plugins** (`plugin.c`, `plugin_store.c`, `src/tools/plugrun.c`, `sdk/`, `plugins/`; design and state in
  `docs/plugins.md`, section 13 what is built; experimental, setting `plugins`, off, exported like the community switch since 2026-11-24: the owner's decision,
  every install still takes the Echo's button): programs beside hassmic, each
  a static binary in a sandbox.  `main.sh` starts hassmic as `plugrun -L -- runas ...` (`PLUGINS=0` in hassmic.conf:
  without): plugrun leaves a root launcher behind (init's child) and becomes runas; hassmic finds the socketpair as
  `HASSMIC_PLUGIN_FD` (`src/include/plugrun.h`: READY, SPAWN with three descriptors, SPAWNED/FAILED, KILL, EXITED,
  and from version 2 USAGE/USED: a plugin's CPU time, which only root can read on Android (hidepid); `cpu_watch`
  asks, `cpu_used` decides, never for the wake engine's; versions only add messages, and hassmic sends an older
  launcher none it does not know).
  The launcher has no policy: per plugin a user 6900 + slot, limits, the plugins' SELinux domain `hassmic_plugin`
  where the policy has it (`scripts/system/sepolicy-hassmic.rules`: types of an attribute of their own, never `domain`
  or `file_type`; only hassmic's descriptors and their sockets and pipes, its program of type `hassmic_plugin_file`,
  which `plugin.c` `program_label` gives it before each start, its threads; entered from su only; what it may do read
  back out of every model's compiled policy by `tools/sepolicy-check.sh`; not tried on an Echo), else hassmic's, else
  su, said in `READY` (`/api/plugins` `sandbox`; `HASSMIC_PLUGIN_DOMAIN` for plugrun-host), the seccomp filter (`sdk/filter.def` -> `tools/mkfilter.py` -> `build/plugfilter.h`; ~70 calls, the rest
  EPERM, above 386 ENOSYS; a static bionic program does not start under it: plugins are built with musl, never the
  NDK; `tkill` allowed as it is, for musl's `raise()`/`abort()`/`pthread_kill()`: no rule holds a thread id to the
  plugin's own, the slot's user does), exec by descriptor.  `plugin.c` is the broker: one thread polls the launcher and every plugin's control
  socket (SOCK_SEQPACKET; `sdk/messages.def` -> `tools/mkplugmsg.py` -> `sdk/plugmsg.h` and `sdk/PROTOCOL.md`'s
  reference and the test vectors `sdk/vectors/<MESSAGE>.vec`: change the def, run the tool, `make unit-plugmsg`
  holds them together, the vectors (a sample, `case empty|long|full`, `refuse` lines) to plugmsg.h's encoder and decoder both ways) and its stderr (the log,
  `plugin <id>: ...`, 50 lines a minute).  Messages are hostile input: form, state, capability and rate checked.
  Locks: `bl` only over work that cannot wait (the capture thread takes it in `wake_plugin`'s feed); order
  `core_lock`, the store's (`ps_lock`, recursive, held while a `struct ps_plugin` is used), `bl`; never `core_lock`
  or the flash under `bl`.  hassmic's own crashes (`fall_*`, `state/plugins/.fall`, `.falls`): the mark names the
  plugin whose message the broker's thread reads, until `core_begin` (before every call into the core: keep it so
  for new messages); only named marks count towards three; the named plugin is held back for the run, the wake
  engine's for `RETRY_MS`.  `plugin_store.c`: packages (`.hmp`: a tar of manifest.json, manifest.sig, plugin,
  LICENSE, listing.sig; EdDSA as otatool's, each over a text naming its purpose; ELF checks: static, a marked
  non-executable stack), `state/plugins/<id>/` (`pkg/`, one package a plugin, `author.pub` pinned, `accepted`: the
  digest of the manifest's accepted section as `digest`, `how`, `when` (`struct acc`); digest form 3, "hassmic plugin accepted 3", over name, author, description, source,
  capabilities, settings, forms 2 and 1 still counting as accepted (`accepted_is`); `config` 0600 with typed lines
  `<row>:<type>=<value>`, read only by a row of that type, rows the manifest no longer has dropped at a scan (`config_tidy`); `files/<list>/<item>/`), settings,
  uploaded files (checked by the plugin before they are kept: FILE_CHECK).  A package is in `pkg/` whole or
  not at all: `ps_stage_install` writes `pkg.new/` and renames it in (the old one `pkg.old/` in between;
  `pkg_settle` from `ps_scan` cleans up after an install that did not end; `accepted` and `enabled` after it;
  `HASSMIC_PLUGIN_FAIL_PACKAGE` for tests), and plugin.c `start` runs no program whose BLAKE2b is not the
  manifest's `digest` (`program_is`, through the descriptor it hands the launcher; `program_open`: digest,
  descriptor, capabilities and limits (`struct run`) under one hold of `ps_lock`, on a mismatch once more after a
  rescan, since an install's caller scans only after the lock was let go; `start`'s own first look only decides
  whether to start).  A first install that ended before its package arrived leaves a folder
  `ps_scan` removes (`never_arrived`: a lone `pkg.new/` and at most `author.pub`, looked at before `pkg_settle`).  Every file the store reads from
  `state/` is opened without waiting and only if regular (`open_reg`, `ps_fopen`: a FIFO there hung `ps_scan`).
  Versioned scopes (docs/plugins.md 3.1, sdk/PROTOCOL.md "Versions"; 2026-11-23, in place of 0.x's exact
  minor and everything that existed because a minor broke): `core` and one scope per capability, each a version
  (all 1 so far); a scope's version N+1 only adds (a message, a field appended, an enum value, a constant, a
  manifest key), never changes or removes.  `sdk/messages.def` gives each `scope`, each message's scope(s)
  (`net.udp|net.discover`: in each; never grows), `@N` for what a later version brings, a field's `enum:E` (its
  values held to what the peer speaks), `keys CONTEXT SCOPE[@N] KEY ...` (the manifest's keys where they stand),
  and for every version above 1 `version SCOPE N widens|within "..."` (required, no default: whether it lets a
  plugin do more than its capability's install line; `pm_widens`, `pm_version_doc`); `tools/mkplugmsg.py` holds the
  rules (since nondecreasing, `@N` within the declared version, HELLO/WELCOME/REFUSED and multi-scope messages
  fixed, the greeting's lists of scopes 255 for good (`PM_SCOPES_MAX`; at most 255 scopes, versions u16), every
  scope a capability's, `CAP_*` values each its own scope's from version 1 and under `CAPS_MAX` 64, the bits of
  `pm_caps_t`/`PM_CAPBIT()`, hassmic's own set, never on the wire: WELCOME has no capability bits, what was granted
  is the scopes spoken) and `sdk/scopes.lock` (each (scope, version)'s layout, enum fields, values, caps, keys and
  mark as recorded; `--check` fails where a recorded one would change, one is missing or a version is unrecorded;
  `--record` adds new ones only; every version any release tag's lock had (`v*`: main's betas and release's
  releases, `released_locks`, `git cat-file --batch`) must be exactly that, in the lock and the def wherever either
  has it (`released_why`: set equality), missing only where no tag HEAD descends from had it (`git merge-base
  --is-ancestor`), and no two tags may differ on one; `--released` (CI) fails without tags, in a shallow clone
  (`--is-shallow-repository`: merge-base says "no" past the boundary) and on a tag whose lock is no file (its
  entry's mode in the tag's `sdk` tree: a directory, a symbolic link, a submodule) or has a line that is no lock's
  (every `cat-file` answer passed over whole by its size), make unit-plugmsg says it skipped (shallow, no tags) or
  names that tag and holds the rest; the release job runs `--released` again, tags fetched anew (`--prune
  --prune-tags`: one deleted on GitHub gone here too), right before `gh release create` (main and release build at once; the way out of two releases that disagree: DEVELOPMENT.md "CI and
  releases"); the lock's head is `LOCK_HEAD` word for word (`--check` fails, a plain run writes it; a note of its
  own below a blank line); re-recorded once, 2026-11-23, before any release had a lock): change the def, run the tool with `--record`
  for a new version, never edit the lock by hand but to delete a version no release carried;
  `tests/unit/plugmsg_scopes/` the rules each broken, `tests/unit/plugmsg_scopes_test.py` the releases in a throwaway
  repository with both branches tagged.  A value of a version above 1 names its fallback (`B=2@2>A`, a value of an
  earlier version, recorded; refused without), `pm_<enum>_for` and `pm_fit` (in place, a packet's every such field)
  map it to what the peer speaks: `psend` and the C client's `put` run `pm_fit` on every packet, never dropping one
  for a value (the vectors' `fit` lines; the Rust client's `fit` and `*_LATER` tables), but for a value that names
  none (`C=3@2>-`: no earlier value is right; `messages.def`'s head has the rule: the fallback is what an older peer
  should act on, never a failure to a success, a command to another, an event to one that did not happen):
  `pm_<enum>_for` `PM_NOT_SENT`, `pm_fit` 1, the packet not sent to that peer, without a word (`psend` 0 and
  `p->unsent`, said once a start, never "hassmic's error"; the vectors' `unsent` lines).  `>-` only on an enum
  that messages marked `drop` alone hold (`msg ... SCOPE drop "..."`, recorded in the lock, `pm_droppable`; never
  the greeting, OK or ERR): events whose whole point is the value, `BUTTON` alone; never an answer, an end, a state
  or a reason/qualifier (mkplugmsg refuses; a new value there names its fallback or is a new message).  So `psend`'s
  0 for a packet left unsent is only ever `BUTTON`'s (any other is "hassmic's error", -1: `action_send`,
  `entity_set`, the `told_*` never take it for sent), and `plugin_button` asks `pnot_for` before `bucket_take` (a
  key a plugin does not hear spends none of its budget).  The clients refuse a request not of what is spoken
  (`pm_check` -1, `pm_fit` 1) with their own code `0xffff` (`HM_ERR_NOT_SPOKEN`, Rust `ERR_NOT_SPOKEN`; ERR's
  65535 refused by mkplugmsg), apart from `HM_ERR_BAD`.  `pm_fit` rewrites in place: `psend`/`put()` take a non-const `struct pm_w`,
  mark it `fitted` and refuse it again (encode a packet per peer).  A multi-scope message holds
  core's enums and `CAP` only.  Decoders are strict: bytes after the last field
  are malformed, and so is an enum's value the peer does not speak (`pm_<enum>_ok`, in `pm_check`; `pm_cap_ok`: any
  of this build's capabilities); `psend` and the C client's `put` hold every packet to `pm_check` before it goes,
  and the C client's `answer()` every answer it takes (`PM_CONST_SPOKEN` for constants).  A version's words are
  `PM_VERSION_DOC_MAX` 300 bytes at most: a raise lists every version between (`requires_wider`, last: after the
  capabilities' lines and the settings' rows; versions within one after another folded into one line; the last
  `PS_CHANGES_ASK` of `PS_CHANGES` only for lines that ask; what has no place counted, `more`/`more_shown`, the
  check's `changes_more`/`shown_more`, "And N more." by `hmbrowse.changes`), a first install
  and a capability asked for anew every widening one up to the one required (`ps_widens_json`, the check's and the
  card's `"widens"`, `web/plugins.js` `plgCapList`/`hmbrowse.capLines`; `widening_lines`).  The manifest's keys by version: `plugin_store.c` `keys_versioned` (`pm_key`;
  a key of a later version than required refuses, one unknown refuses where strict: capabilities' entries), the
  index's `KEYS` the same, `tests/unit/scopes_demo.py` the def as a later hassmic's (net.lan 2 widens, 3 within, 4
  widens; ha.entities 2 within; call 2 within with `HANGUP_ELSEWHERE`/`RESULT_ELSEWHERE` and their fallbacks;
  button 2 `BUTTON_KEY_MIC>-` and audio.music 2 `MUSIC_BUFFERING>PLAYING`; state to 35, 2 to 4 within then by turns;
  `build/scopes_demo/plugmsg.h`), which `make unit-plugin_store`, `unit-plugin_broker` and `unit-hmplugin` build
  their tests against a second time (`-DSCOPES_DEMO`; the C client copied beside that header) and the index's `make
  check` its cross-check.  A manifest's `requires`
  (`{"core": 1, "<capability>": n, ...}`: exactly core and each capability asked for, `requires_parse`, read before
  anything a version may bring) is not in the accepted section; a version this build lacks: refused at install and
  marked at scan "needs a newer hassmic: <scope> <n>" (`m.newer`: name and version kept, the card says so, never
  started, the self test does not wait for it: a fall back to an older build).  hassmic speaks exactly the
  manifest's versions: WELCOME names them, `plugin.c` `plug.ver[]`, `psend` sends nothing of a later version,
  `message()` answers one `ERR_UNKNOWN`; HELLO carries the client's whole table (a scope required and not offered,
  or offered lower: `REFUSED_SCOPES`).  `ps_scope_have` is this build's table; `HASSMIC_PLUGIN_SCOPES=<scope>:<v>,...`
  (`ps_scopes_env`; 0 = lacks it, never higher) makes the PC's build an older one, as the index's C cross-check
  does.  "Never back" over the one package (`pkg_version`), for `web_plugin_push` too.  An update goes without the button only as what was accepted or less
  (`wider`, docs/plugins.md 3.3: capabilities and settings value for value; a capability, a row, an entry of a list
  of grants gone; `storage`, `files`, `max_kb`, `max_items` smaller); anything else, understood or not, asks.  A new
  capability with parameters gets its own rule there or none; `memory_mb`/`threads` are shown (`PS_LIMIT`); a raised
  `requires` (`requires_wider`) into a widening version is `PS_MORE` (asks), within `PS_SPEAKS` (shown, web.c kind
  "speaks"), every raise listed; an unreadable installed manifest of the same section: held against version 1.
  `ps_sealed`: an unread, newer or package-less plugin counts as sealed; `ps_scan`'s newer card keeps `requires`.
  `pi_version.req` is u16 as written (never clamped).  A
  manifest `ps_scan` cannot read is listed as a plugin that cannot run (`refused`), never skipped.  Community plugins (docs/plugins.md 2.7 "As built"; setting `plugins_community`, off by default, no entity): whatever
  the index does not list as official (a community listing, a file) is refused by `ps_stage_check` and marked
  `community_off` by `ps_scan` (refused: stopped and kept; no wake engine, `ps_engine_refused`, which asks the switch
  as it is now, `ps_community_keeps_off`, never the scan's mark alone: that lags it until the broker rescans) while
  it is off.  An
  ordinary setting of the page (no press: each such plugin's install takes the button anyway; the maintainer's
  decision, docs/plugins.md 12's eighth round): on and off by `/api/set` from an approved browser, exported, copied,
  taken from an import and a preset; `plugin.c` `plugin_community` sets the store's flag
  and leaves the rescan to the broker's thread (`community_due`: the caller holds `core_lock`); `settings.c` applies it
  before the other lines of one request but `plugins`, which goes first of all (`switches()`, `rank()`: plugins, the
  switches, the rest; an export lists `plugins`, `plugins_community`, then the table's rows, `wake_engine` first); `main.c` sets it before
  the first scan (`settings_peek`, as this start will have it), off unless set; `settings_load` only notes it and
  `plugins` (`loading`), `settings_preset` tells both once at its end.  The page's dialog before "on", and the install
  dialog of every plugin that is not official (`plgDialog`, `hmwall.of`: the step an import, a preset or a copy does
  not skip), have the warning in full where `sandbox` is "hassmic" or "su", worded by `install_method` (`/api/plugins`,
  and the installing Echo's check, `check_json` `sandbox`/`install_method`; the Makefile's `INSTALL_METHOD` from
  device.conf `INSTALL`; `HASSMIC_INSTALL` for tests); tests that install files write
  `plugins_community=on` into their `state/config` (`tests/fake_plugins_community.py` has the switch).
  Install: the page uploads, the Echo checks, the action button accepts
  (`web.c` `W_PLUGIN`, `plugins_api`; `web/plugins.js`).  An update needs the button only where it is wider than
  what was accepted (`wider`, docs/plugins.md 3.3: a new capability, a larger number, a list entry the accepted
  manifest lacks, a new secret/host/text row or such a row's label, name, author, `wake` dropped; the check's
  `changes` as "New/More/Changed" lines), held against the installed manifest whose section `accepted` names; the
  check also gives the author key's fingerprint (`ps_fingerprint`, as `plugcheck.py show`), named for a sideload.
  Phase 2 (2026-11-21): capabilities `files`, `wake`, `storage`, `log.read`, `net.server`; phase 4 (same day) adds
  `state`, `net.udp`, `net.discover`, `audio.music`, `audio.volume`, `ha.entities`, `ha.actions` (below); phase 5
  `call`; phase 6 `request.audio`, `request.text` (below); then `net.lan`, `net.internet`, `log.read.full` (below),
  `button` and an entity's `"page"` (below): every capability of 3.3's table, each a scope at version 1 (a name
  the build does not know refuses the manifest; the index's `SCOPES` the same table).
  Places (`plugin.c` "Places"): `RUN_MAX` = the launcher's `PLUGRUN_SLOTS` (8, users 6900+slot, `lockdown.sh`
  `PLUGIN_SLOTS`), and the running ones' `memory_mb` (the launcher's RLIMIT_AS, 32 when left out) within `run_mem`
  (`RUN_MEM_MB` 128, a quarter of `/proc/meminfo`'s MemTotal rounded up to the next 128 MB where less, `run_mem_of`):
  `places()` counts processes, ones stopping and the wake engine's whenever it is wanted and not off, running or not
  (`engine_waits`: between restarts, held back, waiting for `room`); one without a place waits (`waits`,
  `wait_since`, card state "waiting", its why), `timers()` starts the oldest that fits, and the spawn asks the places
  again (two starts at once); a wanted plugin left without a launcher slot is asked again in 250 ms; the wake
  engine's plugin evicts the newest others until it fits, and they wait (a stop for its place is no crash:
  `exited`'s `!p->waits`).  Sized by `RUN_MAX`: `LANES` `RUN_MAX + 2` (`LANE()` each; `core.h` `MUSIC_SOURCES` 14),
  `RQ_MAX`; `plugin_net.c` `QUEUE` 16, `WORKERS` 8.  `PLUGIN_TEST_BETWEEN(id)` (the harness's `h_between`): what
  another thread does while `start()` has `bl` let go.
  Storage (`ps_store_*`, `state/plugins/<id>/storage/<key>`, the accepted kB and 128 keys; `plugin.c`
  `store_message`: a bucket of 10 writes, one back a minute, `HASSMIC_PLUGIN_STORE=<bucket>:<ms>` for tests), the log
  (`logread.c`: the page's tail and the plugins' filter and cursor; a line that carries what was said or answered, a
  name a person gave, a media address, a code, the home's country or Home Assistant's words is written with
  `log_private()`, a wake plugin's with `log_sealed()` (`plugin.c` `say`/`seal_note`): one buffer, control characters
  spaced, one write; never `fprintf(stderr, ...)` with such a value, a new line included; a mark anywhere drops a
  line, clock.c's reader carries it onto a cut line's next piece and never cuts one in two (`mark_begun`);
  `tests/unit/logwriters_test.c` and `fake_plugins_net.py`'s sentinels, each set followed by a control line;
  `log.read.full` (official only, `combination`; `ps_scan` holds it to a listing under `pi_least_index()` as to none,
  the tier shown kept) the page's view, `logread_full`: every line but a sealed one or a
  wake plugin's, the token blanked; `messages.def`'s capability column may name several, `a|b`, `pm_caps` a mask; a cursor
  read only from a line's start, else the next one's with a gap, not MAC'd; `plugin.c` `log_pump`, sends only what
  fits, nothing while a CONNECT waits, one LOG at a time: it has an id, the next after the client's OK of it,
  `log_wait`; no OK in `LOG_ANSWER_MS` 10 s: said once and on the card, `log_deaf`, `/api/plugins` `log_unanswered`), the
  first sockets (`net.server` entries `{setting: host row, proto, ports | "setting:<int row>"}`, neither row with a
  default, `plugin_store.c` `net_parse`; `net.lan`/`net.internet` entries `{proto, ports | "any"}`,
  `reach_parse`, `struct ps_reach`, lines by `reach_line`; `CONNECT` only after READY -> `plugin.c` `connect_message`
  by `kind` (net.server: what the owner stored, `ps_config_owner`; net.lan: an address `a.b.c.d` the plugin names,
  `ip_text` + `pnet_local`, never a name, no lookup, never DNS's 53/853 (`PS_PORT_RELAY`, also `net.udp`'s
  `udp_dest_ok` and `net.discover`'s port: the router's forwarder asks the internet; the index's `DNS_PORTS`);
  net.internet: a name or a public address, `ps_host_name_ok`, never `pnet_local` (the worker again after the lookup);
  a listener's or caller's worker lines private, `pnet_req.mark`; the last 30 in a minute; a refusal said once in `REFUSAL_MS` 10 s, the rest counted,
  `refusal()`: a new per-message line a plugin can cause goes through it) -> `plugin_net.c`'s workers (one per place,
  each plugin's requests one at a time, `working[]`; 10 s a request in all, the lookup in a thread of its own waited
  for until then and counted until it ends: `LOOKUPS_EACH` 2 a plugin, 16 for names plugins chose, `LOOKUPS_OWNER` 4
  more for net.server's, `BUSY` past them; `HASSMIC_PLUGIN_LOOKUP_DELAY`,
  `HASSMIC_PLUGIN_NET_MS` for tests; a dead start's dropped, `pnet_live`, by `struct plug` `gen`, new at every spawn
  in a run): only the owner's host, never loopback, the Echo's own, an address not routed unicast
  out of an interface other than lo (`RTM_GETROUTE`), 0/8, multicast, broadcast, port 5555; a private address a plain
  socket, any other `net_socket`; handed over, no copy kept; a server's row changed: `plugin_restart`), after STOP
  STORE_PUT/DEL done and answered, any other request answered FAILED (a worker's CONNECT too: never its socket); a
  process's storage and files its plugin's until it is gone (an update's old process too: its stop's cursor), none
  once removed (`revoked`, read under `ps_lock`): `store_mine`; the seal's wipe waits on `mdigest`
  (`plugin_old_running`); hassmic ending in order (`SIGTERM`, `main.c` `on_term`/`term_thread`; `main()`'s end):
  `plugin_shutdown` sends STOP to all and waits 2 s at most, nothing starts after it; root's `main.sh` `hm_term`
  sends `SIGTERM` before its own restarts and its reboot (init's stop and `ctl.restart` are `SIGKILL` to the
  process group: no stop callback then); the development flag (`state/plugins/<id>/develop`, SPAWN's `d`,
  launcher version 3: `EPERM` refusals as `SECCOMP_RET_TRAP`, the SDK's `SIGSYS` handler names the call; `plugrun -T
  -D`; not under launcher 2: `develop_flag`), storage in the copy between Echos (row `store <id>`, only when picked;
  sealed both ways, web.c `seal2`: the answer under a nonce of the Echo's own in front of it, "hassmic sealed 3
  <hex>", purpose "hassmic seal back 3", the GET taken once over restarts; its first line `hassmic storage 1
  wake=<0|1>` (`ps_sealed`: `wake`, or a wipe still due): never a wake plugin's into one without `wake`, by the page's
  word (not MAC'd: decided); `plugin_storage_replace`, `storage_settle` after a cut swap).
  Phase 4 (docs/plugins.md 13 "Phase 4 on hassmic's side"): music sources are one table (`core.h` `struct
  music_source`, `music_register`, `core_music(n, on)`: newest wins, the others' `pause`; button, stop word, volume,
  `unyield` through it; Sendspin and A2DP registered, `a2dp_music_join` before `sendspin_start`); a plugin is one of
  `LANES` sources (`lane_*` trampolines, `lane_of[]`, registered once).  `MUSIC_OPEN` -> a SOCK_STREAM pair, its
  reader `mstream_thread` (40 ms reads at playback speed, stereo, `music_duck`, `musiclight_feed`, `MUSIC_POS` every
  200 ms) onto the backend's own plugin stream (`audio.h` `plug_open/_write/_queued_us/_close`: one at a time,
  `plug_mu`; a newer plugin's open ends the older one's stream); `MUSIC_STATE` is what the button, "stop" and HA's
  media player go by (`plugin_music_state`, `plugin_music_ha`; `music_tell` sends `MUSIC_CMD` until state and wish
  agree).  `STATE`/`VOLUME`/`NET` (`tell_echo` at READY; main.c `tell_plugins` -> `plugin_core_state`).  `net.udp`/
  `net.discover`: `udp_message` binds ports of the plugin's own (`ps_udp_ports`, `state/plugins/<id>/ports`,
  28940–28999), `private_only` (BPF on the source, `SO_LOCK_FILTER`), `UDP_PEER` (one a second, `r_peer`)/`SENDTO`
  to private unicast the kernel routes out (`udp_dest_ok` -> `pnet_address_ok`'s `route_out`), `BROADCAST` to each
  network's directed broadcast (`net_look`) or the manifest's group, one a second, `IP_MULTICAST_LOOP` 0; the
  search's port never 2330 or 28928–28999 (`PS_DISCOVER_*`: a broadcast comes back to the Echo's own sockets; and
  `lockdown.sh` `hassmic_self`); `PROPOSE` kept in memory per row (`prop[]`), written only by the page (`POST
  /api/plugins/<id>/propose`, `plugin_propose_take`, which asks PROPOSE's rules again).  The stream while not heard
  (paused, `myield` `MY_OTHER`/`MY_HELD`) read and dropped at its rate, never faster; every pause hassmic sends holds
  whether the plugin obeys (`MY_HELD` until RESUME or a new start); the lanes' functions only mark `mdue`, the
  broker's `music_due` tells the core and HA (they run inside `core_music`); a stream's end tells the core while the
  entry still holds the stream and its lane (`HASSMIC_PLUGIN_MSTREAM_GAP_MS` widens that for the unit test);
  `VOLUME_SET` 5/s and 20/min, applied from the latest every `VOL_APPLY_MS` (`vol_flush`), one that waits dropped when
  the volume is set elsewhere (`plugin_core_state`, `vol_own`).  The stream's thread: a paused stream dropped at a
  pace kept as a running frame total; a read under `READ_MIN_MS` (5 ms) of audio has the next wait that long;
  it tells the core off by its lane's `src`, and `start()` takes no `P_FREE` entry with `.ms`.  `ha.entities`/
  `ha.actions` (`entities_parse`, `actions_parse`: labels unique too): keys given once from a rising counter and kept
  in `state/plugins/.hakeys` (`plugin_store.c` `hakeys_assign` at every scan; one not written down is not used),
  never another's, above hassmic's own, starting at a random place when `.hakeys` is missing (a factory reset: Home
  Assistant took a key used again for a rename); names "Plugin <len(id)> <id>: <label>" (`ha_name`: Home Assistant
  keeps an entity by type and name and makes its entity id of `slugify("<area> <device> <name>")`, removing the entry of one
  no longer listed, so the next alike took its id: no own name slugifies as `plugin_<digits>...`,
  `tests/unit/ha_names_test.py` with HA's own slugify from `build/ha-venv`; the id's length makes two plugins' ids
  two; no dedupe between plugins needed); object ids `p<len(id)>_<id, - as _>_<name>` (`ha_object_id`: unambiguous services); listed while the
  plugin is enabled and runnable and plugins can run (`runnable()`: feature, launcher, sandbox; `ha_listed`);
  whatever changes the list closes the links (`ha_relist`: a digest compared at every change and every 5 s ->
  `core_plugin_entities_changed`); states checked against the manifest (`ent_value_ok`, a number's limits as the
  float32 HA has them), every plugin text strict UTF-8 (`ps_utf8_char`; `shown_text` cuts, `ps_utf8_mend` for the
  log and the page); `ENTITY_SET`/`ACTION` only over the keyed link (`proto_esphome.c` `plugin_command`), an
  action's arguments typed by the manifest (`plugin_action`'s `ints`), the page's buttons (`page: true`, `POST
  /api/plugins/<id>/action`).  Every per-plugin rate a `bucket_take` (`bucket_rate` for a minute's).  PC: `HASSMIC_PLUG_MUSIC` (the plugin stream into a file), `HASSMIC_FAKE_SOURCE`
  (a stand-in music source), `make unit-plugin_broker`'s phase-4 block, `tests/fake_plugins_music.py`.
  Phase 5 (docs/plugins.md 13 "Phase 5 on hassmic's side"): `call`.  One call on the Echo (call.c's claim,
  "Call audio"), a plugin's `CALL_PLUGIN` + its entry.  `plugin.c` "calls": `pc` (C_RINGING, C_ANSWERED, C_CALLING,
  C_LIVE), `call_message` (form, P_RUN, `call` by `pm_cap`, `r_call` 5/s), `call_over` (every end: `CALL_HANGUP` to
  the plugin unless it said so, `media_stop`, a ring that ends unanswered counted in `rings[]`), `call_timers`
  (`RING_MS` 30 s, `LIVE_WAIT_MS` 10 s after a person's answer, `CALLING_MS` 2 min; `HASSMIC_PLUGIN_CALL_TIMES`
  for tests), `RINGS_MAX` 3 in `RINGS_WINDOW` 10 min -> `ERR_LIMIT`.  A token (`token_new`, 32 hex from
  /dev/urandom, `TOKENS` 4 per plugin, `TOKEN_MS` 10 s, spent at its first use) goes into `ACTION` only for an action
  marked `"calls"` (`actions_parse`: only with `call`), and both ways an action arrives are a person's (keyed link,
  approved browser); `CALL_OUT` needs one, `CALL_LIVE` for a ringing call needs `CALL_ANSWER` sent (the button, the
  screen) or one.  `CALL_LIVE` -> `call_begin(CALL_PLUGIN + i, CALL_CODEC_PCM, call_send, tag)`, a SOCK_SEQPACKET
  pair (644-byte packets: u32 number, 640 bytes), hassmic's end under `cml` (a leaf: `call_send` under call.c's
  `tx_mu`, one MSG_DONTWAIT send), read by `media_reader` (100/s at most) into `call_rx`; the plugin's end with the
  OK.  The core hears every state from the `teller` thread (bl let go: `core_call` takes core_lock, and the button
  and a "stop" call in from under it), which lets the claim go after "idle".  `stop()` and `exited()` end the
  plugin's call.  The card's `call` (`plugin_json`), `web/plugins.js` "Calls".  `make unit-plugin_broker`'s call
  block, `unit-call`, `tests/fake_plugins_call.py`.
  Phase 6 (docs/plugins.md 13 "Phase 6 on hassmic's side"): a request's copy.  `main.c` `mic_out` (in
  `lead_in` and `send_out`, after `proto->audio`) -> `plugin_request_audio` while `req_open`: set where listening
  mode is set for a request (`pipeline_start(from, source)`; with `-w remote` only `core_remote_wake`), cleared in
  `mic_stopped`; why it ends `req_ending` (first word wins; none: Home Assistant), a mute ends it (`mute_update`).
  Muted, no microphone audio leaves the Echo: `mics_off` drops a queued follow-up (`barge_in`), stops `-w remote`'s
  stream and the runs beside a reply, `mics_on` resumes; `core_restart_after`, `core_pipeline_finish`,
  `core_remote_wake`, `wake_listen`, `pipeline_start`, `req_begin` all refuse while `core_muted()`.  REQ_TEXT only
  for a request (`req_texts`; `plugin_request_none` where `-w remote`'s stream starts); Wyoming's `detection` is
  `core_remote_wake` (under `-D` during a reply: the reply left to the client, no sound, the request begun, `wake_cut`
  so its audio-stop ends nothing).  The broker's lines about a listener private (`say`, `SEAL_PRIVATE`; `seal_note`
  keeps one place per entry of `plugs[]`, set by the start that spawns there: never a table that fills up).
  `plugin.c` "a request's copy": per request and plugin (P_RUN, `request.audio`) a SOCK_SEQPACKET pair made under `bl`
  by the holder of `core_lock` (`rq_fd`, valid only while `rq_open`), each packet u32 run + u32 offset + the block,
  one MSG_DONTWAIT send (`REQ_SNDBUF` ~6 s: a start's lead-in and replay, 4.8 s at most, come at once; full:
  dropped and counted), closed at the end (the plugin reads its end); `REQ_START` (the
  plugin's end with it), `REQ_END`, `REQ_TEXT` queued (`rq_push`, `RQ_MAX` 32) for the broker (`rq_drain`, woken by
  the `bwake` pipe; also from `timers()`).  `REQ_TEXT` (`request.text`) from `proto_esphome.c` (STT end after
  `core_transcript`, TTS start) and `proto_wyoming.c` (the transcript).  A listener's (and a `call` plugin's:
  `plugin.c` `PRIVATE_CAPS`) stderr lines are `LOG_PRIVATE_MARK`.  The second press: `plugin_store.c` `ps_second` (request.audio + any net.*): wherever the
  button is asked for, `ps_stage_install` writes only with `PS_PRESSED2` or `PS_VOUCHED_PRESSED`, else
  `PS_NEEDS_SECOND`; `web.c` slot state `PL_SECOND` (`pl.first`), armed again only by the screen's
  `/api/plugins/install` (`pl_press.press`); page `plgSecond`.  The partner: `ps_engine_set` (from
  `plugin_wake_engine`), `ps_partner` (install, and `start`: the listener is not started), `ps_engine_refused`
  (`core_wake_engine`); "community" = not official, or withdrawn (`vouched`).  The card's `requests`, `second`, the page's `hears`.
  `make unit-plugin_broker`'s request block, `unit-plugin_store` (second press, wider), `unit-hmplugin`,
  `tests/fake_plugins_request.py`, `tests/fake_plugins_sync.py` (the voucher's second press).
  `button` and rows (docs/plugins.md 3.3 "As built (2026-11-22)", on `button`): `button` (parameterless): `BUTTON { key, how }` (message 120)
  from `main.c` `button_event` right after Home Assistant's "Button" is told (`plugin.c` `plugin_button`, no lock
  held, P_RUN plugins with the capability, `BUTTON_PER_SEC` 5 by `bucket_take`, the rest counted apart, `button_n`, never `refusal` nor `dropped`): never call it
  from elsewhere, the exclusions (a press that served a wait, a hold that served a note) hold by being that one call;
  not with `wake` (`combination`'s list).  An entity of `ha.entities` marked `"page": true` (true/false; not on a
  button or an event; `entities_parse`, ha.entities 1's; the index's `manifest_check` the same) is a read-only row on the plugin's card: `plugin_json`
  `rows` (name, kind, label, unit, decimals, value) only while it runs and `ha_entity_out` (its key kept: no row for
  a "nokey" entity), the value not again in `entities`; `web/plugins.js` `plgRows`, `hmrows.text`, `hmrows.listed`
  (pure, `tests/unit/web_rows_test.py`); `page` alone is not
  wider (`wider` `X_ENTS`/`X_ENT`, `x_skip`).  `tests/fake_plugins_button.py`, `tests/plugins/testbutton.c`.
  `PS_CAP_LINE`: a capability's line
  at its longest, every buffer of one sized by it.  The manifest is read as written
  (`plugin_store.c` `form`, `whole`, `tstr`, `jlist`, on json.c's tokens; json.c stays lenient for mass.c): one
  object from the first byte, no NUL, strict numbers, no escaped control characters or lone surrogates, no key twice
  or escaped, whole numbers within 32 bits, lists where lists are meant, a default of its row's kind and range,
  parameterless capabilities `true`; a new field gets a reader of its kind, never `json_int`/`json_bool`.
  `hassmic-plugins`' `tools/index.py` `manifest_check` mirrors these rules and must never take what the Echo
  refuses (its `make check` holds both to each other: change one, change the other).  3.3's combinations
  (`combination`, whose signature that cross-check uses) and `requires` are asked at install and again by
  `ps_scan` (`struct ps_plugin` `refused`): `plugin.c` `start` starts none that has a reason, the card shows it.
  While an install waits for the button the ring shows stock's `wait` (ring slot `plugin`, the last; `web.h`
  `attention(2)`, `main.c` `attention` keeps what the page and a phone's setup each asked for and shows the highest), not the login's.  Home Assistant: the event
  entity "Plugin" (`KEY_PLUGIN`, `core_plugin_events`: installed, updated, removed, synced; types are a promise),
  listed while `plugins` is on, which is a feature for it; `core_plugin_event` (no lock held: it takes `core_lock`)
  from `web.c` after an install or a removal, `PLUGIN_SYNCED` for the sync to fire.
  Watchdog (2.6; each ends the plugin, counted as a crash): three pings unanswered, counted on the broker's own
  clock (`btime`: a round of its loop counts `STALL_MS` at most, so a broker that waited on `core_lock` kills
  nobody); the control socket's queue taking nothing for 5 s (`full_since`, probed with a PING of id 0); an
  ordinary plugin over a core's worth of CPU in a minute (`cpu_watch`, `/proc/<pid>/stat` read without `bl`; never
  the wake engine's; `HASSMIC_PLUGIN_CPU=<percent>:<ms>` for tests).  hassmic's own crashes (`fall_*`): once a
  plugin was started, a fatal signal's handler writes `state/plugins/.fall` ("fall <id>" when the broker's thread
  was in that plugin's message, else "fall -"; one `write`, the only state file not through statefile.c) and hands
  the signal on; the next start counts it in `.falls` (forgotten after 24 h up, not at 3), holds the named plugin
  back for the run, at 3 all but the wake engine's until `plugin_resume` (`POST /api/plugins/resume`, the page's
  "Switch plugins on again", or the feature switched on).  Dot names: "fall" would pass for a plugin's id.
  A plugin as the wake word engine (`wake.c` `wake_plugin`, a model named `plugin:<id>/<word>`; setting
  `wake_engine` = `plugin:<id>`, its choices made at run time, `core_wake_engines`): WAKE_OPEN is only asked, the
  answer comes on the broker's thread; the stream is one SOCK_SEQPACKET pair (u64 position + the block, at most
  `PM_WAKE_SAMPLES_MAX` 4096 samples a packet (`wp_feed` cuts a longer one), never
  waited for, not while muted); WAKE_HEARD is held to the stream's clock (end at most 300 ms old, 0.3 to 1.5 s, one
  a second, 20 in 10 min = broken).  `main.c`: `wake_engine` is what the owner chose, `wake_listed` whose words
  are listed: the plugin missing (not installed, nothing may run) -> Amazon's words and wake word; installed and
  down -> its words stay listed, none is loaded, never Amazon's (the maintainer's rule); `core_wake_problem` 0/1/2
  for the page's warning on every page and the problem sensor "Wake word engine"; `core_wake_plugin_changed`
  (plugin.c, no lock held) rescans (`plugin_installed` before the start too); `settings_set` reads the engine's
  choices anew, the store scanned first, before it refuses a `plugin:<id>` (`core_wake_engines_fresh`; the test's
  `HASSMIC_PLUGIN_INSTALLED_DELAY=<ms>:<file>`).  The wake plugin starts in `main()` before the engine opens; the others once the
  self test passed (every start where the push port runs it, an Echo's: the others wait for it, also
  where it passes late; no audio within its 30 s: not in that run) or, without the push port (`-o 0`), 10 s after
  start.  No plugin lives in this tree, official ones neither
  (docs/plugins.md 5.2): microWakeWord is the repository `hassmic-plugin-microwakeword` (beside this one:
  `../hassmic-plugin-microwakeword`; the SDK and the engine's sources copied in by its `tools/sync.sh`, `make drift`
  there says when they differ; `make test HASSMIC=<this tree>` runs it in this tree's PC build; its own author key),
  where plugin authors start is the repository `hassmic-plugin-template` (`../hassmic-plugin-template`: the C
  example on `sdk/`, the Rust one on a client written from `sdk/PROTOCOL.md`, held to `sdk/vectors/`; `make sync`
  copies `sdk/` in, `make drift` says where it differs, `make test`/`rust-unit`/`arm-smoke`; sync it as mww's),
  the list of plugins is `hassmic-plugins`, the one place for plugins (`tools/index.py`: check, fetch, build (adds
  each package's listing: `--key`; `--arch x86_64` for this tree's tests), sign, verify; its own key; a copy of
  `sdk/plugcheck.py`, `scripts/otatool.py`; `make check` compiles this tree's `plugin_store.c` with
  `plugin_index.c` and `hash.c`).  Official = the index key's listing (`listing.sig`: hex signature, then
  "hassmic plugin listing 2\nid ..\nversion ..\ntier official|community\nmanifest <digest>\nserial <index's>\n";
  form 1 was the release key's, taken by no build), added by the index's build to the package the plugin's own
  repository released, official and community alike (an entry's `package` is that release asset only; a community
  entry also names its `signature`, `license_sha256` and capabilities), once its GitHub artifact attestation holds
  (docs/plugins.md 5.3 "As built"; the index's `tools/attest.py`: `gh attestation verify` and its JSON read again:
  the entry's `workflow` (each version names it) of its `source` at its `commit`, as both the signing workflow and
  the run's, started by the push of the tag its release asset is under, built that SHA-256 on a GitHub-hosted
  runner; gh's verdicts (`Refused`: left out) apart from its failures (`Unavailable`: a version the site lists fails
  the build); the one bundle that held published beside the package; the index builds nothing, its harness a pull
  request's smoke test; `broad` per community version in `index.json`, read by `plugin_index.c` into
  `pi_version.broad`, at most `PI_BROAD_MAX`, shown by browse's `hmbrowse.broad`; `workflow`, `release`,
  `release_sha256`, `attestation`, `attestation_sha256` read by no Echo; `tests/plugsite.py` writes them too).  The release key has no part in plugins.
  `keys/plugins.pub` (a copy of the index's `keys/index.pub`) and `keys/plugins.conf` (`PLUGIN_INDEX`, `PLUGINS_PUB`,
  `PLUGINS_PUB2` with `PLUGINS_PUB2_WITH` (a second index key and the hex of the first it belongs with, empty:
  docs/plugins.md 5.2 "The steps, as built"; built in only where `PLUGINS_PUB` is that key by content (64 hex
  digits, any case), so never beside a fork's own first key; where `PLUGINS_PUB` is keys/plugins.conf's own
  (`PLUGINS_PUB_CONF`) another key stops the build (a typo); `fork-release.sh` keeps the lines and deletes the
  repository variables fork.conf no longer names, `plugins-least.sh` the same rule),
  `PLUGINS_LEAST_INDEX`, `PLUGINS_LEAST_REVOKED`: the least serials this build takes, set by hand before a release
  with `scripts/plugins-least.sh`; a fork's in `secrets/fork.conf`) are built in (`-DPLUGIN_INDEX`, `-DPLUGINS_PUB`
  hex, `-DPLUGINS_PUB2` hex or empty, `-DPLUGINS_LEAST_*`, `-DBUILD_EPOCH` the commit's time; `plugin_index.c`
  `pi_keys` (the first and, where there is one, the second: `pi_signed` takes either, for every list and listing,
  `pi_signer` says which; a kept revocation list neither key signs is no list and no word, `pi_revoked_unread`),
  `pi_base`, `pi_least_*`; `HASSMIC_PLUGINS_PUB` a file (in place of both built-in keys), `HASSMIC_PLUGINS_PUB2` a
  file (beside the first), `HASSMIC_PLUGIN_INDEX`, `HASSMIC_PLUGINS_LEAST=<index>:<revoked>`,
  `HASSMIC_PLUGINS_BUILT` for tests; `make unit-plugin_index` builds its test a second time with a key built in).
  A new index key's steps (5.2): the index's tool reads the published site with `keys/index-previous.pub` beside
  `keys/index.pub` for the switch's publish, and publishes an unchanged revocation list under the published serial +
  1 (`load()`: later builds keep the published serial for the same list; an Echo of both keys also keeps a list of
  the same serial by the second key, never back to the first, `pi_revoked_take` `kept_by`); after every look the Echo takes the index's
  listing of each version installed that the index lists by the other key, a higher tier or under the least
  (`plugin_fetch.c` `relist_due`, `plugin_store.c` `ps_relist_wanted`/`ps_relist`: the same manifest, tier never
  lower, no button, no install; -2 a failure of the moment, `R_FAILED`, asked again; `pf_json` `relist`, `POST
  /api/plugins/index` "relist", the browse row); the daily look (`daily_fetch`) reads the index too and relists
  wherever `ps_relist_reason` (1: one by the first of two keys, one under the least; 2: a listing that does not
  hold, `listing_unread`) or a failed one says so, and `relist_tick` (the daily thread, every minute) where it says 2:
  the index a minute after the start, at once when the network is back (`net_online`), hourly while it stands
  (`RELIST_*`, `HASSMIC_PLUGIN_RELIST=<first>:<every>[:<check>:<gap>]`); versions the last look found not listed
  (`unlisted[]`) are no reason, failures for versions gone are forgotten (`relist_upkeep`); the Echo's own looks are
  `PF_DAILY` (one that fails leaves the page's list); `pf_shutdown` cancels them at the end.  A failed self test only
  keeps a build from becoming the factory copy: nothing goes back, the build runs on.
  `sdk/plugcheck.py`: `pack --listing` (tests), `list` (what the index's build adds), `show --index-pub`.  Change
  `sdk/`, `scripts/otatool.py` or the
  engine: sync the sibling repositories (mww's `tools/sync.sh`, the template's `make sync`; `make drift` in each).
  The plugin index (docs/plugins.md 5.3 to 5.6 "As built", 13): `plugin_index.c` (the key; `pi_signed` over a
  text that opens with its purpose; `pi_index_parse`/`pi_revoked_parse` on json.c's tokens, every field of its
  kind and bound, a list that does not read refused whole, unused fields left alone; the bounds in one place,
  `PI_*` in plugin_index.h, which the index's `tools/index.py` `BOUNDS` mirrors and its tests hold to: `PI_INDEX_MAX`
  1 MB, `PI_PLUGINS_MAX` 4096 and `PI_VERSIONS` 16 (the highest; what is left out counted as `dropped` and shown),
  `PI_REVOCATIONS` 2048 and `PI_REVOKED_MAX` 512 kB (past either the revocation list is refused whole, said), the
  texts' bytes; the parsed index compact: its texts in one pool, its versions in one array, `pi_index_free`; both
  serials are times: `pi_serial_ok` (clock set, `pi_clock(clock_synced)`: no more than `PI_AHEAD_S` a day ahead),
  `pi_floor` (the floor line kept beside a list: never past now, or without a set clock the build's time
  `BUILD_EPOCH`, + a day; kept as made; it holds only where `pi_held` cannot believe the kept serial: the clock set
  and the serial more than a day ahead), `pi_least_index`/`_revoked` (a kept list under the least still withdraws:
  `pi_revoked_current` 0); the revocation list kept in one file,
  `state/plugins/.revoked` ("floor <n>", the signature's hex line, the list: statefile.c), checked again at the
  first question after every start, `pi_revoked_take` (newer, the same, older: kept, said), `pi_revoked_age`,
  `pi_revoked_file` (for the members), `pi_withdrawn` (every tier); `make unit-plugin_index`,
  which also measures a look's peak at the bounds).  `plugin_fetch.c`: `pf_look` GETs `revoked.json`, `.sig` (taken
  first, whatever becomes of the index; not taken: the look fails; older than kept: kept, the look goes on),
  `index.json`, `.sig` through `update_fetch`
  (update.c's one GET: libcurl, `net_socket`, all its time as `secs`; `stop` ends it from the progress, data and
  socket callbacks; `User-Agent: hassmic` and nothing more; redirects to HTTPS only, as every fetch of update.c's),
  the signature first, a serial under the build's least, from the future or under `state/plugins/.serial`'s floor
  refused, and an index only beside a revocation list at least its `revoked_serial` (a newer list:
  `plugin_withdrawn_changed`, which stops what it names and starts what it no longer names, and the members told);
  `pf_best` per plugin the highest version whose `requires` this build meets (`unmet`; a newer one that needs more
  named, "needs a newer hassmic"), not
  withdrawn, not under `ps_official_least`; `pf_download` from `<base>packages/<id>-<version>.hmp` (never the list's
  path), at most its size, refused unless size and SHA-256 are the list's; the list kept with its time, a failed
  look too (`pf_json`, `GET /api/plugins/index`: per plugin `here` and `top`, `site`, `revoked_age`); the pace is the
  Echo's, `pf_look`'s: `POST /api/plugins/index` "look" (a page by itself: "Browse plugins" opened, or the Plugins
  page while `plugins` is on: `PF_AUTO`, an hour whatever came of the last look) or "again" (its button; fetches
  too: `PF_ASKED`, 30 s); a cancelled look leaves the list as it was (`PF_CANCELLED`).  By itself (`pf_start`'s
  thread, 5.6; only while `plugins` is on and one is installed, `plugin_feature_on`): `revoked.json` and its
  signature once a day at a random time, and the members' list: arb `T_MSG` to all, kind 0x43 `M_REVOKED` (19 bytes:
  serial, the settings port, a ticket; hourly and after a newer list; readable by the LAN, said; main.c `arb_message`
  -> `pf_message`, which takes a word only from a private, link-local or loopback source), a newer one fetched from that member's `GET /api/plugins/revoked` (web.c -> `pf_revoked_serve`:
  members only, `X-HM-Ticket` one of its last two tickets (`ticket_new`, two hours) + `X-HM-Ctr` nonce, refused
  while among the ticket's last 64 (`TICKET_RING`, a ring: never a cap, which a member spent) + `X-HM-Mac` by `arb_derive("hassmic plugin revoked 2")`, anyone else and a request again the 404 of no
  such path; `update_fetch_lan`: plain HTTP, a plain socket (the egress lock: local only), no redirect, 5 s), one fetch per place in 10 min whatever serial
  (`struct sender`, 16 places keyed by arb id, source address and named port: ids are a member's own word; the one
  that began to wait first asked first), fetches from all members 4 at once then one per 20 s (`MEMBERS_*`,
  `budget_at`) and their log lines 5 then one per 10 min, the rest counted (`mlog`): bounds against mistakes and
  non-members; a member with the key can delay another's list at will (the maintainer's decision: not held against
  it; the site's daily look and the week's warning are the guarantee, docs/SECURITY.md);
  `HASSMIC_PLUGIN_DAILY=<first>:<period>:<tell>`, `HASSMIC_PLUGIN_MEMBER=<ms>[:<burst>:<every ms>]` for tests).
  `pi_revoked_take(..., PI_SITE|PI_MEMBER, ...)`: `PI_TAKEN`/`PI_SAME`/`PI_OLDER`/`PI_REFUSED`; word (`.revoked-at`
  "word <wall> <age>", at most hourly, `pi_revoked_upkeep` from the daily thread; the older "<wall>" alone is no word
  without a clock or ahead of it) is the site's answer (now) or a member's newer list (as of its serial, `word(as_of)`), never a
  member's same one; the floor line never falls (`pi_revoked_take` keeps the line before); the floor `pi_held` (the kept serial wherever the clock vouches for it, the
  revocation list's also without a clock; the line raised and written then).  A site's older revocation list:
  `PI_OLDER`, the look goes on (`note`).  A package not the kept list's: `pf_download` looks once more (`PF_NOW`).
  `pf_revoked_stale`: plugins on, one switched on, no word on the list in a week (or none, no word known, a kept
  list under the build's least, which still withdraws): `/api/state` warnings and `/api/plugins` `revoked`.  Revocations apply
  to every tier (`plugin_store.c`: `ps_stage_check` refuses, `ps_scan` sets `withdrawn` and `refused`, "the plugin
  index withdrew it: ..."; `plugin.c` `plugin_withdrawn_now`; a withdrawn wake engine is down, main.c's words say
  "choose another engine, or run it anyway").  "Run it anyway": `POST /api/plugins/<id>/anyway` "run"|"cancel"
  (web.c W_ANYWAY, `anyway_ask`; the press writes `state/plugins/<id>/anyway` with the list's serial, `ps_anyway`;
  `ps_scan` honours it while that is the kept list's serial).  A lower tier than the version installed is wider
  (`ps_stage_check` `installed_tier`: the button, every path), and a listing under the build's least index serial
  counts as none at install (a sideload).  The page: `web/plugins.js` Plugins page (switch, the
  cards installed, "Update" where `pf_json`'s `here` is newer, "Browse plugins", "Install from file…") and the sub
  page `#/plugins/browse` (`buildBrowse`, `renderBrowse`; pure `hmbrowse`: `find`, `offer`, `whyNot`,
  `tests/unit/web_browse_test.py`); app.js `PAGES` entry `plugins/browse` with `sub` (no nav entry, the parent
  marked), `route()` takes `#/<page>/<sub>` before a setting's id.  `web.c` "the install slot": one install at a time whoever brings the package, `pl` with an
  owner (`PO_BROWSER` + pub, `PO_HASSMIC`) and a generation new with every claim and release; states
  `PL_UPLOADING/FETCHING/CHECKING/CHECKED/WAITS/INSTALLING/INSTALLED/FAILED`; every change under `lk` and only from
  the generation and state it names (`pl_claim`, the one place a slot changes hands, by the table `pl_rules`: an
  upload after 30 s without a piece, a checked package after 2 min, CHECKING/WAITS/INSTALLING never, hassmic's own
  fetch gives way to a browser or a push at once; `pl_move`, `pl_close`); the staging file checked and dropped only
  by the holder in a state nobody takes (`/install`'s look too: CHECKED -> CHECKING -> WAITS, INSTALLING or back),
  written with the claim's generation, which the store holds each write to (`ps_stage_begin`/`_chunk`'s `gen`,
  `stage_gen`); the press armed for browser, generation and the listed digest (`pl_press`, `plugin_ask`, through
  the page's one wait for the button: "Settings page") and installing only that (`plugin_accepted`,
  `ps_stage_install`'s expect), else nothing and the browser told (`pl_note`); whether a press is needed is the
  store's to decide as it writes (`ps_stage_install`'s `press`: PS_NO_PRESS, PS_PRESSED, PS_VOUCHED; the check run
  again under `ps_lock`, which `ps_remove` takes; `PS_NEEDS_BUTTON`: nothing written, the slot CHECKED again, a
  push and hassmic's own fetch install nothing): never hand a check's earlier "no button" down as the decision; a
  new path into the slot goes through `pl_claim` and never writes `pl.state` from what it read under an earlier
  hold of `lk`.  "packages the Echo brings itself": `POST /api/plugins/fetch` claims the slot,
  `bring_thread` (one fetch, `FETCH_MS` 5 min as a whole; FETCHING while it downloads, CHECKING for the staging
  file) downloads the highest
  version `pf_best` names, stages and checks it as an upload (`bring_check`), refuses what has no listing, is not the
  manifest, id, version and tier the index named (`bring_ok`; one needing a newer hassmic: the check's refusal), and leaves the page the check's answer
  (`check_json`, `GET /api/plugins/fetch`) for the same dialog and `/api/plugins/install`.  Without the button,
  hassmic's own (only where `ps_stage_check` asks for none: the pinned author, what was accepted or less):
  `web_plugin_push` for ota.c's `HMOTA-PLUG1` (the update key's challenge over the package's digest,
  `SEND`, the package; `scripts/ota-push.sh --plugin`, `otatool.py plugin`, the C otatool too).  The engine setting
  at start is read as written first (`settings_peek_raw`): `plugin:<id>` is a value also when that plugin needs
  a newer hassmic.  `HASSMIC_PLUGIN_FRESH`, `HASSMIC_PLUGIN_LEAST`, `HASSMIC_PLUGIN_SLOT=<checked ms>:<upload
  ms>:<fetch ms>` (the table's times), `HASSMIC_PLUGIN_ASK_DELAY=<ms>[:<file>]` (slept between `/install`'s check and its
  ask), `HASSMIC_PLUGIN_OWN_DELAY=<ms>` (between a push's check and its install) for tests.
  The push port and libcurl's loader: "Push updates", "Online updates".  Crash marks of the second kind (`core <id>`,
  `state/plugins/.cores`, `core_note`, `HASSMIC_PLUGIN_CORE=<count>:<ms>`): hassmic fell in the core's code on a
  plugin's report; the broker's thread by `core_begin`, any other by `plugin_core_enter`/`_leave`, which bracket
  only what is the plugin's own data (main.c `plugin_words_scan`; at start its word opened and its answer waited
  for), never Amazon's models or the core's own lists.  The wake engine's plugin: at `CORE_MARKS` its start waits
  `RETRY_MS`, each time anew, never refused; any other: the third mark waits 30 s, the fourth `RETRY_MS`, the
  fifth within the window switches it off as five crashes do (`start()`, `p->off`).
  Sync between Echos (docs/plugins.md 5.4 "As built", 13; web.c "sync"): the page (`web/plugins.js` "Copy
  plugins": `drawCopy`, rows `pkg <id>` / `set <id>` / `item <id> <list>/<item>` on `matrixTable`, order by
  `hmmatrix.steps`) reads the source's package member by member (`GET /api/plugins/<id>/pkg/<member>/<off>/<len>`,
  `ps_pkg_read`), packs it (tar.js) and uploads it to the target through the install slot as its own claim.  Where
  the check asks for the button: the target's nonce (`POST /api/plugins/accept/nonce`, signed, `acc_nonces` 8,
  once, `ACC_NONCE_MS`), the source's voucher (`accept/issue`: only for a plugin installed, runnable and accepted
  there; `ps_voucher`: BLAKE2b-128 keyed with `arb_web_key` over "hassmic plugin acceptance 1", target's web key,
  nonce, browser key, node NUL, id NUL, manifest digest, author key, accepted digest; layout held by
  `unit-plugin_store`'s vector), then `POST /api/plugins/install` "synced <node> <nonce> <voucher>".
  `sync_judge` (all must hold, each the target's own finding): nonce this browser's and fresh; neither end in
  Kiosk mode (`arb_mode`, `arb_member_mode`); the MAC over the target's own check of the staged package; the node
  a member heard; tier official (a sideload never); its `requires` met here; `ps_changes.unsure` 0.  A voucher proves a
  member's key, not a press and not which Echo (any member signs any node): never loosen 5 or 6 on its strength.
  Also refused: a version older than the newest the target saw listed (`pf_seen`, `state/plugins/.listed`) or than
  the highest ever installed there (`ps_installed_max`, `state/plugins/.installed`: raised by `ps_stage_install`
  once the package is in place, by `ps_scan` from each folder and by `ps_remove` before it removes; only rising;
  unknown = closed: the file unreadable, a raise whose write failed (kept in memory, `floors_owed`, until any later
  raise or scan writes the file), or more than `FLOOR_MAX`
  64 ids, which the file keeps as a line `+`, for good; `HASSMIC_PLUGIN_FAIL_FLOOR`, `ps_floors_forget` for
  tests; Remove takes no button), `plugins`
  off, no note written.  `ps_stage_install` with PS_VOUCHED looks again at the store's own rules (official, the
  check, `unsure`, that floor); the listed version and `plugins` on are held as `sync_judge` read them.  Yes: `syn_begin` (the note on the flash first, "pending"; 16, never replaced, at the bound
  "button"; `HASSMIC_PLUGIN_NOTES`), `pl_move` CHECKING -> INSTALLING for that claim and digest, `accepted` how
  "synced, voucher named <node>" (`PS_HOW_SYNCED`), `syn_end` (done, or removed; `syn_load` settles a "pending" by
  the store; `syn_failed`: an install that failed after its files were written keeps it), in `/api/plugins`
  `synced` and `/api/state` `plugins_synced`: app.js `renderAttention` on every page of this Echo, the other
  members' while polled.  Dismiss (`POST /api/plugins/synced`, `syn_ask`): at once only for a browser let in with
  this Echo's button (`struct client` `button`, the positive fact in `state/web_button`, written by `web_approve`:
  whatever is not listed counts as vouched) that is not the note's own and only for a note whose browser is known;
  else the action button held 1 s (`W_NOTE`, `web_hold`).  A dismissal names "done" notes by number (`struct
  syn_ids`, numbers at random, `ids_add` bounds it; 16 standing at most also from the file), never a pending one,
  and the armed browser asking for other numbers is told "busy"; the wait is bounded per Echo whatever the notes
  (`note_arms`: 3 armed per `NOTE_AGAIN_MS`, withdrawn ones counted, in `state/web_wait`, "later";
  `HASSMIC_PLUGIN_NOTE_AGAIN`; a vouched browser can spend them: docs/SECURITY.md).  A note shows the bringer's
  label and its key's first 8 bytes (`key`, as the clients list).  A pending or failed note is settled by the
  folder's package (`syn_there`: its manifest digest, kept on the note's line; an unreadable manifest keeps the
  note); `.synced` there and unreadable: `syn_blind`, no sync without the button.  Event `synced` in place of installed/updated,
  held in the core until a link of Home Assistant's subscribes (`core_plugin_held_flush`; the note's `told` mark
  over a restart, a dismissed note kept as "gone" until told; `PLUGIN_HELD` = `SYN_KEPT` 32: every kept note's
  event has a place, and at 32 kept a sync is answered "button").  `HASSMIC_PLUGIN_SYNC_DELAY=<ms>:<file>`,
  `HASSMIC_PLUGIN_FAIL_ACCEPTED=<file>` for tests.  No: `{"install":"button",
  "why"}`, nothing armed, the slot stays CHECKED for `plgDialog` and a plain install.  Settings by
  `/api/plugins/<id>/set` (a secret row refused), files by `GET /api/plugins/sync`'s digests (`ps_item_digest`,
  by id, without `ps_lock` held by the caller; the answer grown, never cut) through the item upload.  The place for 3.3's second press is marked in `sync_judge`.
  The engine in hassmic stays, deprecated, until the cut release (docs/plugins.md 5.9; the owner's move: "microWakeWord").
  A hassmic update and the plugins (5.9 as built, 2026-11-23): nothing of the plugins holds an update back or is
  fetched for it (a scope only adds, so every later hassmic speaks what an installed plugin requires); an online
  update and a push go to root as they come.  The self test excuses the wake plugin only where the owner chose
  that it does not run, or this build cannot run it, `plugin_selftest_ok`: plugins off, `PLUGINS=0` (`conf_off`),
  not enabled, withdrawn, `m.newer` (a fall back to an older build: its card says so, Amazon's word stands in),
  community off (not where its listing does not hold here: `listing_unread`, 5.2's build of the new key alone,
  and the plugin ran as listed: the `ready` mark's `listed 1`, `ready_note`, an official listing only (a
  community one ran by the owner's switch); a mark without the line counts as 1); the launcher, sandbox, unread,
  acceptance hold the update, the self test asking again every 10 s (late: a mark gone since is no pass).  Gone
  with versioned scopes (built 2026-11-21 for 0.x, never released): `pkg/<api>/`, `PS_BESIDE`, `/drop`,
  `web_plugins_prepare`/`keep_beside`/`pf_for_api`, the bundle's `plugin-api`, held updates and "Install it all
  the same", `ota_forget`, `ps_obsolete_sweep`/`.api-since`/`obsolete_thread`, `web_plugin_auto`.  An update that
  drops `wake` asks for the button and, once it runs here accepted, wipes `files/` and `storage/` (`seal_upkeep`,
  mark `state/plugins/<id>/sealed`, kept until both are gone: `rm_rf` says what is left; never while a process of
  another package of it runs, `ps_busy` = plugin.c `plugin_old_running`, and done before the new one starts,
  `program_open` -> `ps_seal_settle`; microWakeWord's models are its own copies, `state/mww` is never wiped).  PC:
  `build/plugrun-host` (x86_64 filter, no user change), `HASSMIC_PLUGIN_SANDBOX`, `HASSMIC_PLUGIN_HEARD`,
  `HASSMIC_PLUGIN_RETRY`, `HASSMIC_PLUGIN_CPU`, `HASSMIC_PLUGIN_LINES`; `make unit-hmplugin` (the SDK's C client
  against a stand-in broker), `make unit-plugin_broker` (plugin.c itself, a clock the test moves), `make unit-plugrun` (also both seccomp programs against the table, `tests/unit/plugfilter_test.py`; a musl program's `raise()`/`abort()` where zig is, `tests/unit/plugrun_musl.c`),
  `unit-plugmsg`, `unit-plugin_store`, `unit-plugin_broker` (plugin.c itself with a stand-in plugin and a clock the
  test moves, `tests/unit/plugin_harness.h`: the watchdog and the marks), `make unit-plugfuzz` (libFuzzer over the
  broker's `message()`, 60 s, clang; not in `make unit`).  On Echos (2026-11-19, PLAN.md "Plugins"): `scripts/plugin-probe.sh`
  (`src/tools/plugprobe.c` under `plugrun -T`, built with zig) passes on biscuit, donut, checkers; hassmic under the
  launcher and the ARM plugin as the engine on biscuit.  A new model: the probe first.
- **Artifacts** (`artifacts.c`): Amazon's models (`wake:<name>` = `models/<name>/`, `sound` = `aed/`, `whisper`): files
  plus two levels of folders (`whisper_components/`, `BDPGeneratedFiles/`, `nttfusionconfig/ntt_conv/`; a file's name is then its path), listed
  with a digest (BLAKE2b-256 over name, size, content per file), read and written in pieces (`ART_CHUNK_MAX`) over
  signed requests, so the page copies them Echo to Echo, and saves them into a tar and back ("Models on file": `web/tar.js`,
  `hassmic-models.json` with the digests first; `tests/unit/web_tar_test.py`). Staged in `state/artifacts/<stage>/`; commit checks sizes,
  digest and, for wake word sets, `pryon_test` (next to the binary, `HASSMIC_PRYON_TEST`); install writes
  `state/artifacts/request`. The model folders are root's: `main.sh`'s watcher runs `scripts/device/artifact-install.sh`,
  which reads the staged files as the daemon's user (runas: no link can point root at its files), swaps a fresh
  root-owned folder in and restarts hassmic (the wake word list and the whisper model are read at start).
  `artifact-install.sh migrate` (main.sh, every satellite start): wake word folders under the short name (`echo-de`,
  from installs by hand) renamed to artifacts.sh's (`echo-de-DE`) where the language has one region; `state/wake_word`
  follows.
- **Clock and log stamps** (`clock.c`): `proto_esphome.c` asks HA for the time (GetTimeRequest) on every link (on
  subscribe, then every 6 h on pings; the clock takes keyed links' answers only, any does for "Next timer"'s end,
  `ha_time`); `clock_from_ha` leaves `<epoch> <boottime s.cs>` in `state/clock`, root's
  `main.sh` `clock_set` (netwatch) sets clock + RTC and `hassmic.clock.synced`. `clock_log_start()` (first in `main()`
  after options) turns stderr/stdout into a pipe to a double-forked reader that stamps lines and drops known Amazon
  noise; it ignores hassmic's signals and ends at EOF. Never make it hassmic's direct child (Amazon's DHA module waits
  for all children). Host: `HASSMIC_CLOCK_SYNCED=0` plays an unset clock.
  Time zone: GetTimeResponse field 2 (a POSIX TZ string, aioesphomeapi's `timezone.py`; HA keeps the name) ->
  `clock_zone_from_ha` (with `core_lock` held: hands the rule to a thread of its own, `zone_thread`) -> `tz.c` `tz_lookup`: a zone of bionic's one-file `tzdata` whose TZif footer is that rule (a
  country's first by area, then zone.tab order: the country of Home Assistant's home, `zone.home`'s latitude/longitude
  subscribed on keyed links (`proto_esphome.c` `home_subscribe`, no permission needed; `clock_home`; 0,0 = never set)
  through `geo.c` (`geo/borders.bin`, Natural Earth's borders cut down by `tools/geo/build.py`, built in, ~10 ms on a
  Dot 2; within 3 km of a border the other country is named), or the Wi-Fi's country where it is that neighbour or
  there is no location (`apcountry.c`: the associated BSS's 802.11d Country element by an unprivileged nl80211
  GET_SCAN dump, `HASSMIC_FAKE_COUNTRY` a file for tests); else the zone set kept when it has it, else zone.tab's
  names in area order: a guess; no zone set: the last known one stands for it), else the string itself.  How it was
  found stays in hassmic's own `state/zone` (`<zone> home|wifi|kept|guess|rule <its country> <the home's> <the Wi-Fi's>`, `-` none, `zone_note`; a kept zone
  that is hassmic's earlier guess stays a guess, a known one stays known when the country is missing once), which
  `clock_zone` gives `speech_locale` only when known (home, wifi, kept): the language never from a guess (CET:
  Europe/Amsterdam) nor from `state/timezone`, which root takes away at once; `/api/state` `zone` and the page's
  "Language you speak" row say what Automatic stands on.  Coordinates never stored or logged.  The zone -> `state/timezone` ->
  `main.sh` `tz_set` (a name or a rule, never a path): `persist.sys.timezone`, plus `service call alarm 3` for
  names where the framework runs.  `HASSMIC_TZDATA`, `HASSMIC_TIMEZONE` (the zone set) for tests;
  `tests/unit/tz_ref.py` builds a tzdata from Python's.
- **Wi-Fi switch** (`wifi.c`): the settings page's "Switch network…". `state/wifi-request` ("scan <id>" / "join <id> <ssid
  hex> <psk hex|->") for root (`main.sh` ota_watch) -> `scripts/device/wifi.sh` in the background; answers in root's
  `/data/local/hassmic/wifi/` (`status`, `scan`, `result`, `lock/`; the id ties answer to request). The password comes
  sealed with K (`web.c` unseal, `crypto.js` seal) and only its PSK (PBKDF2) leaves hassmic. wifi.sh saves nothing
  until the Echo is on the new network with a fresh lease (dhcpcd runs with `-K` and ignores network changes: wifi.sh
  marks `dhcp.<if>.result`, restarts init's `dhcpcd-<if>` (not `dhcpcd -n`: it waited for a carrier for good), waits
  for the hook to write it) and its router answers ARP; else it removes it and selects the old one. Talks to wpa_supplicant's global socket when the interface's is gone. netwatch
  leaves wifisvc alone while `lock/` exists and runs `wifi.sh status` on every address change; `clean` at the firewall start.
  The fixed address (issue #191, System page "Network address", `POST /api/wifi/address` "auto" | "fixed <ip>
  <prefix> <gw> <dns> <dns2|->", `wifi_address`, checked there and again by root's `fixed_ok`): request "address ...",
  root's `address()` refuses one another device answers ARP for, sets it, waits for the router (`static_up`), then
  `REACH_SECS` (120) for a TCP connection to or from the new address that was not in the snapshot taken as it was set
  (`reached`, `/proc/net/tcp`, ESTABLISHED or TIME_WAIT: the page's one-request connections), else back to what it
  had; the record `wifi/address` ("fixed <ip> <prefix> <gw> <dns> <dns2|-> <ssid hex>", root's; none = DHCP),
  `address-result`.  The Dots: init's `dhcpcd-static` (Amazon's `dhcpcd_static.sh`, values from the `dhcp.wlan0.*`
  properties) in place of `dhcpcd-wlan0` (`dhcp_off` stops both); netwatch runs `wifi.sh keep` (once wifisvc is
  stopped) to put it back after wifisvc's DHCP and to drop it on another network.  Android: wifictl `ip` (the
  network's `StaticIpConfiguration`, `setIpAssignment`) then `reassoc`: checkers stores it and takes it only at a
  reassociation.  A switch that works drops the record (it was the old network's); `back()` sets it again; `forget`
  removes it.  The page follows the Echo to the new address with the same browser key (`new Echo(base, hello)`), and
  after DHCP finds it through the other Echos' hello members.  The guided setup asks for one (`scripts/lib/setup.sh`
  `fixed_ask` before the install, `fixed_set` once the satellite runs: the same request as root's, written over adb;
  the PC asks the new address for the page's hello while root waits; `make unit-setup_fixed`).  Improv carries none.
- **Roaming** (`scripts/device/roam.sh`, issue #234): root's netwatch moves the Echo between the access points of its
  network where it stops wifisvc (device.conf `WIFI_SERVICE`; the Makefile's `WIFI_ROAMING` lists the setting
  `wifi_roaming` there only): Amazon's wpa_supplicant has no bgscan module and no CQM, its picker (`adptClntPickBss`)
  prefers 5 GHz at any signal, the drivers' own roaming is off in wifi.cfg (`DisRoaming 1`), biscuit's and donut's
  refuse FT AKMs (err=22; cannoli's takes them, FT roams untried), none offers RRM.  signal_poll every round; weak (-70 dBm, 30 s): a BTM query where the AP's Extended Capabilities
  offer it, then a scan (the network's learnt channels, every channel at most every 10 min); a candidate 8 dB
  stronger and >= -67 dBm, the move with the target pinned (`set_network bssid`, never left: `wifi/roam-pin`), else
  the picker goes elsewhere.  `wifi/roam` (AP and last moves) -> wifi.c `wifi_current_json` -> the Wi-Fi row; the
  signal from /proc/net/wireless (`HASSMIC_PROC_WIRELESS`).  `tests/fake_wifi_roam.py`; measurements in PLAN.md.
- **Setup (OOBE) and factory reset** (`improv.c`, `main.c`): not set up = from an install or a reset until Home
  Assistant's voice assistant subscribed (`core_link` ready -> `state/adopted`; an existing `state/api_key` counts);
  stock's orange `setup-mode` spinner meanwhile (`core_oobe`).  `improv.c` = Improv Wi-Fi over BLE on `ble.c`'s peripheral role (advertising +
  a GATT server on one link, `conns[SRV]` beside the proxy's slots; `ble_serve`/`ble_advertise`/`ble_server_notify`):
  advertises when not set up and offline 20 s (authorized), or set up and offline 10 min (the action button authorizes,
  `improv_authorize` in `on_action`); Wi-Fi through `wifi.c` -> root's `wifi.sh` like the page's switch; no host name
  or device name commands (the Echo names itself).  Who may switch or scan is `may()`, asked per command, a fact
  with an origin: mode `M_OOBE` and `core_oobe()`, or `M_LOST` and the button pressed less than `AUTH_SECS` ago
  (`auth_until`, set only by `improv_authorize`; for the Echo, whichever phone is connected in that time; spent
  by a switch that worked; gone with `M_LOST`).  Never derived from the state shown (`show()` derives that from
  the facts) or from the mode being `M_OFF`: there nothing is authorized, a link that is up is dropped
  (`on_connection`, the worker), and the advertising's data is cleared before a drop for `M_OFF` (ble.c's
  `adv_upkeep` switches advertising on again when a link ends while data is set, `srv_gone` pokes the loop for it:
  keep that order).  Anything else: `E_NOT_AUTHORIZED`, nothing asked of root.  The press is Improv's only while
  a phone is connected, served and shown "authorization required" (`waits`, exactly while the ring shows it): never
  with no phone there.  Improv has no link identity: the ring does not tell whose phone waits, and the press
  lets whatever is connected within `AUTH_SECS` switch once; and a device in range can hold the one link, and the
  ring's wait with it, for as long as the Echo is offline (improv.c's header, docs/SECURITY.md: not closed, not
  bounded; do not word it as either.  The owner's rule: press only when your own app asked for the button).  How long a link is kept: set up
  and not authorized, until nothing came from it for `WAIT_SECS` 120 (`heard_at`, ble.c's `heard` hook: any ATT
  PDU; `wait_since`; for a forgotten phone only, past py-improv-ble-client's own 90 s idle disconnect, which is
  what the owner sees: asked for the button he has 90 s, then the ring's wait goes off until the client connects
  anew, and a press in that gap is the assistant's; no address
  is held against anyone: the shun is gone, it broke Home Assistant's flow, which connects before its form);
  "provisioned"
  `PROVISIONED_SECS` whatever is connected; on a network `ONLINE_SECS` 60 in all, however often the network goes
  and returns (`on_ms`: every look that finds the network counts the time since the look before, a second at most
  after one that found none; begun anew only by a switch's end and, in `M_LOST`, by what the button paid for: the
  press, a scan asked while authorized; in `M_OOBE` a scan does not: scans kept a central on an Echo that had its
  network).  A scan beside a switch that runs after the button's time: `E_UNKNOWN` from `command()`, nothing asked of
  wifi.c (`join_id` stands until the worker read root's answer).  A carrier while a switch runs, or within `SETTLE_SECS` 5 of one that failed
  (until a look finds it gone or another switch begins), is not the network's return: `offline_since` stays, the Echo goes on advertising.  A link we end is `ended` (`end_link`): not served, not counted.  Identify once
  per 10 s, set up and not authorized once per `IDENT_SECS` 60.  `M_LOST` is entered in one way only: set up and
  `OFFLINE_LOST` without a network, counted from the adoption at the earliest; a mode kept for what runs is never
  turned into it (keep that: web.c, main.c and SECURITY.md argue from "a phone's wait means ten minutes offline").
  Adopted while it advertises as `M_OOBE`: a switch under way and its "provisioned" end as they would have (the
  phone reads the result; `may()` is false, no wait, no press, `improv_holds` 0; device info and identify once per
  `IDENT_SECS` still served, to whatever is connected), a scan that is out dropped at the adoption whatever else
  stands, and "provisioned" ends at its time whether or not a scan is out; anything else is `stale()`: the
  link ended, the mode by the network alone.  `show()` raises `a_waits`/`a_holds` before the hook
  and lowers them after it, and web.c's `login_ask` (`improv_holds`) and `web_approve` (`improv_waits`) read them
  BEFORE `login_expire` takes the mark of a wait's end: a 0 read means the mark is there to be taken or no wait
  had begun (keep both orders).  Locks: `mu` is held for nothing that waits (the controller's and the button's threads take it): what a
  command asks of wifi.c (a state file, PBKDF2) or the core is decided under it and done after (`command` ->
  `ask` -> `asked`, `cmdn` kept meanwhile); the worker's wait is on the monotonic clock (root sets the wall clock
  as the network returns).
  `HASSMIC_IMPROV_TIMES` (`<oobe> <lost> <auth> <provisioned> [<online> [<wait> [<identify>]]]`), `HASSMIC_FAKE_ONLINE`,
  `HASSMIC_HOLD_TIMES` (the hold stages), wifi.sh's `WIFI_STEP` (its looks) for tests.  Reset (`core_reset`: action
  held 10 s via `buttons.c` hold stages, HA button `factory_reset`, page
  `POST /api/reset` "reset") writes `state/reset`; root's `main.sh` ota_watch stops hassmic, `wifi.sh forget`, empties
  state/, starts it.
- **New IP address** (`netaddr.c`, issue #17): polls the addresses; links whose local address is gone are shut down
  (Linux keeps them, and one held the voice assistant); on a new address, and while the Echo is set up but no client
  is connected (100 s, doubling to 30 min: Home Assistant ignores mDNS while it still holds its old link, 90 s), it
  writes `state/mdns` and root's netwatch restarts avahi (`avahi_start` in `main.sh`).  The same per peer
  (`struct netaddr_peer`) for a Sendspin server that played here and is gone (30 s first; issue #28: a restarted Music
  Assistant only browses, and its queries may not reach the Echo). Restart: page `POST
  /api/reboot` / ESPHome button `restart` -> `core_reboot` -> `state/reboot` -> root's ota_watch reboots.
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
  frame from anyone). The MT7668's CSI commands are accepted but the chip firmware sends no data.  RCPI1 where RCPI0 is
  255 (cannoli's second antenna receives).  Modversions kernels (athena, cannoli): `devices/<codename>/Module.symvers`
  from `tools/kcrc.py` (boot.img's `__kcrctab`, checked against stock modules).  Cannoli (Android's common kernel 5.4):
  `HM_DRIVER_SYMS` (no `kallsyms_lookup_name` export: the driver's own symtab via `find_module`; the `bl` written
  through a `vmap` alias), `KMOD_INIT` 0xfc (`tools/kmod-init.py`: its `struct module` has `init` 36 bytes on).
- **Speaker gain** (`spkgain.c`, setting `speaker_gain`, page only, not exported): +0..+12 dB on the ALSA control
  `board.speaker_gain` (Dot 2 / Echo 2: the codec's `HP Driver Gain Volume`, stock 6; Dot 3: the TAS2770 amplifier's
  `DAC Playback Volume` = AMP_LEVEL, stock 16 of 0..20 in 0.5 dB steps (`speaker_gain_steps` 2), so +2 dB at most:
  `spkgain_max()`/`settings_max()` give the page the control's top; Show 5: the RT5616's line out `OUT Playback Volume`,
  stock 29 = -3 dB of 0..39 in 1.5 dB steps, `speaker_gain_steps` 2 per `speaker_gain_db` 3, a whole dB takes the
  nearest step; Dot 5: the MAX98396's `Speaker Volume A`, stock 10 = +14 dB of +4..+21, offered to 16), written by
  hassmic itself (group audio). The Dot 2's
  HAL writes stock at every mixer start (and per its XML on a jack change; the Dot 3's never writes it): a thread on the
  card's control events writes ours back when it sees stock, and leaves other values (the jack's 11) alone.
  `HASSMIC_FAKE_SPEAKER_GAIN=<file>[:<stock>:<steps>:<max>[:<per dB>]]` for tests (`tests/unit/spkgain_test.c`, `fake_web.py`).
- **Push updates**: `ota.c` receives bundles on the device; `src/tools/otatool.c` verifies/unpacks there (monocypher).
  The PC side is `scripts/otatool.py` (stdlib only: keygen, pack/sign, verify/install, push, adb, plugin; same CLI), so the PC
  needs no compiler for it; `tests/otatool_test.sh` holds the two byte for byte to each other, and CI verifies every
  bundle with both. `scripts/bundle.sh` packs (ota-push.sh and CI).  The port (TCP 28929; `HMOTA-PUSH1` signature then
  bundle, `HMOTA-ADB1` and `HMOTA-PLUG1` by challenge; ota.c's head argues each bound): a thread per connection,
  `CONNS_MAX` 8; before a signature of its own was checked (`pconn.pre`, ended by `keyed`, for a bundle by
  `body_done` in one hold with its place) `PRE_ADDR` 2 per source
  address and `PRE_MAX` 4 in all, a newcomer over that ends the oldest that holds no bundle's place (`pconn.body`:
  a push is before the key for all of its body; `body` is the place's own word, set and cleared with it under `cl`,
  `ubody.who`: a takeover clears it, and the signature's read looks at the generation too; told "FAILED busy ..."
  first, then `shutdown`); a refused connection
  gets "FAILED busy ..." and stays half open `PARK_MS` 1 s so the line arrives (`park` in the listener, `refuse` in a
  thread), and both otatools print it (they read the answer also when their upload was cut off).  Times as a whole:
  the request line 5 s, a challenge's answer 10 s; a body (`body_read`) `BODY_STEP` 64 kB in every 10 s counted anew
  from each step (a burst banks nothing) and all of it within 10 s + size at 32 kB/s.  A bundle is in memory before
  its signature can be checked (EdDSA over all of it): `BODIES_MAX` 2 such, one per address (`body_take`, places
  with a generation), a newcomer taking the slowest one's place unless that took `BODY_FAST` 256 kB or more in its
  last one to two seconds or is younger than `BODY_YOUNG_MS` 2 s (a grace once per address: none within
  `BODY_AGAIN_MS` 10 s of that address's bundle that ended unsigned by its own doing, `spent[]`/`spend`: the floor,
  a close, a bad signature, or its place taken while nothing came from it; not a place taken while it was
  arriving; a connection the listener ended, `pconn.ended`, takes no place, and the listener ends none that holds one); a bundle that
  arrived whole keeps its place while its signature is checked (`body_whole`, `ubody.whole`;
  `HASSMIC_OTA_CHECK_DELAY=<ms>:<file>` for tests; `HASSMIC_OTA_STAGE_DELAY=<ms>:<file>` holds the gap before a
  handoff is marked); a package only after its challenge.  `tests/fake_ota_port.py` (strangers from 127.0.0.x
  source addresses).
- **Prebuilt** (`scripts/lib/build.sh`, `build_binaries`): deploy/install-system/ota-push/setup take the device binaries
  from the release CI published of HEAD (tag `v<commit time>[-beta]` pointing at HEAD, clean tree; else the release
  CI found HEAD's bundles alike to, `_ci_same`: its build run on HEAD passed with "Publish" skipped, the newest
  release published before its "Anything new" step ended, issue #132; else, CI not done, the newest release tag before HEAD when nothing since touches a bundle,
  `_bundle_free`: CI publishes no beta of such commits, issue #106; bundle verified against `keys/release.pub`, marked `build/<codename>/PREBUILT`) where there is no NDK,
  or with `PREBUILT=1`;
  `PREBUILT=0` always builds. `scripts/probe.sh` compares with `devices/<codename>/probe.md5`, not the unpacked firmware.
- **Online updates** (`update.c`): "Online updates" on the settings page (off default / beta / release, `state/config`) and an
  ESPHome update entity; checks GitHub's releases API, downloads `hassmic-<board.codename>.bundle` through the firmware's
  libcurl (`fwcurl.c` `fwcurl()`: the one loader for update.c, https.c and davs.c, dlopen and the library's first init
  under its lock; a load that failed is tried again 60 s later at the earliest, one that lacks a function never;
  a new user calls it, never dlopen; sockets via `net_socket` so the egress lock lets them out; `update_fetch` is
  update.c's one GET, also plugin_fetch.c's; every fetch follows redirects to HTTPS only, the API's and the
  bundles' say `hassmic/<version>`, update_fetch's only `hassmic`), hands it to root through `ota_handoff`.
  Root (`main.sh` ota_watch) accepts the owner's `update.pub` or the release key (`release.pub` of the running copy, else
  the system partition). Channel only from an approved browser on the page (no HA entity for it), install only over the keyed connection.  The page switches a channel on
  only once `update_probe` reached GitHub (web.c `updates_gate`, before `core_lock`; else `github_unreachable` in the
  answer and a warning under the row).
- **Sources** (`sources.c`, `sources.h`, issue #175): the System page's "Where software comes from": the online
  updates' server (a repository on GitHub, or Forgejo/Gitea by its host: `<root>/api/v1/repos/<o>/<r>`) and the plugin
  index's site, each the project's or the owner's, in `state/sources` (not exported, not in HA, gone with a reset).  A
  server alone at once (root still checks release.pub/update.pub, the index key still decides).  For a server of the
  owner's a key of the owner's (beside the project's) or no check, only by an approved browser plus the action button
  (web.c `W_SOURCE`; `sources_widens`; less trust at once), the maintainer's decision although root then trusts a key
  hassmic names (docs/SECURITY.md says so): `state/update-key` (32 bytes or empty) -> main.sh `page_pub` ->
  `/data/local/hassmic/page-update.pub` (`HASSMIC_PAGE_UPDATE_PUB`), the third key of ota_watch's loop and of
  `ota_handoff`'s `own`; no check = hassmic's own key `state/update-own.key` signs what update.c downloads (no .sig
  fetched).  The push port takes update.pub only.  Plugin index: `pi_source` in main() (plugin_index.c: the owner's
  key after the built-in ones, `PI_KEYS` 3, `pi_keys_builtin` for 5.2's rules; no check: `pi_signer` takes any 64
  bytes for the two lists, never for a package's listing (purpose ""), plugin_fetch.c `get_sig` a missing .sig as
  zeros; least serials 0 for its lists and the owner key's listings, the build's for a built-in key's
  (`pi_least_listing`); no member revocation lists with a key or no check, `pi_source_trusts`; a built-in key's
  listing never relisted to the owner's); any change restarts hassmic, one of what is trusted also forgets `.serial`,
  `.revoked`, `.revoked-at`, `.listed` at the next start (mark `state/sources-forget`, `sources_load`).  `sources_apply(expect, to)`: only over what the caller judged against.  `/api/state` `sources` (level 0/1/2), `POST /api/sources`; app.js
  `sourcesCallout` on every page (yellow a server, red a key or no check), `sourceDialog`.
- **Media over HTTPS** (`https.c`, issue #30): `proto_esphome.c` `http_get` hands `https://` to `https_open`, which
  runs the firmware's libcurl (`fwcurl()`, as update.c; BoringSSL, system CAs) in a thread writing the body into a socketpair: the
  decoders and the cancel's `shutdown()` work as on a plain fetch.  Setting `https_verify` (default on; off takes any
  certificate).  HA with `ssl_certificate` hands out its external URL only (`get_url`: prefer external, no IP), so
  that name must resolve and be reachable from the LAN.  `HASSMIC_CA_FILE` for tests.
- **Light ring** (`ring.c`, `ring.h`, issue #26): HA's light "Light ring" and the listening ring turned towards the
  talker over other animations.  ledcontroller loads animations only at its start from `/system/etc/led-resources`
  and plays only names in a layer config (each `layer_config*.json` is a compositor of its own, so ours go into
  stock's file).  `hassmic -G dir` (root, `main.sh leds()` at each satellite start, `-G -` = digest only) makes 2422
  files: active-start/its last frame/active-end turned by each LED (`hassmic_listen|hold|end_N`, first in layer 4),
  the light's palette (24 hues x full/half + white, 3 levels, solid/pulse/spin/blink, rainbow; colours as seen, gamma
  2.2 to the driver's duty as Amazon's own; last in layer 0) and each LED alone in each solid colour (issue #57:
  `hassmic_led_<i>_<colour>`, a layer of its own per LED, 0..11, stock's moved up to 12..17 by `relayer`: the
  compositor weighs each pixel by its brightest channel, so only lit patterns light a part of the ring; HA's "Light
  ring fill", "Light ring rotation" (`ring_rotation`) and action `light_ring_leds`; no room for their 9.7 MB:
  `HASSMIC_RING_LEDS=0`, the set without them, `ring_per_led()` 0), plus
  stock's config with them added and a digest; `sysinstall.sh leds` installs them (stock's config kept as
  `/system/etc/layer_config_common.json.pre-hassmic`, digest `/system/etc/hassmic-leds.digest`), ledcontroller
  restarts.  hassmic lists the light only when the installed digest is its own.  Designs (issue #38, `ringdesign.c`,
  page `web/ring.js`): up to 100 animations in `state/ring/<id>`, per slot of `ring_slots` (every pattern `led()`
  plays) Amazon's or one; Apply -> `state/ring-applied` + `state/leds-request` -> root's ota_watch runs `leds()` (-G
  reads the applied file, O_NOFOLLOW; the request is renamed into root's folder before it is read) -> `leds.result` ->
  `rd_tick` -> `ring_reload` (shown patterns set again).  What is confirmed installed is `state/ring-installed`
  (`installed()` promotes the applied file when its digest is the set's); a draft never installed goes back (`rd_start`).
  Listening/end turned by each LED at install (`hassmic_s_listen|hold|end_N`), others `hassmic_s_<slot>` just before
  stock's name.  Slot ids are stored: keep them (a new slot goes last in both `ring_slots`, ring.c's and ring_band.c's, with its row in web/ring.js `RING_PLACES`).  Theme packs (`state/ring/packs/<id>`, `/api/ring/pack*`) are folders
  of the library: `has` lines (a design in one pack at most) and `use` lines naming only those (version 2,
  `pack_normal`); shared with their designs inside (version 1: the page's `.ringpack` and copies).  The page: ring.js.  Every pattern of the core goes
  through `led()` -> `ring_set`/`ring_unset`, which knows from each file how long it shows; while one does, HA's light
  is off.  The Echo Show's band (`ring_band.c`) has the same slots and draws designs itself, flattened and never
  turned (band position p = LED p + 5: the talker's pair in the middle): Apply needs no root there
  (`ring_designs_live`), and `ring_stock` gives the page the band's own patterns as files.  Every ledctrl call (patterns and brightness) goes through `ledq.c` (issue #40): one at a time, in order,
  each waited for, in its own thread (started together, an unset can overtake its set and leave the pattern on);
  sets and unsets queued one behind the other go in one ledctrl (`LEDQ_BATCH`: it takes repeated -s/-u).
  Binary animations (ACS, the Echo Dot 5; found by stock's `acs_animation_list.txt`): `<index>.<name>.animation_bin`
  (big-endian "animagic"/"totl"/"loop"/"fram", 12 RGB pixels, donut's text byte for byte), index files rewritten with
  ours after stock's (`acs_*`, `layer_config_common.txt` = the JSON flattened; stock's kept as `.pre-hassmic`,
  `sysinstall.sh leds`).  Its LED manager on the SCP holds every animation from ledcontroller's start and refuses an
  init past ~400 (a failed one leaves it dead until a reboot), so no palette: Home Assistant's light is drawn when it
  changes into one of two slots (`hassmic_light_a|b`, `light_apply_acs`) through `src/ledshim/ledshim.c`
  (`libhmled.so`, device.mk `LEDSHIM`), preloaded into ledcontroller (abstract socket `@hassmic-led`,
  `aceLedMgr_updateAnimationData`; the daemon's uid, `hassmic_` names only).  Root runs that ledcontroller itself
  (`main.sh` `ledc_start`/`ledc_watch`): a copy without its file capability, else the linker ignores LD_PRELOAD.  The
  beam: `com.amazon.ledd BeamDirection` via liblipc (dlopen), polled while listening (`hassmic_hold_N` when it moves).  PC: `HASSMIC_LED_ROOT` (for /system/etc), `HASSMIC_FAKE_LED` (ledctrl calls into a file),
  `HASSMIC_FAKE_BEAM`.
- **Light ring live** (`ringlive.c`, `ringlive.h`, feature `ring_live`, off): hassmic writes the LED driver's frame (the Dot 5: below)
  itself (`board.ring_frame`, donut's, biscuit's and radar's IS31FL3236 `0-003f/frame`: 12 x `RRGGBB` text, LED 0
  first, on donut scaled by the unit's calibration in the driver; ledcontroller writes it only when its picture
  changes; radar matched to biscuit by firmware, not probed; PLAN.md "Light ring live",
  `scripts/led-probe.sh`).  Root's `main.sh` `led_frame` gives that one file to group `LED_GID` 3991 (device.conf
  `LED_FRAME`, the same path; `tests/unit/ringlive_conf.py`), which runas adds to hassmic's groups; led_current, the
  calibration, boot_animation and pwm1 stay ledcontroller's/root's.  Colours as seen -> gamma 2.2 (as ring.c's
  designs) -> x the brightness table's pwm for the ring's level (`led_mute_ring_config.json` rgbTable by idme
  `capcolor`, as the HAL's scaleDutyCycle), so never above what ledcontroller writes at that level (FD at 100).  One
  writer thread, latest wins, `RINGLIVE_FPS` 30; waits while `ring_busy()` (the Echo's own patterns) or muted, back
  150 ms after; reads back every 200 ms and puts ours back, backs off 5 s if something else keeps writing; nothing
  left: the dark frame once.  HA (`proto_esphome.c`): lights "Light
  ring LED 1..12" (from the fill's start + rotation), actions `light_ring_frame|animation|design|stop`, select "Light
  ring design" (`rd_names`, re-listed when the names change: `rd_generation`, main.c `live_names_check`).  Layers:
  "Light ring" (ring.c keeps its state; `ring_light_live` makes ring.c play no palette pattern, `ring_light_now`;
  `ringlive_effect`: the palette's five and `RING_LIVE_EFFECTS`; the six music ones `ringlive_music_effect` from
  `musiclight.c`: the PCM fed by sendspin.c, a2dp.c and main.c's play_media path with its output's queue (the TTS
  stream's modelled in `playback_thread`: written minus real time, as `s_queued_us`); 12 biquad
  bands against running ranges, the level and L/R as punch against 1.5 s, kick/snare/hi-hat from log-amplitude flux
  with level gates, tempo by autocorrelation, a beat grid locked to the kicks (phase by circular mean, then a PLL), the
  accent every 4 beats; each reading stamped when heard; only while a music effect shows, `musiclight_want`.  Measured
  by `make unit-ringviz`: tools/ringmusic.py's track with its drums written down, build/ringviz, tools/ringviz.py
  (scores, and effects.png to look at)), the LED lights over it, an animation over all; HA's
  transition (LightCommand 14/15, else `LIVE_FADE_MS`) as a fade in seen space (`ringlive_fade`).  SIGTERM and
  `ringlive_shutdown`: the dark frame, never while the Echo's own pattern shows (main.c `led()` and the mute bracket
  `ring_set` with `ringlive_yield`/`_yield_end`: no frame of ours starts in between and one in flight is waited for
  (`write_lock`), so ledcontroller's frame for a static pattern lands after ours; the handler leaves the ring alone
  until the writer looked; only in hassmic's own pid, not a child forked without exec); init's stop kills outright (a
  moving effect stays frozen), so `state/ring-live` (the boot id; written once a run when ours first shows, never per
  frame: /data is flash; deleted at a clean end) has the next start in the same boot make the ring dark at
  `ringlive_begin` (main.c after settings_load: the mute and the Echo's own known), unless `live_hold` then.  Page: `POST /api/ring/live` (a design's text, as drawn; empty: stop), `live` in
  GET /api/ring, ring.js `liveShow`.  PC: `HASSMIC_FAKE_FRAME`, `HASSMIC_FAKE_ALSPARAMS`; `tests/unit/ringlive_test.c`,
  `tests/fake_ha_ring_live.py`.
  The Echo Dot 5 (binary animations, `ring_live_acs`; picked before `board.ring_frame`, not with `HASSMIC_FAKE_FRAME`):
  no frame file, the LEDs are the SCP's LED manager's.  Each frame goes as one looped frame into ring.c's slot
  `hassmic_live` (layer 0 with the light's slots, auto-stop 0): `ring_live_put` (the frame, `shim_update` on the light's
  connection under `shim_lock`), `ring_live_show` (-s/-u queued; set once per
  ledcontroller: `live_on`, `live_put_conn`, `shim_conns` counts connections made and lost), unset when
  nothing of ours is left and by `ringlive_yield` before the Echo's own pattern (the compositor would mix ours into its
  gaps).  Full duty: ledcontroller gives the LED manager its brightness tables (`prepare_brightness_tables_for_acs`).
  No read back, marker or SIGTERM frame: `ring_start` unsets the slot.  `shim_check` (ring_tick, `ring_live_on`): the
  connection hung up = ledcontroller ended, the light and the live frame drawn again into the next one.
  An update stops the animation it replaces (measured), so libhmled.so wraps ledcontroller's
  `aceLedMgr_startAnimationWithConfig`/`stopAnimation`/`stopAllAnimations` (preloaded: its HAL's PLT calls land there)
  and keeps `hassmic_live` as a pair with `hassmic_live_b`: a frame into the one not showing, started with
  ledcontroller's config, the other stopped; ledcontroller only knows `hassmic_live`.  ~47 ms a swap: ~20 frames a
  second.  The shim takes 4 clients, `?leds` reads what each LED shows (`aceLedMgr_getLedState`: on, brightness,
  RGB); `src/tools/ledstate.c` (not in `all`; `runas puffin aipc ledstate [-n N -i MS] [-p RRGGBBx12]`).  ledctrl -g
  is stale on the Dot 5: trust `?leds`.  `tests/fake_ha_ring_live_acs.py`.
- **Screen UI** (`src/ui/`, `screen.c`, `screen.h`; device.mk `SCREEN`, else `screen_none.c`; every Show): the
  Echo Show's own screen, small beside any app (Kiosk Satellite, View Assist...).  `src/ui/ui.h` lists the parts:
  `scene.c` (display list with a clip per item, diffed by id into one dirty rectangle; text cache by hash; image cache
  by key and shown size, asked of the platform once; touches: taps, long presses, HIT_DRAG sliders, scrollers with
  flings and rubber band, a shade's hits block what is under them; the top edge = the drawer once the finger moves
  down, a tap there is still a tap; over an app only from the grip (y < 0: 16 rows along the top that, like the
  band's, no app has, overscan; a pull from the app's own top refreshed Kiosk Satellite); the band along = Back over an
  app; an app launched leaves the last frame up (`leaving_until`, the app's picture under the drawer) until it is in
  front, polled every 150 ms meanwhile (`scene_waiting`); frames only while something moves),
  `widgets.c` (icons drawn from shapes, buttons, sliders, keyboard, action sheet, toast), `face.c` (the face for the
  core's patterns, mouth on the reply's level, clock with HA's tz, bottom line), `drawer.c` (pulled from the top over
  anything: over an app a SurfaceControl.screenshot under it and hassmic's home activity brought up, closed with
  nothing chosen it relaunches that app; Home, Face, Music, DND, Screen off, what plays, volume; no apps), `music.c` (rail: Playing from Sendspin's metadata/artwork/controller, Queue, Library with
  drill-down, Search with the keyboard, Speakers with groups and volumes; lists by "ml" requests, pages of 60).  The
  home app: the page's Screen page (`POST /api/screen`, `state/screen_home`; the apps the UI listed at start, "app"
  lines), else Kiosk Satellite where installed ("kiosk", root's `kiosk-app` mark), else the face; a new choice applies
  at once; relaunched 3x a minute at most, not while our own screen was asked for.  One app_process: `scripts/device/screen.dex`
  (`tools/mkscreen.py`; `ui_start` puts the library's folder on `LD_LIBRARY_PATH`, else the class loader namespace
  refuses `/system/hassmic`, and sets the real group to the effective one, else AT_SECURE drops it, issue #122) loads `libhmui.so` (`android.c`: JNI table C/J/F, JV/CV for some SDKs only (Android 9:
  SurfaceControl.Builder, getTasks(int)), checked by `mkscreen.py --framework`, `make unit-screen` against every
  `SCREEN` model's unpacked firmware for what its SDK resolves; `tools/dexasm.py` reads Android 7's oat and Android
  9's compact dex in vdex, hidden API bits off; images
  decoded on a thread of their own); layer shown only while hassmic's home activity is in front (getTasks polled);
  touches from evdev beside Android (`touch.c`: the panel's node, lowest event number, system_server's uinput
  `amazon_touch` last; protocol B, the first finger followed; `tests/unit/touch_test.c`).  The band runs in the same process (`ring_band.c` -> `screen_band`, one row a
  frame; ringbar.dex only without the UI and in tests).  `screen.c` keeps state by generation and one writer thread
  sends what changed (callers hold `core_lock`: never block on the pipe), plus mass.c's lines from a FIFO (8 MB, a
  new UI drops it and `mass_replay` tells the state again); hassmic's pid in a file's name while a UI is meant to run
  (restarts included, until given up), `state/screen-ui.<pid>` (root lists the folder, never opens it), which makes
  root's `kiosk_tick` leave opening the app to the UI.  PC: `tests/unit/ui_test.c` (every incremental frame == the full one,
  gestures, commands), `tools/uishot.c` (`make build/uishot`: every view as PNG with the firmware's Roboto, FreeType),
  `HASSMIC_SCREEN` (a stand-in UI).
  Words and timers (`src/ui/words.c`, issue #104; settings `screen_text` "Speech on screen", `screen_text_seconds`
  (8, 0 until the next request), `screen_timers`, all off, the page's Screen page, card "Words and timers"): what was
  said, the reply and the timers on a fourth layer of the UI process (`hassmic-words`, TRANSLUCENT, over the UI's and
  every window, under the band; hidden while the drawer is out), drawn only on a change (a timer: once a second), faded
  and pulsed by `SurfaceControl.setAlpha`; text by the platform (`p_text`, wrapped by `p_measure`).  hassmic's side in
  `screen.c` (`screen_words`: `core_set_state` clears on a run from idle and marks idle, proto_esphome's STT end only
  after `core_transcript` passed it, TTS start; proto_wyoming's transcript; `stop_word` clears what it ended; nothing
  kept while off), lines `wo`/`wh`/`wr`/`ws`/`tl` (scene.h); the timers are the core's (`core_timers_running`,
  `screen_timers_changed`), ringing ones the face's `timers`; `screen_keep_on` holds the screen on (ring_band.c's
  timeout) while timers show.  Touch: over our own screen the scene gives the layer the touches while it shows
  (`scene_touch` `wtap`, `words_tap`, only once faded in past half: words away + "words clear" to hassmic, timers
  hidden until the next "tl" + "timers hide", which lets the screen sleep until they change, Stop -> "timer stop");
  over an app the platform grabs the panel for them (`EVIOCGRAB`, `touch_grab`, `scene_words_wants_grab`; taken and given
  back with no finger down; the kernel lets go when the UI dies), else (no grab: `wd.grab` -1) a ring there brings our
  screen up (`ring_from`, `own_asked` kept and given back after, asked again every 10 s) and the app back after.  Tests: `ui_test.c`, `tests/fake_screen_words.py`, `build/uishot` (words*, timer*).
- **Music Assistant's API** (`mass.c`, `mass.h`, `json.c`): the screen's library, search, queue and speakers over
  MA's own websocket (`/ws`, port 8095; the page's URL, or with none given the Sendspin server's address pinned when the
  token is saved: a token only ever goes to that address, `mass_peer` merely suggests one, from sessions whose player
  role is active; without a token mass.c may look at the suggested one to say what it is); the first
  message is the server's info, from schema 28 `auth` with a long-lived token (page: `POST /api/mass`, sealed as the
  Wi-Fi password, `state/mass` 0600, never sent back); events after it.  This Echo = the player whose id is Sendspin's
  client id (`sendspin_client_id`) or has it as an output protocol; its queue by `get_active_queue`.  Digested into the
  UI's lines (scene.h: ma, pl, pq, qc, L/I/E, img, merr), `pl` written only when it changes; group with set_members
  (group/ungroup where unknown), images from `/imageproxy/<proxy_id>?size=` (80/160/256/512, `fmt=jpeg`, the token
  along).  `json.c` is a token reader (no DOM copies, depth 64; `tests/unit/json_test.c` fuzzes it).
  `tests/fake_ma_api.py`: a stand-in MA server.
- **Light band** (`ring_band.c`, the Echo Show's `RING` in device.mk instead of `ring.c`): the same `ring.h` drawn as a
  band along the bottom of the screen, every pattern as the Dots' ring unrolled (12 virtual LEDs blended across the
  width, the talker's side in the middle; never turned: the Show's beam says too little, docs/re-checkers.md) plus
  HA's light, one at a time by priority.  `scripts/device/ringbar.dex` (`tools/mkringbar.py` on `tools/dexasm.py`,
  next to the binary, `HASSMIC_RINGBAR`) runs as app_process: overscan for the band, a SurfaceFlinger layer over it,
  "ready <w> <rows>" on stdout, then frames (w x rows ARGB) on stdin.  Setting `light_band` (rows, 0 off).  Screen
  power as stock's: `service call power` 9 (wakeUp) on a pattern, `screen_off_timeout` 5 min / 1 min muted or DND.
  PC: `HASSMIC_FAKE_BAND` (a program speaking ringbar's protocol), `tests/unit/ring_band_test.c` (designs too, a restart
  on the same state included); `tests/unit/band_map_test.py` holds web/ring.js's band drawing to ring_band.c's, pixel
  for pixel (node).  The band's height comes from `state/config` before `ring_start` (`settings_peek`).
- `src/include/`: headers for the reversed Amazon libraries (`mixer_api.h`, `pryon_api.h`, `aipc_api.h`) and `netio.h`.
- `src/tools/`: standalone device tools (`mixcap`, `mixplay`, `slplay` (the Android models' mixplay: OpenSL ES on the music stream), `pryon_test`, `aed_test`, `latency`, `runas` — AIPC refuses uid 0 and
  the image has no `su`; `curlspy`, `hciscan`, `a2dpprobe`, `scoprobe`, `ebcprobe`, `dha_test` not in `all`).

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
must stay in that range (UDP 28932 Drop In too). Exceptions: UDP 2330 (Kiosk Satellite's fixed port), which `lockdown.sh` admits only while
`state/config` says `arbitration_mode=kiosk` (`kiosk_state`), and, where device.conf has `KIOSK_PACKAGE` (and the board `kiosk`, for the settings), the app's own TCP 2324 and 6053 from private addresses while it is installed, and with `kiosk_dlna` on its DLNA renderer (`kiosk_dlna` in `lockdown.sh`: the TCP port of the app's own setting `flutter.ks.dlna.port`, read through `runas` as its user, while its uid listens there per `/proc/net/tcp*`; UDP 1900 to 239.255.255.250 only while its uid alone holds 1900; issue #109); its uid may also reach the internet unless `kiosk_internet` is off (`KSUID`). Ping (ICMP echo request) is admitted from the private
and link-local ranges of `LOCAL4` only. A stock rule hassmic comes to depend on (INPUT or OUTPUT) goes into `keep`, worded as `iptables -S`
prints it. Plugins' users (6900–6907, `PLUGIN_UID`/`PLUGIN_SLOTS`) get nothing out at all: one DROP rule a user, first in `hassmic_out`
after adbd's and before `-o lo`; a plugin's sockets are hassmic's (plain = local only, `net_socket` = anywhere, for the server the owner entered).
Plugins' own UDP ports (28940–28999, `PLUGIN_PORTS`, plugin_store.h `PS_PORT_FIRST/LAST`; net.udp, net.discover) are
admitted from the private and link-local ranges only: chain `hassmic_in` (`rules_in`: `KSRC` RETURN, then DROP),
jumped from INPUT's second rule (`IN_JUMP`, Wi-Fi interface), checked by `wrong()` as the rest; each socket's own
locked BPF filter holds the same where the chain is missing (the PC).  What comes in from a plugin's port and the
Echo's own address (a broadcast the kernel hands back to the Echo's own sockets) is dropped by chain `hassmic_self`
(`rules_self`: `-s <address>/32 -j DROP` for every address of the Echo's own, `own4`: `/proc/net/fib_trie`'s "/32 host
LOCAL" entries but loopback's (no `ip` on the Dots, no `xt_addrtype` in any model's kernel), read at every look), jumped from INPUT's first rule (`SELF_JUMP`,
`--sport 28940:28999`), checked by `wrong()` (both jumps the first two of INPUT, in that order).

`tools/speechcorpus/` makes a speech corpus of OmniVoice's designed voices into `testdata/corpus/` (Git LFS, CC BY-NC: its README;
DEVELOPMENT.md "Speech corpus"): checked by STT, words timed by MFA; look there before making speech for a test.
`tools/wakealign/corpus_eval.py` measures the replay's start on it (stock Pryon under qemu, the release's rule, the
aligner, adapted) and `tools/vad_leadin.py` HA's VAD with and without the lead-in (build/ha-venv).
`tools/` is PC-side reverse-engineering and firmware tooling (`payload_dump.py`, Thumb disassembly helpers,
`davs-fetch.py` for extra wake word models, `arbsim.py` for arbitration's wait estimator, `kcrc.py` (a modversions
kernel's CRCs out of its boot image) and `kmod-init.py` (Wi-Fi motion's module),  `vol-curve.py`: an Android Echo's
volume curve with a Dot's micRaw beside it, for `board.volume_curve`; `cold-lead.py`: what a sound loses on a closed
output, a PC microphone beside it, for `board.cold_lead_ms`).

## Conventions

- Feature commits update `CHANGELOG.md` (user-facing, dated, plain language), `PLAN.md` (status, measurements) and
  `docs/GUIDE.md` when behaviour visible to users changes. Keep the README short: a row in its table or a figure, the
  detail in the guide.
- Code comments explain *why* with device facts and measurements; match that style and density.
- Proprietary/derived/secret material is git-ignored and must stay out of git: `firmware/` (per model:
  stock image, unpacked rootfs, unlock zips, `re/` disassembly), `toolchain/`, `build/`, `device-logs/`, `secrets/`, `*.bin`, `*.zip`.
- Third-party code in `src/third_party/` keeps its own licence (freeaptx is LGPL, statically linked).
