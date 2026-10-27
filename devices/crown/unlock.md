# Unlock and root by hand (amonet-crown)

The unlock is R0rt1z2's amonet for the Echo Show 8 1st gen, from its XDA thread:
[[UNLOCK][ROOT][TWRP][UNBRICK] Amazon Echo Show 8 1st Gen - 2019 (crown)](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-8-1st-gen-2019-crown.4766687/).
The thread is the authority; this is what the steps come to, as the guided setup (`scripts/setup.sh crown`) and
[README.md](README.md) use them. Files and their sha256 are in the README's table. It does not work on the 2nd gen Show 8.

## Unlock

1. Download `amonet-crown-v2.0.1.zip` from the thread's attachments and unzip it.
2. With the Echo on its power supply, hold all three buttons until "=> FASTBOOT mode..." shows on the screen.
3. Connect micro-USB and run `./fastbrick.sh` from the unzipped folder; type `YES` when asked and follow the screen.
4. **After its 10 s grace period nothing may interrupt it** (up to 5 min): an interruption bricks the Echo for good.
   It ends in TWRP.

Never write lk, preloader, tee1/tee2 or any other bootloader partition.

## Root

The thread's `boot-root.img` is NS6571's; the firmware pinned here is NS6570, so the guided setup builds its own root
boot image from the firmware's `boot.img` instead and takes only `sbin/adbd` out of `boot-root.zip`. By hand, for an
Echo that should only be rooted:

1. Boot into hacked fastboot (from TWRP, or hold Mute while it boots).
2. `fastboot oem flags 61` (adb on), `fastboot flash boot boot-root.img`.
3. `fastboot continue`, not `fastboot reboot` (docs/re-crown.md says why). adb is then a root shell.
