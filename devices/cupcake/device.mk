# Echo Input 2018 (cupcake), Fire OS 6.5.7.5: armv7, Android 7.1.2 bionic (API 25 on the device, API 24 NDK target as the
# Dots).  donut's platform (MT8516, arm64 kernel 4.4.22, A/B) with checkers' software: the Android framework runs, no
# Amazon mixer daemon, the front end (libasp) inside the audio HAL, so audio goes through AudioRecord/AudioTrack via
# OpenSL ES (audio_android.c; docs/re-cupcake.md).  libopus.so and libz.so are donut's; libpryon.so is a build of its own
# with the same functions (pryon_test under qemu: the same detection as checkers' on its own en-US/ALEXA model).
TARGET  := armv7a-linux-androideabi24
# Backends (src/hassmic/audio.h, wake.h) and the stock libraries they link against, from $(STOCK).
AUDIO   := src/hassmic/audio_android.c
WAKE    := src/hassmic/wake_pryon.c
LIBS    := libpryon.so libopus.so libz.so
# NDK system libraries the backend needs (not stock: no stubs for them)
SYSLIBS := -lOpenSLES
# One RGB LED (LP5562) behind Amazon's ledcontroller, with the Dots' led-resources: ring.c (board.c ring_leds 1)

# Wi-Fi motion: donut's kernel build (4.4.22+ arm64, the same vermagic and struct module in every stock module) and its
# gen4m Wi-Fi driver, a build of its own with donut's layout (nicRxFillRFB called at nicRxProcessDataPacket+0x50, the
# same SW_RFB_T offsets): donut's module (src/kmod/hassmic_rcpi4m.c), built from donut's config fragment.
KMOD    := hassmic_rcpi4m
KVER    := 4.4.22
KARCH   := arm64
KCROSS  := $(CURDIR)/toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-
KSRC_URL    := https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.4.22.tar.xz
KSRC_SHA256 := 629d5661dc3b554fb0214b4635d80bb20f7977c6e814810945de31cfa86d1a0f
KCC_URL     := https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/+archive/961622e926a1b21382dba4dd9fe0e5fb3ee5ab7c.tar.gz
KCC_DIGEST  := 6465e57d85ac645a5ebe5fe0537dc3f868f85ea6bbd8d7e1d0de06b349168067
KFRAG   := devices/donut/kconfig
KCFLAGS := -fno-pic                 # Android's GCC builds PIC by default; modules use -mcmodel=large, which refuses it
