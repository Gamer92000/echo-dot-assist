# echo-dot-assist

> [!CAUTION]
> **Vibecoded.** Code, scripts, reverse-engineering notes, plan and this README were written by an AI (Claude) in
> conversation with the author, not by hand. Tried on one Echo of each supported model; nobody has reviewed or audited it.
>
> That includes the parts that can hurt: bootloader unlock, system partition and SELinux writes, the firewall that keeps
> the Echo away from Amazon and firmware updates, and signed push updates. Any of them can brick your Echo, leave it
> online when you think it is not, or open it up on your network.
>
> **Read what you run. No warranty, no support, your risk.**

Turns an **Amazon Echo** into a **Home Assistant voice satellite** that never talks to Amazon. Works on the Echo Dot 3
(2018), the Echo Dot 2 and the Echo 2; see [Supported Echos](#supported-echos).

Amazon's microphone processing (echo cancellation, beamforming, per-device mic calibration) and wake word engine stay,
so it hears you across the room and over its own music like before. Only the Alexa client is replaced, by a small daemon
called `hassmic` that speaks to Home Assistant as an ESPHome device (default) or as a Wyoming satellite.

## Supported Echos

|    | Echo                         | Model  | Codename                               | Way in                                           |
|----|------------------------------|--------|----------------------------------------|--------------------------------------------------|
| ✅ | Echo Dot 3rd gen (2018)      | D9N29T | [`donut`](devices/donut/README.md)     | open the case, wires on test pads                |
| ✅ | Echo Dot 2nd gen (2016)      | RS03QR | [`biscuit`](devices/biscuit/README.md) | micro-USB, no soldering                          |
| ✅ | Echo 2nd gen (2017)          | XC56PY | [`radar`](devices/radar/README.md)     | open the case, solder USB to the amplifier board |
| ❌ | Echo Dot 3rd gen (2019–2020) | C78MP8 | `crumpet`                              | the Dot 3 unlock does not work on it             |
| ❌ | Echo Dot 3rd gen with clock  | 36EBT3 | `doebrite`                             | thought to be `crumpet` hardware                 |

✅ works, tried on one Echo of that model · ❌ not supported yet

Each model needs exactly the firmware its page names (`donut`: Fire OS 6574.1 only). The unlock can brick the device.
Models not in the table: what is known and how to add one is in [`devices/`](devices/README.md).

## Features

|                                               | Stock Alexa             | hassmic, ESPHome (default)                   | hassmic, Wyoming         |
|-----------------------------------------------|-------------------------|----------------------------------------------|--------------------------|
| Voice assistant                               | Alexa (Amazon cloud)    | Home Assistant Assist                        | Home Assistant Assist    |
| Amazon's mic processing (AEC, beamforming)    | ✅                      | ✅                                           | ✅                       |
| Wake word on the device                       | ✅                      | ✅ "Alexa"; "Echo", "Computer", … with `scripts/artifacts.sh` ([details](devices/donut/README.md#3-optional-another-wake-word)) | ✅ same      |
| Wake word in Home Assistant instead           | ❌                      | ✅ (`-w remote`)                             | ✅ (`-w remote`)         |
| Interrupt a reply ("Alexa" / "Alexa, stop")   | ✅                      | ✅                                           | ✅                       |
| Several Echos hear it, only the nearest answers | ✅ (Amazon cloud)      | ✅ between these Echos, on the LAN           | ❌                       |
| Timers                                        | ✅                      | ✅                                           | ❌                       |
| Announcements, follow-up questions            | ✅                      | ✅                                           | ❌                       |
| Media player entity (TTS, `play_media`)       | ❌                      | ✅                                           | ❌                       |
| Multiroom music                               | Amazon speaker groups   | Music Assistant (Sendspin)                   | Music Assistant (Sendspin) |
| Bluetooth speaker                             | SBC                     | SBC, AAC, aptX, aptX HD; pairing from HA     | reconnects already paired devices only |
| Play on a Bluetooth speaker                   | ✅ (Alexa app)          | ✅ found and paired from HA, SBC ([details](#bluetooth-speaker-output)) | keeps playing on one already set up |
| Bluetooth proxy for Home Assistant            | ❌                      | ✅ scanning, connections, pairing            | ❌                       |
| Buttons, LED ring, hardware mute              | ✅                      | ✅                                           | ✅                       |
| Mute state and audio settings in HA           | ❌                      | ✅                                           | ❌                       |
| Do not disturb                                | ✅ (Alexa app)          | ✅ switch in HA                              | ❌                       |
| Equalizer (bass, mid, treble)                 | ✅ (Alexa app)          | ✅ sliders in HA                             | ❌                       |
| Light ring follows the room's light           | ✅                      | ✅ same, or a fixed level from HA; illuminance sensor | ❌ (stock's automatic only) |
| Sound detection (smoke alarm, glass, dog, …)  | ✅ Alexa Guard, checked in Amazon's cloud | optional, off by default: on the Echo only, less reliable ([details](#sound-detection)) | ❌ |
| Whisper detection                             | ✅ answers in a whisper | sensor for the conversation agent's prompt ([details](#whisper)) | ❌ |
| Motion sensor                                 | ❌                      | **experimental**, off by default: from the Wi-Fi signal ([details](#wifi-motion)) | ❌ |
| Encrypted link to Home Assistant              | –                       | ✅ key set by Home Assistant                 | ❌ plain TCP             |
| Talks to Amazon                               | always                  | never (firewalled)                           | never (firewalled)       |
| Updates                                       | automatic, from Amazon  | signed: pushed from your PC, or online from Home Assistant (off by default) | signed, pushed from your PC |

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
  there is no delay. They find each other by themselves ("Wake word arbitration" on the [settings page](#settings-page), on by default); the shared key
  travels through your Home Assistant, so nobody else on the network can join or silence them. Nothing to set up: each
  Echo shows a key on its diagnostic entity "Arbitration handoff" (leave it enabled), and the Echos read each other's
  through Home Assistant. That works while the Echo's entities in Home Assistant carry the name of its `NAME` (so not if
  you renamed the device there). Otherwise either tick "Allow the device to perform Home Assistant actions" in each
  Echo's ESPHome options, or pair two Echos with the buttons: hold Volume up and Volume down together for 2 s on the new
  Echo, then on one already in (a tap sounds; the Bluetooth "connected" sound when it worked, "disconnected" when it
  did not within 2 min). Give every Echo its own `NAME`. Other satellites (ESP32 and so on) are not part of it; Home Assistant itself then lets the first one
  that reports the wake word answer, and the Echo that is second now just goes quiet instead of flashing an error.
- **Buttons**: action = talk without the wake word / pause and resume music / stop an alarm / cancel a request while
  Home Assistant is still listening or thinking (as on a Voice PE; the wake word then cancels it too and listens
  again; ESPHome only); volume in 10 % steps; both volume buttons held for 2 s = pair for arbitration (see above);
  mic-off is the hardware mute it always was (red ring, Alexa's own sounds). The LED ring shows listening, thinking,
  speaking, errors and mute. Silent and dark at boot.
- **Music**: one source at a time, the newest wins. A phone starting over Bluetooth pauses Music Assistant (the whole
  group), Music Assistant starting on the Echo pauses the phone. The voice assistant ducks both.
- **Playing on a Bluetooth speaker**<a id="bluetooth-speaker-output"></a>: everything the Echo plays (replies, timers,
  its sounds, music) can come out of a Bluetooth speaker instead of its own, as with stock. Switch on "Play on a
  Bluetooth speaker" on the [settings page](#settings-page) (its entities then appear in Home Assistant), put the speaker in pairing
  mode near the Echo and switch on "Bluetooth speaker search": within a minute the Echo pairs with the strongest one it
  hears (speakers, headphones, and PCs that offer to play audio) and plays on it. You cannot pick one from a list: Home
  Assistant reads an ESPHome select's choices only when it connects, so keep only the speaker you want in pairing mode.
  "Play on Bluetooth speaker" switches between it and the Echo; the Echo reconnects by itself when the speaker comes
  back, and takes it when the speaker calls the Echo on switching on. "Bluetooth speaker" shows its name and state.
  - **Volume**: the speaker has its own. While the Echo plays on it, the volume buttons, Home Assistant and Music
    Assistant set the speaker's volume, and the light ring shows it; the Echo starts from the speaker's own volume and
    the speaker's buttons move it too. Back on the Echo, its own volume returns. With a speaker that supports
    Bluetooth absolute volume (most do) the Echo sends the sound at full level and the speaker turns it down, which
    sounds best; with one that does not, the Echo turns it down itself, as stock does.
  - **Music Assistant**: a Bluetooth speaker plays late, by its buffer. "Bluetooth speaker delay" (default 250 ms) is
    what the Echo allows for, so that it stays in time with other players; set it by ear for your speaker.
  - The Echo still listens for the wake word while the sound comes from the speaker. How well it hears through loud
    music played elsewhere in the room has not been measured yet.
  - SBC only (every speaker has it), one speaker at a time. ESPHome only for setting it up.
- **Bluetooth**: the proxy works like an ESPHome `bluetooth_proxy` with `active: true`, up to 3 connections, "Just Works"
  pairing only. While a phone plays, the proxy stops scanning: the radio cannot do both without the music stuttering.
- **In Home Assistant, always**: mute switch, "Do not disturb" switch (drops announcements, purple pulse when switched
  on), "Wake sound" switch (covers all local sounds), equalizer (bass, mid, treble, −6 to +6 dB, Amazon's own, applied
  to everything the Echo plays), "LED auto brightness" switch and "LED brightness" slider (the ring dims with the room as
  on a stock Echo, Amazon's own logic, on by default; setting a level holds it there and switches the automatic off),
  "Illuminance" (the Echo's light sensor in lux, as Amazon reads it, for automations), the firmware update entity, and
  the "Sendspin pairing token" (diagnostic, disabled by default).
- **Features**, switched on the [settings page](#settings-page): while one is on, its entities are in Home Assistant;
  off, they are gone (Home Assistant reconnects for a moment when one is switched). Wake word arbitration (on by
  default: "Arbitration peers"), sound detection ("Sound"), whisper detection ("Last request whispered"), Wi-Fi motion
  (motion sensor, sensitivity), Bluetooth audio from phones ("Bluetooth pairing" switch, blue chaser on the ring while it
  is on), playing on a Bluetooth speaker ("Bluetooth speaker search", "Play on Bluetooth speaker", "Bluetooth speaker"
  state and "Bluetooth speaker delay", see [Playing on a Bluetooth speaker](#bluetooth-speaker-output)).
- **On the settings page only**: "Mic level" (how loud speech reaches the voice assistant, -35 to -15 dBFS, default
  -26; the Echo adjusts its gain to it), "Noise reduction" (off by default; low, medium, high: RNNoise on what the voice
  assistant gets takes the background down by up to 6, 9 or 12 dB), Bluetooth announcements and their language, online
  updates channel, "Music Assistant without pairing" (off by default: only Sendspin servers paired with the token may
  play), debug access (adb over Wi-Fi), SoC temperature and CPU usage.
- **Sound detection** (optional, off by default)<a id="sound-detection"></a>: "Sound detection" on the settings page runs Amazon's
  own Alexa Guard model on the Echo, beside the wake word, and the "Sound" event entity reports what it heard:
  `smoke_or_co_alarm`, `glass_break`, `dog_bark`, `baby_cry`, `snoring`, `cough`, `water`, `beeping_appliance`. Use it
  in automations ("When Sound fires with smoke_or_co_alarm"). Please read before relying on it:
  - **Less reliable than on a stock Echo.** Amazon checks every hit in its cloud before it tells anyone; that check
    cannot be had without Amazon, so here every hit of the model counts. In tests it also took a barking dog, pouring
    water and a toilet flush for breaking glass, and a cough for a beeping appliance. Treat an event as a hint, not as
    an alarm system, and never as a replacement for a smoke or CO detector.
  - **Slow**: the model listens in windows of 10 s, so an event comes up to 10 s after the sound, and once per window
    while the sound goes on.
  - **Coarser than stock**: the model gives smoke alarms, smoke sirens and CO alarms the same score, and coughs the same
    as running water, so they are one event each (`smoke_or_co_alarm`; `cough`). "Human presence" is left out: it fires
    on any talk, TV or knock.
  - Nothing is reported while the Echo is muted, or for a window in which the Echo itself played something (a reply, a
    timer, music, its sounds): those are what it would hear.
  - **Private**: it all happens on the Echo; nothing leaves it except the event to Home Assistant (a stock Echo uploads
    the recordings, and near misses for training). Costs about 13 % of one CPU core while on (Echo Dot 2).
  - It uses the model in the Echo's firmware. Amazon's newest can be installed in its place with `scripts/artifacts.sh`
    ("Other artifacts"; so far it scored the same on every test).
  - ESPHome only, not with Wyoming. Background: [docs/re-aed.md](docs/re-aed.md).
- **Whisper detection** (optional)<a id="whisper"></a>: a stock Echo answers a whispered request in a whisper. Here the
  binary sensor "Last request whispered" says whether the last request was whispered, for the conversation agent to
  answer the same way. It uses Amazon's own whisper detector on the Echo, with a model that only Amazon hands out:
  install it from a PC with `scripts/artifacts.sh` ("Other artifacts" → "Whisper detection"). It needs the Echo
  registered to an Amazon account for a few minutes (the script walks you through it and undoes it), as for other
  wake words. Over Wi-Fi, first open debug access on the Echo's [settings page](#settings-page), then run
  `scripts/artifacts.sh <echo-ip>`. The model stays through updates; without it there is no sensor.
  - The sensor is set when you stop speaking, before speech to text has finished, so the agent's prompt template can
    read it. For example, in the LLM conversation agent's instructions (the entity id has your Echo's name in it):

    ```jinja
    {% if is_state('binary_sensor.echo_dot_last_request_whispered', 'on') %}
    The user whispered. Answer in a whisper: mark the whole answer the way your text-to-speech engine whispers.
    {% endif %}
    ```

    Replace the second line with the markup your text-to-speech engine understands; Piper has none.
  - In tests (Echo Dot 2, German commands from 1–2 m) whispered commands scored 984–999 out of 1000, spoken ones
    0–18, quietly spoken ones too. Saying the wake word normally and whispering the rest is fine. Sounds without words (breathing, rustling) can score high, but only what the
    pipeline took for a command is scored.
  - It all happens on the Echo, during your request only. ESPHome only, not with Wyoming. Background:
    [docs/re-whisper.md](docs/re-whisper.md).
- **Wi-Fi motion** (**experimental**, off by default)<a id="wifi-motion"></a>: "Wi-Fi motion (experimental)" on the
  settings page turns the Echo into a motion sensor without any extra hardware. Someone walking between the Echo and your Wi-Fi router
  changes how strongly the Echo receives the router, and "Wi-Fi motion (experimental)" (a motion binary sensor) goes on
  while that happens and off 30 s after it stops, like a PIR sensor. "Wi-Fi motion sensitivity (experimental)", 1 to 10
  (default 5), sets how much change counts. It is a first version, tried in one flat for a few minutes and one night,
  where it mostly did what it should; please read:
  - **Motion, not presence.** Someone sitting still does not show; an empty room and a quiet one look the same.
  - **Only between the Echo and the router.** It sees best what crosses the path between them (also in the next room,
    if the router is there); someone moving elsewhere in the room may not show at all.
  - **Expect false alarms** from other Wi-Fi devices, doors and people in the router's room; how often has not been
    counted yet. Try the sensitivity before you rely on it. On the Echo Dot 2 and Echo 2
    also when the router switches between its faster speeds: their Wi-Fi does not say at which speed a frame came,
    and a router sends each speed at its own strength (the Echo Dot 3 allows for that).
  - **Through a small kernel module.** The Wi-Fi drivers do not report what this needs (the Echo Dot 3's only for the
    last frame from any device nearby), so hassmic brings a kernel module of its own that reads the level of every
    frame from your router in the driver. It is only loaded once you switch Wi-Fi motion on (within 10 s), and then
    stays loaded until the Echo restarts. Running on an Echo Dot 3, an Echo Dot 2 and an Echo 2.
  - ESPHome only. It does not use the microphones; muting the Echo does not stop it.
- **No cloud**: Alexa client, updater and telemetry are stopped at every boot; a firewall drops everything that is not
  going to a local address. Only hassmic itself may go further, to fetch replies and music from where Home Assistant or
  Music Assistant point it. See [Security](#security).
- **Reversible**: delete one file for stock behaviour, run the uninstaller, or reflash stock from recovery.

## Requirements

- A [supported Echo](#supported-echos) and a USB way into it: a plain cable on the Echo Dot 2, wires soldered or held
  on test pads on the Echo Dot 3 and Echo 2. The model's page says what exactly.
- A **Linux PC** with `adb`, `fastboot`, `python3`, `make`, `unzip`, `debugfs` (e2fsprogs), `sqlite3`, ~5 GB free disk.
- **Home Assistant** with a working Assist pipeline (speech-to-text, conversation agent, text-to-speech). Test it with
  the app first. Optional: Music Assistant (tested with 2.10.4).
- **Wi-Fi** with WPA2 passphrase (no captive portal, no enterprise login) that reaches Home Assistant.

## Install

Each model's page, linked in [Supported Echos](#supported-echos), has the steps by hand and what to solder.

The guided way, for every supported model:

```sh
scripts/setup.sh              # picks the Echo on adb, or asks which one; then runs every step
```

A terminal screen with a progress bar and the list of steps. It runs everything on its own and only stops when you
have to do something: download a file into `~/Downloads` (it picks it up from there and checks it), solder or plug a
cable, hold a button, type a name or the Wi-Fi password. Its last step offers another wake word ("Echo",
"Computer", …; see `scripts/artifacts.sh`), or keeps "Alexa". It offers to install missing tools. Before it starts it asks
for a typed `yes`, as it wipes the Echo. Command output goes to `build/<codename>/setup.log`; when something fails it
shows the end of it and offers to try again. Ctrl-C stops it at any point and the next run picks up where it left off;
`--dry-run` walks all steps and shows the commands without running any, `--restart` starts over for the next Echo of
the same model. The model's page has the same steps written out.

**Nothing to compile** on a commit that GitHub has a build of: every commit on `main` and `release` once CI has
published it (a few minutes after the push). The setup then offers that build, the one online updates install too, and
skips the Android NDK (1 GB), the compilers and unpacking the firmware; the build is checked against the project's
release key (`keys/release.pub`) before anything uses it. With changes of your own in the checkout, or on a commit
without a build, it builds here as before. The other scripts that need the Echo's programs (`deploy.sh`,
`install-system.sh`, `ota-push.sh`) do the same: the release build where there is no NDK here, `PREBUILT=1` to insist
on it, `PREBUILT=0` to always build.

## Updating

### From Home Assistant (online updates)

Off by default. Pick a channel in "Online updates" on the Echo's [settings page](#settings-page); the firmware
update entity in Home Assistant then shows what is new and installs it:

- `release`: releases only (built from the `release` branch);
- `beta`: every build of `main`, plus every release;
- `off`: nothing is fetched (the default).

The Echo's "Firmware" update entity then shows the newest build on that channel. Versions are the time of the
build's commit in UTC (`2026.10.02.091530`), on both channels. Its install button downloads the
bundle for this model from the project's GitHub releases and installs it, as a push from your PC would: the Echo
checks the release key's signature (`keys/release.pub`, in every build) and falls back by itself if the new version
does not stay up. Only an encrypted connection to Home Assistant, with the key Home Assistant set, may switch the
channel or install. Once the new version passes its self test it also becomes the copy the Echo falls back to, as for a push.
ESPHome mode only. The release key arrives with the install or with the first push from a build that has it; until
then the entity says so.

Turning online updates on means trusting the project's releases: they are built and signed by GitHub Actions
(`.github/workflows/build.yml`), in a job that only runs for the `main` and `release` branches; its secret is the only
copy of the release key besides the maintainer's.

### From your PC

```sh
git pull
scripts/ota-push.sh <echo-ip>        # remembers the address
```

Builds (or downloads that commit's release build, as the setup does), signs, pushes over Wi-Fi (TCP 28929). The Echo installs only what verifies against your key, restarts hassmic,
and falls back to the installed copy by itself if the new one does not stay up. What changed: [CHANGELOG.md](CHANGELOG.md).

Every update, pushed or online, runs a self test as it starts: wake word engine loaded, ports open, a second of
microphone audio. Once it passes (a few seconds), the Echo makes it the installed copy, start script and update checker
included: the version it falls back to from then on is always the last one that worked.

## Configuration

### Settings page

Every Echo serves a settings page at `http://<echo-ip>:28931/`. On first use, press "Ask the Echo", then the action
button (the dot) within a minute: the ring shows that a login waits, and the press approves this browser on this
Echo from then on. The page shows what does not work and why (Home Assistant not connected, wake word or whisper
models missing), every setting, and the browsers approved (revoke there). "Export" saves the settings as
`hassmic-settings.conf` (name=value lines, without the Echo's name, keys and pairings); "Import" applies such a file,
to this Echo or another. Works with Wyoming too.

### hassmic.conf

One file on the Echo, `/data/local/hassmic/hassmic.conf`, read at boot (edit over adb, reboot):

```sh
NAME="Kitchen Echo"         # device name in Home Assistant
PROTO=esphome               # or wyoming (port 16700)
ARGS=""                     # extra options, below
#MODE=stock-online          # temporary: stock Alexa online without updates, see the model's install page
#ADB_WIFI=1                 # leave adb over Wi-Fi open, see below
```

| `ARGS` option | Effect |
|---|---|
| `-W 0` | no settings page (port 28931) |
| `-m <pryon.manifest>` | wake word model to start with, until one is picked in Home Assistant |
| `-w remote` | wake word detection in Home Assistant (openWakeWord) instead of on the Echo |
| `-E` | no sound on wake |
| `-L` | leave the LED ring alone |
| `-V` | leave the volume buttons alone |
| `-z 0` | no Sendspin player |
| `-a 0` | no arbitration with other Echos (UDP 28930) |
| `-p <port>` | another port (the firewall only admits inbound TCP 16384–32767) |

**adb over Wi-Fi is closed.** adb on an unlocked Echo is a root shell that asks for no key, so an open port 5555
would give it to everyone on the network. Over USB adb always works. Over Wi-Fi:

- open **Debug access** on the Echo's [settings page](#settings-page) and press the action button to confirm, then
  `adb connect <echo-ip>:5555`. It closes by itself after 30 minutes, when you close it there, and at every reboot.
- or run `scripts/adb-wifi.sh <echo-ip>` on the PC you installed from: the same 30 minutes, proven with the key that
  signs your updates (`secrets/update.key`) instead of Home Assistant. This is the way in when Home Assistant cannot
  be: the Echo is not adopted yet, has lost its key, runs `PROTO=wyoming`, or Home Assistant is down. It needs hassmic
  running (like `scripts/ota-push.sh`).
- or put `ADB_WIFI=1` into `hassmic.conf`: open for good, until you take the line out (no reboot needed either way).
  For development, and the only way with `MODE=stock-online` (no hassmic running there).

Nothing else opens it; `boot.log` says when it opens and closes. If hassmic itself does not run, or the update key is
lost, only USB is left.

## Troubleshooting

Log: `adb shell tail -30 /data/local/hassmic/boot.log` (over USB, or over Wi-Fi after opening debug access on the settings page or
`scripts/adb-wifi.sh <echo-ip>`).

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
`scripts/adb-wifi.sh <echo-ip>`, `adb shell rm /data/local/hassmic/state/api_key`, restart hassmic (or reboot), delete the device in Home Assistant, add it
again.

Open issues and measurements: [PLAN.md](PLAN.md).

## Uninstall

- **Temporarily**: `adb shell rm /data/local/hassmic/hassmic.conf`, reboot. The Echo is a stock, unregistered Echo (which
  updates itself if it gets internet).
- **Properly**: `scripts/install-system.sh --uninstall` removes the files from the system partition and restores the policy.
- **Completely**: reflash the stock firmware from TWRP as in step 1 of the model's install page ([donut](devices/donut/README.md#1-unlock-flash-stock-firmware-root)).

## Security

- **ESPHome link**: encrypted like an ESPHome device with `api: encryption` but no key in its YAML. Home Assistant
  generates the key when you add the Echo, sets it over an encrypted connection, and clears it when you delete the
  device. **Until then anyone on the network can connect**, or set a key first (then see
  [Troubleshooting](#troubleshooting)). The key lives in `/data/local/hassmic/state/api_key`.
- **Wyoming link**: unencrypted and unauthenticated, like every Wyoming satellite.
- **Egress**: Amazon's daemons may only reach local addresses (plus DNS to the servers DHCP hands out); `otad` and
  `ace_otad` never get out. hassmic itself may reach any address. Put the Echo on a network without internet as a second
  layer.
- **Inbound**: TCP 16384–32767 only (26053 ESPHome, 16700 Wyoming, 28928 Sendspin, 28929 updates, 28931 settings
  page), UDP 16384–32767 (28930 arbitration between Echos).
- **Settings page**: plain HTTP (an Echo has no certificate a browser takes), so it never shows a secret (API key,
  Sendspin token, network keys). A browser gets in only by a press of the Echo's action button while its login waits;
  two browsers asking at once are both refused. Every request after that is signed with a key only that browser and
  that Echo share (X25519), with a counter against replays. Someone who can change traffic on your network (not only
  read it) could change the page itself, as with any plain HTTP page.
- **adb**: a root shell without authentication (the unlock turns adbd's key check off). Over Wi-Fi it is closed: adbd
  runs without its network listener and the firewall drops port 5555. Opened only from the settings page (30
  minutes; an approved browser, and a press of the action button for this one request), by `scripts/adb-wifi.sh` (30 minutes;
  a fresh challenge signed with your update key, so a recorded exchange does not work twice) or by `ADB_WIFI=1` in
  `hassmic.conf`; while it is open, anyone on the network has root. USB always works: physical access is root access anyway.
  Without `hassmic.conf` (stock behaviour, or before the install) it is open, as stock leaves it.
- **Arbitration between Echos**: an Echo takes the network key only from Home Assistant, over its encrypted API link,
  or from the button pairing. Through Home Assistant: a member offers the key on its "Arbitration handoff" entity only
  to an Echo whose key Home Assistant shows on that Echo's own entity (so only devices you adopted), or, where it finds
  none, asks Home Assistant to run the newcomer's own action `esphome.<node>_arbitration_key` (needs "Allow the device
  to perform Home Assistant actions"). Button pairing: a member hands the key only if exactly one Echo asked, from 2 min
  before its own buttons were held; someone on the network who asks as well only makes it fail, and the newcomer's
  buttons go first. The key travels encrypted to the receiving Echo, so it is not readable in Home Assistant's states,
  traces or logbook. Rounds are authenticated with the key and cannot be replayed. The keys are in
  `state/arb_key` and `state/arbitration`.
- **Updates**: only bundles signed with your `secrets/update.key` (pushed from your PC) or with the project's release key
  (`keys/release.pub`; downloaded by hassmic itself, only once a channel is picked under "Online updates") are installed. Root
  checks the signature with the tool and keys from the system partition or the installed copy before anything is
  unpacked. Your key also opens adb over Wi-Fi; the release key does not.
- **Bluetooth**: keys in `state/ble_bonds` (proxy) and `state/bt_keys` (speaker), both under `/data/local/hassmic/`.

## Development

Architecture, repository layout, building for the PC, tests and contribution notes: [DEVELOPMENT.md](DEVELOPMENT.md).

## Licence

[MIT](LICENSE), for everything written here. The files in `src/third_party/` keep their own licences, stated in each file:
monocypher (BSD-2-Clause OR CC0-1.0), `dr_flac.h` (public domain or MIT-0), `minimp3.h` (CC0-1.0), `rnnoise/`
(BSD-3-Clause, `COPYING` beside it), `freeaptx.c`/`.h` (LGPL-2.1-or-later; hassmic links it statically, and everything needed to rebuild and relink it is in this repository).

Nothing of Amazon's is in this repository and nothing of it is covered by this licence: firmware, libraries and wake-word
models come from your own device and stay Amazon's. Not affiliated with or endorsed by Amazon, Home Assistant or
Music Assistant; "Alexa" and "Echo" are Amazon's trademarks.

[xda]: https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/
