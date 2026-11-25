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

Replies, announcements and media come from the addresses Home Assistant gives out, `http://` or `https://`. A Home
Assistant that serves HTTPS itself (`ssl_certificate` under `http:`) gives out its external URL (not the internal one:
its rule, for every device), so the Echo must reach that name from inside your network: through your router
(NAT loopback) or a local DNS entry for it. The certificate is checked: a self-signed one, or one that does not name
that address, is refused ("SSL peer certificate ... was not OK" in the log). "Check HTTPS certificates" (settings page,
System) off takes any certificate, but then anyone on your network could slip the Echo audio of their own.

- **Speak straight on** after the wake word: Home Assistant gets everything from the end of the word, so "Alexa turn on
  the light" in one breath works, and you need not wait for the tone. The tone itself is partly still in what Home
  Assistant hears: Amazon's echo canceller leaves about as much of it as a quiet voice. "Wake sound removal"
  (experimental, on the settings page under Voice, off by default) takes it out: the Echo holds back the audio while the tone plays, finds
  the tone in it and turns down only its few pitches, then sends it all at once, so the first second of what you said
  reaches Home Assistant about a second late. When you speak much louder than the tone, it changes nothing. The log
  says what it found each time ("wake sound removal: ...").

- **The end of the wake word** is left out of what Home Assistant gets. Amazon's engine says "heard" near the end of
  the word, not at it: for the German "Alexa" typically a tenth of a second before the end (a drawn-out one, or one in
  a room that echoes, up to half a second), for the English one, "Echo" and "Ziggy" a little after it, already in your
  command. The Echo waits half a second after Amazon's "heard" and then finds the end by lining up the word's sounds
  ("a-l-e-k-s-a") with what its microphone heard, as the Montreal Forced Aligner does (its pretrained models, CC BY
  4.0, cut down to the wake words: 2.5 MB for all of them): your request starts right there, also before Amazon's "heard".
  This works for Amazon's wake words (Alexa, Amazon, Computer, Echo, Ziggy) in German, English (US, UK, Canada,
  Australia, India), French, Spanish and Japanese, and for microWakeWord's Okay Nabu, Hey Jarvis and Hey Mycroft
  (measured in German and English, the others made the same way and untried). Not yet for Italian, Portuguese and
  Hindi (no usable models for them). It hears the word best in the language you *speak*,
  which need not be the wake word's: German voices saying the English "Alexa" were found in 48 of 75 tests with
  German sounds, in 23 with English ones. So "Language you speak" (settings page, Wake word) picks it; automatic goes
  by your time zone in Home Assistant (Germany, Austria, Switzerland: German; France: French; Spain, Mexico: Spanish;
  Japan; the UK, Ireland, the US, Canada, Australia, India: English), then by the wake word's own language. Home
  Assistant tells the Echo only the zone's offset and summer time, which Germany shares with France, the Netherlands
  and others, so the country comes from Home Assistant's home location, near a border from your Wi-Fi router (see
  "Time and time zone" below); the settings page shows under the setting what Automatic found and from what. The list also has Italian, Portuguese, Dutch, Swedish,
  Danish, Norwegian, Finnish, Polish and Romanian, which automatic picks by the time zone too: there are no sounds for
  them yet, so for the wake word they count as not said, picked or automatic, and the Echo goes on as before they
  were listed (in Brazil: English); the Echo Show writes its date in the language picked. It also learns from your requests how the word sounds
  in your home (your voices, your room, its microphone: speaker adaptation, as the aligner's models were trained for)
  and finds the end more often from the sixth request on (in tests 74 % right instead of 55 % without it); it forgets
  the oldest as new ones come, and what it learns stays on the Echo ("Learn how the wake word is said here" on the
  settings page, on by default). For words without a model it waits for the pause after the word and starts there;
  said straight on, without a pause, or in a loud room, it never leaves out more than earlier versions did. When the
  sounds give no answer near Amazon's "heard" it looks for no pause either, since a short stop inside your command can
  pass for one, and leaves out at most 0.12 s. The log says what it found (`wake: the word ends +90 ms from Pryon's
  end (aligned in 40 ms)`, or `wake: 260 ms of the word's tail left out (a pause)`), and what the sounds said where
  that was no use (`wake: the aligner's end, from Pryon's end: -680 ms`, or `none`).

- **Silence ahead of each request.** Home Assistant's speech detector starts afresh for each request and does not
  listen to its first three quarters of a second. Without anything ahead, a short command said straight after the
  wake word was over before it listened: Home Assistant then waited 15 seconds for you to finish, and some speech to
  text (Parakeet) found no words at all in audio that opens mid-word. So every request the Echo starts (wake word,
  button, a follow-up question) opens with 0.8 s of silence. It goes out at once, so nothing waits for it; Home
  Assistant's debug recordings start with it. Not with Home Assistant's own wake word engine, which listens all the
  time, except for a follow-up question or the action button cutting a reply: those have no wake word to wait for, so
  they get the silence too.

- **The wake word in the request** ("Wake word in the request" on the [settings page](#settings-page), Voice, off by
  default): Home Assistant gets the audio from just before the wake word on, so speech to text hears "Alexa, turn on
  the light". Nothing of your first word can be cut and no stray end of the word turns up. Meant for an AI (LLM)
  conversation agent: Home Assistant's built-in sentences may not recognise a command with the wake word in front
  (with "Prefer handling commands locally" those go on to the agent). "Alexa, stop" as a transcript still stops a
  ringing timer or music, and "Alexa" with nothing after it still ends quietly. No effect with Home Assistant's wake word engine. The log says
  `wake: the wake word sent too, from 670 ms before its end`.

- **The end of a request heard on the Echo** ("End of request heard on the Echo" on the
  [settings page](#settings-page), Voice; experimental, off by default; ESPHome only). Home Assistant ends a request
  when the room goes quiet, so with others talking it stays open and speech to text writes down their words after
  yours ("Next. Eat some food please"). The Echo knows how loud you said the wake word; with this on, a pause in
  *your* voice ends the request, and quieter voices neither keep it open nor get turned up to speech level for Home
  Assistant. On the Echo Dots, Amazon's own speech detector (in its audio front end) has a say too: the dishes, a
  kettle or a door as loud as your voice do not keep the request open either. Relaxed: a pause of 1.25 s, voices from about 17 dB under yours left out; Normal: 0.7 s and 14 dB;
  Aggressive: 0.45 s and 11 dB. A voice as loud as yours counts as yours. If you turn away or drop your voice by more
  than that mid-sentence the request may end too soon, so start with Relaxed. Home Assistant's own end (its "Finished
  speaking detection") still applies: a request never runs longer than before. The log says
  `end of request: heard here (talker -41.2 dBFS, ...)` each time it ends one, and `not heard here first` with the
  numbers when Home Assistant was first: send those along if it ends too soon or too late.

- **Nothing said** after the wake word (or the button, or a question the assistant asked): the ring goes dark
  without the error animation, as on a stock Echo.

- **No Home Assistant** (it restarts, or the link to the Echo is lost): the wake word and the action button show the
  short error animation and start nothing; the log says `wake word: no Home Assistant link, nothing started` (or
  `button: ...`). Not while muted (with Wyoming, Home Assistant's mute switch too), and not before the Echo was set up
  (the setup spinner shows then).

- **"Alexa, stop"** interrupts a reply, but only right behind the wake word: Amazon's models only listen for "stop" in
  the two seconds after it. Out of silence, "Alexa, stop the music" goes to Home Assistant as a normal command.
  With nothing else to stop, it pauses music from Music Assistant or a phone on the Echo itself (the action button
  resumes it), without waiting for speech-to-text to catch the word over the music. What you say after it still goes
  to Home Assistant ("Alexa, stop the timer"); a bare "stop" does not. When Amazon's model misses the "stop", or with
  microWakeWord or Home Assistant's wake word (no "stop" keyword), a request that is only "stop" does the same: the
  music pauses and Home Assistant gets no request; right after the wake word cut a reply, the reply stays ended. A phone without remote control (AVRCP) goes silent instead, until
  it starts playing again; the button cannot resume it.
  When "stop" ends something (a ringing timer, a Drop In call, a reply, a request still being worked on), the Echo
  does not listen afterwards, also when it comes while several Echos are still deciding which one answers.
  **"Alexa, stop" ends listening** ("Stop after the wake word ends listening" on the [settings page](#settings-page),
  Voice, off by default). An Echo that woke up by mistake (the TV, people talking) listens for up to 15 s, and Home
  Assistant carries out whatever it made of that. With this on, "Alexa, stop" means "be quiet": after all of the above
  the Echo also stops listening at once and nothing of the run reaches Home Assistant, as with the action button; when
  it hears both words together it does not start at all (with several Echos too), and an Echo that was listening
  already stops as well. The cost: "Alexa, stop the timer" no longer reaches Home Assistant ("Alexa, cancel the
  timer" does), media Home Assistant plays on other speakers is paused with "Alexa, pause" instead, and now and then the engine
  hears "stop" in a short command (one Echo's week: 2 in 354 requests it answered): that command is lost, say it
  again. ESPHome only. The log says `stop: listening ended` or `stop: came during the round, nothing started`.
- While a timer rings or something plays, the wake word is accepted more readily, as Amazon's models are tuned to do.
- A finished timer rings for a minute ("Timers ring for" on the [settings page](#settings-page): 10 s to 10 min, or
  until stopped), with the cyan timer animation on the ring. Only an explicit stop ends it: "<wake word>, stop", the
  action button, or the "Stop ringing" button in Home Assistant (the media player's stop does it too, but also stops
  what the media player plays). The wake word alone pauses the ring while you talk, and
  it goes on afterwards. With microWakeWord or Home Assistant's wake word (no "stop" keyword) the transcript does it:
  "stop" alone (also stopp, halt, arrête, basta, para) ends the ring and the run, before Home Assistant answers.
  "Stop the timer" is not "stop": it goes to Home Assistant, which has forgotten the finished timer and says none is
  running. Switch on "Any request stops a ringing timer" on the settings page and whatever you say after the wake
  word during a ring stops it, in any language; a question asked during a ring then only stops it.
  With several Echos, a ringing one wins the wake word round, but only if it heard you: from the next room, or over its
  own ring, it may not, and another Echo gets the "stop". Switch on "A stop another Echo heard ends the ring" (Voice
  page, Timers card) on the Echos whose rings may be stopped that way, and "<wake word>, stop" said to any Echo of their
  arbitration network ends them; that Echo then leaves its music alone. It reaches every room, it needs this version on
  the Echo you speak to as well, and "Any request stops a ringing timer" is not passed on. Off by default.
  Several timers can ring at once: each new one gives the ring its full time again, and one stop ends them all.
  A timer rings with Alexa's own timer tone, from the Echo's firmware. "Timer sound" on the settings page picks
  another: Alexa's alarm tone, a triple beep (as loud as Alexa's tone), or a sound of your own: "Upload a sound…" takes
  any file your browser plays (MP3, WAV, OGG, M4A, ..., up to 20 MB), and its first 30 seconds play over and over. The page
  measures its loudness (LUFS, as streaming services do) against Alexa's tone: more than 3 dB off, it says by how much
  and offers to match it, turning it up or down before it is uploaded (turned up, its loudest moments are held back
  so nothing clips; 24 dB up at most). Or keep it as it is and set the timer volume to suit it. "Play" plays the chosen
  sound once, at the timer volume. Each Echo keeps its own; an export says which one is chosen, but your own sound
  stays on the Echo it was uploaded to (upload it on each). Whichever plays, a stop cuts it off at once, and it
  pauses while you talk. Where the firmware has no timer tone, the beep is the default.
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
- Running timers show up in Home Assistant too. A timer you set by voice is Home Assistant's own (Assist keeps it),
  but it is no entity there: Home Assistant only tells the Echo that set it. So the Echo passes it on as three sensors:
  "Next timer" is when the running timer that ends first ends (a timestamp), "Next timer name" its name (empty for
  "set a timer for 5 minutes") and "Next timer length" the length it was set for, in seconds (time added or taken off
  later does not change it). They follow the timer as it starts, pauses, resumes, gets time added or taken off, is
  cancelled or rings; while none runs they are unknown. A paused timer is left out, so with only paused ones they are
  unknown too. Should Home Assistant restart while a timer runs, it forgets the timer and never rings it; the Echo drops
  it 10 seconds after its end. As a timestamp, "Next timer" stays put while the timer runs, so a countdown on a
  dashboard or another device costs no update per second:

  ```yaml
  # seconds left on the kitchen's next timer, for a template sensor or a display
  "{{ (as_timestamp(states('sensor.kitchen_echo_next_timer')) - now().timestamp()) | round(0) | int(0) if
      has_value('sensor.kitchen_echo_next_timer') else 0 }}"
  ```

## Wake word

Three engines, picked under "Wake word" on the [settings page](#settings-page):

|                                         | Amazon's (default)           | microWakeWord (built in: deprecated, below)     | Home Assistant's           |
|-----------------------------------------|------------------------------|-------------------------------------------------|----------------------------|
| Words                                   | "Alexa"; "Echo", "Computer", … downloaded from Amazon | Okay Nabu, Hey Jarvis, Hey Mycroft, Alexa, your own | any openWakeWord model, your own too |
| How well it hears                       | best                         | **significantly worse**                         | depends on the model       |
| "<word>, stop", keener while playing    | ✅                           | ❌                                              | ❌                         |
| Only the nearest Echo answers           | ✅ by the front end's energies | ✅ by audio level, a rougher guess             | ❌ Home Assistant lets the first answer |
| Runs                                    | on the Echo                  | on the Echo                                     | in Home Assistant, ~256 kbit/s streamed all the time |

### Amazon's wake words

"Alexa" comes with the firmware. Others ("Echo", "Computer", …) only Amazon hands out: "Download from Amazon" on the
settings page, or "Copy models" from another Echo. `scripts/artifacts.sh` from the PC is the old way, kept as a fallback
(it registers the Echo to an Amazon account in the Alexa app for a few minutes and undoes it). Pick one in Home Assistant's wake word select. The same word in two languages
shows with its language and region ("Computer (de-DE)", "Computer (en-US)"). Only the word picked wakes the Echo:
Amazon's English (US) "Computer", "Amazon" and "Ziggy" are one model that also knows the others and "Hey Disney".

### microWakeWord

**The microWakeWord built into hassmic is deprecated.** The [microWakeWord plugin](#plugins) is the same engine in a
sandbox of its own (a model file it reads can break the wake word and nothing else), and a later release takes the
built-in one out. While the built-in engine listens, the Wake word page says so and Home Assistant's problem sensor
"Built-in microWakeWord is deprecated: move to its plugin" is on.

**Move to the plugin** (the card on the Wake word page, wherever the built-in engine has models or listens): one
button, and each step is shown as it is done. Plugins are switched on if they are off (the dialog says so first,
and names the other plugins you have switched on, which start with them; Home Assistant reconnects once; if
nothing gets installed, plugins are switched off again). The Echo fetches the **official** microWakeWord plugin
from the plugin index, shows what it may do and waits for its action button, as any install; without the press
nothing is installed and the move stops. Then the Echo **copies** each of your models into the plugin's own list,
where the plugin checks it as it checks an upload, with the threshold and window you set here (a threshold outside
the plugin's 0.50 to 0.99 is brought to its nearest end, and the card says so); a model it refuses is named with
its reason, and nothing of it is kept. Last, the wake word engine becomes the plugin, listening for the same words
in the same slots ("Wake word" and "Wake word 2"). If anything fails on the way, the built-in engine keeps
listening as before. The built-in engine keeps its own models, as they were: what you delete or change on the
plugin's card is the plugin's copy, and the other way round. So **"Switch back to the built-in engine"** on the same
card (or the engine's choice above) listens for the same words again, tuned as before, and **"Copy the models
again"** brings models added to the built-in engine since (a model the plugin has already gets the built-in engine's
files and keeps the threshold and window you set on the plugin's card; the card names those). The plugin stays the
engine then, with the wake words it has. Nothing moves by itself, also not with an update. An Echo whose plugin used
the built-in engine's folder under an earlier version got the plugin's own copies once, at its first start with this
one (the official plugin only); copies of microWakeWord models from another Echo carry the model's two files only.

Where another engine listens (Amazon's, another plugin), the card copies the models and changes nothing else; it
then offers **"Listen with the plugin for …"** as a step of its own, which says which words the Echo would answer to
and which it would no longer. A plugin installed as microwakeword that is not the official one (a file, or another
author's) is named on the card with its author, its author key's fingerprint and where it came from, and the card
asks before it copies your models to it.

[microWakeWord](https://github.com/kahrendt/microWakeWord) is the engine of ESPHome's own satellites (Home Assistant
Voice PE). Its small models were trained mostly on synthetic speech and never with the Echo's microphones: **expect
more missed wake words**, above all from across the room or over music, and more false wakes. While it is on you also
lose "<wake word>, stop", the extra sensitivity while the Echo plays, rings or speaks, Amazon's wake words, and the
front end's energies in arbitration. Sound and whisper detection keep working.

Switching on, the page lists these costs and, on an Echo without a model yet, asks for one to start with: one of
ESPHome's (fetched from GitHub by your browser) or your own `.tflite` with its `.json` manifest. The same page then
adds, renames, tunes (threshold), saves and deletes models; "Copy models" on the Models page copies them to your
other Echos. The models are ESPHome's own files (TensorFlow Lite, not ONNX); the Echo runs them with its own
interpreter, which gives the same results as TensorFlow Lite. Switching back, or deleting the last model, restores
everything.

### Two wake words

Home Assistant's "Wake word 2" select, beside "Wake word", picks a second word that the Echo listens for at the same
time, as a Voice PE does (ESPHome protocol only). Each word starts its own assistant ("Assistant" and "Assistant 2" on the device page), so
"Alexa" can run one and "Computer" another. Both come from the engine that runs (Amazon's or microWakeWord) and are
picked in Home Assistant; with microWakeWord the settings page marks the second one, and its "Use" leaves one word.
"No wake word" in "Wake word 2" goes back to one as well. The action button starts the first one's assistant.
Several Echos settle each word on its own, and "<wake word>, stop" works after either.

The second word runs a second copy of its engine all the time. With Amazon's engine that is a second model as large
as the first, so the Echo works harder while it listens: not measured on an Echo yet (on the PC's emulator of the
Echo, about two thirds more for hassmic as a whole). Leave "Wake word 2" at "No wake word" if you need only one.

### Home Assistant's

The Echo stops listening itself and streams its microphone to Home Assistant all the time, whose wake word engine
listens: openWakeWord, with your own models too. The page's dialog walks through Home Assistant's side:

1. The openWakeWord app (or `wyoming-openwakeword` beside a Home Assistant container).
2. Custom models in `/share/openwakeword`, `.tflite` only: an `.onnx` (as EchoMuse's Forge makes them) needs converting first.
3. "Add streaming wake word" on the assistant, and that assistant on the Echo's device page.

When Home Assistant hears the wake word, the Echo answers as on its own (ESPHome and Wyoming): the sound, the ring,
the front end held on the talker until the command ends. Muted, the Echo sends Home Assistant nothing, so it hears no
wake word until you unmute. During a reply too: once Home Assistant has finished it, the Echo has it listen for the
wake word again while the reply plays, so saying it cuts the reply and what follows is the next request (ESPHome only;
with Wyoming the button cuts a reply). That relies on the Echo's echo canceller keeping its own voice out of the
microphone, so over a loud reply the wake word may be missed. "<wake word>, stop" right then ends the reply and nothing
goes to the assistant. It costs arbitration between Echos, the "stop" keyword (a bare "stop" in the transcript stands
in, a moment later), the extra sensitivity while playing, everything while Home Assistant is down, and likely some reliability. Switching to or from it restarts the
satellite for a few seconds. `-w remote` in [`ARGS`](#hassmicconf) does the same, and then the page cannot change it.

## Several Echos

The Echos compare by how clearly the word stood out of the room's noise. An Echo in a conversation, or ringing, keeps
the next wake word. With only one Echo there is no delay. The wait delays only the sound and the ring, not what you
say: the Echo that answers sends Home Assistant everything from the end of the wake word, so speak on without a pause.
How clearly the word stood out comes from Amazon's audio front end, which takes 0.15 s to answer on the Dots and up to
0.4 s on an Echo Show; the Echo keeps listening meanwhile, and after 0.5 s without an answer it uses its own measure.

- Only Echos listening for the same word compete: one on "Echo" and one on "Alexa" each answer their own.
- "Wake word arbitration" on the settings page (on by default) says whether an Echo takes part. Off, it answers every
  wake word itself but stays in the Echos' network, so the settings pages still find it.
- Other satellites (ESP32 and so on) are not part of it. Home Assistant lets the first to report answer, and an Echo
  that comes second just goes quiet instead of flashing an error.

**How long they wait.** An Echo decides as soon as every other Echo's claim is in. Only when one did not hear you does
it wait out its window, which it learns from your network: how late the other Echos' claims reach it. The window is
the time that 95 % of them took, plus 50 ms, between 0.15 and 1 s; until ten claims are measured it is 0.5 s. Wi-Fi
repeaters, and routers that hold broadcasts back for devices saving power, make claims late (a few hundred ms is
common); a wait shorter than that lets the farther Echo answer. The settings page shows the wait under "Learn the
wait from the network", what it measured, and which Echo's claims come latest. Switched off, a fixed "Arbitration
window" (100–1000 ms) applies instead. The log notes how late each claim came
(`arbitration: a0dbbafe heard it, score 1791, +233 ms`), and `after the decision` for one that missed it.

**Echos in other rooms.** Waiting only pays when the other Echo's claim could beat this one's, and that depends on
where you stand: on the sofa this Echo may be far ahead, at the kitchen counter the other one. How clearly this Echo
heard you tells the two apart. So each Echo remembers, for its last 64 wake words, how clearly it heard you and how
each other Echo's claim compared, and estimates how likely that Echo is to beat it or come within 3 dB when it hears
you as clearly as now: wake words where it heard you about as clearly count most, recent ones more than old ones, and
an Echo it knows little of, or a loudness it has not seen yet, is waited for. Heard clearly, an Echo no longer waits
for the one at the far end of the house that catches a faint "Alexa" now and then; heard faintly (you are elsewhere),
it still waits for the one next door. A door opened or an Echo moved shows within a few wake words, and with nobody
worth waiting for, the Echo answers at once. A wrong guess costs a wake word where the farther Echo answers although
the nearer heard you better, or, when both were sure of themselves, where both start: Home Assistant takes the first
and the other goes quiet. Either wake word is remembered, so the next one like it waits. Nothing to set; the page
names the Echos that never came close, and the log notes per wake word how many it waited for
(`waits up to 406 ms for 2 of 5`) or that it waited for nobody. In a simulated house (`tools/arbsim.py`) the answer
came after 177 ms on average instead of 368, two Echos started in 0.1 % of wake words, and the best Echo was not the
one answering in 0.02 %.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/arbitration-window-dark.svg">
  <img alt="Timelines of one wake word round. Top: the other Echos' claims came between 0 and 372 ms late; 95 % within 356 ms, plus 50 ms gives a window of 406 ms. Every Echo heard you: the living room's claim at 30 ms and the hall's at 95 ms are in, so the Echo decides at 95 ms and skips the rest of the window. One Echo did not: the hall Echo missed it, so the Echo waits the whole 406 ms, then decides. A claim later than the window, at 440 ms: the Echo decided without it, but it is still measured, so the window learns from it." src="img/arbitration-window-light.svg">
</picture>

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
loudest over its room answers, after the "Arbitration window" (400 ms, as on the tablets; Kiosk mode waits it out
every time and learns nothing, since every device must count claims within the same window). The page shows both
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
  ringing, connected), "Drop In with" (the other Echo) and an "End Drop In" button. And "Drop In result", an event
  for each call the Echo makes, as soon as it is known: `connected`, `ringing` (it rings there; `connected`,
  `declined` or `not_answered` follows), refused by the other Echo (`busy`, `do_not_disturb`, `microphones_off`,
  `drop_in_off`), `no_answer` (no word from it in 4 s: off or out of reach), `not_found` (not in this Echo's
  network), refused here (`busy_here`, `microphones_off_here`), `cancelled` (ended here before it connected) or
  `failed`. An automation triggers on it with a state trigger and reads `trigger.to_state.attributes.event_type`.
- **By voice**: import the blueprint [`blueprints/automation/drop_in.yaml`](../blueprints/automation/drop_in.yaml)
  (Settings › Automations › Blueprints › Import, with the file's GitHub address), then create an automation from it.
  "Drop in kitchen" or "Verbinde mit Küche" then calls, from the Echo you spoke to, the Echo in the area of that name
  (an alias of the area counts, as do accents left out: "Küche", "Kueche", "kuche"), else the one with that name,
  else the one on that floor ("drop in upstairs"), else one whose name has the word. Of two Echos in a room it calls
  one that is free. Sentences come for English, German, French, Spanish, Italian and Dutch ("drop in …" in every
  one); the reply is in the language of the voice assistant you spoke to. Sentences and replies are inputs of the
  blueprint. It says "Calling kitchen" only once the call has started, and otherwise why not: no Echo there; the
  other Echo has do not disturb on, its microphones off, Drop In off, is in a call, did not answer, is offline, or
  is not in this Echo's network; the Echo you spoke to is in a call, has its microphones off, or is not an Echo with
  Drop In on; or you named its own room. The Echo you spoke to plays the end-of-call sound when a call does not go
  through. From an Echo with hassmic from 2026-10-11 on, the reply comes at once (the Echo reports what came of the
  call, "Drop In result"), except for an Echo that does not answer, which takes the 4 s the Echo waits for one.
  From an Echo with an older version the reply for a refused call is "The call to kitchen did not go through", after
  up to 4 s. Renaming an Echo or its entities in Home Assistant does not get in its way: it finds the Echos by what
  they report themselves. A blueprint imported before 2026-10-11 found no Echo at all: re-import it (the blueprint's
  menu › Re-import). Replies set by hand before then have fewer entries; the missing ones are said in English until
  you add them.

  Each call shows up twice in the automation's traces: once for the sentence, once for the call itself, which the
  automation makes in a run of its own (so that an Echo whose action Home Assistant does not know cannot leave you
  with "Done": such a run ends with a repair naming the action, and the sentence's run still answers).
- **From the settings page**: the Drop In button on another Echo's card under Echos.

The Echo called plays Alexa's Drop In chime and connects at once. With "Drop In answers: after the action button" it
rings for 30 s instead and only its action button connects. The ring shows Alexa's call animations meanwhile. To end a
call: the action button, "<wake word>, stop", "<wake word>, hang up" (or "auflegen", "raccroche", "cuelga",
"riattacca", "ophangen", "închide") on either Echo, or "End Drop In".
Music on the Echo pauses when a call connects (it does not resume by itself). The volume buttons work during a call; the wake word too, but not while the other side talks (its "Alexa" for its own
Echo would wake this one).

Calls are refused with do not disturb on, with the microphones off, or with another call running; the caller's log and
Home Assistant say why. The sound goes straight between the two Echos (UDP 28932, Opus, encrypted): no cloud, no Home
Assistant in between.

**Your side's sound.** On the Echo Dot 3 and Echo Dot 2 a call sends what Amazon's own calls sent: the front end's
call processing, with noise reduction, an even level and an echo canceller made for calls. It sounds natural and
steady, a little duller than the assistant's stream (little over 4 kHz). "Call microphone" (Features, under Drop In)
switches back to the assistant's stream, brighter and with more of the room. Should the call microphone stop during a
call, the assistant's takes over at once. The Echo 2 and the Echo Dot 5 send the assistant's stream for now (they have
the call processing too, but it has not been tried on them yet); the Echo Shows and the Echo Input have only the
assistant's.

**Echo.** The Echo puts Amazon's audio front end into its call mode, whose echo canceller works on the other side's
voice. What it leaves would still come back to the other Echo, so an Echo sends its room only while someone there
talks; while the other side talks it is turned down 25 dB. Two people talking at the same time work only if the
one here is clearly louder than the echo of the other: as on a speakerphone, take turns.

## Phone calls over Bluetooth

The Echo as your phone's hands-free speakerphone, as a car kit is (HFP, issue #233): on the Echo Dot 2, Echo Dot 3,
Echo Dot 5 and Echo Show 5, where it was measured where each model's Bluetooth chip puts a call's audio, and on the
Echo 2nd gen, Echo Show 5 2nd gen, Echo Show 8 (1st and 2nd gen) and Echo Input (the call on its jack or a
Bluetooth speaker) as read from their firmware, not tried on them yet ([docs/re-hfp.md](re-hfp.md)).

- **Set up**: turn on "Phone calls over Bluetooth" (Features; off by default), then pair the phone with Home
  Assistant's "Bluetooth pairing" switch, as for music. A phone paired before the switch was on must be paired again:
  a phone looks at what the Echo offers only when it pairs. While it is on, every phone paired with the Echo sends its
  calls here whenever it is connected.
- **A call comes in**: the Echo rings with its call animation (the phone rings too). The action button answers, and
  the call talks through the Echo's speaker and its call microphone, with Amazon's echo cancelling for calls ("Call
  microphone" under Drop In). The action button, "<wake word>, stop" or "hang up" end it.
- **Where a call talks is the phone's choice**, as with a car kit: a call answered on the phone talks on the Echo when
  the phone sends its audio over Bluetooth (most do for a connected hands-free device), and the phone's audio picker
  moves it back to the handset. A paired phone can so turn on the Echo's microphone for its call ([SECURITY.md](SECURITY.md#bluetooth)).
- **In Home Assistant** (listed while the switch is on): "Phone call" says `no phone`, `idle`, `ringing`, `calling`,
  `connected` (its audio on the Echo) or `on the phone` (a call talking on the handset: the phone kept its audio); "Phone caller" (the number,
  or the name the phone gives) and "Phone" (the connected phone's name); the buttons "Answer phone call", "End phone
  call" (rejects a ringing one) and "Take the phone call here" (brings a call talking on the phone to the Echo); and
  the action `esphome.<echo>_phone_call` with `number:`, which has the phone call it, its audio on the Echo. Answering,
  taking a call here, switching calls and calling come only from Home Assistant itself (the keyed connection), as
  Drop In does.
- **A second call**: one that comes in during a call (call waiting) or one on hold shows in "Phone call waiting" (its
  caller, `unknown` where the phone names none; empty without one). The action button answers a waiting call and puts
  the one you were on on hold; "Switch phone call" in Home Assistant does the same, and goes back and forth between
  the two. Ending the call you are on ("End phone call", the action button, "hang up") brings the held one back. A
  waiting call you do not answer stops when the caller gives up (or answer or reject it on the phone).
- **Only for calls**: the Echo's microphone goes to the phone only while the phone has a call (or one dialled or
  answered here); an audio link the phone opens without one (its voice assistant, an app's) is refused.
- **Do not disturb** and the microphones off: the Echo does not ring, the phone does; "Phone call" still says
  `ringing`. One call at a time on the Echo: during a Drop In a phone's call stays on the phone.
- **Volume**: the Echo plays a call at its call volume: the volume, or "Call volume" (Sound) with separate volumes.
  The phone's volume keys move it during a call, and the Echo's volume buttons and Home Assistant move the phone's
  (HFP's remote volume control). Outside a call the phone's volume does not touch the Echo's.
- **Sound**: wideband (HD voice, mSBC, 16 kHz) where the phone offers it, which most phones do; narrowband (CVSD,
  8 kHz) otherwise. Measured on the Echo Dot 2, Dot 3, Dot 5 and Echo Show 5; on the Echo 2nd gen, Echo Input, Echo
  Show 5 2nd gen and Echo Show 8 by the same chips, not tried there yet ([docs/re-hfp.md](re-hfp.md)).

## Buttons and light ring

Beyond the README's two figures:

- **Action**, with "Double and triple press of the action button" on (Voice page; off by default; the Echo Shows have
  no action button): twice skips to the next track, three times back to the previous one, of Music Assistant or a phone
  over Bluetooth. Two or three presses act as one single press when nothing plays, during a Drop In call (ringing
  included) and while a login waits for the button. A single press then waits 450 ms. Home Assistant's "Button" event
  fires for whatever acts as a single press, never for a skip.
- **Action**: cancelling works while Home Assistant still listens or thinks, as on a Voice PE; the wake word then
  cancels too and listens again (ESPHome only).
- **In Home Assistant**: the event entity "Button" fires for each press of the action and volume buttons (`action`,
  `volume_up`, `volume_down`) and once for each held 1 s (`action_hold`, `volume_up_hold`, `volume_down_hold`),
  besides what the button does on the Echo. A press counts when it is let go within the second, so a hold never
  fires both. Nothing fires for both volume buttons held (that pairs Echos) or for the press that approves a login
  or a phone setting up the Wi-Fi. In an automation: trigger "Entity" → the Button event, with the event type as
  condition (`{{ trigger.to_state.attributes.event_type == 'action_hold' }}`). A hold is Home Assistant's alone:
  the volume steps only for a press let go within the second (so a step comes as you let go), the action button acts
  only then too. One exception: while the settings page waits for you to take the note of a copied plugin off
  (the plugins' ring shows, for a minute at most, three times an hour at most), a hold of the action button
  that begins then is that note's, and `action_hold` does not fire for it; an automation on `action_hold`
  misses that one hold. Held on, the action button still warns at 5 s and resets the Echo at 10 s.
- **Echo Show**: no action button. Both volume keys pressed together and let go within a second are it: a login on
  the settings page, debug access, setup from a phone, answering a Drop In. Held 2 s they pair Echos, as on a Dot, so
  there is no hold to reset a Show (see [Factory reset](#factory-reset)).
- **Light ring in Home Assistant**: the light "Light ring" (colour, brightness, effects Pulse, Spin, Blink,
  Rainbow; "flash" blinks for 2 or 10 s) puts a colour on the ring from automations. The Echo's own lights always come
  first: while it listens, answers, has its microphones off, rings a timer or shows the volume, the colour waits and
  comes back afterwards. Colours are the nearest of a fixed set (24 hues, full or pale, and white, at three levels),
  since the ring only plays animations made beforehand: the Echo makes them at its first start with this version and
  restarts the ring for that (a second without light). Not on an Echo where they could not be written: then there is
  no light in Home Assistant.
- **Fill**: "Light ring fill" (0 to 100 %) lights only that share of the ring, in the light's colour; the rest stays
  dark. It fills the way the volume does, LED by LED; "Light ring rotation" (a configuration entity, and on the
  settings page under Lights) turns where it starts by that many LEDs, for how the Echo stands. Rounded up: 1 %
  lights one LED, and the whole ring shows only at 100 %. A part of the ring is a still colour: the effects (pulse,
  spin, blink, rainbow) are for the whole ring, and a flash blinks it whole. The fill stays as set while the light is
  off, as its colour does; after a restart of the Echo it is 100 % again. Set it from an automation, e.g. a washing
  machine's progress:

  ```yaml
  - action: number.set_value
    target:
      entity_id: number.kitchen_echo_light_ring_fill
    data:
      value: "{{ states('sensor.washing_machine_progress') | int(0) }}"
  ```
- **LED by LED**: the action `esphome.<node>_light_ring_leds` (`<node>` the name the Echo was first given, e.g.
  `echo_dot_3_5695c4`, whatever it is called in Home Assistant) gives each LED a colour of its own, in the order the
  fill goes (from where it starts, so the rotation counts): `#rrggbb` each, `""` for a dark one, up to 12. The
  light is on with them, at its brightness, each colour the nearest of the set; "Light ring fill" does not count
  meanwhile. A colour or an effect for the light makes it one colour again.
- Fill and LED by LED need each LED's own animations, 10 MB of the Echo's system partition. Where it has no room for
  them, the Echo installs the ring without them: the light works, and Home Assistant has no fill, rotation or action.
- **Echo Dot 5**: its ring keeps animations in a small co-processor that takes only a few hundred, too few for the set
  above. There the light is drawn when it changes, into an animation the ring already holds, so it needs no set made
  beforehand: same colours, effects, fill and LED by LED as on the other Dots.

  ```yaml
  - action: esphome.echo_dot_3_5695c4_light_ring_leds
    data:
      colors: ["#ff0000", "#ff0000", "", "#00ff00"]
  ```
- **Your own designs** (settings page, Lights): draw animations for the ring and pick, for each of the Echo's own
  (listening, its end, thinking, speaking, not understood, timers, volume, microphones off and on, do not disturb,
  identify, Bluetooth pairing, the Drop In call states, setup, factory reset, page login, plugin install), Amazon's
  or one of yours;
  a design is drawn on its own and can show for as many of them as you like ("Use for…").
  Up to 100 designs; each is frames of the 12 LEDs, every frame shown as long as you set, played again and again, once,
  or an opening then a loop. The designer paints LEDs with a brush or the whole ring, turns, mirrors, dims, makes
  spinning, fading and breathing frames, turns the hue of a whole design or a frame (how light each colour looks stays), has undo, and starts from Amazon's
  animation if you like; a preview plays it. "Recolour Amazon's…" on an animation is the short way to Amazon's own in
  other colours. A Ring / Band switch on the designer's stage shows the design as an Echo Show's band shows it (the
  LEDs in a row, never turned), and paints there too; it opens on what this Echo has.
  Listening and its end turn towards whoever speaks: draw them with the talker where the arrow is, and the Echo makes
  the turned copies itself. Volume takes the colours of the design's first frame through Amazon's steps. Nothing
  changes on the ring until **Apply**: the Echo then writes the designs to its system partition and restarts the
  ring (a second without light). A design goes to other Echos with "Copy models" on the Models page (into
  their designs only), or as a file (Download, then Upload there). What each animation shows is a setting: "Compare & copy"
  copies it, takes the design along where the other Echo has not got it, and applies it there.
- **Designs and theme packs** (Lights, "Designs"): your designs, and packs as folders of them. A pack holds designs
  and says which of them each animation shows; open it to see its packshot, its animations (the same tiles as the
  main card: pick one of its designs, Amazon's, or "not in this pack"; a design from elsewhere comes in as a copy) and
  its designs (opened in the designer from there, or made new in it). A design is in one pack at most: "Move…" takes
  it to another or to the top level. "Save as pack" on the animations card makes one of what the ring shows now.
  Packshots in four styles are drawn from the pack itself, its light on an Echo Dot: Halo (its listening animation large, the next three small), Line-up (up to eight of its animations on a row of Echos, each named), Sequence (four of them as they run, left to right) and Trail (one as a long exposure, its frames rising off the Echo), under "Details" with its
  name, author and a line about it. **Use** sets its animations, only those or all of them (the rest back to
  Amazon's), and applies. A pack travels with its designs: as a `.ringpack` file (Export, Import) or with "Copy
  models". Up to 50 packs; their designs count towards the 100.
- **Light ring live** (Echo Dot 3, Echo Dot 2, Echo 2 and Echo Dot 5; settings page, Lights, "Live from Home Assistant", off by default): the
  Echo draws its ring itself, from Home Assistant, in any colour on any LED, up to 30 pictures a second, and plays your
  designs at once, without Apply. In layers, the lowest first:
  - "Light ring" (the light above) in its exact colour instead of the nearest of the set, fading when it changes
    (Home Assistant's transition, else a quarter of a second), with more effects: Breathe, Color loop, Comet, Wave,
    Sparkle and Fire beside Pulse, Spin, Blink and Rainbow, and six that follow the music the Echo plays (Music
    Assistant, a phone over Bluetooth, Home Assistant's media). The Echo listens to it itself: twelve bands, the kick,
    snare and hi-hat, the tempo and the beat, left and right, how loud it gets. **Music beat** (the ring blooms on every
    beat from LED 1 round to the back and settles to a glow, each beat of the bar a shade apart, every fourth whiter),
    **Music spectrum** (the twelve bands LED by LED, bass at LED 1 round to the highs at LED 12), **Music VU meter**
    (stereo: the right channel from LED 1 the clock's way, the left the other way, green to red, peaks falling),
    **Music orbit** (two comets going round once a bar, opposite ways, flaring on the kick), **Music drums** (the kick a
    wave from LED 1, the snare one from the back, hi-hats sparks) and **Music aurora** (light flowing slowly round,
    swelling on the beat, turning cooler with brighter sound). They take the light's colour; for white, colours of
    their own. They follow what comes out of the speaker, not what is still in its buffer, and the music's own
    loudness, not the volume; with nothing playing they breathe quietly. Fill, rotation and `light_ring_leds` work as before;
  - twelve lights "Light ring LED 1" to "12" over it (colour and brightness each; LED 1 is where the volume starts,
    turned by "Light ring rotation"), for scenes and automations; an LED that is off lets "Light ring" through;
  - an animation over everything: your own frames from `light_ring_animation`, or a design of your library from the
    select "Light ring design", the action `light_ring_design`, or "Show on the Echo" on the Lights page (beside each
    design, and in the designer for what you are drawing, saved or not). "None" in the select, or "Stop" on the page,
    ends it.

  The actions: `light_ring_frame` (12 colours `#rrggbb` from LED 1 on, `""` dark; `brightness` 0 to 255; `timeout`
  in seconds, 0 for none: sets the twelve lights), `light_ring_animation` (`frames`: one `"<ms>:RRGGBB,RRGGBB,..."` per
  picture with all 12 LEDs, as a design's lines; `repeat` 0 for until stopped; `brightness`), `light_ring_design`
  (`name`, `repeat` 0 for as drawn, `brightness`) and `light_ring_stop` (the animation, and the twelve lights off).
  The Echo plays animations itself: smooth, whatever Home Assistant does meanwhile. About 600 pictures fit in one
  action; a picture shorter than 33 ms is skipped.

  The Echo's own lights always come first: listening, thinking, replies, timers, volume, Drop In and the red ring of
  muted microphones show as you set them above, and your lights come back after them. Nothing is brighter than the
  Echo's own lights: it follows "LED brightness" (auto included) the way Amazon's ring does, and no LED goes above what
  Amazon's ring itself writes at that level. The current through the LEDs, the mute button's light and the ring at
  start-up stay Amazon's. Switched off, the ring is Amazon's ring program's again and "Light ring" uses its set.

  ```yaml
  - action: esphome.echo_dot_3_5695c4_light_ring_animation
    data:
      repeat: 3
      brightness: 255
      frames:
        - "150:FF0000,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000"
        - "150:000000,FF0000,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000"
        - "150:000000,000000,FF0000,000000,000000,000000,000000,000000,000000,000000,000000,000000"
  - action: select.select_option
    target: { entity_id: select.echo_dot_3_5695c4_light_ring_design }
    data: { option: Sunrise }
  - action: light.turn_on
    target: { entity_id: light.echo_dot_3_5695c4_light_ring }
    data: { effect: Music drums, brightness: 200 }
  ```
  An Echo installed before this needs the update once: root then lets hassmic write the ring's LEDs (that one file of
  the LED driver, given again at each start); until then the setting is not shown.
- **Listening points at you**, as with Alexa, and also while a timer rings or the light is on (Amazon's ring stops
  turning as soon as a second light shows; hassmic plays the listening ring already turned).
- **Echo Show 5 (both generations) and Echo Show 8**: no ring; its lights run along the bottom edge of the screen as the ring laid out in a line (the
  talker's side always in the middle: two microphones tell directions apart too poorly to turn it). The band keeps
  that strip of the screen for itself, 10 pixels by default ("Light band" on the settings page; 0 gives the whole
  screen to the apps and shows no band), so an app shown on the screen is never covered. The light "Light ring" works
  there too, in any colour, and so do your own designs: each shows on the band as the ring's twelve LEDs in a row,
  the side that faces the talker in the middle, never turned (the settings page draws and edits them that way on
  these models, and Apply shows them at once). A design made on a Dot works on the band and the other way round;
  the designer's Ring / Band switch shows a design either way on any Echo.
  Your words, the reply and timers can show on the screen too: [The screen](#the-screen-echo-show), "Words and timers".
  The volume of voice replies shows in cyan, as on the Dots. The screen wakes for the band and sleeps 5 minutes after the last light (1 minute with
  the microphones off or in Do not disturb); the Screen page's **Screen sleeps after** changes the 5 minutes (0 keeps
  the screen on), and **Clock when idle** shows the time in place of going dark.
- **Microphone off** plays Alexa's own sounds. A switch that is already off when hassmic starts is shown, not announced.
- **Wake sound**: Alexa's own where the firmware has it; the Echo Show 8 1st gen has none and plays hassmic's short blip
  (or one of your own: "Your own sounds", below). On the Echo Show 8 (1st and 2nd gen), the Show 5 2nd gen and the Echo Input a sound
  after a pause starts 0.35 s late: the speaker takes that long to switch on and swallowed the start. The Echo Show 5
  1st gen loses nothing and plays it at once.
- **Your own sounds**: the settings page's "Sounds" card (Voice) replaces any of the Echo's sounds with one of yours:
  the wake sound, the action button, microphone off and on, the volume keys, Bluetooth connected and disconnected,
  Identify, and Drop In's chimes and ringtones (the timer's sound is "Timer sound", above). "Upload…" takes any file
  your browser plays, as the timer sound does, and offers to bring it to the loudness of Alexa's sound it replaces, so
  the sounds volume suits both. The wake, button, volume, Drop In and call connected sounds may be 2 seconds at most: the wake sound plays over
  the start of your command, and the echo canceller leaves part of it in what Home Assistant hears. Ringtones may be
  30 seconds and repeat, the others 5. "Play" plays one once at the sounds volume, even when its switch is off. The wake sound, action
  button, microphone, volume and Bluetooth sounds have a switch each, and "All off" sets them together; a switch silences
  that sound and keeps your choice, so on again plays yours or Alexa's as before. Home Assistant's "Wake sound" is the wake
  sound's switch. Identify and Drop In have none: Identify is asked for, and Drop In's tell the room a microphone is open.
  The bin deletes yours, and Alexa's plays again. Where the firmware has none
  of Alexa's for a sound, yours still plays. Each Echo keeps its own: exports and "Copy models" leave them out, and a
  factory reset deletes them.
- **Sound packs**: sets of those sounds, on the same page. "Save these sounds" keeps what is in use as a pack, "Use"
  puts a pack in use (it replaces your sounds, and those it has none of go back to Alexa's), "Export" gives a `.soundpack`
  file, a plain tar with `pack.json` and one WAV per sound, and "Import" takes one on any Echo. 16 packs and
  30 MB at most; nothing is written with under 32 MB free on the Echo. Switches stay as they are when a pack changes.
- On an **Echo Show** the screen says the same with a face: see [The screen](#the-screen-echo-show).
- Silent and dark at boot.
- The ring dims with the room's light as on a stock Echo (Amazon's own logic, on by default); a fixed level from Home
  Assistant switches that off.

## The screen (Echo Show)

On the Echo Show 5 and the Echo Show 8, both generations. hassmic's own screen is small on purpose: the
Show's screen is yours to give to any app (Kiosk Satellite, View
Assist, a dashboard browser). Install apps with `adb install <app>.apk`; Amazon's WebView is there for those that
show web pages. Only one app can use the microphone at a time, and hassmic has it: an app that runs a voice
satellite of its own has to run without its microphone (as a display) while hassmic listens.

- **The face** shows over hassmic's home screen. Its eyes follow the assistant: wide and cyan while it listens,
  glancing up while it thinks, a mouth moving with the reply, closed and red with the microphone off, asleep in Do not
  disturb, orange with a **Stop** button while a timer rings, green during a Drop In (**Answer** and **Decline** when
  one rings, **End** during a call), orange while the Echo waits to be added to Home Assistant. Tap the eyes and they
  smile. Below them: the clock, in Home Assistant's time zone (12-hour where the Echo's language is US, Canadian or
  Australian English), or what Music Assistant plays, with play/pause; tap it for the music. A request to the
  assistant, a ringing timer or a Drop In bring the face back from the music.
- **Music on the screen** (Screen page, "Music on the screen while it plays", off by default): while a track from Music
  Assistant plays, the Playing tab (the big cover) shows instead, until the music stops. **In place of the face** replaces
  only the face: an app in front (Kiosk Satellite, a dashboard) stays. **In place of the home app too** also comes over the
  app chosen as Home (or Kiosk Satellite), and that app comes back when the music stops; other apps are left alone. A
  pause keeps the page, and so does the gap between two tracks. A request, a call or a ringing timer bring the face and
  the page comes back when they are over. It goes by itself only while it still shows the Playing tab: browse the
  library or search on it and it stays, as if you had opened it yourself (the drawer's **Home** brings the home app back). Choosing the face or Home yourself (the drawer's **Face** or
  **Home**, the face button on the music page), or going back to the app, keeps the music page away until the music has
  stopped; a face chosen that way stays, as it always has, until the drawer's **Home** brings the home app back.
- **Clock when idle** (Screen page, card **When idle**, off by default): where the screen would go dark, it shows the
  time across the whole screen instead, with the date under it (in the language of "Language you speak"), a clock for
  the nightstand. The wait is the screen's
  own: **Screen sleeps after** (5 minutes), 1 minute with the mic off or in Do not disturb (the clock then says so on a
  line under it). Until then the face shows as always; a touch on the clock brings it back (that touch does nothing
  else), and so does anything the Echo does: a request, a ringing timer (with its **Stop**), a Drop In, the volume,
  music starting. Music playing shows its bar under the clock. With an app or the music page in front the screen goes dark
  as before.
  Brightness follows the Brightness card: **Adaptive brightness** dims the clock at night.
- **Motion keeps the screen on** and **Motion wakes the screen** (same card, experimental, off by default; shown
  while [Wi-Fi motion](#wi-fi-motion) is on): a presence sensor of sorts for the screen. With the first, motion counts
  as something happening: the screen goes dark, or shows the clock, only once nobody moved for **Screen sleeps after**
  (plus the sensor's own 30 s), over an app too. With the second, a dark screen comes on as motion starts, showing
  whatever it showed, and the clock gives way to the face; motion under the clock with only the first switched on
  leaves the clock alone. A screen put out with **Screen off** in the drawer stays dark until it is touched or the Echo
  wakes it itself (a request, a timer); after an update, a restart of hassmic or whenever the screen's program starts
  anew (a new **Light band** height, or after it stopped) that is forgotten, and motion wakes it again. Neither in Do not disturb, so the night stays dark.
  Wi-Fi motion sees movement between the Echo and the router, also in the next room, so expect the screen to come on
  for someone passing there now and then.
- **The drawer**: pull down from the top edge of the screen, whatever shows, as on Amazon's own Echo Show. In it:
  **Home** (the home app), **Face** (where the home app is another), **Music**, **Do not disturb**, **Screen off** (a
  touch wakes it), what plays (tap it for the music) and the Echo's volume. Over an app, start the pull on the thin
  black strip along the top of the screen: the app never gets that touch, so it does not scroll or refresh. Pulled over
  an app, the drawer shows on a picture of it; close it (tap below it, or push it up) and the app is back as it was.
- **The home app**: the settings page's **Screen** page picks what the Echo shows at start and brings back when it
  ends or crashes (three times a minute at most), but not while you have the face or the music open: Kiosk Satellite
  until you pick another (if it is installed), the face, or any app you installed. A new choice applies at once.
- **Back**: over an app, swipe along the bottom edge (the light band), left or right.
- **Brightness**: the settings page's **Screen** page and Home Assistant set it as a stock Echo Show's Display
  settings do. **Screen brightness** holds one level, 47 % by default (what the Echo ran at before). **Adaptive
  brightness** follows the room's light on Amazon's own curve, which is dark: under about 5 lux the screen is barely lit,
  and it is near full brightness only from about 100 lux. **Adaptive brightness adjustment** moves that curve brighter (up
  to +100) or dimmer (down to −100), as the stock slider does with adaptive on. With Kiosk Satellite installed, the Echo
  switches off the app's own adaptive brightness (stopping the app for a moment), so these settings set the screen; switched
  on again in the app, it goes off again within seconds.
- **Adaptive colour** (Echo Show 8 2nd gen): under **Screen**, **Brightness and colour** also has Amazon's
  **Adaptive Color** setting. It warms the screen's white with the room's light, which can turn a dashboard yellow
  at night. It is **off by default**. Switch it on to follow the room's light again; your choice survives a restart.
  The switch shows the device's confirmed state, so a change can take a few seconds to appear. It is hidden while
  the display service is unavailable. Amazon can temporarily switch it off during video playback. Returning to
  stock Alexa restores the original setting.
- **Words and timers** (the settings page's Screen page, both off by default), over whatever the screen shows: the
  face, the music or an app (Kiosk Satellite too); not over the menu pulled down from the top, which hides them while
  it is out.
  **Speech on screen**: what you said and the assistant's reply in large letters, your words smaller and grey above the
  reply in white, as large as they fit, over a dark shade. They come as speech-to-text and the reply do, stay while the
  Echo answers and 8 seconds after ("Speech stays for", 0 until the next request), then fade; the next request takes
  them away. With Wyoming only your words show.
  **Timers on screen**: the timers set on this Echo, each its name (or its length) over its time left in large digits
  with its unit ("4:59 min", "1:02:05 h", "45 s") and a bar of what is left, the one that ends first on top, a paused
  one grey. A ringing timer shows in orange and pulses until it is stopped, with a Stop button. The screen stays on while
  timers show; your words and the reply go before them while they show.
  **Touch**: while they show, they take the screen's touches, over the face, the music or an app alike: a tap on the
  words takes them away, a tap on the timers hides them until a timer starts, changes or ends, and Stop stops a ringing
  one; nothing under them reacts, and the app has its touches back once they are gone. The menu still pulls down from
  the top.

### Music Assistant on the screen

The music page has five tabs down its left side, and the face's button above them goes back to the face.

- **Playing**: the cover, title, artist and album; where the track is (drag the bar to move in it); shuffle,
  previous, play/pause, next, repeat (tap again: one track, then off); the group's volume. "Playing on" at the top
  is the group this Echo plays in: tap it for the speakers.
- **Queue**: what this Echo's queue holds, the current track marked. Tap a track to play from there; long press it to
  remove it. **Shuffle** and **Clear** (asks first) above.
- **Library**: artists, albums, playlists, tracks, radio, podcasts and audiobooks, by name; more load as you scroll. An
  artist opens its albums, an album or a playlist its tracks, each with **Play**, **Shuffle**, **+** (to the end of
  the queue) and **Radio** (Music Assistant's radio mode: more like it). Tap the Library tab again to go back to its top.
- **Search**: type on the screen's keyboard; Music Assistant searches once you pause (all its providers, not only the
  library) and the results come grouped: artists, albums, songs, playlists, radio. **Done** hides the keyboard.
- **Speakers**: every player Music Assistant has. A tick is a speaker that plays together with this Echo: tap one to
  add it or take it out (a speaker without a circle cannot play together with this Echo, as Music Assistant says).
  Each one's volume beside it, the whole group's on top. Long press a speaker: play what it plays here instead.
  Joined to another speaker's group, the page says so, with **Leave**.
- A long press on anything in the library or the results: **Play now**, **Play next**, **Add to queue**, **Start
  radio**. Tapping a song plays it at once.

Playing needs nothing to set up: Music Assistant plays on the Echo over Sendspin and says what plays. The queue, the
library, search and the speakers need Music Assistant's own API, which wants a token: in Music Assistant open your
profile (the person icon), **Long-lived tokens**, **Create**, and on the Echo's settings page (Music) press
**Connect…** and paste it. The page sends it sealed for that Echo, which keeps it and never shows it again. The
address, left empty, is taken from the Music Assistant that plays on the Echo when you save (port 8095) and kept:
the token only ever goes there, so a Music Assistant that moves needs **Connect…** again (or its address given).
Until then those tabs say how to connect.

## Music

One source at a time, the newest wins: a phone starting over Bluetooth pauses Music Assistant (the whole group), Music
Assistant starting on the Echo pauses the phone. The voice assistant ducks both, from the wake word until the reply has
ended, by 16 dB unless "Music ducking" (Sound page, 0 to 60 dB) says otherwise: raise it if commands over loud music or
an audiobook are not understood. "<wake word>, stop" pauses either, and the action button pauses and resumes.

With several Echos (wake word arbitration on), every Echo that heard the wake word turns its music down too, by its
own "Music ducking", until the Echo that answers is done: two Echos in one room playing the same Music Assistant group
both go quiet for your command and the reply. An Echo that did not hear you, in another room, plays on. The answering
Echo tells the others over the Echos' own network that it is busy (every 2 s) and when it is done; without Home
Assistant, without touching the volume. A follow-up question keeps the others down for your answer too; music Home
Assistant then plays on the answering Echo (a radio you asked for) does not. If the "done" is lost, the music comes
back about 7 seconds after the last "busy". "Duck for other Echos" (Sound page, on by default) switches it off for
one Echo. The log says
`duck: echo-living-room answers what we heard too, music down until it is done` and `duck: ... is done, music back`.

- **Music Assistant** finds the Echo by itself (Sendspin player) and, with Home Assistant 2026.11 and Music Assistant
  2.10.9 or later, pairs with it by itself too: the Echo tells Home Assistant its Sendspin id, and Home Assistant hands
  Music Assistant the Echo's pairing token, over the encrypted connection only. With older versions pair it by hand
  with the token: shown at the end of `scripts/setup.sh`, and as the diagnostic entity "Sendspin pairing token" in
  Home Assistant (disabled by default). Steps: the model's page, "Optional: Music Assistant". When Music Assistant restarts, the Echo announces itself again
  until it is back (after 30 s, then less and less often, at most every 30 min). Where Music Assistant cannot see the
  Echo's announcements (another subnet or VLAN), add `<echo-ip>:28928` under its Sendspin provider's "Manual IP
  addresses for discovery": with the port, since a bare address means Sendspin's default 8928, which is not the Echo's.
  The Echo speaks both versions of Sendspin in use: Sendspin 1.0 of Music Assistant 2.10.8 and later, and the draft
  before it of Music Assistant up to 2.10.6. It picks whichever the server speaks, each time it connects.
- **From a phone** over Bluetooth: SBC, AAC, aptX, aptX HD, with media controls. Switch on "Bluetooth audio from phones" on the
  settings page; the "Bluetooth pairing" switch in Home Assistant then lets phones pair (blue chaser on the ring).
  Connecting and disconnecting play a chime and, like stock Alexa, "Connected to <phone>" / "Disconnected from
  <phone>": Home Assistant speaks those words, and only once "Allow the device to perform Home Assistant actions" is
  ticked in the Echo's ESPHome options (Settings → Devices & services → ESPHome → the Echo → Configure). Without it
  you hear the chime only. "Bluetooth announcements" on the settings page turns both off and picks their language.

### Even volume steps

Each point of the volume is the same step, wherever it is set from (Home Assistant, Music Assistant, the buttons, the
page): 0.5 dB a point, 5 dB a press of the volume buttons, from 1 (about 50 dB under full, quiet but heard) to 100
(full, as before); 0 is silent. A stock Echo's steps are uneven: the Echo Show 5 has 31 volume steps of its own for
the 100 points (three or four points play the same) and puts 46 dB in the bottom half and 20 in the top; the Echo
Dot 2 gains 1 dB from 60 to 70 and 10 from 90 to 100. The Echo sets its own volume to the nearest step above, by its
measured curve, and turns the sound down the rest of the way itself; on these Echos, whose speakers play 16-bit
audio, that loses nothing. Low volumes are louder than on a stock Echo (an Echo Show 5's 10 is 45.5 dB under full
instead of 51) and middle ones somewhat quieter (50 is 25 dB under full instead of 20 to 22). "Even
volume steps" (Sound page, Speaker card; exported) switched off brings the Echo's own steps back. A Bluetooth
speaker that sets its own volume is set to the number as before. On by default.

### Separate volumes

The settings page's Volume card (Sound) sets the Echo's volume, the same one as the volume buttons and Home
Assistant's media player. Switch it to "Separate volumes" and four kinds of audio get a volume each, 0 to 100 %,
independent of each other:

| | What | In Home Assistant |
|---|---|---|
| Music | Music Assistant, Bluetooth from a phone, `media_player.play_media` (not announcements) | the media player's volume |
| Voice replies | what the voice assistant says, and announcements (also `tts.speak` or `play_media` with `announce` on the media player) | Assistant volume |
| Echo sounds | the wake sound, button presses, Bluetooth and Drop In chimes (not the volume keys' tone: it plays at the volume they just set, so you hear the step; at 0 as quiet as the lowest step) | Sounds volume |
| Timers | the ring of a finished timer | Timer volume |
| Calls | the other side of a Drop In call (only while Drop In is on) | Call volume |

Voice replies, Echo sounds, Timers and Calls each have "Own volume" or "Follow music" (Calls start on "Follow music",
as before they had a volume of their own). Following the music, the slider sets
how far above or below the music volume it plays, in volume points (timers 20 above music at 40 play at 60), and it moves with
the music: the media player's volume and the volume buttons change both. Home Assistant then has no number for it.
Copying settings to another Echo (Export, Compare & copy) takes the switches and the offsets, never a volume itself:
like the Echo's volume, that is for the room it is in.

The same number sounds the same on every slider, and (with [even volume steps](#even-volume-steps) off) as that volume on a stock Echo (measured on an Echo Dot 2,
within 0.3 dB, and on an Echo Show 5, whose volume steps are its own: its top end is much flatter and its bottom end
steeper than the Dot's; the other models use the Dot's until someone measures theirs with `tools/vol-curve.py`). The volume buttons change what is playing: a Drop In call, a reply (the ring shows the steps in cyan),
a ringing timer, otherwise the music. On a Bluetooth speaker that sets its own volume (absolute volume) the speaker is
set to the loudest of them and the Echo turns the others down, as on its own speaker (with the Echo's volume curve:
how loud a number is depends on the speaker); the speaker's own buttons move the music.

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
  the Echo turns it down itself, as stock does. [Separate volumes](#separate-volumes) apply on the speaker too.
- **In time with other players**: a Bluetooth speaker plays late, by its buffer. "Bluetooth speaker delay" (default
  250 ms) is what the Echo allows for; set it by ear, in Home Assistant or on the settings page.
- **First word cut off**: some speakers switch their amplifier off while the Echo is silent and switch it on only
  once sound comes, too late for the start of a reply. "Keep the speaker awake" (settings page, off by default) plays
  a 20 Hz tone at -50 dBFS under everything, too low and too quiet to hear, so the speaker never hears silence.
- **Switching quickly**: the Echo can only play everything on one or the other (Amazon's audio mixer has a single
  output; on the Echo Show 5, Android moves all of it), so music on the speaker with replies on the Echo is a matter of switching "Play on Bluetooth speaker" from an
  automation when music starts and stops. "Stay connected when off" (settings page, off by default) makes that quick:
  switched off, the speaker stays connected and only the sound moves back to the Echo ("Bluetooth speaker" shows
  "Standing by"), and switching on takes well under a second instead of a reconnect. If the speaker drops the link
  meanwhile, the Echo connects again only once the switch is on. With "Keep the speaker awake" as well, the Echo keeps
  sending its inaudible tone while standing by, against speakers that switch themselves off when idle; whether that
  works depends on the speaker. The costs: the speaker is taken (phones usually cannot connect to it), a battery
  speaker runs down sooner, and with the tone the Bluetooth stream never stops, which takes airtime from the Echo's
  Wi-Fi (they share an antenna). The page asks before switching it on.
- The Echo still listens for the wake word meanwhile. How well it hears over loud music from elsewhere in the room is
  not measured yet.
- SBC only (every speaker has it), one speaker at a time. Setting it up needs ESPHome mode.

## Bluetooth proxy

Works like an ESPHome `bluetooth_proxy` with `active: true`: scanning, up to 3 connections, "Just Works" pairing only.
While a phone plays, the proxy stops scanning: the radio cannot do both without the music stuttering. While Home
Assistant talks to a connected device (a desk moving, a lock), scanning pauses too, until a second after the last
request and for about 30 seconds at a time (then it scans for 3 seconds). Connected devices get the short link
ESPHome's proxy asks for (about 10 ms; a device that asks for a link of its own gets that one): an exchange took about
25 ms, measured with a PC as the device (not yet with a desk).

On by default. "Bluetooth proxy" (settings page, Features) switches it off: the Echo then tells Home Assistant it is no
proxy, so Home Assistant sets up no scanner for it, and the Echo stops scanning and refuses connections. Disabling the
Echo's Bluetooth entries in Home Assistant is not enough: the proxy is no entity, and advertisements keep coming.
Bluetooth audio from phones, the Bluetooth speaker and the setup from a phone (Improv) keep working with it off.

## In Home Assistant

| Always there | |
|---|---|
| Media player | TTS, `play_media`; its volume is the Echo's (the music's, see [Separate volumes](#separate-volumes)) |
| Mute, Do not disturb | Mute: no microphone audio leaves the Echo, also not for a conversation Home Assistant asked to go on (an announcement still plays); DND drops announcements; purple flash when switched on |
| Wake sound | the wake sound only, on or off; off keeps the one you chose |
| Equalizer | bass, mid, treble, −6 to +6 dB, Amazon's own, on everything the Echo plays |
| LED auto brightness, LED brightness | a level holds the ring there and switches the automatic off (Echo Show 5: the band's brightness, no automatic) |
| Adaptive brightness, Screen brightness, Adaptive brightness adjustment | Echo Show: the screen's backlight, see [The screen](#the-screen-echo-show) |
| Timer ringing, Ringing timers | on while a finished timer rings; their names (diagnostic), see [Voice](#voice) |
| Stop ringing | button: ends a ringing timer and nothing else |
| Next timer, Next timer name, Next timer length | the running timer that ends first: when (timestamp), its name, the length it was set for (s); unknown while none runs, see [Voice](#voice) |
| Button | event: each press and 1 s hold of the action and volume buttons, see [Buttons and light ring](#buttons-and-light-ring) |
| Light ring | light: colour, brightness, effects; the Echo's own lights first, see [Buttons and light ring](#buttons-and-light-ring) |
| Firmware | update entity, see [Updating](../README.md#updating) |
| Identify, Restart, Factory reset | buttons; Restart reboots the Echo |
| Web UI address | diagnostic: the settings page's address |
| Sendspin pairing token | diagnostic, disabled by default, no state until Home Assistant has set the Echo's key: Music Assistant gets the token from Home Assistant by itself (2026.11 and later) |

Time and time zone: the Echo takes both from Home Assistant, over the encrypted connection only (no time before
Home Assistant has given it its key), when it connects and every 6 hours. Change Home Assistant's time zone and the Echo
follows within those 6 hours, or at once when Home Assistant reconnects. Home Assistant says the zone as its rule
(offset and summer time), not its name, so the Echo picks a zone with that rule from its own time zone data: the
one of the country Home Assistant's home location (its Home zone) is in (Germany: Europe/Berlin, not
Europe/Amsterdam, which has the same time). It reads that location from Home Assistant like any other state and keeps
only the country, never the coordinates. Within about 3 km of a border, or without a location, it takes the country
your Wi-Fi router says it is in. With neither it keeps a zone set on it before, else takes one with the right time
whose name may be another country's; that one never sets the language ("Language you speak", under Voice). Its log
stays in UTC.

A new IP address (the router hands out another one, or a Wi-Fi switch): Home Assistant finds the Echo again by itself,
within about two minutes. It ignores the Echo's announcement while it still holds the old connection (90 s), so the
Echo announces itself again after 100 s without Home Assistant, then less and less often, at most every 30 min.

**Features**, switched on the settings page: while one is on, its entities are in Home Assistant; off, they are gone
(Home Assistant reconnects for a moment when one is switched).

| Feature | Entities |
|---|---|
| Wake word arbitration (on by default) | Arbitration peers |
| [Sound detection](#sound-detection) | Sound (event) |
| [Whisper detection](#whisper-detection) | Last request whispered |
| Wake word events (off by default) | Wake word (event): `detected` (the Echo took it), `near_miss` (Amazon's engine heard something close, did not wake; none with microWakeWord), `answered_elsewhere` (another Echo or satellite answered), `no_speech` (woke, nothing said). A `detected` followed by `no_speech` is a false wake. Nothing while muted. Near misses can come often while the TV plays: an entry in Home Assistant's history each |
| [Plugins](#plugins) (off by default, on the Plugins page) | Plugin (event): `installed`, `updated`, `removed`, `synced` |
| Request and reply text (off by default) | Last heard (what speech-to-text understood), Last reply (what the assistant answered). Both are cleared when a request starts, so the same request twice is still a change. Home Assistant takes at most 255 characters per state: a longer reply ends in "…". Every request and reply goes into Home Assistant's history while this is on. ESPHome only |
| [Wi-Fi motion](#wi-fi-motion) | motion sensor, sensitivity |
| Light sensor (off by default) | Illuminance (the room's light in lux); "LED brightness" then also follows auto brightness, an entry in the logbook every few minutes. Off, it shows only levels you set |
| [Bluetooth proxy](#bluetooth-proxy) (on by default) | no entities: Home Assistant's Bluetooth scanner for this Echo |
| Bluetooth audio from phones | Bluetooth pairing |
| [Playing on a Bluetooth speaker](#playing-on-a-bluetooth-speaker) | Bluetooth speaker search, Play on Bluetooth speaker, Bluetooth speaker, Bluetooth speaker delay |
| [Separate volumes](#separate-volumes) (off by default, on the Volume card) | Assistant volume, Sounds volume, Timer volume, Call volume (each while it has its own; calls with Drop In) |

**On the settings page only**:

- "Mic level": how loud speech reaches the voice assistant, −35 to −15 dBFS (default −26); the Echo adjusts its gain.
- "Noise reduction": off by default; low, medium, high take the background down by up to 6, 9 or 12 dB (RNNoise, on
  what the voice assistant gets).
- "Wake sound removal" (experimental, under Voice): off by default; takes the wake sound out of what the voice
  assistant gets (see [Voice](#voice)).
- "Wake word in the request": off by default; Home Assistant gets the wake word too (see [Voice](#voice)).
- "Language you speak" (Wake word): the language whose sounds find the wake word's end (see [Voice](#voice)); automatic by default.
- "Learn how the wake word is said here" (Wake word): the Echo adapts the wake word's sounds to your home (see [Voice](#voice)); on by default.
- "End of request heard on the Echo" (experimental): off by default; relaxed, normal or aggressive end a request when
  the one who said the wake word stops talking (see [Voice](#voice)).
- "Timers ring for": 1 minute by default, 10 s to 10 min, or until stopped.
- "Timer sound": Alexa's timer tone by default; its alarm tone, a beep, or a sound of your own (see [Voice](#voice)).
- Bluetooth announcements and their language, the online updates channel, debug access (adb over Wi-Fi), SoC
  temperature and CPU usage.
- "Music Assistant without pairing": off by default, so only Sendspin servers paired with the token may play.

## Kiosk Satellite on the screen

On an Echo Show, [Kiosk Satellite](https://kiosksatellite.com/) can show a Home Assistant dashboard while the Echo stays
the voice satellite. It is optional: the guided setup offers it, and on an Echo already installed `scripts/kiosk-install.sh [echo-ip]` does
the same. Do the app's setup from your PC at `http://<echo-ip>:2324`, and set its remote admin password: that page is open to your
network. Then:

- The Echo opens the app within seconds of the install, and again whenever it ends: after a crash, its own update, or a boot. Allow up to a minute.
  On a Show with hassmic's own screen ([The screen](#the-screen-echo-show)) the app is that screen's home app (the
  Screen page can pick another): the drawer from the top edge leaves it for the face or the music, and its Home brings
  it back.
- It gets storage, location, overlay, settings, usage stats and device admin without asking, since nobody is there to
  answer. Its web page (port 2324) and its Home Assistant API (port 6053) are reachable from private addresses.
  Its updates install by themselves.
- Its own adaptive brightness is switched off: the Echo's brightness settings set the screen
  ([The screen](#the-screen-echo-show)).
- Four rows appear on the settings page under System, once the app is installed:
  - **Internet access** (on): only that app gets out, for its updates and outside pages. Off: your network only.
  - **Microphone** (off): for a plugin that talks, such as a doorbell. In the plugin pick "Voice communication": "Automatic"
    and the other sources give silence. The wake word keeps working during such a call.
  - **Camera** (off): needed to scan a QR code on the Echo. The Home Assistant token can also be pasted from a PC at
    `http://<echo-ip>:2324`.
  - **DLNA renderer** (off): phones and media servers on your network find the app's DLNA renderer and play on the
    Echo. Switch the renderer on in the app too (off there by default). The firewall then admits the port the app
    names under Settings > DLNA Renderer > Server port, from private addresses only and only while the app listens
    there, and closes it when the renderer stops. A port you type in the app works too. DLNA has no password: anyone on
    your network can play on the Echo while this is on.
  Turning microphone or camera off closes the app for a moment.

To remove it, `scripts/kiosk-install.sh --uninstall [echo-ip]`. A plain `adb uninstall` fails, because the Echo made the
app a device admin; the script takes that off first. The Echo's own screen shows again and ports 2324 and 6053 close.
The app's own setup goes with it, the Home Assistant login included: installed again, it starts from the beginning.

**Trust it like adb.** The update helper runs the app's own code as the shell user. Those switches set Android's
permissions, but an app that wanted to could get past them. Install only an app you trust; `docs/SECURITY.md` has
the detail. Turn the app's own voice assistant off, and do not make it Android's home app (its launcher).

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
Amazon does not hand this model to the Echo Show 5 (it refuses that device type); "Copy models" from a Dot that has it
is the way there, not yet tried on a Show.

The sensor is set when you stop speaking, before speech to text has finished, so the agent's prompt can read it. Each
request starts it as unknown: a request that is not scored (the wake word alone, a cancelled one) never leaves the one
before's answer standing. For
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
  On an Echo Dot 3 among five Echos, "Alexa" whispered too: 890–993 whispered, 0–1 spoken. Above 500 counts as
  whispered. Saying the wake word normally and whispering the rest is fine: the detector hears what Home Assistant
  hears, from the wake word's end on.
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
  relying on it. On the Echo Dot 2 and Echo 2 also when the router changes speed (the Echo Dot 3, Echo Dot 5 and Echo Show 5
  allow for that).
- **Through a kernel module of ours**: the Wi-Fi drivers do not report what this needs. Loaded only once you switch it
  on (within 10 s), then until the Echo restarts. Where Wi-Fi going off and on reloads the driver (seen on the Echo
  Show 5), the module hooks the new one by itself (from the version of 2026-11-17 on, once the Echo restarted; not yet
  seen on an Echo); should no frame reach it for a minute, the sensor shows unknown and the log says why.
- ESPHome only. It does not use the microphones; muting does not stop it.
- On an Echo Show it can also keep the screen on and wake it: **Motion keeps the screen on** and **Motion wakes the
  screen** on the Screen page ([The screen](#the-screen-echo-show)).

## Settings page

Works in Wyoming mode too; the "Web UI address" entity shows the address.

**Logging in**: press "Ask the Echo", then the action button within a minute (the ring shows that a login waits; without a press it
goes back after the minute, also when the page was closed). Press only once the page says "Waiting… press the
action button": that is the Echo's word that the press is this login's.

**One thing waits for the action button at a time**: a login, adb (Debug access), a plugin's install, a withdrawn
plugin to run anyway, or the note of a copied plugin. Whatever you asked for first keeps the button until you press (for a note: hold it for a
second), a minute has passed, or you cancel it on its page; no request of anyone else puts something else in its
place. And when it is over, the Echo takes no login from **another** browser for half a minute: a press that comes
late, or a second press, lets nobody in, and each such press starts that half minute again. So logging in two
browsers one after the other takes half a minute between them; the second page waits by itself. The Echo
remembers this over a restart of its software (an update): a browser that asks right after one may be told to
wait for up to a minute and a half if something was waiting for the button before. What the page says when the
button is not yours:

- *"Something else waits for the action button, or did a moment ago"* (login page): adb, a plugin's install or
  another browser's login has the button, or just ended. Do not press for this login. The page keeps asking by
  itself and says "Waiting… press the action button" when the Echo has taken your login: leave it open.
- *"Another browser asked to log in at the same time"*: the Echo cannot tell which of two unknown browsers is
  yours, so it lets neither in. The ring goes dark and a press is an ordinary press. No login is taken until half
  a minute after the minute of the browser that asked first, and not while the two keep asking against each other;
  your page keeps asking by itself. If the other browser is not yours and it keeps happening, something on your
  network is asking: it can delay your login this way for as long as it keeps at it, not get in by your press. Log
  in through another Echo of the network, which takes no button, and look at `boot.log` ("a second browser asked
  to log in").
- *"… waits for the action button first"* (Debug access, a plugin's install): a login has the button. A press now
  serves that, so the page asks nothing of you; it asks again by itself when that login is over (within its
  minute), and the Echo takes no further login in between.
- *"Too many requests to log in came to this Echo"*: more than 40 in ten seconds from one address, or 120 from
  all; the page waits and asks again. Devices on your network that ask from three addresses as fast as they may
  can keep every new login out while they do; log in through another Echo of the network then.

**The ring for a login is the same whoever asked.** Any device on your network can ask to log in while nothing
else waits, and a press then lets it in. Press only while your own page says "Waiting… press the action button".

Closing adb ("Close now") from one browser does not cancel another browser's request to open it that still waits
for the button. While a phone sets up Wi-Fi over Bluetooth and waits for the button, a press is the phone's: a
browser's login that waited ends, and its page asks again.

Revoke approved browsers on the page. Echos in one network trust each other: a
browser logged in on one gets into the others without their buttons ("Log in through <another Echo>"). Debug access
still takes that Echo's own button.

The page is a set of pages with a menu down the side (on a phone, behind the button at the top left), each with an
address of its own, so the browser's back button and bookmarks work: `#/voice`, and `#/voice/mic_level` for one
setting. **Search settings** (or the `/` key) finds any setting, and things that are none (Log, Wi-Fi, Factory reset),
in any page and takes you there. Each setting shows its name, one line on what it is for, and its control; the ⓘ
beside its name has the full explanation. Colours follow the system's, or Light or Dark at the foot of the menu.

**Overview** says how the Echo is doing and what does not work and why (models missing, for instance; that Home
Assistant is not connected shows on every page), with volume, mute, do not disturb and the wake sound at hand, your
Echos with how each stands against this one, and every feature with its switch.

**Echos**: every Echo this one hears (and any you add by address), the arbitration network and those outside it with
the reason and what to do. The page talks to the other Echos directly, at their own addresses on port 28931, also when
you opened it through a reverse proxy. Over plain HTTP only, though: a page that came through an HTTPS proxy may not
call them (the browser blocks it), so open `http://<echo-ip>:28931` for those.

**Compare & copy**: a table of your Echos side by side, a row per setting, a column per Echo, the values that stand out
tinted. No Echo is the source: click the value you want and it goes to the other Echos in that row, from whichever Echo
has it; click another value in the row and the copy turns around, click the same one and the row is left as it was.
Nothing changes until you press Apply.

- **Use all** under an Echo's name takes that Echo's value in every row with a difference: "make them all like this one".
- The cross on a cell leaves that Echo out of the row (only with three Echos or more); the lock under an Echo's name
  leaves it out altogether.
- **Group identical Echos** folds Echos with the same settings into one column. A setting one of them has not got
  (another model) is no difference. Picking in such a column picks for all its Echos. Past six Echos it starts on, until
  you choose; the Models table has the same box, and there a model an Echo has not got is a difference.
- **Only differences** (on by default) hides the rows where all agree; the box beside it filters by name.
- A setting an Echo does not have shows a dash there, and that Echo is left out of the row. An animation of the light
  ring copied takes the design it shows along where the other Echo has not got it, and is applied there. A change of
  wake word engine to or from Home Assistant restarts that Echo's satellite, and Apply waits for it.

**Models**: everything that is added to an Echo.

- **Copy models**: wake words, microWakeWord models, whisper and sound detection models and your light ring designs and
  theme packs, between any of your Echos, in the same kind of table: click the Echo a model should come from, or "Fill
  in what is missing". A wake word set is tried on the receiving Echo's engine first; each Echo that got something
  restarts its satellite once.
- **Download from Amazon**: wake words, whisper detection, the newer sound detection model, fetched on the Echo itself.
  Sign in with a code you enter on your Amazon site, tick what you want. If an Echo's attestation key ever refuses,
  download on another Echo, or with `scripts/artifacts.sh`, and copy.
- **Models on file**: "Save all", or "Choose…" to tick some, puts them into one `.tar` on your computer
  (`hassmic-models.json` lists them, each model in a folder of its own). "Load…" reads such a file and
  installs what you tick on this Echo, checked as a copy is; models already there, the same version, are left out.
  Keep one, and you have your Amazon models even if Amazon stops handing them out. Load only files you saved yourself
  or got from someone you trust: a model runs inside the Echo.

**Export / Import**: `hassmic-settings.conf` (name=value lines, without name, keys and pairings). Import applies it to
this Echo or another, or use it as a preset for the next install (`scripts/setup.sh --preset <file>`). A file from
before the sounds had their own switches (a version older than 2026.11.02 in its first line) with the wake sound off
turns the action button, microphone and volume sounds off too, as that switch did then.

**Identify** (at the foot of the menu, and beside each Echo in the lists; also a button in Home Assistant): a rainbow on the ring
for 10 s and stock's setup sound, to tell which Echo is which.

**Note**: a few words of your own under the Echo's name at the top of the menu, such as where it stands. Click "Add a
note" there, or Edit on the Note row of the System page; save it empty to remove it. It shows in the browser tab's
title, on your other Echos' pages (Overview, Echos, the list below) and on the login page, so you know which Echo's
button to press. Up to 80 bytes, one line. Anyone on your network can read it, as the name. It stays with that Echo:
not in the settings file, and a factory reset removes it. The name itself is fixed; name the Echo in Home Assistant.

**Your other Echos from the name**: click the Echo's name at the top of the menu (or press the down arrow key on it) for
a list of your other Echos with their notes and addresses. Pick one and its settings page opens on the page you are on
(Voice here, Voice there). It lists the Echos the Echos page knows; one that does not answer comes last.

**Speaker gain** (Sound; Echo Dot 2, Echo 2, Echo Dot 3 and Echo Dot 5, and the Echo Show 5 and both Show 8s): for an Echo that is too
quiet at full volume. It raises the analog gain in front of the speaker (Echo Dot 2 and Echo 2: the codec's "HP Driver
Gain", +0 to +12 dB over stock; Echo Dot 3: the amplifier's output level, +0 to +2 dB, all it has above where Amazon
leaves it; Echo Show 5: the codec's line out, +0 to +12 dB in steps of 1.5 dB, each dB set going to the nearest step; Echo Show 8 1st gen:
the same, and down to −6 dB, since it is loud at full volume already; Echo Show 8 2nd gen: the amplifier's digital
volume, +0 to +9 dB in steps of 0.5 dB, the top kept below where bass distorts; Echo Show 5 2nd gen: it depends on the
amplifier in that unit: as the Show 5 with Realtek's codec, +0 to +4 dB with TI's, +0 to +6 dB with Maxim's; Echo Dot 5: its
Maxim amplifier, +0 to +6 dB), so everything the
speaker plays gets louder at every volume step; the 3.5 mm jack and Bluetooth speakers keep their level. Owners of an
Echo Dot 2 found +6 dB loud and clean at full volume; speakers differ, though (some Dot 2s are much louder than others
at the same gain), so raise it a step at a time and lower it at the first crackle: loud music near full volume
distorts above what your speaker takes, and a speaker played distorted for long can be damaged. Not copied to other
Echos with the settings, for the same reason.

**Name** (System): the one the Echo gave itself, as the Voice PE does. Its node name `echo-dot-3-5695c4` is the ESPHome
device name, the host name `<node>.local` and the base of entity ids. It never changes, not even with a reset; name
the Echo in Home Assistant.

**Switch Wi-Fi network** (System › Wi-Fi): pick one of the networks it sees or type a name (hidden network); the
password before or after, empty for an open network. The Echo tries the new network without saving it, keeps it only
once it has an address and the router answers, and otherwise returns to the old one within a minute or two, saying
why. Once the new one works it forgets the old ones. Expect a new IP address: find it as "Web UI address" in Home
Assistant (which finds the Echo again within a few minutes if it reaches that network), and approve the browser there
once more. WPA2 and open networks only; a WPA2/WPA3 mixed network joins as WPA2 (tried on an Echo 2).

**Move to a stronger access point** (System; issue #234; Echo Dots and the Echo 2, on by default): where your Wi-Fi has
several access points under one name (a mesh, a router with repeaters), the Echo moves to a stronger one when its
signal stays under -70 dBm for 30 s. It first asks the access point where to go, if the access point takes such
questions (802.11v, "BSS transition" in many routers' settings), then scans: every channel the first time (3 to 6 s
off its channel), only the channels your network was seen on after that (half a second): once a minute while the
signal stays weak, then less often (up to every 5 minutes) while it finds none stronger, as with a single router far
away. It moves only to an access point at least 8 dB stronger and no weaker than -67 dBm, which takes
a quarter of a second and keeps its address. An access point that does not take it is left alone for 10 minutes. The
Wi-Fi row shows the access point the Echo is on (its band and signal) and the last move; the log has each one
("Wi-Fi moved to ..."). Without this the Echo stays on the access point it joined, however weak, until the link
breaks: the Wi-Fi service that moved it in Amazon's software is stopped on these Echos, and the Wi-Fi client they keep
does not look around while connected. Fast roaming (802.11r) is not used: these Echos' Wi-Fi drivers refuse to join
with it, so a network that requires it is one they cannot join at all. The Echo Shows and the Echo Input move by
themselves (Android's Wi-Fi service).

**Network address** (System; issue #191): Automatic, the router hands the Echo its address (DHCP; a reservation in the
router keeps it the same), or Fixed: the address, subnet mask, router and one or two DNS servers, filled in from what
the Echo has now. Pick an address outside the range your router hands out (its DHCP pool), or two devices can end up
with the same one. The Echo tries it before keeping it: if another device already answers at that address it refuses
at once; else it takes the address, waits up to 30 s for the router to answer there, then up to two minutes for
something to reach it there (the settings page, which asks at the new address by itself, or Home Assistant, which
finds it again by its announcement). Only then is it kept; otherwise the Echo goes back to what it had (the fixed
address before, or DHCP) and the page says why. Back to Automatic the same way; the router may hand out another
address then, which the page finds through your other Echos when it can. The fixed address is the Wi-Fi network's: a
switch to another network goes back to Automatic (a failed switch comes back to it), and a factory reset forgets it.
How: on the Dots and the Echo 2, Amazon's own static mode of the firmware (`dhcpcd-static`) in place of its DHCP
client; after a restart the Echo takes an address from the router for some seconds before its fixed one is back. On
the Echo Shows and the Echo Input, Android's own IP settings of the network, which it keeps by itself. The guided
setup (`scripts/setup.sh`) asks for one just before the install and sets it the same way once the satellite runs (the
PC is what reaches it there, so it should be on the Echo's network); setup from a phone cannot carry an address.

**Log** (System): `boot.log` with a filter, the older part, and a download for bug reports ([reading it](../README.md#when-something-is-wrong)). The Sendspin token is
blanked; the rest travels unencrypted, like the whole page.

**Restart** (System; also a button in Home Assistant): reboots the Echo, as pulling the plug does, and nothing is
forgotten. For an Echo that is stuck while its page still answers; back in about a minute.

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
  only after a press of its action button (good for a minute). Connect with the app first: the ring then shows
  that a phone waits, and only a press while it shows is for the setup. With no phone connected the button does
  what it always does (the music, the assistant).
  Until that press the phone gets the Echo's name and can make it identify itself (once a minute), nothing else:
  no list of networks either.
  One press is good for one change of network within that minute; for another, press again.
  **The ring does not tell whose phone waits.** A press while it shows lets whatever is connected over Bluetooth
  within that minute change the Wi-Fi once. **Press only when your own app has asked for the button. If your app
  cannot find the Echo or cannot connect to it while the ring shows that wait, something else is connected: do
  not press.** Only one device can be connected to the setup at a time, and one in Bluetooth range can stay
  connected, with the ring asking, for as long as the Echo is without its network
  ([Security](SECURITY.md#bluetooth)).
  Home Assistant gives you a minute and a half to press, then ends its own connection: the ring stops asking,
  and comes back when Home Assistant connects again to ask once more. A press while the ring is dark is not
  the setup's (the Echo starts listening, as on any press): press again when the ring shows the wait.
  A phone that was forgotten is not kept: when nothing has come from it for two minutes and nobody pressed, the
  Echo ends its connection and offers the setup again; the phone can connect again at once (Home Assistant does
  so by itself at its next step). After a change of network that worked, the connection ends a minute later.
  A change of network that fails (a wrong password, a network that gives no address) leaves the Echo offering
  the setup: try again from the app.
  If its network comes back by itself while a phone is still connected, the Echo ends that connection once it has
  been on the network for a minute in all (a switch under way is let finish first; the press, or a scan asked for
  within the button's minute, begins the minute anew), however often the phone connects again or the network
  comes and goes: until then the ring would go on asking for the button, and the settings page would keep new
  browsers waiting. The same on an Echo that is not set up and got its network some other way; there asking for
  the list of networks does not begin the minute anew.
  When Home Assistant adds an Echo while a phone is still connected from its setup, that connection ends: at
  once, or, where a network was just given over that setup, a minute after the change worked. In that minute
  a phone still gets the result and the settings page's address, the Echo's name and version, and the
  identify sound and rainbow once; a new network or the list of networks it does not get. Such an Echo never asks for the button for a phone until it has been set up and
  without its network for ten minutes. Once the Echo is
  back on its network and no longer offers the setup, a phone that still connects is turned away.
- Improv sends the Wi-Fi password over Bluetooth **unencrypted**: that is the protocol, the same for every Improv
  device. Not with `-B` (Bluetooth off) in `ARGS`.

## Factory reset

At 5 s of holding the action button the ring warns: let go and nothing happens. Or "Factory reset" in Home Assistant
(a button on the device page), or "Factory reset…" under System on the settings page. The Echo Shows have no action
button to hold (both volume keys held pair Echos instead): only Home Assistant's button or the settings page reset them.

- **Forgotten**: everything in `/data/local/hassmic/state/` (settings, Home Assistant's key, approved browsers,
  Bluetooth pairings, Sendspin, microWakeWord models) and every saved Wi-Fi network. It starts as after the install:
  orange spinner, [setup from a phone](#setting-up-from-a-phone). Delete the device in Home Assistant and add it again.
- **Kept**: the install, `hassmic.conf`, Amazon's extra models.
- Do not hold past 20 s: Amazon's own button handler still runs and has a factory reset of its own at 21 s.

## Plugins

**Experimental**, off by default, and not yet tried on an Echo (2026-11-20). A plugin is a program by someone else
that runs beside hassmic: a feature its author keeps working. The first one is microWakeWord, the wake word engine
hassmic also has built in, deprecated there: [Move to the plugin](#microwakeword) takes your models over. Design and state: [plugins.md](plugins.md).

- **Switching them on**: the settings page's Plugins page, "Plugins". Off, no plugin runs. Below the switch: the
  plugins installed on this Echo, one card each ("Update" on a card where the plugin index has a newer version for
  this hassmic, with a button on its foot), "Browse plugins" and "Install from file…". The switch is exported and
  copied between Echos with your other settings ("Compare & copy", "make like this Echo", an import, a preset): on
  another Echo it runs only the plugins installed there, and every install there takes that Echo's action button.
  A file that switches plugins and community plugins on and picks a plugin as the wake word engine is applied in
  that order, whatever order its lines are in.
- **Community plugins** (a switch of its own under "Plugins", **off by default**): everything the plugin index does
  not list as official, which is its community listings and any package from a file. Off, the Echo installs none of
  them, whichever way they come (a file, the index, a copy from another Echo, a push), and stops the ones it has:
  they stay installed with their settings, their cards say why, and they run again when you switch this on. Official
  plugins are not affected. It is a setting like the others: a click switches it on or off, no press of the action
  button, since every community plugin still asks for the button when it is installed and gets only what you accept
  then. It is exported and copied between Echos with your other settings ("Compare & copy", "make like this Echo",
  an import, a preset); on another Echo it lets nothing in by itself, each plugin there takes that Echo's button.
  Home Assistant cannot switch it. Where this Echo's system install is older than the plugins' own SELinux domain,
  the page says in full, never behind an ⓘ, what its sandbox is missing, in two places: in the question before you
  switch community plugins on there, and in the install dialog of every community plugin (the one that asks for the
  action button), which an import, a preset or a copy from another Echo does not skip, though they switch the
  setting without that question. It says which of the three levels this Echo has (complete, without its
  SELinux wall, without SELinux at all), what stays in place, and how to close the gap on your model (run
  `scripts/install-system.sh` again; on the Echo Shows that flashes the boot image again, from fastboot as at the first
  install; the Echo Dot 5 is built and flashed again from its firmware ZIP). A community wake word engine stops with
  the switch (no wake word, the page says why and what to do) and cannot be chosen while it is off.
- **The sandbox's state line** at the top of the Plugins page: "Sandbox: complete" (each plugin in the plugins' own
  SELinux domain, which a system install from 2026-11-22 on has), "without its SELinux wall" (in hassmic's domain: a
  system install from 2026-11-12 on) or "without SELinux at all" (older system installs: hassmic itself also runs
  without its domain there, which the page then says, plugins or not). An update never changes this: only running the
  system install again does. The plugins' own domain is not tried on an Echo yet.
- **Browse plugins** (a page of its own, `#/plugins/browse`): what the
  [plugin index](https://github.com/Gamer92000/hassmic-plugins) lists, one row per plugin: its name, Official or
  Community, its author, what it says of itself, the version this Echo would install, and "Install", "Update" or
  "Installed" ("Installed, version N: needs a newer hassmic" for one installed under a newer hassmic than the Echo
  runs now, where the index has nothing newer for this one: there is nothing to update to). A search (name, author, description, and what it may do: "network", "microphone", "wake word"…), a
  switch "Only what runs on this Echo" (on by default; off, the list also shows plugins that need a newer hassmic,
  each with why: "Needs a newer hassmic: it speaks net.lan 2, which this one does not yet"), and the
  tier. Withdrawn versions are not shown, and community plugins only while they are switched on (the page says how
  many it leaves out and where to switch them on). "Look again" and the time of the last look are at the foot of the search
  card. **Official** plugins are this project's own, reviewed; **Community** ones are listed and not reviewed: each
  is built by its author's own GitHub CI from the commit the index names, and the index checks that GitHub vouches
  for that (an attestation) before it lists it, and ran it for a few seconds to see that it keeps to the sandbox's
  rules; which shows that the package came out of that public commit's build, not that the code is good. A community row
  also says the broad things it may do, as the index flags them ("Broad: it connects to the internet…": any device
  of your network, the internet, calls, every request heard or read), and when the Echo will ask for its button
  twice (it hears every request and has a way out; on an update of a plugin you accepted with both already: twice
  wherever that update asks for the button at all, which is where it asks for more than you accepted).
  After each look the Echo also takes the index's word on the versions you have installed where it is new (a new
  key of the index, or a higher tier: the same version, so no button and nothing installed; settings and files
  stay): the row says "Taking the index's listing of it…", and where that failed, why, with "Take its listing" to
  try again (one that failed for the moment is also tried again at the next look). A plugin whose listing the Echo
  cannot read (a hassmic of another index key) counts as installed from a file, and with community plugins off it
  does not run until such a look: its card says so. The Echo looks at the index for it by itself, so this needs
  nobody to open the Plugins page: about a minute after it starts, when its network comes back (at most once in
  five minutes), and again every hour while the look did not move it (the index out of reach, say). Where a listing
  is merely by the index's old key while a new one is coming, its daily look does it. A version the index no longer
  lists, or lists with nothing your Echo can take, cannot be moved this way: it stays a file's (with community
  plugins off, stopped) until you install a version the index lists, or switch community plugins on; the hourly
  look leaves it alone, and the daily look still asks the index, so a version listed again is moved within a day.
  "Install" has the Echo download the package from the index, check it (its size and SHA-256 against the index's
  signed list, its author's signature, the index's listing in it) and show the same dialog as a file; the action
  button accepts it. **For this the Echo asks the index itself** (GitHub Pages: it sees the Echo's address and
  which files, and "hassmic" as what asks, no version, nothing else): when "Browse plugins" is opened, or the
  Plugins page while plugins are switched on (once, and not again within an hour), and when you press "Look again",
  "Install" or "Update". Two exceptions: the list of withdrawn plugins (below), and the plugin that is the Echo's
  wake word engine, after a hassmic update for which that plugin needs another build; and, before a hassmic update
  that needs other builds of the plugins installed here, those builds (below). hassmic then looks for it when it looks for updates, so only while "Online updates" is on,
  and installs it without the button only if it is listed, by the same author key and asks for nothing beyond what
  you accepted; else it leaves it to you, and the page's warning about the wake word engine stays. What the page
  says when the list is not taken: "cannot reach the plugin index" (an Echo kept off the internet: download the
  `.hmp` on a computer from the index's page and use "Install from file…"), a signature that does not hold, "an
  older list than this Echo has seen" or "than this hassmic takes" (someone serves an old copy of the index;
  nothing of it is taken), "made with a newer revocation list" (the site's files are of two times: wait a minute
  and look again), or a list dated more than a day ahead of the Echo's clock. Beside a list that was taken it may say
  that the index's list of withdrawn plugins is older than the one the Echo has (another Echo handed a newer one
  over first): the Echo keeps its own. An "Install" or "Update" right after the index published looks at the index
  once more by itself.
- **Withdrawn plugins**: the index can withdraw any plugin, official ones included, and one installed from a file
  (by its name or its author's key), with a reason. Such a plugin is stopped and kept, its card says "the plugin
  index withdrew this version: …", and it cannot be installed again in that version. Removing it is yours, and so
  is **"Run it anyway…"** on its card: the Echo waits for its action button, and the plugin then runs with what
  you accepted for it, until the index publishes another list that names it again (then it stops again and you
  are asked anew). A list that no longer names it starts it again by itself. A withdrawn wake word engine stops
  like any other: no wake word is heard (never Amazon's in its place), and the page says to choose another engine
  or run it anyway. **The Echo learns of withdrawals by itself**: while plugins are on and one is installed, it
  asks the index for that list once a day at a random time (that file and its signature only, a request to GitHub
  Pages that names no plugin and no version; the index's list too where a plugin installed may have a new listing
  there, above), it takes the list from the other Echos of its network and hands its
  own on (an Echo kept off the internet learns of it that way; only Echos of its network can ask for it, though
  the hourly word each sends shows anyone on the network that it has plugins; an Echo of the network that
  misbehaves can hold a newer list back from the others for as long as it keeps at it, so an Echo kept off the
  internet relies on its neighbours, and the week's warning below is what tells you), and a
  look at the index brings it too. It never goes back to an older list. The Plugins page shows the list's date and
  when the Echo last had word that it is the newest (the index's answer, or a newer list from another Echo, counted
  from the day that list was made; the same list from another Echo is no word); after seven days without such word, while plugins run, the Plugins page
  and the Overview warn, also when the Echo does not know when it last had word, or after an update that wants a
  newer list than the one the Echo has (what that one withdraws stays withdrawn meanwhile). With plugins off, none
  of this is asked or told.
- **Installing from a file**: "Install from file…" with a package (`.hmp`). The Echo checks it (who signed it, that the program
  is the one its manifest names, that it is a program the sandbox can hold) and the page lists what the plugin may
  do, one line each. Then press the action button on the Echo: that accepts the list. An update installs
  without the button only when it is what you accepted or less of it: everything it may do and every row of its
  settings as before, or a capability or a row gone, a smaller quota, fewer or smaller files.
  Anything else (another capability, a larger quota, another row of any kind, a row renamed or with another
  explanation, preset or range, other files in a file list, another name, another description of itself or
  another address behind "Source and issues", a change this version does not understand) is listed, as "New: …",
  "More: …" or "Changed: …" (a new description and a new address are shown as they would be), and waits for the
  button; until then the version installed keeps running. More memory or threads are shown ("Its limits change")
  and need no button; the label beside the version's number and the licence's name may change too. Another
  author's key is never an update, and a version is never replaced by one with a lower or the same number.
  Where a package comes from counts too: a version installed from the index as official is replaced by one listed
  as community, or by one from a file, only with the button ("Changed: Comes from …"), and so is one whose listing
  is from an index older than this hassmic takes (it counts as a file's).
- **Its settings**: what you type into a plugin's setting belongs to that setting. When an update takes the
  setting away, the value is deleted from the Echo (a secret too), and a later setting of the same name starts
  empty. Clearing a server's name puts back the plugin's default where the row has one; the row of a server the
  plugin connects to has none, and cleared it is unset.
- **What a plugin needs of hassmic** (its manifest's `requires`): the basics every plugin uses and each thing it
  asks for, each at a version. A later hassmic only ever adds to each, so a plugin keeps running under every later
  hassmic. One that needs a version this hassmic does not have yet is refused at install with "needs a newer
  hassmic" and what it needs; "Browse plugins" offers the newest version this hassmic runs instead (the row says
  what each version speaks). An update that needs a later version of something it already had installs without the
  action button when that version only says more of the same, and is listed in the dialog ("It speaks a later
  version of the plugin protocol"); one that comes to need a version that lets a plugin do more than what you
  accepted asks for the button, as a new capability does ("More: Speaks net.lan 2, was 1, which lets a plugin do
  more"); an update that skips versions has a line for each one between, each saying what that version brings. The
  install dialog, an update that asks for something anew and the plugin's card say under each thing it may do what
  every later version it needs lets it do on top ("From net.lan 2: ..."). After the Echo fell back to an older hassmic (an update that did not stay), a plugin
  installed under the newer one is kept, with its settings and files, and does not start: its card says why, and
  it runs again once the Echo runs a hassmic that has what it needs. If it is the wake word engine, Amazon's wake
  word listens meanwhile.
- **Copy plugins** (the card at the foot of the Plugins page; needs another Echo this browser is logged in to): a
  row for each plugin, its settings and each file you gave it, a column per Echo. Click the Echo a row should come
  from and press "Copy"; the page installs the plugin first, then the settings, then the files, Echo by Echo, and
  says what came of each. An official plugin you accepted on one Echo with its action button is installed on the
  others **without theirs**, when both Echos are in one network (Echos page: "In the network"), neither is in
  Kiosk Satellite mode, plugins are switched on on the Echo that gets it, and this browser is logged in to both.
  The Echo that gets it checks the package as it checks any, and says so afterwards: a note at the top of every
  page of its settings, and of your other Echos' pages while they show their neighbours and this browser is logged
  in to it ("… was installed on …
  without its action button … The voucher that stood in for the press named …; the browser … brought the
  package"), a line in its log, and the `synced` event of Home Assistant's "Plugin" entity (sent once Home
  Assistant is connected, if it was not). The Echo named there is the one the voucher named: any Echo of your
  network can make such a voucher, so it is not proof of where the plugin came from. The note stays until it is
  dismissed, and the browser that copied the plugin cannot do that by a click: "Dismiss with the action button"
  asks you to **hold that Echo's action button for a second** and let go (on an Echo Show: both volume keys
  together for a second, let go before two). A short press does not dismiss; it does what it always does (answers
  a call, pauses music, starts the assistant), so nothing that makes you press takes the note off. Home
  Assistant's "Button" entity hears no hold for it. Only another browser, one you let in with that very Echo's
  button, sees a plain "Dismiss": log a browser out there and in again with that Echo's button once, and it is
  one (after the update of 2026-11-20 no browser is yet; one that came in through another Echo never is). The
  note names the browser that brought the plugin with the first digits of its key, which System's list of
  browsers shows too: a browser's name is whatever it called itself. An Echo waits for that hold three times an
  hour at most, whichever notes and whoever asks (the page says when the next is due), so that no browser can
  keep the ring asking. A dismissal takes off the notes that showed when you asked, not one that came
  meanwhile. Sixteen such notes can stand on an Echo; with sixteen standing, read or not, it takes no further
  plugin without its button. An Echo that once saw a wrongly high version of a plugin in the plugin index
  asks for its button for every copy of that plugin from then on; only a factory reset forgets it. In every other case (a plugin from a file, an Echo outside the network, plugins switched off there,
  a version the Echo cannot compare with the one it has, a version older than one it has seen in the plugin
  index or than one that was installed on it before, also when the plugin was removed since) that Echo shows the usual dialog, with the reason, and waits for its own button. An update that is what that Echo accepted already needs neither. Secrets are never copied: the row says
  "Not set here" on the Echo that got the plugin. A file both Echos have is not sent again. Whether a plugin is
  switched on is not copied.
- **A hassmic update and your plugins**: an update never needs new builds of your plugins (above), so nothing of
  the plugins holds an online update or a push back, and nothing is fetched for one. An update's self test waits
  for the wake word engine's plugin where it ran before: a wake word plugin that listened before and does not under
  the update (its launcher, its sandbox, a program that is not the one you accepted) makes the update a bad one: it
  does not become the fallback. Nothing goes back for that: the update keeps running, and only the version the
  Echo falls back to stays the one before. It does not wait for a plugin you switched off (or plugins off), one the
  index withdrew, or one that needs a newer hassmic (the Echo fell back). So does a plugin that ran as listed by the
  index and whose listing the update cannot read (a new key of the index) with community plugins off; that update
  moves the listing itself at its first look (above), and becomes the fallback as soon as the plugin listens again
  (the Echo asks every 10 s). Your other plugins wait for that too: at every start they wait for the Echo's self test
  (a few seconds, usually) and start once it passed (with no microphone sound within its 30 s, not in that run). One
  that ran as a file's or by a community listing (community plugins on then) and stops once they are off is your
  choice, as any file's. If root refuses an update, the Online updates row no longer says it is being installed.
- **An update that stops being a wake word plugin** (it drops "hears the microphone all the time") is a new
  install: it waits for the action button, says that the files you gave it and the data it kept are deleted, and
  once it runs they are. Its settings stay.
- **Pushed from the PC**: `scripts/ota-push.sh --plugin <file.hmp> [host]` sends a listed plugin's package (as the
  index's site has it, `packages/<id>-<version>.hmp`) after the update, for an Echo without internet. The Echo takes it
  only as an update of a plugin you accepted there before, under the rule above (listed, the same author, nothing
  new to accept); anything else it refuses, the script prints why, and the settings page is where that is
  installed. Needs `secrets/update.key`, as the push itself.
- **While an install waits for the button** the ring shows three light-blue LEDs going round, not the blue
  breathing of a login: at the Echo you see which of the two your press accepts. On an Echo Show the band shows
  it and the face says "Plugins: the settings page says how to use both volume keys" (the same pattern shows
  while a copied plugin's note waits for its hold; the page says which). It ends with the press, with "Cancel" on the page, or by itself after
  a minute without a press, and then nothing is installed; nothing else ends it, and a browser that is not
  approved cannot ask for the button meanwhile, so the press is the plugin's for as long as that ring shows. If
  a login had the button when you clicked "Install", the page says so instead of sending you to the Echo. Its animation can be replaced like the others ("Plugin
  install" among the [light ring designs](#buttons-and-light-ring)).
- **What is refused**, with the reason on the page: a package whose manifest is not written the way the Echo reads
  it (a number as text or with a fraction where a whole number is meant, a number past 32 bits or longer than 39
  characters, a text with an escaped control character or one that is not UTF-8, a key twice or written with an
  escape, an object where a list is meant, a default that is not of its row's kind or within its limits, `"wake":
  0` instead of `true`, a `requires` that does not name exactly the basics and each thing the plugin asks for, each
  with a whole number from 1), and a plugin that needs a newer hassmic ("needs a newer hassmic: net.lan 2").
- **Storage, the log and a server** (since 2026-11-21): a card
  shows how much a plugin keeps on the Echo ("Its data", within the kB you accepted; it goes when you remove the
  plugin and stays when it is updated, with what the old version saved as it stopped), whether it follows the log
  and how many lines it was given ("Reads the log"; "Does not answer LOG …" there means the plugin was built on an
  older SDK and gets no more of the log: its author's to fix;
  never what was said to the Echo or answered, names of networks, devices, timers or light ring designs, media
  addresses, Amazon's login code, your home's country, Home Assistant's error texts, or addresses of devices; the
  settings page's own log shows such lines marked "(private)", and a wake word plugin's "(sealed)"), and when it last
  connected to its server ("Its server"). The server is a setting on the card: the plugin connects to what you enter
  there and nowhere else, at the ports its install named; nothing until you enter one (a plugin that comes with a
  server of its own is refused). Changing or clearing it starts the plugin again. "Copy plugins" has a row "What it
  keeps" for such a plugin: copied only if you pick it (it is usually about the Echo it was made on), sealed on its
  way, and never from a wake word plugin into one that is not. Both Echos need this version for that: the page says
  "update it first" of one that seals its answer the older way. When a wake word plugin is updated to a version that
  no longer hears the microphone, what it was given and kept is deleted once its old version has stopped, before the
  new one starts. A plugin is told to stop, and has up to 2 s to save, when it is switched off, updated or removed
  and when hassmic itself restarts (an update, a restart from the page, a reboot from the page); not when the
  Echo loses power or hassmic crashes: then it goes on from what it saved last.
- **Music, the network and Home Assistant** (since 2026-11-21;
  the install's lines say each in your words):
  - *Plays sound on this Echo's speaker*: a plugin that is a player (a Squeezebox client, a radio) plays as music,
    beside Sendspin and a phone over Bluetooth: the newest wins, and the one playing before is paused. Its music is
    at the music's volume, quieter while the assistant listens and answers, and on the light ring's music effects.
    The action button pauses it and resumes it, "<wake word>, stop" pauses it, and Home Assistant's media player of
    the Echo offers pause and play while such a plugin is switched on, its state the plugin's (playing, paused).
    Its card says "Plays music" with what it plays.
  - *Changes this Echo's volume*: as the volume keys do (a player whose server sets the volume).
  - *Sees when this Echo listens and speaks*: idle, listening, thinking, speaking, whether the microphones are off
    and the volume; never what was said.
  - *Can be reached from your network, and sends to devices on it*: UDP ports of its own (28940 to 28999, kept for
    it, shown on its card) that hear only devices on your network (the Echo's firewall lets nothing else in), and
    it sends to such devices only. *Searches your network for its server*: one search a second at most, to the
    port its install named (never one of hassmic's own); the answers come to a UDP port of its own, which devices on
    your network can reach while it runs. What a search finds is **proposed** on its card ("Found 192.168.1.20 … Use it?"): only
    your click writes it into the plugin's server, and the plugin then starts again. A plugin never sets its server
    itself, and only an address on your network can be proposed.
  - *Shows in Home Assistant* and *Home Assistant can tell it to*: entities of its own on the Echo's device in Home
    Assistant (sensors, switches, numbers, choices, buttons, events), named "Plugin <length of its id> <id>: <label>",
    for example "Plugin 9 testmusic: Position" (entity id `sensor.echo_dot_3_5695c4_plugin_9_testmusic_position`):
    no entity id of the Echo's own, nor another plugin's, is ever made that way, so none is ever taken for another
    when it is switched off and on again (Home Assistant removes an entity that is no longer listed, and the next
    one to come under the same entity id took it over, history and automations with it). Rename them in Home
    Assistant as you like: it still knows them by the Echo's name for them. And actions
    (`esphome.<node>_p<length of its id>_<plugin>_<name>`, for example `esphome.echo_dot_3_5695c4_p9_testmusic_search`,
    as the Echo's own `drop_in`); an action without arguments can also be a button on its card, and an entity can also
    be a row on its card (since 2026-11-22: its label and what it shows now, "Connected", "21.5 °C",
    read-only, under "What it shows" while the plugin runs and Home Assistant lists it; it shows you what Home Assistant is told anyway, so it asks
    for nothing at install, and an update that adds or takes away such a row needs no press). They are listed while the plugin is switched on (unavailable while it
    does not run) and go when it is switched off, withdrawn, removed or updated to a version without them, or plugins
    are switched off: Home Assistant reconnects to the Echo once for each such change. Only Home Assistant's own
    (keyed) connection can set them or call the actions: until Home Assistant has given the Echo its encryption key
    (it does so as it adopts the Echo) nothing it sets reaches the plugin, and the plugin's card says so.
  - *Plays sound on this Echo's speaker, whenever it likes*: it may start playing at any time, as the newest music
    source; whatever pauses it (the action button, "stop", another source, Home Assistant) silences it until it is
    resumed or starts anew, whether the plugin obeys or not.
- **Calls** (since 2026-11-21; a phone line's plugin, say, written
  by someone else: hassmic ships none). Its lines at install: *Makes and takes calls: a call coming in is answered
  only at the Echo, one goes out only when Home Assistant or the settings page asks. Hears the room during a call*,
  and each action that may start a call says "and have it place a call".
  - A call coming in rings as a Drop In does: the ringtone, the ring's call light, on an Echo Show the plugin's name
    beside the caller's ("Test phone: Mum"). Answer it at the Echo: the action button or the screen's Answer. The
    ringtone stops as you press, and the light shows the call connected while the other side comes in. Nothing
    else answers: not Home Assistant, not the plugin's card, not an automation, and nothing answers by itself. The
    plugin hears nothing of the room until someone has.
  - A call goes out only from one of the plugin's actions marked as placing calls, in Home Assistant (an automation
    too) or with its button on the card: each such call brings the plugin a one-time permission, good for ten
    seconds, for a call going out and nothing else. Such a call shows on the ring and the screen the whole time. An
    automation that calls the plugin's other actions cannot start a call.
  - "Answered only at the Echo" is about the Echo's own ring. What a call going out means on the other side is the
    plugin's business: a phone line's plugin could, for instance, take its "Call" action to pick up a call that is
    ringing at its server and so connect to someone who called first. On the Echo that is a call going out,
    shown from its first moment, which you allowed by calling the action (or by an automation of yours calling it):
    an automation that calls such an action can therefore connect a caller without anyone at the Echo. Mark an
    action as placing calls, and call it from automations, only where that is what you want.
  - While it is connected: the Drop In's sound (the call microphone where the Echo has one, the same echo
    suppression), the music paused, the call's volume where calls have their own. The action button, "<wake word>,
    stop", "hang up" or the screen's End end it, on either side.
  - One call at a time: a Drop In while a plugin's call is on is refused as busy, and a plugin's call while a Drop
    In is on. Do not disturb and the microphones off refuse it; three rings nobody answered within ten minutes stop
    the plugin ringing until the ten minutes have passed.
  - Home Assistant sees the call only through the plugin's own entities (its "Drop In" entities are Drop In's). Its
    card says "Calls" and whether one rings or is on.
- **Your network, the internet, the whole log** (since 2026-11-22).
  Their lines at install name what the plugin may reach: *"Connects to any device on your network (TCP, ports 80,
  443)"* (any address of your home network the plugin names itself, at those ports, or at any port where it says
  "any port"), *"Connects to the internet (TCP, port 443)"* (any address or name on the internet, never an address
  of your home network: a plugin that needs both asks for both; your router's own public address is one on the
  internet and answers like any server there, which can be its settings page or, through a port it forwards, a
  device of your home), and *"Reads this Echo's whole log, including what was
  said"*.
  - A plugin on your network reaches devices by their address only (it cannot have a name looked up), and never at
    the name service's ports 53 and 853: your router answers there by asking the internet, which would be a way out
    for a plugin that should stay at home. Neither reaches the Echo itself, loopback, adb's port 5555, or a
    broadcast. Its card shows when it last connected.
  - "Your network" is not a wall against the internet: a device on it that passes traffic on (a proxy, your
    router's own services, a box that fetches what it is told to) can carry what the plugin sends further. Give
    "your network" to a plugin you would trust with the internet too.
  - The whole log is what the settings page's log shows you, the lines marked "(private)" included, but never a wake
    word plugin's lines. Only plugins the plugin index lists as official may have it: a community plugin or one from a
    file that asks for it is refused, and one whose listing is gone does not start (its card says why).
  - A plugin that hears every request and also has the network or the internet needs your button twice, the second
    time on a screen that says where it could send what it hears ("to the internet and your network"); the plugin
    index lists such a plugin with the internet only as an official one.
- **The buttons** (since 2026-11-22). Its line at install:
  *"Hears presses of this Echo's buttons, as Home Assistant's "Button" does"*. The plugin hears each press and hold
  of the action and volume buttons that Home Assistant's "Button" event entity hears, at the same moment: never the
  press that let in a browser, opened adb, accepted a plugin, ran a withdrawn one anyway or authorized a phone's
  setup, never the hold that took off a copied plugin's note, never a double or triple press
  that skipped a track. It only hears: what the button does on the Echo (the assistant, pause, answering a call)
  happens all the same. It tells the plugin when someone is at the Echo and touches it, as it tells Home Assistant;
  give it to a plugin you would let know that. A wake word plugin cannot have it.
- **A copy of each request** (since 2026-11-22; a plugin that tells
  who is speaking, or keeps a log, written by someone else: hassmic ships none). Its lines at install:
  *"**Hears everything said to this Echo after the wake word or the button**"* (the audio) and *"Reads what was said
  to this Echo and what it answered"* (the text).
  - The audio is exactly what Home Assistant is sent of each request: from the wake word or the button (or a
    follow-up question) to the end of the request, the voices and the room behind them. Never between requests,
    never while the microphones are muted (muting during a request ends its copy), never on an Echo that did not
    answer the wake word. With the wake word in Home Assistant, only from Home Assistant's wake word on.
  - The plugin only listens. It cannot change, delay, end, start or answer a request; Home Assistant answers as
    always, and what the plugin finds goes to Home Assistant through its own entities (an event "Anna spoke", say),
    for your automations. A plugin that falls behind loses parts of its copy; Home Assistant loses nothing.
  - The text is what speech to text understood and the reply (with Wyoming only the words), also while "Last heard"
    and "Last reply" are off. A "stop" the Echo acted on itself is not passed on.
  - **Two presses** for a plugin that hears every request and can also send what it hears somewhere (to a server you
    enter, to devices on your network): after the first, the page shows a screen of its own ("This plugin hears every
    request and can send what it hears to the server you enter."); ask for the second press there and press the
    button once more. The Echo installs nothing without it. A copy from another Echo asks for that second press on
    the Echo it goes to; an update that gives such a plugin a way out asks for both again.
  - Not beside a wake word engine that is a plugin from a file or a community plugin: its false wake words would
    open the microphone to the listener. The Echo refuses to install the listener while such an engine is chosen,
    refuses to choose such an engine while a listener is switched on, and does not start the listener if both are
    there all the same (its card says why).
  - The Plugins page names, on its top, every plugin that hears requests while it runs; each one's card says how many
    requests it had and whether it lost parts of them. Its own lines in the log are private, and so are those of a
    plugin that makes calls.
- **Writing a plugin**: start from the template repository `hassmic-plugin-template` (an example in C and in Rust,
  its build, tests and release; not on GitHub yet, as of 2026-11-21). The plugin index lists it as a community
  plugin once its entry is merged: your repository's release workflow (the template's `release.yml`) builds the
  package from a tag, signs it with your key, attests it and publishes it, and the index lists that release asset
  once the attestation says your repository's own CI built it at the commit the entry names (`make index-check
  RELEASED=<the asset>` in the template checks that and prints the entry; any language whose program keeps the rules).
  A package you upload by hand is not listed, and the index's maintainer reads the release workflow for what it takes
  into the package: nothing from outside that commit (the template's README "Releases" names what that rules out). Nothing of the index is published yet; until then, and for any plugin
  it does not list, yours is installed from a file.
- **Development flag** (on every card, off): for people writing plugins. A system call the sandbox does not allow
  then stops the plugin, and the log names the call, instead of failing quietly. The plugin starts again for it at
  once. Where the Echo's plugin launcher is an older one the card says the flag is not available.
- **In Home Assistant**: while "Plugins" is on, the event entity "Plugin" fires `installed`, `updated` and
  `removed` when a plugin comes, gets a newer version or goes, and `synced` when one was installed without this
  Echo's button, copied from another Echo ("Copy plugins" above; held until Home Assistant is connected). The event names what happened, not the plugin: the Echo's log and its Plugins page
  do. Use it for a notification when something is installed on an Echo.
- **Where a plugin comes from**: "Official" is this project's own: written in a repository of its own, and listed
  by a hassmic release with the key hassmic's updates are signed with.
  Anything else is "From a file, not reviewed": nobody but its author has looked at it. For such a package the
  install dialog names the fingerprint of the key it is signed with (four groups of four, as `plugcheck.py show`
  prints it): compare it with the one the author publishes.
- **What a plugin can do** is what its lines say, and nothing else: it has no files, no network of its own and none
  of hassmic's keys. A wake word plugin hears the microphone all the time, as the wake word engine does, and can
  have nothing besides the files you give it: no way to send anything anywhere.
- **Its settings and files** are on its card: rows as the page's own, but in a frame that says whose words they are.
  A wake word plugin's models are uploaded there ("Add files"), or fetched by your browser from the list the plugin
  names; the plugin looks at each upload and refuses what it cannot use, with its reason.
- **As the wake word engine**: choose it on the Wake word page ("Plugin: …"). Home Assistant's "Wake word" selects
  then list its wake words. When it does not listen, every page says so and Home Assistant's problem sensor "Wake
  word engine" is on:
  - *the plugin is missing* (removed, plugins switched off): Amazon's wake word listens until it is back;
  - *the plugin is installed and down* (crashed, its model does not load): no wake word is heard, the action button
    works, and the plugin is started again by itself, after 1, 5 and 30 s and then every 5 minutes.
- **A plugin that keeps crashing** is switched off after five crashes in ten minutes (not the wake word engine's);
  its card says why, and its switch starts it again. Ended and counted as a crash too: a plugin that answers three
  pings in a row with nothing, one that does not read what hassmic sends it for 5 s, and one (not the wake word
  engine's) that uses more than a whole core for a minute.
- **Up to eight plugins run at once** (since 2026-11-22; two before), as long as the memory they may use is 128 MB
  between them (each counts what its maker says it may use, the "Up to … MB of memory" on its card; an Echo with less
  than 512 MB gets a quarter of its memory). One that finds no place **waits for one**: its card says "Waits for a
  place" and why, and it starts by itself as soon as another stops (the one that waited longest first). The wake word
  engine's plugin always has a place: if there is none when it is to start, the one started last is stopped for it,
  and that one waits for a place in turn.
- **When hassmic itself crashes over what a plugin reported** three times within ten minutes, that plugin is
  started only five minutes after hassmic's next start, each time, and its card says so. Its switch, a new version
  of it, or "Switch plugins on again" start it at once.
- **A plugin this version cannot read** (installed by an earlier one that read its manifest more kindly) keeps its
  card: it does not run, the card says what is wrong with its manifest and the version its listing names (where it
  has one), and "Remove" or a newer version of it are the ways out ("Browse plugins" offers an update only where the
  index has a newer one than that).
- **If hassmic itself crashes** while it reads a plugin's message, the next start counts it and holds that plugin
  back for that run (its switch, off and on, starts it). The wake word engine's plugin is not left off: it waits
  five minutes and is started again by itself, and its card says why meanwhile. After three such crashes within a
  day no plugin starts but the wake word engine's, and the Plugins page says so, with "Switch plugins on again". A
  crash of hassmic that no plugin's message had a part in counts for nothing, and neither does a restart, an
  update or a power cut.
- **The sandbox** has three strengths, shown on the page: complete, without its SELinux wall, without SELinux at
  all, by how old the Echo's system install is. `scripts/install-system.sh` run again brings the newest.
- `PLUGINS=0` in `hassmic.conf` starts hassmic without plugins at all.

## Where software comes from

The System page's card "Where software comes from" says where online updates and plugins come from, and lets you
pick other servers. While anything there is not the project's own, every page of the settings says so at the top:
yellow for a server of your own, red for a signing key of your own or no signature check.

- **Updates**: the project's releases on GitHub by default. Another repository is given as
  `https://<server>/<owner>/<repository>`, on GitHub, Forgejo or Gitea (told apart by the server: github.com is
  GitHub, anything else is taken for Forgejo or Gitea, whose API answers at `<server>/api/v1`). The releases need the
  same tags (`v<version>`, `v<version>-beta`) and assets (`hassmic-<codename>.bundle` and `.bundle.sig`) as the
  project's. The release channel uses the newest release that is not a prerelease, so with prereleases only, only beta
  finds anything. Many Echos do not know newer certificate authorities (Let's Encrypt among them): a server on your
  network may have to be plain `http://`. That is fine for the bundles, whose signature is what counts.
- **Plugin index**: the project's by default. Another site has the same files as the project's (`index.json`,
  `revoked.json` and their `.sig`, `packages/`), as the index's own tool builds them. A change restarts the satellite;
  plugins installed stay. A change of what is trusted (below) also forgets what the Echo kept of the old index (its
  list of withdrawn plugins, its serials); a mirror under the same keys keeps it, so it cannot hand back an older
  list. With a key of your own or no check, the Echos of your network do not pass lists of withdrawn plugins to this
  one or take its own (a mirror under the project's keys still does).

A server alone is set at once and changes nothing of what is trusted: bundles are still checked against the project's
release key and your update key, plugin lists against the project's index key. So a mirror of the project's releases
or index needs nothing else, and an Echo on a fork's builds (which carry the fork's release key) needs only the fork's
server.

For a server of your own there are two more choices, each of which takes a press of the action button after you save
it (as adb does), and turns the warning red:

- **Your signing key** (its public half, 64 hex digits: `od -An -tx1 -v <key>.pub | tr -d ' \n'`), trusted beside the
  project's. Updates signed with it are installed (root keeps the key; the row says "Root has not taken the key yet"
  until it has, a few seconds), lists of your index signed with it count, and what that index lists as official is
  official on this Echo.
- **No signature check**. Updates: no `.sig` is fetched, the Echo signs what it downloads with a key of its own that
  root trusts. Plugin index: its lists without signatures are taken, but each package's own listing is still checked
  against the project's index key or your own: from an index built without any key, plugins can be browsed but not
  installed. Whoever controls that
  update server, or on plain `http://` anyone on the network on the way to it, can then install anything on the Echo.

Plugins still run in their sandbox, and each install still takes the action button. Pushing an update from your PC
(`scripts/ota-push.sh`) still takes your update key only. Taking a key or no check away, or going back to the
project's ("The project's" in the dialog), works at once. Not exported, not copied to other Echos, not in Home
Assistant; a factory reset goes back to the project's. That does not undo what an update installed under your key or
without a check did: it ran as root, and brings its own release key with it (an update of a fork's carries the
fork's). What each choice costs: [security](SECURITY.md#updates).

## hassmic.conf

One file on the Echo, `/data/local/hassmic/hassmic.conf`, read at boot (edit over adb, reboot):

```sh
PROTO=esphome               # or wyoming (port 16700)
ARGS=""                     # extra options, below
#MODE=stock-online          # temporary: stock Alexa online without updates, see the model's install page
#ADB_WIFI=1                 # leave adb over Wi-Fi open, see below
#PLUGINS=0                  # no plugins at all (Plugins, above)
```

| `ARGS` option | Effect |
|---|---|
| `-W 0` | no settings page (port 28931) |
| `-m <pryon.manifest>` | wake word model to start with, until one is picked in Home Assistant |
| `-w remote` | wake word in Home Assistant; the page then cannot change it |
| `-E` | no sound on wake |
| `-D` | the microphone keeps streaming while a reply plays (Wyoming, for a realtime agent such as hermes-talk's Echo surface; the echo canceller keeps the reply out); a wake word during the reply, the Echo's or Home Assistant's (`-w remote`), leaves the reply to the client |
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
  after 30 minutes, when you close it there, and at every reboot. The row says "a press opens adb" with the
  seconds left while the button is adb's; if a login waits for the button at that moment, the page says so and
  asks nothing (see [the settings page](#settings-page) on what waits for the button).
- `scripts/adb-wifi.sh <echo-ip>` on the PC you installed from: proven with your `secrets/update.key` instead of Home
  Assistant. The way in when the Echo is not adopted, lost its key, runs Wyoming, or Home Assistant is down. Needs
  hassmic running. "FAILED busy ..." from it (or from `ota-push.sh`) means the Echo's update port had its two
  connections from your PC's address, or all its places, taken at that moment: run it again; if it stays, another
  machine on the network keeps the port busy.
- `ADB_WIFI=1` in `hassmic.conf`: open for good, until you take the line out (no reboot needed). For development, and
  the only way with `MODE=stock-online`.

`boot.log` says when it opens and closes. If hassmic does not run, or the update key is lost, only USB is left.

## ESPHome or Wyoming

`PROTO=wyoming` in [hassmic.conf](#hassmicconf) makes the Echo a Wyoming satellite (port 16700). What it keeps and loses:

| | ESPHome (default) | Wyoming |
|---|---|---|
| Voice, wake word on the Echo or in Home Assistant, interrupt | ✅ | ✅ |
| Interrupting a reply with Home Assistant's wake word | ✅ | ❌ (the button) |
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

## Echo Dot 5 (Cannoli) development

For Cannoli NS6575/build 8349, follow the [device instructions](../devices/cannoli/README.md). This port requires the separate NAND/UBI installation workflow and isn't part of the upstream guided setup or release matrix yet. The remaining work is tracked in [PLAN.md](../PLAN.md).
