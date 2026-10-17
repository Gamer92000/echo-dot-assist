Requirements: PySerial
CURRENT RELEASE 10/13/2026: saiyaku-athena-v1.0.0.zip
## bootloader unlock
- extract the zip, cd into its dir
- ./meta-step.sh (might need sudo).
- The script will print a disclaimer. Read it and make sure you agree with it. Then type "YES".
- Connect the micro-USB cable to your Echo Show, followed by the power supply. During boot, these devices expose a serial port that the script will connect to.
- At this point, let the script do its magic and PATIENTLY WAIT. This might take a few minutes and attempts. It is important that you do not disconnect the device.
- If everything goes well, the device will reboot into TWRP.
- You're done, the device has been unlocked!
- At this point, if you want to enable rooted ADB, flash the boot-root.zip file from the attachments.

## ROOT
- flash boot-root.zip via fastboot? or twrp?