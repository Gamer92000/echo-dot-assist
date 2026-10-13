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
  (experimental, a card under Features on the settings page, off by default) takes it out: the Echo holds back the audio while the tone plays, finds
  the tone in it and turns down only its few pitches, then sends it all at once, so the first second of what you said
  reaches Home Assistant about a second late. When you speak much louder than the tone, it changes nothing. The log
  says what it found each time ("wake sound removal: ...").

- **Nothing said** after the wake word (or the button, or a question the assistant asked): the ring goes dark
  without the error animation, as on a stock Echo.

- **"Alexa, stop"** interrupts a reply, but only right behind the wake word: Amazon's models only listen for "stop" in
  the two seconds after it. Out of silence, "Alexa, stop the music" goes to Home Assistant as a normal command.
  With nothing else to stop, it pauses music from Music Assistant or a phone on the Echo itself (the action button
  resumes it), without waiting for speech-to-text to catch the word over the music. What you say after it still goes
  to Home Assistant ("Alexa, stop the timer"); a bare "stop" does not. When Amazon's model misses the "stop", or with
  microWakeWord or Home Assistant's wake word (no "stop" keyword), a request that is only "stop" does the same: the
  music pauses and Home Assistant gets no request. A phone without remote control (AVRCP) goes silent instead, until
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
settings page, or "Copy models" from another Echo. `scripts/artifacts.sh` from the PC is the old way, kept as a fallback
(it registers the Echo to an Amazon account in the Alexa app for a few minutes and undoes it). Pick one in Home Assistant's wake word select. The same word in two languages
shows with its language and region ("Computer (de-DE)", "Computer (en-US)"). Only the word picked wakes the Echo:
Amazon's English (US) "Computer", "Amazon" and "Ziggy" are one model that also knows the others and "Hey Disney".

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

When Home Assistant hears the wake word, the Echo answers as on its own: the sound, the ring, the front end held on the
talker until the command ends. It costs arbitration between Echos, "<wake word>, stop", the extra sensitivity while
playing, everything while Home Assistant is down, and likely some reliability. Switching to or from it restarts the
satellite for a few seconds. `-w remote` in [`ARGS`](#hassmicconf) does the same, and then the page cannot change it.

## Several Echos

The Echos compare by how clearly the word stood out of the room's noise. An Echo in a conversation, or ringing, keeps
the next wake word. With only one Echo there is no delay. The wait delays only the sound and the ring, not what you
say: the Echo that answers sends Home Assistant everything from the end of the wake word, so speak on without a pause.

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
- **In Home Assistant**: the event entity "Button" fires for each press of the action and volume buttons (`action`,
  `volume_up`, `volume_down`) and once for each held 1 s (`action_hold`, `volume_up_hold`, `volume_down_hold`),
  besides what the button does on the Echo. A press counts when it is let go within the second, so a hold never
  fires both. Nothing fires for both volume buttons held (that pairs Echos) or for the press that approves a login
  or a phone setting up the Wi-Fi. In an automation: trigger "Entity" → the Button event, with the event type as
  condition (`{{ trigger.to_state.attributes.event_type == 'action_hold' }}`). A hold is Home Assistant's alone:
  the volume steps only for a press let go within the second (so a step comes as you let go), the action button acts
  only then too. Held on, the action button still warns at 5 s and resets the Echo at 10 s.
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

  ```yaml
  - action: esphome.echo_dot_3_5695c4_light_ring_leds
    data:
      colors: ["#ff0000", "#ff0000", "", "#00ff00"]
  ```
- **Your own designs** (settings page, Lights): draw animations for the ring and pick, for each of the Echo's own
  (listening, its end, thinking, speaking, not understood, timers, volume, microphones off and on, do not disturb,
  identify, Bluetooth pairing, the Drop In call states, setup, factory reset, page login), Amazon's or one of yours;
  a design is drawn on its own and can show for as many of them as you like ("Use for…").
  Up to 100 designs; each is frames of the 12 LEDs, every frame shown as long as you set, played again and again, once,
  or an opening then a loop. The designer paints LEDs with a brush or the whole ring, turns, mirrors, dims, makes
  spinning, fading and breathing frames, turns the hue of a whole design or a frame (how light each colour looks stays), has undo, and starts from Amazon's
  animation if you like; a preview plays it. "Recolour Amazon's…" on an animation is the short way to Amazon's own in
  other colours.
  Listening and its end turn towards whoever speaks: draw them with the talker where the arrow is, and the Echo makes
  the turned copies itself. Volume takes the colours of the design's first frame through Amazon's steps. Nothing
  changes on the ring until **Apply**: the Echo then writes the designs to its system partition and restarts the
  ring (a second without light). A design goes to other Echos with "Copy models" in the Echos section (into
  their designs only), or as a file (Download, then Upload there). What each animation shows is a setting: "Copy settings" in the
  Echos section copies it, takes the design along where the other Echo has not got it, and applies it there.
- **Designs and theme packs** (Lights, "Designs"): your designs, and packs as folders of them. A pack holds designs
  and says which of them each animation shows; open it to see its packshot, its animations (the same tiles as the
  main card: pick one of its designs, Amazon's, or "not in this pack"; a design from elsewhere comes in as a copy) and
  its designs (opened in the designer from there, or made new in it). A design is in one pack at most: "Move…" takes
  it to another or to the top level. "Save as pack" on the animations card makes one of what the ring shows now.
  Packshots in four styles are drawn from the pack itself, its light on an Echo Dot: Halo (its listening animation large, the next three small), Line-up (up to eight of its animations on a row of Echos, each named), Sequence (four of them as they run, left to right) and Trail (one as a long exposure, its frames rising off the Echo), under "Details" with its
  name, author and a line about it. **Use** sets its animations, only those or all of them (the rest back to
  Amazon's), and applies. A pack travels with its designs: as a `.ringpack` file (Export, Import) or with "Copy
  models". Up to 50 packs; their designs count towards the 100.
- **Listening points at you**, as with Alexa, and also while a timer rings or the light is on (Amazon's ring stops
  turning as soon as a second light shows; hassmic plays the listening ring already turned).
- **Echo Show 5**: no ring; its lights run along the bottom edge of the screen as the ring laid out in a line (the
  talker's side always in the middle: two microphones tell directions apart too poorly to turn it). The band keeps
  that strip of the screen for itself, 10 pixels by default ("Light band" on the settings page; 0 gives the whole
  screen to the apps and shows no band), so an app shown on the screen is never covered. The light "Light ring" works
  there too, in any colour. The screen wakes for the band and sleeps 5 minutes after the last light (1 minute with
  the microphones off or in Do not disturb).
- **Microphone off** plays Alexa's own sounds.
- Silent and dark at boot.
- The ring dims with the room's light as on a stock Echo (Amazon's own logic, on by default); a fixed level from Home
  Assistant switches that off.

## Music

One source at a time, the newest wins: a phone starting over Bluetooth pauses Music Assistant (the whole group), Music
Assistant starting on the Echo pauses the phone. The voice assistant ducks both. "<wake word>, stop" pauses either,
and the action button pauses and resumes.

- **Music Assistant** finds the Echo by itself (Sendspin player). Pair it with the Echo's token: shown at the end of
  `scripts/setup.sh`, and as the diagnostic entity "Sendspin pairing token" in Home Assistant (disabled by default).
  Steps: the model's page, "Optional: Music Assistant". When Music Assistant restarts, the Echo announces itself again
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

### Separate volumes

The settings page's Volume card (Sound) sets the Echo's volume, the same one as the volume buttons and Home
Assistant's media player. Switch it to "Separate volumes" and four kinds of audio get a volume each, 0 to 100 %,
independent of each other:

| | What | In Home Assistant |
|---|---|---|
| Music | Music Assistant, Bluetooth from a phone, `media_player.play_media` (not announcements) | the media player's volume |
| Voice replies | what the voice assistant says, and announcements (also `tts.speak` or `play_media` with `announce` on the media player) | Assistant volume |
| Echo sounds | the wake sound, button presses, Bluetooth and Drop In chimes | Sounds volume |
| Timers | the ring of a finished timer | Timer volume |
| Calls | the other side of a Drop In call (only while Drop In is on) | Call volume |

Voice replies, Echo sounds, Timers and Calls each have "Own volume" or "Follow music" (Calls start on "Follow music",
as before they had a volume of their own). Following the music, the slider sets
how far above or below the music volume it plays, in volume points (timers 20 above music at 40 play at 60), and it moves with
the music: the media player's volume and the volume buttons change both. Home Assistant then has no number for it.
Copying settings to another Echo (Export, the Echos section) takes the switches and the offsets, never a volume itself:
like the Echo's volume, that is for the room it is in.

The same number sounds the same on every slider, and as that volume on a stock Echo (measured on an Echo Dot 2,
within 0.3 dB). The volume buttons change what is playing: a Drop In call, a reply (the ring shows the steps in cyan),
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
While a phone plays, the proxy stops scanning: the radio cannot do both without the music stuttering.

## In Home Assistant

| Always there | |
|---|---|
| Media player | TTS, `play_media`; its volume is the Echo's (the music's, see [Separate volumes](#separate-volumes)) |
| Mute, Do not disturb | DND drops announcements; purple flash when switched on |
| Wake sound | covers all the Echo's own sounds |
| Equalizer | bass, mid, treble, −6 to +6 dB, Amazon's own, on everything the Echo plays |
| LED auto brightness, LED brightness | a level holds the ring there and switches the automatic off (Echo Show 5: the band's brightness, no automatic) |
| Timer ringing, Ringing timers | on while a finished timer rings; their names (diagnostic), see [Voice](#voice) |
| Stop ringing | button: ends a ringing timer and nothing else |
| Button | event: each press and 1 s hold of the action and volume buttons, see [Buttons and light ring](#buttons-and-light-ring) |
| Light ring | light: colour, brightness, effects; the Echo's own lights first, see [Buttons and light ring](#buttons-and-light-ring) |
| Firmware | update entity, see [Updating](../README.md#updating) |
| Identify, Restart, Factory reset | buttons; Restart reboots the Echo |
| Web UI address | diagnostic: the settings page's address |
| Sendspin pairing token | diagnostic, disabled by default |

Time and time zone: the Echo takes both from Home Assistant, over the encrypted connection only (no time before
Home Assistant has given it its key), when it connects and every 6 hours. Change Home Assistant's time zone and the Echo
follows within those 6 hours, or at once when Home Assistant reconnects. Home Assistant says the zone as its rule
(offset and summer time), not its name, so the Echo picks a zone with that rule from its own time zone data: it may
call Europe/Berlin "Europe/Amsterdam", with the same time. Its log stays in UTC.

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
| Request and reply text (off by default) | Last heard (what speech-to-text understood), Last reply (what the assistant answered). Both are cleared when a request starts, so the same request twice is still a change. Home Assistant takes at most 255 characters per state: a longer reply ends in "…". Every request and reply goes into Home Assistant's history while this is on. ESPHome only |
| [Wi-Fi motion](#wi-fi-motion) | motion sensor, sensitivity |
| Light sensor (off by default) | Illuminance (the room's light in lux); "LED brightness" then also follows auto brightness, an entry in the logbook every few minutes. Off, it shows only levels you set |
| Bluetooth audio from phones | Bluetooth pairing |
| [Playing on a Bluetooth speaker](#playing-on-a-bluetooth-speaker) | Bluetooth speaker search, Play on Bluetooth speaker, Bluetooth speaker, Bluetooth speaker delay |
| [Separate volumes](#separate-volumes) (off by default, on the Volume card) | Assistant volume, Sounds volume, Timer volume, Call volume (each while it has its own; calls with Drop In) |

**On the settings page only**:

- "Mic level": how loud speech reaches the voice assistant, −35 to −15 dBFS (default −26); the Echo adjusts its gain.
- "Noise reduction": off by default; low, medium, high take the background down by up to 6, 9 or 12 dB (RNNoise, on
  what the voice assistant gets).
- "Wake sound removal" (experimental, under Features): off by default; takes the wake sound out of what the voice
  assistant gets (see [Voice](#voice)).
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
  relying on it. On the Echo Dot 2 and Echo 2 also when the router changes speed (the Echo Dot 3 and Echo Show 5 allow
  for that).
- **Through a kernel module of ours**: the Wi-Fi drivers do not report what this needs. Loaded only once you switch it
  on (within 10 s), then until the Echo restarts.
- ESPHome only. It does not use the microphones; muting does not stop it.

## Settings page

Works in Wyoming mode too; the "Web UI address" entity shows the address.

**Logging in**: press "Ask the Echo", then the action button within a minute (the ring shows that a login waits; without a press it
goes back after the minute, also when the page was closed).
Revoke approved browsers on the page. Echos in one network trust each other: a
browser logged in on one gets into the others without their buttons ("Log in through <another Echo>"). Debug access
still takes that Echo's own button.

At the top it says what does not work and why (Home Assistant not connected, models missing).

**Echos**: every Echo this one hears (and any you add by address), the arbitration network and those outside it with
the reason and what to do. The page talks to the other Echos directly, at their own addresses on port 28931, also when
you opened it through a reverse proxy. Over plain HTTP only, though: a page that came through an HTTPS proxy may not
call them (the browser blocks it), so open `http://<echo-ip>:28931` for this section.

- **Copy settings**: every setting of this Echo beside the others', differences marked. Tick settings and Echos to copy.
- **Copy models**: wake words, microWakeWord models, whisper and sound detection models, Echo to Echo. A wake word set
  is tried on the receiving Echo's engine first; each Echo that got something restarts its satellite once.
- **Download from Amazon**: wake words, whisper detection, the newer sound detection model, fetched on the Echo itself.
  Sign in with a code you enter on your Amazon site, tick what you want. If an Echo's attestation key ever refuses,
  download on another Echo, or with `scripts/artifacts.sh`, and copy.
- **Models on file** (Wake word section): tick models and "Save" puts them into one `.tar` on your computer
  (`hassmic-models.json` lists them, each model in a folder of its own). "Load from file…" reads such a file and
  installs what you tick on this Echo, checked as a copy is; models already there, the same version, are left out.
  Keep one, and you have your Amazon models even if Amazon stops handing them out. Load only files you saved yourself
  or got from someone you trust: a model runs inside the Echo.

**Export / Import**: `hassmic-settings.conf` (name=value lines, without name, keys and pairings). Import applies it to
this Echo or another, or use it as a preset for the next install (`scripts/setup.sh --preset <file>`).

**Identify** (top of the page, and beside each Echo in the list; also a button in Home Assistant): a rainbow on the ring
for 10 s and stock's setup sound, to tell which Echo is which.

**Speaker gain** (Sound; Echo Dot 2, Echo 2 and Echo Dot 3, and the Echo Show 5): for an Echo that is too
quiet at full volume. It raises the analog gain in front of the speaker (Echo Dot 2 and Echo 2: the codec's "HP Driver
Gain", +0 to +12 dB over stock; Echo Dot 3: the amplifier's output level, +0 to +2 dB, all it has above where Amazon
leaves it; Echo Show 5: the codec's line out, +0 to +12 dB in steps of 1.5 dB, each dB set going to the nearest step), so everything the
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
once more. WPA2 and open networks only.

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
