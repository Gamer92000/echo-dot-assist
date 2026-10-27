# Cube Assist

Home Assistant Assist and Sendspin for an **already unlocked and rooted Fire TV Cube 3**. Built from [Echo Dot Assist](https://github.com/Gamer92000/echo-dot-assist), retaining its native ESPHome/Wyoming protocols, signed settings page, Sendspin implementation and arbitration.

This installer does **not** unlock a Cube, flash firmware, modify boot/recovery/system partitions, or install TWRP. Unlocking/rooting must already be complete. This development build accepts `gazelle`, Android API 28, Fire OS `PS7717.5741N`, with working Magisk root. Other firmware needs validation before being enabled.

## What it does

- Captures the processed microphone through Amazon's existing audio front end. Keeps the stock audio HAL, AFE and Fire TV services running.
- Uses the Cube's local Amazon/Pryon Alexa wake-word model. Suppresses stock Alexa while Home Assistant is ready; restores the original setting when the connection or service ends.
- Ducks TV playback at the start of the accepted Assist request. The settings page adds **Extra TV ducking**, 0–24 dB, on top of Android audio focus. The default is 6 dB. Original player gain is restored at the end, with a bounded safety envelope.
- Speaks replies and plays Sendspin audio through the **Cube speaker**. HDMI power detection and automatic output selection are deferred.
- Shows matching state bars on the Cube and along the bottom of the TV picture. Uses the Cube's stock animation files; the overlay does not take focus or remote input. The Lights page explains the bars instead of showing Echo rings.
- Includes EDA arbitration (automatic/keyed network and Kiosk compatibility), settings export/import, browser approval, microphone controls, timers, announcements and Drop In code. Multi-device arbitration and Drop In need testing with another satellite; their inclusion is not a claim of completed end-to-end validation.
- Advertises ESPHome and Sendspin through Android's existing mDNS service.

TV cancellation has been demonstrated with programmes played **by the Cube**. Cancellation of a different TV input is not established. Unlike an isolated Echo conversion, Fire TV apps and Amazon services still have their existing network access.

## Install

On the computer, install Python 3 and Android Platform Tools (`adb`). Enable ADB debugging in Fire TV Developer options and accept this computer's authorization prompt on the TV. The Cube and Home Assistant must be reachable on the same trusted network.

Use the Cube bundle supplied with this project at `dist/cube-assist-gazelle.zip`. It contains our native executable, Android helper and service script, with SHA-256 checksums. It contains no Amazon libraries, firmware or wake-word models. Checksums detect corruption; they are not a publisher signature. Build or obtain the bundle from a source you trust.

Windows:

```powershell
python scripts/cube/setup.py --device 192.0.2.10
```

Linux:

```sh
python3 scripts/cube/setup.py --device 192.0.2.10
```

The wizard checks prerequisites, backs up an existing Cube Assist installation, copies and verifies the bundle, starts it, checks the settings/API/microphone processes, then adds its removable Magisk startup hook. It walks through HA, browser approval, Music Assistant, arbitration and a voice test. Rerunning it preserves settings and creates another backup.

For a separate, bounded 10-minute trial without a startup hook:

```sh
python scripts/cube/setup.py install --device 192.0.2.10 --trial
```

`--preset SETTINGS.conf` applies a settings-page export during setup. `check` performs prerequisite checks without installing. `--yes` skips interactive prompts and prints the setup guide. `--protocol wyoming` retains the old port 10700 for compatibility; use the default **ESPHome** integration for EDA's richer HA controls and encrypted API.

## Home Assistant

1. Settings → Devices & services: configure the discovered **Fire TV Cube 3** ESPHome device. If discovery is unavailable, add ESPHome manually: Cube IP, port **26053**.
2. Select your working Assist pipeline on the new device page. Speech-to-text and text-to-speech must already work in that pipeline.
3. If migrating from the prototype, disable its **Cube Assist Temporary** Wyoming integration. Do not run both microphone bridges together.
4. Say “Alexa, what time is it?” naturally while TV dialogue plays. Check that TV audio ducks while you speak, both status bars appear, the Cube answers, and TV volume recovers.

The ESPHome integration can provision its encryption key through the normal first-connection flow. Do initial pairing on your trusted LAN. Once HA has provisioned a key, generic plaintext API test clients will no longer connect.

## Settings and bars

Open `http://CUBE_IP:28931`, choose **Ask**, then briefly press the Cube's **action button** to approve that browser. Do not hold it: the inherited long-press action resets Cube Assist settings. The browser approval/key protocol is EDA's authenticated signed-request protocol; the page itself uses HTTP.

Voice contains microphone processing and **Extra TV ducking**. Start at 6 dB and increase if programme speech remains distracting. This affects active Fire TV media/game players, not TV programmes from another HDMI input.

**Mic level** sets the automatic speech level sent to HA after a wake, from -35 to -15 dBFS (default -26 dBFS). It does not adjust wake-word sensitivity. The Amazon wake engine receives the processed 16 kHz microphone stream without additional software gain. The current Cube build uses the firmware-bundled en-GB Alexa model and its unchanged thresholds; it does not yet select Alexa's downloaded model or expose an Amazon sensitivity control. It retains Amazon's audio front end, but identical sensitivity to stock Alexa has not been established. In a limited saved-recording comparison, both installed model versions accepted the same wake, and extra input gain did not improve detection; this is not a reliability benchmark.

Lights explains listening, thinking, speaking, finished, microphone mute, timer, error and setup/approval bars. The on-screen bar uses the same animation frames as the physical bar. Stock hardware privacy indication remains under Fire OS control. Custom Echo ring designs, per-LED ring fills and Echo equalizer controls are not advertised as Cube features.

System can restart or reset **Cube Assist only**. A reset archives its state and clears its HA/music/browser pairings; it does not factory-reset Fire TV or forget its Wi-Fi. Manage networking and ADB in Fire TV Settings.

## Music Assistant / Sendspin

Use Music Assistant with its Sendspin provider enabled. The reference interoperability test uses `aiosendspin 10.2.1`, matching Music Assistant 2.10.8. The discovered player is **Fire TV Cube 3**.

Pair with the `SP:…` Sendspin token printed by setup. It is also available through HA's disabled-by-default **Sendspin pairing token** diagnostic entity. Keep it private. If discovery fails, add `CUBE_IP:28928` manually; the websocket path is `/sendspin`. Leave unpaired access disabled.

Run `python scripts/cube/setup.py guide --device CUBE_IP --yes` to repeat the guide and retrieve the token. Start music from Music Assistant, then issue an Assist request during playback to check ducking and recovery. Sendspin protocol/streaming has been tested against the strict reference server on the real Cube; pairing to a particular live MA server still needs its operator's setup.

## Arbitration

The Arbitration settings retain EDA's options: arbitration on/off, EDA or Kiosk mode, automatic or manual timing, arbitration window and score offset. Use the same mode and network key as the other participating satellites. EDA uses UDP **28930**; Kiosk compatibility uses its own protocol/port. Start with automatic timing and offset zero.

The Cube port currently ranks wakes using processed-microphone measurements. It does not claim the Echo mixer's separate AFE energy/beam telemetry. Validate which device wins from several seating positions before tuning offsets. Drop In shares the arbitration network and uses UDP **28932**.

## Stop, restore and remove

```sh
python scripts/cube/setup.py status --device CUBE_IP
python scripts/cube/setup.py stop --device CUBE_IP
python scripts/cube/setup.py start --device CUBE_IP
python scripts/cube/setup.py uninstall --device CUBE_IP
```

Add `--trial` for the separate trial installation. Stopping restores the saved `alexa_wake_word_disabled` value, releases ducking, removes the TV overlay and unsets only LED patterns recorded as owned by Cube Assist. Uninstall also removes its startup hook and **retains** the files/settings in a dated backup.

Installed files: `/data/local/cube-assist`. Startup hook: `/data/adb/service.d/90-cube-assist.sh`. Backups: `/data/local/cube-assist-backups/`. No partition backup is made: this installer changes application files and a recorded reversible setting, not firmware. Preserve the original unlock/recovery backups separately.

If setup reports a failure, it attempts to restore the previous installation and prints the backup path. With ADB root, run `CUBE_ASSIST_HOME=/data/local/cube-assist sh /data/local/cube-assist/service.sh stop` to restore stock wake behaviour. If the application directory is damaged, its `original-wake-setting` is also in the dated backup; restore that exact secure-setting value (`null` means delete the setting).

The supervisor restores Alexa on child-process failure and retries with a bounded failure count. A stale startup lock is recovered on the next boot. No reboot is required for install/uninstall. Full power-cycle behaviour has not yet been tested on the user's Cube.

## Build and test

Use an installed Android SDK, NDK and JDK. Copy `libpryon.so` and `libopus.so` from **your own Cube** into a private local directory for linking; do not redistribute them.

```sh
python scripts/cube/build.py --sdk ANDROID_SDK --java JAVA_HOME --stock PRIVATE_LIB_DIRECTORY
python -m unittest discover -s tests -p test_cube_installer.py
```

`node tests/cube_web.js` checks both Cube and Echo page builders without a device. Installer tests use a fake device. The evidence below comes from the original working development build; this upstream preparation branch has not been installed on hardware. Keep device recordings, pairing keys and firmware out of distributions.

Evidence so far: native build; signed browser login and setting round-trip; ESPHome entity/protocol checks; live microphone streaming; listening/thinking/speaking/idle LED transitions; TV overlay creation/removal; user-confirmed TV and Cube bars; paired strict-reference Sendspin streaming; mDNS discovery from another computer; seven installer validation/rollback tests; helper-crash recovery with stock Alexa restored in 5.24 seconds and Amazon’s audio server unchanged. HA has now provisioned encryption on the installed port, and two natural wake-word requests completed listening, thinking, reply playback through the Cube speaker, and volume recovery in the device logs. Music Assistant 2.10.8 completed long-term Sendspin pairing. The user confirmed the installed port answers correctly, ducks and restores TV audio, shows both bars, and plays Music Assistant tracks through the Cube speaker. A subsequent ADB reboot verified automatic microphone startup, encrypted HA voice subscription and paired Music Assistant reconnection, with all 11 checked settings/pairing files byte-for-byte unchanged and arbitration still off. A post-reboot indicator probe observed both the TV window and physical LED pattern, then clean removal at idle. Audible post-reboot voice/music confirmation, a cold power-cycle and multi-device arbitration testing remain outstanding. This is a development release.

## Attribution

Forked from Gamer92000 / Julian Imhof's Echo Dot Assist, commit `3062695c586b03371ee63a0c63376f6c214721ef`, MIT licensed. `LICENSE` and all third-party notices remain. `UPSTREAM-README.md` describes the original Echo project; its unlocking, flashing and system-install instructions do not apply to this Cube installer. Legacy Echo source/scripts are retained for reference only. This is an independent development port, not an official EDA or Amazon release.
