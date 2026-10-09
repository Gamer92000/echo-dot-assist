# Echo Dot 3 (donut) → Home Assistant voice satellite

Other models: `devices/` (layout and porting guide in [devices/README.md](devices/README.md)); this plan is the donut's.

Goal: keep Amazon's audio front end (`mixer` + `libasp`: AEC, beamforming, mic calibration, speaker path),
replace the Alexa client (`PuffinApp`) with a small daemon that talks to Home Assistant.

Firmware analysed: `donut_puffin` NS65741 / Fire OS 6574.1. Findings: [docs/FINDINGS.md](docs/FINDINGS.md),
[docs/re-pryon.md](docs/re-pryon.md), [docs/re-platform.md](docs/re-platform.md).

Legend: `[x]` done, `[~]` partly done (note says what is missing), `[ ]` open. **(device)** = needs the physical Echo.
"Done" in phases 0–2 means done on the PC. Device unlocked + rooted 2026-09-21 (kamakiri-donut, boot-root); satellite installed and in daily use since, reachable over Wi-Fi (adb, push updates).

## Phase 0 — Workspace (no device)

- [x] Unpack OTA `payload.bin` (`tools/payload_dump.py`)
- [x] Extract rootfs into `firmware/rootfs/`, partition images into `firmware/images/`
- [x] Map audio stack: services, binaries, libraries, configs
- [x] Cross toolchain: Android NDK r21e in `toolchain/`, API 24, armv7, linker lld
- [x] Run device binaries on the PC: `tools/qrun.sh` (qemu-arm + PID namespace, see FINDINGS)
- [x] Disassembly helper that resolves string references: `tools/annotate_thumb.py`
- [x] Findings document `docs/FINDINGS.md`
- [x] `.gitignore` for proprietary blobs and toolchain

## Phase 1 — Reverse the interfaces (no device)

- [x] `libmixerAPI.so` prototypes: open/get/release/drain/flush/pause/resume/close/getters, record and playback
- [x] `libmixerAPI.so` client registration (`DataTrans` ring + descriptor file in `/data/mixer_streams/`)
- [x] C header `src/include/mixer_api.h`
- [x] `mixer` single-client logic and mic gating. Known: log string proves eviction of the previous `micAsr` client; — on device: `AllowMic` is 1 without PuffinApp, `micRaw` can be captured beside `micAsr`
      mic mute comes from LIPC `com.doppler.buttond/muteState`. `AllowMic` is 1 by default without PuffinApp (Phase 3)
- [x] `mixer` playback stream types: `TTS`, `Earcon` and `Music` streams mix; volume per stream type (`MainVolume` for Music and Earcon,
      `TTSVolume` for TTS). hassmic ducks music itself, so the mixer's own ducking rules were never needed and stay unread
- [x] `libpryon.so` prototypes, structs, call sequence (`docs/re-pryon.md`)
- [x] C header `src/include/pryon_api.h`; result fields and `detectionType` values verified by running the library
- [x] LED ring: `ledctrl` CLI, pattern names, state mapping
- [x] Volume: `audio_manager_set_prop` / LIPC `com.doppler.audiod` properties
- [x] Buttons: input device, key codes, `gpio-privacy` driver
- [x] What depends on `PuffinApp`: nothing restarts or reboots; `ledcontroller` is what starts it
- [x] Firewall: inbound only TCP 16384–32767 → daemon port 16700
- [x] OTA services and how to disable them

## Phase 2 — Build the tools (no device)

- [x] `Makefile` linking against stock libs in `firmware/rootfs/system/lib`
- [x] `mixcap`: `micAsr` → stdout / TCP (builds; cannot run off-device)
- [x] `mixplay`: stdin / TCP → `TTS` stream (builds; cannot run off-device)
- [x] `pryon_test`: stock "Alexa" model over a raw file
- [x] `pryon_test` under `qemu-arm`: **detects "Alexa"** in `testdata/alexa_espeak.raw` (Accept, score 0.937)
- [x] `hassmic` daemon: capture + Pryon wake word + Wyoming server + playback queue + LED states
- [x] Protocol test against reference `wyoming` 1.10.2 library: `tests/fake_ha.py` passes (host build)
- [x] Same test with the ARM build and real Pryon under qemu: `tests/fake_ha.py --qemu` passes
- [x] Host-side PoC script `scripts/poc-host.sh` (reference `wyoming-satellite` on PC, Echo only moves audio)
- [x] Deployment scripts: `scripts/deploy.sh`, `scripts/probe.sh`, `scripts/capture-test.sh`, `scripts/device/alexa-{off,on}.sh`
- [x] Action button = manual trigger; mic-mute latch blocks triggers and drives the LED (`src/hassmic/buttons.c`)
- [x] Volume buttons → `MainVolume` + LED volume step (switch off with `-V` if a stock daemon already does it)
- [x] Barge-in: wake word or button while speaking cuts TTS and opens a new pipeline (tested: 0.10 s on PC)
- [x] Cancel while listening/thinking (user request 2026-10-03, as a VPE's center button): button sends
      `VoiceAssistantRequest start=false` (ESPHome's `voice_assistant.stop`; HA `_abort_pipeline` cancels the pipeline task,
      LLM and pending tool calls), wake word the same plus a new pipeline. Events of the aborted run still on the wire are
      dropped until the new run's RUN_START (`proto_esphome.c` `cancelled`); a streamed reply already being fetched has
      its socket shut down and `media_busy` released by `cancel()` itself (`media_job.aborted`): the fetch can wait on HA
      for seconds and held the slot against the next run's reply (found in review, test fails without it); releasing it
      only when the thread noticed still lost a reply that came at once (release CI, 2026-10-04). Button during listening/thinking wins over music pause.
      PC: `fake_ha_esphome.py`. Device 2026-10-04 (radar, kitchen Echo): button and wake word while thinking work (user)
- [x] Wake earcon: generated blip on the `Earcon` stream (`-E` disables). Since 2026-09-23 Amazon's own sounds from the system image
      (`src/hassmic/sounds.c`: WAV, or MP3 through minimp3, mixed to mono): `ui_wakesound` for the wake word, `ui_wakesound_touch`
      for the action button, `state_volume_adjust_tone` for the volume keys, `state_privacy_mode_on/off` for mute; the blip stays
      the fallback where a file is missing (PC build). Only `ui_endpointing` (end of listening) is left out: the image has it as
      Ogg Vorbis only and there is no decoder for that on board
      User: ~30 % of button sounds silent. Cause: `MixerDrain` returns at once, so the stream was closed milliseconds after it
      was opened; the mixer learns of a stream by inotify on `/data/mixer_streams/` and opens the shared ring a moment later,
      by then unlinked (`Mixer_DataTrans:InitFailed:reason=shmOpenFailed,errno=2`, 4 of 10 opens in a burst). `play_earcon`
      now waits until the mixer starts consuming the ring (`MixerGetNumBytes` falls), then until it is empty, and reopens a
      stream the mixer never picked up. 70 sounds in bursts of ten: every ring opened by the mixer (checked per stream in
      the `ShmService` log, the mixer's own lines get dropped by logd for chattiness), no retry needed. Streams that live
      seconds (TTS, music) never hit this; the mixer also plays out what is queued after a close
- [x] Mic after a hassmic (re)start: the mixer delivers nothing on the new `micAsr` client for ~15 s (`InCapture-GetReadBuff:
      retcode=110` every 1.5 s from 21:46:22 to :36 after the 21:46:17 open), then data flows. Same picture when `mixcap` opened
      right after hassmic was stopped and gave up after 17 s. So each push update means ~20 s without wake word.
      **Cause (2026-09-23):** after `AddNewStream` for a record stream the mixer's `AlgoMetadataDecoder` connects over AIPC to
      `perfmonitord` (`/dev/aipc/182`, CPU usage for its corrupted-frame metric) and only opens the HAL record stream after
      `pollConnect: epoll_wait timeout with timeout_ms: 20000`. `lockdown.sh` stopped `perfmonitord` as a cloud daemon. With
      it running the mic opens 2 ms after the stream is added. `lockdown.sh` leaves it running now (~0.1 % CPU)
- [x] mDNS: `scripts/device/hassmic.service` for the stock `avahi-daemon` (`/data/misc/avahi/services/`)
- [x] `scripts/device/run.sh`: Alexa off, mDNS on, daemon in foreground

## Phase 2b — Never contact Amazon

Order matters: lock egress first, join Wi-Fi second. `netmgrd` runs a captive-portal HTTP check on connect and a
dozen daemons phone home within seconds.

- [x] Survey of what phones home (Amazon host names in `PuffinApp`, `dacd`, `gadgetsd`, `assetmgrd`, `otad`, `logmgr`, ...)
- [x] `scripts/device/lockdown.sh <lan-cidr> [watch]`: LAN-only egress (IPv4 + IPv6), stops cloud daemons. Applied on device with
      `10.0.0.0/8,172.16.0.0/12,192.168.0.0/16` (comma list works); `mixer`, `netmgrd`, `wifisvc` stay up. Tighten to the real subnet after DHCP
- [x] `scripts/device/wifi-join.sh`: join via stock `wpa_supplicant`, refuses to run without lockdown (untested). PC wrapper
      `scripts/wifi-join.sh` pushes git-ignored `secrets/wifi.conf` (line 1 SSID, line 2 passphrase), SSID sent as hex
- [x] **(device)** Never-registered device, USB/ADB only → `lockdown.sh` → `scripts/wifi-join.sh`: joined IoT SSID (no-internet VLAN),
      192.168.100.147/22, lock tightened to `192.168.100.0/22`, `ping 1.1.1.1` fails, `lockdown.sh ... watch` running.
      Stock INPUT drops ICMP (PC ping fails) but TCP 16700 is reachable from the PC (VLAN if `enp5s0.2`).
      adb works over Wi-Fi too: stock init sets `service.adb.tcp.port 5555` and stock `firewall.sh` opens the port, so
      `adb connect <ip>:5555` from a PC on the Echo's VLAN is the root shell. Verified 2026-09-22. Wrong at the time: "same
      key authentication as over USB". boot-root sets `ro.adb.secure=0` and the idme `fos_flags` say `noadbauth`, so
      there is no authentication at all (issue #1); closed since 2026-09-30, see "adb over Wi-Fi closed" below.
      Wi-Fi Direct group `p2p-p2p0-0` (left by `oobed`) removed with `wpa_cli -i p2p0 p2p_group_remove`; now in `alexa-off.sh`.
      Do not stop the supplicant: the same `wpa_supplicant` process runs `wlan0`
- [x] **(router)** Echo is on the user's no-internet IoT VLAN — covers the boot window before `lockdown.sh` runs
- [~] **(device)** Verify: `tcpdump`/router log over one full reboot shows zero non-LAN packets. No `tcpdump` on the image (`NFLOG`
      target exists if packet-level proof is wanted). 2026-09-22 after 16 h uptime: `hassmic_out` counters 93 packets / 5.6 kB dropped
      IPv4, 0 IPv6 (the Echo has no global IPv6 address), 80k packets to 192.168.0.0/16 passed. logcat names the source: `tokend`
      posting to `api.amazon.com`, plus DHCP hands out 8.8.8.8 as second DNS. Not covered: the boot window before `lockdown.sh`
      (router-side only)
- [x] **(device)** Persistent: rules in `/system/bin/debug_firewall.sh` (stock `firewall.sh` runs it after its own flush) plus init service for `lockdown.sh watch` — solved differently: init service `hassmic_fw` runs `lockdown.sh watch` from `on boot`
- [ ] Clock: no NTP once locked down. Nothing in hassmic needs wall-clock time (timers are Home Assistant's, hassmic only rings;
      the log carries no timestamps). Only if timestamps in `boot.log` are ever wanted: point `sntp` at the router

## Phase 3 — First contact **(device)**

Run in this order. Each step says what it proves.

- [x] `scripts/probe.sh` — libs identical to analysed firmware (2026-09-21) — records stock state; flags if device libs differ from analysed firmware
- [x] Confirm SELinux state: `getenforce` = Enforcing, but adb shell runs in permissive `u:r:su:s0`; children keep that context
- [x] `logcat` while saying "Alexa" on stock — baseline for mixer log lines — obsolete: stock Alexa never ran registered; our own detector is verified instead
- [x] `scripts/deploy.sh`, then `adb shell /data/local/hassmic/alexa-off.sh` — `mixer` stays up
- [x] Wait 10 min: no reboot, no service restarts — device ran for hours across the session without reboots; `wifisvc` link resets found and fixed (Phase 5)
- [x] `scripts/capture-test.sh 10` — 16 kHz mono confirmed by data rate (32 kB/s); `AllowMic` = 1 without PuffinApp.
      **AIPC refuses uid 0** → all mixer clients run through `runas puffin aipc,audio,...` (`src/tools/runas.c`; no `su` on device).
      `MixerGetRate/NumCh/SampleSizeBits` return -1 on record handles. Still open: listen to the WAV
- [x] `mixplay` test tone: audible at `MainVolume` 50 (scale 0–100; 7 was too quiet to notice). `audio_manager_set_prop MainVolume N` works as root.
      Volume buttons are dead once the Alexa stack is stopped → `hassmic` must handle them (do not pass `-V`)
- [x] AEC test (pink noise + tones at `MainVolume` 50, room quiet): residual converges from −41 dB to −65 dB (noise floor) in ~10 s. Good.
      Open: reproducible low-frequency thumps (80–120 Hz, ~100 ms, up to clipping) in `micAsr` at playback start, ~3–4 s in, and
      ~1.5 s after playback ends. A `Silent` keep-alive stream does not change them → not HAL/amp standby. Need to know if the
      thump is audible from the speaker (acoustic) or only in the capture (AEC/reference glitch). Retest with speech-like TTS audio.
      User listened (2026-09-21): **no audible thump** → capture-side only (AEC/reference path or our ring-buffer read). Check `micRaw` next.
      2026-09-22 attempt over Wi-Fi: `micRaw` is 16 kHz mono like `micAsr` and can be read beside hassmic, but the Echo hung on an
      external speaker through the 3.5 mm jack (internal speaker silent, mic peak 330 at volume 80): no thump can show, not decided.
      Also seen: `mixcap` right after killing the `micAsr` client gets no data on either stream (`status=110`, getters -1 on
      `micRaw`); the mixer stops the mic path when its client dies, unlike after a clean start. Retry with the internal speaker.
      **Resolved 2026-09-22** (internal speaker, volume 50, 440 Hz tone + espeak sentence): recorded from inside hassmic
      (`kill -TTIN <pid>` toggles a dump of the post-AEC stream to `state/capture.raw`, the only way to read `micAsr` while it runs)
      with `micRaw` beside it. No burst at playback start, end, or after; the AEC takes the tone below the noise floor (rms 255 raw
      → 9), speech leaves residual peaks of rms ~140 for a second. Five control dumps at rest: flat. The old thumps came from
      `mixcap` captures that had just opened: a freshly opened record stream starts with zeros and one garbage block (seen again
      in `micRaw`, first 100 ms clipping), not from the AEC
- [x] `pryon_test` on device: canned file Accept (type=2) at 1.39 s; live `mixcap | pryon_test` 6/6 spoken "Alexa" accepted, no near-misses
- [ ] `alexa-on.sh` restores Alexa without reboot

## Phase 4 — End-to-end PoC **(device)**

- [x] `scripts/poc-host.sh <ip>` with HA-side wake word: superseded, the local wake word worked end to end before this was
      needed. Script kept for isolating the audio path from the daemon if that ever comes up
- [x] `run.sh` runs detached on device (log `/data/local/hassmic/hassmic.log`), listening on 16700, PC can connect. mDNS not seen from
      the PC. Added in HA by IP `192.168.100.147`, port 16700.
      **2026-09-21: end-to-end works** — "Alexa" → HA pipeline → TTS reply on the Echo, user verdict "works perfectly"
- [x] LED ring states look right: listening / thinking / talking / idle / error / muted / volume — confirmed by the user during the ESPHome tests (listening / thinking / talking / mute / volume / timer)
- [x] Buttons: `getevent -l /dev/input/event3` works beside `acebuttond`; volume keys not handled twice — volume, action and mute buttons verified; mute comes from `/dev/input/event1`
- [x] Barge-in acoustically: the detector hears "Alexa" while speech plays on the `TTS` stream (4/4 on the device, also during HA's own
      reply), so neither the mixer nor the AEC is in the way. It was ignored because `barge_in` doubled as "already being cut" and as
      "continue conversation", which HA sets for every reply ending in a question. Fixed (cut is now keyed on `flush_playback`), test
      added that fails on the old core. Installed; user: "perfect now" (log: wake → barge-in → listening during a reply)
- [ ] Wake-word accuracy at distance and with music playing: ten tries from across the room, count `wake:` lines in `boot.log`,
      once in silence, once with Music Assistant playing (threshold hints are in place, see "Stop" below)
- [~] Wake word "Echo": firmware ships only `ALEXA` (+`STOP`) in `words.shrunk.txt`. Stock gets other keywords from DAVS (cloud) into
      `/data/.../speech/wakeword_models/davs/resources/`. Options: pull an ECHO model set from another source, or non-Pryon engine
      (microWakeWord on device / openWakeWord on HA via `-w remote`)
      - [x] DAVS route, done 2026-09-21: `MODE=stock-online` in `hassmic.conf` (`main.sh` + `lockdown.sh ota-only watch`) = stock
            Alexa with internet, hassmic off, only the updaters cut off (`otad` + `ace_otad` share uid `ace_otad` → owner-match DROP;
            `update_engine` kept stopped). Verified on the device through registration and OOBE: nothing downloaded, slot and
            build unchanged. The app sits on "updating" for a while (day-0 OTA check that never answers); harmless
      - [x] Real request seen with `src/tools/curlspy.c` (LD_PRELOAD in `assetmgrd`, `scripts/device/davs-spy.sh`):
            `GET https://api.amazonalexa.com/v2/deviceArtifacts/?artifactFilter=<quoted base64 JSON>` + `Authorization: Bearer`;
            answer = JSON with a signed CloudFront `downloadUrl` (expires in minutes). The `/v3/segments/` strings in
            `libacsdkDavsClient.so` are not what this build uses (404). `tools/davs-fetch.py <map.db> <key> [locale]` does the same
            from the PC: fetched echo/computer/alexa/amazon/ziggy de-DE and echo/computer en-US into `device-logs/models/`
      - [x] `echo-de-DE` (1.4 MB, `ECHO` + `STOP`) with stock `libpryon.so` under qemu: 2/2 espeak "Echo" accepted (type=2).
            Installed in `/data/local/hassmic/models/echo-de`, `ARGS="-m …/pryon.manifest"`; hassmic starts with it
      - [x] **(device)** spoken "Echo" live through hassmic with `echo-de-DE`: in daily use since 2026-09-21 ("echo stop works")
      - [ ] **(device)** `echo-en-US` (5 MB, NTT fusion) live. Loads under qemu after all (2026-09-22,
            the earlier "insufficient permissions" on `ntt.cfg.json` did not come back) but gives only a type=0 near miss on espeak
            "Echo" where `echo-de-DE` accepts; the NTT fusion models want a real voice, test on the device
      - [x] Models across Echos (2026-09-28): the DAVS sets are account-independent files, so one fetch serves every Echo.
            Under qemu the NTT fusion sets (`alexa-de-DE`, `echo-en-US`, `computer-en-US`) fail on every model with
            "insufficient permissions" on `ntt.cfg.json`, but load on the real donut (`pryon_test` on 192.168.100.147:
            both load, `alexa-de-DE` scores espeak "Alexa" type=0): a qemu artefact, so compatibility is checked on the
            Echo. The plain sets (`echo/computer/amazon/ziggy-de-DE`) load under qemu with donut's, biscuit's and radar's
            engines, `echo-de-DE` detects espeak "Echo" (type=2) on all three. radar's engine lists `wakeword_ecids` up
            to 35 (donut, biscuit: 37) and throws on the NTT sets under qemu: `davs-fetch.py --ecids` asks with the
            Echo's own list, which `scripts/wakeword.sh` reads from `pryon_test`'s attributes line
      - [x] One keyword per pick (2026-10-08, GitHub issue 14): DAVS's en-US computer/amazon/ziggy is one multi-keyword
            set (`words.shrunk.txt` AMAZON COMPUTER HEY_DISNEY STOP ZIGGY; `op.cfg.json` lets each wake from sleep, no
            switch in the set), so with "Computer" picked "Amazon" and "Ziggy" woke the Echo too (reproduced on biscuit;
            espeak-free, a real voice). `main.c` `wake_open_word`: the keyword is the set id up to the language, applied
            only if a `words.shrunk.txt` of the set (top or one folder down) lists it; `on_wake` drops others but STOP.
            Also HA's ESPHome select keys options by name: of `computer-de-DE` and `computer-en-US` ("Computer" both) it
            showed one and picked the later; a name two sets share now carries the id's region.
      - [x] DAVS token without stock Alexa, for downloads from the settings page (2026-10-06, `docs/re-davs-login.md`,
            probe `tools/davs-login.py`): the Echo's own code pair login gets as far as the code entry on amazon.de/code,
            then `/auth/register` answers `InvalidDevice` (the missing piece is the device attestation token).
            **Built since** (2026-10-06, same day): `dha.c` builds that token exactly as MAP does (biscuit and radar,
            verified against libace_map.so instruction by instruction; the key's TEE session needs group `drmrpc`, now
            in every model's `DAEMON_GROUPS`) and
            `davs.c` does the whole login from the page (code shown with the link, register polled with the token,
            tokens in `state/davs` 0600 never sent to the page, refresh, deregister with a fresh token, the registration
            kept on its own Amazon when a login on another one fails or is cancelled), asks DAVS with this engine's own
            ecids (`wake_attributes`), downloads the tar.gz (streamed to disk, capped as artifacts.c, free space checked
            as it arrives), unpacks it (PAX headers and directory entries skipped, unpacked size and free space checked;
            two levels of folders kept), and stages it through artifacts.c's checks incl. pryon_test and root's installer.
            artifacts.c, the page's Echo-to-Echo copies and `artifact-install.sh` keep two levels of folders too (files
            named `sub/file`, `sub/sub/file`, 127 characters, 128 files; links and deeper folders refused, root still
            reads as the daemon's user). Two levels and 128 files since 2026-10-08 (GitHub issue 14): every en-US set
            (alexa, echo, computer, amazon, ziggy) and alexa-de-DE carry `nttfusionconfig/ntt_conv/` etc. and 67-69
            files; with one level and 64 the page refused each ("a file this Echo cannot keep"). The PC path
            (`davs-fetch.py`, adb push) never had a limit. Page card
            "Download from Amazon" (Echos section) in three steps: the Amazon site (every one with Alexa, by region,
            guessed from the browser language), the code (countdown, cancel), the picker (wake words of a language,
            whisper, sound detection; what is installed, each download's progress and outcome, kept over the reload
            after installing). Tests: `tests/unit/dha_jwt_test.c` (byte for byte), `tests/fake_web_davs.py` (53 checks
            against a fake Amazon: token shape, session header, ecids, region, whisper's own request, every file of each
            set installed byte for byte, folders, a deeper one refused, other sites, cancel, a revoked token at logout,
            an Echo that cannot attest, the Dot 3's drvV3 login and download), `tests/fake_web_artifacts.py` (a whisper set with its folder copied Echo to
            Echo, a set two folders deep with 69 files copied and installed; root refusing a link in a folder and three levels); the card checked in headless Chromium.
            **Against the real Amazon (2026-10-06, biscuit, amazon.de): works.** Register answers `401 Unauthorized`
            until the code is entered, then the tokens; no device secret needed (none sent); Amazon names the device
            after the account whatever `device_name` says; the Echo's clock was a day behind and the token's `dat`
            passed; deregister answered 200. Downloaded and installed: computer, amazon, ziggy (de-DE) and the sound
            detection model. alexa-de-DE (`BDPGeneratedFiles/`) and whisper (`whisper_components/`) were refused for
            their folders, hence the folder support. Found on the way: `untar` never skipped a file's padding, so every
            set arrived as its first file only (the fake's pryon_test passed it; the real one would have refused).
            Not done: the folder sets from the real Amazon, sites other than .de, the Alexa app login route
            **donut's drvV3 attestation reversed and built (2026-10-09):** the Dot 3 proves itself with the dhav2
            certificate its EC key in the TEE had Amazon sign (`/persist/dha_certificate.pem`, HAL field 0x203):
            `dha.c jwt_v3` builds the header `{"typ":"drvV3","alg":"ES256","x5c":["<the PEM's body, CR and LF
            dropped>"]}` and a payload with a cpuid literal of donut's MAP (`dfae219fe47947c7`) and no "cust" part,
            signed like drvV1 but over the DER ECDSA the HAL answers, split into 32-byte R and S (the JWS raw form;
            the token carries the certificate, ~1.9 kB). The HAL's public key tells the shapes apart: an RSA modulus
            parses on biscuit and radar, donut's EC SubjectPublicKeyInfo does not — biscuit's and radar's HALs hold a
            dhav2 certificate file too (same NS65741 family), unused by their MAPs, so the certificate alone cannot
            tell them apart. The TEE (`/dev/trustzone`, 0660 system:drmrpc = gid 1026 on the device) and the
            certificate file (`/persist/dha_certificate.pem`, 0660 keystore:keystore = 1017) need two more groups →
            `drmrpc,keystore` in donut's `DAEMON_GROUPS`, as puffinmrmd's list has them (without keystore the HAL
            answers field 0x203 with -1, measured). Checked on a real Dot 3 with `dha_test` as puffin plus those
            groups: the EC SPKI, the CRLF PEM at 0x203 (682 bytes, cut at the END marker), a 71-byte DER signature.
            Tests as drvV1's (both shapes byte for byte) plus a third Echo in `tests/fake_web_davs.py`
            that logs in and downloads with the drvV3 token. **The sign-in against the real Amazon is still
            untried**: `docs/re-davs-login.md` has the analysis if it refuses.
      - The spied `assetmgrd` must run in its own SELinux domain (`runcon u:r:assetmgrd:s0`, shim labelled `system_file`, log in
        `/data/davs`): from the `su` domain its AIPC service is unreachable and the Alexa app shows the device as unavailable
- [x] Assistant replies ignored the volume: the mixer keeps one volume per stream type, the `TTS` stream follows `TTSVolume`, and
      only `MainVolume` was ever set. `core_set_volume()` now sets both, and `TTSVolume` is synced once at start. Pushed
      2026-09-22, user: works. Untouched: `AlarmVolume`, `NotificationVolume`, `SystemVolume`.
      Then found `MainVolume` 80 / `TTSVolume` 50 on the device (moved from outside): hassmic now polls both every 2 s, adopts an
      outside `MainVolume` change (reported to HA and MA) and pulls `TTSVolume` back in line. Verified 2026-09-22 over adb
- [x] "Stop": Amazon's models report `STOP` only in the `awake` state of `op.cfg.json` (175 frames after the wake word; same in
      the firmware's ALEXA model and the DAVS sets), so it is "<wake word>, stop". hassmic treated every keyword as the wake
      word and reset the engine (= back to `sleep`) when a reply was cut: "stop" was lost, or opened a prompt after an alarm.
      Now: `STOP` never starts a pipeline; it silences an alarm, cuts a reply without listening again, or drops the pipeline
      the wake word opened up to 4 s before (HA's "nothing heard" for it stays quiet); no engine reset for 3 s after the wake
      word. 4 cases in `tests/fake_ha_esphome.py` (SIGHUP = "stop"). Pushed 2026-09-22, user: "echo stop works".
      Open: bare "stop" with a changed `op.cfg.json` (loads; synthetic test inconclusive, not tried with a voice); the models
      lower the wake word threshold through client properties `AlarmState` / `AudioPlayerState` / `audio_playback`: set since
      2026-09-22 (`PryonDecoder_PushClientEvents`, reversed in `docs/re-pryon.md`; verified under qemu: ECHO threshold 0.751 → 0.452
      with `AlarmState`; `pryon_test -p name=value`). hassmic: alarm ringing, Sendspin stream, reply playing. Pushed 2026-09-22, user: works nicely
      The `pryon WARN ... Invalid bitmask frame indices` line in `boot.log` is one per wake word in every version since 0.0.2: harmless
- [x] Latency wake → STT start; TTS playback glitch-free — replies start before TTS_START with streaming TTS; user verdict fine

## Phase 5 — Make it permanent **(device)**

- [x] init service for `hassmic`; keep `puffin`, `puffinmrmd`, `dacd`, `smarthomed` off across reboots.
      Written, **not installed**: `scripts/system/{hassmic.rc,boot.sh,sepolicy.rules}` + `scripts/install-system.sh <lan-cidr>` (via TWRP;
      `/` is dm-0 and cannot be remounted rw live: wrong, only toybox cannot, see the live system writes below). Services run with `seclabel u:r:su:s0`; needs `allow init su process transition`
      in `/sepolicy` (that file is the active policy; boot-root's vendor path does not exist here). Patched copy was test-loaded
      live with `magiskpolicy --live`: kernel accepts it. Kill switch: delete `/data/local/hassmic/hassmic.conf`.
      **Installed 2026-09-21 by the user; verified after reboot:** `hassmic` + `hassmic_fw` running from init in `u:r:su:s0`, hassmic as
      `puffin`, Alexa services + `oobed` stopped, `hassmic_out` first in OUTPUT, HA reconnected on its own ~60 s after power-up.
      `magiskpolicy` aborts inside TWRP (even with `/system` bind-mounted) → installer patches the policy under the running OS
      and only copies it in TWRP, with md5 checks on base and transfer
- [x] Adopted Echo offered again as "discovered" (radar, 2026-09-28): the boot-time avahi service file had hassmic's
      placeholder MAC 02:00:00:00:00:01 (radar's wlan0 appears after main.sh ran), Home Assistant keys ESPHome devices by
      MAC. main.sh now waits for the wlan address (max 120 s, hassmic keeps starting) and gives the service directory to
      DAEMON_USER, so hassmic's own rewrite on a key change (mdns_refresh) works on the device too (it could not write
      there before). avahi's host name was "linux"/"linux-2" (shown by HA as "Küchen Echo (linux)"); now the node name,
      via a copy of avahi-daemon.conf with host-name. Checked by hand on radar: `k--chen-echo.local`, real MAC
- [x] Setup voice prompts + orange spinner at boot (unregistered device): played by `uxeventd`. `boot.sh` now stops `uxeventd` and
      `oobed_*` at `on boot` and again after PuffinApp is stopped. Reinstalled + rebooted 2026-09-21: user confirms no audio, no LED at boot; `uxeventd` stays stopped, device stable
- [x] Volume buttons felt dead: step was 3 %, `volume_step-NN` patterns were never unset (they loop black forever and pile up),
      start value hardcoded, key repeat counted. Now 10 % steps, start value read via `audio_manager_get_prop`, previous pattern
      unset, cleared after 2.5 s. Installed; user verdict: fine. Installer made idempotent for re-installs (patches from
      `/sepolicy.pre-hassmic`, skips the policy write when the installed md5 already matches)
- [x] Disable OTA: `otad`, `ace_otad`, `update_engine` stopped at every boot by `alexa-off.sh`/`lockdown.sh`, egress firewall + no-internet
      VLAN block the hosts. Not removed from the image, by design: the uninstaller must be able to give the stock device back,
      and three layers (stopped, owner-match DROP, no-internet VLAN) are enough
- [x] Wi-Fi provisioning without the Alexa app: `scripts/wifi-join.sh`, profile persists in `wpa_supplicant.conf`, rejoins after reboot
- [x] ESPHome native API (`src/hassmic/proto_esphome.c`, default; Wyoming stays as `-P wyoming`): voice pipeline with TTS over the API
      connection (16 kHz), announcements + `play_media` as WAV over HTTP (HA transcodes to the advertised 48 kHz mono), timers
      (alarm until button / wake word / 60 s), volume both ways, wake-word config, generated mDNS file (`hassmic -S`).
      Port **26053** (stock firewall admits inbound TCP 16384–32767 only). `tests/fake_ha_esphome.py` against `aioesphomeapi`:
      20/20. With real HA (2026-09-21): discovery, voice pipeline, settings entities, mute confirmed by the user.
      Settings entities (saved in `/data/local/hassmic/state/settings`): mic level (since 2026-09-29, see "Mic gain"; replaced
      noise suppression, auto gain and mic volume multiplier, which HA ignored), wake sound; mute switch.
      Mute: button = hardware latch, reported by the `gpio-privacy` input device (`/dev/input/event1`, not the keypad); software can
      set the latch (`enable` <- 1) but not clear it (write 0 rejected while set, second 1 does not toggle; DT has one output, one
      input) → HA switch = soft mute, shows latch OR soft, button unmute clears both.
      Encryption (2026-09-23): Noise_NNpsk0 like ESPHome, key provisioned by HA itself (DeviceInfo 19 supported + 26 provisionable,
      `NoiseEncryptionSetKeyRequest` 124 over a zero-PSK Noise connection, HA core `esphome/manager.py`
      `_handle_dynamic_encryption_key`), stored in `state/api_key` (600), empty key clears. Plaintext and zero-PSK refused once a key
      is set; a plaintext connection from before is closed on its next request. mDNS TXT `api_encryption_supported=` / `api_encryption=`,
      service file rewritten by hassmic (dir is 777, avahi republishes on change, both checked on the device). `fake_ha_esphome.py`:
      17 more checks with aioesphomeapi 46.4.1 (the version HA pins). Real HA (2026-09-23, pushed): plaintext -> zero-PSK -> key set ->
      old plaintext connection closed -> two plaintext retries refused -> encrypted, voice assistant subscribed; mDNS shows api_encryption.
      Found on the way: every push update left one more `lockdown.sh watch` running (18 on the device; `main.sh firewall` re-runs
      by exec without stopping the old one), and the satellite's one-shot `lockdown.sh` rebuilt the chain concurrently with the
      watcher (duplicate rules seen). Now `main.sh firewall` kills earlier watchers first and the satellite runs
      `lockdown.sh services` (daemons only). Verified after three pushes: one watcher, clean chain.
      Egress for hassmic (2026-09-23): `lockdown.sh` RETURNs `-m owner --gid-owner 3990` in both tables before the DROP; the owner
      match checks the socket's fsgid. First try, 3990 as primary group: playback fine, but the mixer refused recording
      (`InCapture-GetReadBuff:retcode=110`, wake word deaf for ~50 min). Saved group does not survive exec. What works:
      `runas -r 3990` = real group 3990, effective aipc; `net_connect` switches the thread's fsgid to the real group around
      `socket()`. Children get plain aipc first (`child_ids()`: mksh would make the real group effective, which AIPC refuses), so
      the volume read is fork/exec instead of popen. On the device: no retcode=110, capture has audio, wake word ECHO/STOP accepted,
      volume reads right; `net_connect` to a public IP hits DROP without `-r`, the gid rule with it. Note `micAsr rate=-1` in the
      log says nothing: it also shows on working starts. DNS still needs the resolver rule (netd resolves, not hassmic).
      **Installed permanently 2026-09-21**, verified after reboot: ESPHome on 26053 from `/system/hassmic`, own avahi in `su` domain answers
      mDNS queries, HA reconnected, settings file read, no test binary left in `/data`.
      First real announcement (2026-09-21) failed twice over: HA's media URL host (`192.168.0.3`, main LAN) was outside the
      `/22` the egress lock allowed, and the URL was MP3 (HA bypasses its transcoding proxy for TTS and asks the engine for
      WAV only after a pipeline has run). Fixed in the repo: `lockdown.sh` allows all local addresses (RFC 1918, link-local,
      multicast, IPv6 link-local/ULA) and takes no CIDR any more; MP3 decoding with vendored minimp3 (CC0), format sniffed from
      the first bytes. Test has an MP3 case (21/21). Installed; real announcement from HA verified in the log 2026-09-21:
      chime as 48 kHz WAV through HA's proxy, then TTS as 24 kHz MP3 from `192.168.0.3`. `play_media` from Music Assistant verified 2026-09-21 (MA serves `…:8097/flow/….wav`; first failed because the router only let the
      IoT VLAN reach HA on 8123 — fixed by the user on the network side, not in hassmic). Timer with real HA verified ("Stell einen Timer für 10 Sekunden": started → finished → alarm ringing → wake word
      silences it). 
- [x] Wi-Fi drops: Amazon's `wifisvc` runs HTTP connectivity tests against AWS hosts (`AceNetSvc_HttpTest ... unreachable`); behind the
      egress lock they fail and it rebuilds the link (seen ~100 s after boot, link down 193 s; explains earlier stray
      "client disconnected/connected" pairs). Verified live: with `wifisvc` stopped, `wpa_supplicant` + `dhcpcd` keep the link and
      survive a forced disconnect/reconnect. `boot.sh` `netwatch`: stop `wifisvc` once there is an address, start it again only after
      60 s without one. hassmic: 5 s send timeout + TCP keepalive on the client socket, so a dead link no longer blocks `core_lock`
      and the single client slot. Installed 2026-09-21: `netwatch: link up, wifisvc stopped` at boot, link stable through the
      user's tests. Long run 2026-09-22: 13 boots in `boot.log`, `wifisvc` stopped at each, never started again by `netwatch`,
      link up 16 h without a gap
- [x] Reply quality: SPEAKER flag dropped (feature flags 61). HA then renders TTS in the media player's announcement format (48 kHz
      mono WAV) and sends the URL (RUN_START with streaming TTS, TTS_END otherwise); hassmic fetches it like an announcement, starts
      at INTENT_PROGRESS `tts_start_streaming=1` when offered, and reports VoiceAssistantAnnounceFinished. Before: 16 kHz over the
      API connection. Test 22/22. Installed; with real HA 2026-09-21: reply arrives as `tts_proxy/….wav`, 48 kHz, playback starts before
      TTS_START (streaming), follow-up question re-opens the mic (continue conversation). User verdict: fine
- [~] Sendspin player (`src/hassmic/sendspin.c`, port 28928, mDNS `_sendspin._tcp`, `-z 0` disables): dialect of aiosendspin 9.1.1 = Music
      Assistant 2.10.4, which differs from the published spec in 22 places (`docs/sendspin-digest.md` §0). WebSocket (`ws.c`), Noise
      KKpsk2 responder (`noise.c`, Monocypher), Sentinel PSK + unpaired access (operator approves in MA), player@v1 PCM 48k stereo,
      Kalman clock filter, scheduler (hard snap, then ≤6 frames/chunk nudging), volume/mute/static delay, ducking under the
      voice assistant. `tests/fake_ma_sendspin.py` against the reference server: 9/9 (4.000 s of 4.000 s played, no discontinuity,
      clock ±0.07 ms). `make unit`: WebSocket vs aiohttp, Noise vs python noiseprotocol, hash vectors.
      **Real Music Assistant 2026-09-21:** MA found the Echo by mDNS, dialled 28928 over its IoT-VLAN interface (no router rule),
      admitted it unpaired without an extra step, music plays; sync err ≈ +0.3 ms vs schedule, clock ±0.27 ms over Wi-Fi, 2 MB buffer
      full, 30 frames nudged in the first minute, hassmic 8 % CPU. Output latency measured with `src/tools/latency.c` (clicks in
      the Music stream found in `micRaw`, which can be captured beside hassmic): 83–88 ms incl. capture path → default 70 ms.
      Controller role: action button pauses the group while music plays and resumes it (within 30 min), otherwise it wakes the
      assistant; local volume changes are reported. Pairing: `pairing_psk` token flow (`hassmic -T` or the boot log prints the
      token; fresh long-term PSK, stored only after `server/pair-finalize`, up to 8 records), in-band re-handshake (prologue =
      previous handshake hash, hello again), `server/unpair`. Servers: one thread per connection, arbitration at the first
      activate (playback > pairing > idle; idle tie only for the last-playback server), loser gets `another_server` /
      `concurrent_attempt`. `tests/fake_ma_sendspin.py`: 17/17 against aiosendspin 9.1.1.
      Codecs: FLAC (vendored `dr_flac`, fed chunk by chunk through its read callback), Opus (the firmware's own `libopus.so`, one packet
      per chunk), PCM; preference `flac,opus,pcm`, override with `HASSMIC_SENDSPIN_CODECS`. Test passes with each (`CODEC=… tests/fake_ma_sendspin.py`).
      Device with real MA: MA picks FLAC, hassmic stays at 8 % CPU, sync err ≤ 0.3 ms; decoded queue sized for the 30 s horizon (8 MB) after
      the PCM-sized limit dropped FLAC audio silently; paired session resumes directly with the long-term key after a reboot.
      Real MA 2026-09-21: token pairing done by the user (sentinel → pairing key → long-term key), action button pause/resume works.
      Radar with real MA 2026-09-28: played without the token (unpaired access, as designed then). Switch "Music Assistant without
      pairing" (2026-09-28, state/sendspin.unpaired, default off since the same day, user's call): off = hello says `unpaired_access.enabled:false`, a Sentinel/pairing-key
      session that declares playback or roles gets `client/goodbye pairing_required`, an admitted unpaired one is cut off.
      `tests/fake_ma_sendspin.py`: approved unpaired → no player role; paired with the token → plays (19/19)
      The pairing token is also an ESPHome text sensor ("Sendspin pairing token", diagnostic, disabled by default) so it can be copied
      from Home Assistant.
      Not implemented: PIN pairing (CPace: SHA-512 + Elligator2) → refused with `pair/abort method_not_supported`;
      metadata/artwork roles (no display). Installed and in daily use. Missing: listening test against another synced player in the
      same room (trim with MA's static delay)
- [x] mDNS: init's `avahi-daemon` runs in SELinux domain `avahi-daemon`, which is denied read on `/data/misc/avahi/services`
      (so nothing was ever published, also not for Wyoming). `magiskpolicy` cannot parse a rule for a type with a hyphen →
      `boot.sh`/`run.sh` stop the init service and start avahi themselves in the `su` domain. Verified: answers queries from the PC.
      Host name is `linux.local` (system host name is `localhost`); HA connects by IP and follows the MAC in the TXT record
- [x] Push updates over Wi-Fi (`scripts/ota-push.sh <host>`), so TWRP is needed once per device only.
      `/system/hassmic/boot.sh` is now a stable bootstrap: runs `/data/local/hassmic/ota/current/main.sh` (root-owned, installed from a
      signed bundle) or the factory `main.sh`; an update that fails to start 3 times is skipped (`ota/tries`, reset after hassmic
      ran 60 s). Bundles: `src/tools/otatool.c` (pack + EdDSA sign on the PC with `secrets/update.key`; verify + unpack on the Echo as
      root with `/system/hassmic/update.pub`). hassmic (puffin) only receives on port 28929, pre-checks the signature and leaves a
      request; `ota_watch` in the firewall service re-verifies, installs, restarts the satellite service and re-execs itself.
      `/data/local/hassmic` is now root 755 (was 777), `state/` puffin 700. `tests/ota_push_test.sh`: good / foreign key / tampered /
      garbage. **On the device 2026-09-21:** bootstrap installed, two real pushes over Wi-Fi (`OK 0.3.0+…`, services back in ~10 s,
      previous version kept). The first push found two bugs, both fixed: unpacked directory was 0700 (root umask 077) so the
      daemon's user could not start hassmic, and the fallback did not cover a daemon crash loop (now: self-check as `puffin`
      before switching, and five quick exits → factory copy at once). Rollback tested with two signed, deliberately broken bundles:
      one that cannot run → refused at install, current version untouched, hassmic never stopped; one that passes the self-check
      and then exits → factory copy running again 26 s later, push port open, HA + MA reconnected; then the real version was
      pushed again over Wi-Fi
- [x] ESPHome API serves up to 4 clients at once (one thread each): replies to the asker, entity states to every state subscriber,
      voice assistant traffic to its one subscriber (first come, first served, like ESPHome firmware). Test with two `aioesphomeapi` clients
- [x] Diagnostics in Home Assistant: SoC temperature (thermal zone `mtktscpu`, 44 °C idle) and CPU usage (`/proc/stat` delta) as
      sensor entities, diagnostic and disabled by default, pushed every 30 s to state subscribers. Other zones on the SoC:
      `skin_virtual`, `case_virtual`, `wifi_temp`, `therm0..3_s`; load average sits around 6 with hassmic at ~8 % CPU (Amazon's
      daemons make the rest)
- [x] Mute sounds: the ESPHome satellites play theirs from the device firmware, Home Assistant sends nothing for it, and hassmic
      only set the red ring. Now Amazon's own `state_privacy_mode_on/off.wav` (48 kHz stereo, on the system image under
      `/system/local/share/earcon/base/`) play on the `Earcon` stream when the effective mute changes: hardware button always,
      HA switch once the satellite runs (not when the saved setting is restored at start). The "Wake sound" switch silences them too
- [x] Log: `boot.log` rotated by `boot.sh` at 1 MB (copy + truncate, because several long-lived processes append to it; one old
      part kept as `boot.log.1`); `ledctrl` / `audio_manager_set_prop` stdout no longer logged (two lines per LED change before)
- [x] Bluetooth proxy (2026-09-24, `src/hassmic/ble.c`): no kernel Bluetooth stack (no BlueZ, no HCI sockets); the MT8516's combo
      chip is `/dev/stpbt` (MediaTek WMT, H4 framing, `bluetooth:net_bt_stack 0660`), driven by Amazon's `btmanagerd`
      (`ro.btstack=blueangel`, started on `audio_mixer.init=true`). The node opens a second time beside `btmanagerd` without
      complaint, so both would lose events: `alexa-off.sh` stops it and hassmic waits for `init.svc.btmanagerd` to leave
      `running`. Controller: HCI 4.2 (version 8), address = `/proc/idme/bt_mac_addr`. hassmic resets it, scans while an API
      client is subscribed and forwards raw advertisements (feature flags 97 = passive scan, raw advertisements, scanner
      state and mode; HA sets passive mode itself). Measured (10 MB adb pull over Wi-Fi during active scanning): no scan
      4.3 MB/s, 30 ms window / 320 ms 3.8, 80/160 3.1, continuous 0.6–1.6; advertisements received were about the same
      (~240/s), so ESPHome's Wi-Fi default 30/320 it is. ~2700 advertisements from 29 devices in 15 s. A daemon killed while
      scanning leaves the chip scanning: startup drains for at most 300 ms, resets, resyncs the H4 stream.
      Connections (same day): GATT client in `ble.c`, feature flags 103 (+ active connections, remote caching: HA caches
      the database and writes CCCDs itself), 3 slots, scanning paused while a connection is set up. Controller: LE ACL
      32 x 251 bytes. Checked with aioesphomeapi against two SteamVR base stations (MTU 23: discovery of 6 services in
      1.3 s, 64-byte value by Read Blob, both connected at once, CCCD write + read back) and a BlueZ peripheral on the PC
      (MTU 517, 500-byte write + read back, 505-byte notification reassembled from 251-byte ACL packets, indications,
      write without response); an absent address fails after 20 s with error 8. Not exercised: prepared writes (needs a
      device with a small MTU and a >20-byte write). Not done: connection parameter requests from HA.
      `BTSinkPlayer` still runs idle
- [x] Bluetooth pairing (2026-09-24, `ble.c` + `ble_crypto.c`): SMP initiator, Just Works, feature flags 111 (+ PAIRING).
      AES-128 / AES-CMAC / f4 f5 f6 c1 s1 ah in software, checked against FIPS-197, RFC 4493 and the spec's sample data
      (`make unit`). LE Secure Connections with the controller's P-256 (supported commands octet 34 bits 1-2: present).
      **Controller quirk:** MediaTek returns its P-256 public key and the DHKey most significant octet first, and wants
      the peer key that way too (spec: least significant first). Found because only the byte-reversed key was on the
      curve and BlueZ refused ours with "unspecified"; converted at the HCI boundary. Bonds in `state/ble_bonds` (0600);
      IRK kept to recognise devices behind private addresses. On reconnect the link is encrypted with the bond, but only
      once the device has sent its first packet: Start Encryption straight after the connection complete killed the link
      (0x3e). A bonded BlueZ peripheral refuses our MTU request until encrypted (asked again afterwards) and runs its own
      exchange first, then refuses ours (its value is kept). Checked against a BlueZ peripheral with an encrypt-read
      characteristic: insufficient encryption -> pair (LE SC) -> read ok -> 8/8 reconnects read with the stored bond ->
      unpair; bonds cleaned up on both sides. Not exercised on air: legacy pairing (BlueZ on the PC always takes Secure
      Connections; `btmgmt sc off` needs root), key sizes below 16
- [x] Bluetooth speaker (2026-09-24, `a2dp.c` + `sbc.c`): A2DP sink on BR/EDR beside the LE proxy, same controller thread
      (`hci.h`: a2dp.c gets the non-LE events and ACL links, sends its commands from `upkeep()`). BR/EDR buffers apart
      from LE: 8 x 1021 (READ_BUFFER). Class 0x240414, EIR with name + AudioSink UUID, SSP NoInputNoOutput / PIN 0000,
      both only inside the pairing window (HA switch "Bluetooth pairing", 120 s, closes after one pairing; the ring runs
      `scone-setup` meanwhile: stock "discovery-in-progress", same frames as the unlisted `btpair-setup`); page scan
      only once a key exists. SDP: one A2DP sink 1.3 record. AVDTP: one SBC SEP, 44.1/48 kHz, bitpool 2..53, delay
      reporting (reports 280 ms). SBC decoder written from the spec (float synthesis); checked against libsbc's `sbcdec`
      in every mode sbcenc offers (`tests/unit/sbc_ref.sh`, part of `make unit`): ~80 dB, <= 9 LSB apart, which is
      libsbc's own fixed-point rounding. Checked on air with BlueZ/PipeWire on the PC: pair, 48 kHz joint stereo
      bitpool 53, 3 min without an underrun with LE scanning on, reconnect with the stored key, pairing off; music
      heard by the user. **micRaw is no proof of playback**: a 6000-amplitude mixer tone at volume 60 moves its rms
      from ~6 to ~20 only; the early "+22 dB in micRaw" checks passed on a sink PipeWire had at 25 % (-36 dB). The
      minute log line now carries the peak level written to the mixer instead. Jitter buffer 150 ms (settles at ~130 ms, first minute
      nudged ~3700 frames, then none), mixer 60 ms. hassmic 6 % CPU while streaming. Not done: refusing an unknown device on air (no second source at hand), phones (only BlueZ)
- [x] Bluetooth speaker codecs (2026-09-24, `a2dp_codecs.c`): stock `bluetooth.default.so` (Fluoride) is SBC only
      (`A2D_BldSbcInfo` / `A2D_ParsSbcInfo`, nothing else). One AVDTP endpoint per codec now, the source picks:
      SBC; AAC (MPEG-2/4 LC, 44.1/48 kHz, VBR <= 320 kbit/s) through the firmware's `libavcodec.so`, FFmpeg 4
      (`avcodec_version` 58, `aac_latm` built in; dlopen, AVFrame/AVPacket offsets of FFmpeg 4 on 32-bit ARM, packet
      layout checked at start), payload = LATM with in-band config, LOAS header put in front for FFmpeg; aptX (raw, no
      RTP) and aptX HD (RTP) through vendored libfreeaptx 0.2.2; Google's A2DP Opus (vendor 0xe0/0x0001, RTP + frame
      count byte) through the firmware's libopus. Formats as PipeWire's `spa/plugins/bluez5` codecs send them.
      `make unit`: aptX / aptX HD / Opus glue with encoded 1 kHz tones (level +-0.02 dB). On air with PipeWire on the
      PC, each codec in turn (profiles a2dp-sink-sbc / -sbc_xq / -aac / -aptx / aptX HD / -opus_g): 880 Hz at
      amplitude 10000 reaches the mixer at peak 10064 / 10097 / 10257 / 11626 / 10995 / 10969, user heard them; hassmic
      6-15 % CPU whatever the codec (AAC 6-12 %). Switching the codec makes PipeWire drop and recreate the sink:
      players fall back to the default sink.
      Jitter buffer now primed with the mixer's share too (150 + 60 ms): before, the mixer took 60 ms at once and the
      drift control duplicated ~3000+ frames per start; now +11 frames in 3 min of AAC. Not done: aptX LL, LDAC
      (no decoder; would also crowd Wi-Fi), HE-AAC
- [x] AVRCP + one music source at a time (2026-09-24, `a2dp.c`, `main.c` `core_music`): SDP records AVRCP 1.5 target
      (category 2) and controller over AVCTP 1.4; target answers UNIT/SUBUNIT INFO, GetCapabilities (company, events:
      volume changed only), RegisterNotification(volume) with INTERIM / CHANGED, SetAbsoluteVolume (0..127 <-> 0..100 %,
      no CHANGED back for the device's own change); controller sends PLAY / PAUSE pass-through (press + release).
      BlueZ opened AVCTP on one connect out of three: we open it ourselves 2 s after AVDTP signalling when the device
      has not (outgoing L2CAP connection, only for AVCTP). Checked with BlueZ/PipeWire: volume Echo -> PC (30 % shows as
      30 %) and PC -> Echo while streaming (50 %, 60 %); PipeWire sends nothing while the sink is idle; transport
      Volume 57 -> Echo 45 %. Action button while streaming: pause accepted, second press play accepted (a device may
      stream silence after pausing, so the second press resumes). Whether a phone's player really pauses: not seen
      (no phone; the PC has no MPRIS bridge). Arbitration in `core_music`: Bluetooth start -> `sendspin_pause()`
      (controller command to the group; a Sendspin client cannot see the group's members, MA sets no group name, a
      leader's group ID does not change when others join, so "only when alone" is impossible); Sendspin start ->
      AVRCP pause, or without AVRCP the Bluetooth audio is dropped until Sendspin stops or the device starts again.
      Sendspin <-> Bluetooth: checked by the user with Music Assistant and a Pixel, both directions work
- [x] Bluetooth speaker on a real phone (2026-09-24, Pixel 10 Pro XL, Android 17; its logcat over adb):
      **Opus not offered**: Android uses A2DP Opus only as its low-latency codec (`btif_av_source_set_low_latency_codec`:
      Opus priority highest when a stream starts with `is_low_latency=true`, -1 otherwise). The Pixel alternated both on
      every stream start (`StartRequest: is_low_latency=true/false`; the audio HAL lists LOW_LATENCY and FREE, the
      output then recommends FREE only): SET_CONFIGURATION Opus, OPEN, START, CLOSE before a single packet, then aptX HD
      the same way, forever, silence. Not the Echo's doing as far as its log shows; the decoder is fine (PipeWire's
      opus_g plays). Endpoint kept in `a2dp_codecs.c`, not offered (`never`). Untested idea: the Echo reports 280 ms
      of delay, maybe Android gives up low latency because of that.
      **Volume dropouts**: SetAbsoluteVolume ran `core_set_volume` on the controller thread (forks audio_manager_set_prop
      and ledctrl, takes core_lock): every slider move stalled the ACL reader, the jitter buffer ran dry. Now a
      volume thread applies it (latest wins), and the volume is read once at start instead of under core_lock.
      **Stutter with aptX HD (576 kbit/s)**: the Pixel's own counters (`dumpsys bluetooth_manager`, TxQueue) showed
      packets dropped at the phone (140, up to 28 in a row) while LE scanning ran. A/B over 3 min each, same stream:
      scanning paused 0 dropped / 1 dry-out, passive scanning 28 dropped / 3 dry-outs. LE scanning now pauses while
      A2DP streams (`a2dp_streaming()` in ble.c's scan decision; HA sees the scanner idle meanwhile).
      **Drift**: dropping / repeating single frames against the buffer level ran continuously after refill bursts
      (-6600 frames/min, heard as roughness). Now linear resampling at a ratio = slow drift estimate (+-1 %) + level
      error x 2e-5/ms: 4 min at 149-154 ms buffer, no dry-out, 0 dropped at the phone, the Pixel measured -261..-1072 ppm.
      The minute log line carries buffer level, clock estimate, peak level, longest packet gap and dry-outs.
      First aptX HD attempts failed because Opus was offered; the Opus loop took aptX HD with it
- [x] Bluetooth connect announcements (2026-09-24, heard on the device with the Pixel): stock Alexa chimed and said "Now
      connected to <name>". Chime: `state_bluetooth_connected.mp3` / `_disconnected.mp3` from the earcon directory. Name:
      HCI Remote Name Request on every ACL connection. Announced once the link has opened AVDTP signalling and the name is
      in (success or failure); "Disconnected from" when an announced link goes. No TTS engine on the image (no pico/svox/
      flite libraries), so the words are Home Assistant's: HomeassistantActionRequest (34/35) `assist_satellite.announce`,
      `preannounce: {{ false }}` (plain data arrives as strings, the schema rejects "false"), entity found by a template over our Wi-Fi MAC in the device registry's connections (entity ids
      can be renamed); the phone's name travels as plain data, never through the template. Needs HA's "Allow the device to
      perform Home Assistant actions". Checked on the PC: aioesphomeapi decodes the request (name with quotes and braces
      intact), the template renders to the satellite in a Jinja sandbox with mocked registry functions. Switch "Bluetooth
      announcements" (config), 6th field in `state/settings` (5-field files load with it on). On the device: a Pixel
      "disconnecting" in its settings closes AVDTP + AVRCP but keeps the ACL link, so the announcement follows AVDTP. HA
      resolved the template to `assist_satellite.echo_dot_assist_satellite`; connect and disconnect both spoken. A reconnect
      during "Disconnected from" got SatelliteBusyError from HA: requests now go one at a time, latest wins: 1 s after
      the previous announcement ended (HA still refuses in the instant after our "finished", its announce call has not
      unwound yet), or 15 s after a request that never played; dropped after 30 s. Checked on air by toggling the
      Pixel's connection quickly: first sentence, then the one for the latest state
- [x] Bluetooth announcement language (2026-09-24): the words were English whatever the satellite's pipeline speaks.
      Following the pipeline's language automatically is not possible: HA's esphome assist_satellite sends no language
      in any pipeline event (STT_END text, INTENT_END conversation id/speech, TTS_END url only), TTS URLs are random
      tokens (`/api/tts_proxy/<token_urlsafe(16)>.<ext>`), the pipeline select's state is the pipeline name only,
      `assist_pipeline` registers no actions (websocket only, needs a user token) and templates cannot read pipeline
      settings or `hass.config.language` (checked in HA `dev` source). So a select "Bluetooth announcement language"
      (config, only with Bluetooth), 12 languages shown by native name, whole sentences per language (cases and
      articles differ between "connected to" and "disconnected from", and for an unnamed device). 8th field in
      `state/settings` as the language code, so the list can grow; older files load as English. Host test covers
      persistence (the select only exists with a Bluetooth controller). On the device: select listed in HA, switched de / es,
      Pixel connect and disconnect spoken as "Verbunden mit" / "Getrennt von" and "Conectado a" / "Desconectado de"
- [x] Do not disturb (2026-09-24): the Alexa app's "Do Not Disturb" (shown as "Focus mode" in the Alexa app we saw) pulsed
      the ring purple once. Switch "Do not disturb" (no entity category: a main control like Mute), 7th field in
      `state/settings` (older files load with it off). While on, VoiceAssistantAnnounceRequest is answered with
      AnnounceFinished success=false and nothing is fetched, except while our own Bluetooth request is out
      (`bt_asked_ms`). Replies, `play_media`, timers and music are left alone. LED: stock `do_not_disturb.animation`
      (layer 2 in `layer_config_common.json`, purple 0x0A0014 → 0x5200A5 → off over about 2 s, nothing after its `loop`
      marker), set when switched on while a client is connected, unset 2.5 s later. No DND earcon on the image
      (`uxeventd` names `do-not-disturb-enable-earcon`, no file in `earcon/base`). Host test covers switch, persistence,
      the dropped announcement and playback after switching off. On the device (2026-09-24): pulse seen, announcement dropped
- [x] Equalizer (2026-09-24): stock path found in the firmware: the Alexa app's bands go PuffinApp (`EqualizerLipcHandler` in
      `libReggaeDevice.so`) → LIPC `com.doppler.lasp` string property `LASP_CMD_SET_USER_EQ_INFO` → `mixer`'s `libasp` user EQ
      (`ASP/UserEq`, `AUDIOALG: setUserEq`). JSON `{"bands":[{"name":"BASS","level":N},{"name":"MIDRANGE",...},{"name":"TREBLE",...}]}`,
      whole dB steps clamped to -6..+6 by the mixer (12 → 6); also `LASP_CMD_ADJUST_USER_EQ_INFO` (`levelDelta`, `levelDirection`),
      `LASP_CMD_RESET_USER_EQ`, read back with `LASP_CMD_GET_USER_EQ_INFO`. Kept by the mixer in `/data/misc/audio/audioCtrl.cfg`
      (not the `userEq.cfg` named in `libasp`). Works with PuffinApp stopped and as `puffin` with hassmic's groups: hassmic runs
      `lipc-set-prop` like `audio_manager_set_prop`, reads the bands once at first use and caches them. Three number entities
      "Equalizer bass / mid / treble" (named so they sort together: HA lists a device's controls alphabetically and
      has no custom groups; sliders, dB, no entity category), ESPHome only. Listening test with pink noise on the device, 3.5 mm
      line-out to external speakers: +6/-6 and -6/+6 audible on both the Music and the TTS stream. Set from an API client on the
      device: mixer reads back the values. Not done: survives a reboot (only by the file), the stock `equalizer-change` ring animation
- [~] Settings page (2026-10-06, asked: fewer entities in HA, features still there, no CLI, simple and secure). Served
      by hassmic on TCP 28931 (inside the firewall's range), files of `web/` gzip'd into the binary. Login by approval:
      the browser's X25519 key waits up to 60 s for a press of the action button (ring `authenticated_setup_mode`, on
      all three models), a second browser asking meanwhile refuses both; approved keys in `state/web_clients`. Requests
      signed (BLAKE2b-128 keyed with K = BLAKE2b(X25519) over method, path, counter, body); counter per browser, on disk
      for writes. Plain HTTP, so no secrets in answers. `crypto.subtle` is missing on plain-HTTP pages: `web/crypto.js`
      has X25519 (after TweetNaCl) and BLAKE2b (BigInt), 20 + 262 cases equal to Python's. Settings by name
      (`settings.c`, `state/config`, old positional file moved once; loaded at start for Wyoming too). Phases: (1) done:
      page, login, status warnings, settings, export/import, revoke; host test `fake_web.py` (23 checks) and the real
      page in jsdom against the host build (login, set, no errors). (2) done: features (arbitration, sound, whisper,
      Wi-Fi motion, Bluetooth audio, Bluetooth speaker): their entities only while on (`listed()` gates list and
      states); a switch closes the HA links, HA re-lists on reconnect and deletes what is gone from the registry
      (`entity.py` `async_static_info_updated` -> `entry_data.async_remove_entities`, checked in HA dev). Lean core
      (user's pick): media player, mute, DND, wake sound, LEDs + illuminance, EQ, firmware, Sendspin token (a secret,
      never on the page). Page-only: mic level, noise reduction, BT announcements + language, update channel, adb (a
      press of its own), Sendspin unpaired, diagnostics (`diag.c`). Bluetooth speaker defaults to on where a speaker
      played before (first `state/config`); the volume-key pairing switches arbitration on through the settings.
      `main.sh` reads `wifi_motion=on` from `state/config` for the module (it read field 13 of the old file). Tests
      reworked onto `tests/webclient.py`: fake_web 35, fake_ha_esphome 97, fake_ha_update 21, fake_ha_arbitration 31. (3) done: arbitration card (`arb_status_json`: network, members with name + IP from
      signed beacons via `recvfrom`, others as none / younger / older with how long; warning when HA does not show our
      handoff entity), Echos card (heard ones + added by address, `host:port` too; a login each with its own button,
      settings that differ, copy one or all). Checked in jsdom with two host builds (log in to B from A's page, 1
      setting differs, copied, B's state/config has it; arbitration card shows B in a second network) and in
      fake_ha_arbitration / fake_web (status JSON, CORS preflight). (4) done: presets: `scripts/setup.sh --preset <export>` or asked
      at the install step, checked as name=value lines, pushed to `state/preset` (daemon user's, 600), hassmic
      restarted; it applies the file once at start like an import (unknown lines logged) and keeps it as
      `preset.applied`; fake_web checks it, `setup.sh --dry-run --preset` walks it. Not done: on the device (the ring
      animation, the press)
- [~] Wake word arbitration between Echos (2026-09-25), stock's ESP ("Echo Spatial Perception", decided in Amazon's cloud)
      on the LAN, `arb.c`. Home Assistant alone only has first-come: `assist_pipeline/run.py` `accept_wake_word` drops a
      second wake-up with the same phrase within `WAKE_WORD_COOLDOWN` = 2 s (we send `wake_word_phrase` "Alexa") with error
      `duplicate_wake_up_detected`; that error is now a quiet finish, not the error ring. Round: score = SNR of the keyword
      (Pryon `beginSampleIndex`..`endSampleIndex`, same index space as `wake_feed`) against 0.5 s ending 0.1 s before it, on
      a 4 s ring of the micAsr stream; claim broadcast twice, decision 200 ms later, lookback 1 s, prio 2 while not idle or
      ringing, winner broadcasts "answers" for late detections; the window's audio is streamed from the ring. No peers: no
      wait. Network: shared key, MAC + counter (reserved in blocks of 4096 in `state/arbitration`). Joining, first way
      (entity lookup: HA showed each Echo's public key on an "Arbitration ID" text sensor, peers read it with
      SubscribeHomeAssistantStateResponse `once`) dropped the same day: HA derives entity ids from the device name the
      user gave it (the installed Echo's was `sensor.julian_echo_dot_arbitration_id`), and a diagnostic entity can be
      disabled. Now: a member hands K to an Echo outside its network (or in a younger one) as a HomeassistantActionRequest
      for `esphome.<node>_arbitration_key`, the receiver's own user action (ListEntitiesServicesResponse; HA names it
      `build_service_name`: `device_info.name` with `-`→`_`, i.e. the node name the Echo reports, not the HA device name),
      which HA delivers as ExecuteServiceRequest over the receiver's keyed link only (accepted only from a client with
      the device key). Sender needs "Allow the device to perform Home Assistant actions" (else HA's
      `service_calls_not_allowed` repair; checked in HA `dev`). K sealed with XChaCha20-Poly1305 under BLAKE2b(X25519(sender,
      receiver's beacon key), both keys, network id), so HA's traces and logbook never hold it readable. No
      network after 5 s: create; two networks merge into the lower id. UDP 28930 broadcast (stock `firewall.sh` admits UDP
      16384-32767 inbound). Under qemu with the stock model and `testdata/alexa_espeak.raw`: detection 2400 samples (150 ms)
      after `end`, keyword -22 dBFS, score 68 dB after silence, 2 dB right behind other speech.
      Host test (`tests/fake_ha_arbitration.py`, fake HA routing actions as its esphome manager does; 3/3 runs): merge
      through HA, K not readable in what HA carried, better score answers alone, prio, different keywords both answer,
      duplicate error quiet, forged beacons (unknown name, a real Echo's name with another key, fake older network) and a
      forged claim change nothing, leave wipes the key, create after 5 s, no key without the actions option, join once allowed.
      Joining without the actions option (2026-10-05): HA gates only action calls; devices fire `esphome.*` events and
      request any entity's state without it (`components/esphome/manager.py` dev: `async_on_service_call` is_event
      branch, `async_on_state_subscription`/`async_on_state_request` unchecked). The entity lookup of 09-25 comes back
      without the guessing: every Echo shows "HMA1 <pub>" on the diagnostic text sensor "Arbitration handoff", asks HA
      (SubscribeHomeAssistantStateResponse `once`: no tracker piles up in HA, and no "old_state None" gap for new
      entities) for `sensor.<node>_arbitration_handoff`, `_2`, `_3` (and one further each time HA answers for the last
      with anything but its key: another device of that name, "unavailable" if it is off; asking only the next one, so
      linear; at most as many as hosts in the subnet: no more Echos of a name can share a broadcast group) until one shows
      its own key, and broadcasts that id
      (`T_ENTITY`, ignored by older Echos). A member asks HA for a newcomer's named entity every 3 s and offers K
      ("<net> <to> <sealed K>", 224 chars, HA's limit 255) on its own entity only once HA shows the beacon key there; the
      newcomer polls members' entities and takes an offer addressed to it. No confirmed entity after 20 s: the action as
      before (so owners without the option see no repair when the entities work). Third way without HA: Volume up +
      Volume down held 2 s (stock gives the combo no meaning; acebuttond owns the 5 s/21 s action holds) = 2 min
      pairing: `T_PAIR` every 1 s, a member hands `T_GIVE` (3 copies, 500 ms apart) 2 s after its press if exactly one
      Echo outside its network asked since 2 min before, and none of an older network is pairing (it gives instead); two =
      refused; taking a key any way ends the window (found by the test: a newcomer that joined through the entity
      mid-window then handed K to a forged request it had recorded). An offer not taken by the next push (30 s) goes
      through the action too. Limit: a LAN attacker who keeps asking gets the key of an Echo pressed while alone in its
      own network. Tap on start, BT connected/disconnected sound for the result. Host test: merge through the entities with actions refused (K not in the states HA saw, offer taken
      down after), a forged `T_ENTITY` naming a real Echo's entity gets no offer, the action after 20 s with neither,
      pairing joins, a second requester refuses it, a forged give does not open, entities back = join without permission,
      action path still joins. Not done: on the device (whether HA really names the entity `sensor.<node>_…` on a fresh
      adoption; the combo under acebuttond; the sounds)
      Not done: on the device with two Echos (score separation at distance, whether the micAsr stream's gain control
      flattens it, claim delay over Wi-Fi with power save)
- [x] Wake word select (2026-09-25): VoiceAssistantConfigurationResponse had "alexa" hard-coded since the ESPHome API
      went in (and VA_SET_CONFIG was ignored), so HA never showed the model `-m` loaded; the request phrase was "Alexa"
      too. Now: stock ALEXA + every `<models>/<keyword>-<lang>/pryon.manifest` offered (name from the folder), HA's pick in
      `state/wake_word`, loaded live in the capture thread (`wake_close` + `wake_open`; under qemu with the stock library
      and echo-de: ECHO detected right after the switch, sample index continuous), phrase = the active name.
      Arbitration compares only claims with the same keyword (BLAKE2b of the lower-case name in the claim).
- [~] micAsr stall (2026-09-25, installed Echo): after hours, every MixerGetBufRec returned NULL, logcat
      `Mixer_DataTrans:InCapture-GetReadBuff:reason=EmptyQueueHungUp` for our stream (plus Minerva metric errors: logd at
      23 % CPU), wake word and capture dump dead, buttons fine; restarting hassmic fixed it (mixer itself untouched,
      started before hassmic). Now 3 empty reads in a row (up to 1.5 s each) reopen the stream. Not seen again yet: the
      fix is untested against the real fault
- [x] Revert procedure tested 2026-09-21: `install-system.sh --uninstall` leaves no trace on `/system` (`/sepolicy` md5 back to the pre-hassmic
      value, stock Alexa + `uxeventd` + `otad` run again, no egress lock: only the VLAN protects then); reinstall brings everything back, and
      `/data/local/hassmic/state` (Sendspin identity, pairing record, settings) survives both. `alexa-on.sh` (no reboot) still untested
- [~] Multi-model layout (2026-09-28): model-specific parts moved to `devices/donut/` (`device.mk`, `board.c` behind
      `src/hassmic/board.h`, `device.conf`, `hassmic.rc`, `sepolicy.rules`); `DEVICE` selects (default donut), outputs in
      `build/<codename>/`, firmware in `firmware/<codename>/`. Device scripts read `device.conf` next to them (bundles and
      `/system/hassmic` carry it); PC adb scripts detect the model via `ro.product.device` and install refuses another
      firmware than `FIRMWARE_ID`; `main.sh` refuses bundles for another product. Firewall still goes up without
      `device.conf`. PC: build clean, unit + all protocol tests, qemu Wyoming test and OTA test pass. Not done: push
      update and `install-system.sh` on the Echo with the new scripts
- [~] Mic gain (2026-09-29, user report: quiet speech misrecognised or cut off, fine on a Voice PE and on stock): HA ignored
      our three mic settings all along. HA core `dev`: `esphome/assist_satellite.py` `handle_pipeline_start` takes
      `audio_settings` and drops it, `assist_satellite/entity.py` builds `AudioSettings(silence_seconds=...)` only; the
      speex AGC/NS in `assist_pipeline/audio_enhancer.py` never runs for ESPHome satellites (Voice PE/ReSpeaker do it on the
      device and offer no such entities). micAsr has no AGC either: AFE.cfg ASR path ends in a fixed "ASR Output Gain"
      +5.2 dB. Measured on the installed Echo: 10 wake words at -49 to -62 dBFS (keyword rms) over a -64..-69 dBFS floor;
      old captures: speech -38..-55 dBFS in 100 ms windows, floor -64..-67, close transients up to 0 dBFS.
      Now `micgain.c` on what goes to the pipeline (wake word still gets the stock stream): per 10 ms frame floor tracker,
      frames 9 dB above it count as speech; their power mean over ~0.5 s is the talker's active speech level (ITU-T P.56
      style, pauses left out), gain = target - that level in -12..+36 dB, falls at once, rises 12 dB/s during speech only,
      per-frame peak limit 29000. Starts from the wake word's rms so the first words already arrive at level. Always on;
      one entity "Mic level" (-35..-15 dBFS, default -26 = P.56's -26 dBov reference) is the output's active level.
      First version aimed an upper envelope (up ~50 ms, down ~2 s) at the target instead: its distance to the active level
      depends on the speech, +4 dB on the unit test's syllables, -7..+2 dB on the captures, so no fixed offset could make
      the slider mean the output. Output active level at -26 on the captures (speech frames, whole file): 1 s mean
      -25.2..-27.0, 0.5 s -26.2..-26.5, 0.33 s -26.4..-26.7 (0.5 s kept: shorter evens out words); micAsr-170444 at
      -31.7 (starts cold with its loudest word, fades 15 dB in 2.5 s). Noise suppression, auto gain
      and mic volume multiplier entities removed (a multiplier on top of an AGC only moves its target). Request carries
      neutral audio settings (0, 0, 1.0). Settings file format 2 (9th field); older files: default level. On the
      captures: speech -22..-30 dBFS in 100 ms windows, floor ~20 dB below it, no clipping.
      `tests/unit/micgain_test.c` (synthetic syllables at -55 and -16 dBFS over a -67 floor: active level within 1.5 dB of
      the target after 1 s, within 3 dB in the first 250 ms, no pumping in pauses, peaks limited, lowest level); `fake_ha_esphome.py` checks migration, neutral settings,
      level. Pushed to the installed Echo; logs "mic gain: talker .. dBFS, gain .. dB" per pipeline.
      Not done: STT results with quiet speech on the device, a second capture with the gain on. The captures this was
      tuned on were made without listening mode (next entry): check the default again
- [~] Listening mode (2026-09-30): the AGC left a user recording at -32 dBFS with speech barely over the noise. Captures
      of micRaw (mixcap beside hassmic) and micAsr (SIGTTIN dump) at once, quiet sentence, no wake word, SNR against the
      silence before and after:
      | | micRaw first 1.5 s / rest | micAsr first 1.5 s / rest |
      | stock, 4 captures | 12.2 / 8.8, 10.9 / 8.5, 10.7 / 10.0, 9.2 / 8.0 | 11.9 / 0.6, 10.7 / 3.2, 10.4 / 5.6 (end 1.9), 5.7 / 2.1 |
      | NDVC bypass | (third row above) | no change |
      | AFE.cfg ARA mu 0.01, all modes | 10.7 / 9.3 (end 8.2) | 10.7 / 9.2 (end 5.9) |
      | listening mode 1, stock AFE.cfg | 7.9 / 6.7 (end 6.6) | 7.5 / 6.1 (end 5.8) |
      Floors: micRaw -77 dBFS, micAsr -65 dBFS. Cause: nobody told the front end that a command was being spoken, so
      its cancellers adapted to the talker; stock does it by reading LASP_CMD_REQUEST_ARBITRATION_JSON after the wake
      word (FINDINGS.md "Listening mode", from libasp.so). hassmic now sets LASP_CMD_SET_LISTENING_MODE 1 at pipeline
      start and 0 when the mic stream stops (HA heard enough, reply starts, pipeline ends or times out) and at start-up;
      not with `-w remote`. On the Echo: SIGUSR1 pipeline logs "set listening mode 1" / "Utterance start detected", 15 s
      later (HA: no text recognized) "set listening mode 0". Host build + fake_ha_esphome, fake_ha, fake_ha_arbitration pass.
      The AFE.cfg bind mount of the test is gone (mixer restarted on the stock file; that restart also showed the
      micAsr stall recovery of 2026-09-25 working: "delivers nothing (status 110), reopening", audio back).
      Not done: a spoken command through HA with the fix, sentences over 4 s, wake word during music (both cancellers
      freeze for the command unless the stream is TTS), "internal" speaker mode (this Echo: lineout), the mic level
      default with the fix. Ideas: voiceEnergy / ambientEnergy of the arbitration JSON as arbitration score; RNNoise if
      6-8 dB SNR of quiet speech stays too little for STT
- [~] Wake sound in the pipeline's audio (2026-09-30): HA's recording at mic level -26 had the wake sound at -15 dBFS and
      the command at -32. 4 triggers without speech, micRaw beside micAsr: the sound is +21..+22 dB over the floor in
      micRaw, +4..+8 dB in micAsr (-57 dBFS): the echo canceller takes ~15 dB, the rest is as loud as a quiet talker
      (this Echo plays through the 3.5 mm jack, "lineout" tuning). Listening mode forced off during the sound: -57.8 and
      -62.4 dBFS against -57.1 and -56.9 with it: not the cause. The gain took the sound for speech (+35.7 dB -> -15 dBFS)
      and came down for the command. Now the gain holds (levels and gain frozen, peak limit on) while one of our sounds
      plays and 250 ms after. The sound itself stays in the stream (RNNoise takes another 4..17 dB of it on the captures).
      Not done: a recording from HA with the fix; whether to skip the stream during the sound as the Voice PE does
- [~] Noise reduction (2026-09-30): RNNoise 0.1.1 (`src/third_party/rnnoise`, BSD-3) in `micdenoise.c`, ahead of the gain,
      only on what goes to the pipeline; HA switch "Noise reduction", off by default, in the settings file's first
      field (superseded the same day: a select with three strengths and no level change around RNNoise, see "Noise
      reduction strengths"). 16 kHz -> 48 kHz (x3, 96-tap low-pass) -> RNNoise -> 16 kHz; +24 dB on the way in and back out (RNNoise was
      trained on levels down to -40 dB, ours is -55 dBFS); reduction capped at 18 dB by mixing the delayed input back in
      (uncapped the floor went from -65 to below -108 dBFS: gated). Delay 12 ms + framing. It hears the second before
      the command from the ring first (cold it needs 2 s on white noise, 0.2 s on the Echo's room noise).
      On the captures of the listening mode entry: floor -65 -> -83 dBFS, SNR of a quiet sentence 7.5 / 6.1 dB (first
      1.5 s / rest) -> 22.8 / 19.1 dB, speech itself ~3 dB down; clean espeak speech passes with correlation 0.978 and
      the same level. With the gain behind it: pauses -33 -> -50 dBFS, speech -27.7 -> -30.1 dBFS (both files in
      `device-logs/denoise/`). CPU: 11 ms per second of audio on the PC, 2755 ms for 15.1 s on the Echo (18 % of one
      A35 core, only while a pipeline runs). Binary +41 kB. `tests/unit/micdenoise_test.c`, switch and stream in
      `fake_ha_esphome.py`. Not done: what STT makes of it (the reason to have it), a listening test
- [~] Arbitration score from the front end (2026-09-30): stock's numbers instead of our own SNR of the mic stream.
      `wake_pryon.c` inflates the result metadata (stock libz.so) for the keyword's place on the front end's clock,
      `main.c` hands it over and reads voiceEnergy / ambientEnergy (FINDINGS.md "Listening mode"); score =
      1000 * log10(voice / ambient), the unit of the old score, which stays as fallback (no marks in the stream, lipc
      fails, PC build, simulated detection) and is logged beside it. Reading it starts the front end's utterance state
      and diagnostics: LASP_CMD_NOTIFY_ASR_STREAM_STOPPED when the Echo loses, answers without a pipeline, or the
      pipeline ends. Checked: metadata parsing under qemu (fake_ha.py --qemu, and pryon_test on a file with a capture's
      low bits: 8823 / 9559 ms for a 770 ms keyword); the lipc line by hand as the daemon's user on the Echo
      (`{"voiceEnergy":10690,"ambientEnergy":66408,..}`, 150 ms); fake_ha_arbitration passes (its scores are given).
      Not done: a spoken wake word on the Echo (the log then shows both scores), two Echos, biscuit and radar (their
      libasp unread: the fallback applies if the property is missing)
- [~] Arbitration with Kiosk Satellite (asked 2026-10-05; analysis in `docs/kiosk-arbitration.md`): theirs is UDP
      broadcast on 2330, plain JSON claims (phrase, dB over the noise floor), ±400 ms window, no authentication, no
      priority, no "answers" message. Done 2026-10-06 as a second mode rather than a bridge: setting
      `arbitration_mode` (`hassmic` default / `kiosk`), page-only, the page compares the two. Kiosk mode speaks only
      their protocol, as they designed it (three copies at 0/15/30 ms, ±window, highest e, lower id on a tie; no
      priority, always waits), window `arbitration_window` 100-500 ms (400). Score: their formula on micAsr's last 3 s,
      frames lifted +30 dB first (micAsr runs ~30 dB under speech level, which would sit on their -75 dBFS floor clamp),
      plus the owner's `arbitration_offset` (-20..20 dB, default 0) added to e on the wire, to even out by ear.
      Beacons flag the mode (1 quiet + 2 kiosk), so own-mode Echos do not wait and the page warns of mixed modes.
      Firewall: `lockdown.sh` admits UDP 2330 while `state/config` has `arbitration_mode=kiosk`. fake_ha_arbitration
      (fake kiosk socket): wire format, loudest wins, kiosk louder silences both, other phrase and malformed claims
      ignored, no priority, offset flips the winner, back to own mode. Not done: a device test next to a real kiosk (calibrate the +30 dB lift,
      claim delay over Wi-Fi), the firewall rule on the Echo
- [x] Clock from Home Assistant, times in the log (2026-10-06). Found: nothing sets the clock behind the egress lock,
      the Dot 2 was 83082 s behind. hassmic asks Home Assistant (ESPHome GetTimeRequest 36, aioesphomeapi answers it
      itself) on each keyed link once it subscribes and every 6 h on its pings; `clock.c` hands "<epoch> <boot clock as
      /proc/uptime>" to root (state/clock); `main.sh` netwatch adds the time since, `date -u @`, `hwclock -w -u`, setprop
      `hassmic.clock.synced`. Log: hassmic's stderr/stdout through a pipe to a reader process ("hassmic-log",
      double-forked: Amazon's DHA module `wait()`s for its idme child and with SIGCHLD ignored that waited for the reader,
      hassmic never opened its ports - seen on the Dot 2, fixed before commit), stamps "2026-10-06 15:41:29.417Z" or
      "boot+31027.076"; drops the idme tool's 3-line error (4x per signing) and the DHA module's setgroups warning.
      Scripts: `say` / `stamped` in main.sh, boot.sh's one line. mksh is 32-bit: all shell numbers stay below 2^31.
      Dot 2: clock set 83082 s, RTC follows, stamps UTC from then on, viewer shows local time. Tests: fake_ha_esphome
      (state/clock only from the keyed link), fake_web (boot+ stamps), fake_ha_arbitration (UTC stamps); clock_set's
      branches run under the Echo's mksh with stubs
- [x] Log viewer on the settings page (2026-10-06): `GET /api/log/0|1`, signed, tail of 2 MB of `boot.log` / `.1`,
      Sendspin pairing token blanked (signed is not encrypted: a sniffer reads the answer); filter, older part, download.
      Kiosk claims logged once a second at most (unauthenticated, and `boot.log` is on flash: /data, mmcblk0p16 ext4).
      fake_web: both parts, token blanked, 401/404; fake_ha_arbitration: 200 claims at once give 1-3 lines
- [x] Rename on the settings page (2026-10-06): display name (`state/name`) and, confirmed, the node name (`state/node`);
      node stays derived from `NAME` otherwise. HA's ESPHome manager (dev, 2026-10) keeps the entry on a new name with
      the same MAC and just stores it; entity ids stay, so arbitration's `sensor.<node>_arbitration_handoff` lookup misses
      after a node rename until they are renamed in HA. Restart via `state/restart` -> root `stop/start hassmic`. Not
      tried on a device yet. fake_web: refused names, both kinds, link not followed by `-S`. Bluetooth speaker delay
      also a page setting (`bluetooth_speaker_delay`, not exported).
- [x] Identify (2026-10-06, a user's idea: which Echo is which on a full desk): `zzz_rainbow` for 10 s (a loop, identical
      on donut/biscuit/radar) + `state_setup_discovery_beacon` (all three). Page header, per Echo in the list, HA button
      (`LIST_BUTTON` 61 / `BUTTON_COMMAND` 62, device class identify, diagnostic). fake_web: page, unsigned, HA button.
      Not seen on a device yet.
- [~] Wi-Fi switch on the settings page (2026-10-07, asked: scan plus a name typed by hand, password before or after
      picking, back to the old network if the new one fails, warn about the new address). hassmic (puffin) may not talk
      to wpa_supplicant: `state/wifi-request` for root (`main.sh` ota_watch, every 2 s) -> `scripts/device/wifi.sh` in the
      background, answers in root's `/data/local/hassmic/wifi/` (status, scan, result, lock/). The password: sealed by
      the page with K (BLAKE2b key stream over the request counter, `web.c` unseal, `crypto.js` seal, padded to 64 bytes);
      `wifi.c` turns it into the PSK (PBKDF2-HMAC-SHA1, 4096) and only that goes to root (0600 request, removed on read)
      and into `wpa_supplicant.conf`. Switch: add + select in wpa_supplicant only; kept once `wpa_state=COMPLETED` on
      it (30 s), an address and the router answers ARP (`/proc/net/arp` flags 0x2 after one ping; 30 s); then every
      other saved network goes (asked: no fallback to an old network once the new one works; P2P groups, disabled=2,
      stay), `save_config`. Else removed, the old network selected and the others enabled again (select_network
      disabled them), nothing ever saved. Why it failed from the furthest `wpa_state`: never
      associating = not found, 4-way handshake = password. netwatch: no wifisvc while `lock/` exists. WPA2-PSK and
      open only (SAE-only, WEP, EAP, OWE shown, not offered). `tests/fake_web_wifi.py`: real wifi.sh against
      `tests/fake_wifi_tools.py` (wpa_cli, ifconfig, getprop, ping...): escapes, merge, PBKDF2, wrong key, out of reach,
      no DHCP, silent router, replacement, open, save failure, replay, busy, interrupted, junk requests.
      Dot 2 (biscuit), 2026-10-07, over USB: wpa_cli as parsed (`get_network` prints no newline; key_mgmt of Amazon's
      entry "WPA-PSK WPA-PSK-SHA256"), toybox `ifconfig`/`ping -W`/`sed [[:space:]]`, no `ip`, no `head -c`; mksh's `echo`
      eats backslashes (`\x41` -> A), hence printf for SSIDs. Scan 5.3 s. Unknown SSID: notfound, back 4 s after the 30 s;
      wrong PSK on a real SSID: wrongkey (4-way handshake seen), back 6 s after; `wpa_supplicant.conf` md5 unchanged
      both times. Found there: back() let its own reconnect overwrite why (reported noassoc); fixed, fake now settles
      through ASSOCIATING. dhcpcd: init's `dhcpcd wlan0 -AdLK` (6.8.2), `-K` = no link events: 15 s on a missing network
      and back left address, router ARP entry (0x2) and `dhcp.wlan0.*` untouched, so the address-only check would have
      passed on another network with the old lease. Now `setprop dhcp.wlan0.result hassmic` + `dhcpcd -n wlan0` and only
      a lease its hook (`95-configured`) writes counts; same network: RENEW in 1 s, switch in 5 s. Other subnet
      (192.168.100.0/22 -> DasImhof'scheTortenstueck 192.168.0.0/23): 17.8 s; there dhcpcd did see the drop (EXPIRE,
      CARRIER, a moment of IPv4LL) and REBIND gave 192.168.1.242, router 192.168.0.1; lockdown re-applied with the new
      resolvers by itself; Music Assistant back at once, Home Assistant after 3 min (zeroconf: mDNS announced the new
      address, checked from the PC). Whole chain through the page API with the biscuit build in /data and a root loop
      for main.sh's part: sealed wrong password -> wrongkey; the real one -> PBKDF2 on ARM right (switched); back from
      the new address with a fresh login, page lost the Echo (as meant), USB: ok on the old network. Also found: the
      page showed the old network while on the new (status written only on link up): netwatch now writes it on every
      change of address. And `wpa_cli reconfigure` (cleanup) took wlan0's socket in /data/misc/wifi/sockets away until
      wpa_supplicant restarts; wifi.sh then talks to the global one (`-g@android:wpa_wlan0 IFNAME=wlan0`, Android's
      way), checked on the Dot 2 with a scan and a failed switch. Not on a device: main.sh's dispatch and netwatch
      parts (an update brings them), the page in a real browser against the Echo.
- [x] Echos dropping out of each other's network (2026-10-07, seen on the settings page: the Echo Dot missing from
      the Echos list for seconds, then back). Watched from all three for 5 min: the Echo Dot heard both others' beacons
      every 30 s (max age 29 s), both others heard its beacons only every 90 s (max age 72/74 s, PEER_TTL_MS 75 s: out
      for ~15 s each cycle, out of rounds too). A wired PC on the VLAN got all of its beacons. Counting rule on the
      Dot 2 (iptables, no target): the lost ones never reached its kernel. From the wired PC, broadcast vs unicast to
      the Dot 2, 60 each: 44/60, 51/60, 48/60 vs 60/60 every time; on the other AP 54/60. Not power save (driver log:
      PS mode CAM since boot; DTIM skip 0; autosleep off, no suspend). The PC's own Wi-Fi card on the same APs, at
      -71 dBm against the Echo's -61, from a spare address (raw frames out of the wired card): 113/120 broadcast,
      120/120 unicast. hassmic stopped: 59/60. Cause: the BLE scan (30 ms per 320 ms, ble.c) has the shared antenna;
      APs never repeat broadcasts. Unicast to every peer was tried and dropped (traffic grows with the square of the
      Echos, claims' copies multiplied). Instead: ble_quiet(1000) when a round starts (main.c wake_heard), so an Echo
      that heard the wake word hears the others' claims: 40 broadcasts in 0.8 s, 4 times each, 136/160 scanning vs
      158/160 in the pause. Beacons every random 20-40 s (no sender stuck in a bad phase), PEER_TTL_MS 150 s (about four
      missed in a row); Dot 2 with it, 5 min: never dropped the Echo Dot (longest gap 59 s). Then 5 min (about ten
      missed, asked): an unplugged Echo stays listed and waited for that long. With that, a restarted Echo (same
      identity) is still counted by the others, so they did not answer its first beacon and its own list stayed empty
      until their next one (up to 40 s; also what was seen first on the page after a push; fake_ha_arbitration's
      action step flaky past its 60 s). F_HELLO (flags bit 4): its beacons for 40 s after a start or a join; members
      answer at once; answers never carry it (two hellos would answer each other for ever); older builds ignore it.
      Still flaky 1 in 3 after that: a newcomer restarted 1.1 s after the last one got no answer (members answer
      newcomers once a second at most), sent its next beacon only after LONER_MS 10 s, and started a network of its
      own at DISCOVER_MS 5 s, which the member then joined. A lost broadcast does the same. Now a newcomer asks every
      LOOK_MS 1.2 s during its first DISCOVER_MS. The other two still on
      the fixed 30 s / 75 s until updated.
- [x] Arbitration key handoff through HA tags (2026-10-07, issue #8: three Echos on the latest beta, all with the
      settings page's "handoff entity under another name" warning). HA 2026.9 names entities from area + parent device +
      device + entity name (`helpers/entity_registry.py` `_async_get_full_entity_name`, default parts AREA,
      PARENT_DEVICE, DEVICE, ENTITY, user-configurable in the registry's `entity_id_parts`; #179996, #183610): the
      issue's ids were `kitchen_echo_dot_kitchen_arbitration_handoff`, so `sensor.<node>_arbitration_handoff` never
      matches once a device has an area, and they had joined through the action. Nothing HA sends a device names an
      entity id (ESPHome API checked: state responses answer an id the device names; templates render in events and
      actions, but only actions answer, with permission). Sub-devices without an area would dodge the area, not the
      parent-device part. Tags do: `tag.async_scan_tag` makes `tag.<slug(tag_id)>` (`object_id_base=tag_id`, no device,
      no area), state = last scan (ISO, ms), and the esphome manager fires `esphome.tag_scanned` in its is_event branch,
      before the permission check. So: tag `hassmic_<pub hex>` scanned every 10 s (12 times, then every 5 min; back to
      fast at most once per 5 min on a new key, as anyone can make keys up) while an Echo outside a network is in sight
      or we are outside one; the other side polls `tag.hassmic_<pub>` every 3 s and confirms the key once the state
      differs from its first read (a stale tag of a removed device never changes; no clock needed); a confirmed member
      sends `T_GIVE` every 5 s, a newcomer takes one outside pairing only from a confirmed Echo for the network it
      beacons. Action after 30 s unconfirmed (was 20). The "Arbitration handoff" entity, `T_ENTITY` and the id search
      are gone (key 38 kept free); older Echos meet new ones through the action. Merge in the host test ~20 s.
      fake_ha_arbitration (fake tag integration): merge without actions, scans stop once in one network, a forger with a
      stale tag and a forged `T_GIVE` gets nothing, no tags = action, tags again = in without permission. Not tried
      against a real HA yet. On the three Echos (2026-10-06 23:11, network key removed on all, restarted together):
      radar started a network after 5 s, all three tags scanned at 23:11:28, both others confirmed and joined by
      23:11:31 (23 s in all), no action, no pairing; so HA 2026.9 names the tag entity `tag.hassmic_<pub hex>` as expected.
- [x] Claim copies spread (2026-10-06): after the rejoin, a wake word heard by radar (score 5836) and biscuit (2187)
      was answered by biscuit and HA turned radar away as a duplicate. biscuit's log has no claim from radar in 2 of 3
      rounds (donut got it, 28 ms after biscuit's), only radar's "answers" 188/279 ms later; claims seen elsewhere
      arrived -17..+59 ms from the own detection, so not the 200 ms window: both copies, sent in the same ms, were lost
      together. Now claim and "answers" go out at 0/30/80 ms (`COPY_MS`, loop woken by a pipe so they leave on time;
      kiosk copies too). fake_ha_arbitration: copies at 0/31/81 ms.
- [x] Stale `-m` after the wake word migration (2026-10-06, donut kept going back to Alexa): `artifact-install.sh
      migrate` renamed `models/echo-de` to `echo-de-DE` and moved `state/wake_word`, but hassmic.conf kept
      `ARGS="-m .../models/echo-de/pryon.manifest"`; `wake_words_scan` added the -m path unchecked, so HA's select had two
      "Echo (de)", the user's pick landed on the dead one (`state/wake_word` = echo-de) and every start logged "cannot
      load ... trying Alexa". Now `wake_word_find`: an id matches itself, else the one set `<id>-<region>` (unique); -m
      only added when readable; a failed live switch sets the active one back to Alexa. On donut: `echo-de-DE` active.
- [x] Firewall service stuck at boot on the Echo 2 (2026-09-30, found when a push update got "the installer did not
      answer"): `main.sh firewall` scanned /proc/*/cmdline with `tr` for old lockdown watchers; a process (pid 209)
      exited between the open and the read, and radar's toybox `tr` (Fire OS 6572) then spun on the read error for ever
      (state R, ~100 % of a core). The service never reached `lockdown.sh watch` or the installer: **no `hassmic_out`
      chain for the 7 minutes of that boot** (OUTPUT accepted wlan0; otad, ace_otad, update_engine and PuffinApp were
      stopped by the satellite service). Killing the `tr` let it run on: 14 rules, first in OUTPUT. Fix: the scan reads
      with the shell's own `read -r -d ''` (returns on the error; checked on radar and donut). Pushed to both Echos
      (radar's first push update with the new build: Pryon metadata format the same as donut's under qemu, mixer takes
      listening mode). The factory copy in /system/hassmic of an installed Echo keeps the old scan until it is
      installed again; it only runs when no update is installed or one failed three times.
      Not done: a check at the end of boot that the chain exists (done the same day, next entries)
- [x] First spoken test of the three entries above (2026-09-30, user beside the Echo 2 "Küchen Echo", the Dot 3 further
      off, both on the new build, wake word "Echo", two commands):
      | | Echo 2 (radar) | Dot 3 (donut) |
      | 1: front end voice / ambient -> score (own score) | 870552 / 16962 -> 1710 (2458) | 169 |
      | 2: the same | 284756 / 26900 -> 1025 (1972) | 77289 / 67210 -> 61 (671) |
      The Echo 2 answered both times, the Dot 3 stayed quiet ("another Echo answers") and its front end left the
      utterance state ("Utterance end detected", "AFE Diagnostics stopped"). Front end on the Dot 3 took the times:
      "WWStartOffset from AFE timestamp:867 ms, WWEndOffset 299 ms, length of WW: 568 ms", "Calling new
      getSpectralFeatures API which receives WW start and end time offset" (no fallback). So the metadata path works on
      radar's Pryon and libasp too, and claims cross the two subnets (192.168.100.x / 101.x).
      Pipeline on the Echo 2: 1 without noise reduction, gain +23.2 dB, transcript "Wie geht es dir so?"; 2 with it,
      gain +26.4 dB, transcript "Und wie geht es dir so mit Noise Reduction?", 844 ms CPU for 4.3 s of audio (20 %).
      Both transcripts right; spoken at normal level near the Echo, so no verdict on quiet speech or on noise reduction.
      `pryon WARN .. {KWS} Bitmask frame indices (currentFrameIdx < wwEndFrameIdx), setting wwEndFrameIdx to
      currentFrameIdx since its within delta` on the Echo 2 before command 2: nothing new and harmless. The Dot 3's log
      has it since hassmic 0.1.0: of 235 accepted wake words 99 with this one (keyword end 1..7 frames ahead, delta 8)
      and 59 with "Invalid bitmask frame indices" (8..21 frames ahead), each followed by the normal accept. It is about
      the audio fingerprint in the result's metadata (the "FP" of JSON_GZ_AND_FP, which Alexa matched against known
      recordings of its wake word): its extractor is a few 10 ms frames behind the keyword's end when the result is
      built. hassmic does not use the fingerprint; the front end's times in the same metadata came through in that
      very detection (score 1025 from the front end).
      Not done: quiet speech at a distance with and without noise reduction, HA's recordings of it, a wake word where
      the two scores disagree about the nearer Echo
- [~] Noise reduction strengths (2026-09-30): two recordings from HA through the Echo 2, both transcribed right: without,
      speech -23.7 dBFS (active level) over a floor of -31..-35; with the 18 dB cap, -27.5 over -50, but the weak parts
      of words down to -35..-45 mid-sentence and artefacts audible to the user. Listening versions of one quiet capture
      at caps 6 / 9 / 12 / 18 dB (pauses -37 / -40 / -43 / -49 dBFS against -33 without): 18 too many artefacts, the
      others fine. So the switch became a select: Off, Low (6 dB), Medium (9), High (12); settings field 0..3 (a "1"
      from the switch reads as Low). Tried for a measure of the artefacts: log-spectral distance to clean espeak speech
      over the Echo's room noise gets better with every dB of reduction (SNR 6 dB: 10.9 dB at cap 6, 9.9 at 18, input
      12.3), so it does not show them; and the +24 dB around RNNoise made no difference (0 / 12 / 24 / 36 dB within
      0.2 dB): removed. Not done: a case where STT fails without noise reduction and succeeds with it
- [x] `scripts/mic-compare.sh` + `tools/mic-compare.py` (2026-09-30): the two scratch scripts of the listening mode
      hunt as a tool. Records micRaw (mixcap) beside hassmic's micAsr dump, `-l` with listening mode, finds the sentence
      in micRaw and prints both streams' signal to noise for its first 1.5 s, the rest and the last 1.5 s. On the two
      captures of that entry it finds the sentences (4.3..7.2 s, 4.5..8.7 s) and gives the hand-measured numbers
      (micAsr rest 2.1 dB without, 6.1 dB with listening mode). Run against the Dot 3 and the Echo 2 (mixcap from the
      installed update's directory); the Echo 2 was muted: it says "digital silence" instead of numbers
- [x] Pryon's two fingerprint warnings ("Bitmask frame indices", "Invalid bitmask frame indices", see the spoken test
      above) no longer go to boot.log (2026-09-30): `on_log` in `wake_pryon.c` drops them. Other Pryon warnings still show
- [x] Firewall check from the satellite service (2026-09-30): its 10 s loop in `main.sh` looks whether `hassmic_out` is
      first in OUTPUT; missing on two looks in a row it logs "== no egress lock ..", stops `hassmic_fw`, applies
      `lockdown.sh` once itself and starts the service again. On the Dot 3: firewall service stopped and the chain
      removed by hand -> lock back after 19 s (14 rules, first in OUTPUT), service running, one log line; a jump
      removed with the watcher running comes back through the watcher within 7 s without the check firing; no alarm in
      normal running on the Dot 3 and the Echo 2. Not covered: MODE=stock-online (no satellite service there; the
      firewall service's own scan no longer hangs), and the seconds before the satellite service starts at boot
- [x] Firewall check covers every rule (2026-09-30, issue #1 comment, Dot 3: stock's `-A OUTPUT -o wlan0 -j ACCEPT`
      missing after one boot; `hassmic_out` was first and only RETURNs, policy DROP, so nothing left the Echo: HA
      "unavailable", hassmic running, adb still up through the `--sport 5555` rule. Both checks only looked at the
      jump). `lockdown.sh`: the chain is one list (`rules`, worded as `iptables -S` prints it), loaded from it and
      compared with it. `keep` is the second list: stock's rules the satellite needs, same wording. OUTPUT: `-o wlan0`
      and `-o lo` ACCEPT. INPUT: `-i lo`; on wlan0 RELATED,ESTABLISHED tcp and udp, 16384:32767 udp and tcp, mDNS
      5353 (avahi announces the Echo to HA); icmp RELATED,ESTABLISHED; adb 5555 on stock's own condition
      (`persist.sys.usb.config` has adb). ip6tables: the lo, wlan0-out and RELATED rules, icmpv6, DHCPv6 546 (hassmic
      listens on IPv4 only but connects out over either). `load` appends those that are missing and sets INPUT policy
      DROP after them. `wrong` names what is off: a rule of the chain missing, changed, added or out of order; the
      jump not first in OUTPUT; INPUT policy not DROP; which of the kept rules are missing; without ip6tables, an
      interface with IPv6 on. The watcher applies again on any of these and logs which; `lockdown.sh check` is the
      same check without changing anything, and `fwcheck` in `main.sh` calls it. MODE=stock-online: same check, its
      own two-rule chain.
      How a stock rule gets lost: stock `firewall.sh` calls iptables without `-w`. Dot 3, two writers side by side:
      2 of 100 appends without `-w` failed with "Another app is currently holding the xtables lock" and were simply
      not there (the 100 with `-w`: all there). At `sys.boot_completed` its run takes 4.4 s on the Dot 2 (logcat
      `Firewall`), so a look of our watcher always falls into it, and the rebuild that follows is such a second
      writer. Seen on the first boot with this build (Dot 2): stock's `-A INPUT -i br0 -p udp --dport 1900` was
      missing afterwards (127 of its 128 rules there). Not proven to be what happened on the reporter's Echo.
      Checked: `check` on the Dot 3 (with ip6tables), the Echo 2 and the Dot 2 against the rules of a normal boot:
      nothing wrong, 0.23 / 0.16 / 0.15 s a look (cutting the OUTPUT rules out of the whole listing with `${x#*..}`
      took mksh 0.9 s: a listing of its own instead). On the Dot 2 (USB), by hand: each of the nine kept rules
      removed, INPUT policy ACCEPT, IPv6 on for p2p0, and earlier a chain rule removed, one added, DROP moved up: each
      named, exit 1. The new watcher in place of the installed one: two INPUT rules, the policy, the way out, the
      chain's DROP each back within 7 s with its log line, no duplicates. `fwcheck` with no watcher running: second
      look logs "== firewall rules wrong .. (iptables: missing: -A INPUT -i wlan0 -p tcp -m tcp --dport 16384:32767
      -j ACCEPT ..)", rule back, service started. Dot 3: rules loaded once by hand with the new script, then ip6tables
      rules removed (icmpv6, wlan0 out, 546, a chain rule, policy): named, back after the next load. `-S` wording of
      the stock-online rule read from a scratch chain (`--uid-owner 5008`).
      Pushed to the Dot 2 (0.3.0+65cb60d-dirty) and rebooted: the rules go up at "on boot" with INPUT already DROP,
      Wi-Fi and the address come up behind them, stock's run falls into our first load ("re-applied .. not first in
      OUTPUT" when its `ahe_out` went in front, then the resolvers), HA connected, `check` clean at 87 s. Four of
      the kept rules are there twice on that boot (lo in and out, icmp, 5555: ours went in before stock reached
      them); harmless.
      Loading in one call (same day): `load_once` puts the whole change into one `iptables-restore -w --noflush`
      (chain declared again = emptied, its rules, the old jumps out and one in at 1, the missing kept rules, `:INPUT
      DROP`), after one listing; `load` tries it twice, then falls back to the rule-by-rule loader (`load_each`), which
      still loads what it can when the kernel refuses a rule. Here `iptables-restore` is the iptables binary, takes the
      xtables lock and knows `-w` (all three models; `ip6tables-restore` on the Dot 3). A refused line commits nothing
      (rc 2). The input has to be complete before the call: with the listing inside the pipe the restore held the lock
      while the listing waited for it, for ever (first try, Dot 2). Dot 2, beside a loop of stock-style appends
      (no `-w`): 57 of them failed during 60 loads of 0.3 s (0.95 a load); rule by rule 38 during 6 loads of 4.7 s
      (6.3 a load; `-w` waits in 1 s steps, and once a `-w` append of ours failed as well). So about seven times
      fewer of stock's rules lost per rebuild, not none, and the chain is never half built.
      Checked on the Dot 2: plain load 0.7 s for the whole script; second jump + another rule in front + kept rules
      gone + policy ACCEPT + chain changed -> one jump, first, check clean; no restore tool (name changed in a copy)
      -> "!! iptables-restore failed twice: loading rule by rule", check clean; a group that does not exist -> restore
      refuses, rule by rule loads the rest with "!! hassmic limited to local addresses", DROP last. Dot 3: both
      tables loaded by hand through the restore tools, ip6tables rules removed and back. Pushed to the Dot 2 and
      rebooted: no fallback line, HA connected, `check` clean at 92 s, every one of stock's 128 rules there on this
      boot; seven kept rules twice (our first load fell into stock's 4.6 s run).
      Not done: the Dot 3 and the Echo 2 still run the old build (only the Dot 2 was updated); the stock-online
      watcher was not run. Not covered: stock's other rules (what Alexa itself needs in stock-online: Spotify,
      multi-room, Matter ports) can still be lost when a rebuild falls into stock's run, about one per rebuild
- [x] adb over Wi-Fi closed (2026-09-30, issue #1 comment: root adb open to the whole LAN without authentication,
      confirmed from another PC). On all three Echos: `ro.adb.secure=0`, `ro.secure=0`, `amazon.fos_flags.noadbauth=1`,
      no `adb_keys`, adbd listening on `:::5555`, stock's `-A INPUT -p tcp --dport 5555 -j ACCEPT` (firewall.sh adds it
      when `persist.sys.usb.config` has adb). The stock adbd does have key authentication (`adb_auth_client.cpp`,
      `/data/misc/adb/adb_keys`) but decides from `ro.adb.secure` (boot ramdisk on radar/biscuit, the system root on
      donut) and `/proc/idme/fos_flags`: switching it on means a change to what boot-root flashed, through TWRP, not
      by push update, and a lost key would lock USB out as well (no screen to confirm a new key). Not done.
      Instead `lockdown.sh` `adb_gate`, every run and every 5 s in both watchers (satellite and stock-online): open
      while `hassmic.adb.until` (uptime s, a property: survives a watcher restart, not a reboot) is ahead or
      `hassmic.conf` has `ADB_WIFI=1`, else `service.adb.tcp.port 0` + `ctl.restart adbd` (USB unaffected) and stock's
      rule taken out (`load_once`/`load_each`) and checked for (`wrong`: "port 5555 (adb) admitted"; `keep` has it only
      while open). hassmic's switch "Debug access (adb over Wi-Fi)" (`adbwifi.c`, config category) writes
      `state/adb-request`; the watcher sets 30 min (`ADB_SECS`) or 0 and removes it; `/data/local/hassmic/adb-open`
      (root's directory, so root never writes through a name the daemon controls) is the answer hassmic reports. The
      switch reports the request until it is taken, then the file; unanswered after 15 s the request is withdrawn.
      Opening is refused unless the command came over the keyed connection (before adoption anyone can connect).
      A request left over from before a reboot is dropped when the watcher starts. `main.sh` makes `hassmic.conf`
      root:root 644 before sourcing it: it was 666 on the Dot 2 and the Echo 2 (`adb push`), i.e. writable by the
      daemon, and root runs it.
      Dot 2 (USB), 0.3.0+1646bc1-dirty pushed: closed at the update, `check` clean, conf 644; request file -> "OPEN"
      within 9 s, `adb connect` over Wi-Fi gives root; end of window moved to 12 s ahead -> closed, rule gone, connect
      times out; `ADB_WIFI=1` -> open, a "0" request does not close it, line removed -> closed; rebooted with a stale
      request and `adb-open`: "closed, USB only (uptime 14s)" before `netwatch: link up`, both files gone, HA back.
      Over the ESPHome API with the device key (aioesphomeapi, beside HA): switch listed, off; on -> reported on at
      once, port 5555 and the rule within 9 s; off -> closed within 9 s. `fake_ha_esphome.py`: refused over plaintext,
      request/answer/expiry and the 15 s withdrawal. Builds for donut/radar/biscuit. Not done: the Dot 3 and the Echo
      2 (both only reachable over Wi-Fi: they close it at the push, set `ADB_WIFI=1` first to keep working as before);
      `wakeword.sh` over Wi-Fi (keeps `ADB_WIFI=1 # wakeword.sh` through its two reboots) not run.
      Gap found the same day: a new Echo that does not get adopted (or loses its key, or runs Wyoming) had no way in
      but USB, and on donut and radar that is soldered wires the setup says may come off. So the push port also
      opens it for the holder of the update key: `HMOTA-ADB1` -> 32 random bytes as hex -> EdDSA signature over
      "HMOTA-ADB1\n" + nonce (never a valid bundle, which starts "HMOTA1\n"; a fresh nonce each time, so a sniffed
      exchange does not replay) -> same request file -> "OK" once `adb-open` is there. `otatool adb`,
      `scripts/adb-wifi.sh`. `ota_push_test.sh`: right key opens, other key refused without a request, new nonce each
      time. Dot 2: `scripts/adb-wifi.sh` -> "OK ... open for 30 min" and root over Wi-Fi in 3.5 s. Left: hassmic not
      running at all, or the update key lost -> USB only.

- [x] Mute shown inverted on the Echo 2 (2026-09-30, user: button lit and mics cut, no red ring, HA "unmuted", each
      press the wrong way round). radar and biscuit ran with `privacy_latch = 0`: `buttons.c` counted KEY_MUTE from
      "unmuted", so a daemon started with the latch on (push update, restart) was wrong from then on (boot.log: two
      "mic mute button: 1" in a row = a restart between them). The state is there after all, not under
      `gpio-privacy` but in the keypad driver: `/sys/devices/soc/10010000.keypad/amz_privacy/state` (world-readable;
      read 1 on the Echo 2 with the button lit while hassmic said 0; `privacy_state` beside it read 1 too; DT
      `amz_privacy/hw_latch` = 0). Both boards now name it with `privacy_latch = 1`; same path in biscuit's
      `init.mt8163_amazon.rc`, not read on a biscuit. `buttons.c`: one `latch_check()` for every model (reports when
      sysfs differs from what was last reported), called on KEY_MUTE / the gpio-privacy event and once a second, so a
      lost event or a read before the driver switched heals itself; a named file that is missing falls back to
      counting, with a log line. Builds for donut/radar/biscuit, `fake_ha_esphome.py` all good.
      Pushed to the Echo 2 with the latch still on: "mic mute button: 1" and the mics-off sound at start, before HA
      connected (the old build said 0 there); the user then pressed the button both ways: ring and HA follow.
      Echo Dot 2 (biscuit) the same day: pushed 0.3.0+58b9b63 (its boot.log after the restart and after a reboot:
      from `ota/current`, HA connected, Bluetooth controller and A2DP up, Sendspin session, `tries` back to 0 after a
      minute). The file is there, world-readable, 0 with the mics on, no "counting presses" line. The reboot was not
      planned: `cat` on every file in `amz_privacy/` reached `power_button_state`, whose show function takes the
      kernel down (`gpiod_get_raw_value` on a NULL gpio from `get_power_button_state`, watchdog reset, last_kmsg);
      only `state` may be read. Rest of `board.c`/`device.conf` checked there: event1 `mtk-kpd` (KEY_HELP, KEY_MUTE,
      volume down), event2 `keys` (volume ±), `mtktscpu` = thermal_zone1, 30 `volume_step` animations, product
      `biscuit_puffin`, slot `_a`, hassmic as puffin with all ten groups, `probe.sh`: the five stock files identical
      to the pinned firmware (its `head -c` does not exist in the image's toybox, on donut neither: now `dd`).
      Live: `kill -USR1` (simulated wake word) → arbitration "this Echo answers", listening, wake sound, HA answers
      `stt-no-text-recognized`, idle; `kill -USR2` (action button) the same with the touch sound. micAsr dump, 28 s
      in a quiet room: -73 dBFS, -45 dBFS in the second of the wake sound (speaker and mics work, the canceller
      leaves that much).
      Not done on the biscuit: the button pressed with this build (latch 1, red ring, HA), a spoken wake word and
      command, a phone on A2DP
- [x] System partition written from the running OS, no TWRP (2026-09-30, from issue #1: toybox `mount -o remount,rw /`
      fails because `/proc/mounts` names `/dev/root`, which does not exist, while `mount(2)` with MS_REMOUNT works).
      On all three models: `dm-0` named `system` spans the whole active slot (donut `system_b` = mmcblk0p14, radar
      and biscuit `system_a` = mmcblk0p13, same size), `ro=0`, `ro.boot.veritymode=disabled` (boot-root). `otatool
      remount rw|ro PATH` (walks up to the mount point: `/`, system-as-root) tried rw then ro on each, nothing written.
      `scripts/system/sysinstall.sh` does the writing: every script `sh -n`, product checked, each file staged as
      `.new` then renamed (boot.sh last), unchanged files skipped, remount ro on exit. `install-system.sh` uses it when
      the OS is up (`--twrp` the old way). Every bundle now also carries `boot.sh`, `otatool`, `sysinstall.sh`,
      `hassmic.rc`, `pryon_test`; `ota_watch` first has the bundle's own otatool verify it (it may become the one on
      /system). `ota-push.sh` pushes, asks the user to try it, and on "y" (or `--approve [host]` later: the version in
      `build/<model>/pushed-<host>`) sends `HMOTA-FACTORY1 <version>`: nonce challenge as for adb, the signature covers
      the version. hassmic drops `state/ota/factory`; `ota_watch` promotes `ota/current` only if its VERSION is that one
      and a running hassmic's `/proc/<pid>/exe` is its binary (not after a fallback, not a deploy.sh test binary).
      First version made it the factory copy automatically after a minute of running (`--factory`); the user wanted the
      approval step instead. 2026-10-02, with online updates, the user: no approval at all, every update (pushed or
      online) promoted after the shortest self test that means something: `main.c` selftest_thread once startup is
      through (capture open, wake word engine loaded, ports bound) and 1 s of mic audio came through the capture loop,
      30 s at most; `ota_healthy()` -> `state/ota/healthy` -> `ota_watch` -> `factory()`. `--approve`, `otatool factory`
      and `HMOTA-FACTORY1` removed. Trade-off: a version that passes and misbehaves later is the fallback too. Older installs get there with one push: the old `ota_watch` installs the bundle, its new
      `main.sh` handles the approval.
      On biscuit over `adb forward` to the push port: install by 0.3.0+1646bc1's ota_watch, then (automatic version) "sysinstall factory: 9 files written, factory copy is 0.3.0+aa353bb-dirty", `/` back ro, no `.new` left; with
      `tries` = 3 and a reboot it ran from `/system/hassmic` (lock up, HA/MA reconnected). Approval version: push without
      a terminal → "not the factory copy yet"; `otatool factory` for 0.0.1+nope → "FAILED … is not the installed update";
      `--approve` → "OK … is now the factory copy" (6 changed files), again → "already"; after a forced fallback
      (tries = 3, reboot) → "FAILED … installed but not what runs now". `tests/ota_push_test.sh`: approval with the
      key, another key, shell characters in the version, a bad signature. `install-system.sh "Echo Dot 2"` live: policy already
      patched, 0 files changed, stale `latency`/`VERSION` removed, rebooted into the factory copy, `/` ro.
      Not tried on a device then: the live policy write (only when the policy changes) and live `--uninstall`; both
      failed on a first install, fixed below (issue #2).
- [x] Light sensor and LED brightness in Home Assistant (2026-10-01). Stock's auto brightness is `ledcontroller`'s own
      (AutoBrightnessManager, `docs/re-platform.md` §1), not PuffinApp's, and it starts at boot without anyone asking:
      Echo Dot 2 with `ledctrl -a off -b 50`, rebooted, showed 9 again at 40 lx; radar and biscuit kept rewriting
      `persist.ledbrightness.bootup` under hassmic. So hassmic already had stock's behaviour and leaves it running.
      New: sensor "Illuminance" (the file the stock HAL reads: donut `0-0039/iio:device0/calibrated_lux`, TSL2572;
      radar/biscuit `0-0039/als_calibrated_lux`, TSL2540, after a `0-0029` TSL2584 path for another revision:
      `board.light_sensor`), switch "LED auto brightness" (default on), number "LED brightness" 0..100 (a level of its
      own switches auto off: the engine would overwrite it at the next change in light). Fixed level = one
      `ledctrl -a off -b N` (in that order within one call; two calls race). Nothing stock keeps the auto flag, so the
      settings file does (two more fields) and a fixed level is applied again at start. Level shown =
      `persist.ledbrightness.bootup`, which ledcontroller writes on every change, auto steps included; polled 1 s,
      lux sent at 10 % (≥ 1 lx) change or after a minute. `fake_ha_esphome.py`: entities, flicker not sent, fixed
      level turns auto off, settings kept (at the end of the test: its sleeps moved the mic checks' place in the 8.4 s
      capture loop and they failed).
      Echo Dot 3 (pushed, not approved): 67-68 lx in HA = sysfs, auto 13 (engine: 0.26 × 67 − 4.3); 60 → ring 60, auto
      off, file `0 60`; auto on → 74, 51, 26, 13 over 4 s (engine restarts from 98 and ramps); fixed 40, then
      `ledctrl -a on` as at boot (13) and hassmic killed: 40 again once HA reconnected. Echo Dot 2 over USB: 40 lx,
      auto 9, same sequence. Echo 2: builds, not tried. Left at auto on all Echos.
- [x] Sound detection (Alexa Guard) as Home Assistant events (found 2026-10-01; stock path and model worked out, and
      built, the same day: `docs/re-aed.md` section 4). ESPHome switch "Sound detection" (off by default, in the
      settings file) and event entity "Sound" (8 types; smoke/CO merged, human presence left out). A window is dropped
      when the Echo played something in it. CPU 13 % of one core (Echo Dot 2, `aed_test`). `fake_ha_esphome.py`: entities, default off,
      event, announcement window dropped. `hassmic-qemu` on the stock model: glass clip gives `glass_break`, footsteps nothing.
      Echo Dot 2 (pushed, not approved), room audio, about 45 min with the switch on: a real cough gave `cough` in
      two windows in a row, twice; nothing else fired. The firmware ships Amazon's acoustic event detector on all three models
      (`/system/local/models/AED/`): 12 types with their thresholds in `AED.json`. `libAED.so` is only the AVS-SDK
      wrapper; `/system/vendor/lib/libaed.so` is MediaTek's crash reporter, unrelated.
      **Stock:** PuffinApp runs a second Pryon decoder on its one mic ring (`puffin-micStream`, fed from `micAsr`,
      the same as the wake word). It is off until the cloud enables it through `SmartHomed`, and it is paused by the
      front end's low-power sound detector. `SmartHomed` cuts the clip out of that ring and sends it to Amazon to
      verify or to report. The ring flashes cyan (with `state_sent_to_cloud.mp3` if the user's "acoustic
      confirmation" is TONE) when a clip goes out for verification. Only a verified event reaches the app or
      routines. Near-miss audio is uploaded for training.
      **Under qemu** (`src/tools/aed_test.c`, stock `libpryon.so`): the same API as the wake word, plus
      `PryonApi_SetAcousticEventDetectionResultCallback` and the client properties `AcousticEventDetectionEnabled` and
      `aed_<type>_enabled`. Without those properties nothing is reported. There is one report per 9.98 s window
      with all enabled types' scores, so up to ~10 s latency. ESC-50 clips:
      - found: dog, glass, snoring, water and real `micAsr` speech; a synthetic T3 smoke alarm 0.990;
      - not found: synthetic T4 CO beeps;
      - false hits: glassBreak on a dog, pouring water and a toilet flush; humanPresence on knocks and an alarm clock;
        beepingAppliance on a cough;
      - shared scores: smokeAlarm = smokeSiren = carbonMonoxideSiren, and cough = runningWater, in all 28 windows.
      Newer DAVS model (`scripts/artifacts.sh`, then `wakeword.sh`, 2026-10-01): retrained weights, smoke/CO
      threshold 0.845, the same results on all clips except a near miss on the CO beeps. The /system model is good enough.
      Open: played test sounds in the room (smoke alarm, glass, dog) and some talking or TV; whether to require a
      type in two windows in a row.
- [x] Whisper detection (asked for 2026-10-04, built the same day; `docs/re-whisper.md`). Binary sensor "Last request
      whispered" for the conversation agent's prompt template (`whisper_pryon.c`): one detector per request, fed from
      the start of streaming, scored on HA's VAD end, so the state is in HA before STT ends. Listed only with the
      model in `/data/local/hassmic/whisper` (`scripts/artifacts.sh`). `fake_ha_esphome.py`: unknown until a request,
      on at its VAD end. Live with HA (Echo Dot 2): fed from the start of streaming, the normally spoken wake word's
      tail took whispered requests to 641-979; with the first 0.5 s not fed, whispered 999/984, normal 1/0.
      Open: that the agent's template sees the new state in time (not checked with an LLM yet), other speakers.
      Background: stock has it on the device: `libpryon.so`
      exports `WhisperApi_*` (a detector of its own: audio in, end of utterance in, one `confidence` out, whispered
      above 500 as AHE reads it). Only AHE (the local "hybrid" engine) calls it, and only when Amazon's
      `ahap-policy` selects static Litespeed pipelines. Donut's policy (2026-09-21) selected caching, so the model was
      never fetched and the detector never ran. PuffinApp has no whisper code; for cloud requests the cloud decides
      from the audio, and the result inside AHE goes upstream only through a stub. API reversed from AHE's call
      sites. On the Echo Dot 2 (same libpryon): attributes `engineCompatibilityIds [1]`, model set load and the
      handlers work, and `createWhisperDetector` refuses a set without a whisper section in `pryon.config`.
      Model: DAVS `alexa-hybrid`/`whisper-static`, AHE's filter `ecid` "6" plus `modelClass` `odie-litespeed`.
      `tools/davs-fetch.py ... whisper` and the "Other artifacts" list of `scripts/artifacts.sh` ask for that with
      `locale` en-US, the only request DAVS answers (2026-10-05, Echo Dot 2's and Dot 3's tokens: 5 filter variants x 13
      locales, 64 x HTTP 404 each; no model per language, none per device type). Fetched 2026-10-04: a DNN on 64 LFBE plus a DNN speech
      detector, thresholds per locale (default 922, de-DE 862). `src/tools/whisper_test.c` on the
      Echo Dot 2, German commands from 1–2 m, one detector per command: whispered 996/995/998, normal 1/0/1, quiet
      voice 18/1/2; with 0.5 s before and 1 s of silence after the speech 997-999 against 0-8. Non-speech sounds
      score high (859), so only the command span may be scored.
- [x] Playing on a Bluetooth speaker (2026-10-01, `btout.c`, `a2dp.c`, `sbc.c`; `docs/re-a2dp-source.md`): the mixer
      keeps its own A2DP route; hassmic stands in for btmanagerd towards it. LIPC `com.doppler.audiod`
      `A2DPSourceConnect` `1:<12 hex>` / `0:…` switches the mixer's single output; the HAL's abstract sockets
      `.a2dp_ctrl` / `.a2dp_data` served as Fluoride does (CHECK_READY only with a stream, START acked once AVDTP START
      is), 44.1 kHz stereo s16 read paced (unpaced the HAL wrote 234 kB/s; paced exactly 176.4 kB/s, 0 drops over
      minutes). AIPC service uuid 0 through `libace_aipc.so` (`thread_option` 1, else `aceAipc_start` never returns),
      created as `btmanagerd_aipc_tmpfs` via fscreate; `alexa-off.sh` removes btmanagerd's `/dev/aipc/0`. Without it
      every route change stalled ~20 s (`lipc-set-prop` timed out at 10 s); with it 0.08 s. SBC encoder (analysis
      filter from the spec, joint stereo per subband, CRC): `make unit` decodes it with libsbc at every bitpool tried,
      SNR equal to libsbc's own encoder within 0.1 dB, first frame bit-identical apart from the CRC we got wrong at
      first. Radio: inquiry (class audio/video or service bits rendering + audio, strongest wins), Create Connection
      (paged every 10 s, then every 60 s), SSP / PIN 0000 only with the chosen speaker, AVDTP initiator (discover, caps,
      SBC 44.1 kHz joint stereo 16/8 loudness, bitpool min(53, sink max), open, media channel) or acceptor when the
      speaker configures our source SEP 0x30; SDP: A2DP source record added, AVRCP controller now categories 1 + 2.
      Checked on air, Echo Dot 2 with BlueZ 5.87 / PipeWire on the PC as the speaker: pairing, configuration (bitpool 53,
      MTU 1021, 8 frames per packet), route switch, reconnect with the stored key; a 48 kHz stereo tone through `mixplay`
      arrived at the PC as 1000.2 / 3000.5 Hz on the right sides, 74 s without a gap. Levels: PipeWire applied the
      Echo's 30 % as AVRCP volume (we answered as a phone's target), -31 dB more: the speaker link offers no absolute
      volume as target now. Volume model: a volume of its own while on the speaker, taken from the speaker
      (RegisterNotification INTERIM), Echo buttons / HA / MA set it (SetAbsoluteVolume), the speaker's CHANGED come
      back; mixer at full scale meanwhile (`core_speaker`), the Echo's own volume set aside in `state/volume.speaker`
      and restored before the route goes back. BlueZ refused the volume event (REJECTED, invalid parameter) while our
      controller record said category 1 only (`avrcp_volume_supported`, `volume_category`); the PC keeps the SDP
      records of the first pairing, so the fix needs a fresh pairing there: **absolute volume not yet seen working**.
      Not done: a real speaker; the speaker's own reconnect to us; echo cancellation with the sound coming from a
      speaker elsewhere (the wake word through loud music); choosing among several speakers (an ESPHome select's
      options reach Home Assistant only on connect, `manager.py` `device_info_and_list_entities`, so it would need a
      reconnect: kept as "strongest in pairing mode"); `BTUnpair` (line out) only switches playing on the speaker off.
- [x] First install from the running OS fixed (2026-10-01, issue #2: an Echo Dot 2 set up by someone else, guided
      setup). Four faults on the way, all first-install only: `firmware/<model>/images` was never created
      (`payload_dump.py` makes it now); adb gone for a moment between `deploy.sh` and the lockdown (now `wait_adb`);
      `otatool remount rw /system/hassmic` before that directory exists (realpath fails; also after `--uninstall`, so
      `/` stayed rw until the reboot: both now `/system`); and no new file can be made in `/` under the running OS.
      `/` is labelled `rootfs` on the ext4 system partition, a new file inherits that, and the policy has `file_type
      labeledfs` and `rootfs rootfs` associate but not `rootfs labeledfs`: biscuit `touch /hm-direct` → "avc: denied {
      associate } scontext=u:object_r:rootfs:s0 tcontext=u:object_r:labeledfs:s0", although `su` is permissive (the
      check's source is the file's label). Relabelling to `rootfs` is refused the same way. A file made in `/system`
      (`system_file`) and renamed into `/` keeps its label, also over an existing file (tried). `sysinstall.sh` stages
      files for `/` that way; `/sepolicy` and `/sepolicy.pre-hassmic` end up `system_file`, as TWRP's install left the
      backup. On biscuit: live `sysinstall uninstall` (base policy back, md5 = `device-logs/backup`, `/system/hassmic`
      gone, `/` ro), then `install-system.sh` with neither `/system/hassmic` nor the backup: 17 files, policy written,
      rebooted Enforcing with the patched policy from a `system_file` `/sepolicy`, hassmic from `/system/hassmic`, lock
      up, puffin stopped. Setup's unlock step now takes an Echo already unlocked (Fire OS on adb: stock has none) to
      TWRP with `adb reboot recovery` instead of asking for the unlock (biscuit: "unlocked already", TWRP up; donut's
      kamakiri and radar's amonet also leave TWRP on the recovery partition). Not tried: donut and radar first installs.
- [x] Ultrasound motion sensing (2026-10-01): tried, not possible with any of these Echos. donut's mics can: the two
      TLV320AIC3101 on TDM in (`TDM_Capture`, card 0 dev 1, 4 ch S32; mixer opens it at 16 kHz) record at 48 and 96 kHz
      with `tinycap` once `mixer` is stopped, and keys jingled over the Echo raise every band by ~40 dB up to 45 kHz. The
      speaker cannot: a 2-23.8 kHz sweep at -24 dBFS on DL1 (pcm6p, which feeds both the TAS2770 and the line out)
      reaches the mics at -22 dB at 2 kHz, -55 dB at 13 kHz and nothing past 13.4-13.6 kHz, the same with playback at
      44.1 kHz and with the mics at 48 kHz (the TAS2770 follows the stream's rate, `0x0a` = 0x17 / 0x37: no resampler).
      The DL1 loopback (`DL1_AWB_Record`) has the sweep clean up to 22.8 kHz, so the cut is the speaker or its output
      stage. 19 / 20.5 / 22 kHz tones at -12 dBFS: nothing over the -120 dB floor. biscuit and radar: the mic PCM
      (`amzn_mt_spi`, dev 24) is 16 kHz, 9 ch S24_3LE only (`tinypcminfo`); `TDM_Debug_Record` (dev 13) opens at 48 kHz
      x 8 but reads all zeros. Amazon's own "Ultrasound Presence Detection" (USPD) is in donut's firmware but off:
      `LASP_CMD_SET_ULTRASOUND*`, `UltrasonicPresenceDetector` in libasp, Alexa.MotionSensor in PuffinApp, no
      `*Uspd.tflite` models, no ultrasound section in AFE.cfg. A `mixer` record stream `ultraSound` exists (reports
      2000 Hz x 9 ch x 32 bit) but the HAL refuses it (`Could not open input stream -22`), and **mixer retries every 10 ms
      for ever after the client has gone** (35 % CPU, logd 18 %) until `stop mixer; start mixer`: do not open it.
- [~] Wi-Fi motion, experimental (2026-10-01, `wifimotion.c`): the receive level of the Wi-Fi radio as a motion sensor.
      donut's driver (`wlan_mt76x8_sdio.ko`, MT7668, gen4m) has CSI commands: `SET_CSI <mode 0/1> <wf 0/1> <frame slot
      0-3> <frame type 0 = beacon>` (argc 2 or 5, band fixed 0, role 1; command 0x4c to the firmware, 8 bytes) and
      `GET_CSI 0|1` (I or Q, up to 256 values, `nicEventCSIData` registered in the event table), but on 2.4 GHz
      channel 6 the firmware never sends any ("No CSI Data" for every variant; the RAM code is encrypted, 7.97
      bits/byte). `RX_STAT` works, also as puffin: RCPI RX0 (RX1 = 255, one chain), SNR, frequency offset, instant
      RSSI, FAGC, counters; ~10 answers a second from a shell loop. Recording at 10 Hz for 7.5 min (router one room
      behind the Dot, -52 dBm): scatter of RCPI over 2 s 0.57 empty room / 0.60 someone still / 1.21 walking (to
      2.2 crossing the path), mean RCPI 112 -> 115 and SNR 31 -> 34 while walking; single odd frames give 2.4 in an
      empty room. SNR, wideband RSSI and FAGC separate less. Detector: median of 3, scatter of 20, at least 10 of the
      last 20 over 1.2 x 1.15^(5 - sensitivity), 30 s hold. Replayed on the recording: motion at 18.7 s (leaving),
      123.6 s (walking), 323.6 s (leaving), 359.7 s (unknown), nothing in the still or empty parts. Entities only where
      the driver answers (biscuit: no private ioctls at all; radar likewise). ESPHome switch (config, off by default),
      binary sensor `motion` (unknown while off), sensitivity 1-10; settings file fields 13 and 14. Tests:
      `tests/unit/wifimotion_test.c`, `fake_ha_esphome.py` (HASSMIC_FAKE_WIFI). Pushed (not approved) to donut, biscuit, radar:
      donut "available, off unless switched on", the other two "not available on this Wi-Fi driver", Home Assistant
      connected on all three. The real ioctl path as puffin on donut (test program around wifimotion.c, 30 s): readings
      from the first poll, "still", "moving" after 16 s (not known whether someone moved).
      biscuit and radar (gen2 driver for the MT8163 connsys, built in): `/proc/net/wireless` makes the driver query the
      firmware on every read (`nicCmdEventQueryLinkQuality: rRssi`, ~7/s) but prints a stale value; the firmware's own is
      smoothed, whole dB (-57 -> -54 over 15 s), only in the kernel log; `signal_poll` is cached; Android's private
      `RSSI` command returns nothing. The driver keeps the access point's beacon level (`scanAddToBssDesc`: RX header
      byte 9 -> `BSS_DESC_T` +90, time in ms at +56; BSSID +8), but its entry was 4 h old: connected, the firmware
      filters beacons. Station record RCPI (+61, 444-byte records) is only set at association and by a statistics
      query. Data frames carry it in the same RX header (`SW_RFB_T` +16), unread. Kernel 3.18.19 (both, different
      builds, the hook site byte-identical): modules, no signing, no modversions, `kallsyms_lookup_name`, no kprobes,
      text writable (no DEBUG_RODATA). `src/kmod/hassmic_rcpi.c`: `b` from `nicRxProcessDataPacket`'s first
      instruction (`push {r3-r9, lr}`, checked) to a trampoline, under `stop_machine`; reads via `probe_kernel_read`;
      `/proc/hassmic_rcpi` "RCPI RX0 / AGE / FRAMES"; no exit. Built against kernel.org 3.18.19 + IKCONFIG from
      `boot.img` (identical to `/proc/config.gz` on both), AOSP arm-eabi-4.8 ("gcc version 4.8 (GCC)" as in the
      Echos' version string; its asm-offsets guard against 4.8.0-4.8.2 removed); vermagic "3.18.19 SMP preempt
      mod_unload ARMv7 p2v8 " and `this_module` 0x170, as Amazon's perfinfo.ko. On biscuit by hand: loaded, no oops,
      ~13 frames/s from the access point at idle, RCPI 103-104, hassmic unaffected (a wake word with arbitration
      meanwhile); `wifimotion.c` reads it as puffin. Pushed to biscuit and radar: "available ... (through the kernel
      module)"; main.sh loads it once the switch is on (radar: below).
      donut the same way since (2026-10-01): RX_STAT is the firmware's last frame from anyone on the channel and one
      firmware query per poll; `src/kmod/hassmic_rcpi4m.c` reads every frame from the access point instead. Kernel
      4.4.22+ arm64 (`boot.img`: MTK header, gzip Image; built by "gcc version 4.9 20150123 (prerelease)"), **no IKCONFIG**,
      no kprobes, `kallsyms_lookup_name` and `aarch64_insn_patch_text` there, `kptr_restrict` 2, SELinux enforcing
      but `su` permissive (insmod logs the `module_load` denial, loads). Stock modules: vermagic "4.4.22+ SMP preempt
      mod_unload aarch64", no `__versions`, no signature, `this_module` 0x300 with init at 0x158 and exit at 0x2d8:
      kernel.org 4.4.22 arm64 defconfig + `devices/donut/kconfig` (PREEMPT, LOCALVERSION "+") gives exactly that, with
      AOSP aarch64-linux-android-4.9 and `-fno-pic` (its default PIC clashes with modules' `-mcmodel=large`). The driver
      is a module itself (`wlan_mt76x8_sdio.ko`, same md5 on the Echo as in the image, not stripped): gen4m
      `nicRxFillRFB` parses the RX descriptor groups into `SW_RFB_T` (`ucGroupVLD` +32 from DW0 bits 25-28, group 3 =
      RX vector at +64); RCPI0 = byte 0 of RXV word 3 (`nicRxGetRcpiValueFromRxv`); `nicRxProcessDataPacket` calls it
      once at +0x50 and then copies the RXV into the AP's `STA_RECORD_T`. The module turns that `bl` into `bl hm_fill`
      (found by target, bl-for-bl is hot-patch safe, written through `aarch64_insn_patch_text`). On donut by hand:
      loaded, "nicRxProcessDataPacket+0x50 (call to nicRxFillRFB) hooked", ~17 frames/s at idle; 60 s beside RX_STAT
      polled at 5 Hz: both 112-120, mean 115.0 / 115.1, scatter over 2 s 1.77 / 1.71. Pushed: hassmic "available ...
      (through the kernel module)". main.sh now loads a module only with the link up (donut's driver loads late in
      the boot; a failed load is not retried until reboot). Rebooted with the switch on: driver up at 19 s
      (`initWlan`, started by wifisvc), main.sh's insmod at 52 s ("kmod: hassmic_rcpi4m.ko loaded"), hooked, no oops,
      hassmic reporting motion. That boot joined at RCPI 92-94 (-64 dBm, before 113-115): RX_STAT says the same.
      **Then motion all the time** (from 23:06): readings at 90-94 and 99-101, interleaved frame by frame. A second,
      throwaway module chained onto `hm_fill` logged descriptor and RX vector per frame (231 s, 2968 frames, 12.9/s):
      three kinds, by RX vector word 0 (bits 0-6 rate, 12-14 mode, 15-16 bandwidth): 0x1000b legacy OFDM 6 Mbit/s
      (WTBL entry 4: broadcasts, 549 frames) at 99-101, 0x14008 VHT80 MCS 8 at 92-94, 0x14009 MCS 9 at 90-91 (entry 5,
      the access point's unicast). The AP sends each rate at its own power. Scatter over 2 s, 10 Hz last-frame polls,
      same recording (nobody walking through, as far as known): all frames 2.16 (63 % of windows over 1.2), unicast
      only 0.62 (3 %), broadcasts only 0.10 but 2 a second, every frame against its own rate's running level (30 s)
      0.21 (p90 0.44, max 0.82, none over 1.2). So the modules report KIND (packed with the RCPI in one atomic) and
      `wm_kind_norm` feeds the detector each reading's distance from its kind's level (30 s); RX_STAT has no kind (one
      level). donut: KIND = RX vector word 0 & 0x1f07f. Before the reboot (113-115, RX_STAT and module alike, presumably
      2.4 GHz) the mix did not show. Replayed through the real detector (10 Hz last-frame polls): one level 96 % of
      the time motion, rate-keyed 0 %; the labelled RX_STAT walk recording (out 0-120 s, walking 120-220, still
      220-320, out again; phases from that session) gives the same events as before (18.7, 123.6, 323.6, 359.7 s).
      biscuit and radar: the gen2 RX header has no rate (bytes 5-7: reorder flags, sequence number + TID, qmHandleRxPackets;
      radar's nicRxProcessDataPacket byte-identical); data frames come as 802.3 behind the 12-byte header plus
      (byte 4 & 3) padding, so `hassmic_rcpi.c` gives KIND = destination address bit 0 (group): the broadcast mix is
      gone, the unicast MCS steps are not. That keying replayed on donut's recording: motion 19 % of the time at
      sensitivity 5 (MCS 8/9 in runs of 1-5 s, 2 dB apart). Neither a step-tolerant statistic (spread after one fitted
      step: 14 % at the threshold that keeps the walk events) nor spread AND mean successive change (0 % but at a third
      of the threshold, 14 % just under it, and the 323 s event lost) holds up, so the detector stays. The Dot 2 and Echo 2
      have no VHT (a/b/g/n). On them (2026-10-01, 10 Hz polls): with the old module, 60 s each, biscuit one broad hump
      66-75 (scatter 0.99, 26 % of windows over 1.2; a weak link), radar two groups 76-80 / 83-87 (30 %): the mix
      there too. After pushing the KIND module and a reboot (main.sh loaded it at 50-60 s on both, radar's first load
      that way; hooked at c03aac70 / c03ad864, no oops) both sat higher (another band or AP: biscuit 97-98, radar 108).
      3 min each: biscuit 17 frames/s, unicast 96-99, broadcasts (8 %) 96-99, no motion either way; radar 9.4/s,
      unicast mostly 108, broadcasts (10 %) 107-109 (109 most), motion 15 % of the time keyed or not, from stretches of
      104-107 with frame-to-frame noise (00:08:10-00:08:45 and shorter), no two-level steps: looks like movement, not
      rates. Open: the threshold (1.2 was set on RX_STAT's 0.57 still scatter) needs a walk test on the normalised
      readings; a gen2 recording where the router changes rate.
      adb after a reboot: the signed open (`adb-wifi.sh`) sets the port and restarts adbd, but adbd resets every
      connection until the HA switch goes off and on again (twice, 2026-10-01). Later the same "offline" on
      biscuit and radar after their reboot, gone with `adb kill-server` on the PC: the PC's adb server keeps the
      transport from before the reboot, `adb disconnect` + `connect` answer "already connected". Not the Echo.
      User, 2026-10-01: with RX_STAT the Dot 3 triggered a lot while nobody was on its floor (what led to its module).
      2026-10-01, all three on with the modules, user's verdict: "seems to mostly work" (not counted). Open:
      false alarms counted over a day and a night; while music streams (more frames, other rates). Without the kernel toolchain the donut build has no module
      and RX_STAT stays the source.
- [ ] Online updates and CI (2026-10-02, issue #3: "centralized config/update instead of the OTA command line").
      `.github/workflows/build.yml`: main -> prerelease `v<version>-beta`, release -> `v<version>`, bundles
      per model signed with a release key (secret `RELEASE_SIGNING_KEY` of environment `release`, main/release only, used by
      the release job alone: build jobs run PR code and sign with a throwaway key; public `keys/release.pub`, generated 2026-10-02 with
      `otatool keygen`). No firmware in CI: `make STUBS=1` links against stand-ins built from `devices/<codename>/stubs/*.syms`.
      First stub lists (one shared list, empty C functions) gave different binaries on biscuit: the stock libraries also
      define or refer to `main`, `_end`, `_edata`, `__bss_start`, `__emutls_get_address`, so lld exports those from the
      executable, in the order it meets them in the libraries (and the GNU hash table orders a library's dynsym by bucket,
      the integrated assembler sorts symbols by name). Now per model, in each library's own order, as GNU as assembly
      linked with a SysV hash: all 18 binaries (6 x 3 models) byte-identical to the firmware link (`tools/mkstubs.sh`).
      That needed reproducible builds: ESPHome's "compiled" time is the commit's (`BUILD_TIME`, `LC_ALL=C`: "Okt" vs "Oct").
      Wi-Fi motion's module in CI for all three: biscuit's and radar's IKCONFIG committed as devices/<codename>/kconfig
      (device.mk KCONFIG, in place of extracting it from boot.img at build time; modules byte-identical either way),
      Linux 3.18.19 (sha256 as kernel.org lists it) and arm-eabi-4.8 pinned to marshmallow-release 26e93f6.
      aarch64-linux-android-4.9's gcc is a Python 2 wrapper around real-*: CI links past it.
      Device side (`update.c`): HA select "Online updates" off/beta/release (settings field 15, off by default) and an
      ESPHome update entity (messages 116-118); GitHub's API (`/releases/latest`; beta: the highest version among
      `/releases?per_page=10`: first published day, the Dot 3 on beta was offered the release 150841 before the newer
      beta 154443, because GitHub's list starts with the release it marks latest, not the newest), bundle download
      through the firmware's libcurl 7.50.1 (dlopen; OpenSSL, CA store has USERTrust/DigiCert/ISRG roots), sockets opened
      by us as group 3990 (curl's own would be firewalled), `ota_handoff` -> root, which takes `update.pub` or the release key.
      `tests/fake_ha_update.py` (fake GitHub + aioesphomeapi + installer): 19 checks, incl. unkeyed channel/install refused,
      wrong-key bundle refused before root, channel kept across restarts. fake_ha_esphome 93/93, ota_push_test all good.
      The firmware's libcurl under qemu-arm (donut rootfs, names given with CURLOPT_RESOLVE: no netd there):
      api.github.com TLS 1.2 ECDHE-ECDSA-AES128-GCM, Sectigo chain verified from /system/etc/security/cacerts, 200; the
      download's 302 to release-assets.githubusercontent.com followed, 200 (ECDHE-RSA-CHACHA20).
      Versions: first semver `0.3.0-rc.<commit count>`, which needed VERSION bumped after every release (HA ranks 0.3.0
      above its candidates). Now the commit time in UTC, `2026.10.02.091530`, the same for beta and release of a commit:
      AwesomeVersion 25 (HA's) takes it as CalVer and orders it right across day and month boundaries; a suffix of any
      kind (`+sha`, `-dev`, `.dev0`) makes it "unknown" and incomparable, so local builds (`+<commit id>`) are offered
      whatever is on the channel (HA: incomparable and different = update available).
      Secret set 2026-10-02 (environment `release`). Open: branch protection on main/release, first real beta; on a device: DNS through netd and the egress lock's group match
      on curl's sockets; biscuit/radar kernel module in CI.
      2026-10-03, issue #5: setup builds lacked Wi-Fi motion's module (the setup never fetched the kernel tools);
      `make kernel-tools` (scripts/kernel-tools.sh, pins in device.mk / Makefile) now does, for the setup and CI alike.
      Radar module from a fresh download = one from the existing toolchain/ apart from .note.gnu.build-id (paths).
      Prebuilt installs (scripts/lib/build.sh): the release of HEAD (tag at HEAD, clean tree) instead of a build here.
      donut's v2026.10.03.203213-beta against `make STUBS=1 RELEASE=1` of ba40444 here: hassmic, runas, otatool, mixcap,
      mixplay, pryon_test, latency byte-identical. The PC's otatool is Python now (scripts/otatool.py, stdlib: EdDSA as
      Monocypher's, BLAKE2b): same keys and byte-identical signatures as the C one (tests/otatool_test.sh, 9 checks), the
      real release bundle verifies, one flipped byte does not; ota_push_test and fake_ha_update pass with it.
      probe.sh: devices/<codename>/probe.md5 from the pinned firmware (all three firmwares' sha256 match their pins).
- [~] microWakeWord as a second wake word engine (2026-10-07, user request: a switch on the page, Amazon's engine the
      default, what is lost said plainly, the models managed on the page).  No TensorFlow on the Echo: `mww_features.c`
      ports TFLite Micro's microfrontend with microWakeWord's settings (30/10 ms, 40 channels 125-7500 Hz, noise
      reduction 10/0.025/0.06/0.05, PCAN 0.95/80/21, log shift 6) including kissfft's 16 bit fixed point FFT;
      `mww_model.c` is an int8 interpreter for exactly the ops microWakeWord's streaming models use (all 9 models of
      esphome/micro-wake-word-models v2 + experiments, and pymicro-wakeword's 4: CONV_2D, DEPTHWISE_CONV_2D,
      FULLY_CONNECTED, LOGISTIC, QUANTIZE, CONCATENATION, STRIDED_SLICE, SPLIT_V, RESHAPE, VAR_HANDLE, READ_VARIABLE,
      ASSIGN_VARIABLE, CALL_ONCE); anything else is refused at upload.  The user asked for "ONNX" models: microWakeWord's
      are .tflite (ONNX is openWakeWord's, a different and much heavier engine), so .tflite + .json it is.
      Measured on the PC: features bit-identical to pymicro-features 2.0.2 (what microWakeWord trains with) on synthetic
      audio, espeak phrases and three micAsr captures (3580 windows), and 13094 windows of cap-first against TFLM's own
      sources; every inference of the 13 models within 1/256 of TFLite 2.3 (ai-edge-litert) with BUILTIN_REF kernels,
      variables carried along (`tests/unit/mww_ref.py`, in `make unit`).  First port differed: kissfft's real-FFT
      twiddles use the complex half's length; TFLM's current tables are single precision (cosf, log1pf, powf).
      The armv7 build under qemu (bionic) gave 2 filterbank weights one step off: bionic's log1pf is not correctly
      rounded; now from double functions rounded to float: tables, features (13094 windows) and probabilities (4364
      inferences) identical ARM vs PC.  Level: espeak "Alexa"/"Okay Nabu"/"Hey Mycroft" at -20..-62 dBFS keyword rms over a
      -67 dBFS white floor, window means 229-255 of 255 with 0, +12 or +24 dB gain alike (PCAN): micAsr goes in as is.
      testdata/alexa_espeak.raw: alexa model fires twice per loop (window mean 255), okay_nabu stays under 10.
      CPU on the PC: 0.05 ms per 30 ms of audio (okay_nabu).  Untrusted files (page uploads, copies from other Echos):
      flatbuffer offsets bounds-checked, zero points and scales validated, accumulators bounded (kernel <= 32768
      products, bias <= 2^30); fuzzed 120k mutated models (22k of them loaded and ran) and 300k mutated manifests under
      ASan + UBSan, clean after fixing what the first run found (an int32 overflow from a huge zero point).
      Engine switch live (`core_wake_engine`): the list is per engine, Home Assistant's link is closed so it reads the
      new one (`fake_web_mww.py`, 32 checks: uploads with and without manifest, refusals, switch refused without a model,
      rename/threshold/window, the page's and Home Assistant's pick, download, copy as artifact `mww:<id>`, a copy that
      does not load, deletes down to none = back to Amazon's engine and saved, a restart loads microWakeWord directly).
      Page (headless Chromium): engine cards with pros and cons; picking microWakeWord opens the page's own dialog (no
      browser popups anywhere on the page now) naming the losses and, without a model, the wake word to start with
      (ESPHome's from GitHub, or files: a lone manifest refused in the dialog), added before the switch; the models card
      only while microWakeWord is on; files paired .tflite/.json, rename, tune, use, delete (the last one: Amazon's again).
      Lost with it, on the page: "<wake word>, stop" (Pryon's second keyword), Pryon's threshold hints while playing,
      Amazon's wake words, the front end's ESP energies in arbitration (`wake_afe_times` 0: the own SNR score; Echos on
      different engines with the same wake word name still compete, on different scores).
      Third engine, "homeassistant" (user request: openWakeWord stays in Home Assistant, the Echo only streams; asked
      after a user's EchoMuse Forge model, an openWakeWord .onnx classifier, which HA's openWakeWord app does not load:
      .tflite only per its README and HA's docs): the existing -w remote, now from the page.  Decided at start
      (arbitration, the protocol's flags): switching writes state/config, then state/restart for root's watcher
      (main.sh, as a rename); main() reads it before anything else.  -w remote in ARGS wins and the page cannot change
      it.  New with it: EV_WAKE_END (Home Assistant heard it) -> wake sound, ring, and the front end's listening mode
      until the command ends (before, -w remote never set it, so a command longer than ~1.5 s faded under the
      cancellers, 2026-09-30 captures).  fake_web_mww.py +9 checks (restart asked, wake=remote after it, no wake words
      offered, the stream at once with USE_WAKE_WORD, the event answered, back again, -w remote refuses).  The page's
      dialog and guide follow HA's docs of 2026-10 (add-ons are "Apps"); not checked against a live HA here.
      Open (device): detection rate against Pryon at the same distances, false wakes per day (TV, music), CPU on the
      Echo, an Echo with Pryon and one with microWakeWord in one arbitration network; the remote path with a real
      openWakeWord on the Dot 2.  Not done: ESPHome's "stop" model
      as a second model while a timer rings or a reply plays (as Voice PE does); openWakeWord/ONNX.
- [~] Setup from a phone (OOBE) and factory reset (2026-10-07, asked: found over Bluetooth by the HA app, Wi-Fi, the
      node name if possible; rainbow while not set up (then stock's orange setup-mode spinner, asked the same day); "not set up" = after the install or a reset until HA took it on;
      no button press while not set up; reset wipes all of hassmic plus Wi-Fi, by the action button held 10 s, HA, the
      page). Improv Wi-Fi over BLE (spec of improv-wifi.com, packets as py-improv-ble-client 2.0.1, HA's improv_ble):
      `ble.c` got the peripheral role: legacy ADV_IND 100-150 ms (flags, the 128-bit service, service data 4677 =
      state + capabilities 0x0f: 31 bytes exactly; the name in the scan response), one link a phone opens (conns[SRV],
      outside the proxy's three slots), a GATT server (Generic Access + Improv; MTU, Find Information, Find By Type
      Value, Read By Type / Group Type, Read / Blob, Write / Command, prepared writes, notifications), SMP pairing
      refused (not supported). `improv.c`: advertises when not set up (`core_oobe`) and 20 s offline (authorized), or
      set up and 600 s offline (authorization required, the action button gives 60 s); "provisioned" 60 s after, then
      off. RPCs: Wi-Fi (through `wifi.c`/root's `wifi.sh`, as the page's switch; error 3 on its failure), identify,
      device info, scan (one result, WPA2/open only, as many as fit 255 bytes); host name = node, device name =
      display name: dropped again the same day (asked: one name, never stored, given in HA only; see the name item).
      Adopted = HA's voice assistant subscribed
      (`core_link` ready) -> `state/adopted`; existing installs with `state/api_key` count as adopted. Ring:
      `setup-mode` (stock's orange OOBE spinner) while not set up; `authenticated_setup_mode` while a phone waits for the button; `factory-reset`
      from 5 s of holding and while root resets (all three on donut, biscuit, radar). Reset: `state/reset` -> root
      (`main.sh` ota_watch): stop hassmic, `wifi.sh forget` (every network but P2P groups, save_config), state/ emptied,
      avahi's service file removed, start hassmic. netwatch no longer starts wifisvc with no network saved. wifi.sh's
      lease on an Echo that never had one (no `dhcp.wlan0.*`): `start dhcpcd-wlan0` (wifi-join.sh's fallback), not
      `dhcpcd -n` (would start a daemon outside init). `tests/fake_improv.py`: a controller on a pty (H4: commands,
      LE Connection Complete as peripheral, ACL with credits), a phone's GATT client, py-improv-ble-client's packets,
      real wifi.sh on the fake tools, aioesphomeapi for adoption and the reset button, the keypad as a FIFO for the
      holds.  Open (device): the MT7668 advertising beside scanning/A2DP; a real phone (HA app on Android and iOS);
      whether wifisvc or Amazon's own Wi-Fi store brings forgotten networks back after a reset; init's
      `dhcpcd-wlan0` on a fresh Echo; acebuttond's 5 s hold (Amazon's setup mode; its services are stopped) doing
      nothing visible; a Wi-Fi password with non-ASCII characters (refused, as on the page).
- [x] One name (2026-10-07, asked after the first reset on the Dot 2 kept "Echo Dot 2" from hassmic.conf: no name in
      several places, a reset resets, the name comes from Home Assistant's adoption; default by model). HA's API has no
      message that gives a device a name, so as the Voice PE: `name_make` = model (board.default_name: Echo Dot 3 /
      Echo Dot 2 / Echo 2) + the Wi-Fi MAC's last 3 bytes, waited for (radar brings wlan0 up late), stored nowhere.
      Gone: hassmic.conf `NAME` (ignored, main.sh no longer passes `-n`), install-system.sh's and setup.sh's name,
      device.conf `DEFAULT_NAME`, `state/name`/`state/node` (deleted at start), the page's rename (`/api/name`) and
      Improv's host name/device name commands (capabilities 0x07). `-n` stays for the PC. Existing Echos change node
      name on update (asked: no migration); HA keys on the MAC. fake_web: old files ignored and the model's name.
- [~] Timers in Home Assistant (2026-10-08, GitHub issue #12: a ringing state that clears on dismissal or timeout,
      the timer's name, a configurable ring time). HA keeps several timers per satellite and pops each from its list
      before it sends "finished" (`intent/timers.py` `_timer_finished`), so a "cancelled" never names a ringing timer;
      the old code stopped the ring on any cancel, and a second "finished" during a ring was dropped (`alarm_on` already
      1, the 60 s not restarted). Now `main.c` keeps up to 4 ringing timers by HA's timer id (field 2), one ring for all,
      the ring time restarted by each, stopped all at once by button / wake word / "stop" / media player STOP (the only
      way from HA) / ring time (`timer_ring`, 0-600 s, 0 = until stopped, default 60, page-only, exported).
      ESPHome: binary sensor "Timer ringing" (`timer_ringing`, always listed), text sensor "Ringing timers" (diagnostic,
      names in order or the length of an unnamed timer: "5 min", "1 h 30 min", "90 s"), names sent before the ring's
      on. fake_ha_esphome: two at once, a cancel of another timer, button, media stop, ring out at 1 s.
      LED (reported the same day: dark while ringing, also on the wake word): hassmic started `active_timer`, which has
      a file in `led-resources/` but no entry in `layer_config_common.json` on donut, biscuit or radar, and
      ledcontroller refuses those ("Animation %s not in map"); every other pattern hassmic uses is in the map on all
      three. Stock (PuffinApp `UXEventArbitrator::onAlertStateChange` at 0x25cfc0, AVS alert type at +0xc, state at
      +0x10): ALARM -> `ready-alarm*`, TIMER and REMINDER -> `ready-timer*`; STARTED / FOCUS_ENTERED_FOREGROUND start
      `ready-timer` (looping, layer 3), FOCUS_ENTERED_BACKGROUND starts `ready-timer-short` (one shot, layer 4),
      READY / STOPPED / SNOOZED / COMPLETED / PAST_DUE / ERROR stop `ready-timer`. Now the same: `ready-timer` with
      the ring, `ready-timer-short` when the state leaves IDLE during one.
      Stopping (asked the same day: explicit stop only): the wake word alone no longer stops a ring; it starts a
      pipeline as usual, with the ring in the background (blips paused while state != IDLE: STT must hear the user;
      stock attenuates instead), and `wake_cut_ms` set so the Pryon "stop" right behind it drops that pipeline too.
      Stops: action button, "stop" keyword, media player STOP, ring time, and `core_transcript` (STT_END text alone
      "stop"/"stopp"/"stoppen"/"stoppa"/"halt"/"arrête"/"basta"/"para"/"pare", punctuation dropped) for engines
      without the keyword: the run is cancelled as by the button (HA forgot the timer when it finished, so no intent of its
      can stop the ring; what it would answer to "stop" is not checked). Arbitration unchanged: a ringing Echo still claims with priority 2,
      so the "stop" goes to the one that rings. Open (device): both seen, with and without
      the mic latch (`micsoff-ready-timer` is not in the map either; layer 3 is above `mics-off_on`'s 1), with real
      HA, and an automation on "still on after 50 s".
- [ ] Other stock features without a Home Assistant counterpart yet (survey 2026-10-01): offline alarm clock and
      reminders (HA has timers only).
      Not worth mapping: Matter (`ace_chip_service`), Sidewalk/BLE mesh, calling outside the house (`commsd`), stereo pairs.
- [x] Drop In between Echos (2026-10-08, asked by a user with ten Echos who used Alexa's as an intercom). `dropin.c`:
      calls signed on the arbitration network (new arb type `T_MSG`, `arb_tell`, broadcast like the rest, repeated:
      invite every 250 ms until answered), audio unicast UDP 28932 (`-i`), Opus 16 kHz 20 ms 24 kbit/s inband FEC (stock
      libopus has the encoder on donut, biscuit, radar; 4 symbols more in `stubs/libopus.syms`), XChaCha20-Poly1305
      under a per-call key (X25519 of ephemeral keys, BLAKE2b keyed with `arb_derive("hassmic drop in 1")`). HA: action
      `drop_in(target)` (keyed link only), text sensors "Drop In"/"Drop In with", button "End Drop In", listed while
      the setting `drop_in` is on; `drop_in_answer` auto/ask; page `POST /api/dropin` and a button per member.
      Blueprint `blueprints/automation/drop_in.yaml` (conversation trigger, area or name, the source's action named by
      the same slug as `node_of`; templates checked with jinja2 stand-ins, not yet in a real HA).
      Device (biscuit, PC host as the other end on the LAN, `HASSMIC_ARB_ADDR=192.168.103.255`):
      - Playback stream: `Voip` exists in the mixer (`PLAYBACK_MODE_VOIP`) and switches libasp to "VoIP mode 1" (call
        tuning, NDVC, normalisation off; logcat). Same speech, volume 50, micRaw -48: residual in micAsr -60..-66 dBFS
        on Voip against -45..-58 on TTS (floor -72). Voip follows MainVolume (+9 dB from 30 to 60). micHfp is the bare
        mic (floor -46), not a call Tx stream.
      - Connect 4 ms after the invite (auto). Jitter: micAsr comes in 50 ms blocks, frames leave in bursts of 2-3; a
        3-frame prebuffer started level with the bursts and stayed a frame ahead: 639 of 802 frames came after their
        turn. Now 4 frames from the oldest and an underrun conceals without moving on: 0 of 805 missing, 9 waits at the
        start, longest gap 63-68 ms.
      - Listening mode on for the whole call froze the canceller before it knew the Voip path: residual -46..-62 in
        micAsr; off for the whole call -63..-69. Now only while this side talks (`core_dropin_listen`, 1.5 s hangover).
      - Echo back to the caller, natural speech (OmniVoice, German, 1.5 s pauses), 20 s: first try (80th percentile duck
        before the gain) sent -47..-55 back; the gain undid the duck (+24 dB at micAsr level), the echo estimate let the
        far end's onsets through, the room went out at -48 in its pauses (gain on the floor), and an echo tail 14 dB
        over the room 0.3-1 s after each sentence opened the gate. Now: gain first, then a gate (open only while this
        side talks, -25 dB else, own sounds -40), echo estimate peak-held 2 dB/s, far level held falling 20 dB/s,
        3 frames of double talk to pass: gate never opened by the echo, -64 dBFS mean back against -26 for speech.
        `HASSMIC_DROPIN_TRACE=<file>` writes the gate's inputs per frame.
      fake_ha_dropin: auto and ask, button / stop / "Auflegen" / HA / page, refusals (DND, off, outside the network, no
      such name), a vanished peer ends the call after 5 s.
      Echo to Echo (same day, all three on the build): biscuit -> donut from the page, connected 8 ms after the invite,
      10 s, 509 frames played on each side, 0 missing, longest gap 44-48 ms.
      Open (device): a talker at the called Echo (does speech open the gate fast
      enough, is the ARA with listening mode toggled fine), the wake word in VoIP mode, call LED animations' look,
      the blueprint in a real HA.
