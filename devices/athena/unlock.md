# Unlock and root by the XDA thread (athena)

The unlock is saiyaku-athena, a serial-port exploit for the Echo Show 8 2nd gen (MT8183), done by hand as its XDA thread
says: [[UNLOCK][ROOT][TWRP][UNBRICK] Amazon Echo Show 8 2nd Gen - 2021 (athena)](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-8-2nd-gen-2021-athena.4804958/). It is not this project's work.

## Unlock

Follow the thread from its first post: which Echos it is for, its files, how to get the Echo ready and every command are
there, kept up to date. R0rt1z2 asks (since kamakiri-donut v1.1.0) that the unlock is only ever run that way, never from
an installer or a script, so this page does not repeat its steps and `scripts/setup.sh athena` does not run them either:
it names the thread, waits for Enter and then checks for the Echo on adb. **Stop once TWRP shows on the Echo's screen**:
the root below comes next, then the install ([README.md](README.md)). Stuck, or the Echo does something the thread does
not say? Ask in the thread, with what the Echo did, in your own words, and the tool's output or log as it is.

## Root

By hand too: `boot-root.zip`, installed from TWRP as the thread says, until Fire OS has started with root adb
(`adb shell id -u` says 0). The zip patches system, vendor and adbd in place (root adb, OTA blocked, the SELinux policy
opened). `scripts/setup.sh athena` does not install it either: it waits for Enter and checks. The install's own policy
tool, magiskpolicy, comes from Magisk's release, not from the zip ([README.md](README.md)'s table).
