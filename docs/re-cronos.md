# Echo Show 5 2nd gen (`cronos`): what differs from checkers

Worked out from the firmware (2026-10-26), then installed on one cronos the same day ("First install" below). Everything
not listed here is as on the
Echo Show 5 1st gen: read [re-checkers.md](re-checkers.md) first. Firmware:
`update-kindle-cronos-NS65741_user_8149_0013222532484.bin`, Fire OS 6574.1 (NS65741/8149), 1388134745 bytes, md5
`c204364d8a65c6da5d60024c6334fe1e` (as ftvdb.com lists it), sha256 `fa6203b5…f1b09`; `ro.product.device=cronos`,
`ro.product.model=AEOCN`, model number C76N8S.

## checkers or crown?

The model numbers (C76N8S, crown's C7H6N3) look alike, but the software is checkers': cronos runs the **same release
and build** as the pinned checkers firmware (NS65741/8149, `ro.build.version.incremental` 0013222532484 on both), crown
an older one (NS6570).

| | against checkers | against crown |
|---|---|---|
| Kernel config (IKCONFIG, both 4.9.77 ARM) | 20 lines differ | 74 lines differ |
| `libpryon.so` | **byte-identical** | crown's is 2.41, newer |
| `libaspclient.so` | identical | identical |
| `libasp.so`, audio HAL, `libaudioflinger.so`, `libwakewordserver_jni.so` | differ | differ |
| `libopus.so`, `libz.so` | what we link, in the same order (stubs identical) | |
| Screen | 1024 x 600 panel, density 160, same LCM family (`st7701s_wsvga`) | crown: 1280 x 800 |
| `STOCK_APPS_OUT` | all present but `HumanDetectionService`, `SmartHomeWifiAdapterService` | five absent |
| Wi-Fi driver | `nicRxFillRFB` at `nicRxProcessDataPacket` +0x64 | the same |

So `devices/cronos/` is checkers' with the differences below. The 20 kernel lines: project and thermal names, the
camera sensor (`ov02b10`), the panel (`st7701s_wsvga_dsi_vdo_cronos`), `CONFIG_CONNECTOR`, a second front end firmware
(`i2s_to_spi_4ch_v208.bin`), the mute driver and the amplifiers.

## Unlock and root

amonet-cronos v2.0.1 (XDA, R0rt1z2: [the thread](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-5-2nd-gen-2021-cronos.4772596/);
`devices/cronos/unlock.md`). The same steps as checkers' (all three buttons into fastboot, `fastbrick.sh`, TWRP);
since v2.0.0 Volume down = hacked fastboot, Volume up = TWRP, Mute + Volume down with USB = the preloader's USB download
("MT8163 Preloader", maker "PWNED"). The firmware's `updater-script` writes the same partitions as checkers' (system,
boot, lk, tee1/tee2, preloader, MBR), at the same `mtk-msdc.0/11230000.MSDC0/by-name/` paths; `system` is 3253731328
bytes as checkers'. The thread's root is a `boot-root.zip` installed in TWRP (not a `boot-root.img` for fastboot as on
checkers): the setup takes the first `.img` inside it for its adbd and checks that adbd against checkers' (`ADBD_SHA256`).
The first Echo tried (2026-10-26) answers `lk_build_desc 44072a3-20240709_162755`, preloader `79a3b50-20231019`,
`unlock_status false`, before and after a stock update: the lk and preloader of 8149 (and amonet v2.0.1's `bin/lk.bin`),
unchanged since July 2024, so they do not tell the Fire OS version. The setup stops only for an older lk (an older
Fire OS: amonet-cronos' first release did not support 6.5.6.x) and asks for a stock update first. amonet's x86_64 `fastboot` read
it under qemu on a Raspberry Pi 4 as the system's did.

Both zips are pinned (2026-10-26): amonet's `lk.bin` and `tz.img` are byte-identical to 8149's, and the `adbd` in the
boot-root zip's `boot.img` (NS6571/6208's, as crown's thread has a newer build's) is checkers' file. `fastbrick.sh`
takes its bundled `bin/fastboot` (x86_64, glibc) on any PC whose `uname -m` has "64", an aarch64 one included.

The firmware's boot image goes through `scripts/mkbootroot.py` as checkers' does: `--check` rebuilds it exact but for the
header ID; `init.cronos.rc` has the same two lines (`setprop service.adb.tcp.port 5555`, `start bootsound_anim`), the
fstab the same `wait,verify` on `/system`. `sepolicy.rules` is checkers': the stock policy has `su` permissive as
checkers' does (`magiskpolicy --print-rules`), and patched it has what `install-boot.sh` checks for (`permissive adbd`,
`allow init su process`); the boot image builds from it.

## Mute button: gpio-privacy, not amazon-gating

No `CONFIG_GATING`: cronos has `CONFIG_KEYBOARD_GPIO_PRIVACY`, the driver athena has. `/init.project.rc` chowns
`/sys/devices/platform/gpio-privacy/state` and `enable` to system; the kernel names its input devices
`gpio-privacy-state` (SW_MUTE_DEVICE) and `gpio-privacy-button` (KEY_POWER); the device trees set
`enable-gpio-toggle-duration` to 1000 ms, as athena's. `board.c` takes athena's values (`privacy_button`,
`privacy_settle_ms` 2000).

## Speaker amplifier: three, by board revision

The kernel appends a device tree per board revision (`cronos_proto` … `cronos_pvt_4_3`); by their order:

| Revisions | Amplifier | Control, stock (HAL's initial setting) |
|---|---|---|
| proto, HVT, EVT, DVT, PVT | Realtek RT5616E line out | `OUT Playback Volume` 29 (`audio_device.xml`) |
| DVT 3.2, PVT 4.2 | Realtek RT5616 line out | `OUT Playback Volume` 29 (`audio_device_rt5616.xml`) |
| DVT 3.1, DVT 3.3, PVT 4.1 | TI TAS5805M | `Master TAS5805m Playback Volume A` 110 (`audio_device_tas5805m.xml`) |
| PVT 4.3 | Maxim MAX98396 | `Digital Volume A` 127, `Speaker Volume A` 8 (`audio_device_max98396.xml`) |

The names with " A" are this kernel's own spelling (its strings). `board.speaker_gain_alt` (new, `board.h`) lists the
other amplifiers' controls; `spkgain.c` takes the first that is on card 0. RT5616/RT5616E: checkers' control and steps.
TAS5805M: athena's assumption of 0.5 dB a step, capped at 118 (+4 dB) until someone listens. MAX98396: `Speaker Volume
A`, 0..17 = +4..+21 dB in 1 dB steps (the kernel's TLV table), stock 8 = +12 dB; capped at 14 (+6 dB) until someone
listens. The first cronos tried has the MAX98396 (I2C `2-003d`; tinymix: `Speaker Volume A` 8 of 0..17, `Digital
Volume A` 127 of 0..127).

## The rest

- Light sensor: the device trees name a JSA1214 (checkers: STK3x1x) on MediaTek's alsps framework, the kernel has
  `CONFIG_ALS_FORMULA_CHANGE`. checkers' `als_ps/lux` is there (250 lux in a lit room on the first cronos).
- Sounds: every one of checkers' entries is stored in this firmware's APKs, SpeechInteractionManager's `res/7E.mp3`
  (= `ful_ui_wakesound_hybrid`) included; `sounds_test` decodes all twelve (`make unit`).
- Bluetooth `/dev/stpbt` (bluetooth:net_bt_stack), `btmtksdio.ko`, `/proc/idme/bt_mac_addr`, thermal `mtktscpu`, the
  attestation service `fireos-dha` (`dha.rc`): as checkers'. Which DHA version it answers (1 as checkers, 2 as crown) is
  to be seen.
- The stock services `device.conf` stops (`ahe`, `dacd`, `trackerd`, `whad_cc`, …) are all in this firmware.
- Apps on cronos that checkers' list does not take off (as crown's extras: left as they are until one runs or crashes):
  `FireOSDownloadProvider(Ui)`, `TabletCamera2`, `com.amazon.kindle.cms-service`, `com.amazon.knight.hds`,
  `KnightCaptivePortalLogin`, `sync-provider_ipc-tablet-release`, `sync-service-fireos-tablet-release`.

## First install (2026-10-26)

One cronos, guided setup (`scripts/setup.sh cronos`) from a Raspberry Pi 4 with Ubuntu 26.04 over ssh. amonet v2.0.1
unlocked it (its x86_64 `fastboot` under qemu-user with amd64 glibc in `QEMU_LD_PREFIX`), TWRP, 8149's system written,
the root boot image flashed in hacked fastboot, the build (the NDK under qemu-user too, ~15 min), lockdown, Wi-Fi,
install; hassmic up after the reboot, address by DHCP, adopted by Home Assistant.

- `getevent -il`: `gpio-keys` (event6), `gpio-privacy-state` (event0), `gpio-privacy-button` (event1), and Amazon's
  `amazon_touch`, `goodix-ts`, `m_alsps_input`, `hwmdata`, `mtk-kpd`; mute pressed and released as the log shows.
- Amplifier: I2C `max98396` at `2-003d` (beside `tlv320aic3101`, the ADC); tinymix `Speaker Volume A` 8 (0..17),
  `Digital Volume A` 127 (0..127). Speaker gain found it after a push update: `"Speaker Volume A" 0..14, stock 8`.
- `als_ps/lux` 250 in a lit room; the band 960 x 10; the wake word engine loaded (OpenSL ES capture, VOICE_RECOGNITION).
- A push update (`scripts/ota-push.sh`) passed its self test and became the factory copy.

What the setup ran into: the Wi-Fi network the stock update had saved could not be changed by root (`wifi-join.sh`
falls back to the system user since). Not this model's, and left to their own PRs (issue #45): Ubuntu 26.04 refuses
users `unshare --user`, which `tools/qrun.sh` needs (the PR ran qemu without it while the PIDs were low; until that
is in, `sudo sysctl kernel.apparmor_restrict_unprivileged_userns=0` before the root step); qemu-arm for the policy tool on an aarch64 PC (the setup downloads
an x86_64 one: `qemu-user` from the package manager instead); the udev rules over ssh, where `uaccess` gives nobody
the device (`GROUP="plugdev"`). The Pi itself dropped its USB SSD once during the setup (a Pi 4 with an
Argon M.2 case; not seen again), so run the setup in `screen`.

Still open: speaker gain by ear, Wi-Fi motion, the DHA version ("Download from Amazon"), Bluetooth.
