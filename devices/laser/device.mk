# Echo 4th gen 2020 (laser), Fire OS NS6575 / 8349: Android 7.1.2, ARM32 bionic; stock mixer and Pryon.
TARGET := armv7a-linux-androideabi24
AUDIO := src/hassmic/audio_mixer.c
WAKE := src/hassmic/wake_pryon.c
LIBS := libmixerAPI.so libpryon.so libopus.so libz.so
# The ring is ledcontroller's, with text animations like the Dots' (/system/etc/led-resources/*.animation, 144 of
# them): ring.c makes ours in that format, so none of cannoli's binary ones and no libhmled.so shim.
# Wi-Fi motion: cannoli's wlan_mt76x3_sdio.ko (MT7663) on laser's own kernel, read out of the firmware's (2026-11-27,
# not run yet): nicRxProcessDataPacket calls nicRxFillRFB once (+0x6c), which keeps ucGroupVLD (DW0 bits 25-28) at +16
# and group 3 (the 24-byte RX vector) at +32, and nicRxGetRcpiValueFromRxv reads word 3 from there: src/kmod/
# hassmic_rcpi4m.c with its 32-bit defaults.  Kernel 4.9.77+ (32-bit ARM), Amazon's MediaTek tree as checkers': its
# config with MODVERSIONS on and LOCALVERSION "+" (devices/laser/kconfig: laser's own is in no readable image), AOSP's
# arm-eabi-4.8.  The kernel's CRCs are known only for what the stock modules import (devices/laser/Module.symvers,
# tools/kcrc.py --modules): HM_KALLSYMS imports nothing else and finds the rest by kallsyms_lookup_name.
KMOD    := hassmic_rcpi4m
KCFLAGS := -DHM_KALLSYMS
KVER    := 4.9.77
KSRC_URL    := https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.9.77.tar.xz
KSRC_SHA256 := 7c29bc3fb96f1e23d98f664e786dddd53a1599f56431b9b7fdfba402a4b3705c
KCONFIG := devices/laser/kconfig
# Thread and Zigbee on the GP712 (docs/re-thread.md): rcpbridge and our own otbr-agent (scripts/otbr-build.sh)
THREAD  := 1
# the GP712's chip clock beside the host's, for timed transmit (src/kmod/hassmic_gptime.c; root loads it only where
# asked: main.sh); the same kernel tree and compiler as Wi-Fi motion's
KMOD_RADIO := hassmic_gptime
