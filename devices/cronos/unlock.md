# Unlock by the XDA thread, root by this project's boot image (cronos)

The unlock is R0rt1z2's amonet for the Echo Show 5 2nd gen, done by hand as its XDA thread says:
[[UNLOCK][ROOT][TWRP][UNBRICK] Amazon Echo Show 5 2nd Gen - 2021 (cronos)](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-5-2nd-gen-2021-cronos.4772596/).
This page is for C76N8S only: the 1st gen (checkers) and the 3rd gen (2023, another SoC) are not this.

## Unlock

Follow the thread from its first post: which Echos it is for, its files, how to get the Echo ready and every command are
there, kept up to date. R0rt1z2 asks (since kamakiri-donut v1.1.0) that the unlock is only ever run that way, never from
an installer or a script, so this page does not repeat its steps and `scripts/setup.sh cronos` does not run them either:
it names the thread, waits for Enter and then checks for TWRP on adb. **Stop once TWRP shows on the Echo's screen**: the
system, root and install are this project's ([README.md](README.md)). Stuck, or the Echo does something the thread does
not say? Ask in the thread, with what the Echo did, in your own words, and the tool's output or log as it is.

Never write lk, preloader, tee1/tee2 or any other bootloader partition, in no mode: there is no bootrom way back on
this model. Part of the exploit lives in the first block of `boot` and `recovery`: write them only from hacked fastboot
or TWRP (as `scripts/install-boot.sh` does), never with `dd` from the running OS.

## Root

The guided setup does not install or read the thread's `boot-root.zip`: it builds its own root boot image from the
firmware's `boot.img` (`scripts/install-boot.sh --boot-only`), with a userdebug `adbd` from Google's Android 7.1.1
emulator image (README.md's table; not tried on a Show yet: until 2026-11-09 it was the CyanogenMod `adbd` out of the
zip's image, the tested one). An Echo that should
only be rooted, without the install: as the thread says.

## Back to Fire OS

Going back to stock and unbricking are the XDA thread's: as it says.
