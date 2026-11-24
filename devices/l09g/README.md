# Mi Smart Speaker L09G: experimental port

[Русская версия](README_RU.md) · [Original echo-dot-assist](https://github.com/Gamer92000/echo-dot-assist)

This branch adapts `hassmic` to one rooted Xiaomi Mi Smart Speaker L09G. It is **vibecoded**: the device port, integrations and supporting scripts were developed with OpenAI Codex and iteratively tested by the owner on a real speaker. It is a community experiment, not a Xiaomi product or an upstream release. The upstream base is commit `1d7d957139f24788f06acc7761854fe2c80d19a0`.

**Status: source and development notes, not a tested fresh-device installer.** Root, SpeakerLab's writable `/data` overlay, a way to recover the device, and knowledge of its firmware are prerequisites. The separate [rooting notes](ROOT_RU.md) describe the owner's experience and the risks of bypassing the BL33 check (in Russian). Rooting is not part of the port installer. Do not use the Echo-specific installer or upstream OTA updates on an L09G. Back up the device before changing boot scripts; losing access or a failed boot is possible. The tests below do not establish compatibility with other firmware builds or devices.

## What was checked on the owner's speaker

| Feature | Result |
| --- | --- |
| Home Assistant voice satellite and local microWakeWord | Alexa tested; Alexa, Okay Nabu and Hey Jarvis models available, one active at a time |
| Wake sound and assistant LED animations | Confirmed |
| Microphone mute, action and volume buttons | Confirmed; volume steps are 3% |
| HA light entity | Ring on/off, colour and brightness confirmed |
| Music Assistant / Sendspin | Discovery and playback confirmed |
| Bluetooth speaker | Playback confirmed; HA switch controls new pairing, previously paired devices can reconnect with pairing off |
| Drop In | Two-way audio with an Echo 1 confirmed; incoming and active-call LED indications confirmed |
| Web UI and Wi-Fi | Login, scan and switching confirmed; Wi-Fi failure and rollback cases also exercised in a local simulator |

These are observations on one unit. Echo-specific web settings are not necessarily functional on Xiaomi. This port uses local microWakeWord, not Amazon's proprietary wake engine. Sound-pack uploads and microphone settings have not been systematically validated on L09G. Models used in the original setup were obtained separately from [ESPHome micro-wake-word-models](https://github.com/esphome/micro-wake-word-models) at `40ff33f57f8fc6ad71a75ef085abbf742495b225`; see that project for licensing and provenance.

## Build

Build on Linux with `make`, Python 3, and a compatible AArch64 glibc cross compiler. The working build used [Bootlin aarch64 glibc stable 2018.11-1](https://toolchains.bootlin.com/releases_aarch64.html). The build also needs the matching shared libraries from the speaker firmware, placed in a private directory outside Git:

```text
libpulse-simple.so.0.1.1
libpulse.so.0.20.3
libopus.so.0.8.0
libz.so.1.2.11
libdbus-1.so.3.19.11
```

Find and copy these from your own device or a legally obtained matching firmware. Do not copy device state, keys or credentials into the repository. Their filenames and ABI are firmware-dependent. These libraries are used for linking and should not be blindly copied back to the speaker. Set paths on your build machine:

```sh
make -f Makefile -f devices/l09g/port.mk l09g \
  PORT_CC=/path/to/aarch64-linux-gcc \
  PORT_LIB=/path/to/target-libs
```

Results: `build/l09g/hassmic` and `build/l09g/bt-agent`. The default paths, if not overridden, are `devices/l09g/toolchain/aarch64--glibc--stable-2018.11-1/bin/aarch64-linux-gcc` and `devices/l09g/target-libs`. The upstream Makefile is included to reuse source and generated assets; use the explicit `l09g` target above, not the default Echo target.

## Runtime notes

The tested layout on the speaker is `/data/l09g-assist`: built binaries and the `.sh` files from this directory go there. Mark the binaries and scripts executable on the speaker (for example, `chmod 755 /data/l09g-assist/hassmic /data/l09g-assist/bt-agent /data/l09g-assist/*.sh`). `start-assist.sh` expects PulseAudio, BlueZ, Avahi and the `playback` ALSA PCM from the original system. It creates the `l09g_direct` sink and starts the pairing and Wi-Fi helpers. State, including ESPHome and Drop In keys, lives under `/data/l09g-assist/state` and must stay private. The web UI listens on port `28931`; use the speaker's current address, select **Ask the Echo**, then press its action button. Sendspin listens on `28928`.

Local wake models live under `state/mww/<id>/` as `manifest.json` and `model.tflite`. After placing compatible models there, select one with:

```sh
sh /data/l09g-assist/select-wake.sh alexa
# alternatives: okay_nabu, hey_jarvis
```

The tested setup suppresses the stock voice, call, stereo and Harman audio init scripts while keeping PulseAudio and BlueZ. `disable-stock-audio.sh` requires the stock voice agent to have been stopped **and** the watchdog keeper to be running; see its checks before attempting changes. The exact first-time migration, model deployment and watchdog handover are not packaged or validated as an installer. In particular, do not stop the stock agent until you understand the device watchdog and have a tested recovery route.

For the tested SpeakerLab overlay layout, `restore-stock-audio.sh` removes only its marked overlay init scripts and the matching Avahi service; reboot to start the stock services. This is not a full factory reset and does not remove `/data/l09g-assist`. Recovery has not been verified on other overlay layouts.

## Local checks

These tests use simulated inputs and do not connect to Home Assistant or a speaker:

```sh
python3 devices/l09g/test_wifi_service.py
cc -std=gnu11 -O2 -Wall -Wextra -I src/hassmic \
  devices/l09g/test_led_states.c src/hassmic/statefile.c \
  -lpthread -lm -o /tmp/l09g-led-test
/tmp/l09g-led-test
```

## Licences and unpublished files

The upstream project's [licence and third-party notices](../../README.md#licence) still apply. The checked-in zlib headers retain their own notices. Speaker firmware, extracted libraries, root credentials, wake-model files and personal device state are not included in this port. Check the terms of each separately obtained component before distributing it.
