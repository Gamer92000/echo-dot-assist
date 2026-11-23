# Xiaomi XiaoAI Speaker Play LX05: experimental port

[Русская версия](README_RU.md) · [echo-dot-assist](https://github.com/Gamer92000/echo-dot-assist)

The LX05 port is **vibecoded**: its hardware adaptations, integration and documentation were developed with OpenAI Codex, with iterative tests on the owner's speaker. It is a community experiment and has not been audited. The base is the `xiaomi-l09g` branch of this fork.

**Status: source code and development notes, not a tested fresh-device installer.** The working device had root, a writable `/data`, a working SSH server and an `/etc` overlay before the port was deployed. Rooting, initial overlay/boot setup, Wi-Fi credentials, SSH provisioning and recovery are not automated here. Do not use the Echo installers or OTA updater on this speaker. Changes to boot settings can make a device inaccessible; keep a verified recovery method and a private backup of its boot environment and data. The working device retained its signed, read-only SquashFS instead of rewriting it. Other firmware versions may differ.

## Tested features and limits

On one LX05, firmware 1.46.2, ARMv7, Linux 4.9.118, the owner confirmed: Home Assistant voice assistant and local Alexa wake word; Sendspin playback; Bluetooth audio, ducking during assistant speech and an HA switch for new pairing; two-way Drop In; web login and Wi-Fi scanning; buttons and RGB status. Bluetooth and phone volume can still be separate controls. There is no dedicated HA light entity. Mute is software mute in Assist, not a physical microphone disconnect. No full acoustic echo cancellation was established for calls.

Those results describe the existing device configuration, not an automated fresh installation. The published version replaces device-specific LAN and MAC values with configuration/runtime discovery; these portability changes have not been tested on a second physical unit. mDNS uses the fixed instance `xiaomi-lx05`, so multiple LX05 units on the same LAN require further work to give them distinct names.

The web UI is on port 28931: choose **Ask the Echo** and press the centre button. Sendspin uses port 28928 and ESPHome API port 26053. The `state` directory contains credentials and pairing keys; never publish it. Wake models are distributed separately; see [ESPHome's micro wake word models](https://github.com/esphome/micro-wake-word-models) for their licensing.

## Build on Linux

The working build used GCC 11.2.1 from an ARMv7 hard-float musl toolchain, Opus 1.5.2 built statically for that target, compatible zlib headers, Python 3 and GNU make. No proprietary device libraries or prebuilt firmware are included. Obtain the toolchain and Opus independently, check their provenance, and build Opus for `arm-linux-musleabihf` with static output enabled. The previously used musl.cc `arm-linux-musleabihf-cross.tgz` archive had SHA-256 `11e155a090789c854afb5d91e57c81f441ccdb92f7f277789c3a6d5884a68e58`; the Opus 1.5.2 archive had SHA-256 `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1`. Verify downloads rather than relying on filenames.

```sh
make -f Makefile -f devices/lx05/port.mk lx05 \
  LX05_CC=/path/to/arm-linux-musleabihf-gcc \
  LX05_OPUS=/path/to/opus-1.5.2/.libs/libopus.a \
  LX05_ZINC=/path/to/zlib-headers
```

Outputs are `build/lx05/hassmic` and `build/lx05/mdns`. `libasound-link.so` and `libz-link.so` built alongside them are **link-time placeholders only**: never install or run them. At runtime the tested system used the speaker's own ALSA and zlib libraries plus the matching musl loader and libatomic from the cross-toolchain in `/data/lx05-assist/lib`; the launcher explicitly selects that loader. Use only libraries you have the right to use. Generated geography and web sources are built from the repository by make.

## Runtime layout and integration

The example scripts expect `/data/lx05-assist`, `state`, `models`, `wifi`, a running BlueZ/BlueALSA, ALSA dmix/softvol, and a local wake model at `state/mww/<model>/`. The deployed launcher uses `/dev/input/event0`, ESPHome, and `Xiaomi-LX05`; actual input mapping can differ by firmware. Place the target musl loader and runtime libraries in `lib/`, and deploy `hassmic`, `mdns`, `start-lx05.sh`, `clock-watch.sh`, `wifi-service.sh` and `bt-pair-service.sh` with appropriate permissions. Additional overlay and init integration must be devised for the target device; these files alone do not install the port.

The tested ALSA arrangement routes BlueALSA through an `lx_bt` PCM with a `lx05_bt_duck` softvol, then through the common `mysoftvol`; Bluetooth's existing init script needs to select that PCM and start `bt-pair-service.sh`. The original mixer and init configuration must be backed up and adapted to the exact firmware. Pairing is off when the state file is absent; the helper applies the HA switch by changing BlueZ Pairable and Discoverable, leaving the adapter on. A separate system pairing agent may be needed.

The tested device starts through an early boot hook, mounts `/data`, uses overlay for `/etc`, disables competing stock voice/audio services and configures a local-only IPv4 policy route before Wi-Fi starts. This branch does **not** supply or verify that boot hook, SSH setup, network guard, ALSA configuration or a recovery installer. Keep a UART recovery path and verify your own setup before relying on it. The Wi-Fi helper saves its network in `/data/lx05/wpa.conf`; it supports switches only within the configured /24 and attempts rollback on failure. Test rollback before relying on web-based changes.

Create `/data/lx05-assist/network.conf` (a root-owned shell file) with the first three octets of your own /24 LAN, for example:

```sh
LX05_LAN_PREFIX=10.42.0.
```

The launcher waits for an address on that prefix, policy rule priority 100 pointing to table 123 and an unreachable default in that table. **These checks do not create or fully verify isolation.** Configure the guard independently before Wi-Fi starts, include LAN, DHCP and mDNS as required, disable IPv6 or protect it separately, and test both IPv4 and IPv6 egress. Policy routing is not a firewall: hosts and proxies within the allowed LAN may still forward traffic. Do not assume the stock cloud or OTA is blocked until the resulting configuration is verified.

On the tested unit the kernel input codes for microphone mute and volume down were reversed; `LX05_PORT` remaps them only for LX05. `led_lx05.c` writes the single packed GRB LED under `/sys/class/leds/sunxi_led0a/brightness` after the stock LED service is stopped. The clock helper accepts time from the paired HA connection. Review these firmware-specific paths and assumptions before deployment.
