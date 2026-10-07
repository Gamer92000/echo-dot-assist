# Echo Show 5 1st gen 2019 (checkers), Fire OS 6574.1: armv7, Android 7.1.2 bionic (API 25 on the device, API 24 NDK
# target as the Dots), full Android framework.  No Amazon mixer daemon: the front end (libasp) runs inside the audio HAL,
# so audio goes through Android's AudioRecord/AudioTrack via OpenSL ES (audio_android.c, docs/re-checkers.md).
# libpryon.so, libopus.so and libz.so are byte-identical to donut's.
TARGET  := armv7a-linux-androideabi24
# Backends (src/hassmic/audio.h, wake.h) and the stock libraries they link against, from $(STOCK).
AUDIO   := src/hassmic/audio_android.c
WAKE    := src/hassmic/wake_pryon.c
LIBS    := libpryon.so libopus.so libz.so
# NDK system libraries the backend needs (not stock: no stubs for them)
SYSLIBS := -lOpenSLES

# Wi-Fi motion: the Wi-Fi driver is donut's gen4m wlan_mt76x8_sdio.ko, but built for a 32-bit ARM kernel (4.9.77, IKCONFIG
# in devices/checkers/kconfig); hassmic_rcpi4m rewrites an arm64 bl, so it needs an ARM variant first.  Until then no
# module: wifimotion.c falls back to the driver's RX_STAT.
KMOD    :=
