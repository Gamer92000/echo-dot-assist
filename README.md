# hassechodot

Echo Dot 3rd gen (2018, `donut`, MT8516) as a Home Assistant voice satellite.

Amazon's audio front end stays (`mixer` + `libasp`: echo cancellation, beamforming, mic calibration, speaker path) and so does
the stock "Alexa" wake word engine (`libpryon`). The Alexa client (`PuffinApp`) is replaced by `hassmic`, a small daemon that
speaks the ESPHome native API to Home Assistant (voice pipeline, announcements, timers, media player, settings entities;
Wyoming is still available with `-P wyoming`). The device never talks to Amazon: egress is locked to local addresses.

Status and open items: [PLAN.md](PLAN.md). Reverse-engineering notes: [docs/](docs/).

**Only for the 2018 Echo Dot 3 (`donut`, D9N29T). Not the 2019–2020 refresh (`crumpet`, C78MP8).** Unlocking can brick the device.

## Layout

| Path | What |
|---|---|
| `src/hassmic/` | the satellite daemon: core (capture, Pryon wake word, playback, LEDs, buttons) + `proto_esphome.c`, `proto_wyoming.c` |
| `src/tools/` | `mixcap`, `mixplay`, `pryon_test`, `runas` (drops root: AIPC refuses uid 0, the image has no `su`) |
| `src/third_party/monocypher.[ch]` | X25519, ChaCha20-Poly1305 for the Sendspin Noise handshake; BSD-2-Clause OR CC0, <https://monocypher.org>, 4.0.2 |
| `src/third_party/minimp3.h` | MP3 decoder, public domain (CC0), <https://github.com/lieff/minimp3> |
| `src/include/` | C headers for the reversed `libmixerAPI.so` and `libpryon.so` |
| `scripts/` | PC side: `deploy.sh`, `probe.sh`, `capture-test.sh`, `wifi-join.sh`, `install-system.sh` |
| `scripts/device/` | run on the Echo: `alexa-off.sh`, `alexa-on.sh`, `lockdown.sh`, `wifi-join.sh`, `run.sh` |
| `scripts/system/` | boot integration: `hassmic.rc`, `boot.sh`, `sepolicy.rules` |
| `tools/` | OTA payload dumper, Thumb disassembly helpers, `qrun.sh` (device binaries under qemu-arm) |
| `tests/` | `fake_ha_esphome.py` (reference `aioesphomeapi` client) and `fake_ha.py` (Wyoming) against a host or qemu build |

## External files (not in this repository)

Proprietary or third-party; fetch them yourself and put them in the repository root. All are git-ignored.

| File | Source | sha256 |
|---|---|---|
| `update-kindle-donut_puffin-NS65741_user_8138_0013222529668.bin` | stock Fire OS 6574.1 for `donut_puffin`, from FTVDB | `ac22b78cf94c2ebacfa90447b770b803d9f72f35858d5218975d97fea95a0245` |
| `kamakiri-donut-v1.0.0.zip` | bootloader unlock + TWRP, attachment of the XDA thread for kamakiri-donut | `4d2bb52eaf661f6616aa4268584d44cf9155c1328c34bfcba01a6558f942b738` |
| `boot-root.zip` | root adb + permissive `su` SELinux domain, same XDA thread | `de49cc88b27a8e77cf97cf0156bee50e4ddc0e116c41aaede06b494e38397be0` |
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

Secrets live in `secrets/` (ignored): `secrets/wifi.conf`, line 1 SSID, line 2 passphrase.

## Build

```sh
make            # ARM binaries into build/ (NDK r21e, API 24, armv7, lld)
make host       # PC build + qemu build for tests
.venv/bin/python tests/fake_ha_esphome.py        # needs: pip install aioesphomeapi
.venv/bin/python tests/fake_ha.py [--qemu]        # Wyoming; needs: pip install wyoming
```

## Install on a device

1. Get USB access to the Echo and unlock it with kamakiri-donut; flash the stock OTA to both slots and `boot-root.zip`
   (steps are in the XDA thread). Do **not** register the device with the Alexa app.
2. `scripts/probe.sh` — checks that the device libraries match the analysed firmware.
3. `scripts/deploy.sh`, then on the device `lockdown.sh` **before** the first Wi-Fi join, then `scripts/wifi-join.sh`.
   The lock allows local addresses only (private ranges, link-local, multicast), so Home Assistant may sit in any local subnet.
   Put the Echo on a network without internet access as a second layer.
4. Try it: `adb shell /data/local/hassmic/run.sh`. Home Assistant discovers an ESPHome device (or add it by IP, port 26053,
   no encryption key). With `-P wyoming`: Wyoming integration, port 16700.
5. Make it permanent: `scripts/install-system.sh "<name>"`. Goes through TWRP, adds `/system/hassmic/`,
   `/system/etc/init/hassmic.rc` and three allow rules to `/sepolicy` (stock copy kept as `/sepolicy.pre-hassmic`).

Undo: delete `/data/local/hassmic/hassmic.conf` (boot script then does nothing), or `scripts/install-system.sh --uninstall`,
or reflash from TWRP (hold Volume Up while powering on).
