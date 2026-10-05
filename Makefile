# Cross-build for one Echo model, linking against that model's stock libraries.
#   make [DEVICE=donut]      device binaries into build/$(DEVICE)/, against firmware/$(DEVICE)/rootfs
# Everything model-specific comes from devices/$(DEVICE)/ (device.mk here, board.c in the daemon); see devices/README.md.
DEVICE  ?= donut
ifeq ($(wildcard devices/$(DEVICE)/device.mk),)
$(error unknown DEVICE "$(DEVICE)": no devices/$(DEVICE)/device.mk)
endif
include devices/$(DEVICE)/device.mk

NDK     ?= $(CURDIR)/toolchain/android-ndk-r21e
CC      := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/$(TARGET)-clang
STOCK   := $(CURDIR)/firmware/$(DEVICE)/rootfs/system/lib
# STUBS=1: link against stand-ins for the stock libraries, built from devices/$(DEVICE)/stubs/*.syms (tools/mkstubs.sh):
# same soname, the same function names, empty.  The binaries come out byte-identical; CI builds this way, without the
# firmware.
ifeq ($(STUBS),1)
STOCK   := $(CURDIR)/build/stubs/$(DEVICE)
endif
OUT     := build/$(DEVICE)
BOARD   := devices/$(DEVICE)/board.c
# the commit id (the tags are versions: git describe would put the newest in front)
BUILD   := $(shell git describe --always --dirty --exclude='*' 2>/dev/null || echo nogit)
# "compiled" in ESPHome's device info: the commit's time rather than the clock's, so that a build can be repeated byte
# for byte (CI's release against a local one, a stub link against the firmware's: tools/mkstubs.sh)
BUILD_TIME := $(shell LC_ALL=C git log -1 --format=%cd --date=format:'%b %e %Y %H:%M:%S' 2>/dev/null || echo unknown)
# The version is the commit's time in UTC, 2026.10.02.091530: it only goes up, beta and release share it, and Home
# Assistant compares it as numbers.  A build CI publishes (RELEASE=1) reports just that; any other reports
# VERSION+BUILD (git describe), which online updates take for that version.
VERSION := $(shell TZ=UTC0 git log -1 --format=%cd --date=format-local:%Y.%m.%d.%H%M%S 2>/dev/null || echo 0)
DEFS    := -DVERSION='"$(VERSION)"' -DBUILD='"$(BUILD)"' -DBUILD_TIME='"$(BUILD_TIME)"' $(if $(RELEASE),-DRELEASE)
CFLAGS  := -O2 -Wall -Wextra -fPIE -Isrc/include $(DEFS)
LDFLAGS := -pie -fuse-ld=lld -Wl,--allow-shlib-undefined -Wl,--unresolved-symbols=ignore-in-shared-libs
STOCK_LIBS = $(addprefix $(STOCK)/,$(filter $(LIBS),$(1)))

# The build id is compiled in; make must notice when it changes (a new commit), not only when sources change.
# Same for the model the PC builds stand in for (they sit in build/, not build/$(DEVICE)/).
build/.build-id: FORCE
	@mkdir -p build; echo '$(DEFS)' | cmp -s - $@ || echo '$(DEFS)' > $@
build/.device: FORCE
	@mkdir -p build; echo '$(DEVICE)' | cmp -s - $@ || echo '$(DEVICE)' > $@
FORCE:

# A model without the Amazon mixer or Pryon has no use for the tools built on them: they drop out with the library.
BIN := $(OUT)/hassmic $(OUT)/runas $(OUT)/otatool \
       $(if $(filter libmixerAPI.so,$(LIBS)),$(OUT)/mixcap $(OUT)/mixplay $(OUT)/latency) \
       $(if $(filter libpryon.so,$(LIBS)),$(OUT)/pryon_test $(OUT)/aed_test $(OUT)/whisper_test)

.DEFAULT_GOAL := all
all: $(BIN)

# Kernel module for Wi-Fi motion: the Wi-Fi driver's frame levels (device.mk: KMOD, e.g. hassmic_rcpi; src/kmod/).
# Built against kernel.org's sources of the Echo's kernel version with its own config, by the compiler Amazon built that
# kernel with: arm-eabi-4.8 for the 3.18 kernels of biscuit and radar, whose config is their own (IKCONFIG, read out of
# the firmware's boot.img into devices/<codename>/kconfig: device.mk KCONFIG); aarch64-linux-android-4.9 for donut's
# 4.4, which carries none, so device.mk names a config fragment (KFRAG) that goes on top of the architecture's defconfig.  All in toolchain/ (DEVELOPMENT.md); without
# them `all` leaves the module out and says so.  The tree is prepared out of the source directory, per model.  3.18
# refuses GCC 4.8.0-4.8.2 (asm-offsets.c); the Echos' kernels were built with exactly that compiler, so that check goes,
# as in their own tree.
KVER    ?= 3.18.19
KARCH   ?= arm
KSRC    ?= $(CURDIR)/toolchain/linux-$(KVER)
KCROSS  ?= $(CURDIR)/toolchain/arm-eabi-4.8/bin/arm-eabi-
# what `make kernel-tools` downloads (scripts/kernel-tools.sh; the guided setup and CI): the sources by their checksum,
# the compiler by the digest of its files (googlesource makes its archive anew each time)
KSRC_URL    ?= https://cdn.kernel.org/pub/linux/kernel/v3.x/linux-3.18.19.tar.xz
KSRC_SHA256 ?= 3d80d3b8d98c3141d9e26f6c25d73575d688f1c1651b8076f0f2bfd76325b7c9
KCC_URL     ?= https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/arm/arm-eabi-4.8/+archive/26e93f6af47f7bd3a9beb5c102a5f45e19bfa38a.tar.gz
KCC_DIGEST  ?= fcd6082697317cfa44c0b876e5ca8285a07a13c763c59c7a962e44273e0668a0
KMAKE    = $(MAKE) -s ARCH=$(KARCH) CROSS_COMPILE=$(KCROSS) HOSTCFLAGS="-fcommon -std=gnu89 -w" KCFLAGS="$(KCFLAGS)"
ifneq ($(KMOD),)
ifneq ($(and $(wildcard $(KSRC)/Makefile),$(wildcard $(KCROSS)gcc)),)
all: $(OUT)/$(KMOD).ko
else
all: kmod-missing
endif
endif
kmod-missing:
	@echo "note: $(OUT)/$(KMOD).ko not built (Wi-Fi motion on $(DEVICE)): needs $(KSRC) and $(KCROSS)gcc: make kernel-tools DEVICE=$(DEVICE)"
kernel-tools:
	$(if $(KMOD),scripts/kernel-tools.sh $(KSRC) $(KCROSS) $(KSRC_URL) $(KSRC_SHA256) $(KCC_URL) $(KCC_DIGEST))

ifneq ($(KCONFIG),)
$(OUT)/ktree/.config: $(KCONFIG)
	@mkdir -p $(OUT)/ktree
	sed -i '/GCC_VERSION >= 40800 && GCC_VERSION < 40803/,/^#endif/d' $(KSRC)/arch/arm/kernel/asm-offsets.c
	cp $< $@
	$(KMAKE) -C $(KSRC) O=$(CURDIR)/$(OUT)/ktree olddefconfig modules_prepare
else
# the fragment last: of two lines for one option, the later counts
$(OUT)/ktree/.config: $(KFRAG)
	@mkdir -p $(OUT)/ktree
	$(KMAKE) -C $(KSRC) O=$(CURDIR)/$(OUT)/ktree defconfig
	cat $< >> $@
	$(KMAKE) -C $(KSRC) O=$(CURDIR)/$(OUT)/ktree olddefconfig modules_prepare 2>&1 | grep -v -e 'override: reassigning' -e 'changes choice state' -e '^$$' || true
endif

$(OUT)/$(KMOD).ko: src/kmod/$(KMOD).c src/kmod/Kbuild $(OUT)/ktree/.config
	@mkdir -p $(OUT)/kmod
	cp src/kmod/$(KMOD).c src/kmod/Kbuild $(OUT)/kmod/
	$(KMAKE) -C $(OUT)/ktree M=$(CURDIR)/$(OUT)/kmod HM_KMOD=$(KMOD) modules 2>&1 | grep -v -e 'Module.symvers' -e 'no dependencies and modversions' || true
	cp $(OUT)/kmod/$(KMOD).ko $@

ifeq ($(STUBS),1)
# Written in the list's order, as assembly: the linker lists what it exports from the executable in the order it meets the
# names in the libraries, and the integrated assembler would sort them by name; GNU as (still in r21e) keeps them.
# Symbol table with a plain SysV hash for the same reason: a GNU hash table reorders it by bucket.
$(STOCK)/%.so: devices/$(DEVICE)/stubs/%.syms
	@mkdir -p $(STOCK)
	awk 'BEGIN { print ".syntax unified\n.arch armv7-a\n.text" } $$1 == "T" { print ".globl " $$2 "\n.type " $$2 ", %function\n" $$2 ": bx lr" } $$1 == "U" { print ".pushsection .data\n.word " $$2 "\n.popsection" }' $< > $(STOCK)/$*.s
	$(CC) -fno-integrated-as -shared -nostdlib -fuse-ld=lld -Wl,--hash-style=sysv -Wl,-soname,$*.so $(STOCK)/$*.s -o $@
else
$(STOCK)/%.so:
	@echo "missing $@: unpack the $(DEVICE) firmware first (devices/$(DEVICE)/README.md), or build with STUBS=1"; exit 1
endif

$(OUT)/mixcap $(OUT)/mixplay: $(OUT)/%: src/tools/%.c src/include/mixer_api.h src/include/netio.h $(STOCK)/libmixerAPI.so
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(STOCK)/libmixerAPI.so

$(OUT)/latency: src/tools/latency.c src/include/mixer_api.h $(STOCK)/libmixerAPI.so
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -lm $(STOCK)/libmixerAPI.so

# update bundles: the same source verifies + unpacks on the Echo and packs + signs on the PC
$(OUT)/otatool: src/tools/otatool.c src/third_party/monocypher.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $^ -o $@ -pie -fuse-ld=lld

build/otatool-host: src/tools/otatool.c src/third_party/monocypher.c
	@mkdir -p build
	cc -O2 -Wall $^ -o $@

$(OUT)/runas: src/tools/runas.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ -pie -fuse-ld=lld

# not part of "all": raw HCI probe on the Bluetooth controller (stop btmanagerd first, see the file)
$(OUT)/hciscan: src/tools/hciscan.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ -pie -fuse-ld=lld

# not part of "all": stands in for the Bluetooth stack on the mixer's A2DP output sockets (see the file)
$(OUT)/a2dpprobe: src/tools/a2dpprobe.c src/include/aipc_api.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ -pie -fuse-ld=lld -ldl

# not part of "all": LD_PRELOAD shim to see a stock daemon's libcurl requests (see the file)
$(OUT)/libcurlspy.so: src/tools/curlspy.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@ -fuse-ld=lld -ldl

$(OUT)/pryon_test: src/tools/pryon_test.c src/include/pryon_api.h $(STOCK)/libpryon.so
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(STOCK)/libpryon.so $(STOCK)/libz.so

$(OUT)/aed_test: src/tools/aed_test.c src/include/pryon_api.h $(STOCK)/libpryon.so
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(STOCK)/libpryon.so

$(OUT)/whisper_test: src/tools/whisper_test.c src/include/pryon_api.h $(STOCK)/libpryon.so
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(STOCK)/libpryon.so

# Sound detection (sound.h) runs on the same engine as the wake word: a model with Pryon has it, the PC build fakes it.
SOUND := $(if $(filter %wake_pryon.c,$(WAKE)),src/hassmic/sound_pryon.c,src/hassmic/sound_none.c)
# So does whisper detection (whisper.h), with a model from Amazon (scripts/artifacts.sh).
WHISPER := $(if $(filter %wake_pryon.c,$(WAKE)),src/hassmic/whisper_pryon.c,src/hassmic/whisper_none.c)

RNNOISE := $(addprefix src/third_party/rnnoise/,denoise.c rnn.c rnn_data.c pitch.c kiss_fft.c celt_lpc.c)
HASSMIC := src/hassmic/main.c src/hassmic/wyoming.c src/hassmic/proto_wyoming.c src/hassmic/proto_esphome.c src/hassmic/buttons.c \
           src/hassmic/sendspin.c src/hassmic/arb.c src/hassmic/ble.c src/hassmic/ble_crypto.c src/hassmic/a2dp.c src/hassmic/a2dp_codecs.c src/hassmic/sbc.c src/hassmic/btout.c src/hassmic/ota.c src/hassmic/update.c src/hassmic/adbwifi.c src/hassmic/wifimotion.c src/hassmic/ws.c src/hassmic/net.c src/hassmic/noise.c src/hassmic/hash.c src/hassmic/sounds.c src/hassmic/micgain.c src/hassmic/micdenoise.c src/hassmic/settings.c src/hassmic/web.c build/web_assets.c \
           src/third_party/monocypher.c src/third_party/freeaptx.c $(RNNOISE)
HASSMIC_H := $(wildcard src/hassmic/*.h src/include/*.h) build/.build-id

# The settings page (web.c), gzip'd into the binary
WEB := web/index.html:/ web/app.js:/app.js web/crypto.js:/crypto.js
build/web_assets.c: tools/embed.py $(foreach w,$(WEB),$(firstword $(subst :, ,$(w))))
	@mkdir -p build
	python3 tools/embed.py $@ $(WEB)

$(OUT)/hassmic: $(HASSMIC) $(BOARD) $(AUDIO) $(WAKE) $(SOUND) $(WHISPER) $(HASSMIC_H) $(addprefix $(STOCK)/,$(LIBS))
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Isrc/hassmic $(filter %.c,$^) -o $@ $(LDFLAGS) -lm -ldl $(addprefix $(STOCK)/,$(LIBS))

# PC build for protocol tests: file audio backend, no wake word (SIGUSR1 triggers), fake sound and whisper detection, identity of $(DEVICE).
build/hassmic-host: $(HASSMIC) $(BOARD) src/hassmic/audio_file.c src/hassmic/wake_none.c src/hassmic/sound_none.c src/hassmic/whisper_none.c $(HASSMIC_H) build/.device
	@mkdir -p build
	cc -O2 -Wall -Wextra $(DEFS) -Isrc/include -Isrc/hassmic $(filter %.c,$^) -o $@ -lpthread -lm -ldl -lopus

# ARM build with file audio but the device's wake word engine, for running under qemu-arm (tools/qrun.sh).
$(OUT)/hassmic-qemu: $(HASSMIC) $(BOARD) src/hassmic/audio_file.c $(WAKE) $(SOUND) $(WHISPER) $(HASSMIC_H) $(call STOCK_LIBS,libpryon.so libopus.so libz.so)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Isrc/hassmic $(filter %.c,$^) -o $@ $(LDFLAGS) -lm -ldl $(call STOCK_LIBS,libpryon.so libopus.so libz.so)

host: build/hassmic-host $(OUT)/hassmic-qemu build/otatool-host

# Building blocks of the Sendspin client, checked against reference implementations (aiohttp, python noiseprotocol).
UNIT := src/hassmic/hash.c src/hassmic/ws.c src/hassmic/net.c src/hassmic/noise.c src/third_party/monocypher.c
unit:
	@mkdir -p build
	cc -O2 -Wall -Isrc/hassmic -Isrc/include tests/unit/hash_test.c $(UNIT) -lpthread -o build/hash_test && build/hash_test
	cc -O2 -Wall -D_GNU_SOURCE -Isrc/hassmic -Isrc/include -include stdlib.h tests/unit/ws_test.c $(UNIT) -lpthread -o build/ws_test
	cc -O2 -Wall -Isrc/hassmic -Isrc/include tests/unit/noise_test.c $(UNIT) -lpthread -o build/noise_test
	cc -O2 -Wall -Isrc/hassmic tests/unit/ble_crypto_test.c src/hassmic/ble_crypto.c -o build/ble_crypto_test && build/ble_crypto_test
	cc -O2 -Wall -Isrc/hassmic tests/unit/micgain_test.c src/hassmic/micgain.c -lm -o build/micgain_test && build/micgain_test
	cc -O2 -Wall -Isrc/hassmic tests/unit/wifimotion_test.c src/hassmic/wifimotion.c -lpthread -lm -o build/wifimotion_test && build/wifimotion_test
	cc -O2 -Wall -Isrc/hassmic tests/unit/micdenoise_test.c src/hassmic/micdenoise.c $(RNNOISE) -lm -o build/micdenoise_test && build/micdenoise_test
	.venv/bin/python tests/unit/ws_ref.py build/ws_test
	.venv/bin/python tests/unit/noise_ref.py build/noise_test
	cc -O2 -Wall -Isrc/hassmic tests/unit/a2dp_codecs_test.c src/hassmic/a2dp_codecs.c src/hassmic/sbc.c src/third_party/freeaptx.c -lm -ldl -lopus -o build/a2dp_codecs_test && build/a2dp_codecs_test
	if command -v sbcenc >/dev/null; then tests/unit/sbc_ref.sh; else echo "sbc: sbcenc/sbcdec (package sbc) missing, skipped"; fi
	if command -v node >/dev/null; then .venv/bin/python tests/unit/web_crypto_test.py; else echo "web crypto: node missing, skipped"; fi

# what the daemon reports, and what bundles are called (ota-push.sh, CI)
version:
	@echo $(VERSION)$(if $(RELEASE),,+$(BUILD))

clean:
	rm -rf build

.PHONY: all host unit version clean kmod-missing kernel-tools FORCE
