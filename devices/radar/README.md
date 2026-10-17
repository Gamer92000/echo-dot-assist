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
`firmware/radar/` (not in git):

| File | Source | sha256 |
|---|---|---|
| `amonet-radar-v1.0.0.zip` | the XDA thread | `ecdb07bc05a508532e5ffed77121592d492b1a91572839e0f17545421f398f1a` |
| `update-kindle-radar_puffin-NS6572_user_6436_0012718777476.bin` | [FTVDB](https://ftvdb.com/echo/firmware/com.amazon.radar.android.os/17af64ecc5b8f0303daf4e2a37c75b1a-12718777476-fire-os-6-5-7-2-ns6572-6436-2026-03-21/) (build the thread pins) | `976e2761c5bb9e5d3d9a2b1b9bc2f2296b89a5b37057ab1780743aa075f2df48` |
| `boot-root.zip` | the XDA thread (byte-identical to donut's; its patches find their sites by pattern) | `de49cc88b27a8e77cf97cf0156bee50e4ddc0e116c41aaede06b494e38397be0` |

### USB access

The Echo 2 has no USB socket. The USB data lines end on pads of the amplifier/tweeter board (pictures in the XDA
thread); solder a data-capable USB cable to them, or use a stable three-pin pogo jig:

| USB cable wire | Pad |
|---|---|
| black (GND) | TP13 |
| white (D−) | TP14 |
| green (D+) | TP15 |
| red (VCC, 5 V) | **nothing**: leave it unconnected and insulate its end |

The Echo takes its power from its own adapter, which the unlock also needs: fastboot mode is entered by plugging it in.

### Unlock and flash

One script, as on `biscuit`: unzip the package, run `./fastbrick.sh`, put the Echo into fastboot mode (power adapter
out, plug it back in while holding the action button •, until the ring shows a **green** LED; an Echo unlocked before shows a rainbow), type `YES` when asked,
do not interrupt after the 10-second grace period, and it ends in TWRP (white LED). Then, as the thread says: wipe, flash
the firmware, `adb reboot recovery` (TWRP makes the slot it flashed active, so the second install lands in the other
one; do not switch back with `bcbtool`, or it goes into the same slot again), flash again, and
install `boot-root.zip` for root adb. `scripts/setup.sh radar` does all of that.

## Getting the newest firmware (FTVDB lags; radar's listing stops at 6572)

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
