# Echo models

The daemon, the protocols, the boot logic and the firewall are the same on every Echo. What differs between models is
kept here, one directory per model, named after Amazon's codename (the first part of `ro.product.device`). The build and
every script pick the model's directory by `DEVICE`. The scripts that use adb ask the connected Echo which model it is
and refuse a mismatch.

| Model | Codename | `ro.product.device` | Status |
|---|---|---|---|
| Echo Dot 3rd gen (2018), D9N29T | [`donut`](donut/README.md) | `donut_puffin` | supported, firmware 6574.1 (NS65741) only |
| Echo Dot 2nd gen (2016), RS03QR | [`biscuit`](biscuit/README.md) | `biscuit_puffin` (OTA metadata) | tested on a real Echo Dot 2; guided setup; some on-device values still marked "verify" |
| Echo 2nd gen (2017) | [`radar`](radar/README.md) | `radar_puffin` (OTA metadata) | in preparation: builds, Pryon verified under qemu against its own ALEXA model; unlock files staged; on-device values to check |
| Echo Dot 3rd gen refresh (2019–2020), C78MP8 | `crumpet` | `crumpet`? (OTA metadata; fastboot says `CRUMPET`) | not started: `kamakiri-donut` does not apply |
| Echo Dot 3rd gen with clock, 36EBT3 | `doebrite` | ? | not started; thought to be `crumpet` hardware plus the clock display |
| Echo Show 5 1st gen (2019), H23K37 | [`checkers`](checkers/README.md) | `checkers` | builds; worked out from the firmware (same 6574.1 as donut, full Android, no mixer: OpenSL ES backend); unlock public (amonet-checkers, TWRP); install written (`INSTALL=boot`), untried on a device |

## What a model directory holds

| File | Used by | What |
|---|---|---|
| `device.mk` | `Makefile` | compiler target (NDK API level), audio and wake word backends, stock libraries to link against; optionally the Wi-Fi motion kernel module (`KMOD`) and its kernel (`KVER`, `KARCH`, `KCROSS`, `KCONFIG`: the kernel's own config (IKCONFIG of `boot.img`) in the model's directory, or `KFRAG`: a fragment where the kernel has none, `KCFLAGS`; and what `make kernel-tools` downloads for it: `KSRC_URL`, `KSRC_SHA256`, `KCC_URL`, `KCC_DIGEST`, unless the Makefile's 3.18 defaults fit) |
| `board.c` | `hassmic` (linked in) | `struct board` from `src/hassmic/board.h`: identity towards Home Assistant, the codename (names the model's bundle in a release), input devices, mute latch, Bluetooth device node and address, stock wake word and earcon paths, thermal zone, light sensor, LED volume steps |
| `device.conf` | PC scripts (`scripts/lib/device.sh`) and, shipped next to them, the scripts on the Echo | product and firmware id, firmware file and checksum, files to compare in `probe.sh`, install method, the daemon's user and groups, names of the stock services to stop; `KMOD`/`KMOD_ARGS` for `main.sh` to load the Wi-Fi motion module |
| `stubs/*.syms` | `make STUBS=1` (CI) | per stock library in `LIBS`, the symbols our binaries need of it, written by `tools/mkstubs.sh` from the firmware: stand-ins to link against without it |
| `hassmic.rc` | `install-system.sh` → `/system/etc/init/` | init services, and the trigger that starts the satellite once the audio stack is up |
| `sepolicy.rules` | `install-system.sh` | allow rules added to the stock policy so init can start the scripts |
| `README.md` | people | what is known about the model, and its install instructions (requirements, downloads, unlock, root, install) |
| `setup.sh` | `scripts/setup.sh` | the same install steps for the guided setup: `STEPS` (id and title per line) and one `step_<id>` function each, built from the helpers in `scripts/lib/setup.sh`; `ROOTED_SKIP`: the steps an Echo rooted already has behind it (`rooted_already` in the steps) |

A model without `setup.sh` is listed by the guided setup as not supported yet; a model without `device.mk` does not
build.

What each model needs on the PC is also split by codename. These directories are git-ignored:

- `firmware/<codename>/`: everything downloaded or derived for the model: the stock firmware image, the unlock and
  root tools, the unpacked firmware, disassembly (`re/`). The build links against `firmware/<codename>/rootfs/system/lib`, and
  `tools/qrun.sh` runs binaries under qemu against it.
- `build/<codename>/`: the device binaries, bundles and patched policy.

The PC builds for the protocol tests (`build/hassmic-host`, `build/otatool-host`) stay in `build/`. A new model also
needs `probe.md5` (the checksums of its `PROBE_FILES` in the pinned firmware, see `scripts/probe.sh`). `hassmic-host`
carries the `board.c` identity of `DEVICE`.

```sh
make DEVICE=donut                 # the default
DEVICE=donut tools/qrun.sh build/donut/pryon_test testdata/alexa_espeak.raw
DEVICE=donut scripts/ota-push.sh  # no adb here: the model comes from DEVICE
```

The Echo checks it too. A push update whose `device.conf` names another product than the Echo's `ro.product.device` is
refused. That check is in `main.sh`, so an Echo applies it from its first update that ships this version of `main.sh`.

## Porting to another model

Rough order. Each step is safe to stop at.

1. **Unlock and root.** This is the hard part, and it is different for almost every model (bootrom exploit, TWRP or
   none, A/B slots or not, how the SELinux policy can be patched). Write down what worked in the new model's
   `README.md`. Pin the firmware version before the Echo ever reaches Amazon: an update can close the hole.
2. **Firmware on the PC.** Unpack it into `firmware/<codename>/rootfs` (`tools/payload_dump.py` handles A/B
   `payload.bin`). `tools/` has the disassembly helpers; `docs/re-platform.md` and `docs/re-pryon.md` show what to
   look for, as found on `donut`.
3. **Start the directory from `donut`**: `cp -r devices/donut devices/<codename>`. Then check every value against the
   new firmware and the running Echo, one at a time:
   - `device.conf`: `PRODUCT` and `FIRMWARE_ID` from `getprop ro.product.device` and `ro.build.display.id`. Stock
     service names from the init rc files (`/init.*.rc`, `/system/etc/init/`). The user and groups of the stock Alexa
     client (AIPC refuses uid 0).
   - `board.c`: `getevent -il` shows the keypad and the mute latch. Also look for the latch's state file (`find /sys -name "*privacy*"`:
     `/sys/devices/platform/gpio-privacy/state` on donut, `…keypad/amz_privacy/state` on radar; counting presses of a mute
     key is a last resort, it goes wrong when the latch moves while hassmic is not running),
     `/sys/class/thermal/thermal_zone*/type` and the Bluetooth device node and its owner. Check the wake word manifest
     and the earcons in the firmware. `light_sensor`: the lux files in `strings` of
     `/system/lib/libacehal_ambientLightSensor.so`, in its order, and `cat` the one that exists on the Echo.
   - `device.mk`: API level from `ro.build.version.sdk`. If the model has Amazon's `mixer` (`libmixerAPI.so`) and
     `libpryon.so`, the existing backends should work once `probe.sh`, `mixcap` and `pryon_test` agree. If not, write a
     new `audio_*.c` or `wake_*.c` behind `audio.h` / `wake.h` and name it here.
   - `hassmic.rc`: the property trigger that starts the stock Alexa client on this model.
   - `sepolicy.rules`: depends on how root was obtained.
   - `INSTALL` in `device.conf`: `twrp-ab` is `scripts/install-system.sh`; `boot` (policy in `boot.img`'s ramdisk, no
     A/B: checkers) is `scripts/install-boot.sh`, which install-system.sh hands over to. A model that installs another
     way gets its own method there, or its own script.
4. **Try it without installing**: `scripts/probe.sh`, `scripts/deploy.sh`, `adb shell sh /data/local/hassmic/run.sh`.
   Both pick the model from the Echo on adb.
5. **Install**, and write the steps down twice: by hand in the model's `README.md` (Install section), and as
   `setup.sh` for the guided setup. Keep them in the same order; `devices/donut/` shows how. Commands go through
   `task` (spinner, output into the log), waits through `waitfor`/`wait_adb`; what only the user can do (soldering,
   button presses) is a short `todo` or `tell`, never a paragraph. A step first checks whether its result is there
   already. Mark a model not tried on a real Echo with `UNTESTED=1`. End with
   `install` (`install_satellite`); models come later from the settings page. Try it with `scripts/setup.sh <codename> --dry-run`.
6. `tools/mkstubs.sh <codename>` for the stub lists, and the codename in the build matrix of
   `.github/workflows/build.yml`: CI builds and publishes the model's bundle from then on.
7. Add the model to the table above, to the root `README.md` and to `CHANGELOG.md`.

Shared code stays free of `#ifdef DEVICE_...`. When a model needs more than a different value, add a field to
`struct board`, a variable to `device.conf`, or a backend, and give `donut` its current behaviour there.
