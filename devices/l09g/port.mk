# Included after upstream Makefile, from the echo-dot-assist source directory.
PORT := devices/l09g
PORT_CC ?= $(PORT)/toolchain/aarch64--glibc--stable-2018.11-1/bin/aarch64-linux-gcc
PORT_AUDIO := $(PORT)/audio_pulse.c
PORT_BOARD := $(PORT)/board.c
PORT_LIB ?= $(PORT)/target-libs
PORT_SOURCES := $(filter %.c,$(HASSMIC) $(PORT_BOARD) $(PORT_AUDIO) $(PORT)/bt_switch.c $(PORT)/led_l09g.c src/hassmic/wake_none.c src/hassmic/sound_none.c src/hassmic/whisper_none.c)
PORT_OBJECTS := $(addprefix build/l09g/obj/,$(notdir $(PORT_SOURCES:.c=.o)))
PORT_FLAGS := -O2 -Wall -Wextra -std=gnu11 -DL09G_PORT $(DEFS) -Isrc/include -Isrc/hassmic -I$(PORT)/include -I$(PORT)

define PORT_COMPILE
build/l09g/obj/$(notdir $(1:.c=.o)): $(1) $(HASSMIC_H)
	@mkdir -p build/l09g/obj
	$(PORT_CC) $(PORT_FLAGS) -c $$< -o $$@
endef
$(foreach source,$(PORT_SOURCES),$(eval $(call PORT_COMPILE,$(source))))

build/l09g/hassmic: $(PORT_OBJECTS)
	@mkdir -p build/l09g
	$(PORT_CC) $(PORT_OBJECTS) -o $@ -pthread -lm -ldl -Wl,--allow-shlib-undefined $(PORT_LIB)/libpulse-simple.so.0.1.1 $(PORT_LIB)/libpulse.so.0.20.3 $(PORT_LIB)/libopus.so.0.8.0 $(PORT_LIB)/libz.so.1.2.11

.PHONY: l09g
l09g: build/l09g/hassmic build/l09g/bt-agent

build/l09g/bt-agent: $(PORT)/bt-agent.c
	@mkdir -p build/l09g
	$(PORT_CC) -O2 -Wall -Wextra $< -o $@ -Wl,--allow-shlib-undefined $(PORT_LIB)/libdbus-1.so.3.19.11
