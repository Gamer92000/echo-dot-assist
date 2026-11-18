# Echo 2nd gen (full size), 2017 (`radar`)

The 2017 tower Echo: 7-microphone array, full-size speaker. Installed on a real Echo 2 through `scripts/setup.sh radar`
(2026-09-28); identity, build config and the audio-stack facts below are checked against the pinned firmware, the
values in `board.c` on the running Echo. `device.conf` still has donut's service lists, marked "verify". Porting guide:
[devices/README.md](../README.md).

## What the firmware says (checked 2026-09-28)

Pinned firmware: `update-kindle-radar_puffin-NS6572_user_6436_0012718777476.bin` — Fire OS 6572, one build older than
donut's 6574.1, same generation (Android 7.1.2, product `radar_puffin`, donut-style partitioning). Against `donut`:

- `lib/libmixerAPI.so`: byte-different build, **same 334 exports, zero difference** — the reversed client API holds.
- `lib/libpryon.so`: the **slimmer engine generation** (9.9 MB / 1609 exports vs donut's 19.8 MB / 3901, the same split
  as `crumpet`); every symbol `wake_pryon.c` needs is present, and `pryon_test` under qemu detects ALEXA against the
  model shipped in this image (`RESULT keyword=ALEXA type=2`).
- `bin/mixer`: different build; stream types and the `/data/mixer_streams/` mechanism assumed same-generation (verify
  with `mixcap` on the device).
- init trigger `on property:com.amazon.puffin.PUFFIN_START=1 && property:audio_mixer.init=true` — identical to donut's,
  so `hassmic.rc` is donut's verbatim.
- ALEXA model ships at `/system/local/models/keyword/en-US/ALEXA/`; earcons at `/system/local/share/earcon/base/`.

Model XC56PY. Hardware: MediaTek **MT8163V**. **No USB socket**: like the Dot 3, USB has to be soldered on (below).
7-mic array: better far-field than any Dot.

## Unlock (public, from R0rt1z2)

XDA thread with the full instructions: [UNLOCK][ROOT][TWRP][UNBRICK] Amazon Echo 2nd Gen / 2017 (radar)
<https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-2nd-gen-2017-radar.4801290/>. These files go into
`firmware/radar/` (not in git); the unlock's and the root's own files (`boot-root.zip`) are the thread's: it says
which, kept current with its procedure, and nothing here downloads or reads them.

| File | Source | sha256 |
|---|---|---|
| `update-kindle-radar_puffin-NS6572_user_6436_0012718777476.bin` | [FTVDB](https://ftvdb.com/echo/firmware/com.amazon.radar.android.os/17af64ecc5b8f0303daf4e2a37c75b1a-12718777476-fire-os-6-5-7-2-ns6572-6436-2026-03-21/) (build the thread pins) | `976e2761c5bb9e5d3d9a2b1b9bc2f2296b89a5b37057ab1780743aa075f2df48` |
| `magiskpolicy32` | `lib/armeabi-v7a/libmagiskpolicy.so` of `Magisk-v30.7.apk`, [Magisk v30.7](https://github.com/topjohnwu/Magisk/releases/tag/v30.7) (APK `e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5`); `scripts/setup.sh` fetches it itself | `e5fafc1fa9486950ce9ba476f5723754d260014515ced72b4560238e6c560f3a` |

### USB access

The Echo 2 has no USB socket. The USB data lines end on pads of the amplifier/tweeter board (pictures in the XDA
thread); solder a data-capable USB cable to them, or use a stable three-pin pogo jig:

| USB cable wire | Pad |
|---|---|
| black (GND) | TP13 |
| white (D−) | TP14 |
| green (D+) | TP15 |
| red (VCC, 5 V) | **nothing**: leave it unconnected and insulate its end |

The Echo takes its power from its own adapter, not from USB.

Use a cable with a USB-A plug for the PC end. With a USB-C plug there (a cut USB-C to USB-C cable), the PC sees no
device: a USB-C port switches the connection on only once the plug's CC wire (often yellow) goes to ground through a
5.1 kΩ resistor, which sits in the device a cable normally leads to (forum, 2026-10-22). Add that resistor, or take a
USB-A cable, which needs none.

### Unlock and flash

Once, first: USB access without sudo (`scripts/setup.sh` does the same).

```sh
sudo cp scripts/51-echo-unlock.rules /etc/udev/rules.d/ && sudo udevadm control --reload
```

Unlock the Echo **by hand, as the XDA thread says**, from its first post: which Echos it is for, its files, how to get
the Echo ready and every command are there, kept up to date. R0rt1z2 asks (since kamakiri-donut v1.1.0) that the unlock
is only ever run that way, never from an installer or a script, so this page does not repeat its steps and
`scripts/setup.sh radar` does not run them either: it names the thread, waits for Enter and then checks for TWRP on adb.
**Stop once TWRP is running (white ring)**: the firmware is this project's step, and boot-root comes after it. Stuck, or
the Echo does something the thread does not say? Ask in the thread, with what the Echo did, in your own words, and the
tool's output or log as it is.

Then, as on donut: wipe, flash the firmware, `adb reboot recovery` (TWRP makes the slot it flashed active, so the second
install lands in the other one; do not switch back with `bcbtool`, or it goes into the same slot again), flash again
(`scripts/setup.sh radar` does that; by hand: [donut's README](../donut/README.md#1-unlock-flash-stock-firmware-root)).
Then root by hand: `boot-root.zip` installed from TWRP as the thread says, until Fire OS has started with root adb
(`adb shell id -u` says 0); the setup waits for Enter and checks.

## Getting the newest firmware (FTVDB lags; radar's listing stops at 6572)

For porting only, not for installing: an install needs exactly the pinned firmware below (setup takes no other file,
issue #62); a newer build works only once it is re-pinned as described at the end of this section.

Once the Echo is unlocked and rooted, the device itself reveals its latest update: watch
`adb shell "logcat | grep -iE 'updateURL|http.*update|ota'"` while asking Alexa to check for updates (the check also
runs at boot), take the signed URL and download the bin on the PC — or, if nothing is logged, let the update download
but not reboot and copy the payload out of update_engine's staging (path in logcat, root-readable). Keep `otad`
stopped while doing this on purpose (`stop otad`), or use the OTA guard from `scripts/device/lockdown.sh`. If a newer
build than the pin below is fetched and flashed, re-pin: unpack, re-check the audio stack, rebuild, `probe.sh`.

## First contact (arrival checklist)

1. Keep it offline until the pinned firmware is on it.
2. Solder USB to the amplifier/tweeter board (see [USB access](#usb-access)); run the unlock following the XDA thread.
3. After unlock: `adb shell getprop` — check `PRODUCT`/`FIRMWARE_ID`, then the "verify" marks
   (keypad — the tower has a mic-mute ring and an action button, no volume buttons?; thermal zone; puffin service
   user/groups; `bcbtool get_active`).
4. Try without installing: `scripts/probe.sh`, `scripts/deploy.sh`, `run.sh`.

## Open questions

- Input devices (answered 2026-09-28, on the Echo): as on `biscuit`, `mtk-kpd` (event1: action = KEY_HELP, mute =
  KEY_MUTE, volume down) and `keys` (event2: volume ±). No `gpio-privacy` device, but the keypad driver keeps the mute
  latch: `/sys/devices/soc/10010000.keypad/amz_privacy/state` (1 = mics off, button lit; found 2026-09-30). hassmic
  reads that; until then it counted key presses and showed the mute the wrong way round after a restart with the
  mics off.
- Whether amonet-radar's TWRP exposes the same A/B/lptools layout as donut's (`INSTALL=twrp-ab` assumption).
- The 7-mic array: does the mixer expose the same post-AEC `micAsr` stream?
