# Unlock and root by hand (amonet-cronos)

The unlock is R0rt1z2's amonet for the Echo Show 5 2nd gen, from its XDA thread:
[[UNLOCK][ROOT][TWRP][UNBRICK] Amazon Echo Show 5 2nd Gen - 2021 (cronos)](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-5-2nd-gen-2021-cronos.4772596/).
The thread is the authority; this is what the steps come to, as the guided setup (`scripts/setup.sh cronos`) and
[README.md](README.md) use them. It is for C76N8S only: the 1st gen (checkers) and the 3rd gen (2023, another SoC) are
not this.

## Unlock

**Update the Echo first.** amonet-cronos' first release supported only Fire OS 6.5.5.0 and 6.5.7.0 or later, not the
builds between (6.5.6.x); the v2.0.1 post no longer says, and its author answered that any newer version is fine:
start it normally, connect it to Wi-Fi and install every update until there is none. The bootloader does not show the
version: 8149 and amonet v2.0.1 carry `lk_build_desc` `44072a3-20240709_162755` (preloader `20231019`), the one Amazon
has shipped since July 2024. The guided setup reads it (it only reads) and stops for an older one.

1. Download `amonet-cronos-v2.0.1.zip` from the thread's attachments and unzip it (Linux: `./fastbrick.sh` in that
   folder).
2. With the Echo on its power supply, hold all three buttons until "=> FASTBOOT mode..." shows on the screen.
3. Connect micro-USB; the script finds the Echo. Type `YES` when asked and follow the screen.
4. **After its 10 s grace period nothing may interrupt it** (up to 5 min): an interruption bricks the Echo for good.
   It ends in TWRP.

Afterwards, with the key held while connecting power: Volume down = hacked fastboot, Volume up = TWRP, Mute + Volume
down (USB connected) = the preloader's USB download for MTKClient. `boot-fastboot.sh` / `boot-recovery.sh` from the zip
do the same over USB. Never write lk, preloader, tee1/tee2 or any other bootloader partition, in no mode: there is no
bootrom way back on this model. Part of the exploit lives in the first block of `boot` and `recovery`: write them only
from hacked fastboot or TWRP, never with `dd` from the running OS.

## Root

The thread's `boot-root.zip` is installed in TWRP and leaves a root adb shell. The guided setup does not install it:
it builds its own root boot image from the firmware's `boot.img` (`scripts/install-boot.sh --boot-only`) and takes only
`sbin/adbd` out of the zip's image.

## Back to Fire OS

amonet v2.0.0 and later: in TWRP, Wipe → Factory Reset, then install the stock `.bin` renamed to `.zip`; the first boot
takes a while.
