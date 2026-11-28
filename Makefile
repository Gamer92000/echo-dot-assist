# Cross-build for one Echo model, linking against that model's stock libraries.
#   make [DEVICE=donut]      device binaries into build/$(DEVICE)/, against firmware/$(DEVICE)/rootfs
# Everything model-specific comes from devices/$(DEVICE)/ (device.mk here, board.c in the daemon); see devices/README.md.
DEVICE  ?= donut
ifeq ($(wildcard devices/$(DEVICE)/device.mk),)
$(error unknown DEVICE "$(DEVICE)": no devices/$(DEVICE)/device.mk)
endif
include devices/$(DEVICE)/device.mk
# the light ring: the Dots' LEDs through ledcontroller, or a band on the screen (device.mk)
RING    ?= src/hassmic/ring.c
# a screen's UI (screen.h): screen.c runs src/ui/ (libhmui.so, loaded by scripts/device/screen.dex) where there is one
SCREEN  ?= src/hassmic/screen_none.c

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
# The same time in seconds: the build's own idea of "now" before an Echo's clock is set (plugin_index.c pi_floor)
BUILD_EPOCH := $(shell git log -1 --format=%ct 2>/dev/null || echo 0)
# Where online updates look (UPDATE_REPO): this project's releases, or a fork's own (scripts/fork-release.sh)
include keys/release.conf
# Where the plugin index is and the key it signs with (keys/plugins.conf): both built into hassmic (plugin_index.c)
include keys/plugins.conf
# (PLUGINS_PUB as this tree names it, before a fork's fork.conf: the second key's rule below tells a typo here from a
# fork's own first key)
PLUGINS_PUB_CONF := $(if $(filter file,$(origin PLUGINS_PUB)),$(PLUGINS_PUB))
-include secrets/fork.conf
UPDATE_REPO := $(strip $(UPDATE_REPO))
ifneq ($(words $(UPDATE_REPO))$(findstring ",$(UPDATE_REPO))$(findstring ',$(UPDATE_REPO)),1)
$(error UPDATE_REPO "$(UPDATE_REPO)": owner/name, no quotes or spaces (keys/release.conf, secrets/fork.conf))
endif
PLUGIN_INDEX := $(strip $(PLUGIN_INDEX))
ifneq ($(words $(PLUGIN_INDEX))$(filter https://%/,$(PLUGIN_INDEX))$(findstring ",$(PLUGIN_INDEX))$(findstring ',$(PLUGIN_INDEX)),1$(PLUGIN_INDEX))
$(error PLUGIN_INDEX "$(PLUGIN_INDEX)": an https:// address ending in /, no quotes or spaces (keys/plugins.conf, secrets/fork.conf))
endif
PLUGINS_LEAST_INDEX := $(strip $(PLUGINS_LEAST_INDEX))
PLUGINS_LEAST_REVOKED := $(strip $(PLUGINS_LEAST_REVOKED))
ifneq ($(shell printf %s '$(PLUGINS_LEAST_INDEX) $(PLUGINS_LEAST_REVOKED)' | grep -Ex '[0-9]{1,12} [0-9]{1,12}'),$(PLUGINS_LEAST_INDEX) $(PLUGINS_LEAST_REVOKED))
$(error PLUGINS_LEAST_INDEX, PLUGINS_LEAST_REVOKED: whole numbers, the serials this build takes at least (keys/plugins.conf, scripts/plugins-least.sh))
endif
PLUGINS_PUB_HEX := $(shell od -An -tx1 -v $(PLUGINS_PUB) 2>/dev/null | tr -d ' \n')
ifneq ($(words $(PLUGINS_PUB_HEX))$(shell printf %s '$(PLUGINS_PUB_HEX)' | wc -c),164)
$(error PLUGINS_PUB "$(PLUGINS_PUB)": not a public key of 32 bytes (keys/plugins.conf, secrets/fork.conf))
endif
# A key under which any signature holds (src/include/edkey.h: small order; the null signature verifies under it for
# every text) is no key: refused for each the build carries (the index's, both, and the release key bundles pack),
# by scripts/otatool.py weak(), the rule plugin_index.c and ota.c apply again on the Echo (a review of a30d6ed, K3).
# weak() also refuses what it cannot read as a point: six such encodings Monocypher reads as points of small order
# (y = p, p + 1 and the order-2 point with the sign bit set), which the Echo then refused at run time while the build
# took them.  "ok" or why not; fails closed: without python3 (or an error in it) no key passes (a review of 3c5d7ff, R4)
key_bad = $(filter-out ok,$(or $(shell python3 -c 'import sys; sys.path.insert(0, "scripts"); import otatool; print("weak" if otatool.weak(bytes.fromhex(sys.argv[1])) else "ok")' $(1) 2>/dev/null),nopython))
key_why = $(if $(filter nopython,$(1)),cannot be checked: the build asks scripts/otatool.py weak() and needs python3 for it,a key under which any signature holds (small order) or no point at all: no key)
PLUGINS_PUB_BAD := $(call key_bad,$(PLUGINS_PUB_HEX))
ifneq ($(PLUGINS_PUB_BAD),)
$(error PLUGINS_PUB "$(PLUGINS_PUB)": $(call key_why,$(PLUGINS_PUB_BAD)) (keys/plugins.conf, secrets/fork.conf))
endif
RELEASE_PUB_HEX := $(shell od -An -tx1 -v $(RELEASE_PUB) 2>/dev/null | tr -d ' \n')
RELEASE_PUB_BAD := $(if $(RELEASE_PUB_HEX),$(call key_bad,$(RELEASE_PUB_HEX)))
ifneq ($(RELEASE_PUB_BAD),)
$(error RELEASE_PUB "$(RELEASE_PUB)": $(call key_why,$(RELEASE_PUB_BAD)) (keys/release.conf, secrets/fork.conf))
endif
# The second index key (5.2's first step of a new key): none, or one of 32 bytes other than the first.  It belongs
# with one first key, by content: PLUGINS_PUB2_WITH is that key's hex, and a build whose PLUGINS_PUB is another key
# (a fork's own, by its fork.conf, on make's command line, or keys/plugins.pub replaced in place) does not carry it.
# Paths were compared before, and each of those three cases came out wrong (a review of a30d6ed, K5)
# PLUGINS_PUB2_WITH as od prints it, whatever case it was typed in, and 64 hex digits or the build stops.  A
# mismatch is a fork's own first key only where PLUGINS_PUB is not this tree's own: where it is, it is a typo in
# keys/plugins.conf, and a silent drop shipped 5.2's step-(1) release without the new key, which its step (3) then
# needs on every Echo (a review of 3c5d7ff, R5).  So is keys/plugins.pub replaced in place by a fork: its key goes
# into secrets/fork.conf (scripts/fork-release.sh), or keys/plugins.conf's PLUGINS_PUB2 is emptied with it
PLUGINS_PUB2 := $(strip $(PLUGINS_PUB2))
# (override: a value given on make's command line is lowercased too; a plain assignment left it as typed, and capitals
# there stopped the build or, against a fork's key, compared as another key: a review of 3fcd319, S6)
override PLUGINS_PUB2_WITH := $(shell printf %s '$(strip $(PLUGINS_PUB2_WITH))' | tr A-F a-f)
ifneq ($(PLUGINS_PUB2),)
ifeq ($(PLUGINS_PUB2_WITH),)
$(error PLUGINS_PUB2 "$(PLUGINS_PUB2)" without PLUGINS_PUB2_WITH, the hex of the first key it belongs with (keys/plugins.conf, secrets/fork.conf))
endif
ifneq ($(shell printf %s '$(PLUGINS_PUB2_WITH)' | grep -Ex '[0-9a-f]{64}'),$(PLUGINS_PUB2_WITH))
$(error PLUGINS_PUB2_WITH "$(PLUGINS_PUB2_WITH)": not the 64 hex digits of a key (od -An -tx1 -v keys/plugins.pub | tr -d ' \n'; keys/plugins.conf, secrets/fork.conf))
endif
ifneq ($(PLUGINS_PUB2_WITH),$(PLUGINS_PUB_HEX))
ifneq ($(and $(PLUGINS_PUB_CONF),$(filter $(PLUGINS_PUB_CONF),$(PLUGINS_PUB))),)
$(error PLUGINS_PUB2 "$(PLUGINS_PUB2)" belongs with $(PLUGINS_PUB2_WITH), and PLUGINS_PUB "$(PLUGINS_PUB)" as keys/plugins.conf names it is $(PLUGINS_PUB_HEX): a typo in PLUGINS_PUB2_WITH, or keys/plugins.pub replaced (a fork's key goes into secrets/fork.conf, or PLUGINS_PUB2 is emptied with it))
endif
override PLUGINS_PUB2 :=
endif
endif
PLUGINS_PUB2_HEX := $(if $(PLUGINS_PUB2),$(shell od -An -tx1 -v $(PLUGINS_PUB2) 2>/dev/null | tr -d ' \n'))
ifneq ($(PLUGINS_PUB2),)
ifneq ($(words $(PLUGINS_PUB2_HEX))$(shell printf %s '$(PLUGINS_PUB2_HEX)' | wc -c),164)
$(error PLUGINS_PUB2 "$(PLUGINS_PUB2)": not a public key of 32 bytes (keys/plugins.conf, secrets/fork.conf))
endif
ifeq ($(PLUGINS_PUB2_HEX),$(PLUGINS_PUB_HEX))
$(error PLUGINS_PUB2 "$(PLUGINS_PUB2)": the same key as PLUGINS_PUB (keys/plugins.conf, secrets/fork.conf))
endif
PLUGINS_PUB2_BAD := $(call key_bad,$(PLUGINS_PUB2_HEX))
ifneq ($(PLUGINS_PUB2_BAD),)
$(error PLUGINS_PUB2 "$(PLUGINS_PUB2)": $(call key_why,$(PLUGINS_PUB2_BAD)) (keys/plugins.conf, secrets/fork.conf))
endif
endif
# The models whose bundle is baked into a firmware image by an offline builder (cannoli-os-builder, laser-os-builder).
SOURCE_ID := cannoli laser
ifneq ($(filter $(DEVICE),$(SOURCE_ID)),)
# BUILD from the sources themselves (tools/source-id.py), not git describe: two builds of one dirty tree must differ, since
# factory promotion compares the bundle's VERSION (VERSION+BUILD).  The version and build time stay the commit's, as for
# every model: update.c reports VERSION to Home Assistant, which compares it with the releases (a fixed one had every
# release show as an update, installed or not).  Fixed only where git gives none (a source archive).
BUILD := $(shell python3 tools/source-id.py $(DEVICE))
ifeq ($(strip $(BUILD)),)
$(error cannot compute $(DEVICE)'s source identity (tools/source-id.py))
endif
ifeq ($(VERSION),0)
VERSION := 2026.11.13.000000
BUILD_TIME := Nov 13 2026 00:00:00
BUILD_EPOCH := 1794528000
endif
endif
# How the model's system install writes its SELinux policy (device.conf INSTALL): the Plugins page's warning names the
# way to run it again (web.c /api/plugins "install_method", docs/plugins.md 2.7)
INSTALL_METHOD := $(shell sed -n 's/^INSTALL=\([a-z0-9-]*\).*/\1/p' devices/$(DEVICE)/device.conf 2>/dev/null)
# Whether root's netwatch roams between access points (scripts/device/roam.sh): where it stops the model's wifisvc
# (device.conf WIFI_SERVICE); the settings page lists the switch there only
WIFI_ROAMING := $(if $(shell sed -n 's/^WIFI_SERVICE=\([a-z]\)/\1/p' devices/$(DEVICE)/device.conf 2>/dev/null),1,0)
DEFS    := -DVERSION='"$(VERSION)"' -DBUILD='"$(BUILD)"' -DBUILD_TIME='"$(BUILD_TIME)"' -DREPO='"$(UPDATE_REPO)"' \
           -DPLUGIN_INDEX='"$(PLUGIN_INDEX)"' -DPLUGINS_PUB='"$(PLUGINS_PUB_HEX)"' -DPLUGINS_PUB2='"$(PLUGINS_PUB2_HEX)"' -DPLUGINS_LEAST_INDEX=$(PLUGINS_LEAST_INDEX)LL \
           -DPLUGINS_LEAST_REVOKED=$(PLUGINS_LEAST_REVOKED)LL -DBUILD_EPOCH=$(BUILD_EPOCH)LL $(if $(RELEASE),-DRELEASE) \
           -DINSTALL_METHOD='"$(INSTALL_METHOD)"' -DWIFI_ROAMING=$(WIFI_ROAMING)
# sdk/: the plugins' wire format (plugmsg.h), the same file their client library has
CFLAGS  := -O2 -Wall -Wextra -fPIE -Isrc/include -Isdk $(DEFS)
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
BIN := $(OUT)/hassmic $(OUT)/runas $(OUT)/plugrun $(OUT)/otatool $(if $(filter %screen.c,$(SCREEN)),$(OUT)/libhmui.so) $(if $(LEDSHIM),$(OUT)/libhmled.so) \
       $(if $(filter libmixerAPI.so,$(LIBS)),$(OUT)/mixcap $(OUT)/mixplay $(OUT)/latency) \
       $(if $(filter -lOpenSLES,$(SYSLIBS)),$(OUT)/slplay) \
       $(if $(filter libpryon.so,$(LIBS)),$(OUT)/pryon_test $(OUT)/aed_test $(OUT)/whisper_test)

.DEFAULT_GOAL := all
all: $(BIN) $(if $(THREAD),$(OUT)/rcpbridge $(OUT)/otbr-agent $(OUT)/otbr-agent-csl $(OUT)/ot-ctl $(OUT)/zigbeed $(OUT)/gprcp)

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
# KMOD_RADIO: the radio's (the Echo 4's GP712 chip clock for timed transmit, hassmic_gptime; PLAN.md "Thread 1.4" 2b)
KMODS := $(KMOD) $(KMOD_RADIO)
ifneq ($(strip $(KMODS)),)
ifneq ($(and $(wildcard $(KSRC)/Makefile),$(wildcard $(KCROSS)gcc)),)
all: $(foreach k,$(KMODS),$(OUT)/$(k).ko)
else
all: kmod-missing
endif
endif
kmod-missing:
	@echo "note: $(foreach k,$(KMODS),$(OUT)/$(k).ko) not built ($(DEVICE)): needs $(KSRC) and $(KCROSS)gcc: make kernel-tools DEVICE=$(DEVICE)"
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

# Module.symvers: the kernel's own CRCs where it has CONFIG_MODVERSIONS (athena, cannoli: tools/kcrc.py), for modpost;
# copied at every build of the module, so a changed list reaches it.  KMOD_INIT: where a kernel whose struct module is
# not kernel.org's reads the init pointer (cannoli: Android's common kernel), tools/kmod-init.py
$(foreach k,$(KMODS),$(OUT)/$(k).ko): $(OUT)/%.ko: src/kmod/%.c src/kmod/Kbuild $(wildcard src/kmod/*.h) $(OUT)/ktree/.config \
                   $(wildcard devices/$(DEVICE)/Module.symvers) $(if $(KMOD_INIT),tools/kmod-init.py)
	@mkdir -p $(OUT)/kmod-$*
	cp src/kmod/$*.c src/kmod/Kbuild $(wildcard src/kmod/*.h) $(OUT)/kmod-$*/
	$(if $(wildcard devices/$(DEVICE)/Module.symvers),cp devices/$(DEVICE)/Module.symvers $(OUT)/ktree/Module.symvers)
	$(KMAKE) -C $(OUT)/ktree M=$(CURDIR)/$(OUT)/kmod-$* HM_KMOD=$* modules 2>&1 | grep -v -e 'Module.symvers' -e 'no dependencies and modversions' || true
	$(if $(KMOD_INIT),python3 tools/kmod-init.py $(OUT)/kmod-$*/$*.ko $(KMOD_INIT))
	cp $(OUT)/kmod-$*/$*.ko $@

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

# the Android models' mixplay: OpenSL ES on the music stream, as audio_android.c plays
$(OUT)/slplay: src/tools/slplay.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -lOpenSLES

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

# The 802.15.4 radio (device.mk THREAD := 1, the Echo 4; docs/re-thread.md): rcpbridge serves Amazon's RCP on a unix
# socket for a remote host (root runs it, main.sh radio_watch), otbr-agent is our own Thread border router, upstream's
# ot-br-posix built by scripts/otbr-build.sh from pinned sources (cmake and ninja on the PC; nothing of Amazon's)
$(OUT)/rcpbridge: src/tools/rcpbridge.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ -pie -fuse-ld=lld
build/rcpbridge-host: src/tools/rcpbridge.c
	@mkdir -p build
	cc -O2 -Wall -Wextra $< -o $@
# zigbeed: the Zigbee stack of our own on the same radio (src/zigbee/, docs/zigbee.md): a coordinator for ZHA and
# Zigbee2MQTT (the deCONZ protocol), or a router of another's network.  Root runs it as rcpbridge's client
ZIGBEE_SRC := $(wildcard src/zigbee/*.c)
$(OUT)/zigbeed: $(ZIGBEE_SRC) src/zigbee/zb.h $(OUT)/libmbedtls.a
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(MBEDTLS_INC) $(ZIGBEE_SRC) $(OUT)/libmbedtls.a -o $@ -pie -fuse-ld=lld
# zigbeed's parsers fuzzed (tests/unit/zb_fuzz.c: frames from the air in every role, the deCONZ host's bytes; the
# MIC check passes in this build only), FUZZ_SECS, clang; not in make unit
unit-zb_fuzz: build/libmbedtls-host.a
	@mkdir -p build/zb_fuzz-corpus
	clang -O1 -g -Wall -Wextra -DZB_FUZZ -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all $(MBEDTLS_INC) tests/unit/zb_fuzz.c \
		$(filter-out src/zigbee/spinel.c src/zigbee/zigbeed.c src/zigbee/store.c,$(ZIGBEE_SRC)) build/libmbedtls-host.a -o build/zb_fuzz
	build/zb_fuzz -max_total_time=$(FUZZ_SECS) -max_len=1024 -print_final_stats=1 -artifact_prefix=build/zb_fuzz-crash- build/zb_fuzz-corpus
build/zigbeed-host: $(ZIGBEE_SRC) src/zigbee/zb.h build/libmbedtls-host.a
	@mkdir -p build
	cc -O2 -g -Wall -Wextra $(MBEDTLS_INC) $(ZIGBEE_SRC) build/libmbedtls-host.a -o $@
$(OUT)/otbr-agent $(OUT)/ot-ctl &: scripts/otbr-build.sh
	scripts/otbr-build.sh $(OUT) $(TARGET) $(NDK)
# the same with the CSL transmitter (OTBR_CSL=1): what root runs where the setting thread_csl is on and our module
# hooked the GP712's driver (main.sh radio_csl); built in a folder of its own, its ot-ctl the same as the other's
$(OUT)/otbr-agent-csl: scripts/otbr-build.sh
	OTBR_CSL=1 scripts/otbr-build.sh $(OUT)/otbr-csl $(TARGET) $(NDK)
	cp $(OUT)/otbr-csl/otbr-agent $@
# gprcp: our own RCP for the GP712 in place of Amazon's ot-rcp (PLAN.md "Thread 1.4"; docs/re-gp712.md): OpenThread's
# RCP on Qorvo's GP712 platform and public library, transmit security and timing in software; zig, static musl
$(OUT)/gprcp: scripts/gprcp-build.sh $(shell find src/gprcp -type f)
	scripts/gprcp-build.sh $(OUT)

# The plugins' launcher (docs/plugins.md 2): root's, static, nothing of Amazon's; its system call filter is made
# from the table the SDK also reads.  The PC's (hassmic's own tests) changes no user and has the x86_64 filter.
build/plugfilter.h: sdk/filter.def tools/mkfilter.py
	@mkdir -p build
	python3 tools/mkfilter.py sdk/filter.def $@

$(OUT)/plugrun: src/tools/plugrun.c src/include/plugrun.h build/plugfilter.h
	@mkdir -p $(OUT)
	$(CC) -Os -Wall -Wextra -ffunction-sections -fdata-sections -Isrc/include -Ibuild $< -o $@ -static -s -fuse-ld=lld -Wl,--gc-sections

build/plugrun-host: src/tools/plugrun.c src/include/plugrun.h build/plugfilter.h
	@mkdir -p build
	cc -O2 -Wall -Wextra -DPLUGRUN_HOST -Isrc/include -Ibuild $< -o $@

# The sandbox asked from inside (docs/plugins.md 6): for the PC against the filter's x86_64 translation (make
# unit-plugrun); for an Echo it is built as a plugin is, never with the NDK (a static bionic program does not start in
# the sandbox) but with a musl toolchain, and run by scripts/plugin-probe.sh.  PLUGIN_CC names one: zig where it is
# installed, or a musl cross gcc.  Plugins themselves are not built here: each has a repository of its own
PLUGIN_CC ?= $(if $(shell command -v zig 2>/dev/null),zig cc -target arm-linux-musleabihf -mcpu=cortex_a7,)
build/plugprobe-host: src/tools/plugprobe.c
	@mkdir -p build
	cc -O2 -Wall -Wextra -static $< -lpthread -o $@
build/plugins/plugprobe: src/tools/plugprobe.c
	@mkdir -p build/plugins
	$(if $(PLUGIN_CC),,$(error no musl toolchain for plugins: install zig, or set PLUGIN_CC (docs/plugins.md 5.8)))
	$(PLUGIN_CC) -O2 -Wall -Wextra -static -Wl,-z,noexecstack $< -o $@

# The screen UI (src/ui/, scene.h): a JNI library for app_process, against the NDK's libandroid (ANativeWindow) and
# libjnigraphics (AndroidBitmap); nothing of the firmware's.  The scene alone is make unit's ui_test on the PC.
UI_SCENE := src/ui/scene.c src/ui/widgets.c src/ui/face.c src/ui/drawer.c src/ui/music.c src/ui/words.c src/ui/lang.c src/ui/gfx.c
UI := src/ui/android.c src/ui/touch.c $(UI_SCENE)
# the Echo Dot 5's ledcontroller loads it (device.mk LEDSHIM, src/ledshim/ledshim.c says why)
$(OUT)/libhmled.so: src/ledshim/ledshim.c build/.build-id
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@ -fuse-ld=lld -Wl,--no-undefined -Wl,-soname,libhmled.so -llog -ldl

$(OUT)/libhmui.so: $(UI) src/ui/scene.h src/ui/ui.h src/ui/gfx.h build/.build-id
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fPIC -shared -Isrc/ui $(UI) -o $@ -fuse-ld=lld -Wl,--no-undefined -Wl,-soname,libhmui.so -landroid -ljnigraphics -lm

# not part of "all": what the Echo Dot 5's LEDs show, read back through libhmled.so (see the file)
$(OUT)/ledstate: src/tools/ledstate.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ -pie -fuse-ld=lld

# not part of "all": raw HCI probe on the Bluetooth controller (stop btmanagerd first, see the file)
$(OUT)/hciscan: src/tools/hciscan.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ -pie -fuse-ld=lld

# not part of "all": SCO/eSCO over HCI between two Echos (issue #233, see the file)
$(OUT)/scoprobe: src/tools/scoprobe.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ -pie -fuse-ld=lld -lm

# not part of "all": BTCVSD's air bitstream on /dev/ebc (the Dot 2's integrated Bluetooth, issue #233, see the file)
$(OUT)/ebcprobe: src/tools/ebcprobe.c
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

# not part of "all": what the Echo's device attestation HAL answers (the key, its fields, a signature), for the
# questions dha.c was written against
$(OUT)/dha_test: src/tools/dha_test.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -o $@ -pie -fuse-ld=lld -ldl

$(OUT)/pryon_test: src/tools/pryon_test.c src/include/pryon_api.h $(STOCK)/libpryon.so $(STOCK)/libz.so
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

# mbedTLS (src/third_party/mbedtls, tools/mbedtls-vendor.sh; issue #236): hassmic's TLS client (tls.c), a library of
# its own per target, so that its 55 files are compiled once and not with every link of hassmic (each target here
# compiles all of hassmic's own files in one cc).  AR is the NDK's for the Echo; D: no timestamps (reproducible).
MBEDTLS_DIR   := src/third_party/mbedtls
MBEDTLS_C     := $(sort $(wildcard $(MBEDTLS_DIR)/library/*.c))     # (sorted: make before 4.3 does not)
MBEDTLS_H     := $(wildcard $(MBEDTLS_DIR)/*.h $(MBEDTLS_DIR)/include/*/*.h $(MBEDTLS_DIR)/library/*.h)
MBEDTLS_INC   := -I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR) -DMBEDTLS_CONFIG_FILE='"hassmic_config.h"'
MBEDTLS_FLAGS := -O2 -fPIE $(MBEDTLS_INC) -I$(MBEDTLS_DIR)/library
AR_ARM        := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar
$(OUT)/mbedtls/%.o: $(MBEDTLS_DIR)/library/%.c $(MBEDTLS_H)
	@mkdir -p $(@D)
	$(CC) $(MBEDTLS_FLAGS) -c $< -o $@
build/mbedtls-host/%.o: $(MBEDTLS_DIR)/library/%.c $(MBEDTLS_H)
	@mkdir -p $(@D)
	cc $(MBEDTLS_FLAGS) -c $< -o $@
$(OUT)/libmbedtls.a: $(patsubst $(MBEDTLS_DIR)/library/%.c,$(OUT)/mbedtls/%.o,$(MBEDTLS_C))
	rm -f $@; $(AR_ARM) rcsD $@ $^
build/libmbedtls-host.a: $(patsubst $(MBEDTLS_DIR)/library/%.c,build/mbedtls-host/%.o,$(MBEDTLS_C))
	rm -f $@; ar rcsD $@ $^
HASSMIC := src/hassmic/main.c src/hassmic/wyoming.c src/hassmic/proto_wyoming.c src/hassmic/proto_esphome.c src/hassmic/buttons.c $(RING) $(SCREEN) src/hassmic/ringdesign.c src/hassmic/ringlive.c src/hassmic/musiclight.c src/hassmic/ledq.c \
           src/hassmic/sendspin.c src/hassmic/mass.c src/hassmic/json.c src/hassmic/arb.c src/hassmic/dropin.c src/hassmic/call.c src/hassmic/duck.c src/hassmic/ringstop.c src/hassmic/ble.c src/hassmic/ble_crypto.c src/hassmic/a2dp.c src/hassmic/a2dp_codecs.c src/hassmic/hfp.c src/hassmic/scoaudio.c src/hassmic/cvsd.c src/hassmic/msbc.c src/hassmic/sbc.c src/hassmic/btout.c src/hassmic/ota.c src/hassmic/update.c src/hassmic/sources.c src/hassmic/tls.c src/hassmic/http.c src/hassmic/https.c src/hassmic/adbwifi.c src/hassmic/radio.c src/hassmic/wifi.c src/hassmic/netaddr.c src/hassmic/improv.c src/hassmic/wifimotion.c src/hassmic/spkgain.c src/hassmic/ws.c src/hassmic/net.c src/hassmic/noise.c src/hassmic/hash.c src/hassmic/sounds.c src/hassmic/soundpack.c src/hassmic/micgain.c src/hassmic/micend.c src/hassmic/afemeta.c src/hassmic/wordtail.c src/hassmic/wakealign.c src/hassmic/micdenoise.c src/hassmic/earsup.c src/hassmic/settings.c src/hassmic/web.c src/hassmic/artifacts.c src/hassmic/dha.c src/hassmic/afe_parcel.c src/hassmic/davs.c src/hassmic/diag.c src/hassmic/clock.c src/hassmic/tz.c src/hassmic/apcountry.c src/hassmic/geo.c build/geo_data.c src/hassmic/statefile.c src/hassmic/volcurve.c src/hassmic/resample.c build/web_assets.c \
           src/hassmic/wake.c src/hassmic/wake_mww.c src/hassmic/mww_features.c src/hassmic/mww_model.c src/hassmic/mww_store.c \
           src/hassmic/plugin.c src/hassmic/plugin_store.c src/hassmic/plugin_fetch.c src/hassmic/plugin_index.c src/hassmic/plugin_net.c src/hassmic/logread.c \
           src/third_party/monocypher.c src/third_party/freeaptx.c $(RNNOISE)
HASSMIC_H := $(wildcard src/hassmic/*.h src/include/*.h) sdk/plugmsg.h build/.build-id

# The settings page (web.c), gzip'd into the binary
WEB := web/index.html:/ web/app.js:/app.js web/crypto.js:/crypto.js web/tar.js:/tar.js web/loudness.js:/loudness.js web/matrix.js:/matrix.js web/ring.js:/ring.js web/plugins.js:/plugins.js web/radio.js:/radio.js
# The countries' borders (geo.c; geo/README.md), built in as they are
build/geo_data.c: tools/geo/embed.py geo/borders.bin
	@mkdir -p build
	python3 tools/geo/embed.py $@ geo/borders.bin

build/web_assets.c: tools/embed.py $(foreach w,$(WEB),$(firstword $(subst :, ,$(w))))
	@mkdir -p build
	python3 tools/embed.py $@ $(WEB)

# SYSLIBS (device.mk): NDK system libraries a backend needs beside the stock ones (-lOpenSLES on checkers): they come
# with the NDK, so no stubs are needed for them.
$(OUT)/hassmic: $(HASSMIC) $(BOARD) $(AUDIO) $(WAKE) $(SOUND) $(WHISPER) $(HASSMIC_H) $(OUT)/libmbedtls.a $(addprefix $(STOCK)/,$(LIBS))
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(MBEDTLS_INC) -Isrc/hassmic $(filter %.c,$^) -o $@ $(LDFLAGS) $(OUT)/libmbedtls.a -lm -ldl $(SYSLIBS) $(addprefix $(STOCK)/,$(LIBS))

# PC build for protocol tests: file audio backend, no wake word (SIGUSR1 triggers), fake sound and whisper detection, identity of $(DEVICE).
build/hassmic-host: $(HASSMIC) $(BOARD) src/hassmic/audio_file.c src/hassmic/wake_none.c src/hassmic/sound_none.c src/hassmic/whisper_none.c $(HASSMIC_H) build/.device build/libmbedtls-host.a
	@mkdir -p build
	cc -O2 -Wall -Wextra $(DEFS) -Isrc/include -Isdk -Isrc/hassmic $(MBEDTLS_INC) $(filter %.c,$^) -o $@ build/libmbedtls-host.a -lpthread -lm -ldl -lopus -lz

# The same with a screen whatever $(DEVICE) has (screen.c, the band): tests/fake_screen.py, with a stand-in UI.
build/hassmic-host-screen: $(filter-out $(RING) $(SCREEN),$(HASSMIC)) src/hassmic/ring_band.c src/hassmic/screen.c $(BOARD) src/hassmic/audio_file.c src/hassmic/wake_none.c src/hassmic/sound_none.c src/hassmic/whisper_none.c $(HASSMIC_H) build/.device build/libmbedtls-host.a
	@mkdir -p build
	cc -O2 -Wall -Wextra $(DEFS) -Isrc/include -Isdk -Isrc/hassmic $(MBEDTLS_INC) $(filter %.c,$^) -o $@ build/libmbedtls-host.a -lpthread -lm -ldl -lopus -lz

# ARM build with file audio but the device's wake word engine, for running under qemu-arm (tools/qrun.sh).
$(OUT)/hassmic-qemu: $(HASSMIC) $(BOARD) src/hassmic/audio_file.c $(WAKE) $(SOUND) $(WHISPER) $(HASSMIC_H) $(OUT)/libmbedtls.a $(call STOCK_LIBS,libpryon.so libopus.so libz.so)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(MBEDTLS_INC) -Isrc/hassmic $(filter %.c,$^) -o $@ $(LDFLAGS) $(OUT)/libmbedtls.a -lm -ldl $(call STOCK_LIBS,libpryon.so libopus.so libz.so)

# A stand-in wake word plugin for tests/fake_plugins.py, built for the PC as a plugin is built for an Echo: static,
# against the SDK's client (sdk/hmplugin.c)
build/testwake: tests/plugins/testwake.c sdk/hmplugin.c sdk/hassmic_plugin.h sdk/plugmsg.h
	@mkdir -p build
	cc -O2 -Wall -Wextra -static -Isdk tests/plugins/testwake.c sdk/hmplugin.c -o $@

# A stand-in forwarder for tests/fake_plugins_net.py: storage, the log, a socket to the owner's server
build/testnet: tests/plugins/testnet.c sdk/hmplugin.c sdk/hassmic_plugin.h sdk/plugmsg.h
	@mkdir -p build
	cc -O2 -Wall -Wextra -static -Isdk tests/plugins/testnet.c sdk/hmplugin.c -o $@

# A stand-in music player for tests/fake_plugins_music.py: a tone as the newest music source, the volume, the Echo's
# state, a UDP port of its own, a search, Home Assistant's entities and actions
build/testmusic: tests/plugins/testmusic.c sdk/hmplugin.c sdk/hassmic_plugin.h sdk/plugmsg.h
	@mkdir -p build
	cc -O2 -Wall -Wextra -static -Isdk tests/plugins/testmusic.c sdk/hmplugin.c -lm -o $@

# a stand-in phone (tests/fake_plugins_call.py): a call out and in, the room's audio and its own both ways
build/testcall: tests/plugins/testcall.c sdk/hmplugin.c sdk/hassmic_plugin.h sdk/plugmsg.h
	@mkdir -p build
	cc -O2 -Wall -Wextra -static -Isdk tests/plugins/testcall.c sdk/hmplugin.c -lm -o $@
# a stand-in listener (tests/fake_plugins_request.py): a copy of every request, what it got said in its log
build/testreq: tests/plugins/testreq.c sdk/hmplugin.c sdk/hassmic_plugin.h sdk/plugmsg.h
	@mkdir -p build
	cc -O2 -Wall -Wextra -static -Isdk tests/plugins/testreq.c sdk/hmplugin.c -o $@

# a stand-in that hears the buttons and shows rows of its own on its card (tests/fake_plugins_button.py)
build/testbutton: tests/plugins/testbutton.c sdk/hmplugin.c sdk/hassmic_plugin.h sdk/plugmsg.h
	@mkdir -p build
	cc -O2 -Wall -Wextra -static -Isdk tests/plugins/testbutton.c sdk/hmplugin.c -o $@

# What tests/run.py needs built (CI's test job builds exactly this: a stand-in plugin left out of a list of its own
# there failed five tests on the runner only, where a PC had it from an earlier make host)
HOST_TESTS := build/hassmic-host build/hassmic-host-screen build/otatool-host build/plugrun-host build/rcpbridge-host build/zigbeed-host build/testwake build/testnet build/testmusic build/testcall build/testreq build/testbutton
host-tests: $(HOST_TESTS)
host: $(HOST_TESTS) $(OUT)/hassmic-qemu

# Building blocks of the Sendspin client, checked against reference implementations (aiohttp, python noiseprotocol).
UNIT := $(MBEDTLS_INC) src/hassmic/hash.c src/hassmic/ws.c src/hassmic/tls.c src/hassmic/net.c src/hassmic/logread.c src/hassmic/noise.c src/third_party/monocypher.c build/libmbedtls-host.a
# One target per test, so make -j unit runs them side by side (tests/run.py does: most of them wait or compile).
UNIT_TESTS := hash http ws noise ble_crypto micgain micend afemeta wordtail wakealign wifimotion spkgain tz apcountry geo statefile ledq micdenoise earsup dha_jwt a2dp_codecs cvsd msbc scoaudio \
	sbc web_crypto web_tar web_soundpack web_matrix web_loudness ring_pack mww blueprint_dropin buttons buttons_privacy buttons_grab afe_parcel lux ring_band ringlive ringviz sounds wifictl ringbar volcurve band_map setup_wipe setup_fixed firmware kiosk backlight alexaoff \
	screen ui touch json pb uninstall_boot sysinstall_aligners resample mixsteer fork_release plugrun plugmsg hmplugin plugin_store plugin_broker plugin_index logread ha_names call dropin web_browse web_rows web_wall netio \
	zb_crypto notice
unit: $(addprefix unit-,$(UNIT_TESTS))
# zigbeed's cryptography (src/zigbee/crypto.c) against Python's AES-CCM and an MMO hash of its own, and the install
# code example of Zigbee's spec
unit-zb_crypto: build/zb_crypto_test
	.venv/bin/python tests/unit/zb_crypto_ref.py | build/zb_crypto_test ref
build/zb_crypto_test: tests/unit/zb_crypto_test.c src/zigbee/crypto.c src/zigbee/zb.h build/libmbedtls-host.a
	cc -O2 -Wall -Wextra $(MBEDTLS_INC) tests/unit/zb_crypto_test.c src/zigbee/crypto.c build/libmbedtls-host.a -o $@
# the plugins' launcher on the PC: the protocol, the sandbox as far as x86_64 shows it (a static test program)
# and both seccomp programs, ARM's too, run in an interpreter against the table they were made from
# A musl program beside the glibc one: plugins are musl (5.8), whose raise(), abort() and pthread_kill() send with
# tkill where glibc's use tgkill (tests/unit/plugrun_musl.c).  Where there is no zig the test says it skipped that part
MUSL_CC ?= $(if $(shell command -v zig 2>/dev/null),zig cc -target x86_64-linux-musl,$(if $(wildcard toolchain/zig/zig),toolchain/zig/zig cc -target x86_64-linux-musl,))
unit-plugrun: build/plugrun-host build/plugprobe-host
	python3 tests/unit/plugfilter_test.py
	cc -O2 -Wall -static tests/unit/plugrun_child.c -lpthread -o build/plugrun_child
	$(if $(MUSL_CC),$(MUSL_CC) -O2 -Wall -static tests/unit/plugrun_musl.c -o build/plugrun_musl,rm -f build/plugrun_musl)
	PLUGRUN_MUSL=build/plugrun_musl python3 tests/unit/plugrun_test.py build/plugrun-host build/plugrun_child build/plugprobe-host
# packages as the Echo checks them (plugin_store.c): wrong in one way each, then at random, under ASan
# sdk/messages.def as a later hassmic's would be, and its header (tests/unit/scopes_demo.py: net.lan 2 widens,
# ha.entities 2 and call 2 within, a key of each, a value of call 2 in each direction): the store, the broker and the
# C client built against it a second time (-DSCOPES_DEMO), one header for all three under make -j
build/scopes_demo/plugmsg.h: tests/unit/scopes_demo.py sdk/messages.def tools/mkplugmsg.py
	@mkdir -p build/scopes_demo
	python3 tests/unit/scopes_demo.py build/scopes_demo/messages.def
	python3 tools/mkplugmsg.py --def build/scopes_demo/messages.def --header $@
unit-plugin_store: build/scopes_demo/plugmsg.h
	@mkdir -p build
	printf 'int main(void){return 0;}' | cc -x c - -static -o build/ps_static
	printf 'int main(void){return 0;}' | cc -x c - -o build/ps_dynamic
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isdk -Isrc/hassmic -Isrc/include tests/unit/plugin_store_test.c src/hassmic/plugin_store.c src/hassmic/plugin_index.c src/hassmic/hash.c src/hassmic/logread.c src/hassmic/json.c src/hassmic/statefile.c src/third_party/monocypher.c $(BOARD) src/hassmic/volcurve.c -lm -lpthread -o build/plugin_store_test
	build/plugin_store_test build/ps_static build/ps_dynamic
	@# the same code built as a later hassmic's would be (tests/unit/scopes_demo.py: net.lan 2 widens, ha.entities 2
	@# within, a key of each): a key of a later version than required, a raised requires into a widening version
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -DSCOPES_DEMO -Ibuild/scopes_demo -Isdk -Isrc/hassmic -Isrc/include tests/unit/plugin_store_test.c src/hassmic/plugin_store.c src/hassmic/plugin_index.c src/hassmic/hash.c src/hassmic/logread.c src/hassmic/json.c src/hassmic/statefile.c src/third_party/monocypher.c $(BOARD) src/hassmic/volcurve.c -lm -lpthread -o build/plugin_store_demo_test
	build/plugin_store_demo_test build/ps_static build/ps_dynamic
# the plugin index as an Echo reads it (plugin_index.c): signatures, the list and the revocation list wrong in one way
# each, then cut and changed at random, under ASan; then the keys again in a build with a second key built in (5.2)
unit-plugin_index:
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isdk -Isrc/hassmic -Isrc/include tests/unit/plugin_index_test.c src/hassmic/plugin_index.c src/hassmic/plugin_store.c src/hassmic/hash.c src/hassmic/logread.c src/hassmic/json.c src/hassmic/statefile.c src/third_party/monocypher.c $(BOARD) src/hassmic/volcurve.c -lm -lpthread -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free -o build/plugin_index_test
	build/plugin_index_test
	pk2=$$(python3 -c 'import sys; sys.path.insert(0, "scripts"); import otatool; print(otatool.key_pair(bytes([42] + [0] * 31))[1].hex())') && \
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isdk -Isrc/hassmic -Isrc/include -DPLUGINS_PUB2="\"$$pk2\"" -DPI_TEST_BUILT_PUB2 tests/unit/plugin_index_test.c src/hassmic/plugin_index.c src/hassmic/plugin_store.c src/hassmic/hash.c src/hassmic/logread.c src/hassmic/json.c src/hassmic/statefile.c src/third_party/monocypher.c $(BOARD) src/hassmic/volcurve.c -lm -lpthread -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free -o build/plugin_index_test_pub2
	build/plugin_index_test_pub2
# the broker's watchdog and hassmic's crash marks (plugin.c, docs/plugins.md 2.6): plugin.c itself with a stand-in
# plugin and a clock the test moves (tests/unit/plugin_harness.h), under ASan
PLUGIN_HARNESS := src/hassmic/plugin_store.c src/hassmic/plugin_index.c src/hassmic/plugin_net.c src/hassmic/logread.c src/hassmic/net.c src/hassmic/hash.c src/hassmic/json.c src/hassmic/statefile.c src/third_party/monocypher.c $(BOARD) src/hassmic/volcurve.c src/hassmic/call.c src/hassmic/micgain.c -lopus
unit-plugin_broker: build/scopes_demo/plugmsg.h
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isdk -Isrc/hassmic -Isrc/include tests/unit/plugin_broker_test.c $(PLUGIN_HARNESS) -lm -lpthread -o build/plugin_broker_test
	build/plugin_broker_test
	@# as a later hassmic's (the start and the versions only): a value of call 2 to a plugin of call 1 goes as its fallback
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -DSCOPES_DEMO -Ibuild/scopes_demo -Isdk -Isrc/hassmic -Isrc/include tests/unit/plugin_broker_test.c $(PLUGIN_HARNESS) -lm -lpthread -o build/plugin_broker_demo_test
	build/plugin_broker_demo_test
# The broker's message() fed whatever a plugin could send (docs/plugins.md 6), by libFuzzer under ASan and UBSan for
# FUZZ_SECS: not one of `make unit` (a minute, and it wants clang with libFuzzer).  What it finds stays in
# build/plugfuzz-corpus and is run again first the next time; a crash is left as build/plugfuzz-crash-*
FUZZ_SECS ?= 60
unit-plugfuzz:
	@mkdir -p build/plugfuzz-corpus
	clang -O1 -g -Wall -Wextra -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -Isdk -Isrc/hassmic -Isrc/include tests/unit/plugfuzz.c $(PLUGIN_HARNESS) -lm -lpthread -o build/plugfuzz
	PLUGFUZZ_SEEDS=build/plugfuzz-corpus build/plugfuzz -max_total_time=$(FUZZ_SECS) -max_len=8192 -close_fd_mask=3 -print_final_stats=1 -artifact_prefix=build/plugfuzz-crash- build/plugfuzz-corpus
# the log as a plugin with log.read gets it (logread.c): what is left out and blanked, the cursor across rotations;
# and the log's real writers (log_private, log_sealed, clock.c's reader) with sentinels that must not come through
unit-logread: build/geo_data.c
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/hassmic tests/unit/logread_test.c src/hassmic/logread.c -o build/logread_test && build/logread_test
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/hassmic -Isrc/include tests/unit/logwriters_test.c src/hassmic/logread.c src/hassmic/clock.c src/hassmic/tz.c src/hassmic/apcountry.c src/hassmic/geo.c build/geo_data.c src/hassmic/statefile.c -lz -lm -lpthread -o build/logwriters_test && build/logwriters_test
# the SDK's C client against a stand-in broker: what it keeps while a request waits, storage it cannot keep, a slow
# tick, a connect before READY, the log followed anew
unit-hmplugin: build/scopes_demo/plugmsg.h
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isdk tests/unit/hmplugin_test.c sdk/hmplugin.c -o build/hmplugin_test && build/hmplugin_test
	@# as a later SDK's (the demo's call 2): a value of call 2 from a plugin that speaks call 1 goes as its fallback.
	@# (the client beside the demo's header: a file's own folder comes first for its quoted includes)
	cp sdk/hmplugin.c sdk/hassmic_plugin.h build/scopes_demo/
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -DSCOPES_DEMO -Ibuild/scopes_demo tests/unit/hmplugin_test.c build/scopes_demo/hmplugin.c -o build/hmplugin_demo_test && build/hmplugin_demo_test
	@# put() itself (static: the test includes the client): one buffer one send, a value of no fallback not sent
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -DSCOPES_DEMO -Ibuild/scopes_demo tests/unit/hmplugin_put_test.c -o build/hmplugin_put_test && build/hmplugin_put_test
# the broker protocol's wire format (sdk/plugmsg.h) is what sdk/messages.def makes, and reads no byte past a packet
# (the vectors' program at -O0: thousands of checks in a few functions, which -O1 took minutes over)
unit-plugmsg:
	@mkdir -p build
	python3 tools/mkplugmsg.py --check
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isdk tests/unit/plugmsg_test.c -o build/plugmsg_test && build/plugmsg_test
	python3 tools/mkplugmsg.py --c build/plugmsg_vectors.c
	cc -O0 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isdk build/plugmsg_vectors.c -o build/plugmsg_vectors && build/plugmsg_vectors sdk/vectors
	@# the versions' rules (sdk/PROTOCOL.md "Versions"), each broken once; and a message that grew, on a definition of
	@# its own (tests/unit/plugmsg_scopes/demo.def): each version's layout, its vectors (a case per earlier version)
	python3 tests/unit/plugmsg_scopes_test.py
	@mkdir -p build/plugmsg_demo
	python3 tools/mkplugmsg.py --def tests/unit/plugmsg_scopes/demo.def --header build/plugmsg_demo/plugmsg.h
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Ibuild/plugmsg_demo tests/unit/plugmsg_scopes_test.c -o build/plugmsg_scopes_test && build/plugmsg_scopes_test
	python3 tools/mkplugmsg.py --def tests/unit/plugmsg_scopes/demo.def --vectors build/plugmsg_demo/vectors
	python3 tools/mkplugmsg.py --def tests/unit/plugmsg_scopes/demo.def --c build/plugmsg_demo/vectors.c
	cc -O0 -g -Wall -Wextra -Ibuild/plugmsg_demo build/plugmsg_demo/vectors.c -o build/plugmsg_demo/vectors_test && build/plugmsg_demo/vectors_test build/plugmsg_demo/vectors
unit-hash: build/libmbedtls-host.a
	@mkdir -p build
	cc -O2 -Wall -Isrc/hassmic -Isrc/include tests/unit/hash_test.c $(UNIT) -lpthread -o build/hash_test && build/hash_test
# http.c and tls.c (issue #236) against Python's servers: TLS 1.3 and 1.2 alone, certificates, bodies, redirects, times
unit-http: build/libmbedtls-host.a
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/hassmic -Isrc/include $(MBEDTLS_INC) tests/unit/http_test.c src/hassmic/http.c src/hassmic/tls.c src/hassmic/net.c src/hassmic/logread.c build/libmbedtls-host.a -lpthread -o build/http_test
	.venv/bin/python tests/unit/http_test.py build/http_test
unit-ws: build/libmbedtls-host.a
	@mkdir -p build
	cc -O2 -Wall -D_GNU_SOURCE -Isrc/hassmic -Isrc/include -include stdlib.h tests/unit/ws_test.c $(UNIT) -lpthread -o build/ws_test
	.venv/bin/python tests/unit/ws_ref.py build/ws_test
unit-noise: build/libmbedtls-host.a
	@mkdir -p build
	cc -O2 -Wall -Isrc/hassmic -Isrc/include tests/unit/noise_test.c $(UNIT) -lpthread -o build/noise_test
	.venv/bin/python tests/unit/noise_ref.py build/noise_test
unit-ble_crypto:
	@mkdir -p build
	cc -O2 -Wall -Isrc/hassmic tests/unit/ble_crypto_test.c src/hassmic/ble_crypto.c -o build/ble_crypto_test && build/ble_crypto_test
unit-micgain:
	@mkdir -p build
	cc -O2 -Wall -Isrc/hassmic tests/unit/micgain_test.c src/hassmic/micgain.c -lm -o build/micgain_test && build/micgain_test
unit-micend:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/micend_test.c src/hassmic/micend.c -lm -o build/micend_test && build/micend_test
unit-afemeta:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/afemeta_test.c src/hassmic/afemeta.c -o build/afemeta_test && build/afemeta_test
unit-wordtail:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/wordtail_test.c src/hassmic/wordtail.c -lm -o build/wordtail_test && build/wordtail_test
unit-wakealign:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/wakealign_test.c src/hassmic/wakealign.c src/hassmic/statefile.c -lm -o build/wakealign_test
	.venv/bin/python tests/unit/wakealign_ref.py build/wakealign_test
unit-wifimotion:
	@mkdir -p build
	cc -O2 -Wall -Isrc/hassmic tests/unit/wifimotion_test.c src/hassmic/wifimotion.c src/hassmic/statefile.c -lpthread -lm -o build/wifimotion_test && build/wifimotion_test
unit-spkgain:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/spkgain_test.c src/hassmic/spkgain.c -lpthread -o build/spkgain_test && build/spkgain_test
unit-tz:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/tz_test.c src/hassmic/tz.c -o build/tz_test && .venv/bin/python tests/unit/tz_ref.py build/tz_test
unit-apcountry:
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/hassmic tests/unit/apcountry_test.c src/hassmic/apcountry.c -o build/apcountry_test && build/apcountry_test

unit-geo: build/geo_data.c
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/geo_test.c src/hassmic/geo.c build/geo_data.c -lz -lm -o build/geo_test && .venv/bin/python tests/unit/geo_ref.py build/geo_test

unit-statefile:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/statefile_test.c src/hassmic/statefile.c -Wl,--wrap=fsync,--wrap=rename -lpthread -o build/statefile_test && build/statefile_test
# an accept() error never ends a server: the listener kept, or opened again (issue #241)
unit-netio:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/include tests/unit/netio_test.c -Wl,--wrap=accept -o build/netio_test && build/netio_test
unit-ledq:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/ledq_test.c src/hassmic/ledq.c -lpthread -o build/ledq_test && build/ledq_test
unit-micdenoise:
	@mkdir -p build
	cc -O2 -Wall -Isrc/hassmic tests/unit/micdenoise_test.c src/hassmic/micdenoise.c $(RNNOISE) -lm -o build/micdenoise_test && build/micdenoise_test
unit-earsup:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/earsup_test.c src/hassmic/earsup.c -lpthread -lm -o build/earsup_test && build/earsup_test
unit-dha_jwt:
	@mkdir -p build
	cc -O2 -Wall -Isrc/hassmic -Isrc/include -include stdlib.h tests/unit/dha_hal_fake.c -shared -fPIC -o build/dha_hal_fake.so
	cc -O2 -Wall -Isrc/hassmic -Isrc/include tests/unit/dha_jwt_test.c src/hassmic/dha.c src/hassmic/afe_parcel.c src/hassmic/hash.c -o build/dha_jwt_test -ldl && build/dha_jwt_test build/dha_hal_fake.so && build/dha_jwt_test --service && build/dha_jwt_test --service2
unit-a2dp_codecs:
	@mkdir -p build
	cc -O2 -Wall -Isrc/hassmic tests/unit/a2dp_codecs_test.c src/hassmic/a2dp_codecs.c src/hassmic/sbc.c src/third_party/freeaptx.c -lm -ldl -lopus -o build/a2dp_codecs_test && build/a2dp_codecs_test
unit-sbc:
	if command -v sbcenc >/dev/null; then tests/unit/sbc_ref.sh; else echo "sbc: sbcenc/sbcdec (package sbc) missing, skipped"; fi
unit-web_crypto:
	if command -v node >/dev/null; then .venv/bin/python tests/unit/web_crypto_test.py; elif [ -n "$$CI" ]; then echo "web crypto: node missing"; exit 1; else echo "web crypto: node missing, skipped"; fi
unit-kiosk:
	.venv/bin/python tests/unit/kiosk_test.py
# no name or object id of hassmic's own entities is one a plugin's can have (plugin.c ha_name, ha_object_id)
unit-ha_names:
	python3 tests/unit/ha_names_test.py
unit-backlight:
	.venv/bin/python tests/unit/backlight_test.py
unit-alexaoff:
	.venv/bin/python tests/unit/alexaoff_test.py
unit-web_soundpack:
	if command -v node >/dev/null; then .venv/bin/python tests/unit/web_soundpack_test.py; else echo "web sound pack: node missing, skipped"; fi

unit-web_tar:
	if command -v node >/dev/null; then .venv/bin/python tests/unit/web_tar_test.py; else echo "web tar: node missing, skipped"; fi
unit-web_matrix:
	if command -v node >/dev/null; then .venv/bin/python tests/unit/web_matrix_test.py; else echo "web matrix: node missing, skipped"; fi
# the plugin index's list as the browse page shows it (web/plugins.js hmbrowse)
unit-web_browse:
	if command -v node >/dev/null; then .venv/bin/python tests/unit/web_browse_test.py; else echo "web browse: node missing, skipped"; fi
unit-web_rows:
	if command -v node >/dev/null; then .venv/bin/python tests/unit/web_rows_test.py; else echo "web rows: node missing, skipped"; fi
# the warning in full in the install dialog of a plugin that is not official where the sandbox misses a wall (hmwall)
unit-web_wall:
	if command -v node >/dev/null; then .venv/bin/python tests/unit/web_wall_test.py; else echo "web wall: node missing, skipped"; fi
# the page's timer sound level (web/loudness.js) against BS.1770, and main.c's ring beep at Alexa's level
unit-web_loudness:
	if command -v node >/dev/null; then .venv/bin/python tests/unit/web_loudness_test.py; elif [ -n "$$CI" ]; then echo "web loudness: node missing"; exit 1; else echo "web loudness: node missing, skipped"; fi
unit-setup_wipe:
	bash tests/unit/setup_wipe_test.sh
# the guided setup's fixed address (issue #191): its checks against the page's, and fixed_set against stand-ins
unit-setup_fixed:
	bash tests/unit/setup_fixed_test.sh
# a fork's own releases (scripts/fork-release.sh, keys/release.conf, secrets/fork.conf), against a stand-in gh
unit-fork_release:
	bash tests/unit/fork_release_test.sh
# which firmware hassmic goes onto (device.sh firmware_ok, device_check_firmware; setup's firmware_accept), PR #98
unit-firmware:
	bash tests/unit/firmware_test.sh
unit-uninstall_boot:
	bash tests/unit/uninstall_boot_test.sh
# the wake word aligners onto /system: the installers stage them, sysinstall.sh where they fit
unit-sysinstall_aligners:
	bash tests/unit/sysinstall_aligners_test.sh
# NOTICE as each model's bundle carries it (scripts/notice.py), and the lists that ship it
unit-notice:
	python3 tests/unit/notice_test.py
unit-ring_pack:
	if command -v node >/dev/null; then node tests/unit/ring_pack_test.js; else echo "ring packs: node missing, skipped"; fi
unit-mww:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/mww_test.c src/hassmic/mww_features.c src/hassmic/mww_model.c -lm -o build/mww_test
	.venv/bin/python tests/unit/mww_ref.py build/mww_test
unit-blueprint_dropin:
	.venv/bin/python tests/blueprint_dropin.py
unit-buttons:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/buttons_test.c src/hassmic/buttons.c -lpthread -o build/buttons_test && build/buttons_test build/buttons_test.fifo
unit-buttons_privacy:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/buttons_privacy_test.c src/hassmic/buttons.c -lpthread -o build/buttons_privacy_test && build/buttons_privacy_test build/buttons_privacy.fifo
unit-buttons_grab:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/buttons_grab_test.c src/hassmic/buttons.c -Wl,--wrap=ioctl -lpthread -o build/buttons_grab_test && build/buttons_grab_test build/buttons_grab.fifo && build/buttons_grab_test build/buttons_grab.fifo nokey
unit-afe_parcel:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/afe_parcel_test.c src/hassmic/afe_parcel.c -o build/afe_parcel_test && build/afe_parcel_test
unit-lux:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/lux_test.c -lm -o build/lux_test && build/lux_test
# What the live ring's music effects make of a track, without an Echo (tools/ringviz.py, tools/ringmusic.py)
build/ringviz: tools/ringviz.c src/hassmic/ringlive.c src/hassmic/musiclight.c src/hassmic/ringdesign.c src/hassmic/json.c src/hassmic/statefile.c $(HASSMIC_H)
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tools/ringviz.c src/hassmic/ringlive.c src/hassmic/musiclight.c src/hassmic/ringdesign.c src/hassmic/json.c src/hassmic/statefile.c -lm -lpthread -o $@
unit-ringviz: build/ringviz
	@mkdir -p build/viz
	.venv/bin/python tools/ringmusic.py build/viz/track.raw build/viz/truth.json
	build/ringviz build/viz/track.raw build/viz
	.venv/bin/python tools/ringviz.py build/viz build/viz/truth.json --check
unit-ringlive:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/ringlive_test.c src/hassmic/ringlive.c src/hassmic/musiclight.c src/hassmic/ringdesign.c src/hassmic/json.c src/hassmic/statefile.c -lm -lpthread -o build/ringlive_test && build/ringlive_test
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/ringlive_leds_test.c src/hassmic/ringlive.c src/hassmic/musiclight.c src/hassmic/ringdesign.c src/hassmic/json.c src/hassmic/statefile.c -lm -lpthread -o build/ringlive_leds_test && build/ringlive_leds_test
	python3 tests/unit/ringlive_conf.py
unit-ring_band:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/ring_band_test.c src/hassmic/ring_band.c src/hassmic/afe_parcel.c src/hassmic/ringdesign.c src/hassmic/statefile.c -lm -lpthread -o build/ring_band_test && build/ring_band_test
# volcurve.c with each model's board.c: each names its curve, measured on it or the Dot 2's as a stand-in (a todo line)
unit-volcurve:
	@mkdir -p build
	for d in $(patsubst devices/%/board.c,%,$(wildcard devices/*/board.c)); do \
	  cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/volcurve_test.c src/hassmic/volcurve.c devices/$$d/board.c -lm -o build/volcurve_$$d && build/volcurve_$$d || exit 1; done
# Sendspin's rate (resample.c): the resampler against the true sine, chunk sizes, its lag; the servo against a model
# of the Echo Show 5's queue with its clock off
# CVSD (cvsd.c, the Dot 2's phone calls): tones through the coder, the filters' pass and stop bands, and the
# bitstream the Echo Dot 5's chip sent of a 500 Hz tone (testdata/hfp/, read off the Dot 2's /dev/ebc) decoding as it
unit-cvsd:
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/hassmic tests/unit/cvsd_test.c src/hassmic/cvsd.c -lm -o build/cvsd_test && build/cvsd_test testdata/hfp/ebc-cannoli-500hz.raw
# mSBC's framing on the air (msbc.c, wideband phone calls): pieces of any length, losses, a broken frame, a late join
unit-msbc:
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/hassmic tests/unit/msbc_frame_test.c src/hassmic/msbc.c src/hassmic/sbc.c -lm -o build/msbc_frame_test && build/msbc_frame_test
# scoaudio.c finds a call's PCM devices by name as each measured Echo lists them (testdata/hfp/asound-pcm-*.txt)
unit-scoaudio:
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/hassmic -Isrc/include tests/unit/scoaudio_test.c src/hassmic/cvsd.c src/hassmic/msbc.c src/hassmic/sbc.c -lm -lpthread -o build/scoaudio_test && build/scoaudio_test
# a call's audio (call.c): the claim, and the media without a codec a plugin's call has (CALL_CODEC_PCM): frames out
# gated and gain-corrected, in by number, a missing one faded, against a Voip stream that plays in real time
unit-call:
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/hassmic -Isrc/include tests/unit/call_test.c src/hassmic/call.c src/hassmic/micgain.c -lopus -lm -lpthread -o build/call_test && build/call_test
# Drop In (dropin.c) with call.c's media, against a stand-in of the other Echo over loopback UDP: frames sent again by
# a host on the LAN (taken once, the address and the call's life by the newest only), the events the core is told
# while it is held up (its "idle" never lost, the claim let go after the last)
unit-dropin:
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -Wno-format-truncation -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/hassmic -Isrc/include tests/unit/dropin_test.c src/hassmic/dropin.c src/hassmic/call.c src/hassmic/micgain.c src/third_party/monocypher.c -lopus -lm -lpthread -o build/dropin_test && build/dropin_test
unit-resample:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/resample_test.c src/hassmic/resample.c -lm -o build/resample_test && build/resample_test
# the Dots' music queue model steered towards the mixer's count (issue #225)
unit-mixsteer:
	@mkdir -p build
	cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/mixsteer_test.c -lm -o build/mixsteer_test && build/mixsteer_test
# the page's band (web/ring.js) against ring_band.c's, pixel for pixel
unit-band_map:
	if command -v node >/dev/null; then .venv/bin/python tests/unit/band_map_test.py; elif [ -n "$$CI" ]; then echo "band map: node missing"; exit 1; else echo "band map: node missing, skipped"; fi
unit-sounds:
	@mkdir -p build
	python3 tests/unit/sounds_zip.py build/sounds_dir build/sounds_test.zip
	cc -O2 -Wall -Wextra -DSYNTH -Isrc/hassmic tests/unit/sounds_test.c src/hassmic/sounds.c src/hassmic/statefile.c -o build/sounds_test && build/sounds_test synth build/sounds_dir build/sounds_test.zip
	if [ -d firmware/checkers/rootfs/system/priv-app ]; then cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/sounds_test.c src/hassmic/sounds.c src/hassmic/statefile.c src/hassmic/volcurve.c devices/checkers/board.c -o build/sounds_checkers && HASSMIC_EARCON_ROOT=firmware/checkers/rootfs build/sounds_checkers; else echo "sounds: firmware/checkers not unpacked, its apps' sounds skipped"; fi
	if [ -d firmware/cronos/rootfs/system/priv-app ]; then cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/sounds_test.c src/hassmic/sounds.c src/hassmic/statefile.c src/hassmic/volcurve.c devices/cronos/board.c -o build/sounds_cronos && HASSMIC_EARCON_ROOT=firmware/cronos/rootfs build/sounds_cronos; else echo "sounds: firmware/cronos not unpacked, its apps' sounds skipped"; fi
	if [ -d firmware/athena/rootfs/system/priv-app ]; then cc -O2 -Wall -Wextra -Isrc/hassmic tests/unit/sounds_test.c src/hassmic/sounds.c src/hassmic/statefile.c src/hassmic/volcurve.c devices/athena/board.c -o build/sounds_athena && HASSMIC_EARCON_ROOT=firmware/athena/rootfs build/sounds_athena; else echo "sounds: firmware/athena not unpacked, its apps' sounds skipped"; fi
# the dex programs against each model's own firmware that runs them, where it is unpacked here (CI: the dex only):
# wifictl.dex where Android's framework runs (device.conf ANDROID), ringbar.dex with the band (SCREEN_BAND)
frameworks = $(foreach m,$(patsubst devices/%/device.conf,%,$(shell grep -l '^$(1)' devices/*/device.conf)),$(if $(wildcard firmware/$(m)/rootfs/system/framework),--framework=firmware/$(m)/rootfs))
unit-wifictl:
	.venv/bin/python tools/mkwifictl.py --check $(call frameworks,ANDROID=1)
	.venv/bin/python tests/unit/wifictl_run.py
unit-ringbar:
	.venv/bin/python tools/mkringbar.py --check $(call frameworks,SCREEN_BAND=1)
	.venv/bin/python tests/unit/ringbar_run.py
# every model with the screen UI (device.mk SCREEN), against its own firmware where that is unpacked here (CI: the dex only)
SCREEN_MODELS := $(patsubst devices/%/device.mk,%,$(shell grep -l '^SCREEN ' devices/*/device.mk))
unit-screen:
	.venv/bin/python tools/mkscreen.py --check $(foreach m,$(SCREEN_MODELS),$(if $(wildcard firmware/$(m)/rootfs/system/framework),--framework=firmware/$(m)/rootfs))
# the screen UI as pictures, on the PC (FreeType, the firmware's Roboto): tools/uishot.c says how
build/uishot: tools/uishot.c $(UI_SCENE) src/ui/scene.h src/ui/ui.h src/ui/gfx.h
	@mkdir -p build
	cc -O2 -Wall -Wextra $$(pkg-config --cflags freetype2) tools/uishot.c $(UI_SCENE) -o $@ $$(pkg-config --libs freetype2) -lz -lm
unit-json:
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tests/unit/json_test.c src/hassmic/json.c -o build/json_test && build/json_test
# the ESPHome API's protobuf reader (pb.h) on what anyone on the network may send
unit-pb:
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/hassmic tests/unit/pb_test.c -o build/pb_test && build/pb_test
unit-ui:
	@mkdir -p build
	cc -O2 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tests/unit/ui_test.c $(UI_SCENE) -lm -o build/ui_test && build/ui_test
unit-touch:
	@mkdir -p build
	cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tests/unit/touch_test.c src/ui/touch.c -o build/touch_test && build/touch_test

# what the daemon reports, and what bundles are called (ota-push.sh, CI)
version:
	@echo $(VERSION)$(if $(RELEASE),,+$(BUILD))

clean:
	rm -rf build

.PHONY: all host host-tests unit $(addprefix unit-,$(UNIT_TESTS)) version clean kmod-missing kernel-tools FORCE
