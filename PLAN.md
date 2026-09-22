# Echo Dot 3 (donut) → Home Assistant voice satellite

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
- [~] Mic after a hassmic (re)start: the mixer delivers nothing on the new `micAsr` client for ~15 s (`InCapture-GetReadBuff:
      retcode=110` every 1.5 s from 21:46:22 to :36 after the 21:46:17 open), then data flows. Same picture when `mixcap` opened
      right after hassmic was stopped and gave up after 17 s. So each push update means ~20 s without wake word; harmless,
      cause unknown (HAL standby?)
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
      adb works over Wi-Fi too: stock init sets `service.adb.tcp.port 5555` and stock `firewall.sh` opens the port (same key
      authentication as over USB), so `adb connect <ip>:5555` from a PC on the Echo's VLAN is the root shell. Verified 2026-09-22;
      USB is only needed for TWRP.
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
- [x] Disable OTA: `otad`, `ace_otad`, `update_engine` stopped at every boot by `alexa-off.sh`/`lockdown.sh`, egress firewall + no-internet
      VLAN block the hosts. Not removed from the image, by design: the uninstaller must be able to give the stock device back,
      and three layers (stopped, owner-match DROP, no-internet VLAN) are enough
- [x] Wi-Fi provisioning without the Alexa app: `scripts/wifi-join.sh`, profile persists in `wpa_supplicant.conf`, rejoins after reboot
- [x] ESPHome native API (`src/hassmic/proto_esphome.c`, default; Wyoming stays as `-P wyoming`): voice pipeline with TTS over the API
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
- [x] Revert procedure tested 2026-09-21: `install-system.sh --uninstall` leaves no trace on `/system` (`/sepolicy` md5 back to the pre-hassmic
      value, stock Alexa + `uxeventd` + `otad` run again, no egress lock: only the VLAN protects then); reinstall brings everything back, and
      `/data/local/hassmic/state` (Sendspin identity, pairing record, settings) survives both. `alexa-on.sh` (no reboot) still untested
