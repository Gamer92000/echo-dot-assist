# Echo Dot 2nd gen, 2016 (`biscuit`)

Model RS03QR. **Tested on a real Echo Dot 2** (2026-09-28): install through `scripts/setup.sh biscuit`. Identity, build
config and the audio-stack facts below are checked against the pinned firmware, the values in `board.c` and
`device.conf` on the running Echo (2026-09-30). Porting guide: [devices/README.md](../README.md).

## What the firmware says (checked 2026-09-28)

Pinned firmware: `update-kindle-biscuit_puffin-NS65741_user_8146_0013222531716.bin` — **Fire OS 6574.1 (NS65741), the
same build generation as donut**, Android 7.1.2, product `biscuit_puffin`, donut-style partitioning (preloader in boot0,
no `brhgptpl`/NAND layout like `crumpet`). Against `donut`'s NS65741 image:

| File | |
|---|---|
| `lib/libmixerAPI.so`, `lib/libpryon.so`, `lib/libAmazonKWD.so`, `libopus.so` | **byte-identical** — the reversed mixer client API and the wake word engine carry over unchanged |
| `bin/mixer` | different build, same stream types (`micAsr`, `micMultiChAsr`, `Music`, `Earcon`, …) and the same `/data/mixer_streams/` descriptor mechanism |
| `lib/libasp.so`, `bin/PuffinApp`, `bin/ledcontroller` | different (SoC audio processing, device client, LED layout) |
| init trigger | `on property:com.amazon.puffin.PUFFIN_START=1 && property:audio_mixer.init=true` — identical to donut's, so `hassmic.rc` is donut's verbatim |
| wake model | ALEXA ships in the image at `/system/local/models/keyword/en-US/ALEXA/` |
| earcons | `/system/local/share/earcon/base/`, same layout as donut |
| stock services | all of donut's names exist (`puffin`, `puffinmrmd`, `dacd`, `smarthomed`, `otad`, `ace_otad`, `btmanagerd`, `uxeventd`, `wifisvc`, `mixer`, `shmd`, `ledcontroller`, …) |
| `/sepolicy` | same generation, slightly different (448 070 vs donut's 448 928 bytes) |
| Bluetooth (on the Echo, 2026-09-28) | `/dev/stpbt` (MediaTek WMT, H4, `bluetooth:net_bt_stack 0660`), powered on by opening it, like donut. The chip refuses LE Set Event Mask with the 4.2 bits (P-256, DHKey: status 0x20); hassmic retries with the 4.0 set, then: ACL 4 x 1021 (no separate LE buffers), legacy pairing only, A2DP sink and scanning up |
| IPv6 (on the Echo) | no `ip6tables` in the image: `lockdown.sh` switches IPv6 off instead of filtering it |
| `ip` (on the Echo) | missing; toybox `ifconfig` is there |
| input devices (on the Echo, 2026-09-30) | event1 `mtk-kpd`: KEY_HELP (action), KEY_MUTE, KEY_VOLUMEDOWN; event2 `keys`: volume ±; event0 `ACCDET` (jack) |
| thermal zones (on the Echo) | `mtktswmt`, `mtktscpu` (zone 1, the one hassmic reads), `mtkts1`, `mtkts5`, `mtkts3`, `mtkts4`, `mtktspmic`, `tmp103` |
| mute latch (on the Echo) | `/sys/devices/soc/10010000.keypad/amz_privacy/state`, world-readable, 0 with the mics on. **Do not `cat` the other files there**: reading `power_button_state` crashes the kernel (NULL gpio in `get_power_button_state`), the watchdog reboots the Echo |
| puffin service (on the Echo) | `/init.project.rc`: user `puffin`, groups `aipc davs dbus inet ace_kvstore ace_group drmrpc system audio keystore cache shell ace_maplite` |
| A/B (on the Echo) | `ro.boot.slot_suffix=_a`; no `bcbtool` in the running image |
| speaker gain (on the Echo, 2026-10-11) | DAC TI TLV320AIC32x4 at i2c 2-0018 (mics: four TLV320AIC3101 at 0-0018..1b). Its `HP Driver Gain Volume` (0..35 = -6..+29 dB) feeds the speaker amplifier; the audio HAL sets it to 6 (0 dB) when the mixer starts (`/system/etc/audio_device.xml`, `ext_speaker_output`), 11 for the 3.5 mm jack. The settings page's "Speaker gain" raises it (`board.c` `speaker_gain`, `spkgain.c`) |

Hardware: MediaTek **MT8163**, quad Cortex-A53 @ 1.5 GHz, 512 MB RAM (64-bit kernel, 32-bit userspace), micro-USB with
data. Reference project: [EchoMuse](https://github.com/wilbowes/EchoMuse) (same idea for this model, but it replaces the
whole OS instead of keeping Amazon's audio front end).

## Unlock (public, from R0rt1z2)

XDA thread with the full instructions: [UNLOCK][ROOT][TWRP][UNBRICK] Amazon Echo Dot 2nd gen (2016, biscuit)
<https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-2nd-gen-2016-biscuit.4761416/>. These files go into
`firmware/biscuit/` (not in git); the unlock's and the root's own files (`boot-root.zip`) are the thread's: it says
which, kept current with its procedure, and nothing here downloads or reads them.

| File | Source | sha256 |
|---|---|---|
| `magiskpolicy32` | `lib/armeabi-v7a/libmagiskpolicy.so` of `Magisk-v30.7.apk`, [Magisk v30.7](https://github.com/topjohnwu/Magisk/releases/tag/v30.7) (APK `e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5`); `scripts/setup.sh` fetches it itself | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |
| `update-kindle-biscuit_puffin-NS65741_user_8146_0013222531716.bin` | [FTVDB](https://ftvdb.com/echo/firmware/com.amazon.biscuit.android.os/8ca06ee4ef2806c974d2944b7fadd543-13222531716-fire-os-6574-1-ns65741-8146-2026-09-17/) | `90832e86498c5e803974c30359aea71c1129757be0ddc59d831a94b27f81487f` |

Use exactly this firmware version, also when the Echo already runs a newer one: hassmic works against its binaries,
`scripts/setup.sh` only takes the file with this checksum (another version renamed to its name does not pass, issue
#62), and `scripts/probe.sh` checks the Echo for it. Flashing it during the unlock replaces whatever the Echo ran before.

Once, first: USB access without sudo (`scripts/setup.sh` does the same).

```sh
sudo cp scripts/51-echo-unlock.rules /etc/udev/rules.d/ && sudo udevadm control --reload
```

Unlock the Echo **by hand, as the XDA thread says**, from its first post: which Echos it is for, its files, how to get
the Echo ready and every command are there, kept up to date. R0rt1z2 asks (since kamakiri-donut v1.1.0) that the unlock
is only ever run that way, never from an installer or a script, so this page does not repeat its steps and
`scripts/setup.sh biscuit` (Linux) does not run them either: it names the thread, waits for Enter and then checks for
TWRP on adb. **Stop once TWRP is running (white ring)**: the firmware is this project's step, and boot-root comes after
it. Stuck, or the Echo does something the thread does not say? Ask in the thread, with what the Echo did, in your own
words, and the tool's output or log as it is.

Then flash the pinned firmware into both slots, as on donut (`scripts/setup.sh biscuit`, or
[donut's README](../donut/README.md#1-unlock-flash-stock-firmware-root) by hand), and after it root by hand:
`boot-root.zip` installed from TWRP as the thread says, after every stock firmware flash, until Fire OS has started with
root adb (`adb shell id -u` says 0); the setup waits for Enter and checks. Unlock before the Echo can update itself
further.

- From Windows (issue #60; for beginners step by step: [docs/WINDOWS.md](../../docs/WINDOWS.md)): an Echo unlocked
  already, with the unlock and boot-root done as the thread says, on adb over USB; `scripts/setup.sh` in Git Bash does
  the rest, as for every model. The guided setup no longer runs [firebreak](https://github.com/bboe/firebreak)
  (2026-11-09): it ran amonet from the setup, and the unlocks are done by hand only.
- No Magisk: this repo's daemon runs as the stock client's user; TWRP plus the system write is the whole root story.

## First contact (arrival checklist)

1. Keep it offline (no Wi-Fi, no registration) until the pinned firmware is flashed.
2. Micro-USB into the PC; run the unlock following the XDA thread. No soldering anywhere.
3. After unlock and the pinned flash: `adb shell getprop` — check `PRODUCT`/`FIRMWARE_ID` against `device.conf`, then
   compare with the table above: `getevent -il` (keypad), `/sys/class/thermal/thermal_zone*/type`,
   `/proc/idme/bt_mac_addr`, the puffin service's user and groups in `/init.project.rc`, `getprop ro.boot.slot_suffix`.
4. Try it without installing: `scripts/probe.sh`, `scripts/deploy.sh`, `adb shell sh /data/local/hassmic/run.sh`.

## Open questions

- Mute latch: the file is there and reads 0 with the mics on (2026-09-30); not yet read with the button lit, nor the
  ring and Home Assistant watched over a press with the build that reads it.
- BLE pairing not tried on biscuit yet (the controller comes up; see the table).

Answered (issue #106, 2026-11-02, build 2026.11.02.000053-beta): a spoken wake word and command reach Home Assistant
with Amazon's engine ("Alexa", and "Echo" and "Ziggy" de-DE from the page's "Download from Amazon", through four
Assist pipelines) and with microWakeWord ("Hey Jarvis" of ESPHome's collection, community models); a phone paired and
played over A2DP; "Request and reply text" fills its sensors.

`INSTALL=twrp-ab` and the policy patch (`magiskpolicy32` from donut's boot-root zip) worked unchanged on the first biscuit (2026-09-28):
`scripts/install-system.sh` wrote `system_a`, the Echo booted into the satellite.
