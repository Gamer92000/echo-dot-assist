# User guide

Each feature and setting in detail, and what it costs. Installing, updating, troubleshooting and undoing it are in
the [README](../README.md).

- [Voice](#voice) · [Wake word](#wake-word) · [Several Echos](#several-echos)
- [Buttons and light ring](#buttons-and-light-ring) · [Music](#music) · [Bluetooth proxy](#bluetooth-proxy)
- [In Home Assistant](#in-home-assistant) · [Sound detection](#sound-detection) · [Whisper detection](#whisper-detection) · [Wi-Fi motion](#wi-fi-motion)
- [Settings page](#settings-page) · [Setting up from a phone](#setting-up-from-a-phone) · [Factory reset](#factory-reset)
- [hassmic.conf](#hassmicconf) · [adb over Wi-Fi](#adb-over-wi-fi) · [ESPHome or Wyoming](#esphome-or-wyoming)

## Voice

Replies start while text to speech is still being generated.

- **"Alexa, stop"** interrupts a reply, but only right behind the wake word: Amazon's models only listen for "stop" in
  the two seconds after it. Out of silence, "Alexa, stop the music" goes to Home Assistant as a normal command.
- While a timer rings or something plays, the wake word is accepted more readily, as Amazon's models are tuned to do.
- A finished timer rings for a minute ("Timers ring for" on the [settings page](#settings-page): 10 s to 10 min, or
  until stopped), with the cyan timer animation on the ring. Only an explicit stop ends it: "<wake word>, stop", the
  action button, or the media player's stop in Home Assistant. The wake word alone pauses the ring while you talk, and
  it goes on afterwards. With microWakeWord or Home Assistant's wake word (no "stop" keyword) the transcript does it:
  "stop" alone (also stopp, halt, arrête, basta, para) ends the ring and the run, before Home Assistant answers.
  Several timers can ring at once: each new one gives the ring its full time again, and one stop ends them all.
  Home Assistant's "Timer ringing" is on just as long, so an automation can tell when nobody heard it:

  ```yaml
  triggers:
    - trigger: state
      entity_id: binary_sensor.kitchen_echo_timer_ringing
      to: "on"
      for: "00:00:50"     # ring time 1 min: still on after 50 s = nobody is there
  actions:
    - action: notify.mobile_app_phone
      data:
        message: "Timer {{ states('sensor.kitchen_echo_ringing_timers') }} is ringing in the kitchen"
  ```

## Wake word

Three engines, picked under "Wake word" on the [settings page](#settings-page):

|                                         | Amazon's (default)           | microWakeWord (experimental)                    | Home Assistant's           |
|-----------------------------------------|------------------------------|-------------------------------------------------|----------------------------|
| Words                                   | "Alexa"; "Echo", "Computer", … downloaded from Amazon | Okay Nabu, Hey Jarvis, Hey Mycroft, Alexa, your own | any openWakeWord model, your own too |
| How well it hears                       | best                         | **significantly worse**                         | depends on the model       |
| "<word>, stop", keener while playing    | ✅                           | ❌                                              | ❌                         |
| Only the nearest Echo answers           | ✅ by the front end's energies | ✅ by audio level, a rougher guess             | ❌ Home Assistant lets the first answer |
| Runs                                    | on the Echo                  | on the Echo                                     | in Home Assistant, ~256 kbit/s streamed all the time |

### Amazon's wake words

"Alexa" comes with the firmware. Others ("Echo", "Computer", …) only Amazon hands out: "Download from Amazon" on the
settings page (not on the Echo Dot 3), `scripts/artifacts.sh` from the PC (it registers the Echo to an Amazon account
for a few minutes and undoes it), or "Copy models" from another Echo. Pick one in Home Assistant's wake word select.

### microWakeWord

[microWakeWord](https://github.com/kahrendt/microWakeWord) is the engine of ESPHome's own satellites (Home Assistant
Voice PE). Its small models were trained mostly on synthetic speech and never with the Echo's microphones: **expect
more missed wake words**, above all from across the room or over music, and more false wakes. While it is on you also
lose "<wake word>, stop", the extra sensitivity while the Echo plays, rings or speaks, Amazon's wake words, and the
front end's energies in arbitration. Sound and whisper detection keep working.

Switching on, the page lists these costs and, on an Echo without a model yet, asks for one to start with: one of
ESPHome's (fetched from GitHub by your browser) or your own `.tflite` with its `.json` manifest. The same section then
adds, renames, tunes (threshold), saves and deletes models; "Copy models" in the Echos section copies them to your
other Echos. The models are ESPHome's own files (TensorFlow Lite, not ONNX); the Echo runs them with its own
interpreter, which gives the same results as TensorFlow Lite. Switching back, or deleting the last model, restores
everything.

### Home Assistant's

The Echo stops listening itself and streams its microphone to Home Assistant all the time, whose wake word engine
listens: openWakeWord, with your own models too. The page's dialog walks through Home Assistant's side:

1. The openWakeWord app (or `wyoming-openwakeword` beside a Home Assistant container).
2. Custom models in `/share/openwakeword`, `.tflite` only: an `.onnx` (as EchoMuse's Forge makes them) needs converting first.
3. "Add streaming wake word" on the assistant, and that assistant on the Echo's device page.

When Home Assistant hears the wake word, the Echo answers as on its own: the sound, the ring, the front end held on the
talker until the command ends. It costs arbitration between Echos, "<wake word>, stop", the extra sensitivity while
playing, everything while Home Assistant is down, and likely some reliability. Switching to or from it restarts the
satellite for a few seconds. `-w remote` in [`ARGS`](#hassmicconf) does the same, and then the page cannot change it.

## Several Echos

The Echos compare by how clearly the word stood out of the room's noise. An Echo in a conversation, or ringing, keeps
the next wake word. With only one Echo there is no delay.

- Only Echos listening for the same word compete: one on "Echo" and one on "Alexa" each answer their own.
- "Wake word arbitration" on the settings page (on by default) says whether an Echo takes part. Off, it answers every
  wake word itself but stays in the Echos' network, so the settings pages still find it.
- Other satellites (ESP32 and so on) are not part of it. Home Assistant lets the first to report answer, and an Echo
  that comes second just goes quiet instead of flashing an error.

**The shared key.** The Echos share a key so that nobody else on the network can join or silence them, and your Home
Assistant vouches for each: every Echo reports a tag named after its key as scanned (Settings › Tags lists one "Tag
hassmic_…" per Echo), and an Echo hands the key, encrypted, only to one whose tag Home Assistant confirms. This needs
the Tags integration (part of the default configuration); names, rooms and renames do not matter. Without it, either:

- tick "Allow the device to perform Home Assistant actions" in each Echo's ESPHome options, or
- pair with the buttons: hold Volume up and Volume down together for 2 s on the new Echo, then on one already in. A tap
  sounds; then Bluetooth's "connected" sound when it worked, "disconnected" when it did not within 2 minutes.

### Together with Kiosk Satellite tablets

"Arbitration mode" on the settings page switches an Echo to [Kiosk Satellite](https://kiosksatellite.com/docs/voice-satellite/#wake-word-arbitration)'s
protocol, so that Echos and tablets listening for the same word answer once between them: the device that heard it
loudest over its room answers, after the "Kiosk Satellite window" (400 ms, as on the tablets). The page shows both
modes side by side. Kiosk mode costs what that protocol does not have:

- anyone on the network can claim every wake word and keep the Echo silent (no key);
- an Echo in a conversation gets no preference;
- every wake word waits the window, even with no other device around;
- a claim lost on Wi-Fi leaves two devices answering (Home Assistant then lets the first through);
- the firewall admits UDP 2330 while the mode is on.

Use the same mode on every Echo: Echos in different modes do not settle wake words with each other (the page warns).
The Echo's loudness is not yet calibrated against a tablet's microphone: "Kiosk Satellite loudness offset" (−20 to
+20 dB) shifts it by hand. Raise it if a tablet answers when you spoke to the Echo, lower it the other way round, and
keep it the same on every Echo. The log notes Kiosk claims once a second at most: anyone on the network can send
them, and the log is on flash.

## Drop In

Talk between two Echos, as with Alexa's Drop In. The Echos must be in the same Echo network (above: the settings
pages list them under Echos) and have "Drop In" on (Features, on by default).

- **From Home Assistant**: the action `esphome.<echo>_drop_in` with `target:` the other Echo's name as it first
  appeared in Home Assistant ("Echo Dot 3 5695c4") or its node name. Each Echo also has "Drop In" (idle, calling,
  ringing, connected), "Drop In with" (the other Echo) and an "End Drop In" button.
- **By voice**: import the blueprint [`blueprints/automation/drop_in.yaml`](../blueprints/automation/drop_in.yaml)
  (Settings › Automations › Blueprints › Import, with the file's GitHub address). "Drop in kitchen" or "Verbinde mit
  Küche" then calls the Echo in the area of that name, or one whose name contains the word, from the Echo you spoke to.
  Sentences come for English, German, French, Spanish, Italian and Dutch ("drop in …" in every one); the reply is
  in the language of the voice assistant you spoke to. Sentences and replies are inputs of the blueprint.
- **From the settings page**: the Drop In button on another Echo's card under Echos.

The Echo called plays Alexa's Drop In chime and connects at once. With "Drop In answers: after the action button" it
rings for 30 s instead and only its action button connects. The ring shows Alexa's call animations meanwhile. To end a
call: the action button, "<wake word>, stop", "<wake word>, hang up" (or "auflegen", "raccroche", "cuelga",
"riattacca", "ophangen") on either Echo, or "End Drop In".
Music on the Echo pauses when a call connects (it does not resume by itself). The volume buttons work during a call; the wake word too, but not while the other side talks (its "Alexa" for its own
Echo would wake this one).

Calls are refused with do not disturb on, with the microphones off, or with another call running; the caller's log and
Home Assistant say why. The sound goes straight between the two Echos (UDP 28932, Opus, encrypted): no cloud, no Home
Assistant in between.

**Echo.** The Echo puts Amazon's audio front end into its call mode, whose echo canceller works on the other side's
voice. What it leaves would still come back to the other Echo, so an Echo sends its room only while someone there
talks; while the other side talks it is turned down 25 dB. Two people talking at the same time work only if the
one here is clearly louder than the echo of the other: as on a speakerphone, take turns.

## Buttons and light ring

Beyond the README's two figures:

- **Action**: cancelling works while Home Assistant still listens or thinks, as on a Voice PE; the wake word then
  cancels too and listens again (ESPHome only).
- **Microphone off** plays Alexa's own sounds.
- Silent and dark at boot.
- The ring dims with the room's light as on a stock Echo (Amazon's own logic, on by default); a fixed level from Home
  Assistant switches that off.

## Music

One source at a time, the newest wins: a phone starting over Bluetooth pauses Music Assistant (the whole group), Music
Assistant starting on the Echo pauses the phone. The voice assistant ducks both.

- **Music Assistant** finds the Echo by itself (Sendspin player). Pair it with the Echo's token: shown at the end of
  `scripts/setup.sh`, and as the diagnostic entity "Sendspin pairing token" in Home Assistant (disabled by default).
  Steps: the model's page, "Optional: Music Assistant".
- **From a phone** over Bluetooth: SBC, AAC, aptX, aptX HD, with media controls. Switch on "Bluetooth audio from phones" on the
  settings page; the "Bluetooth pairing" switch in Home Assistant then lets phones pair (blue chaser on the ring).

### Playing on a Bluetooth speaker

Everything the Echo plays (replies, timers, its sounds, music) can come out of a Bluetooth speaker instead, as with
stock.

1. Switch on "Play on a Bluetooth speaker" on the settings page; its entities appear in Home Assistant.
2. Put the speaker in pairing mode near the Echo and switch on "Bluetooth speaker search".
3. Within a minute the Echo pairs with the strongest one it hears (speakers, headphones, PCs that offer to play audio)
   and plays on it.

You cannot pick from a list: Home Assistant reads an ESPHome select's choices only when it connects, so keep only the
speaker you want in pairing mode. "Play on Bluetooth speaker" switches between it and the Echo; the Echo reconnects by
itself when the speaker comes back, and takes it when the speaker calls on switching on. "Bluetooth speaker" shows its
name and state.

- **Volume**: while on the speaker, the volume buttons, Home Assistant and Music Assistant set the speaker's volume, and
  the ring shows it; the speaker's own buttons move it too. Back on the Echo, its own volume returns. With Bluetooth
  absolute volume (most speakers) the Echo sends full level and the speaker turns it down, which sounds best; without,
  the Echo turns it down itself, as stock does.
- **In time with other players**: a Bluetooth speaker plays late, by its buffer. "Bluetooth speaker delay" (default
  250 ms) is what the Echo allows for; set it by ear, in Home Assistant or on the settings page.
- The Echo still listens for the wake word meanwhile. How well it hears over loud music from elsewhere in the room is
  not measured yet.
- SBC only (every speaker has it), one speaker at a time. Setting it up needs ESPHome mode.

## Bluetooth proxy

Works like an ESPHome `bluetooth_proxy` with `active: true`: scanning, up to 3 connections, "Just Works" pairing only.
While a phone plays, the proxy stops scanning: the radio cannot do both without the music stuttering.

## In Home Assistant

| Always there | |
|---|---|
| Media player | TTS, `play_media` |
| Mute, Do not disturb | DND drops announcements; purple flash when switched on |
| Wake sound | covers all the Echo's own sounds |
| Equalizer | bass, mid, treble, −6 to +6 dB, Amazon's own, on everything the Echo plays |
| LED auto brightness, LED brightness | a level holds the ring there and switches the automatic off |
| Illuminance | the Echo's light sensor in lux, for automations |
| Timer ringing, Ringing timers | on while a finished timer rings; their names (diagnostic), see [Voice](#voice) |
| Firmware | update entity, see [Updating](../README.md#updating) |
| Identify, Factory reset | buttons |
| Web UI address | diagnostic: the settings page's address |
| Sendspin pairing token | diagnostic, disabled by default |

**Features**, switched on the settings page: while one is on, its entities are in Home Assistant; off, they are gone
(Home Assistant reconnects for a moment when one is switched).

| Feature | Entities |
|---|---|
| Wake word arbitration (on by default) | Arbitration peers |
| [Sound detection](#sound-detection) | Sound (event) |
| [Whisper detection](#whisper-detection) | Last request whispered |
| [Wi-Fi motion](#wi-fi-motion) | motion sensor, sensitivity |
| Bluetooth audio from phones | Bluetooth pairing |
| [Playing on a Bluetooth speaker](#playing-on-a-bluetooth-speaker) | Bluetooth speaker search, Play on Bluetooth speaker, Bluetooth speaker, Bluetooth speaker delay |

**On the settings page only**:

- "Mic level": how loud speech reaches the voice assistant, −35 to −15 dBFS (default −26); the Echo adjusts its gain.
- "Noise reduction": off by default; low, medium, high take the background down by up to 6, 9 or 12 dB (RNNoise, on
  what the voice assistant gets).
- "Timers ring for": 1 minute by default, 10 s to 10 min, or until stopped.
- Bluetooth announcements and their language, the online updates channel, debug access (adb over Wi-Fi), SoC
  temperature and CPU usage.
- "Music Assistant without pairing": off by default, so only Sendspin servers paired with the token may play.

## Sound detection

Optional, off by default. Runs Amazon's own Alexa Guard model on the Echo, beside the wake word; the "Sound" event
entity reports `smoke_or_co_alarm`, `glass_break`, `dog_bark`, `baby_cry`, `snoring`, `cough`, `water`,
`beeping_appliance`. Use it in automations ("When Sound fires with smoke_or_co_alarm").

> [!WARNING]
> **A hint, not an alarm system**, and never a replacement for a smoke or CO detector. Amazon checks every hit in its
> cloud before it tells anyone; that cannot be had without Amazon, so here every hit counts. In tests it took a barking
> dog, pouring water and a toilet flush for breaking glass, and a cough for a beeping appliance.

- **Slow**: the model listens in 10 s windows, so an event comes up to 10 s late, and once per window while the sound
  goes on.
- **Coarser than stock**: smoke alarms, smoke sirens and CO alarms score the same, as do coughs and running water, so
  each pair is one event. "Human presence" is left out: it fires on any talk, TV or knock.
- Nothing is reported while muted, or for a window in which the Echo itself played something.
- **Private**: it all happens on the Echo, only the event leaves it (a stock Echo uploads the recordings). About 13 % of
  one CPU core while on (Echo Dot 2).
- Uses the firmware's model; Amazon's newest can replace it ("Download from Amazon", or `scripts/artifacts.sh` "Other
  artifacts"; it scored the same so far). ESPHome only. Background: [re-aed.md](re-aed.md).

## Whisper detection

A stock Echo answers a whispered request in a whisper. Here the binary sensor "Last request whispered" tells your
conversation agent, so it can answer the same way. It uses Amazon's whisper detector on the Echo with a model only
Amazon hands out: "Download from Amazon" on the settings page, or `scripts/artifacts.sh <echo-ip>` ("Other artifacts" ›
"Whisper detection"; over Wi-Fi, open debug access first). The model stays through updates; without it there is no sensor.

The sensor is set when you stop speaking, before speech to text has finished, so the agent's prompt can read it. For
example, in the LLM conversation agent's instructions. The template finds the sensor of whichever Echo you spoke to,
so one prompt serves every Echo, whatever you named them:

```jinja
{%- set w = device_entities(llm_context.device_id) | select('search', '_last_request_whispered$') | first | default(none) %}
{%- if w and is_state(w, 'on') %}
The user whispered. Start your answer with the literal tag [whisper].
{%- endif %}
```

Replace `[whisper]` with the markup your text to speech understands; Piper has none.

> [!NOTE]
> The sensor is reliable; the language model is less so. It may forget the tag or put it somewhere else, and nothing
> on the Echo checks what it wrote. A missing tag means a normal answer. A misplaced one
> may be read out aloud. Expect a whispered answer most of the time, not every time.

- In tests (Echo Dot 2, German, 1–2 m) whispered commands scored 984–999 of 1000, spoken ones 0–18, quiet ones too.
  Saying the wake word normally and whispering the rest is fine.
- Sounds without words (breathing, rustling) can score high, but only what the pipeline took for a command is scored.
- On the Echo, during your request only. ESPHome only. Background: [re-whisper.md](re-whisper.md).

## Wi-Fi motion

**Experimental**, off by default. A motion sensor without extra hardware: someone walking between the Echo and your
router changes how strongly the Echo receives the router. "Wi-Fi motion (experimental)" goes on while that happens and
off 30 s after, like a PIR sensor; "Wi-Fi motion sensitivity (experimental)", 1 to 10 (default 5), sets how much
change counts. Tried in one flat for a few minutes and one night, where it mostly did what it should.

- **Motion, not presence**: someone sitting still does not show.
- **Only between the Echo and the router**, also in the next room if the router is there. Elsewhere in the room may
  not show at all.
- **Expect false alarms** from other Wi-Fi devices, doors, people in the router's room; try the sensitivity before
  relying on it. On the Echo Dot 2 and Echo 2 also when the router changes speed (the Echo Dot 3 allows for that).
- **Through a kernel module of ours**: the Wi-Fi drivers do not report what this needs. Loaded only once you switch it
  on (within 10 s), then until the Echo restarts.
- ESPHome only. It does not use the microphones; muting does not stop it.

## Settings page

Works in Wyoming mode too; the "Web UI address" entity shows the address.

**Logging in**: press "Ask the Echo", then the action button within a minute (the ring shows that a login waits).
Revoke approved browsers on the page. Echos in one network trust each other: a
browser logged in on one gets into the others without their buttons ("Log in through <another Echo>"). Debug access
still takes that Echo's own button.

At the top it says what does not work and why (Home Assistant not connected, models missing).

**Echos**: every Echo this one hears (and any you add by address), the arbitration network and those outside it with
the reason and what to do.

- **Copy settings**: every setting of this Echo beside the others', differences marked. Tick settings and Echos to copy.
- **Copy models**: wake words, microWakeWord models, whisper and sound detection models, Echo to Echo. A wake word set
  is tried on the receiving Echo's engine first; each Echo that got something restarts its satellite once.
- **Download from Amazon**: wake words, whisper detection, the newer sound detection model, fetched on the Echo itself.
  Sign in with a code you enter on your Amazon site, tick what you want. Not on the Echo Dot 3 (its newer attestation
  to Amazon is not reversed): download on another Echo, or with `scripts/artifacts.sh`, and copy.

**Export / Import**: `hassmic-settings.conf` (name=value lines, without name, keys and pairings). Import applies it to
this Echo or another, or use it as a preset for the next install (`scripts/setup.sh --preset <file>`).

**Identify** (top of the page, and beside each Echo in the list; also a button in Home Assistant): a rainbow on the ring
for 10 s and stock's setup sound, to tell which Echo is which.

**Name** (System): the one the Echo gave itself, as the Voice PE does. Its node name `echo-dot-3-5695c4` is the ESPHome
device name, the host name `<node>.local` and the base of entity ids. It never changes, not even with a reset; name
the Echo in Home Assistant.

**Switch Wi-Fi network** (System › Wi-Fi): pick one of the networks it sees or type a name (hidden network); the
password before or after, empty for an open network. The Echo tries the new network without saving it, keeps it only
once it has an address and the router answers, and otherwise returns to the old one within a minute or two, saying
why. Once the new one works it forgets the old ones. Expect a new IP address: find it as "Web UI address" in Home
Assistant (which finds the Echo again within a few minutes if it reaches that network), and approve the browser there
once more. WPA2 and open networks only.

**Log** (System): `boot.log` with a filter, the older part, and a download for bug reports ([reading it](../README.md#when-something-is-wrong)). The Sendspin token is
blanked; the rest travels unencrypted, like the whole page.

## Setting up from a phone

An Echo that is not set up yet (just installed, or after a [factory reset](#factory-reset)) shows the orange spinner
until Home Assistant has added it. While it has no network, it can be set up over Bluetooth with Improv Wi-Fi, as
ESPHome devices and the Voice PE are:

1. Home Assistant app: Settings › Devices & services › Add device; the Echo shows up by its name. (Home Assistant finds
   it too, as "Improv via BLE", when one of its Bluetooth adapters or proxies is in range.)
2. Pick your Wi-Fi network and type its password. A wrong one is reported back, and nothing is saved.
3. Once on the network, Home Assistant discovers it as an ESPHome device: add it. The spinner stops.

- A new Echo advertises after 20 s without a network. An Echo installed with `scripts/setup.sh` is on your Wi-Fi
  already: the phone is for a reset, a new network, or an install by hand.
- An Echo that is set up and lost its network for 10 minutes (new router, moved) advertises too, but takes a network
  only after a press of its action button (good for a minute).
- Improv sends the Wi-Fi password over Bluetooth **unencrypted**: that is the protocol, the same for every Improv
  device. Not with `-B` (Bluetooth off) in `ARGS`.

## Factory reset

At 5 s of holding the action button the ring warns: let go and nothing happens. Or "Factory reset" in Home Assistant
(a button on the device page), or "Factory reset…" under System on the settings page.

- **Forgotten**: everything in `/data/local/hassmic/state/` (settings, Home Assistant's key, approved browsers,
  Bluetooth pairings, Sendspin, microWakeWord models) and every saved Wi-Fi network. It starts as after the install:
  orange spinner, [setup from a phone](#setting-up-from-a-phone). Delete the device in Home Assistant and add it again.
- **Kept**: the install, `hassmic.conf`, Amazon's extra models.
- Do not hold past 20 s: Amazon's own button handler still runs and has a factory reset of its own at 21 s.

## hassmic.conf

One file on the Echo, `/data/local/hassmic/hassmic.conf`, read at boot (edit over adb, reboot):

```sh
PROTO=esphome               # or wyoming (port 16700)
ARGS=""                     # extra options, below
#MODE=stock-online          # temporary: stock Alexa online without updates, see the model's install page
#ADB_WIFI=1                 # leave adb over Wi-Fi open, see below
```

| `ARGS` option | Effect |
|---|---|
| `-W 0` | no settings page (port 28931) |
| `-m <pryon.manifest>` | wake word model to start with, until one is picked in Home Assistant |
| `-w remote` | wake word in Home Assistant; the page then cannot change it |
| `-E` | no sound on wake |
| `-L` | leave the LED ring alone |
| `-V` | leave the volume buttons alone |
| `-z 0` | no Sendspin player |
| `-a 0` | no arbitration with other Echos (UDP 28930), and so no Drop In |
| `-i 0` | no Drop In (UDP 28932) |
| `-B` | no Bluetooth |
| `-p <port>` | another port (the firewall only admits inbound TCP 16384–32767) |

## adb over Wi-Fi

adb on an unlocked Echo is a root shell that asks for no key, so over Wi-Fi it is closed. USB always works. To open it
for 30 minutes:

- **Debug access** on the settings page, confirmed with the action button, then `adb connect <echo-ip>:5555`. Closes
  after 30 minutes, when you close it there, and at every reboot.
- `scripts/adb-wifi.sh <echo-ip>` on the PC you installed from: proven with your `secrets/update.key` instead of Home
  Assistant. The way in when the Echo is not adopted, lost its key, runs Wyoming, or Home Assistant is down. Needs
  hassmic running.
- `ADB_WIFI=1` in `hassmic.conf`: open for good, until you take the line out (no reboot needed). For development, and
  the only way with `MODE=stock-online`.

`boot.log` says when it opens and closes. If hassmic does not run, or the update key is lost, only USB is left.

## ESPHome or Wyoming

`PROTO=wyoming` in [hassmic.conf](#hassmicconf) makes the Echo a Wyoming satellite (port 16700). What it keeps and loses:

| | ESPHome (default) | Wyoming |
|---|---|---|
| Voice, wake word on the Echo or in Home Assistant, interrupt | ✅ | ✅ |
| Buttons, light ring, hardware mute | ✅ | ✅ |
| Music Assistant (Sendspin) | ✅ | ✅ |
| Only the nearest Echo answers | ✅ | ❌ |
| Drop In between Echos | ✅ | ❌ |
| Timers, announcements, follow-ups, media player | ✅ | ❌ |
| Settings and sensors as entities | ✅ | ❌ |
| Bluetooth from phones | ✅ pairing from Home Assistant | phones paired already |
| Playing on a Bluetooth speaker | ✅ | keeps one already set up |
| Bluetooth proxy, sound, whisper, Wi-Fi motion | ✅ | ❌ |
| Encrypted link | ✅ key set by Home Assistant | ❌ plain TCP |
| Online updates | ✅ | ❌ pushed from your PC only |
| Settings page, setup from a phone, factory reset | ✅ | ✅ |
