# Echo Show 8 2nd gen 2021 (athena), Fire OS 7574.1 (PS75741): armv7 (the 32-bit userland of an MT8183 on a 4.4.146
# kernel), Android 9 bionic (API 28 on the device, API 24 NDK target as the other models), full Android framework with
# Treble: /vendor is its own partition.  No Amazon mixer daemon: the front end (libasp, in /vendor/lib) runs inside the
# audio HAL, so audio goes through Android's AudioRecord/AudioTrack via OpenSL ES (audio_android.c, docs/re-athena.md,
# as on checkers and crown).  libpryon.so, libopus.so and libz.so are the firmware's own, none of them crown's.
TARGET  := armv7a-linux-androideabi24
# Backends (src/hassmic/audio.h, wake.h) and the stock libraries they link against, from $(STOCK).
AUDIO   := src/hassmic/audio_android.c
WAKE    := src/hassmic/wake_pryon.c
LIBS    := libpryon.so libopus.so libz.so
# No LED ring: the ring as a band along the bottom of the screen, drawn by scripts/device/ringbar.dex (ring.h)
RING    := src/hassmic/ring_band.c
# NDK system libraries the backend needs (not stock: no stubs for them)
SYSLIBS := -lOpenSLES

# Wi-Fi motion: the Wi-Fi driver is MediaTek's wlan_mt76x3_sdio.ko, a module of its own, but built like the gen4m
# driver of donut and checkers (RX vector, nicRxFillRFB, nicRxGetRcpiValueFromRxv): src/kmod/hassmic_rcpi4m.c, ARM
# variant, with the defaults of checkers' offsets.  The kernel is 4.4.146 (MT8183, 32-bit), config from its /proc/config.gz
# with the stack protector off (strong needs GCC 4.9, the Makefile's arm-eabi-4.8 has none) and LOCALVERSION "+" for the
# vermagic; CONFIG_MODVERSIONS is on, so devices/athena/Module.symvers carries the kernel's own CRCs.  AOSP's arm-eabi-4.8
# (the Makefile's default) builds it, same ABI as Amazon's GCC 4.9.
KMOD    := hassmic_rcpi4m
KVER    := 4.4.146
KSRC_URL    := https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.4.146.tar.xz
KSRC_SHA256 := 1f8810cc954fabca768c4726c72e49bbb8dbc038901089640755c9b5bde17df5
KCONFIG := devices/athena/kconfig
