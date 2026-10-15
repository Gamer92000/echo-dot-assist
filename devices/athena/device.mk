# Echo Show 8 2nd gen 2021 (athena), Fire OS 7.5.7.4.1 (PS75741): armv7 (the 32-bit userland of an MT8183 on a 4.4.146
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

# No Wi-Fi motion yet: the driver is MediaTek's gen2 wlan_mt76x3_sdio.ko on a 32-bit 4.4.146 kernel, and neither
# src/kmod/hassmic_rcpi.c nor hassmic_rcpi4m.c has been tried on it (docs/re-athena.md).  KMOD is left out until then.
