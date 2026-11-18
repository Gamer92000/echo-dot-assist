# Echo Dot 3rd gen, 2018 (`donut`)

Model D9N29T, MediaTek MT8516, `ro.product.device` = `donut_puffin`. Supported with Fire OS 6574.1 (`NS65741`) only.
`scripts/probe.sh` checks that the Echo runs exactly this firmware.

- **Unlock and root**: `kamakiri-donut` (bootrom exploit through test pads on the board, then TWRP), then `boot-root`
  for root adb and a permissive `su` SELinux domain; both by hand, as their [XDA thread][xda] says.
- **Install**: `INSTALL=twrp-ab`. `scripts/install-system.sh` patches `/sepolicy` with magiskpolicy (Magisk's own
  release, [v30.7](https://github.com/topjohnwu/Magisk/releases/tag/v30.7)) under the running OS, then writes the running system partition (`/` is dm-0 over the active slot, verity off; remounted
  writable by `otatool remount`). `--twrp`: the active A/B slot (`bcbtool get_active`) from TWRP, as before.
- **Audio**: Amazon's `mixer` daemon and `libmixerAPI.so` (`micAsr` 16 kHz post-AEC capture; TTS, Music and Earcon
  streams). Wake word: `libpryon.so` with the stock `en-US/ALEXA` model, plus models fetched from Amazon.
- **Hardware**:
  - keypad `/dev/input/event3` (`KEY_HELP` action, volume)
  - mute latch in `gpio-privacy`: state in sysfs, events on `/dev/input/event1`; software can set it but not clear it
  - LED ring through `ledctrl` animations (30 volume steps)
  - Bluetooth: MediaTek combo chip on `/dev/stpbt`, H4 framing, owned by `btmanagerd` in stock
  - SoC temperature: thermal zone `mtktscpu`
  - speaker amplifier: TI TAS2770 at i2c 2-0044 (EVT..HVT boards; the DTB of Proto boards has a TLV320AIC32x4 instead),
    on I2S; the 3.5 mm jack plays from the MT8167's own codec, and the HAL mutes the TAS2770 meanwhile. Its `DAC
    Playback Volume` (AMP_LEVEL, 0..20 = 11..21 dBV full-scale output, 0.5 dB a step) is 16 from boot on, never
    written by the firmware; the settings page's "Speaker gain" raises it by up to 2 dB (`board.c` `speaker_gain`).
    `PCM Playback Volume` (its digital volume) is at 0 dB in the speaker path: no headroom there
  - mics: two TI TLV320AIC3101 ADCs at i2c 0-0018 and 0-001a
- **Findings**: [docs/FINDINGS.md](../../docs/FINDINGS.md), [docs/re-platform.md](../../docs/re-platform.md),
  [docs/re-pryon.md](../../docs/re-pryon.md). Progress and measurements: [PLAN.md](../../PLAN.md).

## Install

The guided way: `scripts/setup.sh donut` runs these same steps and checks each one. They are written out here as well.

Plan an evening. Steps 1–2 are the risky ones and are not this project's work: read the [XDA thread][xda] in full first.

> **Warning: the unlock can leave the Echo unusable** ([issue #112][i112]). **Echos that were updated recently may
> become unusable. The full extent is currently unknown.** On two Echos `kamakiri` ran through without an error, yet
> the Echo came back locked (a green ring instead of the rainbow; `fastboot-step.sh` fails with
> `the command you input is restricted on locked hw`), and no way to finish the unlock or undo it has been found:
> neither has worked since. It is not known yet which Echos or firmware versions are affected, nor whether any unlock
> is safe now. While the issue is open, do not unlock an Echo you want to keep. `scripts/setup.sh donut` asks before
> it sends you to the unlock.

> **Never let the Echo go online with Amazon on the way.** It would update itself, and an update can close the hole the
> unlock uses. Do not set it up with the Alexa app, except in the guarded way described under
> [another wake word](#3-optional-another-wake-word).

### Requirements

Besides the general [requirements](../../README.md#requirements):

- An Echo Dot 3 from 2018 (D9N29T), **not** the 2019–2020 refresh (`crumpet`, C78MP8) or the one with the clock (`doebrite`, 36EBT3), and a way to reach its **hidden
  USB port**: see [USB access](#usb-access). This is the hard part.
- These files, not included (proprietary or third-party). The first two go into `firmware/donut/`, which is
  git-ignored, like everything below `firmware/`:

| File | Source | sha256 |
|---|---|---|
| `update-kindle-donut_puffin-NS65741_user_8138_0013222529668.bin` | stock Fire OS 6574.1 for `donut_puffin`, from [FTVDB](https://ftvdb.com/echo/firmware/com.amazon.donut.android.os/f7cd5f3b6d6bfa59e5ba6a6779285c34-13222529668-fire-os-6574-1-ns65741-8138-2026-08-31/) | `ac22b78cf94c2ebacfa90447b770b803d9f72f35858d5218975d97fea95a0245` |
| `magiskpolicy32` | `lib/armeabi-v7a/libmagiskpolicy.so` of `Magisk-v30.7.apk`, [Magisk v30.7](https://github.com/topjohnwu/Magisk/releases/tag/v30.7) (APK `e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5`); `scripts/setup.sh` fetches it itself | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |
| `toolchain/android-ndk-r21e/` | unpack <https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip> into `toolchain/` | – |

The unlock's and the root's own files (`boot-root.zip`) are the [XDA thread][xda]'s: it says which, kept current with
its procedure. Nothing here downloads or reads them.

Use exactly this firmware version, also when the Echo already runs a newer one, and also where a guide or thread says
to update to the newest first: everything was worked out against its binaries, `scripts/setup.sh` only takes the file
with this checksum (another version renamed to its name does not pass, issue #62), and `scripts/probe.sh` checks the
Echo for it. Flashing it during the unlock replaces whatever the Echo ran before.

### USB access

The Echo Dot 3 has no USB socket; its USB data lines end on test pads on the main board. The case has to be opened and
a USB cable soldered to those pads. How to open the case: the guides linked under "Requirements" in the
[XDA thread][xda].

> [!TIP]
> **Community made 3D printable solderless options:**
>
> - Pogo pins: [Echo Dot 3 (donut) Pogo Pin Holder for TM3 / TM4 / TM5](https://www.printables.com/model/1904685-echo-dot-3-donut-pogo-pin-holder-for-tm3-tm4-tm5-s) (Printables), by [dosdroid](https://github.com/dosdroid)
> - Wires only: [Echo Dot 3 USB jig](https://makerworld.com/cs/models/3551062-echo-dot-3-usb-jig) (MakerWorld), by [gabrielebu](https://github.com/gabrielebu)
>
> Be careful with these: pressed-on contacts (the bare wires most of all) can lose contact and break off the unlock
> midway. The bootrom still answers afterwards, so reseat the contacts and run the unlock again.

| USB cable wire | Pad on the Echo's board |
|---|---|
| black (GND) | TM3, or any other ground (the metal shield/case works too) |
| white (D−) | TM4 |
| green (D+) | TM5 |
| red (VCC, 5 V) | **nothing**: leave it unconnected and insulate its end, so it cannot touch the board |

The Echo takes its power from its own adapter, not from USB.

Use a cable with a USB-A plug for the PC end. With a USB-C plug there (a cut USB-C to USB-C cable), the PC sees no
device: a USB-C port switches the connection on only once the plug's CC wire (often yellow) goes to ground through a
5.1 kΩ resistor, which sits in the device a cable normally leads to (forum, 2026-10-22). Add that resistor, or take a
USB-A cable, which needs none.

Keep the Echo unplugged (no power, no USB) until the thread's unlock says otherwise.

### 1. Unlock, flash stock firmware, root

Once, first: USB access without sudo, and ModemManager kept off the Echo's bootrom.

```sh
sudo cp scripts/51-echo-unlock.rules /etc/udev/rules.d/ && sudo udevadm control --reload
```

Unlock the Echo **by hand, as the [XDA thread][xda] says**, from its first post: which Echos it is for, its files, how
to get the Echo ready and every command are there, kept up to date. R0rt1z2 asks (since kamakiri-donut v1.1.0) that the
unlock is only ever run that way, never from an installer or a script, so this page does not repeat its steps and
`scripts/setup.sh donut` does not run them either: it names the thread, waits for Enter and then checks for TWRP on adb.
**Stop once TWRP is running (white ring)**: the firmware below is done differently from the thread, and boot-root comes
after it. Stuck, or the Echo does something the thread does not say? Ask in the thread, with what the Echo did, in your
own words, and the tool's output or log as it is.

Flash the firmware into both A/B slots:

```sh
F=firmware/donut/update-kindle-donut_puffin-NS65741_user_8138_0013222529668.bin
adb shell twrp wipe cache; adb shell twrp wipe data
adb push $F /sdcard/update.zip
adb shell twrp install /sdcard/update.zip      # "Flashing A/B zip to inactive slot: B" (or A); it becomes the active slot
adb reboot recovery        # wait for the white ring; TWRP now runs from the slot just flashed
adb shell twrp install /sdcard/update.zip      # must name the OTHER slot this time
adb reboot recovery
```

Then root, by hand: `boot-root` installed from TWRP as the [XDA thread][xda] says, with the file the thread offers now,
until Fire OS has started with root adb (`adb shell id -u` says 0). `scripts/setup.sh donut` does not install it
either: it waits for Enter and checks.

TWRP switches the active slot itself after an install (checked 2026-09-25: `bcbtool get_active` said `b` right after
flashing B; a user saw the same on his Dots, GitHub issue 18). Do not switch it back by hand, as the XDA thread's
`bcbtool set_active` line does, or the second install lands in the same slot again and the other one keeps Amazon's
newer firmware. `scripts/setup.sh donut` never switches; it stops if TWRP did not. With a second Echo on adb (Wi-Fi),
point every command and script at the new one:
`export ANDROID_SERIAL=<serial from adb devices>`.

Result: stock Fire OS, unregistered, no Wi-Fi, orange ring, and `adb shell` is root. If adb does not show up after a
reboot, replug the power. Back to TWRP: `adb reboot recovery`, or hold Volume Up while plugging in.

### 2. Unpack the firmware and build

The tools link against Amazon's libraries, so the firmware is unpacked on the PC. On a commit GitHub has a build of
(any pushed commit on `main` or `release`, once CI has published it; [README](../../README.md#install)) none of this
is needed: `export PREBUILT=1`, and the scripts below and in the next steps use that build, checked against the
release key. Then only magiskpolicy is fetched, and `scripts/probe.sh` runs as below.

```sh
unzip firmware/donut/update-kindle-*.bin payload.bin -d firmware/donut/
python3 tools/payload_dump.py firmware/donut/payload.bin firmware/donut/images
mkdir -p firmware/donut/rootfs        # debugfs does not create it
debugfs -R "rdump / firmware/donut/rootfs" firmware/donut/images/system.img
curl -fsSL -o toolchain/Magisk-v30.7.apk https://github.com/topjohnwu/Magisk/releases/download/v30.7/Magisk-v30.7.apk   # the installer's magiskpolicy
sha256sum toolchain/Magisk-v30.7.apk   # e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5
unzip -p toolchain/Magisk-v30.7.apk lib/armeabi-v7a/libmagiskpolicy.so > firmware/donut/magiskpolicy32
chmod 755 firmware/donut/magiskpolicy32
sha256sum firmware/donut/magiskpolicy32   # e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a
make kernel-tools                         # kernel sources + compiler for Wi-Fi motion's module (115 MB, optional)
make                                      # ARM binaries into build/donut/
scripts/probe.sh                          # must not list any DIFFERENT library
```

If `probe.sh` reports a different library, the Echo runs another firmware: stop and redo step 1.

### 3. Optional: another wake word

Want "Echo" or "Computer" instead of "Alexa"? The Echo's settings page has "Download from Amazon" (a code to enter on
Amazon's site, then the wake words of any language, whisper detection, the newer sound detection model). That is the
way meant; the guided setup no longer downloads anything. The old way from the PC still works as a fallback (it
warns first), and installs models already in `device-logs/models/`:

```sh
scripts/artifacts.sh <echo-ip>
```

Its menu has a list of ticks per kind, everything new ticked: "Wake words" (the ones already in `device-logs/models/`,
as Amazon's sets are the same for every Echo: fetch once, copy to every Echo, and the ones Amazon has in the chosen
language) and "Other artifacts", installed on the Echo as well: whisper detection (Home Assistant's "Last request
whispered", [guide](../../docs/GUIDE.md#whisper-detection)), and Amazon's newest sound detection model in place of the
firmware's (not ticked: so far it scores the same, `docs/re-aed.md`). Untick what
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
scripts/install-system.sh                        # the Echo names itself ("Echo Dot 3 5695c4"); name it in Home Assistant
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
without Home Assistant (`scripts/adb-wifi.sh <echo-ip>`: 30 minutes; adb over Wi-Fi is closed otherwise, see
[adb over Wi-Fi](../../docs/GUIDE.md#adb-over-wi-fi)). The USB wires can come off now.

### 6. Optional: Music Assistant

Music Assistant finds the Echo by itself (Sendspin player, TCP 28928). With Home Assistant 2026.11 and Music Assistant
2.10.9 or later it also pairs with it by itself, through Home Assistant, as soon as the Echo is adopted there: nothing
to do. With older versions, pair it by hand with the Echo's token, shown at the end of `scripts/setup.sh`, in the
diagnostic entity "Sendspin pairing token" in Home Assistant (disabled by default), and in
`/data/local/hassmic/boot.log`. Pairing authenticates the connection: unpaired,
it is encrypted under a key everyone knows, so any machine on the LAN could play or sit in between. To allow unpaired
servers anyway, switch on "Music Assistant without pairing" on the Echo's settings page (`http://<echo-ip>:28931/`).
If Music Assistant does not find the Echo (another subnet or VLAN), add `<echo-ip>:28928` under its Sendspin provider's
"Manual IP addresses for discovery"; the port is needed.

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
Suomi, Polski, Română): set it to the language of the Echo's assistant in Home Assistant, whose voice speaks them. Home
Assistant does not tell the Echo which language its assistant uses, so this cannot follow it by itself.

[xda]: https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/
[i112]: https://github.com/Gamer92000/echo-dot-assist/issues/112
