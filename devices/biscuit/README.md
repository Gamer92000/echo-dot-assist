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
`firmware/biscuit/` (not in git):

| File | Source | sha256 |
|---|---|---|
| `amonet-biscuit-v2.0.1.zip` | the XDA thread | `ea9cada067783a0d83f025bdd669fd5689148f555054475c7c70c634c542f3b5` |
| `boot-root.zip` | the XDA thread (byte-identical to donut's) | `de49cc88b27a8e77cf97cf0156bee50e4ddc0e116c41aaede06b494e38397be0` |
| `update-kindle-biscuit_puffin-NS65741_user_8146_0013222531716.bin` | [FTVDB](https://ftvdb.com/echo/firmware/com.amazon.biscuit.android.os/8ca06ee4ef2806c974d2944b7fadd543-13222531716-fire-os-6574-1-ns65741-8146-2026-09-17/) | `90832e86498c5e803974c30359aea71c1129757be0ddc59d831a94b27f81487f` |

Use exactly this firmware version, also when the Echo already runs a newer one: hassmic works against its binaries,
`scripts/setup.sh` only takes the file with this checksum (another version renamed to its name does not pass, issue
#62), and `scripts/probe.sh` checks the Echo for it. Flashing it during the unlock replaces whatever the Echo ran before.

The unlock is **one script**: unzip the package, run `./fastbrick.sh` (Linux), put the Echo into fastboot mode —
unplug its power, reconnect while holding the **action button** (the one with the circle) until the ring shows a
**green LED** (an Echo unlocked before shows a rainbow) — and type `YES` when the script asks. It runs the whole exploit (about a minute, the ring shows
progress) and reboots the Echo into TWRP (white LED). **Do not interrupt it after the 10-second grace period — that
can brick the device.** `amonet-biscuit` **v2 writes newer bootloaders and boots Fire OS 6** — the stream the
pinned firmware belongs to (Fire OS 5 versions do not boot from v2.0.0 on). v2.0.1's kaeru boots the active slot
(v2.0.0's always `boot_a`). Flash `boot-root.zip` after every stock
firmware flash: it gives root adb, as on donut.
- The exploit needs a payload made for the Echo's LK (bootloader) build: `fastbrick.sh` reads `fastboot getvar
  lk_build_desc` and picks one by `profile.sh`, else the default `fastbrick.img`, which a newer LK refuses — fastboot
  answers "the command you input is restricted on locked hw" (issue #50). v2.0.1 maps `63cb91b-20221007_072309`
  (Fire OS 6 4405 to 8149) to `fastbrick-20221007.img` and `2c3569d-20260617_045829` (Fire OS 8338, NS6575,
  October 2026) to `fastbrick-20260617.img`; v2.0.0 knew only the first. That second payload's tail is built from
  8338's LK (135,403 of its 135,411 16-byte windows in the last 137,684 bytes are in 8338's `lk.img`, 122,746 in
  8149's), but no unlock with it is reported yet. An LK newer than both: the guided setup says so when the Echo is
  still in fastboot a minute after the script; power-cycled, the Echo boots the original software again, unharmed.
  Unlock before the Echo can update itself further.
- From Windows (issue #60): [firebreak](https://github.com/bboe/firebreak) unlocks it, and `scripts/setup.sh` in Git
  Bash runs it as its unlock step (pinned `firebreak@0.8.0`, `uvx --managed-python firebreak@0.8.0 unlock fireos6`).
  It needs `adb` and `fastboot` (platform-tools), [uv](https://docs.astral.sh/uv/) and two USB drivers, installed once
  from an administrator PowerShell as [its README says](https://github.com/bboe/firebreak#windows). firebreak writes
  amonet **v2.0.1**, as the setup does on Linux, with TWRP 3.7.0, downloads what it needs itself
  against pinned hashes, and ends where the flash and root steps end here: Fire OS 6574.1 build 8146 in both slots,
  `boot-root.zip` installed, root adb. The setup then checks that and goes on with the build. Not run here as a whole
  yet; firebreak's own README has it measured on Windows 11.
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
- A spoken wake word and a spoken command with the current build (a simulated wake word runs the pipeline through Home
  Assistant; the microphone stream is alive and hears the Echo's own wake sound).
- A2DP with a phone and BLE pairing not tried on biscuit yet (the controller comes up; see the table). A phone paired
  and played on the Echo 2 (radar), same MT8163 combo, 2026-10-10.

`INSTALL=twrp-ab` and the policy patch (`magiskpolicy32` from donut's boot-root zip) worked unchanged on the first biscuit (2026-09-28):
`scripts/install-system.sh` wrote `system_a`, the Echo booted into the satellite.
