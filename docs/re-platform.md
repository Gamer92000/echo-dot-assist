# Platform interfaces without PuffinApp

Static analysis of `firmware/rootfs` (NS65741). Nothing here has been run on a device.
**UNVERIFIED** marks anything not proven from the binaries or configs.
Paths are relative to `firmware/rootfs/` unless absolute.

First thing to run on the device — it enumerates every IPC service, property and current value:

```sh
lipc-probe -a -v      # LIPC (dbus) publishers: com.doppler.audiod, ledcontroller, ...
aipc-probe -a -v      # AIPC services: LedController, uxeventsvc, ...
service list          # Binder services (expect "LedController")
```

## 1. LED ring

### Confirmed

- `ledcontroller` (`system/etc/init/start_animation_noart.rc:11`, user `ledcontroller`, started at `post-fs-data`)
  owns the LED driver at `/sys/bus/i2c/devices/0-003f/{frame,led_current,boot_animation}` and the mute-ring PWM
  (`/sys/class/pwm/pwmchip0/pwm1`). It runs independently of PuffinApp.
- It exposes three front doors:
  1. **Binder** service `LedController` (`amazon::ILedController`, `libled_controller.so`). Client: `system/bin/ledctrl`.
  2. **AIPC** service (`LedCtrl/AipcService`). Client: `system/bin/ledctrl_aipc`, which connects as `com.util.ledctrl`.
  3. **LIPC** events it subscribes to, listed in `system/etc/ledlipcconfig.json`:
     `com.amazon.puffin` → `set_animation`, `unset_animation`, `set_brightness`, `set_brightness_table`;
     `com.doppler.lasp` → `beam_index`; plus `com.doppler.OTAUpdate`, `com.crumpet.oobed`, `com.amazon.factoryreset`, `com.util.ledctrl`.
- Amazon's own boot scripts use the Binder CLI, as a non-puffin user: `system/bin/start_animation.sh` is just `ledctrl -s $1`,
  `stop_animation.sh` is `ledctrl -u $1`. So `ledctrl` needs no PuffinApp and no sender identity.
- `ledctrl` options (from its usage text): `-s PATTERN` set, `-u PATTERN` unset, `-c` clear all, `-g` list active,
  `-b 0..100` brightness, `-l` get brightness, `-a on|off` auto-brightness.
- Pattern names are the file names in `system/etc/led-resources/` without `.animation` (247 files).
  Format is plain text: `<duration>:<12 × RGB hex nibbles>` per frame, optional `loop` marker — custom animations are easy,
  but the directory is on `/system`.
- Layering/priorities come from `system/etc/led-resources/layer_config_common.json`.
- Sender check: the LIPC path only accepts events from the sources in `ledlipcconfig.json` (it subscribes per source:
  `Unable to subscribe to event %s from sender %s`). LIPC lets any process open any source name, so
  `lipc-send-event com.amazon.puffin ...` is not authenticated — **UNVERIFIED** on device.
- **`ledcontroller` is what starts Alexa.** At `bin/ledcontroller` 0x1c012–0x1c026: when the animation named
  `anim_start_phase2` ends, it calls `property_set("com.amazon.puffin.PUFFIN_START", "1")`. That property triggers
  `start puffin`, `start puffinmrmd` (`init.mt8516.rc:228`), `start dacd` (`system/etc/init/ahe_noart.rc:16`) and
  `start smarthomed` (`system/etc/init/init.smarthomed.rc:36`). No other binary or rc file sets it.

### Suggested mapping for the satellite

| State | Pattern |
|---|---|
| listening | `ca-active-start` (UX config uses it; `ca-active-start_step-0..39` point at a direction) or `alexa_wake-up` |
| thinking | `active-thinking` |
| talking | `active-talking` |
| back to idle | `ca-active-end`, then unset |
| error | `anim_start_error_short` or `error` |
| mic muted | `mics-off_start` → `mics-off_on`; leaving: `mics-off_end` |
| volume | `volume_step-01..30` |

### Commands to try

```sh
ledctrl -g
ledctrl -s active-thinking ; sleep 3 ; ledctrl -u active-thinking
ledctrl -s ca-active-start ; sleep 2 ; ledctrl -u ca-active-start ; ledctrl -s ca-active-end
ledctrl -c
# LIPC alternative (parameter type assumed to be one string) — UNVERIFIED
lipc-send-event com.amazon.puffin set_animation -s active-talking
```

### `uxeventd` (animation + earcon bundles)

- `system/etc/init/init.uxeventd.rc`, user `uxevent`, AIPC service `com.amazon.visualindicator.uxeventsvc`.
  It plays the 155 named events in `system/etc/uxconfig.json` (`listening`, `thinking`, `talking`, `speech-end`,
  `mics-off-start`, `volume-step-NN`, `bluetooth-connected`, ...): LED animation through `ledcontroller` over AIPC,
  earcons through its own Reggae media player into `mixer`.
- Request names seen in the binary: `startEvent`, `stopEvent`, `customevent`, `CLEAR`, `CLEAR_LAST`; JSON fields
  `eventName`, `name`, `subName`, `earconName`, `caName`, `raName`, `preemptive`. It also has
  `requestWakeWordBlock` / `requestWakeWordUnblock` and `requestAudioFocus`, which call back into `com.amazon.puffin`.
- Exact request payload is **UNVERIFIED**. Capture it on a stock device:
  `aipc-wait-event -m` while saying "Alexa", then replay with `aipc-send-request -n com.amazon.visualindicator.uxeventsvc ...`.
- Recommendation: use `ledctrl` for LEDs and play earcons ourselves through `MixerOpenPlay(..., "Earcon")`.
  `uxeventd` expects PuffinApp for audio focus and is not needed.

### Verified on device (2026-09-21, unregistered device in setup mode)

- `ledctrl -s active-thinking` shows and loops until `ledctrl -u active-thinking`. Works as root, no PuffinApp needed.
- `ledctrl -g` is unreliable: it keeps listing a pattern after `-u` stopped it (log: `LedCtrl/Leds: Stop <pattern>`) until `-c`,
  and it never listed the `setup-mode` spinner. Trust the `LedCtrl/Leds: Start|Stop` log lines instead.
- `uxeventd` starts the orange `setup-mode` spinner on an unregistered device (`uxconfig.json`: `state-boot-up-oobe`,
  `oobe-setup-mode-on`, groupId 5). `ledctrl -c` does **not** clear it; `ledctrl -u setup-mode` does. While it runs it hides
  other patterns (`mics-off_on` was not visible), and `ledcontroller` first looks for `<pattern>-setup-mode`.
- `ledctrl -l` reports `Current brightness = 0` although patterns are visible (auto brightness; lux sensor "data not valid yet").

### Open questions

- Do two active patterns layer as `layer_config_common.json` says?
- Does `ledcontroller` still follow `com.doppler.lasp beam_index` (published by `mixer`) when PuffinApp is down? If yes, the
  listening animation points at the speaker for free.

## 2. Volume

### Confirmed

- Volume lives in `mixer` (`libaudioCtrl.so`), published on LIPC as **`com.doppler.audiod`**. `mixer` also registers
  `com.doppler.mixerpb`, `com.doppler.mixerrec`.
- CLI: `system/bin/audio_manager_set_prop` / `audio_manager_get_prop` → `liborpheusaudiomanager.so` → LIPC
  `com.doppler.audiod` (`LipcSetIntProperty`, `LipcGetIntProperty`, `LipcSetStringProperty`). No PuffinApp involved.
- Properties and ranges, from the tools' help text:

| Property | Values |
|---|---|
| `MainVolume`, `AlarmVolume`, `TTSVolume`, `TelephonyVolume`, `RingVolume`, `NotificationVolume`, `AccessibilityVolume`, `SystemVolume`, `NavigationVolume` | 0–100 |
| `MaxVolumePercentage` | max volume limit |
| `Mute` | 1 = mute, 0 = unmute |
| `MuteVolType <type> <0/1>`, `ToggleMute <type>` | per volume type |
| `AllowMic` | 1/0 — "Set Mic Recording allow state" |
| `SilenceMic` | 1/0 — silences mic audio while mic is active |
| `LPMState` (get) | low-power-mode state |
| `MixerInfo` | dumps mixer state to `/var/lib/audioserver/MixerState.txt` |
| `InvalidateRecordStreams`, `DumpPlayback`, `CaptureRecordStart <stream>` / `CaptureRecordStop` | debug |

- Change events on the same source: `MainVolumeChanged`, `DebouncedMainVolumeChanged`, `TTSVolumeChanged`,
  `AlarmVolumeChanged`, `MuteChanged`, `muteStateChanged`.
- Volumes persist in `/data/mixer_meta/audiodVolumes` (`init.mt8516.rc:154`).

### Commands to try

```sh
audio_manager_get_prop MainVolume
audio_manager_set_prop MainVolume 40
audio_manager_set_prop Mute 1 ; audio_manager_set_prop Mute 0
audio_manager_get_prop AllowMic
lipc-get-prop -i com.doppler.audiod MainVolume          # same thing via LIPC — property name UNVERIFIED
lipc-set-prop -i -- com.doppler.audiod MainVolume 40
lipc-wait-event -m com.doppler.audiod '*'               # watch volume/mute events
```

### Open questions

- Exact argument order of `audio_manager_set_prop` (help text suggests `<prop> <value>`). **UNVERIFIED**
- Does TTS played on the `TTS` stream follow `TTSVolume`, `MainVolume`, or both?
- Note for the mixer work: `AllowMic` may default to 0 until PuffinApp sets it, which would give silent capture. Check it
  first if `micAsr` returns zeros. `CaptureRecordStart micAsr` is a built-in capture dump worth trying.

## 3. Buttons and mic mute

### Confirmed

- `system/etc/acebutton_config.json`: one GPIO keypad at **`/dev/input/event3`**, `EV_KEY`:

| Button | Key code | Values |
|---|---|---|
| volume up | 115 (`KEY_VOLUMEUP`) | 1 down, 0 up, 2 repeat (300 ms, then 150 ms) |
| volume down | 114 (`KEY_VOLUMEDOWN`) | same |
| action ("uber") | 138 (`KEY_HELP`) | same |
| mic mute | 113 (`KEY_MUTE`) | 1 down, 0 up |

  Timing: 5 s hold on action = OOBE/setup mode, 21 s = factory reset (`oobeHold`, `factoryResetHold`).
- `acebuttond` (`init.mt8516.rc:385`, started at line 71) reads the device; `aceinputmanager`
  (`system/etc/init/init.inputmgrd.rc`) republishes through `ace_eventmgr` (`aceEventMgr_publish`, source
  `com.ace.inputmanager`). `mixer` also knows a `com.doppler.buttond` LIPC source and asks it for the mic-mute state
  (`AudioCtrl:getMicMuteState: get mic state from button failed`).
- Simplest subscription for our daemon: open `/dev/input/event3` and read `struct input_event`. evdev allows several
  readers. Whether `acebuttond` takes an exclusive grab (`EVIOCGRAB`) is **UNVERIFIED** — `getevent -l /dev/input/event3`
  on the device answers it in one second.
- If `acebuttond` keeps running it keeps its own behaviour: 5 s action hold starts `oobed` (setup mode, soft AP),
  21 s hold factory-resets. Keep that as a rescue path, or stop `acebuttond` and handle everything ourselves.
- **Mic mute is a hardware circuit with a kernel driver**, `gpio-privacy` (strings in the kernel in `firmware/images/boot.img`:
  `gpio-privacy-state-gpios`, `privacy_enable_gpio`, `privacy-button`, `gpio-privacy: no state-gpios`). Sysfs:
  `/sys/devices/platform/gpio-privacy/state` and `/enable`, owned by `ace_button_mgr` (`init.mt8516.rc:62-63`).
  The driver reports the latch state and emits the key event; software can read `state`.
  That the latch cuts the microphones electrically, and that software can only *enter* privacy mode through `enable`
  but not leave it, matches Amazon's published `gpio-privacy` driver — **UNVERIFIED** for this board.
- The red mute ring is software: `ledcontroller` drives PWM1 using the brightness table in
  `system/etc/led_mute_ring_config.json` (copied to `/data/ledcontroller/`). Whether it lights by itself from the
  privacy state or waits for a `mics-off_*` pattern is **UNVERIFIED**.

### Commands to try

```sh
getevent -l /dev/input/event3                 # press each button
cat /sys/devices/platform/gpio-privacy/state  # before and after pressing mute
lipc-probe -v com.doppler.buttond
lipc-wait-event -m com.doppler.buttond '*'
ace mw buttonmanager get_button_number
```

### Open questions

- With mute active, does `micAsr` deliver zeros, stop delivering, or does `mixer` close the stream?
- Does the red ring come on with PuffinApp stopped?

## 4. What depends on PuffinApp

### Confirmed

- Nothing in any rc file restarts, watches or reboots on PuffinApp. `puffin` and `puffinmrmd` are plain `disabled`
  services without `critical` or `onrestart` (`init.project.rc:9`, `init.mt8516.rc:448`). The only `critical` services
  are AOSP's (`ueventd`, `servicemanager`...).
- `healthcheck` (`init.mt8516.rc:442`) is a boot-time oneshot gated by `/proc/idme/dev_flags`; it checks the A/B slot, not Alexa.
- `perfrecoveryd` (`system/etc/perfrecoveryd/perfrecoveryd_config.json`): reboots only on critical memory pressure after
  ≥ 7 days uptime and 15 min idle; kills named processes over a memory limit (`perfrecoveryd_memory_guardrail.json`).
  It does not care whether PuffinApp exists. Our daemon is not in its list, so it has no limit.
- Power: `system/etc/pwrsvc_config.json` has `"autosleep_forbidden": true`, and low-power mode is only switched on when
  PuffinApp sets `com.amazon.puffin.LPM_ELIGIBLE=1` (`system/etc/init/pwrsvcd.rc:24`). Without PuffinApp the device never autosleeps.
- PuffinApp start chain: boot animation `anim_start_phase2` ends → `ledcontroller` sets
  `com.amazon.puffin.PUFFIN_START=1` → init starts `puffin`, `puffinmrmd`, `dacd`, `smarthomed` (see section 1).
- OOBE: `oobed` only runs when someone sets `com.amazon.puffin.OOBED_START` to `on_boot`, `on_button_press` or
  `on_network_disconnect` (`system/etc/init/init.oobe.puffin.rc`). The setter is PuffinApp (`com.amazon.puffin.OOBED_START`,
  `OOBE_SETUP` strings in it) and, for the 5 s hold, the button path. With PuffinApp stopped, a deregistered device
  should sit idle rather than enter setup mode — **UNVERIFIED**.
- **Firewall — this matters for the satellite port.** `def_firewall` runs `system/bin/firewall.sh start default` at
  `sys.boot_completed=1`: policy `INPUT DROP`, `OUTPUT` open. Inbound TCP allowed on `wlan0`: 4070, 4071,
  **16384–32767**, 40317, 55442–55443, 55445, 6543, 5540, 5541. Inbound UDP 5353 (mDNS) and 1900 are allowed.
  Wyoming's 10700 and ESPHome's 6053 are **blocked**. Two fixes that need no edit of Amazon files:
  - listen inside 16384–32767 (for example 16053) and advertise that port over mDNS, or
  - create `/system/bin/debug_firewall.sh`; `firewall.sh` sources it as root at the end of the default setup if it exists
    (`DEBUG_RULES_HOOK`), e.g. `iptables -A INPUT -i wlan0 -p tcp --dport 6053 -j ACCEPT`.
- Wi-Fi: standard `wpa_supplicant` with `/data/misc/wifi/wpa_supplicant.conf` and control socket `wpa_wlan0`
  (`init.connectivity.rc:121`). Amazon's HAL (`libacehal_wifi.so`) drives it with `ADD_NETWORK` / `SAVE_CONFIG`, so
  profiles persist in that file; `netmgrd` + `wifisvc` keep the link up and run DHCP, independent of PuffinApp.
  A device already set up with the Alexa app keeps its Wi-Fi after PuffinApp is disabled.

### Commands to try

```sh
getprop | grep -iE "puffin|oobe|audio_mixer|init.svc"
stop puffin ; stop puffinmrmd ; stop dacd ; stop smarthomed
sleep 600 ; uptime ; getprop | grep init.svc | grep -v running      # nothing should have rebooted or restarted
iptables -S INPUT
# Wi-Fi without the app
wpa_cli -i wlan0 -p /data/misc/wifi/sockets status
wpa_cli -i wlan0 -p /data/misc/wifi/sockets add_network            # then set_network N ssid/psk, enable_network, save_config
ace mw net_cli                                                     # lists Amazon's own network commands
```

### Open questions

- Persistent way to keep PuffinApp off. Candidates, least invasive first: (a) our own init service that runs
  `stop puffin; stop puffinmrmd; stop dacd; stop smarthomed` after `PUFFIN_START=1` and starts our daemon on the same trigger;
  (b) comment out `start puffin`/`start puffinmrmd` in `init.mt8516.rc:230-231`. Option (a) leaves Amazon's files untouched
  but lets PuffinApp run for a moment at boot.
- Does `netmgrd` raise a captive-portal/"no internet" state that triggers anything when Amazon endpoints are blocked?
- Does deregistering wipe `wpa_supplicant.conf`? Safer to stay registered and block the cloud than to deregister — **UNVERIFIED**.

## 5. OTA

### Confirmed

| Service | Defined | Starts | Role |
|---|---|---|---|
| `otad` | `init.mt8516.rc:431` | automatically (`class main`) | Amazon OTA client; decides, downloads, launches `update_engine` via `ctl.start`, reboots when idle (`idleOTAReboot`, `acePWR_reboot`) |
| `ace_otad` | `system/etc/init/init.ace.otad.rc:14` | automatically (`class core`) | ACE middleware OTA for components/companions |
| `update_engine` | `system/etc/init/on_demand_update_engine.rc:3` | on demand, `disabled` | applies A/B payload |
| `rollback`, `system_rollback` | `init.mt8516.rc:437` | on demand | `update_verifier` slot rollback |
| `GoogleOtaAgent`, `rbfota` | `init.mt8167.rc` | MediaTek leftovers; binaries likely absent | — |

- State and tuning: `persist.otad.target_sw_version`, `persist.otad.update_count`, `/data/ota_package/`,
  `system/etc/ota-update.conf` (`TIME_TO_FORCE_UPDATE=86400`: a pending update is forced after one day).
- `otad` also watches LED events (`com.doppler.OTAUpdate set_animation`) and announces through `uxeventd`.
- `dacd` is not OTA; it is started by `PUFFIN_START` and belongs to the Alexa side.

### Disabling, least invasive first

1. Runtime, no file changes: `stop otad; stop ace_otad; stop update_engine`. Neither service is `oneshot`, so init will
   not restart a stopped service until reboot. Put it in our boot script.
2. Persistent: add `disabled` to both service blocks (`init.mt8516.rc:431`, `system/etc/init/init.ace.otad.rc:14`).
   `update_engine` is already `disabled` and only `otad` starts it.
3. Belt and braces: `chmod 000 /system/bin/update_engine`, and block the update hosts on the router/DNS.
   A successful OTA replaces `/system` and `boot`, which removes root and everything we installed.

### Commands to try

```sh
getprop | grep -iE "otad|rota|update_engine"
ls -la /data/ota_package /cache
stop otad ; stop ace_otad
logcat -d | grep -iE "otad|update_engine" | tail -40
```

### Open questions

- Update host names (not present as plain strings in `otad`; they come from the Arcus/remote config). Capture with
  `tcpdump`/DNS log on the router while `otad` is running.
- Does stopping `ace_otad` upset `neo-service-coordinator` or `ace_eventmgr`? **UNVERIFIED**
