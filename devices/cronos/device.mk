# Echo Show 5 2nd gen 2021 (cronos), Fire OS 6574.1: checkers' platform and build (MT8163, armv7, Android 7.1.2, API 24
# NDK target), full Android framework.  No Amazon mixer daemon: the front end (libasp) runs inside the audio HAL, so audio
# goes through Android's AudioRecord/AudioTrack via OpenSL ES (audio_android.c, docs/re-cronos.md).  libpryon.so is
# checkers' (byte-identical); libopus.so and libz.so export what we link in the same order (the stubs are checkers').
TARGET  := armv7a-linux-androideabi24
# Backends (src/hassmic/audio.h, wake.h) and the stock libraries they link against, from $(STOCK).
AUDIO   := src/hassmic/audio_android.c
WAKE    := src/hassmic/wake_pryon.c
LIBS    := libpryon.so libopus.so libz.so
# No LED ring: the ring as a band along the bottom of the screen, drawn by scripts/device/ringbar.dex (ring.h)
RING    := src/hassmic/ring_band.c
# NDK system libraries the backend needs (not stock: no stubs for them)
SYSLIBS := -lOpenSLES

# Wi-Fi motion: the Wi-Fi driver is donut's gen4m wlan_mt76x8_sdio.ko, built for a 32-bit ARM kernel (4.9.77), as on
# checkers; the same module as donut's, its ARM variant (src/kmod/hassmic_rcpi4m.c).  The kernel's own config (IKCONFIG of boot.img).  Amazon
# built the kernel with Linaro GCC 6.3-2017.05, which Linaro no longer publishes: AOSP's arm-eabi-4.8 (the Makefile's
# default, biscuit's) builds the module, same ABI.
KMOD    := hassmic_rcpi4m
KVER    := 4.9.77
KSRC_URL    := https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.9.77.tar.xz
KSRC_SHA256 := 7c29bc3fb96f1e23d98f664e786dddd53a1599f56431b9b7fdfba402a4b3705c
KCONFIG := devices/cronos/kconfig
