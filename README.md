# echo-dot-assist

> [!CAUTION]
> **Vibecoded.** Code, scripts, reverse-engineering notes, plan and this README were written by an AI (Claude) in
> conversation with the author, not by hand. Tried on exactly one device; nobody has reviewed or audited it.
>
> That includes the parts that can hurt: bootloader unlock, system partition and SELinux writes, the firewall that keeps
> the Echo away from Amazon and firmware updates, and signed push updates. Any of them can brick your Echo, leave it
> online when you think it is not, or open it up on your network.
>
> **Read what you run. No warranty, no support, your risk.**

Turns an **Amazon Echo Dot 3rd gen (2018)** into a **Home Assistant voice satellite** that never talks to Amazon.

Amazon's microphone processing (echo cancellation, beamforming, per-device mic calibration) and wake word engine stay,
so it hears you across the room and over its own music like before. Only the Alexa client is replaced, by a small daemon
called `hassmic` that speaks to Home Assistant as an ESPHome device (default) or as a Wyoming satellite.

**Only the 2018 Echo Dot 3** (`donut`, model D9N29T), not the 2019–2020 refresh with the clock (`crumpet`, C78MP8).
The unlock needs the case opened and wires on test pads, and can brick the device.

## Features

|                                               | Stock Alexa             | hassmic, ESPHome (default)                   | hassmic, Wyoming         |
|-----------------------------------------------|-------------------------|----------------------------------------------|--------------------------|
| Voice assistant                               | Alexa (Amazon cloud)    | Home Assistant Assist                        | Home Assistant Assist    |
| Amazon's mic processing (AEC, beamforming)    | ✅                      | ✅                                           | ✅                       |
| Wake word on the device                       | ✅                      | ✅ "Alexa"; "Echo", "Computer", … [optional](#3-optional-another-wake-word) | ✅ same      |
| Wake word in Home Assistant instead           | ❌                      | ✅ (`-w remote`)                             | ✅ (`-w remote`)         |
| Interrupt a reply ("Alexa" / "Alexa, stop")   | ✅                      | ✅                                           | ✅                       |
| Several Echos hear it, only the nearest answers | ✅ (Amazon cloud)      | ✅ between these Echos, on the LAN           | ❌                       |
| Timers                                        | ✅                      | ✅                                           | ❌                       |
| Announcements, follow-up questions            | ✅                      | ✅                                           | ❌                       |
| Media player entity (TTS, `play_media`)       | ❌                      | ✅                                           | ❌                       |
| Multiroom music                               | Amazon speaker groups   | Music Assistant (Sendspin)                   | Music Assistant (Sendspin) |
| Bluetooth speaker                             | SBC                     | SBC, AAC, aptX, aptX HD; pairing from HA     | reconnects already paired devices only |
| Bluetooth proxy for Home Assistant            | ❌                      | ✅ scanning, connections, pairing            | ❌                       |
| Buttons, LED ring, hardware mute              | ✅                      | ✅                                           | ✅                       |
| Mute state and audio settings in HA           | ❌                      | ✅                                           | ❌                       |
| Do not disturb                                | ✅ (Alexa app)          | ✅ switch in HA                              | ❌                       |
| Equalizer (bass, mid, treble)                 | ✅ (Alexa app)          | ✅ sliders in HA                             | ❌                       |
| Encrypted link to Home Assistant              | –                       | ✅ key set by Home Assistant                 | ❌ plain TCP             |
| Talks to Amazon                               | always                  | never (firewalled)                           | never (firewalled)       |
| Updates                                       | automatic, from Amazon  | signed, pushed from your PC                  | signed, pushed from your PC |

Details:

- **Voice**: found automatically by Home Assistant's ESPHome integration, no YAML, no ESPHome add-on. Replies start while
  text-to-speech is still being generated. "Stop" works only right behind the wake word ("Alexa, stop"), because
  Amazon's models only hear it in the two seconds after it; out of silence, "Alexa, stop the music" goes to Home
  Assistant as a normal command. While a timer rings or something plays, the wake word is
  accepted more readily, as Amazon's models are tuned to do.
- **Several Echos**: like stock, only the Echo that heard the wake word best answers (among Echos listening for the same
  word: one on "Echo" and one on "Alexa" each answer their own); the others stay silent (no
  sound, no light). The Echos settle it among themselves on the local network in 0.2 s, by how clearly the word stood
  out of the room's noise; an Echo that is in a conversation or ringing keeps the next wake word. With only one Echo
  there is no delay. They find each other by themselves ("Join arbitration network", on by default); the shared key
  travels through your Home Assistant, so nobody else on the network can join or silence them. For that, tick "Allow
  the device to perform Home Assistant actions" in each Echo's ESPHome options (Home Assistant shows a repair until
  then); give every Echo its own `NAME`. Other satellites (ESP32 and so on) are not part of it; Home Assistant itself then lets the first one
  that reports the wake word answer, and the Echo that is second now just goes quiet instead of flashing an error.
- **Buttons**: action = talk without the wake word / pause and resume music / stop an alarm; volume in 10 % steps;
  mic-off is the hardware mute it always was (red ring, Alexa's own sounds). The LED ring shows listening, thinking,
  speaking, errors and mute. Silent and dark at boot.
- **Music**: one source at a time, the newest wins. A phone starting over Bluetooth pauses Music Assistant (the whole
  group), Music Assistant starting on the Echo pauses the phone. The voice assistant ducks both.
- **Bluetooth**: the proxy works like an ESPHome `bluetooth_proxy` with `active: true`, up to 3 connections, "Just Works"
  pairing only. While a phone plays, the proxy stops scanning: the radio cannot do both without the music stuttering.
- **Settings in Home Assistant**: noise suppression level, auto gain, mic volume multiplier, mute switch, "Do not disturb"
  switch (drops announcements, purple pulse when switched on), "Wake sound" switch (covers all local sounds),
  "Bluetooth pairing" switch, "Bluetooth announcements" switch and their language, "Join arbitration network" switch, equalizer (bass, mid, treble, −6 to +6 dB, Amazon's own,
  applied to everything the Echo plays). Diagnostics, off by default: SoC temperature, CPU usage.
- **No cloud**: Alexa client, updater and telemetry are stopped at every boot; a firewall drops everything that is not
  going to a local address. Only hassmic itself may go further, to fetch replies and music from where Home Assistant or
  Music Assistant point it. See [Security](#security).
- **Reversible**: delete one file for stock behaviour, run the uninstaller, or reflash stock from recovery.

## Requirements

- The right Echo Dot and a way to reach its **hidden USB port** (data lines are on test pads inside). The
  [XDA thread][xda] links the guides. This is the hard part.
- A **Linux PC** with `adb`, `fastboot`, `python3`, `make`, `unzip`, `debugfs` (e2fsprogs), `sqlite3`, ~5 GB free disk.
- **Home Assistant** with a working Assist pipeline (speech-to-text, conversation agent, text-to-speech). Test it with
  the app first. Optional: Music Assistant (tested with 2.10.4).
- **Wi-Fi** with WPA2 passphrase (no captive portal, no enterprise login) that reaches Home Assistant.
- These files in the repository root (not included: proprietary or third-party):

| File | Source | sha256 |
|---|---|---|
| `update-kindle-donut_puffin-NS65741_user_8138_0013222529668.bin` | stock Fire OS 6574.1 for `donut_puffin`, from FTVDB | `ac22b78cf94c2ebacfa90447b770b803d9f72f35858d5218975d97fea95a0245` |
| `kamakiri-donut-v1.0.0.zip` | bootloader unlock + TWRP, [XDA thread][xda] | `4d2bb52eaf661f6616aa4268584d44cf9155c1328c34bfcba01a6558f942b738` |
| `boot-root.zip` | root adb + permissive `su` SELinux domain, [XDA thread][xda] | `de49cc88b27a8e77cf97cf0156bee50e4ddc0e116c41aaede06b494e38397be0` |
| `toolchain/android-ndk-r21e/` | unpack <https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip> into `toolchain/` | – |

Use exactly this firmware version: everything was worked out against its binaries, and `scripts/probe.sh` checks for it.

## Install

Plan an evening. Steps 1–2 are the risky ones and are not this project's work: read the [XDA thread][xda] in full first.

> **Never let the Echo go online with Amazon on the way.** It would update itself, and an update can close the hole the
> unlock uses. Do not set it up with the Alexa app, except in the guarded way described under
> [Another wake word](#3-optional-another-wake-word).

### 1. Unlock, flash stock firmware, root

```sh
sudo cp scripts/51-echo-unlock.rules /etc/udev/rules.d/ && sudo udevadm control --reload   # USB without sudo; keeps ModemManager off the bootrom
unzip kamakiri-donut-v1.0.0.zip -d kamakiri && cd kamakiri
./bootrom-step.sh          # start it, THEN plug the Echo in while holding the action (dot) button
./fastboot-step.sh         # when the ring shows a rotating rainbow; ends in TWRP (white ring)
cd ..
```

Flash the firmware into both A/B slots, then root:

```sh
F=update-kindle-donut_puffin-NS65741_user_8138_0013222529668.bin
adb shell twrp wipe cache; adb shell twrp wipe data
adb push $F /sdcard/update.zip
adb shell twrp install /sdcard/update.zip      # "Flashing A/B zip to inactive slot: B" (or A); it becomes the active slot
adb reboot recovery        # wait for the white ring; TWRP now runs from the slot just flashed
adb shell twrp install /sdcard/update.zip      # must name the OTHER slot this time
adb reboot recovery
adb push boot-root.zip /sdcard/ && adb shell twrp install /sdcard/boot-root.zip   # patches both slots
adb reboot
```

TWRP switches the active slot itself after an install (checked 2026-09-25: `bcbtool get_active` said `b` right after
flashing B). Do not switch it back by hand, or the second install lands in the same slot again and the other one keeps
Amazon's newer firmware. `kamakiri`'s `bootrom-step.sh` asks for Enter after the handshake: run it in a terminal, not in
the background. With a second Echo on adb (Wi-Fi), point every command and script at the new one:
`export ANDROID_SERIAL=<serial from adb devices>`.

Result: stock Fire OS, unregistered, no Wi-Fi, orange ring, and `adb shell` is root. If adb does not show up after a
reboot, replug the power. Back to TWRP: `adb reboot recovery`, or hold Volume Up while plugging in.

### 2. Unpack the firmware and build

The tools link against Amazon's libraries, so the firmware is unpacked on the PC:

```sh
unzip update-kindle-*.bin payload.bin -d firmware/
python3 tools/payload_dump.py firmware/payload.bin firmware/images
debugfs -R "rdump / firmware/rootfs" firmware/images/system.img
unzip boot-root.zip -d boot-root          # the installer uses boot-root/patch/magiskpolicy32
make                                      # ARM binaries into build/
scripts/probe.sh                          # must not list any DIFFERENT library
```

If `probe.sh` reports a different library, the Echo runs another firmware: stop and redo step 1.

### 3. Optional: another wake word

Want "Echo" or "Computer" instead of "Alexa"? Do it now: it is the only step that needs the Echo online with Amazon,
and easiest before the lockdown. Can also be done later.

<details>
<summary>"Echo", "Computer", "Amazon", "Ziggy": fetch the model from Amazon once</summary>

The firmware ships only "Alexa". The others are Pryon model sets that a registered Echo downloads from Amazon (DAVS) on
demand, per language; the stock engine loads them as they are. Fetching one needs a registered device's access token,
so the Echo goes online with Amazon once: now, before the lockdown, or on an installed Echo with
`MODE=stock-online` (last paragraph).

**The danger is a firmware update**, which can cost root and the unlock. `otad` and `ace_otad` run as user `ace_otad` and
start `update_engine` on demand, so blocking that one user is enough:

```sh
adb push scripts/device/lockdown.sh /data/local/tmp/
adb shell "sh /data/local/tmp/lockdown.sh ota-only watch > /data/local/tmp/otaguard.log 2>&1 &"
adb shell iptables -S hassmic_out        # must show: -m owner --uid-owner <n> -j DROP
```

The guard is gone after a reboot, the Wi-Fi profile is not: restart the guard after every reboot before anything else,
or block the Echo's internet at the router while it boots, until the token is on the PC.

1. Set the Echo up with the Alexa app (Wi-Fi with internet, Amazon account). The app ends on "updating" and the ring
   spins a while: that is the update check getting no answer, nothing is installed.
2. Pull the registration (the token is valid for an hour, renewed while online) and fetch the model. Language is a
   parameter; no need to change anything in the app.
   ```sh
   adb pull /data/ace/kvstorage/map.db device-logs/map.db
   tools/davs-fetch.py device-logs/map.db echo de-DE          # -> device-logs/models/echo-de-DE/unpacked/
   ```
   Keys known to work: `alexa echo computer amazon ziggy`. `device-logs/` is git-ignored: models are Amazon's, `map.db`
   is your account's device credential.
3. Optional check on the PC: copy `unpacked/` below `firmware/rootfs/`, then
   `tools/qrun.sh build/pryon_test -m /<path below rootfs>/pryon.manifest testdata/echo_espeak_de.raw` must print
   `RESULT ... keyword=ECHO type=2` (de-DE loads under qemu, the larger en-US set did not).
4. Remove the device from your Amazon account in the app. If the Wi-Fi the app added is not the satellite's network,
   remove it on the Echo (`wpa_cli -i wlan0 -p /data/misc/wifi/sockets list_networks`, `remove_network <id>`,
   `save_config`): with two profiles it roamed and kept the wrong lease. Optionally clear the dead registration: on the
   PC `sqlite3 map.db "delete from deviceData; vacuum;"`, push back (owner `ace_maplite:ace_maplite`, mode 660), reboot.
   Then continue with install step 4.
5. Install the model:
   ```sh
   adb shell mkdir -p /data/local/hassmic/models
   adb push device-logs/models/echo-de-DE/unpacked /data/local/hassmic/models/echo-de
   adb shell chmod -R a+rX /data/local/hassmic/models
   ```
   Restart hassmic (or reboot). Home Assistant then offers every installed model in the Echo's wake word select
   ("Alexa" plus "Echo" here, named after the folder: `<keyword>-<language>`); pick one, it switches at once and stays.
   `-m <manifest>` in `ARGS` only sets which one to start with until you pick one in Home Assistant.

**On an installed Echo**: add `MODE=stock-online` to `/data/local/hassmic/hassmic.conf` and reboot. hassmic stays off,
stock Alexa runs with internet, and the update guard is applied at every boot (this is the variant used end to end
here). Allow the Echo internet at the router, do steps 1–5, then remove the `MODE` line. No push port in this mode: use
adb.

</details>

### 4. Lock down, then join Wi-Fi

In this order, so the lock is on before the Echo sees your network:

```sh
scripts/deploy.sh                                        # pushes binaries and scripts to /data/local/hassmic
adb shell sh /data/local/hassmic/lockdown.sh             # firewall: local addresses only; stops Alexa, updater, telemetry
mkdir -p secrets && printf '%s\n%s\n' 'My SSID' 'my passphrase' > secrets/wifi.conf      # git-ignored
scripts/wifi-join.sh                                     # refuses to run without the lock; prints the IP address
```

- **Do not reboot before step 6**: the Wi-Fi profile survives a reboot, this lock does not. Blocking the Echo's
  internet at the router is a good idea anyway.
- Give the Echo a fixed address (DHCP reservation).
- "Local" = private ranges, link-local, multicast, so Home Assistant may sit in another local subnet or VLAN.

### 5. Try it

```sh
adb shell sh /data/local/hassmic/run.sh          # foreground; Ctrl-C stops it
```

1. Home Assistant shows a discovered ESPHome device "Echo Dot" under Settings → Devices & services. Otherwise add it by
   hand: ESPHome, the Echo's IP, port 26053, encryption key empty. Home Assistant sets a key by itself.
2. Pick the Assist pipeline in the device's settings.
3. Say "Alexa", ask something.

Home Assistant must reach TCP 26053 on the Echo; the Echo must reach Home Assistant's port 8123.

### 6. Make it permanent

```sh
scripts/install-system.sh "Kitchen Echo"         # the name Home Assistant will show
```

Patches the SELinux policy, reboots into TWRP, writes `/system/hassmic/`, an init file and the policy into the active slot
(old policy kept as `/sepolicy.pre-hassmic`), reboots. Refuses to write if anything is not as expected. From now on the
Echo boots silent and dark, locks itself down, and is a satellite about a minute after power-up. If Home Assistant still
has the device from step 5 under another name, remove it and add the new one.

**Back up `secrets/update.key`**, created by the installer: it signs your updates. The USB wires can come off now.

### 7. Optional: Music Assistant

Music Assistant finds the Echo by itself (Sendspin player, TCP 28928). For the pairing token it asks for, enable the
diagnostic entity "Sendspin pairing token" in Home Assistant, or read it from `/data/local/hassmic/boot.log`.

### 8. Optional: Bluetooth speaker

Turn on the "Bluetooth pairing" switch in Home Assistant, then pick the Echo in the phone's Bluetooth settings within two
minutes. Paired devices reconnect by themselves whenever you choose the Echo; unknown ones are refused while the switch
is off.

Like stock Alexa the Echo announces connections: a chime and "Connected to <name>" / "Disconnected from <name>". The
words are spoken by Home Assistant, which only does that after you tick "Allow the device to perform Home Assistant
actions" in the Echo's ESPHome options (Settings → Devices & services → ESPHome → the Echo → Configure). The
"Bluetooth announcements" switch turns chime and words off. The words are English until you pick another language in
"Bluetooth announcement language" (Deutsch, Français, Español, Italiano, Português, Nederlands, Svenska, Dansk, Norsk,
Suomi, Polski): set it to the language of the Echo's assistant in Home Assistant, whose voice speaks them. Home
Assistant does not tell the Echo which language its assistant uses, so this cannot follow it by itself.

### Updating

```sh
git pull
scripts/ota-push.sh <echo-ip>        # remembers the address
```

Builds, signs, pushes over Wi-Fi (TCP 28929). The Echo installs only what verifies against your key, restarts hassmic,
and falls back to the installed copy by itself if the new one does not stay up. What changed: [CHANGELOG.md](CHANGELOG.md).

## Configuration

One file on the Echo, `/data/local/hassmic/hassmic.conf`, read at boot (edit over adb, reboot):

```sh
NAME="Kitchen Echo"         # device name in Home Assistant
PROTO=esphome               # or wyoming (port 16700)
ARGS=""                     # extra options, below
#MODE=stock-online          # temporary: stock Alexa online without updates, see install step 3
```

| `ARGS` option | Effect |
|---|---|
| `-m <pryon.manifest>` | wake word model to start with, until one is picked in Home Assistant |
| `-w remote` | wake word detection in Home Assistant (openWakeWord) instead of on the Echo |
| `-E` | no sound on wake |
| `-L` | leave the LED ring alone |
| `-V` | leave the volume buttons alone |
| `-z 0` | no Sendspin player |
| `-a 0` | no arbitration with other Echos (UDP 28930) |
| `-p <port>` | another port (the firewall only admits inbound TCP 16384–32767) |

adb also works over Wi-Fi: `adb connect <echo-ip>:5555`. The cable is only needed for TWRP.

## Troubleshooting

Log: `adb shell tail -30 /data/local/hassmic/boot.log`.

**Wake word and button do nothing.** Most likely no connection to Home Assistant; the Echo does not signal that (known
gap). In the log, `wake: ALEXA type=2` means it heard you, `client connected` / `voice assistant: subscribed` means Home
Assistant is there. Nothing after the last `client disconnected`: check the network (`adb shell ifconfig wlan0`; can Home
Assistant reach that address?). Keep exactly one Wi-Fi profile on the Echo.

**No sound from replies or music.** The Echo fetches every reply, announcement and `play_media` from the URL Home
Assistant or Music Assistant gives it. Home Assistant builds that from its internal URL (Settings → System → Network),
or its LAN IP when none is set. The Echo must resolve the name (DNS from DHCP; `.local` via mDNS works) and route to the
address; on a network without internet that means a URL inside your network. The log names what failed
(`net: cannot ...`). If not even button sounds play, check the volume.

**"Invalid encryption key" in Home Assistant** (Echo reset, or something else set a key first):
`adb shell rm /data/local/hassmic/state/api_key`, restart hassmic (or reboot), delete the device in Home Assistant, add it
again.

Open issues and measurements: [PLAN.md](PLAN.md).

## Uninstall

- **Temporarily**: `adb shell rm /data/local/hassmic/hassmic.conf`, reboot. The Echo is a stock, unregistered Echo (which
  updates itself if it gets internet).
- **Properly**: `scripts/install-system.sh --uninstall` removes the files from the system partition and restores the policy.
- **Completely**: reflash the stock firmware from TWRP as in install step 1.

## Security

- **ESPHome link**: encrypted like an ESPHome device with `api: encryption` but no key in its YAML. Home Assistant
  generates the key when you add the Echo, sets it over an encrypted connection, and clears it when you delete the
  device. **Until then anyone on the network can connect**, or set a key first (then see
  [Troubleshooting](#troubleshooting)). The key lives in `/data/local/hassmic/state/api_key`.
- **Wyoming link**: unencrypted and unauthenticated, like every Wyoming satellite.
- **Egress**: Amazon's daemons may only reach local addresses (plus DNS to the servers DHCP hands out); `otad` and
  `ace_otad` never get out. hassmic itself may reach any address. Put the Echo on a network without internet as a second
  layer.
- **Inbound**: TCP 16384–32767 only (26053 ESPHome, 16700 Wyoming, 28928 Sendspin, 28929 updates), UDP 16384–32767
  (28930 arbitration between Echos).
- **Arbitration between Echos**: an Echo takes the network key only from Home Assistant, over its encrypted API link,
  as a call of its own action `esphome.<node>_arbitration_key`; a member hands it over by asking Home Assistant to run
  that action, which needs "Allow the device to perform Home Assistant actions". So only devices you adopted into Home
  Assistant and allowed to act take part; the key travels encrypted to the receiving Echo, so it is not readable in
  Home Assistant's traces or logbook. Rounds are authenticated with the key and cannot be replayed. The keys are in
  `state/arb_key` and `state/arbitration`.
- **Updates**: only bundles signed with your `secrets/update.key` are installed.
- **Bluetooth**: keys in `state/ble_bonds` (proxy) and `state/bt_keys` (speaker), both under `/data/local/hassmic/`.

## Development

Architecture, repository layout, building for the PC, tests and contribution notes: [DEVELOPMENT.md](DEVELOPMENT.md).

## Licence

[MIT](LICENSE), for everything written here. The files in `src/third_party/` keep their own licences, stated in each file:
monocypher (BSD-2-Clause OR CC0-1.0), `dr_flac.h` (public domain or MIT-0), `minimp3.h` (CC0-1.0), `freeaptx.c`/`.h`
(LGPL-2.1-or-later; hassmic links it statically, and everything needed to rebuild and relink it is in this repository).

Nothing of Amazon's is in this repository and nothing of it is covered by this licence: firmware, libraries and wake-word
models come from your own device and stay Amazon's. Not affiliated with or endorsed by Amazon, Home Assistant or
Music Assistant; "Alexa" and "Echo" are Amazon's trademarks.

[xda]: https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/
