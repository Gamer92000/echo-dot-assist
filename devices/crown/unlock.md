# Unlock and Root
## UNLOCK
Instructions:
- Download the latest amonet package from the attachments section of this post (CURRENT RELEASE: amonet-crown-v2.0.1.zip)
- Extract the ZIP file and open the resulting folder. If you are using Linux, open a terminal window within the extracted directory.
- Run `./fastbrick.sh` in the terminal.
- Connect the device to the AC charger and hold all three buttons simultaneously until => FASTBOOT mode... appears on the screen.
- Once in Fastboot mode, connect the device to the computer using a MicroUSB cable. Script should automatically detect the device.
- When prompted, type "YES". Follow all on-screen instructions on both the computer and the device while the exploit executes.
- DO NOT INTERRUPT THE PROCESS AFTER THE 10‑SECOND GRACE PERIOD, ANY INTERRUPTION WILL PERMANENTLY BRICK THE DEVICE.
- Wait up to five minutes for the exploit to complete. The device will reboot into TWRP.
- Congratulations! Your bootloader is now unlocked!

## ROOT
- Download the attached boot-root.zip and extract it.
- Reboot the device into HACKED FASTBOOT mode (use TWRP or press the MUTE key while booting).
- Run the following command to enable ADB: fastboot oem flags 61.
- Finally, flash the boot image with fastboot flash boot boot-root.img and reboot with fastboot reboot.
- ADB should now be enabled, and the shell should default to root (#).