# Unlock and root by hand (saiyaku-athena)

The unlock is saiyaku-athena, a serial-port exploit for the Echo Show 8 2nd gen (MT8183), from its XDA thread:
[[UNLOCK][ROOT][TWRP][UNBRICK] Amazon Echo Show 8 2nd Gen - 2021 (athena)](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-8-2nd-gen-2021-athena.4804958/). It is not this project's work. The thread is the
authority; this is what its steps come to, as the guided setup (`scripts/setup.sh athena`) and [README.md](README.md)
use them. Files and their sha256 are in the README's table.

## Unlock

1. Download `saiyaku-athena-v1.0.0.zip` from the thread's attachments and unzip it. It needs Python with PySerial.
2. Keep the Echo's micro-USB cable plugged into the PC and its power supply out. Run `./meta-step.sh` from the unzipped
   folder and type `YES` when it asks.
3. When it says it is waiting for the device, plug the power supply in and let the Echo boot: while it boots, it opens a
   serial port the script connects to.
4. It runs by itself from there, for minutes and several attempts. **Disconnect nothing.** It ends with the Echo
   rebooting into TWRP.

"Permission denied" on the serial port: add yourself to the group that owns it (uucp or dialout), or run it with sudo.

## Root

In TWRP: `adb push boot-root.zip /tmp/`, `adb shell twrp install /tmp/boot-root.zip`, then `adb reboot`. The zip patches
system, vendor and adbd in place (root adb, OTA blocked, the SELinux policy opened); `adb shell id -u` then says 0.
Unzip `boot-root.zip` into `firmware/athena/boot-root/` too: the install takes `patch/magiskpolicy32` from it.
