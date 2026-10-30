# Changelog

What changed for people using the Echo, newest first. Details and measurements are in [PLAN.md](PLAN.md).

## 2026-10-30

- **When the wake word's sounds give no answer, the Echo no longer guesses a pause inside your command** (issue #79).
  It then fell back on the old rule, which could take the short stop in the "ts" of "lights" for the pause after the
  wake word: "Alexa lights on" came out as "And so on." Now it leaves out at most 0.12 s there, so at worst a bit of the
  wake word stays in. Words that have no sounds to go by work as before. The log now also shows what the sounds said:
  `wake: the aligner's end, from Pryon's end: -680 ms`.
- **A command said straight after the wake word is no longer squashed at its start** (issue #82). While the wake sound
  played, the microphone's level stayed where the wake word had set it, also for the part of your command spoken before
  the sound began. "Alexa" often arrives much quieter than what follows, so the start of the command came out far too
  loud and was held down by a limiter, syllable by syllable. Now whatever was spoken before the sound sets the level, and
  over the sound the level can only come down, smoothly.
- **The settings page is a set of pages now, and uses your screen.** It was one narrow column to scroll through from
  top to bottom. There is a menu down the side (behind a button on a phone), a page for each subject, and as many
  columns of cards as your screen has room for, from one on a phone to four or five on a big monitor. Every page has an
  address of its own, so the back button and bookmarks work. "Search settings" (or the `/` key) finds any setting and takes you there. Every setting reads the
  same: its name, one line on what it is for, its control in the same place, and the full explanation behind the ⓘ
  beside its name. A new Overview shows how the Echo is doing, your other Echos, every
  feature with its switch, and volume, mute and do not disturb at hand. Everything to do with models is together under Models; Bluetooth is together
  with music; "Wake sound removal" moved to Voice. Colours follow the system, or pick Light or Dark.
- **Copy settings is now "Compare & copy": all your Echos side by side, copying any way round.** A table with a row per
  setting and a column per Echo. Click the value you want and it goes to the other Echos in that row, from whichever
  Echo has it, this one or another: one page is enough now, you no longer have to open the page of the Echo you want to
  copy from. Click another value and the copy turns around; "Use all" makes every row like one Echo. Echos left out by
  a cross or a lock stay as they are, and nothing changes until you press Apply. "Group identical Echos" folds Echos with
  the same settings into one column, so five Echos in two setups read as two columns; past six Echos it starts switched
  on, and the table draws narrower. A setting only another model has is listed too. "Copy models" works the same way, between any of your Echos.
- **A row of "null" under "Timer sound"** on the settings page is gone.
- **microWakeWord can be picked again.** Since the timer sounds of 2026-10-28, clicking it on the settings page did nothing:
  the dialog that asks for a first model failed before it opened.
- **With Home Assistant's wake word the settings page keeps your other Echos.** The Echo network does not run then, and
  the page dropped every other Echo, promising they would show up by themselves. They stay now, and it says to add
  more by address.
- **"Log in through another Echo" no longer loses its question.** The Allow and No buttons went away at the page's next
  refresh, within 15 seconds, and left nothing to answer.

## 2026-10-29

- **The Echo now finds where the wake word ends by its sounds** (issues #72, #79). Amazon's engine says "heard"
  somewhere near the end of the word, not at it: for the German "Alexa" about a tenth of a second before the end, for
  the English one, "Echo" and "Ziggy" already inside your command. Judging by loudness alone, the Echo could not tell
  the end of "Alexa" from the start of a command said in one breath ("Alexa lights on" came out as "On"). Now it lines
  up the word's sounds ("a-l-e-k-s-a") with what the microphone heard, the way speech researchers' forced aligners do,
  and starts your request right where the word ends, before Amazon's "heard" where that came late; with a pause after
  the word, in the middle of it. Measured on 4586 wake words said by 30 synthetic voices in German, English, French and
  Spanish (children to elderly, whispering, accents), against where each word really ends: the request starts at the
  right place in 74 % of them, against 47 % with the release and 42 % at Amazon's "heard" (90 % with a pause after
  the word, 64 % said in one breath). It works for all of Amazon's wake words in German, English, French, Spanish and
  Japanese, and for microWakeWord's "Okay Nabu", "Hey Jarvis" and "Hey Mycroft" (Japanese not measured yet: tell us
  how it goes). It hears the word best in the language you speak, so it
  goes by that, not by the wake word's: "Language you speak" on the settings page (Wake word), automatic from your time
  zone in Home Assistant. And it learns from your requests how the wake word sounds in your home (your voices, your
  room, its microphone) and gets better still from the sixth request on (that is in the 74 %: 55 % without it;
  "Learn how the wake word is said here", on by default; what it learns stays on the Echo). Nothing to switch on.
- **Short commands said straight after the wake word are heard** (issue #80). Home Assistant's speech detector starts
  afresh for each request and does not listen to its first three quarters of a second, so a short command said at
  once was over before it listened: Home Assistant waited 15 seconds before it answered, and some speech to text
  (Parakeet) found no words at all. Every request now opens with 0.8 s of silence, sent at once, so nothing waits for
  it. Nothing to switch on. Thanks to rmaffeo for the recordings behind both, and for PR #71, which had the silence as
  a setting.
- **Joining another Echo's wake word arbitration** no longer falls back to the newcomer's action when Home Assistant
  connected late (after a restart, or both Echos booting at once): the Echos give Home Assistant's tags their 30 s from
  then. Owners who did not allow the Echo to perform actions got a repair in Home Assistant for nothing.

## 2026-10-28

- **Echo Input (2018, `cupcake`).** Worked out from its firmware (Fire OS 6.5.7.5, NS6575/8338): the Echo
  Dot 3's hardware with the Echo Shows' Android software. It builds, the releases carry its bundle, and
  `scripts/setup.sh cupcake` walks the steps (unlock with kamakiri-cupcake over the micro-USB socket, no soldering;
  root with the Echo Dot 3's boot-root). No speaker: replies, sounds and music come from its 3.5 mm jack or a Bluetooth
  speaker. Its one LED shows the Echo's states and Home Assistant's "Light ring" light; nothing turns towards the
  talker. Installed and running on one Echo Input (2026-10-29): everything works as expected (`devices/cupcake/README.md`).
- **Optionally, the wake word goes to Home Assistant too** ("Wake word in the request", settings page, Voice; off by
  default). Speech to text then hears the whole sentence, "Alexa, turn on the light", so nothing of your first word
  can be cut and no stray end of the wake word turns up. Meant for an AI conversation agent: Home Assistant's own
  built-in sentences may not recognise a command with "Alexa" in front. "Alexa, stop" still stops a ringing timer or
  music.
- **Other people talking no longer end up in your request: "End of request heard on the Echo"** (experimental, issue
  #66; settings page, Voice; off by default). Home Assistant ends a request only when the room goes quiet, so in a busy
  kitchen it stayed open while others talked, and speech to text wrote down their words after yours ("Next. Eat some
  food please"). The Echo knows how loud you said the wake word: with this on, it ends the request itself when *you*
  pause, and quieter voices around you no longer get turned up to speech level either (they were, within two
  seconds). On the Echo Dots it also listens to Amazon's own speech detector, which the Echo's audio front end turns
  out to report for every 8 ms of sound: clattering dishes as loud as your voice don't hold the request
  open. Relaxed, Normal and Aggressive differ in how long a pause ends it (1.25, 0.7, 0.45 s) and how much quieter
  than you another voice must be to be left out (about 17, 14, 11 dB). It cannot tell a voice as loud as yours, and
  may end too soon if you turn away mid-sentence: start with Relaxed and tell us how it goes. ESPHome only.
- **The end of the wake word no longer reaches speech to text** (issue #72). Amazon's wake word engine is sure of
  "Alexa" before it has been fully said, and a drawn-out "Alexaaa", or one in a room that echoes, sounds on for a
  third of a second after that. That end went to Home Assistant as loud as the command and came out as a stray word in
  front of it ("The next track", "Faint memories"), so built-in sentences such as "next track" no longer matched. The
  Echo now waits for the pause after the word and starts there. When there is no pause (the command said straight on,
  a loud room) it never leaves out more than before: a sliver of the word left in is better than your first word cut.
  The log says how much it left out and why.
- **Timers ring with Alexa's own timer tone** (issue #63). A finished timer now plays the tone a stock Echo rings with,
  from the Echo's own firmware, instead of the quiet triple beep. "Timer sound" on the settings page (Voice) picks
  another: Alexa's alarm tone, the beep (now as loud as Alexa's tone: it was about 4 dB quieter), or a sound of your
  own. For your own, upload any sound file your browser plays (MP3, WAV, OGG, ...): its first 30 seconds play over
  and over. The page measures how loud it is, and when it is clearly louder or quieter than Alexa's tone it says so
  and offers to bring it to the same level, so one timer volume suits them all. "Play" plays the chosen sound once.
  Each Echo keeps its own; the ring time, every way of stopping it, and the timer volume work as before. An Echo
  whose firmware has no such tone keeps the beep.
- **Nothing lost right after the wake word** (issue #77). With other Echos around, an Echo asks Amazon's audio front
  end how well it heard the wake word before it claims it. On an Echo Show that took up to 0.4 s, during which the Echo
  stopped listening: up to a quarter of a second of the command was lost, and speech to text misheard the word the
  gap fell into. The Echo now listens on while it asks, and sends Home Assistant all of it. With arbitration off, or
  no other Echo to compare with, it no longer asks at all and answers sooner. Switching to "Kiosk Satellite" mode as a
  workaround is no longer needed.

## 2026-10-27

- **Drop In: the wake word works during a call to an empty room.** An Echo in a quiet room sent its own room noise,
  turned up to speech level, to the caller. The caller's Echo took that for the other side talking: it ignored the wake
  word, never switched on listening mode (so "Alexa, hang up" came out as "No text recognized") and turned its own
  voice down. An Echo now sends its room only when something clearly louder than quiet speech is there, and counts
  the other side as talking only when it is well over that side's own noise. Both Echos need this version.
- **"Music Assistant without pairing" takes effect at once.** Switching it on, on the settings page, closes the
  Sendspin connections that wait for a pairing token, so Music Assistant reconnects and plays instead of waiting.
- **Drop In by voice: "drop into the kitchen" and "drop it in the kitchen" work too.** Speech to text often hears
  "drop in" that way. The blueprint's default sentences have both now. An automation made from it before keeps its own
  list: add the two sentences there.
- **Echo Show: hassmic starts sooner after a boot.** Turning Alexa's apps off asked Android about each one
  separately; it now asks once.
- **Echo Show: switching Wi-Fi works on a network Amazon's setup saved.** Android lets only the one who saved a network
  change it. The settings page's "Switch network…" and setup from a phone now do it as Android's system user when root
  is refused, as the guided setup already did.
- **Echo Show: no hold-to-reset.** The guide now says it: the Shows have no action button. Both volume keys pressed
  together stand in for it (logins, debug access, setup from a phone), and held they pair Echos. Reset a Show from
  Home Assistant or the settings page.
- **Guided setup over ssh and on a Raspberry Pi.** Run over ssh, the setup also gives the Echo's USB devices to a
  group you are in (plugdev, adbusers, sudo or wheel). Before, adb found no Echo, because the udev rules only serve
  whoever sits at the PC. The Echo Show setups now install those rules too. On a PC that is not x86_64, they install
  qemu-user from the package manager. Where users may not create namespaces (Ubuntu 24.04 and later), they ask to allow
  them until the next boot. Before, the root step failed with "the policy patch failed" and no reason.
- **Echo Dot 2 on the October 2026 software: an unlock is proven, on Linux** (issues #50, #74). amonet v2.0.1's
  payload for that bootloader (`2c3569d-20260617_045829`) unlocked #50's Echo on 2026-10-26, on Linux. From Windows
  nobody has yet: #74's one attempt ran amonet through a wrapper around platform-tools 37.0.1's fastboot — refused,
  exit 1, no error text kept, the Echo unharmed. amonet's own Windows runner, `fastbrick.bat`, flashes with the zip's
  bundled `bin\fastboot.exe` (platform-tools 36.0.0) before any system fastboot.

- **Echo Dot 2, Echo 2 and Echo Dot 3: "Download from Amazon" signs in again where it said "no device
  attestation".** Amazon's attestation library refused to open its key when an earlier, harmless library lookup in
  hassmic had failed, and the log blamed a missing group ("dlsym(HMI) failed: (null)"). hassmic now clears that
  leftover before it opens the key.
- **Echo Show: your own light ring designs show on the band.** The light designs and theme packs are back on the
  Show's settings page. Each design plays on the band along the bottom of the screen as the ring's twelve LEDs laid
  out in a row, with the side that faces the talker in the middle; it is never turned towards the talker. The
  designer is the same on every Echo, with a Ring / Band switch on its stage: it opens on what this Echo has and
  shows the same design the other way at a click, so you can see how it looks on a Dot and on a Show while you draw.
  Amazon's animations to start from are the band's own, and Apply shows the designs at once, with no restart. Designs and packs move between Dots and Shows as before. The volume of
  voice replies now shows in cyan on the band, as it does on the Dots' ring.
- **The volume keys' tone plays at the volume they set.** With separate volumes it played at the "Echo sounds"
  volume, so with nothing playing the keys sounded like they did nothing. It now plays at the volume you just
  set: the music's, or the reply's, timer's or call's while one of those plays (at 0, as quiet as the lowest step). This applies to every model.
- **Echo Show: with "Light band" at 0 the band no longer shows for a moment at every start**, and apps on the screen
  are no longer laid out twice then. The settings page also says when the band is off instead of "On the band".
- **Echo Show 5: separate volumes now use the Show's own volume steps.** They were worked out from an Echo Dot 2,
  whose steps are quite different, so a reply set well below the music came out about 5 dB too loud. Measured on an
  Echo Show 5: each kind now plays as loud as that number on the Show's volume.

## 2026-10-26

- **Echo Show: the settings page shows only what the light band can do.** "Light ring rotation" and the light ring
  designs are for the Dots' LEDs; the Show's page no longer lists them, where they did nothing or failed on Apply. A
  Bluetooth speaker that sets its own volume now starts at the loudest of the separate volumes, not the music's, so
  replies are not quieter for a moment after it connects. The volume the Show keeps survives a power cut.
- **Echo Dot 2 from Windows, step by step** (issue #67). A new page, [docs/WINDOWS.md](docs/WINDOWS.md), for
  people who have never used a command line: how to tell which Echo you have, the five tools to install with every
  command to paste, how to get this project, what each step of the setup asks of you (with the button gesture for
  the unlock and what the light ring means), adding the Echo to Home Assistant, and what to do when something goes
  wrong.
- **The settings page no longer says "cannot list its models" right after you log in.** Its first requests go out
  together, and the Echo turned away the ones that arrived out of order, so a card could show an error for up to
  30 seconds. The Echo now takes them in any order, each once.
- **Save your models to a file and put them back later.** "Models on file" in the settings page's Wake word section
  saves the wake words, whisper and sound detection models you downloaded from Amazon, and your microWakeWord
  models, as one file on your computer. Loading that file puts them back on this Echo or any other, with the same
  checks as a copy between Echos. So your models no longer depend on Amazon still handing them out. The file is a
  plain `.tar` that any computer opens.
- **The settings page finds the other Echos when you open it through a reverse proxy.** It called them on the port
  of the address in the browser, which behind a proxy is 80 or 443, so every other Echo showed "Not reachable". It
  now calls them on the port the Echos' pages really use. A page that comes through an HTTPS proxy still cannot call
  them (the browser blocks plain HTTP from it); the Echo then says so and which address to open instead.
- **Music Assistant 2.10.8 no longer calls the Echo a "non-compliant client"** (issue #64). Music Assistant 2.10.8
  speaks Sendspin 1.0, and the Echo only spoke the draft of the earlier versions. Music played anyway, but Music
  Assistant logged four warnings on every connection, and a later version could refuse the Echo outright. The Echo
  now speaks Sendspin 1.0 to Music Assistant 2.10.8 and later, and the earlier draft to versions up to 2.10.6,
  picking the right one each time a server connects. Pairing, volume and the delay setting work with both.
- **Guided setup: a Wi-Fi network the Echo had saved before is taken as it is.** Joining failed with "WifiService did
  not add the network" when the Echo Show already knew the network (from a stock update over Wi-Fi, or Amazon's setup):
  Android lets only the system user change it, and the setup now asks as that user.
- **Echo Show 5 (2nd gen, `cronos`): first support, installed on one.** The 2021 Show 5 runs the same Fire OS build as
  the 1st gen, so it installs the same way ([`devices/cronos`](devices/cronos/README.md), `scripts/setup.sh cronos`). Its
  mute button and speaker amplifier differ: the mute button is answered as on the Echo Show 8 2nd gen, and the "Speaker
  gain" setting finds whichever amplifier the unit has (Realtek's as on the 1st gen; TI's up to +4 dB; Maxim's, the
  first one's, up to +6 dB). Its setup installs the udev rules for USB access, as the Dots' do. On the first Echo the
  guided setup ran through; Home Assistant, the wake word, the keys, the mute switch, the light band and the light
  sensor work. Not checked yet: the speaker gain by ear, Wi-Fi motion, "Download from Amazon", Bluetooth.

## 2026-10-25

- **Applying light ring designs again right after a failed install works.** Pressing Apply within the same second
  as an install that failed took that old failure for the new request: the page said "failed" at once, and the
  designs root then installed were not taken as installed.
- **What you said and what the assistant answered, as sensors in Home Assistant** (issue #53). "Request and reply
  text" in the settings page's Features adds "Last heard" and "Last reply", for a dashboard beside the Echo or an
  automation. Both are cleared when a request starts, so saying the same thing twice still counts. Home Assistant
  takes at most 255 characters per sensor: a longer reply ends in "…". Off by default: while it is on, every request
  is kept in Home Assistant's history.
- **The setup says why it will not take another firmware version** (issue #62). A newer firmware renamed to the
  expected file name kept the setup waiting with "download it again". It now says that only that exact file works,
  and the install guides say so too, also for an Echo that already runs a newer version: the unlock flashes the
  pinned one over it.
- **The install guides for the Echo Dot 3 and the Echo 2 say which cable to solder.** A cut USB-C to USB-C cable
  left the PC seeing nothing: a USB-C port needs a 5.1 kΩ resistor on the plug's CC wire before it switches on. The
  guides and the setup now say to take a cable with a USB-A plug, or add that resistor.
- **Echos in other rooms no longer slow the answer down.** With several Echos, each one waited for every other's
  claim before it answered, up to a second when one far away never heard you, and more Echos meant a later wake
  sound. Each Echo now learns which others could beat it: it waits for another Echo only if, the last times it heard
  you about as clearly as now, that one heard you better or nearly as well. Heard clearly, it no longer waits for the
  Echo at the other end of the house that catches a faint "Alexa" now and then; heard faintly, it still waits for
  the one next door. A door opened or an Echo moved shows in the next few wake words. Nothing to set; the settings
  page names the Echos it does not wait for.
- **The ring no longer pulses all night after a settings page login nobody approved.** A browser asking to log in
  (or for debug access) shows a pattern on the ring until the action button is pressed. When the page was closed
  before that, the pattern stayed on, even across restarts of the satellite: an Echo 2 pulsed from 02:43 until the
  next day. The request now runs out after a minute on its own, and every start clears that pattern.
- **"Download from Amazon" no longer ends in "Failed to fetch".** Installing restarts the satellite, and the page
  waited only a fixed 3 seconds before it took the Echo for back: often it was still the old process, about to
  stop, and the reloaded page got no answer. The models were installed all the same. The page now waits until the
  Echo says it started anew, and a page that reloads into a restart tries again for a few seconds. The same goes for
  copying models between Echos and for switching the wake word engine.
- **Echo Dot 2s on the October 2026 software can be unlocked, most likely** (issue #50). Fire OS 8338 brought a new
  bootloader that the unlock tool amonet v2.0.0 had no payload for: fastboot refused it with "restricted on locked
  hw". The setup now uses amonet v2.0.1 (download `amonet-biscuit-v2.0.1.zip` from the XDA thread), which carries a
  payload made for that bootloader. Nobody has reported an unlock with it yet.
- **An Echo Dot 2 can be unlocked from Windows** (issue #60). In Git Bash, `scripts/setup.sh` now unlocks a stock Echo
  Dot 2 too, with [firebreak](https://github.com/bboe/firebreak), which also installs the firmware and root; the setup
  then goes on as before. It needs uv and two USB drivers, which the setup names. Other models still need Linux on the
  PC for the unlock.
- **Setup joins hidden Wi-Fi networks.** The Echo's first Wi-Fi join now asks for the network by its name, as the
  settings page's "Switch network…" already did. A hidden network was never found: the Echo kept looking for it.
- **Setup no longer waits forever after a failed unlock** (issue #50). On an Echo Dot 2 whose bootloader Amazon
  patched (a June 2026 build), the amonet unlock tool reports success while the Echo never leaves fastboot and
  TWRP never comes — the setup waited for it with no end. An Echo still in fastboot a minute on now ends the step
  with what happened (before the setup's usual "unplug the power" tip, which would hide it): the unlock did not take, the bootloader is likely newer than the tool can unlock, and power-cycling the Echo
  boots the original software again, unharmed. Otherwise the wait for TWRP ends after 5 minutes. The Echo 2 (radar) unlock waits the same way now; the Dot 2's
  README names the bootloader build as a prerequisite of the unlock.

## 2026-10-22

- **Setup on Debian finds debugfs.** The guided setup said "missing: debugfs" at the build step even with e2fsprogs
  installed, and installing it again did not help: Debian keeps debugfs where only root looks for programs. The setup
  now looks there too.

## 2026-10-21

- **"Alexa, stop" can end listening** ("Stop after the wake word ends listening", settings page, Voice, off by
  default). An Echo that woke up by mistake listens for up to 15 s and Home Assistant carries out whatever it heard
  meanwhile. With this on, "Alexa, stop" right behind the wake word makes it stop listening at once, quietly, with
  nothing sent on to Home Assistant; said in one go, the Echo does not start at all. Timers, replies, Drop In calls and
  music on the Echo stop as before. "Alexa, stop the timer" then no longer reaches Home Assistant, and now and then a
  short command is heard as "stop" and has to be said again; off, a "stop" out of silence goes to Home Assistant as before.
- **"Alexa, stop" right after a reply, a ring or a busy Echo drops its run for good.** When the wake word cut a reply
  or a ringing timer (or a request still being worked on) and "stop" followed, the Echo ended the new run instead of
  cancelling it: the Echo could show listening again a moment later with its microphone closed, and once Home
  Assistant even carried out the "stop" (it paused music on another speaker). It is now cancelled the way the action
  button cancels a run. The same goes for "Alexa, stop" in a Drop In call, and for a "stop" that comes while several
  Echos are still deciding which one answers: the ring stopped, but the Echo then listened anyway.
- **Light only part of the ring, or each LED in a colour of its own** (issue #57). Home Assistant's "Light ring fill"
  (0 to 100 %) lights that share of the ring in the light's colour, for a timer's or a washing machine's progress or
  a battery level; it fills the way the volume does. "Light ring rotation" (Home Assistant's configuration, or the
  settings page under Lights) turns where it starts, LED by LED. The action "light_ring_leds" gives each LED a colour
  of its own. The effects (pulse, spin, blink, rainbow) stay for the whole ring. The Echo installs its ring animations
  once more at the first start with this version (a second without light). Thanks to abercrombie1892 for the idea.

## 2026-10-19

- **No more "Ja." when you pause after the wake word.** Since the Echo sends what you say right after the wake word,
  the last sound of a drawn-out "Alexaaa" could reach Home Assistant too: speech-to-text heard "Ja." or garbled the
  first word, and when you then paused, Home Assistant took the pause for the end of your command. The Echo now
  leaves out what still sounds like the end of the wake word (at most 0.2 s, never more than was cut before), so
  speaking straight on and pausing both work. Thanks to Thyraz for tracking it down.
- **Draw your own light ring animations** (issue #38). The settings page (Lights) has a designer for the ring: up
  to 100 designs of as many frames as you like, painted LED by LED or as a whole ring, with spinning, fading and
  breathing frames made for you, undo, and Amazon's animation as a start if you want it. For each of the Echo's own
  animations (listening, thinking, speaking, timers, volume, mute, Drop In calls and more) you pick Amazon's or one of
  yours; listening still turns towards whoever speaks. **Apply** puts them on the ring (a second without light).
  Thanks to Nils3311 for the idea and the first themes in PR #39, which this replaces.
  Designs go to your other Echos with "Copy models" (Echos), as models do; "Copy settings" copies what each
  animation shows, and takes the design along where that Echo has not got it.
- **Theme packs for the light ring**, as folders of your designs: a pack holds designs and which of them each
  animation shows, with a name, author and a packshot in one of four styles drawn from the pack itself. Open one to
  change its animations and designs in place; use it in a click, share it as a file, or copy it to your other Echos
  with "Copy models".
- **Home Assistant's light no longer shows through a Drop In call's ring.** The call rings, Amazon's talking ring and
  the identify rainbow play again and again until they are stopped, and the Echo took them for over after one pass: the
  light came back into their gaps, and with a long reply the ring could be stopped under the wrong name.
- **Setup from Windows for an unlocked Echo** (issue #49). `scripts/setup.sh` now runs in Git Bash on Windows when the
  Echo on USB is unlocked already (on adb, in Fire OS or TWRP): firmware, root, the release build, Wi-Fi and the
  install, as on Linux. The unlock itself still needs Linux on the PC; an Echo Dot 2 can reportedly also be unlocked
  from Windows with firebreak (not tried here). Windows needs Git for Windows (it brings `unzip` and `curl`), `adb`
  (platform-tools) and Python, and a commit GitHub has built. The guided setup no longer needs `sqlite3`.
- **An old TWRP is refused before the wipe** (issue #49). An Echo unlocked some other way can have a TWRP too old to
  install the firmware (3.2.3 on Echo Dot 2s sold unlocked by EchoLocal), which only showed after its data was wiped.
  The firmware step now checks for TWRP 3.7 or newer first and stops with nothing wiped.
- **Guided setup: the lock-down step passes the first time** (issue #49). Right after the Echo booted, "Locking down
  the Echo's internet access" failed once with nothing loaded and only passed when run again: closing adb over Wi-Fi
  restarts the Echo's adb, and that ended the USB connection running the step. Now the rules go up first, adb restarts
  after, and the step waits for it and checks the lock.
- **Guided setup: settings from a file are really checked** (issue #49). On an Echo that had been given settings before,
  "Settings taken" showed at once, from the earlier run. It now waits for the new ones.
- **Cloning on Windows gives working scripts** (issue #49). Git on Windows turned line ends into CRLF, which the scripts
  fail on; the repository now keeps LF everywhere. A clone made before needs `git rm --cached -r . && git reset --hard`
  once (or a fresh clone).
- **"Alexa, stop" pauses music** (issue #51). Music from Music Assistant or a phone over Bluetooth used to stop only if
  Home Assistant's speech-to-text caught the one word over the music, which often failed. The Echo now hears "stop"
  itself and pauses the music, as it already did for replies and timers; the action button resumes it. "Alexa, stop
  the timer" still goes to Home Assistant. A request that is only "stop" (after the wake word or the button) pauses the
  music the same way: with microWakeWord or Home Assistant's wake word, or when Amazon's model missed the word.
- **A wake word with nothing after it ends quietly** (issue #48). When the Echo woke and then heard no request (you
  said "Alexa" to another device, or it woke by mistake), the ring flashed the error animation. Now it simply goes
  dark, as a stock Echo does. Other errors still show it.
- **Wake words in Home Assistant** (issue #48). "Wake word events" (settings page, Features, off by default) adds the
  event entity "Wake word": `detected` each time the Echo takes its wake word, `near_miss` when Amazon's engine heard
  something close but did not wake, `answered_elsewhere` when another Echo or satellite answered it, and `no_speech`
  when it woke and nothing was said. Count wakes and false wakes per room in an automation.

## 2026-10-17

- **Separate volumes for music, voice replies, sounds, timers and calls** (issue #32). The settings page has a Volume card
  (Sound) that sets the Echo's volume from the browser. Switched to "Separate volumes", music, the voice assistant's
  replies, the Echo's own sounds (wake sound, buttons), timers and Drop In calls each get a volume of their own, 0 to
  100 %, set on the page or in Home Assistant ("Assistant volume", "Sounds volume", "Timer volume", "Call volume";
  the media player's volume is the music's). Each of them can instead follow the music volume, as far above or below
  it as you set ("Follow music"; calls start that way). 30 % sounds the same on every slider and as 30 % on a stock
  Echo. On a Bluetooth speaker they work too: the speaker is set to the loudest, the Echo turns the rest down, and the
  speaker's own buttons move the music. Speech sent to the media player (`tts.speak`, or `play_media` as an announcement) counts as a voice reply,
  not music. The volume buttons change what is playing: a call, a reply (the light ring shows it in cyan), a ringing
  timer, otherwise the music; one that follows the music moves with it.
- **Settings, pairings and the Home Assistant link survive a power cut.** A power cut or hard reset shortly after the
  Echo saved something could leave that file empty: every setting back to default, approved browsers, Bluetooth and
  Music Assistant pairings gone, or the Echo needing to be adopted in Home Assistant again. Every file the Echo keeps
  is now on the flash before it replaces the old one, so a reset leaves the old content or the new one.
- **The Echo follows Home Assistant's time zone.** It already took the time from Home Assistant; now the zone comes
  with it, on every model, and a change in Home Assistant reaches the Echo within 6 hours. The log stays in UTC.
- **The Echo answers ping from your network.** Home Assistant's Ping integration, a router's device list or a plain
  `ping` can now see whether it is up. Only from private addresses (192.168.x.x, 10.x.x.x and the like); nothing
  else gets in that did not before.
- **An Echo no longer stays off after an update or a restart from the settings page.** Restarting hassmic (after an
  update, a wake word engine change, models copied from another Echo) could lose the start when the old one was slow to
  stop: the Echo was then unreachable until something else started it, minutes later (seen on an Echo Show 8).
- **Guided setup: a second device on USB is named.** With a phone or another Echo plugged in next to the one being set
  up, adb cannot tell them apart and the setup waited for an Echo that was there. It now says which devices are on USB
  and goes on once only the Echo is left.
- **Auto brightness no longer fills Home Assistant's logbook.** With "LED auto brightness" on, "LED brightness" in
  Home Assistant followed the ring as the room got lighter or darker, an entry every few minutes. The light sensor is
  now a feature on the settings page, "Light sensor", off by default: off, there is no "Illuminance" entity and Home
  Assistant only sees brightness levels you set; the ring still dims with the room as before. Switch it on to get
  "Illuminance" back for your automations.
- **Settings page: models from Amazon.** The warnings about missing wake words and whisper detection pointed to
  `scripts/artifacts.sh` on the PC, which needs the Echo on USB. They now point to "Download from Amazon" on the
  page (a link to it), which every model can do now.
- **Guided setup, clearer where people got stuck:**
  - The download step says plainly that you download the three files yourself in your browser and save them to
    `~/Downloads`. The script only waits for them; people waited an hour thinking it was downloading.
  - Echo Dot 3: the unlock step gives the order that works: USB cable into the PC, hold the action button, plug in the
    Echo's power adapter. USB alone does not power it, so before nothing happened.
  - The Wi-Fi step shows the networks the Echo hears and catches a name that is off in capitals only (Wi-Fi names
    are case-sensitive). Joining now fails with a reason (no such network, wrong password) instead of going on with
    an Echo that never connected.
  - Setup refuses to run on macOS or as root/sudo, and warns in a virtual machine or WSL: the unlock needs Linux on
    the PC itself (a live USB stick works). On macOS it failed with `status is 1d1a`.

- **The right Echo answers again after another one crashed.** An Echo that died hard (a freeze and a power cycle, say)
  could come back having lost a small file it keeps for the arbitration. It then numbered its messages from the start
  again, and the other Echos took them all for old ones replayed and ignored them, until they were restarted
  themselves. Nothing looked wrong on the crashed Echo, but another Echo answered every wake word the two heard, even
  standing right next to the crashed one (issue #36). The file now survives a crash, and an Echo that lost it anyway
  comes back as a new member, which the others count at once. If an Echo answered for the wrong room since a crash:
  restart the Echo that answers.
- **The ring no longer stays on after a very short reply.** When a reply was over within a few hundredths of a second
  (a short confirmation, an empty answer), the talking ring could stay lit on an idle Echo until the next wake word.
  The Echo told the ring "on" and "off" at practically the same moment, and "off" sometimes arrived first. The ring now
  gets its changes one after the other, in order.
- **The ring's brightness ends where the slider stops.** Dragging the LED brightness in Home Assistant could leave the
  ring at a level from along the way, for the same reason; brightness changes now take their turn in that same order.
- **Wake sound removal (experimental) on the Echo Show.** The Show starts a sound after a pause 0.35 s late (below); the
  removal now looks for the sound's echo from where it really starts, and no longer misses it.

## 2026-10-16

- **Echo Show 8 (2nd gen): Wi-Fi motion.** The motion sensor (experimental, off by default) now works on the Show 8 2nd gen:
  our module reads the level of every frame from the router out of its Wi-Fi driver, as on the Echo Dot 3 and the Show 5.
  The guided setup downloads the kernel sources and compiler the module is built with.
- **Echo Show 8 (2nd gen): the guided setup unlocks and roots it.** `scripts/setup.sh athena` asks for saiyaku-athena and its
  boot-root zip (picked up from ~/Downloads), runs the unlocker with its output on the console, flashes the root zip in TWRP
  and goes on with the install, as it does for the 1st gen.

## 2026-10-15

- **Echo Show 8 (2nd gen): the mute button is answered at once.** Amazon's driver holds the microphone switch for one
  second before it reports it, so the red band and the "mics off" sound came 1.2 s after the press (the red light under the
  button was instant). hassmic now reacts to the press itself and lets the switch confirm it.
- **Echo Show 8 (2nd gen): speaker gain.** The "Speaker gain" setting goes up to +9 dB: music at full volume was quiet, and
  past that point bass-heavy music distorts.

- **Echo Show 8 (1st gen): a factory reset forgets a Wi-Fi network that Amazon's own setup saved.** Android 7 only lets the
  network's creator or the system user remove it, so the reset reported "forgotten" and the Echo stayed on the network (and
  never offered setup from a phone). The reset now removes it as the system user, and says so when one cannot be removed.
- **Echo Show 8 (1st gen): the speaker gain can go below stock.** The Show 8 is loud at its stock level with music at
  full volume, so the settings page's speaker gain goes down to -6 dB here (the Dots only go up). About -2 dB suited
  the first one tried.

## 2026-10-14

- **Echo Show 8 2nd gen (`athena`): first support, installed on one.** Android 9 and a different chip from the other Shows,
  so it has its own page ([`devices/athena`](devices/athena/README.md)). The Echo has to be unlocked and rooted first (the
  saiyaku-athena unlock and its root zip, not part of this project); the install then needs no flashing from the PC. On
  the first Echo hassmic is up 22 seconds after the kernel starts, hears the wake word, takes the volume keys and the mic
  mute switch and draws the light band, and Home Assistant adopts it and gets spoken answers. Bluetooth (Home Assistant's
  proxy, music from a phone), Music Assistant over Sendspin, the settings page's "Switch network" and the setup's Wi-Fi
  join, and "Download from Amazon" work, and the room's light is reported in lux, computed from the sensor's raw colour
  channels (within about 10 % of Amazon's own value in steady light; under a bright lamp held close it still jumps about).
  Not checked yet: the speaker's volume curve.

## 2026-10-13

- **No more freezing at the first wake word after 18.6 hours** (issue #31). Since 2026-10-12, an Echo whose
  satellite had been running for more than 18.6 hours hung at its next wake word: the listening ring stayed on, the
  buttons did nothing, and only a power cycle helped. A counter of the microphone's bytes ran past what 32 bits hold,
  and the audio from just after the wake word was then sent to Home Assistant over and over, for ever. Fixed.
- **Replies and announcements from a Home Assistant that serves HTTPS itself** (issue #30). With its own certificate
  (`ssl_certificate` under `http:`), Home Assistant gives the Echo https:// addresses for spoken replies,
  announcements and media, and the Echo only played http://: it stayed silent and the log said "only http:// is
  supported". It now plays https:// too, and follows redirects there. The certificate is checked as a browser would:
  a self-signed one, or one that does not name the address Home Assistant gives out, is refused, and the log says so.
  "Check HTTPS certificates" on the settings page (System) switched off accepts any certificate.
- **Echo Show 8 (1st gen, `crown`): first support.** The Show 8 is the same platform as the Show 5, and a trial run on one
  (hassmic started from the PC, nothing installed) heard the wake word, took the volume keys and the mic mute switch,
  and drew the light band. Installed on a real one since: Home Assistant, voice questions, buttons and the band work. Wi-Fi motion
  works on it (the sensor needs a minute after switching on to settle) and "Download from Amazon" works (its attestation
  service is DHA version 2, drvV3 in Amazon's Java MAP). Bluetooth works once the install has taken Android's own stack off
  (Home Assistant's Bluetooth proxy, playing from a phone).
- **No "mics off" announcement when hassmic starts** (every model; found on the Echo Show 8). After a power cycle the
  Show 8's microphone switch reads "off" until the Echo lets it go by itself, 20 to 80 seconds in; hassmic, up by then
  in two boots of three, caught it and said "mics off" to someone who had not pressed anything. A switch that is off when hassmic
  starts is now shown quietly (ring, Home Assistant), on every Echo; pressing it is announced as before.
- **Echo Show (Show 5, Show 8): the wake sound was not heard, a fix to confirm.** The speaker opens a fraction of a second after a sound
  starts, which swallowed the 0.12 s blip; a sound that follows a pause now starts 0.35 s late so it is not clipped.
- **Echo Show 5: Download from Amazon.** The settings page's "Download from Amazon" now works on the Show: sign in
  with the code, then wake words and the newer sound detection model download and install as on the Dots. Amazon
  does not hand out the whisper detection model for the Show; copy it from a Dot that has it ("Copy models").
- **Echo Show 5: Illuminance and Wi-Fi motion.** Home Assistant now gets the Show's light sensor as "Illuminance", in
  lux, the same value Android's own sensor reports. Wi-Fi motion (experimental, off by default) works on the Show as
  on the Dots: switch it on in the settings page. The Show has no "LED auto brightness": the band's brightness is the
  one you set.
- **A satellite start that could hang for good.** On the Echo Show 5, removing a Wi-Fi Direct group left from setup
  could wait forever, and hassmic did not start after an update. It now gives up after 5 seconds.
- **Echo Show 5: the light ring as a band on the screen.** The Show has no light ring, so it shows the ring's lights
  along the bottom edge of the screen, as Alexa showed hers: listening, thinking, replies, mic off, volume, timers,
  calls, Do not disturb, setup, and Home Assistant's "Light ring" light, which the Show now has too. They look and move
  like a Dot's ring laid out in a line. The band keeps its own strip of the screen (10 pixels; "Light band" on the
  settings page, 0 gives it back), so an app shown on the screen is never covered. The screen wakes for the band and
  sleeps again 5 minutes later (1 minute with the mic off or in Do not disturb), as stock's did. The band does not
  point towards the talker: the Show's two microphones do not tell directions apart well enough (Alexa's bar never
  turned either).
- **Bluetooth speakers no longer start at full volume.** A speaker that sets its own volume (most do) used to take
  over with whatever level it reported, so a PC reporting 100 % played the Echo at full volume, and the Echo's buttons
  then only asked it to turn down, which it ignored. The Echo now gives the speaker its own volume first, and lets the
  speaker set the volume only once it has confirmed that level; otherwise the Echo keeps setting it, as stock did.
- **Echo Show 5 runs.** Tried on a tester's Show, installed with `scripts/setup.sh checkers`: the wake word, Home
  Assistant's voice assistant, wake word arbitration with the other Echos, Music Assistant, phones playing to it,
  playing on a Bluetooth speaker, the volume keys, the mute button and the settings page.
- **Echo Show 5: plays on a Bluetooth speaker.** "Play on a Bluetooth speaker" now works on the Show as on the Dots:
  replies, sounds and music go to the speaker, and back to the Show when you switch it off, without a pop. Tried with
  a PC as the speaker.
- **Echo Show 5: Amazon's software is gone, not just switched off.** The install now takes every Amazon app off the
  Show's system partition (Alexa, Amazon's screen, its pull-down menu, Settings, cloud and telemetry services: 120 of
  them), keeping only the few shared libraries Android itself loads; uninstalling puts them back. Disabling them was
  not enough: Fire OS turned some back on at every boot, and the rest kept crashing with "has stopped" dialogs. The
  screen stays black, the Show boots in about 30 s, and half its memory is free. The volume is now Android's own,
  the volume ticks come at once, and the mute sound plays at your volume (Amazon's own copy of it no longer plays
  over it at full volume). The boot chime is gone, and adb over Wi-Fi no longer opens at every boot.
- **Echo Show 5: Speaker gain, and the newest features.** The Show 5 gets the "Speaker gain" setting: its
  codec's line out in front of the speaker amplifier, up to +12 dB in steps of 1.5 dB (each whole dB you pick goes to
  the nearest step). Pressing both volume keys together for the missing action button no longer also moves the
  volume, and fires Home Assistant's Button event `action`. Wake sound removal knows when the Show's sound starts.
- **Music Assistant finds the Echo again after it restarts** (issue #28). Music Assistant looks for Sendspin players
  only when it starts, and on some networks (the Echo on a VLAN of its own, say) that search does not reach the Echo;
  only the Echo's own announcement at boot did. After Music Assistant or Home Assistant restarted, the Echo stayed
  unavailable as a player until it was rebooted. Now, while Music Assistant played on it before and is not connected,
  the Echo announces itself again: after 30 s, then less and less often, at most every 30 min. The guide also says
  how to add the Echo to Music Assistant by hand: `<echo-ip>:28928`, with the port.

## 2026-10-12

- **The right Echo answers on slower Wi-Fi** (issue #27). Echos that heard the same wake word used to wait a fixed
  0.2 s for each other's claims. On many networks the nearer Echo's claim arrives later than that: routers hold
  broadcasts back for devices saving power, and repeaters add more. The farther Echo then answered. Now each Echo
  learns how late the other Echos' claims reach it, and waits just long enough for nearly all of them (0.15 to 1 s;
  0.5 s until it has measured ten). It also stops waiting as soon as every other Echo's claim is in. When all your
  Echos heard you, the answer now comes sooner than before; the full wait applies only when one of them did not. The
  settings page shows what an Echo waits and what it measured. "Learn the wait from the network" off gives a fixed
  "Arbitration window" instead (the Kiosk Satellite window, now used by both modes, up to 1 s). The log shows how late
  each claim came, so you can see what your network takes.
- **Echos with different wake word sets settle who answers again.** An Echo that has two "Echo" sets installed (German
  and English, say) calls them "Echo (de-DE)" and "Echo (en-US)" in Home Assistant, and it also claimed the wake word
  under that name; an Echo with only one set claimed it as "Echo". The two took each other's claims for a different
  word, and both answered. They now claim the word itself.
- **Wake sound removal (experimental).** Speaking straight on after the wake word means talking over the tone, and
  part of it stays in what Home Assistant hears: Amazon's echo canceller leaves it about as loud as a quiet voice. A
  new experimental card under Features on the settings page, "Wake sound removal" (off by default), finds the tone in the microphone each time
  and turns down only its few pitches; your words around it stay. While the tone plays, the Echo holds back what it
  hears and then sends it all at once, so the first second of a command reaches Home Assistant about a second later.
- **No pause needed after the wake word.** What you say right after the wake word reaches Home Assistant whole, so
  "Alexa turn on the light" in one breath works. Before, the first 120-180 ms after the word were lost: the time the
  wake word engine takes to be sure, plus a moment until the microphone stream started. Echos settling who answers
  lost that plus their 200 ms wait (400 ms with Kiosk Satellite). The sound and the ring still come at the same time
  as before; you no longer need to wait for them.
- **The light ring in Home Assistant** (issue #26). A new light, "Light ring", with colour, brightness and the effects
  pulse, spin, blink and rainbow, for automations: a flash when the doorbell rings, an orange pulse for the kitchen
  timer, a colour for the alarm system. Whatever the Echo shows itself comes first (listening, a reply, the
  microphones off, a ringing timer, the volume); the colour comes back afterwards. The ring shows the nearest of 147
  colours (24 hues, full or pale, and white, each at three brightness levels): close, not exact. The first start of
  this version writes the ring's new animations to the Echo's system partition and restarts Amazon's ring service:
  a second without light, once.
- **The listening ring points at you even while a timer rings.** Amazon's ring only turns towards whoever spoke while
  nothing else shows; hassmic now plays it already turned, so it points at you over a ringing timer too, and follows
  you if you move while you speak.
- **The buttons in Home Assistant** (issue #26). A new event entity "Button" fires for every press of the action
  button and the volume buttons, and once when one of them is held for a second: `action`, `volume_up`,
  `volume_down`, `action_hold`, `volume_up_hold`, `volume_down_hold`. What the buttons do on the Echo stays as it was,
  so an automation can add to it: stop an alarm clock that Home Assistant plays, skip a track with volume up held.
  Holding both volume buttons (pairing Echos) fires nothing, nor does the press that lets a browser into the settings
  page.
- **Holding a button belongs to Home Assistant.** A volume button changes the volume when you let go of it within a
  second, as the action button already acted; held longer, it only fires its `_hold` event in Home Assistant, so
  "hold volume up for the next track" no longer changes the volume too. Holding both to pair Echos changes nothing
  either. The action button held 10 s still resets the Echo.
- **Switching between the Bluetooth speaker and the Echo is quick now** (issue #25). With the new setting "Stay
  connected when off" in the Bluetooth speaker's card on the settings page, switching "Play on Bluetooth speaker" off
  keeps the speaker connected and only moves the sound back to the Echo, so switching it on again takes well under a
  second instead of a reconnect: handy for an automation that puts music on the speaker and leaves replies on the
  Echo. Home Assistant's "Bluetooth speaker" shows "Standing by" meanwhile. With "Keep the speaker awake" on as well,
  the Echo keeps the speaker from switching itself off. The page lists what it costs and asks before it goes on.
- **Bluetooth speakers that cut off the first word** (issue #24). Some speakers switch their amplifier off while the
  Echo is silent and need about half a second to switch it on again, so every reply lost its start. A new setting in
  the Bluetooth speaker's card on the settings page, "Keep the speaker awake" (off by default), plays a tone too low
  and too quiet to hear under everything, so the speaker never hears silence. Turn it on only if your speaker cuts off
  replies.

## 2026-10-11

- **The light ring points at you again while you speak** (issue #22). After the wake word, Amazon's software turns
  the light-blue part of the ring towards whoever spoke; with hassmic it always sat in the same place. Amazon's ring
  only turns two of its animations, and hassmic used a look-alike that never turns. As with Amazon's software, the
  ring does not turn while a second light shows at the same time (a ringing timer, say).
- **"LED brightness" in Home Assistant no longer flips back and forth.** With auto brightness on, in light near the
  edge between two levels (at dusk, say), it went 11 %, 12 %, 11 %, 12 % every few seconds and filled the Echo's
  history. The ring itself does the same under Amazon's software too, one step at a time. Home Assistant now hears of
  a change of 2 % or more at once, of a smaller one after 5 minutes. A level you set yourself shows at once as before.
- **Stopping a ringing timer** (issue #21). Home Assistant has a "Stop ringing" button for each Echo: it ends a
  ringing timer and leaves music alone, where the media player's stop also stopped what the media player played. And a
  new setting under Voice on the settings page, "Any request stops a ringing timer" (off by default): with it on,
  whatever you say after the wake word while a timer rings stops it, in any language. Until now only "stop" did, and
  "Alexa, stop the timer" went to Home Assistant, which answered that no timer was running while it rang on. The
  cost: a question asked during a ring only stops it.
- **"Drop in kitchen" calls again, and says when it cannot.** The Drop In blueprint looked for Echos made by
  "Amazon", but Home Assistant lists them as made by "hassmic", so it found none and Home Assistant just said "Done"
  (issue #20). It now finds the Echos by what they report themselves, so an Echo or its entities renamed in Home
  Assistant are found all the same, and it never just says "Done" any more. It only says "Calling kitchen" once the
  call has started; otherwise it says why not: no Echo there, the other Echo has do not disturb on, its microphones
  off, Drop In off, is in a call, did not answer, is offline or not in the same Echo network, the one you spoke to
  is in a call, has its microphones off or cannot drop in, or you named the room it is in. "Drop in upstairs" calls
  the Echo on that floor, area aliases count, and "Küche", "Kueche" and "kuche" are the same room. Re-import the
  blueprint (its menu › Re-import) to get this; replies you set yourself keep working, with English for the new ones
  until you add yours. It works with every hassmic version that has Drop In; with this update on the Echo you speak
  to, the answer comes at once and says exactly why (before, a refused call was told after 4 s as "did not go
  through"), and that Echo plays its end-of-call sound when a call does not go through: a call the other Echo
  refused at once made no sound before, and Home Assistant never saw it start. For your own automations, each Echo
  now has "Drop In result", which says what came of every call it makes.
- **Speaker gain on the Echo Dot 2, Echo 2 and Echo Dot 3** (issue #19): a new setting under Sound on the settings
  page makes the Echo's own speaker louder at every volume, for an Echo that is too quiet at full volume: by up to
  12 dB on an Echo Dot 2 or Echo 2, by up to 2 dB on an Echo Dot 3, whose amplifier has no more above where Amazon
  leaves it. It stays set across restarts of the Echo's audio. +6 dB was heard loud and clean on an Echo Dot 2; go up
  a step at a time and turn it down at the first crackle, as a speaker played distorted for long can be damaged.
- **Home Assistant finds an Echo again after it gets a new IP address** (issue #17), within about two minutes and
  without a power cycle. Home Assistant ignores an Echo's "I am here now" while it still holds the old connection,
  which it does for 90 s, so the one announcement the Echo made at its new address went unheard, and often the one
  after a power cycle too. The Echo now announces itself again while Home Assistant stays away, and drops the dead
  connection at once, which had kept the voice assistant from working when Home Assistant came back.
- **Restart** on the settings page (System) and a "Restart" button in Home Assistant: the Echo reboots, as a power
  cycle does, without anything forgotten. For an Echo that is stuck while its page still answers.
- **Two wake words at once.** Home Assistant's "Wake word 2" select now works on the Echo: the word picked there is
  listened for beside the first one, and it starts "Assistant 2", so each word can have its own assistant (as on a
  Voice PE). Before, a pick there did not stick, or replaced the first word. A second word costs the Echo
  some processing power all the time; leave "Wake word 2" at "No wake word" to keep one.
- **Online updates check that the Echo reaches GitHub before they switch on.** The Echo downloads updates itself, so
  one on a network kept off the internet never got any and only said so in the firmware entity's summary. Picking a
  channel on the settings page now first asks GitHub; if it does not answer, the channel stays off and the page shows
  a warning with what failed and what the Echo needs to reach. An imported settings file goes the same way. The
  setting's description now says this too.

## 2026-10-10

- **Echo Show 5: the rooted boot image works on a real Show.** The first try on a tester's Echo Show 5 found three
  things the PC could not show, now fixed in the installer and the guided setup: Amazon's own adb service never runs
  as root, so the boot image now carries the one from the XDA thread's image (the setup takes it out of that
  thread's `boot-root.zip`); amonet's bootloader sends every restart back to fastboot after a flash, so the
  installer now boots with `fastboot continue` and says when to replug the power; and the setup now shows what that
  step prints instead of only a spinner. With these the Show boots with a root adb shell. The README also says how
  to get the firmware into TWRP whole (TWRP lists only `.zip` files, and a copy cut short reads as corrupt).

- **The Echo Show 5 joins Wi-Fi without its touchscreen, and has a guided setup.** Joining goes through Android's
  own WifiService, from root's shell: `scripts/device/wifictl.dex`, assembled by `tools/mkwifictl.py` from nothing
  but that file, makes the same `WifiConfiguration` call stock's own setup apps make. The guided setup
  (`scripts/setup.sh checkers`) runs amonet's unlock, flashes the rooted boot image, locks the egress down and joins
  Wi-Fi before the Echo ever talks to Amazon, and installs — the steps it walks in a dry run, nothing of it tried on
  a real Show yet. The settings page's network switch works the same way (checked end to end against a fake
  WifiService in `tests/fake_web_wifi.py`; the dex itself is type-checked, checked against the firmware's own
  framework and run in an interpreter on the PC); a mistyped password for a network the Echo knows puts its old one
  back. Updates carry the dex too, and checkers is back in CI's build matrix. Details:
  [devices/checkers](devices/checkers/README.md).
- **Bluetooth on the Echo 2.** It installed with Bluetooth off, untried, yet Home Assistant still showed the
  Bluetooth switches: "Bluetooth pairing" turned itself off again at once. Bluetooth is now on for new installs (a
  phone pairs and plays, aptX HD included). On an Echo 2 installed before, take `-B` out of `ARGS` in
  `/data/local/hassmic/hassmic.conf`. Wherever Bluetooth is off with `-B`, Home Assistant and the settings page no
  longer show its switches.
- Guided setup, Echo 2 and Echo Dot 2: the firmware now goes into both slots. The setup switched the slot back by
  hand after the first install, so the second one landed in the same slot and the other kept whatever ran before.
  Unpacking the firmware no longer stops with "Dangerous symbolic link path" on newer 7-Zip: it uses `debugfs`, as
  the Echo Dot 3 always did.

## 2026-10-09

- **The guided setup no longer downloads models.** Its last step, which put the Echo online as a stock Alexa and
  had you register it in the Alexa app, is gone: "Download from Amazon" on the settings page does it on every Echo
  now, without the app and without the risk of a factory reset from deregistering too early. `scripts/artifacts.sh`
  still works, as a fallback, and says so with a warning first.
- Guided setup: questions end in a colon, so it is clear an answer is wanted, and waiting for the Echo says that a
  rainbow ring is fastboot on an unlocked Echo (`fastboot reboot` gets it out), not the green of a stock one.
- **"Download from Amazon" works on the Echo Dot 3.** Its newer way of proving to Amazon who it is — a certificate
  its hardware key had Amazon sign — is worked out now, so the Dot 3 itself signs in on the settings page and
  downloads wake words, whisper detection and the newer sound detection model, like the other Echos always could.
  Tried against the real Amazon: the Dot 3 signs in and the sets arrive (where Amazon has none for a language,
  "Ziggy" in Italian say, the page says so).

## 2026-10-08

- **Settings page: a feature's own choices sit under their text**, as "Drop In answers" does. Beside it, in a card half
  the page wide, the text shrank to a column one word wide.
- **Only the wake word you picked wakes the Echo.** Amazon's English (US) "Computer", "Amazon" and "Ziggy" are one
  model that knows all three (and "Hey Disney"): with "Computer" picked, "Amazon" and "Ziggy" woke the Echo too.
- **The same wake word in two languages shows twice in Home Assistant**, as "Computer (de-DE)" and "Computer (en-US)".
  Home Assistant listed it once before and always picked the same one of the two.
- **"Download from Amazon" takes the English (US) wake words, and German "Alexa".** Amazon now packs these sets in
  deeper folders and with more files than the Echo accepted, so the settings page refused each one ("a file this Echo
  cannot keep"). Copying such a set from one Echo to another on the settings page works now too.
- **Drop In between Echos.** One Echo calls another of yours and both hear each other, as Alexa's Drop In did. Start it
  from Home Assistant (the new action `drop_in` with the other Echo's name), by voice with the new "Echo Drop In"
  blueprint ("drop in kitchen", "verbinde mit der Küche"; English, German, French, Spanish, Italian and Dutch,
  replies in your assistant's language), or with the Drop In button of an Echo on the settings page. The
  Echo called chimes and connects at once, or, set to answer with the button, rings until someone presses it. End it
  with the action button, "Alexa, stop" or "hang up" / "auflegen", on either Echo, or with "End Drop In" in Home
  Assistant. The ring shows Alexa's call animations, you hear Alexa's call sounds, and music pauses. Only Echos of your own Echo
  network can call; do not disturb or the microphones off refuse a call. "Drop In" on the settings page (on by
  default) switches it off; Home Assistant then shows none of it. The sound goes straight between the two Echos,
  encrypted. The Echo's own echo canceller runs in its call mode; on top of it the Echo sends its room only while
  someone there talks, so the other side does not hear itself back.
- **Home Assistant sees a ringing timer.** "Timer ringing" is on while a finished timer rings and goes off when someone
  stops it or it rings out; "Ringing timers" (diagnostic) names them, or gives the length of a timer without a name
  ("5 min"). An automation can now announce a timer on every Echo, or send a phone alert, when nobody stopped it
  within a minute. The media player's stop in Home Assistant stops the ring too.
- **The ring lights up while a timer rings**, with the Echo's own cyan timer animation, as on a stock Echo; it went
  dark before (the Echo was asked for an animation its light controller does not know). It goes off when the timer
  is stopped. Listening and replies show over it, and it comes back after them.
- **A ringing timer only stops when you say so**: "Alexa, stop", the action button, or the media player's stop in Home
  Assistant. The wake word alone no longer stops it: as on a stock Echo, the ring pauses while you talk ("Alexa, how
  long is left on the pasta timer?") and goes on afterwards. With microWakeWord or Home Assistant's wake word, which
  have no "stop" of their own, saying "stop" after the wake word does it.
- **How long a timer rings is a setting**: "Timers ring for" on the settings page, 10 s to 10 minutes or until stopped
  (default 1 minute, as before).
- **Several timers at once ring properly.** A second timer that finishes while one rings gives the ring its full time
  again, and one stop ends them all. Cancelling a timer that is
  still running no longer silences one that is ringing.

## 2026-10-07

 **An Echo names itself after its model, and you name it in Home Assistant.** Every Echo is now called after its
  model and the end of its Wi-Fi MAC address, like the Voice PE: "Echo Dot 3 5695c4", "Echo Dot 2 …", "Echo 2 …"
  (host name `echo-dot-3-5695c4.local`). The name is no longer asked for at the install, set in `hassmic.conf` or
  changed on the settings page; give the Echo the name you want in Home Assistant, when you add it or on its device
  page. **On updating**, an Echo installed with a name of its own changes to this one: Home Assistant keeps the
  device and its entities (it knows the Echo by its MAC address), shows the name you gave it there, and keeps the old
  entity ids; the host name changes. Music Assistant and Bluetooth show the new name.
- **Set an Echo up from your phone, over Bluetooth.** A freshly installed Echo, or one reset, waits with the orange setup
  spinner on its ring, as a stock Echo does. The Home Assistant app finds it (Settings, Devices & services, Add device), or Home Assistant itself when one
  of its Bluetooth adapters or proxies is near: pick your Wi-Fi network, type its password, and the Echo joins it; Home
  Assistant then discovers it as usual. The spinner stays until Home Assistant has added the Echo. This is Improv Wi-Fi,
  the same way ESPHome devices and the Voice PE are set up. An Echo that is already set up but has had no network for
  10 minutes (new router, moved house) offers the same, but only after a press of its action button, so nobody nearby
  can move it onto their own network. Be aware
  that the Wi-Fi password travels unencrypted over Bluetooth, as with every Improv device.
- **Factory reset.** Hold the action button for 10 seconds (at 5 seconds the ring warns; let go then and nothing
  happens), press "Factory reset" in Home Assistant (a configuration button), or use "Factory reset…" on the settings
  page. The Echo forgets its settings, name, Home Assistant's key, approved browsers, Bluetooth pairings and its Wi-Fi
  networks, and starts the setup above again. The install itself, Amazon's extra models and `hassmic.conf` stay.
- **Groundwork for the Echo Show 5 (1st gen).** Its firmware is worked out and hassmic builds for it, but it cannot
  be installed yet: the unlock exists (amonet-checkers), the installation for this model is still to be written.
- **Echo Show 5 (1st gen): an installer, not tried on a device yet.** `scripts/install-system.sh` builds a root boot
  image from the firmware's own, flashes it in amonet's fastboot and installs hassmic as on the Dots; Alexa's apps are
  switched off and come back when hassmic's config is removed. Details and risks:
  [devices/checkers](devices/checkers/README.md).
- **The settings page is tidier and about a third shorter.** The wake word engines and the arbitration modes are small
  cards to pick from; below them, one click opens a table that compares them question by question (how well it hears,
  which wake words, "<wake word>, stop", several Echos, ...), the current choice highlighted. "System" is two cards of
  short rows (name, Wi-Fi, updates; settings file, log, debug access); renaming and the log open in a dialog, the log
  as big as the window. "Download from Amazon" moved to the "Wake word" section. An Echo alone on the network no
  longer shows two empty copy cards, only a note that other Echos show up by themselves. Fixed: the menu marked the
  wrong section while scrolling, the "Name" card said "Give it a name." although it had one, ticking "Also change the
  node name" showed "[object HTMLSpanElement]" instead of the new node name, and on phones an invisible "Saved" pushed
  help texts a line down.
- **"Copy models" lists only what there is to copy.** A model every Echo listed already has, in the same version, is
  left out; a line says how many, and with nothing left to copy the card says so.
- **The wake word can be left to Home Assistant from the settings page.** "Wake word" has a third choice, Home
  Assistant: the Echo streams its microphone and your Home Assistant listens, with openWakeWord and your own wake words
  if you like. A dialog shows what to set up there (the openWakeWord app, your models, "Add streaming wake word" on
  the assistant) and what it costs (a constant stream, no arbitration between Echos, nothing while Home Assistant is
  down). When Home Assistant hears the wake word, the Echo now plays its wake sound and keeps the talker's voice
  clean to the end of the command, as with its own wake word; before, with `-w remote`, it did neither. Switching
  restarts the satellite for a few seconds.
- **Experimental: other wake words with microWakeWord.** The settings page has a new "Wake word" section: besides
  Amazon's engine (still the default) the Echo can listen with microWakeWord, the wake word engine of ESPHome's voice
  satellites, for "Okay Nabu", "Hey Jarvis", "Hey Mycroft" or any wake word someone trained a model for. Be aware that
  **it detects wake words significantly worse** than Amazon's engine (more missed, more false wakes), and that while it
  is on "<wake word>, stop", the extra sensitivity while the Echo plays music, Amazon's wake words and the front end's
  scores in wake word arbitration are gone; the page lists all of it before you switch. Sound and whisper detection
  keep working. When you pick microWakeWord, the page's dialog lists what you lose and, if the Echo has no model yet,
  asks which wake word to start with (one of ESPHome's or your own `.tflite` and `.json` files) before it switches.
  While it is on, the same section manages the models: add more, rename them, tune their threshold, download and
  delete them; "Copy models" puts them on your other Echos.
  Home Assistant's wake word select offers them while microWakeWord is on.
- **Echos no longer drop out of each other's network every minute or so.** Each Echo's Bluetooth scanning (for Home
  Assistant's Bluetooth proxy) shares the antenna with Wi-Fi, and messages broadcast while it scans are lost: 10 to 30
  out of every 100. One Echo's announcements kept arriving only every third time, so it vanished from the Echos list
  and from wake word arbitration for about 15 seconds every minute and a half. Echos now announce themselves at random
  intervals and count each other as gone only after five minutes without one, and an Echo that heard the wake
  word pauses its Bluetooth scan for a second, so the other Echos' claims reach it. Update every Echo.
- **Switch the Echo to another Wi-Fi network from its settings page.** "Wi-Fi" in the System section shows the
  network the Echo is on; "Switch network…" lists the networks it sees, or takes a name you type (for a hidden one).
  You can type the password before or after you pick the network. The Echo keeps the new network only once it is on it
  with an address and the router answers; otherwise it goes back to the old one within a minute or two, and the page
  says why (wrong password, not found, no address). Once the new network works, the Echo forgets the old ones. It will most likely get a new IP address: look it up afterwards
  under "Web UI address" on the Echo's device page in Home Assistant. The password travels encrypted, and the Echo
  stores only the key derived from it. WPA2 and open networks; WPA3-only networks are shown but cannot be joined.
- **The guided setup takes an Echo that is rooted already** (issue #10). If the Echo on USB has root adb and the
  firmware this project needs, `scripts/setup.sh` offers to keep it as it is: it skips the unlock, the firmware and the
  root steps (which would have wiped it), and asks only for `boot-root.zip`, plus the firmware when it has to build.
  `--from <step>` skips to any step by hand.
- **A broken unpack of boot-root no longer slips through.** If unzipping `boot-root.zip` broke off, a retry of the
  build step passed anyway, and the install failed later with only "sepolicy patch failed". The setup now checks for
  the tool it needs from it, unpacks it again in the install step if it is missing, and the installer says what is
  missing before it touches the Echo.
- **Echos hand each other the arbitration key through Home Assistant again, whatever the devices are called there.**
  Home Assistant now puts the room (and more) into entity ids, so the Echos could no longer find each other's
  "Arbitration handoff" entity and needed "Allow the device to perform Home Assistant actions" or the volume keys.
  Each Echo now reports a tag named after its key as scanned, and an Echo hands the key only to one whose tag Home
  Assistant confirms: nothing to set up, no permission needed. Home Assistant's Tags list shows one "Tag hassmic_…" per
  Echo. The "Arbitration handoff" entity is gone. Echos on older versions still join through the action or the
  volume keys.
- **Fewer wake words answered by the wrong Echo.** An Echo could miss another's report of how well it heard the wake
  word, answer itself, and have Home Assistant turn the better one away. The reports now go out three times, spread
  over 80 ms, instead of twice at the same moment.
- **An Echo set to another wake word no longer falls back to "Alexa" at every start.** Wake word sets installed by
  hand were renamed to their full name (`echo-de` to `echo-de-DE`), but `-m` in `hassmic.conf` still named the old
  folder: Home Assistant then offered "Echo" twice, and the one that no longer existed loaded nothing. The old name
  now finds the renamed set, and a missing `-m` model is no longer offered.
- **No false warning about the key on the settings page.** The Echos section warned that the "Arbitration handoff"
  entity was missing even when the Echo had its key. It now warns only when an Echo has waited over a minute for one.

## 2026-10-06

- **Identify.** Which Echo is which: "Identify" (at the top of the settings page, beside each logged-in Echo in the
  Echos list, and as a button in Home Assistant) turns the ring into a rainbow for 10 seconds and plays the sound a stock
  Echo makes in setup. Pressing it again makes it last longer.
- **Rename an Echo on the settings page** ("Name", in System). Only the name you see changes, unless you also tick
  "Also change the node name"; the page then lists what that costs in Home Assistant and for wake word arbitration and
  asks you to confirm. The satellite restarts once for a few seconds.
- **"Bluetooth speaker delay" on the settings page**, in the "Play on a Bluetooth speaker" card, next to Home
  Assistant's number.
- **"Copy settings" follows changes right away.** A setting changed on this Echo (on the page or from Home
  Assistant) used to be offered with its old value until the next look at the other Echos; the list now shows the
  current value within seconds.
- **The settings page keeps up with the Echo.** After an update it reloads itself instead of showing the old version;
  "Log in" on another Echo stays at "Press its button…" while it waits (it used to turn back after a few seconds and
  could start a second wait); "How Echos get the key" no longer folds itself shut while you read it; a renamed Echo
  shows its new name; in the copy and download lists the keyboard focus stays where it was when the list refreshes.
- **The Echo knows the time again.** Behind the firewall nothing set its clock any more (the Echo Dot 2 was 23 hours
  behind); it now takes the time from Home Assistant, over the encrypted link, when it connects and every 6 hours.
- **Times in the log.** Every line of the log says when it was written; the settings page shows the times in your time
  zone. Until the Echo has the time from Home Assistant they count seconds since it started.
- **Less noise in the log**: the four times three "Could not open /dev/block/mmcblk0boot1! Can't read the idme." lines
  at every start (harmless: Amazon's own sign-in code trying a way it is not allowed) no longer show.
- **The log on the settings page.** "Log" in the System section shows what the Echo wrote (hassmic, the firewall,
  updates), with a filter, the older part, and a download for bug reports — no adb needed any more. The Sendspin
  pairing token is blanked in it; the rest travels unencrypted, like the whole page.
- **Kiosk Satellite claims no longer flood the log**: once a second at most, with a count of the rest.
- **Echos and Kiosk Satellite tablets answer once between them.** Wake word arbitration has a second mode, "Kiosk
  Satellite", next to the Echos' own (still the default): with it an Echo settles wake words the way Kiosk Satellite
  does, with its tablets and with other Echos in that mode — the device that heard the wake word loudest answers. The
  settings page shows the two side by side with what each gives and what it costs; in short, Kiosk Satellite's has no
  protection (anyone on the network can keep the Echo silent), no preference for the Echo you are talking to, and a
  wait of 400 ms (adjustable, as on the tablets) on every wake word. The firewall opens UDP 2330 only while that mode
  is on. Set the same mode on every Echo; the page warns when they differ. A loudness offset (±20 dB) evens the Echo
  out against your tablets' microphones by ear.
- **Amazon's models download on the Echo itself.** The settings page's new "Download from Amazon" signs the Echo in
  to your Amazon account with a code you enter on your Amazon site (any site with Alexa: amazon.com, .co.uk, .de, .co.jp
  and the others), then fetches extra wake words, whisper detection and the newer sound detection model straight from
  Amazon — no stock mode, no Alexa app, no PC (`scripts/artifacts.sh` still works). It goes step by step (site, code,
  what to download), shows each download's progress and outcome, and marks what the Echo already has. The sign-in
  stays until you sign out (the Echo then leaves your account again), and what lands on one Echo can be copied to the
  others on the same page. Not on the Echo Dot 3: its newer device attestation to Amazon is not reversed, so it says
  so and takes the copies instead.
- **Models with folders inside** (whisper detection, the German "Alexa" set) download, copy between Echos and install
  whole now.
- **A new settings page.** Every setting now says what it does and when to change it, each feature which entities it
  adds to Home Assistant, and the page works on a phone. The Echos section shows the network, the other Echos and
  whether their settings match, in one list. The list keeps itself up to date: a new Echo shows up within 15 s, each
  as soon as it answers (one switched off no longer holds the others back), with "Checking…" while it asks them, and
  an Echo that was unreachable comes back by itself once it is on again.
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
  whispered as well, as a stock Echo does ([how](docs/GUIDE.md#whisper-detection)). It is Amazon's own detector, run on the Echo
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
