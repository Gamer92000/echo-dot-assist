# Unlock by the XDA thread, root by this project's boot image (crown)

The unlock is R0rt1z2's amonet for the Echo Show 8 1st gen, done by hand as its XDA thread says:
[[UNLOCK][ROOT][TWRP][UNBRICK] Amazon Echo Show 8 1st Gen - 2019 (crown)](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-8-1st-gen-2019-crown.4766687/).
This page is for the 1st gen Show 8 only, not the 2nd gen ([athena](../athena/README.md)).

## Unlock

Follow the thread from its first post: which Echos it is for, its files, how to get the Echo ready and every command are
there, kept up to date. R0rt1z2 asks (since kamakiri-donut v1.1.0) that the unlock is only ever run that way, never from
an installer or a script, so this page does not repeat its steps and `scripts/setup.sh crown` does not run them either:
it names the thread, waits for Enter and then checks for TWRP on adb. **Stop once TWRP shows on the Echo's screen**: the
system, root and install are this project's ([README.md](README.md)). Stuck, or the Echo does something the thread does
not say? Ask in the thread, with what the Echo did, in your own words, and the tool's output or log as it is.

Never write lk, preloader, tee1/tee2 or any other bootloader partition.

## Root

The thread's `boot-root.img` is NS6571's; the firmware pinned here is NS6570, so the guided setup builds its own root
boot image from the firmware's `boot.img` instead (`scripts/install-boot.sh --boot-only`), with a userdebug `adbd` from
Google's Android 7.1.1 emulator image (README.md's table; not tried on a Show yet: until 2026-11-09 it was the
CyanogenMod `adbd` out of the thread's `boot-root.img`, the tested one). Nothing of the thread's files is used. An Echo
that should only be rooted, without the install: as the thread says.
