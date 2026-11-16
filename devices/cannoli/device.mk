# Cannoli NS6575 / 8349: Android 7.1.2, ARM32 bionic; stock mixer and Pryon.
TARGET := armv7a-linux-androideabi24
AUDIO := src/hassmic/audio_mixer.c
WAKE := src/hassmic/wake_pryon.c
LIBS := libmixerAPI.so libpryon.so libopus.so libz.so
# ledcontroller with libhmled.so: Home Assistant's light drawn into an animation the LED manager holds (ledshim.c)
LEDSHIM := 1
# Wi-Fi motion: MediaTek's wlan_mt76x3_sdio.ko (MT7653, a module of its own, on mac80211 here) has gen4m's RX path as
# athena's driver of that name: nicRxProcessDataPacket calls nicRxFillRFB once (+0xa0), the RFB's group-valid byte at +16
# and RX vector pointer at +32, nicRxGetRcpiValueFromRxv reads word 3 byte 0: src/kmod/hassmic_rcpi4m.c with its 32-bit
# defaults.  Kernel 5.4.86 (32-bit ARM, Linaro GCC 4.9), Android's common kernel: config from boot.img's IKCONFIG with
# the stack protector off (strong needs GCC 4.9, the Makefile's arm-eabi-4.8 has none), the system keyring off (its
# extract-cert does not build against OpenSSL without ENGINE; nothing of it is in a module) and LOCALVERSION for the
# vermagic.  No kallsyms_lookup_name export, no patch_text, read-only module text: HM_DRIVER_SYMS (the driver's own
# symbol table, a vmap'd alias for the write).  Its struct module has init at 0xfc, kernel.org's at 0xd8: KMOD_INIT.
# CONFIG_MODVERSIONS is on, so devices/cannoli/Module.symvers carries the kernel's own CRCs (tools/kcrc.py, out of
# boot.img, checked against the stock modules' __versions).
KMOD    := hassmic_rcpi4m
KCFLAGS := -DHM_DRIVER_SYMS
KMOD_INIT := 0xfc
KVER    := 5.4.86
KSRC_URL    := https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.4.86.tar.xz
KSRC_SHA256 := eb36b5fc6ef7b953acba0a3e62d872e0330c4d34b38d58f5714493a4fe3b0e8b
KCONFIG := devices/cannoli/kconfig
