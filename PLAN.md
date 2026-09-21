# Echo Dot 3 (donut) → Home Assistant voice satellite

Goal: keep Amazon's audio front end (`mixer` + `libasp`: AEC, beamforming, mic calibration, speaker path),
replace the Alexa client (`PuffinApp`) with a small daemon that talks to Home Assistant.

Firmware analysed: `donut_puffin` NS65741 / Fire OS 6574.1. Findings: [docs/FINDINGS.md](docs/FINDINGS.md),
[docs/re-pryon.md](docs/re-pryon.md), [docs/re-platform.md](docs/re-platform.md).

Legend: `[x]` done, `[~]` partly done (note says what is missing), `[ ]` open. **(device)** = needs the physical Echo.
"Done" in phases 0–2 means done on the PC. Device unlocked + rooted 2026-09-21 (kamakiri-donut, boot-root), still in OOBE, no Wi-Fi.

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
- [~] `mixer` single-client logic and mic gating. Known: log string proves eviction of the previous `micAsr` client;
      mic mute comes from LIPC `com.doppler.buttond/muteState`. Missing: default of `AllowMic` without PuffinApp → check on device
- [ ] `mixer` playback stream types: ducking and volume group of `TTS` vs `Earcon` vs `Music` → easier to observe on device
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
- [x] Wake earcon: generated blip on the `Earcon` stream (no stock sound files needed; `-E` disables)
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
      Wi-Fi Direct group `p2p-p2p0-0` (left by `oobed`) removed with `wpa_cli -i p2p0 p2p_group_remove`; now in `alexa-off.sh`.
      Do not stop the supplicant: the same `wpa_supplicant` process runs `wlan0`
- [x] **(router)** Echo is on the user's no-internet IoT VLAN — covers the boot window before `lockdown.sh` runs
- [ ] **(device)** Verify: `tcpdump`/router log over one full reboot shows zero non-LAN packets
- [ ] **(device)** Persistent: rules in `/system/bin/debug_firewall.sh` (stock `firewall.sh` runs it after its own flush) plus init service for `lockdown.sh watch`
- [ ] Clock: no NTP once locked down. Wyoming needs none; point `sntp` at the router later if wanted

## Phase 3 — First contact **(device)**

Run in this order. Each step says what it proves.

- [x] `scripts/probe.sh` — libs identical to analysed firmware (2026-09-21) — records stock state; flags if device libs differ from analysed firmware
- [x] Confirm SELinux state: `getenforce` = Enforcing, but adb shell runs in permissive `u:r:su:s0`; children keep that context
- [ ] `logcat` while saying "Alexa" on stock — baseline for mixer log lines
- [x] `scripts/deploy.sh`, then `adb shell /data/local/hassmic/alexa-off.sh` — `mixer` stays up
- [ ] Wait 10 min: no reboot, no service restarts
- [x] `scripts/capture-test.sh 10` — 16 kHz mono confirmed by data rate (32 kB/s); `AllowMic` = 1 without PuffinApp.
      **AIPC refuses uid 0** → all mixer clients run through `runas puffin aipc,audio,...` (`src/tools/runas.c`; no `su` on device).
      `MixerGetRate/NumCh/SampleSizeBits` return -1 on record handles. Still open: listen to the WAV
- [x] `mixplay` test tone: audible at `MainVolume` 50 (scale 0–100; 7 was too quiet to notice). `audio_manager_set_prop MainVolume N` works as root.
      Volume buttons are dead once the Alexa stack is stopped → `hassmic` must handle them (do not pass `-V`)
- [~] AEC test (pink noise + tones at `MainVolume` 50, room quiet): residual converges from −41 dB to −65 dB (noise floor) in ~10 s. Good.
      Open: reproducible low-frequency thumps (80–120 Hz, ~100 ms, up to clipping) in `micAsr` at playback start, ~3–4 s in, and
      ~1.5 s after playback ends. A `Silent` keep-alive stream does not change them → not HAL/amp standby. Need to know if the
      thump is audible from the speaker (acoustic) or only in the capture (AEC/reference glitch). Retest with speech-like TTS audio.
      User listened (2026-09-21): **no audible thump** → capture-side only (AEC/reference path or our ring-buffer read). Check `micRaw` next
- [x] `pryon_test` on device: canned file Accept (type=2) at 1.39 s; live `mixcap | pryon_test` 6/6 spoken "Alexa" accepted, no near-misses
- [ ] `alexa-on.sh` restores Alexa without reboot

## Phase 4 — End-to-end PoC **(device)**

- [ ] `scripts/poc-host.sh <ip>` with HA-side wake word (isolates audio path from our daemon)
- [x] `run.sh` runs detached on device (log `/data/local/hassmic/hassmic.log`), listening on 16700, PC can connect. mDNS not seen from
      the PC. Added in HA by IP `192.168.100.147`, port 16700.
      **2026-09-21: end-to-end works** — "Alexa" → HA pipeline → TTS reply on the Echo, user verdict "works perfectly"
- [ ] LED ring states look right: listening / thinking / talking / idle / error / muted / volume
- [ ] Buttons: `getevent -l /dev/input/event3` works beside `acebuttond`; volume keys not handled twice
- [ ] Barge-in works acoustically (depends on AEC quality during TTS)
- [ ] Wake-word accuracy at distance and with music playing
- [ ] Wake word "Echo": firmware ships only `ALEXA` (+`STOP`) in `words.shrunk.txt`. Stock gets other keywords from DAVS (cloud) into
      `/data/.../speech/wakeword_models/davs/resources/`. Options: pull an ECHO model set from another source, or non-Pryon engine
      (microWakeWord on device / openWakeWord on HA via `-w remote`)
- [ ] Latency wake → STT start; TTS playback glitch-free

## Phase 5 — Make it permanent **(device)**

- [x] init service for `hassmic`; keep `puffin`, `puffinmrmd`, `dacd`, `smarthomed` off across reboots.
      Written, **not installed**: `scripts/system/{hassmic.rc,boot.sh,sepolicy.rules}` + `scripts/install-system.sh <lan-cidr>` (via TWRP;
      `/` is dm-0 and cannot be remounted rw live). Services run with `seclabel u:r:su:s0`; needs `allow init su process transition`
      in `/sepolicy` (that file is the active policy; boot-root's vendor path does not exist here). Patched copy was test-loaded
      live with `magiskpolicy --live`: kernel accepts it. Kill switch: delete `/data/local/hassmic/hassmic.conf`.
      **Installed 2026-09-21 by the user; verified after reboot:** `hassmic` + `hassmic_fw` running from init in `u:r:su:s0`, hassmic as
      `puffin`, Alexa services + `oobed` stopped, `hassmic_out` first in OUTPUT, HA reconnected on its own ~60 s after power-up.
      `magiskpolicy` aborts inside TWRP (even with `/system` bind-mounted) → installer patches the policy under the running OS
      and only copies it in TWRP, with md5 checks on base and transfer
- [x] Setup voice prompts + orange spinner at boot (unregistered device): played by `uxeventd`. `boot.sh` now stops `uxeventd` and
      `oobed_*` at `on boot` and again after PuffinApp is stopped. Reinstalled + rebooted 2026-09-21: user confirms no audio, no LED at boot; `uxeventd` stays stopped, device stable
- [x] Volume buttons felt dead: step was 3 %, `volume_step-NN` patterns were never unset (they loop black forever and pile up),
      start value hardcoded, key repeat counted. Now 10 % steps, start value read via `audio_manager_get_prop`, previous pattern
      unset, cleared after 2.5 s. Installed; user verdict: fine. Installer made idempotent for re-installs (patches from
      `/sepolicy.pre-hassmic`, skips the policy write when the installed md5 already matches)
- [~] Disable OTA: `otad`, `ace_otad`, `update_engine` stopped at every boot by `alexa-off.sh`/`lockdown.sh`, egress firewall + no-internet
      VLAN block the hosts. Not removed from the image
- [x] Wi-Fi provisioning without the Alexa app: `scripts/wifi-join.sh`, profile persists in `wpa_supplicant.conf`, rejoins after reboot
- [~] ESPHome native API (`src/hassmic/proto_esphome.c`, default; Wyoming stays as `-P wyoming`): voice pipeline with TTS over the API
      connection (16 kHz), announcements + `play_media` as WAV over HTTP (HA transcodes to the advertised 48 kHz mono), timers
      (alarm until button / wake word / 60 s), volume both ways, wake-word config, generated mDNS file (`hassmic -S`).
      Port **26053** (stock firewall admits inbound TCP 16384–32767 only). `tests/fake_ha_esphome.py` against `aioesphomeapi`:
      20/20. With real HA (2026-09-21): discovery, voice pipeline, settings entities, mute confirmed by the user.
      Settings entities (HA applies them to the mic stream, values travel in each request, saved in `/data/local/hassmic/state/settings`):
      noise suppression level, auto gain, mic volume multiplier, wake sound; mute switch.
      Mute: button = hardware latch, reported by the `gpio-privacy` input device (`/dev/input/event1`, not the keypad); software can
      set the latch (`enable` <- 1) but not clear it (write 0 rejected while set, second 1 does not toggle; DT has one output, one
      input) → HA switch = soft mute, shows latch OR soft, button unmute clears both.
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
- [~] Wi-Fi drops: Amazon's `wifisvc` runs HTTP connectivity tests against AWS hosts (`AceNetSvc_HttpTest ... unreachable`); behind the
      egress lock they fail and it rebuilds the link (seen ~100 s after boot, link down 193 s; explains earlier stray
      "client disconnected/connected" pairs). Verified live: with `wifisvc` stopped, `wpa_supplicant` + `dhcpcd` keep the link and
      survive a forced disconnect/reconnect. `boot.sh` `netwatch`: stop `wifisvc` once there is an address, start it again only after
      60 s without one. hassmic: 5 s send timeout + TCP keepalive on the client socket, so a dead link no longer blocks `core_lock`
      and the single client slot. Installed 2026-09-21: `netwatch: link up, wifisvc stopped` at boot, link stable through the
      user's tests. Missing: long-run observation (hours)
- [~] Reply quality: SPEAKER flag dropped (feature flags 61). HA then renders TTS in the media player's announcement format (48 kHz
      mono WAV) and sends the URL (RUN_START with streaming TTS, TTS_END otherwise); hassmic fetches it like an announcement, starts
      at INTENT_PROGRESS `tts_start_streaming=1` when offered, and reports VoiceAssistantAnnounceFinished. Before: 16 kHz over the
      API connection. Test 22/22. Installed; with real HA 2026-09-21: reply arrives as `tts_proxy/….wav`, 48 kHz, playback starts before
      TTS_START (streaming), follow-up question re-opens the mic (continue conversation). Missing: user's verdict on the sound
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
      The pairing token is also an ESPHome text sensor ("Sendspin pairing token", diagnostic, disabled by default) so it can be copied
      from Home Assistant.
      Not implemented: PIN pairing (CPace: SHA-512 + Elligator2) → refused with `pair/abort method_not_supported`;
      metadata/artwork roles (no display). Missing: install, listening test against another synced player (trim with MA's static delay)
- [x] mDNS: init's `avahi-daemon` runs in SELinux domain `avahi-daemon`, which is denied read on `/data/misc/avahi/services`
      (so nothing was ever published, also not for Wyoming). `magiskpolicy` cannot parse a rule for a type with a hyphen →
      `boot.sh`/`run.sh` stop the init service and start avahi themselves in the `su` domain. Verified: answers queries from the PC.
      Host name is `linux.local` (system host name is `localhost`); HA connects by IP and follows the MAC in the TXT record
- [ ] Revert procedure tested
