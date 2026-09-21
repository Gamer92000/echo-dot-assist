# hassechodot

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

Echo Dot 3rd gen (2018, `donut`, MT8516) as a Home Assistant voice satellite.

Amazon's audio front end stays (`mixer` + `libasp`: echo cancellation, beamforming, mic calibration, speaker path) and so does
the stock "Alexa" wake word engine (`libpryon`). The Alexa client (`PuffinApp`) is replaced by `hassmic`, a small daemon that
speaks the ESPHome native API to Home Assistant (voice pipeline, announcements, timers, media player, settings entities;
Wyoming is still available with `-P wyoming`). It is also a Sendspin player for synchronised multiroom audio from
Music Assistant. The device never talks to Amazon: egress is locked to local addresses.

Status and open items: [PLAN.md](PLAN.md). Reverse-engineering notes: [docs/](docs/).

**Only for the 2018 Echo Dot 3 (`donut`, D9N29T). Not the 2019–2020 refresh (`crumpet`, C78MP8).** Unlocking can brick the device.

## Layout

| Path | What |
|---|---|
| `src/hassmic/` | the satellite daemon: core (capture, Pryon wake word, playback, LEDs, buttons) + `proto_esphome.c`, `proto_wyoming.c` |
| `src/tools/` | `mixcap`, `mixplay`, `pryon_test`, `runas` (drops root: AIPC refuses uid 0, the image has no `su`), `curlspy` (LD_PRELOAD shim that logs a stock daemon's libcurl requests) |
| `src/third_party/monocypher.[ch]` | X25519, ChaCha20-Poly1305 for the Sendspin Noise handshake; BSD-2-Clause OR CC0, <https://monocypher.org>, 4.0.2 |
| `src/third_party/dr_flac.h` | FLAC decoder for Sendspin; public domain or MIT-0, <https://github.com/mackron/dr_libs> |
| `src/third_party/minimp3.h` | MP3 decoder, public domain (CC0), <https://github.com/lieff/minimp3> |
| `src/include/` | C headers for the reversed `libmixerAPI.so` and `libpryon.so` |
| `scripts/` | PC side: `deploy.sh`, `probe.sh`, `capture-test.sh`, `wifi-join.sh`, `install-system.sh` |
| `scripts/device/` | run on the Echo: `alexa-off.sh`, `alexa-on.sh`, `lockdown.sh`, `wifi-join.sh`, `run.sh`, `davs-spy.sh` |
| `scripts/system/` | boot integration: `hassmic.rc`, `boot.sh` (stable bootstrap), `main.sh` (updatable), `sepolicy.rules` |
| `tools/` | OTA payload dumper, Thumb disassembly helpers, `qrun.sh` (device binaries under qemu-arm), `davs-fetch.py` (wake-word models from Amazon) |
| `docs/sendspin-digest.md` | what Music Assistant's Sendspin library really speaks, and where it differs from the spec |
| `tests/` | `fake_ma_sendspin.py` (reference `aiosendspin` server), `fake_ha_esphome.py` (reference `aioesphomeapi` client) and `fake_ha.py` (Wyoming) against a host or qemu build |

## External files (not in this repository)

Proprietary or third-party; fetch them yourself and put them in the repository root. All are git-ignored.

| File | Source | sha256 |
|---|---|---|
| `update-kindle-donut_puffin-NS65741_user_8138_0013222529668.bin` | stock Fire OS 6574.1 for `donut_puffin`, from FTVDB | `ac22b78cf94c2ebacfa90447b770b803d9f72f35858d5218975d97fea95a0245` |
| `kamakiri-donut-v1.0.0.zip` | bootloader unlock + TWRP, attachment of the [XDA thread for kamakiri-donut](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/) | `4d2bb52eaf661f6616aa4268584d44cf9155c1328c34bfcba01a6558f942b738` |
| `boot-root.zip` | root adb + permissive `su` SELinux domain, [same XDA thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/) | `de49cc88b27a8e77cf97cf0156bee50e4ddc0e116c41aaede06b494e38397be0` |
| `toolchain/android-ndk-r21e/` | <https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip> | — |

Derived from those, also ignored: `kamakiri/` and `boot-root/` (the unpacked zips; `install-system.sh` uses
`boot-root/patch/magiskpolicy32`), `firmware/` (unpacked OTA), `re/` (disassembly), `build/`, `device-logs/`.

`firmware/` is needed to build, because the tools link against the stock libraries in `firmware/rootfs/system/lib`.
Steps as used here (intermediate files were not kept, adjust paths if your OTA unpacks differently):

```sh
unzip update-kindle-*.bin payload.bin -d firmware/
python3 tools/payload_dump.py firmware/payload.bin firmware/images     # system.img, boot.img, ...
debugfs -R "rdump / firmware/rootfs" firmware/images/system.img        # no root needed
```

Secrets live in `secrets/` (ignored): `secrets/wifi.conf` (line 1 SSID, line 2 passphrase), `secrets/update.key` (signs push updates).

## Build

```sh
make            # ARM binaries into build/ (NDK r21e, API 24, armv7, lld)
make host       # PC build + qemu build for tests (needs libopus on the PC)
.venv/bin/python tests/fake_ha_esphome.py        # needs: pip install aioesphomeapi
.venv/bin/python tests/fake_ha.py [--qemu]        # Wyoming; needs: pip install wyoming
```

## Install on a device

1. Get USB access to the Echo and unlock it with kamakiri-donut; flash the stock OTA to both slots and `boot-root.zip`
   (steps are in the [XDA thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/)). Do **not** register the device with the Alexa app.
2. `scripts/probe.sh` — checks that the device libraries match the analysed firmware.
   Want a wake word other than "Alexa"? Do [that](#another-wake-word-echo-computer-amazon-ziggy) now: it is the one
   step that needs the Echo online and registered, and it is easiest before anything is locked down.
3. `scripts/deploy.sh`, then on the device `lockdown.sh` **before** the first Wi-Fi join, then `scripts/wifi-join.sh`.
   The lock allows local addresses only (private ranges, link-local, multicast), so Home Assistant may sit in any local subnet.
   Put the Echo on a network without internet access as a second layer.
4. Try it: `adb shell /data/local/hassmic/run.sh`. Home Assistant discovers an ESPHome device (or add it by IP, port 26053,
   no encryption key). With `-P wyoming`: Wyoming integration, port 16700.
5. Make it permanent: `scripts/install-system.sh "<name>"`. Goes through TWRP, adds `/system/hassmic/`,
   `/system/etc/init/hassmic.rc` and three allow rules to `/sepolicy` (stock copy kept as `/sepolicy.pre-hassmic`).

6. From then on update over Wi-Fi: `scripts/ota-push.sh <echo-ip>` builds, signs with `secrets/update.key` (created by the
   installer; back it up) and pushes. The Echo installs only what verifies against the public key on its system partition,
   and falls back to the factory copy if an update does not start.

Undo: delete `/data/local/hassmic/hassmic.conf` (boot script then does nothing), or `scripts/install-system.sh --uninstall`,
or reflash from TWRP (hold Volume Up while powering on).

## Another wake word (Echo, Computer, Amazon, Ziggy)

The firmware ships only the "Alexa" model. The others are Pryon model sets that a registered Echo downloads from Amazon
(DAVS) on demand, per language. The stock engine loads them as they are, so `hassmic -m <pryon.manifest>` is all it takes.
Getting one needs an access token of a registered device, which means going online with Amazon once. Best done right after
step 2 of the install, before the lockdown; on an installed device use `MODE=stock-online` instead (below).

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
   Then carry on with step 3 of the install.
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
