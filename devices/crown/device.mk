# Echo Show 8 1st gen 2019 (crown), Fire OS 6.5.7.0 (NS6570): armv7, Android 7.1.2 bionic (API 25 on the device, API 24 NDK
# target as the Dots), full Android framework.  No Amazon mixer daemon: the front end (libasp) runs inside the audio HAL,
# so audio goes through Android's AudioRecord/AudioTrack via OpenSL ES (audio_android.c, docs/re-crown.md, as on checkers).
# libopus.so and libz.so are byte-identical to donut's; libpryon.so is newer (2.41).
TARGET  := armv7a-linux-androideabi24
# Backends (src/hassmic/audio.h, wake.h) and the stock libraries they link against, from $(STOCK).
AUDIO   := src/hassmic/audio_android.c
WAKE    := src/hassmic/wake_pryon.c
LIBS    := libpryon.so libopus.so libz.so
# No LED ring: the ring as a band along the bottom of the screen, drawn by scripts/device/ringbar.dex (ring.h)
RING    := src/hassmic/ring_band.c
# NDK system libraries the backend needs (not stock: no stubs for them)
SYSLIBS := -lOpenSLES

# Wi-Fi motion: the Wi-Fi driver is donut's gen4m wlan_mt76x8_sdio.ko, built for a 32-bit ARM kernel (4.9.77); the same
# module as donut's, its ARM variant (src/kmod/hassmic_rcpi4m.c).  The kernel's own config (IKCONFIG of boot.img).  Amazon
# built the kernel with Linaro GCC 6.3-2017.05, which Linaro no longer publishes: AOSP's arm-eabi-4.8 (the Makefile's
# default, biscuit's) builds the module, same ABI.
KMOD    := hassmic_rcpi4m
KVER    := 4.9.77
KSRC_URL    := https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.9.77.tar.xz
KSRC_SHA256 := 7c29bc3fb96f1e23d98f664e786dddd53a1599f56431b9b7fdfba402a4b3705c
KCONFIG := devices/crown/kconfig
