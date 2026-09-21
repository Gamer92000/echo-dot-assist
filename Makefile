# Cross-build for Echo Dot 3 (armv7, Android 7.1 bionic), linking against stock libs.
NDK     ?= $(CURDIR)/toolchain/android-ndk-r21e
CC      := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi24-clang
STOCK   := $(CURDIR)/firmware/rootfs/system/lib
CFLAGS  := -O2 -Wall -Wextra -fPIE -Isrc/include
LDFLAGS := -pie -fuse-ld=lld -Wl,--allow-shlib-undefined -Wl,--unresolved-symbols=ignore-in-shared-libs

BIN := build/mixcap build/mixplay build/pryon_test build/hassmic build/runas

all: $(BIN)

build/mixcap build/mixplay: build/%: src/tools/%.c src/include/mixer_api.h src/include/netio.h
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(STOCK)/libmixerAPI.so

build/runas: src/tools/runas.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ -pie -fuse-ld=lld

build/pryon_test: src/tools/pryon_test.c src/include/pryon_api.h
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(STOCK)/libpryon.so

HASSMIC := src/hassmic/main.c src/hassmic/wyoming.c src/hassmic/proto_wyoming.c src/hassmic/proto_esphome.c src/hassmic/buttons.c \
           src/hassmic/sendspin.c src/hassmic/ws.c src/hassmic/noise.c src/hassmic/hash.c src/third_party/monocypher.c
HASSMIC_H := $(wildcard src/hassmic/*.h src/include/*.h)

build/hassmic: $(HASSMIC) src/hassmic/audio_mixer.c src/hassmic/wake_pryon.c $(HASSMIC_H)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc/hassmic $(filter %.c,$^) -o $@ $(LDFLAGS) -lm $(STOCK)/libmixerAPI.so $(STOCK)/libpryon.so

# PC build for protocol tests: file audio backend, no wake word (SIGUSR1 triggers).
build/hassmic-host: $(HASSMIC) src/hassmic/audio_file.c src/hassmic/wake_none.c $(HASSMIC_H)
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/include -Isrc/hassmic $(filter %.c,$^) -o $@ -lpthread -lm

# ARM build with file audio but the real wake word, for running under qemu-arm (tools/qrun.sh).
build/hassmic-qemu: $(HASSMIC) src/hassmic/audio_file.c src/hassmic/wake_pryon.c $(HASSMIC_H)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc/hassmic $(filter %.c,$^) -o $@ $(LDFLAGS) -lm $(STOCK)/libpryon.so

host: build/hassmic-host build/hassmic-qemu

# Building blocks of the Sendspin client, checked against reference implementations (aiohttp, python noiseprotocol).
UNIT := src/hassmic/hash.c src/hassmic/ws.c src/hassmic/noise.c src/third_party/monocypher.c
unit:
	@mkdir -p build
	cc -O2 -Wall -Isrc/hassmic -Isrc/include tests/unit/hash_test.c $(UNIT) -lpthread -o build/hash_test && build/hash_test
	cc -O2 -Wall -Isrc/hassmic -Isrc/include -include stdlib.h tests/unit/ws_test.c $(UNIT) -lpthread -o build/ws_test
	cc -O2 -Wall -Isrc/hassmic -Isrc/include tests/unit/noise_test.c $(UNIT) -lpthread -o build/noise_test
	.venv/bin/python tests/unit/ws_ref.py build/ws_test
	.venv/bin/python tests/unit/noise_ref.py build/noise_test

clean:
	rm -rf build

.PHONY: all host unit clean
