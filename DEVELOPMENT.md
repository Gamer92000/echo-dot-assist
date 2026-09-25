# Development

For users: [README.md](README.md). Progress, open issues and every measurement: [PLAN.md](PLAN.md). User-visible
changes by date: [CHANGELOG.md](CHANGELOG.md). Reverse-engineering notes: [docs/](docs/).

## How it works

Amazon's `mixer` daemon owns the audio hardware and runs the whole front end (`libasp`: AEC, beamforming, mic
calibration). `hassmic` takes the place of the Alexa client `PuffinApp` as the mixer's client through the reversed C API
of `libmixerAPI.so`, feeds the 16 kHz post-AEC stream to the stock `libpryon.so` wake word engine, and speaks the ESPHome
native API (default) or Wyoming to Home Assistant.

Boot integration (`scripts/system/`): `hassmic.rc` (init) starts `boot.sh` (fixed, on `/system`), which picks the factory
copy or a verified push update and runs `main.sh` (updatable): `main.sh firewall` keeps the egress lock in place and is
the root side of push updates, `main.sh satellite` stops Alexa, updater and telemetry and keeps hassmic running.
No `hassmic.conf` = stock behaviour.

## Repository layout

| Path | What |
|---|---|
| `src/hassmic/` | the daemon: core (capture, wake word, playback, LEDs, buttons), `proto_esphome.c`, `proto_wyoming.c`, `arb.c` (wake word arbitration between Echos), `sendspin.c`, `a2dp.c`, `ble.c`, push updates |
| `src/tools/` | `mixcap`, `mixplay`, `pryon_test`, `latency`, `otatool`, `runas` (AIPC refuses uid 0, the image has no `su`), `curlspy`, `hciscan` (raw HCI on `/dev/stpbt`) |
| `src/include/` | C headers for the reversed `libmixerAPI.so` and `libpryon.so` |
| `src/third_party/` | monocypher 4.0.2, `dr_flac.h`, `minimp3.h`, libfreeaptx 0.2.2 (own licences, see README) |
| `scripts/` | PC side: `deploy.sh`, `probe.sh`, `capture-test.sh`, `wifi-join.sh`, `install-system.sh`, `ota-push.sh` |
| `scripts/device/`, `scripts/system/` | run on the Echo; boot integration (`hassmic.rc`, `boot.sh`, `main.sh`, `sepolicy.rules`) |
| `tools/` | OTA payload dumper, Thumb disassembly helpers, `qrun.sh` (device binaries under qemu-arm), `davs-fetch.py` |
| `tests/` | protocol tests against the reference implementations |

## Build and test

Device binaries need the NDK and the unpacked firmware (README, install steps 1–2). Without them only the host targets
build.

```sh
make                                              # ARM binaries into build/
make host                                         # PC build + qemu build for the tests (needs libopus)
make unit                                         # C unit tests
.venv/bin/python tests/fake_ha_esphome.py         # ESPHome native API, as Home Assistant (aioesphomeapi)
.venv/bin/python tests/fake_ha_arbitration.py     # two Echos + Home Assistant + an unknown device: wake word arbitration
.venv/bin/python tests/fake_ha.py [--qemu]        # Wyoming (wyoming)
.venv/bin/python tests/fake_ma_sendspin.py        # Sendspin, as Music Assistant (aiosendspin)
tests/ota_push_test.sh                            # signed push-update path end to end
```

`tools/qrun.sh [-t secs] <arm-binary> args` runs a device binary on the PC under qemu-arm against `firmware/rootfs`.

Trial runs on the device: `scripts/deploy.sh`, then `adb shell sh /data/local/hassmic/run.sh`.

## Debugging on the device

- Log: `/data/local/hassmic/boot.log`. State (API key, Bluetooth keys, Sendspin, settings): `/data/local/hassmic/state/`.
- What the wake word hears: `kill -TTIN $(pidof hassmic)` starts writing the processed mic stream to
  `/data/local/hassmic/state/capture.raw` (16 kHz mono s16le); the same signal stops it. `mixcap` cannot read that
  stream while hassmic runs: the mixer feeds it to one client only.

## Wake word download without the tool

`GET https://api.amazonalexa.com/v2/deviceArtifacts/?artifactFilter=` + URL-quoted base64 of
`{"artifactType":"wakeword","artifactKey":"echo","filters":{"engineCompatibilityIdList":[…],"locale":["de-DE"],"modelClass":["B"]}}`,
header `Authorization: Bearer <access_token>`. The answer is JSON with a signed CloudFront `downloadUrl` (`.tar.gz`) that
expires within minutes. Found by preloading `src/tools/curlspy.c` into the stock downloader: `scripts/device/davs-spy.sh`.

## Contributing

- Everything assumes firmware 6574.1 for `donut_puffin`; `scripts/probe.sh` checks the device matches.
- Feature commits update [CHANGELOG.md](CHANGELOG.md) (user-facing, dated, plain language), [PLAN.md](PLAN.md) (status,
  measurements) and the README when behaviour visible to users changes.
- Code comments explain *why*, with device facts and measurements.
- Every listening port must stay in TCP 16384–32767: the Echo's firewall admits nothing else inbound.
- Proprietary, derived or secret material stays out of git (git-ignored): `firmware/`, `re/`, `kamakiri/`,
  `boot-root/`, `toolchain/`, `build/`, `device-logs/`, `secrets/` (`wifi.conf`, `update.key`), `*.bin`, `*.zip`.
- Third-party code in `src/third_party/` keeps its own licence.
