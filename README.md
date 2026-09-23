# echo-dot-assist

> # ⚠️ DISCLAIMER: EVERYTHING IN THIS REPOSITORY IS VIBECODED ⚠️
>
> **All of it: the code, the scripts, the reverse-engineering notes, the plan and this README were written by an AI
> (Claude) in conversation with the author, not by hand.** It was tried on exactly one device. Nobody has reviewed or
> audited it.
>
> That includes the parts that can hurt: the bootloader-unlock workflow, the scripts that write to the system partition and
> patch the SELinux policy, the firewall rules that are supposed to keep the device away from Amazon and from firmware
> updates, and the signed push-update mechanism. Any of them can be wrong in ways that brick your Echo, leave it
> talking to the internet when you think it is not, or open it up on your network.
>
> **Read what you run. No warranty, no support, your risk.**

Turns an **Amazon Echo Dot 3rd gen (2018)** into a **Home Assistant voice satellite** that never talks to Amazon.

You say the wake word, the Echo streams what you say to your Home Assistant, Assist answers through the Echo's speaker.
The good part of the Echo is kept: Amazon's own microphone processing (echo cancellation, beamforming over the mic array,
per-device mic calibration) and Amazon's own wake word engine stay in place, so it hears you across the room and over its
own music like it did before. Only the Alexa client is replaced, by a small daemon called `hassmic`.

**Only for the 2018 Echo Dot 3 (`donut`, model D9N29T). Not the 2019–2020 refresh with the clock or the different board
(`crumpet`, C78MP8).** The unlock can brick the device. It needs the case opened and wires on test pads.

## What you get

- **Voice assistant** in Home Assistant through the ESPHome integration: found automatically, no YAML, no ESPHome add-on.
  Wake word runs on the Echo ("Alexa" out of the box; "Echo", "Computer", "Amazon", "Ziggy" with
  [one extra step](#another-wake-word-echo-computer-amazon-ziggy)). Replies start while the text-to-speech is still being
  generated. Say the wake word, or press the action button, while it talks and it stops and listens again.
  **"Echo, stop"** (wake word and "stop" in one breath) silences a reply or a ringing timer without listening again.
  "Stop" on its own does nothing: Amazon's models only hear it in the two seconds after the wake word.
  While a timer rings, music or a reply plays, the wake word is accepted more readily, as Amazon's models are tuned to do.
- **Announcements and media player**: `assist_satellite.announce`, TTS and `media_player.play_media` play on the Echo.
- **Timers** ("set a timer for ten minutes"): rings on the Echo until you press the button, say the wake word, or 60 s pass.
- **Multiroom music** as a Sendspin player for Music Assistant, in sync with other players; the voice assistant ducks it.
- **The buttons work**: action = talk without the wake word / pause and resume music / stop an alarm; volume up and down in
  10 % steps; the mic-off button is the hardware mute it always was (red ring, Alexa's own mute sounds, reported to Home Assistant).
- **LED ring** shows listening, thinking, speaking, errors and mute. Silent and dark at boot.
- **Settings in Home Assistant**: noise suppression level, auto gain, mic volume multiplier, mute switch, and a "Wake sound"
  switch that covers all local sounds. Those are Alexa's originals from the firmware: wake word, action button, volume keys,
  mute on and off.
  Diagnostics, off by default: SoC temperature and CPU usage.
- **No cloud**: the Alexa client, the updater and the telemetry services are stopped at every boot, and a firewall on the
  Echo drops everything that is not going to a local address (plus DNS to the servers your network hands out). The one
  exception is hassmic itself, which fetches replies and music wherever Home Assistant or Music Assistant point it. Put
  the Echo on a network without internet as a second layer.
- **Updates over Wi-Fi** once installed: one command on the PC, signed, with automatic fallback if an update does not start.
- **Reversible**: one file to delete for stock behaviour, an uninstaller, or reflash stock from the recovery.

Wyoming instead of ESPHome is available too (`-P wyoming`, port 16700).

Security, plainly: the ESPHome connection is encrypted like an ESPHome device with `api: encryption` but no key in its
YAML. Home Assistant generates the key when you add the Echo and sets it over an encrypted connection; from then on only
Home Assistant gets in. Until that moment anyone on the network could connect, or set a key first (then Home Assistant
cannot connect and the key has to be reset, see "Encryption key" below). Push updates must be signed with your key.

## What you need

- The right Echo Dot (see above) and a way to reach its **hidden USB port**: there is no socket, the data lines are on
  test pads inside. The [XDA thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/) links the guides for that. This is the hard part; everything after it is typing.
- A **Linux PC** with `adb`, `fastboot`, `python3`, `make`, `unzip`, `debugfs` (e2fsprogs), `sqlite3`; about 5 GB of disk
  (most of it the Android NDK).
- **Home Assistant** with an Assist pipeline that works (speech-to-text, conversation agent, text-to-speech). Test it with
  another satellite or the app first. Optional: Music Assistant for multiroom audio (tested with 2.10.4).
- Wi-Fi the Echo can use to reach Home Assistant. WPA2 passphrase networks; no captive portals, no enterprise logins.
- These files, which are not in this repository (proprietary or third-party). Put them in the repository root:

| File | Source | sha256 |
|---|---|---|
| `update-kindle-donut_puffin-NS65741_user_8138_0013222529668.bin` | stock Fire OS 6574.1 for `donut_puffin`, from FTVDB | `ac22b78cf94c2ebacfa90447b770b803d9f72f35858d5218975d97fea95a0245` |
| `kamakiri-donut-v1.0.0.zip` | bootloader unlock + TWRP, attachment of the [XDA thread for kamakiri-donut](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/) | `4d2bb52eaf661f6616aa4268584d44cf9155c1328c34bfcba01a6558f942b738` |
| `boot-root.zip` | root adb + permissive `su` SELinux domain, [same XDA thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/) | `de49cc88b27a8e77cf97cf0156bee50e4ddc0e116c41aaede06b494e38397be0` |
| `toolchain/android-ndk-r21e/` | unpack <https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip> into `toolchain/` | — |

Use exactly this firmware version. Everything here was worked out against its binaries, and `scripts/probe.sh` checks for it.

## Install

Plan an evening. Steps 1–2 are the risky ones and are not this project's work: read the [XDA thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/) in full first, it
is the authority for them. **Never let the Echo go online with Amazon on the way** (it would update itself, and an update
can close the hole the unlock uses): do not set it up with the Alexa app, except in the guarded way described under
[Another wake word](#another-wake-word-echo-computer-amazon-ziggy).

### 1. Unlock the Echo, flash the stock firmware, root it

Short version of the thread, as used here:

```sh
sudo cp scripts/51-echo-unlock.rules /etc/udev/rules.d/ && sudo udevadm control --reload    # USB access without sudo; keeps ModemManager off the bootrom
unzip kamakiri-donut-v1.0.0.zip -d kamakiri && cd kamakiri
./bootrom-step.sh          # start it, THEN plug the Echo into the PC while holding the action (dot) button
./fastboot-step.sh         # when the ring shows a rotating rainbow; ends in TWRP (white ring)
cd ..
```

Then the firmware into both slots (the Echo has A/B slots), and root:

```sh
F=update-kindle-donut_puffin-NS65741_user_8138_0013222529668.bin
adb shell twrp wipe cache; adb shell twrp wipe data
adb push $F /sdcard/update.zip
adb shell twrp install /sdcard/update.zip
adb shell 's=$(bcbtool get_active); case $s in a) bcbtool set_active b;; b) bcbtool set_active a;; esac'
adb reboot recovery        # wait for the white ring
adb shell twrp install /sdcard/update.zip
adb push boot-root.zip /sdcard/ && adb shell twrp install /sdcard/boot-root.zip
adb reboot
```

The Echo now boots stock Fire OS, unregistered, without Wi-Fi, talking about setup with an orange ring, and `adb shell` is a
root shell. If adb does not show up after a reboot, pull the power and plug it in again.
Later you get back into TWRP with `adb reboot recovery`, or by holding Volume Up while plugging in the power.

### 2. Unpack the firmware and build

The tools link against Amazon's libraries, so the firmware has to be unpacked on the PC (no root needed):

```sh
unzip update-kindle-*.bin payload.bin -d firmware/
python3 tools/payload_dump.py firmware/payload.bin firmware/images
debugfs -R "rdump / firmware/rootfs" firmware/images/system.img
unzip boot-root.zip -d boot-root          # the installer uses boot-root/patch/magiskpolicy32
make                                      # ARM binaries into build/
scripts/probe.sh                          # must not list any DIFFERENT library
```

If `probe.sh` reports a different library, your Echo runs another firmware than the one analysed: stop and redo step 1.

### 3. Optional: another wake word

If you want "Echo" or "Computer" instead of "Alexa", [get the model now](#another-wake-word-echo-computer-amazon-ziggy).
It is the only thing that needs the Echo online with Amazon, and it is easiest before anything else is set up. It can
also be done later.

### 4. Lock it down, then join Wi-Fi

In this order. The lock goes on before the Echo has ever seen your network:

```sh
scripts/deploy.sh                                        # pushes the binaries and scripts to /data/local/hassmic
adb shell sh /data/local/hassmic/lockdown.sh             # firewall: local addresses only; stops Alexa, updater, telemetry
mkdir -p secrets && printf '%s\n%s\n' 'My SSID' 'my passphrase' > secrets/wifi.conf      # git-ignored
scripts/wifi-join.sh                                     # refuses to run without the lock; prints the IP address
```

"Local" means the private ranges, link-local and multicast, so Home Assistant may be in another local subnet or VLAN.
The Wi-Fi profile survives reboots, this lock does not (yet): **do not reboot the Echo before step 6**, or block its
internet access at the router, which is a good idea anyway.
Give the Echo a fixed address in your router (DHCP reservation).

### 5. Try it

```sh
adb shell sh /data/local/hassmic/run.sh          # foreground; Ctrl-C stops it
```

Home Assistant shows a discovered ESPHome device "Echo Dot" under Settings → Devices & services (or add it by hand:
ESPHome, the Echo's IP, port 26053, leave the encryption key empty). Home Assistant then sets an encryption key on the
Echo by itself. Pick the Assist pipeline for it in the device's settings, say
"Alexa", ask something. If Home Assistant does not find it and cannot connect: it must be able to open TCP 26053 on the
Echo, and the Echo must be able to reach Home Assistant's port 8123 (for media and announcements).

### 6. Make it permanent

```sh
scripts/install-system.sh "Kitchen Echo"         # the name Home Assistant will show
```

This patches the SELinux policy under the running system, reboots into TWRP, writes `/system/hassmic/`, an init file and
the policy into the active slot, and reboots. It refuses to write if anything does not match what it expects, and keeps the
old policy as `/sepolicy.pre-hassmic`. From now on the Echo boots silent and dark, locks itself down, and is a satellite
about a minute after power-up. If Home Assistant still has the device from step 5 under another name, remove it and
add the new one.

The installer creates `secrets/update.key`. **Back it up**: it signs your updates. The USB wires can come off now.

### 7. Optional: Music Assistant

Music Assistant finds the Echo by itself (Sendspin player, TCP 28928 on the Echo). It asks for a pairing token: enable the
diagnostic entity "Sendspin pairing token" of the device in Home Assistant and copy it, or read it from
`/data/local/hassmic/boot.log`.

## Living with it

**Updating.** `git pull`, then `scripts/ota-push.sh <echo-ip>` (it remembers the address). Builds, signs, pushes over
Wi-Fi to TCP 28929; the Echo installs only what verifies against your key, restarts the daemon, and goes back to the
installed copy by itself if the new one does not stay up.

**Configuration** is one file on the Echo, `/data/local/hassmic/hassmic.conf`, read at boot (edit over adb, reboot):

```sh
NAME="Kitchen Echo"         # device name in Home Assistant
PROTO=esphome               # or wyoming
ARGS=""                     # extra options for hassmic, see below
#MODE=stock-online          # temporary: stock Alexa with internet but without updates, see "Another wake word"
```

Options for `ARGS`: `-m <pryon.manifest>` another wake word model · `-w remote` wake word detection in Home Assistant
(openWakeWord) instead of on the Echo · `-E` no sound on wake · `-L` leave the LED ring alone · `-V` leave the volume
buttons alone · `-z 0` no Sendspin player · `-p <port>` another port (the Echo's own firewall only admits inbound TCP
16384–32767).

**Encryption key.** Home Assistant sets it when the device is added and clears it when the device is deleted there.
The Echo keeps it in `/data/local/hassmic/state/api_key`. If Home Assistant says the key is invalid (the Echo was
reset, or something else set a key first): `adb shell rm /data/local/hassmic/state/api_key`, restart hassmic (or
reboot), then delete the device in Home Assistant and add it again.

**No sound from replies or music?** The Echo fetches every reply, announcement and `play_media` from the URL Home
Assistant or Music Assistant gives it. Home Assistant builds that URL from its internal URL (Settings → System →
Network), or from its own LAN IP when none is set. Any address works, local or not (hassmic is exempt from the egress
lock), and so does a `.local` name (the Echo asks by mDNS). What has to hold: the Echo can resolve the name (its DNS
servers come from DHCP) and route to the address. On a network without internet, that means a URL that points into
your network. `boot.log` names the host it could not resolve or reach (`net: cannot ...`). If not even the button sounds play, check
the volume first; a global mute left over from stock Alexa is cleared by hassmic by itself within 2 s of starting.

**Stopping it.** While it talks or a timer rings: the wake word alone cuts it and listens for a new command; "<wake word>,
stop" cuts it and stays quiet; the action button does the same as the wake word. Say "stop" right behind the wake word,
the way the English word sounds. Out of silence, "<wake word>, stop …" is an ordinary sentence for Home Assistant
("Echo, stop the music in the kitchen").

**When it does not react.** Most likely it has no connection to Home Assistant: then wake word and button do nothing, and
it does not tell you (known gap). Look at `adb shell tail -30 /data/local/hassmic/boot.log`: `wake: ALEXA type=2` means
it heard you, `client connected` / `voice assistant: subscribed` means Home Assistant is there. No such lines after the
last `client disconnected`: check the network (`adb shell ifconfig wlan0`, can Home Assistant reach that address?).
Keep exactly one Wi-Fi profile on the Echo; with two it roamed and kept the wrong address here.
`adb` also works over Wi-Fi: `adb connect <echo-ip>:5555` from a machine that can reach the Echo's network (stock firmware
turns that on, same key as over USB), so the cable is only needed for TWRP.
What changed when: [CHANGELOG.md](CHANGELOG.md). Open issues and everything that was measured: [PLAN.md](PLAN.md).

**Going back.**
- For a moment: `adb shell rm /data/local/hassmic/hassmic.conf` and reboot. The boot script then does nothing and the Echo
  is a stock, unregistered Echo (which will update itself if it gets internet).
- Properly: `scripts/install-system.sh --uninstall` removes the files from the system partition and restores the policy.
- Completely: reflash the stock firmware from TWRP as in step 1.

## Another wake word (Echo, Computer, Amazon, Ziggy)

The firmware ships only the "Alexa" model. The others are Pryon model sets that a registered Echo downloads from Amazon
(DAVS) on demand, per language. The stock engine loads them as they are, so `hassmic -m <pryon.manifest>` is all it takes.
Getting one needs an access token of a registered device, which means going online with Amazon once. Best done as step 3
of the install, before the lockdown; on an installed device use `MODE=stock-online` instead (below).

**The danger is a firmware update**: an Echo that is online pulls one, and that can cost root and the unlock. `otad` and
`ace_otad` run as user `ace_otad`, `update_engine` is started by them on demand, so cutting off that one user is enough:

```sh
adb push scripts/device/lockdown.sh /data/local/tmp/
adb shell "sh /data/local/tmp/lockdown.sh ota-only watch > /data/local/tmp/otaguard.log 2>&1 &"
adb shell iptables -S hassmic_out        # must show: -m owner --uid-owner <n> -j DROP
```

That guard is gone after a reboot, while the saved Wi-Fi profile is not: until the token is on the PC, start it again after
every reboot before doing anything else, or keep the Echo's internet blocked at the router while it boots.

1. Set the Echo up with the Alexa app as usual (Wi-Fi with internet, Amazon account). The app ends on an "updating" screen
   and the ring keeps spinning for a while: that is the update check that gets no answer. Nothing is being installed.
2. Pull the registration, which holds the token (valid for an hour after the device fetched it; the device renews it while
   online), and fetch what you want. Language is a parameter: no need to change wake word or language in the app.
   ```sh
   adb pull /data/ace/kvstorage/map.db device-logs/map.db
   tools/davs-fetch.py device-logs/map.db echo de-DE          # -> device-logs/models/echo-de-DE/unpacked/
   ```
   Keys seen to work: `alexa echo computer amazon ziggy`. `device-logs/` is git-ignored: the models are Amazon's, and
   `map.db` is your account's device credential.
3. Check it on the PC with the stock engine: copy `unpacked/` somewhere below `firmware/rootfs/`, then
   `tools/qrun.sh build/pryon_test -m /<path below rootfs>/pryon.manifest testdata/echo_espeak_de.raw` has to print
   `RESULT ... keyword=ECHO type=2`. (de-DE sets load; the larger en-US set did not under qemu, not looked into.)
4. Remove the device from your Amazon account in the app. On the Echo, drop the Wi-Fi profile the app added if it is not
   the network the satellite is meant to live in: `wpa_cli -i wlan0 -p /data/misc/wifi/sockets list_networks`, then
   `remove_network <id>` and `save_config`. Two saved profiles are not harmless: the Echo roamed between them here and
   kept the lease of the wrong one. The registration left in `map.db` is dead once the account forgot the device and
   sits behind the egress lock anyway; to clear it, empty the table on the PC and push the file back
   (`sqlite3 map.db "delete from deviceData; vacuum;"`, owner `ace_maplite:ace_maplite`, mode 660, reboot).
   Then carry on with step 4 of the install.
5. Install the model and point hassmic at it:
   ```sh
   adb shell mkdir -p /data/local/hassmic/models
   adb push device-logs/models/echo-de-DE/unpacked /data/local/hassmic/models/echo-de
   adb shell chmod -R a+rX /data/local/hassmic/models
   ```
   For a trial run: `run.sh -m /data/local/hassmic/models/echo-de/pryon.manifest`. Installed: put
   `ARGS="-m /data/local/hassmic/models/echo-de/pryon.manifest"` into `/data/local/hassmic/hassmic.conf` and reboot.

**On a device that is already installed**: add `MODE=stock-online` to `/data/local/hassmic/hassmic.conf` and reboot. hassmic
stays off, stock Alexa runs with internet, and the same update guard is applied at every boot, so this variant survives
reboots (it is the one that was used end to end here). Let the Echo reach the internet at the router for the duration,
follow steps 1–5, then remove the `MODE` line again. hassmic is down in this mode, so there is no push port: use adb.

What the request looks like, if you want it without the tool: `GET https://api.amazonalexa.com/v2/deviceArtifacts/?artifactFilter=`
+ URL-quoted base64 of `{"artifactType":"wakeword","artifactKey":"echo","filters":{"engineCompatibilityIdList":[…],"locale":["de-DE"],"modelClass":["B"]}}`,
header `Authorization: Bearer <access_token>`. The answer is JSON with a signed CloudFront `downloadUrl` (`.tar.gz`) that
expires within minutes. Found by preloading `src/tools/curlspy.c` into the stock downloader: `scripts/device/davs-spy.sh`.

## For developers

How it works: Amazon's `mixer` daemon owns the audio hardware and runs the whole front end (`libasp`); `hassmic` takes the
place of `PuffinApp` as its client through the reversed C API of `libmixerAPI.so`, feeds the 16 kHz post-AEC stream to
the stock `libpryon.so`, and speaks the ESPHome native API. Notes: [docs/](docs/), progress and measurements: [PLAN.md](PLAN.md),
user-visible changes by date: [CHANGELOG.md](CHANGELOG.md).

| Path | What |
|---|---|
| `src/hassmic/` | the daemon: core (capture, wake word, playback, LEDs, buttons), `proto_esphome.c`, `proto_wyoming.c`, `sendspin.c`, push updates |
| `src/tools/` | `mixcap`, `mixplay`, `pryon_test`, `latency`, `otatool`, `runas` (AIPC refuses uid 0, the image has no `su`), `curlspy` |
| `src/include/` | C headers for the reversed `libmixerAPI.so` and `libpryon.so` |
| `src/third_party/` | monocypher 4.0.2 (BSD-2-Clause OR CC0), `dr_flac.h` (public domain or MIT-0), `minimp3.h` (CC0) |
| `scripts/` | PC side: `deploy.sh`, `probe.sh`, `capture-test.sh`, `wifi-join.sh`, `install-system.sh`, `ota-push.sh` |
| `scripts/device/`, `scripts/system/` | run on the Echo; boot integration (`hassmic.rc`, `boot.sh` bootstrap, `main.sh` updatable part, `sepolicy.rules`) |
| `tools/` | OTA payload dumper, Thumb disassembly helpers, `qrun.sh` (device binaries under qemu-arm), `davs-fetch.py` |
| `tests/` | reference implementations as counterpart: `fake_ha_esphome.py` (aioesphomeapi), `fake_ma_sendspin.py` (aiosendspin), `fake_ha.py` (Wyoming) |

What the wake word hears: `kill -TTIN $(pidof hassmic)` on the Echo starts writing the processed mic stream to
`/data/local/hassmic/state/capture.raw` (16 kHz mono s16le), the same signal again stops it. `mixcap` cannot read that
stream while hassmic runs: the mixer feeds it to one client only.

```sh
make host                                         # PC build + qemu build for the tests (needs libopus)
.venv/bin/python tests/fake_ha_esphome.py         # pip install aioesphomeapi
.venv/bin/python tests/fake_ha.py [--qemu]        # pip install wyoming
```

Git-ignored because proprietary, derived or secret: `firmware/`, `re/`, `kamakiri/`, `boot-root/`, `toolchain/`, `build/`,
`device-logs/`, `secrets/` (`wifi.conf`, `update.key`).

## Licence

[MIT](LICENSE), for everything written here. The files in `src/third_party/` keep their own licences, stated in each file:
monocypher (BSD-2-Clause OR CC0-1.0), `dr_flac.h` (public domain or MIT-0), `minimp3.h` (CC0-1.0).

Nothing of Amazon's is in this repository and nothing of it is covered by this licence: firmware, libraries and wake-word
models come from your own device and stay Amazon's. Not affiliated with or endorsed by Amazon, Home Assistant or
Music Assistant; "Alexa" and "Echo" are Amazon's trademarks.
