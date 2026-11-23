# Include after the top-level Makefile; see README.md for prerequisites.
LX05_DIR := devices/lx05
LX05_CC ?= $(LX05_DIR)/toolchain/arm-linux-musleabihf-cross/bin/arm-linux-musleabihf-gcc
LX05_OPUS ?= $(LX05_DIR)/toolchain/opus-1.5.2/.libs/libopus.a
LX05_ZINC ?= $(LX05_DIR)/toolchain/include
LX05_SOURCES := $(filter %.c,$(HASSMIC)) $(addprefix $(LX05_DIR)/,board.c audio_lx05.c alsa_compat.c led_lx05.c bt_lx05.c) src/hassmic/wake_none.c src/hassmic/sound_none.c src/hassmic/whisper_none.c
LX05_FLAGS := -O2 -std=gnu11 -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -DLX05_PORT $(DEFS) -Isrc/include -Isrc/hassmic -I$(LX05_ZINC)

.PHONY: lx05
lx05: build/lx05/hassmic build/lx05/mdns

build/lx05/libasound-link.so: $(LX05_DIR)/asound-stub.c
	@mkdir -p build/lx05
	$(LX05_CC) -shared -fPIC -Wl,-soname,libasound.so.2 $< -o $@

build/lx05/libz-link.so: $(LX05_DIR)/z-stub.c
	@mkdir -p build/lx05
	$(LX05_CC) -shared -fPIC -Wl,-soname,libz.so.1 $< -o $@

build/lx05/hassmic: $(LX05_SOURCES) $(HASSMIC_H) build/lx05/libasound-link.so build/lx05/libz-link.so
	$(LX05_CC) $(LX05_FLAGS) $(LX05_SOURCES) -o $@ $(LX05_OPUS) build/lx05/libasound-link.so build/lx05/libz-link.so -pthread -lm -ldl -latomic

build/lx05/mdns: $(LX05_DIR)/mdns.c
	@mkdir -p build/lx05
	$(LX05_CC) -O2 -static $< -o $@
