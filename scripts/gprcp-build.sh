#!/bin/bash
# gprcp: our own radio program (RCP) for the Echo 4's Qorvo GP712, in place of Amazon's /system/bin/ot-rcp (PLAN.md
# "Thread 1.4", phase 1; docs/re-gp712.md).  OpenThread's RCP (the commit otbr-agent is built from: one RCP API) on
# ot-qorvo's GP712 platform (our copy, src/gprcp/: the caps, the clock) and Qorvo's public GP712 library, which speaks
# to Amazon's driver once its gpMacDispatcher API is offered as 2.7.0.0 (one byte, docs/re-gp712.md §0).  What a
# Thread 1.2+ host asks of a radio and the chip does not have is OpenThread's own, in software: transmit security, and
# timed transmit (src/gprcp/openthread-core-gp712-config.h) but where our module hassmic_gptime is loaded: then on the
# chip's own clock (src/gprcp/timed.c, Qorvo's DataRequest wrapped at link time).  Spinel in HDLC-lite on
# stdin/stdout, as Amazon's.
#
# The library is the Raspberry Pi's (hard float, glibc's names), so the program is a static musl one, built by zig,
# as the plugins are (Android's NDK is soft float).  Qorvo's EULA: use, and distribution in products, with Qorvo
# silicon only (the Echo 4 is): fetched here by checksum, never in git.
#   scripts/gprcp-build.sh <out dir>      writes <out>/gprcp
set -euo pipefail
out=$1
top=$(cd "$(dirname "$0")/.." && pwd)
src=$top/toolchain/src/gp712
mkdir -p "$src" "$out"; out=$(cd "$out" && pwd)
command -v zig >/dev/null || { echo "gprcp-build: needs zig (ziglang.org) for the static musl build" >&2; exit 1; }

# name  url (GitHub)  sha256
SOURCES="
ot-qorvo    openthread/ot-qorvo/archive/ce0fd368ef6b0a8122433588a17ec8f5258d647a.tar.gz   27f750cf9aaa6100c57cf5f4266cae8f41c9a037239eedfc845e41b81df30cdb
openthread  openthread/openthread/archive/27ded13010ef7ce8c47560108f1e7c27ecde1d26.tar.gz 709e6ee5b3a2304162204ab0b990799a4b5b74b68a2a41cc01a92895e4c280e4
mbedtls     Mbed-TLS/mbedtls/archive/82e69999f91126589a28b70dd75b69fbcabe2711.tar.gz         49562d7a841503708ab8a8d2ef4939e655dfa3d2363ab8400c2f426684f60492
framework   Mbed-TLS/mbedtls-framework/archive/ced31491c5ec907a6be533512f19dc1779230ac1.tar.gz 66c5bea27c94cb0e6838c741593e1832497f386b639d64f0a3c3dbf1d71f9055
qorvo-lib   raw:Qorvo/qpg-openthread/2762d55db1d218be57f251a8c8eafbb1938c36a7/gp712/lib/libQorvoGP712_ftd.a 43b4e631f1ffb8868e930a5ca4b072c05f7968eb610240113c749b9e5362e8f8
"
get() {     # name url sum: the file, fetched once
    local f=$src/$1.${2##*.} u=$2
    case $u in raw:*) u=https://raw.githubusercontent.com/${u#raw:};; *) u=https://github.com/$u;; esac
    if [ ! -f "$f" ] || [ "$(sha256sum "$f" | cut -c1-64)" != "$3" ]; then
        curl -fsSL --retry 3 -o "$f.part" "$u"
        [ "$(sha256sum "$f.part" | cut -c1-64)" = "$3" ] || { echo "gprcp-build: $1: checksum differs" >&2; rm -f "$f.part"; exit 1; }
        mv "$f.part" "$f"
    fi
    echo "$f"
}
pin=$( (echo "$SOURCES"; find "$top/src/gprcp" -type f | sort | xargs cat; cat "$0") | sha256sum | cut -c1-16)
tree=$src/tree-$pin
if [ ! -f "$tree/.complete" ]; then
    rm -rf "$tree"; mkdir -p "$tree"
    tar -xzf "$(echo "$SOURCES" | awk '$1=="ot-qorvo"' | { read -r n u s; get "$n" "$u" "$s"; })" -C "$tree" --strip-components=1
    mkdir -p "$tree/openthread"
    tar -xzf "$(echo "$SOURCES" | awk '$1=="openthread"' | { read -r n u s; get "$n" "$u" "$s"; })" -C "$tree/openthread" --strip-components=1
    # its submodules (GitHub's archives carry none): mbedtls and mbedtls' framework, the commits otbr-build.sh takes
    m=$tree/openthread/third_party/mbedtls/repo; mkdir -p "$m/framework"
    tar -xzf "$(echo "$SOURCES" | awk '$1=="mbedtls"' | { read -r n u s; get "$n" "$u" "$s"; })" -C "$m" --strip-components=1
    tar -xzf "$(echo "$SOURCES" | awk '$1=="framework"' | { read -r n u s; get "$n" "$u" "$s"; })" -C "$m/framework" --strip-components=1
    # zig's linker takes no -Map (the RCP app's link map: nothing of ours reads it)
    r=$tree/openthread/examples/apps/ncp/rcp.cmake
    sed -i 's/-Wl,-Map=ot-rcp.map//' "$r"
    ! grep -q -- '-Map' "$r" || { echo "gprcp-build: the -Map patch no longer applies" >&2; exit 1; }
    # the platform: ours (src/gprcp), over ot-qorvo's
    rm -rf "$tree/src/gp712"; cp -r "$top/src/gprcp" "$tree/src/gp712"
    # Qorvo's library with the one byte Amazon's driver needs
    lib=$(echo "$SOURCES" | awk '$1=="qorvo-lib"' | { read -r n u s; get "$n" "$u" "$s"; })
    sdk=$tree/sdk/gp712/lib; mkdir -p "$sdk/x"
    ( cd "$sdk/x" && ar x "$lib"
      o=gpMacDispatcher_wrapper_client.o
      [ "$(od -An -tx1 -j $((0x8318)) -N4 $o | tr -d " \n")" = 02080000 ] || { echo "gprcp-build: the version bytes are not where docs/re-gp712.md says" >&2; exit 1; }
      printf '\x07' | dd of=$o bs=1 seek=$((0x8319)) conv=notrunc 2>/dev/null
      llvm-ar rcs ../libQorvoGP712_ftd.a $(ar t "$lib") && cp ../libQorvoGP712_ftd.a ../libQorvoGP712_mtd.a )
    rm -rf "$sdk/x"
    touch "$tree/.complete"
fi

# zig as the toolchain: static musl, ARMv7 hard float (the Echo 4's Cortex-A53 runs 32-bit code)
tc=$out/gprcp-tc; mkdir -p "$tc"
for t in cc c++; do printf '#!/bin/sh\nexec zig %s -target arm-linux-musleabihf -mcpu=cortex_a7 "$@"\n' $t > "$tc/z$t"; chmod +x "$tc/z$t"; done
for t in ar ranlib; do printf '#!/bin/sh\nexec zig %s "$@"\n' $t > "$tc/z$t"; chmod +x "$tc/z$t"; done
cat > "$tc/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER $tc/zcc)
set(CMAKE_CXX_COMPILER $tc/zc++)
set(CMAKE_AR $tc/zar)
set(CMAKE_RANLIB $tc/zranlib)
set(CMAKE_C_FLAGS_INIT "-fdata-sections -ffunction-sections -std=gnu99")
set(CMAKE_CXX_FLAGS_INIT "-fdata-sections -ffunction-sections -fno-exceptions -fno-rtti")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -Wl,--gc-sections -Wl,--wrap=gpMacDispatcher_DataRequest")
EOF
b=$out/gprcp-build-$pin
mkdir -p "$b"
( cd "$b"
  QORVO_OT_SDK=$tree/sdk cmake -GNinja -Wno-dev -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_TOOLCHAIN_FILE="$tc/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DQORVO_PLATFORM=gp712 -DOT_PLATFORM=external -DOT_THREAD_VERSION=1.4 \
    -DOT_RCP=ON -DOT_FTD=OFF -DOT_MTD=OFF -DOT_APP_CLI=OFF -DOT_APP_NCP=OFF -DOT_APP_RCP=ON -DOT_COMPILE_WARNING_AS_ERROR=OFF \
    -DOT_PACKAGE_VERSION=openthread-27ded13 \
    '-DCMAKE_C_FLAGS=-DOPENTHREAD_BUILD_DATETIME=\"gprcp\"' '-DCMAKE_CXX_FLAGS=-DOPENTHREAD_BUILD_DATETIME=\"gprcp\"' \
    "$tree" > cmake.log 2>&1 || { tail -30 cmake.log >&2; exit 1; }
  ninja ot-rcp > ninja.log 2>&1 || { grep -E 'error|FAILED|undefined' ninja.log | head -30 >&2; exit 1; } )
llvm-strip -o "$out/gprcp" "$b/bin/ot-rcp"
# the trees and builds of earlier pins (~100 MB each): only this one is used again
for d in "$src"/tree-* "$out"/gprcp-build-*; do [ "$d" = "$tree" ] || [ "$d" = "$b" ] || rm -rf "$d"; done
echo "gprcp-build: $out/gprcp $(stat -c%s "$out/gprcp") bytes"
