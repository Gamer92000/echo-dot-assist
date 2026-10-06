# Changelog

What changed for people using the Echo, newest first. Details and measurements are in [PLAN.md](PLAN.md).

## 2026-10-06

- **A new settings page.** Every setting now says what it does and when to change it, each feature which entities it
  adds to Home Assistant, and the page works on a phone. The Echos section shows the network, the other Echos and
  whether their settings match, in one list.
- **Log in once for all your Echos.** Echos in the same network (the one wake word arbitration uses, which runs on
  every Echo) trust each other: a browser logged in on one is let in on the others without pressing their buttons.
  The page of the Echo you are logged in to does it by itself; opening another Echo's page directly, its login offers
  "Log in through <Echo>", which you confirm on the Echo you are logged in to. Debug access still takes a press of
  that Echo's own button.
- **Wake words installed by hand under a short name get the full one** (`echo-de` becomes `echo-de-DE`, as
  `scripts/artifacts.sh` names them) when the Echo starts after this update, so the same wake word has one name on
  every Echo and the settings page sees it as the same. The chosen wake word stays chosen. Only for languages Amazon
  has in one region (German, Italian, Japanese, Portuguese); English, French and Spanish ones keep their name.
- **Choose what to copy between Echos.** The settings page shows this Echo's settings beside every other Echo's you are
  logged in to, with the differences marked; tick the settings and the Echos to copy them to.
- **Copy Amazon's models from one Echo to the others** on the settings page: extra wake words, whisper detection and
  the newer sound detection model. Run `scripts/artifacts.sh` for one Echo, then copy from it here; no PC needed for
  the rest. Each wake word set is first tried on the receiving Echo, and every Echo that got something restarts its
  satellite once. The page now also checks every answer an Echo sends, so nothing on the network can change what
  travels between them; an Echo on an older version needs updating before the page can work with it.
- **Sound detection says which model it uses**, Amazon's newer one or the firmware's, and when it cannot start (the
  model does not load) it now switches itself off properly: the settings page shows why, and Home Assistant no longer
  lists a "Sound" entity that would never fire.
- **Echos stay in their network with arbitration off.** "Wake word arbitration" now only decides whether an Echo
  takes part: switched off, it answers every wake word itself, and the other Echos stop waiting for it. It keeps the
  network key and its "Arbitration handoff" entity, so the settings pages still find it and settings can be copied
  to it. Echos switched off before this update keep arbitration off and join the network again. The volume-key
  pairing no longer switches arbitration on.

- **Home Assistant shows less, the settings page the rest.** Home Assistant keeps what you use day to day: mute, do
  not disturb, wake sound, the LEDs, the equalizer, the media player and firmware updates. Features are switched on
  the settings page, and while one is on its entities are in Home Assistant: wake word arbitration, sound detection,
  whisper detection, Wi-Fi motion, Bluetooth audio from phones, playing on a Bluetooth speaker. Switched off, their
  entities are removed from Home Assistant (it reconnects for a moment). Mic level, noise reduction, Bluetooth
  announcements, the online updates channel, "Music Assistant without pairing", debug access and SoC temperature / CPU
  usage are on the settings page only. After this update, entities that moved are gone from Home Assistant; the
  settings themselves are kept. Opening debug access takes a press of the action button.
- **The settings page shows the wake word arbitration network**: which Echos are in it, which are not and why (no key
  yet, or a second network beside this one), with what to do about it. It also lists the other Echos it hears: log in
  to each once, and it tells which settings differ from this Echo's and copies them over, to one Echo or to all.
- **Set up the next Echo with the settings of the last one**: `scripts/setup.sh --preset hassmic-settings.conf` (a
  file exported on the settings page), or give the file when the install step asks. The Echo starts with them.

- **Find the settings page from Home Assistant**: "Visit" on the Echo's device page opens it, and a new diagnostic
  entity "Web UI address" shows its address (`http://<echo-ip>:28931`).

- **A settings page on every Echo**, at `http://<echo-ip>:28931/`. Log in once per browser with a press of the Echo's
  action button. It shows every setting, warns about what does not work and why (no Home Assistant link, models
  missing), and exports the settings to a file you can import on other Echos. The settings file on the Echo is now
  `state/config` with one `name=value` per line; the old one is moved over by itself.

## 2026-10-05

- **Several Echos settle the wake word among themselves without "Allow the device to perform Home Assistant
  actions".** Each Echo now shows a key on a diagnostic entity, "Arbitration handoff", and the Echos hand each other the
  network key through those entities: nothing to tick, no repair in Home Assistant. Keep that entity enabled. It
  works as long as the Echo's entities carry the name of its `NAME`; if you renamed the Echo in Home Assistant, the
  old way (the permission) still works, and so does a new one without Home Assistant: hold Volume up and Volume down
  together for 2 s on the new Echo, then on one that is already in.

- **Whisper detection: one download, the one that exists.** Amazon has a single whisper model for every language
  and hands it out only when asked for American English, so `scripts/artifacts.sh` now asks for just that, whatever
  language is picked. Before, it asked for the picked language first and took the English one as a fallback.
- **`scripts/artifacts.sh` says plainly at the end when whisper detection was ticked but not installed.** The log of
  the run before is kept as `build/artifacts.log.1`, so a second try no longer wipes the record of the first.
- **Downloads from Amazon work with older Python 3** (e.g. Ubuntu 20.04's 3.8): unpacking a model failed there after
  the download.
- **`scripts/artifacts.sh` no longer downloads with a dead registration.** An Echo could still carry the registration of
  an earlier run, with a token Amazon refuses; the script took that for "registered already" and every download
  failed. It now asks Amazon whether the token works, and otherwise has you set the Echo up in the Alexa app.
- **`scripts/artifacts.sh` asks you to deregister the Echo only once it is a satellite again.** It asked before,
  while the Echo still ran as a stock Echo online, and a stock Echo resets itself to factory settings when
  deregistered: hassmic's settings, its Home Assistant key and the wake word models were gone, and the Echo stayed a
  plain Alexa. If that happened to yours: block the Echo's internet at the router, then
  `adb shell 'mkdir -p /data/local/hassmic; printf "NAME=\"Echo Dot\"\nARGS=\"\"\n" > /data/local/hassmic/hassmic.conf'`
  and `adb reboot`, add it to Home Assistant again, and run `scripts/artifacts.sh` for the wake words.
- **`scripts/artifacts.sh` tells you when the Echo's adb hangs.** After the Alexa app moved an Echo Dot 3 to another
  Wi-Fi network and back, adb over Wi-Fi took the connection but never answered, and the script waited forever. It
  now says to unplug the Echo's power and plug it back in. An Echo left waiting for the Alexa app (orange ring) by a
  run stopped halfway is explained too.

## 2026-10-04

- **Whisper detection.** Whisper to the Echo, and Home Assistant knows: the binary sensor "Last request whispered" is
  on when your last request was whispered. Use it in your conversation agent's instructions to have the answer
  whispered as well, as a stock Echo does ([how](README.md#whisper)). It is Amazon's own detector, run on the Echo
  with a model that only Amazon hands out: the guided setup's last step offers it next to the wake words, and
  `scripts/artifacts.sh` ("Other artifacts") adds it to an Echo set up before. ESPHome only.
- **Newest sound detection model.** `scripts/artifacts.sh` and the guided setup can install Amazon's newest sound
  detection model; hassmic then takes it in place of the one in the firmware (and goes back to that one if it does not
  load). Not ticked by default: in tests it scored the same.
- **`scripts/artifacts.sh` over Wi-Fi no longer hangs** after restarting the Echo as a stock Echo. A stock Echo
  that is not registered drops your Wi-Fi and opens its own setup network, so it now asks you to set it up in the
  Alexa app first and waits for it to come back afterwards. With the Echo on USB as well, it uses USB.
- **`scripts/artifacts.sh` menus** no longer get garbled when an entry is wider than the terminal.

- **Cancel a request with the action button**, as with the center button of a Voice PE: pressed while Home Assistant is
  still listening or thinking, the request is aborted, the conversation agent included, so a misheard command does not
  go on to switch things it should not (a tool call already under way still finishes). The wake word said while it
  thinks cancels too and starts a new request straight away. Before, both worked only once the reply was being
  spoken. ESPHome only (Wyoming has no way to abort a request).

## 2026-10-03

- **Install without compiling anything.** On a commit that GitHub has a build of (every commit on `main` and
  `release` once CI has published it), `scripts/setup.sh` offers that build: no Android NDK (1 GB), no compilers, no
  unpacking of the firmware, and fewer tools to install. It is the build online updates install, and it is checked
  against the project's release key before anything uses it. With changes of your own in the checkout it builds as
  before. `deploy.sh`, `install-system.sh` and `ota-push.sh` take that build too when there is no NDK on the PC
  (`PREBUILT=1` to insist on it, `PREBUILT=0` to always build).
- **Update keys, push updates and `adb-wifi.sh` no longer need a C compiler on the PC.** The PC's side of the update
  tool is Python now (`scripts/otatool.py`). Keys, signatures and bundles are the same as before; nothing changes on
  the Echo.

- **`scripts/install-system.sh` no longer stops silently on a build without the Wi-Fi motion module** ([issue
  #5](https://github.com/Gamer92000/echo-dot-assist/issues/5)). That module is optional and is only built when the
  kernel source and its toolchain are there. Without it, the install quit before writing anything and gave no message.
  It now installs without the module. If you hit this, run the install again.

- **Guided setup: Wi-Fi motion's kernel module is now part of the build.** Until now only released updates had it.
  The downloads step now also fetches the kernel sources and the compiler the module is built with (115 MB, checked
  against fixed checksums), so a setup build matches the released one. Building by hand: `make kernel-tools`. The
  build needs `bc`, which the setup offers to install along with the other tools.

- **Guided setup: Echo Dot 3 unlock fixed.** `scripts/setup.sh` stopped at "Waiting for the Echo's bootrom" with
  "./bootrom-step.sh: No such file or directory": the kamakiri zip unpacks into a folder of its own, and the step looked
  for its scripts one level too high. It now finds them wherever the zip puts them. If you hit this, run
  `scripts/setup.sh` again; nothing needs deleting. The Echo Dot 2 and Echo 2 steps find amonet the same way now, in
  case a later zip is laid out differently.

- **Guided setup: the Echo Dot 3 unlock no longer hangs after the handshake** ([issue
  #4](https://github.com/Gamer92000/echo-dot-assist/issues/4)). kamakiri waits for Enter right after it reaches the
  Echo's bootrom, and the setup gave it no keyboard, so it waited forever with the ring dark. It now runs in front of
  you: hold the dot button, plug in, and when it asks, release the button and press Enter. Nothing is written to the
  Echo before that point, so an Echo stuck there is unchanged; unplug it and run `scripts/setup.sh` again.

- **Guided setup: the Echo Dot 3's build step unpacks the firmware again** ([issue
  #4](https://github.com/Gamer92000/echo-dot-assist/issues/4)). On a PC where `firmware/donut/rootfs` did not exist yet,
  the unpack silently wrote nothing and the build then stopped with "missing .../libmixerAPI.so". The folder is now
  created first, and the step fails if the firmware did not come out. The manual steps in the README had the same gap.

- **`scripts/probe.sh` says when there is no Echo on adb** instead of listing every file as different. A stock Echo
  has no adb until it is rooted, so this check only works after that.

## 2026-10-02

- **Online updates from Home Assistant** ([issue #3](https://github.com/Gamer92000/echo-dot-assist/issues/3)), off by
  default. A new "Online updates" select picks a channel: `release` (releases only), `beta` (every build of the main
  branch, plus releases) or `off`. The Echo's "Firmware" update entity then shows when there is something newer and
  installs it with one click, without a PC. The builds come from the project's GitHub releases; the Echo only installs
  them if they are signed with the project's release key, and goes back to the previous version by itself if the new
  one does not stay up. Echos installed before this need one push from the PC (or a fresh install) first, to bring that
  key along. Versions are now the date and time of the build's code (UTC), like `2026.10.02.091530`.

- **Updates become the fallback by themselves.** Every update, pushed from the PC or installed from Home Assistant,
  checks itself as it starts (wake word engine, network ports, a second of microphone audio) and, once that passes, is
  what the Echo falls back to from then on. `scripts/ota-push.sh` no longer asks, and `--approve` is gone.

## 2026-10-01

- **Wi-Fi motion, experimental.** The Echo Dot 3, Echo Dot 2 and Echo 2 can now work as a motion sensor, from their
  Wi-Fi signal: someone walking between the Echo and the router changes it. Switch on "Wi-Fi motion detection
  (experimental)" (off by default); "Wi-Fi motion (experimental)" then shows motion in Home Assistant, and "Wi-Fi motion
  sensitivity (experimental)" sets how much it takes. It notices movement, not someone sitting still, and has been
  tried for a few minutes and one night, so expect false alarms; see the README. It works through a small kernel
  module that reads the signal of every frame from your router; the module comes with the update and is only loaded
  once you switch Wi-Fi motion on.

- **First install fixed** ([issue #2](https://github.com/Gamer92000/echo-dot-assist/issues/2)). Installing on an
  Echo for the first time stopped at the "Install" step ("No such file or directory", then "Permission denied" for
  `/sepolicy.new`); only Echos installed before 2026-09-30 got through, because they had run the older installer. Fixed,
  with two smaller hiccups of the guided setup: the firmware unpacking failing on a missing `images` folder, and the
  lockdown failing when adb was gone for a moment. An Echo that is unlocked already is now taken straight to TWRP by
  the "Unlock" step instead of having to be marked done by hand.

- **Play on a Bluetooth speaker.** The Echo can now send everything it plays to a Bluetooth speaker, as a stock Echo
  can. Put the speaker in pairing mode and switch on "Bluetooth speaker search" in Home Assistant; "Play on Bluetooth
  speaker" switches between the speaker and the Echo, and the Echo reconnects by itself. While on the speaker, the
  volume buttons and Home Assistant set the speaker's own volume (shown on the light ring); the Echo's volume comes back
  when you switch back. Music Assistant keeps it in time with other players through "Bluetooth speaker delay".

- **Sound detection, optional.** A new "Sound detection" switch (off by default) runs Amazon's Alexa Guard model on
  the Echo itself, and a "Sound" entity in Home Assistant reports what it heard: smoke or CO alarm, breaking glass, a
  dog barking, a baby crying, snoring, coughing, water, a beeping appliance. It is less reliable than on a stock Echo,
  where Amazon's cloud checks every hit first, and it takes up to 10 s; see the README before you rely on it.

- **`scripts/wakeword.sh` is now `scripts/artifacts.sh`, and asks once.** Its menu has an entry per kind of
  artifact: "Wake words" (to install, from the PC or downloaded from Amazon) and "Other artifacts" (Alexa Guard's
  sound detection model, only kept on the PC for tests; Home Assistant does not use it yet), each a list of ticks with
  everything new ticked, plus the language for Amazon downloads. "Go on" shows what will happen, asks once, and then
  does all of it with a single Amazon registration.

- **The Echo's light sensor in Home Assistant.** A new "Illuminance" sensor reports the room's light in lux, the
  reading the stock Echo uses to dim its light ring. The ring keeps dimming with the room as before (that is Amazon's
  own code on the Echo, on by default); the new "LED brightness" slider holds it at a level of your choice instead, and
  "LED auto brightness" hands it back to the light sensor. Both are kept across restarts.

## 2026-09-30

- **The light ring shows when the Echo is ready to pair.** While the "Bluetooth pairing" switch is on, the ring runs
  Amazon's blue chaser, the one the stock Echo shows while it searches for devices. It stops when a phone has paired,
  when the two minutes are up or when you switch it off.
- **Updates now ask you to try them, then renew the Echo's fallback copy.** The copy on the system partition, which the
  Echo falls back to when an update does not come up, was the one from the day it was installed; changing it took USB
  and a trip through TWRP. `scripts/ota-push.sh` now pushes the update as before, asks you to try it, and when you say
  yes the Echo writes it over that copy, together with its start script and the tool that checks updates. Say no (or
  run it without a terminal) and it stays an update only; `scripts/ota-push.sh --approve <echo-ip>` approves it later.
  Echos installed before this change get there the same way, over Wi-Fi.
- **Installing no longer goes through TWRP.** `scripts/install-system.sh` writes the system partition while the Echo
  runs normally (one reboot instead of two); `--twrp` does it the old way, e.g. for an Echo that no longer starts.
- **adb over Wi-Fi is closed.** On an unlocked Echo adb is a root shell that asks for no key, and it was open to
  everyone on the network (issue #1). It is now closed at every boot and opened only on purpose: the new switch
  **Debug access (adb over Wi-Fi)** in Home Assistant opens it for 30 minutes (it closes by itself, or when you turn
  it off, or at a reboot). Without Home Assistant (an Echo not adopted yet, one that lost its key, Wyoming),
  `scripts/adb-wifi.sh <echo-ip>` does the same with the key that signs your updates. `ADB_WIFI=1` in `hassmic.conf`
  keeps it open. adb over USB works as before. **Keep `secrets/update.key`**: with neither it nor Home Assistant, only
  USB is left. Update with `scripts/ota-push.sh`; to keep adb over Wi-Fi as it was, add `ADB_WIFI=1` first.
- **`hassmic.conf` can only be changed by root.** It was writable by every user on some Echos, although the Echo runs
  what it says as root.

- **Commands no longer fade out after the first second.** Amazon's audio processing has to be told when a command is
  being spoken; otherwise it treats a voice that keeps talking as background noise and removes it after about 1.5
  seconds. The stock Alexa software did that, the Echo as a Home Assistant satellite did not: quietly spoken commands
  lost their second half (wrong words, or Home Assistant stopped listening mid-sentence). It does now, from the wake
  word until Home Assistant has heard the command.
- **New setting "Noise reduction"**: off (default), low, medium, high. Takes background noise out of what the voice
  assistant hears (RNNoise, by up to 6, 9 or 12 dB) before the volume is evened out. Worth trying if quietly spoken
  commands are misunderstood; more than that was audible as artefacts. It costs some processor time while a command
  is being heard.
- **The wake sound no longer makes the command quieter.** The Echo's own wake sound is still faintly in what the
  microphones pick up; the volume control took it for a loud talker and turned the command after it down.
- **Echo 2 and Echo Dot 2: the mute could show the wrong way round.** On these models the Echo only counted presses of
  the mute button. If it restarted (an update, for example) while the microphones were off, it started from "on":
  button lit and microphones cut, but no red ring and "unmuted" in Home Assistant, and every press wrong from then
  on. The Echo now reads the real state of the mute circuit, at start, on every press and once a second besides.
  Update with `scripts/ota-push.sh`. (Read on an Echo 2; on the Dot 2 the same file is expected but not yet seen.)
- **Echo 2: the firewall could fail to come up after a boot.** A helper of the firewall service could hang right at
  its start (a quirk of the Echo 2's system tools), and then the rule that keeps Amazon's software from reaching the
  internet was missing until the next boot, and push updates were not installed. Alexa and the firmware updaters
  were stopped all the same. Fixed; update the Echo 2 (`scripts/ota-push.sh`).
- **The firewall is now watched from a second place.** Should the firewall service ever fail again, the Echo notices
  within 20 seconds, puts the rule back itself, restarts the service and writes it into its log.
- **An Echo could be "unavailable" in Home Assistant after a boot while everything on it was running.** The Echo
  relies on a handful of Amazon's own firewall rules: the one that lets its traffic out, the ones that let Home
  Assistant, Music Assistant and answers in. Amazon's firewall script can lose any of its rules at boot, and nothing
  noticed: with the wrong one missing the Echo was cut off until the next boot
  ([issue #1](https://github.com/Gamer92000/echo-dot-assist/issues/1)). The Echo now checks all of its firewall
  every 5 seconds, not only that its own rule comes first: each of its own rules and their order, each of Amazon's
  rules it needs, that everything else inbound is still refused, and on models that cannot filter IPv6 that IPv6 is
  still off. What is missing or changed is put back within seconds, with a log line naming it. The Echo also loads
  its rules in one step now instead of some thirty: there is no moment in which they are half there, and it gets in
  the way of Amazon's script far less (about seven times fewer of its rules lost). Update with
  `scripts/ota-push.sh`.
- **Several Echos: better pick of the one that answers.** The Echos now compare the measurement Amazon's audio
  processing itself takes of each wake word (the one Alexa's cloud used), instead of one taken from the finished
  microphone stream. Echos on an older version still take part.

## 2026-09-29

- **Quiet speech is understood.** The Echo's microphones deliver speech far quieter than a Voice PE (about 30 dB):
  Amazon's cloud was tuned for that, Home Assistant's speech recognition and "finished speaking" detection are not, so
  softly spoken commands came out as wrong words or were cut off mid-sentence. The Echo now brings speech to a steady
  level itself before sending it, starting from how loud the wake word was, without raising the room noise in pauses
  and without clipping when someone speaks up close. The wake word is not affected.
- **One mic setting that works: "Mic level".** "Noise suppression level", "Auto gain" and "Mic volume multiplier" never
  had an effect: Home Assistant ignores them for ESPHome devices. They are replaced by "Mic level" (-35 to -15 dBFS,
  default -26, the usual reference level for speech): how loud speech reaches the voice assistant; raise it if quiet speech is still missed. Delete the three
  leftover entities in Home Assistant.

## 2026-09-28

- **Echo 2 (`radar`) supported.** Tried on a real Echo 2 with the guided setup (`scripts/setup.sh radar`). It has no
  USB socket: the setup shows where to solder the USB wires (TP13/14/15 on the amplifier/tweeter board) and that the
  power adapter is needed for the unlock. Its firmware is Fire OS 6572 (one build older than the Dot 3's): the same
  334 `libmixerAPI.so` exports as donut, the slimmer Pryon engine generation (as on `crumpet`) with all needed
  symbols. The keys are where the Echo Dot 2 has them. Bluetooth stays off for now (`-B`, written at install).
  Unlock zip and firmware go to `firmware/radar/`.
- **Music Assistant: paired servers only.** Music Assistant now has to pair with the Echo's token before it can play
  (an unpaired connection is encrypted under a key everyone knows). An Echo that played unpaired so far goes quiet in
  Music Assistant until it is paired, or until the new switch "Music Assistant without pairing" in Home Assistant is
  switched on (off by default; switching it off cuts off an unpaired server that is playing).
- **An adopted Echo no longer shows up as "discovered" again.** On the Echo 2 Wi-Fi comes up late in the boot, so the
  Echo announced itself with a placeholder MAC address and Home Assistant took it for a new device. The announcement
  now waits for Wi-Fi. Every Echo also announces its own host name (e.g. `echo-dot.local`) instead of `linux.local`,
  which Home Assistant showed next to the name. Names with umlauts become readable host names ("Küchen Echo" ->
  `kuechen-echo`).
- **Other wake words on every Echo.** `scripts/wakeword.sh` asks each Echo's engine which model sets it can load: the
  Echo 2's engine is older and gets its own.
- **Echo Dot 2 (`biscuit`) supported.** Tried on a real Echo Dot 2 with the guided setup (`scripts/setup.sh biscuit`):
  micro-USB, no soldering. Its pinned firmware (Fire OS 6574.1, the same build generation as `donut`) has
  `libmixerAPI.so` and `libpryon.so` byte-identical to donut's. Unlock files (R0rt1z2's amonet v2.0.0, `boot-root.zip`,
  firmware) go to `firmware/biscuit/`. It has no mute latch: the mute button is a key, toggled in software.
- **Bluetooth on the Echo Dot 2.** Its chip only knows Bluetooth 4.0 LE events and refused hassmic's start-up, so
  Bluetooth was off. hassmic now falls back to the 4.0 set: Bluetooth proxy and speaker mode start on the Dot 2
  (pairing uses the older LE method there, which the chip is limited to). `-B` leaves Bluetooth to the stock stack on
  a model where it does not work yet.
- **Firewall on Echos without IPv6 filtering.** The Dot 2's firmware has no `ip6tables`, so the lock could not cover
  IPv6 while the log claimed it did. There IPv6 is now switched off entirely; hassmic only uses IPv4.
- **Other wake words in one command.** `scripts/wakeword.sh <echo-ip>` puts "Echo", "Computer", "Amazon", "Ziggy" (or
  "Alexa" in another language) on an installed Echo. Models fetched once work on every Echo, so a second Echo needs
  no Amazon account at all: pick from the list, it checks the model on that Echo and restarts it. For a new one it
  does the Amazon part for you and only stops for registering and deregistering in the Alexa app; the update block
  stays on the whole time and everything is undone at the end. Then pick the wake word in Home Assistant. The guided
  setup offers the same as its last step.
- **Guided installation.** `scripts/setup.sh` is a terminal app: it recognises the Echo on adb, shows a progress bar
  and the step list, runs the steps one after the other and only stops when you have to do something (download,
  solder, hold a button, type a name). Downloads are picked up from `~/Downloads` by themselves and checked, missing
  tools are offered for install, the Android NDK is fetched without a question. Command output goes to
  `build/<codename>/setup.log`; you see it only when something fails. One typed `yes` at the start covers everything
  that wipes or flashes the Echo. You can stop at any point; it goes on where it left off. The install instructions
  moved from this README to a page per model: [devices/donut/README.md](devices/donut/README.md).
- **Setup ends with a finished satellite.** The install step asks the name, installs, waits until the Echo is up as a
  satellite, prints its address and tells you to adopt it. Model-specific hassmic arguments are written at install.
  The end screen shows the Sendspin pairing token and reminds you to allow the Echo to perform Home Assistant actions
  (needed for several Echos to agree which one answers).
- **Ready for more Echo models.** Everything that differs between models now sits in one folder per model under
  `devices/`, so other Echos can be added later. The 2018 Echo Dot 3 (`donut`) behaves exactly as before. If you
  build it yourself, two things move:
  - the firmware image, `kamakiri-donut-v1.0.0.zip`, `boot-root.zip` and what is unpacked from them go to
    `firmware/donut/`; move your existing `firmware/rootfs`, `firmware/images`, `kamakiri/` and `boot-root/` there;
  - the Echo binaries are built into `build/donut/`.

  The scripts that use adb now check which model is connected and that it runs the right firmware, and the installer
  refuses to write to any other. An Echo turns down a pushed update built for another model, once it has received one
  update of this version.
- The stock-online guard (install step 3) needs `devices/donut/device.conf` pushed next to `lockdown.sh`. Without it,
  the guard reports "OTA GUARD NOT ACTIVE" instead of running.

## 2026-09-25

- **Only one Echo answers, like Alexa.** With several Echos in earshot, only the one that heard "Alexa" most clearly
  answers; the others stay silent and dark. The Echos agree on it among themselves on your network in 0.2 s. An Echo
  you are already talking to, or that is ringing, keeps the wake word. With one Echo nothing changes and nothing waits.
  The Echos find each other by themselves (new "Join arbitration network" switch, on by default), and the shared key
  is handed from one to the next through your Home Assistant, so another device on the network cannot join or silence
  them. For that, each Echo needs "Allow the device to perform Home Assistant actions" ticked in its ESPHome options
  (Settings → Devices & services → ESPHome → the Echo → Configure), the same option the Bluetooth announcements use;
  Home Assistant shows a repair until it is. Renaming the Echo or its entities in Home Assistant does not matter. Each
  Echo needs its own `NAME` in `hassmic.conf`. Also on UDP port 28930.
- **Pick the wake word in Home Assistant.** The Echo's wake word select now lists every wake word installed on it (the
  stock "Alexa" plus any you fetched, such as "Echo"), and switching takes effect at once and survives restarts. Until
  now Home Assistant was only ever shown "Alexa", even when the Echo actually listened for "Echo".
- **The microphone comes back by itself.** An Echo could stop hearing anything after hours of running (the wake word
  did nothing, the buttons still worked) until hassmic was restarted. It now notices within a few seconds and
  reconnects the microphone.
- **No red flash on the second satellite.** When another voice satellite reports the wake word first, Home Assistant
  lets only that one answer. The Echo that came second used to show the error light; now it just goes quiet.

## 2026-09-24

- **Bluetooth announcements in your language.** "Connected to …" and "Disconnected from …" can now be said in German,
  French, Spanish, Italian, Portuguese, Dutch, Swedish, Danish, Norwegian, Finnish or Polish instead of English: pick
  it in the new "Bluetooth announcement language" setting in Home Assistant, to match the language of the Echo's
  assistant. It cannot follow the assistant by itself because Home Assistant does not tell the Echo which language
  that is. The choice survives restarts.
- **Equalizer, like the Alexa app's.** Three new sliders in Home Assistant: "Equalizer bass", "Equalizer mid" and
  "Equalizer treble", each from −6 to +6 dB. They use Amazon's own equalizer inside the Echo, so they shape everything
  it plays: replies, music from Music Assistant or a phone, and sounds, on the built-in speaker and on the 3.5 mm
  output alike. The Echo keeps the setting itself, so it stays after a restart.
- **Do not disturb, like Alexa's.** A new "Do not disturb" switch in Home Assistant. While it is on, announcements
  (`assist_satellite.announce`, "ask a question") are not played. Everything you start yourself still works: the wake
  word, replies, timers, music and the Bluetooth "Connected to …" message. Turning it on shows Alexa's single purple
  pulse on the ring. The setting survives restarts. For a schedule, use a Home Assistant automation, and you can switch
  it by voice if the switch is exposed to Assist.
- **"Connected to <phone>" like Alexa.** When a phone or computer connects to the Echo as a Bluetooth speaker, the Echo
  plays Amazon's Bluetooth chime and says "Connected to" and the device's name; on disconnect the other chime and
  "Disconnected from …". The words come from Home Assistant's text-to-speech, so Home Assistant has to let the Echo
  ask for it: Settings → Devices & services → ESPHome → the Echo → Configure → tick "Allow the device to perform Home
  Assistant actions" (until then only the chime plays, and Home Assistant shows a repair about it). The new
  "Bluetooth announcements" switch turns both chime and words off.
- **Bluetooth speaker again.** Phones and computers can play to the Echo over Bluetooth, beside the Bluetooth proxy.
  To pair, turn on the new "Bluetooth pairing" switch in Home Assistant and pick the Echo on the phone within two
  minutes; afterwards the phone connects by itself whenever you choose the Echo. Music from the phone is ducked while
  you talk to the assistant, and the wake word listens through it like through other music.
  Codecs: SBC like stock Alexa, and in addition AAC (what iPhones, iPads and Macs use), aptX and aptX HD. While a
  phone plays, the Echo stops scanning for Home Assistant's Bluetooth devices: the radio cannot do both without the
  music stuttering.
  The phone's volume slider moves the Echo's volume and the other way round, and the action button pauses and resumes
  the phone.
- **One music source at a time.** When a phone starts playing over Bluetooth, Music Assistant pauses (the whole group
  the Echo is in); when Music Assistant starts playing on the Echo, the phone pauses. Verified with Music Assistant and
  a Pixel.
- **Bluetooth proxy for Home Assistant.** The Echo now scans for Bluetooth LE devices and passes what it hears to Home
  Assistant, like an ESPHome Bluetooth proxy. After updating, Home Assistant picks it up by itself: the Echo appears
  under Settings → Devices & services → Bluetooth, and BLE sensors, trackers and beacons in range show up.
  Integrations that have to connect to a device can do so through the Echo too, up to 3 devices at a time, and pair
  with it where the device allows pairing without a PIN (like ESPHome's proxies). Paired devices are remembered across
  restarts. Amazon's Bluetooth service is stopped for this, so the Echo no longer works as a Bluetooth speaker (without
  Alexa nothing could pair with it anyway).

## 2026-09-23

- **No more silent speaker after Alexa's mute.** Amazon's mixer has a global mute that silences every sound whatever the
  volume, and it survives reboots, so an Echo muted under stock Alexa stayed silent under hassmic: no replies, no music,
  no sounds. hassmic now clears it when it finds it set.
- **Replies and music from wherever Home Assistant points.** hassmic may now connect to any address, not only local
  ones, so a Home Assistant reached by a public domain, a Tailscale address or IPv6 works too. Amazon's own services
  stay locked to the local network. Safe now that only the paired Home Assistant can tell the Echo what to fetch.
- **Encrypted connection to Home Assistant.** The ESPHome connection now uses the same encryption as ESPHome devices.
  Home Assistant creates the key by itself when the Echo is added, and from then on only Home Assistant can connect.
  Already added? Home Assistant sets the key on its next connection, nothing to do. See "Encryption key" in the README
  if it ever needs a reset.
- **Replies and music from a Home Assistant with a host name.** The Echo now finds `.local` names (like
  `homeassistant.local`) by mDNS, may ask the DNS servers the network hands out even when one is public (8.8.8.8 from
  DHCP used to be dropped), and tries every address a name resolves to instead of giving up after the first. Before, an
  internal URL with a name instead of an IP could leave the Echo silent at any volume. What still has to hold is in the
  README under "No sound from replies or music?".
- **No more 20 s of deafness after a restart or update.** Amazon's mixer waits for its performance monitor before it opens
  the mic, and the lockdown used to stop that daemon. It now keeps running, so the wake word listens again right after
  hassmic starts.
- **Alexa's original sounds** for the wake word, the action button, the volume keys and the mic-off button (mics off / mics
  on), taken from the stock firmware on the device. The generated blip is gone. The "Wake sound" switch in Home Assistant
  silences all of them.
- **Button sounds no longer drop out.** About a third of them were lost to a race in Amazon's mixer library; playback now
  waits for the mixer to take the sound before closing the stream.
- **Volume stays consistent.** Replies play on a separate volume (`TTSVolume`) that only followed the main volume when
  hassmic changed it. It is now kept in line every 2 s, and a main volume changed from outside is reported to Home
  Assistant and Music Assistant.
- **Diagnostics in Home Assistant**: SoC temperature and CPU usage as sensors, disabled by default. Enable them in the
  entity settings of the Echo Dot device.
- For developers: `kill -TTIN $(pidof hassmic)` writes what the wake word hears to `state/capture.raw`, the next one stops
  it. The only way to record the processed mic stream while hassmic runs.

## 2026-09-22

- **Wake word during alarms and playback.** hassmic now tells Amazon's wake word model when a timer rings, music plays or a
  reply is spoken, and the model switches to the lower accept threshold it carries for those moments ("Echo" needs a score
  of 0.45 instead of 0.75 while an alarm rings). Stopping an alarm or interrupting music by voice works from further away.
