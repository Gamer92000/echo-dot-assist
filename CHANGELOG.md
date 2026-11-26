# Changelog

What changed for people using the Echo, newest first. Details and measurements are in [PLAN.md](PLAN.md).

## 2026-11-26 — Wi-Fi: moving to a stronger access point

- **Echo Dots and the Echo 2 now move between the access points of a mesh** (issue #234). Before, an Echo stayed on
  the access point it joined first, however weak, until the link broke: replies cut off, the light red when spoken
  to. Now, when the signal stays under -70 dBm for half a minute, the Echo looks for a stronger access point of the
  same network and moves there in about a quarter of a second, keeping its address. Where the access point offers
  it (802.11v), it asks the access point first. On by default: **Move to a stronger access point** (System) switches
  it off.
- **The Wi-Fi row (System) shows the access point** the Echo is on, its band and signal, and the last move between
  access points. Every move is in the log too.

## 2026-11-25 — Phone calls over Bluetooth

- **The Echo as your phone's speakerphone** (issue #233; Echo Dot 2, Echo Dot 3, Echo Dot 5, Echo Show 5, and from
  their firmware also the Echo 2nd gen, Echo Show 5 2nd gen, Echo Show 8 1st and 2nd gen and Echo Input (its jack or
  a Bluetooth speaker), not tried on those yet). Turn on
  **Phone calls over Bluetooth** (Features), pair the phone with Home Assistant's "Bluetooth pairing" switch, and its
  calls ring on the Echo: the action button answers and ends them, "stop" and "hang up" end them, and the call talks
  through the Echo's speaker and call microphone. In Home Assistant: "Phone call", "Phone caller", "Phone", buttons to
  answer, end and take a call here, and an action that has the phone call a number. Off by default. A phone paired
  before must be paired again.
- **The phone's volume keys work during a call**, and the Echo's volume buttons (or Home Assistant) move the phone's
  call volume too.
- **A second call**: a call that comes in during a call shows in Home Assistant's "Phone call waiting"; the action
  button answers it and puts the first one on hold, "Switch phone call" goes back and forth, and ending one brings the
  held one back.
- **Wideband (HD voice) calls** where the phone offers it (mSBC): measured on the Echo Dot 2, Dot 3, Dot 5 and Echo
  Show 5, and on by the same chips on the Echo 2nd gen, Echo Input, Echo Show 5 2nd gen and Echo Show 8.
- **Echo Dot 5: Bluetooth now uses the Echo's own address.** Every Echo Dot 5 had the same Bluetooth address (the
  chip's default, 00:0C:43:29:50:01), so phones and Home Assistant could mix two of them up. A phone paired with an
  Echo Dot 5 before must be paired again.

## 2026-11-25 — Echo Show: Screen off stays off, motion with Wyoming too

- **Screen off from the drawer now stays off** with **Motion wakes the screen** on: someone moving no longer turns it
  back on. It stays dark until you touch it or the Echo wakes it itself (a request, a timer). An update forgets that
  the screen was put out this way, and the next motion wakes it again.
- **The screen's motion switches work with Wyoming too**, and from the moment the Echo starts rather than once Home
  Assistant first connected.

## 2026-11-24 — Echo Show: the screen follows Wi-Fi motion

- **Echo Show: the screen can follow Wi-Fi motion.** Two new switches on the Screen page under **When idle**, shown
  while Wi-Fi motion is on (Features). **Motion keeps the screen on**: the screen goes dark, or shows the clock, only
  once nobody has moved for "Screen sleeps after". **Motion wakes the screen**: a dark screen comes on when someone
  moves, and the clock gives way to the face. Neither in Do not disturb. Both experimental and off by default: Wi-Fi
  motion also sees people in the next room.

## 2026-11-24 — Music in sync through a busy moment

- **A late answer no longer pulls Sendspin's clock off** (Music Assistant's multi-room sync). The Echo asked the server
  for the time every 3 s as soon as its clock looked right, before it knew how fast the two clocks drift apart; an
  answer read late (the Echo busy for a moment) was then believed nearly as much as the clock, and moved it by up to
  tens of ms: a skip in the music, or the drift learnt the wrong way. Now it asks again before the clock could be more
  than 1 ms off by then, a few more questions in the first seconds of a stream. And two answers that arrive at once
  (a server busy for a moment as the Echo connects) no longer set a drift between the clocks of thousands of percent,
  which made the music jump by most of a second.

## 2026-11-24 — Joining Wi-Fi over USB keeps one network

- **`scripts/wifi-join.sh` no longer piles up saved Wi-Fi networks on the Dots** (issue #213). Every run used to add
  the network again and keep the ones before, so an Echo joined a few times had several, and moved between them. Now it
  saves the network only once the Echo is on it, and then removes the ones saved before, as the settings page's
  "Switch network…" does; a join that fails leaves the saved networks as they were. One more run clears an Echo that
  has several already. Also: a `wifi.conf` written on Windows works, and a missing password line is named as such.

## 2026-11-24 — Echo Dot 5: clearer install steps

- **Every step of the Echo Dot 5's install says where it runs** (`devices/cannoli/README.md`, thanks to PR #212): each
  block of commands starts by changing into the checkout it belongs to, so no step depends on where the one before
  left you.
- **Unpacking Amazon's update for the Dot 5 works as written** (issue #221). `tools/payload_dump.py` now takes the
  update file as downloaded (a zip with `payload.bin` inside), and the README unpacks the copy the OS builder already
  downloaded; before, the command failed with an `AssertionError`.
- **The "Unsupported firmware prerequisite" fix is for any firmware older than 8349**, not factory-fresh Dots only
  (issue #216: a Dot from June 2026 needed it too). The light ring fix is a separate one, needed only where the ring
  stays dark after the install.

## 2026-11-24 — A fixed network address

- **The Echo can keep a fixed IP address** (issue #191), set on the settings page: System, "Network address",
  Automatic (from your router, as before) or Fixed, with the address, subnet mask, router and DNS servers filled in
  from what the Echo has now. Handy where the router has no room for another reservation. The Echo tries the address
  first: it makes sure no other device answers there, takes it, checks the router answers, and keeps it only once the
  settings page or Home Assistant reaches it there within two minutes; otherwise it goes back to what it had and the
  page says why. The page follows it to the new address. A fixed address belongs to its Wi-Fi network: switching to
  another network goes back to automatic, and a factory reset forgets it. On every model: the Dots and the Echo 2 use
  Amazon's own static mode of the firmware, the Echo Shows and the Echo Input Android's (the network's own IP
  settings). After a restart a Dot first takes an address from the router for a few seconds, then its fixed one.
- **The guided setup asks for it too** (`scripts/setup.sh`): just before the install, "Fixed address (Enter: from the
  router, as now)", the mask, router and DNS servers offered as the Echo has them. It is set once the satellite runs,
  the same way as from the settings page; the PC running the setup is what reaches the Echo at the new address, so it
  should be on the Echo's network. Setup from a phone (Improv, the Home Assistant app) cannot carry an address: set it
  on the settings page afterwards.

## 2026-11-24 — Updates and plugins from a server of your own

- **Online updates and the plugin index can come from a server of your own** (issue #175). The System page has a new
  card, "Where software comes from": updates from another repository on GitHub, Forgejo or Gitea (a mirror on your
  network for an Echo kept off the internet, or a fork's own releases), and plugins from another plugin index. A
  server alone changes nothing of what is trusted: updates are still checked against the project's release key and
  your update key, plugin lists against the project's index key. For a server of your own you can also name a signing
  key of yours, trusted beside the project's, or switch the signature check off; either needs the Echo's action
  button. While anything here is not the project's, every page of the settings says so: yellow for a server of your
  own, red for a key of your own or no check, which let whoever holds that key (or controls that server) install
  anything on the Echo. Not in exports or Home Assistant; a factory reset goes back to the project's. Until now this
  only worked through test settings in `hassmic.conf`, which could change without notice.

## 2026-11-24 — Dots and Echo 2: Sendspin sees its own output fall behind

- **A Sendspin group of Echos with Amazon's mixer (Dots, Echo 2) can no longer drift apart unseen** (issue #225). On
  these Echos hassmic worked out how much music was still waiting to play from what it had written and the time that
  had passed, assuming the speaker plays at exactly its sample rate. If an Echo's output ran at a rate of its own, or
  the mixer stalled for a moment, that Echo fell behind (or ran ahead) while its log still said "sync err 0.00 ms".
  It now checks that figure against the mixer's own count of what it has not played yet and follows it slowly, so the
  sync works from what the Echo really plays. On an Echo Dot 2 the two agreed to within a millisecond for 25 minutes,
  so nothing changes there; whether it is what made two Echo 2s drift apart is for their logs to show:
  Sendspin's line every 10 seconds now ends in `mixer +x.xx ms`, how far the output fell behind (+) or ran ahead
  since the music started.

## 2026-11-24 — Echo Show: Play works again after AirPlay into its group

- **The Show's music screen no longer says "This Echo has no queue in Music Assistant yet" after AirPlay ends**
  (issue #222). While Music Assistant's AirPlay receiver plays into a sync group, the group has no queue, and as it
  stops Music Assistant regroups the players a moment before it lets go of the AirPlay source, so the Show asked once,
  too early, and never again. It now asks again when the group's (or its own, or its leader's) source changes, and a
  tap on Play, Add or "Play here" while it knows of no queue asks Music Assistant and plays once there is one, instead
  of refusing; only if there is still none after up to 10 seconds does it say so.

## 2026-11-24 — Setup: a first boot that installs the light ring is no failure

- **The guided setup no longer reports "Checking it stays up" failed on an Echo that is fine** (issue #224). On its
  first boot after the install the Echo copies the light ring's animations before the satellite starts (an Echo 2nd
  gen: about two minutes), and the setup took the tool that makes those files for the satellite, then saw nothing
  while they were copied. It now waits for the satellite itself, up to four minutes.

## 2026-11-24 — One word on firmware and the unlock, for every model

- **No list of which firmware versions still unlock.** The Echo Dot 3's warning that recent updates may leave an Echo
  locked (issue #112), and the setup's question before its unlock, are gone. Every model's page, the README and the
  setup's unlock step now say the same thing: the older the Echo's firmware, the likelier the unlock works; updates
  close the holes the unlocks use, an unlock that does not take can leave the Echo unusable, and the Echo must never
  update on the way (no Alexa app, no Wi-Fi until the install puts it there). Whether a version still unlocks is the
  unlock's XDA thread's to say.

## 2026-11-23 — Plugins: an update that may do more asks first; room for any number of plugin features

Plugins are experimental; nothing of the plugin system is released yet.
- **A plugin update that comes to need a newer part of hassmic that lets a plugin do more** (more than the line you
  accepted at install says) now waits for the action button, as an update that asks for a new capability does. One
  that only needs a newer part that says more of the same installs without it, and the update's dialog lists it.
  The dialog says what each newer part brings, one line for each step between the two (before, a jump over several
  showed one step's words); an install, and an update that asks for a capability anew, show beside each capability's
  line what every newer part it needs lets it do on top, and so does the plugin's card. Newer parts that only say
  more of the same, one after another, are one line in the dialog, and the dialog lists the capabilities and the
  settings first: a plugin that comes to need many newer parts no longer pushes a new capability or a new setting it
  asks you for (a password, say) out of the dialog's lines, and where lines found no place the dialog says how many
  ("And 9 more.", not "And more.").
- **Browse plugins no longer offers "Update" where it could only fail**: after the Echo went back to an older hassmic,
  a plugin installed under the newer one shows "Installed, needs a newer hassmic" when the plugin index has nothing
  newer for this hassmic (before, "Update" fetched an older version, which the Echo then refused). The same for a
  plugin whose manifest this hassmic cannot read: its card now says its version where its listing names it, and
  Browse offers an update only of a newer one.
- **Stricter checks of what a plugin says and is told**: a value hassmic or a plugin sends that the other side does
  not know yet goes as the older value it stands for (a new reason a call ended reaches an older plugin as "stopped",
  never not at all), one nobody knows is never sent, and either is refused as broken if it comes. Where no older
  value is right (a new button, which an older plugin must not take for the action button), the message does not
  go to an older plugin at all, and hassmic does not log that as its own error. Only such an event may go unsent so:
  an answer, the end of a call, a state or a reason always reaches the plugin (a plugin waiting for an answer could
  have waited for good, and Home Assistant could have been told an action reached a plugin that never got it), and a
  press of a button an older plugin does not hear no longer counts against the presses it does. The plugins' C and
  Rust clients now say "not spoken with hassmic" for a request of something its manifest does not ask for, apart
  from a broken request; a plugin card that says which parts
  its plugin's client lacks names as many as fit and says "and more". A plugin's manifest
  that uses something of a newer part of hassmic than it says it needs is refused, by the Echo and by the plugin
  index alike. The plugins' C client also holds hassmic's answers to its requests to these rules (it took a broken
  one as the answer).
- **The "Plugins" switch now travels with your other settings**: it is in an export, "Compare & copy" and "make like
  this Echo" copy it, and an import or a preset (`scripts/setup.sh --preset`) takes it, as the "Community plugins"
  switch already was. Switching plugins on installs nothing: on another Echo it runs only what that Echo has
  installed, and every install there still asks for its action button. One import that switches plugins and
  community plugins on and picks a community plugin as the wake word engine takes all three, in that order, whatever
  order the file has them in.
- **Room for any number of plugin features for good**: the first words between hassmic and a plugin no longer hold
  the features as a set of 32 bits (which would have run out); a plugin's own C and Rust clients changed with it, so
  plugins built before today need a rebuild with the new SDK (none was released).
- A plugin whose package this hassmic cannot read is treated as one that hears the microphone all the time when its
  data is copied to another Echo (its data stays sealed), and its card says what it needs of hassmic.
- What a released hassmic carried of the plugin protocol's record can no longer be changed, by deleting it first or
  by adding to it: the build checks the record against every release, the betas of the main branch and the releases
  of the release branch alike (before, only the newest by its date, which could be the other branch's). Where it
  cannot do that, it says so: in a shallow copy of the repository it skips the check (and the release build fails),
  a release whose record cannot be read is named instead of hiding the releases after it, and the release build asks
  once more right before it publishes, so two branches released at once cannot carry one part of the protocol in
  two ways. A release whose record is a link or not a record at all is named too (before, it read as one that
  carried nothing), and a release deleted on GitHub to get out of such a clash no longer stops the next one.
- **What a review found, fixed**: a plugin built the usual way (musl) could not signal itself, so `abort()` crashed
  instead of aborting, `raise()` did nothing, and with the development flag on the plugin was stopped at its first
  `raise()`; it can now (only its own threads: no plugin can signal another or hassmic). A plugin, a list or a file
  named "chunk" or "upload" made the settings page forget, at the next restart, that a request about it had been
  taken already, so one seen on the network could be sent again; the page now goes by the whole address, and no
  plugin may be called after a word of the page's own addresses (upload, install, chunk, ...: the plugin index
  refuses the same names). The plugins' C client answered nothing to a message it does not know (hassmic then
  waited for its time) and could not send a file's description with many long fields (the upload was refused
  after 5 s without a word why); both fixed, and a description it cannot send is now refused with a reason. The
  microphone stream for a wake word plugin now comes in blocks of a size the protocol states (at most 4096
  samples). Another Echo's word that it has a newer withdrawal list is taken only from the local network, and the
  list fetched from it only there. Sizes and positions in the page's uploads are read as the numbers they must be.

## 2026-11-23 — The plugin index: a listed plugin stays until a person takes it off; tighter release builds

Plugins are experimental; nothing of the index is published yet.
- **A plugin the index lists never drops out of it by itself.** Each publish checks every listed plugin's GitHub
  attestation again with the record the index kept when it listed it. If that check says no one day (a new version
  of GitHub's tool, or of its signing service, reading the same record otherwise), the index is not published until
  someone looks: it no longer quietly leaves the plugin out. Taking a plugin off is a person's step, the index's
  withdrawal list. The index also uses one fixed version of GitHub's tool for this, checked by its fingerprint,
  instead of whatever the build machine has.
- **What the index's maintainer looks for in a plugin's release workflow is now written out in full**: nothing of
  the package may come from outside the commit the index names (no file another job or run left behind, unless only
  to compare with, no other repository at a moving version, no cache, no repository variables or extra secrets as
  inputs, no release file already there, no download not checked by its fingerprint), and running an old release
  again has to give the same bytes.
- **microWakeWord's release builds its program itself**, from the tagged commit with the same pinned compiler as its
  checks, and refuses to release one that is not byte for byte what the checks tested (before, it packed what the
  checks left behind). Its compiler download is now checked by its fingerprint, and its tests run against the
  hassmic version its SDK came from, not whatever hassmic's main branch has that day.
- **The release workflows of the plugin template and microWakeWord are now checked step by step**: their tests
  allow exactly the steps the release job has, so a step added that downloads, copies or packs something else fails
  the tests. A release whose checks left no fingerprint of the tested program, or an empty or broken one, now stops
  (before, an empty one was taken as "nothing to compare with"), and the checks stop when the SDK does not say which
  hassmic it came from, instead of testing against hassmic's main branch.
- **The index takes the word of no other version of GitHub's tool**, wherever it runs, and when it cannot check
  several listed plugins it names all of them in one go, instead of stopping at the first.
- **The list of what may not go into a plugin's package from outside its commit** also names other jobs' outputs,
  the event's own data (a tag's message), container images not pinned by their fingerprint, compilers fetched by a
  moving channel name, actions pinned to a commit that is not the action's own (below), and jobs other than the
  release job that get the signing key's environment. Its eight copies (the index, hassmic's docs, the template and
  microWakeWord) are held to the same words by the index's checks: on a PC where the other repositories are beside
  it, and in the index's CI, which now fetches those files and fails where one is missing (before, its CI saw only
  its own two copies and skipped the rest). That check runs on its own beside the publish, so an edit landing in
  one repository before the others, or another repository renamed, shows as a failed check and never holds up a
  publish (or a withdrawal) of the index.
- **The index refuses a release whose workflow runs a borrowed copy of an action.** GitHub runs an action pinned by
  a commit even when that commit exists only in someone's fork, under the original action's name; such a copy could
  swap the plugin's source and still be attested as the plugin's own build. Before it first lists a version, the
  index now reads the release workflow, and the workflows it calls, at the release's commit, and refuses it unless
  every action they use is pinned to a commit of the action's own repository (on one of its branches or tags) and
  every container image to its fingerprint. It does so all the way down: it also reads each action itself at its
  pinned commit, so an action that runs other actions (a "composite" action, which could be the author's own), a
  shared workflow from another repository, and an action built from a Dockerfile are held to the same rule, the
  Dockerfile's base images to their fingerprints. It reads them strictly: a workflow with characters that different
  programs split into lines differently, or YAML features it does not read, is refused instead of read one way here
  and another way by GitHub. If GitHub cannot be asked, or a release would need more than 50 files read, that
  version waits for a later publish or a person; plugins already listed are not held up, and one already listed is
  never taken off by this check (if an action it uses disappears later, the publish stops for a person to decide).
  An action built from a Dockerfile is read only where Docker's builder could not read it another way: a Dockerfile
  with continued lines (a backslash at a line's end), a first line Docker may take as a choice of builder (`//`,
  `#!`, `# syntax=` and the like), a file in JSON, or quotes and backslashes in its options is refused, and so is a
  build step that mounts files from another image than one of its own earlier stages. An action's own file may now
  open with `---` (as actions/setup-python's does), and an action may name another action of its own repository by
  `$/<path>` (GitHub's newer way): it is read at the same commit and held to the same rules. Some honest actions are
  still refused because the index reads more strictly than GitHub (of 43 popular actions, cachix/install-nix-action,
  houseabsolute/actions-rust-cross and pypa/gh-action-pypi-publish); a plugin's author then picks another action or
  writes the step in the workflow itself.
- **A stuck check names what it was checking**: each plugin version the index cannot check right now is said as it
  is found, and once GitHub's tool hung once, the rest are named at once instead of each waiting minutes, so the
  publish never times out with nothing said. Plugins already listed are checked first and against the index's own
  records, which need no GitHub answer, so GitHub hanging on a new version no longer holds up the listed ones; listed
  plugins that do have to be asked of GitHub again (their entry changed) also go before every new version.
- **Tighter release checks in the template and microWakeWord**: each action is held to one exact commit; the checks
  no secret and no signing environment can reach, and that never carry on past a failure; the release's own steps run
  in their tests exactly as GitHub runs them. Those tests are themselves tested: every rule is broken on purpose and
  has to fail with its own message, and each rule switched off in turn has to let some broken workflow through, so a
  rule that stopped working cannot hide behind another. Every rule is now a check of its own (before, several rules
  shared one check, and 47 of the 68 rules in the template's checks and 46 of the 64 in microWakeWord's could each be
  switched off without any broken workflow getting through); this found one check of the template's Rust download
  that could never fail, now mended. Their copy of the index's workflow reader is checked against the index's own.
  The template's Rust build now uses a Rust compiler checked file by file by its fingerprint instead of whatever
  rustup fetches, and its README says how to release a plugin in Rust and what to change in the tests when changing
  the release.
- **A plugin's name, author and description in the index are its own**: the index lists a version only where the
  plugin's manifest says exactly what its entry says, so the list you browse and the plugin's card and install
  dialog on the Echo always agree (a community plugin could call itself "microWakeWord" by "hassmic" on the Echo
  under an entry that said otherwise). No plugin may be called after a word of the Echo's settings page (upload,
  install, chunk, ...). A release workflow whose services or jobs the index cannot read as plain settings is
  refused, never passed over. Each publish still checks every listed plugin again, now several at once, so the
  index can list thousands of versions within the publish's time.

## 2026-11-23 — A hassmic update never needs new builds of your plugins

Plugins are experimental.
- **A plugin says what it needs of hassmic, and every later hassmic gives it that.** Each thing a plugin may ask for
  (its storage, the network, music, calls, the buttons...) and the basics every plugin uses are versioned, and a
  later version only ever adds. So a plugin made once keeps running under every later hassmic, and an update of
  hassmic never waits for, fetches or keeps new builds of your plugins (the earlier way, never released, needed a
  build of each plugin for each hassmic).
- **A plugin that needs more than this hassmic has** is refused at install with "needs a newer hassmic" and what
  it needs; "Browse plugins" offers the newest version this hassmic can run and names a newer one that needs more.
  After going back to an older hassmic, such a plugin is kept, does not start and its card says why; the wake word
  engine's plugin then stands aside for Amazon's wake word, and the older hassmic is not held back by it.

## 2026-11-22 — Plugins may hear the buttons, and show rows of their own on their card

- **A plugin may hear the Echo's buttons**, if it asks: its line at install is "Hears presses of this Echo's
  buttons, as Home Assistant's "Button" does". It hears exactly what Home Assistant's "Button" entity hears, at the
  same moment: each press and each hold of a second of the action and volume buttons, and never the press that let
  in a browser, accepted a plugin or authorized a phone's setup, nor the hold that took off a copied plugin's note.
  It only hears: the button does on the Echo what it always does. It tells the plugin when someone is at the Echo,
  so give it to plugins you would let know that. An update that adds it asks for your button, as any new thing.
- **A plugin's card may show rows of its own**: values the plugin also shows in Home Assistant (a sensor, a switch's
  state, a text), under their names, read-only, updated as the card is, while the plugin runs and Home Assistant
  lists the value. They show you what Home Assistant is told anyway, so they need no press: neither at install nor
  when an update adds or takes one away. One the Echo could not list in Home Assistant is no row: the card says
  "Not listed in Home Assistant yet" for it, as for any other.
- Under the hood: the settings page's unit test of "Browse plugins" now runs with `make unit` (it was left out).

## 2026-11-22 — Community plugins from the plugin index, built by their authors' own CI; ready for a new index key

- **The plugin index can list community plugins**: plugins by anyone, each built by its author's own GitHub CI from
  the commit the index names. The index lists a plugin, community or this project's own, only where GitHub vouches
  for that (an attestation: that repository's own release workflow, the one the index names, run on GitHub's
  machines when the release was tagged, made exactly this file), so a file someone uploaded by hand, or one another
  of the repository's workflows or a run started by hand made, is not listed, and anyone can check it again with the
  command the index's page shows. That says which build made it, not what the code does. When GitHub cannot be
  asked, nothing the index lists drops out of it: the index waits. The index also ran each for a few seconds to see that it keeps to the sandbox's rules: a quick check, not
  a review; the Echo's own sandbox is what holds. Nobody reviews their code, and "Browse plugins" says "Community,
  not reviewed", now with how it was built. Nothing of it is published yet, and community plugins still install only
  once you switched them on.
- **"Browse plugins" says what is broad about a community plugin**, as the index flags it: whether it may connect
  to any device of your network or to the internet, make calls, or hear or read every request, and when the Echo
  will ask for its button twice (a plugin that hears every request and has a way out).
- Under the hood, for a later change of the plugin index's key: a hassmic build can carry a second index key beside
  the first and then takes the index's lists and plugins signed by either. This release carries one key, as before:
  nothing changes on your Echo. When the index's key does change, your Echo moves the plugins it has to the new
  key by itself: it takes the index's word for the very version installed, without the action button and without
  installing anything, settings and files kept, each time you open the Plugins page and with its daily look at the
  index, so also on an Echo whose Plugins page nobody opens ("Browse plugins" shows it on the plugin's row, with
  "Take its listing" to try again where it failed; one that failed for the moment, a full flash, is tried again at
  the next look). A plugin your Echo could not move yet is treated as installed from a file under a hassmic of the
  new key alone, and with community plugins off it does not start: its card and the wake word engine's words say
  why ("from a file", not "a community plugin"). Your Echo then reads the index by itself about a minute after it
  starts, when its network comes back (at most once in five minutes), and again every hour until the plugin is moved
  (one failed look waited a day for the next, the wake word down meanwhile). A version the index no longer lists, or
  lists with nothing your Echo can take, cannot be moved: it stays a file's until you install a version the index
  lists, or switch community plugins on; the hourly look leaves it alone, and the daily look still asks, so a
  version the index lists again is moved within a day. An update that starts with the wake word engine's plugin
  stopped that way does not become the version your Echo falls back to while it is so; nothing goes back: the
  update keeps running, moves the plugin with that look, and becomes the fallback as soon as the plugin listens
  again (it asks every 10 s). Your other plugins wait for that, as they wait for that check at every start (where
  the plugin had run as installed from a file or by a community listing, with community plugins on, and you
  switched them off since, that is your choice, and the update becomes the fallback as any). The list of
  withdrawn plugins your Echo keeps is kept with the new key's signature too, so a hassmic of the new key alone
  still reads it, also when another Echo hands over the old key's copy of the same list afterwards. A list of withdrawn plugins your Echo can no
  longer read (a hassmic of other keys) now says why in its warning.
- No key of hassmic's may be one under which any signature holds (a key of all zeros or a few others like it, in
  any of their spellings): the build refuses such a key for the plugin index or releases, also where it cannot
  check one (no Python), and the Echo takes none as the plugin index's, the release's, the update key or a plugin
  author's. A second index key typed in wrong in this project's own settings stops the build instead of being left
  out without a word.
- For forks with releases of their own: `scripts/fork-release.sh` deletes the repository variables of a plugin
  index its settings no longer name, so CI stops building a dropped key in.
- "Browse plugins" says when an update may ask for the action button twice: where the version you installed already
  hears every request and has a way out, the update asks twice wherever it asks for the button at all (it asks for
  more than you accepted).
- The version your Echo falls back to is only ever one that passed its own check on your Echo: in rare timing (an
  update arriving while the version before still waited for its wake word plugin, in the first 30 s after a start) the new update or the
  running version could become the fallback without that check. An update your Echo handed over while its installer
  was busy for more than a minute (making a light ring's set, say) and that the installer then turned down no longer
  keeps the running version from becoming the fallback, and your other plugins stopped, until the next restart; nor,
  turned down after that minute or replaced by another push before the installer took it, does it leave the next
  update's check without your wake word plugin until a restart, also where a push after it could not be stored
  (the Echo's storage full while the installer unpacked the one before). A push from your PC hears the installer's
  answer to that push, never its late answer to the one before, and gets one where it came while the installer was
  just taking the push before (it heard nothing, and your wake word plugin's check stayed off until a restart). And a wake word plugin you had installed from the plugin
  index's community list no longer keeps updates from becoming the fallback after you switched community plugins
  off, also where its listing is one the new version cannot read.
- A look at the plugin index your Echo makes by itself that fails (the internet down for a moment) no longer takes
  the place of the list on the Plugins page, which said the index was out of reach for an hour without asking
  again. And the daily look no longer reads the whole index day after day for a plugin removed since, or for a
  version the index no longer lists whose listing still holds on your Echo.

## 2026-11-22 — Up to eight plugins at once; plugins may reach your network or the internet, and read the whole log

- **Up to eight plugins run at once** (two until now), as long as the memory they may use is 128 MB between them
  (each counts what its card says it may use; an Echo with less than 512 MB gets a quarter of what it has). A plugin
  that finds no place **waits for one**: its card says "Waits for a place" and why, and it starts by itself when
  another stops. The wake word engine's plugin always has a place, also while it is down for a moment between a
  crash and its start again; a plugin stopped to make room for it waits for a place and starts again by itself
  (until now it stayed off until you switched it on), and being stopped for it never counts as a crash. A plugin
  whose start fails before it runs (its program could not be opened for a moment) is tried again by itself after
  1, 5 and 30 s, then every 5 minutes, as one that crashed: the wake word engine's plugin stayed down for good then,
  and kept the plugins that waited for a place waiting.
- **Three more things a plugin may ask for**, each its own line at install with what it reaches:
  "Connects to any device on your network (TCP, port 80)" (any address of your home network the plugin names, at
  those ports; never by name, and never your router's name service, ports 53 and 853, which would ask the internet
  for it), "Connects to the internet (TCP, port 443)" (addresses on the internet only, never one of your home network,
  which is the line before; your router's own public address is one on the internet and answers like any server
  there: often its settings page, and through a port it forwards, a device of your home), and "Reads this Echo's
  whole log, including what was said". The whole log is for plugins the plugin index lists as official only: any
  other that asks for it is refused, and one that is no longer listed so does not start.
  A plugin that hears every request and has the network or the internet still needs your button twice, the second
  screen saying where it could send it; for your network it also says that a device there that passes traffic on
  (a proxy, your router's own services) can carry it further. One plugin's slow connections or names nobody answers
  no longer hold up another plugin's. A plugin that makes calls has its lines in the log kept private, as one that
  hears requests does.

## 2026-11-22 — Community plugins have a switch of their own; plugins get an SELinux domain of their own

- **Community plugins: a switch of their own on the Plugins page, off by default.** A community plugin is any
  plugin the plugin index does not list as official: its community listings, and any package from a file. Off, the
  Echo installs none of them, whichever way they come (a file, the index, a copy from another Echo, a push), and
  stops the ones it has: they stay installed with their settings, their cards say why, and they run again once you
  switch community plugins on. Official plugins are not affected. It is a setting like the others: on and off with
  a click on the settings page (switching on asks you first), no press needed, since each community plugin still
  asks for the Echo's action button when it is installed. It is exported and copied to other Echos with your other
  settings ("Compare & copy", an import, a preset), and lets nothing in there by itself: each plugin installed there
  takes that Echo's button. Home Assistant cannot switch it. "Browse plugins" shows community listings only while it
  is on, and says how many it leaves out.
- Where this Echo's sandbox lacks its SELinux wall, the page shows, in full, what is missing, what stays in place, and
  how to close the gap on your model (run the system install again; on the Echo Shows that flashes the boot image
  again; the Echo Dot 5 is built and flashed again from its firmware ZIP): when you switch community plugins on, and
  again in the install dialog of every community plugin, so also where an import, a preset or a copy from another
  Echo switched them on.
- An import (or "make like this Echo", Compare & copy, a preset) that switches community plugins on and makes one of
  them the wake word engine now takes both; one that switches them off with such an engine refuses the engine,
  whatever order its lines come in.
- A community plugin that is your wake word engine stops when you switch community plugins off: no wake word is
  heard until you switch them on again or choose another engine, and every page says so. While they are off it cannot
  be chosen as the engine.
- **Plugins get an SELinux domain of their own** on Echos whose system install is from today on (`hassmic_plugin`):
  nothing but what hassmic hands a plugin, its own program, its threads and the pipes it makes itself (which Go's
  runtime needs). It is one domain for all plugins: what keeps them apart from each other is still each one's own
  user and the system call filter. An update does not change an Echo's policy: run the system install again to get
  it. The Plugins page says which of the three levels this Echo has ("complete", "without its SELinux wall", "without
  SELinux at all"). **Not yet tried on an Echo.**
- On an Echo whose system install is older than hassmic's own SELinux domain, the Plugins page now says, plugins or
  not, that hassmic itself would gain from running the system install again.

## 2026-11-22 — Muted means the microphones are off for every request; plugins that hear requests, reviewed

- **Muted, no microphone audio leaves the Echo, also for a conversation asked for before.** Until now, when Home
  Assistant had asked a question back (a conversation that goes on), or the wake word or the action button had cut a
  reply, and you muted the Echo meanwhile, it still listened once the reply was over, muted, and sent what it heard
  to Home Assistant (and to a plugin that hears requests). Home Assistant could also have a muted Echo listen with an
  announcement that starts a conversation. Released builds all did this. Now nothing starts while muted: an
  announcement still plays, the conversation after it does not.
- **With the wake word in Home Assistant, muting stops the stream to Home Assistant.** Released builds kept streaming
  through Home Assistant's mute switch and the page's (the button's mute sent silence), and Home Assistant's wake word
  then started a request. Unmuted, it streams again.
- With the wake word in Home Assistant over Wyoming, the Echo now plays the wake sound and shows the listening ring
  when Home Assistant hears the wake word, as over ESPHome.
  With `-D` (a realtime client that cuts its own replies), Home Assistant's wake word during a reply leaves the
  reply to the client, as the Echo's own wake word does with `-D`, and the request begins there.
- With `-w remote` in hassmic.conf's `ARGS`, any setting changed meanwhile (muting too) saved Home Assistant's wake
  word as your choice: taking `-w remote` out again left the Echo on Home Assistant's wake word. Your own choice now
  stays.
- Plugins that hear requests:
  - The start of a request is no longer lost by a plugin that keeps up: the first seconds (the silence ahead of the
    request, the wake word with "Wake word in the request", a long wait for the other Echos) come all at once, and its
    copy now holds about 6 seconds of them, not 2.
  - With the wake word in Home Assistant over Wyoming, the copy now starts at Home Assistant's wake word (there was
    none), and what Home Assistant heard between requests is passed on as no request's words.
  - What hassmic writes in the log about such a plugin is private too, as the plugin's own lines are, however many
    such plugins were installed and removed before it.
  - A wake word plugin the plugin index withdrew, which you run anyway, counts as one nobody vouches for: no plugin
    that hears requests runs beside it.
  - A plugin copied from another Echo that wants two presses of the action button can now be installed with the
    Echo's own two presses when the other Echo's word for the first no longer holds; the Echo asked for the same
    press again and again.

## 2026-11-22 — Plugins: a copy of each request

Plugins (experimental, off by default) can now be given what Home Assistant hears of a request, to listen in beside
it: who is speaking, a log, a recording for your own training data (hassmic ships no such plugin):
- **A copy of each request's audio** (`request.audio`): exactly what Home Assistant is sent, from the wake word or the
  action button to the end of the request, and nothing else: never between requests, never the wake word engine's
  stream, never while the microphones are muted (a mute during a request ends the copy). With the wake word in Home
  Assistant, only from Home Assistant's wake word on. The plugin cannot change, delay, end or start a request, and
  cannot answer one: Home Assistant answers. A plugin that does not keep up loses parts of its copy; Home Assistant
  loses nothing. The install dialog says it in bold: "**Hears everything said to this Echo after the wake word or the
  button**".
- **What was said and the reply, as text** (`request.text`): "Reads what was said to this Echo and what it answered",
  also while "Last heard" and "Last reply" are off for Home Assistant. A "stop" the Echo acted on itself is not
  passed on.
- **A plugin that hears every request and can send what it hears off the Echo** (to the server you enter, your
  network) **is installed only with two presses of the action button**, the second on a screen of its own that says
  where it can send it. A copy from another Echo takes that second press too, on the Echo it goes to; an update that
  gives such a plugin a way out asks for both.
- **No plugin hears requests beside a wake word engine that is a plugin nobody vouches for** (one from a file, or not
  listed as official): its false detections would open the microphone to it. The Echo refuses whichever comes second
  (the install, the choice of the engine) and does not start the listener where both are there.
- The Plugins page says on its top which plugins hear every request, and each such plugin's card how many requests it
  had. Its own lines in the log are private (what it was told may be in them): no plugin that reads the log gets them.

## 2026-11-22 — Drop In and calls, a second review

- **Drop In: packets sent again by someone on the network are ignored.** Each piece of a call's audio is taken once,
  and only the other Echo's newest can change where the Echo sends its microphone. A device on the network that
  recorded a Drop In's packets and sent them again could cut short bits of the other side's voice, have the Echo send
  its room, still encrypted, to that device instead of the other Echo, and keep a call open after the other Echo had
  gone. Nothing changes between Echos: older ones call newer ones as before.
- A Drop In placed right after another ended, before the Echo had shown the first one's end, is now shown (its
  sound, light and Home Assistant's "Drop In" state); it could pass unseen. And when calls were placed faster than
  the Echo could show them, the end of one could be lost, leaving the Echo unable to make or take another call
  until a restart.
- A plugin whose call audio keeps switching between two numberings no longer fills the log (one line, then one
  every 10 seconds, and a count at the call's end).
- The guide says plainly what "a call coming in is answered only at the Echo" covers: the Echo's own ring. A plugin's
  action marked as placing calls can, on the plugin's side, connect to someone who called first; on the Echo that is
  a call going out that you (or your automation) asked for.

## 2026-11-22 — Plugins: calls, a review

- **A plugin's call coming in is answered only at the Echo**: the action button or the Show's screen. Home
  Assistant, the plugin's card and automations no longer answer one (an action marked as placing calls brought a
  permission that also answered; now it only places a call going out, which shows on the light and the screen the
  whole time). The install dialog says so: "Makes and takes calls: a call coming in is answered only at the Echo,
  one goes out only when Home Assistant or the settings page asks. Hears the room during a call".
- **A plugin could ring unseen right after a Drop In ended**: the Echo showed the Drop In's end after the plugin's
  ring had begun and took the ringtone and the light away from it; the next press of the button answered a call
  nobody had seen. The line now comes free only once the Echo shows the Drop In over.
- **The ringtone stops as you answer** a plugin's call, while its other side comes in; it rang on for up to ten
  seconds, and a second press hung up.
- A plugin's call no longer falls silent for good when the far side's frame numbers jump (one stray number, or a
  phone line's 16-bit numbers wrapping after 22 minutes); a plugin flooding the call's audio with tiny packets no
  longer takes a core of the Echo's time; the first 10 ms of a call no longer carry the room of the call before; a
  long caller name is cut on the Show's screen without a broken character.

## 2026-11-21 — Plugins: calls

Plugins (experimental, off by default) can now make and take calls through the Echo, a phone line's plugin for
example (hassmic ships none):
- **A call rings as a Drop In does**: the ringtone, the ring's call light, on an Echo Show the plugin's name beside
  the caller's. Only a person answers it: the action button, the Show's screen, or an action of the plugin's marked as
  placing calls, from Home Assistant or a button on its card. Nothing answers by itself.
- **The plugin hears the room only while a call is connected**, and a call goes out only when a person asked for it
  (such an action, in Home Assistant or on the card: an automation that calls the plugin's other actions cannot).
  The call's sound is the Drop In's: the call microphone where the Echo has one, the same echo suppression, the music
  paused. The action button, "<wake word>, stop" and "hang up" end it.
- **One call at a time**: a Drop In while a plugin's call is on is refused as busy, and the other way round. Do not
  disturb and the microphones off refuse a plugin's calls; three rings nobody answered within ten minutes stop the
  plugin ringing for a while. Home Assistant sees a plugin's calls only through the plugin's own entities; the Drop
  In entities and the voice blueprint are unchanged.
- The install dialog says it ("Makes and takes calls. Hears the room during a call", and "and have it place a call"
  for each action that does); an update that adds either asks again.

## 2026-11-21 — Plugins: a review of music and Home Assistant's entities

- **A plugin's text can no longer cut Home Assistant off.** A plugin's text sensor that said something that is not
  UTF-8 in one of its subtler forms made Home Assistant drop its link to the Echo, and again at every reconnect,
  voice assistant and all. Every text of a plugin's that Home Assistant, the settings page or the log shows is now
  held to strict UTF-8.
- **A plugin's entities are never another's.** Each gets a key of its own, kept on the Echo (two plugins could share
  one before, and then their states and switches got mixed up; after a factory reset the keys no longer start where
  they started before, which Home Assistant took for the old entities renamed). They are named "Plugin <length of
  its id> <id>: <label>", for example "Plugin 9 testmusic: Position" (`sensor.<node>_plugin_9_testmusic_position`),
  an entity id that none of the Echo's own entities and no other plugin's can have: Home Assistant removes an entity
  that is switched off, and the next one under the same entity id took it over, history and automations with it. An
  action's service is `esphome.<node>_p<length of the plugin's id>_<plugin>_<name>`.
- **Home Assistant's list follows every change**: an update that drops a plugin's entities, a withdrawn plugin and
  plugins that cannot run take them out (they stayed, frozen, before). Only Home Assistant's own (keyed) link can
  set a plugin's switches or call its actions, and until Home Assistant has given the Echo its key the plugin's card
  says that what Home Assistant sets is not taken yet. The top and bottom of a plugin's number slider work; an empty
  text in an action reaches the plugin as empty, not "0".
- **Music plugins can no longer play through a pause.** Whatever pauses one (the action button, "stop", another
  source, Home Assistant) silences it until it is resumed or starts anew, whether it obeys or not, and the button
  goes on to resume it rather than pausing it again and again. One that wrote non-stop while paused, or a few bytes
  at a time, no longer costs the Echo two thirds of a core. A music plugin whose process ended, or that was removed,
  at an unlucky moment could leave the Echo thinking music still played (its wake word less sensitive, sound
  detection off) until a restart: fixed. A plugin setting the volume over and over is held to twenty times a minute,
  and the volume you set in Home Assistant or with the keys is no longer undone by one it set a moment before.
- **A plugin's search stays on the network**: it can no longer reach the Echo's own ports (with Kiosk Satellite's
  arbitration it could keep the Echo from answering its wake word), its install says that the port its answers come
  to is open to your network, it can no longer try an address a second for an open port, and an address that is the
  Echo's own by a route is refused as the Echo's own address is. The firewall keeps a search's echo out from every
  address the Echo has, not only the first.

## 2026-11-21 — Plugins: music, and plugins in Home Assistant

Plugins (experimental, off by default) can now be music players, and show up in Home Assistant:
- **A music plugin** (a Lyrion or Snapcast client, say, written by someone else: hassmic ships none) plays on the
  Echo's speaker like Sendspin and a phone over Bluetooth: the newest one plays and pauses the others, the voice
  assistant turns it down while it listens and answers, the action button pauses and resumes it, "<wake word>, stop"
  pauses it. While one is installed, Home Assistant's media player for the Echo gets pause and play for it, and stop
  stops it. Its card on the settings page says what it plays.
- **The Echo's volume and state**: a plugin may be allowed to set the volume, or to see when the Echo listens and
  speaks (never what was said).
- **Its own network ports**: a plugin may be allowed UDP ports of its own (28940 to 28999, the same at every start)
  that only devices on your network reach (the Echo's firewall and the socket itself drop anything from outside),
  and to search your network for its server. What it finds it only proposes: its card shows "Found 192.168.1.20 …
  Use it?", and nothing is written until you click.
- **Entities and actions in Home Assistant**, named by the plugin: sensors, switches, numbers, choices, buttons and
  events, and actions an automation calls (some also as a button on the plugin's card). They are listed while the
  plugin is switched on, and unavailable while it does not run.
- Each of these is one more line in the install dialog, accepted with the action button as before; an update that
  asks for more asks again.
- For plugin authors: the template repository has a second example, "Tone", a music source that plays a sine, in C
  and in Rust.

## 2026-11-21 — Plugins: a third review's fixes

- **A plugin that is updated** keeps what its old version saved as it stopped (a forwarder's place in the log, say):
  the last fix had refused that, and every update lost it. A wake word plugin updated to one that no longer hears the
  microphone still loses all of it.
- **Plugins get to save when hassmic restarts**: for an update, a restart or a reboot from the settings page, hassmic
  now tells its plugins to stop and gives them up to 2 s first; before, they were ended at once. A power cut or a
  crash still ends them without that.
- A plugin's card says when the plugin does not answer the log it was sent ("Does not answer LOG …"): it was built on
  an older SDK and gets no more of the log until its author updates it.
- Copying a plugin's files to another Echo right after an update that no longer hears the microphone is now refused
  as it should be; copying what a plugin keeps is taken once over a restart also for plugins whose name begins with
  "storage".
- Plugin authors start from a template repository, `hassmic-plugin-template` (an example in C and in Rust).

## 2026-11-21 — Plugins: a second review's fixes

A second review of the plugins' storage, log and server found more, all closed before anything shipped:
- **A wake word plugin updated to one that no longer hears the microphone** keeps nothing of what it had: its old
  version could still write its data for a few seconds after that data was deleted. A plugin removed and installed
  again at once no longer gets data from the removed one.
- **The log**: a plugin can no longer read a hidden line from its middle with a made-up place in the log; a hidden
  line cut at the log's line limit stays hidden; microWakeWord's model names, the server addresses you entered, a
  host that does not answer and a few more names are now marked private.
- **Copying a plugin's data**: each answer is sealed afresh, and a request for it is taken once, also after a
  restart. Copying needs this version on both Echos.
- A plugin that is told to stop while it waits for something stops at once, and keeps what it saves as it ends; a
  plugin that reads the log slowly gets it at its own pace instead of being stopped; a plugin that keeps asking for
  what it may not fills no longer the log; a server name that does not resolve no longer holds other plugins up.

## 2026-11-21 — Plugins: storage, the log and a server, a review's fixes

A review of the change below found holes in it, all closed before anything shipped:
- **The server is yours to enter, only yours**: a plugin whose server or port row comes with a value of its author's
  is refused, and the Echo connects only to what you typed. Before, such a plugin reached its author's server, also
  outside your network, with nothing typed. Changing or clearing the server starts the plugin again, so the server
  you no longer mean hears nothing more; an address that is the Echo itself in disguise is refused.
- **Nothing private through line breaks**: a reply or a request with a line break in it no longer leaks its second
  line to a plugin that reads the log; media addresses (which fetch what was answered), a timer's name when it rings,
  Amazon's login code, the country of your home and Home Assistant's error texts are now marked private in the log.
  A wake word plugin's lines stay hidden from other plugins after it is removed.
- **Copying a plugin's data to another Echo** sends it sealed, and never from a wake word plugin into one that is not.
- The install dialog shows a plugin's network line whole, however long.
- Plugins no longer get stopped for connecting too early or to a slow server, and their client no longer loses log
  lines while it connects. A plugin's data written as it ends is kept.
- The development flag shows as not available where the Echo's plugin launcher is older.

## 2026-11-21 — Plugins: storage, the log and a server

Plugins get what a log forwarder needs (#193; the forwarder itself is left to a community author). All of it is new, experimental and has
not run on an Echo yet.
- **Storage**: a plugin may keep a little data of its own on the Echo (a few kB, as much as you accept at install).
  Its card shows how much; it goes when you remove the plugin. The Echo takes ten writes of it, then one a minute.
- **The log**: a plugin may read the Echo's log. It never gets what was said to the Echo or what it answered, the
  names of your networks, Bluetooth devices or timers, or device addresses; the settings page's own log shows such
  lines marked "(private)".
- **A server you enter**: a plugin may connect to the server you type into its settings, at the ports its install
  named, and nowhere else. The card says when it last connected.
- The install dialog names each of these on a line of its own, and an update that asks for more (more storage,
  another port) waits for the action button as any wider update does.
- **Copy plugins** can copy what a plugin keeps to another Echo, only if you pick that row.
- A **development flag** on every plugin's card: a call the sandbox does not allow stops the plugin and the log names
  it. For people writing plugins.
- The firewall now drops anything a plugin's own user would send, as a wall behind the sandbox.

## 2026-11-21 — The microWakeWord plugin and updates: a second review's fixes

- **An update that breaks a working wake word plugin is a bad update again**: it does not become the fallback when
  the plugin that listened before does not under it, unless you switched plugins or that plugin off. The earlier fix below let an update whose plugin launcher failed pass.
- **Both of your wake words listen right after the first start with this version** on an Echo that used the plugin
  before: the second one no longer waits for a restart.
- An Echo that never had a plugin no longer writes an error to its log at every start, and the plugin's copies of
  your models with the thresholds you set on its card are no longer lost when a start could not read the plugin.
- A threshold just above the plugin's top (0.995) is copied as 0.99, and the note says 0.995.

## 2026-11-21 — Plugins: the list of withdrawn plugins between Echos, said as it is

A review of the earlier change below found that it promised more than it holds.
- **An Echo of your network that misbehaves** (one that has the network's key) can still hold another Echo's newer
  list of withdrawn plugins back, for as long as it keeps at it; it can disturb wake word arbitration, Drop In and
  music ducking in the same way, and could before. It cannot change the list. An Echo that reaches the internet
  still gets the list from the plugin index within a day; one kept off the internet gets it only from the other
  Echos, and warns after a week without word.
- Such an Echo can no longer make an Echo refuse the honest Echos' requests for the list by sending many requests
  of its own.
- An Echo whose record of the last word on that list is in the older form and dated ahead of its clock now warns
  instead of counting the list as fresh.

## 2026-11-21 — The move to the microWakeWord plugin, and updates with plugins: a review's fixes

A review of the move to the microWakeWord plugin and of updates beside plugins found these.
- **The plugin's copies of your models are taken over once**, at the first start after an earlier version, and only
  by the official plugin: models copied later from another Echo can no longer replace the plugin's own copy or
  give it a name nobody checked, and a plugin under that name that is not the official one, or no longer a wake
  word plugin, gets nothing. The plugin checks and names each copy itself. Copies of microWakeWord models between
  Echos now carry the model's two files and nothing else.
- **"Copy the models again"** keeps the threshold and window you set on the plugin's card for a model the plugin has
  already, and says which; a threshold of 0.99 is no longer called outside the plugin's range. While the plugin
  listens, the dialog says that it stays the engine, and "Listen with the plugin" changes nothing. A plugin under
  that name that is not the official one is named with its author key's fingerprint, not only with the name it
  gives itself.
- **An update installed while plugins or the wake word plugin were switched off** becomes the fallback as any
  other: its self test no longer waits for a plugin that does not run.
- When root refuses an update, the Online updates row no longer says the update is being installed.

## 2026-11-21 — Plugins: Echos hand on the list of withdrawn plugins more carefully

Plugins are experimental; a review of how Echos pass on the list of withdrawn plugins found these.
- **The week's warning** ("no word on the list of withdrawn plugins") now counts a list another Echo handed over
  from the day that list was made, not from the day it arrived. An Echo that only ever gets the list from its
  neighbours may show the warning sooner.
- **An Echo of your network that misbehaves** can no longer make another fetch from it hundreds of times a second
  or fill its log, nor stand in for an honest Echo and keep that Echo's newer list away; a request for the list seen
  on the network cannot be sent again.
- **The list never goes back**, also when Home Assistant's clock is more than a day behind after a list was taken
  while the clock was not set.
- The announcement each Echo sends hourly shows anyone on your network that it has plugins, the date of its list
  and the port of its settings page (not which plugins); docs/SECURITY.md says so.

## 2026-11-21 — The move to the microWakeWord plugin copies, and updates with plugins say more

A review of the move to the microWakeWord plugin found these.
- **The move copies your models** into the plugin's own list; the built-in engine keeps its own, as they were.
  Until now both used one folder: deleting a model on the plugin's card deleted it for the built-in engine too, a
  model uploaded again there could keep "Switch back" from working, and a copy or a new version on the built-in
  engine's page dropped what the plugin knew of the model. An Echo whose plugin already used that folder gets the
  plugin's own copies at its first start with this version, and the plugin keeps listening as before.
- **The move switches the wake word engine only from the built-in microWakeWord.** Where Amazon's engine or another
  plugin listens, it copies the models and changes nothing else, then offers making the plugin the engine as a
  separate step that says which wake words the Echo would answer to.
- **Only the official microWakeWord plugin gets your models without a question.** One installed under that name
  from a file or by someone else is named on the card with its author, and the card asks first.
- A threshold the plugin does not take (below 0.50, from a copy or an older version) is brought to its lowest and
  said, and the model is copied, where the card said "not taken" while it had kept it.
- A plugin update that stops hearing the microphone deletes the files it had, the models it was given included, and
  tries again where a file would not go.

## 2026-11-21 — Plugins: withdrawn plugins stay withdrawn

Plugins are experimental; a second review of the list of withdrawn plugins found these.
- **The list of withdrawn plugins never goes back to an older one**, whether or not the Echo's clock is set (an
  older list could be taken after the clock was set, and a plugin withdrawn since ran again), and a hassmic update
  that wants a newer list no longer forgets the one the Echo has: what it withdraws stays withdrawn until the newer
  one comes, and the Plugins page says the list is old.
- **The week's warning** ("no word on the list of withdrawn plugins") now survives restarts, also on an Echo whose
  clock is never set (Wyoming), is not silenced by a Home Assistant clock that is behind, and is not reset by
  another Echo handing over the same list again.
- **Echos hand the list to each other more carefully**: only to Echos of their own network (it told anyone on the
  network whether the Echo has plugins), without following redirects, at most once in ten minutes from each Echo,
  and one slow or misbehaving Echo no longer keeps another's newer list out.
- **Installing or updating a plugin right after the plugin index published** no longer fails with "its size or its
  SHA-256 differs": the Echo looks at the index once more. A list of withdrawn plugins on the index older than the
  one the Echo has no longer stops the whole look.
- "Run it anyway" cancelled for one plugin no longer cancels the wait for another.

## 2026-11-21 — The built-in microWakeWord is deprecated: move to its plugin with one button

- **The microWakeWord built into hassmic is deprecated.** The microWakeWord plugin is the same engine in a sandbox
  of its own, and a later release will take the built-in one out; until then it keeps working as it does. The Wake
  word page says so wherever it can be chosen, and while it listens, Home Assistant has a problem sensor "Built-in
  microWakeWord is deprecated: move to its plugin".
- **"Move to the plugin"** on the Wake word page: switches plugins on if they are off (and back off if nothing
  comes of it), installs the microWakeWord plugin from the plugin index (you see what it may do and press the Echo's
  action button, as for any plugin), hands it all your models with the thresholds and windows you set, and makes it
  the wake word engine listening for the same wake words. Step by step on the card; if anything fails, the built-in
  engine keeps listening. Your models stay, and "Switch back to the built-in engine" undoes it in one click. Nothing
  is moved or installed by itself, also not with an update.
- **A plugin update that stops hearing the microphone** deletes the files and data the plugin had while it did,
  once it runs (it always asked for the action button).

## 2026-11-21 — Plugins: withdrawals reach every Echo, and what a review of the plugin index found

- **Withdrawn plugins reach the Echo by themselves.** While plugins are on and one is installed, the Echo asks the
  plugin index once a day, at a random time, for its list of withdrawn plugins (that file and its signature, saying
  only "hassmic"; GitHub Pages sees the Echo's address), and the Echos of one network hand that list to each other:
  one with internet covers the house. With plugins off nothing is asked or told. The Plugins page shows the list's
  date and when the Echo last had word of it; after a week without, while plugins run, it and the Overview warn.
- **"Run it anyway…"** on a withdrawn plugin's card: with the Echo's action button it runs regardless, until the
  index names it again in a newer list. A list that no longer names a plugin starts it again by itself (it stayed
  stopped until the next start).
- **The index can withdraw official plugins too**, and the Echo stops them. A withdrawn wake word engine leaves the
  Echo without a wake word (never Amazon's), and the page now says to choose another engine or run it anyway
  instead of "until it runs again".
- **An update from a lower tier needs the button**: a plugin installed as official is replaced by a community
  package or one from a file only after a press, whoever brings it. Before, someone with an official plugin's
  author key could replace it from any logged-in browser without one.
- **Old lists are refused**, also by an Echo that never saw the index (new, or after a factory reset): each hassmic
  release takes the index and its revocation list only from a certain date on, and a package listed by an older
  index counts as one from a file. A new index served beside an older revocation list is refused; a list dated
  more than a day ahead of the Echo's clock too (one such list could stop every Echo from taking honest ones).
- **Smaller fixes**: a revocation list longer than an Echo reads is refused whole and said (the rest was dropped
  without a word); the list is kept in one file (a power cut between two could forget it); the version of a
  plugin seen listed (what copies between Echos are held to) counts only versions this hassmic can run; requests to
  the index no longer carry hassmic's version; downloads follow redirects to HTTPS only; the Echo holds far less
  memory for a long index.

## 2026-11-20 — Plugins: browse the plugin index (experimental)

- **"Browse plugins"**, a page of its own under Plugins: everything the plugin index lists, one row per plugin with
  its name, Official or Community, its author, what it says of itself and the version this Echo would install, and
  Install, Update or Installed. Search by name, author, description or what a plugin may do ("network",
  "microphone"); show only what runs on this Echo (on by default; off, the others say why, such as "Needs a newer
  hassmic"); official or community. Before, the Plugins page listed only file names from hassmic's own releases,
  32 at most.
- **The Plugins page shows the plugins installed here first**, each card with "Update" when the index has a newer
  version this hassmic can run, then "Browse plugins" and "Install from file…".
- **Plugins come from the plugin index, not from hassmic's releases.** The Echo reads the index's site itself and
  takes its lists only with the index's signature and never older than one it has seen; a package only with the
  size and checksum the list names, then checked as a file is. Official plugins are the ones the index lists as
  official, with a key every hassmic carries. What the Echo asks for: four files of the index when a Plugins page
  is opened (at most once an hour by itself), and a package when you install one; GitHub Pages sees the Echo's
  address and nothing else.
- **Withdrawn plugins are stopped**: a plugin that is not official (a community one, or one installed from a file)
  that the index withdraws is stopped and kept, its card says why, and it cannot be installed again. Official
  plugins are never withdrawn this way.
- Pushing a plugin with `scripts/ota-push.sh --plugin` takes a package as the index publishes it.

## 2026-11-20 — Security: what two more reviews found in the fixes below

Two reviewers read the fixes of this day and ran what they found on the PC build. None of the builds in between
was released; nothing of it is known to have been used.

- **A press could still let in a browser you never asked for**, in ways the first fixes left: you pressed for a
  login that another device had contested meanwhile (ring dark, your page in a pocket), or a moment after your
  login's minute, and pressed again when nothing happened: the other device's login had been taken in between and
  your second press approved it. Now every press that finds nothing to serve keeps other browsers' logins back
  for another half minute. The same after a restart of the Echo's software (an update, a crash): it forgot that
  anything was waiting, and the first device to ask got the press that was on its way; it now remembers, until
  the Echo is rebooted. A browser that asks right after such a restart may be told to wait up to a minute and
  a half.
- **A device could keep its own login waiting beside your open page** by sending so many requests that your
  page's were no longer counted. A waiting login's own requests are not counted any more, and a request beyond
  the count still makes another browser's waiting login "contested" (ring dark, nobody let in).
- **The note of a copied plugin is dismissed by holding the action button for a second, not by a press.** The
  browser that asks for the dismissal could make you press: it armed the wait and rang a Drop In on that Echo,
  and your press to answer the call took the note off instead. A short press now does what it always does while
  the note waits; hold the button for a second and let go (Echo Show: both volume keys together for a second)
  to dismiss. Home Assistant's "Button" entity hears no hold for that. The page says so.
- **"Dismiss with the action button" three times an hour per Echo**, whichever notes and whoever asks. Before, a
  plugin removed and copied again got a fresh wait each time, and one browser could use the hour up for all.
  What this leaves: a browser that is on an Echo only through another Echo can use the three up, and yours is
  then told "later" for up to an hour; the notes stay, and a browser you let in with that Echo's own button,
  which did not bring the plugin, still dismisses at once.
- **A copied plugin's note names the browser by the first digits of its key** beside its name; System's list of
  browsers shows the same digits. A browser's name is whatever it called itself.
- **No older version of a plugin comes by "Copy plugins" after you removed a newer one.** Removing takes no
  button, so an Echo now remembers the highest version of each plugin it ever installed (until a factory reset)
  and asks for its own button for anything older.
- **A plugin's files are installed whole or not at all.** An update that failed halfway could leave the new
  program under the old description of what it may do, and it ran after a restart. The Echo now puts a package in
  place in one step, and starts no plugin whose program is not the one its description names (its card says so).
- **Updates and adb from the PC**: four addresses that each announced an update and sent nothing kept everyone
  out of the update port again; they no longer do. An update that arrives slowly and loses its place to another
  sender keeps its two seconds of grace for the next try.
- **A phone left connected over Bluetooth after the Wi-Fi came back** kept the ring asking for the button and
  every new browser waiting, without end. The Echo ends that connection after a minute on its network, also when
  the phone keeps asking for the list of networks (a third review: that kept it connected for good). On an Echo
  that is set up, the list of networks now needs the press of the action button, as a new network does.
- **An Echo that is set up could be moved to another Wi-Fi from a phone without a press of its button.** In
  every version so far: when the Echo had been without its network for ten minutes and the network came back, a
  phone in Bluetooth range that was connected to the Echo's setup (connecting needs no button) could drop its
  connection and connect again in the same moment, and the Echo then joined and saved whatever network that
  phone named. The Echo now ends a connection that comes while it is not offering the setup, and takes a new
  network or shows the list of networks only when it is not set up, or within a minute of a press while it is
  offering the setup. One press is good for one change of network. The minute a connected phone is given after
  the network came back is counted once, however often it connects again.
- **The press for a phone's setup is only the one you were asked for** (a further review of that fix, same day,
  nothing released in between). An Echo that had been without its network for ten minutes took every press of
  the action button for the setup, with no phone connected and nothing on the ring: the music did not pause,
  the assistant did not start, and for the next minute any phone in range could change the Wi-Fi. Now the press
  is the setup's only while a phone is connected and the ring shows that it waits; otherwise the button does
  what it always does. **The ring does not tell whose phone waits**: a press while it shows lets whatever is
  connected over Bluetooth in the next minute change the Wi-Fi once. Press when your own app says it is
  connected and asks for the button ([Guide](docs/GUIDE.md#setting-up-from-a-phone),
  [Security](docs/SECURITY.md#bluetooth)).
- **A phone that is connected to the setup and forgotten is not kept**: when nothing has come from it for two
  minutes and nobody pressed, the Echo ends its connection, with or without a network, and offers the setup
  again; the phone can connect again at once. **This does not keep out a device that means to stay**: one in
  Bluetooth range can hold the Echo's one setup connection, and the ring's wait with it, for as long as the
  Echo is without its network, and your own app then cannot find or connect to the Echo. The rule to go by:
  **press only when your own app has asked for the button; if the app cannot find or connect to the Echo
  while the ring shows the wait, do not press.** (An earlier fix of this day ended every connection two minutes
  after it began and turned that phone away for a minute. A further review showed that this stopped nobody who
  came back a second early or under another address, and that it broke Home Assistant's own setup, which
  connects first and asks for the button only after you typed the network's name and password. Never
  released.) A network that went away for a moment every minute no longer keeps a phone connected either: the
  minute on the network is counted together, also when the network is back for less than a second at a time. After a change of network that worked, the phone's connection ends a minute later (it could stay
  for good, and if the new network went away again the Echo could not be set up until that phone left). An Echo
  that is not set up and got its network some other way lets a connected phone go after a minute too. Identify
  from a phone (the sound and the rainbow): once in ten seconds, before the press once a minute.
- **No unknown browser's login starts to wait around a phone's setup**: not while a phone is connected to an
  Echo that is set up, nor in the minute its press opened, during the change of network and the minute after,
  nor for half a minute after the ring stopped asking for the phone. Before, a login asked for right after your
  press for the phone showed the same ring, and a second press ("did it take?") let that browser in. A login
  that was already waiting when a phone connected ends when the ring stops asking for the phone (its page asks
  again by itself): it stayed, and a press meant for the phone a moment after it left let that browser in.
- **An Echo that Home Assistant has just added does not ask for the button for a phone** (a fifteenth review,
  same day, nothing released in between). A device in Bluetooth range could connect to an Echo that was not set
  up yet (that needs no button), stay connected after the Echo had its network by asking for the list of
  networks every few seconds, and be there when Home Assistant added the Echo: from that moment the ring asked
  for the button for it, on an Echo that was online, your press for your own browser's login was refused for
  the browser and given to that device, and it could move the Echo to a network of its own. Now asking for the
  list of networks keeps nobody connected to an Echo that has a network (a minute, as before), and when Home
  Assistant adds the Echo, whatever is still connected from the setup is disconnected; only where a network
  was just given over that setup does the connection stay, for the minute a phone has to read the result (the
  Echo cannot tell which phone it is: whatever is connected in that minute can read it, and the Echo's name,
  and make it identify itself once, and change nothing; a list of networks asked for before is no longer
  sent). The ring asks
  for the button for a phone only on an Echo that is set up and has been without its network for ten minutes.
- **A restart of the Echo's software while the ring asks for the button for a phone** (a crash, an update) no
  longer lets the next unknown browser's login take the press that is on its way: unknown browsers are kept
  back for up to a minute after it. The setup itself is offered again only after ten more minutes without the
  network. While a phone is connected to an Echo without its network, the Echo writes one small file every
  half minute for this.
- **While Home Assistant waits for your press for a phone's setup, the ring goes dark for a moment about every
  minute and a half**: Home Assistant ends its connection after that long and connects again when it asks
  again. A press while the ring is dark is not the setup's (the Echo starts listening); press again when the
  ring is back. Nothing changed here; the guide now says it, where it promised two minutes.
- **A list of networks asked for right as a change of network failed**, after the button's minute, is no
  longer given. And a change of network that works within five seconds of one that failed is counted as the
  network's return at once.
- **A failed change of network from a phone no longer stops the setup for ten minutes.** The Echo joins the
  new network before it knows whether it works; when it did not (no address, a router that does not answer),
  the Echo took that moment for its network's return, and once the phone disconnected to ask for the password
  again it stopped offering the setup. It now goes on offering it.
- **A Bluetooth device connecting to the setup and leaving again, over and over, no longer makes the Echo write
  to its flash each time** (one small file, 118 times in 20 seconds in a test), nor can it hold up the Echo's
  Bluetooth behind the settings page's own writes.
- **Setup from a phone no longer holds up the button and Bluetooth** while the Echo writes the request for a
  new network to its flash, and its timers no longer stand still when the Echo's clock is set back as the
  network returns.
- **A third review of these fixes** (same day, nothing released in between): the hold that dismisses a note
  counts only when it began after the page asked for it, and on an Echo Show only when the two volume keys are
  let go before two seconds; while such a wait shows, that hold is not sent to Home Assistant as `action_hold`
  (an automation on it misses that one). An update pushed from the PC that had fully arrived could be installed
  while the PC was told "busy"; it now keeps its place and is told what happened. A healthy plugin could be shown
  as "not the one its manifest names" when it was started in the moment of its own update. The Echo's memory of
  the highest plugin version it installed now also survives more than 64 different plugins and a failed
  install. The settings page no longer waits for the flash while somebody asks to log in. If you press a moment
  after your login's minute ran out, "Try again" waits half a minute like everyone (the page retries by itself).
- **Smaller**: the settings page starts also when one of its own files was replaced by something that is no file;
  a browser pushed out of the list of 32 no longer counts as let in with the button when it comes back through
  another Echo; a damaged file of notes with more than sixteen entries no longer crashes the page's "dismiss
  all"; Home Assistant hears `synced` for every note it missed, also for more than sixteen; an Echo that cannot
  read its notes takes no copied plugin without its button, and says so in its log. A plugin updated in the
  moment it was started runs with what its new description allows, not the old one's. After two plugins were
  installed while the Echo could not write its memory of versions, copies from other Echos work again once it
  can, without a restart. A browser logged out while the Echo could not write its list of browsers let in with
  the button is off that list after the next start. On an Echo Show the face says
  "Plugins: the settings page says how to use both volume keys" while a plugin or a note waits.

## 2026-11-20 — Security: the action button's press is the one you were asked for

Found by a review, and confirmed by trying it on the PC build. Nothing of it is known to have been used.

- **Security: a press of the action button could approve another browser.** The same button lets a browser into
  the settings page, opens adb over Wi-Fi and accepts a plugin. Until now, a request to log in from any device on
  your network ended whatever was waiting for the button at that moment and took its place: you clicked
  "Install" or "Open" or "Ask the Echo", walked to the Echo and pressed, and the press let that other device's
  browser in instead. A browser that is let in can change every setting of the Echo. This was in every released
  version (for logins and adb), not only with plugins. If you pressed the button for a login, for adb or for a
  plugin and your page then said the button was not pressed in time, look at "Approved browsers" on the settings
  page and revoke what you do not know.
- **Now one thing waits for the button at a time, and a press is not handed from what you were asked to press
  for to another device's login.** A wait ends with your press, after a minute, or when you cancel it on its
  page. The ring shows what waits (a plugin has its own pattern), and the page says what the press will do. And
  after a wait has ended, the Echo takes no login from another browser for half a minute (after a login that
  two browsers asked for at once: until the first one's minute is over, and half a minute more), so a press that
  comes late, or a second press, lets nobody in. A second review found that the first version of this fix held
  for adb and plugins only: a login that had run out, had been approved, or had been asked for by two browsers
  could still be followed at once by another device's, which got the press that was on its way.
- **What this does not do, said plainly.** Any device on your network can ask to log in when nothing else waits;
  the ring then shows a login, the same as for yours, and a press on it lets that device in. Press for a login
  only while your own page says "Waiting… press the action button". And a device on your network can delay new
  logins for as long as it keeps asking (longer than before: at least a minute and a half each time), and with
  three addresses asking as fast as they may, keep any new login from being taken while it does.
- **New words on the page.** On the login page: "something else waits for the action button, or did a moment
  ago" and "another browser asked to log in at the same time" (the Echo lets neither in, the ring goes dark).
  In both cases the page now keeps asking by itself and tells you not to press meanwhile; leave it open, and
  press only once it says "Waiting… press the action button". On Debug access and the Plugins page: "a login
  waits for the action button first", when a login has the button; the page does not send you to the Echo then,
  and asks again by itself when that login is over.
- **Logging in two browsers one after the other** now has a pause of half a minute between them; the second
  page waits by itself.
- **"Close now" for adb from one browser no longer cancels another browser's request to open it** that still
  waited for the button; adb is closed all the same.
- **While a phone sets up Wi-Fi over Bluetooth and waits for the button**, a press goes to the phone, also when
  a browser's login was already waiting; that login ends and its page asks again.
- **Updates and adb from the PC are much harder to keep out.** Someone on the network without your update key
  could keep the Echo's update port busy for minutes at a time, or for good, with a handful of connections;
  `ota-push.sh` and `adb-wifi.sh` then failed without a reason. Each address now gets a small share, an update
  on its way is no longer cut off by connections that only sit there (it was, in the first version of this fix),
  and when the port is busy the scripts say "FAILED busy ..." with why. Someone who floods the port from many
  addresses can still keep a push out while he does.
- **Plugins**: an update that needed no button is no longer installed without one if you removed the plugin
  in the same moment; a plugin (other than the wake word's) over whose reports hassmic keeps crashing is switched
  off after the fifth time instead of being tried every five minutes for good; online updates, HTTPS media and
  Amazon's downloads come back by themselves when the Echo's own library for them failed to load once.

## 2026-11-20 — Plugins copied between Echos: said so that it cannot be hidden (experimental)

A review of "Copy plugins" found no way to get anything but an official plugin onto an Echo with it, but found
that the Echo's word about it was too easy to take back. An Echo of your network that someone else controls could
have copied a plugin and dismissed the note itself.

- **The note about a copied plugin cannot be dismissed by the browser that copied it.** "Dismiss with the action
  button" asks for a press on that Echo. A plain "Dismiss" is only for another browser that you let in with that
  very Echo's button (not one that came in through another Echo).
- **Which browsers count as let in with an Echo's button is kept anew, and starts empty.** After this update
  every browser counts as one that came in through another Echo, until you log it out on that Echo and let it in
  again with that Echo's button (once per Echo). Until then its "Dismiss" asks for the button. The earlier list
  counted everything it did not know as the button's.
- **"Dismiss with the action button" is not there for the asking.** The Echo waits for the button once for a
  note; asked again, it does so once an hour (the page says when). Otherwise a browser could keep the ring
  asking for a press, and the next press for anything would take every note off.
- **A dismissal takes off the notes you saw.** A plugin copied in the same moment keeps its note.
- **The note shows on your other Echos' pages too**, named by Echo, where a page shows its neighbours and your
  browser is logged in to the Echo the note is on.
- **No note is lost**: one for every copy, none replaced by the next, written before the plugin is installed, and
  kept when an install fails after its files were written. With sixteen standing on an Echo, the next copy asks
  for its button. Home Assistant gets its `synced` event also when the note was dismissed before it connected
  and the Echo restarted in between.
- **It says what it knows.** The Echo named in the note is the one the copy's voucher named; any Echo of your
  network could have made it. The note, the log and the documentation now say so.
- **No older version**: an Echo takes no copied plugin older than the newest it has seen among hassmic's releases.
- **With plugins switched off, nothing is copied to an Echo without its button.**
- **Home Assistant's `synced` event waits for Home Assistant**: if it was not connected when the plugin came, the
  event is sent when it connects, also after a restart of the Echo.
- A plugin's secret is no longer accepted when sent unprotected (the page never did); listing an Echo's plugin
  files for a copy no longer holds up the other pages and plugins, and is never cut short.

## 2026-11-19 — Plugins: copy a plugin to your other Echos (experimental)

- **"Copy plugins" on the Plugins page**: a table with your Echos side by side, as "Copy models" has. Click the
  Echo a plugin, its settings or its files should come from, then "Copy".
- **No second button press for a plugin you already accepted.** An official plugin that you accepted on one Echo,
  with its action button, is installed on the other Echos of the same network without theirs. Each of them checks
  the package itself, and tells you afterwards: a note on its settings page until you dismiss it, a line in its
  log, and the event `synced` of the "Plugin" entity in Home Assistant.
- **Everything else still asks for that Echo's own button**: a plugin from a file, an Echo that is not in the
  network, an Echo in Kiosk Satellite mode. The page then shows the usual list of what the plugin may do.
- **Secrets are not copied.** A plugin's secret setting shows "Not set here" on the Echo that got the plugin: set
  it there. Files you gave a plugin are sent only where the other Echo has not got them.

## 2026-11-19 — Plugins: the action button accepts only the package you were shown (experimental)

Found by a review of the plugin code; nothing of it was known to have been used.

- **A press of the action button installs exactly the plugin whose list you saw, or nothing.** With two browsers
  on the settings page, one could slip its own package (one from a file, not reviewed, too) under the other's
  dialog in a narrow moment, and the press then installed that. Now the Echo remembers which browser, which
  package and which install the press is for. While your dialog is open (two minutes) or waits for the button,
  nobody else's package takes its place; if something did come in between, nothing is installed and the page
  says so.
- **Installing from the Plugins page no longer hangs on a slow connection**: the Echo gives a download up after
  five minutes and says so, and "Cancel" frees it at once.
- **The list of official plugins asks GitHub less.** Opening the Plugins page looks at most once an hour, also
  after a look that failed (the page shows why and how long ago); "Look again" asks when you press it.
- **A plugin's install dialog shows its "Source and issues" address** in full, so you see where that link on its
  card will lead before you accept.
- **Pushing updates and opening adb with the update key cannot be blocked for an hour** by another device on the
  network that connects to the update port and sends nothing. A push over a link slower than 32 kB/s is refused.

## 2026-11-19 — Plugins: what an update may change without the action button, once more (experimental)

Found by a review of the plugin code; nothing of it was known to have been used.

- **A plugin's description and its "Source and issues" address are part of what you accept.** An update that
  changes either now waits for the action button and shows the new text or the new address first. Before, an
  update could change the text on a plugin's card (to something like "type your password into Port") or point
  that link at another site without asking. The version's label and the licence's name may still change freely;
  the card now shows them as just that: "version 7 (1.2.0)", "Licence: MIT".
- **A plugin's secret no longer outlives its setting.** When an update takes a setting away, what you had stored
  for it is deleted, and a value is only ever given to a setting of the kind it was typed into. Before, a secret
  could reappear as the value of a later version's text setting of the same name, readable on the page.
- **A hassmic crash that a plugin's report sets off no longer loops.** The third within ten minutes holds that
  plugin's start back for five minutes (its card says why), then it is tried again.
- **The wake word engine's plugin always gets to run**: when two other plugins run already (the most this version
  runs at once), the one started last is stopped for it, and its card says so.
- A server setting no longer takes "-" or "..."; clearing it puts back its default.
- A plugin's folder that this hassmic cannot run anything of (a manifest it cannot read, nothing at all) shows as a card with "Remove".
- A plugin's id may not end in "-" or contain "--" any more.

## 2026-11-19 — Plugins: official plugins from hassmic's releases, on the Plugins page (experimental)

- **The Plugins page lists this project's own plugins and installs them**: "From hassmic's releases", with
  "Install", or "Update" when a release has a newer version than the Echo. The Echo downloads the package itself
  and takes it only if it is listed with this project's release key; then you see what the plugin may do and
  press the action button, exactly as with "Install from file…" (an update that is what you accepted or less
  needs no button). No release has a plugin yet: the list is empty until the first one is published.
- **What the Echo asks GitHub for this, and when**: the list when you open the Plugins page while plugins are
  switched on (once, then not again within an hour) or press "Look"; a package when you press "Install" or
  "Update". GitHub sees the Echo's address, its hassmic version and the package's name. With plugins off and
  nobody pressing those buttons, nothing is asked. An Echo that cannot reach GitHub says so on the page, and
  "Install from file…" stays.
- **`scripts/ota-push.sh --plugin <file.hmp>`** sends an official plugin's package after the update, for Echos
  without internet. The Echo takes it under the same rule (an update of a plugin you accepted, nothing new to
  accept) and tells the script why when it does not.
- **Fixed: an Echo whose wake word engine is a plugin kept that choice only while the plugin was installed.**
  Started without its package (removed, or after an update the plugin was not built for), it listened for
  Amazon's wake word without the warning, and the choice was gone at the next change of any setting. The choice
  now stays and the warning shows, as when the plugin goes missing while hassmic runs.

## 2026-11-19 — Plugins: an update is installed without the button only when it is the same or less (experimental)

- **An update of a plugin asks for the action button for anything that is not what you accepted, or less of it.**
  The rule of earlier today listed what counts as "more" and let the rest through, and the rest was too much: a
  file list could be renamed and take other and far larger files, a number row could be added or renamed into
  "Your alarm system's code", a row's explanation or its preset value could be rewritten, all without the button.
  Now an update installs with one click only when everything a plugin may do and every row of its settings is as
  you accepted it, or when it has less: a capability, a row or a granted entry gone, a smaller quota, fewer or
  smaller files. Everything else is listed ("New: …", "More: …", "Changed: …") and waits for the button, also a
  change this version does not understand. More memory or threads for a plugin are shown in the dialog ("Its
  limits change") and need no button.
- **A wake word plugin is no longer left off after a crash of hassmic itself.** When hassmic crashed while it read
  a plugin's message, that plugin was held back until someone switched it off and on, the wake word engine's too:
  no wake word until then. The wake word engine's plugin now waits five minutes and is started again by itself,
  with the reason on its card. And a crash in hassmic's own handling of a wake word no longer counts as the
  plugin's.
- **Crashes of hassmic that no plugin had a part in no longer switch plugins off.** Only a crash while hassmic
  read a plugin's message counts towards "three: all plugins off".
- **The limit on a plugin's CPU works on an Echo**: the launcher reads how much a plugin used (hassmic itself is
  not allowed to see another user's process on Android) and hassmic ends one that takes more than a whole core for
  a minute, as before on the PC.
- **A plugin whose manifest this version cannot read has a card again**, saying why it cannot run, with "Remove".
  It used to be left out of the page, and could not be removed there.
- **Refused in a plugin's manifest from now on**: text
  that is not UTF-8; a number written with more than 39 characters; a host row preset to something that is no host
  name.
- **The ring keeps showing that a phone's setup waits for the button** when the settings page's own wait ends
  meanwhile, and the other way round: each took the other's pattern off.

## 2026-11-19 — Plugins: in Home Assistant, on the ring, and manifests read as written (experimental)

- **Home Assistant hears when a plugin comes or goes.** While "Plugins" is on, the Echo has an event entity
  "Plugin" with the types `installed`, `updated` and `removed` (and `synced`, for plugins copied from another Echo,
  which nothing does yet): the logbook has each, and an automation can tell you. The event says what happened, not
  to which plugin; the Echo's log and its Plugins page do. Switching "Plugins" now makes Home Assistant reconnect
  for a moment, as every feature does.
- **The ring shows that a plugin waits for the button, and not as a login does.** While an install waits for the
  action button, three light-blue LEDs go round the ring (stock's "wait"; on an Echo Show the band, and the face
  says "A plugin wants in"). A login keeps its blue breathing. It goes off with the press, with "Cancel" on the
  page, and by itself when nobody presses within a minute. The pattern has a slot of its own among the light ring
  designs ("Plugin install").
- **A plugin's manifest is taken as it is written, or the package is refused.** The Echo used to read some values
  as something else than the file said: a number too large for it as a small one, a number written as text as the
  default, a text with an escaped control character as another text, a key written twice or with an escape as
  absent, `"wake": 0` as asked for, a default that fits its row neither in kind nor in range. Each is now a refusal
  that says what is wrong. A plugin that asks for something this version has nothing behind yet (anything but
  files and the wake word) is refused with "this hassmic does not have … yet" instead of being installed and
  given nothing.
- **A plugin whose files reached the Echo by another way than the page is not started** when it breaks the rules an
  install would have held it to (a wake word plugin with a way out, the whole log for an unofficial one); its card
  says why.

## 2026-11-19 — Plugins: updates that ask for less, and whose key a file is signed with (experimental)

- **An update of a plugin needs the action button only when it asks for more.** Until now any change to what a
  plugin may do asked for the button, a smaller quota too. Now the Echo compares the new version with what you
  accepted, point by point: the same or less installs with one click; anything more is listed as "New: …",
  "More: …" or "Changed: …" (a new capability, a larger quota, a new row that takes a password, another name) and
  waits for the button.
- **A plugin installed from a file shows its author key's fingerprint** in the install dialog (four groups of four
  characters), to compare with the one the author publishes.
- **A plugin that hangs is ended sooner**: one that does not read what hassmic sends it for 5 s, and one (not the
  wake word engine's) that uses more than a whole core for a minute. Both count as crashes, as before for one that
  stops answering.
- **If hassmic itself crashes with plugins started**, it notes that and, at the next start, holds back the plugin
  whose message it was handling. After three such crashes in a day only the wake word engine's plugin starts, and
  the Plugins page says so and offers "Switch plugins on again". Restarts, updates and power cuts are not counted.

## 2026-11-18 — Plugins, a first step (experimental)

- **Programs by others can run beside hassmic, each in a sandbox.** A plugin is a feature that its author keeps
  working instead of this project: another wake word engine today, a log forwarder, another music player or a phone
  later. It runs as a user of its own, under a filter that leaves it no files and no sockets of its own making, and
  gets only what you accept when you install it: the settings page lists that line by line, and the Echo installs
  only after you press its action button. Off by default: "Plugins" on the new Plugins page, where a plugin is
  installed from a file (`.hmp`).
- **The first plugin is microWakeWord.** The same engine hassmic has built in, as a program of its own: the model
  you upload is then read where it can reach none of hassmic's keys. Both are there for now; the built-in one goes
  once the plugin has been measured on an Echo. Not yet in the releases. Like every plugin, the project's own
  too, it has a repository of its own (`hassmic-plugin-microwakeword`), and a second one lists the plugins there
  are (`hassmic-plugins`); neither is published yet ([docs/plugins.md](docs/plugins.md), section 13).
- **When the wake word engine is a plugin and it does not listen**, the page says so on every page and Home
  Assistant has a problem sensor "Wake word engine". If the plugin is missing, Amazon's wake word listens until it
  is there. If it is installed and has crashed, no wake word is heard until it runs again (it is started again by
  itself); the action button works.
- **Not tried on an Echo yet.** All of it passes on the PC; the launcher and the sandbox still have to be seen on a
  device. `PLUGINS=0` in `hassmic.conf` starts hassmic without any of it.

## 2026-11-18 — The language you speak, from the right time zone

- **A Portuguese "Alexa" in Brazil finds where the wake word ends by its sounds again.** Since more languages came to
  "Language you speak", an Echo in a Brazilian time zone on Automatic took Portuguese, which the Echo has no sounds
  for, and Amazon's Brazilian "Alexa" none either: it went by the level only. Languages without sounds now count as
  not said, whether picked on the page or from the time zone, so the Echo does as before they were listed: in Brazil
  English sounds. The date on the Echo Show's screen stays in the zone's language. An Echo Show in Mexico, Central
  America or Saskatchewan no longer writes its date in Portuguese. Thanks to giggio (issue #176).
- **"Language you speak" on Automatic now follows the country you are in.** Home Assistant tells the Echo only its
  zone's offset and summer time, which Germany shares with France, the Netherlands and others; the Echo picked the
  first such zone (Europe/Amsterdam for all of them), and on an Echo the time zone did not reach the language at all.
  The Echo now reads Home Assistant's home location (its Home zone) and looks up the country it is in,
  with the borders built into the Echo (from Natural Earth): German sounds for the wake word in Germany, the Echo
  Show's date in German. Only the country is kept, never the coordinates. Within about 3 km of a border, or where Home
  Assistant has no location, the country your Wi-Fi router says decides (most routers send it). With neither, a zone
  set on the Echo before counts, a mere guess never does: then the wake word's own language and an English date, as
  with no zone at all. The settings page shows under "Language you speak" what Automatic found and from what.

## 2026-11-17 — Drop In sounds as Alexa's did

- **Drop In sounds as Alexa's did: the microphone of Amazon's own calls (Echo Dot 3 and Echo Dot 2).** A call sent the
  stream made for speech to text, turned up by the Echo: dull, with much of the room in it. It now sends what Amazon's
  front end makes for calls: its own noise reduction, an even level and its echo canceller for calls (measured on a
  Dot 3: the voice 23 dB over the room, against 17 dB before and 13 dB for the bare microphone; on a Dot 2 the Echo's
  own sound 15 dB further down than in the bare microphone). It is a little duller (little over 4 kHz) and much
  steadier. New setting "Call microphone" (Features, under Drop In): Amazon's call processing (default) or the
  assistant's stream as before. Should the call microphone stop delivering during a call, the assistant's takes over
  within a quarter of a second, and for the rest of the call if it does not come back. The Echo 2 and the Echo Dot 5
  have the same call processing but keep the assistant's stream until it has been tried on them; the Echo Shows and
  the Echo Input keep it too: their audio has no call stream. Thanks to arpena.

## 2026-11-17 — Fire smooth again on an Echo running for weeks

- **Light ring live: the "Fire" effect no longer stutters on an Echo that has been running for a week or more (Echo
  Dot 3, Echo Dot 2, Echo 2 and Echo Dot 5).** Fire's flicker was worked out from the time since the Echo started in a
  number too coarse for it: after about a week it moved in steps, and after about 25 days only four times a second. It
  is smooth again however long the Echo has been up; the other effects were not affected. Thanks to malammar (issue
  #192).

## 2026-11-17 — Fewer lookups of Amazon's hosts on the Echo Shows and the Echo Input

- **Android's own captive portal test switched off: it looked Amazon's hosts up every few minutes (tried on an Echo
  Show 5, 1st generation; the same change, not tried yet, on the Echo Show 5 2nd generation, both Echo Show 8 and the
  Echo Input).** Android on these Echos keeps testing whether the Wi-Fi reaches the internet by fetching a page from
  Amazon's captive portal hosts (mmechocaptiveportal.com and fireoscaptiveportal.com on the Shows). The firewall has
  always stopped the fetch itself, but every attempt still looked the names up first, so the queries left the Echo
  again and again (seen by an owner monitoring the network). Android's test is now switched off (its setting "captive
  portal mode"; Amazon's value is put back when the Echo goes back to Alexa). On the Echo Show 8 2nd generation Fire
  OS may set Amazon's value again at every start, which would undo it there; not known yet. It is set when the
  satellite starts after the update (or at its next start, should Android not answer then) and takes effect the next
  time the Echo joins the Wi-Fi: from the Echo's next restart on (a restart of the satellite alone does not join the
  Wi-Fi again). Android then counts the Wi-Fi as working without testing it. Amazon's own network test may still look
  the names up once in the first minute after a restart, before the satellite stops it. On the Echo Show 5, after a
  restart: no lookup of those names in 40 minutes, where Android had made one every few minutes. Thanks to
  Edowndotdown (issue #189).

## 2026-11-17 — Wi-Fi motion after Wi-Fi off and on

- **Wi-Fi motion: the Wi-Fi driver is hooked again after Wi-Fi went off and on.** On the Echo Dot 3, Echo Dot 5, Echo
  Input, Echo Show 5 (both generations) and Echo Show 8 (both generations) the sensor reads the Wi-Fi driver through a
  kernel module of ours. On an Echo Show 5, Android was seen reloading that driver when Wi-Fi was switched off and on
  (Amazon's watchdog did that every few minutes until the change listed under 2026-11-11): the module's hook went with
  the old driver, no frame was counted any more (37 hours on that Echo), and the sensor never changed again. The module
  now hooks the driver again each time it comes back, and should no frame reach it for a minute, the sensor shows
  unknown and the log says why instead of staying silent. Takes effect after a restart of the Echo (the old module
  cannot be unloaded). Only the log and the sensor's side were tried, on the PC; the module hooking a reloaded driver
  has not run on an Echo yet. Thanks to borky.

## 2026-11-16 — The light ring live

- **The light ring live from Home Assistant (Echo Dot 3, Echo Dot 2, Echo 2 and Echo Dot 5).** New setting "Live from Home Assistant" (settings page,
  Lights, off by default): the Echo draws its ring itself. "Light ring" then shows exactly the colour you pick, fades
  between colours and has twelve more effects: Breathe, Color loop, Comet, Wave, Sparkle, Fire, and six that move with
  the music the Echo plays (Music beat, spectrum, VU meter, orbit, drums, aurora): the Echo hears the beat, the kick,
  snare and hi-hat, left and right itself, and the ring keeps time with what you hear.
  Twelve new lights, one per
  LED, lie over it; actions play a whole picture or an animation of your own frames, which the Echo runs smoothly by
  itself; and your designs play at once, without Apply, from the new select "Light ring design" in Home Assistant or
  "Show on the Echo" on the Lights page (the designer shows a draft that way too). The Echo's own lights (listening,
  replies, timers, mute) always come first, and the ring never gets brighter than Amazon's own lights at the
  brightness it runs at.

- **Echo Dot 5: Home Assistant's light comes back after Amazon's ring program restarts.** It stayed dark until the
  light was changed again.

## 2026-11-15 — Cannoli: Wi-Fi motion

- **Wi-Fi motion (experimental) on the Echo Dot 5**, as on the other Echos: switch it on on the settings page, and
  Home Assistant gets a motion sensor from how strongly the Echo hears your router.
- **The Echo Dot 5's first boot step comes with updates**, not only with a reflashed firmware (and its USB cable),
  once the Echo has firmware built from this version.  If the updated step fails, hangs or does not set up the system
  folder, the Echo does the one it was flashed with as well and starts as usual.
- **An Echo Dot 5 that an update cut off from the network steps back to an earlier version**: when nobody on the
  network (Home Assistant, the settings page, Music Assistant) reaches it on two boots in a row, the next start runs
  an earlier version that worked (in the end the one it was flashed with), and the version taken out is not
  installed again.  Pull the plug a few times instead of opening the Echo.  Boots without a network or before set-up
  do not count once a version has been reached (three in a row at most); Home Assistant down for two restarts in a
  row does, and so do two power cuts in a row before a new version was ever reached.

## 2026-11-15 — Cannoli: the light ring in Home Assistant

- **Echo Dot 5: "Light ring" in Home Assistant**, with its effects, the fill and LED by LED, and the listening ring
  turned towards whoever speaks, as on the other Dots. Its ring holds only a few hundred animations, so the light's
  colours are not made beforehand: the Echo draws each one when it changes into an animation the ring already has.

## 2026-11-15 — Cannoli: light sensor, speaker gain, the mute button at once

- **Amazon's PowerDaemon is stopped with the other cloud daemons on Cannoli.** Its night mode needs the Alexa client,
  which the satellite replaces; without it the daemon crashed and was restarted every 25 seconds, for good.
- **The microphone button reaches Home Assistant at once on Cannoli**, not up to a second later: the satellite looked
  for the button's input device under a name it does not have.
- **The settings page's diagnostics show Cannoli's chip temperature**: they looked for a sensor this model does not
  have.
- **Cannoli's light sensor works**: Illuminance in Home Assistant (with the Light sensor feature on), in lux as
  Amazon's own software computes it, with each unit's factory calibration.
- **Speaker gain on Cannoli**: up to +6 dB over stock on the settings page, for an Echo Dot 5 too quiet at full volume.
- **Cannoli keeps its Wi-Fi awake** instead of in power save: answers on the network came up to 115 ms late in power
  save, 14 ms at most awake, and messages to every Echo at once (wake word arbitration) wait longest of all.

## 2026-11-15 — Setup from a phone gets an address

- **Wi-Fi from the phone's setup (Improv) works on an Echo that has had no network since it booted.** The Echo
  joined the network but never got an address, and the setup failed after 30 s with "no address". Its DHCP client
  either had never been started (the switch took that for a model without one) or had started with no network and,
  asked to try again, kept waiting for a link it never looked at again. It is now restarted for every switch, as at
  boot. Seen on Cannoli after a reset; the settings page's network switch had the same weakness.
- **After a failed try, the phone's next one starts clean.** The Echo kept the failure on show for the next
  connection, and Home Assistant's app, which can read it as it connects, said "unable to connect" before
  sending anything.

## 2026-11-15 — Cannoli's light ring

- **The stock ring animations are back on Cannoli, and the boot swirl ends.** The swirl is the kernel's `boot-anim`
  LED trigger, which stock's LED HAL takes off — the HAL answers only when the SCP co-processor runs, and the
  2022 launch BL2 of a factory-fresh unit never brings the 8349 SCP up. Flashing 8349's `brhgptlk` (raw MTD,
  from TWRP, all four copies readback-verified) starts the SCP and the HAL with it; the launch BL2 is kept for
  rollback. Until then, and on units that keep the launch BL2, the satellite's start takes the trigger off and
  the ring stays dark instead of lying about booting. `ledctrl` calls in `alexa-off.sh` are bounded now: with
  ledcontroller not up they waited on its socket for good, and with them the satellite's start — hassmic never
  came up on a boot where ledcontroller had failed.

## 2026-11-13 — local Cannoli port

- Add an experimental Echo Dot 5 target for NS6575/8349, using its mixer and Pryon.
  Rooted stock A boots with TWRP B retained; Wi-Fi, encrypted Home Assistant
  adoption, Music Assistant pairing and enforcing microphone capture are verified.
  The matching UBI firmware builder lives in a repository of its own,
  cannoli-os-builder (devices/cannoli/README.md).
- Reject the ext4 system installer for Cannoli and retain its compiled stock LED
  animations while the ACS custom-animation format is being investigated.
- Keep Cannoli's immutable init configuration during signed-update promotion;
  refuse incompatible boot changes and include its overlay preparation script.
- Start Cannoli from storage/mixer readiness instead of the Alexa flag that
  lockdown clears. Honor stock-online mode during early boot and preserve DHCP.

## 2026-11-13

- **Whisper detection works again with several Echos.** Since the Echo waits for the other Echos before it answers a
  wake word (October), the whisper detector began listening up to 1.7 seconds after the wake word and missed most of
  the request: on an Echo Dot 3 among five Echos, one whispered request in seven was taken for whispered. It now hears
  what Home Assistant hears, from the wake word's end on, and counts above 500 of 1000 as whispered (Amazon's own
  cut-off; the model names 922): the same seven recordings all came out whispered, eight spoken ones not. "Last
  request whispered" also goes unknown when a request starts, so a request that is not scored (the wake word alone)
  no longer shows the one before's answer, and the answer no longer comes late, after the reply, now and then.
- **Duplex for a realtime agent (`-D`, Wyoming).** With `-D` in `ARGS` the microphone keeps streaming while a reply
  plays, and a wake word during the reply is left to the client: for a client that cuts replies itself, such as
  hermes-talk's Echo surface (`hermes talk echo`), which talks with a realtime voice model straight from the Echo.
  Amazon's echo canceller keeps the reply out of the microphone, and the microphone gain holds while the reply plays
  so it does not turn the canceller's leftover up into something the client hears as speech. The action button still
  cuts a reply. `-D` is ignored with the ESPHome connection. Without `-D` nothing changes. Thanks to patrickhaahr.

- **Bluetooth proxy: each request to a connected device took about 25 ms instead of 100 in a test on an Echo Dot 2
  (issue #167).** Devices that Home Assistant drives through the Echo, such as IKEA's Idasen desk, ask for something
  and wait for the answer several times a second. Once a device is connected, the Echo now asks for the short link
  ESPHome's proxy asks for (a device that asks for a link of its own gets that one). Measured on an Echo Dot 2 with a
  PC standing in for the device, running a loop like the desk's (write, wait 100 ms, read): 300 ms a round before, 150
  ms now; in a short, rough test Wi-Fi was no slower. While Home Assistant talks to a connected device the Echo also
  stops looking for other Bluetooth devices, until a second after the last request (about 30 seconds at a time, then 3
  seconds of looking); in three runs that took a little more off the loop. Not tried with a desk yet. Thanks to
  lebheisenberg for the report.

## 2026-11-12

- **Echo Shows: a clock where the screen used to go dark.** On the settings page's **Screen** page, the new **When
  idle** card has **Clock when idle**: when the screen would turn off (5 minutes after the last thing happened, 1 minute
  with the mic off or in Do not disturb), it shows the time across the whole screen with the date under it instead.
  Until then the face shows as always. A touch on the clock, a request, a timer, the volume or music starting bring
  the face back, and the wait starts again. **Screen sleeps after** sets that wait in minutes (0: never, the face
  stays). With an app or the music page in front the screen goes dark as before. Off and 5 minutes by default.

- **A tighter wall between hassmic and root (issue #130).** If something ever took hassmic over, it could have had
  root overwrite one of root's own files, set system properties (open adb, start any service) or connect to an open
  adb itself, and from there run as root. Root now treats everything hassmic leaves for it as hostile, the firewall
  lets nothing on the Echo but root reach adb, and hassmic runs in an SELinux domain of its own that may set no system
  property. That domain comes with `scripts/install-system.sh` run once more; an update alone keeps the old way.
  Opening adb over Wi-Fi through the ESPHome connection is refused too: only the settings page opens it, with the
  action button. Thanks to kevinsaucier for the report.

## 2026-11-11

- **Music Assistant pairs with the Echo by itself.** With Home Assistant 2026.11 and Music Assistant 2.10.9 or later
  there is no token to copy any more: once the Echo is adopted in Home Assistant, Music Assistant pairs with it within
  a minute and it is ready to play. The Echo tells Home Assistant its Sendspin id, and hands Home Assistant its pairing
  token when asked, over the encrypted connection only (as ESPHome devices do). The "Sendspin pairing token" entity
  stays, for older versions, and now shows the token only over that encrypted connection too: before Home Assistant
  has set the Echo's key it has no state (it used to go to any client on the network then). For this the Echo speaks
  version 1.15 of the ESPHome API (1.10 before); nothing else changes in Home Assistant.

- **Kiosk Satellite's DLNA renderer can be reached from your network (issue #109), optional and off by default.**
  The app can play media sent from phones and media servers, but the Echo's firewall kept them from finding or
  controlling it. New setting "Kiosk Satellite DLNA renderer" (settings page, System, shown once the app is installed):
  switch it on here and the renderer on in the app. The firewall then admits, from private addresses only, the port the
  app's "Server port" names, wherever the renderer moved to or you set it, while the app listens there, and the
  network's searches for renderers on port 1900 while only the app listens there. Everything closes again when the
  renderer stops, the setting goes off or the app is removed. Thanks to rmaffeo, who measured what the renderer needs.

- **The Echo Show can show the music page while a track plays, in place of the face or of the home app.** New setting
  "Music on the screen while it plays" (Screen page, `music_on_screen`, off by default): the Playing tab with the big
  cover, the seek bar and the buttons shows as long as music from Music Assistant plays, until it stops (not at a pause,
  nor in the few seconds between two tracks). "In place of the face" leaves an app in front (Kiosk Satellite, a
  dashboard) alone; "In place of the home app too" also comes over the app chosen as Home, which comes back when the
  music stops. A request, a call or a ringing timer bring the face as before, and the music page comes back when they
  are over. It goes by itself only while it still shows the Playing tab, so browsing the library when the queue ends
  keeps you there. The drawer's Face or Home, the face button on the music page, or going back to the app, hold that
  until the music has stopped. Contributed by M0ebiu5 (#163).

- **A note of your own on each Echo, and your other Echos one click away on the settings page (issue #153).** Several
  Echos of one model differed only by the six characters at the end of their names. Click "Add a note" under the
  name at the top of the menu (or the Note row on the System page) and write where it stands, say "Kitchen, by the
  window". The note shows under the name there, in the browser tab's title, on your other Echos' pages and on the
  login page, so you know which Echo's button to press. Anyone on your network can read it, as the name. It stays
  with that Echo: not in the settings file, and a factory reset removes it. The name at the top now opens a list of
  your other Echos with their notes: pick one and its settings page opens on the same page you are on. Suggested by
  baudneo.
- **More languages for the Echo's own words, Romanian among them.** "Language you speak" (settings page, Wake word)
  now also lists Italian, Portuguese, Dutch, Swedish, Danish, Norwegian, Finnish, Polish and Romanian; automatic picks
  them from your time zone too (Romania and Moldova: Romanian). The Echo Show writes the date on its screen (the clock,
  the drawer) in that language: "Miercuri, 11 noiembrie". Finding where the wake word ends works as before for these
  (there are no sound models for them yet: the wake word's own). Bluetooth announcements can be in Romanian ("Conectat
  la …"). "Oprește" stops like "stop", and "închide" / "închide apelul" ends a Drop In like "hang up"; these words are
  now recognised with or without their accents and capitals in every language ("Închide", "inchide", "Arrête"). The
  Drop In blueprint has Romanian sentences ("conectează-mă cu bucătăria") and replies: re-import it to get them. In
  English the clock's date now reads like the drawer's: "Wednesday, 1 November" (was "Wednesday 01 November").
- **Echo Show 5: Wi-Fi no longer drops every few minutes.** Amazon's Wi-Fi diagnostics, still running inside Android,
  test the connection against Amazon's servers; the satellite's firewall blocks them, so the test always failed and
  the diagnostics then disconnected and reconnected the Wi-Fi (a few seconds each time, every link to Home Assistant
  and Music Assistant dropped, music stopped and restarted), and their watchdog turned the Wi-Fi off and on when the
  test never passed. The satellite now switches both off. The other Echo Shows and the Echo Input have the same
  diagnostics and get the same change, not tried on them yet.

## 2026-11-10

- **"Stop" can end a timer ringing on another Echo (issue #161), optional and off by default.** A ringing Echo
  always wins the wake word round, but only if it heard the wake word itself. From the next room, or over its own
  ring, it may not: then another Echo got the "stop", paused its own music or handed the word to Home Assistant, and
  the timer rang on. New setting "A stop another Echo heard ends the ring" (Voice page, Timers card,
  `timer_stop_from_others`): switched on, that Echo's ring stops on "<wake word>, stop" said to any Echo of its
  arbitration network, as it does when you say it to the ringing Echo itself. The Echo that heard you then leaves its
  music alone and starts no request. It reaches the whole house: a "stop" in the bedroom ends the kitchen's timer. A
  "stop" that has something to end where it was heard (a ring, a reply, a call there) ends only that, and "Any request
  stops a ringing timer" stays with the Echo that heard you. The Echos tell each other over their own network, without
  Home Assistant; the Echo that hears you needs this version too. The log of the one that heard says
  `stop: nothing here, <name> rings: told to stop`, the ringing one's `alarm: off (stop heard by <name>)`. Not in
  Kiosk Satellite's arbitration mode. Suggested by redeye86.
- **Music also drops on the other Echos that heard you (issue #165).** With two Echos in one room playing the same
  music, only the one answering turned its music down; the other played on at full volume over your command and the
  reply. Now every Echo that heard the wake word turns its music down by its own "Music ducking" until the answering
  Echo is done. An Echo in another room that did not hear you plays on. The Echos tell each other over their own
  network, with no Home Assistant automation and no change to the volume itself: Music Assistant and the light ring
  see nothing of it. New setting "Duck for other Echos" (Sound page, `duck_for_others`, on by default); it needs
  wake word arbitration on. Both Echos need this version. A follow-up question keeps them down for your answer too;
  music Home Assistant plays on the answering Echo afterwards (a radio you asked for) does not. If the "done" message
  is lost, the music comes back after about 7 seconds. The answering Echo's own music now also drops from the wake word on rather than once the Echos have
  agreed which one answers (up to a second later). Suggested by nima-1102.
- **A freshly installed Echo went without the wake word aligners until its first update.** The files that find where
  the wake word ends (since 2026-10-29) came with every update, but the installer (`scripts/setup.sh`,
  `scripts/install-system.sh`, `scripts/install-boot.sh`) left them out. Such an Echo found the word's end by the
  loudness alone, the rule the aligners replaced: on an Echo Dot 3 it took up to 0.36 s after Amazon's end of the word
  for the word's tail and left it out of the request. The installer brings them now. An Echo installed since
  2026-10-29 that has had no update since gets them with its next one, at once with `scripts/ota-push.sh`, or by
  installing again; its log says `wake: ... aligned by ...` with them, only `wake: ... ms of the word's tail left out`
  without. Where the system partition is nearly full, an update no longer takes them off one time and puts them back
  the next: those already there stay. Found by derFinlay, who also wrote the first fix (#160).
- **Echo Show 8 2nd gen: Adaptive colour can be switched off or on from the settings page.** Under **Screen**, the
  **Brightness and colour** card has the stock display setting that warms the screen's colours with the room's
  light, often turning a dashboard yellow at night. It is off by default; an explicit choice is kept across restarts.
  Setup presets keep the choice even before the display service starts. The switch shows the device's confirmed
  state and appears only while the display service is available. Returning
  to stock Alexa restores the original setting (issue #162).

## 2026-11-09

- **A stop from Home Assistant before the sound starts no longer leaves the Echo silent.** When the media player's
  stop came while the Echo was still waiting for what to play (a `media_stop` right after `tts.speak` or a
  `play_media`, or during a reply Home Assistant was still preparing), and that then failed to load or was an MP3,
  the Echo played no reply, announcement or media again until hassmic restarted. Now the stop cancels only what was
  on its way, at once: what is sent right after it plays.
- **Debug access: revoking the browser that asked for it withdraws the request.** While Debug access waited for the
  action button, revoking that browser's login on the settings page left the request waiting: the page still said so,
  and the next press of the button opened adb over Wi-Fi for 30 minutes. Now the request goes with the login, and
  that press does what the button does otherwise.
- **Settings page: "Try again" after a login that was not approved works.** When the action button was not pressed
  within the minute, or another browser asked at the same time, every new try from the same browser was refused at
  once, with nothing on the ring for the button to approve, until another browser asked or the Echo restarted. Now
  the next try waits for the button as the first one did. A browser whose login was revoked is asked for the button
  again too, instead of showing "Logged in" over a page that loads nothing.
- **The unlock and boot-root are now done by hand, as the XDA thread says, on every model.** The unlock tools
  (kamakiri, amonet, saiyaku) and boot-root are R0rt1z2's, and with kamakiri-donut v1.1.0 he asks that they are only
  ever run by hand, following his XDA thread, never from an installer or a script. The guided setup respects that: it
  no longer asks for, unpacks, runs or installs them, and the model pages no longer copy their steps. At the unlock
  step it shows the model's XDA thread; follow it in a terminal window of your own until TWRP is running, come back,
  press Enter, and the setup checks that the Echo is on adb. It then puts the firmware on as before (Echo Dots, Echo 2,
  Echo Input), and at the root step sends you to the thread again for boot-root: install it from TWRP as the thread
  says, press Enter once Fire OS has started, and the setup checks for root over adb and goes on with the install.
  The Echo Shows' root is this project's own boot image, as before. Stuck? Ask in the thread, with what the Echo did
  and the tool's output or log. This replaces the setup's own words around amonet's unlock from earlier today (issue
  #154). It also ends issue #156: the setup insisted on `kamakiri-donut-v1.0.0.zip`, which the thread no longer offers,
  and refused v1.1.0; it now asks for no unlock file at all. kamakiri-donut v1.1.0 writes the same files to the Echo as
  v1.0.0, so the warning about issue #112 stays as it was.
- **Nothing of the XDA threads' files is used any more.** The setup took two programs out of `boot-root.zip`; it now
  fetches them from where they were made: magiskpolicy (which adds this project's rules to the Echo's security policy)
  from Magisk's own release, v30.7, the very same file; on the Echo Show 5 (both generations) and the Echo Show 8 1st
  gen also adbd, now Google's own from its Android 7.1.1 emulator image (only 1.3 MB of it is downloaded). You no
  longer download `boot-root.zip` for the setup: get it from the thread when the root step sends you there. The new
  adbd on the Echo Shows has not been tried on a device yet.
- **Windows: the setup no longer runs firebreak.** firebreak unlocked an Echo Dot 2 from Windows by running amonet,
  so it goes for the same reason. On Windows every model, the Echo Dot 2 included, now needs an Echo that is unlocked
  already (TWRP or Fire OS on adb); the setup does everything after that, and [the Windows guide](docs/WINDOWS.md) says
  how.
- **The Bluetooth proxy can be switched off (issue #152).** "Bluetooth proxy" (settings page, Features; on by default,
  so nothing changes) stops Home Assistant from getting Bluetooth devices through this Echo: the Echo no longer says it
  is a proxy, stops scanning and takes no connections. Disabling its Bluetooth entries in Home Assistant never did
  that: advertisements kept arriving. Phones, a Bluetooth speaker and the setup from a phone work either way. Home
  Assistant reconnects for a moment when you switch it, as with every feature.
- **Double and triple press of the action button skip tracks.** New setting "Double and triple press of the action button"
  (Voice page, `multi_press`, off by default; not on the Echo Shows, which have no action button): twice is next, three
  times previous, for Music Assistant and for a phone over Bluetooth (AVRCP). Two or three presses act as one single press
  when nothing plays, during a Drop In call (ringing included) and while a login waits for the button. While it is
  on, a single press waits 450 ms to see whether more follow. Home Assistant's "Button" event fires for whatever acts
  as a single press, never for a skip.
- **Music ducking has a depth you can set.** While the assistant listens, thinks or speaks, music from Music Assistant
  and from a phone over Bluetooth was always turned down by the same 15.6 dB (to a sixth), which over loud music or an
  audiobook can leave too little of your voice for speech-to-text. The setting "Music ducking" (Sound page, `music_duck`,
  0 to 60 dB, default 16 dB: about what it was) sets how far; 0 turns ducking off.
- **Clearer unlock (issue #154).** Amazon's lock is opened by a script, amonet's fastbrick, which ended with "DO NOT
  INTERRUPT IT" and "Press any key to exit...", and nobody could tell whether it was finished or whether a key would
  stop it. The guided setup now says before the unlock how long it takes and what not to touch (the Echo's power and
  USB cable, Ctrl+C, the window) until the sign it is over: the white ring on an Echo Dot 2 or Echo 2, TWRP on the
  screen of an Echo Show. It asks for YES itself, answers the script's questions for it, says "hands off" while the
  Echo finishes on its own, and says when the unlock worked. The model pages say the same for an unlock by hand.
- **Echo Show: what you said and the reply on the screen.** "Speech on screen" (settings page, Screen; off by default)
  shows your words and the assistant's answer in large letters over whatever the screen shows: the face, the music, or
  an app such as Kiosk Satellite (not over the pulled-down menu). They fade 8 seconds after the answer ("Speech stays
  for": any number of seconds, or until the next request). A "stop" the Echo carries out itself (a ringing timer,
  music, "Stop after the wake word ends listening") is not shown, and takes what was showing away. Experimental.
  Thanks to taucher4000, who wrote it and tried it on an Echo Show 5 2nd gen.
- **Echo Show: timers on the screen.** "Timers on screen" (next to it, off by default) counts running timers down in
  large digits with their unit ("4:59 min", "1:02:05 h", "45 s") and a bar of what is left, and shows a ringing one in
  orange, pulsing until it is stopped, with a Stop button. The screen stays on while a timer runs.
- **Echo Show: words and timers by touch.** A tap takes the words away, hides the timers until the next change, and
  Stop stops a ringing timer, over an app too: while they show, the app gets none of those touches.
- **A settings file from before the sounds had their own switches keeps the Echo as quiet as it was.** In a file
  exported before then (its first line names a version older than 2026.11.02), "Wake sound" off also stood for the
  action button, microphone and volume sounds. Imported on the settings page or given to `scripts/setup.sh --preset`,
  it turned only the wake sound off; now those sounds go off with it, as they already did for an Echo that had it off
  when it updated. Bluetooth's sounds stay on: that switch never covered them.

## 2026-11-08

- **Wake word in Home Assistant: answering a question now works.** When the assistant asked something back
  ("which light?"), the Echo opened the microphone but asked Home Assistant to wait for a wake word first, so the
  answer was ignored. A follow-up question now goes without that wait, as with the Echo's own wake word, and the
  Echo shows it is listening. The same for the action button while a reply plays: what is said after it is the
  request.

## 2026-11-07

- **Echo Shows: screen brightness as on a stock Echo Show (issue #126).** The screen's brightness is now a setting,
  on the settings page's Screen page and in Home Assistant: **Screen brightness** (one level, 47 % by default, what the
  Shows ran at before), or **Adaptive brightness**, Amazon's own following of the room's light, with **Adaptive
  brightness adjustment** to make it brighter or dimmer, as the slider on a stock Echo Show does. The face no longer
  dims on top of that in a dim room: it was at half brightness most of the day where the light sensor reads low.
  With Kiosk Satellite installed, the Echo switches off the app's own adaptive brightness, which kept overriding these
  settings: the app restarts once for it.

- **Setup on an Echo Dot 3: a clearer hint when TWRP does not come up.** If TWRP (the white ring) is not there a
  minute after the unlock flashed it, or after any later restart into it, the setup now says to unplug the power and
  hold only Volume up (+) while plugging it back in, which starts TWRP. It used to say only to replug the power, which
  can start the Echo's own software instead.

- **Echo Show 5 (1st gen): the wake sound right away after a pause.** After a few seconds of quiet, the Echo Shows waited 0.35 s
  before every sound, because the Echo Show 8's speaker swallowed the start of the first one while it switched on.
  Measured on an Echo Show 5 1st gen, it loses nothing, so it now plays at once. The other Shows keep the wait until someone
  measures theirs (issue #75).

- **Setup: fewer dead ends on an Echo Show (issue #132).** When the unlock does not take and the Echo starts its own
  software again, the setup now says so and offers to run the step again, instead of waiting for TWRP forever. It
  names the right buttons for fastboot (Volume down, Volume up and Mute; it said the camera shutter). It checks free
  disk space before the big downloads and unpacks instead of breaking off with "disk full", and the README gives the
  real figures (an Echo Show needs about 7 GB). On a commit GitHub published no build of, it no longer downloads the
  1 GB Android NDK without asking: it takes the release with the same Echo software where CI found one, or offers
  the newest release first.

- **Echo Shows: a key held while the Echo starts no longer sticks.** Holding a key while hassmic started (during
  boot, for example) left Android thinking it was still held until hassmic restarted: volume down kept turning the
  volume to 0 and undid every attempt to raise it (issue #140). The Echo now takes over the keys only
  once every key is up.
- **"Noise reduction" says what it costs.** Measured on an Echo Dot 2 in a kitchen (issue #131): besides the
  background, it turns down the hiss of consonants, and every speech-to-text tried understood more with it off than
  on medium. The setting's description and the guide now say so, and to leave it off (as it is by default) unless a
  loud, steady noise drowns your commands.

## 2026-11-06

- **Wake word in Home Assistant: saying it while the Echo talks now interrupts the reply.** Until now this worked
  only with the Echo's own wake word. Once Home Assistant has finished a reply, the Echo asks it to listen for the
  wake word again while the reply is still playing; when Home Assistant hears it, the reply stops and the Echo
  listens for your next request. It relies on the Echo cancelling its own voice from the microphone, so a loud
  reply may be missed; the button still always works. "Okay Nabu, stop" during a reply now ends it without a request
  going to the assistant, as "Alexa, stop" does with the Echo's own wake word.
- **Wake word in Home Assistant: "Last heard" and "Last reply" keep what was said.** Both went blank as soon as a
  reply ended, because the Echo starts listening for the wake word again at once; now they clear when the next
  request begins.

## 2026-11-05

- **Echo Dot 3: the unlock can leave the Echo unusable; the setup now warns before it.** On two Echo Dot 3s the
  unlock (kamakiri) ran through without an error, yet left the Echo locked and unusable, and no way on from there has
  been found (issue #112). Echos that were updated recently may become unusable; the full extent is currently unknown.
  Until the cause is known, the setup says so and asks before it unlocks; the default answer is not to. If fastboot
  turns out locked after the unlock, it points to the issue before anyone runs the unlock again.

## 2026-11-04

- **Even volume steps.** Every point of the volume is now the same step: 0.5 dB a point, 5 dB a press of the volume
  buttons, from 1 (about 50 dB under full) to 100 (as loud as before). The Echos' own volume puts most of the change at
  one end: on an Echo Show 5 the bottom half covers 46 dB and the top half 20, and since it has only 31 steps for 100
  points, a slider moved by one point often changed nothing; on an Echo Dot, 60 to 70 was 1 dB and 90 to 100 was 10.
  Low volumes are louder than before (on an Echo Show 5, 5 and 10 were 60 and 51 dB under full, now 47.5 and 45.5) and
  middle ones somewhat quieter (50 is 25 dB under full, where it was 20 to 22). Your volume stays
  at its number through the update. "Even volume steps" on the settings page (Sound, Speaker) brings the Echo's own
  steps back. On by default.
- **Echo Show 5, Show 5 2nd gen and Show 8 (1st gen): the screen turns off on time.** hassmic sets how soon the
  screen turns off (5 minutes, 1 minute while muted or on Do not disturb, as stock) with Android's `settings`, which
  on these Echos is a shell script without a `#!` line: started directly it failed, and the setting was never made.
  hassmic now runs such a script through the shell, as a shell would. A tool that hangs now holds up the part of
  hassmic that asked for 20 seconds at most and is then stopped, and what a tool prints as an error reaches the log.
- **The Echo Shows start sooner** (the Echo Show 5 about 15 seconds). Turning Alexa off at each start no longer asks
  Android questions it already knows the answers to (the Echo Show 5 took 16 of its 41 seconds there).
- **No more ticks in music.** Music Assistant's music had a small tick several times a second on the Echo Show 5,
  most noticeable in quiet passages. The Show 5's speaker runs a little slow, and to stay in sync with the other
  speakers the Echo skipped a few samples now and then. It now evens that out all along, by a change of speed far
  too small to hear (0.035 %), so nothing is skipped or repeated, on every Echo. On the Echo Shows the music also starts
  without the short break that came right after its first moment. And the music no longer clicks as it is turned down
  for a request and back up after: it glides.
- **Echo Shows: the screen's face comes up right after the install** (issue #122). Installed by the setup, the
  screen's own UI never started: the screen stayed black but for the light band, until the first update arrived.
  Android lets the screen's program load its library only from a few folders, and the system partition, where the
  install puts hassmic, is not one of them (updates live in `/data`, which is); hassmic now tells it where to look,
  in a way Android does not ignore. Seen on an Echo Show 5 2nd gen, checked on an Echo Show 5.
- **Echo Show 5 and Show 5 2nd gen: the screen at a fixed brightness**, as on the Show 8s. Android's automatic
  brightness took the screen down to almost nothing in a dim room. Where Kiosk Satellite is installed, it sets the
  brightness, as before: its own adaptive brightness or the level chosen in it (on every Echo Show).
- **Running timers in Home Assistant.** A timer set by voice is Home Assistant's, but no entity: only the Echo that set
  it hears of it, and the Echo used to wait until it rang. Now "Next timer" (when the running timer that ends first
  ends, a timestamp), "Next timer name" and "Next timer length" follow it as it starts, pauses, resumes, gets time
  added, is cancelled or rings, so a dashboard, another speaker or a display elsewhere in the house can count it down
  ([Voice](docs/GUIDE.md#voice)). Paused timers are left out; with none running they are unknown.

## 2026-11-03

- **Uninstalling from an Echo Show no longer leaves "OOBE has stopped" on the screen.** The install takes Amazon's apps
  off the system partition, and Android forgets their data and permissions; put back, Amazon's setup wizard and Alexa's
  speech service crashed in a loop ("OOBE has stopped", "SpeechUI has stopped"). `scripts/install-system.sh
  --uninstall` now ends with a wipe of cache and data in TWRP (tap "Keep Read Only" when it asks), and the Echo starts
  at Amazon's setup as a new one. It asks first (or `--uninstall --yes`). Already uninstalled and seeing these
  messages? Run `scripts/install-system.sh --uninstall` again with the Echo on USB: it does only the wipe.
- **Another firmware build with the same audio and wake word libraries installs like the pinned one.** The install
  stopped on any firmware but the one each model is built for, even where those libraries are byte for byte the same.
  Now it compares them (`scripts/probe.sh`) and goes on when they match, and the setup offers to keep an Echo rooted
  already on such a build as it is. This holds on every model except the Echo Show 5, Show 5 2nd gen and Show 8 1st
  gen, whose install flashes a boot image made for that one build. The Echo Show 8 2nd gen on PS75741.7082N installs
  this way.
- **Echo Show 8 2nd gen: a firmware with other libraries asks instead of stopping.** Its setup has no step that could
  put the pinned PS75741.7080N back, so where the libraries differ, the setup and `scripts/install-system.sh` show a
  large warning naming them and ask whether to go on (`HASSMIC_FIRMWARE_OK=1` answers yes for `install-system.sh`
  without a terminal). Amazon's new line PS75751.7806N is such a build: two of its libraries were rebuilt; on it the
  Echo booted, heard its wake word, started the voice assistant and ran Kiosk Satellite.
- **Setup on the Echo Show 5 and Echo Show 8 1st gen stops when the unlock was not confirmed.** amonet asks you to type
  `YES`; anything else (a typo, a stray key) makes it stop without touching the Echo, but setup took that as done and
  waited for a TWRP that never came. Now it says amonet stopped, and the step can simply be run again.
- **Echo Shows: the screen's face and drawer now come with the releases.** The published updates had left them out,
  so a Show installed or updated from a release showed only the light band on a black screen. Builds of your own
  (`make`, `scripts/ota-push.sh`) had it all along.
- **Echo Show 5 2nd gen: the screen of its own too.** The face, the drawer from the top edge, the home app and Music
  Assistant on the screen, as on the 1st gen ([guide](docs/GUIDE.md#the-screen-echo-show)). It comes with the next
  update; not tried on one of these Echos yet.
- **Echo Show 8 1st gen: the screen of its own too**, the same as on the Show 5, laid out for its larger screen. It
  comes with the next update; not tried on a Show 8 yet.
- **Echo Show 8 2nd gen: the screen of its own too**, now on every Echo Show hassmic runs on. It comes with the next
  update; not tried on one yet.
- **Echo Show 5: a screen of its own** ([guide](docs/GUIDE.md#the-screen-echo-show)). Over hassmic's home screen the
  Show now has a face: two eyes that listen, think and speak with the assistant (a mouth moves with the reply), close
  when the microphone is off, sleep in Do not disturb, light up for a ringing timer (with a Stop button) and for a Drop
  In (Answer, Decline, End), and the clock below in Home Assistant's time zone. While Music Assistant plays on the
  Show, the track and its cover sit at the bottom, with play/pause. Kiosk Satellite or any other app can still have the
  screen: hassmic's own stays small.
- **Echo Show 5: a drawer from the top edge, as on Amazon's Echo Show.** Pull down from the top of the screen, over any
  app: Home, Face, Music, Do not disturb, Screen off, the Echo's volume and what plays. Over an app, start the pull on the
  thin black strip along the top: the app never gets that touch (Kiosk Satellite took it as its pull to refresh), and
  the app's picture stays under the drawer until the app is back, without the face showing in between. Over an app, a
  swipe along the light band is Back.
- **Echo Show 5: a home app.** The settings page's new Screen page picks what the Show shows at start and brings back
  when it ends (three times a minute at most): Kiosk Satellite until you pick another, the face, or any app you
  installed. The drawer's Home goes there.
- **Echo Show 5: Music Assistant on the screen.** A music page with five tabs on the left: Playing (cover, title,
  artist, album, a progress bar you can drag, shuffle, previous, play/pause, next, repeat, the volume), Queue (what comes
  next; tap a track to play from there, long press to remove it, Clear, Shuffle), Library (artists, albums, playlists,
  tracks, radio, podcasts, audiobooks; an artist opens its albums, an album or a playlist its tracks, with Play,
  Shuffle, Add to queue and Radio), Search (a keyboard on the screen; results as you type, grouped) and Speakers (every
  player Music Assistant has: tap one to play it together with the Show, or take it out of the group, each one's
  volume and the group's, long press to move what another speaker plays to the Show). A long press on anything in
  the library or the results: Play now, Play next, Add to queue, Start radio. Playing works as it did with nothing to
  set up; the rest needs Music Assistant's API: on the settings page (Music), paste a long-lived token from Music
  Assistant (your profile, Long-lived tokens). It is sent sealed for that Echo and kept on it. The address is that of
  the Music Assistant that plays on the Echo when you save it, and the token only ever goes there.
- **Echo Show 5: the light band uses less memory.** The band is now drawn by the same process as the screen, which
  saves about 16 MB.
- **Kiosk Satellite and the screen get along.** Where hassmic's screen runs, it opens Kiosk Satellite (its home app)
  instead of the background check that did so every 10 seconds: the face or the music, once asked for, stay on the
  screen until you leave them.
- **Kiosk Satellite on the Echo Shows** ([guide](docs/GUIDE.md#kiosk-satellite-on-the-screen)): with the app on a
  Show 5 (1st or 2nd gen) or Show 8 (1st or 2nd gen), the Echo opens it by itself, opens it again when it ends, gives it
  the permissions it would otherwise ask a person for and keeps its update helper running, so it updates itself. Its web
  page (port 2324) and Home Assistant API (port 6053) are reachable from your network. Trust it like adb: the helper
  runs the app's code as the shell user. The guided setup offers it with Simple Keyboard (open source, no network access: the image has no
  keyboard); on an Echo already installed, `scripts/kiosk-install.sh`, and `scripts/kiosk-install.sh --uninstall` takes it
  off again (a plain `adb uninstall` fails: the Echo makes it a device admin). The Show 5s keep Amazon's WebView for
  this, an update puts it back, and they now stay in landscape.
- **Kiosk Satellite internet access** (settings page, System, once the app is installed; on by default): only that app may
  reach the internet; the voice satellite and Amazon's software stay as before.
- **Kiosk Satellite microphone and camera** (same place; off by default). The microphone is for a plugin that talks, such
  as a doorbell: pick "Voice communication" in it, the wake word keeps working during the call. Turning either off closes
  the app for a moment.
- **Setup on Windows: no "no release build of this commit" after a change to the docs.** Only changes to what goes
  onto the Echo get a release build of their own, so with the newest commit changing only a README or the PC's
  scripts, the setup refused to go on in Git Bash, where it cannot build anything. It now takes the newest release
  before that commit, which holds the same files for the Echo (issue #106). So [docs/WINDOWS.md](docs/WINDOWS.md) no
  longer has you check out the newest version by its tag: the project as cloned works, with the setup's newest fixes.
- **Setup on Windows: prompts no longer stop taking keys.** In Git Bash, a question asked after the setup had talked
  to the Echo (whether firebreak's drivers are installed, the Wi-Fi name and password) could ignore what you typed,
  and Ctrl-C too, until the window was closed. The programs the setup runs no longer get the keyboard unless they
  ask something themselves, the likely cause; should it happen again, please say so in issue #106.
- **Setup on Windows: Ctrl-C no longer empties your Temp folder.** Stopping `scripts/setup.sh` (or
  `scripts/artifacts.sh`) with Ctrl-C in Git Bash deleted everything in `C:\Users\<you>\AppData\Local\Temp` that no
  running program held open: the scripts cleaned up a temporary folder under a name Windows already uses for that
  folder. They now only delete what they made themselves (issue #103).
- **Echo Show 5: the firewall no longer reloads every few seconds.** On a network whose router announces its own IPv6
  address as DNS server, the log said "lockdown re-applied (ip6tables: hassmic_out is not as loaded)" every 5 seconds,
  and "firewall rules wrong on two looks" with a restart of the firewall service every 20. The Echo stayed locked
  down all along; only the check misread that DNS server's address (issue #100).
- **"Download from Amazon" says when Amazon does not hand a model to this Echo.** Amazon refuses the whisper detection
  model to the Echo Show 5, and the page called that a lost sign-in ("log in again"), which changes nothing. It now
  says Amazon does not hand it out to this model, and the whisper warning and the download list point at Copy models:
  bring it over from an Echo that has it. Should Amazon hand it out later, a download still works.

## 2026-11-01

- **Your own sounds in place of Alexa's.** A new "Sounds" card on the settings page (Voice) lists the Echo's sounds:
  the wake sound, the action button, microphone off and on, the volume keys, Bluetooth connected and disconnected,
  Identify, and Drop In's chimes and ringtones. Each can be replaced by any sound your browser plays, the same way as
  the timer sound: the page converts it and offers to bring it to the loudness of Alexa's sound it replaces. "Play"
  plays it once; deleting yours brings Alexa's back. The wake, button, volume, Drop In and call connected sounds may be
  2 seconds long at most (they play over the start of your command or call), ringtones 30, the others 5. One longer
  than 2 seconds stops when you speak to the Echo or another sound plays. Each Echo keeps its own; a factory reset deletes them.
- **Sound packs.** The Voice page keeps sets of sounds on the Echo: save the sounds in use as a pack, switch between
  packs, and save one as a `.soundpack` file to load on another Echo. A pack is a whole set: sounds it has none of go
  back to Alexa's. Up to 16 packs and 30 MB on an Echo, and none are written when its storage is nearly full.
- **Each sound has its own switch.** The Sounds card turns the wake sound, action button, microphone, volume and
  Bluetooth sounds on and off one by one, with "All off" for all of them. The "Wake sound" switch in Home Assistant now
  does what it says: it used to silence the button, microphone and volume sounds too, and now only the wake sound. Off
  keeps the sound you chose. An Echo that had it off stays quiet after the update: those sounds start with their own
  switches off.
- **Setup on the Echo Show 5 (both generations) and Echo Show 8 1st gen wipes the old data with the old system.** An
  Echo that had updated itself past the firmware the setup installs kept that newer build's app updates, and Android
  ran them over the older system: on an Echo Show 8, "Spotlight has stopped" popped up every minute or so. Setup now
  wipes cache and data before it writes the system partition, as it always did on the Dots, and checks that the apps
  are gone. "Start from scratch" on a rooted Echo now wipes there too, also when the firmware is the right one already.
  Already installed and seeing such messages? `scripts/setup.sh <model> --restart` and "Start from scratch" clears them,
  along with the Echo's settings and its link to Home Assistant. Or by hand: `adb shell pm uninstall <package>` for each
  Amazon package left in `/data/app` (keep `com.amazon.webview.chromium`); Android may restart once on that and comes
  back by itself.
- **The Echo says when Home Assistant is not there.** While Home Assistant restarts, or when the Echo has lost its link
  to it, the wake word and the action button used to do nothing at all: no sound, no light, nothing in the log, so the
  Echo looked deaf. Now the ring shows the short error animation, and the log says why (`no Home Assistant link`).
  Not while the microphones are off, or muted from Home Assistant over Wyoming, and not before the Echo was set up.
- **A fix for the Echo 2 going offline on WPA2/WPA3 mixed Wi-Fi** (issue #90). Amazon's Wi-Fi service changes the saved
  network at every start to a form the Echo 2's Wi-Fi refuses on such routers: it joined at setup and was never online
  again after a reboot. Now, after 30 s off the network, the Echo puts it back by itself. Tried on an Echo 2 in an
  earlier form, by the owner who reported it.
- **Setup on the Dots and the Echo 2 says so when a network is WPA3 only** (issue #90), instead of blaming the name or
  the password: setup joins WPA2 networks.
- **The Echo 2's setup uses amonet-radar v1.0.1** (issue #88), which knows the bootloaders of newer Fire OS builds.
  Get the new zip from the XDA thread into `firmware/radar/`; a setup already under way asks for it at the unlock.

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
