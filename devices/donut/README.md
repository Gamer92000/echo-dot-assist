# Echo Dot 3rd gen, 2018 (`donut`)

Model D9N29T, MediaTek MT8516, `ro.product.device` = `donut_puffin`. Supported with Fire OS 6574.1 (`NS65741`) only.
`scripts/probe.sh` checks that the Echo runs exactly this firmware.

- **Unlock and root**: `kamakiri-donut` (bootrom exploit through test pads on the board, then TWRP), then `boot-root`
  for root adb and a permissive `su` SELinux domain. Steps and files are in the root [README](../../README.md#install).
- **Install**: `INSTALL=twrp-ab`. `scripts/install-system.sh` patches `/sepolicy` with `boot-root`'s magiskpolicy
  under the running OS, then writes the running system partition (`/` is dm-0 over the active slot, verity off; remounted
  writable by `otatool remount`). `--twrp`: the active A/B slot (`bcbtool get_active`) from TWRP, as before.
- **Audio**: Amazon's `mixer` daemon and `libmixerAPI.so` (`micAsr` 16 kHz post-AEC capture; TTS, Music and Earcon
  streams). Wake word: `libpryon.so` with the stock `en-US/ALEXA` model, plus models fetched from Amazon.
- **Hardware**:
  - keypad `/dev/input/event3` (`KEY_HELP` action, volume)
  - mute latch in `gpio-privacy`: state in sysfs, events on `/dev/input/event1`; software can set it but not clear it
  - LED ring through `ledctrl` animations (30 volume steps)
  - Bluetooth: MediaTek combo chip on `/dev/stpbt`, H4 framing, owned by `btmanagerd` in stock
  - SoC temperature: thermal zone `mtktscpu`
- **Findings**: [docs/FINDINGS.md](../../docs/FINDINGS.md), [docs/re-platform.md](../../docs/re-platform.md),
  [docs/re-pryon.md](../../docs/re-pryon.md). Progress and measurements: [PLAN.md](../../PLAN.md).

## Install

The guided way: `scripts/setup.sh donut` runs these same steps and checks each one. They are written out here as well.

Plan an evening. Steps 1–2 are the risky ones and are not this project's work: read the [XDA thread][xda] in full first.

> **Never let the Echo go online with Amazon on the way.** It would update itself, and an update can close the hole the
> unlock uses. Do not set it up with the Alexa app, except in the guarded way described under
> [another wake word](#3-optional-another-wake-word).

### Requirements

Besides the general [requirements](../../README.md#requirements):

- An Echo Dot 3 from 2018 (D9N29T), **not** the 2019–2020 refresh (`crumpet`, C78MP8) or the one with the clock (`doebrite`, 36EBT3), and a way to reach its **hidden
  USB port**: see [USB access](#usb-access). This is the hard part.
- These files, not included (proprietary or third-party). The first three go into `firmware/donut/`, which is
  git-ignored, like everything below `firmware/`:

| File | Source | sha256 |
|---|---|---|
| `update-kindle-donut_puffin-NS65741_user_8138_0013222529668.bin` | stock Fire OS 6574.1 for `donut_puffin`, from [FTVDB](https://ftvdb.com/echo/firmware/com.amazon.donut.android.os/f7cd5f3b6d6bfa59e5ba6a6779285c34-13222529668-fire-os-6574-1-ns65741-8138-2026-08-31/) | `ac22b78cf94c2ebacfa90447b770b803d9f72f35858d5218975d97fea95a0245` |
| `kamakiri-donut-v1.0.0.zip` | bootloader unlock + TWRP, [XDA thread][xda] | `4d2bb52eaf661f6616aa4268584d44cf9155c1328c34bfcba01a6558f942b738` |
| `boot-root.zip` | root adb + permissive `su` SELinux domain, [XDA thread][xda] | `de49cc88b27a8e77cf97cf0156bee50e4ddc0e116c41aaede06b494e38397be0` |
| `toolchain/android-ndk-r21e/` | unpack <https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip> into `toolchain/` | – |

Use exactly this firmware version: everything was worked out against its binaries, and `scripts/probe.sh` checks for it.

### USB access

The Echo Dot 3 has no USB socket; its USB data lines end on test pads on the main board. The case has to be opened and
a USB cable soldered to those pads. How to open the case: the guides linked under "Requirements" in the
[XDA thread][xda].

| USB cable wire | Pad on the Echo's board |
|---|---|
| black (GND) | TM3, or any other ground (the metal shield/case works too) |
| white (D−) | TM4 |
| green (D+) | TM5 |
| red (VCC, 5 V) | **nothing**: leave it unconnected and insulate its end, so it cannot touch the board |

The Echo takes its power from its own adapter, not from USB.

Keep the Echo unplugged (no power, no USB) until step 1 asks for it.

### 1. Unlock, flash stock firmware, root

```sh
sudo cp scripts/51-echo-unlock.rules /etc/udev/rules.d/ && sudo udevadm control --reload   # USB without sudo; keeps ModemManager off the bootrom
unzip firmware/donut/kamakiri-donut-v1.0.0.zip -d firmware/donut/kamakiri
K=$(dirname "$(find firmware/donut/kamakiri -name bootrom-step.sh)")   # the zip keeps its scripts in a folder of their own
(cd "$K" && ./bootrom-step.sh)    # start it, THEN plug the Echo in while holding the action (dot) button
(cd "$K" && ./fastboot-step.sh)   # when the ring shows a rotating rainbow; ends in TWRP (white ring)
```

Flash the firmware into both A/B slots, then root:

```sh
F=firmware/donut/update-kindle-donut_puffin-NS65741_user_8138_0013222529668.bin
adb shell twrp wipe cache; adb shell twrp wipe data
adb push $F /sdcard/update.zip
adb shell twrp install /sdcard/update.zip      # "Flashing A/B zip to inactive slot: B" (or A); it becomes the active slot
adb reboot recovery        # wait for the white ring; TWRP now runs from the slot just flashed
adb shell twrp install /sdcard/update.zip      # must name the OTHER slot this time
adb reboot recovery
adb push firmware/donut/boot-root.zip /sdcard/ && adb shell twrp install /sdcard/boot-root.zip   # patches both slots
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

The tools link against Amazon's libraries, so the firmware is unpacked on the PC. On a commit GitHub has a build of
(any pushed commit on `main` or `release`, once CI has published it; [README](../../README.md#install)) none of this
is needed: `export PREBUILT=1`, and the scripts below and in the next steps use that build, checked against the
release key. Then only `boot-root` is unpacked, and `scripts/probe.sh` runs as below.

```sh
unzip firmware/donut/update-kindle-*.bin payload.bin -d firmware/donut/
python3 tools/payload_dump.py firmware/donut/payload.bin firmware/donut/images
mkdir -p firmware/donut/rootfs        # debugfs does not create it
debugfs -R "rdump / firmware/donut/rootfs" firmware/donut/images/system.img
unzip firmware/donut/boot-root.zip -d firmware/donut/boot-root    # the installer uses its patch/magiskpolicy32
make kernel-tools                         # kernel sources + compiler for Wi-Fi motion's module (115 MB, optional)
make                                      # ARM binaries into build/donut/
scripts/probe.sh                          # must not list any DIFFERENT library
```

If `probe.sh` reports a different library, the Echo runs another firmware: stop and redo step 1.

### 3. Optional: another wake word

Want "Echo" or "Computer" instead of "Alexa"? Once the Echo is installed:

```sh
scripts/artifacts.sh <echo-ip>
```

Its menu has a list of ticks per kind, everything new ticked: "Wake words" (the ones already in `device-logs/models/`,
as Amazon's sets are the same for every Echo: fetch once, copy to every Echo, and the ones Amazon has in the chosen
language) and "Other artifacts", installed on the Echo as well: whisper detection (Home Assistant's "Last request
whispered", [README](../../README.md#whisper)), and Amazon's newest sound detection model in place of the
firmware's (not ticked: so far it scores the same, `docs/re-aed.md`). The guided setup's last step offers the same. Untick what
you do not want; "Go on" shows what it will do and asks once. It tries each wake word on the Echo's own engine before installing it,
and restarts hassmic. For downloads it walks through the Amazon route below by itself, once for all of them: stock
mode with the update block, you register the Echo in the Alexa app, it downloads the models (asking for what this
Echo's engine can load), it removes the app's Wi-Fi and the registration and goes back to satellite mode, and only
then you deregister: a stock Echo that is deregistered while online resets itself to factory settings, hassmic's
settings and models included. Home Assistant then offers every installed model in the
Echo's wake word select. What it does, by hand:

<details>
<summary>"Echo", "Computer", "Amazon", "Ziggy": fetch the model from Amazon once</summary>

The firmware ships only "Alexa". The others are Pryon model sets that a registered Echo downloads from Amazon (DAVS) on
demand, per language; the stock engine loads them as they are. Fetching one needs a registered device's access token,
so the Echo goes online with Amazon once: now, before the lockdown, or on an installed Echo with
`MODE=stock-online` (last paragraph).

**The danger is a firmware update**, which can cost root and the unlock. `otad` and `ace_otad` run as user `ace_otad` and
start `update_engine` on demand, so blocking that one user is enough:

```sh
adb push scripts/device/lockdown.sh devices/donut/device.conf /data/local/tmp/
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
3. Optional check on the PC: copy `unpacked/` below `firmware/donut/rootfs/`, then
   `tools/qrun.sh build/donut/pryon_test -m /<path below rootfs>/pryon.manifest testdata/echo_espeak_de.raw` must print
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

- **Do not reboot before step 5**: the Wi-Fi profile survives a reboot, this lock does not. Blocking the Echo's
  internet at the router is a good idea anyway.
- Give the Echo a fixed address (DHCP reservation).
- "Local" = private ranges, link-local, multicast, so Home Assistant may sit in another local subnet or VLAN.

### 5. Install it, adopt it in Home Assistant

```sh
scripts/install-system.sh "Kitchen Echo"         # the name Home Assistant will show
```

Patches the SELinux policy, writes `/system/hassmic/`, an init file and the policy into the active slot while the Echo runs
(no TWRP: the system partition is remounted writable, which boot-root's disabled dm-verity allows; old policy kept as
`/sepolicy.pre-hassmic`), reboots. `--twrp` writes it from TWRP instead, e.g. for an Echo that no longer starts. Refuses to write if anything is not as expected. From now on the
Echo boots silent and dark, locks itself down, and is a satellite about a minute after power-up.

1. Home Assistant shows a discovered ESPHome device with that name under Settings → Devices & services. Otherwise add it
   by hand: ESPHome, the Echo's IP, port 26053, encryption key empty. Home Assistant sets a key by itself.
2. Pick the Assist pipeline in the device's settings.
3. Say "Alexa", ask something.

Home Assistant must reach TCP 26053 on the Echo; the Echo must reach Home Assistant's port 8123. The log is
`/data/local/hassmic/boot.log`.

**Back up `secrets/update.key`**, created by the installer: it signs your updates, and it is how you get adb over Wi-Fi
without Home Assistant (`scripts/adb-wifi.sh <echo-ip>`: 30 minutes; adb over Wi-Fi is closed otherwise, see the
README's Configuration). The USB wires can come off now.

### 6. Optional: Music Assistant

Music Assistant finds the Echo by itself (Sendspin player, TCP 28928); to play on it, pair it with the Echo's token.
The token is shown at the end of `scripts/setup.sh`, in the diagnostic entity "Sendspin pairing token" in Home
Assistant (disabled by default), and in `/data/local/hassmic/boot.log`. Pairing authenticates the connection: unpaired,
it is encrypted under a key everyone knows, so any machine on the LAN could play or sit in between. To allow unpaired
servers anyway, switch on "Music Assistant without pairing" on the Echo's settings page (`http://<echo-ip>:28931/`).

### 7. Optional: Bluetooth speaker

"Bluetooth audio from phones" is on by default on the Echo's settings page; with it on, Home Assistant has a
"Bluetooth pairing" switch. Turn it on, then pick the Echo in the phone's Bluetooth settings within two
minutes. Paired devices reconnect by themselves whenever you choose the Echo; unknown ones are refused while the switch
is off.

Like stock Alexa the Echo announces connections: a chime and "Connected to <name>" / "Disconnected from <name>". The
words are spoken by Home Assistant, which only does that after you tick "Allow the device to perform Home Assistant
actions" in the Echo's ESPHome options (Settings → Devices & services → ESPHome → the Echo → Configure). The
"Bluetooth announcements" setting on the settings page turns chime and words off. The words are English until you pick
another language there (Deutsch, Français, Español, Italiano, Português, Nederlands, Svenska, Dansk, Norsk,
Suomi, Polski): set it to the language of the Echo's assistant in Home Assistant, whose voice speaks them. Home
Assistant does not tell the Echo which language its assistant uses, so this cannot follow it by itself.

[xda]: https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/
