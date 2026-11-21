# Echo Spot 1st gen 2017 (rook), Fire OS 5.5.6.9 (304.6.9.0, 690918120): armv7 userland on an aarch64 kernel (3.18.19),
# Android 5.1.1 bionic (API 22), full Android framework.  No Amazon mixer daemon: the front end (libasp) runs inside the
# audio HAL, so audio goes through Android's AudioRecord/AudioTrack via OpenSL ES (audio_android.c, as checkers).
TARGET  := armv7a-linux-androideabi22
# Backends (src/hassmic/audio.h, wake.h) and the stock libraries they link against, from $(STOCK).
AUDIO   := src/hassmic/audio_android.c
WAKE    := src/hassmic/wake_pryon.c
LIBS    := libpryon.so libopus.so libz.so
# No LED ring: the ring as a band along the bottom of the (round, 480x480) screen, drawn by scripts/device/ringbar.dex
RING    := src/hassmic/ring_band.c
# NDK system libraries the backend needs (not stock: no stubs for them)
SYSLIBS := -lOpenSLES
# No Wi-Fi motion module yet: the Wi-Fi is a Broadcom bcmdhd (rook has no MediaTek Wi-Fi), none of ours applies.
# Android 5.1's toolbox has no sed, tr, cut, head, tail, wc, printf, pkill, pidof, timeout, stat, sort or od, which the
# scripts on the Echo use: our toybox (tools/toybox.sh) ships with them, and main.sh puts the applets the system lacks on PATH
TOYBOX  := 1
